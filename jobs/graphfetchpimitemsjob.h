/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Delta fetch of calendar events or contacts for one collection.
      Events:   GET /me/calendars/{id}/events/delta
      Contacts: GET /me/contactFolders/{defaultFolderId}/contacts/delta
    Both support change tracking: the first call returns everything plus an
    @odata.deltaLink; later calls follow that link and return only added/updated
    items plus @removed tombstones — exactly like the mail messages/delta path.
    Payloads are delivered whole (these datasets are small), so no separate $value step.
*/

#pragma once

#include <Akonadi/Collection>
#include <Akonadi/Item>
#include <KJob>

class GraphClient;

class GraphFetchPimItemsJob : public KJob
{
    Q_OBJECT
public:
    enum Type {
        Events,
        Contacts
    };

    GraphFetchPimItemsJob(GraphClient &client, const Akonadi::Collection &collection, Type type, const QString &deltaLink, QObject *parent = nullptr);

    void start() override;

    [[nodiscard]] Akonadi::Collection collection() const;
    [[nodiscard]] Akonadi::Item::List changedItems() const;
    [[nodiscard]] Akonadi::Item::List removedItems() const;
    [[nodiscard]] QString deltaLink() const;

private Q_SLOTS:
    void onDeltaFinished(KJob *);

private:
    void startDelta(const QString &absoluteUrlOrPath, bool isAbsolute);
    void resolveContactsFolderThenStart();
    void fetchPhotos();

    GraphClient &mClient;
    Akonadi::Collection mCollection;
    Type mType;
    QString mDeltaLink;
    Akonadi::Item::List mChanged;
    Akonadi::Item::List mRemoved;
    int mPhotoIndex = 0; // next contact whose photo has not been requested yet
};
