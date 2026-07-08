/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    End-to-end write-path test THROUGH the running resource (like KOrganizer /
    KAddressBook do): create, modify and delete an event and a contact via Akonadi.
    Each step drives the resource's itemAdded/itemChanged/itemsRemoved replay to Graph.
    A create is confirmed when the resource stamps the item with a server remoteId.

    Usage: writetest <resource instance id>
    Cleans up everything it creates.
*/

#include "calendar/grapheventhandler.h"
#include "contact/graphcontacthandler.h"

#include <Akonadi/CollectionFetchJob>
#include <Akonadi/CollectionFetchScope>
#include <Akonadi/ItemCreateJob>
#include <Akonadi/ItemDeleteJob>
#include <Akonadi/ItemFetchJob>
#include <Akonadi/ItemFetchScope>
#include <Akonadi/ItemModifyJob>

#include <KCalendarCore/Event>
#include <KContacts/Addressee>

#include <QCoreApplication>
#include <QDate>
#include <QEventLoop>
#include <QTimeZone>
#include <QTimer>

#include <cstdio>

using namespace Akonadi;

static bool runJob(KJob *job, const char *what)
{
    QEventLoop loop;
    QObject::connect(job, &KJob::result, &loop, &QEventLoop::quit);
    job->start();
    loop.exec();
    if (job->error()) {
        std::printf("FAIL  %-30s %s\n", what, qPrintable(job->errorText()));
        return false;
    }
    return true;
}

static void wait(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// Fetch an item fresh and return its remoteId (empty if the resource hasn't committed).
static QString remoteIdOf(const Item &item)
{
    auto job = new ItemFetchJob(Item(item.id()));
    if (!runJob(job, "refetch")) {
        return {};
    }
    const auto items = job->items();
    return items.isEmpty() ? QString() : items.first().remoteId();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::printf("usage: %s <resource instance id>\n", argv[0]);
        return 2;
    }
    const QString resource = QString::fromLatin1(argv[1]);

    // Locate the calendar and contacts collections.
    auto cjob = new CollectionFetchJob(Collection::root(), CollectionFetchJob::Recursive);
    cjob->fetchScope().setResource(resource);
    if (!runJob(cjob, "collection fetch")) {
        return 1;
    }
    Collection calendar, contacts;
    for (const Collection &c : cjob->collections()) {
        // Pick a WRITABLE calendar (read-only ones like Birthdays/Holidays can't be modified).
        if (c.contentMimeTypes().contains(GraphEventHandler::mimeType()) && (c.rights() & Collection::CanCreateItem)) {
            calendar = c;
        }
        if (c.contentMimeTypes().contains(GraphContactHandler::mimeType())) {
            contacts = c;
        }
    }
    if (!calendar.isValid() || !contacts.isValid()) {
        std::printf("FAIL  calendar/contacts collection not found\n");
        return 1;
    }

    int failures = 0;

    // ===== EVENT: create -> modify -> delete =====
    {
        auto ev = KCalendarCore::Event::Ptr(new KCalendarCore::Event);
        ev->setSummary(QStringLiteral("writetest Termin"));
        ev->setDtStart(QDateTime(QDate(2026, 9, 3), QTime(11, 0), QTimeZone::utc()));
        ev->setDtEnd(QDateTime(QDate(2026, 9, 3), QTime(12, 0), QTimeZone::utc()));
        Item item(GraphEventHandler::mimeType());
        item.setPayload<KCalendarCore::Incidence::Ptr>(ev);

        auto create = new ItemCreateJob(item, calendar);
        if (runJob(create, "event create")) {
            Item created = create->item();
            wait(4000); // let the resource POST and changeCommitted()
            const QString rid = remoteIdOf(created);
            if (!rid.isEmpty()) {
                std::printf("OK    event create -> server id %s...\n", qPrintable(rid.left(20)));

                // modify
                auto fetch = new ItemFetchJob(Item(created.id()));
                fetch->fetchScope().fetchFullPayload();
                runJob(fetch, "event refetch");
                Item toModify = fetch->items().first();
                auto inc = toModify.payload<KCalendarCore::Incidence::Ptr>();
                inc->setSummary(QStringLiteral("writetest Termin (geändert)"));
                toModify.setPayload<KCalendarCore::Incidence::Ptr>(inc);
                if (runJob(new ItemModifyJob(toModify), "event modify")) {
                    wait(3000);
                    std::printf("OK    event modify committed\n");
                }

                // delete
                if (runJob(new ItemDeleteJob(created), "event delete")) {
                    wait(3000);
                    std::printf("OK    event delete committed\n");
                }
            } else {
                std::printf("FAIL  event create: no remoteId assigned (resource did not commit)\n");
                ++failures;
                runJob(new ItemDeleteJob(created), "event cleanup");
            }
        } else {
            ++failures;
        }
    }

    // ===== CONTACT: create -> modify -> delete =====
    {
        KContacts::Addressee a;
        a.setGivenName(QStringLiteral("Test"));
        a.setFamilyName(QStringLiteral("Schreibpfad"));
        a.setOrganization(QStringLiteral("writetest"));
        Item item(GraphContactHandler::mimeType());
        item.setPayload<KContacts::Addressee>(a);

        auto create = new ItemCreateJob(item, contacts);
        if (runJob(create, "contact create")) {
            Item created = create->item();
            wait(4000);
            const QString rid = remoteIdOf(created);
            if (!rid.isEmpty()) {
                std::printf("OK    contact create -> server id %s...\n", qPrintable(rid.left(20)));

                auto fetch = new ItemFetchJob(Item(created.id()));
                fetch->fetchScope().fetchFullPayload();
                runJob(fetch, "contact refetch");
                Item toModify = fetch->items().first();
                auto addr = toModify.payload<KContacts::Addressee>();
                addr.setTitle(QStringLiteral("Cheftester"));
                toModify.setPayload<KContacts::Addressee>(addr);
                if (runJob(new ItemModifyJob(toModify), "contact modify")) {
                    wait(3000);
                    std::printf("OK    contact modify committed\n");
                }

                if (runJob(new ItemDeleteJob(created), "contact delete")) {
                    wait(3000);
                    std::printf("OK    contact delete committed\n");
                }
            } else {
                std::printf("FAIL  contact create: no remoteId assigned (resource did not commit)\n");
                ++failures;
                runJob(new ItemDeleteJob(created), "contact cleanup");
            }
        } else {
            ++failures;
        }
    }

    std::printf("%s (%d failures)\n", failures ? "WRITE TEST FAILED" : "WRITE TEST PASSED", failures);
    return failures ? 1 : 0;
}
