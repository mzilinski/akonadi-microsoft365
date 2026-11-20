/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Unit tests for how GraphRequest reads Graph's answers.
*/

#include "graphclient/graphrequest.h"
#include "graphclient/auth/graphoauth.h"
#include "graphclient/graphclient.h"
#include "scriptedgraphserver.h"

#include <QJsonArray>
#include <QSignalSpy>
#include <QTest>
#include <QTimeZone>

class GraphRequestTest : public QObject
{
    Q_OBJECT
private:
    struct Run {
        int error = 0;
        GraphRequest::Failure failure = GraphRequest::Failure::None;
        QJsonArray values;
        QJsonObject object;
        QString deltaLink;
        QList<ScriptedGraphServer::Request> requests;
    };

    [[nodiscard]] Run run(QList<ScriptedGraphServer::Reply> replies,
                          GraphRequest::Method method = GraphRequest::Method::Get,
                          const QString &path = QStringLiteral("/me/things"),
                          const QUrl &absoluteUrl = {})
    {
        ScriptedGraphServer server(std::move(replies));
        GraphOAuth auth(QStringLiteral("tenant"), QStringLiteral("client"), QStringLiteral("wallet"));
        GraphClient client;
        client.setAuth(&auth);
        client.setBaseUrl(server.baseUrl());
        auto req = new GraphRequest(client, this);
        req->setMethod(method);
        req->setPath(path);
        if (absoluteUrl.isValid()) {
            req->setAbsoluteUrl(absoluteUrl);
        }
        if (method == GraphRequest::Method::Post) {
            req->setBody(QJsonObject{{QStringLiteral("subject"), QStringLiteral("x")}});
        }
        QSignalSpy spy(req, &KJob::result);
        req->start();
        Run r;
        if (!spy.wait(60000)) {
            r.error = -1;
            return r;
        }
        r.error = req->error();
        r.failure = req->failure();
        r.values = req->aggregatedValue();
        r.object = req->responseObject();
        r.deltaLink = req->deltaLink();
        r.requests = server.requests();
        return r;
    }

private Q_SLOTS:
    // Review 2026-11, C7: garbage was read as {} and taken for an empty answer.
    void shouldFailOnAnUnreadableSuccess()
    {
        const Run r = run({{200, QByteArrayLiteral("<html>gateway says hi</html>")}});
        QVERIFY(r.error != 0);
        // The server answered with success: it may have acted.
        QCOMPARE(r.failure, GraphRequest::Failure::Uncertain);
    }

    void shouldFailOnAJsonArrayInsteadOfAnObject()
    {
        QVERIFY(run({{200, QByteArrayLiteral("[]")}}).error != 0);
    }

    void shouldAcceptAnEmptySuccess()
    {
        const Run r = run({{202, {}}}, GraphRequest::Method::Post, QStringLiteral("/me/messages/m1/send"));
        QCOMPARE(r.error, 0);
        QVERIFY(r.object.isEmpty());
    }

    void shouldFailOnAPageWithoutAList()
    {
        const Run r = run({
            {200, QByteArrayLiteral("{\"value\":[{\"id\":\"a\"}],\"@odata.nextLink\":\"{BASE}/page2\"}")},
            {200, QByteArrayLiteral("{\"id\":\"not a page\"}")},
        });
        QVERIFY(r.error != 0);
    }

    void shouldAggregatePages()
    {
        const Run r = run({
            {200, QByteArrayLiteral("{\"value\":[{\"id\":\"a\"}],\"@odata.nextLink\":\"{BASE}/page2\"}")},
            {200, QByteArrayLiteral("{\"value\":[{\"id\":\"b\"}],\"@odata.deltaLink\":\"DELTA\"}")},
        });
        QCOMPARE(r.error, 0);
        QCOMPARE(r.values.size(), 2);
        QCOMPARE(r.deltaLink, QStringLiteral("DELTA"));
        QCOMPARE(r.requests.size(), 2);
        QCOMPARE(r.requests.at(1).target, QByteArrayLiteral("/page2"));
    }

    // Review 2026-11, I1: a 503 can come after Graph did the work; a POST (create,
    // move, /send) repeated on it may run twice.
    void shouldNotRepeatAPostOn503()
    {
        const Run r = run({{503, {}}, {201, QByteArrayLiteral("{\"id\":\"x\"}")}}, GraphRequest::Method::Post, QStringLiteral("/me/events"));
        QVERIFY(r.error != 0);
        QCOMPARE(r.requests.size(), 1);
        QCOMPARE(r.failure, GraphRequest::Failure::Uncertain);
    }

    void shouldRepeatAGetOn503()
    {
        const Run r = run({{503, {}, {{"Retry-After", "1"}}}, {200, QByteArrayLiteral("{\"id\":\"x\"}")}});
        QCOMPARE(r.error, 0);
        QCOMPARE(r.requests.size(), 2);
    }

    void shouldRepeatAPostOn429()
    {
        const Run r =
            run({{429, {}, {{"Retry-After", "1"}}}, {201, QByteArrayLiteral("{\"id\":\"x\"}")}}, GraphRequest::Method::Post, QStringLiteral("/me/events"));
        QCOMPARE(r.error, 0);
        QCOMPARE(r.requests.size(), 2);
    }

    // Review 2026-11, I2: every link was followed with the bearer token.
    void shouldNotFollowANextLinkToAnotherServer()
    {
        const Run r = run({{200, QByteArrayLiteral("{\"value\":[],\"@odata.nextLink\":\"https://graph.example.net/v1.0/me/things?page=2\"}")}});
        QVERIFY(r.error != 0);
        QCOMPARE(r.requests.size(), 1);
    }

    void shouldNotSendAStoredLinkToAnotherServer()
    {
        const Run r =
            run({{200, QByteArrayLiteral("{\"value\":[]}")}}, GraphRequest::Method::Get, {}, QUrl(QStringLiteral("https://graph.example.net/delta?token=1")));
        QVERIFY(r.error != 0);
        QVERIFY(r.requests.isEmpty());
    }

    // Found in review: a tampered stored link failed every poll of its collection.
    void shouldTakeAForeignStoredLinkForAnExpiredDeltaState()
    {
        ScriptedGraphServer server({});
        GraphOAuth auth(QStringLiteral("tenant"), QStringLiteral("client"), QStringLiteral("wallet"));
        GraphClient client;
        client.setAuth(&auth);
        client.setBaseUrl(server.baseUrl());
        auto req = new GraphRequest(client, this);
        req->setAbsoluteUrl(QUrl(QStringLiteral("https://graph.example.net/delta?token=1")));
        QSignalSpy spy(req, &KJob::result);
        req->start();
        QVERIFY(spy.wait(10000));
        QVERIFY(req->error() != 0);
        QVERIFY(req->deltaStateExpired());
    }

    void shouldReadOnlyTheFirstPageWhenAsked()
    {
        ScriptedGraphServer server({{200, QByteArrayLiteral("{\"value\":[{\"id\":\"a\"}],\"@odata.nextLink\":\"{BASE}/page2\"}")}});
        GraphOAuth auth(QStringLiteral("tenant"), QStringLiteral("client"), QStringLiteral("wallet"));
        GraphClient client;
        client.setAuth(&auth);
        client.setBaseUrl(server.baseUrl());
        auto req = new GraphRequest(client, this);
        req->setPath(QStringLiteral("/me/contacts?$top=1"));
        req->setFollowNextLink(false);
        QSignalSpy spy(req, &KJob::result);
        req->start();
        QVERIFY(spy.wait(10000));
        QCOMPARE(req->error(), 0);
        QCOMPARE(req->aggregatedValue().size(), 1);
        QCOMPARE(server.requests().size(), 1);
    }

    void shouldStopAListThatLinksToItself()
    {
        const Run r = run({
            {200, QByteArrayLiteral("{\"value\":[{\"id\":\"a\"}],\"@odata.nextLink\":\"{BASE}/me/things\"}")},
            {200, QByteArrayLiteral("{\"value\":[{\"id\":\"a\"}],\"@odata.nextLink\":\"{BASE}/me/things\"}")},
        });
        QVERIFY(r.error != 0);
        QCOMPARE(r.requests.size(), 1);
    }

    void shouldNotFollowRedirects()
    {
        const Run r = run({{302, {}, {{"Location", "https://graph.example.net/steal"}}}, {200, QByteArrayLiteral("{}")}});
        QVERIFY(r.error != 0);
        QCOMPARE(r.requests.size(), 1);
    }

    void shouldOnlyCountTheOwnServerAsOwn_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("own");
        QTest::newRow("same") << QStringLiteral("https://graph.microsoft.com/v1.0/me/x") << true;
        QTest::newRow("explicit port") << QStringLiteral("https://graph.microsoft.com:443/beta/x") << true;
        QTest::newRow("other host") << QStringLiteral("https://evil.example.com/v1.0/me/x") << false;
        QTest::newRow("suffix trick") << QStringLiteral("https://graph.microsoft.com.evil.example/v1.0") << false;
        QTest::newRow("http") << QStringLiteral("http://graph.microsoft.com/v1.0/me/x") << false;
        QTest::newRow("other port") << QStringLiteral("https://graph.microsoft.com:8443/v1.0") << false;
        QTest::newRow("user info") << QStringLiteral("https://user@graph.microsoft.com/v1.0") << false;
        QTest::newRow("relative") << QStringLiteral("/v1.0/me/x") << false;
    }

    void shouldOnlyCountTheOwnServerAsOwn()
    {
        QFETCH(QString, url);
        QFETCH(bool, own);
        QCOMPARE(GraphRequest::isOwnServer(QUrl(url), QStringLiteral("https://graph.microsoft.com/v1.0")), own);
    }

    // Review 2026-11, I7: an HTTP date became 0, and with it a 1 s backoff.
    void shouldReadRetryAfter_data()
    {
        QTest::addColumn<QByteArray>("value");
        QTest::addColumn<int>("seconds");
        QTest::newRow("seconds") << QByteArrayLiteral("120") << 120;
        QTest::newRow("absent") << QByteArray() << 0;
        QTest::newRow("http date") << QByteArrayLiteral("Fri, 20 Nov 2026 12:02:00 GMT") << 120;
        QTest::newRow("past date") << QByteArrayLiteral("Fri, 20 Nov 2026 11:00:00 GMT") << 0;
        QTest::newRow("garbage") << QByteArrayLiteral("soon") << 0;
    }

    void shouldReadRetryAfter()
    {
        QFETCH(QByteArray, value);
        QFETCH(int, seconds);
        const QDateTime now(QDate(2026, 11, 20), QTime(12, 0), QTimeZone::utc());
        QCOMPARE(GraphRequest::retryAfterSeconds(value, now), seconds);
    }

    void shouldTakeA503ForNotExecutedOnlyWhenRepeatable()
    {
        QCOMPARE(GraphRequest::failureForStatus(503, true), GraphRequest::Failure::NotExecuted);
        QCOMPARE(GraphRequest::failureForStatus(503, false), GraphRequest::Failure::Uncertain);
        QCOMPARE(GraphRequest::failureForStatus(429, false), GraphRequest::Failure::NotExecuted);
    }
};

QTEST_GUILESS_MAIN(GraphRequestTest)

#include "graphrequesttest.moc"
