/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Unit tests for the Graph event JSON <-> KCalendarCore::Event mapping.
*/

#include "calendar/grapheventhandler.h"

#include <KCalendarCore/Attendee>
#include <KCalendarCore/ICalFormat>
#include <KCalendarCore/Recurrence>

#include <QJsonArray>
#include <QJsonObject>
#include <QTest>
#include <QTimeZone>

#include <functional>

using namespace KCalendarCore;

namespace
{
QJsonObject graphDateTime(const QString &dateTime)
{
    QJsonObject o;
    o.insert(QStringLiteral("dateTime"), dateTime);
    o.insert(QStringLiteral("timeZone"), QStringLiteral("UTC"));
    return o;
}

QJsonObject graphDateTime(const QString &dateTime, const QString &timeZone)
{
    QJsonObject o;
    o.insert(QStringLiteral("dateTime"), dateTime);
    o.insert(QStringLiteral("timeZone"), timeZone);
    return o;
}

// A series as Graph returns it: UTC instants, the creation zone beside them.
QJsonObject seriesJson(const QString &start, const QString &end, const QJsonObject &pattern, const QString &recurrenceTimeZone = {})
{
    QJsonObject json;
    json.insert(QStringLiteral("id"), QStringLiteral("AAMkAGI2"));
    json.insert(QStringLiteral("start"), graphDateTime(start));
    json.insert(QStringLiteral("end"), graphDateTime(end));
    QJsonObject range;
    range.insert(QStringLiteral("type"), QStringLiteral("noEnd"));
    range.insert(QStringLiteral("startDate"), start.left(10));
    if (!recurrenceTimeZone.isEmpty()) {
        range.insert(QStringLiteral("recurrenceTimeZone"), recurrenceTimeZone);
    }
    QJsonObject recurrence;
    recurrence.insert(QStringLiteral("pattern"), pattern);
    recurrence.insert(QStringLiteral("range"), range);
    json.insert(QStringLiteral("recurrence"), recurrence);
    return json;
}

QJsonObject relativePattern(const QString &type, const QString &index, const QJsonArray &days, int month = 0)
{
    QJsonObject pattern;
    pattern.insert(QStringLiteral("type"), type);
    pattern.insert(QStringLiteral("interval"), 1);
    pattern.insert(QStringLiteral("index"), index);
    pattern.insert(QStringLiteral("daysOfWeek"), days);
    pattern.insert(QStringLiteral("month"), month);
    pattern.insert(QStringLiteral("dayOfMonth"), 0);
    return pattern;
}

QJsonObject absolutePattern(const QString &type, int dayOfMonth, int month = 0)
{
    QJsonObject pattern;
    pattern.insert(QStringLiteral("type"), type);
    pattern.insert(QStringLiteral("interval"), 1);
    pattern.insert(QStringLiteral("dayOfMonth"), dayOfMonth);
    pattern.insert(QStringLiteral("month"), month);
    pattern.insert(QStringLiteral("index"), QStringLiteral("first"));
    return pattern;
}

QJsonObject writtenPattern(const Event::Ptr &event)
{
    return GraphEventHandler::toJson(event).value(QLatin1String("recurrence")).toObject().value(QLatin1String("pattern")).toObject();
}

QJsonObject emailAddress(const QString &name, const QString &address)
{
    QJsonObject email;
    email.insert(QStringLiteral("name"), name);
    email.insert(QStringLiteral("address"), address);
    QJsonObject o;
    o.insert(QStringLiteral("emailAddress"), email);
    return o;
}
} // namespace

class GraphEventHandlerTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void shouldReturnNullWithoutId()
    {
        QJsonObject json;
        json.insert(QStringLiteral("subject"), QStringLiteral("No id"));
        QVERIFY(!GraphEventHandler::toEvent(json));
    }

    void shouldMapBasicFields()
    {
        QJsonObject json;
        json.insert(QStringLiteral("id"), QStringLiteral("AAMkAGI2"));
        json.insert(QStringLiteral("iCalUId"), QStringLiteral("040000008200E00074C5B7101A82E008"));
        json.insert(QStringLiteral("subject"), QStringLiteral("Team Meeting"));
        QJsonObject body;
        body.insert(QStringLiteral("contentType"), QStringLiteral("text"));
        body.insert(QStringLiteral("content"), QStringLiteral("Agenda: Ä, Ö, Ü"));
        json.insert(QStringLiteral("body"), body);
        json.insert(QStringLiteral("isAllDay"), false);
        // Graph returns 7 fractional digits; the parser must cope with them.
        json.insert(QStringLiteral("start"), graphDateTime(QStringLiteral("2026-07-10T09:30:00.0000000")));
        json.insert(QStringLiteral("end"), graphDateTime(QStringLiteral("2026-07-10T10:00:00.0000000")));
        QJsonObject location;
        location.insert(QStringLiteral("displayName"), QStringLiteral("Room 1"));
        json.insert(QStringLiteral("location"), location);
        json.insert(QStringLiteral("organizer"), emailAddress(QStringLiteral("Alice"), QStringLiteral("alice@example.com")));
        json.insert(QStringLiteral("categories"), QJsonArray{QStringLiteral("Work"), QStringLiteral("Travel")});
        json.insert(QStringLiteral("showAs"), QStringLiteral("free"));
        json.insert(QStringLiteral("sensitivity"), QStringLiteral("private"));

        const Event::Ptr event = GraphEventHandler::toEvent(json);
        QVERIFY(event);
        QCOMPARE(event->uid(), QStringLiteral("040000008200E00074C5B7101A82E008"));
        QCOMPARE(event->summary(), QStringLiteral("Team Meeting"));
        QCOMPARE(event->description(), QStringLiteral("Agenda: Ä, Ö, Ü"));
        QVERIFY(!event->allDay());
        // QDateTime comparison is instant-based, so this is time-zone independent.
        QCOMPARE(event->dtStart(), QDateTime(QDate(2026, 7, 10), QTime(9, 30), QTimeZone::utc()));
        QCOMPARE(event->dtEnd(), QDateTime(QDate(2026, 7, 10), QTime(10, 0), QTimeZone::utc()));
        QCOMPARE(event->location(), QStringLiteral("Room 1"));
        QCOMPARE(event->organizer().email(), QStringLiteral("alice@example.com"));
        QCOMPARE(event->categories(), QStringList({QStringLiteral("Work"), QStringLiteral("Travel")}));
        QCOMPARE(event->transparency(), Event::Transparent);
        QCOMPARE(event->secrecy(), Incidence::SecrecyPrivate);
    }

    void shouldFallBackToGraphIdAsUid()
    {
        QJsonObject json;
        json.insert(QStringLiteral("id"), QStringLiteral("AAMkAGI2"));
        const Event::Ptr event = GraphEventHandler::toEvent(json);
        QVERIFY(event);
        QCOMPARE(event->uid(), QStringLiteral("AAMkAGI2"));
    }

    void shouldMapAttendeesWithStatus()
    {
        QJsonObject json;
        json.insert(QStringLiteral("id"), QStringLiteral("AAMkAGI2"));
        QJsonArray attendees;
        const auto withStatus = [](QJsonObject attendee, const QString &response) {
            QJsonObject status;
            status.insert(QStringLiteral("response"), response);
            attendee.insert(QStringLiteral("status"), status);
            return attendee;
        };
        attendees.append(withStatus(emailAddress(QStringLiteral("Bob"), QStringLiteral("bob@example.com")), QStringLiteral("accepted")));
        attendees.append(withStatus(emailAddress(QStringLiteral("Carol"), QStringLiteral("carol@example.com")), QStringLiteral("declined")));
        attendees.append(withStatus(emailAddress(QStringLiteral("Dave"), QStringLiteral("dave@example.com")), QStringLiteral("tentativelyAccepted")));
        json.insert(QStringLiteral("attendees"), attendees);

        const Event::Ptr event = GraphEventHandler::toEvent(json);
        QVERIFY(event);
        const Attendee::List list = event->attendees();
        QCOMPARE(list.size(), 3);
        QCOMPARE(list.at(0).email(), QStringLiteral("bob@example.com"));
        QCOMPARE(list.at(0).status(), Attendee::Accepted);
        QCOMPARE(list.at(1).status(), Attendee::Declined);
        QCOMPARE(list.at(2).status(), Attendee::Tentative);
    }

    void shouldMapWeeklyRecurrence()
    {
        QJsonObject json;
        json.insert(QStringLiteral("id"), QStringLiteral("AAMkAGI2"));
        json.insert(QStringLiteral("start"), graphDateTime(QStringLiteral("2026-07-06T08:00:00.0000000")));
        json.insert(QStringLiteral("end"), graphDateTime(QStringLiteral("2026-07-06T09:00:00.0000000")));
        QJsonObject pattern;
        pattern.insert(QStringLiteral("type"), QStringLiteral("weekly"));
        pattern.insert(QStringLiteral("interval"), 2);
        pattern.insert(QStringLiteral("daysOfWeek"), QJsonArray{QStringLiteral("monday"), QStringLiteral("wednesday")});
        QJsonObject range;
        range.insert(QStringLiteral("type"), QStringLiteral("numbered"));
        range.insert(QStringLiteral("numberOfOccurrences"), 10);
        QJsonObject recurrence;
        recurrence.insert(QStringLiteral("pattern"), pattern);
        recurrence.insert(QStringLiteral("range"), range);
        json.insert(QStringLiteral("recurrence"), recurrence);

        const Event::Ptr event = GraphEventHandler::toEvent(json);
        QVERIFY(event);
        Recurrence *r = event->recurrence();
        QVERIFY(r->recurs());
        QCOMPARE(r->recurrenceType(), static_cast<ushort>(Recurrence::rWeekly));
        QCOMPARE(r->frequency(), 2);
        QCOMPARE(r->duration(), 10);
        const QBitArray days = r->days();
        QVERIFY(days.testBit(0)); // Monday
        QVERIFY(!days.testBit(1));
        QVERIFY(days.testBit(2)); // Wednesday
    }

    void shouldMapDailyRecurrenceWithEndDate()
    {
        QJsonObject json;
        json.insert(QStringLiteral("id"), QStringLiteral("AAMkAGI2"));
        json.insert(QStringLiteral("start"), graphDateTime(QStringLiteral("2026-07-06T08:00:00.0000000")));
        json.insert(QStringLiteral("end"), graphDateTime(QStringLiteral("2026-07-06T09:00:00.0000000")));
        QJsonObject pattern;
        pattern.insert(QStringLiteral("type"), QStringLiteral("daily"));
        pattern.insert(QStringLiteral("interval"), 1);
        QJsonObject range;
        range.insert(QStringLiteral("type"), QStringLiteral("endDate"));
        range.insert(QStringLiteral("endDate"), QStringLiteral("2026-08-31"));
        QJsonObject recurrence;
        recurrence.insert(QStringLiteral("pattern"), pattern);
        recurrence.insert(QStringLiteral("range"), range);
        json.insert(QStringLiteral("recurrence"), recurrence);

        const Event::Ptr event = GraphEventHandler::toEvent(json);
        QVERIFY(event);
        Recurrence *r = event->recurrence();
        QVERIFY(r->recurs());
        QCOMPARE(r->recurrenceType(), static_cast<ushort>(Recurrence::rDaily));
        QCOMPARE(r->endDate(), QDate(2026, 8, 31));
    }

    void shouldWriteJsonForTimedEvent()
    {
        Event::Ptr event(new Event);
        event->setSummary(QStringLiteral("Review"));
        event->setDescription(QStringLiteral("Bring the numbers"));
        event->setAllDay(false);
        event->setDtStart(QDateTime(QDate(2026, 7, 10), QTime(9, 30), QTimeZone::utc()));
        event->setDtEnd(QDateTime(QDate(2026, 7, 10), QTime(10, 0), QTimeZone::utc()));
        event->setLocation(QStringLiteral("Room 2"));
        event->setCategories(QStringList{QStringLiteral("Work")});

        const QJsonObject json = GraphEventHandler::toJson(event);
        QCOMPARE(json.value(QLatin1String("subject")).toString(), QStringLiteral("Review"));
        const QJsonObject body = json.value(QLatin1String("body")).toObject();
        QCOMPARE(body.value(QLatin1String("contentType")).toString(), QStringLiteral("text"));
        QCOMPARE(body.value(QLatin1String("content")).toString(), QStringLiteral("Bring the numbers"));
        QCOMPARE(json.value(QLatin1String("isAllDay")).toBool(), false);
        const QJsonObject start = json.value(QLatin1String("start")).toObject();
        QCOMPARE(start.value(QLatin1String("dateTime")).toString(), QStringLiteral("2026-07-10T09:30:00.0000000"));
        QCOMPARE(start.value(QLatin1String("timeZone")).toString(), QStringLiteral("UTC"));
        QCOMPARE(json.value(QLatin1String("location")).toObject().value(QLatin1String("displayName")).toString(), QStringLiteral("Room 2"));
        QCOMPARE(json.value(QLatin1String("categories")).toArray(), QJsonArray{QStringLiteral("Work")});
        QCOMPARE(json.value(QLatin1String("showAs")).toString(), QStringLiteral("busy"));
    }

    void shouldReadAllDayEventWithInclusiveEnd()
    {
        QJsonObject json;
        json.insert(QStringLiteral("id"), QStringLiteral("AAMkAGI2"));
        json.insert(QStringLiteral("subject"), QStringLiteral("Birthday"));
        json.insert(QStringLiteral("isAllDay"), true);
        // Graph's all-day end is exclusive: a one-day event ends at the next midnight.
        json.insert(QStringLiteral("start"), graphDateTime(QStringLiteral("2026-07-10T00:00:00.0000000")));
        json.insert(QStringLiteral("end"), graphDateTime(QStringLiteral("2026-07-11T00:00:00.0000000")));

        const Event::Ptr event = GraphEventHandler::toEvent(json);
        QVERIFY(event);
        QVERIFY(event->allDay());
        QCOMPARE(event->dtStart().date(), QDate(2026, 7, 10));
        // KCalendarCore's all-day end is inclusive: same day, not two days.
        QCOMPARE(event->dtEnd().date(), QDate(2026, 7, 10));
    }

    void shouldWriteJsonForAllDayEvent()
    {
        Event::Ptr event(new Event);
        event->setSummary(QStringLiteral("Holiday"));
        event->setAllDay(true);
        // Inclusive KCalendarCore semantics: a one-day event starts and ends on the 10th.
        event->setDtStart(QDateTime(QDate(2026, 7, 10), QTime(0, 0)));
        event->setDtEnd(QDateTime(QDate(2026, 7, 10), QTime(0, 0)));

        const QJsonObject json = GraphEventHandler::toJson(event);
        QCOMPARE(json.value(QLatin1String("isAllDay")).toBool(), true);
        // All-day events must carry a midnight timestamp, whatever the local zone.
        QCOMPARE(json.value(QLatin1String("start")).toObject().value(QLatin1String("dateTime")).toString(), QStringLiteral("2026-07-10T00:00:00.0000000"));
        // ...and Graph expects the exclusive end (midnight of the following day).
        QCOMPARE(json.value(QLatin1String("end")).toObject().value(QLatin1String("dateTime")).toString(), QStringLiteral("2026-07-11T00:00:00.0000000"));
    }

    void shouldWriteWeeklyRecurrence()
    {
        Event::Ptr event(new Event);
        event->setSummary(QStringLiteral("Weekly sync"));
        event->setDtStart(QDateTime(QDate(2026, 7, 6), QTime(8, 0), QTimeZone::utc())); // a Monday
        event->setDtEnd(QDateTime(QDate(2026, 7, 6), QTime(9, 0), QTimeZone::utc()));
        QBitArray days(7);
        days.setBit(0); // Monday
        days.setBit(2); // Wednesday
        event->recurrence()->setWeekly(2, days);
        event->recurrence()->setDuration(10);

        const QJsonObject json = GraphEventHandler::toJson(event);
        const QJsonObject recurrence = json.value(QLatin1String("recurrence")).toObject();
        QVERIFY(!recurrence.isEmpty());
        const QJsonObject pattern = recurrence.value(QLatin1String("pattern")).toObject();
        QCOMPARE(pattern.value(QLatin1String("type")).toString(), QStringLiteral("weekly"));
        QCOMPARE(pattern.value(QLatin1String("interval")).toInt(), 2);
        QCOMPARE(pattern.value(QLatin1String("daysOfWeek")).toArray(), (QJsonArray{QStringLiteral("monday"), QStringLiteral("wednesday")}));
        const QJsonObject range = recurrence.value(QLatin1String("range")).toObject();
        QCOMPARE(range.value(QLatin1String("type")).toString(), QStringLiteral("numbered"));
        QCOMPARE(range.value(QLatin1String("numberOfOccurrences")).toInt(), 10);
        QCOMPARE(range.value(QLatin1String("startDate")).toString(), QStringLiteral("2026-07-06"));
    }

    void shouldRoundTripRecurrence()
    {
        // read -> write must preserve the rule.
        QJsonObject json;
        json.insert(QStringLiteral("id"), QStringLiteral("AAMkAGI2"));
        json.insert(QStringLiteral("start"), graphDateTime(QStringLiteral("2026-07-06T08:00:00.0000000")));
        json.insert(QStringLiteral("end"), graphDateTime(QStringLiteral("2026-07-06T09:00:00.0000000")));
        QJsonObject pattern;
        pattern.insert(QStringLiteral("type"), QStringLiteral("weekly"));
        pattern.insert(QStringLiteral("interval"), 2);
        pattern.insert(QStringLiteral("daysOfWeek"), QJsonArray{QStringLiteral("monday"), QStringLiteral("wednesday")});
        QJsonObject range;
        range.insert(QStringLiteral("type"), QStringLiteral("numbered"));
        range.insert(QStringLiteral("numberOfOccurrences"), 10);
        QJsonObject recurrence;
        recurrence.insert(QStringLiteral("pattern"), pattern);
        recurrence.insert(QStringLiteral("range"), range);
        json.insert(QStringLiteral("recurrence"), recurrence);

        const QJsonObject back = GraphEventHandler::toJson(GraphEventHandler::toEvent(json)).value(QLatin1String("recurrence")).toObject();
        const QJsonObject backPattern = back.value(QLatin1String("pattern")).toObject();
        QCOMPARE(backPattern.value(QLatin1String("type")).toString(), QStringLiteral("weekly"));
        QCOMPARE(backPattern.value(QLatin1String("interval")).toInt(), 2);
        QCOMPARE(backPattern.value(QLatin1String("daysOfWeek")).toArray(), (QJsonArray{QStringLiteral("monday"), QStringLiteral("wednesday")}));
        QCOMPARE(back.value(QLatin1String("range")).toObject().value(QLatin1String("numberOfOccurrences")).toInt(), 10);
    }

    void shouldRoundTripThroughJson()
    {
        QJsonObject json;
        json.insert(QStringLiteral("id"), QStringLiteral("AAMkAGI2"));
        json.insert(QStringLiteral("subject"), QStringLiteral("Round trip"));
        QJsonObject body;
        body.insert(QStringLiteral("contentType"), QStringLiteral("text"));
        body.insert(QStringLiteral("content"), QStringLiteral("Body text"));
        json.insert(QStringLiteral("body"), body);
        json.insert(QStringLiteral("isAllDay"), false);
        json.insert(QStringLiteral("start"), graphDateTime(QStringLiteral("2026-07-10T09:30:00.0000000")));
        json.insert(QStringLiteral("end"), graphDateTime(QStringLiteral("2026-07-10T10:00:00.0000000")));

        const QJsonObject back = GraphEventHandler::toJson(GraphEventHandler::toEvent(json));
        QCOMPARE(back.value(QLatin1String("subject")), json.value(QLatin1String("subject")));
        QCOMPARE(back.value(QLatin1String("body")), json.value(QLatin1String("body")));
        QCOMPARE(back.value(QLatin1String("start")), json.value(QLatin1String("start")));
        QCOMPARE(back.value(QLatin1String("end")), json.value(QLatin1String("end")));
    }
    void shouldKeepTheWallClockTimeOfASeriesAcrossDst()
    {
        // A weekly 06:45 series created in Berlin during summer time, as Graph returns it.
        QJsonObject pattern;
        pattern.insert(QStringLiteral("type"), QStringLiteral("weekly"));
        pattern.insert(QStringLiteral("interval"), 1);
        pattern.insert(QStringLiteral("daysOfWeek"), QJsonArray{QStringLiteral("wednesday")});
        pattern.insert(QStringLiteral("firstDayOfWeek"), QStringLiteral("sunday"));
        QJsonObject json = seriesJson(QStringLiteral("2026-06-24T04:45:00.0000000"),
                                      QStringLiteral("2026-06-24T06:00:00.0000000"),
                                      pattern,
                                      QStringLiteral("W. Europe Standard Time"));
        json.insert(QStringLiteral("originalStartTimeZone"), QStringLiteral("Europe/Berlin"));

        const Event::Ptr event = GraphEventHandler::toEvent(json);
        QVERIFY(event);
        const QTimeZone berlin("Europe/Berlin");
        QCOMPARE(event->dtStart().timeZone(), berlin);
        QCOMPARE(event->dtStart(), QDateTime(QDate(2026, 6, 24), QTime(4, 45), QTimeZone::utc()));
        // Winter: still 06:45 local, i.e. 05:45 UTC instead of 04:45.
        QCOMPARE(event->recurrence()->recurTimesOn(QDate(2026, 12, 2), berlin), QList<QTime>{QTime(6, 45)});

        const QJsonObject back = GraphEventHandler::toJson(event);
        const QJsonObject start = back.value(QLatin1String("start")).toObject();
        QCOMPARE(start.value(QLatin1String("dateTime")).toString(), QStringLiteral("2026-06-24T06:45:00.0000000"));
        QCOMPARE(start.value(QLatin1String("timeZone")).toString(), QStringLiteral("W. Europe Standard Time"));
        const QJsonObject range = back.value(QLatin1String("recurrence")).toObject().value(QLatin1String("range")).toObject();
        QCOMPARE(range.value(QLatin1String("recurrenceTimeZone")).toString(), QStringLiteral("W. Europe Standard Time"));
        QCOMPARE(range.value(QLatin1String("startDate")).toString(), QStringLiteral("2026-06-24"));
    }

    void shouldWriteASeriesCreatedInKdeInItsZone()
    {
        const QTimeZone berlin("Europe/Berlin");
        Event::Ptr event(new Event);
        event->setDtStart(QDateTime(QDate(2026, 7, 6), QTime(10, 0), berlin));
        event->setDtEnd(QDateTime(QDate(2026, 7, 6), QTime(11, 0), berlin));
        event->recurrence()->setWeekly(1);

        const QJsonObject json = GraphEventHandler::toJson(event);
        const QJsonObject start = json.value(QLatin1String("start")).toObject();
        QCOMPARE(start.value(QLatin1String("dateTime")).toString(), QStringLiteral("2026-07-06T10:00:00.0000000"));
        QCOMPARE(start.value(QLatin1String("timeZone")).toString(), QStringLiteral("W. Europe Standard Time"));
        QCOMPARE(json.value(QLatin1String("end")).toObject().value(QLatin1String("dateTime")).toString(), QStringLiteral("2026-07-06T11:00:00.0000000"));
        const QJsonObject recurrence = json.value(QLatin1String("recurrence")).toObject();
        QCOMPARE(recurrence.value(QLatin1String("range")).toObject().value(QLatin1String("recurrenceTimeZone")).toString(),
                 QStringLiteral("W. Europe Standard Time"));
        QCOMPARE(recurrence.value(QLatin1String("pattern")).toObject().value(QLatin1String("daysOfWeek")).toArray(), QJsonArray{QStringLiteral("monday")});
    }

    void shouldWriteLegacyZoneIdsWithTheirWindowsName_data()
    {
        // Legacy id, and the current id it is an alias of.
        QTest::addColumn<QByteArray>("ianaId");
        QTest::addColumn<QByteArray>("currentId");
        QTest::newRow("Europe/Kiev") << QByteArrayLiteral("Europe/Kiev") << QByteArrayLiteral("Europe/Kyiv");
        QTest::newRow("US/Eastern") << QByteArrayLiteral("US/Eastern") << QByteArrayLiteral("America/New_York");
        QTest::newRow("Asia/Calcutta") << QByteArrayLiteral("Asia/Calcutta") << QByteArrayLiteral("Asia/Kolkata");
        QTest::newRow("Asia/Saigon") << QByteArrayLiteral("Asia/Saigon") << QByteArrayLiteral("Asia/Ho_Chi_Minh");
    }

    void shouldWriteLegacyZoneIdsWithTheirWindowsName()
    {
        // Older systems still use ids that CLDR's Windows table lacks. Written as UTC,
        // a series would drift at every daylight-saving change.
        QFETCH(QByteArray, ianaId);
        QFETCH(QByteArray, currentId);
        const QString windowsId = QString::fromLatin1(QTimeZone::ianaIdToWindowsId(currentId));
        QVERIFY(!windowsId.isEmpty());
        const QTimeZone zone(ianaId);
        if (!zone.isValid()) {
            QSKIP("time zone id not known to this system");
        }
        Event::Ptr event(new Event);
        event->setDtStart(QDateTime(QDate(2026, 7, 6), QTime(10, 0), zone));
        event->setDtEnd(event->dtStart().addSecs(3600));
        event->recurrence()->setWeekly(1);
        const QJsonObject json = GraphEventHandler::toJson(event);
        QCOMPARE(json.value(QLatin1String("start")).toObject().value(QLatin1String("timeZone")).toString(), windowsId);
        QCOMPARE(json.value(QLatin1String("start")).toObject().value(QLatin1String("dateTime")).toString(), QStringLiteral("2026-07-06T10:00:00.0000000"));
        QCOMPARE(
            json.value(QLatin1String("recurrence")).toObject().value(QLatin1String("range")).toObject().value(QLatin1String("recurrenceTimeZone")).toString(),
            windowsId);
    }

    void shouldWriteFloatingTimesInTheSystemZone()
    {
        Event::Ptr event(new Event);
        event->setDtStart(QDateTime(QDate(2026, 7, 6), QTime(10, 0)));
        event->setDtEnd(QDateTime(QDate(2026, 7, 6), QTime(11, 0)));

        const QJsonObject start = GraphEventHandler::toJson(event).value(QLatin1String("start")).toObject();
        QCOMPARE(start.value(QLatin1String("dateTime")).toString(), QStringLiteral("2026-07-06T10:00:00.0000000"));
        QCOMPARE(GraphEventHandler::timeZoneFromGraph(start.value(QLatin1String("timeZone")).toString()).offsetFromUtc(event->dtStart()),
                 QTimeZone::systemTimeZone().offsetFromUtc(event->dtStart()));
    }

    void shouldResolveGraphTimeZoneNames()
    {
        QCOMPARE(GraphEventHandler::timeZoneFromGraph(QStringLiteral("W. Europe Standard Time")).id(), QByteArrayLiteral("Europe/Berlin"));
        QCOMPARE(GraphEventHandler::timeZoneFromGraph(QStringLiteral("Europe/Berlin")).id(), QByteArrayLiteral("Europe/Berlin"));
        QCOMPARE(GraphEventHandler::timeZoneFromGraph(QStringLiteral("Pacific Standard Time")).id(), QByteArrayLiteral("America/Los_Angeles"));
        QCOMPARE(GraphEventHandler::timeZoneFromGraph(QStringLiteral("UTC")), QTimeZone::utc());
        QCOMPARE(GraphEventHandler::timeZoneFromGraph(QStringLiteral("tzone://Microsoft/Utc")), QTimeZone::utc());
        QVERIFY(!GraphEventHandler::timeZoneFromGraph(QStringLiteral("tzone://Microsoft/Custom")).isValid());
        QVERIFY(!GraphEventHandler::timeZoneFromGraph(QString()).isValid());
    }

    void shouldParseTimesInTheirNamedZone()
    {
        const QDateTime dt =
            GraphEventHandler::parseDateTimeTimeZone(graphDateTime(QStringLiteral("2026-10-05T09:00:00.0000000"), QStringLiteral("W. Europe Standard Time")));
        QCOMPARE(dt, QDateTime(QDate(2026, 10, 5), QTime(7, 0), QTimeZone::utc()));
        // An unknown zone is read as UTC rather than dropped.
        const QDateTime unknown =
            GraphEventHandler::parseDateTimeTimeZone(graphDateTime(QStringLiteral("2026-10-05T09:00:00.0000000"), QStringLiteral("tzone://Microsoft/Custom")));
        QCOMPARE(unknown, QDateTime(QDate(2026, 10, 5), QTime(9, 0), QTimeZone::utc()));
    }

    void shouldMoveSingleEventsIntoTheirOriginalZones()
    {
        QJsonObject json;
        json.insert(QStringLiteral("id"), QStringLiteral("AAMkAGI2"));
        json.insert(QStringLiteral("start"), graphDateTime(QStringLiteral("2026-07-10T08:00:00.0000000")));
        json.insert(QStringLiteral("end"), graphDateTime(QStringLiteral("2026-07-10T18:00:00.0000000")));
        json.insert(QStringLiteral("originalStartTimeZone"), QStringLiteral("W. Europe Standard Time"));
        json.insert(QStringLiteral("originalEndTimeZone"), QStringLiteral("Eastern Standard Time"));

        const Event::Ptr event = GraphEventHandler::toEvent(json);
        QVERIFY(event);
        QCOMPARE(event->dtStart().timeZone().id(), QByteArrayLiteral("Europe/Berlin"));
        QCOMPARE(event->dtEnd().timeZone().id(), QByteArrayLiteral("America/New_York"));
        // Same instants as Graph sent.
        QCOMPARE(event->dtStart(), QDateTime(QDate(2026, 7, 10), QTime(8, 0), QTimeZone::utc()));
        QCOMPARE(event->dtEnd(), QDateTime(QDate(2026, 7, 10), QTime(18, 0), QTimeZone::utc()));
    }

    void shouldExpandSeriesWithoutKnownZoneInTheLocalZone()
    {
        QJsonObject pattern;
        pattern.insert(QStringLiteral("type"), QStringLiteral("daily"));
        pattern.insert(QStringLiteral("interval"), 1);
        QJsonObject json = seriesJson(QStringLiteral("2026-07-06T08:00:00.0000000"), QStringLiteral("2026-07-06T09:00:00.0000000"), pattern);
        json.insert(QStringLiteral("originalStartTimeZone"), QStringLiteral("tzone://Microsoft/Custom"));

        const Event::Ptr event = GraphEventHandler::toEvent(json);
        QVERIFY(event);
        QCOMPARE(event->dtStart().timeZone(), QTimeZone::systemTimeZone());
    }

    void shouldCountWeeksFromTheSeriesWeekStart()
    {
        // Every other week on Sunday and Monday, weeks starting on Sunday as in
        // Outlook: Sunday and the following Monday belong to the same week.
        QJsonObject pattern;
        pattern.insert(QStringLiteral("type"), QStringLiteral("weekly"));
        pattern.insert(QStringLiteral("interval"), 2);
        pattern.insert(QStringLiteral("daysOfWeek"), QJsonArray{QStringLiteral("sunday"), QStringLiteral("monday")});
        pattern.insert(QStringLiteral("firstDayOfWeek"), QStringLiteral("sunday"));
        const Event::Ptr event = GraphEventHandler::toEvent(
            seriesJson(QStringLiteral("2026-10-04T08:00:00.0000000"), QStringLiteral("2026-10-04T09:00:00.0000000"), pattern, QStringLiteral("UTC")));
        QVERIFY(event);
        const Recurrence *r = event->recurrence();
        QCOMPARE(r->weekStart(), 7);
        QVERIFY(r->recursOn(QDate(2026, 10, 4), QTimeZone::utc()));
        QVERIFY(r->recursOn(QDate(2026, 10, 5), QTimeZone::utc()));
        QVERIFY(!r->recursOn(QDate(2026, 10, 11), QTimeZone::utc()));
        QVERIFY(!r->recursOn(QDate(2026, 10, 12), QTimeZone::utc()));
        QVERIFY(r->recursOn(QDate(2026, 10, 18), QTimeZone::utc()));
        QVERIFY(r->recursOn(QDate(2026, 10, 19), QTimeZone::utc()));
        QCOMPARE(writtenPattern(event).value(QLatin1String("firstDayOfWeek")).toString(), QStringLiteral("sunday"));
    }

    void shouldMapNthWeekdayOfTheMonth()
    {
        // "Every first Monday", starting Monday 2026-10-05.
        const QJsonObject pattern = relativePattern(QStringLiteral("relativeMonthly"), QStringLiteral("first"), QJsonArray{QStringLiteral("monday")});
        const Event::Ptr event = GraphEventHandler::toEvent(
            seriesJson(QStringLiteral("2026-10-05T08:00:00.0000000"), QStringLiteral("2026-10-05T09:00:00.0000000"), pattern, QStringLiteral("UTC")));
        QVERIFY(event);
        const Recurrence *r = event->recurrence();
        QCOMPARE(r->recurrenceType(), static_cast<ushort>(Recurrence::rMonthlyPos));
        QVERIFY(r->recursOn(QDate(2026, 11, 2), QTimeZone::utc()));
        QVERIFY(!r->recursOn(QDate(2026, 11, 5), QTimeZone::utc()));
        QVERIFY(r->recursOn(QDate(2026, 12, 7), QTimeZone::utc()));

        const QJsonObject back = writtenPattern(event);
        QCOMPARE(back.value(QLatin1String("type")).toString(), QStringLiteral("relativeMonthly"));
        QCOMPARE(back.value(QLatin1String("index")).toString(), QStringLiteral("first"));
        QCOMPARE(back.value(QLatin1String("daysOfWeek")).toArray(), QJsonArray{QStringLiteral("monday")});
    }

    void shouldMapTheFirstOfSeveralWeekdays()
    {
        // "The first weekday of every month": one occurrence a month, not five.
        const QJsonArray weekdays{QStringLiteral("monday"),
                                  QStringLiteral("tuesday"),
                                  QStringLiteral("wednesday"),
                                  QStringLiteral("thursday"),
                                  QStringLiteral("friday")};
        const QJsonObject pattern = relativePattern(QStringLiteral("relativeMonthly"), QStringLiteral("first"), weekdays);
        const Event::Ptr event = GraphEventHandler::toEvent(
            seriesJson(QStringLiteral("2026-07-01T08:00:00.0000000"), QStringLiteral("2026-07-01T09:00:00.0000000"), pattern, QStringLiteral("UTC")));
        QVERIFY(event);
        const Recurrence *r = event->recurrence();
        QVERIFY(r->recursOn(QDate(2026, 8, 3), QTimeZone::utc())); // 1 August is a Saturday
        QVERIFY(!r->recursOn(QDate(2026, 8, 4), QTimeZone::utc()));
        QVERIFY(r->recursOn(QDate(2026, 12, 1), QTimeZone::utc()));
        QVERIFY(!r->recursOn(QDate(2026, 12, 2), QTimeZone::utc()));

        const QJsonObject back = writtenPattern(event);
        QCOMPARE(back.value(QLatin1String("type")).toString(), QStringLiteral("relativeMonthly"));
        QCOMPARE(back.value(QLatin1String("index")).toString(), QStringLiteral("first"));
        QCOMPARE(back.value(QLatin1String("daysOfWeek")).toArray(), weekdays);
    }

    void shouldMapNthWeekdayOfAYear()
    {
        // "The last Thursday of November".
        const QJsonObject pattern = relativePattern(QStringLiteral("relativeYearly"), QStringLiteral("last"), QJsonArray{QStringLiteral("thursday")}, 11);
        const Event::Ptr event = GraphEventHandler::toEvent(
            seriesJson(QStringLiteral("2026-11-26T18:00:00.0000000"), QStringLiteral("2026-11-26T20:00:00.0000000"), pattern, QStringLiteral("UTC")));
        QVERIFY(event);
        const Recurrence *r = event->recurrence();
        QVERIFY(r->recursOn(QDate(2027, 11, 25), QTimeZone::utc()));
        QVERIFY(!r->recursOn(QDate(2027, 11, 26), QTimeZone::utc()));

        const QJsonObject back = writtenPattern(event);
        QCOMPARE(back.value(QLatin1String("type")).toString(), QStringLiteral("relativeYearly"));
        QCOMPARE(back.value(QLatin1String("index")).toString(), QStringLiteral("last"));
        QCOMPARE(back.value(QLatin1String("month")).toInt(), 11);
        QCOMPARE(back.value(QLatin1String("daysOfWeek")).toArray(), QJsonArray{QStringLiteral("thursday")});
    }

    void shouldPutMissingMonthDaysOnTheLastDay()
    {
        // Outlook's "day 31 of every month" falls on the last day of shorter months.
        const QJsonObject pattern = absolutePattern(QStringLiteral("absoluteMonthly"), 31);
        const Event::Ptr event = GraphEventHandler::toEvent(
            seriesJson(QStringLiteral("2026-01-31T08:00:00.0000000"), QStringLiteral("2026-01-31T09:00:00.0000000"), pattern, QStringLiteral("UTC")));
        QVERIFY(event);
        const Recurrence *r = event->recurrence();
        QVERIFY(r->recursOn(QDate(2026, 2, 28), QTimeZone::utc()));
        QVERIFY(r->recursOn(QDate(2026, 3, 31), QTimeZone::utc()));
        QVERIFY(!r->recursOn(QDate(2026, 3, 30), QTimeZone::utc()));
        QVERIFY(r->recursOn(QDate(2026, 4, 30), QTimeZone::utc()));

        const QJsonObject back = writtenPattern(event);
        QCOMPARE(back.value(QLatin1String("type")).toString(), QStringLiteral("absoluteMonthly"));
        QCOMPARE(back.value(QLatin1String("dayOfMonth")).toInt(), 31);
    }

    void shouldReadDay31AsTheLastDayOfTheMonth()
    {
        // Day 31 of every month and 29 February every year are "the last day", a rule
        // KOrganizer's editor can show and keep.
        const Event::Ptr monthly = GraphEventHandler::toEvent(seriesJson(QStringLiteral("2026-01-31T08:00:00.0000000"),
                                                                         QStringLiteral("2026-01-31T09:00:00.0000000"),
                                                                         absolutePattern(QStringLiteral("absoluteMonthly"), 31),
                                                                         QStringLiteral("UTC")));
        QVERIFY(monthly);
        QCOMPARE(monthly->recurrence()->recurrenceType(), static_cast<ushort>(Recurrence::rMonthlyDay));
        QCOMPARE(monthly->recurrence()->monthDays(), QList<int>{-1});

        QJsonObject json = seriesJson(QStringLiteral("2028-02-29T00:00:00.0000000"),
                                      QStringLiteral("2028-03-01T00:00:00.0000000"),
                                      absolutePattern(QStringLiteral("absoluteYearly"), 29, 2));
        json.insert(QStringLiteral("isAllDay"), true);
        const Event::Ptr yearly = GraphEventHandler::toEvent(json);
        QVERIFY(yearly);
        QCOMPARE(yearly->recurrence()->recurrenceType(), static_cast<ushort>(Recurrence::rYearlyMonth));
    }

    void shouldWriteTheLastDayOfTheMonthFromKde()
    {
        // KOrganizer's "monthly on the last day" is BYMONTHDAY=-1; Graph has it as day 31.
        const QDateTime start(QDate(2026, 10, 31), QTime(12, 0), QTimeZone::utc());
        Event::Ptr event(new Event);
        event->setDtStart(start);
        event->setDtEnd(start.addSecs(3600));
        event->recurrence()->setMonthly(1);
        event->recurrence()->addMonthlyDate(-1);
        QJsonObject pattern = writtenPattern(event);
        QCOMPARE(pattern.value(QLatin1String("type")).toString(), QStringLiteral("absoluteMonthly"));
        QCOMPARE(pattern.value(QLatin1String("dayOfMonth")).toInt(), 31);

        // The same yearly, in February: day 29, which Outlook moves to the 28th when needed.
        Event::Ptr feb(new Event);
        feb->setDtStart(QDateTime(QDate(2027, 2, 28), QTime(12, 0), QTimeZone::utc()));
        feb->setDtEnd(feb->dtStart().addSecs(3600));
        feb->recurrence()->setYearly(1);
        feb->recurrence()->addYearlyMonth(2);
        feb->recurrence()->addYearlyDate(-1);
        pattern = writtenPattern(feb);
        QCOMPARE(pattern.value(QLatin1String("type")).toString(), QStringLiteral("absoluteYearly"));
        QCOMPARE(pattern.value(QLatin1String("month")).toInt(), 2);
        QCOMPARE(pattern.value(QLatin1String("dayOfMonth")).toInt(), 29);

        // Other days counted from the end have no Graph form.
        Event::Ptr second(new Event);
        second->setDtStart(start);
        second->setDtEnd(start.addSecs(3600));
        second->recurrence()->setMonthly(1);
        second->recurrence()->addMonthlyDate(-2);
        QVERIFY(!GraphEventHandler::toJson(second).contains(QLatin1String("recurrence")));
    }

    void shouldTakeTheDayOfMonthFromThePattern()
    {
        // The pattern, not the start date, says which day recurs.
        const QJsonObject pattern = absolutePattern(QStringLiteral("absoluteMonthly"), 15);
        const Event::Ptr event = GraphEventHandler::toEvent(
            seriesJson(QStringLiteral("2026-07-15T08:00:00.0000000"), QStringLiteral("2026-07-15T09:00:00.0000000"), pattern, QStringLiteral("UTC")));
        QVERIFY(event);
        QVERIFY(event->recurrence()->recursOn(QDate(2026, 8, 15), QTimeZone::utc()));
        QCOMPARE(writtenPattern(event).value(QLatin1String("dayOfMonth")).toInt(), 15);
    }

    void shouldMapYearlyAllDaySeries()
    {
        // A birthday from Exchange's birthday calendar, exactly as Graph returns it.
        QJsonObject json = seriesJson(QStringLiteral("1974-10-02T00:00:00.0000000"),
                                      QStringLiteral("1974-10-03T00:00:00.0000000"),
                                      absolutePattern(QStringLiteral("absoluteYearly"), 2, 10),
                                      QStringLiteral("Romance Standard Time"));
        json.insert(QStringLiteral("isAllDay"), true);
        json.insert(QStringLiteral("originalStartTimeZone"), QStringLiteral("UTC"));

        const Event::Ptr event = GraphEventHandler::toEvent(json);
        QVERIFY(event);
        QVERIFY(event->allDay());
        QCOMPARE(event->dtStart().date(), QDate(1974, 10, 2));
        QCOMPARE(event->dtEnd().date(), QDate(1974, 10, 2));
        QVERIFY(event->recurrence()->recursOn(QDate(2026, 10, 2), QTimeZone::systemTimeZone()));
        QVERIFY(!event->recurrence()->recursOn(QDate(2026, 10, 3), QTimeZone::systemTimeZone()));

        const QJsonObject back = GraphEventHandler::toJson(event).value(QLatin1String("recurrence")).toObject();
        const QJsonObject backPattern = back.value(QLatin1String("pattern")).toObject();
        QCOMPARE(backPattern.value(QLatin1String("type")).toString(), QStringLiteral("absoluteYearly"));
        QCOMPARE(backPattern.value(QLatin1String("month")).toInt(), 10);
        QCOMPARE(backPattern.value(QLatin1String("dayOfMonth")).toInt(), 2);
        QCOMPARE(back.value(QLatin1String("range")).toObject().value(QLatin1String("startDate")).toString(), QStringLiteral("1974-10-02"));
    }

    void shouldPutFebruary29OnTheLastDayInCommonYears()
    {
        QJsonObject json = seriesJson(QStringLiteral("2028-02-29T00:00:00.0000000"),
                                      QStringLiteral("2028-03-01T00:00:00.0000000"),
                                      absolutePattern(QStringLiteral("absoluteYearly"), 29, 2));
        json.insert(QStringLiteral("isAllDay"), true);
        const Event::Ptr event = GraphEventHandler::toEvent(json);
        QVERIFY(event);
        QVERIFY(event->recurrence()->recursOn(QDate(2029, 2, 28), QTimeZone::systemTimeZone()));
        QVERIFY(event->recurrence()->recursOn(QDate(2032, 2, 29), QTimeZone::systemTimeZone()));
        QVERIFY(!event->recurrence()->recursOn(QDate(2032, 2, 28), QTimeZone::systemTimeZone()));
        QCOMPARE(writtenPattern(event).value(QLatin1String("dayOfMonth")).toInt(), 29);
    }

    void shouldWriteRulesCreatedInKde()
    {
        const auto newEvent = [] {
            const QDateTime start(QDate(2026, 10, 13), QTime(8, 0), QTimeZone::utc()); // a Tuesday
            Event::Ptr event(new Event);
            event->setDtStart(start);
            event->setDtEnd(start.addSecs(3600));
            return event;
        };

        // "Every second Tuesday", as KOrganizer's editor builds it.
        Event::Ptr event = newEvent();
        event->recurrence()->setMonthly(1);
        event->recurrence()->addMonthlyPos(2, 2);
        QJsonObject pattern = writtenPattern(event);
        QCOMPARE(pattern.value(QLatin1String("type")).toString(), QStringLiteral("relativeMonthly"));
        QCOMPARE(pattern.value(QLatin1String("index")).toString(), QStringLiteral("second"));
        QCOMPARE(pattern.value(QLatin1String("daysOfWeek")).toArray(), QJsonArray{QStringLiteral("tuesday")});

        // Plain monthly and yearly rules repeat the start date.
        event = newEvent();
        event->recurrence()->setMonthly(1);
        pattern = writtenPattern(event);
        QCOMPARE(pattern.value(QLatin1String("type")).toString(), QStringLiteral("absoluteMonthly"));
        QCOMPARE(pattern.value(QLatin1String("dayOfMonth")).toInt(), 13);
        event = newEvent();
        event->recurrence()->setYearly(1);
        pattern = writtenPattern(event);
        QCOMPARE(pattern.value(QLatin1String("type")).toString(), QStringLiteral("absoluteYearly"));
        QCOMPARE(pattern.value(QLatin1String("month")).toInt(), 10);
        QCOMPARE(pattern.value(QLatin1String("dayOfMonth")).toInt(), 13);
    }

    void shouldNotWriteRulesWithoutGraphEquivalent()
    {
        const QDateTime start(QDate(2026, 10, 5), QTime(8, 0), QTimeZone::utc());
        Event::Ptr event(new Event);
        event->setDtStart(start);
        event->setDtEnd(start.addSecs(3600));
        // "The first Monday and the last Friday": two positions, no Graph form. Leaving
        // the recurrence out of the update keeps the server's series untouched.
        event->recurrence()->setMonthly(1);
        event->recurrence()->addMonthlyPos(1, 1);
        event->recurrence()->addMonthlyPos(-1, 5);
        QVERIFY(event->recurs());
        QVERIFY(!GraphEventHandler::toJson(event).contains(QLatin1String("recurrence")));
    }

    void shouldKeepASeriesItCouldNotRead()
    {
        // A pattern this mapping does not know arrives as a single event. Editing it in
        // KDE must not send "recurrence": null, which would end the series on the server.
        QJsonObject pattern;
        pattern.insert(QStringLiteral("type"), QStringLiteral("everyFullMoon"));
        pattern.insert(QStringLiteral("interval"), 1);
        const Event::Ptr event =
            GraphEventHandler::toEvent(seriesJson(QStringLiteral("2026-10-05T08:00:00.0000000"), QStringLiteral("2026-10-05T09:00:00.0000000"), pattern));
        QVERIFY(event);
        QVERIFY(!event->recurs());
        event->setSummary(QStringLiteral("Renamed in KDE"));
        QVERIFY(!GraphEventHandler::toJson(event).contains(QLatin1String("recurrence")));

        // The mark survives the iCalendar round trip through the Akonadi cache.
        KCalendarCore::ICalFormat format;
        const auto cached = format.fromString(format.toICalString(event)).dynamicCast<Event>();
        QVERIFY(cached);
        QVERIFY(!GraphEventHandler::toJson(cached).contains(QLatin1String("recurrence")));
    }

    void shouldKeepSeriesTheEditorCannotRepresent()
    {
        // KOrganizer's editor has no form for BYSETPOS rules and drops them whenever an
        // event is saved, even when only the title changed. That must not end the
        // series on the server.
        const QJsonArray weekdays{QStringLiteral("monday"),
                                  QStringLiteral("tuesday"),
                                  QStringLiteral("wednesday"),
                                  QStringLiteral("thursday"),
                                  QStringLiteral("friday")};
        const QList<QJsonObject> patterns{relativePattern(QStringLiteral("relativeMonthly"), QStringLiteral("first"), weekdays),
                                          absolutePattern(QStringLiteral("absoluteMonthly"), 30)};
        for (const QJsonObject &pattern : patterns) {
            const Event::Ptr event = GraphEventHandler::toEvent(
                seriesJson(QStringLiteral("2026-01-31T08:00:00.0000000"), QStringLiteral("2026-01-31T09:00:00.0000000"), pattern, QStringLiteral("UTC")));
            QVERIFY(event);
            QCOMPARE(event->recurrence()->recurrenceType(), static_cast<ushort>(Recurrence::rOther));
            const QJsonObject before = GraphEventHandler::toJson(event).value(QLatin1String("recurrence")).toObject();
            event->recurrence()->unsetRecurs(); // what the editor's save does
            event->setSummary(QStringLiteral("Renamed in KOrganizer"));
            // The server's rule goes out unchanged (also right for a create elsewhere).
            QCOMPARE(GraphEventHandler::toJson(event).value(QLatin1String("recurrence")).toObject(), before);
        }
    }

    void shouldKeepRulesTheEditorOnlyRewrote()
    {
        // KOrganizer's editor rebuilds every rule from the start date when saving, even
        // when only the title changed. Each case: the server's rule, the editor's
        // rewrite of it; the server's rule must be what goes out.
        struct Case {
            const char *name = nullptr;
            QString start;
            QJsonObject pattern;
            std::function<void(Recurrence *)> editor;
        };
        QJsonObject biweekly{{QStringLiteral("type"), QStringLiteral("weekly")},
                             {QStringLiteral("interval"), 2},
                             {QStringLiteral("firstDayOfWeek"), QStringLiteral("sunday")},
                             {QStringLiteral("daysOfWeek"), QJsonArray{QStringLiteral("sunday"), QStringLiteral("tuesday")}}};
        QJsonObject tuesdays{{QStringLiteral("type"), QStringLiteral("weekly")},
                             {QStringLiteral("interval"), 1},
                             {QStringLiteral("firstDayOfWeek"), QStringLiteral("monday")},
                             {QStringLiteral("daysOfWeek"), QJsonArray{QStringLiteral("tuesday")}}};
        const QList<Case> cases{
            // Week start becomes Monday: shifts every other Tuesday by a week.
            {"week start",
             QStringLiteral("2026-10-04T08:00:00.0000000"),
             biweekly,
             [](Recurrence *r) {
                 QBitArray days(7);
                 days.setBit(1);
                 days.setBit(6);
                 r->setWeekly(2, days);
             }},
            // A start off the pattern: the editor adds the start day (a Monday).
            {"start day",
             QStringLiteral("2026-10-05T08:00:00.0000000"),
             tuesdays,
             [](Recurrence *r) {
                 QBitArray days(7);
                 days.setBit(0);
                 days.setBit(1);
                 r->setWeekly(1, days);
             }},
            // ...and takes the day of month from the start.
            {"day of month",
             QStringLiteral("2026-10-10T08:00:00.0000000"),
             absolutePattern(QStringLiteral("absoluteMonthly"), 15),
             [](Recurrence *r) {
                 r->setMonthly(1);
                 r->addMonthlyDate(10);
             }},
        };
        for (const Case &c : cases) {
            const Event::Ptr event = GraphEventHandler::toEvent(seriesJson(c.start, c.start, c.pattern, QStringLiteral("UTC")));
            QVERIFY2(event, c.name);
            const QJsonObject server = GraphEventHandler::toJson(event).value(QLatin1String("recurrence")).toObject();
            event->recurrence()->unsetRecurs();
            c.editor(event->recurrence());
            event->setSummary(QStringLiteral("Renamed in KOrganizer"));
            QVERIFY2(event->recurs(), c.name);
            QCOMPARE(GraphEventHandler::toJson(event).value(QLatin1String("recurrence")).toObject(), server);
        }
    }

    void shouldKeepTheWeekStartWhenTheTimeChanges()
    {
        // Moving a fortnightly Sunday/Tuesday series from 10:00 to 11:00 in KOrganizer:
        // the editor rewrites the week start to Monday, which would shift every other
        // Tuesday by a week for everyone.
        const QJsonObject pattern{{QStringLiteral("type"), QStringLiteral("weekly")},
                                  {QStringLiteral("interval"), 2},
                                  {QStringLiteral("firstDayOfWeek"), QStringLiteral("sunday")},
                                  {QStringLiteral("daysOfWeek"), QJsonArray{QStringLiteral("sunday"), QStringLiteral("tuesday")}}};
        const Event::Ptr event = GraphEventHandler::toEvent(
            seriesJson(QStringLiteral("2026-10-04T10:00:00.0000000"), QStringLiteral("2026-10-04T11:00:00.0000000"), pattern, QStringLiteral("UTC")));
        QVERIFY(event);
        event->setDtStart(event->dtStart().addSecs(3600));
        event->setDtEnd(event->dtEnd().addSecs(3600));
        event->recurrence()->unsetRecurs();
        QBitArray days(7);
        days.setBit(1);
        days.setBit(6);
        event->recurrence()->setWeekly(2, days);
        QCOMPARE(writtenPattern(event).value(QLatin1String("firstDayOfWeek")).toString(), QStringLiteral("sunday"));

        // Even with a deliberate change, the week start stays the server's.
        event->recurrence()->unsetRecurs();
        days.setBit(3);
        event->recurrence()->setWeekly(2, days);
        const QJsonObject changed = writtenPattern(event);
        QCOMPARE(changed.value(QLatin1String("daysOfWeek")).toArray().size(), 3);
        QCOMPARE(changed.value(QLatin1String("firstDayOfWeek")).toString(), QStringLiteral("sunday"));
    }

    void shouldWriteADeliberateSwitchBetweenLastAndFourth()
    {
        // 26 October 2026 is both the last and the fourth Monday; the editor shows the
        // server's "last", so choosing "fourth" is the user's decision.
        const QJsonObject last = relativePattern(QStringLiteral("relativeMonthly"), QStringLiteral("last"), QJsonArray{QStringLiteral("monday")});
        const Event::Ptr event = GraphEventHandler::toEvent(
            seriesJson(QStringLiteral("2026-10-26T08:00:00.0000000"), QStringLiteral("2026-10-26T09:00:00.0000000"), last, QStringLiteral("UTC")));
        QVERIFY(event);
        event->recurrence()->unsetRecurs();
        event->recurrence()->setMonthly(1);
        event->recurrence()->addMonthlyPos(4, 1);
        QCOMPARE(writtenPattern(event).value(QLatin1String("index")).toString(), QStringLiteral("fourth"));

        // Likewise "the last day" -> "day 30" on 30 November.
        const Event::Ptr lastDay = GraphEventHandler::toEvent(seriesJson(QStringLiteral("2026-11-30T08:00:00.0000000"),
                                                                         QStringLiteral("2026-11-30T09:00:00.0000000"),
                                                                         absolutePattern(QStringLiteral("absoluteMonthly"), 31),
                                                                         QStringLiteral("UTC")));
        QVERIFY(lastDay);
        lastDay->recurrence()->unsetRecurs();
        lastDay->recurrence()->setMonthly(1);
        lastDay->recurrence()->addMonthlyDate(30);
        QCOMPARE(writtenPattern(lastDay).value(QLatin1String("dayOfMonth")).toInt(), 30);
    }

    void shouldWriteADeliberateChangeWithoutTheEditorsAdditions()
    {
        // Tuesday and Thursday, the series starting on a Monday that is no occurrence.
        // The user changes Thursday to Friday; the editor adds the start day on top.
        const QJsonObject pattern{{QStringLiteral("type"), QStringLiteral("weekly")},
                                  {QStringLiteral("interval"), 1},
                                  {QStringLiteral("firstDayOfWeek"), QStringLiteral("sunday")},
                                  {QStringLiteral("daysOfWeek"), QJsonArray{QStringLiteral("tuesday"), QStringLiteral("thursday")}}};
        const Event::Ptr event = GraphEventHandler::toEvent(
            seriesJson(QStringLiteral("2026-10-05T08:00:00.0000000"), QStringLiteral("2026-10-05T09:00:00.0000000"), pattern, QStringLiteral("UTC")));
        QVERIFY(event);
        event->recurrence()->unsetRecurs();
        QBitArray days(7);
        days.setBit(0); // Monday, the start day
        days.setBit(1);
        days.setBit(4);
        event->recurrence()->setWeekly(1, days);
        QCOMPARE(writtenPattern(event).value(QLatin1String("daysOfWeek")).toArray(), (QJsonArray{QStringLiteral("tuesday"), QStringLiteral("friday")}));
    }

    void shouldWriteAMovedSeriesStart()
    {
        // Same rule, a week later: the new start date must reach the server.
        const QJsonObject pattern{{QStringLiteral("type"), QStringLiteral("weekly")},
                                  {QStringLiteral("interval"), 1},
                                  {QStringLiteral("firstDayOfWeek"), QStringLiteral("monday")},
                                  {QStringLiteral("daysOfWeek"), QJsonArray{QStringLiteral("monday")}}};
        const Event::Ptr event = GraphEventHandler::toEvent(
            seriesJson(QStringLiteral("2026-10-05T08:00:00.0000000"), QStringLiteral("2026-10-05T09:00:00.0000000"), pattern, QStringLiteral("UTC")));
        QVERIFY(event);
        event->setDtStart(event->dtStart().addDays(7));
        event->setDtEnd(event->dtEnd().addDays(7));
        const QJsonObject range = GraphEventHandler::toJson(event).value(QLatin1String("recurrence")).toObject().value(QLatin1String("range")).toObject();
        QCOMPARE(range.value(QLatin1String("startDate")).toString(), QStringLiteral("2026-10-12"));
    }

    void shouldWriteRulesTheUserChanged()
    {
        QJsonObject weekly{{QStringLiteral("type"), QStringLiteral("weekly")},
                           {QStringLiteral("interval"), 1},
                           {QStringLiteral("firstDayOfWeek"), QStringLiteral("sunday")},
                           {QStringLiteral("daysOfWeek"), QJsonArray{QStringLiteral("monday")}}};
        // Every other week instead of every week.
        Event::Ptr event = GraphEventHandler::toEvent(
            seriesJson(QStringLiteral("2026-10-05T08:00:00.0000000"), QStringLiteral("2026-10-05T09:00:00.0000000"), weekly, QStringLiteral("UTC")));
        QVERIFY(event);
        QBitArray monday(7);
        monday.setBit(0);
        event->recurrence()->unsetRecurs(); // the editor starts from scratch
        event->recurrence()->setWeekly(2, monday);
        QJsonObject pattern = writtenPattern(event);
        QCOMPARE(pattern.value(QLatin1String("interval")).toInt(), 2);
        // The editor cannot set a week start, so the server's stays.
        QCOMPARE(pattern.value(QLatin1String("firstDayOfWeek")).toString(), QStringLiteral("sunday"));

        // Another weekday.
        event = GraphEventHandler::toEvent(
            seriesJson(QStringLiteral("2026-10-05T08:00:00.0000000"), QStringLiteral("2026-10-05T09:00:00.0000000"), weekly, QStringLiteral("UTC")));
        QBitArray mondayFriday(7);
        mondayFriday.setBit(0);
        mondayFriday.setBit(4);
        event->recurrence()->unsetRecurs();
        event->recurrence()->setWeekly(1, mondayFriday);
        QCOMPARE(writtenPattern(event).value(QLatin1String("daysOfWeek")).toArray(), (QJsonArray{QStringLiteral("monday"), QStringLiteral("friday")}));

        // A new start: the editor's day of month is then the user's choice.
        event = GraphEventHandler::toEvent(seriesJson(QStringLiteral("2026-10-15T08:00:00.0000000"),
                                                      QStringLiteral("2026-10-15T09:00:00.0000000"),
                                                      absolutePattern(QStringLiteral("absoluteMonthly"), 15),
                                                      QStringLiteral("UTC")));
        event->setDtStart(QDateTime(QDate(2026, 10, 20), QTime(8, 0), QTimeZone::utc()));
        event->setDtEnd(event->dtStart().addSecs(3600));
        event->recurrence()->unsetRecurs();
        event->recurrence()->setMonthly(1);
        event->recurrence()->addMonthlyDate(20);
        QCOMPARE(writtenPattern(event).value(QLatin1String("dayOfMonth")).toInt(), 20);

        // Ending the series after five times.
        event = GraphEventHandler::toEvent(
            seriesJson(QStringLiteral("2026-10-05T08:00:00.0000000"), QStringLiteral("2026-10-05T09:00:00.0000000"), weekly, QStringLiteral("UTC")));
        event->recurrence()->setDuration(5);
        const QJsonObject range = GraphEventHandler::toJson(event).value(QLatin1String("recurrence")).toObject().value(QLatin1String("range")).toObject();
        QCOMPARE(range.value(QLatin1String("numberOfOccurrences")).toInt(), 5);
    }

    void shouldEndASeriesTheUserMadeSingle()
    {
        // A rule the editor shows can only disappear by choosing "no recurrence".
        const QJsonObject pattern = relativePattern(QStringLiteral("relativeMonthly"), QStringLiteral("first"), QJsonArray{QStringLiteral("monday")});
        const Event::Ptr event = GraphEventHandler::toEvent(
            seriesJson(QStringLiteral("2026-10-05T08:00:00.0000000"), QStringLiteral("2026-10-05T09:00:00.0000000"), pattern, QStringLiteral("UTC")));
        QVERIFY(event);
        event->recurrence()->unsetRecurs();
        const QJsonObject json = GraphEventHandler::toJson(event);
        QVERIFY(json.contains(QLatin1String("recurrence")));
        QVERIFY(json.value(QLatin1String("recurrence")).isNull());
    }

    void shouldClearTheRecurrenceOfASingleEvent()
    {
        const QDateTime start(QDate(2026, 10, 5), QTime(8, 0), QTimeZone::utc());
        Event::Ptr event(new Event);
        event->setDtStart(start);
        event->setDtEnd(start.addSecs(3600));
        const QJsonObject json = GraphEventHandler::toJson(event);
        QVERIFY(json.contains(QLatin1String("recurrence")));
        QVERIFY(json.value(QLatin1String("recurrence")).isNull());
    }
};

QTEST_GUILESS_MAIN(GraphEventHandlerTest)

#include "grapheventhandlertest.moc"
