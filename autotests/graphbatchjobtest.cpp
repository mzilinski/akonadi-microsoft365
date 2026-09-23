/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Unit tests for the fan-out of one change notification into per-item calls.

    Two ways to reach the failure paths without talking to Microsoft: a GraphClient
    without an authentication object fails every request before it reaches the
    network, and FakeGraphServer answers /$batch with a scripted list of per-call
    status codes where a real response is needed.
*/

#include "jobs/graphbatchjob.h"
#include "graphclient/auth/graphoauth.h"
#include "graphclient/graphclient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>

#include <QTest>
#include <memory>

/// Answers each /$batch sub-request with the next status code from the list it was
/// given (200 once the list is used up).
class FakeGraphServer : public QTcpServer
{
    Q_OBJECT
public:
    explicit FakeGraphServer(QList<int> statuses, QObject *parent = nullptr)
        : QTcpServer(parent)
        , mStatuses(std::move(statuses))
    {
        listen(QHostAddress::LocalHost);
    }

    [[nodiscard]] QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(serverPort());
    }

    /// How many sub-requests arrived — the calls the job really issued.
    [[nodiscard]] int served() const
    {
        return mServed;
    }

    /// Throttle the last entry of every batch that has more than one, the way Graph
    /// answers when a mailbox has too many requests in flight.
    void setThrottleLastOfEachBatch(bool throttle)
    {
        mThrottleLast = throttle;
    }

    /// How many /$batch round trips it took.
    [[nodiscard]] int batches() const
    {
        return mBatches;
    }

protected:
    void incomingConnection(qintptr handle) override
    {
        auto socket = new QTcpSocket(this);
        socket->setSocketDescriptor(handle);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
            mRequest += socket->readAll();
            const int headerEnd = mRequest.indexOf("\r\n\r\n");
            if (headerEnd < 0) {
                return; // headers still incomplete
            }
            const int bodyStart = headerEnd + 4;
            const int lengthPos = mRequest.toLower().indexOf("content-length:");
            const int length = lengthPos < 0 ? 0 : mRequest.mid(lengthPos + 15, mRequest.indexOf("\r\n", lengthPos) - lengthPos - 15).trimmed().toInt();
            if (mRequest.size() < bodyStart + length) {
                return; // body still incomplete
            }
            const QByteArray reply = answer(mRequest.mid(bodyStart, length));
            mRequest.clear();
            socket->write(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Connection: close\r\n"
                "Content-Length: "
                + QByteArray::number(reply.size()) + "\r\n\r\n" + reply);
            socket->disconnectFromHost();
        });
    }

private:
    [[nodiscard]] QByteArray answer(const QByteArray &batchBody)
    {
        ++mBatches;
        QJsonArray responses;
        const QJsonArray requests = QJsonDocument::fromJson(batchBody).object().value(QLatin1String("requests")).toArray();
        for (qsizetype n = 0; n < requests.size(); ++n) {
            const QJsonValue r = requests.at(n);
            int status = mServed < mStatuses.size() ? mStatuses.at(mServed) : 200;
            if (mThrottleLast && requests.size() > 1 && n == requests.size() - 1) {
                status = 429;
            }
            ++mServed;
            QJsonObject resp;
            resp.insert(QStringLiteral("id"), r.toObject().value(QLatin1String("id")));
            resp.insert(QStringLiteral("status"), status);
            if (status == 429) {
                resp.insert(QStringLiteral("headers"), QJsonObject{{QStringLiteral("Retry-After"), QStringLiteral("1")}});
            }
            resp.insert(QStringLiteral("body"),
                        status < 300 ? QJsonObject{{QStringLiteral("id"), QStringLiteral("moved")}}
                                     : QJsonObject{{QStringLiteral("error"),
                                                    QJsonObject{{QStringLiteral("code"), QStringLiteral("ErrorItemNotFound")},
                                                                {QStringLiteral("message"), QStringLiteral("not found")}}}});
            responses.append(resp);
        }
        return QJsonDocument(QJsonObject{{QStringLiteral("responses"), responses}}).toJson(QJsonDocument::Compact);
    }

    const QList<int> mStatuses;
    QByteArray mRequest;
    int mServed = 0;
    int mBatches = 0;
    bool mThrottleLast = false;
};

// Answers every connection with a fixed raw response, then closes it.
class RawServer : public QTcpServer
{
public:
    explicit RawServer(const QByteArray &response)
        : mResponse(response)
    {
        listen(QHostAddress::LocalHost);
    }
    [[nodiscard]] QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(serverPort());
    }

protected:
    void incomingConnection(qintptr handle) override
    {
        auto socket = new QTcpSocket(this);
        socket->setSocketDescriptor(handle);
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
            // The whole request has arrived once the headers end (no body in these tests
            // matters); answer and hang up.
            if (socket->readAll().contains("\r\n\r\n")) {
                socket->write(mResponse);
                socket->flush();
                socket->disconnectFromHost();
            }
        });
    }

private:
    QByteArray mResponse;
};

// Takes every connection and reads what arrives, but never answers.
class SilentServer : public QTcpServer
{
public:
    SilentServer()
    {
        listen(QHostAddress::LocalHost);
    }
    [[nodiscard]] QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(serverPort());
    }

protected:
    void incomingConnection(qintptr handle) override
    {
        auto socket = new QTcpSocket(this);
        socket->setSocketDescriptor(handle);
        connect(socket, &QTcpSocket::readyRead, socket, [socket] {
            socket->readAll();
        });
    }
};

class GraphBatchJobTest : public QObject
{
    Q_OBJECT
private:
    [[nodiscard]] static QList<GraphBatchJob::Call> deleteCalls(int count)
    {
        QList<GraphBatchJob::Call> calls;
        calls.reserve(count);
        for (int i = 0; i < count; ++i) {
            calls.append({GraphRequest::Method::Delete, QStringLiteral("/me/messages/id%1").arg(i), {}});
        }
        return calls;
    }

    [[nodiscard]] static bool run(GraphBatchJob *job)
    {
        QSignalSpy spy(job, &KJob::result);
        job->start();
        return spy.wait(30000);
    }

    struct LiveClient {
        explicit LiveClient(const FakeGraphServer &server)
            : auth(QStringLiteral("tenant"), QStringLiteral("client"), QStringLiteral("wallet"))
        {
            client.setAuth(&auth);
            client.setBaseUrl(server.baseUrl());
        }
        GraphOAuth auth;
        GraphClient client;
    };

private Q_SLOTS:
    void shouldSucceedWithoutCalls()
    {
        GraphClient client;
        auto job = new GraphBatchJob(client, {}, this);
        QVERIFY(run(job));
        QCOMPARE(job->error(), 0);
        QVERIFY(job->responses().isEmpty());
    }

    void shouldAccountForEveryCallWhenTheServerIsUnreachable()
    {
        // No authentication: the batch request itself fails before the network.
        GraphClient client;
        auto job = new GraphBatchJob(client, deleteCalls(5), this);
        QVERIFY(run(job));
        QVERIFY(job->error() != 0);
        // One response per call, so callers can still match responses to items.
        QCOMPARE(job->responses().size(), 5);
        // Not translated here (no catalog is loaded), so the source string applies.
        QVERIFY(job->errorText().contains(QLatin1String("5 of 5")));
        // Nothing left the machine: safe to send again, and signing in is what is missing.
        QCOMPARE(job->failed(GraphRequest::Failure::NotExecuted), 5);
        QCOMPARE(job->succeeded(), 0);
        QVERIFY(job->authenticationRejected());
    }

    void shouldReportASingleFailureVerbatim()
    {
        GraphClient client;
        auto job = new GraphBatchJob(client, deleteCalls(1), this);
        QVERIFY(run(job));
        // No "1 of 1" noise around the only error there is.
        QCOMPARE(job->errorText(), QStringLiteral("Not authenticated with Microsoft 365 yet"));
    }

    void shouldBundleTwentyCallsPerRoundTrip()
    {
        FakeGraphServer server({});
        LiveClient live(server);
        auto job = new GraphBatchJob(live.client, deleteCalls(25), this);
        QVERIFY(run(job));
        QCOMPARE(job->error(), 0);
        QCOMPARE(server.batches(), 2);
        QCOMPARE(server.served(), 25);
        QCOMPARE(job->responses().size(), 25);
        for (const QJsonObject &response : job->responses()) {
            QCOMPARE(response.value(QLatin1String("id")).toString(), QStringLiteral("moved"));
        }
    }

    void shouldTolerateAMissingItemWhenAsked()
    {
        // Deleting or moving a message that is already gone server-side must not
        // cost the other messages in the same notification their call.
        FakeGraphServer server({200, 404, 200});
        LiveClient live(server);
        auto job = new GraphBatchJob(live.client, deleteCalls(3), this);
        job->setIgnoreNotFound(true);
        QVERIFY(run(job));
        QCOMPARE(job->error(), 0);
        QCOMPARE(server.served(), 3);
        QCOMPARE(job->responses().size(), 3);
    }

    void shouldIssueEveryCallAfterAServerError()
    {
        FakeGraphServer server({200, 500, 200});
        LiveClient live(server);
        auto job = new GraphBatchJob(live.client, deleteCalls(3), this);
        QVERIFY(run(job));
        // The call after the failing one still went out.
        QCOMPARE(server.served(), 3);
        QCOMPARE(job->responses().size(), 3);
        QVERIFY(job->error() != 0);
        QVERIFY(job->errorText().contains(QLatin1String("1 of 3")));
        // Successful calls keep their response (itemsMoved() reads the new ids from
        // it); the failed one is a placeholder.
        QCOMPARE(job->responses().at(0).value(QLatin1String("id")).toString(), QStringLiteral("moved"));
        QVERIFY(job->responses().at(1).isEmpty());
        QCOMPARE(job->responses().at(2).value(QLatin1String("id")).toString(), QStringLiteral("moved"));
        // A 500 may have done the work anyway: only repeatable changes may go again.
        QCOMPARE(job->succeeded(), 2);
        QCOMPARE(job->failed(GraphRequest::Failure::Uncertain), 1);
        QCOMPARE(job->failed(GraphRequest::Failure::NotExecuted), 0);
        QCOMPARE(job->percent(), 100UL);
    }

    void shouldClassifyEveryFailureOfAMixedBatch()
    {
        FakeGraphServer server({400, 401, 504, 200});
        LiveClient live(server);
        auto job = new GraphBatchJob(live.client, deleteCalls(4), this);
        QVERIFY(run(job));
        QCOMPARE(job->succeeded(), 1);
        QCOMPARE(job->failed(GraphRequest::Failure::Permanent), 1);
        QCOMPARE(job->failed(GraphRequest::Failure::NotExecuted), 1);
        QCOMPARE(job->failed(GraphRequest::Failure::Uncertain), 1);
        QVERIFY(job->authenticationRejected());
    }

    void shouldClassifyHttpStatuses_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<GraphRequest::Failure>("failure");
        QTest::newRow("200") << 200 << GraphRequest::Failure::None;
        QTest::newRow("204") << 204 << GraphRequest::Failure::None;
        QTest::newRow("401") << 401 << GraphRequest::Failure::NotExecuted;
        QTest::newRow("408") << 408 << GraphRequest::Failure::NotExecuted;
        QTest::newRow("429") << 429 << GraphRequest::Failure::NotExecuted;
        QTest::newRow("503") << 503 << GraphRequest::Failure::NotExecuted;
        QTest::newRow("500") << 500 << GraphRequest::Failure::Uncertain;
        QTest::newRow("502") << 502 << GraphRequest::Failure::Uncertain;
        QTest::newRow("504") << 504 << GraphRequest::Failure::Uncertain;
        QTest::newRow("400") << 400 << GraphRequest::Failure::Permanent;
        QTest::newRow("403") << 403 << GraphRequest::Failure::Permanent;
        QTest::newRow("404") << 404 << GraphRequest::Failure::Permanent;
    }

    void shouldClassifyHttpStatuses()
    {
        QFETCH(int, status);
        QFETCH(GraphRequest::Failure, failure);
        QCOMPARE(GraphRequest::failureForStatus(status), failure);
    }

    void shouldTreatACutOffSuccessAsUncertain()
    {
        // The status line says 201, then the connection drops mid-body: the server has
        // acted, so this must not count as "nothing happened" (nor as success).
        RawServer server("HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n{\"id\":");
        GraphOAuth auth(QStringLiteral("tenant"), QStringLiteral("client"), QStringLiteral("wallet"));
        GraphClient client;
        client.setAuth(&auth);
        client.setBaseUrl(server.baseUrl());
        auto req = new GraphRequest(client, this);
        req->setMethod(GraphRequest::Method::Post);
        req->setPath(QStringLiteral("/me/events"));
        req->setBody(QJsonObject{{QStringLiteral("subject"), QStringLiteral("x")}});
        QSignalSpy spy(req, &KJob::result);
        req->start();
        QVERIFY(spy.wait(30000));
        QVERIFY(req->error() != 0);
        QCOMPARE(req->httpStatus(), 201);
        QCOMPARE(req->failure(), GraphRequest::Failure::Uncertain);
    }

    void shouldTellAnUnsentTimeoutFromASentOne()
    {
        const auto post = [this](const QString &baseUrl) {
            auto auth = new GraphOAuth(QStringLiteral("tenant"), QStringLiteral("client"), QStringLiteral("wallet"), this);
            auto client = std::make_shared<GraphClient>();
            client->setAuth(auth);
            client->setBaseUrl(baseUrl);
            client->setTransferTimeout(std::chrono::seconds(1));
            auto req = new GraphRequest(*client, this);
            req->setMethod(GraphRequest::Method::Post);
            req->setPath(QStringLiteral("/me/events"));
            req->setBody(QJsonObject{{QStringLiteral("subject"), QStringLiteral("x")}});
            QSignalSpy spy(req, &KJob::result);
            req->start();
            spy.wait(30000);
            return std::make_pair(req, client);
        };

        // The connection never comes up (no answer from a documentation address): the
        // body never left, so the server cannot have created anything.
        const auto unreachable = post(QStringLiteral("http://192.0.2.1:9"));
        QVERIFY(unreachable.first->error() != 0);
        QCOMPARE(unreachable.first->failure(), GraphRequest::Failure::NotExecuted);

        // The server took the request and went silent: it may have acted.
        SilentServer server;
        const auto silent = post(server.baseUrl());
        QVERIFY(silent.first->error() != 0);
        QCOMPARE(silent.first->failure(), GraphRequest::Failure::Uncertain);
    }

    void shouldTrackWorkInFlight()
    {
        FakeGraphServer server({});
        LiveClient live(server);
        int idle = 0;
        live.client.setIdleCallback([&idle] {
            ++idle;
        });
        auto job = new GraphBatchJob(live.client, deleteCalls(25), this);
        QSignalSpy spy(job, &KJob::result);
        job->start();
        QVERIFY(live.client.isBusy());
        QVERIFY(spy.wait(30000));
        QVERIFY(!live.client.isBusy());
        // Once for the whole batch, not for each of its round trips.
        QCOMPARE(idle, 1);
    }

    void shouldOutliveItsClient()
    {
        // On shutdown, jobs can end after the client they ran on is gone.
        auto client = std::make_unique<GraphClient>();
        int idle = 0;
        client->setIdleCallback([&idle] {
            ++idle;
        });
        auto job = new GraphBatchJob(*client, deleteCalls(2), this);
        QSignalSpy spy(job, &KJob::result);
        job->start();
        QVERIFY(client->isBusy());
        client.reset();
        QVERIFY(spy.wait(30000));
        delete job;
        QCOMPARE(idle, 1);
    }

    void shouldKeepRetryingWhileThrottledCallsProgress()
    {
        // Six rounds in a row throttle a different call each. Every call is throttled
        // only once, so nothing may count as failed however many rounds it takes.
        FakeGraphServer server({});
        server.setThrottleLastOfEachBatch(true);
        LiveClient live(server);
        auto job = new GraphBatchJob(live.client, deleteCalls(100), this);
        QVERIFY(run(job));
        QCOMPARE(job->error(), 0);
        QCOMPARE(server.batches(), 7);
        QCOMPARE(server.served(), 106);
    }

    void shouldRetryThrottledCalls()
    {
        // Graph throttles per sub-request; the second one is told to come back later.
        FakeGraphServer server({200, 429, 200});
        LiveClient live(server);
        auto job = new GraphBatchJob(live.client, deleteCalls(3), this);
        QVERIFY(run(job));
        QCOMPARE(job->error(), 0);
        QCOMPARE(server.batches(), 2);
        QCOMPARE(server.served(), 4);
        QCOMPARE(job->responses().at(1).value(QLatin1String("id")).toString(), QStringLiteral("moved"));
    }
};

QTEST_GUILESS_MAIN(GraphBatchJobTest)

#include "graphbatchjobtest.moc"
