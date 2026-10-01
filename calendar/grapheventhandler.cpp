/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "grapheventhandler.h"

#include "graph_debug.h"

#include <KCalendarCore/Attendee>
#include <KCalendarCore/Person>
#include <KCalendarCore/Recurrence>
#include <KCalendarCore/RecurrenceRule>

#include <QBitArray>
#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>

using namespace KCalendarCore;

namespace
{
const QString kGraphDateTimeFormat = QStringLiteral("yyyy-MM-ddTHH:mm:ss.0000000");

// Marks a series whose recurrence KDE may lose without the user asking for it
// (X-KDE-GRAPH-KEEP-SERVER-RECURRENCE): one that could not be read, or one that
// KOrganizer's editor cannot represent (BYSETPOS) and drops on every save. toJson()
// then never sends "recurrence": null for it; the next delta restores the series.
constexpr QByteArrayView kCustomApp("GRAPH");
constexpr QByteArrayView kKeepServerRecurrence("KEEP-SERVER-RECURRENCE");
// The server's rule as read (X-KDE-GRAPH-SERVER-RECURRENCE), in the form toJson()
// writes it; its range carries the start date it was read with. KOrganizer's editor
// rewrites parts of a rule from the start date on every save (week start, start day,
// day of month, position); those parts go back as the server has them.
constexpr QByteArrayView kServerRecurrence("SERVER-RECURRENCE");

// Graph day names, Monday first like KCalendarCore's day bits (Monday = bit 0 = day 1).
constexpr const char *kDayNames[] = {"monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"};

// Graph's patternedRecurrence index values; KCalendarCore positions 1..4 and -1.
constexpr const char *kIndexNames[] = {"first", "second", "third", "fourth"};

// 1 = Monday .. 7 = Sunday, 0 if unknown.
[[nodiscard]] int dayFromGraph(const QString &name)
{
    for (int i = 0; i < 7; ++i) {
        if (name == QLatin1String(kDayNames[i])) {
            return i + 1;
        }
    }
    return 0;
}

[[nodiscard]] QString dayToGraph(int day)
{
    return QLatin1String(kDayNames[(day - 1) % 7]);
}

// 1..4 or -1 ("last"), 0 if unknown.
[[nodiscard]] int positionFromGraph(const QString &index)
{
    if (index == QLatin1String("last")) {
        return -1;
    }
    for (int i = 0; i < 4; ++i) {
        if (index == QLatin1String(kIndexNames[i])) {
            return i + 1;
        }
    }
    return 0;
}

[[nodiscard]] QString positionToGraph(int pos)
{
    if (pos == -1) {
        return QStringLiteral("last");
    }
    if (pos >= 1 && pos <= 4) {
        return QLatin1String(kIndexNames[pos - 1]);
    }
    return {};
}

// Windows name of an IANA zone. Ids that are older than CLDR's table ("Europe/Kiev",
// "US/Eastern", "Asia/Calcutta") have none: take the name of the zone they are an
// alias of, else of a zone with the same rules for the next years.
[[nodiscard]] QByteArray windowsIdFor(const QTimeZone &zone)
{
    static QHash<QByteArray, QByteArray> cache;
    const QByteArray id = zone.id();
    const auto cached = cache.constFind(id);
    if (cached != cache.constEnd()) {
        return cached.value();
    }
    QByteArray windowsId = QTimeZone::ianaIdToWindowsId(id);
    if (windowsId.isEmpty()) {
        const QList<QByteArray> all = QTimeZone::availableTimeZoneIds();
        for (const QByteArray &candidate : all) {
            const QByteArray candidateWindowsId = QTimeZone::ianaIdToWindowsId(candidate);
            if (!candidateWindowsId.isEmpty() && candidate != id && QTimeZone(candidate).hasAlternativeName(id)) {
                windowsId = candidateWindowsId;
                break;
            }
        }
    }
    if (windowsId.isEmpty()) {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const auto rules = [&now](const QTimeZone &z) {
            QList<std::pair<qint64, int>> list;
            const auto transitions = z.transitions(now, now.addYears(3));
            list.reserve(transitions.size());
            for (const auto &t : transitions) {
                list.append({t.atUtc.toSecsSinceEpoch(), t.offsetFromUtc});
            }
            return list;
        };
        const auto own = rules(zone);
        const QList<QByteArray> candidates = QTimeZone::availableTimeZoneIds(zone.standardTimeOffset(now));
        for (const QByteArray &candidate : candidates) {
            const QByteArray candidateWindowsId = QTimeZone::ianaIdToWindowsId(candidate);
            const QTimeZone candidateZone(candidate);
            if (!candidateWindowsId.isEmpty() && candidateZone.offsetFromUtc(now) == zone.offsetFromUtc(now) && rules(candidateZone) == own) {
                windowsId = candidateWindowsId;
                break;
            }
        }
    }
    cache.insert(id, windowsId);
    return windowsId;
}

// Zone name Graph accepts for a zone: "UTC" or a Windows name, which every Exchange
// version understands. Empty if the zone has no Windows equivalent.
[[nodiscard]] QString timeZoneToGraph(const QTimeZone &zone)
{
    if (zone.timeSpec() == Qt::UTC || zone.id() == "UTC") {
        return QStringLiteral("UTC");
    }
    return QString::fromLatin1(windowsIdFor(zone));
}

// The zone a timed incidence is written in: its own when Graph has a name for it (so
// a series keeps its wall-clock time across daylight-saving changes), else UTC.
[[nodiscard]] QTimeZone writableZone(const QDateTime &dt)
{
    QTimeZone zone = dt.timeRepresentation();
    if (zone.timeSpec() == Qt::LocalTime) {
        zone = QTimeZone::systemTimeZone();
    }
    if (zone.timeSpec() == Qt::TimeZone && !timeZoneToGraph(zone).isEmpty()) {
        return zone;
    }
    return QTimeZone::utc();
}

// All-day events carry a bare date: midnight, written as UTC like Outlook does.
[[nodiscard]] QJsonObject toAllDayDateTime(const QDate &date)
{
    QJsonObject o;
    o.insert(QStringLiteral("dateTime"), QString(date.toString(Qt::ISODate) + QLatin1String("T00:00:00.0000000")));
    o.insert(QStringLiteral("timeZone"), QStringLiteral("UTC"));
    return o;
}

// The zone a timed event lives in. Graph returns UTC instants; a series is expanded in
// its recurrenceTimeZone, a single event belongs to the zone it was created in.
[[nodiscard]] QTimeZone eventZone(const QJsonObject &json, const QDateTime &instant)
{
    const QJsonObject recurrence = json.value(QLatin1String("recurrence")).toObject();
    const bool recurs = !recurrence.isEmpty();
    if (recurs) {
        const QTimeZone zone =
            GraphEventHandler::timeZoneFromGraph(recurrence.value(QLatin1String("range")).toObject().value(QLatin1String("recurrenceTimeZone")).toString());
        if (zone.isValid()) {
            return zone;
        }
    }
    const QTimeZone zone = GraphEventHandler::timeZoneFromGraph(json.value(QLatin1String("originalStartTimeZone")).toString());
    if (zone.isValid()) {
        return zone;
    }
    // Custom zones ("tzone://Microsoft/Custom") have no id. A series anchored to UTC
    // would drift at daylight-saving changes, so the local zone is the better guess.
    return recurs ? QTimeZone::systemTimeZone() : instant.timeRepresentation();
}

// Shortest and longest length of a month (1..12), or of any month for 0.
[[nodiscard]] int shortestMonth(int month)
{
    return month > 0 ? QDate(2001, month, 1).daysInMonth() : 28;
}

[[nodiscard]] int longestMonth(int month)
{
    return month > 0 ? QDate(2000, month, 1).daysInMonth() : 31;
}

// A day of the month as Outlook applies it: days that a month lacks fall on its last
// day ("day 31" is the 30th in April). A day no month exceeds is "the last day"
// (BYMONTHDAY=-1), which KOrganizer's editor can show; other days past the shortest
// month are the last of the days from its length up to the given day (BYSETPOS=-1).
void addMonthDay(Recurrence *r, int day, int month)
{
    const bool yearly = month > 0;
    const auto add = [r, yearly](int d) {
        if (yearly) {
            r->addYearlyDate(d);
        } else {
            r->addMonthlyDate(static_cast<short>(d));
        }
    };
    if (day <= shortestMonth(month)) {
        add(day);
        return;
    }
    if (day >= longestMonth(month)) {
        add(-1);
        return;
    }
    for (int d = shortestMonth(month); d <= day; ++d) {
        add(d);
    }
    r->defaultRRule()->setBySetPos({-1});
}

// Inverse of addMonthDay(): the Graph dayOfMonth of a rule's BYMONTHDAY/BYSETPOS,
// or 0 if the combination cannot be expressed.
[[nodiscard]] int monthDayFromRule(const RecurrenceRule *rule, int month, const QDate &start)
{
    const QList<int> &days = rule->byMonthDays();
    const QList<int> &setPos = rule->bySetPos();
    if (days.isEmpty()) {
        return setPos.isEmpty() ? start.day() : 0;
    }
    if (setPos.isEmpty()) {
        if (days.size() != 1) {
            return 0;
        }
        const int day = days.constFirst();
        if (day == -1) {
            // "The last day": Outlook puts the longest month's last day on every
            // shorter month's last day.
            return longestMonth(month);
        }
        return day > 0 ? day : 0;
    }
    // The "last of shortest..day" shape written by addMonthDay().
    const int shortest = shortestMonth(month);
    if (setPos != QList<int>{-1} || days.constFirst() != shortest) {
        return 0;
    }
    for (int i = 1; i < days.size(); ++i) {
        if (days.at(i) != shortest + i) {
            return 0;
        }
    }
    return days.constLast();
}

// "The <index> <days> of the month": a single day becomes a positioned day
// (BYDAY=1MO); several days are a set to pick from (BYDAY=MO,TU;BYSETPOS=1), which is
// what Graph means by e.g. "first weekday".
void addRelativeDays(Recurrence *r, int pos, const QBitArray &days, bool yearly)
{
    const bool single = days.count(true) == 1;
    if (yearly) {
        r->addYearlyPos(static_cast<short>(single ? pos : 0), days);
    } else {
        r->addMonthlyPos(static_cast<short>(single ? pos : 0), days);
    }
    if (!single) {
        r->defaultRRule()->setBySetPos({pos});
    }
}

// Inverse of addRelativeDays(): fills index/daysOfWeek, false if not expressible.
[[nodiscard]] bool relativeDaysFromRule(const RecurrenceRule *rule, QJsonObject &pattern)
{
    const QList<RecurrenceRule::WDayPos> &byDays = rule->byDays();
    const QList<int> &setPos = rule->bySetPos();
    int pos = 0;
    QBitArray days(7);
    if (setPos.isEmpty()) {
        // Only one positioned day: "first Monday and first Tuesday" has no Graph form.
        if (byDays.size() != 1) {
            return false;
        }
        pos = byDays.constFirst().pos();
        days.setBit(byDays.constFirst().day() - 1);
    } else {
        if (setPos.size() != 1) {
            return false;
        }
        pos = setPos.constFirst();
        for (const auto &d : byDays) {
            if (d.pos() != 0) {
                return false;
            }
            days.setBit(d.day() - 1);
        }
    }
    const QString index = positionToGraph(pos);
    if (index.isEmpty()) {
        return false;
    }
    QJsonArray daysOfWeek;
    for (int i = 0; i < 7; ++i) {
        if (days.testBit(i)) {
            daysOfWeek.append(dayToGraph(i + 1));
        }
    }
    pattern.insert(QStringLiteral("index"), index);
    pattern.insert(QStringLiteral("daysOfWeek"), daysOfWeek);
    return true;
}

// RecurrenceRule -> Graph recurrencePattern; empty if the rule has no Graph equivalent.
[[nodiscard]] QJsonObject patternFromRule(const RecurrenceRule *rule, const QDate &start)
{
    if (!rule->bySeconds().isEmpty() || !rule->byMinutes().isEmpty() || !rule->byHours().isEmpty() || !rule->byYearDays().isEmpty()
        || !rule->byWeekNumbers().isEmpty()) {
        return {};
    }
    const QList<RecurrenceRule::WDayPos> &byDays = rule->byDays();
    const QList<int> &months = rule->byMonths();

    QJsonObject pattern;
    pattern.insert(QStringLiteral("interval"), qMax(1, static_cast<int>(rule->frequency())));
    switch (rule->recurrenceType()) {
    case RecurrenceRule::rDaily:
        if (!byDays.isEmpty() || !rule->byMonthDays().isEmpty() || !months.isEmpty() || !rule->bySetPos().isEmpty()) {
            return {};
        }
        pattern.insert(QStringLiteral("type"), QStringLiteral("daily"));
        return pattern;
    case RecurrenceRule::rWeekly: {
        if (!rule->byMonthDays().isEmpty() || !months.isEmpty() || !rule->bySetPos().isEmpty()) {
            return {};
        }
        QBitArray days(7);
        for (const auto &d : byDays) {
            if (d.pos() != 0) {
                return {};
            }
            days.setBit(d.day() - 1);
        }
        if (days.count(true) == 0) {
            // Graph requires at least one day; a plain weekly rule repeats the start day.
            days.setBit(start.dayOfWeek() - 1);
        }
        QJsonArray daysOfWeek;
        for (int i = 0; i < 7; ++i) {
            if (days.testBit(i)) {
                daysOfWeek.append(dayToGraph(i + 1));
            }
        }
        pattern.insert(QStringLiteral("type"), QStringLiteral("weekly"));
        pattern.insert(QStringLiteral("daysOfWeek"), daysOfWeek);
        pattern.insert(QStringLiteral("firstDayOfWeek"), dayToGraph(rule->weekStart()));
        return pattern;
    }
    case RecurrenceRule::rMonthly:
    case RecurrenceRule::rYearly: {
        const bool yearly = rule->recurrenceType() == RecurrenceRule::rYearly;
        int month = 0;
        if (yearly) {
            if (months.size() > 1) {
                return {};
            }
            month = months.isEmpty() ? start.month() : months.constFirst();
            pattern.insert(QStringLiteral("month"), month);
        } else if (!months.isEmpty()) {
            return {};
        }
        if (byDays.isEmpty()) {
            const int day = monthDayFromRule(rule, month, start);
            if (day <= 0) {
                return {};
            }
            pattern.insert(QStringLiteral("type"), yearly ? QStringLiteral("absoluteYearly") : QStringLiteral("absoluteMonthly"));
            pattern.insert(QStringLiteral("dayOfMonth"), day);
            return pattern;
        }
        if (!rule->byMonthDays().isEmpty() || !relativeDaysFromRule(rule, pattern)) {
            return {};
        }
        pattern.insert(QStringLiteral("type"), yearly ? QStringLiteral("relativeYearly") : QStringLiteral("relativeMonthly"));
        return pattern;
    }
    default:
        return {};
    }
}

// The server's rule stored by applyRecurrence(), if any.
[[nodiscard]] QJsonObject storedServerRecurrence(const Incidence::Ptr &incidence)
{
    const QString stored = incidence->customProperty(kCustomApp.toByteArray(), kServerRecurrence.toByteArray());
    if (stored.isEmpty()) {
        return {};
    }
    return QJsonDocument::fromJson(stored.toUtf8()).object().value(QLatin1String("recurrence")).toObject();
}

// "first".."fourth" from the week of the month a date lies in, and "last" when it is
// in the last seven days — the positions KOrganizer's editor can derive from a start.
[[nodiscard]] bool isPositionOf(const QString &index, const QDate &date)
{
    return index == positionToGraph((date.day() - 1) / 7 + 1) || (index == QLatin1String("last") && date.day() + 7 > date.daysInMonth());
}

// The local rule with what KOrganizer's editor substituted put back as the server has
// it. The editor rebuilds a rule from the start date on every save: it checks the start
// day among the weekdays and takes the day of month, the month and the position from
// the start (and writes Monday as week start; recurrenceToJson() keeps the server's).
// A part differs deliberately when the editor could have shown the server's value, or
// when the local value is not what the editor derives from the start. Another type or
// start date is a new rule altogether.
[[nodiscard]] QJsonObject undoEditorRewrites(const QJsonObject &server, const QJsonObject &local, const QDate &start)
{
    const QJsonObject serverPattern = server.value(QLatin1String("pattern")).toObject();
    QJsonObject localPattern = local.value(QLatin1String("pattern")).toObject();
    const QString type = serverPattern.value(QLatin1String("type")).toString();
    if (localPattern.value(QLatin1String("type")).toString() != type
        || local.value(QLatin1String("range")).toObject().value(QLatin1String("startDate"))
            != server.value(QLatin1String("range")).toObject().value(QLatin1String("startDate"))) {
        return local;
    }
    const QString startDay = dayToGraph(start.dayOfWeek());
    const int month = serverPattern.value(QLatin1String("month")).toInt();
    QStringList keys = serverPattern.keys() + localPattern.keys();
    keys.removeDuplicates();
    for (const QString &key : std::as_const(keys)) {
        const QJsonValue serverValue = serverPattern.value(key);
        const QJsonValue localValue = localPattern.value(key);
        if (serverValue == localValue || serverValue.isUndefined()) {
            continue;
        }
        if (key == QLatin1String("daysOfWeek") && type == QLatin1String("weekly")) {
            // The editor always keeps the start day checked; drop it again when the
            // server's rule did not have it, whatever else the user changed.
            const QJsonArray serverDays = serverValue.toArray();
            if (serverDays.contains(startDay)) {
                continue;
            }
            QJsonArray days;
            for (const auto &day : localValue.toArray()) {
                if (day.toString() != startDay) {
                    days.append(day);
                }
            }
            if (!days.isEmpty()) {
                localPattern.insert(key, days);
            }
            continue;
        }
        bool editorCouldShowServer = true;
        bool derivedFromStart = false;
        if (key == QLatin1String("daysOfWeek")) {
            editorCouldShowServer = serverValue.toArray() == QJsonArray{startDay};
            derivedFromStart = localValue.toArray() == QJsonArray{startDay};
        } else if (key == QLatin1String("dayOfMonth")) {
            // "The last day" is day 31 (or the month's length) in Graph.
            const int serverDay = serverValue.toInt();
            editorCouldShowServer = serverDay == start.day() || (serverDay == longestMonth(month) && start.day() == start.daysInMonth());
            derivedFromStart = localValue.toInt() == start.day();
        } else if (key == QLatin1String("month")) {
            editorCouldShowServer = serverValue.toInt() == start.month();
            derivedFromStart = localValue.toInt() == start.month();
        } else if (key == QLatin1String("index")) {
            editorCouldShowServer = isPositionOf(serverValue.toString(), start);
            derivedFromStart = isPositionOf(localValue.toString(), start);
        }
        if (!editorCouldShowServer && derivedFromStart) {
            localPattern.insert(key, serverValue);
        }
    }
    QJsonObject merged = local;
    merged.insert(QStringLiteral("pattern"), localPattern);
    return merged;
}

} // namespace

namespace GraphEventHandler
{
QTimeZone timeZoneFromGraph(const QString &name)
{
    if (name.isEmpty()) {
        return {};
    }
    if (name == QLatin1String("UTC") || name == QLatin1String("tzone://Microsoft/Utc")) {
        return QTimeZone::utc();
    }
    // Graph mixes IANA ids ("Europe/Berlin") and Windows names ("W. Europe Standard
    // Time"), even within one event; QTimeZone only knows the former directly.
    const QByteArray id = name.toUtf8();
    if (QTimeZone::isTimeZoneIdAvailable(id)) {
        return QTimeZone(id);
    }
    const QByteArray iana = QTimeZone::windowsIdToDefaultIanaId(id);
    if (!iana.isEmpty()) {
        return QTimeZone(iana);
    }
    return {};
}

QDateTime parseDateTimeTimeZone(const QJsonObject &dtz)
{
    QString raw = dtz.value(QLatin1String("dateTime")).toString();
    if (raw.isEmpty()) {
        return {};
    }
    // Graph uses 7 fractional digits; QDateTime::fromString(Qt::ISODateWithMs) wants <=3.
    const int dot = raw.indexOf(QLatin1Char('.'));
    if (dot >= 0) {
        raw = raw.left(dot);
    }
    QDateTime dt = QDateTime::fromString(raw, QStringLiteral("yyyy-MM-ddTHH:mm:ss"));
    if (!dt.isValid()) {
        return {};
    }
    // The timestamp is the wall-clock time in the named zone.
    const QString tz = dtz.value(QLatin1String("timeZone")).toString();
    QTimeZone zone = tz.isEmpty() ? QTimeZone::utc() : timeZoneFromGraph(tz);
    if (!zone.isValid()) {
        qCWarning(GRAPH_LOG) << "unknown time zone" << tz << "- reading" << raw << "as UTC";
        zone = QTimeZone::utc();
    }
    dt.setTimeZone(zone);
    return dt;
}

QJsonObject toDateTimeTimeZone(const QDateTime &dt)
{
    const QTimeZone zone = writableZone(dt);
    QJsonObject o;
    o.insert(QStringLiteral("dateTime"), dt.toTimeZone(zone).toString(kGraphDateTimeFormat));
    o.insert(QStringLiteral("timeZone"), timeZoneToGraph(zone));
    return o;
}

void applyRecurrence(const QJsonObject &recurrence, const Incidence::Ptr &incidence)
{
    const QJsonObject pattern = recurrence.value(QLatin1String("pattern")).toObject();
    const QJsonObject range = recurrence.value(QLatin1String("range")).toObject();
    if (pattern.isEmpty()) {
        return;
    }
    const QString type = pattern.value(QLatin1String("type")).toString();
    const int interval = qMax(1, pattern.value(QLatin1String("interval")).toInt(1));
    Recurrence *r = incidence->recurrence();
    const QDate start = incidence->dateTime(Incidence::RoleRecurrenceStart).date();

    QBitArray days(7);
    const QJsonArray daysOfWeek = pattern.value(QLatin1String("daysOfWeek")).toArray();
    for (const auto &d : daysOfWeek) {
        const int day = dayFromGraph(d.toString());
        if (day > 0) {
            days.setBit(day - 1);
        }
    }
    // Graph sends 0 for fields that do not apply to the pattern type.
    const int dayOfMonth = pattern.value(QLatin1String("dayOfMonth")).toInt();
    const int month = pattern.value(QLatin1String("month")).toInt();

    if (type == QLatin1String("daily")) {
        r->setDaily(interval);
    } else if (type == QLatin1String("weekly")) {
        if (days.count(true) == 0 && start.isValid()) {
            days.setBit(start.dayOfWeek() - 1);
        }
        const int weekStart = dayFromGraph(pattern.value(QLatin1String("firstDayOfWeek")).toString());
        r->setWeekly(interval, days, weekStart > 0 ? weekStart : 7); // Graph's default is Sunday
    } else if (type == QLatin1String("absoluteMonthly")) {
        r->setMonthly(interval);
        addMonthDay(r, dayOfMonth > 0 ? dayOfMonth : start.day(), 0);
    } else if (type == QLatin1String("absoluteYearly")) {
        r->setYearly(interval);
        const int m = month > 0 ? month : start.month();
        r->addYearlyMonth(static_cast<short>(m));
        addMonthDay(r, dayOfMonth > 0 ? dayOfMonth : start.day(), m);
    } else if (type == QLatin1String("relativeMonthly") || type == QLatin1String("relativeYearly")) {
        const int pos = positionFromGraph(pattern.value(QLatin1String("index")).toString());
        const int index = pos != 0 ? pos : 1; // Graph's default is "first"
        if (days.count(true) == 0 && start.isValid()) {
            days.setBit(start.dayOfWeek() - 1);
        }
        if (type == QLatin1String("relativeMonthly")) {
            r->setMonthly(interval);
            addRelativeDays(r, index, days, false);
        } else {
            r->setYearly(interval);
            r->addYearlyMonth(static_cast<short>(month > 0 ? month : start.month()));
            addRelativeDays(r, index, days, true);
        }
    } else {
        qCWarning(GRAPH_LOG) << "unsupported recurrence pattern type" << type;
        return; // treat as single occurrence
    }

    const QString rangeType = range.value(QLatin1String("type")).toString();
    if (rangeType == QLatin1String("numbered")) {
        r->setDuration(range.value(QLatin1String("numberOfOccurrences")).toInt());
    } else if (rangeType == QLatin1String("endDate")) {
        const QDate end = QDate::fromString(range.value(QLatin1String("endDate")).toString(), Qt::ISODate);
        if (end.isValid()) {
            r->setEndDate(end);
        }
    }

    const QJsonObject asWritten = recurrenceToJson(incidence);
    if (!asWritten.isEmpty()) {
        const QJsonObject stored{{QStringLiteral("recurrence"), asWritten}};
        incidence->setCustomProperty(kCustomApp.toByteArray(),
                                     kServerRecurrence.toByteArray(),
                                     QString::fromUtf8(QJsonDocument(stored).toJson(QJsonDocument::Compact)));
    }
}

QJsonObject recurrenceToJson(const Incidence::Ptr &incidence)
{
    if (!incidence->recurs()) {
        return {};
    }
    const Recurrence *r = incidence->recurrence();
    if (r->rRules().size() != 1 || !r->exRules().isEmpty()) {
        qCWarning(GRAPH_LOG) << "recurrence of" << incidence->uid() << "has several rules; Graph supports one, not writing it";
        return {};
    }
    // Dates of the range are in the recurrence's zone, the one the start is written in.
    const bool allDay = r->allDay();
    const QDateTime anchor = incidence->dateTime(Incidence::RoleRecurrenceStart);
    const QTimeZone zone = allDay ? QTimeZone::utc() : writableZone(anchor);
    const QDate startDate = allDay ? anchor.date() : anchor.toTimeZone(zone).date();

    const QJsonObject server = storedServerRecurrence(incidence);
    QJsonObject pattern = patternFromRule(r->defaultRRuleConst(), startDate);
    if (pattern.isEmpty()) {
        // Without a rule of our own, the server's series stays as it is.
        qCWarning(GRAPH_LOG) << "recurrence rule of" << incidence->uid() << "has no Graph equivalent, not writing it";
        return server;
    }
    // KOrganizer's editor writes Monday as week start whatever the rule had, and offers
    // no way to choose one; keep the server's, which decides the weeks of a series that
    // repeats every other week.
    const QJsonObject serverPattern = server.value(QLatin1String("pattern")).toObject();
    if (pattern.value(QLatin1String("type")) == QLatin1String("weekly") && r->defaultRRuleConst()->weekStart() == 1
        && serverPattern.value(QLatin1String("type")) == QLatin1String("weekly") && serverPattern.contains(QLatin1String("firstDayOfWeek"))) {
        pattern.insert(QStringLiteral("firstDayOfWeek"), serverPattern.value(QLatin1String("firstDayOfWeek")));
    }

    // Graph requires range.type and range.startDate.
    QJsonObject rangeObj;
    rangeObj.insert(QStringLiteral("startDate"), startDate.toString(Qt::ISODate));
    rangeObj.insert(QStringLiteral("recurrenceTimeZone"), timeZoneToGraph(zone));
    const int duration = r->duration();
    if (duration > 0) {
        rangeObj.insert(QStringLiteral("type"), QStringLiteral("numbered"));
        rangeObj.insert(QStringLiteral("numberOfOccurrences"), duration);
    } else if (duration == 0 && r->endDateTime().isValid()) {
        const QDate endDate = allDay ? r->endDate() : r->endDateTime().toTimeZone(zone).date();
        rangeObj.insert(QStringLiteral("type"), QStringLiteral("endDate"));
        rangeObj.insert(QStringLiteral("endDate"), endDate.toString(Qt::ISODate));
    } else {
        rangeObj.insert(QStringLiteral("type"), QStringLiteral("noEnd"));
    }

    QJsonObject recurrence;
    recurrence.insert(QStringLiteral("pattern"), pattern);
    recurrence.insert(QStringLiteral("range"), rangeObj);
    // What the editor only rewrote goes back as the server has it; when that was all,
    // this is the server's rule unchanged.
    return server.isEmpty() ? recurrence : undoEditorRewrites(server, recurrence, startDate);
}

QString mimeType()
{
    return QStringLiteral("application/x-vnd.akonadi.calendar.event");
}

KCalendarCore::Event::Ptr toEvent(const QJsonObject &json)
{
    if (json.value(QLatin1String("id")).toString().isEmpty()) {
        return {};
    }
    auto event = Event::Ptr(new Event);
    const QString uid = json.value(QLatin1String("iCalUId")).toString();
    event->setUid(uid.isEmpty() ? json.value(QLatin1String("id")).toString() : uid);
    event->setSummary(json.value(QLatin1String("subject")).toString());

    const QJsonObject body = json.value(QLatin1String("body")).toObject();
    if (body.value(QLatin1String("contentType")).toString() == QLatin1String("text")) {
        event->setDescription(body.value(QLatin1String("content")).toString());
    } else {
        event->setDescription(json.value(QLatin1String("bodyPreview")).toString());
    }

    const bool allDay = json.value(QLatin1String("isAllDay")).toBool();
    event->setAllDay(allDay);
    QDateTime start = parseDateTimeTimeZone(json.value(QLatin1String("start")).toObject());
    QDateTime end = parseDateTimeTimeZone(json.value(QLatin1String("end")).toObject());
    if (allDay) {
        // Graph sends all-day boundaries as midnight in their own zone; take the dates
        // verbatim (converting them would shift the day on one side of UTC).
        if (start.isValid()) {
            start = QDateTime(start.date(), QTime(0, 0));
        }
        if (end.isValid()) {
            // Graph's all-day end is exclusive (midnight of the next day); KCalendarCore
            // expects the inclusive last day — otherwise every all-day event shows two days.
            end = QDateTime(end.date().addDays(-1), QTime(0, 0));
        }
    } else if (start.isValid()) {
        const QTimeZone startZone = eventZone(json, start);
        start = start.toTimeZone(startZone);
        // A series keeps one zone for both ends; a single event may end elsewhere
        // (a flight), so its end follows originalEndTimeZone when Graph names one.
        QTimeZone endZone = startZone;
        if (json.value(QLatin1String("recurrence")).toObject().isEmpty()) {
            const QTimeZone original = timeZoneFromGraph(json.value(QLatin1String("originalEndTimeZone")).toString());
            if (original.isValid()) {
                endZone = original;
            }
        }
        end = end.toTimeZone(endZone);
    }
    event->setDtStart(start);
    event->setDtEnd(end);

    event->setLocation(json.value(QLatin1String("location")).toObject().value(QLatin1String("displayName")).toString());

    const QJsonObject organizer = json.value(QLatin1String("organizer")).toObject().value(QLatin1String("emailAddress")).toObject();
    if (!organizer.isEmpty()) {
        event->setOrganizer(Person(organizer.value(QLatin1String("name")).toString(), organizer.value(QLatin1String("address")).toString()));
    }

    const QJsonArray attendees = json.value(QLatin1String("attendees")).toArray();
    for (const auto &a : attendees) {
        const QJsonObject ao = a.toObject();
        const QJsonObject email = ao.value(QLatin1String("emailAddress")).toObject();
        if (email.isEmpty()) {
            continue;
        }
        Attendee att(email.value(QLatin1String("name")).toString(), email.value(QLatin1String("address")).toString());
        const QString resp = ao.value(QLatin1String("status")).toObject().value(QLatin1String("response")).toString();
        if (resp == QLatin1String("accepted")) {
            att.setStatus(Attendee::Accepted);
        } else if (resp == QLatin1String("declined")) {
            att.setStatus(Attendee::Declined);
        } else if (resp == QLatin1String("tentativelyAccepted")) {
            att.setStatus(Attendee::Tentative);
        }
        event->addAttendee(att);
    }

    QStringList categories;
    const QJsonArray cats = json.value(QLatin1String("categories")).toArray();
    categories.reserve(cats.count());
    for (const auto &c : cats) {
        categories << c.toString();
    }
    event->setCategories(categories);

    if (json.value(QLatin1String("showAs")).toString() == QLatin1String("free")) {
        event->setTransparency(Event::Transparent);
    }
    const QString sensitivity = json.value(QLatin1String("sensitivity")).toString();
    if (sensitivity == QLatin1String("private")) {
        event->setSecrecy(Incidence::SecrecyPrivate);
    } else if (sensitivity == QLatin1String("confidential")) {
        event->setSecrecy(Incidence::SecrecyConfidential);
    }

    const QJsonObject recurrence = json.value(QLatin1String("recurrence")).toObject();
    if (!recurrence.isEmpty()) {
        applyRecurrence(recurrence, event);
        if (!event->recurs() || event->recurrence()->recurrenceType() == Recurrence::rOther) {
            event->setCustomProperty(kCustomApp.toByteArray(), kKeepServerRecurrence.toByteArray(), QStringLiteral("1"));
        }
    }

    return event;
}

QJsonObject toJson(const KCalendarCore::Event::Ptr &event)
{
    QJsonObject json;
    json.insert(QStringLiteral("subject"), event->summary());

    QJsonObject body;
    body.insert(QStringLiteral("contentType"), QStringLiteral("text"));
    body.insert(QStringLiteral("content"), event->description());
    json.insert(QStringLiteral("body"), body);

    const bool allDay = event->allDay();
    json.insert(QStringLiteral("isAllDay"), allDay);
    if (allDay) {
        json.insert(QStringLiteral("start"), toAllDayDateTime(event->dtStart().date()));
        // Mirror the inclusive/exclusive conversion from toEvent().
        json.insert(QStringLiteral("end"), toAllDayDateTime(event->dtEnd().date().addDays(1)));
    } else {
        json.insert(QStringLiteral("start"), toDateTimeTimeZone(event->dtStart()));
        json.insert(QStringLiteral("end"), toDateTimeTimeZone(event->dtEnd()));
    }

    if (!event->location().isEmpty()) {
        QJsonObject loc;
        loc.insert(QStringLiteral("displayName"), event->location());
        json.insert(QStringLiteral("location"), loc);
    }

    if (!event->categories().isEmpty()) {
        json.insert(QStringLiteral("categories"), QJsonArray::fromStringList(event->categories()));
    }
    json.insert(QStringLiteral("showAs"), event->transparency() == Event::Transparent ? QStringLiteral("free") : QStringLiteral("busy"));

    if (!event->recurs()) {
        // Explicitly, so that an update turns a series on the server into a single
        // event — unless the series may have been lost unasked (see above): then the
        // server's rule goes out again, which also keeps it when the event is created
        // anew in another calendar.
        if (event->customProperty(kCustomApp.toByteArray(), kKeepServerRecurrence.toByteArray()).isEmpty()) {
            json.insert(QStringLiteral("recurrence"), QJsonValue::Null);
        } else {
            const QJsonObject server = storedServerRecurrence(event);
            if (!server.isEmpty()) {
                json.insert(QStringLiteral("recurrence"), server);
            }
        }
    } else {
        const QJsonObject recurrence = recurrenceToJson(event);
        if (!recurrence.isEmpty()) {
            json.insert(QStringLiteral("recurrence"), recurrence);
        }
    }
    return json;
}
}
