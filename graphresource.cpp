/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "graphresource.h"

#include "graph_debug.h"
#include "graphclient/auth/graphoauth.h"
#include "graphresourceadaptor.h"
#include "graphretrypolicy.h"
#include "graphsettingsbase.h"
#include "graphsyncstateattribute.h"

#include "calendar/grapheventhandler.h"
#include "contact/graphcontacthandler.h"
#include "jobs/graphbatchjob.h"
#include "jobs/graphfetchfoldersjob.h"
#include "jobs/graphfetchitempayloadjob.h"
#include "jobs/graphfetchitemsjob.h"
#include "jobs/graphfetchpimitemsjob.h"
#include "jobs/graphmigrateidsjob.h"
#include "mail/graphmailhandler.h"
#include "todo/graphtodohandler.h"

#include <KContacts/Addressee>

#include <Akonadi/AttributeFactory>
#include <Akonadi/ChangeRecorder>
#include <Akonadi/CollectionFetchJob>
#include <Akonadi/CollectionFetchScope>
#include <Akonadi/CollectionModifyJob>
#include <Akonadi/EntityDisplayAttribute>
#include <Akonadi/ItemFetchJob>
#include <Akonadi/ItemFetchScope>
#include <Akonadi/ItemMoveJob>
#include <Akonadi/ItemSync>
#include <Akonadi/SpecialCollectionAttribute>
#include <Akonadi/SpecialMailCollections>
#include <KLocalizedString>
#include <KMime/Message>
#include <QBuffer>
#include <QCryptographicHash>
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
constexpr int kMillisecondsPerMinute = 60 * 1000;
// How soon to try signing in again when Microsoft 365 could not be reached.
constexpr int kSignInRetrySeconds = 60;

// Graph uses a different endpoint family per collection type; mail is the default.
enum class CollectionKind {
    Mail,
    Calendar,
    Contacts,
    Todo,
};

CollectionKind collectionKind(const Akonadi::Collection &col)
{
    const QStringList mimes = col.contentMimeTypes();
    if (mimes.contains(GraphEventHandler::mimeType())) {
        return CollectionKind::Calendar;
    }
    if (mimes.contains(GraphContactHandler::mimeType())) {
        return CollectionKind::Contacts;
    }
    if (mimes.contains(GraphTodoHandler::mimeType())) {
        return CollectionKind::Todo;
    }
    return CollectionKind::Mail;
}
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

    // ResourceBase has already put the task scheduler online. Hold it back until the
    // first token has arrived (startSyncing() releases it): Akonadi starts replaying
    // queued changes right away, and every one of them would fail without a token.
    ResourceWidgetBase::doSetOnline(false);
    // Queued: KJob reports finished() before result(), and the result handler of the
    // last request must run before the scheduler resumes.
    mClient.setIdleCallback([this] {
        if (mResumeWhenIdle) {
            QMetaObject::invokeMethod(
                this,
                [this] {
                    if (mResumeWhenIdle && isOnline() && !mAuthPending) {
                        resumeScheduler();
                    }
                },
                Qt::QueuedConnection);
        }
    });

    QMetaObject::invokeMethod(this, &GraphResource::delayedInit, Qt::QueuedConnection);
}

GraphResource::~GraphResource()
{
    // Jobs still running are deleted after this object's members; their end must not
    // call back into a resource that is going away.
    mClient.setIdleCallback({});
}

void GraphResource::delayedInit()
{
    // Seed the known calendar/contact/todo ids from the local cache so a collection
    // deleted on the server while the resource was not running is still cleaned up.
    auto fetch = new CollectionFetchJob(Collection::root(), CollectionFetchJob::Recursive, this);
    fetch->fetchScope().setResource(identifier());
    connect(fetch, &CollectionFetchJob::result, this, [this](KJob *job) {
        if (job->error()) {
            qCWarning(GRAPH_LOG) << "seeding known collections failed:" << job->errorText();
            return;
        }
        const auto cols = qobject_cast<CollectionFetchJob *>(job)->collections();
        for (const Collection &col : cols) {
            if (collectionKind(col) != CollectionKind::Mail) {
                mKnownExtraIds.insert(col.remoteId());
            }
        }
    });

    // doSetOnline(true) may already have started signing in.
    if (!mAuthPending) {
        setUpAuth();
    }
}

void GraphResource::setUpAuth()
{
    // OAuth2 (Auth Code + PKCE) against login.microsoftonline.com, Graph scopes.
    // Silent refresh via the keychain-stored refresh token; interactive browser
    // login only when that fails. Tokens are persisted via QtKeychain/KWallet.
    mAuthPending = true;
    mReauthenticate = false;
    mAuth.reset(new GraphOAuth(mSettings->tenantId(), mSettings->clientId(), identifier(), this));
    connect(mAuth.data(), &GraphOAuth::ready, this, &GraphResource::onAuthReady);
    connect(mAuth.data(), &GraphOAuth::failed, this, &GraphResource::onAuthFailed);
    connect(mAuth.data(), &GraphOAuth::unreachable, this, &GraphResource::onAuthUnreachable);
    // Repoint the client before any request can run again: doSetOnline(true) re-enters
    // here while a poll sync or throttling retry may still be in flight, and such a
    // request would otherwise dereference the just-destroyed auth object in
    // GraphRequest::issue(). With the fresh (token-less) auth it gets a 401 instead.
    reconfigureClient();
    Q_EMIT status(Running, i18nc("@info:status", "Authenticating with Microsoft 365"));
    mAuth->authenticate();
}

void GraphResource::onAuthReady()
{
    qCDebug(GRAPH_LOG) << "auth ready, starting collection tree sync";
    mAuthPending = false;
    mReauthenticate = false;
    reconfigureClient();

    // Every request now asks Graph for immutable ids (see GraphRequest::issue), so
    // remoteIds stored by earlier versions must be translated first — a delta that
    // returns unknown id forms would duplicate the whole mailbox. Runs once; a fresh
    // account has nothing to translate and just sets the flag.
    if (!mSettings->immutableIdsMigrated()) {
        Q_EMIT status(Running, i18nc("@info:status", "Migrating item identifiers"));
        auto job = new GraphMigrateIdsJob(mClient, identifier(), this);
        connect(job, &KJob::result, this, [this](KJob *job) {
            if (job->error()) {
                qCWarning(GRAPH_LOG) << "id migration failed:" << job->errorText();
                Q_EMIT status(Broken, i18nc("@info:status", "Failed to migrate item identifiers: %1", job->errorText()));
                // Retry on the poll interval; syncing before the migration is done is
                // blocked in retrieveCollections()/retrieveItems().
                QTimer::singleShot(qMax(1, mSettings->pollInterval()) * kMillisecondsPerMinute, this, &GraphResource::onAuthReady);
                return;
            }
            mSettings->setImmutableIdsMigrated(true);
            mSettings->save();
            startSyncing();
        });
        job->start();
        return;
    }
    startSyncing();
}

void GraphResource::startSyncing()
{
    Q_EMIT status(Idle, i18nc("@info:status", "Ready"));
    // Release the task scheduler held back until now: requests carry a token from here on.
    if (isOnline()) {
        resumeScheduler();
    }
    synchronizeCollectionTree();

    // No push notifications on Graph for desktop clients — poll with delta queries.
    if (!mPollTimer) {
        mPollTimer = new QTimer(this);
        connect(mPollTimer, &QTimer::timeout, this, [this] {
            // After a suspend the token may have run out while the renewal timer stood
            // still; every poll would fail with 401 until it fires.
            if (mAuth && !mAuthPending && !mAuth->hasValidToken()) {
                signInAgain();
                return;
            }
            synchronize();
        });
    }
    if (mSettings->pollInterval() > 0) {
        mPollTimer->start(mSettings->pollInterval() * kMillisecondsPerMinute);
    }
}

void GraphResource::onAuthFailed(const QString &error)
{
    qCWarning(GRAPH_LOG) << "auth failed:" << error;
    mAuthPending = false;
    // Go offline like the IMAP resource does after a failed login: item requests then
    // fail at once instead of waiting for a scheduler that has no token to work with,
    // queued changes are kept, and switching the account online signs in again.
    setOnline(false);
    Q_EMIT status(Broken, i18nc("@info:status", "Authentication failed: %1", error));
}

void GraphResource::onAuthUnreachable()
{
    mAuthPending = false;
    if (mAuth && mAuth->hasValidToken()) {
        return; // a background renewal; the current token still works for a while
    }
    qCInfo(GRAPH_LOG) << "Microsoft 365 not reachable, signing in again in" << kSignInRetrySeconds << "s";
    // Try again shortly. Without a network the agent is offline already and nothing
    // happens here; it signs in from doSetOnline(true) once the network returns.
    setTemporaryOffline(kSignInRetrySeconds);
    Q_EMIT status(Idle, i18nc("@info:status", "Microsoft 365 is not reachable, trying again shortly"));
}

void GraphResource::resumeScheduler()
{
    // A request started before going offline may still be out. Akonadi has put its
    // change back at the head of the queue: replaying it now could send it a second
    // time, and the late answer would complete whichever change runs at that moment.
    if (mClient.isBusy()) {
        mResumeWhenIdle = true;
        return;
    }
    mResumeWhenIdle = false;
    ResourceWidgetBase::doSetOnline(true);
}

bool GraphResource::isStale(quint64 generation) const
{
    return generation != mTaskGeneration;
}

bool GraphResource::startReplay(qint64 change, Replay &replay)
{
    replay = {mTaskGeneration, change};
    const auto late = mLateOutcomes.constFind(change);
    if (late == mLateOutcomes.constEnd()) {
        return true;
    }
    qCDebug(GRAPH_LOG) << "replay: applying the outcome of an earlier run of change" << change;
    const std::function<void()> finish = late.value();
    mLateOutcomes.erase(late);
    finish();
    return false;
}

void GraphResource::finishReplay(const Replay &replay, const std::function<void()> &finish)
{
    if (isStale(replay.generation)) {
        // Akonadi queued the change again and the scheduler waits for this answer;
        // the next run applies what this one achieved instead of sending it twice.
        qCDebug(GRAPH_LOG) << "replay: change" << replay.change << "finished after it was given up, keeping the outcome for its next run";
        mLateOutcomes.insert(replay.change, finish);
        return;
    }
    mLateOutcomes.remove(replay.change);
    if (replay.change == mRetriedChange) {
        // Through at last: a later change to the same item starts afresh.
        mRetriedChange = 0;
        mReplayRetries = 0;
    }
    finish();
}

void GraphResource::commitItem(const Replay &replay, const Akonadi::Item &item)
{
    finishReplay(replay, [this, item] {
        changeCommitted(item);
    });
}

void GraphResource::commitItems(const Replay &replay, const Akonadi::Item::List &items)
{
    finishReplay(replay, [this, items] {
        changesCommitted(items);
    });
}

void GraphResource::commitCollection(const Replay &replay, const Akonadi::Collection &collection)
{
    finishReplay(replay, [this, collection] {
        changeCommitted(collection);
    });
}

void GraphResource::skipChange(const Replay &replay)
{
    finishReplay(replay, [this] {
        changeProcessed();
    });
}

void GraphResource::failChange(const Replay &replay, const QString &message)
{
    finishReplay(replay, [this, message] {
        cancelTask(message);
    });
}

void GraphResource::trackWork(KJob *job)
{
    connect(job, &KJob::finished, this, [done = mClient.beginWork()] {
        done();
    });
}

void GraphResource::holdScheduler()
{
    // The running task goes back to the head of its queue; whatever it still delivers
    // is stale now (see isStale() and resumeScheduler()).
    ++mTaskGeneration;
    ResourceWidgetBase::doSetOnline(false);
    // A task the scheduler started anyway (its next step does not check whether it is
    // online) goes back too; without one this does nothing.
    deferTask();
    // ResourceBase does not stop the jobs it attached to the given-up task (a commit, a
    // payload store): they finish and then complete whichever task runs by then. Keep
    // the scheduler held until they are through; the session runs its jobs in order,
    // so this one ends after them.
    trackWork(new CollectionFetchJob(Collection::root(), CollectionFetchJob::Base, this));
}

void GraphResource::signInAgain()
{
    // Hold the scheduler back: startSyncing() releases it once the new token is there.
    holdScheduler();
    if (!mAuthPending) {
        setUpAuth();
    }
}

void GraphResource::reconfigureClient()
{
    mClient.setBaseUrl(QStringLiteral("https://graph.microsoft.com/v1.0"));
    mClient.setAuth(mAuth.data());
}

void GraphResource::doSetOnline(bool online)
{
    if (!online) {
        holdScheduler();
        if (mPollTimer) {
            mPollTimer->stop();
        }
        return;
    }
    if (mAuth && !mAuthPending && !mReauthenticate && mAuth->hasValidToken() && mSettings->immutableIdsMigrated()) {
        // Back after a held-back replay or a network change, with a token that is
        // still good (GraphOAuth renews it in the background, but not during suspend):
        // carry on where we were, without signing in again or re-listing the folder tree.
        resumeScheduler();
        if (mPollTimer && mSettings->pollInterval() > 0) {
            mPollTimer->start(mSettings->pollInterval() * kMillisecondsPerMinute);
        }
        return;
    }
    signInAgain();
}

// Graph drops a second POST of an event with the same transactionId, which covers a
// create that goes out again after its answer got lost. Not documented as a lasting
// guarantee, so it backs up retryLater()'s caution rather than replacing it. Stable
// across the attempts of one change, different per target calendar and per move
// (the current server id changes with each), so that moving an event back to a
// calendar it was in never repeats an id.
static QString eventTransactionId(const Item &item, const Collection &target)
{
    const QByteArray key = QByteArray::number(item.id()) + '/' + item.remoteId().toUtf8() + '/' + target.remoteId().toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex());
}

// Identifies a change for retryLater(): the Akonadi id of its first item.
static qint64 changeId(const Item::List &items)
{
    return items.isEmpty() ? 0 : items.constFirst().id();
}

bool GraphResource::retryLater(KJob *job, const Replay &replay, bool repeatable, bool applied)
{
    if (!job || !job->error()) {
        return false; // finishReplay() resets the attempt count
    }
    int notExecuted = 0;
    int uncertain = 0;
    int permanent = 0;
    bool authRejected = false;
    if (auto batch = qobject_cast<GraphBatchJob *>(job)) {
        notExecuted = batch->failed(GraphRequest::Failure::NotExecuted);
        uncertain = batch->failed(GraphRequest::Failure::Uncertain);
        permanent = batch->failed(GraphRequest::Failure::Permanent);
        authRejected = batch->authenticationRejected();
        applied = applied || batch->succeeded() > 0;
    } else if (auto req = qobject_cast<GraphRequest *>(job)) {
        switch (req->failure()) {
        case GraphRequest::Failure::None:
            break;
        case GraphRequest::Failure::NotExecuted:
            notExecuted = 1;
            break;
        case GraphRequest::Failure::Uncertain:
            uncertain = 1;
            break;
        case GraphRequest::Failure::Permanent:
            permanent = 1;
            break;
        }
        authRejected = req->authenticationRejected();
    }
    const qint64 change = replay.change;
    if (isStale(replay.generation)) {
        // The change is queued again and goes out once the scheduler resumes, which
        // waits for this answer. Keep it so if it may go out again; otherwise the
        // caller's failChange() drops it on that run.
        return GraphRetryPolicy::decide(repeatable, applied, notExecuted, uncertain, permanent, 0).retry;
    }
    if (change != mRetriedChange) {
        mRetriedChange = change;
        mReplayRetries = 0;
    }
    const GraphRetryPolicy::Decision decision = GraphRetryPolicy::decide(repeatable, applied, notExecuted, uncertain, permanent, mReplayRetries);
    if (!decision.retry) {
        if (mReplayRetries > 0) {
            qCWarning(GRAPH_LOG) << "replay: giving up on change" << change << "after" << mReplayRetries << "attempts:" << job->errorText();
        }
        mRetriedChange = 0;
        mReplayRetries = 0;
        return false;
    }
    ++mReplayRetries;
    mReauthenticate = mReauthenticate || authRejected;
    qCWarning(GRAPH_LOG) << "replay: keeping change" << change << "for another attempt in" << decision.delaySeconds << "s:" << job->errorText();
    // Going offline aborts the running task and puts it back at the head of its queue;
    // doSetOnline(true) resumes afterwards (signing in again only after a 401).
    setTemporaryOffline(decision.delaySeconds);
    // When the agent was offline already, that did nothing; the scheduler may still
    // have started this task (its next step does not check), so put it back here.
    deferTask();
    // After going offline, which resets the status message.
    Q_EMIT status(Idle,
                  i18ncp("@info:status",
                         "Microsoft 365 did not take a change, retrying in %1 second",
                         "Microsoft 365 did not take a change, retrying in %1 seconds",
                         decision.delaySeconds));
    return true;
}

void GraphResource::announceReplay(GraphBatchJob *job, int count)
{
    // Called before the caller connects its own result handler, so "Ready" is out
    // before that handler reports an error or holds the change back.
    Q_EMIT status(Running, i18ncp("@info:status", "Applying %1 change", "Applying %1 changes", count));
    connect(job, &KJob::percentChanged, this, [this](KJob *, unsigned long progress) {
        Q_EMIT percent(static_cast<int>(progress));
    });
    connect(job, &KJob::result, this, [this] {
        Q_EMIT status(Idle, i18nc("@info:status", "Ready"));
    });
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
        signInAgain();
    } else if (mPollTimer) {
        if (mSettings->pollInterval() > 0) {
            mPollTimer->start(mSettings->pollInterval() * kMillisecondsPerMinute);
        } else {
            mPollTimer->stop();
        }
    }
}

// ============================================================================
//  Synchronisation: Graph -> local
// ============================================================================

void GraphResource::retrieveCollections()
{
    // Syncing with un-migrated remoteIds would duplicate everything (see onAuthReady).
    if (!mSettings->immutableIdsMigrated()) {
        cancelTask(i18n("Waiting for the item identifier migration to finish"));
        return;
    }
    // Resolve the well-known folder ids once per session, then fetch the folder tree.
    // The special-folder attributes are applied inline while delivering collections
    // (below), so the normal collection sync persists them — no extra modify jobs.
    const quint64 generation = mTaskGeneration;
    if (mSpecialFolderIndex.isEmpty()) {
        QList<GraphBatchJob::Call> calls;
        for (const auto &sf : kSpecialFolders) {
            calls.append({GraphRequest::Method::Get, QStringLiteral("/me/mailFolders/%1?$select=id").arg(QLatin1String(sf.wellKnownName)), {}});
        }
        auto job = new GraphBatchJob(mClient, calls, this);
        job->setIgnoreNotFound(true); // a mailbox may lack e.g. a real Outbox folder
        connect(job, &KJob::result, this, [this, job, generation](KJob *kjob) {
            if (isStale(generation)) {
                return;
            }
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
            fetchExtraCollections(generation);
        });
        job->start();
    } else {
        // Special-folder ids are stable per mailbox; the calendar/contact/todo
        // collections are cheap to list and must be re-fetched so server-side
        // additions, renames and deletions show up without a resource restart.
        fetchExtraCollections(generation);
    }
}

void GraphResource::fetchExtraCollections(quint64 generation)
{
    // Calendars + a default contacts collection, re-fetched on every tree sync and
    // delivered alongside the mail folders. Content mime types drive the
    // retrieveItems dispatch.
    auto req = new GraphRequest(mClient, this);
    req->setPath(QStringLiteral("/me/calendars?$select=id,name,canEdit,isDefaultCalendar"));
    // Calendar ids become collection remoteIds and must stay in the traditional form —
    // IdType would rewrite them and the tree sync would see all calendars as replaced.
    req->setUseImmutableIds(false);
    connect(req, &KJob::result, this, [this, req, generation](KJob *job) {
        if (isStale(generation)) {
            return;
        }
        if (job->error()) {
            // Keep the previous set: replacing it with a partial one would make the
            // known-ids diff report the missing collections as deleted on the server.
            qCWarning(GRAPH_LOG) << "calendar list failed, keeping cached collections:" << job->errorText();
            fetchFolderTree(generation);
            return;
        }
        Collection::List fresh;
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
            // even offer editing and no write is ever attempted. Editable ones can
            // be renamed/deleted (/me/calendars) but cannot contain subfolders.
            const Collection::Rights editable = Collection::CanChangeItem | Collection::CanCreateItem | Collection::CanDeleteItem
                | Collection::CanChangeCollection | Collection::CanDeleteCollection;
            col.setRights(canEdit ? editable : Collection::Rights(Collection::ReadOnly));
            auto *display = col.attribute<EntityDisplayAttribute>(Collection::AddIfMissing);
            display->setIconName(QStringLiteral("view-calendar"));
            fresh.append(col);
        }

        // One default contacts collection (/me/contacts).
        Collection contacts;
        contacts.setRemoteId(QStringLiteral("contacts-default"));
        contacts.setName(i18nc("@item mail folder", "Contacts"));
        contacts.setContentMimeTypes({GraphContactHandler::mimeType()});
        // A single built-in folder: items are editable, the folder itself is not.
        contacts.setRights(Collection::CanChangeItem | Collection::CanCreateItem | Collection::CanDeleteItem);
        contacts.attribute<EntityDisplayAttribute>(Collection::AddIfMissing)->setIconName(QStringLiteral("view-pim-contacts"));
        fresh.append(contacts);

        fetchTodoListCollections(fresh, generation);
    });
    req->start();
}

void GraphResource::fetchTodoListCollections(Akonadi::Collection::List fresh, quint64 generation)
{
    // Task lists (Microsoft To Do): GET /me/todo/lists -> one collection per list.
    auto req = new GraphRequest(mClient, this);
    req->setPath(QStringLiteral("/me/todo/lists"));
    connect(req, &KJob::result, this, [this, req, fresh, generation](KJob *job) mutable {
        if (isStale(generation)) {
            return;
        }
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
                // Lists can be renamed/deleted (/me/todo/lists) but have no subfolders.
                col.setRights(Collection::CanChangeItem | Collection::CanCreateItem | Collection::CanDeleteItem | Collection::CanChangeCollection
                              | Collection::CanDeleteCollection);
                col.attribute<EntityDisplayAttribute>(Collection::AddIfMissing)->setIconName(QStringLiteral("view-calendar-tasks"));
                fresh.append(col);
            }
            mExtraCollections = fresh;
            qCDebug(GRAPH_LOG) << "resolved" << mExtraCollections.size() << "calendar/contact/todo collections";
        } else {
            // Keep the previous set — see fetchExtraCollections().
            qCWarning(GRAPH_LOG) << "todo list fetch failed, keeping cached collections:" << job->errorText();
        }
        fetchFolderTree(generation);
    });
    req->start();
}

void GraphResource::fetchFolderTree(quint64 generation)
{
    qCDebug(GRAPH_LOG) << "retrieveCollections, incremental:" << !mFolderDeltaLink.isEmpty();
    // GET /me/mailFolders/delta  (uses stored top-level deltaLink for incremental sync)
    auto job = new GraphFetchFoldersJob(mClient, mRootCollection, mFolderDeltaLink, this);
    connect(job, &KJob::result, this, [this, generation](KJob *job) {
        fetchFoldersJobFinished(job, generation);
    });
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

void GraphResource::fetchFoldersJobFinished(KJob *job, quint64 generation)
{
    // Before the deltaLink is saved: the changes it covers would never be delivered.
    if (isStale(generation)) {
        return;
    }
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

    // Parent the calendar/contact/todo collections under the account root. They are
    // re-fetched fresh on every tree sync, so deliver them as changed in the
    // incremental case too — otherwise a newly created list would never show up.
    Collection::List extra;
    for (Collection col : std::as_const(mExtraCollections)) {
        Collection parent;
        parent.setRemoteId(mRootCollection.remoteId());
        col.setParentCollection(parent);
        extra.append(col);
    }

    // The extra collections come from plain list requests, not a delta — track their
    // ids so a calendar/list deleted on the server is reported as removed too.
    QSet<QString> currentExtraIds;
    for (const Collection &col : std::as_const(extra)) {
        currentExtraIds.insert(col.remoteId());
    }

    if (fj->isIncremental()) {
        Collection::List changed = fj->changedCollections();
        for (Collection &col : changed) {
            applySpecialAttributes(col);
        }
        changed.append(extra);
        Collection::List removed = fj->removedCollections();
        for (const QString &gone : mKnownExtraIds - currentExtraIds) {
            Collection col;
            col.setRemoteId(gone);
            removed.append(col);
        }
        if (removed.isEmpty()) {
            collectionsRetrievedIncremental(changed, removed);
        } else {
            deliverIncrementalTree(changed, removed, generation);
        }
    } else {
        Collection::List cols = fj->allCollections();
        for (Collection &col : cols) {
            applySpecialAttributes(col);
        }
        cols.prepend(mRootCollection);
        cols.append(extra);
        collectionsRetrieved(cols);
    }
    mKnownExtraIds = currentExtraIds;
}

void GraphResource::deliverIncrementalTree(const Akonadi::Collection::List &changed, const Akonadi::Collection::List &removed, quint64 generation)
{
    // CollectionSync matches a removed collection only inside its parent's bucket, so
    // a tombstone carrying just the remote id is silently dropped. Resolve each one to
    // the locally cached collection (whose parent chain is intact) before delivering.
    auto fetch = new CollectionFetchJob(Collection::root(), CollectionFetchJob::Recursive, this);
    fetch->fetchScope().setResource(identifier());
    fetch->fetchScope().setAncestorRetrieval(CollectionFetchScope::Parent);
    connect(fetch, &CollectionFetchJob::result, this, [this, changed, removed, generation](KJob *job) {
        if (isStale(generation)) {
            return;
        }
        Collection::List resolved;
        if (job->error()) {
            qCWarning(GRAPH_LOG) << "could not resolve removed collections:" << job->errorText();
        } else {
            QHash<QString, Collection> byRid;
            const auto locals = qobject_cast<CollectionFetchJob *>(job)->collections();
            for (const Collection &col : locals) {
                byRid.insert(col.remoteId(), col);
            }
            for (const Collection &col : removed) {
                const auto it = byRid.constFind(col.remoteId());
                if (it != byRid.constEnd()) {
                    resolved.append(*it);
                } // not found locally -> already gone
            }
            qCDebug(GRAPH_LOG) << "resolved" << resolved.size() << "of" << removed.size() << "removed collections";
        }
        collectionsRetrievedIncremental(changed, resolved);
    });
}

void GraphResource::retrieveItems(const Akonadi::Collection &collection)
{
    if (!mSettings->immutableIdsMigrated()) {
        cancelTask(i18n("Waiting for the item identifier migration to finish"));
        return;
    }
    const QStringList mimeTypes = collection.contentMimeTypes();
    if (mimeTypes.contains(GraphEventHandler::mimeType()) || mimeTypes.contains(GraphContactHandler::mimeType())
        || mimeTypes.contains(GraphTodoHandler::mimeType())) {
        // Calendar/contacts/tasks: delta change tracking, payloads delivered whole.
        const auto type = mimeTypes.contains(GraphEventHandler::mimeType()) ? GraphFetchPimItemsJob::Type::Events
            : mimeTypes.contains(GraphTodoHandler::mimeType())              ? GraphFetchPimItemsJob::Type::Todos
                                                                            : GraphFetchPimItemsJob::Type::Contacts;
        auto job = new GraphFetchPimItemsJob(mClient, collection, type, collectionDeltaLink(collection), this);
        connect(job, &KJob::result, this, [this, generation = mTaskGeneration](KJob *job) {
            fetchPimItemsJobFinished(job, generation);
        });
        job->start();
        return;
    }

    // Mail: GET /me/mailFolders/{id}/messages/delta  (per-collection deltaLink)
    const QString deltaLink = collectionDeltaLink(collection);
    auto job = new GraphFetchItemsJob(mClient, collection, deltaLink, this);
    connect(job, &KJob::result, this, [this, generation = mTaskGeneration](KJob *job) {
        fetchItemsJobFinished(job, generation);
    });
    job->start();
}

void GraphResource::fetchItemsJobFinished(KJob *job, quint64 generation)
{
    if (isStale(generation)) {
        return;
    }
    if (job->error()) {
        cancelTask(job->errorText());
        return;
    }
    auto *fj = qobject_cast<GraphFetchItemsJob *>(job);
    qCDebug(GRAPH_LOG) << "mail delta" << fj->collection().name() << "changed:" << fj->changedItems().size() << "removed:" << fj->removedItems().size()
                       << "deltaLink:" << (fj->deltaLink().isEmpty() ? "none" : "present");
    // Delta returns light stubs (id/flags); payload is fetched on demand below.
    deliverItemsIncremental(fj->collection(), fj->changedItems(), fj->removedItems(), fj->deltaLink(), generation);
}

void GraphResource::fetchPimItemsJobFinished(KJob *job, quint64 generation)
{
    if (isStale(generation)) {
        return;
    }
    if (job->error()) {
        cancelTask(job->errorText());
        return;
    }
    auto *fj = qobject_cast<GraphFetchPimItemsJob *>(job);
    qCDebug(GRAPH_LOG) << "pim delta" << fj->collection().name() << "changed:" << fj->changedItems().size() << "removed:" << fj->removedItems().size()
                       << "deltaLink:" << (fj->deltaLink().isEmpty() ? "none" : "present");
    deliverItemsIncremental(fj->collection(), fj->changedItems(), fj->removedItems(), fj->deltaLink(), generation);
}

void GraphResource::deliverItemsIncremental(const Collection &collection,
                                            const Item::List &changed,
                                            const Item::List &removed,
                                            const QString &deltaLink,
                                            quint64 generation)
{
    // Deltas may tombstone items this cache never saw (removed server-side between two
    // polls, or emitted under a different id encoding by old Graph responses). ItemSync
    // aborts on such deletes, which would wedge the collection: the deltaLink is only
    // advanced after a successful commit, so the same tombstone would fail every poll.
    if (!removed.isEmpty()) {
        auto known = new ItemFetchJob(collection, this);
        known->fetchScope().setFetchModificationTime(false);
        connect(known, &ItemFetchJob::result, this, [this, collection, changed, removed, deltaLink, known, generation](KJob *job) {
            if (isStale(generation)) {
                return;
            }
            if (job->error()) {
                cancelTask(job->errorText());
                return;
            }
            QSet<QString> localRids;
            const auto items = known->items();
            for (const Item &item : items) {
                localRids.insert(item.remoteId());
            }
            Item::List knownRemoved;
            for (const Item &item : removed) {
                if (localRids.contains(item.remoteId())) {
                    knownRemoved.append(item);
                }
            }
            if (knownRemoved.size() != removed.size()) {
                qCDebug(GRAPH_LOG) << "dropping" << removed.size() - knownRemoved.size() << "tombstones for items not in the local cache";
            }
            syncItemsIncremental(collection, changed, knownRemoved, deltaLink, generation);
        });
        return;
    }
    syncItemsIncremental(collection, changed, removed, deltaLink, generation);
}

void GraphResource::syncItemsIncremental(const Collection &collection,
                                         const Item::List &changed,
                                         const Item::List &removed,
                                         const QString &deltaLink,
                                         quint64 generation)
{
    // Run the ItemSync ourselves instead of via itemsRetrievedIncremental() so the
    // deltaLink is persisted only after the local commit succeeded. Saving it upfront
    // (as ResourceBase's flow forces) silently loses the whole batch when the commit
    // fails: the next delta starts after the never-applied changes.
    auto sync = new ItemSync(collection, {}, this);
    sync->setTransactionMode(ItemSync::SingleTransaction);
    // Connect before feeding: an empty delta finishes inside setIncrementalSyncItems().
    connect(sync, &ItemSync::result, this, [this, collection, deltaLink, generation](KJob *job) {
        if (!job->error()) {
            // Committed: the deltaLink is right even when the task has been given up.
            saveCollectionDeltaLink(collection, deltaLink);
        }
        if (isStale(generation)) {
            return;
        }
        if (job->error()) {
            cancelTask(job->errorText());
            return;
        }
        itemsRetrievalDone();
    });
    sync->setIncrementalSyncItems(changed, removed);
}

bool GraphResource::retrieveItems(const Akonadi::Item::List &items, [[maybe_unused]] const QSet<QByteArray> &parts)
{
    // Calendar/contact payloads are delivered whole during collection sync; if Akonadi
    // still asks (cache miss), the items already carry their payload, so just return them.
    if (!items.isEmpty()) {
        const QString mime = items.constFirst().mimeType();
        if (mime == GraphEventHandler::mimeType() || mime == GraphContactHandler::mimeType() || mime == GraphTodoHandler::mimeType()) {
            if (items.constFirst().hasPayload()) {
                itemsRetrieved(items);
                return true;
            }
            // Cache miss: re-fetch each requested event/contact/task individually and
            // rebuild its payload. (Rare — payloads are normally cached from the delta sync.)
            const bool isEvent = mime == GraphEventHandler::mimeType();
            const bool isTodo = mime == GraphTodoHandler::mimeType();
            auto pending = std::make_shared<Item::List>();
            auto remaining = std::make_shared<int>(items.size());
            const quint64 generation = mTaskGeneration;
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
                connect(req, &KJob::result, this, [this, item, req, isEvent, isTodo, pending, remaining, generation](KJob *j) {
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
                        deliverFreshPayloads(*pending, generation);
                    }
                });
                req->start();
            }
            return true;
        }
    }

    // Mail: GET /me/messages/{id}/$value  -> raw MIME -> KMime::Message
    auto job = new GraphFetchItemPayloadJob(mClient, items, this);
    connect(job, &KJob::result, this, [this, generation = mTaskGeneration](KJob *job) {
        fetchPayloadJobFinished(job, generation);
    });
    job->start();
    return true;
}

void GraphResource::fetchPayloadJobFinished(KJob *job, quint64 generation)
{
    if (isStale(generation)) {
        return;
    }
    if (job->error()) {
        qCWarning(GRAPH_LOG) << "payload fetch failed:" << job->errorText();
        cancelTask(job->errorText());
        return;
    }
    auto *fj = qobject_cast<GraphFetchItemPayloadJob *>(job);
    deliverFreshPayloads(fj->items(), generation);
}

void GraphResource::deliverFreshPayloads(const Item::List &filled, quint64 generation)
{
    if (isStale(generation)) {
        return;
    }
    if (filled.isEmpty()) {
        cancelTask(i18n("The requested items no longer exist"));
        return;
    }
    // The items in a retrieval request carry the revision from when the request was
    // queued; a delta poll delivering e.g. a read-state change in the meantime bumps
    // it and the payload store then aborts with a revision conflict. Deliver on
    // freshly fetched items instead: only the payload is marked dirty, so the store
    // touches neither flags nor anything else, and the revision is current.
    auto fetch = new ItemFetchJob(filled, this);
    fetch->fetchScope().setFetchModificationTime(false);
    fetch->fetchScope().setAncestorRetrieval(ItemFetchScope::Parent);
    connect(fetch, &ItemFetchJob::result, this, [this, filled, fetch, generation](KJob *job) {
        if (isStale(generation)) {
            return;
        }
        if (job->error()) {
            cancelTask(job->errorText());
            return;
        }
        QHash<Item::Id, const Item *> byId;
        for (const Item &item : filled) {
            byId.insert(item.id(), &item);
        }
        Item::List fresh = fetch->items();
        for (Item &item : fresh) {
            if (const Item *source = byId.value(item.id())) {
                item.setPayloadFromData(source->payloadData());
            }
        }
        if (fresh.isEmpty()) {
            cancelTask(i18n("The requested items no longer exist"));
            return;
        }
        itemsRetrieved(fresh);
    });
}

// ============================================================================
//  Change replay: local -> Graph   (Phase 2/3)
// ============================================================================

void GraphResource::itemsFlagsChanged(const Item::List &items,
                                      [[maybe_unused]] const QSet<QByteArray> &addedFlags,
                                      [[maybe_unused]] const QSet<QByteArray> &removedFlags)
{
    qCDebug(GRAPH_LOG) << "replay: flags changed on" << items.size() << "items";
    Replay replay;
    if (!startReplay(changeId(items), replay)) {
        return;
    }
    // item.flags() already carries the final state, so the added/removed sets are not needed.
    QList<GraphBatchJob::Call> calls;
    calls.reserve(items.size());
    for (const Item &item : items) {
        calls.append({GraphRequest::Method::Patch, QStringLiteral("/me/messages/%1").arg(item.remoteId()), GraphMailHandler::flagPatchBody(item)});
    }
    auto job = new GraphBatchJob(mClient, calls, this);
    // A flag on a message that is already gone server-side is moot, and failing the
    // whole batch over it would drop the flags of every other message with it.
    job->setIgnoreNotFound(true);
    announceReplay(job, items.size());
    connect(job, &KJob::result, this, [this, items, replay](KJob *job) {
        if (retryLater(job, replay, true, false)) {
            return;
        }
        if (job->error()) {
            failChange(replay, job->errorText());
        } else {
            commitItems(replay, items);
        }
    });
    job->start();
}

void GraphResource::itemChanged(const Item &item, const QSet<QByteArray> &partIdentifiers)
{
    qCDebug(GRAPH_LOG) << "replay: item changed" << item.remoteId() << item.mimeType() << "parts" << partIdentifiers;
    Replay replay;
    if (!startReplay(item.id(), replay)) {
        return;
    }
    const QString mime = item.mimeType();
    if (mime == GraphEventHandler::mimeType() && item.hasPayload<KCalendarCore::Incidence::Ptr>()) {
        auto incidence = item.payload<KCalendarCore::Incidence::Ptr>();
        auto event = incidence.dynamicCast<KCalendarCore::Event>();
        if (event) {
            patchPimItem(item, QStringLiteral("/me/events/%1").arg(item.remoteId()), GraphEventHandler::toJson(event), replay);
            return;
        }
    } else if (mime == GraphContactHandler::mimeType() && item.hasPayload<KContacts::Addressee>()) {
        patchPimItem(item, QStringLiteral("/me/contacts/%1").arg(item.remoteId()), GraphContactHandler::toJson(item.payload<KContacts::Addressee>()), replay);
        return;
    } else if (mime == GraphTodoHandler::mimeType() && item.hasPayload<KCalendarCore::Incidence::Ptr>()) {
        auto todo = item.payload<KCalendarCore::Incidence::Ptr>().dynamicCast<KCalendarCore::Todo>();
        if (todo) {
            patchPimItem(item,
                         QStringLiteral("/me/todo/lists/%1/tasks/%2").arg(item.parentCollection().remoteId(), item.remoteId()),
                         GraphTodoHandler::toJson(todo),
                         replay);
            return;
        }
    }
    // Mail: Graph cannot replace an existing message's MIME in place. For a draft the
    // updated MIME is the complete truth, so recreate it: POST the new MIME (which
    // Graph only accepts as a draft — exactly what we want), then delete the old copy.
    if (mime == GraphMailHandler::mimeType() && partIdentifiers.contains(QByteArrayLiteral("PLD:RFC822")) && item.hasPayload<std::shared_ptr<KMime::Message>>()
        && isDraftsCollection(item.parentCollection())) {
        replaceDraft(item, replay);
        return;
    }
    // Other mail edits (attribute-only changes, non-draft folders) stay local-only.
    changeProcessed();
}

bool GraphResource::isDraftsCollection(const Akonadi::Collection &col) const
{
    const auto it = mSpecialFolderIndex.constFind(col.remoteId());
    return it != mSpecialFolderIndex.constEnd() && qstrcmp(kSpecialFolders[it.value()].attributeType, "drafts") == 0;
}

void GraphResource::replaceDraft(const Akonadi::Item &item, const Replay &replay)
{
    const auto [rawMime, contentType] = GraphMailHandler::createFromMime(item);
    if (rawMime.isEmpty()) {
        changeProcessed();
        return;
    }
    // Create first, delete afterwards — a failure in between must never lose content.
    createMimeMessage(rawMime,
                      contentType,
                      item.parentCollection().remoteId(),
                      [this, item, replay](const QString &newId, const QString &error, KJob *failedCreate) {
                          if (newId.isEmpty()) {
                              // Only a failed create leaves nothing behind that a retry could duplicate.
                              if (retryLater(failedCreate, replay, false, false)) {
                                  return;
                              }
                              failChange(replay, error);
                              return;
                          }
                          auto del = new GraphRequest(mClient, this);
                          del->setMethod(GraphRequest::Method::Delete);
                          del->setPath(QStringLiteral("/me/messages/%1").arg(item.remoteId()));
                          connect(del, &KJob::result, this, [this, item, newId, del, replay](KJob *dj) {
                              if (dj->error() && del->httpStatus() != 404) {
                                  // The new draft exists; committing it is still right. The stale copy
                                  // will surface in the next delta and can be deleted by the user.
                                  qCWarning(GRAPH_LOG) << "could not delete the old draft" << item.remoteId() << ":" << dj->errorText();
                              }
                              Item newItem(item);
                              newItem.setRemoteId(newId);
                              commitItem(replay, newItem);
                          });
                          del->start();
                      });
}

void GraphResource::createMimeMessage(const QByteArray &rawMime,
                                      const QByteArray &contentType,
                                      const QString &targetFolderRid,
                                      const std::function<void(const QString &, const QString &, KJob *)> &done)
{
    // Graph ingests MIME only on the unscoped /me/messages endpoint — the folder-scoped
    // variant rejects the identical body with HTTP 400 UnableToDeserializePostBody. The
    // message therefore always appears as a draft in the Drafts folder first and has to
    // be moved when the target is some other folder (a move assigns a new id).
    auto create = new GraphRequest(mClient, this);
    create->setMethod(GraphRequest::Method::Post);
    create->setPath(QStringLiteral("/me/messages"));
    create->setRawBody(rawMime, contentType);
    connect(create, &KJob::result, this, [this, targetFolderRid, done, create](KJob *job) {
        if (job->error()) {
            done(QString(), job->errorText(), job);
            return;
        }
        const QJsonObject response = create->responseObject();
        const QString id = response.value(QLatin1String("id")).toString();
        if (id.isEmpty()) {
            done(QString(), i18n("The server did not return an identifier for the new message"), nullptr);
            return;
        }
        if (response.value(QLatin1String("parentFolderId")).toString() == targetFolderRid) {
            done(id, QString(), nullptr);
            return;
        }
        auto move = new GraphRequest(mClient, this);
        move->setMethod(GraphRequest::Method::Post);
        move->setPath(QStringLiteral("/me/messages/%1/move").arg(id));
        move->setBody(QJsonObject{{QStringLiteral("destinationId"), targetFolderRid}});
        connect(move, &KJob::result, this, [move, done](KJob *mj) {
            if (mj->error()) {
                done(QString(), mj->errorText(), nullptr);
                return;
            }
            const QString movedId = move->responseObject().value(QLatin1String("id")).toString();
            done(movedId, movedId.isEmpty() ? i18n("The server did not return an identifier for the new message") : QString(), nullptr);
        });
        move->start();
    });
    create->start();
}

void GraphResource::patchPimItem(const Akonadi::Item &item, const QString &path, const QJsonObject &body, const Replay &replay)
{
    auto req = new GraphRequest(mClient, this);
    req->setMethod(GraphRequest::Method::Patch);
    req->setPath(path);
    req->setBody(body);
    connect(req, &KJob::result, this, [this, item, replay](KJob *job) {
        if (retryLater(job, replay, true, false)) {
            return;
        }
        if (job->error()) {
            failChange(replay, job->errorText());
        } else {
            putContactPhotoThenCommit(item, replay);
        }
    });
    req->start();
}

void GraphResource::itemAdded(const Item &item, const Collection &collection)
{
    qCDebug(GRAPH_LOG) << "replay: item added to" << collection.name() << item.mimeType();
    Replay replay;
    if (!startReplay(item.id(), replay)) {
        return;
    }
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
            QJsonObject body = GraphEventHandler::toJson(event);
            body.insert(QStringLiteral("transactionId"), eventTransactionId(item, collection));
            postPimItem(item, QStringLiteral("/me/calendars/%1/events").arg(collection.remoteId()), body, true, replay);
        } else {
            qCWarning(GRAPH_LOG) << "event itemAdded without usable payload";
            changeProcessed();
        }
        return;
    }
    // Contact -> POST /me/contacts (default folder)
    if (mime == GraphContactHandler::mimeType()) {
        if (item.hasPayload<KContacts::Addressee>()) {
            postPimItem(item, QStringLiteral("/me/contacts"), GraphContactHandler::toJson(item.payload<KContacts::Addressee>()), false, replay);
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
            postPimItem(item, QStringLiteral("/me/todo/lists/%1/tasks").arg(collection.remoteId()), GraphTodoHandler::toJson(todo), false, replay);
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
            reconcileSentItem(item, messageId, replay);
            return;
        }
    }

    const auto [rawMime, contentType] = GraphMailHandler::createFromMime(item);
    if (rawMime.isEmpty()) {
        changeProcessed();
        return;
    }
    createMimeMessage(rawMime, contentType, collection.remoteId(), [this, item, replay](const QString &newId, const QString &error, KJob *failedCreate) {
        if (newId.isEmpty()) {
            // Only a failed create leaves nothing behind that a retry could duplicate.
            if (retryLater(failedCreate, replay, false, false)) {
                return;
            }
            failChange(replay, error);
            return;
        }
        Item newItem(item);
        newItem.setRemoteId(newId);
        commitItem(replay, newItem);
    });
}

void GraphResource::postPimItem(const Akonadi::Item &item, const QString &path, const QJsonObject &body, bool repeatable, const Replay &replay)
{
    auto req = new GraphRequest(mClient, this);
    req->setMethod(GraphRequest::Method::Post);
    req->setPath(path);
    req->setBody(body);
    connect(req, &KJob::result, this, [this, item, req, repeatable, replay](KJob *job) {
        if (retryLater(job, replay, repeatable, false)) {
            return;
        }
        if (job->error()) {
            failChange(replay, job->errorText());
            return;
        }
        Item newItem(item);
        newItem.setRemoteId(req->responseObject().value(QLatin1String("id")).toString());
        putContactPhotoThenCommit(newItem, replay);
    });
    req->start();
}

void GraphResource::movePimItem(const Item::List &items,
                                int index,
                                const std::shared_ptr<Item::List> &moved,
                                const Collection &source,
                                const Collection &destination,
                                const Replay &replay)
{
    if (index >= items.size()) {
        commitItems(replay, *moved);
        return;
    }
    const Item item = items.at(index);
    const bool isTodo = item.mimeType() == GraphTodoHandler::mimeType();

    QJsonObject body;
    if (isTodo) {
        const auto todo =
            item.hasPayload<KCalendarCore::Incidence::Ptr>() ? item.payload<KCalendarCore::Incidence::Ptr>().dynamicCast<KCalendarCore::Todo>() : nullptr;
        if (!todo) {
            failChange(replay, i18n("Cannot move task %1: no payload", item.remoteId()));
            return;
        }
        body = GraphTodoHandler::toJson(todo);
    } else {
        const auto event =
            item.hasPayload<KCalendarCore::Incidence::Ptr>() ? item.payload<KCalendarCore::Incidence::Ptr>().dynamicCast<KCalendarCore::Event>() : nullptr;
        if (!event) {
            failChange(replay, i18n("Cannot move event %1: no payload", item.remoteId()));
            return;
        }
        body = GraphEventHandler::toJson(event);
        body.insert(QStringLiteral("transactionId"), eventTransactionId(item, destination));
    }

    auto create = new GraphRequest(mClient, this);
    create->setMethod(GraphRequest::Method::Post);
    create->setPath(isTodo ? QStringLiteral("/me/todo/lists/%1/tasks").arg(destination.remoteId())
                           : QStringLiteral("/me/calendars/%1/events").arg(destination.remoteId()));
    create->setBody(body);
    connect(create, &KJob::result, this, [this, create, items, index, moved, source, destination, item, isTodo, replay](KJob *job) {
        // From the second item on, the first ones already exist in the destination.
        if (retryLater(job, replay, false, index > 0)) {
            return;
        }
        if (job->error()) {
            failChange(replay, job->errorText());
            return;
        }
        const QString newId = create->responseObject().value(QLatin1String("id")).toString();
        auto del = new GraphRequest(mClient, this);
        del->setMethod(GraphRequest::Method::Delete);
        del->setPath(isTodo ? QStringLiteral("/me/todo/lists/%1/tasks/%2").arg(source.remoteId(), item.remoteId())
                            : QStringLiteral("/me/events/%1").arg(item.remoteId()));
        connect(del, &KJob::result, this, [this, del, items, index, moved, source, destination, item, newId, replay](KJob *dj) {
            if (dj->error() && del->httpStatus() != 404) {
                failChange(replay, dj->errorText());
                return;
            }
            Item movedItem(item);
            if (!newId.isEmpty()) {
                movedItem.setRemoteId(newId);
            }
            moved->append(movedItem);
            movePimItem(items, index + 1, moved, source, destination, replay);
        });
        del->start();
    });
    create->start();
}

void GraphResource::putContactPhotoThenCommit(const Akonadi::Item &item, const Replay &replay)
{
    if (item.mimeType() != GraphContactHandler::mimeType() || !item.hasPayload<KContacts::Addressee>() || item.remoteId().isEmpty()) {
        commitItem(replay, item);
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
        commitItem(replay, item);
        return;
    }
    auto req = new GraphRequest(mClient, this);
    req->setMethod(GraphRequest::Method::Put);
    req->setPath(QStringLiteral("/me/contacts/%1/photo/$value").arg(item.remoteId()));
    req->setRawBody(data, contentType);
    connect(req, &KJob::result, this, [this, item, replay](KJob *job) {
        if (job->error()) {
            // Best effort: the contact itself is saved; a failed photo upload
            // should not fail the change replay.
            qCWarning(GRAPH_LOG) << "contact photo upload failed for" << item.remoteId() << ":" << job->errorText();
        }
        commitItem(replay, item);
    });
    req->start();
}

void GraphResource::reconcileSentItem(const Akonadi::Item &item, const QString &messageId, const Replay &replay)
{
    // Find Graph's auto-filed copy in Sent Items by its Internet Message-ID.
    // OData string literals escape a single quote by doubling it.
    QString escapedId = messageId;
    escapedId.replace(QLatin1Char('\''), QStringLiteral("''"));
    const QString filter = QStringLiteral("internetMessageId eq '%1'").arg(escapedId);
    auto req = new GraphRequest(mClient, this);
    req->setPath(QStringLiteral("/me/mailFolders/sentitems/messages?$select=id&$top=1&$filter=%1").arg(QString::fromUtf8(QUrl::toPercentEncoding(filter))));
    connect(req, &KJob::result, this, [this, item, req, replay](KJob *job) {
        if (retryLater(job, replay, true, false)) {
            return;
        }
        if (job->error()) {
            failChange(replay, job->errorText());
            return;
        }
        const auto values = req->aggregatedValue();
        if (!values.isEmpty()) {
            Item newItem(item);
            newItem.setRemoteId(values.first().toObject().value(QLatin1String("id")).toString());
            commitItem(replay, newItem); // adopt the server copy; no duplicate created
        } else {
            // Auto-copy not visible yet — drop the local duplicate; the next delta
            // sync will bring the server's copy with its real id.
            skipChange(replay);
        }
    });
    req->start();
}

void GraphResource::itemsMoved(const Item::List &items, const Collection &source, const Collection &destination)
{
    qCDebug(GRAPH_LOG) << "replay: moving" << items.size() << "items from" << source.name() << "to" << destination.name();
    Replay replay;
    if (!startReplay(changeId(items), replay)) {
        return;
    }
    const QString mime = items.isEmpty() ? QString() : items.constFirst().mimeType();
    if (mime == GraphEventHandler::mimeType() || mime == GraphTodoHandler::mimeType()) {
        // Graph has no move API for events or tasks — recreate in the destination and
        // delete the original (Outlook on the web does the same). Attendees are not
        // part of the write mapping, so recreating never sends out invitations.
        movePimItem(items, 0, std::make_shared<Item::List>(), source, destination, replay);
        return;
    }
    if (mime == GraphContactHandler::mimeType()) {
        // Only the default contacts folder is modelled, so this should be unreachable.
        failChange(replay, i18n("Moving contacts between folders is not supported"));
        return;
    }

    QList<GraphBatchJob::Call> calls;
    calls.reserve(items.size());
    QJsonObject body;
    body.insert(QStringLiteral("destinationId"), destination.remoteId());
    for (const Item &item : items) {
        calls.append({GraphRequest::Method::Post, QStringLiteral("/me/messages/%1/move").arg(item.remoteId()), body});
    }
    auto job = new GraphBatchJob(mClient, calls, this);
    // A message that no longer exists cannot be moved; treat it like any other
    // failed item below rather than failing the other moves too.
    job->setIgnoreNotFound(true);
    announceReplay(job, items.size());
    connect(job, &KJob::result, this, [this, items, source, job, replay](KJob *kjob) {
        // A move is not repeatable (the old id is gone once it went through), so this
        // only holds back a change of which no message has been moved yet.
        if (retryLater(kjob, replay, false, false)) {
            return;
        }
        // Graph assigns a new message id on move — push the new remote ids back for
        // every move that went through, even when others failed: with the old id
        // left in place, the next delta of the destination folder would not
        // recognise the message and insert it a second time.
        Item::List moved;
        moved.reserve(items.size());
        Item::List failed;
        const QList<QJsonObject> responses = job->responses();
        for (int i = 0; i < items.size(); ++i) {
            const QString newId = i < responses.size() ? responses.at(i).value(QLatin1String("id")).toString() : QString();
            if (newId.isEmpty()) {
                failed.append(items.at(i));
            } else {
                Item item = items.at(i);
                item.setRemoteId(newId);
                moved.append(item);
            }
        }
        if (kjob->error()) {
            Q_EMIT error(kjob->errorText());
        }
        qCDebug(GRAPH_LOG) << "replay: moved" << moved.size() << "of" << items.size() << "messages on the server";
        if (failed.isEmpty()) {
            commitItems(replay, moved);
            return;
        }
        // The rest is still in the source folder on the server (or, after a tolerated
        // 404, nowhere at all), while the client already shows it in the destination.
        // Put it back where the server has it, keeping the remote id: a message that
        // is genuinely gone then meets its tombstone in the source folder on the next
        // poll. The resource's own session is not recorded for replay, so this does
        // not come back as another move.
        auto undo = new ItemMoveJob(failed, source, this);
        trackWork(undo);
        connect(undo, &KJob::result, this, [this, moved, failed, replay](KJob *undoJob) {
            if (undoJob->error()) {
                qCWarning(GRAPH_LOG) << "could not move" << failed.size() << "unmoved items back locally:" << undoJob->errorText();
            }
            if (moved.isEmpty()) {
                skipChange(replay);
            } else {
                commitItems(replay, moved);
            }
        });
        undo->start();
    });
    job->start();
}

void GraphResource::itemsRemoved(const Item::List &items)
{
    // ResourceBase only passes on items with a remoteId; one that never got to the
    // server has nothing to delete there.
    qCDebug(GRAPH_LOG) << "replay: removing" << items.size() << "items";
    Replay replay;
    if (!startReplay(changeId(items), replay)) {
        return;
    }
    // Calendar/contact/task deletes hit different endpoints (and never carry mail flags).
    const QString mime = items.isEmpty() ? QString() : items.constFirst().mimeType();
    QString pimBase;
    if (mime == GraphEventHandler::mimeType()) {
        pimBase = QStringLiteral("/me/events/%1");
    } else if (mime == GraphContactHandler::mimeType()) {
        pimBase = QStringLiteral("/me/contacts/%1");
    }
    if (!pimBase.isEmpty() || mime == GraphTodoHandler::mimeType()) {
        QList<GraphBatchJob::Call> calls;
        calls.reserve(items.size());
        for (const Item &item : items) {
            // Tasks are addressed through their list; the parent collection carries it.
            const QString path = pimBase.isEmpty() ? QStringLiteral("/me/todo/lists/%1/tasks/%2").arg(item.parentCollection().remoteId(), item.remoteId())
                                                   : pimBase.arg(item.remoteId());
            calls.append({GraphRequest::Method::Delete, path, {}});
        }
        auto job = new GraphBatchJob(mClient, calls, this);
        job->setIgnoreNotFound(true);
        announceReplay(job, items.size());
        connect(job, &KJob::result, this, [this, replay](KJob *j) {
            if (retryLater(j, replay, true, false)) {
                return;
            }
            if (j->error()) {
                failChange(replay, j->errorText());
            } else {
                skipChange(replay);
            }
        });
        job->start();
        return;
    }

    QList<GraphBatchJob::Call> calls;
    calls.reserve(items.size());
    for (const Item &item : items) {
        // DELETE moves to Deleted Items' recoverable items; matches user expectation.
        calls.append({GraphRequest::Method::Delete, QStringLiteral("/me/messages/%1").arg(item.remoteId()), {}});
    }
    auto job = new GraphBatchJob(mClient, calls, this);
    job->setIgnoreNotFound(true);
    announceReplay(job, items.size());
    connect(job, &KJob::result, this, [this, count = items.size(), replay](KJob *job) {
        if (retryLater(job, replay, true, false)) {
            return;
        }
        if (job->error()) {
            failChange(replay, job->errorText());
        } else {
            qCDebug(GRAPH_LOG) << "replay: deleted" << count << "messages on the server";
            skipChange(replay);
        }
    });
    job->start();
}

void GraphResource::collectionAdded(const Collection &collection, const Collection &parent)
{
    qCDebug(GRAPH_LOG) << "replay: collection added" << collection.name() << "under" << parent.name();
    Replay replay;
    if (!startReplay(-collection.id(), replay)) {
        return;
    }
    QString path;
    QJsonObject body;
    switch (collectionKind(collection)) {
    case CollectionKind::Calendar:
        path = QStringLiteral("/me/calendars");
        body.insert(QStringLiteral("name"), collection.name());
        break;
    case CollectionKind::Todo:
        path = QStringLiteral("/me/todo/lists");
        body.insert(QStringLiteral("displayName"), collection.name());
        break;
    case CollectionKind::Contacts:
        // Only the single built-in contacts folder is modelled.
        qCWarning(GRAPH_LOG) << "contact folder creation is not supported; keeping" << collection.name() << "local-only";
        changeProcessed();
        return;
    case CollectionKind::Mail: {
        const bool topLevel = parent.remoteId() == mRootCollection.remoteId();
        path = topLevel ? QStringLiteral("/me/mailFolders") : QStringLiteral("/me/mailFolders/%1/childFolders").arg(parent.remoteId());
        body.insert(QStringLiteral("displayName"), collection.name());
        break;
    }
    }

    auto req = new GraphRequest(mClient, this);
    req->setMethod(GraphRequest::Method::Post);
    req->setPath(path);
    req->setBody(body);
    connect(req, &KJob::result, this, [this, collection, req, replay](KJob *job) {
        if (retryLater(job, replay, false, false)) {
            return;
        }
        if (job->error()) {
            failChange(replay, job->errorText());
            return;
        }
        Collection col(collection);
        col.setRemoteId(req->responseObject().value(QLatin1String("id")).toString());
        commitCollection(replay, col);
    });
    req->start();
}

void GraphResource::collectionChanged(const Collection &collection, const QSet<QByteArray> &changedAttributes)
{
    qCDebug(GRAPH_LOG) << "replay: collection changed" << collection.name() << changedAttributes;
    Replay replay;
    if (!startReplay(-collection.id(), replay)) {
        return;
    }
    if (!changedAttributes.contains("NAME")) {
        changeProcessed(); // only renames are propagated to Graph
        return;
    }
    QString path;
    QString nameField = QStringLiteral("displayName");
    switch (collectionKind(collection)) {
    case CollectionKind::Calendar:
        path = QStringLiteral("/me/calendars/%1").arg(collection.remoteId());
        nameField = QStringLiteral("name");
        break;
    case CollectionKind::Todo:
        path = QStringLiteral("/me/todo/lists/%1").arg(collection.remoteId());
        break;
    case CollectionKind::Contacts:
        changeProcessed(); // the single built-in folder has no server-side name
        return;
    case CollectionKind::Mail:
        path = QStringLiteral("/me/mailFolders/%1").arg(collection.remoteId());
        break;
    }
    QJsonObject body;
    body.insert(nameField, collection.name());
    auto req = new GraphRequest(mClient, this);
    req->setMethod(GraphRequest::Method::Patch);
    req->setPath(path);
    req->setBody(body);
    connect(req, &KJob::result, this, [this, collection, replay](KJob *job) {
        if (retryLater(job, replay, true, false)) {
            return;
        }
        if (job->error()) {
            failChange(replay, job->errorText());
        } else {
            commitCollection(replay, collection);
        }
    });
    req->start();
}

void GraphResource::collectionMoved(const Collection &collection, [[maybe_unused]] const Collection &source, const Collection &destination)
{
    qCDebug(GRAPH_LOG) << "replay: collection moved" << collection.name() << "to" << destination.name();
    Replay replay;
    if (!startReplay(-collection.id(), replay)) {
        return;
    }
    if (collectionKind(collection) != CollectionKind::Mail) {
        // Graph calendars/task lists are flat; the next tree sync re-parents the
        // collection under the account root again.
        qCWarning(GRAPH_LOG) << "collection" << collection.name() << "cannot be moved on the server";
        changeProcessed();
        return;
    }
    QJsonObject body;
    body.insert(QStringLiteral("destinationId"), destination.remoteId());
    auto req = new GraphRequest(mClient, this);
    req->setMethod(GraphRequest::Method::Post);
    req->setPath(QStringLiteral("/me/mailFolders/%1/move").arg(collection.remoteId()));
    req->setBody(body);
    connect(req, &KJob::result, this, [this, collection, req, replay](KJob *job) {
        if (retryLater(job, replay, false, false)) {
            return;
        }
        if (job->error()) {
            failChange(replay, job->errorText());
            return;
        }
        Collection col(collection);
        const QString newId = req->responseObject().value(QLatin1String("id")).toString();
        if (!newId.isEmpty()) {
            col.setRemoteId(newId);
        }
        commitCollection(replay, col);
    });
    req->start();
}

void GraphResource::collectionRemoved(const Collection &collection)
{
    qCDebug(GRAPH_LOG) << "replay: collection removed" << collection.name();
    Replay replay;
    if (!startReplay(-collection.id(), replay)) {
        return;
    }
    QString path;
    switch (collectionKind(collection)) {
    case CollectionKind::Calendar:
        path = QStringLiteral("/me/calendars/%1").arg(collection.remoteId());
        break;
    case CollectionKind::Todo:
        path = QStringLiteral("/me/todo/lists/%1").arg(collection.remoteId());
        break;
    case CollectionKind::Contacts:
        changeProcessed(); // the built-in folder cannot be deleted; contents sync back
        return;
    case CollectionKind::Mail:
        path = QStringLiteral("/me/mailFolders/%1").arg(collection.remoteId());
        break;
    }
    auto req = new GraphRequest(mClient, this);
    req->setMethod(GraphRequest::Method::Delete);
    req->setPath(path);
    connect(req, &KJob::result, this, [this, req, replay](KJob *job) {
        if (req->httpStatus() != 404 && retryLater(job, replay, true, false)) {
            return;
        }
        if (job->error() && req->httpStatus() != 404) {
            failChange(replay, job->errorText());
        } else {
            skipChange(replay);
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
    createReq->setMethod(GraphRequest::Method::Post);
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
        sendReq->setMethod(GraphRequest::Method::Post);
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
