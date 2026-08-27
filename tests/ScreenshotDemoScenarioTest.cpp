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
#include "gif/GifResultModel.h"
#include "gif/GifSearchController.h"
#include "matrix/MockMatrixClient.h"
#include "models/EmojiCatalog.h"
#include "threads/ThreadController.h"

#include <QFile>
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

    // THE CHANNELS SCENARIOS MUST ACTUALLY SWITCH THE LAYOUT.
    //
    // Naming a rail scope proves nothing on its own: with the layout left at
    // Classic there is no rail scope to honour, and all three would
    // photograph the same conversation list under three different names —
    // which is the failure this catalogue exists to prevent.
    void theChannelsScenariosSelectTheChannelsLayoutAndDistinctViews()
    {
        QFile file(QStringLiteral(
            SOURCE_DIR "/src/app/ScreenshotDemoController.cpp"));
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 "the demo controller is not where this test looks for it");
        const QString source = QString::fromUtf8(file.readAll());

        // The layout is applied at all, and BEFORE the room is opened —
        // Channels resolves its view from the scope, so a later write would
        // photograph one frame of the wrong column.
        const int applyAt = source.indexOf(
            QStringLiteral("setRoomNavigationLayout(s.navLayout)"));
        QVERIFY2(applyAt > 0, "the scenario's navigation layout is never "
                              "applied, so a Channels scenario renders "
                              "Classic");
        const int openAt = source.indexOf(QStringLiteral("m_app->openRoom("));
        QVERIFY2(openAt > applyAt,
                 "the layout is applied after the room is opened");
        QVERIFY2(source.contains(
                     QStringLiteral("setScopeSpaceId(s.channelsScope)")),
                 "the scenario's Channels view is never selected");

        // And the three views are genuinely different scopes.
        for (const char *scope : { "\"@home\"", "\"@people\"",
                                   "!space-studio" }) {
            QVERIFY2(source.contains(QLatin1String(scope)),
                     qPrintable(QStringLiteral(
                         "no Channels scenario selects the %1 view")
                                    .arg(QLatin1String(scope))));
        }
    }

    // THE DEMO CALL MUST NOT FOLLOW THE USER THROUGH THE CATALOGUE.
    //
    // It is process-local state, so a scenario that does not ask for a call
    // has to END one — otherwise every screenshot taken after `call-grid`
    // carries a call panel over the top of whatever it was meant to show.
    void aScenarioWithoutACallEndsOne()
    {
        QFile file(QStringLiteral(
            SOURCE_DIR "/src/app/ScreenshotDemoController.cpp"));
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString source = QString::fromUtf8(file.readAll());
        QVERIFY2(source.contains(QStringLiteral("endDemoCall()")),
                 "no scenario ever ends a staged call, so one leaks into "
                 "every screenshot taken after it");
        const int start = source.indexOf(QStringLiteral("startDemoCall("));
        const int end = source.indexOf(QStringLiteral("endDemoCall()"));
        QVERIFY2(end > 0 && start > 0 && end < start,
                 "the empty-call branch does not come first, so a scenario "
                 "with no call may not clear one");
        // And it must be staged only in a build that has the seam at all.
        QFile controller(QStringLiteral(
            SOURCE_DIR "/src/calls/SfuCallController.h"));
        QVERIFY(controller.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString header = QString::fromUtf8(controller.readAll());
        QVERIFY2(header.contains(
                     QStringLiteral("#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO")),
                 "the demo call is not compiled out of release builds");
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
            // 0.8.0: the Channels navigation layout is THREE views, and one
            // screenshot of it would show a third of the feature. Classic is
            // stated explicitly beside them so a release pair can be shot
            // without depending on what the demo profile was left in.
            QStringLiteral("channels-home"), QStringLiteral("channels-space"),
            QStringLiteral("channels-people"), QStringLiteral("classic-home"),
            QStringLiteral("call-grid"), QStringLiteral("call-screen-share"),
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
        // v0.6.7: Storm (11) is the demo default — the 0.6.5 brand theme — so
        // a release gallery is coherent. Only the two scenarios that exist to
        // show a DIFFERENT theme keep their own (settings-themes 9,
        // quick-switcher-command 10).
        const Case cases[] = {
            { "main-chat", "!design-lounge:lightning.example", "@alex:lightning.example", 11, 1440, 900 },
            { "direct-message", "!dm-maya:lightning.example", "@alex:lightning.example", 11, 1280, 800 },
            { "development", "!dev:lightning.example", "@alex:lightning.example", 11, 1440, 900 },
            { "media-gallery", "!photography:lightning.example", "@alex:lightning.example", 11, 1440, 900 },
            { "poll", "!feedback:lightning.example", "@alex:lightning.example", 11, 1280, 800 },
            { "work-overview", "!aurora:workplace.example", "@taylor:workplace.example", 11, 1440, 900 },
            { "community-overview", "!general:community.example", "@nova:community.example", 11, 1440, 900 },
            { "responsive-chat", "!dm-maya:lightning.example", "@alex:lightning.example", 11, 760, 900 },
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
        QCOMPARE(int(app.settings()->theme()), 11);
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
        QCOMPARE(int(app.settings()->theme()), 11);

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

    // v0.6.7: the picker used to be the one surface the demo could not
    // photograph — no network, no key, and a mock transport reporting
    // available() == false meant it could only ever render "GIFs are
    // unavailable on this backend". The seed is now a browsable catalogue of
    // BUNDLED animated fixtures plus a real locally-saved GIF for the Saved
    // tab, so every tile renders a real moving picture.
    void gifPickerScenarioSeedsABrowsableLocalCatalogue()
    {
        AppController app(AppController::MockBackend, true);
        app.beginScreenshotDemo();
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        auto *d = demo(app);
        QVERIFY(app.gif());

        QSignalSpy spy(d, &ScreenshotDemoController::demoOpenGifPicker);
        d->activateScenario(QStringLiteral("gif-picker"));
        QTRY_VERIFY(spy.count() >= 1);
        QTRY_COMPARE(app.currentRoomId(), QStringLiteral("!weekend:lightning.example"));
        QCOMPARE(int(app.settings()->theme()), 11);

        // The picker must report itself usable, or its overlay covers the grid.
        QVERIFY(app.gif()->demoCatalogueActive());
        QVERIFY(app.gif()->available());
        QVERIFY(app.gif()->providerConfigured(QStringLiteral("giphy")));
        QVERIFY(app.gif()->providerConfigured(QStringLiteral("klipy")));
        QCOMPARE(app.gif()->state(), int(GifSearchController::Ready));

        auto *results = app.gif()->results();
        QVERIFY(results != nullptr);
        const int count = results->count();
        QVERIFY2(count >= 12, "catalogue must overfill one screenful so the "
                              "bottom grid row photographs too");

        // Every tile points at a BUNDLED resource — never a provider CDN, and
        // never the fictional *.example host whose broken thumbnail this
        // replaces. Both providers appear so the tab strip and the per-tile
        // source tags photograph with real variety.
        bool sawGiphy = false, sawKlipy = false;
        for (int i = 0; i < count; ++i) {
            const QVariantMap row = results->get(i);
            for (const char *key : { "previewUrl", "stillUrl", "gifUrl" }) {
                const QString url = row.value(QString::fromLatin1(key)).toString();
                QVERIFY2(url.startsWith(QStringLiteral("qrc:/")),
                         qPrintable(QStringLiteral("non-bundled url: %1").arg(url)));
                QVERIFY(!url.contains(QStringLiteral(".example")));
            }
            const QString provider = row.value(QStringLiteral("provider")).toString();
            sawGiphy = sawGiphy || provider == QStringLiteral("giphy");
            sawKlipy = sawKlipy || provider == QStringLiteral("klipy");
            // A real byte size, so the tile's size badge is not blank.
            QVERIFY(row.value(QStringLiteral("gifBytes")).toLongLong() > 0);
        }
        QVERIFY(sawGiphy);
        QVERIFY(sawKlipy);

        // Re-activating a scenario must be idempotent — the panel re-clicks it
        // and resetScenario runs it again.
        d->activateScenario(QStringLiteral("gif-picker"));
        QCOMPARE(results->count(), count);
        QVERIFY(app.gif()->demoCatalogueActive());

        // Closing the picker calls reset(); the grid must not be left empty for
        // the next open.
        app.gif()->reset();
        QCOMPARE(results->count(), count);
        QCOMPARE(app.gif()->state(), int(GifSearchController::Ready));
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
