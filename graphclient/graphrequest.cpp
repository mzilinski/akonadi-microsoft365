/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "graphrequest.h"

#include "auth/graphoauth.h"
#include "graphclient.h"

#include <KLocalizedString>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <algorithm>
#include <chrono>

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

void GraphRequest::setUseImmutableIds(bool use)
{
    mUseImmutableIds = use;
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
    // Counted until the result is out (KJob also emits finished() when killed or
    // deleted early), including throttling waits between attempts.
    connect(this, &KJob::finished, this, [done = mClient.beginWork()] {
        done();
    });
    // Requests can be scheduled before the OAuth handshake has produced a token —
    // e.g. a sync or send triggered while the interactive login is still open.
    if (!mClient.auth()) {
        mNotAuthenticated = true;
        setError(KJob::UserDefinedError);
        setErrorText(i18n("Not authenticated with Microsoft 365 yet"));
        QTimer::singleShot(0, this, [this] {
            emitResult();
        });
        return;
    }
    const QUrl url = mAbsoluteUrl.isValid() ? mAbsoluteUrl : QUrl(mClient.baseUrl() + mPath);
    issue(url);
}

void GraphRequest::issue(const QUrl &url)
{
    QNetworkRequest req(url);
    // Without it a request on a connection that silently died (NAT timeout, roaming)
    // never finishes, and the change replay waits behind it. The timeout counts idle
    // time, so a large but progressing transfer is not cut off.
    req.setTransferTimeout(mClient.transferTimeout());
    req.setRawHeader("Authorization", "Bearer " + mClient.auth()->accessToken().toUtf8());
    if (!mContentType.isEmpty()) {
        req.setHeader(QNetworkRequest::ContentTypeHeader, mContentType);
    }
    // Ask for immutable ids so a message is never reported under both of Graph's
    // mutable encodings ("AAMk…" vs "AQMk…", which broke delta tombstone matching).
    // A move still hands out a new id, which the move handlers commit. Preference
    // values must share one header line, so fold any caller-supplied Prefer entries
    // (timezone, maxpagesize) into it.
    QByteArray prefer;
    if (mUseImmutableIds && usesImmutableIds(url.path())) {
        prefer = "IdType=\"ImmutableId\"";
    }
    for (const auto &[name, value] : std::as_const(mHeaders)) {
        if (name == "Prefer") {
            prefer += (prefer.isEmpty() ? "" : ", ") + value;
        } else {
            req.setRawHeader(name, value);
        }
    }
    if (!prefer.isEmpty()) {
        req.setRawHeader("Prefer", prefer);
    }

    QNetworkAccessManager *nam = mClient.networkAccessManager();
    QNetworkReply *reply = nullptr;
    switch (mMethod) {
    case Method::Get:
        reply = nam->get(req);
        break;
    case Method::Delete:
        reply = nam->deleteResource(req);
        break;
    case Method::Post:
        reply = nam->post(req, mBody);
        break;
    case Method::Put:
        reply = nam->put(req, mBody);
        break;
    case Method::Patch:
        reply = nam->sendCustomRequest(req, "PATCH", mBody);
        break;
    }
    // Whether the request got out decides what a failure means (see failure()).
    // HTTP/1 reports it once headers and body are on the socket, HTTP/2 once the stream
    // is opened, which errs on the safe side.
    mRequestSent = false;
    connect(reply, &QNetworkReply::requestSent, this, [this] {
        mRequestSent = true;
    });
    connect(reply, &QNetworkReply::finished, this, &GraphRequest::onReplyFinished);
}

void GraphRequest::onReplyFinished()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();

    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    mHttpStatus = http;
    mNetworkError = reply->error();

    // --- 429 / 503 throttling: honour Retry-After and re-issue -----------------
    if ((http == 429 || http == 503) && mRetryCount < MaxRetries) {
        scheduleRetry(retryDelaySeconds(reply->rawHeader("Retry-After").toInt(), mRetryCount), reply->url());
        ++mRetryCount;
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        // Graph error bodies carry the useful message: { "error": { "code", "message" } }.
        const QJsonObject err = QJsonDocument::fromJson(reply->readAll()).object().value(QLatin1String("error")).toObject();
        mGraphErrorCode = err.value(QLatin1String("code")).toString();
        setError(KJob::UserDefinedError);
        if (!err.isEmpty()) {
            setErrorText(formatError(err, http));
        } else {
            setErrorText(reply->errorString());
        }
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
        if (obj.contains(QLatin1String("@odata.nextLink"))) {
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

bool GraphRequest::usesImmutableIds(const QString &path)
{
    // Microsoft To Do has a separate id space that IdType must not switch.
    return !path.contains(QLatin1String("/me/todo"));
}

QString GraphRequest::formatError(const QJsonObject &graphError, int httpStatus)
{
    return i18nc("%1 is the server error message, %2 the HTTP status code, %3 the server error code",
                 "%1 (HTTP %2, %3)",
                 graphError.value(QLatin1String("message")).toString(),
                 httpStatus,
                 graphError.value(QLatin1String("code")).toString());
}

int GraphRequest::retryDelaySeconds(int retryAfter, int attempt)
{
    constexpr int maxSeconds = 300;
    return retryAfter > 0 ? std::min(retryAfter, maxSeconds) : std::min(1 << std::min(attempt, 8), maxSeconds);
}

GraphRequest::Failure GraphRequest::failureForStatus(int httpStatus)
{
    if (httpStatus >= 200 && httpStatus < 300) {
        return Failure::None;
    }
    switch (httpStatus) {
    case 401: // token missing, expired or revoked: checked before the work
    case 408: // the request did not arrive in full in time (RFC 9110)
    case 429: // throttled: "the requests fail" (Graph throttling guidance)
    case 503: // unavailable or overloaded; Microsoft's own SDKs repeat even POSTs on it
        return Failure::NotExecuted;
    case 500:
    case 502:
    case 504: // may have failed after the work was done, or the answer got lost
        return Failure::Uncertain;
    default:
        return Failure::Permanent;
    }
}

GraphRequest::Failure GraphRequest::failure() const
{
    if (!error()) {
        return Failure::None;
    }
    if (mNotAuthenticated) {
        return Failure::NotExecuted;
    }
    if (mHttpStatus >= 300) {
        return failureForStatus(mHttpStatus);
    }
    if (mHttpStatus != 0) {
        // A success status whose body did not arrive: the server has acted.
        return Failure::Uncertain;
    }
    // No HTTP answer at all. A request that never left in full (the connection was not
    // even up, say, when the transfer timed out) cannot have been acted on.
    if (!mRequestSent) {
        return Failure::NotExecuted;
    }
    // Otherwise only errors that occur before the request can have reached the server
    // are certain; a timeout or a dropped connection may hit after Graph already did
    // the work.
    switch (mNetworkError) {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::SslHandshakeFailedError:
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyNotFoundError:
        return Failure::NotExecuted;
    default:
        return Failure::Uncertain;
    }
}

bool GraphRequest::authenticationRejected() const
{
    return error() && (mNotAuthenticated || mHttpStatus == 401);
}

QJsonObject GraphRequest::responseObject() const
{
    return mResponseObject;
}

QJsonArray GraphRequest::aggregatedValue() const
{
    return mAggregated;
}

QString GraphRequest::deltaLink() const
{
    return mDeltaLink;
}

int GraphRequest::httpStatus() const
{
    return mHttpStatus;
}

QString GraphRequest::graphErrorCode() const
{
    return mGraphErrorCode;
}

#include "moc_graphrequest.cpp"
