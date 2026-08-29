// Profile bios (MSC4440 over MSC4133) and the decorative badge table.
//
// The wire format and the sanitizing live in rust/src/bio.rs and are covered
// there. This is the POLICY half, and what it pins is the honesty:
//
//   * "this user has no bio", "we have not asked yet" and "this homeserver
//     does not implement extended profile fields" are three different facts
//     that all render as NOTHING, and only the third stops the client asking;
//   * an absent field is M_NOT_FOUND — a server answering correctly — and is
//     never an error, never a latch, and never a placeholder;
//   * nothing is applied optimistically: `ownBio` changes only once the
//     server has accepted the write, and it takes the value the WRITE PATH
//     reports, which is the bounded text actually stored;
//   * a badge is a fixed local table lookup with no session, no network and
//     no Matrix state behind it.

#include "matrix/MockMatrixClient.h"
#include "profile/ProfileBadges.h"
#include "profile/ProfileBioManager.h"

#include <QSignalSpy>
#include <QtTest>

namespace {

struct RecordedFetch {
    QString userId;
    quint64 opId = 0;
};

class FakeBioClient : public MockMatrixClient
{
public:
    using MockMatrixClient::MockMatrixClient;

    bool supportsProfileBios() const override { return supports; }
    void fetchProfileBio(const QString &userId, quint64 opId) override
    {
        fetches.append({ userId, opId });
    }
    void setProfileBio(const QString &text, quint64 opId) override
    {
        writes.append(text);
        lastWriteOp = opId;
    }
    QString currentUserId() const override { return self; }

    bool supports = true;
    QString self = QStringLiteral("@me:example.org");
    QList<RecordedFetch> fetches;
    QStringList writes;
    quint64 lastWriteOp = 0;
};

const QString kAlice = QStringLiteral("@alice:example.org");

} // namespace

class ProfileBioBadgeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void anUnknownUserRendersNothingAndIsAskedExactlyOnce()
    {
        FakeBioClient client;
        ProfileBioManager bios;
        bios.setClient(&client);
        QVERIFY(bios.available());

        // Not asked yet is not "no bio"; both are "".
        QCOMPARE(bios.bioFor(kAlice), QString());

        bios.request(kAlice);
        QCOMPARE(client.fetches.size(), 1);
        // A profile card that opens, closes and opens again must not cost a
        // second request.
        bios.request(kAlice);
        bios.request(kAlice);
        QCOMPARE(client.fetches.size(), 1);

        QSignalSpy revisions(&bios, &ProfileBioManager::revisionChanged);
        Q_EMIT client.profileBioReceived(client.fetches.first().opId, kAlice,
                                         QStringLiteral("hello\nworld"), true);
        QCOMPARE(bios.bioFor(kAlice), QStringLiteral("hello\nworld"));
        QCOMPARE(revisions.count(), 1);
    }

    // THE distinction the whole feature turns on: a user with no bio answers
    // M_NOT_FOUND, which the Rust side reports as supported == true with an
    // empty string. It must not look like a server without extended profiles,
    // because that one hides the editing surface for everybody.
    void anAbsentBioIsNotAnUnsupportedServer()
    {
        FakeBioClient client;
        ProfileBioManager bios;
        bios.setClient(&client);

        bios.request(kAlice);
        QSignalSpy supported(&bios, &ProfileBioManager::supportedChanged);
        Q_EMIT client.profileBioReceived(client.fetches.first().opId, kAlice,
                                         QString(), true);
        QCOMPARE(bios.bioFor(kAlice), QString());
        QVERIFY(bios.supported());
        QCOMPARE(supported.count(), 0);
        // ...and the client keeps asking about OTHER users.
        bios.request(QStringLiteral("@bob:example.org"));
        QCOMPARE(client.fetches.size(), 2);
    }

    void aServerWithoutExtendedProfilesLatchesAndStopsBeingAsked()
    {
        FakeBioClient client;
        ProfileBioManager bios;
        bios.setClient(&client);

        bios.request(kAlice);
        QSignalSpy supported(&bios, &ProfileBioManager::supportedChanged);
        Q_EMIT client.profileBioReceived(client.fetches.first().opId, kAlice,
                                         QString(), false);
        QVERIFY(!bios.supported());
        QCOMPARE(supported.count(), 1);

        // Latched: every further request would ask the same question and get
        // the same answer.
        bios.request(QStringLiteral("@bob:example.org"));
        bios.refresh(kAlice);
        QCOMPARE(client.fetches.size(), 1);
    }

    // A stale answer belongs to a session that is gone. It must not populate
    // the next account's cards.
    void anAnswerForAnUnknownOpIsDropped()
    {
        FakeBioClient client;
        ProfileBioManager bios;
        bios.setClient(&client);
        bios.request(kAlice);

        Q_EMIT client.profileBioReceived(9999, kAlice,
                                         QStringLiteral("ghost"), true);
        QCOMPARE(bios.bioFor(kAlice), QString());
        // ...and a real answer for the same op is still delivered exactly
        // once — the drop above must not have consumed the in-flight record.
        Q_EMIT client.profileBioReceived(client.fetches.first().opId, kAlice,
                                         QStringLiteral("real"), true);
        QCOMPARE(bios.bioFor(kAlice), QStringLiteral("real"));
    }

    void aWriteIsNeverAppliedOptimisticallyAndTakesTheStoredValue()
    {
        FakeBioClient client;
        ProfileBioManager bios;
        bios.setClient(&client);

        bios.setOwnBio(QStringLiteral("  typed  "));
        QCOMPARE(client.writes.size(), 1);
        QVERIFY(bios.busy());
        // NOT applied yet: the server has not accepted anything.
        QCOMPARE(bios.ownBio(), QString());
        // ...and a second write cannot be started on top of the first.
        bios.setOwnBio(QStringLiteral("again"));
        QCOMPARE(client.writes.size(), 1);

        // The value cached is what the WRITE PATH reports actually stored —
        // the bounded, sanitized text, not what was typed.
        Q_EMIT client.profileBioSet(client.lastWriteOp, true,
                                    QStringLiteral("typed"), QString());
        QVERIFY(!bios.busy());
        QCOMPARE(bios.ownBio(), QStringLiteral("typed"));
        QCOMPARE(bios.lastError(), QString());
    }

    void aRefusedWriteReportsAndChangesNothing()
    {
        FakeBioClient client;
        ProfileBioManager bios;
        bios.setClient(&client);

        bios.setOwnBio(QStringLiteral("first"));
        Q_EMIT client.profileBioSet(client.lastWriteOp, true,
                                    QStringLiteral("first"), QString());
        QCOMPARE(bios.ownBio(), QStringLiteral("first"));

        bios.setOwnBio(QStringLiteral("second"));
        Q_EMIT client.profileBioSet(client.lastWriteOp, false, QString(),
                                    QStringLiteral("forbidden"));
        // The stored value is untouched, and the failure is disclosed.
        QCOMPARE(bios.ownBio(), QStringLiteral("first"));
        QCOMPARE(bios.lastError(), QStringLiteral("forbidden"));
        QVERIFY(bios.supported());

        // A write that came back UNRECOGNISED settles the capability
        // question the same way a read does, so the account is not invited
        // to fail at the same write for the rest of the session.
        bios.setOwnBio(QStringLiteral("third"));
        Q_EMIT client.profileBioSet(client.lastWriteOp, false, QString(),
                                    QStringLiteral("unsupported"));
        QVERIFY(!bios.supported());
    }

    // A bio belongs to the account that fetched it.
    void signingOutDropsEveryAnswer()
    {
        FakeBioClient client;
        ProfileBioManager bios;
        bios.setClient(&client);
        bios.request(kAlice);
        Q_EMIT client.profileBioReceived(client.fetches.first().opId, kAlice,
                                         QStringLiteral("mine"), true);
        QCOMPARE(bios.bioFor(kAlice), QStringLiteral("mine"));

        Q_EMIT client.loggedOut();
        QCOMPARE(bios.bioFor(kAlice), QString());
        // ...and the same user is asked about again for the new session.
        bios.request(kAlice);
        QCOMPARE(client.fetches.size(), 2);
    }

    void aBackendWithoutBiosOffersNothing()
    {
        FakeBioClient client;
        client.supports = false;
        ProfileBioManager bios;
        bios.setClient(&client);
        QVERIFY(!bios.available());
        bios.request(kAlice);
        bios.setOwnBio(QStringLiteral("hello"));
        bios.clearOwnBio();
        QCOMPARE(client.fetches.size(), 0);
        QCOMPARE(client.writes.size(), 0);
    }

    // --- the badge table ------------------------------------------------

    void theBadgeIsATableAndAlmostEveryUserHasNone()
    {
        ProfileBadges badges;
        QVERIFY(!ProfileBadges::badges().isEmpty());
        for (const ProfileBadges::Badge &badge : ProfileBadges::badges()) {
            QVERIFY2(!badge.userId.isEmpty(), "a badge with no holder");
            QVERIFY2(badge.userId.startsWith(QLatin1Char('@')),
                     qPrintable(badge.userId));
            QVERIFY2(!badge.label.isEmpty(), qPrintable(badge.userId));
            // Spoken instead of the bare label: a decorative tag beside a
            // name is exactly what a reader assumes is a permission.
            QVERIFY2(!badge.description.isEmpty(), qPrintable(badge.userId));
            QVERIFY(badges.hasBadge(badge.userId));
            QCOMPARE(badges.labelFor(badge.userId), badge.label);
            QCOMPARE(badges.descriptionFor(badge.userId), badge.description);
            const QVariantMap map = badges.badgeFor(badge.userId);
            QCOMPARE(map.value(QStringLiteral("label")).toString(), badge.label);
            QCOMPARE(map.value(QStringLiteral("description")).toString(),
                     badge.description);
        }

        // The ordinary answer, and it must be empty rather than a placeholder.
        QVERIFY(!badges.hasBadge(kAlice));
        QCOMPARE(badges.labelFor(kAlice), QString());
        QCOMPARE(badges.descriptionFor(kAlice), QString());
        QVERIFY(badges.badgeFor(kAlice).isEmpty());
        QCOMPARE(badges.labelFor(QString()), QString());
    }

    // Matrix localparts are case-sensitive. Guessing at equivalence between
    // two ids is how the wrong person gets somebody else's badge.
    void aBadgeIsNeverGivenToASimilarLookingId()
    {
        ProfileBadges badges;
        const QString holder = ProfileBadges::badges().first().userId;
        QVERIFY(badges.hasBadge(holder));
        QVERIFY(!badges.hasBadge(holder.toUpper()));
        QVERIFY(!badges.hasBadge(holder + QStringLiteral("x")));
        QVERIFY(!badges.hasBadge(holder.mid(1)));
        // A different server, same localpart, is a different person.
        const int colon = holder.indexOf(QLatin1Char(':'));
        QVERIFY(colon > 0);
        QVERIFY(!badges.hasBadge(holder.left(colon)
                                 + QStringLiteral(":elsewhere.example")));
    }

    // The thank-you the round exists for.
    void theRecordedBadgeIsTheOneThatWasAwarded()
    {
        ProfileBadges badges;
        QCOMPARE(badges.labelFor(
                     QStringLiteral("@romanticanimegerl:cutefunny.art")),
                 QStringLiteral("idea master"));
    }

    // A badge is decoration. Nothing in its description may claim a
    // permission, a role or a verification state.
    void noBadgeDescriptionClaimsAPermissionOrAVerification()
    {
        for (const ProfileBadges::Badge &badge : ProfileBadges::badges()) {
            const QString lowered = badge.description.toLower();
            QVERIFY2(lowered.contains(QStringLiteral("not a moderation")),
                     qPrintable(badge.description));
            QVERIFY2(lowered.contains(QStringLiteral("not a verification")),
                     qPrintable(badge.description));
            const QString label = badge.label.toLower();
            for (const char *banned : { "verified", "admin", "moderator",
                                        "official", "staff", "trusted" }) {
                QVERIFY2(!label.contains(QLatin1String(banned)),
                         qPrintable(badge.label));
            }
        }
    }
};

QTEST_GUILESS_MAIN(ProfileBioBadgeTest)
#include "ProfileBioBadgeTest.moc"
