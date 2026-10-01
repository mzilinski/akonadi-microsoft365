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

#include <functional>
#include <memory>

class GraphBatchJob;
class GraphSettings;
class GraphOAuth;
class KJob;
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

protected Q_SLOTS:
    // --- Synchronisation (Graph -> local) ------------------------------------
    /// Full/delta folder list: GET /me/mailFolders/delta -> collectionsRetrieved[Incremental]().
    void retrieveCollections() override;
    /// Per-folder message delta: GET /me/mailFolders/{id}/messages/delta -> itemsRetrieved[Incremental]().
    void retrieveItems(const Akonadi::Collection &collection) override;
    /// On-demand payloads: GET /me/messages/{id}/$value -> KMime, then itemsRetrieved().
    bool retrieveItems(const Akonadi::Item::List &items, const QSet<QByteArray> &parts) override;

private:
    void delayedInit();
    // Config was changed from outside (graphconfig plugin in the client process).
    void reloadConfig();
    // Keep the account root collection's name in sync with instance renames.
    void updateRootCollectionName(const QString &name);
    void onAuthReady();
    void onAuthFailed(const QString &error);
    void onAuthUnreachable();
    void holdScheduler(); // stop the scheduler; what the running task still delivers is stale
    void signInAgain(); // hold the scheduler and get a new token
    void resumeScheduler(); // once no request from before going offline is still out

    // Going offline (or signing in again) makes the scheduler give up the running task
    // and run it again later. Whatever the given-up run still delivers must not reach
    // ResourceBase: it would complete whichever task runs by then, and a late
    // itemsRetrieved() without one runs a full sync that empties the last synced
    // folder. Every task therefore carries the generation it started in.
    [[nodiscard]] bool isStale(quint64 generation) const;

    // A change replay: the generation it started in and the change it replays (the
    // Akonadi id of its first item, negated for collections).
    struct Replay {
        quint64 generation = 0;
        qint64 change = 0;
    };
    // At the start of a replay handler. Returns false when an earlier run of the same
    // change finished after it had been given up: its outcome is applied now, without
    // sending anything again, and the handler has nothing left to do.
    [[nodiscard]] bool startReplay(qint64 change, Replay &replay);
    // Completes a replay, or keeps the outcome for the next run when this one was given up.
    void finishReplay(const Replay &replay, const std::function<void()> &finish);
    void commitItem(const Replay &replay, const Akonadi::Item &item);
    void commitItems(const Replay &replay, const Akonadi::Item::List &items);
    void commitCollection(const Replay &replay, const Akonadi::Collection &collection);
    void skipChange(const Replay &replay); // done, nothing to commit
    void failChange(const Replay &replay, const QString &message);
    // Keeps the scheduler held while a local follow-up job of a replay runs.
    void trackWork(KJob *job);

    void fetchFoldersJobFinished(KJob *job, quint64 generation);
    void fetchItemsJobFinished(KJob *job, quint64 generation);
    void fetchPimItemsJobFinished(KJob *job, quint64 generation);
    void fetchPayloadJobFinished(KJob *job, quint64 generation);

    void setUpAuth();
    void reconfigureClient();
    // Entered once the one-time remoteId migration has run (see onAuthReady).
    void startSyncing();
    // Incremental delivery with tombstones resolved against the local cache and the
    // deltaLink persisted only after the ItemSync committed (both via own ItemSync —
    // itemsRetrievedIncremental() cannot sequence the save after the commit).
    void deliverItemsIncremental(const Akonadi::Collection &collection,
                                 const Akonadi::Item::List &changed,
                                 const Akonadi::Item::List &removed,
                                 const QString &deltaLink,
                                 quint64 generation);
    void syncItemsIncremental(const Akonadi::Collection &collection,
                              const Akonadi::Item::List &changed,
                              const Akonadi::Item::List &removed,
                              const QString &deltaLink,
                              quint64 generation);
    // Deliver on-demand payloads on freshly fetched items so the store cannot race
    // a concurrent delta (revision conflict) or clobber newer flags.
    void deliverFreshPayloads(const Akonadi::Item::List &filled, quint64 generation);

    void fetchFolderTree(quint64 generation);
    // Resolve removed-collection tombstones against the local cache before delivery —
    // CollectionSync drops removed entries whose parent is unknown.
    void deliverIncrementalTree(const Akonadi::Collection::List &changed, const Akonadi::Collection::List &removed, quint64 generation);
    void fetchExtraCollections(quint64 generation); // calendars + contacts + todo lists
    // Continues fetchExtraCollections(); mExtraCollections is only replaced once both
    // list requests have succeeded, so a transient failure cannot surface as deletions.
    void fetchTodoListCollections(Akonadi::Collection::List fresh, quint64 generation);
    // Tag Inbox/Sent/Drafts/Trash/Junk/Outbox as Akonadi special collections so KMail
    // and the unified-mailbox agent treat them correctly. Applied inline during sync.
    void applySpecialAttributes(Akonadi::Collection &col);
    // Adopt Graph's server-side sent copy instead of creating a duplicate.
    void reconcileSentItem(const Akonadi::Item &item, const QString &messageId, const Replay &replay);
    // Draft edits: Graph cannot replace MIME in place — recreate the draft, drop the old one.
    [[nodiscard]] bool isDraftsCollection(const Akonadi::Collection &col) const;
    void replaceDraft(const Akonadi::Item &item, const Replay &replay);
    // MIME ingestion; calls done(remoteId, errorText, failedCreate) with an empty id on
    // failure. failedCreate is the creating request when that one failed (so nothing
    // was created and the replay may be held back), otherwise null.
    void createMimeMessage(const QByteArray &rawMime,
                           const QByteArray &contentType,
                           const QString &targetFolderRid,
                           const std::function<void(const QString &, const QString &, KJob *)> &done);
    // Akonadi drops a cancelled change replay for good, so a change that failed only
    // because of the moment it was tried is held back instead: the resource goes
    // offline for a while, which returns the task to the head of its queue. Only what
    // is safe to send again qualifies — anything whose outcome is unknown only if
    // @p repeatable, and nothing once part of a non-repeatable change has @p applied.
    // Gives up after a number of attempts at the same change. Returns true when the
    // change was held back; the caller must then do nothing else.
    [[nodiscard]] bool retryLater(KJob *job, const Replay &replay, bool repeatable, bool applied);
    // Report a mass change as Running with progress while it is applied.
    void announceReplay(GraphBatchJob *job, int count);
    // Calendar/contact change replay helpers.
    // repeatable: the create may be sent again after an uncertain failure (events,
    // whose transactionId lets Graph drop the second one).
    void postPimItem(const Akonadi::Item &item, const QString &path, const QJsonObject &body, bool repeatable, const Replay &replay);
    // Contact photos live on a separate endpoint; upload after the JSON write, then commit.
    void putContactPhotoThenCommit(const Akonadi::Item &item, const Replay &replay);
    // An update. repeatable: it may be sent again after an uncertain failure (not so the
    // due date of a recurring task, see GraphTodoHandler::serverDue()). onSuccess runs
    // once the server took it, before the change is committed.
    struct Patch {
        bool repeatable = true;
        std::function<void()> onSuccess;
    };
    void patchPimItem(const Akonadi::Item &item, const QString &path, const QJsonObject &body, const Replay &replay, const Patch &patch);
    void patchPimItem(const Akonadi::Item &item, const QString &path, const QJsonObject &body, const Replay &replay);
    // Graph has no move API for events/tasks: recreate in the destination, delete the
    // original, then continue with the next item (sequential; commits when done).
    void movePimItem(const Akonadi::Item::List &items,
                     int index,
                     const std::shared_ptr<Akonadi::Item::List> &moved,
                     const Akonadi::Collection &source,
                     const Akonadi::Collection &destination,
                     const Replay &replay);

    // Per-collection delta state (the @odata.deltaLink) is stored as a collection
    // attribute, exactly like EwsSyncStateAttribute, tagged with the mapping version
    // its items were built with.
    [[nodiscard]] static int mappingVersion(const Akonadi::Collection &col);
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
    QSet<QString> mKnownExtraIds; // to report server-side deletions (no delta for these)
    QTimer *mPollTimer = nullptr;
    // The retry budget lives in memory only: an agent restart gives a held-back change
    // a fresh set of attempts, which is what a server that has recovered meanwhile
    // needs; within one run the attempts stay bounded.
    qint64 mRetriedChange = 0; // Akonadi id of the change retryLater() last held back
    int mReplayRetries = 0;
    bool mAuthPending = false; // an OAuth flow is running
    quint64 mTaskGeneration = 0; // bumped whenever the scheduler gives up the running task
    QHash<qint64, std::function<void()>> mLateOutcomes; // change -> outcome of a given-up run
    // Due dates this resource wrote, per task: the server's modification time as read,
    // and the due date written since. Until a sync delivers the task again (with a new
    // modification time), the written one is what the server has
    // (see GraphTodoHandler::serverDue()).
    QHash<Akonadi::Item::Id, std::pair<QString, QString>> mWrittenDue;
    bool mResumeWhenIdle = false; // resume the scheduler once the client is idle
    bool mReauthenticate = false; // the server rejected the token; get a new one when back online
};
