/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Unit tests for the Akonadi mail item -> Graph message mapping.
*/

#include "mail/graphmailhandler.h"

#include <Akonadi/Item>
#include <Akonadi/MessageFlags>
#include <KMime/Message>

#include <QJsonObject>
#include <QTest>

using namespace Akonadi;

class GraphMailHandlerTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void shouldUseRfc822MimeType()
    {
        QCOMPARE(GraphMailHandler::mimeType(), KMime::Message::mimeType());
    }

    void shouldMapFlagsToPatchBody()
    {
        Item item;
        item.setFlag(MessageFlags::Seen);
        item.setFlag(MessageFlags::Flagged);

        const QJsonObject body = GraphMailHandler::flagPatchBody(item);
        QCOMPARE(body.value(QLatin1String("isRead")).toBool(), true);
        QCOMPARE(body.value(QLatin1String("flag")).toObject().value(QLatin1String("flagStatus")).toString(), QStringLiteral("flagged"));
    }

    void shouldMapClearedFlagsToPatchBody()
    {
        const Item item;
        const QJsonObject body = GraphMailHandler::flagPatchBody(item);
        QCOMPARE(body.value(QLatin1String("isRead")).toBool(), false);
        QCOMPARE(body.value(QLatin1String("flag")).toObject().value(QLatin1String("flagStatus")).toString(), QStringLiteral("notFlagged"));
    }

    void shouldEncodeMimeAsBase64()
    {
        auto msg = std::make_shared<KMime::Message>();
        msg->subject()->fromUnicodeString(QStringLiteral("Hello"));
        msg->from()->fromUnicodeString(QStringLiteral("Alice <alice@example.com>"));
        msg->to()->fromUnicodeString(QStringLiteral("Bob <bob@example.com>"));
        msg->contentType()->setMimeType("text/plain");
        msg->setBody("Test body\n");
        msg->assemble();

        Item item;
        item.setMimeType(KMime::Message::mimeType());
        item.setPayload(msg);

        const auto [rawBody, contentType] = GraphMailHandler::createFromMime(item);
        QCOMPARE(contentType, QByteArray("text/plain"));

        const QByteArray mime = QByteArray::fromBase64(rawBody);
        QVERIFY(mime.contains("Subject: Hello"));
        QVERIFY(mime.contains("From: Alice <alice@example.com>"));
        // Graph expects CRLF line endings in the encoded MIME.
        QVERIFY(mime.contains("\r\n"));
    }

    void shouldReadMessageIdOfRawMime()
    {
        const QByteArray mime = "From: Alice <alice@example.com>\r\nMessage-ID: <abc.123@example.com>\r\nSubject: Hello\r\n\r\n"
                                "Message-ID: <in.the.body@example.com>\r\n";
        QCOMPARE(GraphMailHandler::messageIdOfMime(mime), QStringLiteral("<abc.123@example.com>"));
    }

    void shouldReadMessageIdLikeAParsedMessage()
    {
        // The sent-copy reconciliation compares this with the id of the Akonadi payload.
        auto msg = std::make_shared<KMime::Message>();
        msg->messageID()->from7BitString("<same.form@example.com>");
        msg->subject()->fromUnicodeString(QStringLiteral("Hello"));
        msg->setBody("Test body\n");
        msg->assemble();

        QCOMPARE(GraphMailHandler::messageIdOfMime(msg->encodedContent(KMime::NewlineType::CRLF)), msg->messageID()->asUnicodeString());
    }

    void shouldReturnNoMessageIdWithoutHeader()
    {
        QVERIFY(GraphMailHandler::messageIdOfMime("Subject: Hello\r\n\r\nBody\r\n").isEmpty());
        QVERIFY(GraphMailHandler::messageIdOfMime(QByteArray()).isEmpty());
    }

    void shouldReturnEmptyWithoutPayload()
    {
        const Item item;
        const auto [rawBody, contentType] = GraphMailHandler::createFromMime(item);
        QVERIFY(rawBody.isEmpty());
        QVERIFY(contentType.isEmpty());
    }
};

QTEST_GUILESS_MAIN(GraphMailHandlerTest)

#include "graphmailhandlertest.moc"
