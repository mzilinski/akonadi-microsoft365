/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Unit tests for fetching message bodies through /$batch.
*/

#include "jobs/graphfetchitempayloadjob.h"
#include "graphclient/auth/graphoauth.h"
#include "graphclient/graphclient.h"
#include "scriptedgraphserver.h"

#include <KMime/Message>
#include <QSignalSpy>
#include <QTest>

using namespace Akonadi;

class GraphFetchItemPayloadJobTest : public QObject
{
    Q_OBJECT
private:
    struct Run {
        int error = 0;
        Item::List items;
        int requests = 0;
    };

    [[nodiscard]] Run run(QList<ScriptedGraphServer::Reply> replies, const QStringList &ids)
    {
        ScriptedGraphServer server(std::move(replies));
        GraphOAuth auth(QStringLiteral("tenant"), QStringLiteral("client"), QStringLiteral("wallet"));
        GraphClient client;
        client.setAuth(&auth);
        client.setBaseUrl(server.baseUrl());
        Item::List items;
        for (const QString &id : ids) {
            Item item;
            item.setRemoteId(id);
            items.append(item);
        }
        auto job = new GraphFetchItemPayloadJob(client, items, this);
        QSignalSpy spy(job, &KJob::result);
        job->start();
        Run r;
        if (!spy.wait(60000)) {
            r.error = -1;
            return r;
        }
        r.error = job->error();
        r.items = job->items();
        r.requests = server.requests().size();
        return r;
    }

    static QByteArray mime()
    {
        return QByteArrayLiteral("Subject: Hello\r\nFrom: a@example.org\r\n\r\nBody\r\n").toBase64();
    }

private Q_SLOTS:
    // Review 2026-11, I11: one message deleted meanwhile failed the bodies of all
    // twenty in its batch.
    void shouldDeliverTheOthersWhenOneIsGone()
    {
        const QByteArray batch = "{\"responses\":[{\"id\":\"0\",\"status\":200,\"body\":\"" + mime()
            + "\"},{\"id\":\"1\",\"status\":404,\"body\":{\"error\":{\"code\":\"ErrorItemNotFound\",\"message\":\"gone\"}}}]}";
        const Run r = run({{200, batch}}, {QStringLiteral("m1"), QStringLiteral("m2")});
        QCOMPARE(r.error, 0);
        QCOMPARE(r.items.size(), 1);
        QCOMPARE(r.items.first().remoteId(), QStringLiteral("m1"));
        QCOMPARE(r.items.first().payload<std::shared_ptr<KMime::Message>>()->subject()->asUnicodeString(), QStringLiteral("Hello"));
    }

    void shouldAskAgainForAThrottledMessage()
    {
        const QByteArray first = "{\"responses\":[{\"id\":\"0\",\"status\":429,\"headers\":{\"Retry-After\":\"1\"},\"body\":{}}]}";
        const QByteArray second = "{\"responses\":[{\"id\":\"0\",\"status\":200,\"body\":\"" + mime() + "\"}]}";
        const Run r = run({{200, first}, {200, second}}, {QStringLiteral("m1")});
        QCOMPARE(r.error, 0);
        QCOMPARE(r.items.size(), 1);
        QCOMPARE(r.requests, 2);
    }
};

QTEST_GUILESS_MAIN(GraphFetchItemPayloadJobTest)

#include "graphfetchitempayloadjobtest.moc"
