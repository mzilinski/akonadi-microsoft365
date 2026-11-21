/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "graphcontacthandler.h"

#include <KContacts/Address>
#include <KContacts/Email>
#include <KContacts/PhoneNumber>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>

using namespace KContacts;

namespace
{
// What vCard cannot hold, kept as custom fields (X-GRAPH-...) of the contact:
// the display name Graph keeps with each e-mail address (a JSON object address -> name),
// and which of the single-valued fields the server had, so that removing one in KDE
// clears it there.
const QString kCustomApp = QStringLiteral("GRAPH");
const QString kEmailNames = QStringLiteral("EMAIL-NAMES");
const QString kServerHad = QStringLiteral("SERVER-HAD");
// Graph keeps at most three e-mail addresses per contact and rejects more.
constexpr int kMaxEmails = 3;

QStringList serverHad(const Addressee &a)
{
    return a.custom(kCustomApp, kServerHad).split(QLatin1Char(','), Qt::SkipEmptyParts);
}

void addPhones(Addressee &a, const QJsonArray &numbers, PhoneNumber::Type type)
{
    for (const auto &n : numbers) {
        const QString num = n.toString();
        if (!num.isEmpty()) {
            a.insertPhoneNumber(PhoneNumber(num, type));
        }
    }
}

Address toAddress(const QJsonObject &json, Address::Type type)
{
    Address addr(type);
    addr.setStreet(json.value(QLatin1String("street")).toString());
    addr.setLocality(json.value(QLatin1String("city")).toString());
    addr.setRegion(json.value(QLatin1String("state")).toString());
    addr.setPostalCode(json.value(QLatin1String("postalCode")).toString());
    addr.setCountry(json.value(QLatin1String("countryOrRegion")).toString());
    return addr;
}

QJsonObject addressToJson(const Address &addr)
{
    QJsonObject o;
    o.insert(QStringLiteral("street"), addr.street());
    o.insert(QStringLiteral("city"), addr.locality());
    o.insert(QStringLiteral("state"), addr.region());
    o.insert(QStringLiteral("postalCode"), addr.postalCode());
    o.insert(QStringLiteral("countryOrRegion"), addr.country());
    return o;
}

QJsonObject emptyAddress()
{
    return addressToJson(Address());
}
} // namespace

namespace GraphContactHandler
{
QString mimeType()
{
    return KContacts::Addressee::mimeType();
}

KContacts::Addressee toAddressee(const QJsonObject &json)
{
    Addressee a;
    a.setGivenName(json.value(QLatin1String("givenName")).toString());
    a.setFamilyName(json.value(QLatin1String("surname")).toString());
    a.setAdditionalName(json.value(QLatin1String("middleName")).toString());
    a.setPrefix(json.value(QLatin1String("title")).toString());
    a.setNickName(json.value(QLatin1String("nickName")).toString());
    a.setFormattedName(json.value(QLatin1String("displayName")).toString());

    const QJsonArray emails = json.value(QLatin1String("emailAddresses")).toArray();
    QJsonObject names;
    for (int i = 0; i < emails.size(); ++i) {
        const QJsonObject entry = emails.at(i).toObject();
        const QString addr = entry.value(QLatin1String("address")).toString();
        if (!addr.isEmpty()) {
            Email email(addr);
            email.setPreferred(i == 0);
            a.addEmail(email);
            const QString name = entry.value(QLatin1String("name")).toString();
            if (!name.isEmpty()) {
                names.insert(addr.toLower(), name);
            }
        }
    }
    if (!names.isEmpty()) {
        a.insertCustom(kCustomApp, kEmailNames, QString::fromUtf8(QJsonDocument(names).toJson(QJsonDocument::Compact)));
    }
    QStringList had;

    addPhones(a, json.value(QLatin1String("businessPhones")).toArray(), PhoneNumber::Work);
    addPhones(a, json.value(QLatin1String("homePhones")).toArray(), PhoneNumber::Home);
    const QString mobile = json.value(QLatin1String("mobilePhone")).toString();
    if (!mobile.isEmpty()) {
        a.insertPhoneNumber(PhoneNumber(mobile, PhoneNumber::Cell));
        had << QStringLiteral("mobilePhone");
    }

    a.setTitle(json.value(QLatin1String("jobTitle")).toString());
    a.setOrganization(json.value(QLatin1String("companyName")).toString());
    a.setDepartment(json.value(QLatin1String("department")).toString());
    a.setNote(json.value(QLatin1String("personalNotes")).toString());

    const QJsonObject business = json.value(QLatin1String("businessAddress")).toObject();
    if (!business.isEmpty() && business.contains(QLatin1String("street"))) {
        const Address addr = toAddress(business, Address::Work);
        if (!addr.isEmpty()) {
            a.insertAddress(addr);
            had << QStringLiteral("businessAddress");
        }
    }
    const QJsonObject home = json.value(QLatin1String("homeAddress")).toObject();
    if (!home.isEmpty() && home.contains(QLatin1String("street"))) {
        const Address addr = toAddress(home, Address::Home);
        if (!addr.isEmpty()) {
            a.insertAddress(addr);
            had << QStringLiteral("homeAddress");
        }
    }
    if (!had.isEmpty()) {
        a.insertCustom(kCustomApp, kServerHad, had.join(QLatin1Char(',')));
    }

    const QString birthday = json.value(QLatin1String("birthday")).toString();
    if (!birthday.isEmpty()) {
        const QDateTime bd = QDateTime::fromString(birthday, Qt::ISODate);
        if (bd.isValid()) {
            a.setBirthday(bd.date());
        }
    }

    QStringList categories;
    const QJsonArray cats = json.value(QLatin1String("categories")).toArray();
    categories.reserve(cats.count());
    for (const auto &c : cats) {
        const QString category = c.toString();
        if (!category.isEmpty()) {
            categories << category;
        }
    }
    a.setCategories(categories);

    // The photo is not part of the contact resource; GraphFetchPimItemsJob fetches
    // it separately from /me/contacts/{id}/photo/$value and sets it on the Addressee.
    return a;
}

QStringList emailsBeyondLimit(const KContacts::Addressee &a)
{
    Email::List emailList = a.emailList();
    std::stable_partition(emailList.begin(), emailList.end(), [](const Email &e) {
        return e.isPreferred();
    });
    QStringList beyond;
    int kept = 0;
    for (const Email &e : std::as_const(emailList)) {
        if (e.mail().isEmpty()) {
            continue;
        }
        if (++kept > kMaxEmails) {
            beyond << e.mail();
        }
    }
    return beyond;
}

QJsonObject toJson(const KContacts::Addressee &a)
{
    QJsonObject json;
    json.insert(QStringLiteral("givenName"), a.givenName());
    json.insert(QStringLiteral("surname"), a.familyName());
    json.insert(QStringLiteral("middleName"), a.additionalName());
    json.insert(QStringLiteral("title"), a.prefix());
    json.insert(QStringLiteral("nickName"), a.nickName());
    json.insert(QStringLiteral("displayName"), a.formattedName());
    json.insert(QStringLiteral("jobTitle"), a.title());
    json.insert(QStringLiteral("companyName"), a.organization());
    json.insert(QStringLiteral("department"), a.department());
    json.insert(QStringLiteral("personalNotes"), a.note());
    // birthday is read in toAddressee() but deliberately never written back:
    // Exchange reacts to a birthday write by creating a recurring event in the
    // Birthdays calendar, which a plain contact edit must not silently do.

    // The preferred address first, with the display name the server had for it
    // (rewriting the list without names wiped them); at most three, as Graph takes.
    const QJsonObject names = QJsonDocument::fromJson(a.custom(kCustomApp, kEmailNames).toUtf8()).object();
    Email::List emailList = a.emailList();
    std::stable_partition(emailList.begin(), emailList.end(), [](const Email &e) {
        return e.isPreferred();
    });
    QJsonArray emails;
    for (const Email &e : std::as_const(emailList)) {
        if (e.mail().isEmpty() || emails.size() >= kMaxEmails) {
            continue;
        }
        QJsonObject eo;
        eo.insert(QStringLiteral("address"), e.mail());
        const QString name = names.value(e.mail().toLower()).toString();
        if (!name.isEmpty()) {
            eo.insert(QStringLiteral("name"), name);
        }
        emails.append(eo);
    }
    json.insert(QStringLiteral("emailAddresses"), emails);

    // Graph has business and home numbers and one mobile number. The preferred mobile
    // number goes there; other mobile, fax, pager and further numbers have no place of
    // their own and go with the business numbers: left out, they would vanish locally
    // with the next sync, which brings the contact back as the server has it.
    QJsonArray business, home;
    const auto phones = a.phoneNumbers();
    QString mobile;
    for (const PhoneNumber &p : phones) {
        if (p.number().isEmpty() || !(p.type() & PhoneNumber::Cell)) {
            continue;
        }
        if (mobile.isEmpty() || p.type().testFlag(PhoneNumber::Pref)) {
            mobile = p.number();
            if (p.type().testFlag(PhoneNumber::Pref)) {
                break;
            }
        }
    }
    for (const PhoneNumber &p : phones) {
        if (p.number().isEmpty() || p.number() == mobile) {
            continue;
        }
        if ((p.type() & PhoneNumber::Home) && !(p.type() & (PhoneNumber::Fax | PhoneNumber::Pager | PhoneNumber::Cell))) {
            home.append(p.number());
        } else {
            business.append(p.number());
        }
    }
    json.insert(QStringLiteral("businessPhones"), business);
    json.insert(QStringLiteral("homePhones"), home);
    const QStringList had = serverHad(a);
    if (!mobile.isEmpty()) {
        json.insert(QStringLiteral("mobilePhone"), mobile);
    } else if (had.contains(QLatin1String("mobilePhone"))) {
        json.insert(QStringLiteral("mobilePhone"), QString());
    }

    // An address removed in KDE is emptied on the server, when it had one.
    const Address work = a.address(Address::Work);
    if (!work.isEmpty()) {
        json.insert(QStringLiteral("businessAddress"), addressToJson(work));
    } else if (had.contains(QLatin1String("businessAddress"))) {
        json.insert(QStringLiteral("businessAddress"), emptyAddress());
    }
    const Address homeAddr = a.address(Address::Home);
    if (!homeAddr.isEmpty()) {
        json.insert(QStringLiteral("homeAddress"), addressToJson(homeAddr));
    } else if (had.contains(QLatin1String("homeAddress"))) {
        json.insert(QStringLiteral("homeAddress"), emptyAddress());
    }

    json.insert(QStringLiteral("categories"), QJsonArray::fromStringList(a.categories()));
    return json;
}
}
