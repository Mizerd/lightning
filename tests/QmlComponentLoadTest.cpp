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
#include <QQuickWindow>
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
#include "spaces/RailLayoutStore.h"

#include <memory>

namespace {

constexpr const char *kComponents[] = {
    "StickerPackEditor",
    "QrLoginDialog",
    "AboutToolsSection",
    "DeveloperToolsSection",
    "UserClientsInfo",
    "LogExportCard",
    "UpdatesSettingsSection",
    "PolicyListDialog",
    "AddWidgetDialog",
    "MemberProfilePopover",
    "CallPipWindow",
    "CallDeviceMenu",
    "CallDeviceSettings",
    "CallQualitySettings",
    "NotificationRulesSettings",
    "AudioLevelBar",
    "NoiseSuppressionSelector",
    "GameActivitySettings",
    "MusicActivitySettings",
    "ShareAudioAppList",
    "ShareAudioAppsDialog",
    "CallShareOptionsMenu",
    "SoftwareVideoNotice",
    "ScreenSharePicker",
    "MediaBrowser",
    "ContextView",
    "ForwardSelectionDialog",
    "EmojiCompletionPopup",
    "StreamInfoOverlay",
    "CallStage",
    "SettingsScreen",
    "RoomInfoPanel",
    "GifPicker",
    "StickerPicker",
    "PackChipStrip",
    "EmojiPicker",
    "QuickSwitcher",
    "MessageSearchDialog",
    "ThemeEditorDialog",
    "IncomingCallPrompt",
    "EncryptionBrokenPrompt",
    "IndexAllPrompt",
    "BackgroundSyncPrompt",
    "CallHeaderBar",
    "CallFormatMenu",
    "LegacyCallVideo",
    "ActivityCenterPanel",
    "JumpToDateDialog",
    "WidgetOpenSheet",
    "HomePane",
    "CollapsedEmbedRow",
    "LinkMediaCard",
    "SpaceLobby",
    "SpaceMemberActionDialog",
    "RoomCloseDialog",
    "ChangePasswordSection",
    "PhotoMetadataSetting",
    "StickerPacksSettings",
    "RoomKeyExportSection",
    "TopicText",
    "VoiceMiniPlayer",
    "VoicePlayerBar",
    "VoiceSeekBar",
    "VoiceSpeedChip",
    "DownloadsCard",
    "PaneDownloadsCard",
    "NativeFileDialog",
    "FileChooserDialog",
    "RoomListSortMenu",
    "RecoveryKeyEntry",
    "CrossSigningSetupCard",
    "VerifySessionPrompt",
    "ChatBackdrop",
    "ThemedSurface",
    "ChatBackgroundEditor",
    "ChatBackgroundSettings",
    "SettingsBackupSection",
    "MediaPreviewSettings",
    "NetworkRouteSettings",
    "GradientEditor",
    "ThemeLevelSwitch",
    "ThemeToast",
    "ThemeLinkRow",
    "ThemeSimplePanel",
    "ThemePresetPrompt",
    "ThemePopover",
    "ThemeMenuPanel",
    "ThemeSharePanel",
    "ThemeReadabilityPanel",
    "ChatBackgroundDialog",
    "SettingsPercentBox",
    "SettingsNeedsBadge",
    "SettingsHint",
    "SettingsChoiceRow",
    "SettingsNavGroupLabel",
    "SettingsNavOutline",
    "LeaveRoomConfirmDialog",
    "InvitePreviewCard",
    "LabsConcurrentAccountsCard",
    "AccountActivityBadge",
    "AccountActivityFeed",
};

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

}

class QmlComponentLoadTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void streamInfoPollsOnlyWhileTheSurfaceIsVisible()
    {
        QQmlApplicationEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"QML(
import QtQuick
import MatrixClient
Item {
    width: 480; height: 320
    visible: false
    property alias requests: fake.requests
    property alias resets: fake.resets
    QtObject {
        id: fake
        property bool streamInfoVisible: true
        property int requests: 0
        property int resets: 0
        function refreshStreamInfo() { ++requests }
        function resetStreamInfoRates() { ++resets }
    }
    StreamInfoOverlay { anchors.fill: parent; controller: fake }
}
)QML", QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY(root);
        QQuickWindow window;
        window.resize(480, 320);
        auto *item = qobject_cast<QQuickItem *>(root.get());
        QVERIFY(item);
        item->setParentItem(window.contentItem());
        window.show();
        QTest::qWait(30);
        QCOMPARE(root->property("requests").toInt(), 0);
        root->setProperty("visible", true);
        QTRY_VERIFY(root->property("requests").toInt() >= 2);
        QCOMPARE(root->property("resets").toInt(), 1);
        root->setProperty("visible", false);
        const int before = root->property("requests").toInt();
        QTest::qWait(1150);
        QCOMPARE(root->property("requests").toInt(), before);
        root->setProperty("visible", true);
        QCOMPARE(root->property("resets").toInt(), 2);
        QVERIFY(root->property("requests").toInt() > before);
        item->setParentItem(nullptr);
    }

    void streamInfoPopulatedDelegateLoadsAsPlainText()
    {
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &error : errors) warnings << error.toString();
                });
        QQmlComponent component(&engine);
        component.loadFromModule(QStringLiteral("MatrixClient"),
                                 QStringLiteral("StreamInfoOverlay"));
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        const QVariantMap track{
            {"participant", "<b>Alice</b>"}, {"outgoing", true}, {"screen", true},
            {"codec", "VP8"}, {"encoder", "vp8enc"}, {"source", "pipewiresrc"},
            {"scaling", "GPU"}, {"width", 1920}, {"height", 1080},
            {"fps", 30.0}, {"kbps", 1200.0}, {"packets", 120},
            {"encrypted", true}, {"decrypted", 0}, {"dropped", 0}};
        std::unique_ptr<QObject> root(component.createWithInitialProperties(
            {{"stats", QVariantList{track}}, {"width", 460}, {"height", 300}}));
        QVERIFY(root);
        QQuickWindow window;
        window.resize(480, 320);
        auto *item = qobject_cast<QQuickItem *>(root.get());
        QVERIFY(item);
        item->setParentItem(window.contentItem());
        window.show();
        QTest::qWait(30);
        auto findRow = [](auto &&self, QQuickItem *node) -> QQuickItem * {
            if (node->objectName() == QStringLiteral("streamInfoRow")) return node;
            for (auto *child : node->childItems())
                if (auto *found = self(self, child)) return found;
            return nullptr;
        };
        QQuickItem *row = findRow(findRow, item);
        QVERIFY2(row, qPrintable(warnings.join('\n')));
        const QString text = row->property("text").toString();
        QVERIFY(text.contains(QStringLiteral("<b>Alice</b>")));
        QVERIFY(text.contains(QStringLiteral("1920")));
        QVERIFY(text.contains(QStringLiteral("vp8enc")));
        QCOMPARE(row->property("textFormat").toInt(), 0);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        item->setParentItem(nullptr);
    }

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

    void theHomePaneResolvesItsGreetingWithoutABindingLoop()
    {
        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(3000));

        const QString uid = QStringLiteral("@alice:mock.local");
        controller.settings()->setSecretStore(nullptr);
        controller.settings()->saveSession(QStringLiteral("https://mock.local"),
                                           uid, QStringLiteral("MOCKDEV"),
                                           QString());
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

        QCOMPARE(root->property("activeUserId").toString(), uid);
        QCOMPARE(root->property("displayName").toString(),
                 QStringLiteral("alice"));
        QCoreApplication::processEvents();

        for (const QString &warning : warnings) {
            QVERIFY2(!warning.contains(QStringLiteral("Binding loop")),
                     qPrintable(warning));
        }
    }

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

    void theRoomInfoPanelDoesNotPolishLoop()
    {
        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(3000));

        for (const int width : { 420 }) {
            QQmlEngine engine;
            engine.rootContext()->setContextProperty("app", &controller);
            QStringList warnings;
            connect(&engine, &QQmlEngine::warnings, this,
                    [&warnings](const QList<QQmlError> &errors) {
                        for (const QQmlError &e : errors)
                            warnings << e.toString();
                    });
            controller.openRoom(QStringLiteral("!general:mock.local"));
            QQmlComponent component(&engine);
            component.setData(QStringLiteral(R"(
import QtQuick
import QtQuick.Window
import MatrixClient

Window {
    width: %1
    height: 700
    visible: true
    RoomInfoPanel { anchors.fill: parent }
}
)").arg(width).toUtf8(), QUrl(QStringLiteral("qrc:/roominfopolishtest.qml")));
            QScopedPointer<QObject> window(component.create());
            QVERIFY2(window, qPrintable(component.errorString()));
            QTest::qWait(300);
            for (qreal w = 520; w >= 180; w -= 3.7) {
                window->setProperty("width", w);
                QTest::qWait(20);
            }
            for (const QString &warning : std::as_const(warnings)) {
                QVERIFY2(!warning.contains(QStringLiteral("polish()")),
                         qPrintable(QStringLiteral("width %1: ").arg(width)
                                    + warning));
            }
        }
    }

    void theRoomListSortMenuWritesTheSettingAndBothModelsFollow()
    {
        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(3000));
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

        auto *unread = menu->findChild<QObject *>(
            QStringLiteral("roomSortUnreadItem"));
        QVERIFY2(unread, "the menu lost its Unread first sort");
        QVERIFY(QMetaObject::invokeMethod(unread, "triggered"));
        QCOMPARE(controller.settings()->roomListSort(), 3);
        QVERIFY(unread->property("radioSelected").toBool());
        QCOMPARE(controller.roomList()->sortMode(), 3);
        QCOMPARE(controller.spaceChannels()->sortMode(), 3);
    }

    void theRailSortSettingReachesTheStoreAndTheMenuOffersIt()
    {
        AppController controller(AppController::MockBackend);
        QCOMPARE(controller.railLayout()->sortMode(), 0);
        controller.settings()->setRailSort(1);
        QCOMPARE(controller.railLayout()->sortMode(), 1);
        controller.settings()->setRailSort(2);
        QCOMPARE(controller.railLayout()->sortMode(), 2);
        controller.settings()->setRailSort(0);
        QCOMPARE(controller.railLayout()->sortMode(), 0);

        QFile rail(QStringLiteral(QML_DIR "/SpacesRail.qml"));
        QVERIFY(rail.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(rail.readAll());
        for (const char *name : { "railSortManual", "railSortName",
                                  "railSortActivity" }) {
            QVERIFY2(text.contains(QStringLiteral("objectName: \"%1\"")
                                       .arg(QLatin1String(name))),
                     name);
        }
    }

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

        QVERIFY(!controller.audioTester()->available());
        QVERIFY(!button->property("enabled").toBool());
        QVERIFY(!tone->property("enabled").toBool());
        QVERIFY(!notice->property("text").toString().isEmpty());

#ifdef HAVE_LIGHTNING_WEBRTC
        controller.audioTester()->setRuntimeAvailable(true);
        QVERIFY(button->property("enabled").toBool());
        QVERIFY(tone->property("enabled").toBool());
        QVERIFY2(notice->property("text").toString().isEmpty(),
                 qPrintable(notice->property("text").toString()));

        controller.audioTester()->setCallActive(true);
        QVERIFY(!button->property("enabled").toBool());
        QVERIFY(!tone->property("enabled").toBool());
        QVERIFY(!notice->property("text").toString().isEmpty());
        controller.audioTester()->setCallActive(false);
        controller.audioTester()->setRuntimeAvailable(false);
#endif

        auto *menu = owner->property("menu").value<QObject *>();
        QVERIFY(menu != nullptr);
        QVERIFY(menu->findChild<QObject *>(QStringLiteral("callMenuMicLevelRow")));
        QVERIFY(menu->findChild<QObject *>(QStringLiteral("callMenuMicLevelBar")));
        QVERIFY(!menu->property("showsLevel").toBool());

        for (const QString &warning : warnings) {
            const bool ours = warning.contains(QStringLiteral("audioTester"))
                || warning.contains(QStringLiteral("AudioLevelBar"))
                || warning.contains(QStringLiteral("microphoneLevel"))
                || warning.contains(QStringLiteral("microphoneTest"));
            QVERIFY2(!ours && !warning.contains(QStringLiteral("Binding loop")),
                     qPrintable(warning));
        }
    }

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
            int at = src.indexOf(QStringLiteral("delegate:"));
            while (at >= 0) {
                const int next =
                    src.indexOf(QStringLiteral("delegate:"), at + 1);
                const QString block =
                    src.mid(at, (next < 0 ? src.size() : next) - at);
                const bool disables =
                    block.contains(QStringLiteral("enabled: false"));
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
