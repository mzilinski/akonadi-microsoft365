/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "graphtodohandler.h"

#include "calendar/grapheventhandler.h"

#include <KCalendarCore/Alarm>

#include <QJsonArray>
#include <QTimeZone>

using namespace KCalendarCore;

namespace
{
// Marks the due date read from the server (X-KDE-GRAPH-SERVER-DUE), see toJson().
constexpr QByteArrayView kCustomApp("GRAPH");
constexpr QByteArrayView kServerDue("SERVER-DUE");
// The server's lastModifiedDateTime as read (X-KDE-GRAPH-SERVER-MODIFIED): tells whether
// a sync has delivered the task since a given moment.
constexpr QByteArrayView kServerModified("SERVER-MODIFIED");

// To Do keeps start and due as dates: it drops the time and takes the date in the zone
// sent along. Its own clients write the user's local midnight, which Graph hands out
// in UTC (the 5th in Berlin as 22:00 on the 4th). Taken back to the local zone that is
// midnight again — rounded to the nearest one, in case the client was in a zone up
// to twelve hours away. A named zone carries the date itself.
QDate taskDate(const QJsonObject &dtz)
{
    const QDateTime dt = GraphEventHandler::parseDateTimeTimeZone(dtz);
    if (!dt.isValid()) {
        return {};
    }
    const QString tz = dtz.value(QLatin1String("timeZone")).toString();
    if (GraphEventHandler::timeZoneFromGraph(tz.isEmpty() ? QStringLiteral("UTC") : tz) == QTimeZone::utc()) {
        return dt.toTimeZone(QTimeZone::systemTimeZone()).addSecs(12 * 60 * 60).date();
    }
    return dt.date();
}

// The date the user sees, as midnight in their zone: the form To Do's clients write.
QJsonObject taskDateToJson(const QDateTime &dt, bool allDay)
{
    const QDate date = allDay ? dt.date() : dt.toTimeZone(QTimeZone::systemTimeZone()).date();
    return GraphEventHandler::toDateTimeTimeZone(QDateTime(date, QTime(0, 0)));
}

QJsonObject utcTaskDateTime(const QDateTime &dt)
{
    QJsonObject o;
    o.insert(QStringLiteral("dateTime"), dt.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.0000000")));
    o.insert(QStringLiteral("timeZone"), QStringLiteral("UTC"));
    return o;
}
} // namespace

namespace GraphTodoHandler
{
QString mimeType()
{
    return QStringLiteral("application/x-vnd.akonadi.calendar.todo");
}

KCalendarCore::Todo::Ptr toTodo(const QJsonObject &json)
{
    const QString id = json.value(QLatin1String("id")).toString();
    if (id.isEmpty()) {
        return {};
    }
    auto todo = Todo::Ptr(new Todo);
    todo->setUid(id);
    todo->setSummary(json.value(QLatin1String("title")).toString());

    const QJsonObject body = json.value(QLatin1String("body")).toObject();
    if (body.value(QLatin1String("contentType")).toString() == QLatin1String("text")) {
        todo->setDescription(body.value(QLatin1String("content")).toString());
    }

    const QDate start = taskDate(json.value(QLatin1String("startDateTime")).toObject());
    if (start.isValid()) {
        todo->setDtStart(QDateTime(start, QTime(0, 0)));
    }
    const QDate due = taskDate(json.value(QLatin1String("dueDateTime")).toObject());
    if (due.isValid()) {
        todo->setDtDue(QDateTime(due, QTime(0, 0)), true);
        todo->setCustomProperty(kCustomApp.toByteArray(), kServerDue.toByteArray(), due.toString(Qt::ISODate));
    }
    const QString modified = json.value(QLatin1String("lastModifiedDateTime")).toString();
    if (!modified.isEmpty()) {
        todo->setCustomProperty(kCustomApp.toByteArray(), kServerModified.toByteArray(), modified);
    }
    todo->setAllDay(start.isValid() || due.isValid());

    const QString status = json.value(QLatin1String("status")).toString();
    if (status == QLatin1String("completed")) {
        const QDateTime completed = GraphEventHandler::parseDateTimeTimeZone(json.value(QLatin1String("completedDateTime")).toObject());
        if (completed.isValid()) {
            todo->setCompleted(completed);
        } else {
            todo->setCompleted(true);
        }
    } else if (status == QLatin1String("inProgress")) {
        todo->setStatus(Incidence::StatusInProcess);
    }

    const QString importance = json.value(QLatin1String("importance")).toString();
    if (importance == QLatin1String("high")) {
        todo->setPriority(1);
    } else if (importance == QLatin1String("low")) {
        todo->setPriority(9);
    }

    if (json.value(QLatin1String("isReminderOn")).toBool()) {
        const QDateTime reminder = GraphEventHandler::parseDateTimeTimeZone(json.value(QLatin1String("reminderDateTime")).toObject());
        if (reminder.isValid()) {
            Alarm::Ptr alarm = todo->newAlarm();
            alarm->setDisplayAlarm(todo->summary());
            alarm->setTime(reminder);
            alarm->setEnabled(true);
        }
    }

    QStringList categories;
    const QJsonArray cats = json.value(QLatin1String("categories")).toArray();
    categories.reserve(cats.count());
    for (const auto &c : cats) {
        categories << c.toString();
    }
    todo->setCategories(categories);

    const QJsonObject recurrence = json.value(QLatin1String("recurrence")).toObject();
    if (!recurrence.isEmpty()) {
        GraphEventHandler::applyRecurrence(recurrence, todo);
    }
    return todo;
}

QJsonObject toJson(const KCalendarCore::Todo::Ptr &todo)
{
    QJsonObject json;
    json.insert(QStringLiteral("title"), todo->summary());

    // Only plain-text bodies are mapped (mirroring toTodo). Omit the body when there
    // is no local description so a server-side HTML body is not clobbered by an
    // empty text one.
    if (!todo->description().isEmpty()) {
        QJsonObject body;
        body.insert(QStringLiteral("contentType"), QStringLiteral("text"));
        body.insert(QStringLiteral("content"), todo->description());
        json.insert(QStringLiteral("body"), body);
    }

    if (todo->dtStart().isValid()) {
        json.insert(QStringLiteral("startDateTime"), taskDateToJson(todo->dtStart(), todo->allDay()));
    }
    if (todo->dtDue().isValid()) {
        const QJsonObject due = taskDateToJson(todo->dtDue(), todo->allDay());
        // To Do moves the due date of a recurring task on by one occurrence whenever
        // it is written, whatever the value; leave an unchanged one out.
        const QString serverDue = todo->customProperty(kCustomApp.toByteArray(), kServerDue.toByteArray());
        const bool unchanged = todo->recurs() && !serverDue.isEmpty() && due.value(QLatin1String("dateTime")).toString().startsWith(serverDue);
        if (!unchanged) {
            json.insert(QStringLiteral("dueDateTime"), due);
        }
    }

    if (todo->isCompleted()) {
        json.insert(QStringLiteral("status"), QStringLiteral("completed"));
    } else if (todo->status() == Incidence::StatusInProcess || todo->percentComplete() > 0) {
        json.insert(QStringLiteral("status"), QStringLiteral("inProgress"));
    } else {
        json.insert(QStringLiteral("status"), QStringLiteral("notStarted"));
    }

    const int priority = todo->priority();
    if (priority >= 1 && priority <= 3) {
        json.insert(QStringLiteral("importance"), QStringLiteral("high"));
    } else if (priority >= 7) {
        json.insert(QStringLiteral("importance"), QStringLiteral("low"));
    } else {
        json.insert(QStringLiteral("importance"), QStringLiteral("normal"));
    }

    QDateTime reminder;
    const auto alarms = todo->alarms();
    for (const Alarm::Ptr &alarm : alarms) {
        if (alarm->enabled() && alarm->hasTime()) {
            reminder = alarm->time();
            break;
        }
    }
    if (reminder.isValid()) {
        json.insert(QStringLiteral("isReminderOn"), true);
        json.insert(QStringLiteral("reminderDateTime"), utcTaskDateTime(reminder));
    } else {
        json.insert(QStringLiteral("isReminderOn"), false);
    }

    if (!todo->categories().isEmpty()) {
        json.insert(QStringLiteral("categories"), QJsonArray::fromStringList(todo->categories()));
    }

    const QJsonObject recurrence = GraphEventHandler::recurrenceToJson(todo);
    if (!recurrence.isEmpty()) {
        json.insert(QStringLiteral("recurrence"), recurrence);
    }
    return json;
}

QString serverDue(const KCalendarCore::Todo::Ptr &todo)
{
    return todo->customProperty(kCustomApp.toByteArray(), kServerDue.toByteArray());
}

void setServerDue(const KCalendarCore::Todo::Ptr &todo, const QString &date)
{
    // An empty value would be ignored rather than clear the property.
    if (date.isEmpty()) {
        todo->removeCustomProperty(kCustomApp.toByteArray(), kServerDue.toByteArray());
    } else {
        todo->setCustomProperty(kCustomApp.toByteArray(), kServerDue.toByteArray(), date);
    }
}

QString serverModified(const KCalendarCore::Todo::Ptr &todo)
{
    return todo->customProperty(kCustomApp.toByteArray(), kServerModified.toByteArray());
}

QJsonObject toJsonForCreate(const KCalendarCore::Todo::Ptr &todo)
{
    // A new task (also one moved or copied to another list) gets its due date: the
    // server-side one read with the payload belongs to the original.
    const Todo::Ptr copy(todo->clone());
    setServerDue(copy, QString());
    return toJson(copy);
}
}
