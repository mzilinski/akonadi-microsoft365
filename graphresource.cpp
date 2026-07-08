/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "graphresource.h"

#include "graph_debug.h"
#include "graphclient/auth/graphoauth.h"
#include "graphresourceadaptor.h"
#include "graphsettingsbase.h"
#include "graphsyncstateattribute.h"

#include "calendar/grapheventhandler.h"
#include "contact/graphcontacthandler.h"
#include "jobs/graphbatchjob.h"
#include "jobs/graphfetchfoldersjob.h"
#include "jobs/graphfetchitempayloadjob.h"
#include "jobs/graphfetchitemsjob.h"
#include "jobs/graphfetchpimitemsjob.h"
#include "mail/graphmailhandler.h"
#include "todo/graphtodohandler.h"

#include <KContacts/Addressee>

#include <Akonadi/AttributeFactory>
#include <Akonadi/ChangeRecorder>
#include <Akonadi/CollectionFetchJob>
#include <Akonadi/CollectionFetchScope>
#include <Akonadi/CollectionModifyJob>
#include <Akonadi/EntityDisplayAttribute>
#include <Akonadi/ItemFetchScope>
#include <Akonadi/SpecialCollectionAttribute>
#include <Akonadi/SpecialMailCollections>
#include <KLocalizedString>
#include <KMime/Message>
#include <QBuffer>
#include <QTimer>
#include <QUrl>

using namespace Akonadi;

namespace
{
// Graph well-known mail folder names (language independent) -> Akonadi special type.
struct SpecialFolder {
    const char *wellKnownName; // segment for /me/mailFolders/<name>
    SpecialMailCollections::Type type;
    const char *attributeType; // SpecialCollectionAttribute value
    const char *iconName;
};
const SpecialFolder kSpecialFolders[] = {
    {"inbox", SpecialMailCollections::Inbox, "inbox", "mail-folder-inbox"},
    {"sentitems", SpecialMailCollections::SentMail, "sent-mail", "mail-folder-sent"},
    {"drafts", SpecialMailCollections::Drafts, "drafts", "document-edit"},
    {"deleteditems", SpecialMailCollections::Trash, "trash", "user-trash"},
    {"junkemail", SpecialMailCollections::Spam, "spam", "mail-mark-junk"},
    {"outbox", SpecialMailCollections::Outbox, "outbox", "mail-folder-outbox"},
};
}

GraphResource::GraphResource(const QString &id)
    : ResourceWidgetBase(id)
    , mSettings(new GraphSettings(this))
{
    setNeedsNetwork(true);

    AttributeFactory::registerAttribute<GraphSyncStateAttribute>();

    new GraphResourceAdaptor(this); // exports sendMessage/messageSent for the MTA agent

    // The root collection is virtual; its children are the Graph mail folders.
    mRootCollection.setParentCollection(Collection::root());
    mRootCollection.setContentMimeTypes({Collection::mimeType()});
    mRootCollection.setRemoteId(QStringLiteral("msgfolderroot")); // Graph well-known root

    // Ensure our delta-link attribute survives collection fetches (see EwsSyncStateAttribute).
    changeRecorder()->collectionFetchScope().setAncestorRetrieval(CollectionFetchScope::All);
    changeRecorder()->collectionFetchScope().fetchAttribute<GraphSyncStateAttribute>();
    // Change-replay handlers need the payload (event/contact/MIME) and the parent
    // collection of each changed item — otherwise itemAdded/itemChanged see empty items.
    changeRecorder()->itemFetchScope().fetchFullPayload(true);
    changeRecorder()->itemFetchScope().setAncestorRetrieval(ItemFetchScope::Parent);
    changeRecorder()->itemFetchScope().setFetchModificationTime(false);

    mFolderDeltaLink = mSettings->folderDeltaLink();

    connect(this, &AgentBase::error, this, [](const QString &msg) {
        qCWarning(GRAPH_LOG) << "AgentBase error:" << msg;
    });
    // The config dialog runs as a plugin in the client process (graphconfig.cpp);
    // this fires after it saved the new settings.
    connect(this, &AgentBase::reloadConfiguration, this, &GraphResource::reloadConfig);
    // The account root collection carries the instance name in client views; the folder
    // delta never re-delivers it, so propagate renames explicitly.
    connect(this, &AgentBase::agentNameChanged, this, &GraphResource::updateRootCollectionName);

    QMetaObject::invokeMethod(this, &GraphResource::delayedInit, Qt::QueuedConnection);
}

GraphResource::~GraphResource() = default;

void GraphResource::delayedInit()
{
    setUpAuth();
}

void GraphResource::setUpAuth()
{
    // OAuth2 (Auth Code + PKCE) against login.microsoftonline.com, Graph scopes.
    // Silent refresh via the keychain-stored refresh token; interactive browser
    // login only when that fails. Tokens are persisted via QtKeychain/KWallet.
    mAuth.reset(new GraphOAuth(mSettings->tenantId(), mSettings->clientId(), identifier(), this));
    connect(mAuth.data(), &GraphOAuth::ready, this, &GraphResource::onAuthReady);
    connect(mAuth.data(), &GraphOAuth::failed, this, &GraphResource::onAuthFailed);
    Q_EMIT status(Running, i18nc("@info:status", "Authenticating with Microsoft 365"));
    mAuth->authenticate();
}

void GraphResource::onAuthReady()
{
    qCDebug(GRAPH_LOG) << "auth ready, starting collection tree sync";
    reconfigureClient();
    Q_EMIT status(Idle, i18nc("@info:status", "Ready"));
    synchronizeCollectionTree();

    // No push notifications on Graph for desktop clients — poll with delta queries.
    if (!mPollTimer) {
        mPollTimer = new QTimer(this);
        connect(mPollTimer, &QTimer::timeout, this, [this] {
            synchronize();
        });
    }
    if (mSettings->pollInterval() > 0) {
        mPollTimer->start(mSettings->pollInterval() * 60 * 1000);
    }
}

void GraphResource::onAuthFailed(const QString &error)
{
    qCWarning(GRAPH_LOG) << "auth failed:" << error;
    Q_EMIT status(Broken, i18nc("@info:status", "Authentication failed: %1", error));
}

void GraphResource::reconfigureClient()
{
    mClient.setBaseUrl(QStringLiteral("https://graph.microsoft.com/v1.0"));
    mClient.setAuth(mAuth.data());
}

void GraphResource::doSetOnline(bool online)
{
    ResourceWidgetBase::doSetOnline(online);
    if (online) {
        setUpAuth();
    } else if (mPollTimer) {
        mPollTimer->stop();
    }
}

void GraphResource::updateRootCollectionName(const QString &name)
{
    // Our only first-level collection is the account root; rename it to match.
    auto fetch = new CollectionFetchJob(Collection::root(), CollectionFetchJob::FirstLevel, this);
    fetch->fetchScope().setResource(identifier());
    connect(fetch, &CollectionFetchJob::result, this, [name](KJob *job) {
        if (job->error()) {
            qCWarning(GRAPH_LOG) << "root collection fetch for rename failed:" << job->errorText();
            return;
        }
        const auto cols = qobject_cast<CollectionFetchJob *>(job)->collections();
        for (Collection col : cols) {
            if (col.name() != name) {
                col.setName(name);
                new CollectionModifyJob(col);
            }
        }
    });
}

void GraphResource::reloadConfig()
{
    const QString oldTenant = mSettings->tenantId();
    const QString oldClient = mSettings->clientId();

    // The graphconfig plugin wrote the file from the client process.
    mSettings->sharedConfig()->reparseConfiguration();
    mSettings->load();
    mFolderDeltaLink = mSettings->folderDeltaLink();

    // A different tenant/client id invalidates the stored token — sign in again.
    if (mSettings->tenantId() != oldTenant || mSettings->clientId() != oldClient) {
        if (mAuth) {
            mAuth->forgetTokens();
        }
        setUpAuth();
    } else if (mPollTimer && mSettings->pollInterval() > 0) {
        mPollTimer->start(mSettings->pollInterval() * 60 * 1000);
    }
}

// ============================================================================
//  Synchronisation: Graph -> local
// ============================================================================

void GraphResource::retrieveCollections()
{
    // Resolve the well-known folder ids once per session, then fetch the folder tree.
    // The special-folder attributes are applied inline while delivering collections
    // (below), so the normal collection sync persists them — no extra modify jobs.
    if (mSpecialFolderIndex.isEmpty()) {
        QList<GraphBatchJob::Call> calls;
        for (const auto &sf : kSpecialFolders) {
            calls.append({GraphRequest::Get, QStringLiteral("/me/mailFolders/%1?$select=id").arg(QLatin1String(sf.wellKnownName)), {}});
        }
        auto job = new GraphBatchJob(mClient, calls, this);
        job->setIgnoreNotFound(true); // a mailbox may lack e.g. a real Outbox folder
        connect(job, &KJob::result, this, [this, job](KJob *kjob) {
            if (!kjob->error()) {
                const QList<QJsonObject> responses = job->responses();
                for (int i = 0; i < responses.size() && i < int(std::size(kSpecialFolders)); ++i) {
                    const QString id = responses.at(i).value(QLatin1String("id")).toString();
                    if (!id.isEmpty()) {
                        mSpecialFolderIndex.insert(id, i);
                        if (kSpecialFolders[i].type == SpecialMailCollections::SentMail) {
                            mSentFolderRemoteId = id;
                        }
                    }
                }
                qCDebug(GRAPH_LOG) << "resolved" << mSpecialFolderIndex.size() << "special folders";
            } else {
                qCWarning(GRAPH_LOG) << "special folder resolve failed:" << kjob->errorText();
            }
            fetchExtraCollections();
        });
        job->start();
    } else {
        fetchFolderTree();
    }
}

void GraphResource::fetchExtraCollections()
{
    // Calendars + a default contacts collection. Built once; delivered alongside the
    // mail folders on a full sync. Content mime types drive the retrieveItems dispatch.
    auto req = new GraphRequest(mClient, this);
    req->setPath(QStringLiteral("/me/calendars?$select=id,name,canEdit,isDefaultCalendar"));
    connect(req, &KJob::result, this, [this, req](KJob *job) {
        mExtraCollections.clear();
        if (!job->error()) {
            for (const auto &v : req->aggregatedValue()) {
                const QJsonObject cal = v.toObject();
                const QString id = cal.value(QLatin1String("id")).toString();
                if (id.isEmpty()) {
                    continue;
                }
                const bool canEdit = cal.value(QLatin1String("canEdit")).toBool();
                qCDebug(GRAPH_LOG) << "calendar" << cal.value(QLatin1String("name")).toString() << "canEdit" << canEdit << "default"
                                   << cal.value(QLatin1String("isDefaultCalendar")).toBool();
                Collection col;
                col.setRemoteId(id);
                col.setName(cal.value(QLatin1String("name")).toString());
                col.setContentMimeTypes({GraphEventHandler::mimeType()});
                // Read-only calendars advertise read rights only, so KOrganizer won't
                // even offer editing and no write is ever attempted.
                col.setRights(canEdit ? Collection::AllRights : Collection::Rights(Collection::ReadOnly));
                auto *display = col.attribute<EntityDisplayAttribute>(Collection::AddIfMissing);
                display->setIconName(QStringLiteral("view-calendar"));
                mExtraCollections.append(col);
            }
        } else {
            qCWarning(GRAPH_LOG) << "calendar list failed:" << job->errorText();
        }

        // One default contacts collection (/me/contacts).
        Collection contacts;
        contacts.setRemoteId(QStringLiteral("contacts-default"));
        contacts.setName(i18nc("@item mail folder", "Contacts"));
        contacts.setContentMimeTypes({GraphContactHandler::mimeType()});
        contacts.setRights(Collection::AllRights);
        contacts.attribute<EntityDisplayAttribute>(Collection::AddIfMissing)->setIconName(QStringLiteral("view-pim-contacts"));
        mExtraCollections.append(contacts);

        fetchTodoListCollections();
    });
    req->start();
}

void GraphResource::fetchTodoListCollections()
{
    // Task lists (Microsoft To Do): GET /me/todo/lists -> one collection per list.
    auto req = new GraphRequest(mClient, this);
    req->setPath(QStringLiteral("/me/todo/lists"));
    connect(req, &KJob::result, this, [this, req](KJob *job) {
        if (!job->error()) {
            for (const auto &v : req->aggregatedValue()) {
                const QJsonObject list = v.toObject();
                const QString id = list.value(QLatin1String("id")).toString();
                if (id.isEmpty()) {
                    continue;
                }
                Collection col;
                col.setRemoteId(id);
                col.setName(list.value(QLatin1String("displayName")).toString());
                col.setContentMimeTypes({GraphTodoHandler::mimeType()});
                col.setRights(Collection::AllRights);
                col.attribute<EntityDisplayAttribute>(Collection::AddIfMissing)->setIconName(QStringLiteral("view-calendar-tasks"));
                mExtraCollections.append(col);
            }
        } else {
            qCWarning(GRAPH_LOG) << "todo list fetch failed:" << job->errorText();
        }
        qCDebug(GRAPH_LOG) << "resolved" << mExtraCollections.size() << "calendar/contact/todo collections";
        fetchFolderTree();
    });
    req->start();
}

void GraphResource::fetchFolderTree()
{
    qCDebug(GRAPH_LOG) << "retrieveCollections, incremental:" << !mFolderDeltaLink.isEmpty();
    // GET /me/mailFolders/delta  (uses stored top-level deltaLink for incremental sync)
    auto job = new GraphFetchFoldersJob(mClient, mRootCollection, mFolderDeltaLink, this);
    connect(job, &KJob::result, this, &GraphResource::fetchFoldersJobFinished);
    job->start();
}

void GraphResource::applySpecialAttributes(Akonadi::Collection &col)
{
    const auto it = mSpecialFolderIndex.constFind(col.remoteId());
    if (it == mSpecialFolderIndex.constEnd()) {
        return;
    }
    const SpecialFolder &sf = kSpecialFolders[it.value()];
    col.attribute<SpecialCollectionAttribute>(Collection::AddIfMissing)->setCollectionType(QByteArray(sf.attributeType));
    auto *display = col.attribute<EntityDisplayAttribute>(Collection::AddIfMissing);
    if (display->iconName().isEmpty()) {
        display->setIconName(QLatin1String(sf.iconName));
    }
    if (!SpecialMailCollections::self()->registerCollection(sf.type, col)) {
        qCWarning(GRAPH_LOG) << "could not register special collection" << col.name() << "as" << sf.attributeType;
    }
    qCDebug(GRAPH_LOG) << "tagged special collection" << col.name() << "as" << sf.attributeType;
}

void GraphResource::fetchFoldersJobFinished(KJob *job)
{
    if (job->error()) {
        cancelTask(job->errorText());
        return;
    }
    auto *fj = qobject_cast<GraphFetchFoldersJob *>(job);
    mFolderDeltaLink = fj->deltaLink();
    mSettings->setFolderDeltaLink(mFolderDeltaLink);
    mSettings->save();

    // The job resolved the real msgfolderroot id; folders reference it as parent.
    mRootCollection = fj->rootCollection();
    mRootCollection.setName(name());

    qCDebug(GRAPH_LOG) << "folders fetched, incremental:" << fj->isIncremental() << "all:" << fj->allCollections().size()
                       << "changed:" << fj->changedCollections().size() << "removed:" << fj->removedCollections().size();

    if (fj->isIncremental()) {
        Collection::List changed = fj->changedCollections();
        for (Collection &col : changed) {
            applySpecialAttributes(col);
        }
        collectionsRetrievedIncremental(changed, fj->removedCollections());
    } else {
        Collection::List cols = fj->allCollections();
        for (Collection &col : cols) {
            applySpecialAttributes(col);
        }
        cols.prepend(mRootCollection);
        // Parent the calendar/contact collections under the account root and add them.
        for (Collection col : std::as_const(mExtraCollections)) {
            Collection parent;
            parent.setRemoteId(mRootCollection.remoteId());
            col.setParentCollection(parent);
            cols.append(col);
        }
        collectionsRetrieved(cols);
    }
}

void GraphResource::retrieveItems(const Akonadi::Collection &collection)
{
    const QStringList mimeTypes = collection.contentMimeTypes();
    if (mimeTypes.contains(GraphEventHandler::mimeType()) || mimeTypes.contains(GraphContactHandler::mimeType())
        || mimeTypes.contains(GraphTodoHandler::mimeType())) {
        // Calendar/contacts/tasks: delta change tracking, payloads delivered whole.
        const auto type = mimeTypes.contains(GraphEventHandler::mimeType()) ? GraphFetchPimItemsJob::Events
            : mimeTypes.contains(GraphTodoHandler::mimeType())              ? GraphFetchPimItemsJob::Todos
                                                                            : GraphFetchPimItemsJob::Contacts;
        auto job = new GraphFetchPimItemsJob(mClient, collection, type, collectionDeltaLink(collection), this);
        connect(job, &KJob::result, this, &GraphResource::fetchPimItemsJobFinished);
        job->start();
        return;
    }

    // Mail: GET /me/mailFolders/{id}/messages/delta  (per-collection deltaLink)
    const QString deltaLink = collectionDeltaLink(collection);
    auto job = new GraphFetchItemsJob(mClient, collection, deltaLink, this);
    connect(job, &KJob::result, this, &GraphResource::fetchItemsJobFinished);
    job->start();
}

void GraphResource::fetchItemsJobFinished(KJob *job)
{
    if (job->error()) {
        cancelTask(job->errorText());
        return;
    }
    auto *fj = qobject_cast<GraphFetchItemsJob *>(job);
    saveCollectionDeltaLink(fj->collection(), fj->deltaLink());

    // Delta returns light stubs (id/flags); payload is fetched on demand below.
    itemsRetrievedIncremental(fj->changedItems(), fj->removedItems());
}

void GraphResource::fetchPimItemsJobFinished(KJob *job)
{
    if (job->error()) {
        cancelTask(job->errorText());
        return;
    }
    auto *fj = qobject_cast<GraphFetchPimItemsJob *>(job);
    qCDebug(GRAPH_LOG) << "pim delta" << fj->collection().name() << "changed:" << fj->changedItems().size() << "removed:" << fj->removedItems().size()
                       << "deltaLink:" << (fj->deltaLink().isEmpty() ? "none" : "stored");
    saveCollectionDeltaLink(fj->collection(), fj->deltaLink());
    // Incremental: unmentioned items are kept, @removed tombstones are removed.
    itemsRetrievedIncremental(fj->changedItems(), fj->removedItems());
}

bool GraphResource::retrieveItems(const Akonadi::Item::List &items, [[maybe_unused]] const QSet<QByteArray> &parts)
{
    // Calendar/contact payloads are delivered whole during collection sync; if Akonadi
    // still asks (cache miss), the items already carry their payload, so just return them.
    if (!items.isEmpty()) {
        const QString mime = items.first().mimeType();
        if (mime == GraphEventHandler::mimeType() || mime == GraphContactHandler::mimeType() || mime == GraphTodoHandler::mimeType()) {
            if (items.first().hasPayload()) {
                itemsRetrieved(items);
                return true;
            }
            // Cache miss: re-fetch each requested event/contact/task individually and
            // rebuild its payload. (Rare — payloads are normally cached from the delta sync.)
            const bool isEvent = mime == GraphEventHandler::mimeType();
            const bool isTodo = mime == GraphTodoHandler::mimeType();
            auto pending = std::make_shared<Item::List>();
            auto remaining = std::make_shared<int>(items.size());
            for (const Item &item : items) {
                auto req = new GraphRequest(mClient, this);
                if (isEvent) {
                    req->setPath(QStringLiteral("/me/events/%1").arg(item.remoteId()));
                } else if (isTodo) {
                    req->setPath(QStringLiteral("/me/todo/lists/%1/tasks/%2").arg(item.parentCollection().remoteId(), item.remoteId()));
                } else {
                    req->setPath(QStringLiteral("/me/contacts/%1").arg(item.remoteId()));
                }
                if (isEvent) {
                    req->addHeader("Prefer", "outlook.timezone=\"UTC\"");
                }
                connect(req, &KJob::result, this, [this, item, req, isEvent, isTodo, pending, remaining](KJob *j) {
                    if (!j->error()) {
                        Item filled(item);
                        if (isEvent) {
                            auto ev = GraphEventHandler::toEvent(req->responseObject());
                            if (ev) {
                                filled.setPayload<KCalendarCore::Incidence::Ptr>(ev);
                            }
                        } else if (isTodo) {
                            auto todo = GraphTodoHandler::toTodo(req->responseObject());
                            if (todo) {
                                filled.setPayload<KCalendarCore::Incidence::Ptr>(todo);
                            }
                        } else {
                            filled.setPayload<KContacts::Addressee>(GraphContactHandler::toAddressee(req->responseObject()));
                        }
                        pending->append(filled);
                    }
                    if (--(*remaining) == 0) {
                        itemsRetrieved(*pending);
                    }
                });
                req->start();
            }
            return true;
        }
    }

    // Mail: GET /me/messages/{id}/$value  -> raw MIME -> KMime::Message
    auto job = new GraphFetchItemPayloadJob(mClient, items, this);
    connect(job, &KJob::result, this, &GraphResource::fetchPayloadJobFinished);
    job->start();
    return true;
}

void GraphResource::fetchPayloadJobFinished(KJob *job)
{
    if (job->error()) {
        qCWarning(GRAPH_LOG) << "payload fetch failed:" << job->errorText();
        cancelTask(job->errorText());
        return;
    }
    auto *fj = qobject_cast<GraphFetchItemPayloadJob *>(job);
    itemsRetrieved(fj->items());
}

// ============================================================================
//  Change replay: local -> Graph   (Phase 2/3)
// ============================================================================

void GraphResource::itemsFlagsChanged(const Item::List &items,
                                      [[maybe_unused]] const QSet<QByteArray> &addedFlags,
                                      [[maybe_unused]] const QSet<QByteArray> &removedFlags)
{
    // item.flags() already carries the final state, so the added/removed sets are not needed.
    QList<GraphBatchJob::Call> calls;
    calls.reserve(items.size());
    for (const Item &item : items) {
        calls.append({GraphRequest::Patch, QStringLiteral("/me/messages/%1").arg(item.remoteId()), GraphMailHandler::flagPatchBody(item)});
    }
    auto job = new GraphBatchJob(mClient, calls, this);
    connect(job, &KJob::result, this, [this, items](KJob *job) {
        if (job->error()) {
            cancelTask(job->errorText());
        } else {
            changesCommitted(items);
        }
    });
    job->start();
}

void GraphResource::itemChanged(const Item &item, [[maybe_unused]] const QSet<QByteArray> &partIdentifiers)
{
    const QString mime = item.mimeType();
    if (mime == GraphEventHandler::mimeType() && item.hasPayload<KCalendarCore::Incidence::Ptr>()) {
        auto incidence = item.payload<KCalendarCore::Incidence::Ptr>();
        auto event = incidence.dynamicCast<KCalendarCore::Event>();
        if (event) {
            patchPimItem(item, QStringLiteral("/me/events/%1").arg(item.remoteId()), GraphEventHandler::toJson(event));
            return;
        }
    } else if (mime == GraphContactHandler::mimeType() && item.hasPayload<KContacts::Addressee>()) {
        patchPimItem(item, QStringLiteral("/me/contacts/%1").arg(item.remoteId()), GraphContactHandler::toJson(item.payload<KContacts::Addressee>()));
        return;
    } else if (mime == GraphTodoHandler::mimeType() && item.hasPayload<KCalendarCore::Incidence::Ptr>()) {
        auto todo = item.payload<KCalendarCore::Incidence::Ptr>().dynamicCast<KCalendarCore::Todo>();
        if (todo) {
            patchPimItem(item,
                         QStringLiteral("/me/todo/lists/%1/tasks/%2").arg(item.parentCollection().remoteId(), item.remoteId()),
                         GraphTodoHandler::toJson(todo));
            return;
        }
    }
    // Mail: Graph cannot replace an existing message's MIME in place. Accept locally.
    changeProcessed();
}

void GraphResource::patchPimItem(const Akonadi::Item &item, const QString &path, const QJsonObject &body)
{
    auto req = new GraphRequest(mClient, this);
    req->setMethod(GraphRequest::Patch);
    req->setPath(path);
    req->setBody(body);
    connect(req, &KJob::result, this, [this, item](KJob *job) {
        if (job->error()) {
            cancelTask(job->errorText());
        } else {
            putContactPhotoThenCommit(item);
        }
    });
    req->start();
}

void GraphResource::itemAdded(const Item &item, const Collection &collection)
{
    const QString mime = item.mimeType();
    // Calendar event -> POST /me/calendars/{cal}/events
    if (mime == GraphEventHandler::mimeType()) {
        KCalendarCore::Event::Ptr event;
        if (item.hasPayload<KCalendarCore::Incidence::Ptr>()) {
            event = item.payload<KCalendarCore::Incidence::Ptr>().dynamicCast<KCalendarCore::Event>();
        } else if (item.hasPayload<KCalendarCore::Event::Ptr>()) {
            event = item.payload<KCalendarCore::Event::Ptr>();
        }
        if (event) {
            postPimItem(item, QStringLiteral("/me/calendars/%1/events").arg(collection.remoteId()), GraphEventHandler::toJson(event));
        } else {
            qCWarning(GRAPH_LOG) << "event itemAdded without usable payload";
            changeProcessed();
        }
        return;
    }
    // Contact -> POST /me/contacts (default folder)
    if (mime == GraphContactHandler::mimeType()) {
        if (item.hasPayload<KContacts::Addressee>()) {
            postPimItem(item, QStringLiteral("/me/contacts"), GraphContactHandler::toJson(item.payload<KContacts::Addressee>()));
        } else {
            changeProcessed();
        }
        return;
    }
    // Task -> POST /me/todo/lists/{list}/tasks
    if (mime == GraphTodoHandler::mimeType()) {
        KCalendarCore::Todo::Ptr todo;
        if (item.hasPayload<KCalendarCore::Incidence::Ptr>()) {
            todo = item.payload<KCalendarCore::Incidence::Ptr>().dynamicCast<KCalendarCore::Todo>();
        }
        if (todo) {
            postPimItem(item, QStringLiteral("/me/todo/lists/%1/tasks").arg(collection.remoteId()), GraphTodoHandler::toJson(todo));
        } else {
            qCWarning(GRAPH_LOG) << "todo itemAdded without usable payload";
            changeProcessed();
        }
        return;
    }

    // Server-side sent copy: Graph files the message into "Sent Items" itself when the
    // MTA calls /send. If KMail then also files an Fcc copy here, adopt the server's copy
    // (matched by Message-ID) instead of POSTing a duplicate.
    if (!mSentFolderRemoteId.isEmpty() && collection.remoteId() == mSentFolderRemoteId && item.hasPayload<std::shared_ptr<KMime::Message>>()) {
        const auto msg = item.payload<std::shared_ptr<KMime::Message>>();
        auto *midHeader = msg->messageID(KMime::DontCreate);
        const QString messageId = midHeader ? midHeader->asUnicodeString() : QString();
        if (!messageId.isEmpty()) {
            reconcileSentItem(item, messageId);
            return;
        }
    }

    const auto [rawMime, contentType] = GraphMailHandler::createFromMime(item);
    if (rawMime.isEmpty()) {
        changeProcessed();
        return;
    }
    // POST base64 MIME -> creates the message (as draft) in the target folder.
    auto req = new GraphRequest(mClient, this);
    req->setMethod(GraphRequest::Post);
    req->setPath(QStringLiteral("/me/mailFolders/%1/messages").arg(collection.remoteId()));
    req->setRawBody(rawMime, contentType);
    connect(req, &KJob::result, this, [this, item, req](KJob *job) {
        if (job->error()) {
            cancelTask(job->errorText());
            return;
        }
        Item newItem(item);
        newItem.setRemoteId(req->responseObject().value(QLatin1String("id")).toString());
        changeCommitted(newItem);
    });
    req->start();
}

void GraphResource::postPimItem(const Akonadi::Item &item, const QString &path, const QJsonObject &body)
{
    auto req = new GraphRequest(mClient, this);
    req->setMethod(GraphRequest::Post);
    req->setPath(path);
    req->setBody(body);
    connect(req, &KJob::result, this, [this, item, req](KJob *job) {
        if (job->error()) {
            cancelTask(job->errorText());
            return;
        }
        Item newItem(item);
        newItem.setRemoteId(req->responseObject().value(QLatin1String("id")).toString());
        putContactPhotoThenCommit(newItem);
    });
    req->start();
}

void GraphResource::putContactPhotoThenCommit(const Akonadi::Item &item)
{
    if (item.mimeType() != GraphContactHandler::mimeType() || !item.hasPayload<KContacts::Addressee>() || item.remoteId().isEmpty()) {
        changeCommitted(item);
        return;
    }
    const KContacts::Picture photo = item.payload<KContacts::Addressee>().photo();
    QByteArray data = photo.rawData();
    QByteArray contentType = "image/" + (photo.type().isEmpty() ? QByteArray("jpeg") : photo.type().toUtf8());
    if (data.isEmpty() && !photo.data().isNull()) {
        // Editor-set photos may carry only a QImage; encode it for the upload.
        QBuffer buffer(&data);
        buffer.open(QIODevice::WriteOnly);
        photo.data().save(&buffer, "PNG");
        contentType = QByteArrayLiteral("image/png");
    }
    if (data.isEmpty()) {
        // No local photo. Deliberately do not delete a server-side one.
        changeCommitted(item);
        return;
    }
    auto req = new GraphRequest(mClient, this);
    req->setMethod(GraphRequest::Put);
    req->setPath(QStringLiteral("/me/contacts/%1/photo/$value").arg(item.remoteId()));
    req->setRawBody(data, contentType);
    connect(req, &KJob::result, this, [this, item](KJob *job) {
        if (job->error()) {
            // Best effort: the contact itself is saved; a failed photo upload
            // should not fail the change replay.
            qCWarning(GRAPH_LOG) << "contact photo upload failed for" << item.remoteId() << ":" << job->errorText();
        }
        changeCommitted(item);
    });
    req->start();
}

void GraphResource::reconcileSentItem(const Akonadi::Item &item, const QString &messageId)
{
    // Find Graph's auto-filed copy in Sent Items by its Internet Message-ID.
    const QString filter = QStringLiteral("internetMessageId eq '%1'").arg(messageId);
    auto req = new GraphRequest(mClient, this);
    req->setPath(QStringLiteral("/me/mailFolders/sentitems/messages?$select=id&$top=1&$filter=%1").arg(QString::fromUtf8(QUrl::toPercentEncoding(filter))));
    connect(req, &KJob::result, this, [this, item, req](KJob *job) {
        if (job->error()) {
            cancelTask(job->errorText());
            return;
        }
        const auto values = req->aggregatedValue();
        if (!values.isEmpty()) {
            Item newItem(item);
            newItem.setRemoteId(values.first().toObject().value(QLatin1String("id")).toString());
            changeCommitted(newItem); // adopt the server copy; no duplicate created
        } else {
            // Auto-copy not visible yet — drop the local duplicate; the next delta
            // sync will bring the server's copy with its real id.
            changeProcessed();
        }
    });
    req->start();
}

void GraphResource::itemsMoved(const Item::List &items, [[maybe_unused]] const Collection &source, const Collection &destination)
{
    QList<GraphBatchJob::Call> calls;
    calls.reserve(items.size());
    QJsonObject body;
    body.insert(QStringLiteral("destinationId"), destination.remoteId());
    for (const Item &item : items) {
        calls.append({GraphRequest::Post, QStringLiteral("/me/messages/%1/move").arg(item.remoteId()), body});
    }
    auto job = new GraphBatchJob(mClient, calls, this);
    connect(job, &KJob::result, this, [this, items, job](KJob *kjob) {
        if (kjob->error()) {
            cancelTask(kjob->errorText());
            return;
        }
        // Graph assigns a new message id on move — push the new remote ids back.
        Item::List movedItems = items;
        const QList<QJsonObject> responses = job->responses();
        for (int i = 0; i < movedItems.size() && i < responses.size(); ++i) {
            const QString newId = responses.at(i).value(QLatin1String("id")).toString();
            if (!newId.isEmpty()) {
                movedItems[i].setRemoteId(newId);
            }
        }
        changesCommitted(movedItems);
    });
    job->start();
}

void GraphResource::itemsRemoved(const Item::List &items)
{
    // Calendar/contact/task deletes hit different endpoints (and never carry mail flags).
    const QString mime = items.isEmpty() ? QString() : items.first().mimeType();
    QString pimBase;
    if (mime == GraphEventHandler::mimeType()) {
        pimBase = QStringLiteral("/me/events/%1");
    } else if (mime == GraphContactHandler::mimeType()) {
        pimBase = QStringLiteral("/me/contacts/%1");
    }
    if (!pimBase.isEmpty() || mime == GraphTodoHandler::mimeType()) {
        QList<GraphBatchJob::Call> calls;
        for (const Item &item : items) {
            // Tasks are addressed through their list; the parent collection carries it.
            const QString path = pimBase.isEmpty() ? QStringLiteral("/me/todo/lists/%1/tasks/%2").arg(item.parentCollection().remoteId(), item.remoteId())
                                                   : pimBase.arg(item.remoteId());
            calls.append({GraphRequest::Delete, path, {}});
        }
        auto job = new GraphBatchJob(mClient, calls, this);
        job->setIgnoreNotFound(true);
        connect(job, &KJob::result, this, [this](KJob *j) {
            if (j->error()) {
                cancelTask(j->errorText());
            } else {
                changeProcessed();
            }
        });
        job->start();
        return;
    }

    QList<GraphBatchJob::Call> calls;
    calls.reserve(items.size());
    for (const Item &item : items) {
        // DELETE moves to Deleted Items' recoverable items; matches user expectation.
        calls.append({GraphRequest::Delete, QStringLiteral("/me/messages/%1").arg(item.remoteId()), {}});
    }
    auto job = new GraphBatchJob(mClient, calls, this);
    job->setIgnoreNotFound(true);
    connect(job, &KJob::result, this, [this](KJob *job) {
        if (job->error()) {
            cancelTask(job->errorText());
        } else {
            changeProcessed();
        }
    });
    job->start();
}

void GraphResource::collectionAdded(const Collection &collection, const Collection &parent)
{
    const bool topLevel = parent.remoteId() == mRootCollection.remoteId();
    const QString path = topLevel ? QStringLiteral("/me/mailFolders") : QStringLiteral("/me/mailFolders/%1/childFolders").arg(parent.remoteId());
    QJsonObject body;
    body.insert(QStringLiteral("displayName"), collection.name());

    auto req = new GraphRequest(mClient, this);
    req->setMethod(GraphRequest::Post);
    req->setPath(path);
    req->setBody(body);
    connect(req, &KJob::result, this, [this, collection, req](KJob *job) {
        if (job->error()) {
            cancelTask(job->errorText());
            return;
        }
        Collection col(collection);
        col.setRemoteId(req->responseObject().value(QLatin1String("id")).toString());
        changeCommitted(col);
    });
    req->start();
}

void GraphResource::collectionChanged(const Collection &collection, const QSet<QByteArray> &changedAttributes)
{
    if (!changedAttributes.contains("NAME")) {
        changeProcessed(); // only renames are propagated to Graph
        return;
    }
    QJsonObject body;
    body.insert(QStringLiteral("displayName"), collection.name());
    auto req = new GraphRequest(mClient, this);
    req->setMethod(GraphRequest::Patch);
    req->setPath(QStringLiteral("/me/mailFolders/%1").arg(collection.remoteId()));
    req->setBody(body);
    connect(req, &KJob::result, this, [this, collection](KJob *job) {
        if (job->error()) {
            cancelTask(job->errorText());
        } else {
            changeCommitted(collection);
        }
    });
    req->start();
}

void GraphResource::collectionMoved(const Collection &collection, [[maybe_unused]] const Collection &source, const Collection &destination)
{
    QJsonObject body;
    body.insert(QStringLiteral("destinationId"), destination.remoteId());
    auto req = new GraphRequest(mClient, this);
    req->setMethod(GraphRequest::Post);
    req->setPath(QStringLiteral("/me/mailFolders/%1/move").arg(collection.remoteId()));
    req->setBody(body);
    connect(req, &KJob::result, this, [this, collection, req](KJob *job) {
        if (job->error()) {
            cancelTask(job->errorText());
            return;
        }
        Collection col(collection);
        const QString newId = req->responseObject().value(QLatin1String("id")).toString();
        if (!newId.isEmpty()) {
            col.setRemoteId(newId);
        }
        changeCommitted(col);
    });
    req->start();
}

void GraphResource::collectionRemoved(const Collection &collection)
{
    auto req = new GraphRequest(mClient, this);
    req->setMethod(GraphRequest::Delete);
    req->setPath(QStringLiteral("/me/mailFolders/%1").arg(collection.remoteId()));
    connect(req, &KJob::result, this, [this, req](KJob *job) {
        if (job->error() && req->httpStatus() != 404) {
            cancelTask(job->errorText());
        } else {
            changeProcessed();
        }
    });
    req->start();
}

// ============================================================================
//  Sending (called via D-Bus from graphmtaresource)
// ============================================================================

void GraphResource::sendItem([[maybe_unused]] const Item &item)
{
    // Sending goes through the MTA agent -> sendMessage(); nothing to do here.
}

void GraphResource::sendMessage(const QString &id, const QByteArray &content)
{
    // 1. Create a draft from the raw MIME (base64, Content-Type: text/plain)...
    auto createReq = new GraphRequest(mClient, this);
    createReq->setMethod(GraphRequest::Post);
    createReq->setPath(QStringLiteral("/me/messages"));
    createReq->setRawBody(content.toBase64(), QByteArrayLiteral("text/plain"));
    connect(createReq, &KJob::result, this, [this, id, createReq](KJob *job) {
        if (job->error()) {
            Q_EMIT messageSent(id, job->errorText());
            return;
        }
        const QString draftId = createReq->responseObject().value(QLatin1String("id")).toString();
        // 2. ...then send it.
        auto sendReq = new GraphRequest(mClient, this);
        sendReq->setMethod(GraphRequest::Post);
        sendReq->setPath(QStringLiteral("/me/messages/%1/send").arg(draftId));
        connect(sendReq, &KJob::result, this, [this, id](KJob *job) {
            Q_EMIT messageSent(id, job->error() ? job->errorText() : QString());
        });
        sendReq->start();
    });
    createReq->start();
}

void GraphResource::clearFolderSyncState()
{
    mFolderDeltaLink.clear();
    mSettings->setFolderDeltaLink(QString());
    mSettings->save();

    // Also drop the per-collection message deltaLinks so the next sync re-lists
    // every message (items are merged by remoteId, nothing is duplicated).
    auto fetch = new CollectionFetchJob(Collection::root(), CollectionFetchJob::Recursive, this);
    fetch->fetchScope().setResource(identifier());
    connect(fetch, &CollectionFetchJob::collectionsReceived, this, [this](const Collection::List &cols) {
        for (const Collection &col : cols) {
            if (col.hasAttribute<GraphSyncStateAttribute>()) {
                Collection modified(col);
                modified.removeAttribute<GraphSyncStateAttribute>();
                (new CollectionModifyJob(modified, this))->start();
            }
        }
    });
}

// ----- delta-link (sync state) helpers -----
QString GraphResource::collectionDeltaLink(const Collection &col)
{
    const auto *attr = col.attribute<GraphSyncStateAttribute>();
    return attr ? attr->deltaLink() : QString();
}

void GraphResource::saveCollectionDeltaLink(Collection col, const QString &deltaLink)
{
    col.addAttribute(new GraphSyncStateAttribute(deltaLink));
    auto job = new CollectionModifyJob(col, this);
    job->start();
}

AKONADI_RESOURCE_MAIN(GraphResource)

#include "moc_graphresource.cpp"
