/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    OAuth2 (Authorization Code + PKCE) against Azure AD v2.0 endpoint, Graph scopes.
    Adapted from resources/ews/ewsclient/auth/ewsoauth.{h,cpp} — the flow is identical,
    only the authority endpoint (v2.0) and the requested scopes change.

    Token lifecycle:
      authenticate() -> read refresh token from the keychain
        -> found:   silent refresh (no UI); on failure fall back to interactive
        -> missing: interactive browser login (loopback redirect)
      granted -> persist new refresh token, schedule a proactive renewal shortly
                 before the access token expires, emit ready() (first time only).
      no network -> unreachable(); the refresh token is kept, and no browser opens for
                 what is only a missing connection.
*/

#pragma once

#include <QAbstractOAuth>
#include <QObject>
#include <QSet>
#include <QString>
#include <memory>

class QOAuth2AuthorizationCodeFlow;
class QOAuthHttpServerReplyHandler;
class QTimer;

class GraphOAuth : public QObject
{
    Q_OBJECT
public:
    // tenantId: "common" (multi-tenant) or a concrete tenant GUID.
    // clientId: your Azure app registration's Application (client) ID.
    // walletKey: keychain entry name for the refresh token (use the resource instance id).
    GraphOAuth(const QString &tenantId, const QString &clientId, const QString &walletKey, QObject *parent = nullptr);
    ~GraphOAuth() override;

    /// Try a silent refresh (stored refresh token via QtKeychain); fall back to interactive.
    void authenticate();

    /// Drop persisted tokens (reconfigure/logout).
    void forgetTokens();

    /// Current bearer token, or empty if not yet authenticated.
    [[nodiscard]] QString accessToken() const;

    /// Whether accessToken() can still be used for a while. The proactive renewal does
    /// not run during suspend, so after a resume the token may have expired.
    [[nodiscard]] bool hasValidToken() const;

Q_SIGNALS:
    /// First successful token acquisition. Later silent renewals do not re-emit.
    void ready();
    void failed(const QString &error);
    /// The token endpoint could not be reached. Nothing is wrong with the account:
    /// authenticate() again once the network is back.
    void unreachable();

private:
    [[nodiscard]] static QSet<QByteArray> graphScopes();

    void setUpFlow();
    void startSilentRefresh(const QString &refreshToken);
    void startInteractive();
    void onGranted();
    void onRequestFailed(QAbstractOAuth::Error error);
    void persistRefreshToken();
    void scheduleProactiveRefresh();

    QString mTenantId;
    QString mClientId;
    QString mWalletKey;
    QString mEnvToken; // GRAPH_ACCESS_TOKEN test hook
    bool mWasReady = false; // ready() already emitted
    bool mInteractive = false; // current attempt is the interactive flow
    std::unique_ptr<QOAuth2AuthorizationCodeFlow> mFlow;
    QOAuthHttpServerReplyHandler *mReplyHandler = nullptr;
    QTimer *mRefreshTimer = nullptr;
    QTimer *mInteractiveTimeout = nullptr; // a browser sign-in nobody finishes
    QString mServerError; // OAuth error code of the last failed token request (Qt >= 6.12)
    int mTokenHttpStatus = 0; // HTTP status of the last token request
};
