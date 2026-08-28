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

using namespace matrix::bridge;

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
};

QTEST_APPLESS_MAIN(BridgeNetworkTest)
#include "BridgeNetworkTest.moc"
