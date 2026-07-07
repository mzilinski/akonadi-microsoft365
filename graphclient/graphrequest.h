/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Base KJob for a single Graph REST call. Centralises the three things every Graph
    call must handle and that EWS did *not* need:
      1. Bearer-token injection (Authorization: Bearer ...).
      2. 429 Too Many Requests  -> honour `Retry-After` and re-issue.
      3. Paging: transparently follow `@odata.nextLink` and aggregate results.
         (`@odata.deltaLink` is exposed to callers as the persisted sync state.)
*/

#pragma once

#include <KJob>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QUrl>

class GraphClient;
class QNetworkReply;

class GraphRequest : public KJob
{
    Q_OBJECT
public:
    enum Method { Get, Post, Patch, Delete };

    GraphRequest(GraphClient &client, QObject *parent = nullptr);

    void setMethod(Method m);
    void setPath(const QString &path);          // e.g. "/me/mailFolders/delta" (relative to baseUrl)
    void setAbsoluteUrl(const QUrl &url);        // e.g. a stored nextLink/deltaLink (overrides path)
    void setBody(const QJsonObject &body);
    void setRawBody(const QByteArray &body, const QByteArray &contentType);
    void setFollowPaging(bool follow);           // aggregate @odata.value across nextLinks
    void setExpectRawPayload(bool raw);          // for /$value (returns MIME bytes, not JSON)
    void addHeader(const QByteArray &name, const QByteArray &value); // e.g. Prefer

    void start() override;

    // Results (available in result slot):
    QJsonObject responseObject() const;          // single-object responses
    QJsonArray aggregatedValue() const;          // paged list responses ("value" concatenated)
    QByteArray rawPayload() const;               // when setExpectRawPayload(true)
    QString deltaLink() const;                   // @odata.deltaLink from the final page (if any)
    int httpStatus() const;                      // HTTP status of the (last) reply

private Q_SLOTS:
    void onReplyFinished();

private:
    void issue(const QUrl &url);
    void scheduleRetry(int seconds, const QUrl &url); // 429 backoff

    GraphClient &mClient;
    Method mMethod = Get;
    QString mPath;
    QUrl mAbsoluteUrl;
    QByteArray mBody;
    QByteArray mContentType;
    QList<QPair<QByteArray, QByteArray>> mHeaders;
    bool mFollowPaging = true;
    bool mExpectRaw = false;

    QJsonObject mResponseObject;
    QJsonArray mAggregated;
    QByteArray mRawPayload;
    QString mDeltaLink;
    int mRetryCount = 0;
    int mHttpStatus = 0;
};
