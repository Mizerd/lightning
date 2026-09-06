// Bridge-network recognition. Pure string logic, so this is the one piece of
// the convergence/unified-inbox work that is fully covered by a fast unit
// test rather than by a contract scan.
//
// The interesting cases are the negatives. A false positive here mislabels a
// perfectly ordinary Matrix contact as a bridged account, which is worse
// than showing no badge at all — so the ordinary-user cases below are the
// ones that matter most.
#include <QtTest/QtTest>

#include "matrix/BridgeNetwork.h"
#include "matrix/MatrixClient.h"
#include "models/RoomListModel.h"

using namespace matrix::bridge;

namespace {

/// The smallest client the room list will accept. It exists so the badge
/// tests can drive the MODEL rather than the pure functions: the defect this
/// round fixes is not in the string logic (which was always right), it is
/// that nothing ever fed the model an answer for a non-DM room.
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
        // The bot DM is where login and bridge status live, so it has to be
        // labelled too — and it has no remote id to separate with.
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

        // An underscore in a human localpart must not be read as a network
        // separator. This is the failure mode a naive prefix split has.
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

    // A missing sigil or server part must not throw the parse off — the
    // model hands over whatever the SDK gave it.
    void toleratesPartialIdentifiers()
    {
        QCOMPARE(networkIdForUserId("whatsapp_447700900123"),
                 QStringLiteral("whatsapp"));
        QCOMPARE(networkIdForUserId("@telegram_1:"), QStringLiteral("telegram"));
    }

    void roomPrefersTheDirectPartner()
    {
        // Both present and disagreeing: the DM partner wins, because it
        // names a real remote account.
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

    // Every id the recognisers can produce must have a label, or the UI ends
    // up with a badge it cannot render.
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

    // The ghost-name repair (first observed live against a Beeper account,
    // 2026-08-28): profile-less LinkedIn DMs rendered their ghost localpart
    // ("linkedin___a_co_a_a…") and a 1:1 Signal chat rendered its bridge
    // plumbing ("Sim, and 2 others").
    void bridgedDmNamesAreHumane()
    {
        const QString ghost = QStringLiteral("@linkedin_a_co_x9:beeper.local");

        // A hydrated human name passes through untouched.
        auto dm = presentableDmName(QStringLiteral("Nayara Lanes"), ghost);
        QCOMPARE(dm.name, QStringLiteral("Nayara Lanes"));
        QVERIFY(dm.networkLabel.isEmpty());

        // The hero arithmetic is stripped for a bridged DM: the extras are
        // the ghost and the bridge bot, not people.
        dm = presentableDmName(QStringLiteral("Sim, and 2 others"),
                               QStringLiteral("@signal_uuid7:beeper.local"));
        QCOMPARE(dm.name, QStringLiteral("Sim"));
        dm = presentableDmName(QStringLiteral("Sim and 1 other"),
                               QStringLiteral("@signal_uuid7:beeper.local"));
        QCOMPARE(dm.name, QStringLiteral("Sim"));

        // A ghost localpart is never presented as a name — the caller gets
        // the network label for its "<label> contact" placeholder. Both the
        // full user-id form and the bare localpart occur.
        dm = presentableDmName(QStringLiteral("linkedin_a_co_x9"), ghost);
        QVERIFY(dm.name.isEmpty());
        QCOMPARE(dm.networkLabel, QStringLiteral("LinkedIn"));
        dm = presentableDmName(ghost, ghost);
        QVERIFY(dm.name.isEmpty());
        QCOMPARE(dm.networkLabel, QStringLiteral("LinkedIn"));

        // …unless the remote id reads as a phone number, which IS a humane
        // name for a phone-network chat.
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

        // A NATIVE Matrix DM is untouchable: no network, no surgery — even
        // when the name happens to contain the hero-suffix shape.
        dm = presentableDmName(QStringLiteral("Alice, and 2 others"),
                               QStringLiteral("@alice:example.org"));
        QCOMPARE(dm.name, QStringLiteral("Alice, and 2 others"));
        QVERIFY(dm.networkLabel.isEmpty());
    }

    // ── MSC2346: what the bridge itself advertises ───────────────────────
    //
    // The reported defect ("bridge tags appear only on direct messages") is
    // structural: everything above needs a ghost mxid from `m.direct` or a
    // portal alias, and a bridged GROUP has neither. These cases cover the
    // answer that does reach a group, and above all its PRECEDENCE — the
    // advertised text is attacker-writable room state.

    void aKnownProtocolAlwaysGetsOurOwnLabel()
    {
        // The curated table wins over whatever the bridge would rather be
        // called. A room admin can write "protocol.displayname" freely, and
        // "WhatsApp (verified)" or "Signal — official" must be unable to
        // reach a chip when the protocol id is one we recognise.
        auto badge = labelForAdvertisedBridge(
            QStringLiteral("whatsapp"),
            QStringLiteral("WhatsApp (verified by admin)"),
            QStringLiteral("Definitely Real"));
        QCOMPARE(badge.networkId, QStringLiteral("whatsapp"));
        QCOMPARE(badge.label, QStringLiteral("WhatsApp"));

        // Case-folded, because MSC2346 calls the id case-insensitive.
        badge = labelForAdvertisedBridge(QStringLiteral("Discord"),
                                         QStringLiteral("nope"), QString());
        QCOMPARE(badge.networkId, QStringLiteral("discord"));
        QCOMPARE(badge.label, QStringLiteral("Discord"));

        // And the curated answer is the SAME string the inference produces,
        // so a bridged DM cannot read one way in the list and another in the
        // panel depending on which signal answered.
        QCOMPARE(badge.label,
                 labelForNetworkId(networkIdForUserId(
                     QStringLiteral("@discord_1:example.org"))));
    }

    void anUnknownProtocolMayNameItselfOnceAndBounded()
    {
        // A bridge for a network the table has never heard of is exactly the
        // case MSC2346 is worth reading for, so it may name itself — in a
        // muted chip, as plain text, bounded, beside a room name that is
        // equally attacker-chosen.
        auto badge = labelForAdvertisedBridge(QStringLiteral("irc"),
                                              QStringLiteral("IRC"),
                                              QStringLiteral("Freenode"));
        QCOMPARE(badge.networkId, QStringLiteral("irc"));
        QCOMPARE(badge.label, QStringLiteral("IRC"));

        // The network name is the second choice, never the first.
        badge = labelForAdvertisedBridge(QStringLiteral("irc"), QString(),
                                         QStringLiteral("Freenode"));
        QCOMPARE(badge.label, QStringLiteral("Freenode"));

        // Nothing nameable is NO badge, never a guess at the raw id.
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
        // second gate, because the badge is also reachable from a backend
        // that is not the Rust one. A right-to-left override reverses
        // everything drawn after it, which is a spoofing surface in a chip.
        const QString hostile =
            QStringLiteral("Disc") + QChar(0x202E) + QStringLiteral("drocsi")
            + QChar(0x202C) + QStringLiteral("ord") + QChar(0x200F);
        auto badge = labelForAdvertisedBridge(QStringLiteral("custom"),
                                              hostile, QString());
        QCOMPARE(badge.label, QStringLiteral("Discdrocsiord"));

        // Control characters and whitespace runs.
        badge = labelForAdvertisedBridge(
            QStringLiteral("custom"),
            // BEL, spelled \a: a \u escape below U+00A0 is ill-formed C++.
            QStringLiteral("  Free \anode \n\t IRC  "), QString());
        QCOMPARE(badge.label, QStringLiteral("Free node IRC"));

        // And it cannot be a paragraph: a chip is one line beside a name.
        badge = labelForAdvertisedBridge(QStringLiteral("custom"),
                                         QString(80, QLatin1Char('x')),
                                         QString());
        QVERIFY2(badge.label.size() <= 24, qPrintable(badge.label));
        QVERIFY(badge.label.endsWith(QChar(0x2026)));
    }

    // The defect itself, at the layer that showed it. A bridged GROUP has no
    // DM partner and no portal alias, so the inference answers nothing and
    // the row showed no badge however obviously bridged the room was.
    void theRoomListShowsAnAdvertisedBridgeOnANonDirectRoom()
    {
        FakeClient client;
        RoomListModel model;
        RoomInfo group = plainRoom(QStringLiteral("!portal:example.org"));
        client.mirror = { group };
        model.setClient(&client);
        QCOMPARE(model.rowCount(), 1);

        // Before the answer: nothing to infer from, so no badge. This is the
        // reported behaviour.
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

        // The panel reads findRoom(), and the two must never disagree.
        const QVariantMap found = model.findRoom(group.id);
        QCOMPARE(found.value(QStringLiteral("bridgeLabel")).toString(),
                 QStringLiteral("Discord"));
        QCOMPARE(found.value(QStringLiteral("bridgeNetwork")).toString(),
                 QStringLiteral("discord"));
    }

    void theInferenceStillAnswersWhenNothingIsAdvertised()
    {
        // The advertisement SUPPLEMENTS the inference; it does not replace
        // it. A bridged DM whose bridge publishes no MSC2346 state — which
        // is most of them today — must keep the badge it already had.
        FakeClient client;
        RoomListModel model;
        RoomInfo dm = plainRoom(QStringLiteral("!dm:example.org"));
        dm.isDirect = true;
        dm.directUserId = QStringLiteral("@whatsapp_447700900123:example.org");
        client.mirror = { dm };
        model.setClient(&client);
        QCOMPARE(labelAt(model, 0), QStringLiteral("WhatsApp"));

        // "This room advertises no bridge" is not evidence that the room is
        // not bridged, so it must not erase an inference that is correct.
        model.setAdvertisedBridge(dm.id, QString(), QString());
        QCOMPARE(labelAt(model, 0), QStringLiteral("WhatsApp"));

        // An advertisement nobody could name is the same non-answer.
        model.setAdvertisedBridge(dm.id, QStringLiteral("mystery"), QString());
        QCOMPARE(labelAt(model, 0), QStringLiteral("WhatsApp"));

        // A real advertisement DOES override it: the bridge knows better
        // than a localpart convention does.
        model.setAdvertisedBridge(dm.id, QStringLiteral("signal"),
                                  QStringLiteral("Signal"));
        QCOMPARE(labelAt(model, 0), QStringLiteral("Signal"));
        QCOMPARE(networkAt(model, 0), QStringLiteral("signal"));
    }

    void advertisedAnswersAreAccountScoped()
    {
        // Room ids belong to an account. Carrying one account's bridge
        // answers into the next one would label the wrong rooms.
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
