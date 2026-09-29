// What the incoming-call card calls the caller: the room member's display
// name when it is known, else the MXID localpart. Reported live: a Nheko user
// named in the room rang as "cmtest11 is calling".
#include <QtTest/QtTest>

#include <QDateTime>
#include <QHash>
#include <QSignalSpy>

#include "calls/CallController.h"
#include "matrix/CallSignal.h"
#include "matrix/MockMatrixClient.h"

namespace {

const QString kRoom = QStringLiteral("!dm:x");
const QString kCaller = QStringLiteral("@cmtest11:x");

class NamedMembersClient : public MockMatrixClient
{
public:
    using MockMatrixClient::MockMatrixClient;

    bool supportsCallSignaling() const override { return true; }
    QString displayNameFor(const QString &roomId,
                           const QString &userId) const override
    {
        // As the real client: the MXID when the member is not known.
        return names.value(roomId + QLatin1Char('|') + userId, userId);
    }
    RoomInfo roomInfo(const QString &roomId) const override
    {
        RoomInfo room;
        room.id = roomId;
        const QString prefix = roomId + QLatin1Char('|');
        for (auto it = names.cbegin(); it != names.cend(); ++it) {
            if (!it.key().startsWith(prefix))
                continue;
            const QString user = it.key().mid(prefix.size());
            room.members.insert(user, MemberInfo{user, it.value(), QString()});
        }
        return room;
    }
    QHash<QString, QString> names;
};

CallSignal invite(const QString &callId)
{
    CallSignal s;
    s.kind = CallSignal::Kind::Invite;
    s.roomId = kRoom;
    s.eventId = QStringLiteral("$invite-") + callId;
    s.sender = kCaller;
    s.callId = callId;
    s.partyId = QStringLiteral("peer-party");
    s.lifetimeMs = 60000;
    s.originServerTs = QDateTime::currentMSecsSinceEpoch();
    s.version = QStringLiteral("1");
    s.sessionType = QStringLiteral("offer");
    s.hasDescription = true;
    return s;
}

} // namespace

class CallCallerNameTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aNameIsPreferredAndTheLocalpartIsTheFallback()
    {
        using C = CallController;
        QCOMPARE(C::presentableCallerName(kCaller, QStringLiteral("Nheko Tester")),
                 QStringLiteral("Nheko Tester"));
        // Unknown member: the client answers with the MXID itself.
        QCOMPARE(C::presentableCallerName(kCaller, kCaller),
                 QStringLiteral("cmtest11"));
        QCOMPARE(C::presentableCallerName(kCaller, QString()),
                 QStringLiteral("cmtest11"));
        QCOMPARE(C::presentableCallerName(kCaller, QStringLiteral("   ")),
                 QStringLiteral("cmtest11"));
        // One line, no direction overrides that reorder the sentence.
        const QString spoof = QStringLiteral("Ali") + QChar(0x202E)
            + QStringLiteral("ce\nBob") + QChar(0x2066);
        QCOMPARE(C::presentableCallerName(kCaller, spoof),
                 QStringLiteral("Alice Bob"));
        // Bounded, and never cut inside a surrogate pair.
        const QString longName = QString(63, QLatin1Char('a'))
            + QStringLiteral("\U0001F600") + QStringLiteral("tail");
        const QString bounded = C::presentableCallerName(kCaller, longName);
        QVERIFY(bounded.size() <= 64);
        QVERIFY(!bounded.back().isHighSurrogate());
    }

    // Review S5: the card asks the user to pick up, so it must not wear
    // someone else's name or address.
    void aBorrowedNameOrAddressCarriesTheLocalpart()
    {
        using C = CallController;
        QCOMPARE(C::presentableCallerName(QStringLiteral("@mallory:x"),
                                          QStringLiteral("@alice:matrix.org")),
                 QStringLiteral("@alice:matrix.org (mallory)"));
        QCOMPARE(C::presentableCallerName(QStringLiteral("@mallory:x"),
                                          QStringLiteral("Alice"), true),
                 QStringLiteral("Alice (mallory)"));
        // Nothing to disambiguate when the name is the localpart itself.
        QCOMPARE(C::presentableCallerName(QStringLiteral("@bob:x"),
                                          QStringLiteral("bob"), true),
                 QStringLiteral("bob"));

        NamedMembersClient client;
        client.names.insert(kRoom + QStringLiteral("|@alice:x"),
                            QStringLiteral("Alice"));
        client.names.insert(kRoom + QStringLiteral("|@mallory:x"),
                            QStringLiteral("alice "));
        client.names.insert(kRoom + QStringLiteral("|@carol:x"),
                            QStringLiteral("Carol"));
        QCOMPARE(C::callerNameIn(&client, kRoom, QStringLiteral("@mallory:x")),
                 QStringLiteral("alice (mallory)"));
        QCOMPARE(C::callerNameIn(&client, kRoom, QStringLiteral("@alice:x")),
                 QStringLiteral("Alice (alice)"));
        QCOMPARE(C::callerNameIn(&client, kRoom, QStringLiteral("@carol:x")),
                 QStringLiteral("Carol"));
        // Another room's members are not this room's.
        QCOMPARE(C::callerNameIn(&client, QStringLiteral("!other:x"),
                                 QStringLiteral("@carol:x")),
                 QStringLiteral("carol"));
    }

    // Final review S4: the collision check compared RAW names while the card
    // showed them sanitised, so "Alice" + LRM was "not ambiguous" and
    // rendered as "Alice"; zero-width characters were not stripped at all.
    void anInvisibleCharacterCannotEscapeTheCollisionCheck()
    {
        using C = CallController;
        // Every one of these renders as "Alice".
        const QList<QChar> invisible{QChar(0x200E), QChar(0x200B),
                                     QChar(0x200D), QChar(0x2060),
                                     QChar(0x2063), QChar(0xFEFF),
                                     QChar(0x061C), QChar(0x00AD)};
        for (const QChar mark : invisible) {
            const QString disguised =
                QStringLiteral("Al") + mark + QStringLiteral("ice") + mark;
            QCOMPARE(C::sanitizedCallerName(disguised), QStringLiteral("Alice"));

            NamedMembersClient client;
            client.names.insert(kRoom + QStringLiteral("|@alice:x"),
                                QStringLiteral("Alice"));
            client.names.insert(kRoom + QStringLiteral("|@mallory:x"),
                                disguised);
            QVERIFY2(C::callerNameIn(&client, kRoom,
                                     QStringLiteral("@mallory:x"))
                         == QStringLiteral("Alice (mallory)"),
                     qPrintable(QStringLiteral("U+%1 hid the collision")
                                    .arg(int(mark.unicode()), 4, 16,
                                         QLatin1Char('0'))));
        }
        // Invisible marks that are not Cf: the combining grapheme joiner,
        // a Hangul filler, a Mongolian selector, a variation selector.
        for (const char32_t mark : {char32_t(0x034F), char32_t(0x2065),
                                    char32_t(0x3164),
                                    char32_t(0x180B), char32_t(0xFE0F),
                                    char32_t(0xE0100)}) {
            const QString disguised = QStringLiteral("Alice")
                + QString::fromUcs4(&mark, 1);
            QCOMPARE(C::sanitizedCallerName(disguised), QStringLiteral("Alice"));
        }
        // A fullwidth address is still an address.
        QCOMPARE(C::presentableCallerName(QStringLiteral("@mallory:x"),
                                          QStringLiteral("\uFF20alice\uFF1Ax.org")),
                 QStringLiteral("\uFF20alice\uFF1Ax.org (mallory)"));
        // A name that is nothing but a filler shows the localpart.
        QCOMPARE(C::presentableCallerName(QStringLiteral("@mallory:x"),
                                          QString(QChar(0x3164))),
                 QStringLiteral("mallory"));
        {
            // The same name in another normalization form is the same name.
            NamedMembersClient client;
            client.names.insert(kRoom + QStringLiteral("|@zoe:x"),
                                QStringLiteral("Zo\u00EB"));
            client.names.insert(kRoom + QStringLiteral("|@mallory:x"),
                                QStringLiteral("Zoe\u0308"));
            QCOMPARE(C::callerNameIn(&client, kRoom,
                                     QStringLiteral("@mallory:x")),
                     QStringLiteral("Zoe\u0308 (mallory)"));
        }
        // A format character outside the BMP (a tag character) too.
        const char32_t tag = 0xE0041;
        QCOMPARE(C::sanitizedCallerName(QStringLiteral("Alice")
                                        + QString::fromUcs4(&tag, 1)),
                 QStringLiteral("Alice"));
        // Visible text is left alone.
        QCOMPARE(C::sanitizedCallerName(QStringLiteral("Zoë 🎉")),
                 QStringLiteral("Zoë 🎉"));
    }

    void theRingingCardUsesTheRoomMembersName()
    {
        NamedMembersClient client;
        CallController calls;
        calls.setClient(&client);
        calls.setOwnUserId(QStringLiteral("@me:x"));
        calls.setBacklogSuppressed(false);
        QVERIFY(calls.callerDisplayName().isEmpty());

        // The member list has not loaded yet: the localpart, never the MXID.
        QSignalSpy nameChanged(&calls,
                               &CallController::callerDisplayNameChanged);
        client.emitCallSignalForTest(invite(QStringLiteral("call-1")));
        QCOMPARE(calls.state(), CallController::State::Ringing);
        QCOMPARE(calls.activeSenderId(), kCaller);
        QCOMPARE(calls.callerDisplayName(), QStringLiteral("cmtest11"));
        QVERIFY(!nameChanged.isEmpty());

        // The roster lands while it rings: the card follows.
        client.names.insert(kRoom + QLatin1Char('|') + kCaller,
                            QStringLiteral("Nheko Tester"));
        nameChanged.clear();
        Q_EMIT client.membersChanged(kRoom);
        QCOMPARE(nameChanged.count(), 1);
        QCOMPARE(calls.callerDisplayName(), QStringLiteral("Nheko Tester"));
        // Another room's roster is not this call's.
        Q_EMIT client.membersChanged(QStringLiteral("!other:x"));
        QCOMPARE(nameChanged.count(), 1);

        // Exposed to QML under that name.
        QCOMPARE(calls.property("callerDisplayName").toString(),
                 QStringLiteral("Nheko Tester"));

        QVERIFY(calls.rejectIncoming());
        QVERIFY(calls.callerDisplayName().isEmpty());
    }
};

QTEST_GUILESS_MAIN(CallCallerNameTest)
#include "CallCallerNameTest.moc"
