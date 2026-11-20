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
#include <KCalendarCore/Event>
#include <KJob>
#include <QHash>
#include <QStringList>

class GraphClient;

class GraphFetchPimItemsJob : public KJob
{
    Q_OBJECT
public:
    enum class Type : uint8_t {
        Events,
        Contacts,
        Todos
    };
    Q_ENUM(Type)

    GraphFetchPimItemsJob(GraphClient &client, const Akonadi::Collection &collection, Type type, const QString &deltaLink, QObject *parent = nullptr);

    void start() override;

    [[nodiscard]] Akonadi::Collection collection() const;
    [[nodiscard]] Akonadi::Item::List changedItems() const;
    [[nodiscard]] Akonadi::Item::List removedItems() const;
    [[nodiscard]] QString deltaLink() const;
    /// The round listed the whole collection (no usable deltaLink): what it did not
    /// list is gone from the server, tombstones or not.
    [[nodiscard]] bool isFullListing() const;
    /// Events: for every series master of this round, the remote ids of its changed
    /// occurrences as they are now (empty list: none). Local exception items of these
    /// series that are not listed are gone; their items carry "series:" + the master's
    /// id as remote revision.
    [[nodiscard]] QHash<QString, QStringList> seriesExceptions() const;
    /// Remote revision of the item of a changed occurrence of the series @p masterId.
    [[nodiscard]] static QString exceptionRevision(const QString &masterId);

private:
    void onDeltaFinished(KJob *);
    void startDelta(const QString &absoluteUrlOrPath, bool isAbsolute);
    void resolveContactsFolderThenStart();
    void fetchPhotos();
    void fetchExceptionIds();
    void fetchExceptions(const QList<QPair<QString, QString>> &exceptions); // (master id, exception id)

    GraphClient &mClient;
    Akonadi::Collection mCollection;
    Type mType = Type::Events;
    QString mDeltaLink;
    bool mFullListing = false;
    Akonadi::Item::List mChanged;
    Akonadi::Item::List mRemoved;
    QHash<QString, KCalendarCore::Event::Ptr> mMasters; // series of this round, by id
    QHash<QString, QStringList> mSeriesExceptions;
};
