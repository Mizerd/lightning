// Development-only screenshot-demo scenario controller contract.
//
// Drives a real AppController(MockBackend, screenshotDemo=true) + its
// ScreenshotDemoController (app.demo) and asserts that every required scenario
// activates deterministically to the right account / room / page / thread /
// theme / window size, that window presets resolve, that per-account selected
// rooms are restored on switch-back, that the panel toggles reach the mock, and
// that reset restores the deterministic default. Offscreen; no network.

#include "app/AppController.h"
#include "app/ScreenshotDemoController.h"
#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "gif/GifFavoritesModel.h"
#include "gif/GifSearchController.h"
#include "matrix/MockMatrixClient.h"
#include "models/EmojiCatalog.h"
#include "threads/ThreadController.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class ScreenshotDemoScenarioTest : public QObject
{
    Q_OBJECT

    ScreenshotDemoController *demo(AppController &app) const
    {
        return qobject_cast<ScreenshotDemoController *>(app.demoController());
    }

private Q_SLOTS:
    void initTestCase()
    {
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("screenshot-demo-scenario-test"));
    }

    void init()
    {
        QSettings s; s.clear(); s.sync();
    }

    void catalogueHasEveryRequiredScenario()
    {
        const QStringList ids = ScreenshotDemoController::scenarioIds();
        const QStringList required = {
            QStringLiteral("home-overview"), QStringLiteral("main-chat"),
            QStringLiteral("direct-message"), QStringLiteral("development"),
            QStringLiteral("media-gallery"), QStringLiteral("thread-view"),
            QStringLiteral("poll"), QStringLiteral("settings-themes"),
            QStringLiteral("account-switching"), QStringLiteral("security"),
            QStringLiteral("invite"), QStringLiteral("work-overview"),
            QStringLiteral("community-overview"), QStringLiteral("responsive-chat"),
            // v0.6.5 (Wave 2): menu/popup/dialog surface scenarios.
            QStringLiteral("menu-message"), QStringLiteral("menu-room"),
            // v0.6.5 (Storm round, C7): the floating find-in-room card.
            QStringLiteral("find-in-room"),
            QStringLiteral("quick-switcher"), QStringLiteral("quick-switcher-command"),
            QStringLiteral("emoji-picker"), QStringLiteral("gif-picker"),
            QStringLiteral("member-profile"), QStringLiteral("mention-popup"),
            QStringLiteral("trust-card"), QStringLiteral("new-conversation"),
            QStringLiteral("settings-search"), QStringLiteral("invite-people"),
            QStringLiteral("create-poll"),
        };
        for (const QString &r : required)
            QVERIFY2(ids.contains(r), qUtf8Printable("missing scenario: " + r));
        QCOMPARE(ids.size(), required.size());
    }

    void bootAppliesHomeOverviewByDefault()
    {
        AppController app(AppController::MockBackend, true);
        app.beginScreenshotDemo();
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        auto *d = demo(app);
        QVERIFY(d);
        // The launch handler auto-activates home-overview → Design Lounge.
        QTRY_COMPARE(app.currentRoomId(),
                     QStringLiteral("!design-lounge:lightning.example"));
        QCOMPARE(app.accounts()->activeUserId(),
                 QStringLiteral("@alex:lightning.example"));
    }

    void chatScenariosNavigateDeterministically()
    {
        AppController app(AppController::MockBackend, true);
        app.beginScreenshotDemo();
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        auto *d = demo(app);
        QVERIFY(d);

        struct Case { const char *id; const char *room; const char *account; int theme; int w; int h; };
        const Case cases[] = {
            { "main-chat", "!design-lounge:lightning.example", "@alex:lightning.example", 4, 1440, 900 },
            { "direct-message", "!dm-maya:lightning.example", "@alex:lightning.example", 4, 1280, 800 },
            { "development", "!dev:lightning.example", "@alex:lightning.example", 9, 1440, 900 },
            { "media-gallery", "!photography:lightning.example", "@alex:lightning.example", 9, 1440, 900 },
            { "poll", "!feedback:lightning.example", "@alex:lightning.example", 9, 1280, 800 },
            { "work-overview", "!aurora:workplace.example", "@taylor:workplace.example", 8, 1440, 900 },
            { "community-overview", "!general:community.example", "@nova:community.example", 9, 1440, 900 },
            { "responsive-chat", "!dm-maya:lightning.example", "@alex:lightning.example", 9, 760, 900 },
        };
        for (const Case &c : cases) {
            d->activateScenario(QString::fromLatin1(c.id));
            QTRY_COMPARE(app.accounts()->activeUserId(),
                         QString::fromLatin1(c.account));
            QTRY_COMPARE(app.currentRoomId(), QString::fromLatin1(c.room));
            QCOMPARE(int(app.settings()->theme()), c.theme);
            QCOMPARE(d->requestedWidth(), c.w);
            QCOMPARE(d->requestedHeight(), c.h);
            QCOMPARE(app.currentScreen(), AppController::MainScreen);
        }
    }

    void settingsScenariosOpenRealSettingsScreen()
    {
        AppController app(AppController::MockBackend, true);
        app.beginScreenshotDemo();
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        auto *d = demo(app);

        d->activateScenario(QStringLiteral("settings-themes"));
        QTRY_COMPARE(app.currentScreen(), AppController::SettingsScreen);

        // Back to a chat scenario returns to the main screen.
        d->activateScenario(QStringLiteral("main-chat"));
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
    }

    void accountSwitchingScenarioRequestsSwitcher()
    {
        AppController app(AppController::MockBackend, true);
        app.beginScreenshotDemo();
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        auto *d = demo(app);
        QSignalSpy spy(d, &ScreenshotDemoController::accountSwitcherRequested);
        d->activateScenario(QStringLiteral("account-switching"));
        QTRY_VERIFY(spy.count() >= 1);
        QCOMPARE(app.currentScreen(), AppController::MainScreen);
    }

    void threadScenarioOpensThreadPanel()
    {
        AppController app(AppController::MockBackend, true);
        app.beginScreenshotDemo();
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        auto *d = demo(app);
        d->activateScenario(QStringLiteral("thread-view"));
        QTRY_COMPARE(app.currentRoomId(), QStringLiteral("!dev:lightning.example"));
        QTRY_VERIFY(app.thread()->active());
        QVERIFY(!app.thread()->rootEventId().isEmpty());
    }

    // ── v0.6.5 (Wave 2): menu/popup/dialog surface scenarios ─────────────

    void menuScenariosNavigateAndEmitTheirContextMenuSignal()
    {
        AppController app(AppController::MockBackend, true);
        app.beginScreenshotDemo();
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        auto *d = demo(app);

        QSignalSpy messageSpy(d, &ScreenshotDemoController::demoOpenMessageContextMenu);
        d->activateScenario(QStringLiteral("menu-message"));
        QTRY_COMPARE(app.currentRoomId(), QStringLiteral("!design-lounge:lightning.example"));
        QCOMPARE(int(app.settings()->theme()), 9);
        QTRY_VERIFY(messageSpy.count() >= 1);

        QSignalSpy roomSpy(d, &ScreenshotDemoController::demoOpenRoomContextMenu);
        d->activateScenario(QStringLiteral("menu-room"));
        QTRY_COMPARE(app.currentRoomId(), QStringLiteral("!design-lounge:lightning.example"));
        QTRY_VERIFY(roomSpy.count() >= 1);
        // Only the room-menu signal fires for this scenario, never the
        // message-menu one (each row maps to exactly one popup).
        QCOMPARE(messageSpy.count(), 1);
    }

    void quickSwitcherScenariosEmitWithExpectedQuery()
    {
        AppController app(AppController::MockBackend, true);
        app.beginScreenshotDemo();
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        auto *d = demo(app);

        QSignalSpy spy(d, &ScreenshotDemoController::demoOpenQuickSwitcher);
        d->activateScenario(QStringLiteral("quick-switcher"));
        QTRY_VERIFY(spy.count() >= 1);
        QCOMPARE(spy.last().at(0).toString(), QStringLiteral("de"));
        QCOMPARE(int(app.settings()->theme()), 9);

        spy.clear();
        d->activateScenario(QStringLiteral("quick-switcher-command"));
        QTRY_VERIFY(spy.count() >= 1);
        QCOMPARE(spy.last().at(0).toString(), QStringLiteral(">theme"));
        QCOMPARE(int(app.settings()->theme()), 10);
    }

    void emojiPickerScenarioSeedsRecentsDeterministicallyAndEmits()
    {
        AppController app(AppController::MockBackend, true);
        app.beginScreenshotDemo();
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        auto *d = demo(app);

        QSignalSpy spy(d, &ScreenshotDemoController::demoOpenEmojiPicker);
        d->activateScenario(QStringLiteral("emoji-picker"));
        QTRY_VERIFY(spy.count() >= 1);
        QTRY_COMPARE(app.currentRoomId(), QStringLiteral("!design-lounge:lightning.example"));

        // Deterministic, most-recent-first order — the same 5 fictional
        // emoji every time, no real Matrix/provider data involved.
        const QStringList expected = {
            QStringLiteral("❤️"), QStringLiteral("\U0001F44D"),
            QStringLiteral("\U0001F602"), QStringLiteral("\U0001F525"),
            QStringLiteral("\U0001F389"),
        };
        QCOMPARE(app.settings()->recentEmoji(), expected);
        // Also assert through EmojiCatalog::recentEmoji() — the actual
        // property the picker's GridView and the message-menu quick-react
        // strip bind to. Seeding through SettingsManager directly leaves
        // this stale (no rebuild(), no recentEmojiChanged()) even though
        // the underlying settings value is identical; a live capture caught
        // exactly that gap, which is why this must go through
        // EmojiCatalog::recordUse(), never SettingsManager::
        // recordRecentEmoji() directly.
        QVERIFY(app.emojiCatalog());
        QCOMPARE(app.emojiCatalog()->recentEmoji(), expected);

        // Re-activating (panel re-click / reset) does not duplicate or
        // reorder entries — recordRecentEmoji is idempotent for a fixed
        // call sequence.
        d->activateScenario(QStringLiteral("emoji-picker"));
        QCOMPARE(app.settings()->recentEmoji(), expected);
        QCOMPARE(app.emojiCatalog()->recentEmoji(), expected);
    }

    void gifPickerScenarioSeedsOneFavoriteIdempotentlyAndEmits()
    {
        AppController app(AppController::MockBackend, true);
        app.beginScreenshotDemo();
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        auto *d = demo(app);
        QVERIFY(app.gif());
        QVERIFY(app.gif()->favorites());

        QSignalSpy spy(d, &ScreenshotDemoController::demoOpenGifPicker);
        d->activateScenario(QStringLiteral("gif-picker"));
        QTRY_VERIFY(spy.count() >= 1);
        QTRY_COMPARE(app.currentRoomId(), QStringLiteral("!weekend:lightning.example"));
        QCOMPARE(int(app.settings()->theme()), 8);

        auto *favorites = app.gif()->favorites();
        QVERIFY(favorites->isFavorite(QStringLiteral("giphy"),
                                      QStringLiteral("demo-favorite-loop")));
        const int count = favorites->count();
        QVERIFY(count >= 1);

        // Re-activation must not flip the seeded favorite back off (toggle()
        // is guarded by an isFavorite() check in seedDemoGifFavorite()).
        d->activateScenario(QStringLiteral("gif-picker"));
        QVERIFY(favorites->isFavorite(QStringLiteral("giphy"),
                                      QStringLiteral("demo-favorite-loop")));
        QCOMPARE(favorites->count(), count);

        // Only fictional, non-resolving URLs — no real provider CDN.
        const QVariantMap entry = favorites->get(0);
        for (const char *key : { "previewUrl", "stillUrl", "gifUrl" }) {
            const QString url = entry.value(QString::fromLatin1(key)).toString();
            QVERIFY(url.isEmpty() || url.contains(QStringLiteral(".example")));
        }
    }

    void memberProfileAndMentionPopupScenariosEmit()
    {
        AppController app(AppController::MockBackend, true);
        app.beginScreenshotDemo();
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        auto *d = demo(app);

        QSignalSpy profileSpy(d, &ScreenshotDemoController::demoOpenMemberProfile);
        d->activateScenario(QStringLiteral("member-profile"));
        QTRY_VERIFY(profileSpy.count() >= 1);

        QSignalSpy mentionSpy(d, &ScreenshotDemoController::demoOpenMentionPopup);
        d->activateScenario(QStringLiteral("mention-popup"));
        QTRY_VERIFY(mentionSpy.count() >= 1);
        QCOMPARE(mentionSpy.last().at(0).toString(), QStringLiteral("ma"));
    }

    void trustCardScenarioOpensSessionsSettingsAndEmits()
    {
        AppController app(AppController::MockBackend, true);
        app.beginScreenshotDemo();
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        auto *d = demo(app);

        QSignalSpy spy(d, &ScreenshotDemoController::demoOpenTrustCard);
        d->activateScenario(QStringLiteral("trust-card"));
        QTRY_COMPARE(app.currentScreen(), AppController::SettingsScreen);
        QTRY_VERIFY(spy.count() >= 1);

        // Back to a chat scenario returns to the main screen (same contract
        // as settingsScenariosOpenRealSettingsScreen() above).
        d->activateScenario(QStringLiteral("main-chat"));
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
    }

    void settingsSearchScenarioFocusesSearchWithExpectedQuery()
    {
        AppController app(AppController::MockBackend, true);
        app.beginScreenshotDemo();
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        auto *d = demo(app);

        QSignalSpy spy(d, &ScreenshotDemoController::demoFocusSettingsSearch);
        d->activateScenario(QStringLiteral("settings-search"));
        QTRY_COMPARE(app.currentScreen(), AppController::SettingsScreen);
        QTRY_VERIFY(spy.count() >= 1);
        QCOMPARE(spy.last().at(0).toString(), QStringLiteral("security"));
    }

    void dialogScenariosEmitTheirOwnSignalOnly()
    {
        AppController app(AppController::MockBackend, true);
        app.beginScreenshotDemo();
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        auto *d = demo(app);

        QSignalSpy newConv(d, &ScreenshotDemoController::demoOpenNewConversation);
        QSignalSpy invite(d, &ScreenshotDemoController::demoOpenInvitePeople);
        QSignalSpy poll(d, &ScreenshotDemoController::demoOpenCreatePoll);

        d->activateScenario(QStringLiteral("new-conversation"));
        QTRY_VERIFY(newConv.count() >= 1);
        QCOMPARE(invite.count(), 0);
        QCOMPARE(poll.count(), 0);

        d->activateScenario(QStringLiteral("invite-people"));
        QTRY_VERIFY(invite.count() >= 1);
        QCOMPARE(poll.count(), 0);

        d->activateScenario(QStringLiteral("create-poll"));
        QTRY_VERIFY(poll.count() >= 1);
    }

    void windowPresetsResolveAndRejectGarbage()
    {
        int w = 0, h = 0;
        QVERIFY(ScreenshotDemoController::sizeForPreset(QStringLiteral("1920x1080"), &w, &h));
        QCOMPARE(w, 1920); QCOMPARE(h, 1080);
        QVERIFY(ScreenshotDemoController::sizeForPreset(QStringLiteral("narrow"), &w, &h));
        QCOMPARE(w, 760); QCOMPARE(h, 900);
        QVERIFY(ScreenshotDemoController::sizeForPreset(QStringLiteral("wide"), &w, &h));
        QCOMPARE(w, 1720); QCOMPARE(h, 960);
        // Freeform WxH within bounds is accepted; absurd/unsafe is rejected.
        QVERIFY(ScreenshotDemoController::sizeForPreset(QStringLiteral("1360x850"), &w, &h));
        QVERIFY(!ScreenshotDemoController::sizeForPreset(QStringLiteral("50x50"), &w, &h));
        QVERIFY(!ScreenshotDemoController::sizeForPreset(QStringLiteral("99999x1"), &w, &h));
        QVERIFY(!ScreenshotDemoController::sizeForPreset(QStringLiteral("bogus"), &w, &h));
    }

    void windowSizeEmitsRequest()
    {
        AppController app(AppController::MockBackend, true);
        app.beginScreenshotDemo();
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        auto *d = demo(app);
        QSignalSpy spy(d, &ScreenshotDemoController::windowSizeRequested);
        d->setWindowSize(QStringLiteral("1600x1000"));
        QCOMPARE(d->requestedWidth(), 1600);
        QCOMPARE(d->requestedHeight(), 1000);
        QVERIFY(spy.count() >= 1);
        QCOMPARE(spy.last().at(0).toInt(), 1600);
        QCOMPARE(spy.last().at(1).toInt(), 1000);
    }

    void selectedRoomRestoredOnSwitchBack()
    {
        AppController app(AppController::MockBackend, true);
        app.beginScreenshotDemo();
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        auto *d = demo(app);

        // Go to Taylor (wait for the switch to fully settle — activeUserId is
        // set synchronously, but a second switch is dropped while one is in
        // flight, exactly as the real switcher guards it) and its default room.
        d->activateScenario(QStringLiteral("work-overview"));
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.currentRoomId(),
                     QStringLiteral("!aurora:workplace.example"));
        // Navigate to a specific room; this is remembered for Taylor.
        d->setRoom(QStringLiteral("!engineering:workplace.example"));
        QCOMPARE(app.currentRoomId(),
                 QStringLiteral("!engineering:workplace.example"));

        // Switch to Nova, then back to Taylor via the (real) switch path.
        d->setAccount(QStringLiteral("@nova:community.example"));
        QTRY_VERIFY(!app.accountSwitching());
        QCOMPARE(app.accounts()->activeUserId(),
                 QStringLiteral("@nova:community.example"));
        d->setAccount(QStringLiteral("@taylor:workplace.example"));
        QTRY_VERIFY(!app.accountSwitching());
        QCOMPARE(app.accounts()->activeUserId(),
                 QStringLiteral("@taylor:workplace.example"));
        // Taylor's last room is restored, not reset.
        QTRY_COMPARE(app.currentRoomId(),
                     QStringLiteral("!engineering:workplace.example"));
    }

    void panelTogglesReachTheMock()
    {
        AppController app(AppController::MockBackend, true);
        app.beginScreenshotDemo();
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        auto *d = demo(app);

        d->setTypingEnabled(false);
        QVERIFY(!d->typingEnabled());
        d->setUnreadBadgesEnabled(false);
        QVERIFY(!d->unreadBadgesEnabled());

        // Reset all restores the defaults and returns to home-overview.
        d->resetAllDemoState();
        QVERIFY(d->typingEnabled());
        QVERIFY(d->unreadBadgesEnabled());
        QTRY_COMPARE(app.currentRoomId(),
                     QStringLiteral("!design-lounge:lightning.example"));
    }

    void controlsVisibilityToggles()
    {
        AppController app(AppController::MockBackend, true);
        app.beginScreenshotDemo();
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        auto *d = demo(app);
        QVERIFY(d->controlsVisible());
        d->toggleControls();
        QVERIFY(!d->controlsVisible());
        d->setControlsVisible(true);
        QVERIFY(d->controlsVisible());
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_MAIN(ScreenshotDemoScenarioTest)
#include "ScreenshotDemoScenarioTest.moc"
