/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "graphmtaresource.h"

#include "graphresourceinterface.h"

#include <KLocalizedString>
#include <KMime/Message>
#include <QDBusConnection>
#include <QDBusConnectionInterface>

using namespace Akonadi;

static const auto kResourceServicePrefix = QLatin1String("org.freedesktop.Akonadi.Resource.akonadi_graph_resource");

GraphMtaResource::GraphMtaResource(const QString &id)
    : Akonadi::ResourceWidgetBase(id)
{
    setNeedsNetwork(true);
}

GraphMtaResource::~GraphMtaResource() = default;

bool GraphMtaResource::connectToMasterResource()
{
    if (mGraphResource) {
        return true;
    }
    // Find the (first) running Graph receive resource on the session bus. A config
    // option to pin a specific instance can come with the config dialog.
    const QStringList services = QDBusConnection::sessionBus().interface()->registeredServiceNames().value();
    for (const QString &service : services) {
        if (!service.startsWith(kResourceServicePrefix)) {
            continue;
        }
        auto iface = new OrgKdeAkonadiGraphResourceInterface(service, QStringLiteral("/"), QDBusConnection::sessionBus(), this);
        if (iface->isValid()) {
            mGraphResource = iface;
            connect(mGraphResource, &OrgKdeAkonadiGraphResourceInterface::messageSent, this, &GraphMtaResource::messageSent);
            return true;
        }
        delete iface;
    }
    return false;
}

void GraphMtaResource::sendItem(const Akonadi::Item &item)
{
    if (!connectToMasterResource()) {
        itemSent(item, TransportFailed, i18n("Unable to connect to master Microsoft 365 (Graph) resource"));
        return;
    }

    const QString id = QString::number(item.id());
    mItemHash.insert(id, item);

    auto msg = item.payload<std::shared_ptr<KMime::Message>>();
    /* Exchange re-assembles the message from parsed MIME parts and mangles
     * quoted-printable bodies (KMail's preferred encoding) in the process.
     * Force base64 on all parts — same workaround as the EWS MTA. */
    if (msg->contents().isEmpty()) {
        msg->changeEncoding(KMime::Headers::CEbase64);
        msg->contentTransferEncoding(KMime::CreatePolicy::Create)->setEncoding(KMime::Headers::CEbase64);
    } else {
        const auto contents = msg->contents();
        for (KMime::Content *content : contents) {
            content->changeEncoding(KMime::Headers::CEbase64);
            content->contentTransferEncoding(KMime::CreatePolicy::Create)->setEncoding(KMime::Headers::CEbase64);
        }
    }
    msg->assemble();

    mGraphResource->sendMessage(id, msg->encodedContent(KMime::NewlineType::CRLF));
}

void GraphMtaResource::messageSent(const QString &id, const QString &error)
{
    const auto it = mItemHash.constFind(id);
    if (it == mItemHash.constEnd()) {
        return;
    }
    itemSent(*it, error.isEmpty() ? TransportSucceeded : TransportFailed, error);
    mItemHash.erase(it);
}

AKONADI_RESOURCE_MAIN(GraphMtaResource)

#include "moc_graphmtaresource.cpp"
