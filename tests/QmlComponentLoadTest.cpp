// Every component in this list must actually load.
//
// A load-time QML error (such as assigning `font.families`) is invisible to
// qmlformat, qmlcachegen and source-text contract scans, and makes the
// component and every parent unavailable.
//
// # Adding a component
//
// Put its name in kComponents. If it cannot load standalone (it needs a
// required property, a parent, or is a delegate), list it in kNotLoadable
// with the reason rather than leaving it out silently.
//
// # This proves loading, not correctness
//
// A failure inside a `Loader`'s `sourceComponent` still leaves the root
// loading fine.

#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QDir>
#include <QFile>
#include <QtTest/QtTest>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "auth/AuthManager.h"
#include "models/RoomListModel.h"
#include "models/SpaceChannelModel.h"

#include <memory>

namespace {

// Components that must load with nothing but `app` in context.
constexpr const char *kComponents[] = {
    "StickerPackEditor",     // pack CRUD dialog
    "QrLoginDialog",         // MSC4108 sign-in-another-device
    "PolicyListDialog",      // Mjolnir-style moderation lists
    "AddWidgetDialog",       // widget kind picker
    "MemberProfilePopover",  // carries the policy-list notice and its Connections
    "CallPipWindow",         // the floating call window
    // The in-call device menu (a Slider, Labels and a Layout inside a Menu)
    // and its Settings sibling.
    "CallDeviceMenu",
    "CallDeviceSettings",
    // The microphone level bar both of those draw.
    "AudioLevelBar",
    // Settings → Labs: the microphone noise suppression selector (#20).
    "NoiseSuppressionSelector",
    // Screen share sound: the live application list (IconImage from
    // QtQuick.Controls.impl), the call bar's dialog around it, and both
    // share surfaces that host the choice.
    "ShareAudioAppList",
    "ShareAudioAppsDialog",
    "CallShareOptionsMenu",
    "ScreenSharePicker",
    "MediaBrowser",          // room media/files/links over all history
    "ContextView",           // read-only message context (app.eventContext)
    "ForwardSelectionDialog",
    "EmojiCompletionPopup",
    // Long-standing surfaces with the same exposure. Cheap to cover, and
    // each one is a file somebody will edit without running its own suite.
    "CallStage",
    "SettingsScreen",
    "RoomInfoPanel",
    "GifPicker",
    "StickerPicker",
    "EmojiPicker",
    "QuickSwitcher",
    "MessageSearchDialog",
    "ThemeEditorDialog",
    "IncomingCallPrompt",
    "EncryptionBrokenPrompt",  // B011: the undecryptable-device card
    // The one-time "index all messages now?" card; reads app.messageSearch.
    "IndexAllPrompt",
    "BackgroundSyncPrompt",
    "CallHeaderBar",
    "ActivityCenterPanel",
    "JumpToDateDialog",
    "WidgetOpenSheet",
    "HomePane",
    // Collapsed-embed summary row; reads only AppTheme and Icon.
    "CollapsedEmbedRow",
    // A link's own video or large image; reads app.linkPreviews and
    // app.mediaBridge, and every preview field defaults to empty.
    "LinkMediaCard",
    // Space Home lobby; every input has an empty default and it reads no `app`.
    "SpaceLobby",
    // Space kick/ban confirmation; renders app.spaceModeration.
    "SpaceMemberActionDialog",
    // Close / server-admin delete confirmation; renders app.roomClosure.
    "RoomCloseDialog",
    // Settings -> Account -> Change password; renders app.passwordChange.
    "ChangePasswordSection",
    // Room and Space topics; reads app.linkPreviews. A TextEdit, where a Text
    // property such as lineHeight is a load-time error.
    "TopicText",
    // The floating voice/audio mini-player; renders app.voicePlayback and
    // app.settings.voiceMiniPlayerCorner, and shows nothing while idle.
    "VoiceMiniPlayer",
    // The downloads card (app.downloads.items) and the native chooser
    // wrapper (app.files); both bind app objects at load.
    "DownloadsCard",
    "NativeFileDialog",
    // The "..." menu beside the room-list search: Activity / A-Z. Reads and
    // writes app.settings.roomListSort.
    "RoomListSortMenu",
    // Recovery key entry shared by Settings and the first-run prompt; reads
    // app.requestRecoverFromBackup and app.sessionTrustState.
    "RecoveryKeyEntry",
    // Cross-signing setup/reset card; reads app.cryptoHealth and app.backup.
    "CrossSigningSetupCard",
    // The first-run "verify this session" corner card, which hosts it inline.
    "VerifySessionPrompt",
    // Chat backgrounds and gradient surfaces (2026-10-06). The backdrop
    // layer, the gradient-aware surface, the room/Space/default editor, the
    // Settings section and the theme editor's gradient block; all read
    // app.backdrops / app.customTheme and render nothing without a room.
    "ChatBackdrop",
    "ThemedSurface",
    "ChatBackgroundEditor",
    "ChatBackgroundSettings",
    "GradientEditor",
    // The "Chat background…" dialog the room header, the room menu and Home
    // open; hosts ChatBackgroundEditor behind a Loader.
    "ChatBackgroundDialog",
};

// Deliberately NOT loaded standalone, each with the reason. Kept here rather
// than omitted so that "not covered" is a decision on the record.
struct Excluded { const char *name; const char *why; };
constexpr Excluded kNotLoadable[] = {
    { "MessageDelegate", "a timeline delegate: required properties come from "
                         "the model, and TimelinePaneQmlTest drives it for real" },
    { "MediaBrowserTile", "a GridView delegate with required properties" },
    { "MediaBrowserRow", "a ListView delegate with required properties" },
    { "RoomDelegate", "a room-list delegate with required properties" },
    { "CallParticipantTile", "a call-grid delegate; CallUiContractTest covers it" },
    { "Main", "the application window; StartupSessionTest loads it" },
};

} // namespace

class QmlComponentLoadTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void everyListedComponentLoads_data()
    {
        QTest::addColumn<QString>("component");
        for (const char *name : kComponents)
            QTest::newRow(name) << QString::fromUtf8(name);
    }

    void everyListedComponentLoads()
    {
        QFETCH(QString, component);

        // A logged-in mock controller: most of these read `app.<something>`
        // in a creation-time binding, and a null model there is its own
        // class of load failure.
        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(3000));

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        // A binding loop is a load-time fact: the component loads, but Qt
        // abandons the binding and the property keeps whatever the aborted
        // evaluation left behind.
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const QQmlError &e : errors)
                        warnings << e.toString();
                });
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"), component);
        if (createdSpy.isEmpty())
            QVERIFY2(createdSpy.wait(8000),
                     qPrintable(component + QStringLiteral(
                         " never finished loading")));

        // objectCreated carries a NULL object when the component failed, so
        // THIS is the assertion — the spy having fired is not enough.
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY2(root != nullptr,
                 qPrintable(component + QStringLiteral(
                     ".qml failed to load — the qWarning above names the "
                     "property or type that does not exist")));

        for (const QString &warning : warnings) {
            QVERIFY2(!warning.contains(QStringLiteral("Binding loop")),
                     qPrintable(component + QStringLiteral(": ") + warning));
        }
    }

    // HomePane must resolve its greeting without a binding loop. Unlike the
    // data-driven case above, this needs a real active account id at creation:
    // `displayName` falls back to the localpart and so reads `activeUserId`.
    // Asserts both that no loop was reported and that the greeting resolved.
    void theHomePaneResolvesItsGreetingWithoutABindingLoop()
    {
        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(3000));

        // A record with no display name, so the greeting takes the localpart
        // fallback. It must be upserted first: `setActiveUser` refuses an id
        // with no saved record. The secret store is detached first so
        // `saveSession` cannot write a token to the real keyring.
        const QString uid = QStringLiteral("@alice:mock.local");
        controller.settings()->setSecretStore(nullptr);
        controller.settings()->saveSession(QStringLiteral("https://mock.local"),
                                           uid, QStringLiteral("MOCKDEV"),
                                           QString());
        // Removed again on every exit path: this suite's QSettings file
        // outlives the process and would change the data-driven case's next
        // run.
        const auto forgetAccount = qScopeGuard([&controller, &uid] {
            controller.accounts()->removeAccount(uid);
        });
        QCOMPARE(controller.accounts()->activeUserId(), uid);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const QQmlError &e : errors)
                        warnings << e.toString();
                });
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("HomePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(8000));
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);

        // Read first, then scan: the loop fires on lazy first-read
        // evaluation.
        QCOMPARE(root->property("activeUserId").toString(), uid);
        QCOMPARE(root->property("displayName").toString(),
                 QStringLiteral("alice"));
        QCoreApplication::processEvents();

        for (const QString &warning : warnings) {
            QVERIFY2(!warning.contains(QStringLiteral("Binding loop")),
                     qPrintable(warning));
        }
    }

    // "Your profile in this room" and the chat background editor are for every
    // member: anyone may set their own room profile, their own background
    // ("Only me") and hide what others set. Both sections used to sit INSIDE
    // roomAdminBlock, which is visible only to people who can change the join
    // rule, alias, history visibility or guest access, so an ordinary member
    // never saw either (found in the 2026-10-06 GUI check with a PL 0
    // account). Neither may have that block as an ancestor. Fails on the old
    // layout, where both did.
    void roomInfoMemberSectionsAreNotInsideTheAdminBlock()
    {
        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(3000));

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("RoomInfoPanel"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(8000));
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);

        auto *adminBlock =
            root->findChild<QQuickItem *>(QStringLiteral("roomAdminBlock"));
        QVERIFY2(adminBlock, "roomAdminBlock is gone: re-point this test at "
                             "whatever now gates the admin-only controls");

        int checked = 0;
        for (const QString &name : { QStringLiteral("roomProfileSection"),
                                     QStringLiteral("roomChatBackgroundEditor") }) {
            auto *section = root->findChild<QQuickItem *>(name);
            QVERIFY2(section, qPrintable(name + QStringLiteral(" not found")));
            for (QQuickItem *p = section->parentItem(); p; p = p->parentItem()) {
                QVERIFY2(p != adminBlock,
                         qPrintable(name + QStringLiteral(
                             " is inside roomAdminBlock, so a member who cannot "
                             "change the join rule never sees it")));
            }
            ++checked;
        }
        QCOMPARE(checked, 2);
    }

    // The room-list "..." menu: it shows the persisted mode, writes it, and
    // the change reaches BOTH layouts' models through the controller.
    void theRoomListSortMenuWritesTheSettingAndBothModelsFollow()
    {
        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(3000));
        // The suite's QSettings file outlives the process.
        controller.settings()->setRoomListSort(0);
        const auto restore = qScopeGuard(
            [&controller] { controller.settings()->setRoomListSort(0); });

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 400
    height: 300
    property alias menu: m
    RoomListSortMenu { id: m }
}
)"), QUrl(QStringLiteral("qrc:/roomlistsortmenutest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY2(owner != nullptr, "RoomListSortMenu must instantiate");
        auto *menu = owner->property("menu").value<QObject *>();
        QVERIFY(menu != nullptr);
        auto *activity = menu->findChild<QObject *>(
            QStringLiteral("roomSortActivityItem"));
        auto *byName = menu->findChild<QObject *>(
            QStringLiteral("roomSortNameItem"));
        QVERIFY2(activity && byName, "the menu lost one of its two sorts");

        // Activity is the default and is the one marked.
        QVERIFY(activity->property("radioSelected").toBool());
        QVERIFY(!byName->property("radioSelected").toBool());
        QCOMPARE(controller.roomList()->sortMode(), 0);

        QVERIFY(QMetaObject::invokeMethod(byName, "triggered"));
        QCOMPARE(controller.settings()->roomListSort(), 1);
        QVERIFY(byName->property("radioSelected").toBool());
        QVERIFY(!activity->property("radioSelected").toBool());
        QCOMPARE(controller.roomList()->sortMode(), 1);
        QCOMPARE(controller.spaceChannels()->sortMode(), 1);

        QVERIFY(QMetaObject::invokeMethod(activity, "triggered"));
        QCOMPARE(controller.settings()->roomListSort(), 0);
        QCOMPARE(controller.roomList()->sortMode(), 0);
        QCOMPARE(controller.spaceChannels()->sortMode(), 0);
    }

    // Settings' microphone test (Discord's "Let's check") and speaker test:
    // their controls exist, are disabled with a reason while the tester
    // cannot run, become usable when it can, and are refused again during a
    // call. Fails on a tree without the test (no such controls).
    void theSoundSettingsMicrophoneTestSaysWhyItCannotRun()
    {
        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(3000));
        QVERIFY(controller.audioTester() != nullptr);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const QQmlError &e : errors)
                        warnings << e.toString();
                });
        QQmlComponent component(&engine);
        // Not activated: no device is enumerated or opened here.
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 500
    height: 900
    property alias settings: s
    property alias menu: m
    CallDeviceSettings { id: s; width: 480 }
    CallDeviceMenu { id: m; kind: "microphone" }
}
)"), QUrl(QStringLiteral("qrc:/sounddevicetest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY2(owner != nullptr, "CallDeviceSettings must instantiate");
        auto *settings = owner->property("settings").value<QObject *>();
        QVERIFY(settings != nullptr);

        auto *button =
            settings->findChild<QObject *>(QStringLiteral("microphoneTestButton"));
        auto *tone =
            settings->findChild<QObject *>(QStringLiteral("outputTestButton"));
        auto *notice =
            settings->findChild<QObject *>(QStringLiteral("microphoneTestNotice"));
        auto *bar =
            settings->findChild<QObject *>(QStringLiteral("microphoneTestLevel"));
        QVERIFY2(button && tone && notice && bar,
                 "the Sound & video page lost its microphone or speaker test");

        // Tests never find the runtime: disabled, with the reason on screen.
        QVERIFY(!controller.audioTester()->available());
        QVERIFY(!button->property("enabled").toBool());
        QVERIFY(!tone->property("enabled").toBool());
        QVERIFY(!notice->property("text").toString().isEmpty());

#ifdef HAVE_LIGHTNING_WEBRTC
        // Usable once the runtime is known (no device is opened by this).
        controller.audioTester()->setRuntimeAvailable(true);
        QVERIFY(button->property("enabled").toBool());
        QVERIFY(tone->property("enabled").toBool());
        QVERIFY2(notice->property("text").toString().isEmpty(),
                 qPrintable(notice->property("text").toString()));

        // And refused again, with a reason, during a call.
        controller.audioTester()->setCallActive(true);
        QVERIFY(!button->property("enabled").toBool());
        QVERIFY(!tone->property("enabled").toBool());
        QVERIFY(!notice->property("text").toString().isEmpty());
        controller.audioTester()->setCallActive(false);
        controller.audioTester()->setRuntimeAvailable(false);
#endif

        // The in-call menu carries the live level row, shown only during a
        // group call (the 1:1 lane has no meter).
        auto *menu = owner->property("menu").value<QObject *>();
        QVERIFY(menu != nullptr);
        QVERIFY(menu->findChild<QObject *>(QStringLiteral("callMenuMicLevelRow")));
        QVERIFY(menu->findChild<QObject *>(QStringLiteral("callMenuMicLevelBar")));
        QVERIFY(!menu->property("showsLevel").toBool());

        // Nothing this feature reads may be undefined at load.
        for (const QString &warning : warnings) {
            const bool ours = warning.contains(QStringLiteral("audioTester"))
                || warning.contains(QStringLiteral("AudioLevelBar"))
                || warning.contains(QStringLiteral("microphoneLevel"))
                || warning.contains(QStringLiteral("microphoneTest"));
            QVERIFY2(!ours && !warning.contains(QStringLiteral("Binding loop")),
                     qPrintable(warning));
        }
    }

    // ContextController::open is not Q_INVOKABLE: a QML call to it is a
    // TypeError at click time that no load test sees. Navigation goes through
    // app.pagination.jumpToEvent.
    void qmlNeverCallsTheNonInvokableContextOpen()
    {
        int scanned = 0;
        const QDir dir(QStringLiteral(QML_DIR));
        for (const QString &name : dir.entryList({ QStringLiteral("*.qml") })) {
            QFile file(dir.filePath(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            ++scanned;
            QVERIFY2(!QString::fromUtf8(file.readAll())
                          .contains(QStringLiteral("eventContext.open(")),
                     qPrintable(name));
        }
        QVERIFY(scanned > 50);
    }

    // `enabled` propagates to children, so `enabled: false` on a delegate
    // root also disables every control inside it. A text scan, because these
    // delegates are only built when a model supplies rows; the `found` guard
    // stops it from sweeping nothing after a rename.
    void noInteractiveDelegateDisablesItself()
    {
        static constexpr const char *kFiles[] = {
            "PolicyListDialog.qml", "StickerPackEditor.qml",
            "ForwardSelectionDialog.qml", "MediaBrowserRow.qml",
            "MediaBrowserTile.qml", "EmojiCompletionPopup.qml",
        };
        int scanned = 0;
        QStringList offenders;
        for (const char *name : kFiles) {
            QFile file(QStringLiteral(QML_DIR "/") + QString::fromUtf8(name));
            if (!file.open(QIODevice::ReadOnly))
                continue;
            // Comments are stripped first, so an explanation of the rule does
            // not trip the scan.
            QString src;
            const QStringList lines =
                QString::fromUtf8(file.readAll()).split(u'\n');
            for (const QString &line : lines) {
                const QString trimmed = line.trimmed();
                if (trimmed.startsWith(QStringLiteral("//")))
                    continue;
                src += line;
                src += u'\n';
            }
            ++scanned;
            // Every delegate block, from `delegate:` to end of file: a coarse
            // bound, enough to see whether `enabled: false` and an interactive
            // control share one delegate.
            int at = src.indexOf(QStringLiteral("delegate:"));
            while (at >= 0) {
                const int next =
                    src.indexOf(QStringLiteral("delegate:"), at + 1);
                const QString block =
                    src.mid(at, (next < 0 ? src.size() : next) - at);
                const bool disables =
                    block.contains(QStringLiteral("enabled: false"));
                // A CheckBox alone is the legitimate case — an indicator
                // whose row owns the click, with no interactive descendant.
                const bool hasControl =
                    block.contains(QStringLiteral("AppButton"))
                    || block.contains(QStringLiteral("AppTextField"))
                    || block.contains(QStringLiteral("IconButton"));
                if (disables && hasControl) {
                    offenders << QString::fromUtf8(name);
                }
                at = next;
            }
        }
        QVERIFY2(scanned >= 4,
                 "the scan found almost no files — a rename has made it "
                 "sweep nothing, which passes for the wrong reason");
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "a delegate sets `enabled: false` while containing a "
                     "control: enabled PROPAGATES, so that control is dead. "
                     "Offenders: %1").arg(offenders.join(u", "))));
    }

    // The exclusions are a list of DECISIONS, and a decision with no reason
    // is an omission wearing a decision's clothes.
    void everyExclusionCarriesItsReason()
    {
        for (const Excluded &e : kNotLoadable) {
            QVERIFY2(e.why != nullptr && qstrlen(e.why) > 20,
                     qPrintable(QStringLiteral(
                         "%1 is excluded without a real reason")
                         .arg(QString::fromUtf8(e.name))));
        }
    }
};

QTEST_MAIN(QmlComponentLoadTest)
#include "QmlComponentLoadTest.moc"
