/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Unit tests for the decision whether a change the server did not take is sent again.
*/

#include "graphretrypolicy.h"

#include <QTest>

using namespace GraphRetryPolicy;

class GraphRetryPolicyTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void shouldDecide_data()
    {
        QTest::addColumn<bool>("repeatable");
        QTest::addColumn<bool>("applied");
        QTest::addColumn<int>("notExecuted");
        QTest::addColumn<int>("uncertain");
        QTest::addColumn<int>("permanent");
        QTest::addColumn<bool>("retry");

        // A create, a move: only what certainly did not reach the server goes again.
        QTest::newRow("create not executed") << false << false << 1 << 0 << 0 << true;
        QTest::newRow("create uncertain") << false << false << 0 << 1 << 0 << false;
        QTest::newRow("create rejected") << false << false << 0 << 0 << 1 << false;
        QTest::newRow("move partly applied") << false << true << 1 << 0 << 0 << false;
        QTest::newRow("moves mixed") << false << false << 2 << 1 << 0 << false;
        // Flags, deletes, edits: harmless twice, so any temporary failure goes again.
        QTest::newRow("flags uncertain") << true << false << 0 << 1 << 0 << true;
        QTest::newRow("flags not executed") << true << true << 3 << 0 << 0 << true;
        QTest::newRow("flags with a lost item") << true << true << 1 << 0 << 1 << true;
        QTest::newRow("flags rejected only") << true << false << 0 << 0 << 2 << false;
        QTest::newRow("nothing failed") << true << false << 0 << 0 << 0 << false;
    }

    void shouldDecide()
    {
        QFETCH(bool, repeatable);
        QFETCH(bool, applied);
        QFETCH(int, notExecuted);
        QFETCH(int, uncertain);
        QFETCH(int, permanent);
        QFETCH(bool, retry);
        QCOMPARE(decide(repeatable, applied, notExecuted, uncertain, permanent, 0).retry, retry);
    }

    void shouldBackOffUpToALimit()
    {
        QList<int> delays;
        for (int attempt = 0; attempt < 8; ++attempt) {
            delays.append(decide(true, false, 1, 0, 0, attempt).delaySeconds);
        }
        QCOMPARE(delays, (QList<int>{30, 60, 120, 240, 300, 300, 300, 300}));
    }

    void shouldHoldBackWhatNeverReachedTheServerForLong()
    {
        // Offline, throttled or without a token for a while: the change waits, it is not
        // dropped after a few attempts — but it does not block everything forever.
        QVERIFY(decide(false, false, 1, 0, 0, kMaxAttempts + 1).retry);
        QVERIFY(decide(true, false, 5, 0, 0, kMaxNotExecutedAttempts - 1).retry);
        QVERIFY(!decide(true, false, 5, 0, 0, kMaxNotExecutedAttempts).retry);
    }

    void shouldGiveUpOnUncertainFailuresAfterAFewAttempts()
    {
        QVERIFY(decide(true, false, 0, 1, 0, kMaxAttempts - 1).retry);
        QVERIFY(!decide(true, false, 0, 1, 0, kMaxAttempts).retry);
        QVERIFY(!decide(true, false, 1, 0, 1, kMaxAttempts).retry);
    }
};

QTEST_GUILESS_MAIN(GraphRetryPolicyTest)

#include "graphretrypolicytest.moc"
