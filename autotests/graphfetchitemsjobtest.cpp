/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Unit tests for turning a mail delta into Akonadi item stubs.

    FakeDeltaServer answers the delta GET with a scripted page and /$batch with the
    full envelope of every requested message (404 for ids marked as gone), so the
    job runs against something that looks like Graph without talking to Microsoft.
*/

#include "graphclient/auth/graphoauth.h"
#include "graphclient/graphclient.h"
#include "jobs/graphfetchitemsjob.h"

#include <Akonadi/MessageFlags>
#include <KMime/Message>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

using namespace Akonadi;

class FakeDeltaServer : public QTcpServer
{
    Q_OBJECT
public:
    FakeDeltaServer(QJsonArray deltaEntries, QStringList goneIds, QObject *parent = nullptr)
        : QTcpServer(parent)
        , mDeltaEntries(std::move(deltaEntries))
        , mGoneIds(std::move(goneIds))
    {
        listen(QHostAddress::LocalHost);
    }

    [[nodiscard]] QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(serverPort());
    }

    /// The raw delta request (request line and headers).
    [[nodiscard]] QByteArray deltaRequest() const
    {
        return mDeltaRequest;
    }

    /// Message ids the job asked for through /$batch.
    [[nodiscard]] QStringList batchedIds() const
    {
        return mBatchedIds;
    }

    /// Envelope the server reports for a message id.
    [[nodiscard]] static QJsonObject envelope(const QString &id)
    {
        return QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("isRead"), true},
            {QStringLiteral("subject"), QStringLiteral("Subject of %1").arg(id)},
            {QStringLiteral("from"),
             QJsonObject{{QStringLiteral("emailAddress"),
                          QJsonObject{{QStringLiteral("name"), QStringLiteral("Sender")}, {QStringLiteral("address"), QStringLiteral("sender@example.org")}}}}},
            {QStringLiteral("receivedDateTime"), QStringLiteral("2026-10-27T14:46:33Z")},
        };
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
            if (!mRequest.startsWith("POST")) {
                mDeltaRequest = mRequest.left(headerEnd);
            }
            const QByteArray reply = mRequest.startsWith("POST") ? answerBatch(mRequest.mid(bodyStart, length)) : answerDelta();
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
    [[nodiscard]] QByteArray answerDelta() const
    {
        const QJsonObject page{{QStringLiteral("value"), mDeltaEntries}, {QStringLiteral("@odata.deltaLink"), QString(baseUrl() + QStringLiteral("/delta?token=1"))}};
        return QJsonDocument(page).toJson(QJsonDocument::Compact);
    }

    [[nodiscard]] QByteArray answerBatch(const QByteArray &batchBody)
    {
        QJsonArray responses;
        const QJsonArray requests = QJsonDocument::fromJson(batchBody).object().value(QLatin1String("requests")).toArray();
        for (const auto &r : requests) {
            const QString url = r.toObject().value(QLatin1String("url")).toString();
            const QString id = url.section(QLatin1Char('/'), -1).section(QLatin1Char('?'), 0, 0);
            mBatchedIds.append(id);
            const bool gone = mGoneIds.contains(id);
            QJsonObject resp;
            resp.insert(QStringLiteral("id"), r.toObject().value(QLatin1String("id")));
            resp.insert(QStringLiteral("status"), gone ? 404 : 200);
            resp.insert(QStringLiteral("body"),
                        gone ? QJsonObject{{QStringLiteral("error"),
                                            QJsonObject{{QStringLiteral("code"), QStringLiteral("ErrorItemNotFound")},
                                                        {QStringLiteral("message"), QStringLiteral("not found")}}}}
                             : envelope(id));
            responses.append(resp);
        }
        return QJsonDocument(QJsonObject{{QStringLiteral("responses"), responses}}).toJson(QJsonDocument::Compact);
    }

    const QJsonArray mDeltaEntries;
    const QStringList mGoneIds;
    QStringList mBatchedIds;
    QByteArray mDeltaRequest;
    QByteArray mRequest;
};

class GraphFetchItemsJobTest : public QObject
{
    Q_OBJECT
private:
    struct Run {
        Item::List changed;
        Item::List removed;
        QStringList batchedIds;
        QByteArray deltaRequest;
        int error = 0;
    };

    [[nodiscard]] Run run(const QJsonArray &entries, const QStringList &goneIds = {})
    {
        FakeDeltaServer server(entries, goneIds);
        GraphOAuth auth(QStringLiteral("tenant"), QStringLiteral("client"), QStringLiteral("wallet"));
        GraphClient client;
        client.setAuth(&auth);
        client.setBaseUrl(server.baseUrl());
        Collection inbox(1);
        inbox.setRemoteId(QStringLiteral("inbox"));

        auto job = new GraphFetchItemsJob(client, inbox, QString(), this);
        QSignalSpy spy(job, &KJob::result);
        job->start();
        Run result;
        if (!spy.wait(30000)) {
            result.error = -1;
            return result;
        }
        result.changed = job->changedItems();
        result.removed = job->removedItems();
        result.batchedIds = server.batchedIds();
        result.deltaRequest = server.deltaRequest();
        result.error = job->error();
        return result;
    }

    [[nodiscard]] static std::shared_ptr<KMime::Message> message(const Item &item)
    {
        return item.payload<std::shared_ptr<KMime::Message>>();
    }

private Q_SLOTS:
    void shouldBuildStubsFromCompleteEntries()
    {
        const Run r = run({FakeDeltaServer::envelope(QStringLiteral("m1"))});
        QCOMPARE(r.error, 0);
        QCOMPARE(r.changed.size(), 1);
        // Nothing to fetch on top of the delta.
        QVERIFY(r.batchedIds.isEmpty());
        QCOMPARE(message(r.changed.first())->subject()->asUnicodeString(), QStringLiteral("Subject of m1"));
    }

    void shouldAskForPagesInsteadOfATotal()
    {
        const Run r = run({});
        QCOMPARE(r.error, 0);
        const QByteArray requestLine = r.deltaRequest.left(r.deltaRequest.indexOf("\r\n"));
        // $top would end the whole round after 200 messages.
        QVERIFY(!requestLine.contains("top="));
        QVERIFY(r.deltaRequest.contains("odata.maxpagesize=200"));
    }

    void shouldFetchTheEnvelopeOfEntriesWithoutIt()
    {
        // What Graph sent for the message that showed up blank in KMail.
        const QJsonObject bare{{QStringLiteral("id"), QStringLiteral("m2")}, {QStringLiteral("isRead"), true}};
        const Run r = run({FakeDeltaServer::envelope(QStringLiteral("m1")), bare});
        QCOMPARE(r.error, 0);
        QCOMPARE(r.batchedIds, QStringList{QStringLiteral("m2")});
        QCOMPARE(r.changed.size(), 2);
        const Item &item = r.changed.at(1);
        QCOMPARE(item.remoteId(), QStringLiteral("m2"));
        QVERIFY(item.hasFlag(MessageFlags::Seen));
        const auto msg = message(item);
        QCOMPARE(msg->subject()->asUnicodeString(), QStringLiteral("Subject of m2"));
        QCOMPARE(msg->from()->asUnicodeString(), QStringLiteral("Sender <sender@example.org>"));
        QVERIFY(msg->date()->dateTime().isValid());
    }

    void shouldDropEntriesGoneBeforeTheirEnvelopeArrives()
    {
        const QJsonObject bare{{QStringLiteral("id"), QStringLiteral("gone")}, {QStringLiteral("isRead"), true}};
        const Run r = run({bare}, {QStringLiteral("gone")});
        QCOMPARE(r.error, 0);
        // No empty stub for it; the tombstone comes with the next delta.
        QVERIFY(r.changed.isEmpty());
    }

    void shouldKeepTombstones()
    {
        const QJsonObject removed{{QStringLiteral("id"), QStringLiteral("m3")}, {QStringLiteral("@removed"), QJsonObject{{QStringLiteral("reason"), QStringLiteral("deleted")}}}};
        const Run r = run({removed});
        QCOMPARE(r.error, 0);
        QVERIFY(r.changed.isEmpty());
        QVERIFY(r.batchedIds.isEmpty());
        QCOMPARE(r.removed.size(), 1);
        QCOMPARE(r.removed.first().remoteId(), QStringLiteral("m3"));
    }
};

QTEST_GUILESS_MAIN(GraphFetchItemsJobTest)

#include "graphfetchitemsjobtest.moc"
