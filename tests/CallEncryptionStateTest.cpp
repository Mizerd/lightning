// A CALL MUST NEVER QUIETLY DOWNGRADE AN ENCRYPTED ROOM TO CLEARTEXT.
//
// Measured live 2026-09-16, and it is the defect behind "they cannot hear
// me": the SAME room logged `join begin encrypted= true` on one run and
// `encrypted= false` on the next. On the false run Lightning published its
// media in the CLEAR while the peer ran its frame decryptor over it, so
// nothing arrived, and the peer's own UI reported the sender as
// "not encrypted". Every other explanation -- key index, target device,
// codec negotiation, membership permissions -- had been eliminated by
// measurement first; this was the cause.
//
// The record only ever moves one way because Matrix says so:
// `m.room.encryption` cannot be removed once set. A read claiming a
// known-encrypted room is now plaintext is therefore not news, it is a stale
// or incomplete view, and obeying it is a silent downgrade of a promise the
// user was given (CLAUDE.md section 6).
//
// FAIL-ON-OLD: delete the downgrade guard in RtcController::setRoomEncrypted
// and `aKnownEncryptedRoomCannotBeDowngraded` reads false.

#include "calls/RtcController.h"

#include <QSignalSpy>
#include <QtTest>

class CallEncryptionStateTest : public QObject
{
    Q_OBJECT

private slots:
    /// Unknown fails CLOSED. A room nobody has told us about is treated as
    /// encrypted, so the honest failure is a refused call, never a cleartext
    /// one.
    void anUnknownRoomIsTreatedAsEncrypted()
    {
        RtcController rtc;
        QVERIFY(rtc.roomEncrypted(QStringLiteral("!never-seen:example.org")));
    }

    /// A genuinely unencrypted room is still allowed to say so -- this guard
    /// must not force every room to encrypted, only refuse the reversal.
    void anUnencryptedRoomIsRecordedAsSuch()
    {
        RtcController rtc;
        const QString room = QStringLiteral("!plain:example.org");
        rtc.setRoomEncrypted(room, false);
        QVERIFY(!rtc.roomEncrypted(room));
    }

    /// THE DEFECT. A stale read said "not encrypted" for a room already known
    /// to be encrypted, and the next call joined in the clear.
    void aKnownEncryptedRoomCannotBeDowngraded()
    {
        RtcController rtc;
        const QString room = QStringLiteral("!secret:example.org");
        rtc.setRoomEncrypted(room, true);
        QVERIFY(rtc.roomEncrypted(room));

        // The stale view arrives. It must change nothing.
        rtc.setRoomEncrypted(room, false);
        QVERIFY2(rtc.roomEncrypted(room),
                 "an encrypted room was downgraded to cleartext by a stale read");

        // And repeating it stays refused rather than eroding.
        rtc.setRoomEncrypted(room, false);
        rtc.setRoomEncrypted(room, false);
        QVERIFY(rtc.roomEncrypted(room));
    }

    /// The refusal must not fire a change signal: nothing changed, and a
    /// spurious sessionChanged re-runs every consumer of it.
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

    /// Rooms are independent: refusing one room's downgrade must not pin
    /// another room's state.
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

    // ── 2026-09-18: the record was PUSHED, and two of five join surfaces
    //    never pushed ───────────────────────────────────────────────────
    //
    // `roomEncrypted()` read a map whose only writers were
    // AppController::startCall() and setCurrentRoomId() — so it was filled
    // only for a room the user had OPENED or called FROM. The global
    // incoming-call card opens no room. Live: the answerer required
    // encryption in a room with no `m.room.encryption` at all while the
    // caller correctly published in the clear, and dropped every frame of
    // their audio, reporting a missing key.
    void aRoomTheOwnerKnowsIsUnencryptedIsNotTreatedAsEncrypted()
    {
        RtcController rtc;
        const QString room = QStringLiteral("!plain:example.org");
        // NOTHING is pushed — exactly the state a join from the ring card
        // finds.
        rtc.setEncryptionResolver([room](const QString &id) {
            return id == room ? RtcController::RoomEncryption::No
                              : RtcController::RoomEncryption::Unknown;
        });
        QVERIFY2(!rtc.roomEncrypted(room),
                 "a room the owner knows is unencrypted still read as "
                 "encrypted, so a join from a surface that opens no room "
                 "requires encryption the peer is not using");
        // A room the resolver knows nothing about still fails CLOSED.
        QVERIFY(rtc.roomEncrypted(QStringLiteral("!unknown:example.org")));
    }

    /// The resolver never weakens a room that is KNOWN encrypted, however
    /// it answers — encryption cannot be removed in Matrix.
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

    /// ...and a resolver that says Yes wins over anything stored, because
    /// that is the only direction Matrix encryption moves.
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

    // THE MEMO. roomEncrypted() records a resolver answer of Yes, so the
    // irreversibility guard covers every room this client has ever seen
    // encrypted and not merely the ones something happened to push — which,
    // after the resolver landed, is a much smaller set. Without the memo the
    // guard's own claim ("a KNOWN Yes still wins") holds only for opened
    // rooms, and the room this whole change exists for is the one nothing
    // opened. Delete the insert in roomEncrypted() and this case fails.
    void aRoomTheResolverOnceCalledEncryptedIsNotLaterDowngraded()
    {
        RtcController rtc;
        const QString room = QStringLiteral("!memo:example.org");
        bool encryptedNow = true;
        rtc.setEncryptionResolver([&encryptedNow](const QString &) {
            return encryptedNow ? RtcController::RoomEncryption::Yes
                                : RtcController::RoomEncryption::No;
        });
        // The read that learns it — and, with the memo, remembers it.
        QVERIFY(rtc.roomEncrypted(room));
        // The room list now says otherwise. Encryption cannot be removed in
        // Matrix, so this is a stale or partial view and must not be obeyed.
        encryptedNow = false;
        QVERIFY2(rtc.roomEncrypted(room),
                 "a room the resolver itself reported encrypted was "
                 "downgraded by its own later answer, because the first one "
                 "was never recorded");
    }

    // THE LATCH, AT THE LAYER THAT CAUSED IT. Both writers passed
    // `!known || encrypted`, which turns an UNKNOWN room into a stored,
    // KNOWN `true` — and the downgrade guard then refused the correct
    // answer for the rest of the session, because a stored `true` cannot
    // say which of the two it was. AppController records only a known
    // answer now; this case pins the property the guard depends on, that a
    // room this class was never told about stays correctable.
    //
    // HONEST ABOUT ITSELF: this one passes on the old code too — with no
    // prior entry the old guard never fired either. It is a characterization
    // test for the tri-state, not a regression test for the defect; the
    // regression case for the latch lives in CallRingPolicyTest, where the
    // fabricating caller is. The mutation that DOES falsify it is changing
    // the guard's lookup default to `RoomEncryption::Yes`.
    void aRoomThisClassWasNeverToldAboutStaysCorrectable()
    {
        RtcController rtc;
        const QString room = QStringLiteral("!unknown-first:example.org");
        // Never pushed: fail-closed, exactly as before.
        QVERIFY(rtc.roomEncrypted(room));
        // ...and the first KNOWN answer is honoured rather than refused as
        // a downgrade of an assumption nobody ever made.
        rtc.setRoomEncrypted(room, false);
        QVERIFY2(!rtc.roomEncrypted(room),
                 "a room this controller was never told about could not be "
                 "corrected, so every call in it required encryption no "
                 "peer was using");
    }
};

QTEST_MAIN(CallEncryptionStateTest)
#include "CallEncryptionStateTest.moc"
