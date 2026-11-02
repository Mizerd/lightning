
#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "app/SettingsManager.h"
#include "matrix/MockMatrixClient.h"
#include "models/EmojiCatalog.h"
#include "privacy/AccountPrefs.h"
#include "privacy/AccountPrefsController.h"

using namespace AccountPrefs;

namespace {

class FakePrefsClient : public MockMatrixClient
{
public:
    using MockMatrixClient::MockMatrixClient;
    void readAccountPref(const QString &type) override { reads << type; }
    bool supportsAccountPrefs() const override { return true; }
    void requestInvitePolicySupport() override { ++supportProbes; }
    int supportProbes = 0;
    void writeAccountPref(const QString &type, const QVariantMap &content) override
    {
        writes.append({ type, content });
    }
    QStringList reads;
    QList<QPair<QString, QVariantMap>> writes;
};

QVariantList pair(const QString &emoji, int count)
{
    return QVariantList{ emoji, count };
}

}

class AccountPrefsTest : public QObject
{
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> m_config;

private Q_SLOTS:
    void initTestCase()
    {
        m_config = std::make_unique<QTemporaryDir>();
        QVERIFY(m_config->isValid());
        qputenv("XDG_CONFIG_HOME", m_config->path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("LightningAccountPrefsTest"));
        QCoreApplication::setApplicationName(QStringLiteral("accountprefs"));
    }

    void anAbsentFieldIsOnButAnUnknownValueIsOff()
    {
        auto none = parseMediaPreview({});
        QCOMPARE(none.mediaPreviews, QStringLiteral("on"));
        QVERIFY(none.inviteAvatars);
        auto odd = parseMediaPreview({ { "media_previews", "sometimes" },
                                       { "invite_avatars", "maybe" } });
        QCOMPARE(odd.mediaPreviews, QStringLiteral("off"));
        QVERIFY(!odd.inviteAvatars);
        auto typed = parseMediaPreview({ { "media_previews", 1 },
                                         { "invite_avatars", true } });
        QCOMPARE(typed.mediaPreviews, QStringLiteral("off"));
        QVERIFY(!typed.inviteAvatars);
        QCOMPARE(parseMediaPreview({ { "media_previews", "private" } }).mediaPreviews,
                 QStringLiteral("private"));
        QCOMPARE(parseMediaPreview({ { "media_previews", "off" } }).mediaPreviews,
                 QStringLiteral("off"));
        auto partial = parseMediaPreview({ { "invite_avatars", "off" } });
        QCOMPARE(partial.mediaPreviews, QStringLiteral("on"));
        QVERIFY(!partial.inviteAvatars);
    }

    void privateModeFollowsTheJoinRuleWithACompatibilityGuard()
    {
        const QString on = QStringLiteral("on"), priv = QStringLiteral("private"),
                      off = QStringLiteral("off");
        const QString pub = QStringLiteral("public"), inv = QStringLiteral("invite");
        QVERIFY(!collapsesMedia(on, pub, false, false, false));
        QVERIFY(collapsesMedia(off, inv, true, true, true));
        QVERIFY(collapsesMedia(QStringLiteral("anything else"), inv, true, true, true));
        QVERIFY2(collapsesMedia(priv, pub, true, false, true),
                 "a public encrypted room is not private");
        QVERIFY2(!collapsesMedia(priv, inv, false, false, true),
                 "a non-public unencrypted room is private");
        QVERIFY(!collapsesMedia(priv, QStringLiteral("restricted"), false, false, true));
        QVERIFY(!collapsesMedia(priv, QStringLiteral("knock"), false, false, true));
        QVERIFY2(collapsesMedia(priv, QString(), true, true, true),
                 "an unknown join rule must collapse");
        QVERIFY(collapsesMedia(priv, QStringLiteral("future_rule"), true, true, true));
        QVERIFY(collapsesMedia(priv, inv, false, false, false));
        QVERIFY(!collapsesMedia(priv, inv, true, false, false));
        QVERIFY(!collapsesMedia(priv, inv, false, true, false));
        QVERIFY(collapsesMedia(priv, pub, true, true, false));
    }

    // A backend with no account data answers "absent" (the base
    // MatrixClient), so the fail-closed gate opens on the MSC defaults rather
    // than hiding every picture for ever on the mock and HTTP backends.
    void aBackendWithoutAccountDataAnswersAbsent()
    {
        MockMatrixClient client;
        AccountPrefsController c;
        c.setClient(&client);
        QVERIFY(c.mediaPreviewKnown());
        c.refresh();
        QTRY_VERIFY(c.mediaPreviewKnown());
        QCOMPARE(c.mediaPreviews(), QStringLiteral("on"));
        QVERIFY(!c.collapsesMedia(QStringLiteral("public"), false, false));
        QVERIFY(c.inviteAvatarsAllowed());
    }

    void everyMessageRowAsksTheControllerWithTheJoinRule()
    {
        auto read = [](const char *name) {
            QFile f(QStringLiteral(QML_DIR "/") + QLatin1String(name));
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
        };
        const QString row = read("MessageDelegate.qml");
        QVERIFY(!row.isEmpty());
        const int at = row.indexOf(QStringLiteral("readonly property bool accountCollapsesMedia:"));
        QVERIFY(at > 0);
        const QString body = row.mid(at, 700);
        QVERIFY(body.contains(QStringLiteral("prefs.collapsesMedia(root.roomJoinRule")));
        QVERIFY(body.contains(QStringLiteral("prefs.mediaPreviewKnown")));
        QVERIFY2(!body.contains(QStringLiteral("root.roomEncrypted || root.isDirectRoom")),
                 "the old encrypted-or-DM meaning is back in the row");
        for (const char *host : { "TimelinePane.qml", "ContextView.qml", "ThreadPanel.qml" })
            QVERIFY2(read(host).contains(QStringLiteral("property string roomJoinRule")), host);
    }

    void anUnknownMediaChoiceStaysCollapsedUntilTheServerAnswers()
    {
        FakePrefsClient client;
        AccountPrefsController c;
        c.setClient(&client);
        c.refresh();
        QVERIFY(client.reads.contains(QStringLiteral("m.media_preview_config")));
        Q_EMIT client.accountPrefReadFailed(QStringLiteral("m.media_preview_config"),
                                            QStringLiteral("timeout"));
        QVERIFY(!c.mediaPreviewKnown());
        QVERIFY(c.collapsesMedia(QStringLiteral("public"), false, false));
        QVERIFY(!c.inviteAvatarsAllowed());
        c.setMediaPreviews(QStringLiteral("on"));
        c.setInviteAvatars(true);
        QVERIFY2(client.writes.isEmpty(), "a write was composed from an unknown choice");

        Q_EMIT client.accountPrefRead(QStringLiteral("m.media_preview_config"), true,
            { { "media_previews", "off" }, { "invite_avatars", "off" } });
        QVERIFY(c.mediaPreviewKnown());
        QVERIFY(c.collapsesMedia(QStringLiteral("public"), false, false));
        QVERIFY(!c.inviteAvatarsAllowed());
        Q_EMIT client.accountPrefReadFailed(QStringLiteral("m.media_preview_config"),
                                            QStringLiteral("timeout"));
        QVERIFY(c.mediaPreviewKnown());
        QCOMPARE(c.mediaPreviews(), QStringLiteral("off"));
    }

    void aMediaWriteKeepsForeignFieldsAndIsHeldAgainstAStaleRead()
    {
        FakePrefsClient client;
        AccountPrefsController c;
        c.setClient(&client);
        const QVariantMap stored{ { "media_previews", "private" },
                                  { "invite_avatars", "on" },
                                  { "org.other_client.flag", 7 },
                                  { "org.lightning_matrix.private_rooms", "non_public" } };
        Q_EMIT client.accountPrefRead(QStringLiteral("m.media_preview_config"), true, stored);
        c.setInviteAvatars(false);
        QCOMPARE(client.writes.size(), 1);
        const QVariantMap written = client.writes.at(0).second;
        QCOMPARE(written.value("org.other_client.flag").toInt(), 7);
        QCOMPARE(written.value("org.lightning_matrix.private_rooms").toString(),
                 QStringLiteral("non_public"));
        QCOMPARE(written.value("invite_avatars").toString(), QStringLiteral("off"));

        client.reads.clear();
        Q_EMIT client.accountPrefWritten(QStringLiteral("m.media_preview_config"), true, {});
        QVERIFY(!c.inviteAvatars());
        QVERIFY(client.reads.contains(QStringLiteral("m.media_preview_config")));
        client.reads.clear();
        Q_EMIT client.accountPrefRead(QStringLiteral("m.media_preview_config"), true, stored);
        QVERIFY2(!c.inviteAvatars(), "a stale read snapped the choice back");
        QCOMPARE(client.reads, QStringList{ QStringLiteral("m.media_preview_config") });
        client.writes.clear();
        c.setMediaPreviews(QStringLiteral("on"));
        QCOMPARE(client.writes.size(), 1);
        QCOMPARE(client.writes.at(0).second.value("org.other_client.flag").toInt(), 7);
        QCOMPARE(client.writes.at(0).second.value("invite_avatars").toString(),
                 QStringLiteral("off"));
    }

    void theNonPublicMarkerIsReadKeptAndWrittenOnlyWhenChosen()
    {
        QVERIFY(!parseMediaPreview({ { "media_previews", "private" } }).privateMeansNonPublic);
        QVERIFY(parseMediaPreview({ { "media_previews", "private" },
                                    { "org.lightning_matrix.private_rooms", "non_public" } })
                    .privateMeansNonPublic);
        QVERIFY(!composeMediaPreview({}, QStringLiteral("private"), true, false)
                     .contains("org.lightning_matrix.private_rooms"));
        QCOMPARE(composeMediaPreview({}, QStringLiteral("private"), true, true)
                     .value("org.lightning_matrix.private_rooms").toString(),
                 QStringLiteral("non_public"));
        QCOMPARE(composeMediaPreview({ { "org.lightning_matrix.private_rooms", "non_public" } },
                                     QStringLiteral("on"), true, false)
                     .value("org.lightning_matrix.private_rooms").toString(),
                 QStringLiteral("non_public"));
    }

    void composeKeepsFieldsItDoesNotOwn()
    {
        const auto out = composeMediaPreview(
            { { "future_field", "keep me" }, { "media_previews", "on" } },
            QStringLiteral("private"), false);
        QCOMPARE(out.value("future_field").toString(), QStringLiteral("keep me"));
        QCOMPARE(out.value("media_previews").toString(), QStringLiteral("private"));
        QCOMPARE(out.value("invite_avatars").toString(), QStringLiteral("off"));
        QCOMPARE(composeMediaPreview({}, QStringLiteral("garbage"), true)
                     .value("media_previews").toString(), QStringLiteral("off"));
    }

    void recentEmojiParsingSkipsMalformedEntriesAndKeepsOrder()
    {
        const auto list = parseRecentEmoji({ { "recent_emoji", QVariantList{
            pair("👍", 5), QVariantList{ "x" }, pair("", 2), pair("😀", -1),
            QVariant(7), pair("🎉", 3), pair("👍", 9) } } });
        QCOMPARE(list.size(), 2);
        QCOMPARE(list.at(0), EmojiCount("👍", 5));
        QCOMPARE(list.at(1), EmojiCount("🎉", 3));
        QVERIFY(parseRecentEmoji({}).isEmpty());
    }

    static QVariantMap entry(const QString &emoji, const QVariant &total)
    {
        return QVariantMap{ { "emoji", emoji }, { "total", total } };
    }

    void theStableObjectShapeIsReadWithZeroLargeAndUnknownEntries()
    {
        QVariantMap withExtra = entry(QStringLiteral("🧑‍🚀"), 2);
        withExtra.insert("x-future", QStringLiteral("kept"));
        QString future;
        for (int i = 0; i < 40; ++i)
            future += QString::fromUcs4(U"\U0001F600", 1);
        const auto list = parseRecentEmoji({ { "recent_emoji", QVariantList{
            entry(QStringLiteral("👍"), 0),
            entry(QStringLiteral("🎉"), qlonglong(3000000000LL)),
            entry(QStringLiteral("🙂"), double(9007199254740991.0)),
            entry(QStringLiteral("😡"), double(9007199254740992.0)),
            entry(QStringLiteral("😢"), -1),
            entry(QStringLiteral("🤔"), 1.5),
            entry(QStringLiteral("🙃"), QStringLiteral("7")),
            QVariantMap{ { "total", 1 } },
            withExtra,
            entry(future, 1) } } });
        QStringList emojis;
        for (const auto &e : list)
            emojis << e.emoji;
        QCOMPARE(emojis, (QStringList{ "👍", "🎉", "🙂", "🧑‍🚀", future }));
        QCOMPARE(list.at(0).total, qint64(0));
        QCOMPARE(list.at(1).total, qint64(3000000000LL));
        QCOMPARE(list.at(2).total, qint64(9007199254740991LL));
        QCOMPARE(list.at(3).extra.value("x-future").toString(), QStringLiteral("kept"));
    }

    void aUseMovesTheEmojiToTheFrontAndTheWriteIsTheStableShape()
    {
        QVariantMap withExtra = entry(QStringLiteral("😀"), 2);
        withExtra.insert("x-future", 1);
        QList<EmojiCount> list = parseRecentEmoji({ { "recent_emoji", QVariantList{
            entry(QStringLiteral("👍"), 9), withExtra, entry(QStringLiteral("🦄"), 1) } } });
        list = bumpRecentEmoji(list, QStringLiteral("😀"));
        QCOMPARE(list.at(0), EmojiCount("😀", 3));
        QCOMPARE(list.at(1), EmojiCount("👍", 9));
        list = bumpRecentEmoji(list, QStringLiteral("🎉"));
        QCOMPARE(list.at(0), EmojiCount("🎉", 1));
        const auto composed = composeRecentEmoji({ { "other", 1 } }, list);
        QCOMPARE(composed.value("other").toInt(), 1);
        const QVariantList rows = composed.value("recent_emoji").toList();
        QCOMPARE(rows.size(), 4);
        for (const QVariant &row : rows)
            QCOMPARE(row.metaType().id(), int(QMetaType::QVariantMap));
        QCOMPARE(rows.at(1).toMap().value("emoji").toString(), QStringLiteral("😀"));
        QCOMPARE(rows.at(1).toMap().value("total").toLongLong(), 3LL);
        QCOMPARE(rows.at(1).toMap().value("x-future").toInt(), 1);
        QCOMPARE(rows.at(3).toMap().value("emoji").toString(), QStringLiteral("🦄"));

        QList<EmojiCount> big{ { "x", kRecentEmojiTotalBound - 1 } };
        big = bumpRecentEmoji(big, QStringLiteral("x"));
        QCOMPARE(big.at(0).total, kRecentEmojiTotalBound - 1);
        QList<EmojiCount> many;
        for (int i = 0; i < 120; ++i)
            many = bumpRecentEmoji(many, QString::number(i));
        QCOMPARE(many.size(), kRecentEmojiServerLimit);
        QCOMPARE(many.at(0).emoji, QStringLiteral("119"));
    }

    void mergeKeepsLocalFirstAndAddsTheAccountsTheRest()
    {
        const auto merged = mergeRecentEmoji(
            { "🎉", "👍" }, { EmojiCount("👍", 9), EmojiCount("😀", 4), EmojiCount("🙏", 1) }, 100);
        QCOMPARE(merged, (QStringList{ "🎉", "👍", "😀", "🙏" }));
        QCOMPARE(mergeRecentEmoji({ "a" }, { EmojiCount("b", 1), EmojiCount("c", 1) }, 2),
                 (QStringList{ "a", "b" }));
    }

    void theControllerStartsOnTheDefaultsAndFollowsTheAccount()
    {
        FakePrefsClient client;
        AccountPrefsController c;
        c.setClient(&client);
        QCOMPARE(c.mediaPreviews(), QStringLiteral("on"));
        QVERIFY(c.inviteAvatars());
        QVERIFY(!c.mediaPreviewKnown());
        QVERIFY2(c.collapsesMedia(QStringLiteral("invite"), true, true),
                 "media loads before the account's choice is known");

        Q_EMIT client.accountPrefRead(QStringLiteral("m.media_preview_config"), true,
                                      { { "media_previews", "private" },
                                        { "invite_avatars", "off" } });
        QCOMPARE(c.mediaPreviews(), QStringLiteral("private"));
        QVERIFY(!c.inviteAvatars());
        QVERIFY(c.mediaPreviewKnown());
        QVERIFY(c.collapsesMedia(QStringLiteral("invite"), false, false));
        QVERIFY(!c.collapsesMedia(QStringLiteral("invite"), true, false));
        QVERIFY(!c.privateMeansNonPublic());
        client.writes.clear();
        c.setMediaPreviews(QStringLiteral("private"));
        QVERIFY(c.privateMeansNonPublic());
        QVERIFY(!c.collapsesMedia(QStringLiteral("invite"), false, false));
        QCOMPARE(client.writes.size(), 1);
        QCOMPARE(client.writes.at(0).second.value("org.lightning_matrix.private_rooms").toString(),
                 QStringLiteral("non_public"));

        client.reads.clear();
        Q_EMIT client.accountPrefChanged(QStringLiteral("m.media_preview_config"));
        QCOMPARE(client.reads, QStringList{ QStringLiteral("m.media_preview_config") });
        Q_EMIT client.accountPrefRead(QStringLiteral("m.media_preview_config"), true,
                                      { { "media_previews", "newer-mode" } });
        QCOMPARE(c.mediaPreviews(), QStringLiteral("off"));
        QVERIFY(c.collapsesMedia(QStringLiteral("invite"), true, true));

        Q_EMIT client.accountPrefRead(QStringLiteral("m.media_preview_config"), false, {});
        QCOMPARE(c.mediaPreviews(), QStringLiteral("on"));
        QVERIFY(c.inviteAvatars());
    }

    void invitePolicyComposesTheRightWritesAndClearsEveryBlock()
    {
        using AccountPrefs::composeInvitePolicy;
        const QVariantMap none;
        QVERIFY(composeInvitePolicy(false, false, none, none, true).isEmpty());
        auto w = composeInvitePolicy(true, true, { { "extra", 1 } }, none, true);
        QCOMPARE(w.size(), 1);
        QCOMPARE(w.first().type, QStringLiteral("m.invite_permission_config"));
        QCOMPARE(w.first().content.value("default_action").toString(), QStringLiteral("block"));
        QCOMPARE(w.first().content.value("extra").toInt(), 1);
        w = composeInvitePolicy(false, true, none, none, true);
        QCOMPARE(w.size(), 1);
        QCOMPARE(w.first().type, QStringLiteral("org.matrix.msc4380.invite_permission_config"));
        QCOMPARE(w.first().content.value("block_all").toBool(), true);
        w = composeInvitePolicy(true, false, { { "default_action", "block" } },
                                { { "block_all", true } }, false);
        QCOMPARE(w.size(), 2);
        QVERIFY(!w.at(0).content.contains("default_action"));
        QCOMPARE(w.at(1).content.value("block_all").toBool(), false);
        QVERIFY(composeInvitePolicy(true, false, { { "default_action", "block" } },
                                    none, true).isEmpty());
        QVERIFY(composeInvitePolicy(true, true, none, none, false).isEmpty());
        QVERIFY(!AccountPrefs::stableInvitePolicyBlocks({ { "default_action", "Block" } }));
        QVERIFY(!AccountPrefs::unstableInvitePolicyBlocks({ { "block_all", "true" } }));
    }

    void theInvitePolicyIsOfferedOnlyWhenTheServerSupportsIt()
    {
        FakePrefsClient client;
        AccountPrefsController c;
        c.setClient(&client);
        c.refresh();
        QVERIFY(client.supportProbes > 0);
        QVERIFY(client.reads.contains(QStringLiteral("m.invite_permission_config")));
        QCOMPARE(c.invitePolicyState(), QStringLiteral("unknown"));
        c.setInvitesBlocked(true);
        QVERIFY(client.writes.isEmpty());

        Q_EMIT client.invitePolicySupportReceived(true, false, false);
        QCOMPARE(c.invitePolicyState(), QStringLiteral("unsupported"));
        Q_EMIT client.invitePolicySupportReceived(false, false, false);
        QCOMPARE(c.invitePolicyState(), QStringLiteral("offline"));
        Q_EMIT client.invitePolicySupportReceived(true, true, false);
        QCOMPARE(c.invitePolicyState(), QStringLiteral("unknown"));
        Q_EMIT client.accountPrefRead(QStringLiteral("m.invite_permission_config"), false, {});
        Q_EMIT client.accountPrefRead(
            QStringLiteral("org.matrix.msc4380.invite_permission_config"), true,
            { { "block_all", true } });
        QCOMPARE(c.invitePolicyState(), QStringLiteral("ready"));
        QVERIFY(c.invitesBlocked());

        client.writes.clear();
        client.reads.clear();
        c.setInvitesBlocked(false);
        QCOMPARE(client.writes.size(), 1);
        QVERIFY(c.invitePolicyPending());
        c.setInvitesBlocked(true);
        QCOMPARE(client.writes.size(), 1);
        QVERIFY(c.invitesBlocked());
        Q_EMIT client.accountPrefWritten(
            QStringLiteral("org.matrix.msc4380.invite_permission_config"), false,
            QStringLiteral("forbidden"));
        QVERIFY(!c.invitePolicyPending());
        QVERIFY(c.invitePolicyWriteFailed());
        QVERIFY(c.invitesBlocked());
        QVERIFY(client.reads.contains(
            QStringLiteral("org.matrix.msc4380.invite_permission_config")));
    }

    void inviteAvatarsAreFailClosedUntilTheAccountAnswers()
    {
        FakePrefsClient client;
        AccountPrefsController c;
        c.setClient(&client);
        QVERIFY(c.inviteAvatars());
        QVERIFY(!c.inviteAvatarsAllowed());
        Q_EMIT client.accountPrefRead(QStringLiteral("m.media_preview_config"), true,
                                      { { "invite_avatars", "off" } });
        QVERIFY(!c.inviteAvatarsAllowed());
        Q_EMIT client.accountPrefRead(QStringLiteral("m.media_preview_config"), false, {});
        QVERIFY(c.inviteAvatarsAllowed());
        c.setClient(nullptr);
        QVERIFY(!c.inviteAvatarsAllowed());
    }

    void aChoiceIsWrittenWithTheFieldsWeDoNotOwnAndRevertsWhenRefused()
    {
        FakePrefsClient client;
        AccountPrefsController c;
        c.setClient(&client);
        Q_EMIT client.accountPrefRead(QStringLiteral("m.media_preview_config"), true,
                                      { { "future_field", "keep" },
                                        { "media_previews", "on" } });
        c.setMediaPreviews(QStringLiteral("off"));
        QCOMPARE(c.mediaPreviews(), QStringLiteral("off"));
        QCOMPARE(client.writes.size(), 1);
        QCOMPARE(client.writes.at(0).first, QStringLiteral("m.media_preview_config"));
        QCOMPARE(client.writes.at(0).second.value("future_field").toString(),
                 QStringLiteral("keep"));
        QCOMPARE(client.writes.at(0).second.value("media_previews").toString(),
                 QStringLiteral("off"));
        Q_EMIT client.accountPrefWritten(QStringLiteral("m.media_preview_config"),
                                         false, QStringLiteral("network"));
        QCOMPARE(c.mediaPreviews(), QStringLiteral("on"));
        QVERIFY(c.writeFailed());

        client.writes.clear();
        c.setMediaPreviews(QStringLiteral("whenever"));
        QVERIFY(client.writes.isEmpty());

        c.setInviteAvatars(false);
        QVERIFY(!c.inviteAvatars());
        Q_EMIT client.accountPrefWritten(QStringLiteral("m.media_preview_config"),
                                         true, {});
        QVERIFY(!c.inviteAvatars());
        QVERIFY(!c.writeFailed());
    }

    void aWriteBeforeTheAccountsCopyWasReadIsRefused()
    {
        FakePrefsClient client;
        AccountPrefsController c;
        c.setClient(&client);
        c.setMediaPreviews(QStringLiteral("off"));
        c.setInviteAvatars(false);
        QVERIFY(client.writes.isEmpty());
        QCOMPARE(c.mediaPreviews(), QStringLiteral("on"));
    }

    void anEchoInsideTheFlushWindowKeepsUnwrittenBumps()
    {
        SettingsManager settings;
        settings.clearRecentEmoji();
        EmojiCatalog catalog(&settings);
        FakePrefsClient client;
        AccountPrefsController c;
        c.setEmojiCatalog(&catalog);
        c.setClient(&client);
        Q_EMIT client.accountPrefRead(QStringLiteral("m.recent_emoji"), true,
            { { "recent_emoji", QVariantList{ pair("👍", 2) } } });
        catalog.recordUse(QStringLiteral("🎉"));
        QVERIFY(client.writes.isEmpty());
        Q_EMIT client.accountPrefRead(QStringLiteral("m.recent_emoji"), true,
            { { "recent_emoji", QVariantList{ pair("👍", 2) } } });
        QTRY_COMPARE_WITH_TIMEOUT(client.writes.size(), 1, 5000);
        const auto rows = client.writes.at(0).second.value("recent_emoji").toList();
        QCOMPARE(rows.size(), 2);
        bool hasNew = false;
        for (const QVariant &r : rows)
            hasNew = hasNew || r.toMap().value("emoji").toString() == QStringLiteral("🎉");
        QVERIFY2(hasNew, "the echo replaced a bump that was not written yet");
        settings.clearRecentEmoji();
    }

    void signingOutReturnsToTheDefaults()
    {
        FakePrefsClient client;
        AccountPrefsController c;
        c.setClient(&client);
        Q_EMIT client.accountPrefRead(QStringLiteral("m.media_preview_config"), true,
                                      { { "media_previews", "off" },
                                        { "invite_avatars", "off" } });
        QCOMPARE(c.mediaPreviews(), QStringLiteral("off"));
        Q_EMIT client.loggedOut();
        QCOMPARE(c.mediaPreviews(), QStringLiteral("on"));
        QVERIFY(c.inviteAvatars());
        QVERIFY(!c.mediaPreviewKnown());
    }

    void recentEmojiAreMergedAndOnlyAUseIsWritten()
    {
        SettingsManager settings;
        settings.clearRecentEmoji();
        EmojiCatalog catalog(&settings);
        catalog.recordUse(QStringLiteral("🎉"));
        FakePrefsClient client;
        AccountPrefsController c;
        c.setEmojiCatalog(&catalog);
        c.setClient(&client);

        Q_EMIT client.accountPrefRead(QStringLiteral("m.recent_emoji"), true,
            { { "other", 1 }, { "recent_emoji", QVariantList{
                pair("👍", 9), pair("😀", 4) } } });
        QCOMPARE(catalog.recentEmoji(), (QStringList{ "🎉", "👍", "😀" }));
        QVERIFY(client.writes.isEmpty());

        catalog.recordUse(QStringLiteral("😀"));
        catalog.recordUse(QStringLiteral("😀"));
        QTRY_COMPARE_WITH_TIMEOUT(client.writes.size(), 1, 5000);
        const auto content = client.writes.at(0).second;
        QCOMPARE(client.writes.at(0).first, QStringLiteral("m.recent_emoji"));
        QCOMPARE(content.value("other").toInt(), 1);
        const auto rows = content.value("recent_emoji").toList();
        QCOMPARE(rows.at(0).toMap().value("emoji").toString(), QStringLiteral("😀"));
        QCOMPARE(rows.at(0).toMap().value("total").toInt(), 6);
        QCOMPARE(rows.at(1).toMap().value("emoji").toString(), QStringLiteral("👍"));
        QCOMPARE(rows.at(1).toMap().value("total").toInt(), 9);
        settings.clearRecentEmoji();
    }

    void nothingIsWrittenBeforeTheAccountsOwnListWasRead()
    {
        SettingsManager settings;
        settings.clearRecentEmoji();
        EmojiCatalog catalog(&settings);
        FakePrefsClient client;
        AccountPrefsController c;
        c.setEmojiCatalog(&catalog);
        c.setClient(&client);
        client.reads.clear();
        catalog.recordUse(QStringLiteral("🎉"));
        QCOMPARE(client.reads, QStringList{ QStringLiteral("m.recent_emoji") });
        QTest::qWait(1800);
        QVERIFY(client.writes.isEmpty());
        Q_EMIT client.accountPrefRead(QStringLiteral("m.recent_emoji"), true,
            { { "recent_emoji", QVariantList{ pair("👍", 3) } } });
        QTRY_COMPARE_WITH_TIMEOUT(client.writes.size(), 1, 5000);
        const auto rows = client.writes.at(0).second.value("recent_emoji").toList();
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows.at(0).toMap().value("emoji").toString(), QStringLiteral("🎉"));
        QCOMPARE(rows.at(1).toMap().value("emoji").toString(), QStringLiteral("👍"));
        settings.clearRecentEmoji();
    }

    void aFailedRecentsWriteIsRetriedBoundedlyAndNotClaimedSynced()
    {
        SettingsManager settings;
        settings.clearRecentEmoji();
        EmojiCatalog catalog(&settings);
        FakePrefsClient client;
        AccountPrefsController c;
        c.setRecentsRetryBaseMsForTest(40);
        c.setEmojiCatalog(&catalog);
        c.setClient(&client);
        Q_EMIT client.accountPrefRead(QStringLiteral("m.recent_emoji"), true,
            { { "recent_emoji", QVariantList{} } });
        catalog.recordUse(QStringLiteral("🎉"));
        QTRY_COMPARE_WITH_TIMEOUT(client.writes.size(), 1, 5000);
        QCOMPARE(c.recentsWriteAttempts(), 1);

        for (int attempt = 1; attempt <= 3; ++attempt) {
            Q_EMIT client.accountPrefWritten(QStringLiteral("m.recent_emoji"),
                                             false, QString());
            QVERIFY(c.recentsWriteFailed());
            if (attempt < 3) {
                QVERIFY2(c.recentsRetryScheduled(),
                         qPrintable(QStringLiteral("no retry after failure %1")
                                        .arg(attempt)));
                QTRY_COMPARE_WITH_TIMEOUT(client.writes.size(), attempt + 1, 2000);
                QCOMPARE(c.recentsWriteAttempts(), attempt + 1);
            }
        }
        QCOMPARE(client.writes.size(), 3);
        QVERIFY2(!c.recentsRetryScheduled(),
                 "a fourth attempt is scheduled after the third failure");
        QCOMPARE(c.recentsWriteAttempts(), 3);
        QTest::qWait(400);
        QCOMPARE(client.writes.size(), 3);
        QVERIFY(c.recentsWriteFailed());

        catalog.recordUse(QStringLiteral("👍"));
        QTRY_COMPARE_WITH_TIMEOUT(client.writes.size(), 4, 5000);
        QCOMPARE(c.recentsWriteAttempts(), 1);
        Q_EMIT client.accountPrefWritten(QStringLiteral("m.recent_emoji"), true, QString());
        QVERIFY(!c.recentsWriteFailed());
        QVERIFY(!c.recentsRetryScheduled());
        settings.clearRecentEmoji();
    }

    void aFailedInvitePolicyReadIsUnknownNotAllowed()
    {
        FakePrefsClient client;
        AccountPrefsController c;
        c.setClient(&client);
        Q_EMIT client.invitePolicySupportReceived(true, true, false);
        Q_EMIT client.accountPrefReadFailed(QStringLiteral("m.invite_permission_config"),
                                            QStringLiteral("timeout"));
        Q_EMIT client.accountPrefRead(
            QStringLiteral("org.matrix.msc4380.invite_permission_config"), false, {});
        QCOMPARE(c.invitePolicyState(), QStringLiteral("offline"));
        c.setInvitesBlocked(true);
        c.setInvitesBlocked(false);
        QVERIFY2(client.writes.isEmpty(), "a write was composed from an unknown policy");

        Q_EMIT client.accountPrefRead(QStringLiteral("m.invite_permission_config"), true,
            { { "default_action", "block" }, { "future", "keep" } });
        QCOMPARE(c.invitePolicyState(), QStringLiteral("ready"));
        QVERIFY(c.invitesBlocked());
        c.setInvitesBlocked(false);
        QCOMPARE(client.writes.size(), 1);
        QVERIFY(!client.writes.at(0).second.contains("default_action"));
        QCOMPARE(client.writes.at(0).second.value("future").toString(),
                 QStringLiteral("keep"));
    }

    void aConfirmedInviteWriteIsHeldAgainstAStaleRead()
    {
        FakePrefsClient client;
        AccountPrefsController c;
        c.setClient(&client);
        Q_EMIT client.invitePolicySupportReceived(true, true, false);
        Q_EMIT client.accountPrefRead(QStringLiteral("m.invite_permission_config"), false, {});
        Q_EMIT client.accountPrefRead(
            QStringLiteral("org.matrix.msc4380.invite_permission_config"), false, {});
        QCOMPARE(c.invitePolicyState(), QStringLiteral("ready"));
        c.setInvitesBlocked(true);
        QCOMPARE(client.writes.size(), 1);
        client.reads.clear();
        Q_EMIT client.accountPrefWritten(QStringLiteral("m.invite_permission_config"),
                                         true, QString());
        QVERIFY(!c.invitePolicyPending());
        QVERIFY2(c.invitesBlocked(), "a confirmed block is not shown at once");
        QVERIFY(client.reads.contains(QStringLiteral("m.invite_permission_config")));

        client.reads.clear();
        Q_EMIT client.accountPrefRead(QStringLiteral("m.invite_permission_config"), false, {});
        QVERIFY2(c.invitesBlocked(), "a stale read snapped the switch back");
        QCOMPARE(client.reads, QStringList{ QStringLiteral("m.invite_permission_config") });

        client.writes.clear();
        c.setInvitesBlocked(false);
        QCOMPARE(client.writes.size(), 1);
        QVERIFY(!client.writes.at(0).second.contains("default_action"));
        Q_EMIT client.accountPrefWritten(QStringLiteral("m.invite_permission_config"),
                                         true, QString());
        QVERIFY(!c.invitesBlocked());

        Q_EMIT client.accountPrefRead(QStringLiteral("m.invite_permission_config"), true, {});
        Q_EMIT client.accountPrefRead(QStringLiteral("m.invite_permission_config"), true,
            { { "default_action", "block" } });
        QVERIFY(c.invitesBlocked());
    }
};

QTEST_MAIN(AccountPrefsTest)
#include "AccountPrefsTest.moc"
