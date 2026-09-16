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
};

QTEST_MAIN(CallEncryptionStateTest)
#include "CallEncryptionStateTest.moc"
