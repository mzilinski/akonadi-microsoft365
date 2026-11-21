/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    Unit tests for the Graph contact JSON <-> KContacts::Addressee mapping.
*/

#include "contact/graphcontacthandler.h"

#include <KContacts/Address>
#include <KContacts/PhoneNumber>
#include <KContacts/VCardConverter>

#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

using namespace KContacts;

namespace
{
QJsonObject sampleContact()
{
    QJsonObject json;
    json.insert(QStringLiteral("givenName"), QStringLiteral("Jürgen"));
    json.insert(QStringLiteral("surname"), QStringLiteral("Müßig"));
    json.insert(QStringLiteral("middleName"), QStringLiteral("K."));
    json.insert(QStringLiteral("title"), QStringLiteral("Dr."));
    json.insert(QStringLiteral("nickName"), QStringLiteral("Jü"));
    json.insert(QStringLiteral("displayName"), QStringLiteral("Dr. Jürgen Müßig"));
    json.insert(QStringLiteral("jobTitle"), QStringLiteral("Engineer"));
    json.insert(QStringLiteral("companyName"), QStringLiteral("Example GmbH"));
    json.insert(QStringLiteral("department"), QStringLiteral("R&D"));
    json.insert(QStringLiteral("personalNotes"), QStringLiteral("Prefers e-mail"));

    QJsonArray emails;
    QJsonObject email1;
    email1.insert(QStringLiteral("address"), QStringLiteral("juergen@example.com"));
    QJsonObject email2;
    email2.insert(QStringLiteral("address"), QStringLiteral("j.muessig@example.org"));
    emails.append(email1);
    emails.append(email2);
    json.insert(QStringLiteral("emailAddresses"), emails);

    json.insert(QStringLiteral("businessPhones"), QJsonArray{QStringLiteral("+49 30 123456")});
    json.insert(QStringLiteral("homePhones"), QJsonArray{QStringLiteral("+49 30 654321")});
    json.insert(QStringLiteral("mobilePhone"), QStringLiteral("+49 170 1234567"));

    QJsonObject home;
    home.insert(QStringLiteral("street"), QStringLiteral("Musterstraße 1"));
    home.insert(QStringLiteral("city"), QStringLiteral("Berlin"));
    home.insert(QStringLiteral("state"), QStringLiteral("Berlin"));
    home.insert(QStringLiteral("postalCode"), QStringLiteral("10115"));
    home.insert(QStringLiteral("countryOrRegion"), QStringLiteral("Germany"));
    json.insert(QStringLiteral("homeAddress"), home);

    json.insert(QStringLiteral("birthday"), QStringLiteral("1980-05-15T00:00:00Z"));
    json.insert(QStringLiteral("categories"), QJsonArray{QStringLiteral("Familie"), QStringLiteral("Verein")});
    return json;
}
} // namespace

class GraphContactHandlerTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void shouldMapNamesAndOrganization()
    {
        const Addressee a = GraphContactHandler::toAddressee(sampleContact());
        QCOMPARE(a.givenName(), QStringLiteral("Jürgen"));
        QCOMPARE(a.familyName(), QStringLiteral("Müßig"));
        QCOMPARE(a.additionalName(), QStringLiteral("K."));
        QCOMPARE(a.prefix(), QStringLiteral("Dr."));
        QCOMPARE(a.nickName(), QStringLiteral("Jü"));
        QCOMPARE(a.formattedName(), QStringLiteral("Dr. Jürgen Müßig"));
        QCOMPARE(a.title(), QStringLiteral("Engineer"));
        QCOMPARE(a.organization(), QStringLiteral("Example GmbH"));
        QCOMPARE(a.department(), QStringLiteral("R&D"));
        QCOMPARE(a.note(), QStringLiteral("Prefers e-mail"));
    }

    void shouldMapEmailsWithFirstPreferred()
    {
        const Addressee a = GraphContactHandler::toAddressee(sampleContact());
        const auto emails = a.emailList();
        QCOMPARE(emails.size(), 2);
        QCOMPARE(emails.at(0).mail(), QStringLiteral("juergen@example.com"));
        QVERIFY(emails.at(0).isPreferred());
        QCOMPARE(emails.at(1).mail(), QStringLiteral("j.muessig@example.org"));
        QVERIFY(!emails.at(1).isPreferred());
    }

    void shouldMapPhones()
    {
        const Addressee a = GraphContactHandlerTest::addressee();
        QCOMPARE(a.phoneNumbers().size(), 3);
        QCOMPARE(a.phoneNumber(PhoneNumber::Work).number(), QStringLiteral("+49 30 123456"));
        QCOMPARE(a.phoneNumber(PhoneNumber::Home).number(), QStringLiteral("+49 30 654321"));
        QCOMPARE(a.phoneNumber(PhoneNumber::Cell).number(), QStringLiteral("+49 170 1234567"));
    }

    void shouldMapAddressAndBirthday()
    {
        const Addressee a = GraphContactHandlerTest::addressee();
        const Address home = a.address(Address::Home);
        QCOMPARE(home.street(), QStringLiteral("Musterstraße 1"));
        QCOMPARE(home.locality(), QStringLiteral("Berlin"));
        QCOMPARE(home.postalCode(), QStringLiteral("10115"));
        QCOMPARE(home.country(), QStringLiteral("Germany"));
        QCOMPARE(a.birthday().date(), QDate(1980, 5, 15));
    }

    void shouldMapCategories()
    {
        const Addressee a = GraphContactHandlerTest::addressee();
        QCOMPARE(a.categories(), QStringList({QStringLiteral("Familie"), QStringLiteral("Verein")}));
    }

    void shouldRoundTripThroughJson()
    {
        const QJsonObject json = sampleContact();
        const QJsonObject back = GraphContactHandler::toJson(GraphContactHandler::toAddressee(json));

        QCOMPARE(back.value(QLatin1String("givenName")), json.value(QLatin1String("givenName")));
        QCOMPARE(back.value(QLatin1String("surname")), json.value(QLatin1String("surname")));
        QCOMPARE(back.value(QLatin1String("displayName")), json.value(QLatin1String("displayName")));
        QCOMPARE(back.value(QLatin1String("companyName")), json.value(QLatin1String("companyName")));
        QCOMPARE(back.value(QLatin1String("emailAddresses")), json.value(QLatin1String("emailAddresses")));
        QCOMPARE(back.value(QLatin1String("businessPhones")), json.value(QLatin1String("businessPhones")));
        QCOMPARE(back.value(QLatin1String("homePhones")), json.value(QLatin1String("homePhones")));
        QCOMPARE(back.value(QLatin1String("mobilePhone")), json.value(QLatin1String("mobilePhone")));
        QCOMPARE(back.value(QLatin1String("homeAddress")), json.value(QLatin1String("homeAddress")));
        QCOMPARE(back.value(QLatin1String("categories")), json.value(QLatin1String("categories")));
    }

    void shouldUseVCardMimeType()
    {
        QCOMPARE(GraphContactHandler::mimeType(), Addressee::mimeType());
    }

private:
    static Addressee addressee()
    {
        return GraphContactHandler::toAddressee(sampleContact());
    }

    // Review 2026-11, I5: the contact round-trip lost data.
    static QJsonObject serverContact()
    {
        QJsonObject json;
        json.insert(QStringLiteral("id"), QStringLiteral("C1"));
        json.insert(QStringLiteral("givenName"), QStringLiteral("Ada"));
        json.insert(
            QStringLiteral("emailAddresses"),
            QJsonArray{QJsonObject{{QStringLiteral("address"), QStringLiteral("ada@example.org")}, {QStringLiteral("name"), QStringLiteral("Ada Lovelace")}},
                       QJsonObject{{QStringLiteral("address"), QStringLiteral("ada@work.example")}, {QStringLiteral("name"), QStringLiteral("Ada (Work)")}}});
        json.insert(QStringLiteral("mobilePhone"), QStringLiteral("+49 170 1"));
        json.insert(QStringLiteral("homeAddress"),
                    QJsonObject{{QStringLiteral("street"), QStringLiteral("Main St 1")}, {QStringLiteral("city"), QStringLiteral("Berlin")}});
        return json;
    }

    // As Akonadi stores it: through vCard and back.
    static KContacts::Addressee throughVCard(const KContacts::Addressee &a)
    {
        KContacts::VCardConverter converter;
        return converter.parseVCard(converter.createVCard(a));
    }

private Q_SLOTS:
    void shouldKeepTheDisplayNamesOfEmailAddresses()
    {
        const KContacts::Addressee a = throughVCard(GraphContactHandler::toAddressee(serverContact()));
        const QJsonArray emails = GraphContactHandler::toJson(a).value(QLatin1String("emailAddresses")).toArray();
        QCOMPARE(emails.size(), 2);
        QCOMPARE(emails.at(0).toObject().value(QLatin1String("name")).toString(), QStringLiteral("Ada Lovelace"));
        QCOMPARE(emails.at(1).toObject().value(QLatin1String("name")).toString(), QStringLiteral("Ada (Work)"));
    }

    void shouldSendAtMostThreeEmailAddressesPreferredFirst()
    {
        KContacts::Addressee a;
        for (int i = 0; i < 4; ++i) {
            KContacts::Email email(QStringLiteral("a%1@example.org").arg(i));
            email.setPreferred(i == 3);
            a.addEmail(email);
        }
        const QJsonArray emails = GraphContactHandler::toJson(a).value(QLatin1String("emailAddresses")).toArray();
        QCOMPARE(emails.size(), 3);
        QCOMPARE(emails.at(0).toObject().value(QLatin1String("address")).toString(), QStringLiteral("a3@example.org"));
    }

    void shouldKeepEveryNumberAndSendThePreferredMobile()
    {
        // Numbers Graph has no field for go with the business numbers: left out, the
        // next sync would drop them locally (found in review).
        KContacts::Addressee a;
        a.insertPhoneNumber(KContacts::PhoneNumber(QStringLiteral("1"), KContacts::PhoneNumber::Work));
        a.insertPhoneNumber(KContacts::PhoneNumber(QStringLiteral("2"), KContacts::PhoneNumber::Work | KContacts::PhoneNumber::Fax));
        a.insertPhoneNumber(KContacts::PhoneNumber(QStringLiteral("3"), KContacts::PhoneNumber::Pager));
        a.insertPhoneNumber(KContacts::PhoneNumber(QStringLiteral("4"), KContacts::PhoneNumber::Cell));
        a.insertPhoneNumber(KContacts::PhoneNumber(QStringLiteral("5"), KContacts::PhoneNumber::Cell | KContacts::PhoneNumber::Pref));
        a.insertPhoneNumber(KContacts::PhoneNumber(QStringLiteral("6"), KContacts::PhoneNumber::Home));
        const QJsonObject json = GraphContactHandler::toJson(a);
        QCOMPARE(json.value(QLatin1String("mobilePhone")).toString(), QStringLiteral("5"));
        // Two business numbers at most (Graph rejects three, live-checked): the work
        // number first, then the fax; what is left takes the free home place, then is
        // given up (and named in a warning).
        QCOMPARE(json.value(QLatin1String("businessPhones")).toArray(), (QJsonArray{QStringLiteral("1"), QStringLiteral("2")}));
        QCOMPARE(json.value(QLatin1String("homePhones")).toArray(), (QJsonArray{QStringLiteral("6"), QStringLiteral("3")}));
        QCOMPARE(GraphContactHandler::phonesBeyondLimit(a), QStringList{QStringLiteral("4")});
    }

    void shouldNeverSendMoreThanTwoNumbersOfAKind()
    {
        KContacts::Addressee a;
        for (int i = 0; i < 3; ++i) {
            a.insertPhoneNumber(KContacts::PhoneNumber(QStringLiteral("w%1").arg(i), KContacts::PhoneNumber::Work));
            a.insertPhoneNumber(KContacts::PhoneNumber(QStringLiteral("h%1").arg(i), KContacts::PhoneNumber::Home));
        }
        const QJsonObject json = GraphContactHandler::toJson(a);
        QCOMPARE(json.value(QLatin1String("businessPhones")).toArray().size(), 2);
        QCOMPARE(json.value(QLatin1String("homePhones")).toArray().size(), 2);
        QCOMPARE(GraphContactHandler::phonesBeyondLimit(a).size(), 2);
    }

    void shouldKeepAWorkNumberEqualToTheMobileOne()
    {
        // Found in review: the same number as work and as mobile lost the work entry.
        KContacts::Addressee a;
        a.insertPhoneNumber(KContacts::PhoneNumber(QStringLiteral("5"), KContacts::PhoneNumber::Work));
        a.insertPhoneNumber(KContacts::PhoneNumber(QStringLiteral("5"), KContacts::PhoneNumber::Cell));
        const QJsonObject json = GraphContactHandler::toJson(a);
        QCOMPARE(json.value(QLatin1String("mobilePhone")).toString(), QStringLiteral("5"));
        QCOMPARE(json.value(QLatin1String("businessPhones")).toArray(), QJsonArray{QStringLiteral("5")});
    }

    // Found in the fourth review: when places run out, the preferred number and real
    // home numbers come first, and the same number twice takes one place.
    void shouldKeepThePreferredNumberWhenPlacesRunOut()
    {
        KContacts::Addressee a;
        a.insertPhoneNumber(KContacts::PhoneNumber(QStringLiteral("w1"), KContacts::PhoneNumber::Work));
        a.insertPhoneNumber(KContacts::PhoneNumber(QStringLiteral("w2"), KContacts::PhoneNumber::Work));
        a.insertPhoneNumber(KContacts::PhoneNumber(QStringLiteral("w3"), KContacts::PhoneNumber::Work | KContacts::PhoneNumber::Pref));
        a.insertPhoneNumber(KContacts::PhoneNumber(QStringLiteral("h1"), KContacts::PhoneNumber::Home));
        a.insertPhoneNumber(KContacts::PhoneNumber(QStringLiteral("h2"), KContacts::PhoneNumber::Home));
        const QJsonObject json = GraphContactHandler::toJson(a);
        QVERIFY(json.value(QLatin1String("businessPhones")).toArray().contains(QStringLiteral("w3")));
        QCOMPARE(GraphContactHandler::phonesBeyondLimit(a), QStringList{QStringLiteral("w2")});
    }

    void shouldPlaceAHomeNumberBeforeAFax()
    {
        KContacts::Addressee a;
        a.insertPhoneNumber(KContacts::PhoneNumber(QStringLiteral("h1"), KContacts::PhoneNumber::Home));
        a.insertPhoneNumber(KContacts::PhoneNumber(QStringLiteral("h2"), KContacts::PhoneNumber::Home));
        a.insertPhoneNumber(KContacts::PhoneNumber(QStringLiteral("h3"), KContacts::PhoneNumber::Home));
        a.insertPhoneNumber(KContacts::PhoneNumber(QStringLiteral("w1"), KContacts::PhoneNumber::Work));
        a.insertPhoneNumber(KContacts::PhoneNumber(QStringLiteral("f1"), KContacts::PhoneNumber::Work | KContacts::PhoneNumber::Fax));
        const QJsonObject json = GraphContactHandler::toJson(a);
        QCOMPARE(json.value(QLatin1String("businessPhones")).toArray(), (QJsonArray{QStringLiteral("w1"), QStringLiteral("h3")}));
        QCOMPARE(GraphContactHandler::phonesBeyondLimit(a), QStringList{QStringLiteral("f1")});
    }

    void shouldTakeTheSameNumberOnceWithinAKind()
    {
        KContacts::Addressee a;
        a.insertPhoneNumber(KContacts::PhoneNumber(QStringLiteral("+49 30 123"), KContacts::PhoneNumber::Work));
        a.insertPhoneNumber(KContacts::PhoneNumber(QStringLiteral("+49 (30) 123"), KContacts::PhoneNumber::Work));
        a.insertPhoneNumber(KContacts::PhoneNumber(QStringLiteral("+49 30 456"), KContacts::PhoneNumber::Work));
        const QJsonObject json = GraphContactHandler::toJson(a);
        QCOMPARE(json.value(QLatin1String("businessPhones")).toArray(), (QJsonArray{QStringLiteral("+49 30 123"), QStringLiteral("+49 30 456")}));
        QVERIFY(GraphContactHandler::phonesBeyondLimit(a).isEmpty());
    }

    void shouldNameTheEmailAddressesGraphCannotKeep()
    {
        KContacts::Addressee a;
        for (int i = 0; i < 4; ++i) {
            a.addEmail(KContacts::Email(QStringLiteral("a%1@example.org").arg(i)));
        }
        QCOMPARE(GraphContactHandler::emailsBeyondLimit(a), QStringList{QStringLiteral("a3@example.org")});
    }

    void shouldClearARemovedAddressAndMobileTheServerHad()
    {
        KContacts::Addressee a = throughVCard(GraphContactHandler::toAddressee(serverContact()));
        a.removeAddress(a.address(KContacts::Address::Home));
        const auto phones = a.phoneNumbers();
        for (const auto &p : phones) {
            a.removePhoneNumber(p);
        }
        const QJsonObject json = GraphContactHandler::toJson(a);
        QVERIFY(json.contains(QLatin1String("homeAddress")));
        QCOMPARE(json.value(QLatin1String("homeAddress")).toObject().value(QLatin1String("street")).toString(), QString());
        QVERIFY(json.contains(QLatin1String("mobilePhone")));
        QCOMPARE(json.value(QLatin1String("mobilePhone")).toString(), QString());
        // Nothing to clear that the server never had.
        QVERIFY(!json.contains(QLatin1String("businessAddress")));
    }
};

QTEST_GUILESS_MAIN(GraphContactHandlerTest)

#include "graphcontacthandlertest.moc"
