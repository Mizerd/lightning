// A call must never quietly downgrade an encrypted room to cleartext.
// Publishing in the clear while the peer runs its frame decryptor loses all
// media. `m.room.encryption` cannot be removed in Matrix, so a read claiming
// a known-encrypted room is now plaintext is a stale or partial view and is
// refused.

#include "calls/RtcController.h"

#include <QSignalSpy>
#include <QtTest>

class CallEncryptionStateTest : public QObject
{
    Q_OBJECT

private slots:
    /// Unknown fails closed: an unknown room is treated as encrypted, so the
    /// failure mode is a refused call, never a cleartext one.
    void anUnknownRoomIsTreatedAsEncrypted()
    {
        RtcController rtc;
        QVERIFY(rtc.roomEncrypted(QStringLiteral("!never-seen:example.org")));
    }

    /// A genuinely unencrypted room can still say so; only the reversal is
    /// refused.
    void anUnencryptedRoomIsRecordedAsSuch()
    {
        RtcController rtc;
        const QString room = QStringLiteral("!plain:example.org");
        rtc.setRoomEncrypted(room, false);
        QVERIFY(!rtc.roomEncrypted(room));
    }

    /// A stale "not encrypted" read for a known-encrypted room is ignored.
    void aKnownEncryptedRoomCannotBeDowngraded()
    {
        RtcController rtc;
        const QString room = QStringLiteral("!secret:example.org");
        rtc.setRoomEncrypted(room, true);
        QVERIFY(rtc.roomEncrypted(room));

        // The stale view arrives and changes nothing.
        rtc.setRoomEncrypted(room, false);
        QVERIFY2(rtc.roomEncrypted(room),
                 "an encrypted room was downgraded to cleartext by a stale read");

        // Repeating it stays refused.
        rtc.setRoomEncrypted(room, false);
        rtc.setRoomEncrypted(room, false);
        QVERIFY(rtc.roomEncrypted(room));
    }

    /// A refused downgrade emits no change signal; a spurious sessionChanged
    /// re-runs every consumer.
    void arefusedDowngradeAnnouncesNothing()
    {
        RtcController rtc;
        const QString room = QStringLiteral("!secret2:example.org");
        rtc.setRoomEncrypted(room, true);
        QSignalSpy changed(&rtc, &RtcController::sessionChanged);
        rtc.setRoomEncrypted(room, false);
        QCOMPARE(changed.count(), 0);
        QVERIFY(rtc.roomEncrypted(room));
    }

    /// Rooms are independent.
    void oneRoomsRefusalDoesNotAffectAnother()
    {
        RtcController rtc;
        const QString a = QStringLiteral("!a:example.org");
        const QString b = QStringLiteral("!b:example.org");
        rtc.setRoomEncrypted(a, true);
        rtc.setRoomEncrypted(a, false);   // refused
        rtc.setRoomEncrypted(b, false);   // honoured
        QVERIFY(rtc.roomEncrypted(a));
        QVERIFY(!rtc.roomEncrypted(b));
    }

    // An owner-supplied resolver answers for rooms nothing pushed a state
    // for. The global incoming-call card opens no room, so without it the
    // answerer treated an unencrypted room as encrypted and dropped the
    // caller's audio.
    void aRoomTheOwnerKnowsIsUnencryptedIsNotTreatedAsEncrypted()
    {
        RtcController rtc;
        const QString room = QStringLiteral("!plain:example.org");
        // Nothing is pushed: the state a join from the ring card finds.
        rtc.setEncryptionResolver([room](const QString &id) {
            return id == room ? RtcController::RoomEncryption::No
                              : RtcController::RoomEncryption::Unknown;
        });
        QVERIFY2(!rtc.roomEncrypted(room),
                 "a room the owner knows is unencrypted still read as "
                 "encrypted, so a join from a surface that opens no room "
                 "requires encryption the peer is not using");
        // A room the resolver knows nothing about still fails closed.
        QVERIFY(rtc.roomEncrypted(QStringLiteral("!unknown:example.org")));
    }

    /// The resolver cannot weaken a room known to be encrypted.
    void theResolverCannotDowngradeAKnownEncryptedRoom()
    {
        RtcController rtc;
        const QString room = QStringLiteral("!secret3:example.org");
        rtc.setRoomEncrypted(room, true);
        rtc.setEncryptionResolver([](const QString &) {
            return RtcController::RoomEncryption::No;
        });
        QVERIFY2(rtc.roomEncrypted(room),
                 "a stale resolver answer downgraded a room this client has "
                 "already seen encrypted");
    }

    /// A resolver Yes wins over anything stored, the only direction Matrix
    /// encryption moves.
    void theResolverCanUpgradeARoomItLearnsIsEncrypted()
    {
        RtcController rtc;
        const QString room = QStringLiteral("!later:example.org");
        rtc.setRoomEncrypted(room, false);
        QVERIFY(!rtc.roomEncrypted(room));
        rtc.setEncryptionResolver([](const QString &) {
            return RtcController::RoomEncryption::Yes;
        });
        QVERIFY(rtc.roomEncrypted(room));
    }

    /// roomEncrypted() memoises a resolver Yes, so the downgrade guard covers
    /// every room ever seen encrypted, not only those something pushed.
    void aRoomTheResolverOnceCalledEncryptedIsNotLaterDowngraded()
    {
        RtcController rtc;
        const QString room = QStringLiteral("!memo:example.org");
        bool encryptedNow = true;
        rtc.setEncryptionResolver([&encryptedNow](const QString &) {
            return encryptedNow ? RtcController::RoomEncryption::Yes
                                : RtcController::RoomEncryption::No;
        });
        // The read that learns it, and with the memo remembers it.
        QVERIFY(rtc.roomEncrypted(room));
        // The room list now says otherwise: a stale or partial view.
        encryptedNow = false;
        QVERIFY2(rtc.roomEncrypted(room),
                 "a room the resolver itself reported encrypted was "
                 "downgraded by its own later answer, because the first one "
                 "was never recorded");
    }

    // A room this class was never told about stays correctable: only known
    // answers are recorded, so the first known answer is honoured rather
    // than refused as a downgrade. A characterization test for the
    // tri-state (it also passes on older code); the regression case for the
    // caller that fabricated `true` is in CallRingPolicyTest.
    void aRoomThisClassWasNeverToldAboutStaysCorrectable()
    {
        RtcController rtc;
        const QString room = QStringLiteral("!unknown-first:example.org");
        // Never pushed: fail closed.
        QVERIFY(rtc.roomEncrypted(room));
        // The first known answer is honoured.
        rtc.setRoomEncrypted(room, false);
        QVERIFY2(!rtc.roomEncrypted(room),
                 "a room this controller was never told about could not be "
                 "corrected, so every call in it required encryption no "
                 "peer was using");
    }
};

QTEST_MAIN(CallEncryptionStateTest)
#include "CallEncryptionStateTest.moc"
