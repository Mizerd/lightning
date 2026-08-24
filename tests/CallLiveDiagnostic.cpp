// A REAL MatrixRTC call, driven headlessly, against a real homeserver.
//
// This exists because the calling lane's failures are invisible from the
// outside: signalling runs over a WebSocket and keeps working — participants,
// speakers and mute all update — while ICE, DTLS or the media pipeline never
// carries a packet. Reading code cannot tell those apart, and neither can a
// screenshot. This joins a room's call for real and prints what happened.
//
// Opt-in and credential-free by default: it SKIPS unless
// LIGHTNING_LIVE_* are set, so it is inert in CI and in every ordinary run.
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
        // A PERSISTENT store when one is named, so repeated runs RESTORE a
        // session instead of logging in again. Synapse's default login rate
        // limit is a burst of 5 and then one per several minutes, which a
        // fresh login per run exhausts almost immediately — and the failure
        // then looks like a call defect rather than a limiter.
        m_storePath = env("LIGHTNING_LIVE_STORE");
        if (m_storePath.isEmpty()) {
            QVERIFY(m_store.isValid());
            m_storePath = m_store.path();
        }
        QDir().mkpath(m_storePath);
        qputenv("XDG_DATA_HOME", m_storePath.toUtf8());
        qputenv("XDG_CONFIG_HOME", m_storePath.toUtf8());
        // Every category this diagnostic exists to read.
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
        // Synthetic capture: this runs with no microphone and no display.
        engine.setTestSourceMode(true);

        client.setCallMediaCapable(true);
        rtc.setClient(&client);
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
        // WAIT EITHER WAY. `restoreSession()` returning true only means the
        // restore was DISPATCHED — the session becomes usable when the Rust
        // side answers and the poll handler sets the logged-in state. Calling
        // startSync() before that returns silently (`if (!m_loggedIn) return`)
        // and the client then never syncs at all, while still being able to
        // send and to read state over the network. That produced a harness
        // that looked healthy — rooms resolved, membership published, the SFU
        // connected — and never fetched a single to-device message, which is
        // exactly the thing under test here.
        QVERIFY2(loggedIn.wait(60000),
                 restoring ? "session restore failed" : "login failed");
        qInfo() << (restoring ? "restored a saved session (no login spent)"
                              : "logged in");
        // Sync has to be RUNNING: membership publishing goes through the
        // SDK's joined-room lookup, and without a sync the SDK knows no
        // rooms at all — which surfaces only as "Couldn't announce you in
        // the call."
        client.startSync();
        qInfo() << "logged in; syncing";

        const QString room = env("LIGHTNING_LIVE_ROOM");
        // Let the first sync land so the room and its MatrixRTC membership
        // exist before discovery is asked anything.
        QTest::qWait(15000);

        // Both: refresh() reads the room's MatrixRTC MEMBERSHIP, discover()
        // runs TRANSPORT discovery. The app drives both from its room-change
        // path; a harness that calls only one waits forever on the other.
        rtc.discover(room);
        rtc.refresh(room);
        // Discovery is asynchronous; wait for it to settle either way.
        QTRY_VERIFY_WITH_TIMEOUT(!rtc.focusUrlFor(room).isEmpty()
                                     || rtc.joinBlockReason(room)
                                            != QLatin1String("undiscovered"),
                                 30000);
        qInfo() << "join block =" << rtc.joinBlockReason(room)
                << "focus known =" << !rtc.focusUrlFor(room).isEmpty();

        QSignalSpy failed(&group, &SfuCallController::callFailed);
        // Membership publishing needs the SDK to have IMPORTED the room, and
        // the first sync decides when that is. Retry rather than guess a
        // sleep: op=0 means the room was not joined yet, which is a timing
        // fact, not a failure.
        // LIGHTNING_LIVE_VIDEO=1 publishes a camera track too (a test
        // pattern in this mode), so the VP8 send path — payloader, frame
        // encryption, and the far end's ability to decrypt it — is exercised
        // and not merely assumed from the audio path working.
        const bool withVideo = env("LIGHTNING_LIVE_VIDEO") == QLatin1String("1");
        bool joined = false;
        for (int attempt = 0; attempt < 20 && !joined; ++attempt) {
            joined = group.join(room, withVideo);
            if (!joined)
                QTest::qWait(3000);
        }
        qInfo() << "join() returned" << joined << "lastError" << group.lastError();
        QVERIFY2(joined, qPrintable(group.lastError()));

        // LIGHTNING_LIVE_SCREEN=1 publishes a SCREEN SHARE (synthetic source in
        // this mode) so the source the SFU records — and whether a remote
        // client's `isScreenShareEnabled` is ever true — can be checked without
        // a desktop portal.
        //
        // BEFORE the hold, not after: started afterwards it exists only for the
        // last instant of the run, and anything sampling the SFU mid-hold sees
        // a call with no screen share at all and calls it a forwarding failure.
        //
        // And only once the call is CONNECTED. `join()` merely starts the
        // sequence — the media engine is created when the SFU join lands — so
        // publishing before that reaches `ensurePeer()` with an inactive
        // engine, which returns silently. The share then "starts" (true) and
        // puts no track on the wire at all.
        if (env("LIGHTNING_LIVE_SCREEN") == QLatin1String("1")) {
            QTRY_VERIFY_WITH_TIMEOUT(
                group.stateInt()
                    == static_cast<int>(SfuCallController::State::Connected),
                45000);
            const bool shared = group.startScreenShare(/*pipewireNodeId=*/0);
            qInfo() << "screen share started:" << shared;
        }

        // How long to stay in the call. 40s is generous for a solo probe
        // (LiveKit's own join timeout is 60s and an ICE failure shows up long
        // before that); a two-client run needs longer, because the second
        // client has to be started far enough apart to clear the
        // homeserver's per-IP login rate limit and still overlap.
        bool holdOk = false;
        const int hold = env("LIGHTNING_LIVE_HOLD_MS").toInt(&holdOk);
        QTest::qWait(holdOk && hold > 0 ? hold : 40000);
        qInfo() << "participants json:" << group.participants();

        // THE WHOLE CHAIN: attach a real sink to the remote participant's
        // video exactly as a tile does, and require FRAMES to arrive on it.
        //
        // Decrypting and rendering are different facts — the crypto counters
        // climb either way — and the gap between them is where a remote screen
        // share was lost: it arrived, decrypted, and nothing was watching the
        // key it was routed under.
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
            group.detachVideoSink(remote);
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
        // With a second client present, media must arrive AND decrypt. Zero
        // decrypted frames next to thousands sent is the exact signature of
        // every fault this harness was built to find.
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
