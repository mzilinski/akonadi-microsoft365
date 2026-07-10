/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    End-to-end smoke test of the graphclient layer + sync jobs against a real tenant.
    Needs a valid bearer token in GRAPH_ACCESS_TOKEN (see GraphOAuth test hook).

    Exercises, in order:
      1. GraphFetchFoldersJob      full folder delta (GET, paging, deltaLink)
      2. GraphFetchItemsJob        message delta on Inbox (GET, deltaLink)
      3. GraphFetchItemPayloadJob  MIME payload -> KMime parse (GET /$value)
      4. GraphBatchJob             idempotent flag PATCH (isRead set to current value)
      5. draft create + delete     POST base64 MIME (itemAdded path) + DELETE

    Read-mostly: step 4 writes the value that is already set, step 5 removes what it
    created. Nothing user-visible changes in the mailbox.
*/

#include "graphclient/auth/graphoauth.h"
#include "graphclient/graphclient.h"
#include "graphclient/graphrequest.h"
#include "jobs/graphbatchjob.h"
#include "jobs/graphfetchfoldersjob.h"
#include "jobs/graphfetchitempayloadjob.h"
#include "jobs/graphfetchitemsjob.h"

#include <Akonadi/MessageFlags>
#include <KMime/Message>

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonObject>

#include <cstdio>

using namespace Akonadi;

static bool runJob(KJob *job, const char *name)
{
    QEventLoop loop;
    QObject::connect(job, &KJob::result, &loop, &QEventLoop::quit);
    job->start();
    loop.exec();
    if (job->error()) {
        std::printf("FAIL  %-28s %s\n", name, qPrintable(job->errorText()));
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (!qEnvironmentVariableIsSet("GRAPH_ACCESS_TOKEN")) {
        std::printf("GRAPH_ACCESS_TOKEN not set\n");
        return 2;
    }

    GraphOAuth auth(QStringLiteral("common"), QString(), QStringLiteral("smoketest"));
    auth.authenticate(); // env token: emits ready via queued signal, token available immediately

    GraphClient client;
    client.setBaseUrl(QStringLiteral("https://graph.microsoft.com/v1.0"));
    client.setAuth(&auth);

    int failures = 0;

    // --- 1. folder delta -----------------------------------------------------
    Collection root;
    root.setRemoteId(QStringLiteral("msgfolderroot"));
    auto foldersJob = new GraphFetchFoldersJob(client, root, QString());
    if (runJob(foldersJob, "GraphFetchFoldersJob")) {
        std::printf("OK    %-28s %lld folders, deltaLink: %s\n",
                    "GraphFetchFoldersJob",
                    static_cast<long long>(foldersJob->allCollections().size()),
                    foldersJob->deltaLink().isEmpty() ? "MISSING" : "yes");
        if (foldersJob->allCollections().isEmpty() || foldersJob->deltaLink().isEmpty()) {
            ++failures;
        }
    } else {
        return 1; // nothing else can run without folders
    }

    Collection inbox;
    const auto cols = foldersJob->allCollections();
    for (const Collection &col : cols) {
        if (col.name().compare(QLatin1String("inbox"), Qt::CaseInsensitive) == 0 || col.name() == QLatin1String("Posteingang")) {
            inbox = col;
            break;
        }
    }
    if (inbox.remoteId().isEmpty()) {
        std::printf("FAIL  no inbox found\n");
        return 1;
    }

    // --- 2. message delta ----------------------------------------------------
    auto itemsJob = new GraphFetchItemsJob(client, inbox, QString());
    Item::List stubs;
    if (runJob(itemsJob, "GraphFetchItemsJob")) {
        stubs = itemsJob->changedItems();
        std::printf("OK    %-28s %lld items, deltaLink: %s\n",
                    "GraphFetchItemsJob",
                    static_cast<long long>(stubs.size()),
                    itemsJob->deltaLink().isEmpty() ? "MISSING" : "yes");
        if (stubs.isEmpty()) {
            ++failures;
        }
    } else {
        ++failures;
    }

    // --- 3. MIME payload (batch of several -> exercises /$batch, out-of-order) --
    if (!stubs.isEmpty()) {
        Item::List batch = stubs.mid(0, qMin(5, stubs.size()));
        auto payloadJob = new GraphFetchItemPayloadJob(client, batch);
        if (runJob(payloadJob, "GraphFetchItemPayloadJob")) {
            int parsed = 0;
            for (const Item &it : payloadJob->items()) {
                if (it.hasPayload<std::shared_ptr<KMime::Message>>() && !it.payload<std::shared_ptr<KMime::Message>>()->head().isEmpty()) {
                    ++parsed;
                }
            }
            const auto msg = payloadJob->items().first().payload<std::shared_ptr<KMime::Message>>();
            std::printf("OK    %-28s %d/%lld parsed via $batch, first: \"%s\"\n",
                        "GraphFetchItemPayloadJob",
                        parsed,
                        static_cast<long long>(batch.size()),
                        qPrintable(msg->subject()->asUnicodeString().left(40)));
            if (parsed != batch.size()) {
                ++failures;
            }
        } else {
            ++failures;
        }
    }

    // --- 4. idempotent flag PATCH (write path, no visible change) -------------
    if (!stubs.isEmpty()) {
        const Item &item = stubs.first();
        QJsonObject body;
        body.insert(QStringLiteral("isRead"), item.hasFlag(MessageFlags::Seen));
        auto patchJob = new GraphBatchJob(client, {{GraphRequest::Method::Patch, QStringLiteral("/me/messages/%1").arg(item.remoteId()), body}});
        if (runJob(patchJob, "GraphBatchJob (PATCH flags)")) {
            std::printf("OK    %-28s isRead re-asserted\n", "GraphBatchJob (PATCH flags)");
        } else {
            ++failures;
        }
    }

    // --- 5. draft create (base64 MIME) + delete ------------------------------
    {
        KMime::Message msg;
        msg.subject()->fromUnicodeString(QStringLiteral("akonadi_graph_resource smoke test"));
        msg.contentType()->setMimeType("text/plain");
        msg.setBody("smoke test - safe to delete\n");
        msg.assemble();

        auto createReq = new GraphRequest(client);
        createReq->setMethod(GraphRequest::Method::Post);
        createReq->setPath(QStringLiteral("/me/messages"));
        createReq->setRawBody(msg.encodedContent(KMime::NewlineType::CRLF).toBase64(), QByteArrayLiteral("text/plain"));
        if (runJob(createReq, "draft create (POST MIME)")) {
            const QString draftId = createReq->responseObject().value(QLatin1String("id")).toString();
            std::printf("OK    %-28s id: %s...\n", "draft create (POST MIME)", qPrintable(draftId.left(20)));

            auto delJob = new GraphBatchJob(client, {{GraphRequest::Method::Delete, QStringLiteral("/me/messages/%1").arg(draftId), {}}});
            if (runJob(delJob, "draft delete (DELETE)")) {
                std::printf("OK    %-28s cleaned up\n", "draft delete (DELETE)");
            } else {
                ++failures;
            }
        } else {
            ++failures;
        }
    }

    std::printf("%s (%d failures)\n", failures ? "SMOKE TEST FAILED" : "SMOKE TEST PASSED", failures);
    return failures ? 1 : 0;
}
