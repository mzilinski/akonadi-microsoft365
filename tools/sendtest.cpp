/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Sends a test mail through the running Graph resource's D-Bus send API — the same
    call the MTA agent makes. Verifies Phase 3 end-to-end (draft from MIME + /send).

    Usage: sendtest <resource instance id> <from> <to>
*/

#include <KMime/Message>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QTimer>

#include <cstdio>

class SentWaiter : public QObject
{
    Q_OBJECT
public Q_SLOTS:
    void onMessageSent(const QString &id, const QString &error)
    {
        if (error.isEmpty()) {
            std::printf("OK    messageSent id=%s — mail accepted by Graph\n", qPrintable(id));
            QCoreApplication::exit(0);
        } else {
            std::printf("FAIL  messageSent id=%s error: %s\n", qPrintable(id), qPrintable(error));
            QCoreApplication::exit(1);
        }
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 4) {
        std::printf("usage: %s <resource instance id> <from> <to>\n", argv[0]);
        return 2;
    }
    const QString service = QStringLiteral("org.freedesktop.Akonadi.Resource.%1").arg(QLatin1String(argv[1]));

    KMime::Message msg;
    msg.from()->fromUnicodeString(QString::fromLatin1(argv[2]));
    msg.to()->fromUnicodeString(QString::fromLatin1(argv[3]));
    msg.subject()->fromUnicodeString(QStringLiteral("Akonadi Graph Resource Testmail"));
    msg.contentType()->setMimeType("text/plain");
    msg.contentType()->setCharset("utf-8");
    msg.date()->setDateTime(QDateTime::currentDateTime());
    msg.setBody("Testmail von der neuen Akonadi-Graph-Resource (Phase 3, Senden).\n");
    msg.assemble();

    SentWaiter waiter;
    QDBusConnection::sessionBus().connect(service,
                                          QStringLiteral("/"),
                                          QStringLiteral("org.kde.Akonadi.Graph.Resource"),
                                          QStringLiteral("messageSent"),
                                          &waiter,
                                          SLOT(onMessageSent(QString, QString)));

    QDBusInterface iface(service, QStringLiteral("/"), QStringLiteral("org.kde.Akonadi.Graph.Resource"));
    if (!iface.isValid()) {
        std::printf("FAIL: resource D-Bus interface not reachable (%s)\n", qPrintable(service));
        return 1;
    }
    iface.call(QStringLiteral("sendMessage"), QStringLiteral("sendtest-1"), msg.encodedContent(KMime::NewlineType::CRLF));
    std::printf("sendMessage dispatched, waiting for messageSent...\n");

    QTimer::singleShot(30000, &app, [] {
        std::printf("TIMEOUT: no messageSent signal within 30s\n");
        QCoreApplication::exit(1);
    });

    return app.exec();
}

#include "sendtest.moc"
