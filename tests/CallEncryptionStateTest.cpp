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
    void anUnknownRoomIsTreatedAsEncrypted()
    {
        RtcController rtc;
        QVERIFY(rtc.roomEncrypted(QStringLiteral("!never-seen:example.org")));
    }

    void anUnencryptedRoomIsRecordedAsSuch()
    {
        RtcController rtc;
        const QString room = QStringLiteral("!plain:example.org");
        rtc.setRoomEncrypted(room, false);
        QVERIFY(!rtc.roomEncrypted(room));
    }

    void aKnownEncryptedRoomCannotBeDowngraded()
    {
        RtcController rtc;
        const QString room = QStringLiteral("!secret:example.org");
        rtc.setRoomEncrypted(room, true);
        QVERIFY(rtc.roomEncrypted(room));

        rtc.setRoomEncrypted(room, false);
        QVERIFY2(rtc.roomEncrypted(room),
                 "an encrypted room was downgraded to cleartext by a stale read");

        rtc.setRoomEncrypted(room, false);
        rtc.setRoomEncrypted(room, false);
        QVERIFY(rtc.roomEncrypted(room));
    }

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

    void oneRoomsRefusalDoesNotAffectAnother()
    {
        RtcController rtc;
        const QString a = QStringLiteral("!a:example.org");
        const QString b = QStringLiteral("!b:example.org");
        rtc.setRoomEncrypted(a, true);
        rtc.setRoomEncrypted(a, false);
        rtc.setRoomEncrypted(b, false);
        QVERIFY(rtc.roomEncrypted(a));
        QVERIFY(!rtc.roomEncrypted(b));
    }

    void aRoomTheOwnerKnowsIsUnencryptedIsNotTreatedAsEncrypted()
    {
        RtcController rtc;
        const QString room = QStringLiteral("!plain:example.org");
        rtc.setEncryptionResolver([room](const QString &id) {
            return id == room ? RtcController::RoomEncryption::No
                              : RtcController::RoomEncryption::Unknown;
        });
        QVERIFY2(!rtc.roomEncrypted(room),
                 "a room the owner knows is unencrypted still read as "
                 "encrypted, so a join from a surface that opens no room "
                 "requires encryption the peer is not using");
        QVERIFY(rtc.roomEncrypted(QStringLiteral("!unknown:example.org")));
    }

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

    void aRoomTheResolverOnceCalledEncryptedIsNotLaterDowngraded()
    {
        RtcController rtc;
        const QString room = QStringLiteral("!memo:example.org");
        bool encryptedNow = true;
        rtc.setEncryptionResolver([&encryptedNow](const QString &) {
            return encryptedNow ? RtcController::RoomEncryption::Yes
                                : RtcController::RoomEncryption::No;
        });
        QVERIFY(rtc.roomEncrypted(room));
        encryptedNow = false;
        QVERIFY2(rtc.roomEncrypted(room),
                 "a room the resolver itself reported encrypted was "
                 "downgraded by its own later answer, because the first one "
                 "was never recorded");
    }

    void aRoomThisClassWasNeverToldAboutStaysCorrectable()
    {
        RtcController rtc;
        const QString room = QStringLiteral("!unknown-first:example.org");
        QVERIFY(rtc.roomEncrypted(room));
        rtc.setRoomEncrypted(room, false);
        QVERIFY2(!rtc.roomEncrypted(room),
                 "a room this controller was never told about could not be "
                 "corrected, so every call in it required encryption no "
                 "peer was using");
    }
};

QTEST_MAIN(CallEncryptionStateTest)
#include "CallEncryptionStateTest.moc"
