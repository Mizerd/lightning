// Fields the Rust lane computes must survive the bridge. SfuCallController's
// tests drive a fake client that emits the signal directly, so only the real
// dispatcher (`handleRustEventForTest`) with the real payload shows whether
// `RustSdkMatrixClient::handleRustEvent` carries a field.
//
// `delayed_category` in `rtc_membership_published` says whether a refused
// delayed event means "no MSC4140 endpoint" (permanent) or "this write was
// refused" (transient).

#include "app/SettingsManager.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

#ifdef ENABLE_RUST_SDK_BACKEND
#include "matrix/RustSdkMatrixClient.h"
#endif

class RtcBridgePayloadTest : public QObject
{
    Q_OBJECT

#ifdef ENABLE_RUST_SDK_BACKEND
private:
    /// The payload rust/src/rtc.rs enqueues, field for field. Spelled out
    /// here rather than built by a helper so that a rename on the Rust side
    /// shows up as a failing test and not as a silently absent field.
    static QJsonObject publishedEvent(const QString &delayId,
                                      const QString &delayedCategory)
    {
        QJsonObject out;
        out.insert(QStringLiteral("type"),
                   QStringLiteral("rtc_membership_published"));
        out.insert(QStringLiteral("op_id"), 7);
        out.insert(QStringLiteral("room_id"), QStringLiteral("!r:example.org"));
        out.insert(QStringLiteral("ok"), true);
        out.insert(QStringLiteral("category"), QString());
        out.insert(QStringLiteral("event_id"), QStringLiteral("$evt"));
        out.insert(QStringLiteral("delay_id"), delayId);
        out.insert(QStringLiteral("delayed_category"), delayedCategory);
        return out;
    }
#endif

private slots:
    /// rooms.rs reports `own_can_publish_rtc_membership` as null when our own
    /// member is not in the store yet. That must reach the call gate as
    /// UNKNOWN (no key), not as false, which would read as refused.
    void anUnknownCallPermissionIsNotReportedAsRefused()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        QSignalSpy spy(&client, &MatrixClient::roomMembersReceived);
        QJsonObject event;
        event.insert(QStringLiteral("type"), QStringLiteral("room_members"));
        event.insert(QStringLiteral("op_id"), 9);
        event.insert(QStringLiteral("room_id"),
                     QStringLiteral("!r:example.org"));
        event.insert(QStringLiteral("ok"), true);
        event.insert(QStringLiteral("partial"), true);
        event.insert(QStringLiteral("own_can_publish_rtc_membership"),
                     QJsonValue(QJsonValue::Null));
        client.handleRustEventForTest(event);
        QCOMPARE(spy.count(), 1);
        QVariantMap snapshot = spy.takeFirst().at(2).toMap();
        QVERIFY2(!snapshot.contains(QStringLiteral("canPublishCallMembership")),
                 "an unknown permission crossed the bridge as a refusal");

        // A real answer still crosses, both ways.
        event.insert(QStringLiteral("own_can_publish_rtc_membership"), false);
        client.handleRustEventForTest(event);
        QCOMPARE(spy.count(), 1);
        snapshot = spy.takeFirst().at(2).toMap();
        QCOMPARE(snapshot.value(QStringLiteral("canPublishCallMembership")),
                 QVariant(false));
        event.insert(QStringLiteral("own_can_publish_rtc_membership"), true);
        client.handleRustEventForTest(event);
        snapshot = spy.takeFirst().at(2).toMap();
        QCOMPARE(snapshot.value(QStringLiteral("canPublishCallMembership")),
                 QVariant(true));
#endif
    }

    /// rooms.rs's posting/redaction answers and own membership must reach
    /// RoomInfoController: null is unknown (no key), a bool crosses, and a
    /// not_joined answer still says what the membership is (B027/B123).
    void theSendRedactAndMembershipAnswersSurviveTheBridge()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        QSignalSpy spy(&client, &MatrixClient::roomMembersReceived);
        QJsonObject event;
        event.insert(QStringLiteral("type"), QStringLiteral("room_members"));
        event.insert(QStringLiteral("op_id"), 11);
        event.insert(QStringLiteral("room_id"),
                     QStringLiteral("!r:example.org"));
        event.insert(QStringLiteral("ok"), true);
        event.insert(QStringLiteral("partial"), true);
        event.insert(QStringLiteral("own_can_send_message"),
                     QJsonValue(QJsonValue::Null));
        event.insert(QStringLiteral("own_can_redact_other"),
                     QJsonValue(QJsonValue::Null));
        event.insert(QStringLiteral("own_membership"), QStringLiteral("joined"));
        client.handleRustEventForTest(event);
        QCOMPARE(spy.count(), 1);
        QVariantMap snapshot = spy.takeFirst().at(2).toMap();
        QVERIFY2(!snapshot.contains(QStringLiteral("canSendMessages")),
                 "an unknown send permission crossed as an answer");
        QVERIFY2(!snapshot.contains(QStringLiteral("canRedactOthers")),
                 "an unknown redaction permission crossed as an answer");
        QCOMPARE(snapshot.value(QStringLiteral("ownMembership")).toString(),
                 QStringLiteral("joined"));

        // Two DIFFERENT answers, so a swapped key fails.
        event.insert(QStringLiteral("own_can_send_message"), false);
        event.insert(QStringLiteral("own_can_redact_other"), true);
        client.handleRustEventForTest(event);
        snapshot = spy.takeFirst().at(2).toMap();
        QCOMPARE(snapshot.value(QStringLiteral("canSendMessages")), QVariant(false));
        QCOMPARE(snapshot.value(QStringLiteral("canRedactOthers")), QVariant(true));

        QJsonObject notJoined;
        notJoined.insert(QStringLiteral("type"), QStringLiteral("room_members"));
        notJoined.insert(QStringLiteral("op_id"), 12);
        notJoined.insert(QStringLiteral("room_id"),
                         QStringLiteral("!r:example.org"));
        notJoined.insert(QStringLiteral("ok"), false);
        notJoined.insert(QStringLiteral("category"), QStringLiteral("not_joined"));
        notJoined.insert(QStringLiteral("own_membership"), QStringLiteral("invited"));
        client.handleRustEventForTest(notJoined);
        snapshot = spy.takeFirst().at(2).toMap();
        QCOMPARE(snapshot.value(QStringLiteral("ok")), QVariant(false));
        QCOMPARE(snapshot.value(QStringLiteral("ownMembership")).toString(),
                 QStringLiteral("invited"));
#endif
    }

    /// A typist who left (kicked, banned) gets no typing stop from the server;
    /// a full roster without them drops them from the indicator, a partial or
    /// truncated one cannot prove they left (B026).
    void aFullRosterDropsTypistsWhoAreNoLongerJoined()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        const QString room = QStringLiteral("!r:example.org");
        QJsonObject listed;
        listed.insert(QStringLiteral("id"), room);
        listed.insert(QStringLiteral("name"), QStringLiteral("Room"));
        QJsonObject rooms;
        rooms.insert(QStringLiteral("type"), QStringLiteral("rooms"));
        rooms.insert(QStringLiteral("rooms"), QJsonArray{listed});
        client.handleRustEventForTest(rooms);

        auto user = [](const QString &id) {
            QJsonObject o;
            o.insert(QStringLiteral("user_id"), id);
            return o;
        };
        QJsonObject typing;
        typing.insert(QStringLiteral("type"), QStringLiteral("typing_update"));
        typing.insert(QStringLiteral("room_id"), room);
        typing.insert(QStringLiteral("users"),
                      QJsonArray{user(QStringLiteral("@alice:example.org")),
                                 user(QStringLiteral("@bob:example.org"))});
        client.handleRustEventForTest(typing);
        QCOMPARE(client.typingUsersFor(room).size(), 2);

        auto row = [](const QString &id, const QString &membership) {
            QJsonObject o;
            o.insert(QStringLiteral("user_id"), id);
            o.insert(QStringLiteral("membership"), membership);
            return o;
        };
        QJsonObject roster;
        roster.insert(QStringLiteral("type"), QStringLiteral("room_members"));
        roster.insert(QStringLiteral("op_id"), 21);
        roster.insert(QStringLiteral("room_id"), room);
        roster.insert(QStringLiteral("ok"), true);
        roster.insert(QStringLiteral("members"),
                      QJsonArray{row(QStringLiteral("@alice:example.org"),
                                     QStringLiteral("joined")),
                                 row(QStringLiteral("@bob:example.org"),
                                     QStringLiteral("banned"))});
        QSignalSpy typingSpy(&client, &MatrixClient::typingChanged);

        // A partial roster proves nothing.
        roster.insert(QStringLiteral("partial"), true);
        client.handleRustEventForTest(roster);
        QCOMPARE(client.typingUsersFor(room).size(), 2);
        // Nor does a truncated one.
        roster.insert(QStringLiteral("partial"), false);
        roster.insert(QStringLiteral("truncated"), true);
        client.handleRustEventForTest(roster);
        QCOMPARE(client.typingUsersFor(room).size(), 2);
        QCOMPARE(typingSpy.count(), 0);

        roster.insert(QStringLiteral("truncated"), false);
        client.handleRustEventForTest(roster);
        QCOMPARE(client.typingUsersFor(room),
                 QStringList{QStringLiteral("@alice:example.org")});
        QCOMPARE(typingSpy.count(), 1);
#endif
    }

    /// The room payload's joined head count and join rule reach RoomInfo, so
    /// the room list's Leave can say whether the room would be left empty
    /// (B133).
    void theJoinedCountAndJoinRuleReachTheRoom()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        const QString room = QStringLiteral("!r:example.org");
        QJsonObject listed;
        listed.insert(QStringLiteral("id"), room);
        listed.insert(QStringLiteral("member_count"), 1);
        listed.insert(QStringLiteral("active_member_count"), 2);
        listed.insert(QStringLiteral("join_rule"), QStringLiteral("invite"));
        QJsonObject rooms;
        rooms.insert(QStringLiteral("type"), QStringLiteral("rooms"));
        rooms.insert(QStringLiteral("rooms"), QJsonArray{listed});
        client.handleRustEventForTest(rooms);
        const QList<RoomInfo> all = client.rooms();
        QCOMPARE(all.size(), 1);
        QCOMPARE(all.first().joinedMemberCount, 1);
        QCOMPARE(all.first().activeMemberCount, 2);
        QCOMPARE(all.first().joinRule, QStringLiteral("invite"));
#endif
    }

    void theDelayedRefusalReasonSurvivesTheBridge()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        QSignalSpy spy(&client, &MatrixClient::rtcMembershipPublished);
        // A homeserver with no MSC4140 endpoint at all: no delay id, and the
        // category that says the absence is PERMANENT rather than a refusal
        // this once. rtc.rs latches on exactly this vocabulary.
        client.handleRustEventForTest(
            publishedEvent(QString(), QStringLiteral("unrecognized")));
        QCOMPARE(spy.count(), 1);
        const QList<QVariant> args = spy.takeFirst();
        QCOMPARE(args.at(1).toBool(), true);
        QCOMPARE(args.at(3).toString(), QStringLiteral("$evt"));
        QVERIFY(args.at(4).toString().isEmpty());
        // The field under test. Empty here is the defect, not a default.
        QCOMPARE(args.at(5).toString(), QStringLiteral("unrecognized"));
#endif
    }

    /// `auto_key_recovery`'s `inconclusive` count must survive the bridge: it
    /// is what distinguishes one downloaded session out of 32 from a server
    /// that refused thirty-one times.
    void theInconclusiveCountSurvivesTheBridge()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        QSignalSpy spy(&client, &RustSdkMatrixClient::cryptoBootstrapEvent);

        QJsonObject out;
        out.insert(QStringLiteral("type"),
                   QStringLiteral("crypto_bootstrap"));
        out.insert(QStringLiteral("kind"),
                   QStringLiteral("auto_key_recovery"));
        out.insert(QStringLiteral("state"), QStringLiteral("ok"));
        out.insert(QStringLiteral("count"), 1);
        out.insert(QStringLiteral("inconclusive"), 31);
        client.handleRustEventForTest(out);

        QCOMPARE(spy.count(), 1);
        const QList<QVariant> args = spy.takeFirst();
        QCOMPARE(args.at(0).toString(), QStringLiteral("auto_key_recovery"));
        QCOMPARE(args.at(1).toString(), QStringLiteral("ok"));
        QCOMPARE(args.at(2).toULongLong(), 1ULL);
        // The field under test. 0 here is the defect, not a default.
        QCOMPARE(args.at(3).toULongLong(), 31ULL);
#endif
    }

    /// A kind with no such notion must report zero rather than stale data.
    void anEventWithoutTheCountReportsZero()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        QSignalSpy spy(&client, &RustSdkMatrixClient::cryptoBootstrapEvent);

        QJsonObject out;
        out.insert(QStringLiteral("type"),
                   QStringLiteral("crypto_bootstrap"));
        out.insert(QStringLiteral("kind"), QStringLiteral("backup_state"));
        out.insert(QStringLiteral("state"), QStringLiteral("enabled"));
        client.handleRustEventForTest(out);

        QCOMPARE(spy.count(), 1);
        const QList<QVariant> args = spy.takeFirst();
        QCOMPARE(args.at(2).toULongLong(), 0ULL);
        QCOMPARE(args.at(3).toULongLong(), 0ULL);
#endif
    }

    void anArmedDelayedRetractionCarriesNoReason()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        // Success must not invent a reason: an empty `delayed_category` beside
        // a non-empty delay id means nothing went wrong.
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        QSignalSpy spy(&client, &MatrixClient::rtcMembershipPublished);
        client.handleRustEventForTest(
            publishedEvent(QStringLiteral("delay-1"), QString()));
        QCOMPARE(spy.count(), 1);
        const QList<QVariant> args = spy.takeFirst();
        QCOMPARE(args.at(4).toString(), QStringLiteral("delay-1"));
        QVERIFY(args.at(5).toString().isEmpty());
#endif
    }
};

QTEST_MAIN(RtcBridgePayloadTest)
#include "RtcBridgePayloadTest.moc"
