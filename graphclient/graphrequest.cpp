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
// A listing longer than this is taken for a server that keeps sending the same page
// (at 50 to 200 entries a page that is far beyond any real mailbox folder).
static constexpr int MaxPages = 5000;

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

void GraphRequest::setRepeatable(bool repeatable)
{
    mRepeatable = repeatable;
}

bool GraphRequest::repeatable() const
{
    return mRepeatable.value_or(isRepeatable(mMethod));
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
    // A stored link (a delta link kept in a collection attribute, which any local
    // Akonadi client can rewrite) gets the token only on the server it belongs to.
    if (!isOwnServer(url, mClient.baseUrl())) {
        failForeignLink();
        return;
    }
    issue(url);
}

void GraphRequest::issue(const QUrl &url)
{
    QNetworkRequest req(url);
    // Redirects are not followed: Graph does not send them for these calls, and one
    // could carry the Authorization header to another host.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
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
    // 429 is refused before any work; 503 may come after it, so only a request that
    // may run twice is sent again (see failureForStatus()).
    if ((http == 429 || (http == 503 && repeatable())) && mRetryCount < MaxRetries) {
        const int retryAfter = retryAfterSeconds(reply->rawHeader("Retry-After"), QDateTime::currentDateTimeUtc());
        scheduleRetry(retryDelaySeconds(retryAfter, mRetryCount), reply->url());
        ++mRetryCount;
        return;
    }

    if (http >= 300 && http < 400) {
        setError(KJob::UserDefinedError);
        setErrorText(i18n("Microsoft 365 redirected a request to another address (HTTP %1); it was not followed", http));
        emitResult();
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

    // An empty body is fine (204, /send, a photo upload). Anything else must be a JSON
    // object: read leniently, garbage became {}, so a broken page of a full listing
    // looked like an empty folder (and deleted its local items) and a create like an
    // item without id.
    const QByteArray data = reply->readAll();
    QJsonObject obj;
    if (!data.trimmed().isEmpty()) {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            failUnreadable(http);
            return;
        }
        obj = doc.object();
    }
    // A page that a nextLink led to is part of a list and must say so.
    if (mFollowingNextLink && !obj.value(QLatin1String("value")).isArray()) {
        failUnreadable(http);
        return;
    }

    // --- list vs single object -------------------------------------------------
    if (obj.contains(QLatin1String("value"))) {
        for (const auto &v : obj.value(QLatin1String("value")).toArray()) {
            mAggregated.append(v);
        }
        // Delta/paging links.
        if (obj.contains(QLatin1String("@odata.nextLink"))) {
            const QUrl next(obj.value(QLatin1String("@odata.nextLink")).toString());
            if (!isOwnServer(next, mClient.baseUrl())) {
                failForeignLink();
                return;
            }
            if (next == reply->url() || ++mPages >= MaxPages) {
                setError(KJob::UserDefinedError);
                setErrorText(i18n("Microsoft 365 kept sending further pages of the same list; the listing was stopped"));
                emitResult();
                return;
            }
            mFollowingNextLink = true;
            mRetryCount = 0; // the retry budget is per page, not per listing
            issue(next);
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

void GraphRequest::failForeignLink()
{
    // The link is never requested; asking again would only fail the same way.
    setError(KJob::UserDefinedError);
    setErrorText(i18n("Microsoft 365 pointed to a link outside of its own server; it was not followed"));
    QTimer::singleShot(0, this, [this] {
        emitResult();
    });
}

void GraphRequest::failUnreadable(int httpStatus)
{
    // failure() counts it as Uncertain: the success status says the server acted.
    setError(KJob::UserDefinedError);
    setErrorText(i18n("Microsoft 365 sent an answer that could not be read (HTTP %1)", httpStatus));
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

bool GraphRequest::isRepeatable(Method method)
{
    return method != Method::Post;
}

int GraphRequest::retryAfterSeconds(const QByteArray &value, const QDateTime &now)
{
    const QByteArray trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        return 0;
    }
    bool isNumber = false;
    const int seconds = trimmed.toInt(&isNumber);
    if (isNumber) {
        return std::max(seconds, 0);
    }
    // HTTP date, e.g. "Wed, 21 Oct 2026 07:28:00 GMT". HTTP always says GMT, which
    // Qt's RFC 2822 parser does not take as a zone; it means +0000.
    QString date = QString::fromLatin1(trimmed);
    if (date.endsWith(QLatin1String(" GMT"))) {
        date.chop(4);
        date += QLatin1String(" +0000");
    }
    const QDateTime when = QDateTime::fromString(date, Qt::RFC2822Date);
    if (!when.isValid()) {
        return 0;
    }
    return static_cast<int>(std::max<qint64>(now.secsTo(when), 0));
}

bool GraphRequest::isOwnServer(const QUrl &url, const QString &baseUrl)
{
    const QUrl base(baseUrl);
    return url.isValid() && url.scheme() == base.scheme() && url.host().compare(base.host(), Qt::CaseInsensitive) == 0
        && url.port(url.scheme() == QLatin1String("https") ? 443 : 80) == base.port(base.scheme() == QLatin1String("https") ? 443 : 80)
        && url.userInfo().isEmpty();
}

GraphRequest::Failure GraphRequest::failureForStatus(int httpStatus, bool repeatable)
{
    if (httpStatus >= 200 && httpStatus < 300) {
        return Failure::None;
    }
    switch (httpStatus) {
    case 401: // token missing, expired or revoked: checked before the work
    case 408: // the request did not arrive in full in time (RFC 9110)
    case 429: // throttled: "the requests fail" (Graph throttling guidance)
        return Failure::NotExecuted;
    case 503: // unavailable or overloaded; may also come after the work was done
        return repeatable ? Failure::NotExecuted : Failure::Uncertain;
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
        return failureForStatus(mHttpStatus, repeatable());
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
