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

    void privateModeLoadsOnlyInPrivateRooms()
    {
        QVERIFY(!collapsesMedia(QStringLiteral("on"), false));
        QVERIFY(!collapsesMedia(QStringLiteral("on"), true));
        QVERIFY(collapsesMedia(QStringLiteral("private"), false));
        QVERIFY(!collapsesMedia(QStringLiteral("private"), true));
        QVERIFY(collapsesMedia(QStringLiteral("off"), true));
        QVERIFY(collapsesMedia(QStringLiteral("anything else"), true));
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
        const auto list = parseRecentEmoji({ { "recent_emoji", QVariantList{
            pair("👍", 5), QVariantList{ "x" }, pair("", 2), pair("😀", 0),
            QVariant(7), pair("🎉", 3), pair("👍", 9) } } });
        QCOMPARE(list.size(), 2);
        QCOMPARE(list.at(0), EmojiCount("👍", 5)); // first wins, order kept
        QCOMPARE(list.at(1), EmojiCount("🎉", 3));
        QVERIFY(parseRecentEmoji({}).isEmpty());
    }

    void aUseBumpsTheCountAndKeepsMostUsedFirst()
    {
        QList<EmojiCount> list{ { "👍", 2 }, { "😀", 2 } };
        list = bumpRecentEmoji(list, QStringLiteral("😀"));
        QCOMPARE(list.at(0), EmojiCount("😀", 3));
        list = bumpRecentEmoji(list, QStringLiteral("🎉"));
        QCOMPARE(list.last(), EmojiCount("🎉", 1));
        const auto composed = composeRecentEmoji({ { "other", 1 } }, list);
        QCOMPARE(composed.value("other").toInt(), 1);
        QCOMPARE(composed.value("recent_emoji").toList().size(), 3);
        // Element's exact shape: an array of [emoji, count] arrays.
        QCOMPARE(composed.value("recent_emoji").toList().at(0).toList().at(0).toString(),
                 QStringLiteral("😀"));
    }

    void mergeKeepsLocalFirstAndAddsTheAccountsTheRest()
    {
        const auto merged = mergeRecentEmoji(
            { "🎉", "👍" }, { { "👍", 9 }, { "😀", 4 }, { "🙏", 1 } }, 100);
        QCOMPARE(merged, (QStringList{ "🎉", "👍", "😀", "🙏" }));
        QCOMPARE(mergeRecentEmoji({ "a" }, { { "b", 1 }, { "c", 1 } }, 2),
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
        QVERIFY(!c.collapsesMedia(false));

        Q_EMIT client.accountPrefRead(QStringLiteral("m.media_preview_config"), true,
                                      { { "media_previews", "private" },
                                        { "invite_avatars", "off" } });
        QCOMPARE(c.mediaPreviews(), QStringLiteral("private"));
        QVERIFY(!c.inviteAvatars());
        QVERIFY(c.mediaPreviewKnown());
        QVERIFY(c.collapsesMedia(false));
        QVERIFY(!c.collapsesMedia(true));

        // A change from another device re-reads; the reply is adopted.
        client.reads.clear();
        Q_EMIT client.accountPrefChanged(QStringLiteral("m.media_preview_config"));
        QCOMPARE(client.reads, QStringList{ QStringLiteral("m.media_preview_config") });
        Q_EMIT client.accountPrefRead(QStringLiteral("m.media_preview_config"), true,
                                      { { "media_previews", "newer-mode" } });
        QCOMPARE(c.mediaPreviews(), QStringLiteral("off"));
        QVERIFY(c.collapsesMedia(true));

        // The data being deleted elsewhere returns the defaults.
        Q_EMIT client.accountPrefRead(QStringLiteral("m.media_preview_config"), false, {});
        QCOMPARE(c.mediaPreviews(), QStringLiteral("on"));
        QVERIFY(c.inviteAvatars());
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
            hasNew = hasNew || r.toList().at(0).toString() == QStringLiteral("🎉");
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
        QCOMPARE(rows.at(0).toList().at(0).toString(), QStringLiteral("👍"));
        QCOMPARE(rows.at(0).toList().at(1).toInt(), 9);
        QCOMPARE(rows.at(1).toList().at(0).toString(), QStringLiteral("😀"));
        QCOMPARE(rows.at(1).toList().at(1).toInt(), 6);
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
        QCOMPARE(rows.at(0).toList().at(0).toString(), QStringLiteral("👍"));
        settings.clearRecentEmoji();
    }
};

QTEST_MAIN(AccountPrefsTest)
#include "AccountPrefsTest.moc"
