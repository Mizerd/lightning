#include "smoke/RustSdkSmokeTest.h"

#ifdef ENABLE_RUST_SDK_BACKEND

#include "matrix/MatrixClient.h"
#include "matrix/RoomInfo.h"
#include "matrix/RustSdkMatrixClient.h"
#include "matrix/TimelineEvent.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLatin1String>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <cstdlib>
#include <functional>
#include <memory>

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
    QString sendReason;  // only populated when sendResult is skipped/blocked
    QString encryptedSendResult = QStringLiteral("n/a");
    QString encryptedSendReason;
    QString encryptedSendMarker;
    int roomCount = 0;
    int encryptedRoomCount = 0;
    int spaceCount = 0;
    int timelineEventCount = 0;
    int encryptedEventCount = 0;
    int decryptedEventCount = 0;
    int undecryptableCount = 0;
    bool initialSyncDone = false;
    bool sendActive = false;
    bool encryptedSendActive = false;
    bool finalised = false;

    // LIGHTNING_TEST_EXPECT_TEXT — the caller sends a known marker
    // from Element Classic into an encrypted room. We watch decrypted
    // event bodies and record only whether we saw the marker, never
    // the marker itself. Never printed.
    QString expectText;
    bool    requireExpect = false;
    QString expectResult = QStringLiteral("n/a");
};

// Exit-code policy:
//   10 login failed
//   11 sync did not complete
//   12 zero joined rooms
//   13 real send failed / timed out (send=skipped and send=blocked are NOT
//      failures — expected on all-encrypted accounts)
//   14 LIGHTNING_TEST_REQUIRE_EXPECT=1 was set and the marker was not seen
//   15 LIGHTNING_TEST_SEND_ENCRYPTED=1 was set and the probe failed / timed out
int exitCodeFor(const Counters &c)
{
    if (c.loginResult != QLatin1String("ok"))                       return 10;
    if (c.syncResult  != QLatin1String("ok"))                       return 11;
    if (c.roomCount   < 1)                                          return 12;
    const bool sendFinal = c.sendResult != QLatin1String("n/a");
    if (sendFinal
        && c.sendResult != QLatin1String("ok")
        && c.sendResult != QLatin1String("skipped")
        && c.sendResult != QLatin1String("blocked"))                return 13;
    if (c.requireExpect && c.expectResult != QLatin1String("seen")) return 14;
    const bool encFinal = c.encryptedSendResult != QLatin1String("n/a");
    if (encFinal
        && c.encryptedSendResult != QLatin1String("ok")
        && c.encryptedSendResult != QLatin1String("skipped"))       return 15;
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
    const bool    doSendEncrypted =
        envStr("LIGHTNING_TEST_SEND_ENCRYPTED") == QLatin1String("1");
    const QString expectText     = envStr("LIGHTNING_TEST_EXPECT_TEXT");
    const bool    requireExpect  =
        envStr("LIGHTNING_TEST_REQUIRE_EXPECT") == QLatin1String("1");

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

    // Fresh crypto store per run. QTemporaryDir defaults to autoRemove =
    // true and lives on the stack past the client so background SDK
    // threads writing to the store when we stop sync won't race the
    // cleanup destructor.
    QTemporaryDir tempStore(QDir::tempPath()
        + QStringLiteral("/lightning-rust-sdk-smoke-XXXXXX"));
    if (!tempStore.isValid()) {
        QTextStream(stderr) <<
            "matrix-client --rust-sdk-smoke-test: failed to create "
            "temporary Rust SDK store under "
            << QDir::tempPath() << ".\n";
        return 2;
    }
    const QString storePath = tempStore.path()
        + QStringLiteral("/matrix-rust-sdk-store");

    // No parent — we destroy the client explicitly before exec() returns
    // so tempStore is still valid when SDK background threads shut down.
    auto client = std::make_unique<RustSdkMatrixClient>(nullptr, nullptr);
    client->setStorePathOverride(storePath);

    say(QStringLiteral("store=temporary"));
    say(QStringLiteral("store_path=%1").arg(storePath));
    say(QStringLiteral("store_exists=%1")
            .arg(QFileInfo::exists(storePath)
                 ? QStringLiteral("yes")
                 : QStringLiteral("no")));
    say(QStringLiteral("supports_e2ee=%1")
            .arg(client->rustSupportsE2ee()
                 ? QStringLiteral("true")
                 : QStringLiteral("false")));

    Counters counters;
    counters.expectText = expectText;
    counters.requireExpect = requireExpect;
    if (!expectText.isEmpty()) {
        counters.expectResult = QStringLiteral("not_seen");
        // Deliberately not printing the expected text itself.
        say(QStringLiteral("expect_text=configured require=%1")
                .arg(requireExpect
                     ? QStringLiteral("true")
                     : QStringLiteral("false")));
    }

    auto finalise = std::make_shared<std::function<void()>>();
    *finalise = [&]() {
        if (counters.finalised) return;
        counters.finalised = true;
        const QString sendSummary = counters.sendReason.isEmpty()
            ? counters.sendResult
            : QStringLiteral("%1(%2)").arg(counters.sendResult, counters.sendReason);
        const QString encSendSummary = counters.encryptedSendReason.isEmpty()
            ? counters.encryptedSendResult
            : QStringLiteral("%1(%2)").arg(counters.encryptedSendResult,
                                           counters.encryptedSendReason);
        say(QStringLiteral(
                "summary login=%1 sync=%2 rooms=%3 encrypted_rooms=%4 "
                "spaces=%5 timeline_events=%6 encrypted_events=%7 "
                "decrypted_events=%8 undecryptable=%9 send=%10 "
                "encrypted_send=%11 expect_text=%12 supports_e2ee=%13")
                .arg(counters.loginResult, counters.syncResult)
                .arg(counters.roomCount)
                .arg(counters.encryptedRoomCount)
                .arg(counters.spaceCount)
                .arg(counters.timelineEventCount)
                .arg(counters.encryptedEventCount)
                .arg(counters.decryptedEventCount)
                .arg(counters.undecryptableCount)
                .arg(sendSummary)
                .arg(encSendSummary)
                .arg(counters.expectResult,
                     client->rustSupportsE2ee()
                     ? QStringLiteral("true")
                     : QStringLiteral("false")));
        QCoreApplication::exit(exitCodeFor(counters));
    };

    // Total time budget. If nothing else fires by 60s, wrap up with
    // whatever we have.
    QTimer::singleShot(60000, &app, [finalise]() {
        warn(QStringLiteral("60s budget exhausted"));
        (*finalise)();
    });

    QObject::connect(client.get(), &MatrixClient::loginSucceeded,
                     &app, [&, finalise](const QString &) {
        counters.loginResult = QStringLiteral("ok");
        say(QStringLiteral("login=ok"));
        client->startSync();
        QTimer::singleShot(30000, &app, [&, finalise]() {
            if (!counters.initialSyncDone) {
                warn(QStringLiteral("initial sync did not complete within 30s"));
                counters.syncResult = QStringLiteral("timeout");
                (*finalise)();
            }
        });
    });

    QObject::connect(client.get(), &MatrixClient::loginFailed,
                     &app, [&, finalise](const QString &reason) {
        counters.loginResult = QStringLiteral("failed");
        say(QStringLiteral("login=failed reason=%1").arg(sanitiseError(reason)));
        (*finalise)();
    });

    QObject::connect(client.get(), &MatrixClient::connectionStateChanged,
                     &app, [](MatrixClient::ConnectionState s) {
        say(QStringLiteral("state=%1").arg(QLatin1String(stateLabel(s))));
    });

    auto attemptSend = std::make_shared<std::function<void()>>();
    *attemptSend = [&, finalise]() {
        const auto rooms = client->rooms();
        QString target = explicitRoomId;
        const RoomInfo *targetRoom = nullptr;
        bool explicitTarget = !target.isEmpty();
        if (explicitTarget) {
            for (const auto &r : rooms) {
                if (r.id == target) {
                    targetRoom = &r;
                    break;
                }
            }
            if (!targetRoom) {
                counters.sendResult = QStringLiteral("blocked");
                counters.sendReason = QStringLiteral("target_not_in_synced_rooms");
                say(QStringLiteral(
                    "send=blocked reason=target_not_in_synced_rooms"));
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
                counters.sendResult = QStringLiteral("skipped");
                counters.sendReason = QStringLiteral("no_unencrypted_room");
                say(QStringLiteral(
                    "send=skipped reason=no_unencrypted_room"));
                (*finalise)();
                return;
            }
        }
        if (targetRoom->isSpace) {
            counters.sendResult = QStringLiteral("blocked");
            counters.sendReason = QStringLiteral("target_is_space");
            say(QStringLiteral("send=blocked reason=target_is_space"));
            (*finalise)();
            return;
        }
        if (targetRoom->encrypted) {
            counters.sendResult = QStringLiteral("blocked");
            counters.sendReason = QStringLiteral("encrypted_room_e2ee_disabled");
            say(QStringLiteral(
                "send=blocked reason=encrypted_room_e2ee_disabled"));
            (*finalise)();
            return;
        }
        counters.sendActive = true;
        const QString body = QStringLiteral("Lightning smoke-test %1")
            .arg(QDateTime::currentSecsSinceEpoch());
        say(QStringLiteral("send=start room=%1").arg(target));
        client->sendTextMessage(target, body);
        QTimer::singleShot(15000, &app, [&, finalise]() {
            if (counters.sendActive) {
                warn(QStringLiteral("send did not confirm within 15s"));
                counters.sendResult = QStringLiteral("timeout");
                counters.sendActive = false;
                (*finalise)();
            }
        });
    };

    auto attemptEncryptedProbe = std::make_shared<std::function<void()>>();
    *attemptEncryptedProbe = [&, finalise]() {
        const auto rooms = client->rooms();
        QString target = explicitRoomId;
        const RoomInfo *targetRoom = nullptr;
        if (!target.isEmpty()) {
            for (const auto &r : rooms) {
                if (r.id == target) { targetRoom = &r; break; }
            }
            if (!targetRoom || !targetRoom->encrypted || targetRoom->isSpace) {
                counters.encryptedSendResult = QStringLiteral("skipped");
                counters.encryptedSendReason = QStringLiteral("target_not_encrypted_room");
                say(QStringLiteral(
                    "encrypted_send=skipped reason=target_not_encrypted_room"));
                (*finalise)();
                return;
            }
        } else {
            for (const auto &r : rooms) {
                if (r.isSpace || !r.encrypted) continue;
                target = r.id;
                targetRoom = &r;
                break;
            }
            if (!targetRoom) {
                counters.encryptedSendResult = QStringLiteral("skipped");
                counters.encryptedSendReason = QStringLiteral("no_encrypted_room");
                say(QStringLiteral(
                    "encrypted_send=skipped reason=no_encrypted_room"));
                (*finalise)();
                return;
            }
        }
        counters.encryptedSendActive = true;
        counters.encryptedSendMarker = QStringLiteral("SMK-%1")
            .arg(QDateTime::currentSecsSinceEpoch());
        // The body embeds the marker but the marker is what we log.
        // Body content is not printed by the harness.
        const QString body = QStringLiteral(
            "Lightning encrypted-send probe %1")
            .arg(counters.encryptedSendMarker);
        say(QStringLiteral("encrypted_send=start room=%1 marker=%2")
                .arg(target, counters.encryptedSendMarker));
        client->probeEncryptedSend(target, body, counters.encryptedSendMarker);
        QTimer::singleShot(30000, &app, [&, finalise]() {
            if (counters.encryptedSendActive) {
                warn(QStringLiteral(
                    "encrypted-send probe did not confirm within 30s"));
                counters.encryptedSendResult = QStringLiteral("timeout");
                counters.encryptedSendActive = false;
                (*finalise)();
            }
        });
    };

    QObject::connect(client.get(),
                     &RustSdkMatrixClient::encryptedSendProbeResult, &app,
                     [&, finalise](const QString &,
                                   const QString &marker,
                                   bool ok,
                                   const QString &serverEventId,
                                   const QString &message) {
        if (!counters.encryptedSendActive) return;
        counters.encryptedSendActive = false;
        if (marker != counters.encryptedSendMarker) return;
        if (ok) {
            counters.encryptedSendResult = QStringLiteral("ok");
            say(QStringLiteral(
                    "encrypted_send=ok marker=%1 event_id=%2")
                    .arg(marker, serverEventId));
        } else {
            counters.encryptedSendResult = QStringLiteral("failed");
            counters.encryptedSendReason = sanitiseError(message);
            say(QStringLiteral(
                    "encrypted_send=failed marker=%1 reason=%2")
                    .arg(marker, counters.encryptedSendReason));
        }
        (*finalise)();
    });

    QObject::connect(client.get(), &MatrixClient::initialSyncDoneChanged,
                     &app, [&, finalise, attemptSend,
                            attemptEncryptedProbe]() {
        if (!client->initialSyncDone()) return;
        counters.initialSyncDone = true;
        counters.syncResult = QStringLiteral("ok");
        say(QStringLiteral("initial_sync=done"));
        QTimer::singleShot(2000, &app, [&, finalise, attemptSend,
                                        attemptEncryptedProbe]() {
            // Order: EXPECT_TEXT is passive (event handler already
            // watches). SEND runs before SEND_ENCRYPTED so a fully-set
            // caller drives both without either blocking the other.
            if (doSendEncrypted) (*attemptEncryptedProbe)();
            else if (doSend)     (*attemptSend)();
            else                 (*finalise)();
        });
    });

    QObject::connect(client.get(), &MatrixClient::roomsChanged,
                     &app, [&]() {
        const auto rooms = client->rooms();
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

    QObject::connect(client.get(), &MatrixClient::eventAppended,
                     &app, [&](const QString &, const TimelineEvent &ev) {
        ++counters.timelineEventCount;
        if (ev.isEncrypted)   ++counters.encryptedEventCount;
        if (ev.isDecrypted)   ++counters.decryptedEventCount;
        if (ev.undecryptable) ++counters.undecryptableCount;

        // EXPECT_TEXT match runs against decrypted bodies only. We never
        // echo the marker itself; only "seen" / "not_seen" is exposed.
        if (!counters.expectText.isEmpty()
            && ev.isDecrypted && !ev.undecryptable
            && !ev.body.isEmpty()
            && ev.body.contains(counters.expectText)) {
            if (counters.expectResult != QLatin1String("seen")) {
                counters.expectResult = QStringLiteral("seen");
                say(QStringLiteral("expect_text=seen"));
            }
        }
    });

    QObject::connect(client.get(), &MatrixClient::eventReplaced,
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

    QObject::connect(client.get(), &MatrixClient::eventStatusChanged,
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

    QObject::connect(client.get(), &MatrixClient::errorOccurred,
                     &app, [](const QString &err) {
        warn(sanitiseError(err));
    });

    say(QStringLiteral("start homeserver=%1").arg(homeserver));
    QTimer::singleShot(0, &app, [&]() {
        client->login(homeserver, user, password);
    });

    const int rc = QCoreApplication::exec();

    // Give the SDK background sync callback a chance to notice
    // sync_stop between stopSync() and the crypto store being deleted.
    // The Rust destructor also aborts sync, but async threads race
    // with QTemporaryDir::remove() otherwise.
    client->stopSync();
    QThread::msleep(200);
    client.reset();

    return rc;
}

#endif  // ENABLE_RUST_SDK_BACKEND
