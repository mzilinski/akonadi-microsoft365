/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Unit tests for the delta-link collection attribute.
*/

#include "graphsyncstateattribute.h"

#include <QTest>
#include <QUrl>

#include <memory>

class GraphSyncStateAttributeTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void shouldBeEmptyByDefault()
    {
        const GraphSyncStateAttribute attr;
        QVERIFY(attr.deltaLink().isEmpty());
        QCOMPARE(attr.type(), QByteArrayLiteral("graphsyncstate"));
    }

    void shouldRoundTripThroughSerialization()
    {
        const QString link = QStringLiteral("https://graph.microsoft.com/v1.0/me/mailFolders/AAA/messages/delta?$deltatoken=äöü%20token");
        GraphSyncStateAttribute attr(link);
        QCOMPARE(attr.deltaLink(), link);

        GraphSyncStateAttribute copy;
        copy.deserialize(attr.serialized());
        QCOMPARE(copy.deltaLink(), link);
    }

    void shouldRoundTripTheVersion()
    {
        const QString link = QStringLiteral("https://graph.microsoft.com/v1.0/me/calendars/AAA/events/delta?$deltatoken=x");
        const GraphSyncStateAttribute attr(link, 3);
        GraphSyncStateAttribute copy;
        copy.deserialize(attr.serialized());
        QCOMPARE(copy.deltaLink(), link);
        QCOMPARE(copy.version(), 3);

        const std::unique_ptr<Akonadi::Attribute> clone(attr.clone());
        QCOMPARE(static_cast<GraphSyncStateAttribute *>(clone.get())->version(), 3);
    }

    void shouldStayUsableForBuildsWithoutVersions()
    {
        // Older builds take the stored string as the request URL. It must still point
        // at the same resource; the fragment is never sent over HTTP.
        const QString link = QStringLiteral("https://graph.microsoft.com/v1.0/me/calendars/AAA/events/delta?$deltatoken=a-b_c");
        const QUrl url(QString::fromUtf8(GraphSyncStateAttribute(link, 1).serialized()));
        QVERIFY(url.isValid());
        QCOMPARE(url.adjusted(QUrl::RemoveFragment), QUrl(link));
    }

    void shouldReadUnversionedLinksAsVersionZero()
    {
        // What earlier versions stored: the bare link.
        const QByteArray stored("https://graph.microsoft.com/v1.0/me/todo/lists/AAA/tasks/delta?$deltatoken=x");
        GraphSyncStateAttribute attr;
        attr.deserialize(stored);
        QCOMPARE(attr.deltaLink(), QString::fromUtf8(stored));
        QCOMPARE(attr.version(), 0);
        QCOMPARE(attr.serialized(), stored);
    }

    void shouldCloneIndependently()
    {
        GraphSyncStateAttribute attr(QStringLiteral("first"));
        const std::unique_ptr<Akonadi::Attribute> clone(attr.clone());
        attr.setDeltaLink(QStringLiteral("second"));

        const auto cloned = static_cast<GraphSyncStateAttribute *>(clone.get());
        QCOMPARE(cloned->deltaLink(), QStringLiteral("first"));
        QCOMPARE(cloned->type(), attr.type());
    }
};

QTEST_GUILESS_MAIN(GraphSyncStateAttributeTest)

#include "graphsyncstateattributetest.moc"
