/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Custom attribute to keep the Graph delta sync state with the collection.
    Stores the @odata.deltaLink returned by the last messages/delta round, so the
    next sync only retrieves changes. Mirrors EwsSyncStateAttribute. The version tells
    which item mapping the cached items were built with; a resource drops links of an
    older version to re-list the collection once. It is stored as a URL fragment, so
    that builds without versions can still use the link.
*/

#pragma once

#include <Akonadi/Attribute>

class GraphSyncStateAttribute : public Akonadi::Attribute
{
public:
    GraphSyncStateAttribute() = default;
    explicit GraphSyncStateAttribute(const QString &deltaLink, int version = 0);

    void setDeltaLink(const QString &deltaLink);
    [[nodiscard]] const QString &deltaLink() const;
    [[nodiscard]] int version() const;

    QByteArray type() const override;
    Attribute *clone() const override;
    QByteArray serialized() const override;
    void deserialize(const QByteArray &data) override;

private:
    QString mDeltaLink;
    int mVersion = 0;
};
