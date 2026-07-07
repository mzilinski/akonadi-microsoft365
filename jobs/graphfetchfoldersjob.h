/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    GET /me/mailFolders/delta  ->  Akonadi::Collection list.
    Mirrors EwsFetchFoldersJob / EwsFetchFoldersIncrJob (full + incremental in one).
*/

#pragma once

#include <Akonadi/Collection>
#include <KJob>

class GraphClient;

class GraphFetchFoldersJob : public KJob
{
    Q_OBJECT
public:
    GraphFetchFoldersJob(GraphClient &client, const Akonadi::Collection &root, const QString &deltaLink, QObject *parent = nullptr);

    void start() override;

    bool isIncremental() const;                          // true if a deltaLink was supplied
    Akonadi::Collection::List allCollections() const;    // full sync
    Akonadi::Collection::List changedCollections() const;// incremental
    Akonadi::Collection::List removedCollections() const;// incremental (@removed)
    QString deltaLink() const;                           // persist for next sync

    /// Root collection with its remoteId resolved to the real msgfolderroot id.
    Akonadi::Collection rootCollection() const;

private Q_SLOTS:
    void onRootResolved(KJob *);
    void onRequestFinished(KJob *);

private:
    void startDelta();
    Akonadi::Collection collectionFromJson(const QJsonObject &folder) const;

    GraphClient &mClient;
    Akonadi::Collection mRoot;
    QString mDeltaLink;
    bool mIncremental;
    Akonadi::Collection::List mAll, mChanged, mRemoved;
};
