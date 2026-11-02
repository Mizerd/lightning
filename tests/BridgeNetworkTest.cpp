#include <QtTest/QtTest>

#include "matrix/BridgeNetwork.h"
#include "matrix/MatrixClient.h"
#include "models/RoomListModel.h"

using namespace matrix::bridge;

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    QList<RoomInfo> mirror;

    using MatrixClient::MatrixClient;
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override
    { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return mirror; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override
    { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override
    { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override
    { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &, const QString &) override {}
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &,
                        const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }
};

RoomInfo plainRoom(const QString &id)
{
    RoomInfo value;
    value.id = id;
    value.name = id;
    value.lastActivity = QDateTime::currentDateTimeUtc();
    return value;
}

QString labelAt(const RoomListModel &model, int row)
{
    return model.data(model.index(row), RoomListModel::NetworkLabelRole)
        .toString();
}

QString networkAt(const RoomListModel &model, int row)
{
    return model.data(model.index(row), RoomListModel::NetworkRole).toString();
}

}

class BridgeNetworkTest : public QObject
{
    Q_OBJECT

private slots:
    void ghostUsersResolve_data()
    {
        QTest::addColumn<QString>("userId");
        QTest::addColumn<QString>("network");

        QTest::newRow("whatsapp")
            << "@whatsapp_447700900123:example.org" << "whatsapp";
        QTest::newRow("signal")
            << "@signal_0f9c1d2e-aaaa:example.org" << "signal";
        QTest::newRow("telegram")
            << "@telegram_123456789:example.org" << "telegram";
        QTest::newRow("underscore prefix")
            << "@_discord_987654321:example.org" << "discord";
        QTest::newRow("case insensitive")
            << "@WhatsApp_447700900123:example.org" << "whatsapp";
        QTest::newRow("bridge bot")
            << "@whatsappbot:example.org" << "whatsapp";
        QTest::newRow("bridge bot underscored")
            << "@_signalbot:example.org" << "signal";
    }

    void ghostUsersResolve()
    {
        QFETCH(QString, userId);
        QFETCH(QString, network);
        QCOMPARE(networkIdForUserId(userId), network);
    }

    void ordinaryUsersAreNeverBridged_data()
    {
        QTest::addColumn<QString>("userId");

        QTest::newRow("human with underscore") << "@thomas_redstone:example.org";
        QTest::newRow("plain human")           << "@rokas:example.org";
        QTest::newRow("unknown network")       << "@myspace_42:example.org";
        QTest::newRow("generic bot")           << "@bot:example.org";
        QTest::newRow("unknown bot")           << "@weatherbot:example.org";
        QTest::newRow("empty")                 << "";
        QTest::newRow("sigil only")            << "@";
        QTest::newRow("leading underscore only") << "@_:example.org";
    }

    void ordinaryUsersAreNeverBridged()
    {
        QFETCH(QString, userId);
        QVERIFY2(networkIdForUserId(userId).isEmpty(),
                 qPrintable(QStringLiteral("%1 was misread as bridged")
                                .arg(userId)));
    }

    void aliasesResolve()
    {
        QCOMPARE(networkIdForAlias("#whatsapp_447700900123:example.org"),
                 QStringLiteral("whatsapp"));
        QCOMPARE(networkIdForAlias("#_slack_T01_C02:example.org"),
                 QStringLiteral("slack"));
        QVERIFY(networkIdForAlias("#general:example.org").isEmpty());
        QVERIFY(networkIdForAlias("").isEmpty());
    }

    void toleratesPartialIdentifiers()
    {
        QCOMPARE(networkIdForUserId("whatsapp_447700900123"),
                 QStringLiteral("whatsapp"));
        QCOMPARE(networkIdForUserId("@telegram_1:"), QStringLiteral("telegram"));
    }

    void roomPrefersTheDirectPartner()
    {
        QCOMPARE(networkIdForRoom("@signal_abc:example.org",
                                  "#whatsapp_123:example.org"),
                 QStringLiteral("signal"));
        QCOMPARE(networkIdForRoom("", "#telegram_9:example.org"),
                 QStringLiteral("telegram"));
        QCOMPARE(networkIdForRoom("@rokas:example.org",
                                  "#discord_9:example.org"),
                 QStringLiteral("discord"));
        QVERIFY(networkIdForRoom("@rokas:example.org",
                                 "#general:example.org").isEmpty());
        QVERIFY(networkIdForRoom("", "").isEmpty());
    }

    void labelsAreCuratedNotDerived()
    {
        QCOMPARE(labelForNetworkId("whatsapp"), QStringLiteral("WhatsApp"));
        QCOMPARE(labelForNetworkId("imessage"), QStringLiteral("iMessage"));
        QCOMPARE(labelForNetworkId("googlechat"), QStringLiteral("Google Chat"));
        QCOMPARE(labelForNetworkId("WHATSAPP"), QStringLiteral("WhatsApp"));
        QVERIFY(labelForNetworkId("myspace").isEmpty());
        QVERIFY(labelForNetworkId("").isEmpty());
    }

    void everyRecognisedIdHasALabel()
    {
        const QStringList ids{
            "whatsapp", "telegram", "signal", "discord", "slack",
            "instagram", "facebook", "messenger", "googlechat",
            "gmessages", "gvoice", "twitter", "imessage", "linkedin",
            "bluesky", "sms"
        };
        for (const QString &id : ids) {
            QVERIFY2(!labelForNetworkId(id).isEmpty(), qPrintable(id));
            QCOMPARE(networkIdForUserId(QStringLiteral("@%1_1:example.org")
                                            .arg(id)), id);
        }
    }

    void bridgedDmNamesAreHumane()
    {
        const QString ghost = QStringLiteral("@linkedin_a_co_x9:beeper.local");

        auto dm = presentableDmName(QStringLiteral("Nayara Lanes"), ghost);
        QCOMPARE(dm.name, QStringLiteral("Nayara Lanes"));
        QVERIFY(dm.networkLabel.isEmpty());

        dm = presentableDmName(QStringLiteral("Sim, and 2 others"),
                               QStringLiteral("@signal_uuid7:beeper.local"));
        QCOMPARE(dm.name, QStringLiteral("Sim"));
        dm = presentableDmName(QStringLiteral("Sim and 1 other"),
                               QStringLiteral("@signal_uuid7:beeper.local"));
        QCOMPARE(dm.name, QStringLiteral("Sim"));

        dm = presentableDmName(QStringLiteral("linkedin_a_co_x9"), ghost);
        QVERIFY(dm.name.isEmpty());
        QCOMPARE(dm.networkLabel, QStringLiteral("LinkedIn"));
        dm = presentableDmName(ghost, ghost);
        QVERIFY(dm.name.isEmpty());
        QCOMPARE(dm.networkLabel, QStringLiteral("LinkedIn"));

        dm = presentableDmName(QStringLiteral("@signal_+447700900123:beeper.local"),
                               QStringLiteral("@signal_+447700900123:beeper.local"));
        QCOMPARE(dm.name, QStringLiteral("+447700900123"));
        dm = presentableDmName(QStringLiteral("whatsapp_447700900123"),
                               QStringLiteral("@whatsapp_447700900123:beeper.local"));
        QCOMPARE(dm.name, QStringLiteral("447700900123"));
        dm = presentableDmName(QStringLiteral("@signal_9a2f-4b:beeper.local"),
                               QStringLiteral("@signal_9a2f-4b:beeper.local"));
        QVERIFY(dm.name.isEmpty());
        QCOMPARE(dm.networkLabel, QStringLiteral("Signal"));

        dm = presentableDmName(QString(), ghost);
        QVERIFY(dm.name.isEmpty());
        QCOMPARE(dm.networkLabel, QStringLiteral("LinkedIn"));

        dm = presentableDmName(QStringLiteral("Alice, and 2 others"),
                               QStringLiteral("@alice:example.org"));
        QCOMPARE(dm.name, QStringLiteral("Alice, and 2 others"));
        QVERIFY(dm.networkLabel.isEmpty());
    }

    void aKnownProtocolAlwaysGetsOurOwnLabel()
    {
        auto badge = labelForAdvertisedBridge(
            QStringLiteral("whatsapp"),
            QStringLiteral("WhatsApp (verified by admin)"),
            QStringLiteral("Definitely Real"));
        QCOMPARE(badge.networkId, QStringLiteral("whatsapp"));
        QCOMPARE(badge.label, QStringLiteral("WhatsApp"));

        badge = labelForAdvertisedBridge(QStringLiteral("Discord"),
                                         QStringLiteral("nope"), QString());
        QCOMPARE(badge.networkId, QStringLiteral("discord"));
        QCOMPARE(badge.label, QStringLiteral("Discord"));

        QCOMPARE(badge.label,
                 labelForNetworkId(networkIdForUserId(
                     QStringLiteral("@discord_1:example.org"))));
    }

    void anUnknownProtocolMayNameItselfOnceAndBounded()
    {
        auto badge = labelForAdvertisedBridge(QStringLiteral("irc"),
                                              QStringLiteral("IRC"),
                                              QStringLiteral("Freenode"));
        QCOMPARE(badge.networkId, QStringLiteral("irc"));
        QCOMPARE(badge.label, QStringLiteral("IRC"));

        badge = labelForAdvertisedBridge(QStringLiteral("irc"), QString(),
                                         QStringLiteral("Freenode"));
        QCOMPARE(badge.label, QStringLiteral("Freenode"));

        badge = labelForAdvertisedBridge(QStringLiteral("irc"), QString(),
                                         QString());
        QVERIFY(badge.label.isEmpty());
        badge = labelForAdvertisedBridge(QString(), QString(), QString());
        QVERIFY(badge.label.isEmpty());
        QVERIFY(badge.networkId.isEmpty());
    }

    void advertisedTextCannotForgeLayoutOrRunAway()
    {
        const QString hostile =
            QStringLiteral("Disc") + QChar(0x202E) + QStringLiteral("drocsi")
            + QChar(0x202C) + QStringLiteral("ord") + QChar(0x200F);
        auto badge = labelForAdvertisedBridge(QStringLiteral("custom"),
                                              hostile, QString());
        QCOMPARE(badge.label, QStringLiteral("Discdrocsiord"));

        badge = labelForAdvertisedBridge(
            QStringLiteral("custom"),
            QStringLiteral("  Free \anode \n\t IRC  "), QString());
        QCOMPARE(badge.label, QStringLiteral("Free node IRC"));

        badge = labelForAdvertisedBridge(QStringLiteral("custom"),
                                         QString(80, QLatin1Char('x')),
                                         QString());
        QVERIFY2(badge.label.size() <= 24, qPrintable(badge.label));
        QVERIFY(badge.label.endsWith(QChar(0x2026)));
    }

    void theRoomListShowsAnAdvertisedBridgeOnANonDirectRoom()
    {
        FakeClient client;
        RoomListModel model;
        RoomInfo group = plainRoom(QStringLiteral("!portal:example.org"));
        client.mirror = { group };
        model.setClient(&client);
        QCOMPARE(model.rowCount(), 1);

        QVERIFY(labelAt(model, 0).isEmpty());
        QVERIFY(networkAt(model, 0).isEmpty());

        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        model.setAdvertisedBridge(group.id, QStringLiteral("discord"),
                                  QStringLiteral("Discord"));
        QCOMPARE(labelAt(model, 0), QStringLiteral("Discord"));
        QCOMPARE(networkAt(model, 0), QStringLiteral("discord"));
        QVERIFY2(changed.count() >= 1,
                 "the row must be told, or the badge appears only on the "
                 "next unrelated redraw");

        const QVariantMap found = model.findRoom(group.id);
        QCOMPARE(found.value(QStringLiteral("bridgeLabel")).toString(),
                 QStringLiteral("Discord"));
        QCOMPARE(found.value(QStringLiteral("bridgeNetwork")).toString(),
                 QStringLiteral("discord"));
    }

    void theInferenceStillAnswersWhenNothingIsAdvertised()
    {
        FakeClient client;
        RoomListModel model;
        RoomInfo dm = plainRoom(QStringLiteral("!dm:example.org"));
        dm.isDirect = true;
        dm.directUserId = QStringLiteral("@whatsapp_447700900123:example.org");
        client.mirror = { dm };
        model.setClient(&client);
        QCOMPARE(labelAt(model, 0), QStringLiteral("WhatsApp"));

        model.setAdvertisedBridge(dm.id, QString(), QString());
        QCOMPARE(labelAt(model, 0), QStringLiteral("WhatsApp"));

        model.setAdvertisedBridge(dm.id, QStringLiteral("mystery"), QString());
        QCOMPARE(labelAt(model, 0), QStringLiteral("WhatsApp"));

        model.setAdvertisedBridge(dm.id, QStringLiteral("signal"),
                                  QStringLiteral("Signal"));
        QCOMPARE(labelAt(model, 0), QStringLiteral("Signal"));
        QCOMPARE(networkAt(model, 0), QStringLiteral("signal"));
    }

    void advertisedAnswersAreAccountScoped()
    {
        FakeClient client;
        RoomListModel model;
        RoomInfo group = plainRoom(QStringLiteral("!portal:example.org"));
        client.mirror = { group };
        model.setClient(&client);
        model.setAdvertisedBridge(group.id, QStringLiteral("discord"),
                                  QStringLiteral("Discord"));
        QCOMPARE(labelAt(model, 0), QStringLiteral("Discord"));

        client.logout();
        QVERIFY2(labelAt(model, 0).isEmpty(),
                 "a bridge answer survived the session that produced it");
    }
};

QTEST_GUILESS_MAIN(BridgeNetworkTest)
#include "BridgeNetworkTest.moc"
