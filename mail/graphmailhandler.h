/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Maps between Akonadi mail items (KMime) and Graph `message` resources.
    In Phase 1 the read path needs almost nothing here because Graph gives us MIME via
    /$value (see GraphFetchItemPayloadJob). This handler carries the *write* mappings
    (create draft, flag changes) and exists so calendar/contact handlers can follow the
    same shape later (cf. ews mail/calendar/contact/task handlers).
*/

#pragma once

#include <Akonadi/Item>
#include <QJsonObject>
#include <QStringList>

namespace KMime
{
class Message;
}

namespace GraphMailHandler
{
[[nodiscard]] QString mimeType();

/// Seen/Flagged -> PATCH body for /me/messages/{id}, with only what @p changedFlags
/// names (the flags added or removed): "flag" is a complex value that Graph replaces
/// as a whole, so sending it along with a read mark turned a completed flag into
/// notFlagged and dropped its dates. Empty when neither changed.
[[nodiscard]] QJsonObject flagPatchBody(const Akonadi::Item &item, const QSet<QByteArray> &changedFlags);

/// KMime payload -> body for creating a message from MIME:
///   POST /me/messages  with Content-Type: text/plain and base64(MIME)
/// Returns (rawBody, contentType).
[[nodiscard]] QPair<QByteArray, QByteArray> createFromMime(const Akonadi::Item &item);

/// Puts the envelope's Bcc recipients (@p bcc, from the outbox item's AddressAttribute)
/// into @p message when it carries no Bcc header of its own. Graph sends raw MIME
/// with no envelope beside it: who is not named in the MIME gets nothing. Exchange
/// drops the header from what the recipients receive.
void addMissingBcc(KMime::Message &message, const QStringList &bcc);

/// The Message-ID header of a raw MIME message, in the form KMime reports it for a
/// parsed message ("<id@host>"); empty when the message has none.
[[nodiscard]] QString messageIdOfMime(const QByteArray &rawMime);
}
