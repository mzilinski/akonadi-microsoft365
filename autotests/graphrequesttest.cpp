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

    [[nodiscard]] Run
    run(QList<ScriptedGraphServer::Reply> replies, GraphRequest::Method method = GraphRequest::Method::Get, const QString &path = QStringLiteral("/me/things"))
    {
        ScriptedGraphServer server(std::move(replies));
        GraphOAuth auth(QStringLiteral("tenant"), QStringLiteral("client"), QStringLiteral("wallet"));
        GraphClient client;
        client.setAuth(&auth);
        client.setBaseUrl(server.baseUrl());
        auto req = new GraphRequest(client, this);
        req->setMethod(method);
        req->setPath(path);
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
};

QTEST_GUILESS_MAIN(GraphRequestTest)

#include "graphrequesttest.moc"
