/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "graphfetchpimitemsjob.h"

#include "calendar/grapheventhandler.h"
#include "contact/graphcontacthandler.h"
#include "graphclient/graphrequest.h"

#include <KCalendarCore/Event>
#include <QJsonArray>
#include <QJsonObject>

using namespace Akonadi;

// Delta change tracking rejects $top; page size is expressed via this header instead.
static const QByteArray kMaxPageSize = QByteArrayLiteral("odata.maxpagesize=50");

GraphFetchPimItemsJob::GraphFetchPimItemsJob(GraphClient &client, const Collection &collection, Type type, const QString &deltaLink, QObject *parent)
    : KJob(parent)
    , mClient(client)
    , mCollection(collection)
    , mType(type)
    , mDeltaLink(deltaLink)
{
}

void GraphFetchPimItemsJob::start()
{
    if (!mDeltaLink.isEmpty()) {
        startDelta(mDeltaLink, true); // resume from stored deltaLink
        return;
    }
    if (mType == Events) {
        // The events delta returns only a minimal default field set; $select pulls the
        // fields we map. The returned deltaLink preserves this selection.
        startDelta(QStringLiteral("/me/calendars/%1/events/delta"
                                  "?$select=id,iCalUId,subject,body,bodyPreview,isAllDay,start,end,location,"
                                  "organizer,attendees,categories,showAs,sensitivity,recurrence")
                       .arg(mCollection.remoteId()),
                   false);
    } else {
        resolveContactsFolderThenStart();
    }
}

void GraphFetchPimItemsJob::resolveContactsFolderThenStart()
{
    // The default contacts folder is not listed by /me/contactFolders; get its id from
    // any contact's parentFolderId, then start the folder-scoped delta.
    auto *pre = new GraphRequest(mClient, this);
    pre->setPath(QStringLiteral("/me/contacts?$top=1&$select=id,parentFolderId"));
    connect(pre, &KJob::result, this, [this, pre](KJob *job) {
        if (job->error()) {
            setError(job->error());
            setErrorText(job->errorText());
            emitResult();
            return;
        }
        const auto values = pre->aggregatedValue();
        if (values.isEmpty()) {
            emitResult(); // empty contacts folder: nothing to sync, no deltaLink yet
            return;
        }
        const QString folderId = values.first().toObject().value(QLatin1String("parentFolderId")).toString();
        if (folderId.isEmpty()) {
            emitResult();
            return;
        }
        startDelta(QStringLiteral("/me/contactFolders/%1/contacts/delta").arg(folderId), false);
    });
    pre->start();
}

void GraphFetchPimItemsJob::startDelta(const QString &url, bool isAbsolute)
{
    auto *req = new GraphRequest(mClient, this);
    if (isAbsolute) {
        req->setAbsoluteUrl(QUrl(url));
    } else {
        req->setPath(url);
    }
    req->addHeader("Prefer", kMaxPageSize);
    connect(req, &KJob::result, this, &GraphFetchPimItemsJob::onDeltaFinished);
    req->start();
}

void GraphFetchPimItemsJob::onDeltaFinished(KJob *job)
{
    if (job->error()) {
        setError(job->error());
        setErrorText(job->errorText());
        emitResult();
        return;
    }
    auto *req = qobject_cast<GraphRequest *>(job);
    mDeltaLink = req->deltaLink();

    for (const auto &v : req->aggregatedValue()) {
        const QJsonObject obj = v.toObject();
        const QString id = obj.value(QLatin1String("id")).toString();
        if (id.isEmpty()) {
            continue;
        }
        if (obj.contains(QLatin1String("@removed"))) {
            Item item;
            item.setRemoteId(id);
            mRemoved.append(item);
            continue;
        }
        if (mType == Events) {
            auto event = GraphEventHandler::toEvent(obj);
            if (!event) {
                continue;
            }
            Item item(GraphEventHandler::mimeType());
            item.setRemoteId(id);
            item.setParentCollection(mCollection);
            item.setPayload<KCalendarCore::Incidence::Ptr>(event);
            mChanged.append(item);
        } else {
            Item item(GraphContactHandler::mimeType());
            item.setRemoteId(id);
            item.setParentCollection(mCollection);
            item.setPayload<KContacts::Addressee>(GraphContactHandler::toAddressee(obj));
            mChanged.append(item);
        }
    }
    emitResult();
}

Collection GraphFetchPimItemsJob::collection() const { return mCollection; }
Item::List GraphFetchPimItemsJob::changedItems() const { return mChanged; }
Item::List GraphFetchPimItemsJob::removedItems() const { return mRemoved; }
QString GraphFetchPimItemsJob::deltaLink() const { return mDeltaLink; }

#include "moc_graphfetchpimitemsjob.cpp"
