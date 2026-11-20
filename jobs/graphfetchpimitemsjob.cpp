/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "graphfetchpimitemsjob.h"

#include "calendar/grapheventhandler.h"
#include "contact/graphcontacthandler.h"
#include "graph_debug.h"
#include "graphclient/graphrequest.h"
#include "jobs/graphbatchjob.h"
#include "todo/graphtodohandler.h"

#include <KCalendarCore/Event>
#include <KContacts/Picture>
#include <QJsonArray>
#include <QJsonObject>

using namespace Akonadi;

// Delta change tracking rejects $top; page size is expressed via this header instead.
static const QByteArray kMaxPageSize = QByteArrayLiteral("odata.maxpagesize=50");
// The event fields mapped, for the delta and for the occurrences of a series alike.
// The events delta lists single events and series masters only: a changed occurrence
// (type "exception") is fetched through its master, which the delta reports whenever
// one of its occurrences changes or is cancelled.
static const QString kEventFields = QStringLiteral(
    "id,iCalUId,subject,body,bodyPreview,isAllDay,start,end,location,"
    "organizer,attendees,categories,showAs,sensitivity,recurrence,"
    "originalStartTimeZone,originalEndTimeZone,type,seriesMasterId,originalStart,cancelledOccurrences,"
    "isReminderOn,reminderMinutesBeforeStart");

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
    mFullListing = mDeltaLink.isEmpty();
    if (!mDeltaLink.isEmpty()) {
        startDelta(mDeltaLink, true); // resume from stored deltaLink
        return;
    }
    switch (mType) {
    case Type::Events:
        // The events delta returns only a minimal default field set; $select pulls the
        // fields we map. The returned deltaLink preserves this selection.
        startDelta(QStringLiteral("/me/calendars/%1/events/delta?$select=%2").arg(mCollection.remoteId(), kEventFields), false);
        break;
    case Type::Todos:
        startDelta(QStringLiteral("/me/todo/lists/%1/tasks/delta").arg(mCollection.remoteId()), false);
        break;
    case Type::Contacts:
        resolveContactsFolderThenStart();
        break;
    }
}

void GraphFetchPimItemsJob::resolveContactsFolderThenStart()
{
    // The default contacts folder is not listed by /me/contactFolders; get its id from
    // any contact's parentFolderId, then start the folder-scoped delta.
    auto pre = new GraphRequest(mClient, this);
    pre->setPath(QStringLiteral("/me/contacts?$top=1&$select=id,parentFolderId"));
    pre->setFollowNextLink(false); // one contact is all it takes, not one page per contact
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
            // Not "no contacts": a full listing without any would delete the local ones.
            setError(KJob::UserDefinedError);
            setErrorText(QStringLiteral("Microsoft 365 did not name the contacts folder"));
            emitResult();
            return;
        }
        startDelta(QStringLiteral("/me/contactFolders/%1/contacts/delta").arg(folderId), false);
    });
    pre->start();
}

void GraphFetchPimItemsJob::startDelta(const QString &url, bool isAbsolute)
{
    auto req = new GraphRequest(mClient, this);
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
    auto *req = qobject_cast<GraphRequest *>(job);
    if (job->error()) {
        if (req->deltaStateExpired() && !mDeltaLink.isEmpty()) {
            // The stored delta token expired (HTTP 410 Gone) or the server no longer
            // accepts its state (HTTP 400 ErrorInvalidSyncStateData) — drop it and
            // rerun the delta from scratch. That round is a full listing: an initial
            // delta carries no tombstones, so what was deleted meanwhile is only
            // noticed by its absence.
            mDeltaLink.clear();
            mChanged.clear();
            mRemoved.clear();
            mMasters.clear();
            mSeriesExceptions.clear();
            start();
            return;
        }
        setError(job->error());
        setErrorText(job->errorText());
        emitResult();
        return;
    }
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
        switch (mType) {
        case Type::Events: {
            auto event = GraphEventHandler::toEvent(obj);
            if (!event) {
                continue;
            }
            Item item(GraphEventHandler::mimeType());
            item.setRemoteId(id);
            item.setParentCollection(mCollection);
            item.setPayload<KCalendarCore::Incidence::Ptr>(event);
            mChanged.append(item);
            if (obj.value(QLatin1String("type")).toString() == QLatin1String("seriesMaster")) {
                mMasters.insert(id, event);
            }
            break;
        }
        case Type::Todos: {
            auto todo = GraphTodoHandler::toTodo(obj);
            if (!todo) {
                continue;
            }
            Item item(GraphTodoHandler::mimeType());
            item.setRemoteId(id);
            item.setParentCollection(mCollection);
            item.setPayload<KCalendarCore::Incidence::Ptr>(todo);
            mChanged.append(item);
            break;
        }
        case Type::Contacts: {
            Item item(GraphContactHandler::mimeType());
            item.setRemoteId(id);
            item.setParentCollection(mCollection);
            item.setPayload<KContacts::Addressee>(GraphContactHandler::toAddressee(obj));
            mChanged.append(item);
            break;
        }
        }
    }
    if (mType == Type::Contacts && !mChanged.isEmpty()) {
        fetchPhotos(); // photos are a separate endpoint, not part of the contact JSON
        return;
    }
    if (mType == Type::Events && !mMasters.isEmpty()) {
        fetchExceptionIds();
        return;
    }
    emitResult();
}

void GraphFetchPimItemsJob::fetchExceptionIds()
{
    // $expand=exceptionOccurrences names the changed occurrences of a series (ids
    // only, whatever is selected); their fields need a request of their own.
    const QStringList masters = mMasters.keys();
    QList<GraphBatchJob::Call> calls;
    calls.reserve(masters.size());
    for (const QString &id : masters) {
        calls.append({GraphRequest::Method::Get, QStringLiteral("/me/events/%1?$select=id&$expand=exceptionOccurrences").arg(id), {}});
    }
    auto batch = new GraphBatchJob(mClient, calls, this);
    batch->setIgnoreNotFound(true); // a series deleted meanwhile: its tombstone follows
    connect(batch, &KJob::result, this, [this, batch, masters](KJob *job) {
        if (job->error()) {
            setError(job->error());
            setErrorText(job->errorText());
            emitResult();
            return;
        }
        QList<QPair<QString, QString>> exceptions;
        const QList<QJsonObject> responses = batch->responses();
        for (int i = 0; i < masters.size(); ++i) {
            const QJsonObject master = responses.value(i);
            if (master.isEmpty()) {
                continue; // gone: no statement about its occurrences
            }
            QStringList ids;
            const QJsonArray occurrences = master.value(QLatin1String("exceptionOccurrences")).toArray();
            for (const auto &o : occurrences) {
                const QString id = o.toObject().value(QLatin1String("id")).toString();
                if (!id.isEmpty()) {
                    ids << id;
                    exceptions.append({masters.at(i), id});
                }
            }
            mSeriesExceptions.insert(masters.at(i), ids);
        }
        if (exceptions.isEmpty()) {
            emitResult();
            return;
        }
        fetchExceptions(exceptions);
    });
    batch->start();
}

void GraphFetchPimItemsJob::fetchExceptions(const QList<QPair<QString, QString>> &exceptions)
{
    QList<GraphBatchJob::Call> calls;
    calls.reserve(exceptions.size());
    for (const auto &[master, id] : exceptions) {
        calls.append({GraphRequest::Method::Get, QStringLiteral("/me/events/%1?$select=%2").arg(id, kEventFields), {}});
    }
    auto batch = new GraphBatchJob(mClient, calls, this);
    batch->setIgnoreNotFound(true);
    connect(batch, &KJob::result, this, [this, batch, exceptions](KJob *job) {
        if (job->error()) {
            setError(job->error());
            setErrorText(job->errorText());
            emitResult();
            return;
        }
        const QList<QJsonObject> responses = batch->responses();
        for (int i = 0; i < exceptions.size(); ++i) {
            const auto &[masterId, id] = exceptions.at(i);
            const auto event = GraphEventHandler::toException(responses.value(i), mMasters.value(masterId));
            if (!event) {
                // Gone since it was listed (no body), or unreadable: dropped from the
                // list, so that a local copy goes too.
                mSeriesExceptions[masterId].removeAll(id);
                continue;
            }
            Item item(GraphEventHandler::mimeType());
            item.setRemoteId(id);
            item.setRemoteRevision(exceptionRevision(masterId));
            item.setParentCollection(mCollection);
            item.setPayload<KCalendarCore::Incidence::Ptr>(event);
            mChanged.append(item);
        }
        emitResult();
    });
    batch->start();
}

QHash<QString, QStringList> GraphFetchPimItemsJob::seriesExceptions() const
{
    return mSeriesExceptions;
}

QString GraphFetchPimItemsJob::exceptionRevision(const QString &masterId)
{
    return QStringLiteral("series:") + masterId;
}

QString GraphFetchPimItemsJob::masterOfRevision(const QString &remoteRevision)
{
    const QString prefix = exceptionRevision(QString());
    return remoteRevision.startsWith(prefix) ? remoteRevision.mid(prefix.size()) : QString();
}

QString GraphFetchPimItemsJob::eventFields()
{
    return kEventFields;
}

void GraphFetchPimItemsJob::fetchPhotos()
{
    // GET /me/contacts/{id}/photo/$value for each changed contact, through GraphBatchJob
    // (20 to a /$batch, throttled ones asked again). 404 = the contact has no photo.
    QList<GraphBatchJob::Call> calls;
    calls.reserve(mChanged.size());
    for (const Item &item : std::as_const(mChanged)) {
        calls.append({GraphRequest::Method::Get, QStringLiteral("/me/contacts/%1/photo/$value").arg(item.remoteId()), {}});
    }
    auto batch = new GraphBatchJob(mClient, calls, this);
    batch->setIgnoreNotFound(true);
    connect(batch, &KJob::result, this, [this, batch](KJob *job) {
        if (job->error()) {
            // Photos are decoration: the contacts go without the ones that failed.
            qCWarning(GRAPH_LOG) << "contact photos:" << job->errorText();
        }
        const QList<QJsonValue> bodies = batch->bodies();
        const QList<QJsonObject> headers = batch->headers();
        for (int i = 0; i < mChanged.size(); ++i) {
            // /$value in a batch comes back as a base64-encoded string body.
            const QByteArray data = QByteArray::fromBase64(bodies.value(i).toString().toLatin1());
            if (data.isEmpty()) {
                continue;
            }
            QString type = headers.value(i).value(QLatin1String("Content-Type")).toString();
            type = type.startsWith(QLatin1String("image/")) ? type.mid(6) : QStringLiteral("jpeg");
            auto addressee = mChanged.at(i).payload<KContacts::Addressee>();
            KContacts::Picture photo;
            photo.setRawData(data, type);
            addressee.setPhoto(photo);
            mChanged[i].setPayload(addressee);
        }
        emitResult();
    });
    batch->start();
}

Collection GraphFetchPimItemsJob::collection() const
{
    return mCollection;
}

Item::List GraphFetchPimItemsJob::changedItems() const
{
    return mChanged;
}

Item::List GraphFetchPimItemsJob::removedItems() const
{
    return mRemoved;
}
QString GraphFetchPimItemsJob::deltaLink() const
{
    return mDeltaLink;
}

bool GraphFetchPimItemsJob::isFullListing() const
{
    return mFullListing;
}

#include "moc_graphfetchpimitemsjob.cpp"
