/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Lists collections of a resource that carry a SpecialCollectionAttribute — the
    authoritative check (via the running Akonadi server) that special-folder tagging
    persisted. Usage: specialcheck <resource instance id>
*/

#include <Akonadi/CollectionFetchJob>
#include <Akonadi/CollectionFetchScope>
#include <Akonadi/EntityDisplayAttribute>
#include <Akonadi/SpecialCollectionAttribute>

#include <QCoreApplication>

#include <cstdio>

using namespace Akonadi;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::printf("usage: %s <resource instance id>\n", argv[0]);
        return 2;
    }
    auto *job = new CollectionFetchJob(Collection::root(), CollectionFetchJob::Recursive);
    job->fetchScope().setResource(QString::fromLatin1(argv[1]));
    job->fetchScope().fetchAttribute<SpecialCollectionAttribute>();
    job->fetchScope().fetchAttribute<EntityDisplayAttribute>();
    QObject::connect(job, &CollectionFetchJob::result, [&](KJob *j) {
        if (j->error()) {
            std::printf("FAIL: %s\n", qPrintable(j->errorText()));
            app.exit(1);
            return;
        }
        int tagged = 0;
        const auto cols = static_cast<CollectionFetchJob *>(j)->collections();
        for (const Collection &c : cols) {
            if (const auto *a = c.attribute<SpecialCollectionAttribute>()) {
                const auto *d = c.attribute<EntityDisplayAttribute>();
                std::printf("  %-24s -> %-10s icon=%s\n",
                            qPrintable(c.name()),
                            a->collectionType().constData(),
                            d ? qPrintable(d->iconName()) : "(none)");
                ++tagged;
            }
        }
        std::printf("%d special collection(s) tagged\n", tagged);
        app.exit(tagged > 0 ? 0 : 1);
    });
    return app.exec();
}
