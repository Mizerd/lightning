// A real MatrixRTC call, driven headlessly against a real homeserver.
// Signalling can keep working while ICE, DTLS or the media pipeline carries
// nothing, and neither code reading nor a screenshot tells those apart; this
// joins a room's call and prints what happened.
//
// Opt-in: it skips unless LIGHTNING_LIVE_* are set, so it is inert in CI.
//
//   LIGHTNING_LIVE_HOMESERVER=https://…
//   LIGHTNING_LIVE_USER=@someone:…      LIGHTNING_LIVE_PASSWORD=…
//   LIGHTNING_LIVE_ROOM=!room:…
//
// The password is read from the environment and never logged.
#include "calls/RtcController.h"
#include "calls/SfuCallController.h"
#include "calls/SfuMediaEngine.h"
#include "matrix/RustSdkMatrixClient.h"
#include "app/SettingsManager.h"

#include <QSignalSpy>
#include <QVideoFrame>
#include <QVideoSink>
#include <QDir>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {
QString env(const char *name)
{
    return QString::fromLocal8Bit(qgetenv(name));
}
} // namespace

class CallLiveDiagnostic : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        if (env("LIGHTNING_LIVE_USER").isEmpty()
            || env("LIGHTNING_LIVE_ROOM").isEmpty()) {
            QSKIP("live call diagnostic: set LIGHTNING_LIVE_* to run");
        }
        // A persistent store when named, so repeated runs restore a session
        // instead of logging in: Synapse's login rate limit is quickly
        // exhausted and then looks like a call defect.
        m_storePath = env("LIGHTNING_LIVE_STORE");
        if (m_storePath.isEmpty()) {
            QVERIFY(m_store.isValid());
            m_storePath = m_store.path();
        }
        QDir().mkpath(m_storePath);
        qputenv("XDG_DATA_HOME", m_storePath.toUtf8());
        qputenv("XDG_CONFIG_HOME", m_storePath.toUtf8());
        // Every category this diagnostic reads.
        QLoggingCategory::setFilterRules(
            QStringLiteral("lightning.calls*.debug=true\n"
                           "lightning.rtc*.debug=true\n"));
    }

    void aRealCallCarriesMedia()
    {
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        RtcController rtc;
        SfuCallController group;
        SfuMediaEngine engine;

        QString whyNot;
        if (!SfuMediaEngine::runtimeAvailable(&whyNot))
            QSKIP(qPrintable(QStringLiteral("no media runtime: %1").arg(whyNot)));
        // Synthetic capture: no microphone and no display.
        engine.setTestSourceMode(true);

        client.setCallMediaCapable(true);
        rtc.setClient(&client);
        // The same encryption resolver the app installs. Without it every room
        // reads as encrypted (the fail-closed default), and an unencrypted
        // room's media would be dropped as undecryptable.
        rtc.setEncryptionResolver([&client](const QString &roomId) {
            if (roomId.isEmpty())
                return RtcController::RoomEncryption::Unknown;
            const RoomInfo room = client.roomInfo(roomId);
            if (room.id.isEmpty() || !room.encryptionKnown)
                return RtcController::RoomEncryption::Unknown;
            return room.encrypted ? RtcController::RoomEncryption::Yes
                                  : RtcController::RoomEncryption::No;
        });
        rtc.setMediaAvailable(true);
        rtc.setMediaEncryptionAvailable(true);
        group.setClient(&client);
        group.setRtcController(&rtc);
        group.setMediaEngine(&engine);

        QSignalSpy loggedIn(&client, &MatrixClient::loginSucceeded);
        const bool restoring = client.restoreSession();
        if (!restoring) {
            client.login(env("LIGHTNING_LIVE_HOMESERVER"),
                         env("LIGHTNING_LIVE_USER"),
                         env("LIGHTNING_LIVE_PASSWORD"));
        }
        // Wait either way: restoreSession() returning true only means the
        // restore was dispatched. startSync() before login completes returns
        // silently, and the client then never fetches to-device messages
        // while otherwise looking healthy.
        QVERIFY2(loggedIn.wait(60000),
                 restoring ? "session restore failed" : "login failed");
        qInfo() << (restoring ? "restored a saved session (no login spent)"
                              : "logged in");
        // Sync must be running: membership publishing uses the SDK's
        // joined-room lookup, which knows no rooms without a sync.
        client.startSync();
        qInfo() << "logged in; syncing";

        const QString room = env("LIGHTNING_LIVE_ROOM");
        // Let the first sync land so the room and its membership exist before
        // discovery.
        QTest::qWait(15000);

        // Both: refresh() reads the room's MatrixRTC membership and discover()
        // runs transport discovery, as the app's room-change path does.
        rtc.discover(room);
        rtc.refresh(room);
        // Discovery is asynchronous; wait for it to settle.
        QTRY_VERIFY_WITH_TIMEOUT(!rtc.focusUrlFor(room).isEmpty()
                                     || rtc.joinBlockReason(room)
                                            != QLatin1String("undiscovered"),
                                 30000);
        qInfo() << "join block =" << rtc.joinBlockReason(room)
                << "focus known =" << !rtc.focusUrlFor(room).isEmpty();

        QSignalSpy failed(&group, &SfuCallController::callFailed);
        // Publishing needs the SDK to have imported the room, which the first
        // sync decides, so retry rather than sleep (op=0 means not joined yet).
        // LIGHTNING_LIVE_VIDEO=1 also publishes a camera test pattern, to
        // exercise the VP8 send path and far-end decryption.
        const bool withVideo = env("LIGHTNING_LIVE_VIDEO") == QLatin1String("1");
        bool joined = false;
        for (int attempt = 0; attempt < 20 && !joined; ++attempt) {
            joined = group.join(room, withVideo);
            if (!joined)
                QTest::qWait(3000);
        }
        qInfo() << "join() returned" << joined << "lastError" << group.lastError();
        QVERIFY2(joined, qPrintable(group.lastError()));

        // LIGHTNING_LIVE_SCREEN=1 publishes a synthetic screen share, so the
        // source the SFU records can be checked without a desktop portal.
        // Started before the hold (so it exists while sampled) and only once
        // connected: before the SFU join lands the engine is inactive and the
        // share puts no track on the wire.
        if (env("LIGHTNING_LIVE_SCREEN") == QLatin1String("1")) {
            QTRY_VERIFY_WITH_TIMEOUT(
                group.stateInt()
                    == static_cast<int>(SfuCallController::State::Connected),
                45000);
            const bool shared = group.startScreenShare(/*pipewireNodeId=*/0);
            qInfo() << "screen share started:" << shared;
        }

        // How long to stay: 40s suffices for a solo probe (ICE failures show
        // well before LiveKit's 60s join timeout); a two-client run needs
        // longer to stagger logins past the per-IP rate limit.
        bool holdOk = false;
        const int hold = env("LIGHTNING_LIVE_HOLD_MS").toInt(&holdOk);
        QTest::qWait(holdOk && hold > 0 ? hold : 40000);
        qInfo() << "participants json:" << group.participants();

        // The whole chain: attach a real sink to the remote participant's
        // video as a tile does, and require frames. Decrypting and rendering
        // are different facts; the crypto counters climb either way.
        QVideoSink sink;
        int framesRendered = 0;
        connect(&sink, &QVideoSink::videoFrameChanged, this,
                [&framesRendered](const QVideoFrame &) { ++framesRendered; });
        QString remote;
        for (const QVariant &row : group.participants()) {
            const QVariantMap p = row.toMap();
            if (!p.value(QStringLiteral("local")).toBool()) {
                remote = p.value(QStringLiteral("identity")).toString();
                break;
            }
        }
        if (!remote.isEmpty()) {
            group.attachVideoSink(remote, &sink);
            QTest::qWait(8000);
            // Release by sink, not participant, so it gives up exactly what
            // this sink holds.
            group.detachSink(&sink);
            qInfo() << "frames RENDERED on an attached sink:" << framesRendered;
        }

        qInfo() << "=== RESULT ==="
                << "state=" << group.stateInt()
                << "participants=" << group.participantCount()
                << "encrypted=" << group.mediaEncrypted()
                << "framesEncrypted=" << engine.framesEncrypted()
                << "framesDecrypted=" << engine.framesDecrypted()
                << "framesDropped=" << engine.framesDropped()
                << "lastError=" << group.lastError();
        for (const QList<QVariant> &row : failed)
            qWarning() << "callFailed:" << row.value(0).toString();

        group.leave();
        QTest::qWait(2000);

        QVERIFY2(engine.framesEncrypted() > 0,
                 "our own media never reached the wire");
        // With a second client present, media must arrive and decrypt; zero
        // decrypted frames beside thousands sent is the fault signature.
        if (!remote.isEmpty() && withVideo) {
            QVERIFY2(framesRendered > 0,
                     "remote video decrypted but never reached an attached "
                     "sink: the routing key the tile waits on does not match "
                     "the one the engine routes under");
        }
        if (group.participantCount() > 1) {
            QVERIFY2(engine.framesDecrypted() > 0,
                     qPrintable(QStringLiteral(
                         "media arrived from %1 peer(s) and none decrypted; "
                         "dropped=%2")
                                    .arg(group.participantCount() - 1)
                                    .arg(engine.framesDropped())));
        }
    }

private:
    QTemporaryDir m_store;
    QString m_storePath;
};

QTEST_MAIN(CallLiveDiagnostic)
#include "CallLiveDiagnostic.moc"
