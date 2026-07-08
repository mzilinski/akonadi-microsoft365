/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Live end-to-end check of the calendar + contacts mapping against a real tenant.
    Needs a bearer token with Calendars.ReadWrite + Contacts.ReadWrite in
    GRAPH_ACCESS_TOKEN. Creates a test event and contact, reads them back through the
    real fetch job + handlers, verifies the mapping, and deletes them again.
*/

#include "calendar/grapheventhandler.h"
#include "contact/graphcontacthandler.h"
#include "graphclient/auth/graphoauth.h"
#include "graphclient/graphclient.h"
#include "graphclient/graphrequest.h"
#include "jobs/graphfetchpimitemsjob.h"

#include <KCalendarCore/Event>
#include <KContacts/Addressee>
#include <KContacts/Email>

#include <QDate>
#include <QTime>
#include <QTimeZone>

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>

#include <cstdio>

using namespace Akonadi;

static bool runJob(KJob *job)
{
    QEventLoop loop;
    QObject::connect(job, &KJob::result, &loop, &QEventLoop::quit);
    job->start();
    loop.exec();
    return !job->error();
}

// Minimal synchronous request helper for setup/teardown (create/delete).
static QJsonObject request(GraphClient &client, GraphRequest::Method method, const QString &path, const QJsonObject &body = {})
{
    auto req = new GraphRequest(client);
    req->setMethod(method);
    req->setPath(path);
    if (method == GraphRequest::Post || method == GraphRequest::Patch) {
        req->setBody(body);
    }
    QJsonObject result;
    QEventLoop loop;
    QObject::connect(req, &KJob::result, [&](KJob *j) {
        if (!j->error()) {
            auto *r = static_cast<GraphRequest *>(j);
            result = r->responseObject();
            if (result.isEmpty() && !r->aggregatedValue().isEmpty()) {
                result.insert(QStringLiteral("value"), r->aggregatedValue());
            }
        } else {
            std::printf("  request %s failed: %s\n", qPrintable(path), qPrintable(j->errorText()));
        }
        loop.quit();
    });
    req->start();
    loop.exec();
    return result;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (!qEnvironmentVariableIsSet("GRAPH_ACCESS_TOKEN")) {
        std::printf("GRAPH_ACCESS_TOKEN not set\n");
        return 2;
    }
    GraphOAuth auth(QStringLiteral("common"), QString(), QStringLiteral("phase4"));
    auth.authenticate();
    GraphClient client;
    client.setBaseUrl(QStringLiteral("https://graph.microsoft.com/v1.0"));
    client.setAuth(&auth);

    int failures = 0;

    // --- calendar -----------------------------------------------------------
    const auto cals = request(client, GraphRequest::Get, QStringLiteral("/me/calendars?$select=id,name"));
    const QString calId = cals.value(QLatin1String("value")).toArray().first().toObject().value(QLatin1String("id")).toString();
    if (calId.isEmpty()) {
        std::printf("FAIL  no calendar found\n");
        return 1;
    }
    QJsonObject evStart{{QStringLiteral("dateTime"), QStringLiteral("2026-07-15T09:00:00")}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}};
    QJsonObject evEnd{{QStringLiteral("dateTime"), QStringLiteral("2026-07-15T10:30:00")}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}};
    QJsonObject evLoc{{QStringLiteral("displayName"), QStringLiteral("Testraum")}};
    QJsonObject evBody{{QStringLiteral("subject"), QStringLiteral("phase4check event")},
                       {QStringLiteral("start"), evStart},
                       {QStringLiteral("end"), evEnd},
                       {QStringLiteral("location"), evLoc}};
    const auto createdEv = request(client, GraphRequest::Post, QStringLiteral("/me/calendars/%1/events").arg(calId), evBody);
    const QString evId = createdEv.value(QLatin1String("id")).toString();

    Collection calCol;
    calCol.setRemoteId(calId);
    calCol.setContentMimeTypes({GraphEventHandler::mimeType()});
    auto evJob = new GraphFetchPimItemsJob(client, calCol, GraphFetchPimItemsJob::Events, QString());
    if (runJob(evJob)) {
        bool found = false;
        for (const Item &it : evJob->changedItems()) {
            if (it.remoteId() == evId) {
                auto ev = it.payload<KCalendarCore::Incidence::Ptr>().dynamicCast<KCalendarCore::Event>();
                const bool ok = ev && ev->summary() == QLatin1String("phase4check event") && ev->dtStart().isValid() && ev->dtEnd().isValid()
                    && ev->location() == QLatin1String("Testraum");
                std::printf("%s  event mapped: summary=\"%s\" start=%s end=%s loc=\"%s\"\n",
                            ok ? "OK   " : "FAIL ",
                            qPrintable(ev ? ev->summary() : QString()),
                            qPrintable(ev ? ev->dtStart().toString(Qt::ISODate) : QString()),
                            qPrintable(ev ? ev->dtEnd().toString(Qt::ISODate) : QString()),
                            qPrintable(ev ? ev->location() : QString()));
                found = true;
                if (!ok) {
                    ++failures;
                }
            }
        }
        if (!found) {
            std::printf("FAIL  created event not returned by fetch job\n");
            ++failures;
        }
    } else {
        std::printf("FAIL  event fetch job errored\n");
        ++failures;
    }
    if (!evId.isEmpty()) {
        request(client, GraphRequest::Delete, QStringLiteral("/me/events/%1").arg(evId));
    }

    // --- contacts -----------------------------------------------------------
    QJsonObject ctEmail{{QStringLiteral("address"), QStringLiteral("erika@example.com")}, {QStringLiteral("name"), QStringLiteral("Erika Musterfrau")}};
    QJsonObject ctBody{{QStringLiteral("givenName"), QStringLiteral("Erika")},
                       {QStringLiteral("surname"), QStringLiteral("Musterfrau")},
                       {QStringLiteral("emailAddresses"), QJsonArray{ctEmail}},
                       {QStringLiteral("mobilePhone"), QStringLiteral("+49 170 111")},
                       {QStringLiteral("companyName"), QStringLiteral("ACME")},
                       {QStringLiteral("jobTitle"), QStringLiteral("Chefin")}};
    const auto createdCt = request(client, GraphRequest::Post, QStringLiteral("/me/contacts"), ctBody);
    const QString ctId = createdCt.value(QLatin1String("id")).toString();

    Collection ctCol;
    ctCol.setRemoteId(QStringLiteral("contacts-default"));
    ctCol.setContentMimeTypes({GraphContactHandler::mimeType()});
    auto ctJob = new GraphFetchPimItemsJob(client, ctCol, GraphFetchPimItemsJob::Contacts, QString());
    if (runJob(ctJob)) {
        bool found = false;
        for (const Item &it : ctJob->changedItems()) {
            if (it.remoteId() == ctId) {
                const auto a = it.payload<KContacts::Addressee>();
                const bool ok = a.givenName() == QLatin1String("Erika") && a.familyName() == QLatin1String("Musterfrau")
                    && a.preferredEmail() == QLatin1String("erika@example.com") && a.organization() == QLatin1String("ACME");
                std::printf("%s  contact mapped: name=\"%s %s\" email=%s org=\"%s\" phones=%lld\n",
                            ok ? "OK   " : "FAIL ",
                            qPrintable(a.givenName()),
                            qPrintable(a.familyName()),
                            qPrintable(a.preferredEmail()),
                            qPrintable(a.organization()),
                            static_cast<long long>(a.phoneNumbers().size()));
                found = true;
                if (!ok) {
                    ++failures;
                }
            }
        }
        if (!found) {
            std::printf("FAIL  created contact not returned by fetch job\n");
            ++failures;
        }
    } else {
        std::printf("FAIL  contact fetch job errored\n");
        ++failures;
    }
    if (!ctId.isEmpty()) {
        request(client, GraphRequest::Delete, QStringLiteral("/me/contacts/%1").arg(ctId));
    }

    // --- WRITE round-trip: event via toJson() -------------------------------
    {
        auto ev = KCalendarCore::Event::Ptr(new KCalendarCore::Event);
        ev->setSummary(QStringLiteral("phase4 write event"));
        ev->setDtStart(QDateTime(QDate(2026, 8, 1), QTime(14, 0), QTimeZone::utc()));
        ev->setDtEnd(QDateTime(QDate(2026, 8, 1), QTime(15, 0), QTimeZone::utc()));
        ev->setLocation(QStringLiteral("Büro"));
        const auto created = request(client, GraphRequest::Post, QStringLiteral("/me/calendars/%1/events").arg(calId), GraphEventHandler::toJson(ev));
        const QString id = created.value(QLatin1String("id")).toString();
        // Read it back through the handler.
        const auto readBack = request(client, GraphRequest::Get, QStringLiteral("/me/events/%1").arg(id));
        auto parsed = GraphEventHandler::toEvent(readBack);
        const bool ok = !id.isEmpty() && parsed && parsed->summary() == QLatin1String("phase4 write event") && parsed->location() == QStringLiteral("Büro")
            && parsed->dtStart().toUTC().time() == QTime(14, 0);
        std::printf("%s  event WRITE round-trip: summary=\"%s\" loc=\"%s\" start=%s\n",
                    ok ? "OK   " : "FAIL ",
                    qPrintable(parsed ? parsed->summary() : QString()),
                    qPrintable(parsed ? parsed->location() : QString()),
                    qPrintable(parsed ? parsed->dtStart().toUTC().toString(Qt::ISODate) : QString()));
        if (!ok) {
            ++failures;
        }
        if (!id.isEmpty()) {
            request(client, GraphRequest::Delete, QStringLiteral("/me/events/%1").arg(id));
        }
    }

    // --- WRITE round-trip: contact via toJson() -----------------------------
    {
        KContacts::Addressee a;
        a.setGivenName(QStringLiteral("Hans"));
        a.setFamilyName(QStringLiteral("Schreiber"));
        a.setOrganization(QStringLiteral("WriteCo"));
        KContacts::Email mail(QStringLiteral("hans@example.com"));
        mail.setPreferred(true);
        a.addEmail(mail);
        const auto created = request(client, GraphRequest::Post, QStringLiteral("/me/contacts"), GraphContactHandler::toJson(a));
        const QString id = created.value(QLatin1String("id")).toString();
        const auto readBack = request(client, GraphRequest::Get, QStringLiteral("/me/contacts/%1").arg(id));
        const auto parsed = GraphContactHandler::toAddressee(readBack);
        const bool ok = !id.isEmpty() && parsed.givenName() == QLatin1String("Hans") && parsed.familyName() == QLatin1String("Schreiber")
            && parsed.organization() == QLatin1String("WriteCo") && parsed.preferredEmail() == QLatin1String("hans@example.com");
        std::printf("%s  contact WRITE round-trip: name=\"%s %s\" org=\"%s\" email=%s\n",
                    ok ? "OK   " : "FAIL ",
                    qPrintable(parsed.givenName()),
                    qPrintable(parsed.familyName()),
                    qPrintable(parsed.organization()),
                    qPrintable(parsed.preferredEmail()));
        if (!ok) {
            ++failures;
        }
        if (!id.isEmpty()) {
            request(client, GraphRequest::Delete, QStringLiteral("/me/contacts/%1").arg(id));
        }
    }

    // --- DELTA incrementality: initial -> 0 -> +1 (create) -> -1 (delete) ----
    {
        Collection cal;
        cal.setRemoteId(calId);
        cal.setContentMimeTypes({GraphEventHandler::mimeType()});

        auto init = new GraphFetchPimItemsJob(client, cal, GraphFetchPimItemsJob::Events, QString());
        runJob(init);
        const long long initialCount = init->changedItems().size(); // capture before KJob auto-deletes
        const QString dl1 = init->deltaLink();

        auto empty = new GraphFetchPimItemsJob(client, cal, GraphFetchPimItemsJob::Events, dl1);
        runJob(empty);
        const bool emptyOk = empty->changedItems().isEmpty() && empty->removedItems().isEmpty() && !empty->deltaLink().isEmpty();
        const QString dl2 = empty->deltaLink();

        // create -> delta should report exactly the new event as changed
        QJsonObject s{{QStringLiteral("dateTime"), QStringLiteral("2026-10-01T08:00:00")}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}};
        QJsonObject e{{QStringLiteral("dateTime"), QStringLiteral("2026-10-01T09:00:00")}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}};
        QJsonObject body{{QStringLiteral("subject"), QStringLiteral("delta probe")}, {QStringLiteral("start"), s}, {QStringLiteral("end"), e}};
        const QString newId =
            request(client, GraphRequest::Post, QStringLiteral("/me/calendars/%1/events").arg(calId), body).value(QLatin1String("id")).toString();

        auto added = new GraphFetchPimItemsJob(client, cal, GraphFetchPimItemsJob::Events, dl2);
        runJob(added);
        bool addOk = false;
        for (const Item &it : added->changedItems()) {
            if (it.remoteId() == newId) {
                addOk = true;
            }
        }
        const QString dl3 = added->deltaLink();

        // delete -> delta should report it as removed
        request(client, GraphRequest::Delete, QStringLiteral("/me/events/%1").arg(newId));
        auto removed = new GraphFetchPimItemsJob(client, cal, GraphFetchPimItemsJob::Events, dl3);
        runJob(removed);
        bool delOk = false;
        for (const Item &it : removed->removedItems()) {
            if (it.remoteId() == newId) {
                delOk = true;
            }
        }

        const bool ok = emptyOk && addOk && delOk;
        std::printf("%s  delta: initial=%lld, 2nd-empty=%s, create->+1=%s, delete->removed=%s\n",
                    ok ? "OK   " : "FAIL ",
                    initialCount,
                    emptyOk ? "yes" : "NO",
                    addOk ? "yes" : "NO",
                    delOk ? "yes" : "NO");
        if (!ok) {
            ++failures;
        }
    }

    std::printf("%s (%d failures)\n", failures ? "PHASE 4 CHECK FAILED" : "PHASE 4 CHECK PASSED", failures);
    return failures ? 1 : 0;
}
