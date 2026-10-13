/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Unit tests for what a failed token request means: try again later, or ask the
    user to sign in. A dead refresh token must never open the browser by itself.
*/

#include "graphclient/auth/graphoauth.h"

#include <QTest>

using Failure = GraphOAuth::Failure;
using Error = QAbstractOAuth::Error;

class GraphOAuthTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void shouldClassifyFailure_data()
    {
        QTest::addColumn<Error>("error");
        QTest::addColumn<int>("httpStatus");
        QTest::addColumn<QString>("serverError");
        QTest::addColumn<bool>("interactive");
        QTest::addColumn<Failure>("failure");

        // Silent refresh: a missing network or a service outage leaves the account alone.
        QTest::newRow("no network") << Error::NetworkError << 0 << QString() << false << Failure::Unreachable;
        QTest::newRow("service down") << Error::ServerError << 503 << QString() << false << Failure::Unreachable;
        QTest::newRow("throttled") << Error::ServerError << 429 << QString() << false << Failure::Unreachable;
        QTest::newRow("temporarily unavailable") << Error::ServerError << 400 << QStringLiteral("temporarily_unavailable") << false << Failure::Unreachable;
        QTest::newRow("server error") << Error::ServerError << 0 << QStringLiteral("server_error") << false << Failure::Unreachable;
        // Silent refresh: the refresh token is dead; the user decides when to sign in.
        QTest::newRow("revoked") << Error::ServerError << 400 << QStringLiteral("invalid_grant") << false << Failure::SignInRequired;
        QTest::newRow("revoked, status unknown") << Error::ServerError << 0 << QStringLiteral("invalid_grant") << false << Failure::SignInRequired;
        QTest::newRow("no token in answer") << Error::OAuthTokenNotFoundError << 200 << QString() << false << Failure::SignInRequired;
        // The browser sign-in itself: whatever went wrong, it failed.
        QTest::newRow("interactive, no network") << Error::NetworkError << 0 << QString() << true << Failure::Failed;
        QTest::newRow("interactive, denied") << Error::ServerError << 400 << QStringLiteral("access_denied") << true << Failure::Failed;
    }

    void shouldClassifyFailure()
    {
        QFETCH(Error, error);
        QFETCH(int, httpStatus);
        QFETCH(QString, serverError);
        QFETCH(bool, interactive);
        QFETCH(Failure, failure);

        QCOMPARE(GraphOAuth::classifyFailure(error, httpStatus, serverError, interactive), failure);
    }
};

QTEST_GUILESS_MAIN(GraphOAuthTest)

#include "graphoauthtest.moc"
