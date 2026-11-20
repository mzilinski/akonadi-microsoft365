/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Maps between Graph `event` resources and KCalendarCore::Event. Read (toEvent) covers
    the common fields plus recurrences; write (toJson) mirrors them back.
    Graph hands out start/end as UTC instants. Timed events are moved into the zone
    they were created in (recurrenceTimeZone / originalStartTimeZone), because a
    recurrence is expanded in the zone of its start: anchored to UTC, a series would
    drift by an hour at every daylight-saving change.
*/

#pragma once

#include <KCalendarCore/Event>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTimeZone>

namespace GraphEventHandler
{
/// Akonadi item content mime type for calendar events.
[[nodiscard]] QString mimeType();

/// Graph event JSON -> KCalendarCore::Event (returns null on missing id). A series
/// master's cancelledOccurrences become EXDATEs.
[[nodiscard]] KCalendarCore::Event::Ptr toEvent(const QJsonObject &json);

/// One changed occurrence of a series (Graph type "exception") -> an event with the
/// series' uid and the occurrence's original start as RECURRENCE-ID, the way KDE
/// keeps exceptions. Null without originalStart or master.
[[nodiscard]] KCalendarCore::Event::Ptr toException(const QJsonObject &json, const KCalendarCore::Event::Ptr &master);

/// Dates of occurrences excluded in KDE (EXDATE) that the server had not cancelled
/// when the series was read: those to cancel there.
[[nodiscard]] QStringList newlyCancelledDates(const KCalendarCore::Event::Ptr &master);

/// KCalendarCore::Event -> Graph event JSON with every mapped field (for creating
/// an event, also anew in another calendar).
[[nodiscard]] QJsonObject toJson(const KCalendarCore::Event::Ptr &event);

/// Like toJson(), for a PATCH of an existing event: leaves out what must not be sent
/// back unchanged (the body, while the description is still the one read).
[[nodiscard]] QJsonObject toJsonForUpdate(const KCalendarCore::Event::Ptr &event);

/// Apply a Graph patternedRecurrence to an incidence (shared with the todo handler).
/// The incidence's dtStart must already be set.
void applyRecurrence(const QJsonObject &recurrence, const KCalendarCore::Incidence::Ptr &incidence);

/// Incidence recurrence -> Graph patternedRecurrence (empty if not recurring or the
/// rule has no Graph equivalent). Shared with the todo handler.
[[nodiscard]] QJsonObject recurrenceToJson(const KCalendarCore::Incidence::Ptr &incidence);

/// Graph time-zone name (Windows name such as "W. Europe Standard Time", IANA id or
/// "UTC") -> QTimeZone; invalid if the name is empty or unknown.
[[nodiscard]] QTimeZone timeZoneFromGraph(const QString &name);

/// Graph dateTimeTimeZone -> QDateTime in the zone it names. A missing zone means
/// UTC; an unknown one is logged and treated as UTC.
[[nodiscard]] QDateTime parseDateTimeTimeZone(const QJsonObject &dtz);

/// QDateTime -> Graph dateTimeTimeZone: the wall-clock time in its own zone, named
/// the Windows way; UTC for zones Graph has no name for. Floating times are taken
/// in the system zone.
[[nodiscard]] QJsonObject toDateTimeTimeZone(const QDateTime &dt);
}
