// Bridge-network recognition: pure string logic. The negatives matter most:
// labelling an ordinary Matrix contact as bridged is worse than no badge.
#include <QtTest/QtTest>

#include "matrix/BridgeNetwork.h"
#include "matrix/MatrixClient.h"
#include "models/RoomListModel.h"

using namespace matrix::bridge;

namespace {

/// The smallest client the room list accepts, so badge tests can drive the
/// model and not only the pure functions (non-DM rooms get their answer
/// through the model).
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

} // namespace

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
        // The matrix-appservice-* family prefixes with an underscore.
        QTest::newRow("underscore prefix")
            << "@_discord_987654321:example.org" << "discord";
        QTest::newRow("case insensitive")
            << "@WhatsApp_447700900123:example.org" << "whatsapp";
        // The bot DM (login and bridge status) is labelled too, and has no
        // remote id to separate.
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

        // An underscore in a human localpart is not a network separator.
        QTest::newRow("human with underscore") << "@thomas_redstone:example.org";
        QTest::newRow("plain human")           << "@rokas:example.org";
        QTest::newRow("unknown network")       << "@myspace_42:example.org";
        // "bot" alone is not a bridge bot.
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

    // A missing sigil or server part must not break the parse; the model
    // passes on whatever the SDK gave it.
    void toleratesPartialIdentifiers()
    {
        QCOMPARE(networkIdForUserId("whatsapp_447700900123"),
                 QStringLiteral("whatsapp"));
        QCOMPARE(networkIdForUserId("@telegram_1:"), QStringLiteral("telegram"));
    }

    void roomPrefersTheDirectPartner()
    {
        // Both present and disagreeing: the DM partner wins, since it names a
        // real remote account.
        QCOMPARE(networkIdForRoom("@signal_abc:example.org",
                                  "#whatsapp_123:example.org"),
                 QStringLiteral("signal"));
        // DM partner absent or native: fall back to the alias.
        QCOMPARE(networkIdForRoom("", "#telegram_9:example.org"),
                 QStringLiteral("telegram"));
        QCOMPARE(networkIdForRoom("@rokas:example.org",
                                  "#discord_9:example.org"),
                 QStringLiteral("discord"));
        // A native Matrix room stays unlabelled.
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
        // Never invent a label for an id we do not know.
        QVERIFY(labelForNetworkId("myspace").isEmpty());
        QVERIFY(labelForNetworkId("").isEmpty());
    }

    // Every id the recognisers produce has a label, or the UI gets a badge it
    // cannot render.
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
            // And each is reachable from a ghost id.
            QCOMPARE(networkIdForUserId(QStringLiteral("@%1_1:example.org")
                                            .arg(id)), id);
        }
    }

    // Bridged DM names: a profile-less ghost localpart or the bridge's hero
    // arithmetic ("Sim, and 2 others") is never presented as the name.
    void bridgedDmNamesAreHumane()
    {
        const QString ghost = QStringLiteral("@linkedin_a_co_x9:beeper.local");

        // A hydrated human name passes through untouched.
        auto dm = presentableDmName(QStringLiteral("Nayara Lanes"), ghost);
        QCOMPARE(dm.name, QStringLiteral("Nayara Lanes"));
        QVERIFY(dm.networkLabel.isEmpty());

        // The hero suffix is stripped for a bridged DM: the extras are the
        // ghost and the bridge bot, not people.
        dm = presentableDmName(QStringLiteral("Sim, and 2 others"),
                               QStringLiteral("@signal_uuid7:beeper.local"));
        QCOMPARE(dm.name, QStringLiteral("Sim"));
        dm = presentableDmName(QStringLiteral("Sim and 1 other"),
                               QStringLiteral("@signal_uuid7:beeper.local"));
        QCOMPARE(dm.name, QStringLiteral("Sim"));

        // A ghost localpart is never a name; the caller gets the network label
        // for its "<label> contact" placeholder. Both the full user id and the
        // bare localpart occur.
        dm = presentableDmName(QStringLiteral("linkedin_a_co_x9"), ghost);
        QVERIFY(dm.name.isEmpty());
        QCOMPARE(dm.networkLabel, QStringLiteral("LinkedIn"));
        dm = presentableDmName(ghost, ghost);
        QVERIFY(dm.name.isEmpty());
        QCOMPARE(dm.networkLabel, QStringLiteral("LinkedIn"));

        // ...unless the remote id is a phone number, which is a humane name.
        dm = presentableDmName(QStringLiteral("@signal_+447700900123:beeper.local"),
                               QStringLiteral("@signal_+447700900123:beeper.local"));
        QCOMPARE(dm.name, QStringLiteral("+447700900123"));
        dm = presentableDmName(QStringLiteral("whatsapp_447700900123"),
                               QStringLiteral("@whatsapp_447700900123:beeper.local"));
        QCOMPARE(dm.name, QStringLiteral("447700900123"));
        // A UUID or username remainder is machine identity, not a phone.
        dm = presentableDmName(QStringLiteral("@signal_9a2f-4b:beeper.local"),
                               QStringLiteral("@signal_9a2f-4b:beeper.local"));
        QVERIFY(dm.name.isEmpty());
        QCOMPARE(dm.networkLabel, QStringLiteral("Signal"));

        // An empty computed name still yields the placeholder.
        dm = presentableDmName(QString(), ghost);
        QVERIFY(dm.name.isEmpty());
        QCOMPARE(dm.networkLabel, QStringLiteral("LinkedIn"));

        // A native Matrix DM is untouched, even if its name has the
        // hero-suffix shape.
        dm = presentableDmName(QStringLiteral("Alice, and 2 others"),
                               QStringLiteral("@alice:example.org"));
        QCOMPARE(dm.name, QStringLiteral("Alice, and 2 others"));
        QVERIFY(dm.networkLabel.isEmpty());
    }

    // ---- MSC2346: what the bridge advertises ----
    // A bridged group has no `m.direct` ghost or portal alias, so bridge room
    // state is its only signal. The advertised text is attacker-writable, so
    // precedence is what matters.

    void aKnownProtocolAlwaysGetsOurOwnLabel()
    {
        // The curated table wins over what the bridge calls itself: a room
        // admin can write "protocol.displayname" freely.
        auto badge = labelForAdvertisedBridge(
            QStringLiteral("whatsapp"),
            QStringLiteral("WhatsApp (verified by admin)"),
            QStringLiteral("Definitely Real"));
        QCOMPARE(badge.networkId, QStringLiteral("whatsapp"));
        QCOMPARE(badge.label, QStringLiteral("WhatsApp"));

        // Case-folded: MSC2346 makes the id case-insensitive.
        badge = labelForAdvertisedBridge(QStringLiteral("Discord"),
                                         QStringLiteral("nope"), QString());
        QCOMPARE(badge.networkId, QStringLiteral("discord"));
        QCOMPARE(badge.label, QStringLiteral("Discord"));

        // The curated answer is the same string inference produces, so list
        // and panel cannot disagree.
        QCOMPARE(badge.label,
                 labelForNetworkId(networkIdForUserId(
                     QStringLiteral("@discord_1:example.org"))));
    }

    void anUnknownProtocolMayNameItselfOnceAndBounded()
    {
        // An unknown network may name itself, in a muted, bounded, plain-text
        // chip beside an equally attacker-chosen room name.
        auto badge = labelForAdvertisedBridge(QStringLiteral("irc"),
                                              QStringLiteral("IRC"),
                                              QStringLiteral("Freenode"));
        QCOMPARE(badge.networkId, QStringLiteral("irc"));
        QCOMPARE(badge.label, QStringLiteral("IRC"));

        // The network name is the second choice, never the first.
        badge = labelForAdvertisedBridge(QStringLiteral("irc"), QString(),
                                         QStringLiteral("Freenode"));
        QCOMPARE(badge.label, QStringLiteral("Freenode"));

        // Nothing nameable means no badge, never the raw id.
        badge = labelForAdvertisedBridge(QStringLiteral("irc"), QString(),
                                         QString());
        QVERIFY(badge.label.isEmpty());
        badge = labelForAdvertisedBridge(QString(), QString(), QString());
        QVERIFY(badge.label.isEmpty());
        QVERIFY(badge.networkId.isEmpty());
    }

    void advertisedTextCannotForgeLayoutOrRunAway()
    {
        // Rust sanitises this already (rust/src/bridges.rs); this is the
        // second gate for other backends. A right-to-left override would
        // reverse everything after it, a spoofing surface.
        const QString hostile =
            QStringLiteral("Disc") + QChar(0x202E) + QStringLiteral("drocsi")
            + QChar(0x202C) + QStringLiteral("ord") + QChar(0x200F);
        auto badge = labelForAdvertisedBridge(QStringLiteral("custom"),
                                              hostile, QString());
        QCOMPARE(badge.label, QStringLiteral("Discdrocsiord"));

        // Control characters and whitespace runs.
        badge = labelForAdvertisedBridge(
            QStringLiteral("custom"),
            // BEL spelled \a: a \u escape below U+00A0 is ill-formed C++.
            QStringLiteral("  Free \anode \n\t IRC  "), QString());
        QCOMPARE(badge.label, QStringLiteral("Free node IRC"));

        // One line: a chip sits beside a name.
        badge = labelForAdvertisedBridge(QStringLiteral("custom"),
                                         QString(80, QLatin1Char('x')),
                                         QString());
        QVERIFY2(badge.label.size() <= 24, qPrintable(badge.label));
        QVERIFY(badge.label.endsWith(QChar(0x2026)));
    }

    // A bridged group room shows the advertised badge in the room list,
    // where inference has nothing to go on.
    void theRoomListShowsAnAdvertisedBridgeOnANonDirectRoom()
    {
        FakeClient client;
        RoomListModel model;
        RoomInfo group = plainRoom(QStringLiteral("!portal:example.org"));
        client.mirror = { group };
        model.setClient(&client);
        QCOMPARE(model.rowCount(), 1);

        // Before the answer: no badge.
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

        // The panel reads findRoom(); the two never disagree.
        const QVariantMap found = model.findRoom(group.id);
        QCOMPARE(found.value(QStringLiteral("bridgeLabel")).toString(),
                 QStringLiteral("Discord"));
        QCOMPARE(found.value(QStringLiteral("bridgeNetwork")).toString(),
                 QStringLiteral("discord"));
    }

    void theInferenceStillAnswersWhenNothingIsAdvertised()
    {
        // The advertisement supplements inference: a bridged DM whose bridge
        // publishes no MSC2346 state keeps its badge.
        FakeClient client;
        RoomListModel model;
        RoomInfo dm = plainRoom(QStringLiteral("!dm:example.org"));
        dm.isDirect = true;
        dm.directUserId = QStringLiteral("@whatsapp_447700900123:example.org");
        client.mirror = { dm };
        model.setClient(&client);
        QCOMPARE(labelAt(model, 0), QStringLiteral("WhatsApp"));

        // "Advertises no bridge" is not evidence of not bridged, so it does not
        // erase a correct inference.
        model.setAdvertisedBridge(dm.id, QString(), QString());
        QCOMPARE(labelAt(model, 0), QStringLiteral("WhatsApp"));

        // An advertisement nobody could name is the same non-answer.
        model.setAdvertisedBridge(dm.id, QStringLiteral("mystery"), QString());
        QCOMPARE(labelAt(model, 0), QStringLiteral("WhatsApp"));

        // A real advertisement overrides the localpart convention.
        model.setAdvertisedBridge(dm.id, QStringLiteral("signal"),
                                  QStringLiteral("Signal"));
        QCOMPARE(labelAt(model, 0), QStringLiteral("Signal"));
        QCOMPARE(networkAt(model, 0), QStringLiteral("signal"));
    }

    void advertisedAnswersAreAccountScoped()
    {
        // Room ids belong to an account; bridge answers must not carry over.
        FakeClient client;
        RoomListModel model;
        RoomInfo group = plainRoom(QStringLiteral("!portal:example.org"));
        client.mirror = { group };
        model.setClient(&client);
        model.setAdvertisedBridge(group.id, QStringLiteral("discord"),
                                  QStringLiteral("Discord"));
        QCOMPARE(labelAt(model, 0), QStringLiteral("Discord"));

        client.logout(); // emits loggedOut -> clearProfileCaches
        QVERIFY2(labelAt(model, 0).isEmpty(),
                 "a bridge answer survived the session that produced it");
    }
};

QTEST_GUILESS_MAIN(BridgeNetworkTest)
#include "BridgeNetworkTest.moc"
