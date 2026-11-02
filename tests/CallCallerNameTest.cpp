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
    quint64 requestRoomMembers(const QString &roomId) override
    {
        memberRequests.append(roomId);
        return 1;
    }
    QHash<QString, QString> names;
    QStringList memberRequests;
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

}

class CallCallerNameTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aNameIsPreferredAndTheLocalpartIsTheFallback()
    {
        using C = CallController;
        QCOMPARE(C::presentableCallerName(kCaller, QStringLiteral("Nheko Tester")),
                 QStringLiteral("Nheko Tester"));
        QCOMPARE(C::presentableCallerName(kCaller, kCaller),
                 QStringLiteral("cmtest11"));
        QCOMPARE(C::presentableCallerName(kCaller, QString()),
                 QStringLiteral("cmtest11"));
        QCOMPARE(C::presentableCallerName(kCaller, QStringLiteral("   ")),
                 QStringLiteral("cmtest11"));
        const QString spoof = QStringLiteral("Ali") + QChar(0x202E)
            + QStringLiteral("ce\nBob") + QChar(0x2066);
        QCOMPARE(C::presentableCallerName(kCaller, spoof),
                 QStringLiteral("Alice Bob"));
        const QString longName = QString(63, QLatin1Char('a'))
            + QStringLiteral("\U0001F600") + QStringLiteral("tail");
        const QString bounded = C::presentableCallerName(kCaller, longName);
        QVERIFY(bounded.size() <= 64);
        QVERIFY(!bounded.back().isHighSurrogate());
    }

    void aBorrowedNameOrAddressCarriesTheLocalpart()
    {
        using C = CallController;
        QCOMPARE(C::presentableCallerName(QStringLiteral("@mallory:x"),
                                          QStringLiteral("@alice:matrix.org")),
                 QStringLiteral("@alice:matrix.org (mallory)"));
        QCOMPARE(C::presentableCallerName(QStringLiteral("@mallory:x"),
                                          QStringLiteral("Alice"), true),
                 QStringLiteral("Alice (mallory)"));
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
        QCOMPARE(C::callerNameIn(&client, QStringLiteral("!other:x"),
                                 QStringLiteral("@carol:x")),
                 QStringLiteral("carol"));
    }

    void anInvisibleCharacterCannotEscapeTheCollisionCheck()
    {
        using C = CallController;
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
        for (const char32_t mark : {char32_t(0x034F), char32_t(0x2065),
                                    char32_t(0x3164),
                                    char32_t(0x180B), char32_t(0xFE0F),
                                    char32_t(0xE0100)}) {
            const QString disguised = QStringLiteral("Alice")
                + QString::fromUcs4(&mark, 1);
            QCOMPARE(C::sanitizedCallerName(disguised), QStringLiteral("Alice"));
        }
        QCOMPARE(C::presentableCallerName(QStringLiteral("@mallory:x"),
                                          QStringLiteral("\uFF20alice\uFF1Ax.org")),
                 QStringLiteral("\uFF20alice\uFF1Ax.org (mallory)"));
        QCOMPARE(C::presentableCallerName(QStringLiteral("@mallory:x"),
                                          QString(QChar(0x3164))),
                 QStringLiteral("mallory"));
        {
            NamedMembersClient client;
            client.names.insert(kRoom + QStringLiteral("|@zoe:x"),
                                QStringLiteral("Zo\u00EB"));
            client.names.insert(kRoom + QStringLiteral("|@mallory:x"),
                                QStringLiteral("Zoe\u0308"));
            QCOMPARE(C::callerNameIn(&client, kRoom,
                                     QStringLiteral("@mallory:x")),
                     QStringLiteral("Zoe\u0308 (mallory)"));
        }
        const char32_t tag = 0xE0041;
        QCOMPARE(C::sanitizedCallerName(QStringLiteral("Alice")
                                        + QString::fromUcs4(&tag, 1)),
                 QStringLiteral("Alice"));
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

        QSignalSpy nameChanged(&calls,
                               &CallController::callerDisplayNameChanged);
        client.emitCallSignalForTest(invite(QStringLiteral("call-1")));
        QCOMPARE(calls.state(), CallController::State::Ringing);
        QCOMPARE(calls.activeSenderId(), kCaller);
        QCOMPARE(calls.callerDisplayName(), QStringLiteral("cmtest11"));
        QVERIFY(!nameChanged.isEmpty());

        client.names.insert(kRoom + QLatin1Char('|') + kCaller,
                            QStringLiteral("Nheko Tester"));
        nameChanged.clear();
        Q_EMIT client.membersChanged(kRoom);
        QCOMPARE(nameChanged.count(), 1);
        QCOMPARE(calls.callerDisplayName(), QStringLiteral("Nheko Tester"));
        Q_EMIT client.membersChanged(QStringLiteral("!other:x"));
        QCOMPARE(nameChanged.count(), 1);

        QCOMPARE(calls.property("callerDisplayName").toString(),
                 QStringLiteral("Nheko Tester"));

        QVERIFY(calls.rejectIncoming());
        QVERIFY(calls.callerDisplayName().isEmpty());
    }

    void aRingFromAnUnloadedRoomAsksForTheRoster()
    {
        NamedMembersClient client;
        CallController calls;
        calls.setClient(&client);
        calls.setOwnUserId(QStringLiteral("@me:x"));
        calls.setBacklogSuppressed(false);
        client.emitCallSignalForTest(invite(QStringLiteral("call-r")));
        QCOMPARE(calls.state(), CallController::State::Ringing);
        QCOMPARE(client.memberRequests, QStringList{kRoom});
        QVERIFY(calls.rejectIncoming());

        NamedMembersClient known;
        known.names.insert(kRoom + QLatin1Char('|') + kCaller,
                           QStringLiteral("Nheko Tester"));
        CallController calls2;
        calls2.setClient(&known);
        calls2.setOwnUserId(QStringLiteral("@me:x"));
        calls2.setBacklogSuppressed(false);
        known.emitCallSignalForTest(invite(QStringLiteral("call-k")));
        QCOMPARE(calls2.state(), CallController::State::Ringing);
        QVERIFY(known.memberRequests.isEmpty());
    }
};

QTEST_GUILESS_MAIN(CallCallerNameTest)
#include "CallCallerNameTest.moc"
