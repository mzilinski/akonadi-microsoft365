/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Fetches one item's full payload through a running Akonadi server — exercises the
    resource's on-demand retrieveItems(items, parts) path exactly like KMail would.

    Usage: akonadifetchtest <collection id>
*/

#include <Akonadi/ItemFetchJob>
#include <Akonadi/ItemFetchScope>
#include <KMime/Message>

#include <QCoreApplication>

#include <cstdio>

using namespace Akonadi;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::printf("usage: %s <collection id>\n", argv[0]);
        return 2;
    }

    auto job = new ItemFetchJob(Collection(QString::fromLatin1(argv[1]).toLongLong()));
    job->fetchScope().setFetchModificationTime(false);
    QObject::connect(job, &ItemFetchJob::result, [&](KJob *j) {
        if (j->error()) {
            std::printf("FAIL list: %s\n", qPrintable(j->errorText()));
            app.exit(1);
            return;
        }
        const auto items = static_cast<ItemFetchJob *>(j)->items();
        if (items.isEmpty()) {
            std::printf("FAIL: no items in collection\n");
            app.exit(1);
            return;
        }
        // Now fetch the full payload of the first item — triggers the resource.
        auto pj = new ItemFetchJob(items.first());
        pj->fetchScope().fetchFullPayload();
        QObject::connect(pj, &ItemFetchJob::result, [&](KJob *j2) {
            if (j2->error()) {
                std::printf("FAIL payload: %s\n", qPrintable(j2->errorText()));
                app.exit(1);
                return;
            }
            const auto fetched = static_cast<ItemFetchJob *>(j2)->items();
            if (fetched.isEmpty() || !fetched.first().hasPayload<std::shared_ptr<KMime::Message>>()) {
                std::printf("FAIL: item has no KMime payload\n");
                app.exit(1);
                return;
            }
            const auto msg = fetched.first().payload<std::shared_ptr<KMime::Message>>();
            std::printf("OK payload via Akonadi: %lld bytes, subject: \"%s\"\n",
                        static_cast<long long>(msg->encodedContent().size()),
                        qPrintable(msg->subject()->asUnicodeString().left(60)));
            app.exit(0);
        });
    });
    return app.exec();
}
