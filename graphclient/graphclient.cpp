/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "graphclient.h"

#include <QNetworkAccessManager>

GraphClient::GraphClient()
    : mBaseUrl(QStringLiteral("https://graph.microsoft.com/v1.0"))
    , mNam(std::make_unique<QNetworkAccessManager>())
    , mWork(std::make_shared<WorkState>())
{
}

GraphClient::~GraphClient() = default;

void GraphClient::setBaseUrl(const QString &baseUrl)
{
    mBaseUrl = baseUrl;
}

void GraphClient::setAuth(GraphOAuth *auth)
{
    mAuth = auth;
}

QString GraphClient::baseUrl() const
{
    return mBaseUrl;
}

GraphOAuth *GraphClient::auth() const
{
    return mAuth;
}

QNetworkAccessManager *GraphClient::networkAccessManager() const
{
    return mNam.get();
}

void GraphClient::setTransferTimeout(std::chrono::milliseconds timeout)
{
    mTransferTimeout = timeout;
}

std::chrono::milliseconds GraphClient::transferTimeout() const
{
    return mTransferTimeout;
}

std::function<void()> GraphClient::beginWork()
{
    ++mWork->busy;
    return [state = mWork, done = std::make_shared<bool>(false)] {
        if (*done) {
            return;
        }
        *done = true;
        if (state->busy > 0 && --state->busy == 0 && state->onIdle) {
            state->onIdle();
        }
    };
}

bool GraphClient::isBusy() const
{
    return mWork->busy > 0;
}

void GraphClient::setIdleCallback(std::function<void()> callback)
{
    mWork->onIdle = std::move(callback);
}
