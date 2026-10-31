// One end of a real legacy 1:1 call, driven line by line on stdin, for the
// scenario scripts behind docs/live-validation.md: a real homeserver, the real
// Rust SDK and E2EE, the real CallController and the real engine in test-tone
// mode (a sine and a moving pattern, so no devices). It prints one JSON line a
// second, with what actually arrived, so a script can tell a call that looks
// connected from one that carries media.
//
// Built only with -DLIGHTNING_BUILD_LEGACY_CALL_PEER=ON. Never run by CTest.
//
//   LIGHTNING_LIVE_HOMESERVER=http://127.0.0.1:18008
//   LIGHTNING_LIVE_USER=@someone:…   LIGHTNING_LIVE_PASSWORD=…
//   LIGHTNING_LIVE_ROOM=!room:…      LIGHTNING_LIVE_STORE=/scratch/dir
//
// Commands: room <id>, call voice|video, answer [audio], decline, hangup, mute 0|1,
// deafen 0|1, camera 0|1, quit. The password is never logged.
#include "app/SettingsManager.h"
#include "calls/CallController.h"
#include "calls/GstCallMediaBackend.h"
#include "matrix/RustSdkMatrixClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSocketNotifier>
#include <QTimer>

#include <cstdio>
#include <unistd.h>

namespace {
QString env(const char *name)
{
    return QString::fromLocal8Bit(qgetenv(name));
}
} // namespace

int main(int argc, char **argv)
{
    const QString store = env("LIGHTNING_LIVE_STORE");
    if (store.isEmpty() || env("LIGHTNING_LIVE_ROOM").isEmpty())
        return 2;
    QDir().mkpath(store);
    qputenv("XDG_DATA_HOME", store.toUtf8());
    qputenv("XDG_CONFIG_HOME", store.toUtf8());
    QGuiApplication app(argc, argv);

    SettingsManager settings;
    RustSdkMatrixClient client(&settings);
    CallController calls;
    // Initialises GStreamer, as AppController's probe does.
    if (!GstCallMediaBackend::runtimeAvailable())
        return 3;
    GstCallMediaBackend engine;
    engine.setTestToneMode(true);
    calls.setClient(&client);
    calls.setMediaBackend(&engine);

    QString room = env("LIGHTNING_LIVE_ROOM");
    QString peer = env("LIGHTNING_LIVE_PEER");

    const auto report = [&](const QString &event) {
        QJsonObject line;
        line.insert(QStringLiteral("event"), event);
        line.insert(QStringLiteral("state"), calls.stateInt());
        line.insert(QStringLiteral("end"),
                    static_cast<int>(calls.endReason()));
        line.insert(QStringLiteral("video"), calls.videoCall());
        line.insert(QStringLiteral("camera"), calls.cameraOn());
        line.insert(QStringLiteral("remoteVisible"),
                    calls.remoteVideoVisible());
        line.insert(QStringLiteral("remoteMicMuted"),
                    calls.remoteMicrophoneMuted());
        line.insert(QStringLiteral("audioPackets"),
                    engine.receivedAudioPacketsForTest());
        line.insert(QStringLiteral("peak"), engine.receivedPeakForTest());
        line.insert(QStringLiteral("videoFrames"),
                    engine.decodedVideoFramesForTest());
        line.insert(QStringLiteral("sentFrames"),
                    engine.sentVideoFramesForTest());
        line.insert(QStringLiteral("refusal"), calls.lastRefusal());
        std::fprintf(stdout, "%s\n",
                     QJsonDocument(line).toJson(QJsonDocument::Compact)
                         .constData());
        std::fflush(stdout);
    };

    QObject::connect(&calls, &CallController::stateChanged, &app,
                     [&] { report(QStringLiteral("state")); });
    QObject::connect(&calls, &CallController::videoStateChanged, &app,
                     [&] { report(QStringLiteral("video")); });
    QObject::connect(&client, &MatrixClient::loginSucceeded, &app, [&] {
        client.startSync();
        // The app lowers the backlog gate once sync is live.
        QTimer::singleShot(8000, &app, [&] {
            calls.setBacklogSuppressed(false);
            report(QStringLiteral("ready"));
        });
    });
    if (!client.restoreSession()) {
        client.login(env("LIGHTNING_LIVE_HOMESERVER"),
                     env("LIGHTNING_LIVE_USER"),
                     env("LIGHTNING_LIVE_PASSWORD"));
    }

    QTimer ticker;
    ticker.setInterval(1000);
    QObject::connect(&ticker, &QTimer::timeout, &app,
                     [&] { report(QStringLiteral("tick")); });
    ticker.start();

    QByteArray pending;
    QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&input, &QSocketNotifier::activated, &app, [&] {
        char chunk[256];
        const ssize_t got = ::read(STDIN_FILENO, chunk, sizeof chunk);
        if (got <= 0) {
            QCoreApplication::quit();
            return;
        }
        pending.append(chunk, got);
        int end = 0;
        while ((end = pending.indexOf('\n')) >= 0) {
            const QStringList words = QString::fromUtf8(pending.left(end))
                                          .split(QLatin1Char(' '),
                                                 Qt::SkipEmptyParts);
            pending.remove(0, end + 1);
            if (words.isEmpty())
                continue;
            const QString command = words.first();
            const QString arg = words.value(1);
            bool ok = true;
            if (command == QLatin1String("room"))
                room = arg;
            else if (command == QLatin1String("call"))
                ok = calls.placeCall(room, peer, arg == QLatin1String("video"));
            else if (command == QLatin1String("answer"))
                ok = calls.answer(arg != QLatin1String("audio"));
            else if (command == QLatin1String("decline"))
                ok = calls.rejectIncoming();
            else if (command == QLatin1String("hangup"))
                ok = calls.hangup();
            else if (command == QLatin1String("mute"))
                calls.setMicrophoneMuted(arg == QLatin1String("1"));
            else if (command == QLatin1String("deafen"))
                calls.setDeafened(arg == QLatin1String("1"));
            else if (command == QLatin1String("camera"))
                ok = calls.setCameraOn(arg == QLatin1String("1"));
            else if (command == QLatin1String("quit"))
                QCoreApplication::quit();
            report(command + (ok ? QStringLiteral(":ok")
                                 : QStringLiteral(":refused")));
        }
    });
    return app.exec();
}
