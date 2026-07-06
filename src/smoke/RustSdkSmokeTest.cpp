#include "smoke/RustSdkSmokeTest.h"

#ifdef ENABLE_RUST_SDK_BACKEND

#include "matrix/MatrixClient.h"
#include "matrix/RoomInfo.h"
#include "matrix/RustSdkMatrixClient.h"
#include "matrix/TimelineEvent.h"
#include "storage/AppDataPaths.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLatin1String>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUrl>

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

QString sanitizeHomeserver(QString homeserver)
{
    homeserver = homeserver.trimmed();
    while (homeserver.endsWith(QLatin1Char('/')))
        homeserver.chop(1);
    return homeserver;
}

QString userIdForLoginStore(const QString &homeserver, const QString &user)
{
    const QString trimmed = user.trimmed();
    if (trimmed.startsWith(QLatin1Char('@')))
        return trimmed;

    const QString host = QUrl(homeserver).host();
    if (!host.isEmpty())
        return QStringLiteral("@%1:%2").arg(trimmed, host);
    return trimmed;
}

QString redactedDeviceId(const QString &deviceId)
{
    const QString id = deviceId.trimmed();
    if (id.isEmpty())
        return QStringLiteral("unknown");
    if (id.size() <= 8)
        return id;
    return id.left(4) + QLatin1String("...") + id.right(4);
}

bool isAccountDeviceMismatch(const QString &reason)
{
    return reason.contains(
        QLatin1String("account in the store doesn't match the account in the constructor"),
        Qt::CaseInsensitive);
}

struct StoredSessionMeta {
    bool exists = false;
    bool valid = false;
    QString homeserver;
    QString userId;
    QString deviceId;
};

StoredSessionMeta readStoredSessionMeta(const QString &path)
{
    StoredSessionMeta meta;
    QFile f(path);
    if (!f.exists())
        return meta;
    meta.exists = true;
    if (!f.open(QIODevice::ReadOnly))
        return meta;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        return meta;
    const QJsonObject root = doc.object();
    const QJsonObject session = root.value(QStringLiteral("session")).toObject();
    meta.homeserver = root.value(QStringLiteral("homeserver")).toString();
    meta.userId = session.value(QStringLiteral("user_id")).toString();
    meta.deviceId = session.value(QStringLiteral("device_id")).toString();
    meta.valid = !meta.userId.isEmpty() && !meta.deviceId.isEmpty();
    return meta;
}

struct Counters {
    QString loginResult = QStringLiteral("n/a");
    QString syncResult  = QStringLiteral("n/a");
    QString restoreResult = QStringLiteral("n/a");
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
    int     expectWaitSeconds = 90;

    // v0.5.0-prep+7: wait loop after send/probe completes.
    bool    waitingForExpect = false;
    bool    firstTimelineAfterExpect = false;
    int     encryptedEventsSinceExpect = 0;
    int     decryptedEventsSinceExpect = 0;
    int     undecryptableSinceExpect = 0;

    // Phase A — key backup recovery via matrix-sdk.
    QString keyBackupResult = QStringLiteral("n/a");
    QString keyBackupReason;
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
    const bool    persistentStore =
        envStr("LIGHTNING_TEST_PERSISTENT_STORE") == QLatin1String("1");
    const bool    doSend         = envStr("LIGHTNING_TEST_SEND") == QLatin1String("1");
    const QString explicitRoomId = envStr("LIGHTNING_TEST_ROOM_ID");
    const bool    doSendEncrypted =
        envStr("LIGHTNING_TEST_SEND_ENCRYPTED") == QLatin1String("1");
    const QString expectText     = envStr("LIGHTNING_TEST_EXPECT_TEXT");
    const bool    requireExpect  =
        envStr("LIGHTNING_TEST_REQUIRE_EXPECT") == QLatin1String("1");
    const QString recoveryKey    = envStr("LIGHTNING_TEST_RECOVERY_KEY");
    const QString recoveryPass   = envStr("LIGHTNING_TEST_RECOVERY_PASSPHRASE");
    int           expectWaitSeconds = 90;
    {
        const QString raw = envStr("LIGHTNING_TEST_EXPECT_WAIT_SECONDS");
        if (!raw.isEmpty()) {
            bool ok = false;
            const int v = raw.toInt(&ok);
            if (ok && v > 0 && v <= 3600) expectWaitSeconds = v;
        }
    }

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

    const QString canonicalUserId =
        userIdForLoginStore(sanitizeHomeserver(homeserver), user);

    std::unique_ptr<QTemporaryDir> tempStore;
    QString storePath;
    QString sessionPath;
    StoredSessionMeta storedSession;
    QString storeAccountMatch = QStringLiteral("unknown");
    bool restoreAvailable = false;

    if (persistentStore) {
        storePath = matrix::app_data::rustSdkStorePath(canonicalUserId);
        sessionPath = matrix::app_data::rustSdkSmokeSessionPath(canonicalUserId);
        if (storePath.isEmpty() || sessionPath.isEmpty()) {
            QTextStream(stderr) <<
                "matrix-client --rust-sdk-smoke-test: no app data root "
                "available for persistent Rust SDK store "
                "(neither $HOME nor $XDG_DATA_HOME is set).\n";
            return 2;
        }

        storedSession = readStoredSessionMeta(sessionPath);
        if (storedSession.exists && storedSession.valid) {
            const bool accountMatches =
                storedSession.userId == canonicalUserId
                && sanitizeHomeserver(storedSession.homeserver)
                   == sanitizeHomeserver(homeserver);
            storeAccountMatch = accountMatches
                ? QStringLiteral("yes")
                : QStringLiteral("no");
            restoreAvailable = accountMatches;
        }
    } else {
        // Fresh crypto store per run. QTemporaryDir defaults to autoRemove =
        // true and lives longer than the client, so SDK background threads
        // can stop before the directory cleanup runs.
        tempStore = std::make_unique<QTemporaryDir>(
            QDir::tempPath()
            + QStringLiteral("/lightning-rust-sdk-smoke-XXXXXX"));
        if (!tempStore->isValid()) {
            QTextStream(stderr) <<
                "matrix-client --rust-sdk-smoke-test: failed to create "
                "temporary Rust SDK store under "
                << QDir::tempPath() << ".\n";
            return 2;
        }
        storePath = tempStore->path()
            + QStringLiteral("/matrix-rust-sdk-store");
    }

    // No parent — we destroy the client explicitly before exec() returns
    // so tempStore is still valid when SDK background threads shut down.
    auto client = std::make_unique<RustSdkMatrixClient>(nullptr, nullptr);
    if (persistentStore)
        client->setPersistentSessionFile(sessionPath);
    else
        client->setStorePathOverride(storePath);

    say(QStringLiteral("store=%1")
            .arg(persistentStore
                 ? QStringLiteral("persistent")
                 : QStringLiteral("temporary")));
    say(QStringLiteral("store_path=%1").arg(storePath));
    say(QStringLiteral("store_exists=%1")
            .arg(QFileInfo::exists(storePath)
                 ? QStringLiteral("yes")
                 : QStringLiteral("no")));
    say(QStringLiteral("store_account_match=%1").arg(storeAccountMatch));
    if (persistentStore && storedSession.exists && storedSession.valid) {
        say(QStringLiteral("device_id=%1")
                .arg(redactedDeviceId(storedSession.deviceId)));
    } else {
        say(QStringLiteral("device_id=unknown"));
    }
    say(QStringLiteral("supports_e2ee=%1")
            .arg(client->rustSupportsE2ee()
                 ? QStringLiteral("true")
                 : QStringLiteral("false")));

    Counters counters;
    counters.expectText = expectText;
    counters.requireExpect = requireExpect;
    counters.expectWaitSeconds = expectWaitSeconds;
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
        const QString kbSummary = counters.keyBackupReason.isEmpty()
            ? counters.keyBackupResult
            : QStringLiteral("%1(%2)").arg(counters.keyBackupResult,
                                           counters.keyBackupReason);
        say(QStringLiteral(
                "summary restore=%1 login=%2 sync=%3 rooms=%4 encrypted_rooms=%5 "
                "spaces=%6 timeline_events=%7 encrypted_events=%8 "
                "decrypted_events=%9 undecryptable=%10 send=%11 "
                "encrypted_send=%12 expect_text=%13 key_backup=%14 "
                "encrypted_events_since_expect=%15 "
                "decrypted_events_since_expect=%16 "
                "undecryptable_since_expect=%17 "
                "first_timeline_after_expect=%18 "
                "supports_e2ee=%19")
                .arg(counters.restoreResult, counters.loginResult)
                .arg(counters.syncResult)
                .arg(counters.roomCount)
                .arg(counters.encryptedRoomCount)
                .arg(counters.spaceCount)
                .arg(counters.timelineEventCount)
                .arg(counters.encryptedEventCount)
                .arg(counters.decryptedEventCount)
                .arg(counters.undecryptableCount)
                .arg(sendSummary)
                .arg(encSendSummary)
                .arg(counters.expectResult, kbSummary)
                .arg(counters.encryptedEventsSinceExpect)
                .arg(counters.decryptedEventsSinceExpect)
                .arg(counters.undecryptableSinceExpect)
                .arg(counters.firstTimelineAfterExpect
                     ? QStringLiteral("yes")
                     : QStringLiteral("no"),
                     client->rustSupportsE2ee()
                     ? QStringLiteral("true")
                     : QStringLiteral("false")));
        QCoreApplication::exit(exitCodeFor(counters));
    };

    // v0.5.0-prep+7. Post-send finalise: if EXPECT_TEXT is configured
    // and we haven't seen the marker yet, enter a bounded wait phase
    // instead of exiting immediately. Any decrypted event containing
    // the marker cancels the wait and finalises early.
    auto postSendFinalise = std::make_shared<std::function<void()>>();
    *postSendFinalise = [&, finalise]() {
        if (counters.finalised) return;
        if (counters.expectText.isEmpty()
            || counters.expectResult == QLatin1String("seen")) {
            (*finalise)();
            return;
        }
        if (counters.waitingForExpect) return;
        counters.waitingForExpect = true;
        counters.encryptedEventsSinceExpect = 0;
        counters.decryptedEventsSinceExpect = 0;
        counters.undecryptableSinceExpect = 0;
        counters.firstTimelineAfterExpect = false;
        say(QStringLiteral("expect_text=waiting timeout_s=%1")
                .arg(counters.expectWaitSeconds));
        QTimer::singleShot(counters.expectWaitSeconds * 1000, &app,
                           [&, finalise]() {
            if (!counters.waitingForExpect) return;
            counters.waitingForExpect = false;
            if (counters.expectResult != QLatin1String("seen")) {
                counters.expectResult = QStringLiteral("not_seen");
                say(QStringLiteral(
                    "expect_text=not_seen reason=timeout"));
            }
            (*finalise)();
        });
    };

    // Total time budget. If nothing else fires by 60s, wrap up with
    // whatever we have.
    QTimer::singleShot(60000, &app, [finalise]() {
        warn(QStringLiteral("60s budget exhausted"));
        (*finalise)();
    });

    enum class AuthAttempt { None, Restore, Login };
    AuthAttempt authAttempt = AuthAttempt::None;
    bool resetRetried = false;

    auto startPasswordLogin = std::make_shared<std::function<void()>>();
    *startPasswordLogin = [&]() {
        authAttempt = AuthAttempt::Login;
        client->login(homeserver, user, password);
    };

    auto resetStoreAndRetry = std::make_shared<std::function<void(const QString &)>>();
    *resetStoreAndRetry = [&, finalise, startPasswordLogin](const QString &reason) {
        resetRetried = true;
        if (authAttempt == AuthAttempt::Restore) {
            counters.restoreResult = QStringLiteral("failed");
            say(QStringLiteral("restore=failed reason=%1").arg(sanitiseError(reason)));
        }
        say(QStringLiteral("store_reset=account_device_mismatch"));
        if (!client->resetRustStore()) {
            counters.loginResult = QStringLiteral("failed");
            say(QStringLiteral("login=failed reason=%1")
                    .arg(QStringLiteral("failed_to_reset_rust_sdk_store")));
            (*finalise)();
            return;
        }
        if (!sessionPath.isEmpty())
            QFile::remove(sessionPath);
        say(QStringLiteral("store_exists=no"));
        (*startPasswordLogin)();
    };

    QObject::connect(client.get(), &MatrixClient::loginSucceeded,
                     &app, [&, finalise](const QString &) {
        if (authAttempt == AuthAttempt::Restore) {
            counters.restoreResult = QStringLiteral("ok");
            say(QStringLiteral("restore=ok"));
        }
        counters.loginResult = QStringLiteral("ok");
        say(QStringLiteral("login=ok"));
        say(QStringLiteral("device_id=%1")
                .arg(redactedDeviceId(client->currentDeviceId())));
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
        if (persistentStore && !resetRetried && isAccountDeviceMismatch(reason)) {
            (*resetStoreAndRetry)(reason);
            return;
        }
        if (authAttempt == AuthAttempt::Restore) {
            counters.restoreResult = QStringLiteral("failed");
            say(QStringLiteral("restore=failed reason=%1").arg(sanitiseError(reason)));
        }
        counters.loginResult = QStringLiteral("failed");
        say(QStringLiteral("login=failed reason=%1").arg(sanitiseError(reason)));
        (*finalise)();
    });

    QObject::connect(client.get(), &MatrixClient::connectionStateChanged,
                     &app, [](MatrixClient::ConnectionState s) {
        say(QStringLiteral("state=%1").arg(QLatin1String(stateLabel(s))));
    });

    auto attemptSend = std::make_shared<std::function<void()>>();
    *attemptSend = [&, postSendFinalise]() {
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
                (*postSendFinalise)();
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
                (*postSendFinalise)();
                return;
            }
        }
        if (targetRoom->isSpace) {
            counters.sendResult = QStringLiteral("blocked");
            counters.sendReason = QStringLiteral("target_is_space");
            say(QStringLiteral("send=blocked reason=target_is_space"));
            (*postSendFinalise)();
            return;
        }
        if (targetRoom->encrypted) {
            counters.sendResult = QStringLiteral("blocked");
            counters.sendReason = QStringLiteral("encrypted_room_e2ee_disabled");
            say(QStringLiteral(
                "send=blocked reason=encrypted_room_e2ee_disabled"));
            (*postSendFinalise)();
            return;
        }
        counters.sendActive = true;
        const QString body = QStringLiteral("Lightning smoke-test %1")
            .arg(QDateTime::currentSecsSinceEpoch());
        say(QStringLiteral("send=start room=%1").arg(target));
        client->sendTextMessage(target, body);
        QTimer::singleShot(15000, &app, [&, postSendFinalise]() {
            if (counters.sendActive) {
                warn(QStringLiteral("send did not confirm within 15s"));
                counters.sendResult = QStringLiteral("timeout");
                counters.sendActive = false;
                (*postSendFinalise)();
            }
        });
    };

    auto attemptEncryptedProbe = std::make_shared<std::function<void()>>();
    *attemptEncryptedProbe = [&, postSendFinalise]() {
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
                (*postSendFinalise)();
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
                (*postSendFinalise)();
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
        QTimer::singleShot(30000, &app, [&, postSendFinalise]() {
            if (counters.encryptedSendActive) {
                warn(QStringLiteral(
                    "encrypted-send probe did not confirm within 30s"));
                counters.encryptedSendResult = QStringLiteral("timeout");
                counters.encryptedSendActive = false;
                (*postSendFinalise)();
            }
        });
    };

    QObject::connect(client.get(),
                     &RustSdkMatrixClient::encryptedSendProbeResult, &app,
                     [&, postSendFinalise](const QString &,
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
        (*postSendFinalise)();
    });

    // Phase A — key backup recovery. Fires the moment login-then-sync
    // stabilises (see initial_sync=done chain below). Never prints the
    // recovery key or the imported key material.
    QObject::connect(client.get(),
                     &RustSdkMatrixClient::keyBackupResult, &app,
                     [&](const QString &state, const QString &message) {
        counters.keyBackupResult = state;
        if (state == QLatin1String("failed") && !message.isEmpty())
            counters.keyBackupReason = sanitiseError(message);
        say(QStringLiteral("key_backup=%1%2")
                .arg(state,
                     (state == QLatin1String("failed") && !message.isEmpty())
                         ? QStringLiteral(" reason=%1")
                               .arg(sanitiseError(message))
                         : QString()));
    });

    QObject::connect(client.get(), &MatrixClient::initialSyncDoneChanged,
                     &app, [&, postSendFinalise, attemptSend,
                            attemptEncryptedProbe, recoveryKey,
                            recoveryPass, persistentStore]() {
        if (!client->initialSyncDone()) return;
        counters.initialSyncDone = true;
        counters.syncResult = QStringLiteral("ok");
        say(QStringLiteral("initial_sync=done"));

        // Phase A — kick off key backup recovery if a key was supplied
        // via env. Runs in parallel with any send/probe; results arrive
        // asynchronously on keyBackupResult.
        if (!recoveryKey.isEmpty()) {
            if (!persistentStore) {
                counters.keyBackupResult = QStringLiteral("failed");
                counters.keyBackupReason =
                    QStringLiteral("persistent_store_required");
                say(QStringLiteral(
                    "key_backup=failed reason=persistent_store_required"));
            } else {
                counters.keyBackupResult = QStringLiteral("attempted");
                say(QStringLiteral("key_backup=attempted"));
                client->recoverFromBackup(recoveryKey);
            }
        } else if (!recoveryPass.isEmpty()) {
            counters.keyBackupResult = QStringLiteral("failed");
            counters.keyBackupReason =
                QStringLiteral("passphrase_not_supported");
            say(QStringLiteral(
                "key_backup=failed reason=passphrase_not_supported"));
        } else {
            counters.keyBackupResult = QStringLiteral("not_configured");
        }

        QTimer::singleShot(2000, &app, [&, postSendFinalise, attemptSend,
                                        attemptEncryptedProbe]() {
            // Order: SEND / SEND_ENCRYPTED run first. postSendFinalise
            // then either enters the EXPECT_TEXT wait phase or exits.
            if (doSendEncrypted) (*attemptEncryptedProbe)();
            else if (doSend)     (*attemptSend)();
            else                 (*postSendFinalise)();
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
                     &app, [&, finalise](const QString &, const TimelineEvent &ev) {
        ++counters.timelineEventCount;
        if (ev.isEncrypted)   ++counters.encryptedEventCount;
        if (ev.isDecrypted)   ++counters.decryptedEventCount;
        if (ev.undecryptable) ++counters.undecryptableCount;

        if (counters.waitingForExpect) {
            if (!counters.firstTimelineAfterExpect)
                counters.firstTimelineAfterExpect = true;
            if (ev.isEncrypted)   ++counters.encryptedEventsSinceExpect;
            if (ev.isDecrypted)   ++counters.decryptedEventsSinceExpect;
            if (ev.undecryptable) ++counters.undecryptableSinceExpect;
        }

        // EXPECT_TEXT match runs against decrypted bodies only. We never
        // echo the marker itself; only "seen" / "not_seen" is exposed.
        if (!counters.expectText.isEmpty()
            && ev.isDecrypted && !ev.undecryptable
            && !ev.body.isEmpty()
            && ev.body.contains(counters.expectText)) {
            if (counters.expectResult != QLatin1String("seen")) {
                counters.expectResult = QStringLiteral("seen");
                say(QStringLiteral("expect_text=seen"));
                if (counters.waitingForExpect) {
                    counters.waitingForExpect = false;
                    (*finalise)();
                }
            }
        }
    });

    QObject::connect(client.get(), &MatrixClient::eventReplaced,
                     &app, [&, postSendFinalise](const QString &,
                                         const QString &,
                                         const TimelineEvent &ev) {
        if (!counters.sendActive) return;
        if (ev.status == TimelineEvent::Sent) {
            counters.sendResult = QStringLiteral("ok");
            counters.sendActive = false;
            say(QStringLiteral("send=ok"));
            (*postSendFinalise)();
        }
    });

    QObject::connect(client.get(), &MatrixClient::eventStatusChanged,
                     &app, [&, postSendFinalise](const QString &,
                                         const QString &,
                                         TimelineEvent::Status s) {
        if (!counters.sendActive) return;
        if (s == TimelineEvent::Sent) {
            counters.sendResult = QStringLiteral("ok");
            counters.sendActive = false;
            say(QStringLiteral("send=ok"));
            (*postSendFinalise)();
        } else if (s == TimelineEvent::Failed) {
            counters.sendResult = QStringLiteral("failed");
            counters.sendActive = false;
            say(QStringLiteral("send=failed"));
            (*postSendFinalise)();
        }
    });

    QObject::connect(client.get(), &MatrixClient::errorOccurred,
                     &app, [](const QString &err) {
        warn(sanitiseError(err));
    });

    say(QStringLiteral("start homeserver=%1").arg(homeserver));
    QTimer::singleShot(0, &app, [&, startPasswordLogin]() {
        if (persistentStore && restoreAvailable) {
            authAttempt = AuthAttempt::Restore;
            counters.restoreResult = QStringLiteral("attempted");
            say(QStringLiteral("restore=attempted"));
            if (!client->restoreSessionFromFile(homeserver, canonicalUserId)) {
                counters.restoreResult = QStringLiteral("failed");
                say(QStringLiteral("restore=failed reason=%1")
                        .arg(QStringLiteral("restore_start_failed")));
                (*startPasswordLogin)();
            }
            return;
        }

        counters.restoreResult = persistentStore
            ? QStringLiteral("not_available")
            : QStringLiteral("skipped");
        say(QStringLiteral("restore=%1").arg(counters.restoreResult));
        (*startPasswordLogin)();
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
