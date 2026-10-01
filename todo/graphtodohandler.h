/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Maps between Graph `todoTask` resources (Microsoft To Do) and KCalendarCore::Todo.
    Task times come as a naive local timestamp plus a separate timeZone field, which
    may be a Windows name (the To Do API ignores the Prefer: outlook.timezone header);
    parsing resolves the zone with GraphEventHandler::parseDateTimeTimeZone().
*/

#pragma once

#include <KCalendarCore/Todo>
#include <QJsonObject>
#include <QString>

namespace GraphTodoHandler
{
/// Akonadi item content mime type for todos.
[[nodiscard]] QString mimeType();

/// Graph todoTask JSON -> KCalendarCore::Todo (returns null on missing id).
[[nodiscard]] KCalendarCore::Todo::Ptr toTodo(const QJsonObject &json);

/// KCalendarCore::Todo -> Graph todoTask JSON for an update.
[[nodiscard]] QJsonObject toJson(const KCalendarCore::Todo::Ptr &todo);
/// KCalendarCore::Todo -> Graph todoTask JSON for a create, which always carries the
/// due date (see serverDue()).
[[nodiscard]] QJsonObject toJsonForCreate(const KCalendarCore::Todo::Ptr &todo);

/// The due date (yyyy-MM-dd) the server had when the task was read. toJson() leaves an
/// unchanged due date of a recurring task out, as To Do moves the task on whenever
/// one is written; after writing a new one, setServerDue() keeps that check right
/// until the next sync brings the task back.
[[nodiscard]] QString serverDue(const KCalendarCore::Todo::Ptr &todo);
void setServerDue(const KCalendarCore::Todo::Ptr &todo, const QString &date);
/// The server's lastModifiedDateTime as read: changes whenever a sync delivers the task.
[[nodiscard]] QString serverModified(const KCalendarCore::Todo::Ptr &todo);
}
