#include "matrix/MockMatrixClient.h"
#include "profile/ProfileFieldsManager.h"
#include "ZoneDataForTests.h"

#include <QRegularExpression>
#include <QSignalSpy>
#include <QTimeZone>
#include <QtTest>

#include <algorithm>

namespace {

struct Write {
    QString name;
    QString value;
};

class FakeFieldsClient : public MockMatrixClient
{
public:
    using MockMatrixClient::MockMatrixClient;

    bool supportsProfileFields() const override { return true; }
    void fetchProfileFields(const QString &userId, quint64 opId) override
    {
        fetches.append(userId);
        lastFetchOp = opId;
    }
    void setProfileField(const QString &name, const QString &value,
                         quint64 opId) override
    {
        writes.append({ name, value });
        lastWriteOp = opId;
    }
    QString currentUserId() const override { return self; }

    QString self = QStringLiteral("@me:example.org");
    QStringList fetches;
    QList<Write> writes;
    quint64 lastFetchOp = 0;
    quint64 lastWriteOp = 0;
};

const QString kAlice = QStringLiteral("@alice:example.org");
const QString kClock24 = QStringLiteral("HH:mm");

QDateTime utc(int year, int month, int day, int hour, int minute = 0)
{
    return QDateTime(QDate(year, month, day), QTime(hour, minute), QTimeZone::UTC);
}

QString minus(const QString &text)
{
    return QString(text).replace(QLatin1Char('-'), QChar(0x2212));
}

}

class ProfileFieldsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        ensureZoneDataForTests();
        QLocale::setDefault(QLocale::c());
        QVERIFY2(QTimeZone::isTimeZoneIdAvailable("Europe/Vilnius"),
                 "no IANA zone database on this machine (TZDIR?)");
    }

    void anOffsetIsWrittenTheWayPeopleReadIt()
    {
        QCOMPARE(ProfileFieldsManager::offsetText(0), QStringLiteral("UTC"));
        QCOMPARE(ProfileFieldsManager::offsetText(2 * 3600), QStringLiteral("UTC+2"));
        QCOMPARE(ProfileFieldsManager::offsetText(19800), QStringLiteral("UTC+5:30"));
        QCOMPARE(ProfileFieldsManager::offsetText(-3 * 3600), minus("UTC-3"));
        QCOMPARE(ProfileFieldsManager::offsetText(-12600), minus("UTC-3:30"));
        QCOMPARE(ProfileFieldsManager::offsetText(20700), QStringLiteral("UTC+5:45"));
    }

    void localTimeFollowsDaylightSaving()
    {
        const auto at = [](const char *zone, const QDateTime &when) {
            return ProfileFieldsManager::localTimeText(QLatin1String(zone), when,
                                                       kClock24);
        };
        QCOMPARE(at("Europe/Vilnius", utc(2026, 1, 15, 12)),
                 QStringLiteral("14:00 (UTC+2)"));
        QCOMPARE(at("Europe/Vilnius", utc(2026, 7, 15, 12)),
                 QStringLiteral("15:00 (UTC+3)"));
        // The switch itself: 01:00 UTC on the last Sunday of March.
        QCOMPARE(at("Europe/Vilnius", utc(2026, 3, 29, 0, 59)),
                 QStringLiteral("02:59 (UTC+2)"));
        QCOMPARE(at("Europe/Vilnius", utc(2026, 3, 29, 1, 0)),
                 QStringLiteral("04:00 (UTC+3)"));

        QCOMPARE(at("America/New_York", utc(2026, 1, 15, 12)),
                 minus("07:00 (UTC-5)"));
        QCOMPARE(at("America/New_York", utc(2026, 7, 15, 12)),
                 minus("08:00 (UTC-4)"));
        // Southern hemisphere: summer time in January.
        QCOMPARE(at("Australia/Sydney", utc(2026, 1, 15, 12)),
                 QStringLiteral("23:00 (UTC+11)"));
        QCOMPARE(at("Australia/Sydney", utc(2026, 7, 15, 12)),
                 QStringLiteral("22:00 (UTC+10)"));
        QCOMPARE(at("Asia/Kolkata", utc(2026, 7, 15, 12)),
                 QStringLiteral("17:30 (UTC+5:30)"));
        QCOMPARE(at("UTC", utc(2026, 7, 15, 12)), QStringLiteral("12:00 (UTC)"));
    }

    void localTimeUsesTheClockFormatItIsGiven()
    {
        QCOMPARE(ProfileFieldsManager::localTimeText(
                     QStringLiteral("Europe/Vilnius"), utc(2026, 1, 15, 12),
                     QStringLiteral("h:mm AP")),
                 QStringLiteral("2:00 PM (UTC+2)"));
    }

    void aNameThatIsNotAZoneGivesNoTime()
    {
        for (const char *bad : { "", "Not/AZone", "../../etc/passwd",
                                 "/etc/localtime", "europe/vilnius x" }) {
            QVERIFY2(!ProfileFieldsManager::isKnownTimezone(QLatin1String(bad)), bad);
            QCOMPARE(ProfileFieldsManager::localTimeText(
                         QLatin1String(bad), utc(2026, 1, 15, 12), kClock24),
                     QString());
        }
        QVERIFY(ProfileFieldsManager::isKnownTimezone(QStringLiteral("Europe/Vilnius")));
    }

    void theListOffersRealZonesInOrder()
    {
        ProfileFieldsManager fields;
        const QStringList zones = fields.timezones();
        QVERIFY(zones.size() > 100);
        QVERIFY(zones.contains(QStringLiteral("Europe/Vilnius")));
        QVERIFY(zones.contains(QStringLiteral("America/New_York")));
        QVERIFY(std::is_sorted(zones.cbegin(), zones.cend()));
        // Every offered zone must be one the bridge will write.
        const QRegularExpression writable(QStringLiteral("^[A-Za-z0-9_+/-]{1,64}$"));
        for (const QString &zone : zones)
            QVERIFY2(writable.match(zone).hasMatch(), qPrintable(zone));
        QVERIFY(fields.timezonesAvailable());
    }

    void fixedOffsetsAndMalformedIdsAreNotOffered()
    {
        QCOMPARE(ProfileFieldsManager::writableZones(
                     { "UTC+05:30", "Europe/Vilnius", "UTC", "UTC-14:00",
                       "Bad Zone", "/abs", "Europe/Vilnius" }),
                 (QStringList{ QStringLiteral("Europe/Vilnius"),
                               QStringLiteral("UTC") }));
        QVERIFY(!ProfileFieldsManager::isKnownTimezone(QStringLiteral("UTC+05:30")));
    }

    // What Qt lists with no zone database: UTC and its fixed offsets.
    void aMachineWithoutAZoneDatabaseHasNoZonesToOffer()
    {
        const QStringList bare = ProfileFieldsManager::writableZones(
            { "UTC", "UTC+05:30", "UTC-14:00", "UTC+00:00" });
        QCOMPARE(ProfileFieldsManager::ianaZoneCount(bare), 0);
        QCOMPARE(ProfileFieldsManager::ianaZoneCount(
                     { QStringLiteral("UTC"), QStringLiteral("Europe/Vilnius") }),
                 1);
        QVERIFY(ProfileFieldsManager::ianaZoneCount() > 100);
    }

    void aFullCacheGivesUpItsOldestAnswerAndKeepsThisAccounts()
    {
        FakeFieldsClient client;
        ProfileFieldsManager fields;
        fields.setClient(&client);
        const QVariantMap answer{ { QStringLiteral("pronouns"),
                                    QStringLiteral("she/her") } };
        fields.requestOwn();
        Q_EMIT client.profileFieldsReceived(client.lastFetchOp, client.self, true,
                                            answer);
        const auto user = [](int i) {
            return QStringLiteral("@u%1:example.org").arg(i);
        };
        for (int i = 0; i < 300; ++i) {
            fields.request(user(i));
            QCOMPARE(client.fetches.last(), user(i));
            Q_EMIT client.profileFieldsReceived(client.lastFetchOp, user(i), true,
                                                answer);
        }
        QVERIFY(!fields.fieldsFor(user(299)).isEmpty());
        QVERIFY(fields.fieldsFor(user(0)).isEmpty());
        QVERIFY(!fields.fieldsFor(client.self).isEmpty());
        // An evicted user is asked again.
        const int before = client.fetches.size();
        fields.request(user(0));
        QCOMPARE(client.fetches.size(), before + 1);
    }

    void aUserIsAskedOnceAndTheAnswerIsServed()
    {
        FakeFieldsClient client;
        ProfileFieldsManager fields;
        fields.setClient(&client);
        QVERIFY(fields.available());
        QVERIFY(fields.fieldsFor(kAlice).isEmpty());

        fields.request(kAlice);
        fields.request(kAlice);
        QCOMPARE(client.fetches, QStringList{ kAlice });

        QSignalSpy revisions(&fields, &ProfileFieldsManager::revisionChanged);
        Q_EMIT client.profileFieldsReceived(
            client.lastFetchOp, kAlice, true,
            { { QStringLiteral("timezone"), QStringLiteral("Asia/Kolkata") },
              { QStringLiteral("pronouns"), QStringLiteral("she/her") } });
        QCOMPARE(revisions.count(), 1);
        QCOMPARE(fields.fieldsFor(kAlice).value(QStringLiteral("pronouns")).toString(),
                 QStringLiteral("she/her"));
        const QString shown = fields.localTimeFor(kAlice, kClock24);
        QVERIFY2(shown.endsWith(QStringLiteral(" (UTC+5:30)")), qPrintable(shown));
    }

    void aZoneThisMachineDoesNotKnowIsTreatedAsNotSet()
    {
        FakeFieldsClient client;
        ProfileFieldsManager fields;
        fields.setClient(&client);
        fields.request(kAlice);
        Q_EMIT client.profileFieldsReceived(
            client.lastFetchOp, kAlice, true,
            { { QStringLiteral("timezone"), QStringLiteral("Mars/Olympus_Mons") } });
        QCOMPARE(fields.fieldsFor(kAlice).value(QStringLiteral("timezone")).toString(),
                 QString());
        QCOMPARE(fields.localTimeFor(kAlice, kClock24), QString());
    }

    void aFailedFetchIsNotCachedAndAnAnswerForAnotherOpIsIgnored()
    {
        FakeFieldsClient client;
        ProfileFieldsManager fields;
        fields.setClient(&client);
        fields.request(kAlice);
        Q_EMIT client.profileFieldsReceived(
            client.lastFetchOp + 7, kAlice, true,
            { { QStringLiteral("pronouns"), QStringLiteral("stale") } });
        QVERIFY(fields.fieldsFor(kAlice).isEmpty());

        Q_EMIT client.profileFieldsReceived(client.lastFetchOp, kAlice, false, {});
        QVERIFY(fields.fieldsFor(kAlice).isEmpty());
        fields.request(kAlice);
        QCOMPARE(client.fetches.size(), 2);
    }

    void pickingAZoneWritesTheTimezoneFieldAndNotSetClearsIt()
    {
        FakeFieldsClient client;
        ProfileFieldsManager fields;
        fields.setClient(&client);

        fields.setOwnTimezone(QStringLiteral("Europe/Vilnius"));
        QCOMPARE(client.writes.size(), 1);
        QCOMPARE(client.writes.last().name, QStringLiteral("timezone"));
        QCOMPARE(client.writes.last().value, QStringLiteral("Europe/Vilnius"));
        QVERIFY(fields.busy());

        // One write at a time.
        fields.setOwnPronouns(QStringLiteral("they/them"));
        QCOMPARE(client.writes.size(), 1);

        Q_EMIT client.profileFieldSet(client.lastWriteOp, true,
                                      QStringLiteral("timezone"), QString());
        QVERIFY(!fields.busy());
        // A successful write re-reads the profile rather than trusting itself.
        QCOMPARE(client.fetches, QStringList{ client.self });
        Q_EMIT client.profileFieldsReceived(
            client.lastFetchOp, client.self, true,
            { { QStringLiteral("timezone"), QStringLiteral("Europe/Vilnius") } });
        QCOMPARE(fields.ownTimezone(), QStringLiteral("Europe/Vilnius"));

        fields.setOwnTimezone(QString());
        QCOMPARE(client.writes.size(), 2);
        QCOMPARE(client.writes.last().name, QStringLiteral("timezone"));
        QCOMPARE(client.writes.last().value, QString());
    }

    void aZoneOutsideTheListIsNeverSent()
    {
        FakeFieldsClient client;
        ProfileFieldsManager fields;
        fields.setClient(&client);
        fields.setOwnTimezone(QStringLiteral("Mars/Olympus_Mons"));
        QVERIFY(client.writes.isEmpty());
        QCOMPARE(fields.lastError(), QStringLiteral("invalid_value"));
        QVERIFY(!fields.busy());
    }

    void pronounsAreWrittenToThePronounsField()
    {
        FakeFieldsClient client;
        ProfileFieldsManager fields;
        fields.setClient(&client);
        fields.setOwnPronouns(QStringLiteral("  she/her, de:sie/ihr "));
        QCOMPARE(client.writes.size(), 1);
        QCOMPARE(client.writes.last().name, QStringLiteral("pronouns"));
        QCOMPARE(client.writes.last().value, QStringLiteral("she/her, de:sie/ihr"));

        Q_EMIT client.profileFieldSet(client.lastWriteOp, true,
                                      QStringLiteral("pronouns"), QString());
        Q_EMIT client.profileFieldsReceived(
            client.lastFetchOp, client.self, true,
            { { QStringLiteral("pronouns"), QStringLiteral("she/her, sie/ihr") },
              { QStringLiteral("pronouns_input"),
                QStringLiteral("she/her, de:sie/ihr") } });
        QCOMPARE(fields.ownPronouns(), QStringLiteral("she/her, de:sie/ihr"));
    }

    void theCapabilityAndARefusedWriteBothCloseTheEditor()
    {
        FakeFieldsClient client;
        ProfileFieldsManager fields;
        fields.setClient(&client);
        QVERIFY(fields.canEditTimezone() && fields.canEditPronouns());

        fields.requestOwn();
        Q_EMIT client.profileFieldsReceived(
            client.lastFetchOp, client.self, true,
            { { QStringLiteral("can_edit_timezone"), false },
              { QStringLiteral("can_edit_pronouns"), true } });
        QVERIFY(!fields.canEditTimezone());
        QVERIFY(fields.canEditPronouns());

        fields.setOwnPronouns(QStringLiteral("they/them"));
        Q_EMIT client.profileFieldSet(client.lastWriteOp, false,
                                      QStringLiteral("pronouns"),
                                      QStringLiteral("unsupported"));
        QVERIFY(!fields.canEditPronouns());
        QCOMPARE(fields.lastError(), QStringLiteral("unsupported"));
        QVERIFY(!fields.busy());
    }

    void anotherAccountNeverSeesTheLastOnesAnswers()
    {
        FakeFieldsClient client;
        ProfileFieldsManager fields;
        fields.setClient(&client);
        fields.request(kAlice);
        Q_EMIT client.profileFieldsReceived(
            client.lastFetchOp, kAlice, true,
            { { QStringLiteral("pronouns"), QStringLiteral("she/her") } });
        QVERIFY(!fields.fieldsFor(kAlice).isEmpty());
        Q_EMIT client.loggedOut();
        QVERIFY(fields.fieldsFor(kAlice).isEmpty());
        fields.request(kAlice);
        QCOMPARE(client.fetches.size(), 2);
    }
};

QTEST_GUILESS_MAIN(ProfileFieldsTest)
#include "ProfileFieldsTest.moc"
