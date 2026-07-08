/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>

    SPDX-License-Identifier: LGPL-2.0-or-later

    Akonadi resource for Microsoft 365 / Exchange Online using the Microsoft Graph API.
    Structure mirrors resources/ews (EWS resource) so it can be upstreamed easily.
*/

#pragma once

#include <QScopedPointer>

#include <Akonadi/ResourceWidgetBase>
#include <Akonadi/TransportResourceBase>

#include "graphclient/graphclient.h"

#include <QJsonObject>

class GraphSettings;
class GraphOAuth;
class QTimer;

/**
 * Receiving resource: folders + messages (+ later calendar/contacts).
 *
 * ResourceBase drives synchronisation by calling the retrieve*() slots below.
 * Each retrieve*() kicks off an async KJob (see jobs/), and on completion we hand the
 * result back to Akonadi via collectionsRetrieved()/itemsRetrieved()/changeCommitted().
 *
 * The AgentBase::ObserverV3 overrides implement *change replay*: local edits in KMail
 * are pushed back to Graph (PATCH/POST/DELETE ...).
 *
 * Sending: the separate MTA agent (graphmtaresource) forwards the MIME content here
 * over D-Bus (sendMessage/messageSent), so only one process holds the OAuth tokens.
 */
class GraphResource : public Akonadi::ResourceWidgetBase, public Akonadi::AgentBase::ObserverV3, public Akonadi::TransportResourceBase
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.Akonadi.Graph.Resource")
public:
    explicit GraphResource(const QString &id);
    ~GraphResource() override;

    [[nodiscard]] GraphSettings *settings()
    {
        return mSettings.data();
    }

    [[nodiscard]] const Akonadi::Collection &rootCollection() const
    {
        return mRootCollection;
    }

    // --- Change replay (local -> Graph) --------------------------------------
    // Collections == Graph mailFolders
    void collectionAdded(const Akonadi::Collection &collection, const Akonadi::Collection &parent) override; // POST /me/mailFolders
    void collectionChanged(const Akonadi::Collection &collection, const QSet<QByteArray> &changedAttributes) override; // PATCH /me/mailFolders/{id}
    void collectionRemoved(const Akonadi::Collection &collection) override; // DELETE /me/mailFolders/{id}
    void collectionMoved(const Akonadi::Collection &collection,
                         const Akonadi::Collection &source,
                         const Akonadi::Collection &destination) override; // POST /me/mailFolders/{id}/move

    // Items == Graph messages
    void itemAdded(const Akonadi::Item &item, const Akonadi::Collection &collection) override; // POST /me/mailFolders/{id}/messages
    void itemChanged(const Akonadi::Item &item, const QSet<QByteArray> &partIdentifiers) override; // PATCH /me/messages/{id}
    void itemsFlagsChanged(const Akonadi::Item::List &items,
                           const QSet<QByteArray> &addedFlags,
                           const QSet<QByteArray> &removedFlags) override; // PATCH isRead / flag
    void itemsMoved(const Akonadi::Item::List &items,
                    const Akonadi::Collection &sourceCollection,
                    const Akonadi::Collection &destinationCollection) override; // POST /me/messages/{id}/move
    void itemsRemoved(const Akonadi::Item::List &items) override; // DELETE /me/messages/{id}

    // Sending is delegated to the separate MTA resource; kept here for on-prem/testing.
    void sendItem(const Akonadi::Item &item) override;

    /// D-Bus entry point for the MTA agent: create a draft from MIME and send it.
    Q_SCRIPTABLE void sendMessage(const QString &id, const QByteArray &content);

    /// Drop the persisted folder deltaLink so the next sync is a full one (recovery).
    Q_SCRIPTABLE void clearFolderSyncState();

Q_SIGNALS:
    /// D-Bus reply to sendMessage: error is empty on success.
    Q_SCRIPTABLE void messageSent(const QString &id, const QString &error);

protected:
    void doSetOnline(bool online) override;

public Q_SLOTS:
    void configure(WId windowId) override;

protected Q_SLOTS:
    // --- Synchronisation (Graph -> local) ------------------------------------
    /// Full/delta folder list: GET /me/mailFolders/delta -> collectionsRetrieved[Incremental]().
    void retrieveCollections() override;
    /// Per-folder message delta: GET /me/mailFolders/{id}/messages/delta -> itemsRetrieved[Incremental]().
    void retrieveItems(const Akonadi::Collection &collection) override;
    /// On-demand payloads: GET /me/messages/{id}/$value -> KMime, then itemsRetrieved().
    bool retrieveItems(const Akonadi::Item::List &items, const QSet<QByteArray> &parts) override;

private Q_SLOTS:
    void delayedInit();
    void onAuthReady();
    void onAuthFailed(const QString &error);

    void fetchFoldersJobFinished(KJob *job);
    void fetchItemsJobFinished(KJob *job);
    void fetchPimItemsJobFinished(KJob *job);
    void fetchPayloadJobFinished(KJob *job);

private:
    void setUpAuth();
    void reconfigureClient();

    void fetchFolderTree();
    void fetchExtraCollections(); // calendars + contacts
    // Tag Inbox/Sent/Drafts/Trash/Junk/Outbox as Akonadi special collections so KMail
    // and the unified-mailbox agent treat them correctly. Applied inline during sync.
    void applySpecialAttributes(Akonadi::Collection &col);
    // Adopt Graph's server-side sent copy instead of creating a duplicate.
    void reconcileSentItem(const Akonadi::Item &item, const QString &messageId);
    // Calendar/contact change replay helpers.
    void postPimItem(const Akonadi::Item &item, const QString &path, const QJsonObject &body);
    void patchPimItem(const Akonadi::Item &item, const QString &path, const QJsonObject &body);

    // Per-collection delta state (the @odata.deltaLink) is stored as a collection
    // attribute, exactly like EwsSyncStateAttribute.
    [[nodiscard]] static QString collectionDeltaLink(const Akonadi::Collection &col);
    void saveCollectionDeltaLink(Akonadi::Collection col, const QString &deltaLink);

    GraphClient mClient;
    QScopedPointer<GraphOAuth> mAuth;
    QScopedPointer<GraphSettings> mSettings;
    Akonadi::Collection mRootCollection;
    QString mFolderDeltaLink; // top-level mailFolders delta
    QString mSentFolderRemoteId; // resolved "Sent Items" folder id (sent reconciliation)
    QHash<QString, int> mSpecialFolderIndex; // remoteId -> kSpecialFolders index
    Akonadi::Collection::List mExtraCollections; // calendars + contacts
    QTimer *mPollTimer = nullptr;
};
