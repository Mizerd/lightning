
#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "models/RoomListModel.h"
#include "models/SpaceChannelModel.h"
#include "spaces/SpaceManager.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#ifdef ENABLE_RUST_SDK_BACKEND
#include "matrix/RustSdkMatrixClient.h"
#endif

namespace {
const QString kRoomId = QStringLiteral("!room:example.org");
const QString kAliceId = QStringLiteral("@alice:example.org");
const QString kBobId = QStringLiteral("@bob:example.org");
const QString kHomeserver = QStringLiteral("https://example.org");

class SpaceFakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    QList<RoomInfo> roomList;

    void announce() { Q_EMIT roomsChanged(); }

    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return kAliceId; }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return roomList; }
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
    void editMessage(const QString &, const QString &,
                     const QString &) override {}
    void redactEvent(const QString &, const QString &,
                     const QString &) override {}
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

const QString kSpaceId = QStringLiteral("!space:example.org");
const QString kAlphaId = QStringLiteral("!alpha:example.org");
const QString kBetaId = QStringLiteral("!beta:example.org");
const QString kGammaId = QStringLiteral("!gamma:example.org");
const QString kDeltaId = QStringLiteral("!delta:example.org");

RoomInfo joinedRoom(const QString &id, bool isSpace = false)
{
    RoomInfo info;
    info.id = id;
    info.name = id;
    info.isSpace = isSpace;
    info.membership = RoomInfo::Joined;
    return info;
}

void attachSpace(AppController &app)
{
    auto *fake = new SpaceFakeClient(&app);
    RoomInfo space = joinedRoom(kSpaceId, true);
    space.childRoomIds = { kAlphaId, kBetaId, kGammaId, kDeltaId };
    fake->roomList = { space, joinedRoom(kAlphaId), joinedRoom(kBetaId),
                       joinedRoom(kGammaId), joinedRoom(kDeltaId) };
    app.spaces()->setClient(fake);
    fake->announce();
    QCoreApplication::processEvents();
}
}

class RoomNotificationModeTest : public QObject
{
    Q_OBJECT

#ifdef ENABLE_RUST_SDK_BACKEND
private:
    static RustSdkMatrixClient *rustClient(AppController &app)
    {
        return app.findChild<RustSdkMatrixClient *>();
    }
#endif

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("room-notification-mode-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void mockBackendKeepsTheLocalOnlyContract()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(!app.serverRoomNotificationModes());
        app.setRoomNotificationMode(kRoomId, 2);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 2);
        app.requestRoomNotificationMode(kRoomId);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 2);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));
    }

    void confirmedLowPriorityMovesInClassicAndChannels()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The tag confirmation comes from the Rust backend.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        const QString ordinary = QStringLiteral("!ordinary:example.org");
        const QString tagged = QStringLiteral("!tagged:example.org");
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("rooms")},
            {QStringLiteral("rooms"), QJsonArray{
                QJsonObject{{QStringLiteral("id"), ordinary},
                            {QStringLiteral("name"), QStringLiteral("Ordinary")},
                            {QStringLiteral("last_activity_ms"), 1900000000000.0},
                            {QStringLiteral("membership"), QStringLiteral("joined")}},
                QJsonObject{{QStringLiteral("id"), tagged},
                            {QStringLiteral("name"), QStringLiteral("Tagged")},
                            {QStringLiteral("last_activity_ms"), 2000000000000.0},
                            {QStringLiteral("membership"), QStringLiteral("joined")}}}}});
        auto *classic = app.roomList();
        auto *channels = app.spaceChannels();
        QVERIFY(classic && channels);
        auto classicRow = [&] {
            for (int row = 0; row < classic->rowCount(); ++row)
                if (classic->data(classic->index(row), RoomListModel::RoomIdRole)
                        .toString() == tagged)
                    return row;
            return -1;
        };
        QTRY_VERIFY(classicRow() >= 0);
        QTRY_VERIFY(channels->rowForRoom(tagged) >= 0);
        QTRY_VERIFY(channels->rowForRoom(tagged) < channels->rowForRoom(ordinary));
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("room_tag_confirmed")},
            {QStringLiteral("room_id"), tagged},
            {QStringLiteral("low_priority"), true}});
        QTRY_COMPARE(classic->data(classic->index(classicRow()),
                                   RoomListModel::CategoryRole).toString(),
                     QStringLiteral("lowpriority"));
        QTRY_VERIFY(channels->data(channels->index(channels->rowForRoom(tagged)),
                                   SpaceChannelModel::IsLowPriorityRole).toBool());
        QTRY_VERIFY(channels->rowForRoom(tagged) > channels->rowForRoom(ordinary));
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("room_tag_confirmed")},
            {QStringLiteral("room_id"), tagged},
            {QStringLiteral("low_priority"), false}});
        QTRY_COMPARE(classic->data(classic->index(classicRow()),
                                   RoomListModel::CategoryRole).toString(),
                     QStringLiteral("conversation"));
        QTRY_VERIFY(!channels->data(channels->index(channels->rowForRoom(tagged)),
                                    SpaceChannelModel::IsLowPriorityRole).toBool());
        QTRY_VERIFY(channels->rowForRoom(tagged) < channels->rowForRoom(ordinary));
#endif
    }

    void entryPointRejectsInvalidInput()
    {
        AppController app(AppController::MockBackend);
        app.setRoomNotificationMode(kRoomId, 1);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 1);
        app.setRoomNotificationMode(kRoomId, -1);
        app.setRoomNotificationMode(kRoomId, 4);
        app.setRoomNotificationMode(QString{}, 2);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 1);
        app.setRoomNotificationMode(kRoomId, 3);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 3);
        app.setRoomNotificationMode(kRoomId, 1);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 1);
        const QString threadId = MatrixClient::threadTimelineId(
            kRoomId, QStringLiteral("$root:example.org"));
        app.setRoomNotificationMode(threadId, 2);
        QCOMPARE(app.settings()->roomNotificationMode(threadId), 0);
        app.requestRoomNotificationMode(threadId);
    }

    void modeZeroStillRemovesTheLocalKey()
    {
        AppController app(AppController::MockBackend);
        const QString key =
            QStringLiteral("notifications/room-mode/") + kRoomId;
        app.setRoomNotificationMode(kRoomId, 1);
        {
            QSettings store;
            QVERIFY(store.contains(key));
        }
        app.setRoomNotificationMode(kRoomId, 0);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 0);
        {
            QSettings store;
            QVERIFY(!store.contains(key));
        }
    }

    void accountsKeepIndependentModes()
    {
        SettingsManager settings;
        settings.saveSession(kHomeserver, kAliceId,
                             QStringLiteral("ALICEDEV"), QString{});
        settings.saveSession(kHomeserver, kBobId,
                             QStringLiteral("BOBDEV"), QString{});
        QVERIFY(settings.hasSavedAccount(kAliceId));
        QVERIFY(settings.hasSavedAccount(kBobId));

        settings.setActiveAccountUserId(kAliceId);
        settings.setRoomNotificationMode(kRoomId, 2);
        QCOMPARE(settings.roomNotificationMode(kRoomId), 2);

        settings.setActiveAccountUserId(kBobId);
        QCOMPARE(settings.roomNotificationMode(kRoomId), 0);
        settings.setRoomNotificationMode(kRoomId, 1);
        QCOMPARE(settings.roomNotificationMode(kRoomId), 1);

        settings.setActiveAccountUserId(kAliceId);
        QCOMPARE(settings.roomNotificationMode(kRoomId), 2);

        settings.setRoomNotificationMode(kRoomId, 0);
        QCOMPARE(settings.roomNotificationMode(kRoomId), 0);
        QSettings raw;
        const QStringList keys = raw.allKeys();
        int aliceKeys = 0;
        for (const QString &key : keys) {
            if (key.endsWith(QStringLiteral("notifications/room-mode/")
                             + kRoomId)
                && key.contains(QStringLiteral("alice"))) {
                ++aliceKeys;
            }
        }
        QCOMPARE(aliceKeys, 0);
    }

    void legacyGlobalModeFallsBackUntilFirstWrite()
    {
        SettingsManager settings;
        {
            QSettings raw;
            raw.setValue(QStringLiteral("notifications/room-mode/") + kRoomId,
                         2);
        }
        settings.saveSession(kHomeserver, kAliceId,
                             QStringLiteral("ALICEDEV"), QString{});
        settings.saveSession(kHomeserver, kBobId,
                             QStringLiteral("BOBDEV"), QString{});
        QVERIFY(settings.hasSavedAccount(kAliceId));
        QVERIFY(settings.hasSavedAccount(kBobId));

        settings.setActiveAccountUserId(kAliceId);
        QCOMPARE(settings.roomNotificationMode(kRoomId), 2);
        settings.setActiveAccountUserId(kBobId);
        QCOMPARE(settings.roomNotificationMode(kRoomId), 2);

        settings.setRoomNotificationMode(kRoomId, 0);
        QCOMPARE(settings.roomNotificationMode(kRoomId), 0);

        settings.setActiveAccountUserId(kAliceId);
        QCOMPARE(settings.roomNotificationMode(kRoomId), 2);
        settings.setRoomNotificationMode(kRoomId, 1);
        QCOMPARE(settings.roomNotificationMode(kRoomId), 1);

        settings.setActiveAccountUserId(kBobId);
        QCOMPARE(settings.roomNotificationMode(kRoomId), 0);
    }

    void rustBackendReportsServerCapability()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Server notification modes exist on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        QVERIFY(app.serverRoomNotificationModes());
        app.setRoomNotificationMode(kRoomId, 2);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 2);
#endif
    }

    void serverReportReconcilesTheCache()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Server notification modes exist on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        auto *settings = app.settings();

        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 2, true);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 2);

        QSignalSpy changed(settings,
                           &SettingsManager::roomNotificationModeChanged);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, false);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 2);
        QCOMPARE(changed.count(), 0);

        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 7, true);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, -1, true);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 2);
        QCOMPARE(changed.count(), 0);

        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 2, true);
        QCOMPARE(changed.count(), 0);

        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 1, true);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 1);
#endif
    }

    void failedServerWriteFlipsTheHonestSyncState()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Server notification modes exist on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));

        QSignalSpy syncState(
            &app, &AppController::roomNotificationModeSyncStateChanged);

        app.setRoomNotificationMode(kRoomId, 2);
        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 2);
        QCOMPARE(syncState.count(), 1);

        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        QCOMPARE(syncState.count(), 1);

        QVERIFY(!app.roomNotificationModeSyncFailed(
            QStringLiteral("!other:example.org")));

        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, true);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 2);
        QCOMPARE(syncState.count(), 1);

        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 2, true);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(syncState.count(), 2);

        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 1, true);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 1);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));

        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        QCOMPARE(syncState.count(), 3);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, false);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 1);
#endif
    }

    void failedWriteIsRetriedOnReconnectButNotPrematurelyCleared()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);

        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        app.setRoomNotificationMode(kRoomId, 2);
        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));

        QSignalSpy syncState(
            &app, &AppController::roomNotificationModeSyncStateChanged);
        QSignalSpy retried(&app,
                           &AppController::roomNotificationModesRetried);

        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(retried.count(), 1);
        QCOMPARE(retried.first().at(0).toInt(), 1);

        Q_EMIT rust->connectionStateChanged(MatrixClient::Offline);
        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(retried.count(), 2);

        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 2);
        QCOMPARE(syncState.count(), 0);

        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 2, true);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(syncState.count(), 1);
#endif
    }

    void aFailedWriteSurvivesARestartAndASignOutEvent()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        {
            AppController app(AppController::RustBackend);
            auto *rust = rustClient(app);
            QVERIFY(rust);
            app.setRoomNotificationMode(kRoomId, 1);
            Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
            QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
            Q_EMIT rust->loggedOut();
            QVERIFY2(app.roomNotificationModeSyncFailed(kRoomId),
                     "a sign-out event forgot the unsynced room");
        }
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        QVERIFY2(app.roomNotificationModeSyncFailed(kRoomId),
                 "a restart forgot the unsynced room");
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 1);
        QSignalSpy retried(&app, &AppController::roomNotificationModesRetried);
        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(retried.count(), 0);
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        QCOMPARE(retried.count(), 1);
        QCOMPARE(retried.first().at(0).toInt(), 1);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 1, true);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));
        QVERIFY(app.settings()->unsyncedRoomNotificationModes().isEmpty());
#endif
    }

    void aRuleReportedBeforeTheFirstSyncIsNotAFailedWritesBase()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 1, true);
        app.setRoomNotificationMode(kRoomId, 2);
        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(app.settings()->roomNotificationModeUnsyncedBase(kRoomId),
                 -1);
#endif
    }

    void theRetryResendsTheLatestChoiceWhenAnEarlierOneWasAcknowledged()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, true);
        QSignalSpy sent(&app, &AppController::roomNotificationModeSent);
        app.setRoomNotificationMode(kRoomId, 2);
        app.setRoomNotificationMode(kRoomId, 1);
        QCOMPARE(sent.count(), 2);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 2, true);
        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(app.settings()->roomNotificationModeUnsyncedBase(kRoomId), 2);

        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 2, true);
        QCOMPARE(sent.count(), 3);
        QCOMPARE(sent.last().at(1).toInt(), 1);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 1);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 1, true);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));
#endif
    }

    void anEarlierChoicesFailureDoesNotResendItAfterALaterOneIsAcknowledged()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        QSignalSpy sent(&app, &AppController::roomNotificationModeSent);
        QSignalSpy retried(&app, &AppController::roomNotificationModesRetried);

        app.setRoomNotificationMode(kRoomId, 2);
        app.setRoomNotificationMode(kRoomId, 1);
        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 1, true);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));
        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(retried.count(), 0);
        QCOMPARE(sent.count(), 2);

        app.setRoomNotificationMode(kRoomId, 0);
        app.setRoomNotificationMode(kRoomId, 2);
        QCOMPARE(sent.count(), 4);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 2, true);
        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        Q_EMIT rust->connectionStateChanged(MatrixClient::Offline);
        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(retried.count(), 1);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 2, true);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(sent.count(), 4);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 2);
#endif
    }

    void aLaterChoiceSupersedesAPendingRetry()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, true);
        app.setRoomNotificationMode(kRoomId, 1);
        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        QSignalSpy sent(&app, &AppController::roomNotificationModeSent);
        QSignalSpy retried(&app, &AppController::roomNotificationModesRetried);
        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(retried.count(), 1);

        app.setRoomNotificationMode(kRoomId, 2);
        QCOMPARE(sent.count(), 1);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, true);
        QCOMPARE(sent.count(), 1);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 2);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 2, true);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));
#endif
    }

    void unsyncedRoomsArePerAccount()
    {
        SettingsManager settings;
        settings.saveSession(kHomeserver, kAliceId,
                             QStringLiteral("ALICEDEV"), QString{});
        settings.saveSession(kHomeserver, kBobId,
                             QStringLiteral("BOBDEV"), QString{});
        settings.setActiveAccountUserId(kAliceId);
        settings.setRoomNotificationModeUnsynced(kRoomId, true);
        QVERIFY(settings.roomNotificationModeUnsynced(kRoomId));
        QCOMPARE(settings.unsyncedRoomNotificationModes(),
                 QStringList{kRoomId});

        settings.setActiveAccountUserId(kBobId);
        QVERIFY(!settings.roomNotificationModeUnsynced(kRoomId));
        QVERIFY(settings.unsyncedRoomNotificationModes().isEmpty());

        settings.setActiveAccountUserId(kAliceId);
        QVERIFY(settings.roomNotificationModeUnsynced(kRoomId));
        settings.setRoomNotificationModeUnsynced(kRoomId, false);
        QVERIFY(!settings.roomNotificationModeUnsynced(kRoomId));
        QVERIFY(settings.unsyncedRoomNotificationModes().isEmpty());
    }

    void aDeviceOnlyLegacyModeDecidesLocallyAndIsNeverSent()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        {
            QSettings raw;
            raw.setValue(QStringLiteral("notifications/room-mode/") + kRoomId,
                         2);
        }
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        auto *settings = app.settings();
        settings->saveSession(kHomeserver, kAliceId,
                              QStringLiteral("ALICEDEV"), QString{});
        settings->saveSession(kHomeserver, kBobId,
                              QStringLiteral("BOBDEV"), QString{});
        settings->setActiveAccountUserId(kAliceId);
        QSignalSpy sent(&app, &AppController::roomNotificationModeSent);
        QSignalSpy refreshed(&app,
                             &AppController::roomNotificationModesRefreshed);
        QSignalSpy retried(&app, &AppController::roomNotificationModesRetried);

        QVERIFY(settings->roomNotificationModeDeviceOnly(kRoomId));
        QVERIFY(app.roomNotificationModeDecidesLocally(kRoomId));
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));

        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        QCOMPARE(refreshed.count(), 1);
        QCOMPARE(refreshed.first().at(0).toInt(), 0);
        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(retried.count(), 0);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, false);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 2);
        QVERIFY(settings->roomNotificationModeDeviceOnly(kRoomId));

        settings->setActiveAccountUserId(kBobId);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, false);
        QVERIFY(settings->accountRoomNotificationModeRooms().isEmpty());
        QVERIFY(settings->unsyncedRoomNotificationModes().isEmpty());

        QCOMPARE(sent.count(), 0);
        {
            QSettings raw;
            const QStringList keys = raw.allKeys();
            for (const QString &key : keys) {
                QVERIFY2(!(key.startsWith(QStringLiteral("accounts/"))
                           && key.contains(QStringLiteral("room-mode"))),
                         qPrintable(QStringLiteral("an account key was "
                                                   "written: ") + key));
            }
        }

        settings->setActiveAccountUserId(kAliceId);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 1, true);
        QVERIFY(!settings->roomNotificationModeDeviceOnly(kRoomId));
        QVERIFY(!app.roomNotificationModeDecidesLocally(kRoomId));
#endif
    }

    void anExplicitChoiceInADeviceOnlyRoomIsSentAgainNotAdopted()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        const QString unasked = QStringLiteral("!unasked:example.org");
        {
            QSettings raw;
            raw.setValue(QStringLiteral("notifications/room-mode/") + kRoomId,
                         2);
            raw.setValue(QStringLiteral("notifications/room-mode/") + unasked,
                         1);
        }
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        auto *settings = app.settings();
        settings->saveSession(kHomeserver, kAliceId,
                              QStringLiteral("ALICEDEV"), QString{});
        settings->setActiveAccountUserId(kAliceId);
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        QVERIFY(settings->roomNotificationModeDeviceOnly(kRoomId));
        QVERIFY(settings->roomNotificationModeDeviceOnly(unasked));

        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, false);
        QSignalSpy sent(&app, &AppController::roomNotificationModeSent);
        app.setRoomNotificationMode(kRoomId, 1);
        app.setRoomNotificationMode(unasked, 2);
        QCOMPARE(sent.count(), 2);
        QVERIFY(!settings->roomNotificationModeDeviceOnly(kRoomId));
        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        Q_EMIT rust->roomNotificationModeWriteFailed(unasked);
        QCOMPARE(settings->roomNotificationModeUnsyncedBase(kRoomId), 3);
        QCOMPARE(settings->roomNotificationModeUnsyncedBase(unasked), -1);

        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, false);
        Q_EMIT rust->roomNotificationModeChanged(unasked, 0, false);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 1);
        QCOMPARE(settings->roomNotificationMode(unasked), 2);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        QVERIFY(app.roomNotificationModeSyncFailed(unasked));
        QCOMPARE(sent.count(), 4);
        QCOMPARE(sent.at(2).at(1).toInt(), 1);
        QCOMPARE(sent.at(3).at(1).toInt(), 2);
#endif
    }

    void anExplicitChoiceEqualToTheLegacyModeWritesTheAccountKey()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        {
            QSettings raw;
            raw.setValue(QStringLiteral("notifications/room-mode/") + kRoomId,
                         2);
        }
        AppController app(AppController::RustBackend);
        auto *settings = app.settings();
        settings->saveSession(kHomeserver, kAliceId,
                              QStringLiteral("ALICEDEV"), QString{});
        settings->setActiveAccountUserId(kAliceId);
        QVERIFY(settings->roomNotificationModeDeviceOnly(kRoomId));
        QSignalSpy sent(&app, &AppController::roomNotificationModeSent);
        app.setRoomNotificationMode(kRoomId, 2);
        QCOMPARE(sent.count(), 1);
        QVERIFY(!settings->roomNotificationModeDeviceOnly(kRoomId));
        QVERIFY(settings->accountRoomNotificationModeRooms().contains(kRoomId));
        QCOMPARE(settings->roomNotificationMode(kRoomId), 2);
#endif
    }

    void aRuleTheServerNoLongerHasIsAdoptedAsFollowDefault()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        auto *settings = app.settings();
        settings->saveSession(kHomeserver, kAliceId,
                              QStringLiteral("ALICEDEV"), QString{});
        settings->setActiveAccountUserId(kAliceId);
        QSignalSpy sent(&app, &AppController::roomNotificationModeSent);
        QSignalSpy refreshed(&app,
                             &AppController::roomNotificationModesRefreshed);

        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 2, true);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 2);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, false);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 2);

        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        QCOMPARE(refreshed.count(), 1);
        QCOMPARE(refreshed.first().at(0).toInt(), 1);

        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, false);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 3);
        QVERIFY(!app.roomNotificationModeDecidesLocally(kRoomId));
        QCOMPARE(sent.count(), 0);
#endif
    }

    void aReadRacingThisDevicesOwnWriteIsNotAdopted()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        auto *settings = app.settings();
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 1, true);

        QSignalSpy sent(&app, &AppController::roomNotificationModeSent);
        app.setRoomNotificationMode(kRoomId, 2);
        QCOMPARE(sent.count(), 1);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 1, true);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 2);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, false);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 2);

        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(settings->roomNotificationMode(kRoomId), 2);
        QCOMPARE(settings->roomNotificationModeUnsyncedBase(kRoomId), 1);
#endif
    }

    void theRetryReadsBeforeItWrites()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        {
            AppController app(AppController::RustBackend);
            auto *rust = rustClient(app);
            QVERIFY(rust);
            rust->handleRustEventForTest(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
            Q_EMIT rust->roomNotificationModeChanged(kRoomId, 1, true);
            app.setRoomNotificationMode(kRoomId, 2);
            Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
            QCOMPARE(app.settings()->roomNotificationModeUnsyncedBase(kRoomId),
                     1);
        }
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        auto *settings = app.settings();
        QSignalSpy sent(&app, &AppController::roomNotificationModeSent);
        QSignalSpy retried(&app, &AppController::roomNotificationModesRetried);
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(retried.count(), 1);
        QCOMPARE(sent.count(), 0);

        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 1, true);
        QCOMPARE(sent.count(), 1);
        QCOMPARE(sent.first().at(1).toInt(), 2);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(settings->roomNotificationMode(kRoomId), 2);

        Q_EMIT rust->connectionStateChanged(MatrixClient::Offline);
        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(retried.count(), 2);
        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, true);
        QCOMPARE(settings->roomNotificationMode(kRoomId), 0);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(sent.count(), 1);
#endif
    }

    void theRetryHandlesARuleThatIsGone()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        const QString wasMuted = QStringLiteral("!was-muted:example.org");
        const QString hadNoRule = QStringLiteral("!no-rule:example.org");
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        auto *settings = app.settings();
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        Q_EMIT rust->roomNotificationModeChanged(wasMuted, 2, true);
        app.setRoomNotificationMode(hadNoRule, 3);
        Q_EMIT rust->roomNotificationModeCleared(hadNoRule);
        QCOMPARE(settings->roomNotificationMode(hadNoRule), 3);
        app.setRoomNotificationMode(wasMuted, 1);
        app.setRoomNotificationMode(hadNoRule, 1);
        Q_EMIT rust->roomNotificationModeWriteFailed(wasMuted);
        Q_EMIT rust->roomNotificationModeWriteFailed(hadNoRule);

        QSignalSpy sent(&app, &AppController::roomNotificationModeSent);
        Q_EMIT rust->connectionStateChanged(MatrixClient::Syncing);
        Q_EMIT rust->roomNotificationModeChanged(wasMuted, 0, false);
        QCOMPARE(settings->roomNotificationMode(wasMuted), 3);
        QVERIFY(!app.roomNotificationModeSyncFailed(wasMuted));
        Q_EMIT rust->roomNotificationModeChanged(hadNoRule, 0, false);
        QCOMPARE(sent.count(), 1);
        QCOMPARE(sent.first().at(0).toString(), hadNoRule);
        QCOMPARE(settings->roomNotificationMode(hadNoRule), 1);
        QVERIFY(app.roomNotificationModeSyncFailed(hadNoRule));
#endif
    }

    void followAccountDefaultIsStoredDistinctlyFromAnAbsentKey()
    {
        AppController app(AppController::MockBackend);
        app.setRoomNotificationMode(kRoomId, 3);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 3);
        QCOMPARE(app.settings()->roomNotificationMode(
                     QStringLiteral("!never-configured:example.org")), 0);
    }

    void clearAcknowledgementRetiresAFailedFollowDefault()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);

        app.setRoomNotificationMode(kRoomId, 3);
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 3);
        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));

        Q_EMIT rust->roomNotificationModeChanged(kRoomId, 0, true);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));

        Q_EMIT rust->roomNotificationModeCleared(kRoomId);
        QVERIFY(!app.roomNotificationModeSyncFailed(kRoomId));
        QCOMPARE(app.settings()->roomNotificationMode(kRoomId), 3);

        app.setRoomNotificationMode(kRoomId, 1);
        Q_EMIT rust->roomNotificationModeWriteFailed(kRoomId);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
        Q_EMIT rust->roomNotificationModeCleared(kRoomId);
        QVERIFY(app.roomNotificationModeSyncFailed(kRoomId));
#endif
    }

    void anAccountDefaultChangeRefreshesTheRoomModes()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("server notification modes require the Rust backend");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("initial_sync_done")}});
        QSignalSpy refreshed(&app,
                             &AppController::roomNotificationModesRefreshed);
        QVERIFY(app.pushRules() != nullptr);
        Q_EMIT app.pushRules()->accountDefaultChanged();
        QCOMPARE(refreshed.count(), 1);
#endif
    }

    void unmutingASpaceRestoresEachRoomsOwnMode()
    {
        {
            SettingsManager settings;
            settings.saveSession(kHomeserver, kAliceId,
                                 QStringLiteral("ALICEDEV"), QString{});
            settings.setActiveAccountUserId(kAliceId);
        }
        {
            AppController app(AppController::MockBackend);
            QCOMPARE(app.settings()->activeAccountUserId(), kAliceId);
            attachSpace(app);
            QCOMPARE(app.spaces()->roomsInSpace(kSpaceId).size(), 4);
            app.setRoomNotificationMode(kBetaId, 1);
            app.setRoomNotificationMode(kGammaId, 2);
            app.setRoomNotificationMode(kDeltaId, 3);

            app.setSpaceMuted(kSpaceId, true);
            QVERIFY(app.spaceIsMuted(kSpaceId));
            app.setSpaceMuted(kSpaceId, true);
            app.setRoomNotificationMode(kDeltaId, 1);
        }
        AppController app(AppController::MockBackend);
        QCOMPARE(app.settings()->activeAccountUserId(), kAliceId);
        attachSpace(app);
        auto *settings = app.settings();
        app.setSpaceMuted(kSpaceId, false);
        QCOMPARE(settings->roomNotificationMode(kAlphaId), 3);
        QCOMPARE(settings->roomNotificationMode(kBetaId), 1);
        QCOMPARE(settings->roomNotificationMode(kGammaId), 2);
        QCOMPARE(settings->roomNotificationMode(kDeltaId), 1);
        QVERIFY(!app.spaceIsMuted(kSpaceId));

        app.setRoomNotificationMode(kBetaId, 2);
        app.setSpaceMuted(kSpaceId, false);
        QCOMPARE(settings->roomNotificationMode(kBetaId), 3);
        QCOMPARE(settings->roomNotificationMode(kGammaId), 3);
    }

    void anOpenRoomThatLeavesTheListIsClosed()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The room list is driven through the Rust backend here.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        const QString joined = QStringLiteral("!joined:example.org");
        const QString invite = QStringLiteral("!invite:example.org");
        const auto listed = [](const QString &id, const char *membership) {
            return QJsonObject{{QStringLiteral("id"), id},
                               {QStringLiteral("name"), id},
                               {QStringLiteral("membership"),
                                QString::fromLatin1(membership)}};
        };
        const auto publish = [rust](const QJsonArray &rooms) {
            rust->handleRustEventForTest(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("rooms")},
                {QStringLiteral("rooms"), rooms}});
        };
        publish({listed(joined, "joined"), listed(invite, "invited")});

        app.openRoom(invite);
        QCOMPARE(app.currentRoomId(), invite);
        publish({listed(joined, "joined")});
        QCOMPARE(app.currentRoomId(), invite);
        QTRY_COMPARE_WITH_TIMEOUT(app.currentRoomId(), QString(),
                                  InviteNoticeTracker::kGoneGraceMs + 2000);

        publish({listed(joined, "joined"), listed(invite, "invited")});
        app.openRoom(invite);
        publish({listed(joined, "joined")});
        publish({listed(joined, "joined"), listed(invite, "invited")});
        QTest::qWait(InviteNoticeTracker::kGoneGraceMs + 300);
        QCOMPARE(app.currentRoomId(), invite);

        app.openRoom(invite);
        publish({listed(joined, "joined")});
        app.openRoom(joined);
        app.openRoom(invite);
        QTest::qWait(InviteNoticeTracker::kGoneGraceMs + 300);
        QCOMPARE(app.currentRoomId(), invite);

        app.openRoom(joined);
        publish({listed(joined, "left")});
        QCOMPARE(app.currentRoomId(), QString());

        const QString unlisted = QStringLiteral("!unlisted:example.org");
        app.openRoom(unlisted);
        publish({listed(joined, "joined")});
        QTest::qWait(InviteNoticeTracker::kGoneGraceMs + 300);
        QCOMPARE(app.currentRoomId(), unlisted);
#endif
    }

    void aRoomLostElsewhereClosesAtOnceAndSaysWhy()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The membership reconciler runs on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        const QString open = QStringLiteral("!open:example.org");
        const QString other = QStringLiteral("!other:example.org");
        const QString quiet = QStringLiteral("!quiet:example.org");
        QJsonArray rooms;
        for (const QString &id : {open, other, quiet}) {
            rooms.append(QJsonObject{{QStringLiteral("id"), id},
                                     {QStringLiteral("name"), id},
                                     {QStringLiteral("membership"),
                                      QStringLiteral("joined")}});
        }
        rust->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("rooms")},
            {QStringLiteral("rooms"), rooms}});
        const auto lost = [rust](const QString &id, const char *loss,
                                 const QString &name) {
            rust->handleRustEventForTest(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("room_membership_lost")},
                {QStringLiteral("room_id"), id},
                {QStringLiteral("loss"), QString::fromLatin1(loss)},
                {QStringLiteral("name"), name}});
        };
        QSignalSpy reported(&app, &AppController::errorReported);

        app.openRoom(open);
        QCOMPARE(app.currentRoomId(), open);
        lost(open, "kicked", QStringLiteral("Ops"));
        QCOMPARE(app.currentRoomId(), QString());
        QVERIFY(!reported.isEmpty());
        QCOMPARE(reported.last().at(0).toString(),
                 QStringLiteral("You were removed from Ops."));

        reported.clear();
        lost(other, "banned", QString());
        QCOMPARE(reported.size(), 1);
        QCOMPARE(reported.last().at(0).toString(),
                 QStringLiteral("You were banned from %1.").arg(other));
        reported.clear();
        lost(quiet, "left", QStringLiteral("Quiet"));
        QVERIFY(reported.isEmpty());

        app.openRoom(quiet);
        lost(quiet, "left", QStringLiteral("Quiet"));
        QCOMPARE(app.currentRoomId(), QString());
        QCOMPARE(reported.last().at(0).toString(),
                 QStringLiteral("You left Quiet from another session."));
#endif
    }

    void aLossFromTheAccountBeforeASwitchLeavesTheSameRoomOpen()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The membership reconciler runs on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        const QString shared = QStringLiteral("!shared:example.org");
        const QJsonObject list{
            {QStringLiteral("type"), QStringLiteral("rooms")},
            {QStringLiteral("rooms"),
             QJsonArray{QJsonObject{{QStringLiteral("id"), shared},
                                    {QStringLiteral("name"), shared},
                                    {QStringLiteral("membership"),
                                     QStringLiteral("joined")}}}}};
        const QJsonObject lost{
            {QStringLiteral("type"), QStringLiteral("room_membership_lost")},
            {QStringLiteral("room_id"), shared},
            {QStringLiteral("loss"), QStringLiteral("kicked")},
            {QStringLiteral("name"), QStringLiteral("Ops")}};

        rust->handleRustEventForTest(list);
        const quint64 oldAccount = rust->m_lifecycle.activeGeneration();
        QVERIFY(oldAccount != 0);

        rust->m_handleGeneration = rust->m_lifecycle.beginSession();
        QVERIFY(rust->m_handleGeneration != oldAccount);
        rust->handleRustEventForTest(list);
        app.openRoom(shared);
        QCOMPARE(app.currentRoomId(), shared);
        QSignalSpy reported(&app, &AppController::errorReported);

        rust->handleRustEvent(lost, oldAccount);
        QCOMPARE(app.currentRoomId(), shared);
        QVERIFY(reported.isEmpty());

        rust->handleRustEvent(lost, rust->m_lifecycle.activeGeneration());
        QCOMPARE(app.currentRoomId(), QString());
        QCOMPARE(reported.size(), 1);
#endif
    }

    void anEventQueueNeverOutlivesItsAccount()
    {
        QFile file(QStringLiteral(QT_TESTCASE_SOURCEDIR
                                  "/src/matrix/RustSdkMatrixClient.cpp"));
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 qPrintable(file.errorString()));
        const QString source = QString::fromUtf8(file.readAll());
        const auto body = [&source](const QString &signature) {
            const qsizetype start = source.indexOf(signature);
            if (start < 0)
                return QString();
            const qsizetype end = source.indexOf(
                QLatin1String("\nbool RustSdkMatrixClient::"), start + 1);
            const qsizetype otherEnd = source.indexOf(
                QLatin1String("\nvoid RustSdkMatrixClient::"), start + 1);
            qsizetype stop = end < 0 ? source.size() : end;
            if (otherEnd > 0 && otherEnd < stop)
                stop = otherEnd;
            return source.mid(start, stop - start);
        };

        const QString ensure = body(QStringLiteral(
            "bool RustSdkMatrixClient::ensureRustHandleForStorePath("));
        QVERIFY(!ensure.isEmpty());
        const qsizetype released = ensure.indexOf(QLatin1String("releaseRustHandle();"));
        const qsizetype created = ensure.indexOf(QLatin1String("mx_rust_create("));
        const qsizetype stamped = ensure.indexOf(
            QLatin1String("m_handleGeneration = m_lifecycle.beginSession();"));
        QVERIFY2(released >= 0 && created > released,
                 "a new handle is created while the old one may still be polled");
        QVERIFY2(stamped > created, "a new handle reuses the old generation");

        const QString poll = body(QStringLiteral("void RustSdkMatrixClient::pollRustEvents()"));
        QVERIFY(!poll.isEmpty());
        QVERIFY(poll.contains(QLatin1String("const quint64 eventGeneration = m_handleGeneration;")));
        QCOMPARE(poll.count(QStringLiteral("if (m_lifecycle.acceptsActive(eventGeneration)) {")), 2);
    }

    void anIgnoreListChangeReopensTheOpenTimelines()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The SDK event cache exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        const QString room = QStringLiteral("!room-a:example.org");
        const QString other = QStringLiteral("!room-b:example.org");
        const QString root = QStringLiteral("$root:example.org");
        QSignalSpy reloaded(&app,
                            &AppController::timelinesReloadedAfterIgnoreChange);

        app.openRoom(room);
        app.thread()->openThread(room, root);
        QVERIFY(app.thread()->state() != ThreadController::Closed);
        Q_EMIT rust->ignoredUsersChanged({QStringLiteral("@troll:example.org")});
        QTRY_COMPARE(reloaded.count(), 1);
        QCOMPARE(reloaded.at(0).at(0).toBool(), true);
        QCOMPARE(reloaded.at(0).at(1).toBool(), true);
        QCOMPARE(app.thread()->rootEventId(), root);

        Q_EMIT rust->ignoredUsersChanged({});
        app.openRoom(other);
        QTRY_COMPARE(reloaded.count(), 2);
        QCOMPARE(reloaded.at(1).at(0).toBool(), false);
        QCOMPARE(reloaded.at(1).at(1).toBool(), false);
        QCOMPARE(app.currentRoomId(), other);
#endif
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_GUILESS_MAIN(RoomNotificationModeTest)
#include "RoomNotificationModeTest.moc"
