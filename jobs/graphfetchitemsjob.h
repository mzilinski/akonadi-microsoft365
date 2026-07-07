/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    GET /me/mailFolders/{id}/messages/delta  ->  Akonadi::Item stubs (id + flags only).
    The full MIME payload is fetched lazily by GraphFetchItemPayloadJob.
    Mirrors EwsFetchItemsJob.
*/

#pragma once

#include <Akonadi/Collection>
#include <Akonadi/Item>
#include <KJob>

class GraphClient;

class GraphFetchItemsJob : public KJob
{
    Q_OBJECT
public:
    GraphFetchItemsJob(GraphClient &client, const Akonadi::Collection &collection, const QString &deltaLink, QObject *parent = nullptr);

    void start() override;

    Akonadi::Collection collection() const;
    Akonadi::Item::List changedItems() const;
    Akonadi::Item::List removedItems() const;
    QString deltaLink() const;

private Q_SLOTS:
    void onRequestFinished(KJob *);

private:
    Akonadi::Item itemStubFromJson(const QJsonObject &message) const;

    GraphClient &mClient;
    Akonadi::Collection mCollection;
    QString mDeltaLink;
    Akonadi::Item::List mChanged, mRemoved;
};
