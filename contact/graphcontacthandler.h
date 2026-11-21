/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Maps between Graph `contact` resources and KContacts::Addressee.
*/

#pragma once

#include <KContacts/Addressee>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace GraphContactHandler
{
/// Akonadi item content mime type for contacts.
[[nodiscard]] QString mimeType();

[[nodiscard]] KContacts::Addressee toAddressee(const QJsonObject &json);
/// The phone numbers for which Graph has no place left (one mobile, two business and
/// two home numbers), which toJson() leaves out.
[[nodiscard]] QStringList phonesBeyondLimit(const KContacts::Addressee &a);

/// The e-mail addresses beyond the three Graph keeps (the preferred one first), which
/// toJson() leaves out.
[[nodiscard]] QStringList emailsBeyondLimit(const KContacts::Addressee &a);

[[nodiscard]] QJsonObject toJson(const KContacts::Addressee &addressee);
}
