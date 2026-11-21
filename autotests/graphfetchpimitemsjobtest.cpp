/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Unit tests for the calendar/contacts/task delta job.
*/

#include "jobs/graphfetchpimitemsjob.h"
#include "graphclient/auth/graphoauth.h"
#include "graphclient/graphclient.h"
#include "scriptedgraphserver.h"

#include <KCalendarCore/Incidence>
#include <QSignalSpy>
#include <QTest>
#include <QTimeZone>

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
        QHash<QString, QStringList> series;
    };

    /// A task list delta (or @p type); @p storedLink is appended to the server's address.
    [[nodiscard]] Run
    run(QList<ScriptedGraphServer::Reply> replies, const QString &storedLink = {}, GraphFetchPimItemsJob::Type type = GraphFetchPimItemsJob::Type::Todos)
    {
        ScriptedGraphServer server(std::move(replies));
        GraphOAuth auth(QStringLiteral("tenant"), QStringLiteral("client"), QStringLiteral("wallet"));
        GraphClient client;
        client.setAuth(&auth);
        client.setBaseUrl(server.baseUrl());
        Collection list(1);
        list.setRemoteId(QStringLiteral("list1"));
        const QString deltaLink = storedLink.isEmpty() ? QString() : server.baseUrl() + storedLink;
        auto job = new GraphFetchPimItemsJob(client, list, type, deltaLink, this);
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
        r.series = job->seriesExceptions();
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

    // Review 2026-11, C2: the events delta lists series masters only; their changed
    // occurrences are fetched through them and become exception items of the series.
    void shouldFetchTheChangedOccurrencesOfASeries()
    {
        const QByteArray delta = QByteArrayLiteral(
            "{\"value\":[{\"id\":\"MASTER\",\"iCalUId\":\"SERIES-UID\",\"type\":\"seriesMaster\",\"subject\":\"Daily\","
            "\"start\":{\"dateTime\":\"2027-03-01T09:00:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"end\":{\"dateTime\":\"2027-03-01T09:30:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"recurrence\":{\"pattern\":{\"type\":\"daily\",\"interval\":1},"
            "\"range\":{\"type\":\"numbered\",\"startDate\":\"2027-03-01\",\"numberOfOccurrences\":5,\"recurrenceTimeZone\":\"UTC\"}}},"
            "{\"id\":\"SINGLE\",\"type\":\"singleInstance\",\"subject\":\"Once\","
            "\"start\":{\"dateTime\":\"2027-03-01T12:00:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"end\":{\"dateTime\":\"2027-03-01T13:00:00.0000000\",\"timeZone\":\"UTC\"}}],"
            "\"@odata.deltaLink\":\"{BASE}/delta?token=2\"}");
        const QByteArray ids =
            QByteArrayLiteral("{\"responses\":[{\"id\":\"0\",\"status\":200,\"body\":{\"id\":\"MASTER\",\"exceptionOccurrences\":[{\"id\":\"OCC2\"}]}}]}");
        const QByteArray occurrence = QByteArrayLiteral(
            "{\"responses\":[{\"id\":\"0\",\"status\":200,\"body\":{\"id\":\"OCC2\",\"iCalUId\":\"OTHER\",\"type\":\"exception\","
            "\"subject\":\"Daily, later\",\"originalStart\":\"2027-03-02T09:00:00Z\","
            "\"start\":{\"dateTime\":\"2027-03-02T15:00:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"end\":{\"dateTime\":\"2027-03-02T15:30:00.0000000\",\"timeZone\":\"UTC\"}}}]}");
        const Run r = run({{200, delta}, {200, ids}, {200, occurrence}}, {}, GraphFetchPimItemsJob::Type::Events);
        QCOMPARE(r.error, 0);
        QCOMPARE(r.changed.size(), 3);
        const Item &exception = r.changed.at(2);
        QCOMPARE(exception.remoteId(), QStringLiteral("OCC2"));
        QCOMPARE(exception.remoteRevision(), GraphFetchPimItemsJob::exceptionRevision(QStringLiteral("MASTER")));
        const auto incidence = exception.payload<KCalendarCore::Incidence::Ptr>();
        QCOMPARE(incidence->uid(), QStringLiteral("SERIES-UID"));
        QCOMPARE(incidence->recurrenceId(), QDateTime(QDate(2027, 3, 2), QTime(9, 0), QTimeZone::utc()));
        QCOMPARE(r.series.value(QStringLiteral("MASTER")), QStringList{QStringLiteral("OCC2")});
        // Only the series was asked about.
        QVERIFY(r.requests.at(1).body.contains("/me/events/MASTER?$select=id&$expand=exceptionOccurrences"));
        QVERIFY(!r.requests.at(1).body.contains("SINGLE"));
    }

    void shouldListASeriesWithoutChangedOccurrences()
    {
        const QByteArray delta = QByteArrayLiteral(
            "{\"value\":[{\"id\":\"MASTER\",\"type\":\"seriesMaster\",\"subject\":\"Daily\","
            "\"start\":{\"dateTime\":\"2027-03-01T09:00:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"end\":{\"dateTime\":\"2027-03-01T09:30:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"recurrence\":{\"pattern\":{\"type\":\"daily\",\"interval\":1},"
            "\"range\":{\"type\":\"noEnd\",\"startDate\":\"2027-03-01\",\"recurrenceTimeZone\":\"UTC\"}}}],"
            "\"@odata.deltaLink\":\"{BASE}/delta?token=2\"}");
        const QByteArray ids = QByteArrayLiteral("{\"responses\":[{\"id\":\"0\",\"status\":200,\"body\":{\"id\":\"MASTER\",\"exceptionOccurrences\":[]}}]}");
        const Run r = run({{200, delta}, {200, ids}}, QStringLiteral("/delta?token=1"), GraphFetchPimItemsJob::Type::Events);
        QCOMPARE(r.error, 0);
        QCOMPARE(r.changed.size(), 1);
        // An empty list, not none: local exception items of this series are stale.
        QVERIFY(r.series.contains(QStringLiteral("MASTER")));
        QVERIFY(r.series.value(QStringLiteral("MASTER")).isEmpty());
    }

    // Found in the second review: a series made a single event in Outlook comes as
    // singleInstance; its local exception items must go.
    void shouldListNoChangedOccurrencesForASingleEvent()
    {
        const QByteArray delta = QByteArrayLiteral(
            "{\"value\":[{\"id\":\"WAS-SERIES\",\"type\":\"singleInstance\",\"subject\":\"Once now\","
            "\"start\":{\"dateTime\":\"2027-03-01T12:00:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"end\":{\"dateTime\":\"2027-03-01T13:00:00.0000000\",\"timeZone\":\"UTC\"}}],"
            "\"@odata.deltaLink\":\"{BASE}/delta?token=2\"}");
        const Run r = run({{200, delta}}, QStringLiteral("/delta?token=1"), GraphFetchPimItemsJob::Type::Events);
        QCOMPARE(r.error, 0);
        QVERIFY(r.series.contains(QStringLiteral("WAS-SERIES")));
        QVERIFY(r.series.value(QStringLiteral("WAS-SERIES")).isEmpty());
        QCOMPARE(r.requests.size(), 1); // nothing to ask of a single event
    }

    // Found in the second review: one series refused for good failed every poll.
    void shouldGoOnWithoutASeriesThatIsRefusedForGood()
    {
        const QByteArray delta = QByteArrayLiteral(
            "{\"value\":[{\"id\":\"MASTER\",\"type\":\"seriesMaster\",\"subject\":\"Daily\","
            "\"start\":{\"dateTime\":\"2027-03-01T09:00:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"end\":{\"dateTime\":\"2027-03-01T09:30:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"recurrence\":{\"pattern\":{\"type\":\"daily\",\"interval\":1},"
            "\"range\":{\"type\":\"noEnd\",\"startDate\":\"2027-03-01\",\"recurrenceTimeZone\":\"UTC\"}}}],"
            "\"@odata.deltaLink\":\"{BASE}/delta?token=2\"}");
        const QByteArray refused =
            QByteArrayLiteral("{\"responses\":[{\"id\":\"0\",\"status\":403,\"body\":{\"error\":{\"code\":\"ErrorAccessDenied\",\"message\":\"no\"}}}]}");
        const Run r = run({{200, delta}, {200, refused}}, QStringLiteral("/delta?token=1"), GraphFetchPimItemsJob::Type::Events);
        QCOMPARE(r.error, 0);
        QCOMPARE(r.changed.size(), 1);
        // No statement about its occurrences: its local exception items are kept.
        QVERIFY(!r.series.contains(QStringLiteral("MASTER")));
    }

    void shouldFailTheRoundWhenTheOccurrencesCannotBeAskedNow()
    {
        const QByteArray delta = QByteArrayLiteral(
            "{\"value\":[{\"id\":\"MASTER\",\"type\":\"seriesMaster\",\"subject\":\"Daily\","
            "\"start\":{\"dateTime\":\"2027-03-01T09:00:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"end\":{\"dateTime\":\"2027-03-01T09:30:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"recurrence\":{\"pattern\":{\"type\":\"daily\",\"interval\":1},"
            "\"range\":{\"type\":\"noEnd\",\"startDate\":\"2027-03-01\",\"recurrenceTimeZone\":\"UTC\"}}}],"
            "\"@odata.deltaLink\":\"{BASE}/delta?token=2\"}");
        const QByteArray down = QByteArrayLiteral("{\"responses\":[{\"id\":\"0\",\"status\":500,\"body\":{}}]}");
        const Run r = run({{200, delta}, {200, down}}, QStringLiteral("/delta?token=1"), GraphFetchPimItemsJob::Type::Events);
        // Uncertain: the round fails, so that its delta link is not saved.
        QVERIFY(r.error != 0);
    }

    // Found in review: a full listing deletes whatever it does not list, so a series
    // whose occurrences could not be asked must fail it (the mapping version bump
    // makes the next round of every calendar a full listing).
    void shouldFailAFullListingWhenASeriesIsRefused()
    {
        const QByteArray delta = QByteArrayLiteral(
            "{\"value\":[{\"id\":\"MASTER\",\"type\":\"seriesMaster\",\"subject\":\"Daily\","
            "\"start\":{\"dateTime\":\"2027-03-01T09:00:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"end\":{\"dateTime\":\"2027-03-01T09:30:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"recurrence\":{\"pattern\":{\"type\":\"daily\",\"interval\":1},"
            "\"range\":{\"type\":\"noEnd\",\"startDate\":\"2027-03-01\",\"recurrenceTimeZone\":\"UTC\"}}}],"
            "\"@odata.deltaLink\":\"{BASE}/delta?token=2\"}");
        const QByteArray refused =
            QByteArrayLiteral("{\"responses\":[{\"id\":\"0\",\"status\":403,\"body\":{\"error\":{\"code\":\"ErrorAccessDenied\",\"message\":\"no\"}}}]}");
        const Run r = run({{200, delta}, {200, refused}}, {}, GraphFetchPimItemsJob::Type::Events);
        QVERIFY(r.error != 0);
    }

    void shouldFailTheRoundOnABatchRefusedAsAWhole()
    {
        // The /$batch itself answered 400: every call counts as refused, which is no
        // statement about any series; in a full listing that must not delete them all.
        const QByteArray delta = QByteArrayLiteral(
            "{\"value\":[{\"id\":\"MASTER\",\"type\":\"seriesMaster\",\"subject\":\"Daily\","
            "\"start\":{\"dateTime\":\"2027-03-01T09:00:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"end\":{\"dateTime\":\"2027-03-01T09:30:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"recurrence\":{\"pattern\":{\"type\":\"daily\",\"interval\":1},"
            "\"range\":{\"type\":\"noEnd\",\"startDate\":\"2027-03-01\",\"recurrenceTimeZone\":\"UTC\"}}}],"
            "\"@odata.deltaLink\":\"{BASE}/delta?token=2\"}");
        const Run r =
            run({{200, delta}, {400, QByteArrayLiteral("{\"error\":{\"code\":\"BadRequest\",\"message\":\"no\"}}")}}, {}, GraphFetchPimItemsJob::Type::Events);
        QVERIFY(r.error != 0);
    }

    void shouldNotTakeAnUnlistedStatusForRefusedForGood()
    {
        // 423 (locked) and the like pass: they fail an incremental round too.
        const QByteArray delta = QByteArrayLiteral(
            "{\"value\":[{\"id\":\"MASTER\",\"type\":\"seriesMaster\",\"subject\":\"Daily\","
            "\"start\":{\"dateTime\":\"2027-03-01T09:00:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"end\":{\"dateTime\":\"2027-03-01T09:30:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"recurrence\":{\"pattern\":{\"type\":\"daily\",\"interval\":1},"
            "\"range\":{\"type\":\"noEnd\",\"startDate\":\"2027-03-01\",\"recurrenceTimeZone\":\"UTC\"}}}],"
            "\"@odata.deltaLink\":\"{BASE}/delta?token=2\"}");
        const QByteArray locked = QByteArrayLiteral("{\"responses\":[{\"id\":\"0\",\"status\":423,\"body\":{}}]}");
        const Run r = run({{200, delta}, {200, locked}}, QStringLiteral("/delta?token=1"), GraphFetchPimItemsJob::Type::Events);
        QVERIFY(r.error != 0);
    }

    void shouldKeepASeriesWithoutStatementWhenOneOccurrenceIsRefused()
    {
        // A refused, B gone, C fine: no statement about the series (found in review:
        // the removal of the statement was undone by B, and C was deleted).
        const QByteArray delta = QByteArrayLiteral(
            "{\"value\":[{\"id\":\"MASTER\",\"iCalUId\":\"UID\",\"type\":\"seriesMaster\",\"subject\":\"Daily\","
            "\"start\":{\"dateTime\":\"2027-03-01T09:00:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"end\":{\"dateTime\":\"2027-03-01T09:30:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"recurrence\":{\"pattern\":{\"type\":\"daily\",\"interval\":1},"
            "\"range\":{\"type\":\"noEnd\",\"startDate\":\"2027-03-01\",\"recurrenceTimeZone\":\"UTC\"}}}],"
            "\"@odata.deltaLink\":\"{BASE}/delta?token=2\"}");
        const QByteArray ids = QByteArrayLiteral(
            "{\"responses\":[{\"id\":\"0\",\"status\":200,\"body\":{\"id\":\"MASTER\",\"exceptionOccurrences\":[{\"id\":\"A\"},{\"id\":\"B\"},{\"id\":\"C\"}]}}"
            "]}");
        const QByteArray occurrences = QByteArrayLiteral(
            "{\"responses\":["
            "{\"id\":\"0\",\"status\":403,\"body\":{\"error\":{\"code\":\"ErrorAccessDenied\",\"message\":\"no\"}}},"
            "{\"id\":\"1\",\"status\":404,\"body\":{\"error\":{\"code\":\"ErrorItemNotFound\",\"message\":\"gone\"}}},"
            "{\"id\":\"2\",\"status\":200,\"body\":{\"id\":\"C\",\"type\":\"exception\",\"subject\":\"Later\","
            "\"originalStart\":\"2027-03-03T09:00:00Z\","
            "\"start\":{\"dateTime\":\"2027-03-03T15:00:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"end\":{\"dateTime\":\"2027-03-03T15:30:00.0000000\",\"timeZone\":\"UTC\"}}}]}");
        const Run r = run({{200, delta}, {200, ids}, {200, occurrences}}, QStringLiteral("/delta?token=1"), GraphFetchPimItemsJob::Type::Events);
        QCOMPARE(r.error, 0);
        QVERIFY(!r.series.contains(QStringLiteral("MASTER")));
        // C came, though A before it was refused: it is delivered all the same.
        QCOMPARE(r.changed.size(), 2);
        QCOMPARE(r.changed.at(1).remoteId(), QStringLiteral("C"));
    }

    // Found in the fourth review: the batch itself failing must fail the round, in
    // an incremental round too, and also when its status reads 200.
    void shouldFailAnIncrementalRoundOnABatchRefusedAsAWhole()
    {
        const QByteArray delta = QByteArrayLiteral(
            "{\"value\":[{\"id\":\"MASTER\",\"type\":\"seriesMaster\",\"subject\":\"Daily\","
            "\"start\":{\"dateTime\":\"2027-03-01T09:00:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"end\":{\"dateTime\":\"2027-03-01T09:30:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"recurrence\":{\"pattern\":{\"type\":\"daily\",\"interval\":1},"
            "\"range\":{\"type\":\"noEnd\",\"startDate\":\"2027-03-01\",\"recurrenceTimeZone\":\"UTC\"}}}],"
            "\"@odata.deltaLink\":\"{BASE}/delta?token=2\"}");
        const Run r = run({{200, delta}, {400, QByteArrayLiteral("{\"error\":{\"code\":\"BadRequest\",\"message\":\"no\"}}")}},
                          QStringLiteral("/delta?token=1"),
                          GraphFetchPimItemsJob::Type::Events);
        QVERIFY(r.error != 0);
    }

    void shouldFailTheRoundOnABatchAnswerThatCannotBeRead()
    {
        const QByteArray delta = QByteArrayLiteral(
            "{\"value\":[{\"id\":\"MASTER\",\"type\":\"seriesMaster\",\"subject\":\"Daily\","
            "\"start\":{\"dateTime\":\"2027-03-01T09:00:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"end\":{\"dateTime\":\"2027-03-01T09:30:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"recurrence\":{\"pattern\":{\"type\":\"daily\",\"interval\":1},"
            "\"range\":{\"type\":\"noEnd\",\"startDate\":\"2027-03-01\",\"recurrenceTimeZone\":\"UTC\"}}}],"
            "\"@odata.deltaLink\":\"{BASE}/delta?token=2\"}");
        const Run r =
            run({{200, delta}, {200, QByteArrayLiteral("{\"responses\":[{\"id\":")}}, QStringLiteral("/delta?token=1"), GraphFetchPimItemsJob::Type::Events);
        QVERIFY(r.error != 0);
    }

    void shouldTakeTheLaterEntryOfAnIdListedTwice()
    {
        // First as a series, then (made a single event meanwhile) as a single event:
        // nothing to ask, and its exception items go.
        const QByteArray delta = QByteArrayLiteral(
            "{\"value\":[{\"id\":\"X\",\"type\":\"seriesMaster\",\"subject\":\"Daily\","
            "\"start\":{\"dateTime\":\"2027-03-01T09:00:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"end\":{\"dateTime\":\"2027-03-01T09:30:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"recurrence\":{\"pattern\":{\"type\":\"daily\",\"interval\":1},"
            "\"range\":{\"type\":\"noEnd\",\"startDate\":\"2027-03-01\",\"recurrenceTimeZone\":\"UTC\"}}},"
            "{\"id\":\"X\",\"type\":\"singleInstance\",\"subject\":\"Once\","
            "\"start\":{\"dateTime\":\"2027-03-01T09:00:00.0000000\",\"timeZone\":\"UTC\"},"
            "\"end\":{\"dateTime\":\"2027-03-01T09:30:00.0000000\",\"timeZone\":\"UTC\"}}],"
            "\"@odata.deltaLink\":\"{BASE}/delta?token=2\"}");
        const Run r = run({{200, delta}}, QStringLiteral("/delta?token=1"), GraphFetchPimItemsJob::Type::Events);
        QCOMPARE(r.error, 0);
        QCOMPARE(r.requests.size(), 1);
        QVERIFY(r.series.contains(QStringLiteral("X")));
        QVERIFY(r.series.value(QStringLiteral("X")).isEmpty()); // One item for it, the later one.
        QCOMPARE(r.changed.size(), 1);
        QCOMPARE(r.changed.first().payload<KCalendarCore::Incidence::Ptr>()->summary(), QStringLiteral("Once"));
    }
};

QTEST_GUILESS_MAIN(GraphFetchPimItemsJobTest)

#include "graphfetchpimitemsjobtest.moc"
