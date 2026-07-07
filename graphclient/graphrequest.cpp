/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "graphrequest.h"

#include "auth/graphoauth.h"
#include "graphclient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>

static constexpr int MaxRetries = 5;

GraphRequest::GraphRequest(GraphClient &client, QObject *parent)
    : KJob(parent)
    , mClient(client)
{
}

void GraphRequest::setMethod(Method m)
{
    mMethod = m;
}
void GraphRequest::setPath(const QString &path)
{
    mPath = path;
}
void GraphRequest::setAbsoluteUrl(const QUrl &url)
{
    mAbsoluteUrl = url;
}
void GraphRequest::setFollowPaging(bool follow)
{
    mFollowPaging = follow;
}
void GraphRequest::setExpectRawPayload(bool raw)
{
    mExpectRaw = raw;
}
void GraphRequest::addHeader(const QByteArray &name, const QByteArray &value)
{
    mHeaders.append({name, value});
}

void GraphRequest::setBody(const QJsonObject &body)
{
    mBody = QJsonDocument(body).toJson(QJsonDocument::Compact);
    mContentType = "application/json";
}

void GraphRequest::setRawBody(const QByteArray &body, const QByteArray &contentType)
{
    mBody = body;
    mContentType = contentType;
}

void GraphRequest::start()
{
    const QUrl url = mAbsoluteUrl.isValid() ? mAbsoluteUrl : QUrl(mClient.baseUrl() + mPath);
    issue(url);
}

void GraphRequest::issue(const QUrl &url)
{
    QNetworkRequest req(url);
    req.setRawHeader("Authorization", "Bearer " + mClient.auth()->accessToken().toUtf8());
    if (!mContentType.isEmpty()) {
        req.setHeader(QNetworkRequest::ContentTypeHeader, mContentType);
    }
    for (const auto &[name, value] : std::as_const(mHeaders)) {
        req.setRawHeader(name, value);
    }

    QNetworkAccessManager *nam = mClient.networkAccessManager();
    QNetworkReply *reply = nullptr;
    switch (mMethod) {
    case Get:
        reply = nam->get(req);
        break;
    case Delete:
        reply = nam->deleteResource(req);
        break;
    case Post:
        reply = nam->post(req, mBody);
        break;
    case Patch:
        reply = nam->sendCustomRequest(req, "PATCH", mBody);
        break;
    }
    connect(reply, &QNetworkReply::finished, this, &GraphRequest::onReplyFinished);
}

void GraphRequest::onReplyFinished()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();

    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    mHttpStatus = http;

    // --- 429 / 503 throttling: honour Retry-After and re-issue -----------------
    if ((http == 429 || http == 503) && mRetryCount < MaxRetries) {
        const int retryAfter = reply->rawHeader("Retry-After").toInt();
        scheduleRetry(retryAfter > 0 ? retryAfter : (1 << mRetryCount), reply->url());
        ++mRetryCount;
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        // Graph error bodies carry the useful message: { "error": { "code", "message" } }.
        const QJsonObject err = QJsonDocument::fromJson(reply->readAll()).object().value(QLatin1String("error")).toObject();
        setError(KJob::UserDefinedError);
        if (!err.isEmpty()) {
            setErrorText(QStringLiteral("%1 (HTTP %2, %3)")
                             .arg(err.value(QLatin1String("message")).toString())
                             .arg(http)
                             .arg(err.value(QLatin1String("code")).toString()));
        } else {
            setErrorText(reply->errorString());
        }
        emitResult();
        return;
    }

    // --- raw payload (/$value) -------------------------------------------------
    if (mExpectRaw) {
        mRawPayload = reply->readAll();
        emitResult();
        return;
    }

    const QByteArray data = reply->readAll();
    const QJsonObject obj = QJsonDocument::fromJson(data).object();

    // --- list vs single object -------------------------------------------------
    if (obj.contains(QLatin1String("value"))) {
        for (const auto &v : obj.value(QLatin1String("value")).toArray()) {
            mAggregated.append(v);
        }
        // Delta/paging links.
        if (obj.contains(QLatin1String("@odata.nextLink")) && mFollowPaging) {
            issue(QUrl(obj.value(QLatin1String("@odata.nextLink")).toString()));
            return; // keep aggregating
        }
        if (obj.contains(QLatin1String("@odata.deltaLink"))) {
            mDeltaLink = obj.value(QLatin1String("@odata.deltaLink")).toString();
        }
    } else {
        mResponseObject = obj;
    }

    emitResult();
}

void GraphRequest::scheduleRetry(int seconds, const QUrl &url)
{
    QTimer::singleShot(seconds * 1000, this, [this, url] {
        issue(url);
    });
}

QJsonObject GraphRequest::responseObject() const
{
    return mResponseObject;
}
QJsonArray GraphRequest::aggregatedValue() const
{
    return mAggregated;
}
QByteArray GraphRequest::rawPayload() const
{
    return mRawPayload;
}
QString GraphRequest::deltaLink() const
{
    return mDeltaLink;
}
int GraphRequest::httpStatus() const
{
    return mHttpStatus;
}

#include "moc_graphrequest.cpp"
