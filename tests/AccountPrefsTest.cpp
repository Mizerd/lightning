// MSC4278 `m.media_preview_config` and Element's `m.recent_emoji`: the pure
// parsing/composing rules and the controller that honours them.
//
// The rules that matter, each pinned below: an ABSENT field is the MSC's
// default (on) but an UNKNOWN value is off; a write keeps the fields this
// build does not own; a refused write puts the old value back; a sign-out
// returns to the defaults; Lightning's recents are merged with the account's
// and never uploaded in bulk, and nothing is written before the account's own
// list has been read.

#include <QtTest/QtTest>

#include <QCoreApplication>
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

} // namespace

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

    // ---- MSC4278 parsing ------------------------------------------------
    void anAbsentFieldIsOnButAnUnknownValueIsOff()
    {
        // Nothing stored: the MSC's default, load everything.
        auto none = parseMediaPreview({});
        QCOMPARE(none.mediaPreviews, QStringLiteral("on"));
        QVERIFY(none.inviteAvatars);
        // A mode this build has never heard of (a newer client's) is OFF,
        // not "on": it must never read as permission to load.
        auto odd = parseMediaPreview({ { "media_previews", "sometimes" },
                                       { "invite_avatars", "maybe" } });
        QCOMPARE(odd.mediaPreviews, QStringLiteral("off"));
        QVERIFY(!odd.inviteAvatars);
        // Wrong JSON types are unknown values too.
        auto typed = parseMediaPreview({ { "media_previews", 1 },
                                         { "invite_avatars", true } });
        QCOMPARE(typed.mediaPreviews, QStringLiteral("off"));
        QVERIFY(!typed.inviteAvatars);
        // The three real modes, and a partial object (only one field).
        QCOMPARE(parseMediaPreview({ { "media_previews", "private" } }).mediaPreviews,
                 QStringLiteral("private"));
        QCOMPARE(parseMediaPreview({ { "media_previews", "off" } }).mediaPreviews,
                 QStringLiteral("off"));
        auto partial = parseMediaPreview({ { "invite_avatars", "off" } });
        QCOMPARE(partial.mediaPreviews, QStringLiteral("on"));
        QVERIFY(!partial.inviteAvatars);
    }

    // Master prompt Part 6, 6 (UI-043/044): "Private rooms" is a non-public
    // join rule, not encryption or a DM; an unknown join rule collapses; and
    // until the account adopts the new meaning the old, narrower set is the
    // most that loads (never silently broadened).
    void privateModeFollowsTheJoinRuleWithACompatibilityGuard()
    {
        const QString on = QStringLiteral("on"), priv = QStringLiteral("private"),
                      off = QStringLiteral("off");
        const QString pub = QStringLiteral("public"), inv = QStringLiteral("invite");
        QVERIFY(!collapsesMedia(on, pub, false, false, false));
        QVERIFY(collapsesMedia(off, inv, true, true, true));
        QVERIFY(collapsesMedia(QStringLiteral("anything else"), inv, true, true, true));
        // Adopted: the join rule alone decides.
        QVERIFY2(collapsesMedia(priv, pub, true, false, true),
                 "a public encrypted room is not private");
        QVERIFY2(!collapsesMedia(priv, inv, false, false, true),
                 "a non-public unencrypted room is private");
        QVERIFY(!collapsesMedia(priv, QStringLiteral("restricted"), false, false, true));
        QVERIFY(!collapsesMedia(priv, QStringLiteral("knock"), false, false, true));
        QVERIFY2(collapsesMedia(priv, QString(), true, true, true),
                 "an unknown join rule must collapse");
        QVERIFY(collapsesMedia(priv, QStringLiteral("future_rule"), true, true, true));
        // Not adopted: non-public AND (encrypted or DM), so nothing that the
        // old meaning collapsed starts loading.
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
        // Known at once: there is no stored choice to wait for.
        QVERIFY(c.mediaPreviewKnown());
        // And a read (after a sign-in reset) is answered "absent".
        c.refresh();
        QTRY_VERIFY(c.mediaPreviewKnown());
        QCOMPARE(c.mediaPreviews(), QStringLiteral("on"));
        QVERIFY(!c.collapsesMedia(QStringLiteral("public"), false, false));
        QVERIFY(c.inviteAvatarsAllowed());
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
        // Another client's later write keeps it (fields we do not own are
        // carried by them, and ours carries it here).
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
        // Never writes a mode it does not know how to read back.
        QCOMPARE(composeMediaPreview({}, QStringLiteral("garbage"), true)
                     .value("media_previews").toString(), QStringLiteral("off"));
    }

    // ---- m.recent_emoji ---------------------------------------------------
    void recentEmojiParsingSkipsMalformedEntriesAndKeepsOrder()
    {
        // Legacy [emoji, count] tuples are still imported (PACK-REC-002).
        const auto list = parseRecentEmoji({ { "recent_emoji", QVariantList{
            pair("👍", 5), QVariantList{ "x" }, pair("", 2), pair("😀", -1),
            QVariant(7), pair("🎉", 3), pair("👍", 9) } } });
        QCOMPARE(list.size(), 2);
        QCOMPARE(list.at(0), EmojiCount("👍", 5)); // first wins, order kept
        QCOMPARE(list.at(1), EmojiCount("🎉", 3));
        QVERIFY(parseRecentEmoji({}).isEmpty());
    }

    static QVariantMap entry(const QString &emoji, const QVariant &total)
    {
        return QVariantMap{ { "emoji", emoji }, { "total", total } };
    }

    void theStableObjectShapeIsReadWithZeroLargeAndUnknownEntries()
    {
        // PACK-REC-001/004/005/006/007/014: the old parser accepted only
        // tuples, so a stable list from another client read as EMPTY and the
        // next local use replaced it.
        QVariantMap withExtra = entry(QStringLiteral("🧑‍🚀"), 2);
        withExtra.insert("x-future", QStringLiteral("kept"));
        QString future;  // a long sequence of a non-BMP code point
        for (int i = 0; i < 40; ++i)
            future += QString::fromUcs4(U"\U0001F600", 1);
        const auto list = parseRecentEmoji({ { "recent_emoji", QVariantList{
            entry(QStringLiteral("👍"), 0),
            entry(QStringLiteral("🎉"), qlonglong(3000000000LL)),
            entry(QStringLiteral("🙂"), double(9007199254740991.0)),  // 2^53-1
            entry(QStringLiteral("😡"), double(9007199254740992.0)),  // 2^53: invalid
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
        // PACK-REC-003/013: wire order is recency; a remote object list is
        // kept and written back as objects, never as tuples.
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
        // An entry the catalogue does not know survives (PACK-REC-004).
        QCOMPARE(rows.at(3).toMap().value("emoji").toString(), QStringLiteral("🦄"));

        // Saturates below 2^53 and never decreases a received value.
        QList<EmojiCount> big{ { "x", kRecentEmojiTotalBound - 1 } };
        big = bumpRecentEmoji(big, QStringLiteral("x"));
        QCOMPARE(big.at(0).total, kRecentEmojiTotalBound - 1);
        // Bounded at 100.
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

    // ---- the controller -------------------------------------------------
    void theControllerStartsOnTheDefaultsAndFollowsTheAccount()
    {
        FakePrefsClient client;
        AccountPrefsController c;
        c.setClient(&client);
        QCOMPARE(c.mediaPreviews(), QStringLiteral("on"));
        QVERIFY(c.inviteAvatars());
        QVERIFY(!c.mediaPreviewKnown());
        // Fail-closed while the account's choice is loading (UI-042).
        QVERIFY2(c.collapsesMedia(QStringLiteral("invite"), true, true),
                 "media loads before the account's choice is known");

        Q_EMIT client.accountPrefRead(QStringLiteral("m.media_preview_config"), true,
                                      { { "media_previews", "private" },
                                        { "invite_avatars", "off" } });
        QCOMPARE(c.mediaPreviews(), QStringLiteral("private"));
        QVERIFY(!c.inviteAvatars());
        QVERIFY(c.mediaPreviewKnown());
        // Stored "private" without the marker: the compatibility guard.
        QVERIFY(c.collapsesMedia(QStringLiteral("invite"), false, false));
        QVERIFY(!c.collapsesMedia(QStringLiteral("invite"), true, false));
        QVERIFY(!c.privateMeansNonPublic());
        // Choosing it again adopts the non-public meaning, in the write too.
        client.writes.clear();
        c.setMediaPreviews(QStringLiteral("private"));
        QVERIFY(c.privateMeansNonPublic());
        QVERIFY(!c.collapsesMedia(QStringLiteral("invite"), false, false));
        QCOMPARE(client.writes.size(), 1);
        QCOMPARE(client.writes.at(0).second.value("org.lightning_matrix.private_rooms").toString(),
                 QStringLiteral("non_public"));

        // A change from another device re-reads; the reply is adopted.
        client.reads.clear();
        Q_EMIT client.accountPrefChanged(QStringLiteral("m.media_preview_config"));
        QCOMPARE(client.reads, QStringList{ QStringLiteral("m.media_preview_config") });
        Q_EMIT client.accountPrefRead(QStringLiteral("m.media_preview_config"), true,
                                      { { "media_previews", "newer-mode" } });
        QCOMPARE(c.mediaPreviews(), QStringLiteral("off"));
        QVERIFY(c.collapsesMedia(QStringLiteral("invite"), true, true));

        // The data being deleted elsewhere returns the defaults.
        Q_EMIT client.accountPrefRead(QStringLiteral("m.media_preview_config"), false, {});
        QCOMPARE(c.mediaPreviews(), QStringLiteral("on"));
        QVERIFY(c.inviteAvatars());
    }

    // UI-041/042: an invitation avatar is a stranger's picture, and a fetch
    // made before the account's answer cannot be undone when the answer is
    // "off". inviteAvatars keeps the MSC default (on) for display of the
    // setting; inviteAvatarsAllowed, which every invitation surface reads, is
    // false until the account's own copy is known.
    // Master prompt Part 6, 6 (UI-045/046/047): the incoming-invite policy.
    void invitePolicyComposesTheRightWritesAndClearsEveryBlock()
    {
        using AccountPrefs::composeInvitePolicy;
        const QVariantMap none;
        // Neither form supported: nothing is written (the setting is hidden).
        QVERIFY(composeInvitePolicy(false, false, none, none, true).isEmpty());
        // Stable preferred; unknown fields kept.
        auto w = composeInvitePolicy(true, true, { { "extra", 1 } }, none, true);
        QCOMPARE(w.size(), 1);
        QCOMPARE(w.first().type, QStringLiteral("m.invite_permission_config"));
        QCOMPARE(w.first().content.value("default_action").toString(), QStringLiteral("block"));
        QCOMPARE(w.first().content.value("extra").toInt(), 1);
        // Unstable only.
        w = composeInvitePolicy(false, true, none, none, true);
        QCOMPARE(w.size(), 1);
        QCOMPARE(w.first().type, QStringLiteral("org.matrix.msc4380.invite_permission_config"));
        QCOMPARE(w.first().content.value("block_all").toBool(), true);
        // Allow-all clears BOTH forms, even the one the server no longer
        // advertises, so no stale rule keeps rejecting.
        w = composeInvitePolicy(true, false, { { "default_action", "block" } },
                                { { "block_all", true } }, false);
        QCOMPARE(w.size(), 2);
        QVERIFY(!w.at(0).content.contains("default_action"));
        QCOMPARE(w.at(1).content.value("block_all").toBool(), false);
        // Nothing to do when the account already says so.
        QVERIFY(composeInvitePolicy(true, false, { { "default_action", "block" } },
                                    none, true).isEmpty());
        QVERIFY(composeInvitePolicy(true, true, none, none, false).isEmpty());
        // Unknown values allow (the spec), a string "true" is not block_all.
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
        // Supported, but the account's copies are not read yet.
        QCOMPARE(c.invitePolicyState(), QStringLiteral("unknown"));
        Q_EMIT client.accountPrefRead(QStringLiteral("m.invite_permission_config"), false, {});
        Q_EMIT client.accountPrefRead(
            QStringLiteral("org.matrix.msc4380.invite_permission_config"), true,
            { { "block_all", true } });
        QCOMPARE(c.invitePolicyState(), QStringLiteral("ready"));
        // A residual unstable block shows as blocked.
        QVERIFY(c.invitesBlocked());

        client.writes.clear();
        client.reads.clear();
        c.setInvitesBlocked(false);
        QCOMPARE(client.writes.size(), 1);
        QVERIFY(c.invitePolicyPending());
        // A second request while pending is refused.
        c.setInvitesBlocked(true);
        QCOMPARE(client.writes.size(), 1);
        // The switch keeps the CONFIRMED value until the account says so.
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
        // Nothing stored: the MSC default applies once that is KNOWN.
        Q_EMIT client.accountPrefRead(QStringLiteral("m.media_preview_config"), false, {});
        QVERIFY(c.inviteAvatarsAllowed());
        // A sign-out or switch forgets it again.
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
        QCOMPARE(c.mediaPreviews(), QStringLiteral("off")); // optimistic
        QCOMPARE(client.writes.size(), 1);
        QCOMPARE(client.writes.at(0).first, QStringLiteral("m.media_preview_config"));
        QCOMPARE(client.writes.at(0).second.value("future_field").toString(),
                 QStringLiteral("keep"));
        QCOMPARE(client.writes.at(0).second.value("media_previews").toString(),
                 QStringLiteral("off"));
        Q_EMIT client.accountPrefWritten(QStringLiteral("m.media_preview_config"),
                                         false, QStringLiteral("network"));
        QCOMPARE(c.mediaPreviews(), QStringLiteral("on")); // the account's value
        QVERIFY(c.writeFailed());

        // An unknown mode is refused before any write.
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
        // Composed from nothing it would drop the fields we do not own.
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
        catalog.recordUse(QStringLiteral("🎉")); // bump pending, not written
        QVERIFY(client.writes.isEmpty());
        // An older echo (still without the new emoji) arrives meanwhile.
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

        // The account's list arrives: merged in, nothing uploaded for it.
        Q_EMIT client.accountPrefRead(QStringLiteral("m.recent_emoji"), true,
            { { "other", 1 }, { "recent_emoji", QVariantList{
                pair("👍", 9), pair("😀", 4) } } });
        QCOMPARE(catalog.recentEmoji(), (QStringList{ "🎉", "👍", "😀" }));
        QVERIFY(client.writes.isEmpty());

        // A use is bumped on the account's list, kept with its other fields,
        // and written once for a burst.
        catalog.recordUse(QStringLiteral("😀"));
        catalog.recordUse(QStringLiteral("😀"));
        QTRY_COMPARE_WITH_TIMEOUT(client.writes.size(), 1, 5000);
        const auto content = client.writes.at(0).second;
        QCOMPARE(client.writes.at(0).first, QStringLiteral("m.recent_emoji"));
        QCOMPARE(content.value("other").toInt(), 1);
        const auto rows = content.value("recent_emoji").toList();
        // Most recently used first on the wire (the standard's rule); the
        // stable object shape, never tuples.
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
        // It asks for the list instead of writing one built from nothing.
        QCOMPARE(client.reads, QStringList{ QStringLiteral("m.recent_emoji") });
        QTest::qWait(1800);
        QVERIFY(client.writes.isEmpty());
        // Once read, the earlier use is applied on top of the account's list.
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
        // PACK-REC-008: the write's result used to be ignored for recents.
        SettingsManager settings;
        settings.clearRecentEmoji();
        EmojiCatalog catalog(&settings);
        FakePrefsClient client;
        AccountPrefsController c;
        c.setRecentsRetryBaseMsForTest(40);   // retries at 40, then 80 ms
        c.setEmojiCatalog(&catalog);
        c.setClient(&client);
        Q_EMIT client.accountPrefRead(QStringLiteral("m.recent_emoji"), true,
            { { "recent_emoji", QVariantList{} } });
        catalog.recordUse(QStringLiteral("🎉"));
        QTRY_COMPARE_WITH_TIMEOUT(client.writes.size(), 1, 5000);
        QCOMPARE(c.recentsWriteAttempts(), 1);

        // Three failures in a row: two retries are scheduled, the third
        // failure schedules nothing (the cap), and it is never claimed synced.
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
        // Past the longest delay the cap could have produced (3 x 40 ms).
        QTest::qWait(400);
        QCOMPARE(client.writes.size(), 3);
        QVERIFY(c.recentsWriteFailed());

        // A later use starts a new burst, and its success clears the failure.
        catalog.recordUse(QStringLiteral("👍"));
        QTRY_COMPARE_WITH_TIMEOUT(client.writes.size(), 4, 5000);
        QCOMPARE(c.recentsWriteAttempts(), 1);
        Q_EMIT client.accountPrefWritten(QStringLiteral("m.recent_emoji"), true, QString());
        QVERIFY(!c.recentsWriteFailed());
        QVERIFY(!c.recentsRetryScheduled());
        settings.clearRecentEmoji();
    }

    // Review S2: the invite-policy switch must reflect the SERVER. A read that
    // fails is unknown ("offline"), never "absent" (an empty store before the
    // first sync used to read as allowed while the account blocked).
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

        // The server's answer: the account blocks, with a field we do not own.
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

    // Review S2: after a confirmed write, a read that was already in flight
    // (the old value) must not snap the switch back; the next toggle is
    // composed from the confirmed content.
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

        // The stale read lands: the switch stays, and the server is asked again.
        client.reads.clear();
        Q_EMIT client.accountPrefRead(QStringLiteral("m.invite_permission_config"), false, {});
        QVERIFY2(c.invitesBlocked(), "a stale read snapped the switch back");
        QCOMPARE(client.reads, QStringList{ QStringLiteral("m.invite_permission_config") });

        // The next toggle is built from the confirmed content.
        client.writes.clear();
        c.setInvitesBlocked(false);
        QCOMPARE(client.writes.size(), 1);
        QVERIFY(!client.writes.at(0).second.contains("default_action"));
        Q_EMIT client.accountPrefWritten(QStringLiteral("m.invite_permission_config"),
                                         true, QString());
        QVERIFY(!c.invitesBlocked());

        // An agreeing read releases the hold; a later change from elsewhere
        // is then adopted.
        Q_EMIT client.accountPrefRead(QStringLiteral("m.invite_permission_config"), true, {});
        Q_EMIT client.accountPrefRead(QStringLiteral("m.invite_permission_config"), true,
            { { "default_action", "block" } });
        QVERIFY(c.invitesBlocked());
    }
};

QTEST_MAIN(AccountPrefsTest)
#include "AccountPrefsTest.moc"
