#include "smoke/RustSdkSmokeTest.h"

#ifdef ENABLE_RUST_SDK_BACKEND

#include "matrix/MatrixClient.h"
#include "matrix/RoomInfo.h"
#include "matrix/RustSdkMatrixClient.h"
#include "matrix/TimelineEvent.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QLatin1String>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QTimer>

#include <cstdlib>
#include <functional>

namespace {

QString envStr(const char *name)
{
    const char *v = std::getenv(name);
    return v ? QString::fromLocal8Bit(v) : QString();
}

void say(const QString &line)
{
    QTextStream out(stdout);
    out << "smoke: " << line << "\n";
    out.flush();
}

void warn(const QString &line)
{
    QTextStream err(stderr);
    err << "smoke: WARN " << line << "\n";
    err.flush();
}

// Trim Matrix SDK error strings for stdout. The SDK's default Display
// impl is generally token-free but can contain full JSON bodies on
// unexpected paths; keep the surface small and one-line.
QString sanitiseError(const QString &raw)
{
    QString clean = raw;
    clean.replace(QLatin1Char('\r'), QLatin1Char(' '));
    clean.replace(QLatin1Char('\n'), QLatin1Char(' '));
    if (clean.size() > 240)
        clean = clean.left(240) + QLatin1String("…");
    return clean;
}

const char *stateLabel(MatrixClient::ConnectionState s)
{
    switch (s) {
    case MatrixClient::Disconnected: return "disconnected";
    case MatrixClient::Connecting:   return "connecting";
    case MatrixClient::Syncing:      return "syncing";
    case MatrixClient::Error:        return "error";
    }
    return "?";
}

struct Counters {
    QString loginResult = QStringLiteral("n/a");
    QString syncResult  = QStringLiteral("n/a");
    QString sendResult  = QStringLiteral("n/a");
    int roomCount = 0;
    int encryptedRoomCount = 0;
    int spaceCount = 0;
    int timelineEventCount = 0;
    int undecryptableCount = 0;
    bool initialSyncDone = false;
    bool sendActive = false;
    bool finalised = false;
};

int exitCodeFor(const Counters &c, bool doSend)
{
    if (c.loginResult != QLatin1String("ok"))       return 10;
    if (c.syncResult  != QLatin1String("ok"))       return 11;
    if (c.roomCount   < 1)                          return 12;
    if (doSend && c.sendResult != QLatin1String("ok")) return 13;
    return 0;
}

} // namespace

int runRustSdkSmokeTest(int argc, char *argv[])
{
    const QString homeserver     = envStr("LIGHTNING_TEST_HOMESERVER");
    const QString user           = envStr("LIGHTNING_TEST_USER");
    const QString password       = envStr("LIGHTNING_TEST_PASSWORD");
    const bool    doSend         = envStr("LIGHTNING_TEST_SEND") == QLatin1String("1");
    const QString explicitRoomId = envStr("LIGHTNING_TEST_ROOM_ID");

    if (homeserver.isEmpty() || user.isEmpty() || password.isEmpty()) {
        QTextStream(stderr) <<
            "matrix-client --rust-sdk-smoke-test: missing environment.\n"
            "Set LIGHTNING_TEST_HOMESERVER, LIGHTNING_TEST_USER, "
            "LIGHTNING_TEST_PASSWORD.\n"
            "Optional: LIGHTNING_TEST_SEND=1, LIGHTNING_TEST_ROOM_ID=<id>.\n"
            "See docs/build-and-test.md for details.\n";
        return 2;
    }

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("matrix-client-smoke"));
    QCoreApplication::setApplicationVersion(QLatin1String(APP_VERSION));

    // Passing nullptr for SettingsManager is deliberate: the smoke test
    // must NEVER overwrite the interactive user's real cached session
    // (access token, syncToken, homeserver). RustSdkMatrixClient guards
    // every m_settings->... call with a null check.
    RustSdkMatrixClient client(nullptr, &app);

    Counters counters;

    auto finalise = std::make_shared<std::function<void()>>();
    *finalise = [&]() {
        if (counters.finalised) return;
        counters.finalised = true;
        say(QStringLiteral(
                "summary login=%1 sync=%2 rooms=%3 encrypted_rooms=%4 "
                "spaces=%5 timeline_events=%6 undecryptable=%7 send=%8")
                .arg(counters.loginResult, counters.syncResult)
                .arg(counters.roomCount)
                .arg(counters.encryptedRoomCount)
                .arg(counters.spaceCount)
                .arg(counters.timelineEventCount)
                .arg(counters.undecryptableCount)
                .arg(counters.sendResult));
        QCoreApplication::exit(exitCodeFor(counters, doSend));
    };

    // Total time budget. If nothing else fires by 60s, wrap up with
    // whatever we have.
    QTimer::singleShot(60000, &app, [finalise]() {
        warn(QStringLiteral("60s budget exhausted"));
        (*finalise)();
    });

    QObject::connect(&client, &MatrixClient::loginSucceeded,
                     &app, [&, finalise](const QString &) {
        counters.loginResult = QStringLiteral("ok");
        say(QStringLiteral("login=ok"));
        client.startSync();
        QTimer::singleShot(30000, &app, [&, finalise]() {
            if (!counters.initialSyncDone) {
                warn(QStringLiteral("initial sync did not complete within 30s"));
                counters.syncResult = QStringLiteral("timeout");
                (*finalise)();
            }
        });
    });

    QObject::connect(&client, &MatrixClient::loginFailed,
                     &app, [&, finalise](const QString &reason) {
        counters.loginResult = QStringLiteral("failed");
        say(QStringLiteral("login=failed reason=%1").arg(sanitiseError(reason)));
        (*finalise)();
    });

    QObject::connect(&client, &MatrixClient::connectionStateChanged,
                     &app, [](MatrixClient::ConnectionState s) {
        say(QStringLiteral("state=%1").arg(QLatin1String(stateLabel(s))));
    });

    auto attemptSend = std::make_shared<std::function<void()>>();
    *attemptSend = [&, finalise]() {
        const auto rooms = client.rooms();
        QString target = explicitRoomId;
        const RoomInfo *targetRoom = nullptr;
        if (!target.isEmpty()) {
            for (const auto &r : rooms) {
                if (r.id == target) {
                    targetRoom = &r;
                    break;
                }
            }
            if (!targetRoom) {
                warn(QStringLiteral(
                    "LIGHTNING_TEST_ROOM_ID not in synced rooms"));
                (*finalise)();
                return;
            }
        } else {
            for (const auto &r : rooms) {
                if (r.isSpace || r.encrypted) continue;
                target = r.id;
                targetRoom = &r;
                break;
            }
            if (!targetRoom) {
                warn(QStringLiteral("no unencrypted room available for send"));
                (*finalise)();
                return;
            }
        }
        if (targetRoom->isSpace) {
            warn(QStringLiteral("selected target is a Space; refusing"));
            (*finalise)();
            return;
        }
        if (targetRoom->encrypted) {
            warn(QStringLiteral(
                "selected target is encrypted; refusing "
                "(smoke never sends into encrypted rooms)"));
            (*finalise)();
            return;
        }
        counters.sendActive = true;
        const QString body = QStringLiteral(
            "Lightning smoke-test %1")
            .arg(QDateTime::currentSecsSinceEpoch());
        say(QStringLiteral("send=start room=%1").arg(target));
        client.sendTextMessage(target, body);
        QTimer::singleShot(15000, &app, [&, finalise]() {
            if (counters.sendActive) {
                warn(QStringLiteral("send did not confirm within 15s"));
                counters.sendResult = QStringLiteral("timeout");
                counters.sendActive = false;
                (*finalise)();
            }
        });
    };

    QObject::connect(&client, &MatrixClient::initialSyncDoneChanged,
                     &app, [&, finalise, attemptSend]() {
        if (!client.initialSyncDone()) return;
        counters.initialSyncDone = true;
        counters.syncResult = QStringLiteral("ok");
        say(QStringLiteral("initial_sync=done"));
        QTimer::singleShot(2000, &app, [&, finalise, attemptSend]() {
            if (doSend) (*attemptSend)();
            else (*finalise)();
        });
    });

    QObject::connect(&client, &MatrixClient::roomsChanged,
                     &app, [&]() {
        const auto rooms = client.rooms();
        int joined = 0, encrypted = 0, spaces = 0;
        for (const auto &room : rooms) {
            if (room.isSpace) { ++spaces; continue; }
            ++joined;
            if (room.encrypted) ++encrypted;
        }
        counters.roomCount = joined;
        counters.encryptedRoomCount = encrypted;
        counters.spaceCount = spaces;
        say(QStringLiteral(
                "rooms joined=%1 encrypted=%2 spaces=%3")
                .arg(joined).arg(encrypted).arg(spaces));
    });

    QObject::connect(&client, &MatrixClient::eventAppended,
                     &app, [&](const QString &, const TimelineEvent &ev) {
        ++counters.timelineEventCount;
        // The Rust bridge routes undecryptable events with an empty body
        // that the C++ wrapper replaces with a placeholder marked
        // TimelineEvent::Notice. We count that shape without ever
        // touching decrypted message content.
        if (ev.type == TimelineEvent::Notice
            && ev.body.contains(QLatin1String("unable to decrypt"))) {
            ++counters.undecryptableCount;
        }
    });

    QObject::connect(&client, &MatrixClient::eventReplaced,
                     &app, [&, finalise](const QString &,
                                         const QString &,
                                         const TimelineEvent &ev) {
        if (!counters.sendActive) return;
        if (ev.status == TimelineEvent::Sent) {
            counters.sendResult = QStringLiteral("ok");
            counters.sendActive = false;
            say(QStringLiteral("send=ok"));
            (*finalise)();
        }
    });

    QObject::connect(&client, &MatrixClient::eventStatusChanged,
                     &app, [&, finalise](const QString &,
                                         const QString &,
                                         TimelineEvent::Status s) {
        if (!counters.sendActive) return;
        if (s == TimelineEvent::Sent) {
            counters.sendResult = QStringLiteral("ok");
            counters.sendActive = false;
            say(QStringLiteral("send=ok"));
            (*finalise)();
        } else if (s == TimelineEvent::Failed) {
            counters.sendResult = QStringLiteral("failed");
            counters.sendActive = false;
            say(QStringLiteral("send=failed"));
            (*finalise)();
        }
    });

    QObject::connect(&client, &MatrixClient::errorOccurred,
                     &app, [](const QString &err) {
        warn(sanitiseError(err));
    });

    say(QStringLiteral("start homeserver=%1").arg(homeserver));
    QTimer::singleShot(0, &app, [&]() {
        client.login(homeserver, user, password);
    });

    return QCoreApplication::exec();
}

#endif  // ENABLE_RUST_SDK_BACKEND
