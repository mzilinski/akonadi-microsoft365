/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Thin Graph REST client: owns the QNetworkAccessManager and the bearer token source.
    Equivalent role to EwsClient, but REST/JSON instead of SOAP/XML.
*/

#pragma once

#include <QString>
#include <chrono>
#include <functional>
#include <memory>

class QNetworkAccessManager;
class GraphOAuth;

class GraphClient
{
public:
    GraphClient();
    ~GraphClient();

    void setBaseUrl(const QString &baseUrl); // https://graph.microsoft.com/v1.0
    void setAuth(GraphOAuth *auth); // token provider (not owned)

    [[nodiscard]] QString baseUrl() const;
    [[nodiscard]] GraphOAuth *auth() const;
    [[nodiscard]] QNetworkAccessManager *networkAccessManager() const;

    /// How long a request may go without any data moving before it is given up.
    void setTransferTimeout(std::chrono::milliseconds timeout);
    [[nodiscard]] std::chrono::milliseconds transferTimeout() const;

    // Work in flight: requests and batch jobs register while they run. A resource that
    // went offline keeps its task scheduler stopped until what it started before has
    // finished, so that no change is replayed while its first attempt is still out.
    // beginWork() registers a piece of work; call the returned function once it is
    // done. It stays safe to call after the client is gone (jobs may outlive it on
    // shutdown). The idle callback runs whenever the last piece of work has finished.
    [[nodiscard]] std::function<void()> beginWork();
    [[nodiscard]] bool isBusy() const;
    void setIdleCallback(std::function<void()> callback);

private:
    QString mBaseUrl;
    GraphOAuth *mAuth = nullptr; // not owned
    std::unique_ptr<QNetworkAccessManager> mNam;
    std::chrono::milliseconds mTransferTimeout = std::chrono::minutes(2);
    struct WorkState {
        int busy = 0;
        std::function<void()> onIdle;
    };
    std::shared_ptr<WorkState> mWork;
};
