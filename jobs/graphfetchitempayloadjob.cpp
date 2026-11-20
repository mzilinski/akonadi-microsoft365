/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "graphfetchitempayloadjob.h"

#include "graphclient/graphrequest.h"
#include "jobs/graphbatchjob.h"

#include <KMime/Message>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

using namespace Akonadi;

GraphFetchItemPayloadJob::GraphFetchItemPayloadJob(GraphClient &client, const Item::List &items, QObject *parent)
    : KJob(parent)
    , mClient(client)
    , mItems(items)
{
}

void GraphFetchItemPayloadJob::start()
{
    if (mItems.isEmpty()) {
        QTimer::singleShot(0, this, [this] {
            emitResult();
        });
        return;
    }
    // Through GraphBatchJob (20 to a /$batch): a throttled message is asked again by
    // itself, and one deleted meanwhile (404) does not fail the others.
    QList<GraphBatchJob::Call> calls;
    calls.reserve(mItems.size());
    for (const Item &item : std::as_const(mItems)) {
        calls.append({GraphRequest::Method::Get, QStringLiteral("/me/messages/%1/$value").arg(item.remoteId()), {}});
    }
    auto batch = new GraphBatchJob(mClient, calls, this);
    batch->setIgnoreNotFound(true);
    connect(batch, &KJob::result, this, [this, batch](KJob *job) {
        if (job->error()) {
            setError(job->error());
            setErrorText(job->errorText());
            emitResult();
            return;
        }
        const QList<QJsonValue> bodies = batch->bodies();
        Item::List filled;
        filled.reserve(mItems.size());
        for (int i = 0; i < mItems.size(); ++i) {
            if (batch->wasNotFound(i)) {
                // Gone on the server; its tombstone comes with the next delta.
                continue;
            }
            // /$value in a batch comes back as a base64-encoded string body.
            Item item = mItems.at(i);
            applyMime(item, QByteArray::fromBase64(bodies.value(i).toString().toLatin1()));
            filled.append(item);
        }
        mItems = filled;
        emitResult();
    });
    batch->start();
}

void GraphFetchItemPayloadJob::applyMime(Item &item, const QByteArray &mime)
{
    auto msg = std::make_shared<KMime::Message>();
    // KMime expects LF line endings; Graph hands out raw CRLF MIME. Without this the
    // head/body split fails, the whole message lands in the head and KMail refetches
    // the "missing" body forever (cf. EwsMailHandler::setItemPayload).
    msg->setContent(KMime::CRLFtoLF(mime));
    msg->parse();
    // An empty body reads as "body not yet loaded" to Akonadi and would also loop.
    if (msg->body().isEmpty() && msg->contents().isEmpty()) {
        msg->setBody("\n");
    }
    item.setPayload(msg);
}

Item::List GraphFetchItemPayloadJob::items() const
{
    return mItems;
}

#include "moc_graphfetchitempayloadjob.cpp"
