
#include "app/SettingsManager.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#ifdef ENABLE_RUST_SDK_BACKEND
#include "matrix/RustSdkMatrixClient.h"
#include "matrix_rust.h"
#endif

class RtcBridgePayloadTest : public QObject
{
    Q_OBJECT

#ifdef ENABLE_RUST_SDK_BACKEND
private:
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
    void aNamelessPickerReactionReachesRustValidation_data()
    {
        QTest::addColumn<QString>("emoji");
        QTest::newRow("generic-fox") << QString::fromUtf8("🦊");
        QTest::newRow("preset-thumbsup") << QString::fromUtf8("👍");
    }

    void aNamelessPickerReactionReachesRustValidation()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        QFETCH(QString, emoji);
        QTemporaryDir store;
        QVERIFY(store.isValid());
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        client.m_rustHandle = mx_rust_create(store.path().toUtf8().constData());
        QVERIFY(client.m_rustHandle);
        const auto cleanup = qScopeGuard([&] {
            mx_rust_destroy(client.m_rustHandle);
            client.m_rustHandle = nullptr;
        });
        const auto before = client.m_opCounter;
        QCOMPARE(client.rtcSendCallReaction(QStringLiteral("!r:example.org"),
                                           QStringLiteral("$membership"),
                                           emoji, QString()), 0ULL);
        QCOMPARE(client.m_opCounter, before + 1);
        client.rtcSendCallReaction(QStringLiteral("!r:example.org"),
                                   QStringLiteral("$membership"),
                                   QString(), QString());
        QCOMPARE(client.m_opCounter, before + 1);
#endif
    }

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

    void theOpenRoomFactsSurviveTheBridge()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        QSignalSpy spy(&client, &MatrixClient::roomOwnPermissionsReceived);
        QJsonObject event;
        event.insert(QStringLiteral("type"),
                     QStringLiteral("room_own_permissions"));
        event.insert(QStringLiteral("op_id"), 21);
        event.insert(QStringLiteral("room_id"),
                     QStringLiteral("!r:example.org"));
        event.insert(QStringLiteral("own_membership"), QStringLiteral("joined"));
        event.insert(QStringLiteral("own_can_send_message"),
                     QJsonValue(QJsonValue::Null));
        event.insert(QStringLiteral("own_can_redact_other"),
                     QJsonValue(QJsonValue::Null));
        client.handleRustEventForTest(event);
        QCOMPARE(spy.count(), 1);
        QList<QVariant> args = spy.takeFirst();
        QCOMPARE(args.at(0).toULongLong(), 21ULL);
        QCOMPARE(args.at(1).toString(), QStringLiteral("!r:example.org"));
        QVariantMap facts = args.at(2).toMap();
        QVERIFY2(!facts.contains(QStringLiteral("canSendMessages")),
                 "an unknown send permission crossed as an answer");
        QVERIFY2(!facts.contains(QStringLiteral("canRedactOthers")),
                 "an unknown redaction permission crossed as an answer");
        QCOMPARE(facts.value(QStringLiteral("ownMembership")).toString(),
                 QStringLiteral("joined"));

        event.insert(QStringLiteral("own_can_send_message"), false);
        event.insert(QStringLiteral("own_can_redact_other"), true);
        client.handleRustEventForTest(event);
        QCOMPARE(spy.count(), 1);
        facts = spy.takeFirst().at(2).toMap();
        QCOMPARE(facts.value(QStringLiteral("canSendMessages")), QVariant(false));
        QCOMPARE(facts.value(QStringLiteral("canRedactOthers")), QVariant(true));

        event.insert(QStringLiteral("own_membership"), QStringLiteral("left"));
        event.insert(QStringLiteral("own_can_send_message"),
                     QJsonValue(QJsonValue::Null));
        event.insert(QStringLiteral("own_can_redact_other"),
                     QJsonValue(QJsonValue::Null));
        client.handleRustEventForTest(event);
        facts = spy.takeFirst().at(2).toMap();
        QCOMPARE(facts.value(QStringLiteral("ownMembership")).toString(),
                 QStringLiteral("left"));
        QVERIFY(!facts.contains(QStringLiteral("canSendMessages")));
#endif
    }

    void theOpenRoomModerationFactsSurviveTheBridge()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        QSignalSpy spy(&client, &MatrixClient::roomOwnPermissionsReceived);
        QJsonObject event;
        event.insert(QStringLiteral("type"),
                     QStringLiteral("room_own_permissions"));
        event.insert(QStringLiteral("op_id"), 31);
        event.insert(QStringLiteral("room_id"),
                     QStringLiteral("!r:example.org"));
        event.insert(QStringLiteral("own_membership"), QStringLiteral("joined"));
        const QStringList keys = {"canKick",      "canBan",       "canUnban",
                                  "canInvite",    "canEditTopic", "canEditName"};
        const QStringList wire = {"own_can_kick",       "own_can_ban",
                                  "own_can_unban",      "own_can_invite",
                                  "own_can_edit_topic", "own_can_edit_name"};
        const QList<bool> values = {true, false, true, false, true, false};
        for (int i = 0; i < wire.size(); ++i)
            event.insert(wire.at(i), values.at(i));
        client.handleRustEventForTest(event);
        QCOMPARE(spy.count(), 1);
        QVariantMap facts = spy.takeFirst().at(2).toMap();
        for (int i = 0; i < keys.size(); ++i)
            QCOMPARE(facts.value(keys.at(i)), QVariant(values.at(i)));

        for (const QString &w : wire)
            event.insert(w, QJsonValue(QJsonValue::Null));
        client.handleRustEventForTest(event);
        facts = spy.takeFirst().at(2).toMap();
        for (const QString &key : keys)
            QVERIFY2(!facts.contains(key),
                     "an unknown moderation fact crossed as an answer");

        for (const QString &w : wire)
            event.remove(w);
        client.handleRustEventForTest(event);
        facts = spy.takeFirst().at(2).toMap();
        for (const QString &key : keys)
            QVERIFY(!facts.contains(key));
#endif
    }

    void aTypingSetNobodyRefreshesExpires()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        client.m_typingExpiryMs = 300;
        const QString room = QStringLiteral("!r:example.org");
        QJsonObject listed;
        listed.insert(QStringLiteral("id"), room);
        listed.insert(QStringLiteral("name"), QStringLiteral("Room"));
        QJsonObject rooms;
        rooms.insert(QStringLiteral("type"), QStringLiteral("rooms"));
        rooms.insert(QStringLiteral("rooms"), QJsonArray{listed});
        client.handleRustEventForTest(rooms);
        QJsonObject who;
        who.insert(QStringLiteral("user_id"), QStringLiteral("@alice:example.org"));
        QJsonObject typing;
        typing.insert(QStringLiteral("type"), QStringLiteral("typing_update"));
        typing.insert(QStringLiteral("room_id"), room);
        typing.insert(QStringLiteral("users"), QJsonArray{who});
        QSignalSpy changed(&client, &MatrixClient::typingChanged);

        client.handleRustEventForTest(typing);
        QCOMPARE(client.typingUsersFor(room).size(), 1);
        QTest::qWait(200);
        client.handleRustEventForTest(typing);
        QTest::qWait(200);
        QCOMPARE(client.typingUsersFor(room).size(), 1);
        const int before = changed.size();
        QTRY_VERIFY_WITH_TIMEOUT(client.typingUsersFor(room).isEmpty(), 2000);
        QVERIFY(changed.size() > before);
        typing.insert(QStringLiteral("users"), QJsonArray{});
        client.handleRustEventForTest(typing);
        QVERIFY(client.typingUsersFor(room).isEmpty());
#endif
    }

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

        roster.insert(QStringLiteral("partial"), true);
        client.handleRustEventForTest(roster);
        QCOMPARE(client.typingUsersFor(room).size(), 2);
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
        client.handleRustEventForTest(
            publishedEvent(QString(), QStringLiteral("unrecognized")));
        QCOMPARE(spy.count(), 1);
        const QList<QVariant> args = spy.takeFirst();
        QCOMPARE(args.at(1).toBool(), true);
        QCOMPARE(args.at(3).toString(), QStringLiteral("$evt"));
        QVERIFY(args.at(4).toString().isEmpty());
        QCOMPARE(args.at(5).toString(), QStringLiteral("unrecognized"));
#endif
    }

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
        QCOMPARE(args.at(3).toULongLong(), 31ULL);
#endif
    }

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
