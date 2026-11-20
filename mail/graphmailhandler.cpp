/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "graphmailhandler.h"

#include <Akonadi/MessageFlags>
#include <KMime/Message>

namespace GraphMailHandler
{
QString mimeType()
{
    return KMime::Message::mimeType();
}

QJsonObject flagPatchBody(const Akonadi::Item &item, const QSet<QByteArray> &changedFlags)
{
    QJsonObject body;
    if (changedFlags.contains(Akonadi::MessageFlags::Seen)) {
        body.insert(QStringLiteral("isRead"), item.hasFlag(Akonadi::MessageFlags::Seen));
    }
    if (changedFlags.contains(Akonadi::MessageFlags::Flagged)) {
        QJsonObject flag;
        flag.insert(QStringLiteral("flagStatus"), item.hasFlag(Akonadi::MessageFlags::Flagged) ? QStringLiteral("flagged") : QStringLiteral("notFlagged"));
        body.insert(QStringLiteral("flag"), flag);
    }
    return body;
}

void addMissingBcc(KMime::Message &message, const QStringList &bcc)
{
    if (bcc.isEmpty()) {
        return;
    }
    const auto *existing = message.bcc(KMime::CreatePolicy::DontCreate);
    if (existing && !existing->isEmpty()) {
        return;
    }
    message.bcc()->fromUnicodeString(bcc.join(QStringLiteral(", ")));
    message.assemble();
}

QPair<QByteArray, QByteArray> createFromMime(const Akonadi::Item &item)
{
    if (!item.hasPayload<std::shared_ptr<KMime::Message>>()) {
        return {};
    }
    const auto msg = item.payload<std::shared_ptr<KMime::Message>>();
    // Graph accepts a full MIME message on POST /me/messages when the body is the
    // *base64-encoded* MIME with Content-Type: text/plain. It creates a draft;
    // caller then POSTs .../send.
    return {msg->encodedContent(KMime::NewlineType::CRLF).toBase64(), QByteArray("text/plain")};
}

QString messageIdOfMime(const QByteArray &rawMime)
{
    // Only the header block is needed; leave the (possibly large) body unparsed.
    const QByteArray lf = KMime::CRLFtoLF(rawMime);
    const qsizetype headEnd = lf.indexOf("\n\n");
    KMime::Message head;
    head.setContent(headEnd < 0 ? lf : lf.left(headEnd + 1));
    head.parse();
    const auto *messageId = head.messageID(KMime::DontCreate);
    return messageId ? messageId->asUnicodeString() : QString();
}
}
