/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "graphsyncstateattribute.h"

GraphSyncStateAttribute::GraphSyncStateAttribute(const QString &deltaLink, int version)
    : mDeltaLink(deltaLink)
    , mVersion(version)
{
}

void GraphSyncStateAttribute::setDeltaLink(const QString &deltaLink)
{
    mDeltaLink = deltaLink;
}

const QString &GraphSyncStateAttribute::deltaLink() const
{
    return mDeltaLink;
}

int GraphSyncStateAttribute::version() const
{
    return mVersion;
}

QByteArray GraphSyncStateAttribute::type() const
{
    return QByteArrayLiteral("graphsyncstate");
}

Akonadi::Attribute *GraphSyncStateAttribute::clone() const
{
    return new GraphSyncStateAttribute(mDeltaLink, mVersion);
}

// The version travels as a URL fragment ("#mapping=1"): builds that predate it use
// the stored string as the request URL, and HTTP never sends a fragment, so they keep
// syncing after a downgrade. The link they then store has no fragment, which makes a
// later upgrade re-list the collection, as it must.
namespace
{
constexpr QByteArrayView kVersionFragment("#mapping=");
}

QByteArray GraphSyncStateAttribute::serialized() const
{
    if (mVersion <= 0 || mDeltaLink.isEmpty()) {
        return mDeltaLink.toUtf8();
    }
    QByteArray data = mDeltaLink.toUtf8();
    data += kVersionFragment;
    data += QByteArray::number(mVersion);
    return data;
}

void GraphSyncStateAttribute::deserialize(const QByteArray &data)
{
    mVersion = 0;
    QByteArray link = data;
    const qsizetype fragment = data.lastIndexOf(kVersionFragment);
    if (fragment >= 0) {
        bool ok = false;
        const int version = data.mid(fragment + kVersionFragment.size()).toInt(&ok);
        if (ok) {
            mVersion = version;
            link.truncate(fragment);
        }
    }
    mDeltaLink = QString::fromUtf8(link);
}
