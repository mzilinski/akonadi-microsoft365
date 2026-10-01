/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Unit tests for the Graph todoTask JSON <-> KCalendarCore::Todo mapping.
*/

#include "todo/graphtodohandler.h"

#include <KCalendarCore/Alarm>
#include <KCalendarCore/Recurrence>

#include <QJsonArray>
#include <QJsonObject>
#include <QTest>
#include <QTimeZone>

using namespace KCalendarCore;

namespace
{
QJsonObject taskDateTime(const QString &dateTime, const QString &timeZone = QStringLiteral("UTC"))
{
    QJsonObject o;
    o.insert(QStringLiteral("dateTime"), dateTime);
    o.insert(QStringLiteral("timeZone"), timeZone);
    return o;
}
// How Graph hands out a To Do date written as midnight by a client whose zone is this
// system's shifted by @p shiftHours: in UTC. Keeps the tests independent of where they run.
QString localMidnightUtc(const QDate &date, int shiftHours = 0)
{
    return QDateTime(date, QTime(0, 0), QTimeZone::systemTimeZone())
        .toUTC()
        .addSecs(-shiftHours * 3600)
        .toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.0000000"));
}
} // namespace

class GraphTodoHandlerTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void shouldReturnNullWithoutId()
    {
        QJsonObject json;
        json.insert(QStringLiteral("title"), QStringLiteral("No id"));
        QVERIFY(!GraphTodoHandler::toTodo(json));
    }

    void shouldMapBasicFields()
    {
        QJsonObject json;
        json.insert(QStringLiteral("id"), QStringLiteral("AAMkTask1"));
        json.insert(QStringLiteral("title"), QStringLiteral("Steuererklärung"));
        QJsonObject body;
        body.insert(QStringLiteral("contentType"), QStringLiteral("text"));
        body.insert(QStringLiteral("content"), QStringLiteral("Belege äöü sammeln"));
        json.insert(QStringLiteral("body"), body);
        // Graph uses 7 fractional digits; the parser must cope with them.
        json.insert(QStringLiteral("startDateTime"), taskDateTime(localMidnightUtc(QDate(2026, 7, 10))));
        json.insert(QStringLiteral("dueDateTime"), taskDateTime(localMidnightUtc(QDate(2026, 8, 1))));
        json.insert(QStringLiteral("importance"), QStringLiteral("high"));
        json.insert(QStringLiteral("status"), QStringLiteral("inProgress"));
        json.insert(QStringLiteral("categories"), QJsonArray{QStringLiteral("Privat")});

        const Todo::Ptr todo = GraphTodoHandler::toTodo(json);
        QVERIFY(todo);
        QCOMPARE(todo->uid(), QStringLiteral("AAMkTask1"));
        QCOMPARE(todo->summary(), QStringLiteral("Steuererklärung"));
        QCOMPARE(todo->description(), QStringLiteral("Belege äöü sammeln"));
        // Start and due are dates in To Do, handed out as local midnight in UTC.
        QVERIFY(todo->allDay());
        QCOMPARE(todo->dtStart().date(), QDate(2026, 7, 10));
        QCOMPARE(todo->dtDue().date(), QDate(2026, 8, 1));
        QCOMPARE(todo->priority(), 1);
        QCOMPARE(todo->status(), Incidence::StatusInProcess);
        QVERIFY(!todo->isCompleted());
        QCOMPARE(todo->categories(), QStringList{QStringLiteral("Privat")});
    }

    void shouldResolveIanaTimeZones()
    {
        QJsonObject json;
        json.insert(QStringLiteral("id"), QStringLiteral("AAMkTask2"));
        json.insert(QStringLiteral("dueDateTime"), taskDateTime(QStringLiteral("2026-07-10T12:00:00.0000000"), QStringLiteral("Europe/Berlin")));
        const Todo::Ptr todo = GraphTodoHandler::toTodo(json);
        QVERIFY(todo);
        // A named zone carries the date itself.
        QCOMPARE(todo->dtDue().date(), QDate(2026, 7, 10));
    }

    void shouldResolveWindowsTimeZones()
    {
        // Outlook-created reminders carry a Windows zone name, which QTimeZone does
        // not know; read as UTC the reminder would ring two hours late.
        QJsonObject json;
        json.insert(QStringLiteral("id"), QStringLiteral("AAMkTask5"));
        json.insert(QStringLiteral("title"), QStringLiteral("Anrufen"));
        json.insert(QStringLiteral("isReminderOn"), true);
        json.insert(QStringLiteral("reminderDateTime"), taskDateTime(QStringLiteral("2026-10-05T09:00:00.0000000"), QStringLiteral("W. Europe Standard Time")));
        json.insert(QStringLiteral("dueDateTime"), taskDateTime(QStringLiteral("2026-10-06T00:00:00.0000000"), QStringLiteral("W. Europe Standard Time")));
        const Todo::Ptr todo = GraphTodoHandler::toTodo(json);
        QVERIFY(todo);
        QCOMPARE(todo->alarms().constFirst()->time(), QDateTime(QDate(2026, 10, 5), QTime(7, 0), QTimeZone::utc()));
        QCOMPARE(todo->dtDue().date(), QDate(2026, 10, 6));

        // Written back, the due date stays the 6th, at midnight of the local zone.
        const QJsonObject back = GraphTodoHandler::toJson(todo);
        QCOMPARE(back.value(QLatin1String("dueDateTime")).toObject().value(QLatin1String("dateTime")).toString(),
                 QStringLiteral("2026-10-06T00:00:00.0000000"));
        // The reminder is a moment, written in UTC.
        QCOMPARE(back.value(QLatin1String("reminderDateTime")).toObject().value(QLatin1String("dateTime")).toString(),
                 QStringLiteral("2026-10-05T07:00:00.0000000"));
        QCOMPARE(back.value(QLatin1String("reminderDateTime")).toObject().value(QLatin1String("timeZone")).toString(), QStringLiteral("UTC"));
    }

    void shouldReadDueDatesWrittenAtMidnight_data()
    {
        QTest::addColumn<QDate>("date");
        QTest::addColumn<int>("shift");
        // To Do's own clients write midnight in the user's zone; Graph returns it in UTC.
        // A client in another zone (up to eleven hours away) still gives the same date.
        // Summer and winter: UTC+13 in New Zealand's summer, and the like.
        for (const QDate &date : {QDate(2026, 12, 5), QDate(2026, 7, 5)}) {
            for (int shift : {0, 2, -5, 11, -11}) {
                QTest::addRow("%s %+d h", qPrintable(date.toString(Qt::ISODate)), shift) << date << shift;
            }
        }
    }

    void shouldReadDueDatesWrittenAtMidnight()
    {
        QFETCH(QDate, date);
        QFETCH(int, shift);
        QJsonObject json;
        json.insert(QStringLiteral("id"), QStringLiteral("AAMkTask7"));
        json.insert(QStringLiteral("dueDateTime"), taskDateTime(localMidnightUtc(date, shift)));
        const Todo::Ptr todo = GraphTodoHandler::toTodo(json);
        QVERIFY(todo);
        QCOMPARE(todo->dtDue().date(), date);
    }

    void shouldLeaveOutADueDateWrittenBefore()
    {
        // After writing a new due date of a recurring task, a second edit must not send
        // it again (To Do would move the task on by one occurrence).
        QJsonObject json;
        json.insert(QStringLiteral("id"), QStringLiteral("AAMkTask10"));
        json.insert(QStringLiteral("dueDateTime"), taskDateTime(QStringLiteral("2026-10-19T00:00:00.0000000"), QStringLiteral("W. Europe Standard Time")));
        QJsonObject pattern{{QStringLiteral("type"), QStringLiteral("weekly")},
                            {QStringLiteral("interval"), 1},
                            {QStringLiteral("daysOfWeek"), QJsonArray{QStringLiteral("monday")}}};
        json.insert(QStringLiteral("recurrence"), QJsonObject{{QStringLiteral("pattern"), pattern}});
        const Todo::Ptr todo = GraphTodoHandler::toTodo(json);
        QVERIFY(todo);
        QCOMPARE(GraphTodoHandler::serverDue(todo), QStringLiteral("2026-10-19"));
        todo->setDtDue(QDateTime(QDate(2026, 10, 21), QTime(0, 0)), true);
        QVERIFY(GraphTodoHandler::toJson(todo).contains(QLatin1String("dueDateTime")));
        GraphTodoHandler::setServerDue(todo, QStringLiteral("2026-10-21"));
        QVERIFY(!GraphTodoHandler::toJson(todo).contains(QLatin1String("dueDateTime")));
    }

    void shouldNotRewriteTheDueDateOfARecurringTask()
    {
        // To Do moves a recurring task's due date on by one occurrence whenever it is
        // written; an unchanged one stays out of the update.
        QJsonObject json;
        json.insert(QStringLiteral("id"), QStringLiteral("AAMkTask8"));
        json.insert(QStringLiteral("dueDateTime"), taskDateTime(localMidnightUtc(QDate(2026, 10, 5))));
        QJsonObject pattern{{QStringLiteral("type"), QStringLiteral("weekly")},
                            {QStringLiteral("interval"), 1},
                            {QStringLiteral("daysOfWeek"), QJsonArray{QStringLiteral("monday")}}};
        json.insert(QStringLiteral("recurrence"), QJsonObject{{QStringLiteral("pattern"), pattern}});
        const Todo::Ptr todo = GraphTodoHandler::toTodo(json);
        QVERIFY(todo);
        QVERIFY(todo->recurs());
        QCOMPARE(todo->dtDue().date(), QDate(2026, 10, 5));
        todo->setSummary(QStringLiteral("Renamed"));
        QVERIFY(!GraphTodoHandler::toJson(todo).contains(QLatin1String("dueDateTime")));

        // A due date changed in KDE is written.
        todo->setDtDue(QDateTime(QDate(2026, 10, 12), QTime(0, 0)), true);
        QCOMPARE(GraphTodoHandler::toJson(todo).value(QLatin1String("dueDateTime")).toObject().value(QLatin1String("dateTime")).toString(),
                 QStringLiteral("2026-10-12T00:00:00.0000000"));
    }

    void shouldSendTheDueDateWhenCreatingAMovedTask()
    {
        // Moving or copying a recurring task creates it anew: its due date must go along,
        // even though it is the one read from the server.
        QJsonObject json;
        json.insert(QStringLiteral("id"), QStringLiteral("AAMkTask11"));
        json.insert(QStringLiteral("lastModifiedDateTime"), QStringLiteral("2026-10-01T10:00:00.0000000Z"));
        json.insert(QStringLiteral("dueDateTime"), taskDateTime(QStringLiteral("2026-10-19T00:00:00.0000000"), QStringLiteral("W. Europe Standard Time")));
        QJsonObject pattern{{QStringLiteral("type"), QStringLiteral("weekly")},
                            {QStringLiteral("interval"), 1},
                            {QStringLiteral("daysOfWeek"), QJsonArray{QStringLiteral("monday")}}};
        json.insert(QStringLiteral("recurrence"), QJsonObject{{QStringLiteral("pattern"), pattern}});
        const Todo::Ptr todo = GraphTodoHandler::toTodo(json);
        QVERIFY(todo);
        QCOMPARE(GraphTodoHandler::serverModified(todo), QStringLiteral("2026-10-01T10:00:00.0000000Z"));
        QVERIFY(!GraphTodoHandler::toJson(todo).contains(QLatin1String("dueDateTime")));
        QVERIFY(GraphTodoHandler::toJsonForCreate(todo).contains(QLatin1String("dueDateTime")));
        // ...without touching the original.
        QCOMPARE(GraphTodoHandler::serverDue(todo), QStringLiteral("2026-10-19"));
    }

    void shouldWriteADueDateFromKdeAtLocalMidnight()
    {
        // A due date set in KOrganizer is a floating date: written in the system zone,
        // not converted to the evening before in UTC.
        Todo::Ptr todo(new Todo);
        todo->setSummary(QStringLiteral("Steuer"));
        todo->setDtDue(QDateTime(QDate(2026, 10, 5), QTime(0, 0)), true);
        todo->setAllDay(true);
        const QJsonObject due = GraphTodoHandler::toJson(todo).value(QLatin1String("dueDateTime")).toObject();
        QCOMPARE(due.value(QLatin1String("dateTime")).toString(), QStringLiteral("2026-10-05T00:00:00.0000000"));
    }

    void shouldMapCompletion()
    {
        QJsonObject json;
        json.insert(QStringLiteral("id"), QStringLiteral("AAMkTask3"));
        json.insert(QStringLiteral("status"), QStringLiteral("completed"));
        json.insert(QStringLiteral("completedDateTime"), taskDateTime(QStringLiteral("2026-07-01T15:30:00.0000000")));
        const Todo::Ptr todo = GraphTodoHandler::toTodo(json);
        QVERIFY(todo);
        QVERIFY(todo->isCompleted());
        QCOMPARE(todo->completed(), QDateTime(QDate(2026, 7, 1), QTime(15, 30), QTimeZone::utc()));
    }

    void shouldMapReminderToAlarm()
    {
        QJsonObject json;
        json.insert(QStringLiteral("id"), QStringLiteral("AAMkTask4"));
        json.insert(QStringLiteral("title"), QStringLiteral("Anrufen"));
        json.insert(QStringLiteral("isReminderOn"), true);
        json.insert(QStringLiteral("reminderDateTime"), taskDateTime(QStringLiteral("2026-07-09T09:00:00.0000000")));
        const Todo::Ptr todo = GraphTodoHandler::toTodo(json);
        QVERIFY(todo);
        QCOMPARE(todo->alarms().size(), 1);
        QCOMPARE(todo->alarms().constFirst()->time(), QDateTime(QDate(2026, 7, 9), QTime(9, 0), QTimeZone::utc()));
    }

    void shouldMapDailyRecurrence()
    {
        QJsonObject json;
        json.insert(QStringLiteral("id"), QStringLiteral("AAMkTask5"));
        json.insert(QStringLiteral("dueDateTime"), taskDateTime(QStringLiteral("2026-07-10T08:00:00.0000000")));
        QJsonObject pattern;
        pattern.insert(QStringLiteral("type"), QStringLiteral("daily"));
        pattern.insert(QStringLiteral("interval"), 2);
        QJsonObject recurrence;
        recurrence.insert(QStringLiteral("pattern"), pattern);
        json.insert(QStringLiteral("recurrence"), recurrence);

        const Todo::Ptr todo = GraphTodoHandler::toTodo(json);
        QVERIFY(todo);
        QVERIFY(todo->recurs());
        QCOMPARE(todo->recurrence()->recurrenceType(), static_cast<ushort>(Recurrence::rDaily));
        QCOMPARE(todo->recurrence()->frequency(), 2);
    }

    void shouldWriteJson()
    {
        Todo::Ptr todo(new Todo);
        todo->setSummary(QStringLiteral("Review"));
        todo->setDescription(QStringLiteral("Kapitel 3"));
        todo->setDtDue(QDateTime(QDate(2026, 7, 31), QTime(17, 30))); // a time To Do drops
        todo->setPriority(9);
        todo->setCategories(QStringList{QStringLiteral("Arbeit")});
        Alarm::Ptr alarm = todo->newAlarm();
        alarm->setDisplayAlarm(todo->summary());
        alarm->setTime(QDateTime(QDate(2026, 7, 30), QTime(9, 0), QTimeZone::utc()));
        alarm->setEnabled(true);

        const QJsonObject json = GraphTodoHandler::toJson(todo);
        QCOMPARE(json.value(QLatin1String("title")).toString(), QStringLiteral("Review"));
        QCOMPARE(json.value(QLatin1String("body")).toObject().value(QLatin1String("content")).toString(), QStringLiteral("Kapitel 3"));
        QCOMPARE(json.value(QLatin1String("dueDateTime")).toObject().value(QLatin1String("dateTime")).toString(),
                 QStringLiteral("2026-07-31T00:00:00.0000000"));
        QCOMPARE(json.value(QLatin1String("importance")).toString(), QStringLiteral("low"));
        QCOMPARE(json.value(QLatin1String("status")).toString(), QStringLiteral("notStarted"));
        QCOMPARE(json.value(QLatin1String("isReminderOn")).toBool(), true);
        QCOMPARE(json.value(QLatin1String("reminderDateTime")).toObject().value(QLatin1String("dateTime")).toString(),
                 QStringLiteral("2026-07-30T09:00:00.0000000"));
        QCOMPARE(json.value(QLatin1String("categories")).toArray(), QJsonArray{QStringLiteral("Arbeit")});
    }

    void shouldWriteCompletedStatus()
    {
        Todo::Ptr todo(new Todo);
        todo->setSummary(QStringLiteral("Done"));
        todo->setCompleted(QDateTime(QDate(2026, 7, 1), QTime(12, 0), QTimeZone::utc()));
        const QJsonObject json = GraphTodoHandler::toJson(todo);
        QCOMPARE(json.value(QLatin1String("status")).toString(), QStringLiteral("completed"));
    }

    void shouldWriteDailyRecurrenceWithEndDate()
    {
        Todo::Ptr todo(new Todo);
        todo->setSummary(QStringLiteral("Gießen"));
        todo->setDtStart(QDateTime(QDate(2026, 7, 10), QTime(8, 0), QTimeZone::utc()));
        todo->recurrence()->setDaily(1);
        todo->recurrence()->setEndDate(QDate(2026, 8, 31));

        const QJsonObject recurrence = GraphTodoHandler::toJson(todo).value(QLatin1String("recurrence")).toObject();
        QVERIFY(!recurrence.isEmpty());
        QCOMPARE(recurrence.value(QLatin1String("pattern")).toObject().value(QLatin1String("type")).toString(), QStringLiteral("daily"));
        const QJsonObject range = recurrence.value(QLatin1String("range")).toObject();
        QCOMPARE(range.value(QLatin1String("type")).toString(), QStringLiteral("endDate"));
        QCOMPARE(range.value(QLatin1String("endDate")).toString(), QStringLiteral("2026-08-31"));
        QCOMPARE(range.value(QLatin1String("startDate")).toString(), QStringLiteral("2026-07-10"));
    }

    void shouldRoundTripThroughJson()
    {
        QJsonObject json;
        json.insert(QStringLiteral("id"), QStringLiteral("AAMkTask6"));
        json.insert(QStringLiteral("title"), QStringLiteral("Round trip"));
        QJsonObject body;
        body.insert(QStringLiteral("contentType"), QStringLiteral("text"));
        body.insert(QStringLiteral("content"), QStringLiteral("Body"));
        json.insert(QStringLiteral("body"), body);
        json.insert(QStringLiteral("dueDateTime"), taskDateTime(localMidnightUtc(QDate(2026, 8, 1))));

        QJsonObject back = GraphTodoHandler::toJson(GraphTodoHandler::toTodo(json));
        QCOMPARE(back.value(QLatin1String("title")), json.value(QLatin1String("title")));
        QCOMPARE(back.value(QLatin1String("body")), json.value(QLatin1String("body")));
        // The due date survives the round trip as a date (local midnight, 1 August).
        back.insert(QStringLiteral("id"), QStringLiteral("AAMkTask6"));
        QCOMPARE(GraphTodoHandler::toTodo(back)->dtDue().date(), QDate(2026, 8, 1));
    }
};

QTEST_GUILESS_MAIN(GraphTodoHandlerTest)

#include "graphtodohandlertest.moc"
