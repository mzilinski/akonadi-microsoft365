/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Runs a list of Graph calls through /$batch (20 sub-requests per round trip, the
    batches themselves one after the other — Graph throttles aggressively) and
    collects the per-call JSON responses. Used by the change-replay handlers (flags,
    move, delete, create) where one Akonadi change notification fans out into one
    call per item.

    Every call is issued even when others failed, as long as the server itself
    answers; the job reports the first error once all of them are done. Sub-requests
    that Graph throttles are re-issued after the announced delay.
*/

#pragma once

#include "graphclient/graphrequest.h"

#include <KJob>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QSet>

class GraphClient;

class GraphBatchJob : public KJob
{
    Q_OBJECT
public:
    struct Call {
        GraphRequest::Method method;
        QString path;
        QJsonObject body; // ignored for Get/Delete
    };

    GraphBatchJob(GraphClient &client, const QList<Call> &calls, QObject *parent = nullptr);

    /// Treat HTTP 404 as success (delete/move of an item already gone on the server).
    void setIgnoreNotFound(bool ignore);

    void start() override;

    /// One entry per call, in order; empty object for responses without a body (204)
    /// and for calls that failed.
    [[nodiscard]] QList<QJsonObject> responses() const;
    /// The body of each successful call as it came, in order: a JSON object, or the
    /// base64 string Graph puts in a batch for a raw answer (/$value). Undefined for
    /// calls that failed or answered without a body.
    [[nodiscard]] QList<QJsonValue> bodies() const;
    /// Whether call @p index was answered 404 and taken as done (setIgnoreNotFound).
    [[nodiscard]] bool wasNotFound(int index) const;
    /// The response headers of each successful call (e.g. Content-Type of a /$value).
    [[nodiscard]] QList<QJsonObject> headers() const;
    /// Calls the server carried out (including tolerated 404s).
    [[nodiscard]] int succeeded() const;
    /// Failed calls of the given class (see GraphRequest::Failure).
    [[nodiscard]] int failed(GraphRequest::Failure failure) const;
    /// Some call failed for lack of a valid token.
    [[nodiscard]] bool authenticationRejected() const;

private:
    void issueNextBatch();
    void finishCall(int index, int status, const QJsonValue &body, const QJsonObject &headers);
    void failCall(int error, const QString &errorText, GraphRequest::Failure failure, bool authRejected);
    void finish();

    GraphClient &mClient;
    const QList<Call> mCalls;
    QList<QJsonObject> mResponses;
    QList<QJsonValue> mBodies;
    QList<QJsonObject> mHeaders;
    QList<int> mPending; // indexes not yet answered, in issue order
    QString mFirstErrorText;
    int mFailed = 0;
    int mSucceeded = 0;
    QHash<GraphRequest::Failure, int> mFailedBy;
    bool mAuthRejected = false;
    int mFirstError = 0;
    QHash<int, int> mThrottleRetries; // per call index
    bool mIgnoreNotFound = false;
    QSet<int> mNotFound;
};
