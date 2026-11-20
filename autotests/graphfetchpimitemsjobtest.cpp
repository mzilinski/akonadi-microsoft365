/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Unit tests for the calendar/contacts/task delta job.
*/

#include "jobs/graphfetchpimitemsjob.h"
#include "graphclient/auth/graphoauth.h"
#include "graphclient/graphclient.h"
#include "scriptedgraphserver.h"

#include <QSignalSpy>
#include <QTest>

using namespace Akonadi;

namespace
{
const QByteArray kOnePage = QByteArrayLiteral("{\"value\":[{\"id\":\"t1\",\"title\":\"Task\"}],\"@odata.deltaLink\":\"{BASE}/delta?token=2\"}");
}

class GraphFetchPimItemsJobTest : public QObject
{
    Q_OBJECT
private:
    struct Run {
        int error = 0;
        bool fullListing = false;
        Item::List changed;
        QString deltaLink;
        QList<ScriptedGraphServer::Request> requests;
    };

    /// A task list delta; @p storedLink is appended to the server's address.
    [[nodiscard]] Run run(QList<ScriptedGraphServer::Reply> replies, const QString &storedLink = {})
    {
        ScriptedGraphServer server(std::move(replies));
        GraphOAuth auth(QStringLiteral("tenant"), QStringLiteral("client"), QStringLiteral("wallet"));
        GraphClient client;
        client.setAuth(&auth);
        client.setBaseUrl(server.baseUrl());
        Collection list(1);
        list.setRemoteId(QStringLiteral("list1"));
        const QString deltaLink = storedLink.isEmpty() ? QString() : server.baseUrl() + storedLink;
        auto job = new GraphFetchPimItemsJob(client, list, GraphFetchPimItemsJob::Type::Todos, deltaLink, this);
        QSignalSpy spy(job, &KJob::result);
        job->start();
        Run r;
        if (!spy.wait(60000)) {
            r.error = -1;
            return r;
        }
        r.error = job->error();
        r.fullListing = job->isFullListing();
        r.changed = job->changedItems();
        r.deltaLink = job->deltaLink();
        r.requests = server.requests();
        return r;
    }

private Q_SLOTS:
    void shouldListInFullWithoutADeltaLink()
    {
        const Run r = run({{200, kOnePage}});
        QCOMPARE(r.error, 0);
        QVERIFY(r.fullListing);
        QCOMPARE(r.changed.size(), 1);
    }

    void shouldContinueIncrementallyFromADeltaLink()
    {
        const Run r = run({{200, kOnePage}}, QStringLiteral("/delta?token=1"));
        QCOMPARE(r.error, 0);
        QVERIFY(!r.fullListing);
    }

    // Review 2026-11, C6: the listing after an expired delta state was delivered as
    // incremental, so what had been deleted meanwhile stayed in the local cache.
    void shouldListInFullAfterTheDeltaStateExpired_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("code");
        QTest::newRow("410 Gone") << 410 << QByteArrayLiteral("SyncStateNotFound");
        QTest::newRow("400 ErrorInvalidSyncStateData") << 400 << QByteArrayLiteral("ErrorInvalidSyncStateData");
        // Review 2026-11, I8: other names Graph gives an expired state.
        QTest::newRow("400 syncStateNotFound") << 400 << QByteArrayLiteral("syncStateNotFound");
        QTest::newRow("400 resyncRequired") << 400 << QByteArrayLiteral("resyncRequired");
    }

    void shouldListInFullAfterTheDeltaStateExpired()
    {
        QFETCH(int, status);
        QFETCH(QByteArray, code);
        const Run r = run({{status, "{\"error\":{\"code\":\"" + code + "\",\"message\":\"expired\"}}"}, {200, kOnePage}}, QStringLiteral("/delta?token=1"));
        QCOMPARE(r.error, 0);
        QVERIFY(r.fullListing);
        QCOMPARE(r.requests.size(), 2);
        QVERIFY(r.requests.at(1).target.startsWith("/me/todo/lists/list1/tasks/delta"));
        QVERIFY(r.deltaLink.endsWith(QLatin1String("token=2")));
    }
};

QTEST_GUILESS_MAIN(GraphFetchPimItemsJobTest)

#include "graphfetchpimitemsjobtest.moc"
