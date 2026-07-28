#include "matrix/RustSdkMatrixClient.h"

#include "app/SettingsManager.h"
#include "crypto/E2eeDiagnostics.h"
#include "matrix/EventPreview.h"
#include "matrix/MediaHelpers.h"
#include "matrix/RustSessionPolicy.h"
#include "matrix_rust.h"
#include "models/UserLookup.h"
#include "storage/AppDataPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantList>
#include <QVariantMap>
#include <QLoggingCategory>
#include <QSet>
#include <QTimeZone>
#include <QUrl>

#include <algorithm>

Q_LOGGING_CATEGORY(lcRust, "matrix.rust")

namespace {

QString takeRustString(char *raw)
{
    if (!raw)
        return {};
    QString out = QString::fromUtf8(raw);
    mx_rust_free_cstring(raw);
    return out;
}

bool pathExistsOrIsLink(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

QDateTime timestampFromMs(qint64 ms)
{
    if (ms <= 0)
        return {};
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC);
}

TimelineEvent::Type typeFromString(const QString &msgtype)
{
    if (msgtype == QLatin1String("notice"))
        return TimelineEvent::Notice;
    if (msgtype == QLatin1String("emote"))
        return TimelineEvent::Emote;
    return TimelineEvent::TextMessage;
}

QString previewFor(const TimelineEvent &event)
{
    // One normalizing choke point for every side-surface summary: a poll's
    // multi-line MSC3381 fallback, a mention's markdown permalink, or any
    // multi-line body must never reach the room list verbatim.
    return matrix::preview::oneLineSummary(event);
}

} // namespace

RustSdkMatrixClient::RustSdkMatrixClient(SettingsManager *settings, QObject *parent)
    : MatrixClient(parent)
    , m_settings(settings)
{
    m_pollTimer.setInterval(100);
    connect(&m_pollTimer, &QTimer::timeout, this, &RustSdkMatrixClient::pollRustEvents);

    qCInfo(lcRust) << "Rust SDK backend loaded:"
                   << rustBackendName()
                   << "version" << rustBackendVersion()
                   << "supports_e2ee=" << rustSupportsE2ee();
}

RustSdkMatrixClient::~RustSdkMatrixClient()
{
    m_pollTimer.stop();
    m_lifecycle.invalidate();
    releaseRustHandle();
}

QString RustSdkMatrixClient::rustBackendName() const
{
    return takeRustString(mx_rust_backend_name());
}

QString RustSdkMatrixClient::rustBackendStatus() const
{
    return takeRustString(mx_rust_status_string());
}

QString RustSdkMatrixClient::rustBackendVersion() const
{
    return takeRustString(mx_rust_version());
}

bool RustSdkMatrixClient::rustSupportsE2ee() const
{
    return mx_rust_supports_e2ee(m_rustHandle) != 0;
}

void RustSdkMatrixClient::setStorePathOverride(const QString &absolutePath)
{
    m_storePathOverride = absolutePath;
}

void RustSdkMatrixClient::setPersistentSessionFile(const QString &absolutePath)
{
    m_sessionFilePath = absolutePath;
    if (!m_rustHandle)
        return;
    const QByteArray path = m_sessionFilePath.toUtf8();
    const QString result = takeRustString(mx_rust_set_session_file(m_rustHandle,
                                                                    path.constData()));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                             ? result.mid(7)
                             : result);
    }
}

QString RustSdkMatrixClient::rustStorePath() const
{
    return m_storePath;
}

bool RustSdkMatrixClient::rustStorePathIsOverride() const
{
    return !m_storePathOverride.isEmpty();
}

QString RustSdkMatrixClient::currentDeviceId() const
{
    return m_deviceId;
}

void RustSdkMatrixClient::setState(ConnectionState state)
{
    if (m_state == state)
        return;
    m_state = state;
    Q_EMIT connectionStateChanged(m_state);
}

void RustSdkMatrixClient::setInitialSyncDone(bool done)
{
    if (m_initialSyncDone == done)
        return;
    m_initialSyncDone = done;
    Q_EMIT initialSyncDoneChanged();
}

void RustSdkMatrixClient::clearLocalState()
{
    m_loggedIn = false;
    m_homeserver.clear();
    m_userId.clear();
    m_deviceId.clear();
    m_rooms.clear();
    m_roomOrder.clear();
    m_lastReceiptSent.clear();
    m_syncMode = QStringLiteral("stopped");
    m_lastSyncState.clear();
    m_timelines.clear();
    m_pendingSends.clear();
    m_pendingProbes.clear();
    m_timelineTracker.reset();
    m_threadTracker.reset();
    m_pagination.clear();
    m_maxUploadSize = 0;
    m_uploadLimitRequested = false;
    setInitialSyncDone(false);
    Q_EMIT roomsChanged();
    setState(Disconnected);
}

void RustSdkMatrixClient::ensurePollTimer()
{
    if (!m_pollTimer.isActive())
        m_pollTimer.start();
}

QString RustSdkMatrixClient::rustStorePathForUser(const QString &userIdForStore) const
{
    // Testing hook wins: the smoke harness passes an absolute
    // QTemporaryDir path so every run starts from a clean crypto store.
    if (!m_storePathOverride.isEmpty())
        return m_storePathOverride;

    return matrix::app_data::rustSdkStorePath(userIdForStore);
}

bool RustSdkMatrixClient::ensureRustHandleForUser(const QString &userIdForStore)
{
    const QString storePath = rustStorePathForUser(userIdForStore);
    if (storePath.isEmpty())
        return false;
    if (m_storePathOverride.isEmpty() && QFileInfo(storePath).isSymLink()) {
        qCWarning(lcRust) << "refusing symlinked Rust SDK store";
        return false;
    }

    // A handle/event queue is never reused across login generations. This is
    // the ownership boundary that makes stale async callbacks unobservable.
    releaseRustHandle();

    if (!QDir().mkpath(storePath)) {
        Q_EMIT errorOccurred(tr("Failed to create Rust SDK store directory: %1").arg(storePath));
        return false;
    }

    // Safe path diagnostic — paths only, never tokens/keys/bodies.
    // Logged at INFO so users can grep matrix.rust: from the terminal
    // when the SDK complains about crypto-store mismatches.
    const QString slug = m_storePathOverride.isEmpty()
        ? matrix::app_data::safeUserSlug(userIdForStore)
        : QStringLiteral("(override)");
    qCInfo(lcRust) << "Rust SDK store path resolved"
                   << "base=" << matrix::app_data::primaryRoot()
                   << "slug=" << slug
                   << "store=" << storePath
                   << "exists=" << QFileInfo::exists(storePath)
                   << "mode=" << (m_storePathOverride.isEmpty()
                                  ? QStringLiteral("persistent")
                                  : QStringLiteral("temporary"));

    const QByteArray path = QFileInfo(storePath).absoluteFilePath().toUtf8();
    m_rustHandle = mx_rust_create(path.constData());
    if (!m_rustHandle) {
        Q_EMIT errorOccurred(tr("Failed to create Rust SDK backend handle."));
        return false;
    }

    m_storePath = storePath;
    m_handleGeneration = m_lifecycle.beginSession();

    if (!m_sessionFilePath.isEmpty()) {
        const QByteArray sessionPath = m_sessionFilePath.toUtf8();
        const QString result = takeRustString(mx_rust_set_session_file(m_rustHandle,
                                                                        sessionPath.constData()));
        if (!result.isEmpty()) {
            mx_rust_destroy(m_rustHandle);
            m_rustHandle = nullptr;
            m_handleGeneration = 0;
            Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
            return false;
        }
    }

    ensurePollTimer();
    return true;
}

void RustSdkMatrixClient::releaseRustHandle()
{
    m_pollTimer.stop();
    if (!m_rustHandle)
        return;
    if (!m_typingRoom.isEmpty()) {
        const QByteArray room = m_typingRoom.toUtf8();
        takeRustString(mx_rust_send_typing(m_rustHandle, room.constData(), 0));
        m_typingRoom.clear();
    }
    // v0.5.7: deterministic teardown — timeline subscriptions are cancelled
    // and joined, an in-flight room-key import is joined (bounded last-resort
    // timeout), the sync loop is stopped. Only then is the handle destroyed,
    // so no task can still own the SDK store when callers delete it.
    const QString shutdown = takeRustString(mx_rust_shutdown_tasks(m_rustHandle));
    qCInfo(lcRust) << "rust managed-task shutdown" << shutdown;
    m_timelineTracker.reset();
    m_threadTracker.reset();
    m_pagination.clear();
    mx_rust_destroy(m_rustHandle);
    m_rustHandle = nullptr;
    m_handleGeneration = 0;
    m_storePath.clear();
    qCInfo(lcRust) << "rust client released";
}

void RustSdkMatrixClient::login(const QString &homeserver,
                                const QString &user,
                                const QString &password)
{
    matrix::app_data::AccountIdentity identity;
    if (!matrix::app_data::resolveAccountIdentity(homeserver, user, &identity)
        || password.isEmpty()) {
        Q_EMIT loginFailed(tr("Homeserver, user, and password are required."));
        return;
    }

    // The slug flattening is not injective: refuse a login whose identity
    // collides with a DIFFERENT saved account before contacting the server,
    // since both would alias one settings record and one SDK store.
    if (m_settings && m_settings->accountSlugConflicts(identity.userId)) {
        qCWarning(lcRust) << "login refused: account slug collision"
                          << "slug=" << identity.slug;
        Q_EMIT loginFailed(tr(
            "This account's local storage name collides with a different "
            "account already saved on this device. Remove that account "
            "first if you want to sign in with this one."));
        return;
    }

    bool storeExists = pathExistsOrIsLink(identity.rustStorePath);
    // v0.7 multi-account: only the TARGET account's own saved record is
    // consulted — other signed-in accounts never block a new login.
    const bool targetHasSavedSession = m_settings
        && m_settings->hasSavedAccount(identity.userId)
        && !m_settings->accessTokenFor(identity.userId).isEmpty();

    // An orphaned store — the directory exists but there is no saved
    // session/token that could ever restore it — is unusable by definition.
    // The classic source is an earlier failed or cancelled login attempt
    // (the store directory is created before the server accepts the
    // password). Clean it up instead of dead-ending the user on the
    // "belongs to a different session or device" reset prompt.
    if (storeExists && !targetHasSavedSession) {
        const auto removed = matrix::app_data::removeAccountRustState(identity);
        qCInfo(lcRust) << "removed orphaned store before login"
                       << "slug=" << identity.slug
                       << "deleted=" << removed.deleted
                       << "failed=" << removed.failed;
        storeExists = pathExistsOrIsLink(identity.rustStorePath);
        if (storeExists) {
            setState(Error);
            Q_EMIT loginFailed(tr(
                "An unusable local store for this account could not be "
                "removed. Check filesystem permissions and try again."));
            return;
        }
    }
    // Remember fresh-store attempts so a failure can clean up after
    // itself instead of poisoning the next attempt.
    m_freshLoginIdentity = storeExists ? matrix::app_data::AccountIdentity{}
                                       : identity;
    const QString targetSavedDeviceId = m_settings
        ? m_settings->accountRecord(identity.userId)
              .value(QStringLiteral("deviceId")).toString()
        : QString{};
    const auto block = matrix::rust_session::passwordLoginBlockReason(
        identity, storeExists, targetHasSavedSession, targetSavedDeviceId);
    if (block == matrix::rust_session::StoreBlockReason::ExistingStoreNeedsRestore
        && targetHasSavedSession) {
        // Not an error state: the account is already usable on this device.
        qCInfo(lcRust) << "login redirected to switch"
                       << "slug=" << identity.slug;
        Q_EMIT loginFailed(tr(
            "This account is already signed in on this device. Switch to it "
            "from the account menu instead of signing in again."));
        return;
    }
    if (block != matrix::rust_session::StoreBlockReason::None) {
        qCWarning(lcRust) << "login blocked reason=local_store_session_mismatch"
                          << "detail=" << matrix::rust_session::diagnosticName(block)
                          << "slug=" << identity.slug;
        requireLocalReset(matrix::rust_session::diagnosticName(block));
        setState(Error);
        Q_EMIT loginFailed(tr(
            "This local Lightning Rust SDK store belongs to a different "
            "Matrix session or device. Reset the local Lightning session "
            "for this account, then sign in again. This does not delete "
            "server messages or Element data."));
        return;
    }

    if (!ensureRustHandleForUser(identity.userId)) {
        setState(Error);
        Q_EMIT loginFailed(tr("Rust SDK backend could not be initialized."));
        return;
    }

    m_homeserver = identity.homeserver;
    m_userId.clear();
    m_deviceId.clear();
    m_loggedIn = false;
    m_rooms.clear();
    m_roomOrder.clear();
    m_timelines.clear();
    m_pendingSends.clear();
    setInitialSyncDone(false);
    Q_EMIT roomsChanged();
    setState(Connecting);

    const QByteArray hsBytes = identity.homeserver.toUtf8();
    const QByteArray userBytes = identity.userId.toUtf8();
    const QByteArray passwordBytes = password.toUtf8();
    const QString result = takeRustString(mx_rust_login(m_rustHandle,
                                                        hsBytes.constData(),
                                                        userBytes.constData(),
                                                        passwordBytes.constData()));
    if (!result.isEmpty()) {
        setState(Error);
        Q_EMIT loginFailed(result.startsWith(QLatin1String("error: "))
                           ? result.mid(7)
                           : result);
    }
}

bool RustSdkMatrixClient::detachSession()
{
    // A real sign-out is in flight: its completion event is the ONLY path
    // that deletes this account's persisted token, record, and store.
    // Invalidating the lifecycle now would discard that completion and
    // silently downgrade the sign-out to a local detach — refuse instead;
    // the caller reports "try again in a moment".
    if (m_lifecycle.signingOut()) {
        qCWarning(lcRust) << "detach refused: sign-out still in flight";
        return false;
    }
    // v0.7 account switch: end the local session without a server logout.
    // The account's SDK store, SecretStore token, and account record are
    // deliberately untouched — restoreSession() reactivates it later.
    qCInfo(lcRust) << "detaching local session"
                   << "slug=" << matrix::app_data::safeUserSlug(m_userId);
    // Stale callbacks from this session become unobservable immediately;
    // releaseRustHandle() then cancels/joins every managed task before the
    // handle is destroyed.
    m_lifecycle.invalidate();
    releaseRustHandle();
    clearLocalState();
    Q_EMIT loggedOut();
    return true;
}

bool RustSdkMatrixClient::restoreSession()
{
    if (!m_settings || !m_settings->hasSession())
        return false;

    matrix::app_data::AccountIdentity identity;
    if (!matrix::app_data::resolveAccountIdentity(
            m_settings->homeserverUrl(), m_settings->userId(), &identity)) {
        requireLocalReset(QStringLiteral("invalid_saved_account_identity"));
        Q_EMIT loginFailed(tr(
            "This local Lightning Rust SDK store belongs to a different "
            "Matrix session or device. Reset the local Lightning session "
            "for this account, then sign in again. This does not delete "
            "server messages or Element data."));
        return false;
    }

    const QString hs = identity.homeserver;
    const QString userId = m_settings->userId();
    const QString deviceId = m_settings->deviceId();
    const QString accessToken = m_settings->accessToken();
    if (hs.isEmpty() || userId.isEmpty() || accessToken.isEmpty())
        return false;

    const auto block = matrix::rust_session::restoreBlockReason(
        identity, pathExistsOrIsLink(identity.rustStorePath), deviceId);
    if (block != matrix::rust_session::StoreBlockReason::None) {
        qCWarning(lcRust) << "restore blocked reason=local_store_session_mismatch"
                          << "detail=" << matrix::rust_session::diagnosticName(block)
                          << "slug=" << identity.slug;
        requireLocalReset(matrix::rust_session::diagnosticName(block));
        setState(Error);
        Q_EMIT loginFailed(tr(
            "This local Lightning Rust SDK store belongs to a different "
            "Matrix session or device. Reset the local Lightning session "
            "for this account, then sign in again. This does not delete "
            "server messages or Element data."));
        return false;
    }

    if (!ensureRustHandleForUser(userId)) {
        setState(Error);
        Q_EMIT loginFailed(tr("Rust SDK backend could not be initialized."));
        return false;
    }

    m_homeserver = hs;
    m_userId = userId;
    m_deviceId = deviceId;
    m_loggedIn = false;
    m_rooms.clear();
    m_roomOrder.clear();
    m_timelines.clear();
    m_pendingSends.clear();
    setInitialSyncDone(false);
    Q_EMIT roomsChanged();
    setState(Connecting);

    const QByteArray hsBytes = hs.toUtf8();
    const QByteArray userBytes = userId.toUtf8();
    const QByteArray deviceBytes = deviceId.toUtf8();
    const QByteArray tokenBytes = accessToken.toUtf8();
    const QString result = takeRustString(mx_rust_restore(m_rustHandle,
                                                          hsBytes.constData(),
                                                          userBytes.constData(),
                                                          deviceBytes.constData(),
                                                          tokenBytes.constData()));
    if (!result.isEmpty()) {
        setState(Error);
        Q_EMIT loginFailed(result.startsWith(QLatin1String("error: "))
                           ? result.mid(7)
                           : result);
        return false;
    }
    return true;
}

bool RustSdkMatrixClient::restoreSessionFromFile(const QString &homeserver,
                                                 const QString &userIdForStore)
{
    matrix::app_data::AccountIdentity identity;
    if (!matrix::app_data::resolveAccountIdentity(
            homeserver, userIdForStore, &identity)
        || m_sessionFilePath.isEmpty())
        return false;
    const QString hs = identity.homeserver;
    const QString expectedUser = identity.userId;

    if (!ensureRustHandleForUser(expectedUser)) {
        setState(Error);
        Q_EMIT loginFailed(tr("Rust SDK backend could not be initialized."));
        return false;
    }

    m_homeserver = hs;
    m_userId = expectedUser;
    m_deviceId.clear();
    m_loggedIn = false;
    m_rooms.clear();
    m_roomOrder.clear();
    m_timelines.clear();
    m_pendingSends.clear();
    setInitialSyncDone(false);
    Q_EMIT roomsChanged();
    setState(Connecting);

    const QByteArray hsBytes = hs.toUtf8();
    const QByteArray userBytes = expectedUser.toUtf8();
    const QString result = takeRustString(mx_rust_restore_from_file(m_rustHandle,
                                                                     hsBytes.constData(),
                                                                     userBytes.constData()));
    if (!result.isEmpty()) {
        setState(Error);
        Q_EMIT loginFailed(result.startsWith(QLatin1String("error: "))
                           ? result.mid(7)
                           : result);
        return false;
    }
    return true;
}

bool RustSdkMatrixClient::resetRustStore()
{
    const QString storePath = m_storePath;
    m_lifecycle.invalidate();
    releaseRustHandle();
    m_storePath.clear();
    clearLocalState();

    if (storePath.isEmpty() || !QFileInfo::exists(storePath))
        return true;

    QDir storeDir(storePath);
    return storeDir.removeRecursively();
}

bool RustSdkMatrixClient::clearPersistedAccount(
    const matrix::app_data::AccountIdentity &identity)
{
    return !m_settings || m_settings->clearSessionForAccount(identity.userId);
}

bool RustSdkMatrixClient::resetLocalSession(
    const matrix::app_data::AccountIdentity &identity,
    QString *message)
{
    if (message)
        message->clear();
    if (!identity.isValid()) {
        if (message) {
            *message = tr("Enter a valid homeserver and Matrix user ID before "
                          "resetting the local Lightning session.");
        }
        return false;
    }
    if (m_loggedIn || m_lifecycle.signingOut()) {
        if (message) {
            *message = tr("Lightning could not completely reset the local "
                          "session for this account. Check the application "
                          "logs and filesystem permissions, then try again.");
        }
        return false;
    }
    if (m_rustHandle && !m_storePath.isEmpty()
        && QFileInfo(m_storePath).absoluteFilePath()
            != QFileInfo(identity.rustStorePath).absoluteFilePath()) {
        if (message) {
            *message = tr("Lightning could not completely reset the local "
                          "session for this account. Check the application "
                          "logs and filesystem permissions, then try again.");
        }
        return false;
    }

    m_lifecycle.invalidate();
    releaseRustHandle();
    m_storePath.clear();
    clearLocalState();
    const bool sessionOk = clearPersistedAccount(identity);
    const auto files = matrix::app_data::removeAccountRustState(identity);
    const bool ok = sessionOk && files.ok();
    qCInfo(lcRust) << "local Rust reset"
                   << "slug=" << identity.slug
                   << "deleted=" << files.deleted
                   << "missing=" << files.missing
                   << "failed=" << files.failed
                   << "session=" << (sessionOk ? "ok" : "failed");

    if (ok) {
        if (message)
            *message = tr("Local Lightning session reset. You can sign in again.");
    } else {
        requireLocalReset(QStringLiteral("cleanup_incomplete"));
        if (message) {
            *message = tr("Lightning could not completely reset the local "
                          "session for this account. Check the application "
                          "logs and filesystem permissions, then try again.");
        }
    }
    return ok;
}

void RustSdkMatrixClient::logout()
{
    if (m_lifecycle.signingOut())
        return;

    matrix::app_data::resolveAccountIdentity(
        m_homeserver, m_userId, &m_signOutIdentity);
    m_signOutDeviceId = m_deviceId;
    qCInfo(lcRust) << "rust sign-out started"
                   << "slug=" << m_signOutIdentity.slug
                   << "device_known=" << !m_signOutDeviceId.isEmpty();
    m_lifecycle.beginSignOut(m_handleGeneration);

    // Queue typing=false before the deterministic join so the old room is
    // cleared while the client and sync transport still belong to this
    // lifecycle.
    if (m_rustHandle && !m_typingRoom.isEmpty()) {
        const QByteArray room = m_typingRoom.toUtf8();
        takeRustString(mx_rust_send_typing(m_rustHandle, room.constData(), 0));
        m_typingRoom.clear();
    }

    // v0.5.7: deterministic managed-task shutdown replaces the 0.5.6
    // import_active poll loop. Rust cancels and *joins* the timeline
    // subscription, joins an in-flight room-key import (the crypto store
    // must never be deleted under a live write), and stops the sync loop.
    // The bounded timeout inside is a last-resort error boundary only.
    if (m_rustHandle) {
        const QString shutdown =
            takeRustString(mx_rust_shutdown_tasks(m_rustHandle));
        qCInfo(lcRust) << "logout: managed-task shutdown" << shutdown;
        if (mx_rust_room_key_import_active(m_rustHandle)) {
            qCWarning(lcRust)
                << "logout: import did not finish in time; proceeding anyway";
        }
    }
    m_timelineTracker.reset();
    m_threadTracker.reset();
    m_pagination.clear();
    stopSync();

    if (m_rustHandle) {
        mx_rust_logout(m_rustHandle);
        ensurePollTimer();
    } else {
        finishSignOut(QStringLiteral("no_active_session"), QString{});
    }
}

void RustSdkMatrixClient::startSync()
{
    if (!m_loggedIn || !m_rustHandle)
        return;
    setState(Syncing);
    mx_rust_start_sync(m_rustHandle);
    ensurePollTimer();
}

void RustSdkMatrixClient::stopSync()
{
    if (m_rustHandle) {
        const bool stopped = mx_rust_stop_sync(m_rustHandle) != 0;
        qCInfo(lcRust) << "rust sync stop result="
                       << (stopped ? "ok" : "already_stopped");
    }
    if (m_state == Syncing || m_state == Offline)
        setState(Disconnected);
}

QList<RoomInfo> RustSdkMatrixClient::rooms() const
{
    QList<RoomInfo> list;
    list.reserve(m_rooms.size());
    QSet<QString> seen;
    for (const auto &roomId : m_roomOrder) {
        const auto it = m_rooms.constFind(roomId);
        if (it != m_rooms.constEnd() && !seen.contains(roomId)) {
            list.append(*it);
            seen.insert(roomId);
        }
    }
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it) {
        if (!seen.contains(it.key())) list.append(*it);
    }
    return list;
}

QList<TimelineEvent> RustSdkMatrixClient::timeline(const QString &roomId) const
{
    return m_timelines.value(roomId);
}

QString RustSdkMatrixClient::displayNameFor(const QString &roomId, const QString &userId) const
{
    const auto it = m_rooms.constFind(roomId);
    if (it == m_rooms.constEnd())
        return userId;
    const auto memberIt = it->members.constFind(userId);
    if (memberIt == it->members.constEnd() || memberIt->displayName.isEmpty())
        return userId;
    return memberIt->displayName;
}

QString RustSdkMatrixClient::avatarMxcFor(const QString &roomId, const QString &userId) const
{
    const auto it = m_rooms.constFind(roomId);
    if (it == m_rooms.constEnd())
        return {};
    const auto memberIt = it->members.constFind(userId);
    return memberIt == it->members.constEnd() ? QString() : memberIt->avatarMxcUrl;
}

QStringList RustSdkMatrixClient::typingUsersFor(const QString &roomId) const
{
    const auto it = m_rooms.constFind(roomId);
    return it == m_rooms.constEnd() ? QStringList() : it->typingUserIds;
}

QUrl RustSdkMatrixClient::mediaDownloadUrl(const QString &mxcUrl) const
{
    return matrix::media::downloadUrl(m_homeserver, mxcUrl);
}

QUrl RustSdkMatrixClient::mediaThumbnailUrl(const QString &mxcUrl,
                                            int width,
                                            int height,
                                            bool crop) const
{
    return matrix::media::thumbnailUrl(m_homeserver, mxcUrl, width, height, crop);
}

QString RustSdkMatrixClient::nextTxnId()
{
    return QStringLiteral("r%1.%2")
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(++m_txnCounter);
}

bool RustSdkMatrixClient::isRoomEncrypted(const QString &roomId) const
{
    const auto it = m_rooms.constFind(roomId);
    return it != m_rooms.constEnd() && it->encrypted;
}

TimelineEvent RustSdkMatrixClient::buildOwnEcho(const QString &roomId,
                                                const QString &body,
                                                TimelineEvent::Type type) const
{
    TimelineEvent event;
    event.roomId = roomId;
    event.sender = m_userId;
    event.senderDisplayName = QStringLiteral("You");
    event.body = body;
    event.timestamp = QDateTime::currentDateTimeUtc();
    event.type = type;
    event.status = TimelineEvent::Sending;
    return event;
}

void RustSdkMatrixClient::sendTextMessage(const QString &roomId, const QString &body)
{
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT errorOccurred(tr("Not signed in."));
        return;
    }
    if (!m_rooms.contains(roomId)) {
        Q_EMIT errorOccurred(tr("Unknown room: %1").arg(roomId));
        return;
    }
    if (isRoomEncrypted(roomId) && !rustSupportsE2ee()) {
        Q_EMIT errorOccurred(tr(
            "Cannot send to encrypted rooms yet: Rust SDK encrypted send is not verified."));
        return;
    }

    // v0.5.7: rooms with a live SDK timeline send through Timeline::send —
    // the SDK creates the local echo, drives sending → sent/failed
    // transitions, and reconciles the remote echo in place. No C++-side
    // echo, no duplicate.
    if (timelineActiveFor(roomId)) {
        const QByteArray roomBytes = roomId.toUtf8();
        const QByteArray bodyBytes = body.toUtf8();
        const QString result = takeRustString(mx_rust_timeline_send_text(
            m_rustHandle, roomBytes.constData(), bodyBytes.constData(),
            nullptr));
        if (!result.isEmpty()) {
            Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                     ? result.mid(7)
                                     : result);
        }
        return;
    }

    const QString txnId = nextTxnId();
    TimelineEvent echo = buildOwnEcho(roomId, body, TimelineEvent::TextMessage);
    echo.eventId = QLatin1String("local:") + txnId;
    m_timelines[roomId].append(echo);
    m_pendingSends.insert(txnId, PendingSend{roomId, echo.eventId});
    Q_EMIT eventAppended(roomId, echo);

    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray bodyBytes = body.toUtf8();
    const QByteArray txnBytes = txnId.toUtf8();
    const QString result = takeRustString(mx_rust_send_text(m_rustHandle,
                                                            roomBytes.constData(),
                                                            bodyBytes.constData(),
                                                            txnBytes.constData()));
    if (!result.isEmpty()) {
        failPendingSend(txnId, result.startsWith(QLatin1String("error: "))
                                   ? result.mid(7)
                                   : result);
    }
}

// v0.7: outgoing @-mentions. The body already carries matrix.to markdown
// links; the id list is forwarded to the SDK so it writes m.mentions. Empty
// ids or a room without a live timeline fall back to the plain send path.
void RustSdkMatrixClient::sendTextMessage(const QString &roomId,
                                          const QString &body,
                                          const QStringList &mentionUserIds)
{
    if (mentionUserIds.isEmpty()) {
        sendTextMessage(roomId, body);
        return;
    }
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT errorOccurred(tr("Not signed in."));
        return;
    }
    if (!m_rooms.contains(roomId)) {
        Q_EMIT errorOccurred(tr("Unknown room: %1").arg(roomId));
        return;
    }
    if (isRoomEncrypted(roomId) && !rustSupportsE2ee()) {
        Q_EMIT errorOccurred(tr(
            "Cannot send to encrypted rooms yet: Rust SDK encrypted send is not verified."));
        return;
    }
    if (!timelineActiveFor(roomId)) {
        sendTextMessage(roomId, body);
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray bodyBytes = body.toUtf8();
    const QByteArray mentionBytes =
        mentionUserIds.join(QLatin1Char('\n')).toUtf8();
    const QString result = takeRustString(mx_rust_timeline_send_text(
        m_rustHandle, roomBytes.constData(), bodyBytes.constData(),
        mentionBytes.constData()));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

// v0.5.7: replies, edits, reactions, and redactions route through the
// official matrix-sdk-ui timeline actions when the room's live timeline is
// open (relation JSON is never hand-built in C++). Rooms without a live
// timeline keep the previous refusal.
void RustSdkMatrixClient::sendReply(const QString &roomId,
                                    const QString &replyToEventId,
                                    const QString &body)
{
    if (!timelineActiveFor(roomId)) {
        refuseSend("sendReply");
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray targetBytes = replyToEventId.toUtf8();
    const QByteArray bodyBytes = body.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_send_reply(
        m_rustHandle, roomBytes.constData(), targetBytes.constData(),
        bodyBytes.constData(), nullptr));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

void RustSdkMatrixClient::sendReply(const QString &roomId,
                                    const QString &replyToEventId,
                                    const QString &body,
                                    const QStringList &mentionUserIds)
{
    if (mentionUserIds.isEmpty() || !timelineActiveFor(roomId)) {
        sendReply(roomId, replyToEventId, body);
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray targetBytes = replyToEventId.toUtf8();
    const QByteArray bodyBytes = body.toUtf8();
    const QByteArray mentionBytes =
        mentionUserIds.join(QLatin1Char('\n')).toUtf8();
    const QString result = takeRustString(mx_rust_timeline_send_reply(
        m_rustHandle, roomBytes.constData(), targetBytes.constData(),
        bodyBytes.constData(), mentionBytes.constData()));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

void RustSdkMatrixClient::editMessage(const QString &roomId,
                                      const QString &targetEventId,
                                      const QString &newBody)
{
    if (!timelineActiveFor(roomId)) {
        refuseSend("editMessage");
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray targetBytes = targetEventId.toUtf8();
    const QByteArray bodyBytes = newBody.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_edit(
        m_rustHandle, roomBytes.constData(), targetBytes.constData(),
        bodyBytes.constData(), nullptr));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

void RustSdkMatrixClient::editMessage(const QString &roomId,
                                      const QString &targetEventId,
                                      const QString &newBody,
                                      const QStringList &mentionUserIds)
{
    if (mentionUserIds.isEmpty() || !timelineActiveFor(roomId)) {
        editMessage(roomId, targetEventId, newBody);
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray targetBytes = targetEventId.toUtf8();
    const QByteArray bodyBytes = newBody.toUtf8();
    const QByteArray mentionBytes =
        mentionUserIds.join(QLatin1Char('\n')).toUtf8();
    const QString result = takeRustString(mx_rust_timeline_edit(
        m_rustHandle, roomBytes.constData(), targetBytes.constData(),
        bodyBytes.constData(), mentionBytes.constData()));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

void RustSdkMatrixClient::redactEvent(const QString &roomId,
                                      const QString &eventId,
                                      const QString &reason)
{
    if (!timelineActiveFor(roomId)) {
        refuseSend("redactEvent");
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray targetBytes = eventId.toUtf8();
    const QByteArray reasonBytes = reason.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_redact(
        m_rustHandle, roomBytes.constData(), targetBytes.constData(),
        reasonBytes.constData()));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

void RustSdkMatrixClient::toggleReaction(const QString &roomId,
                                         const QString &targetEventId,
                                         const QString &key)
{
    if (!timelineActiveFor(roomId)) {
        refuseSend("toggleReaction");
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray targetBytes = targetEventId.toUtf8();
    const QByteArray keyBytes = key.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_toggle_reaction(
        m_rustHandle, roomBytes.constData(), targetBytes.constData(),
        keyBytes.constData()));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

// v0.7 polls. Votes and ends act on a poll visible in the CURRENT room (or
// one of its threads), so the room-timeline-active guard applies to all
// three actions; the thread target is resolved Rust-side (open panel
// timeline, else a transient thread-focused timeline).
void RustSdkMatrixClient::sendPollResponse(const QString &roomId,
                                           const QString &threadRootId,
                                           const QString &pollStartEventId,
                                           const QStringList &answerIds)
{
    if (!timelineActiveFor(roomId)) {
        refuseSend("sendPollResponse");
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray threadBytes = threadRootId.toUtf8();
    const QByteArray pollBytes = pollStartEventId.toUtf8();
    // The FFI list is newline-joined; a hostile poll whose answer ids embed
    // newlines would otherwise submit split, non-matching ids (a spoiled
    // vote). Such ids are dropped rather than mangled.
    QStringList safeIds;
    for (const QString &id : answerIds) {
        if (!id.contains(QLatin1Char('\n')))
            safeIds.append(id);
    }
    const QByteArray answerBytes = safeIds.join(QLatin1Char('\n')).toUtf8();
    const QString result = takeRustString(mx_rust_timeline_poll_response(
        m_rustHandle, roomBytes.constData(), threadBytes.constData(),
        pollBytes.constData(), answerBytes.constData()));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

void RustSdkMatrixClient::endPoll(const QString &roomId,
                                  const QString &threadRootId,
                                  const QString &pollStartEventId)
{
    if (!timelineActiveFor(roomId)) {
        refuseSend("endPoll");
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray threadBytes = threadRootId.toUtf8();
    const QByteArray pollBytes = pollStartEventId.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_poll_end(
        m_rustHandle, roomBytes.constData(), threadBytes.constData(),
        pollBytes.constData()));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

void RustSdkMatrixClient::createPoll(const QString &roomId,
                                     const QString &threadRootId,
                                     const QString &question,
                                     const QStringList &answers,
                                     bool undisclosed,
                                     int maxSelections)
{
    if (!timelineActiveFor(roomId)) {
        refuseSend("createPoll");
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray threadBytes = threadRootId.toUtf8();
    const QByteArray questionBytes = question.toUtf8();
    const QByteArray answerBytes = answers.join(QLatin1Char('\n')).toUtf8();
    const QString result = takeRustString(mx_rust_timeline_poll_create(
        m_rustHandle, roomBytes.constData(), threadBytes.constData(),
        questionBytes.constData(), answerBytes.constData(),
        undisclosed ? 1 : 0,
        static_cast<unsigned int>(qMax(1, maxSelections))));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

void RustSdkMatrixClient::sendTyping(const QString &roomId, bool typing, int)
{
    if (!m_rustHandle || roomId.isEmpty()) return;
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_send_typing(
        m_rustHandle, room.constData(), typing ? 1 : 0));
    if (result.isEmpty()) {
        if (typing) m_typingRoom = roomId;
        else if (m_typingRoom == roomId) m_typingRoom.clear();
    } else {
        qCWarning(lcRust) << "typing command rejected";
    }
}

void RustSdkMatrixClient::sendReadReceipt(const QString &roomId, const QString &eventId)
{
    if (!m_rustHandle || roomId.isEmpty() || eventId.isEmpty()
        || m_lastReceiptSent.value(roomId) == eventId)
        return;
    m_lastReceiptSent.insert(roomId, eventId);
    const QByteArray room = roomId.toUtf8();
    const QByteArray event = eventId.toUtf8();
    const QString result = takeRustString(mx_rust_send_read_receipt(
        m_rustHandle, room.constData(), event.constData()));
    if (!result.isEmpty()) {
        m_lastReceiptSent.remove(roomId);
        qCWarning(lcRust) << "read receipt command rejected";
    }
}

void RustSdkMatrixClient::setRoomMarkedUnread(const QString &roomId, bool unread)
{
    if (!m_rustHandle || roomId.isEmpty()) return;
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_set_marked_unread(
        m_rustHandle, room.constData(), unread ? 1 : 0));
    if (!result.isEmpty()) qCWarning(lcRust) << "marked-unread command rejected";
}

void RustSdkMatrixClient::acceptInvite(const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty()) return;
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_accept_invite(m_rustHandle, room.constData()));
    if (!result.isEmpty()) qCWarning(lcRust) << "invite accept command rejected";
}

void RustSdkMatrixClient::rejectInvite(const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty()) return;
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_reject_invite(m_rustHandle, room.constData()));
    if (!result.isEmpty()) qCWarning(lcRust) << "invite reject command rejected";
}

void RustSdkMatrixClient::sendImage(const QString &, const QString &)
{
    refuseSend("sendImage");
}

void RustSdkMatrixClient::sendFile(const QString &, const QString &)
{
    refuseSend("sendFile");
}

namespace {
// One pagination batch. Matches timeline::PAGINATION_BATCH on the Rust
// side; large enough to fill a screen, small enough to stay responsive.
constexpr unsigned short kPaginationBatch = 20;
} // namespace

void RustSdkMatrixClient::loadOlderMessages(const QString &roomId)
{
    if (!timelineActiveFor(roomId))
        return;
    auto &state = m_pagination[roomId];
    if (state.loading || state.reachedStart)
        return;
    QString result;
    if (isThreadTimelineId(roomId)) {
        const QByteArray room = threadTimelineRoomId(roomId).toUtf8();
        const QByteArray root = threadTimelineRootId(roomId).toUtf8();
        result = takeRustString(mx_rust_thread_paginate_back(
            m_rustHandle, room.constData(), root.constData(),
            kPaginationBatch));
    } else {
        const QByteArray roomBytes = roomId.toUtf8();
        result = takeRustString(mx_rust_timeline_paginate_back(
            m_rustHandle, roomBytes.constData(), kPaginationBatch));
    }
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "timeline pagination dispatch failed";
        state.failed = true;
        state.failureTransient = true;
        Q_EMIT paginationStateChanged(roomId);
    } else if (state.failed) {
        // An accepted explicit retry has left the previous terminal state.
        // The Rust loading event follows asynchronously, but presentation
        // must enter loading immediately instead of flashing the old error.
        state.failed = false;
        state.failureTransient = false;
        Q_EMIT paginationStateChanged(roomId);
    }
}

bool RustSdkMatrixClient::canPaginate(const QString &roomId) const
{
    if (!timelineActiveFor(roomId))
        return false;
    const auto it = m_pagination.constFind(roomId);
    if (it == m_pagination.constEnd())
        return true;
    return !it->loading && !it->reachedStart;
}

bool RustSdkMatrixClient::paginating(const QString &roomId) const
{
    const auto it = m_pagination.constFind(roomId);
    return it != m_pagination.constEnd() && it->loading;
}

bool RustSdkMatrixClient::paginationFailed(const QString &roomId) const
{
    const auto it = m_pagination.constFind(roomId);
    return it != m_pagination.constEnd() && it->failed;
}

bool RustSdkMatrixClient::paginationFailureTransient(const QString &roomId) const
{
    const auto it = m_pagination.constFind(roomId);
    return it != m_pagination.constEnd() && it->failed
        && it->failureTransient;
}

void RustSdkMatrixClient::retryFailedSend(const QString &roomId,
                                          const QString &transactionId)
{
    if (isThreadTimelineId(roomId)) {
        // A thread echo is a room send-queue entry; retry it through the
        // room timeline, which always outlives its thread panel.
        retryFailedSend(threadTimelineRoomId(roomId), transactionId);
        return;
    }
    if (!timelineActiveFor(roomId) || transactionId.isEmpty())
        return;
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray txnBytes = transactionId.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_retry_send(
        m_rustHandle, roomBytes.constData(), txnBytes.constData()));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

bool RustSdkMatrixClient::timelineActiveFor(const QString &roomId) const
{
    if (isThreadTimelineId(roomId))
        return threadTimelineActiveFor(roomId);
    return m_rustHandle && !roomId.isEmpty()
        && (m_timelineTracker.activeRoom() == roomId
            || m_timelineTracker.requestedRoom() == roomId);
}

bool RustSdkMatrixClient::threadTimelineActiveFor(const QString &timelineId) const
{
    return m_rustHandle && !timelineId.isEmpty()
        && (m_threadTracker.activeRoom() == timelineId
            || m_threadTracker.requestedRoom() == timelineId);
}

bool RustSdkMatrixClient::timelineReadyForPagination(const QString &roomId) const
{
    // A requested room is not yet pagination-ready: the Rust registry's
    // timeline_for() accepts requests only after the initial timeline_reset
    // snapshot has supplied and adopted a live room generation.
    if (isThreadTimelineId(roomId))
        return m_rustHandle && m_threadTracker.readyForPagination(roomId);
    return m_rustHandle && !roomId.isEmpty()
        && m_timelineTracker.readyForPagination(roomId);
}

void RustSdkMatrixClient::openRoomTimeline(const QString &roomId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty())
        return;
    clearThreadTimelineState();
    m_timelineTracker.request(roomId);
    m_pagination.insert(roomId, PaginationState{});
    qCInfo(lcRust) << "timeline open room=" << roomId.right(12);
    const QByteArray roomBytes = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_timeline_open(m_rustHandle, roomBytes.constData()));
    if (!result.isEmpty()) {
        m_timelineTracker.reset();
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
        return;
    }
    Q_EMIT paginationStateChanged(roomId);
}

// ── v0.6.0: SDK-backed thread timelines ─────────────────────────────────

void RustSdkMatrixClient::openThread(const QString &roomId,
                                     const QString &rootEventId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty()
        || rootEventId.isEmpty()) {
        Q_EMIT threadTimelineFailed(roomId, rootEventId,
                                    QStringLiteral("not_ready"));
        return;
    }
    clearThreadTimelineState();
    const QString timelineId = threadTimelineId(roomId, rootEventId);
    m_threadTracker.request(timelineId);
    m_pagination.insert(timelineId, PaginationState{});
    qCInfo(lcRust) << "thread open root=" << rootEventId.right(12);
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray rootBytes = rootEventId.toUtf8();
    const QString result = takeRustString(mx_rust_thread_open(
        m_rustHandle, roomBytes.constData(), rootBytes.constData()));
    if (!result.isEmpty()) {
        m_threadTracker.reset();
        m_pagination.remove(timelineId);
        Q_EMIT threadTimelineFailed(roomId, rootEventId,
                                    QStringLiteral("dispatch_failed"));
        return;
    }
    Q_EMIT paginationStateChanged(timelineId);
}

void RustSdkMatrixClient::closeThread()
{
    if (m_rustHandle && m_threadTracker.hasActiveTimeline())
        qCInfo(lcRust) << "thread close";
    if (m_rustHandle)
        takeRustString(mx_rust_thread_close(m_rustHandle));
    clearThreadTimelineState();
}

void RustSdkMatrixClient::sendThreadReply(const QString &roomId,
                                          const QString &threadRootEventId,
                                          const QString &body)
{
    sendThreadReplyTo(roomId, threadRootEventId, QString{}, body);
}

void RustSdkMatrixClient::sendThreadReplyTo(const QString &roomId,
                                            const QString &threadRootEventId,
                                            const QString &inReplyToEventId,
                                            const QString &body)
{
    sendThreadReplyTo(roomId, threadRootEventId, inReplyToEventId, body,
                      QStringList());
}

void RustSdkMatrixClient::sendThreadReplyTo(const QString &roomId,
                                            const QString &threadRootEventId,
                                            const QString &inReplyToEventId,
                                            const QString &body,
                                            const QStringList &mentionUserIds)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty()
        || threadRootEventId.isEmpty() || body.trimmed().isEmpty()) {
        refuseSend("sendThreadReply");
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray rootBytes = threadRootEventId.toUtf8();
    const QByteArray bodyBytes = body.toUtf8();
    const QByteArray replyBytes = inReplyToEventId.toUtf8();
    const QByteArray mentionBytes =
        mentionUserIds.join(QLatin1Char('\n')).toUtf8();
    const QString result = takeRustString(mx_rust_thread_send_text(
        m_rustHandle, roomBytes.constData(), rootBytes.constData(),
        bodyBytes.constData(),
        inReplyToEventId.isEmpty() ? nullptr : replyBytes.constData(),
        mentionUserIds.isEmpty() ? nullptr : mentionBytes.constData()));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

void RustSdkMatrixClient::queryCryptoHealth()
{
    if (!m_loggedIn || !m_rustHandle)
        return;
    takeRustString(mx_rust_query_crypto_health(m_rustHandle));
}

void RustSdkMatrixClient::requestDeviceList()
{
    if (!m_loggedIn || !m_rustHandle)
        return;
    takeRustString(mx_rust_list_devices(m_rustHandle));
}

void RustSdkMatrixClient::retryDecryption(const QString &roomId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty())
        return;
    // A thread panel retry targets its parent room (both timelines are
    // retried in one Rust pass).
    const QString targetRoom = isThreadTimelineId(roomId)
        ? threadTimelineRoomId(roomId)
        : roomId;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 last = m_lastDecryptionRetryMs.value(targetRoom, 0);
    if (now - last < 2000) {
        qCDebug(lcE2ee) << "manual-retry coalesced" << "room="
                        << matrix::e2ee::redactId(targetRoom);
        return;   // bounded: coalesce rapid repeat requests
    }
    m_lastDecryptionRetryMs.insert(targetRoom, now);
    const QByteArray roomBytes = targetRoom.toUtf8();
    takeRustString(mx_rust_timeline_retry_decryption(
        m_rustHandle, roomBytes.constData()));
    qCInfo(lcRust) << "manual decryption retry dispatched";
    qCDebug(lcE2ee) << "manual-retry dispatched" << "room="
                    << matrix::e2ee::redactId(targetRoom);
}

void RustSdkMatrixClient::openThreadList(const QString &roomId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty())
        return;
    m_threadListRoom = roomId;
    m_threadListGeneration = 0;
    const QByteArray roomBytes = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_thread_list_open(m_rustHandle, roomBytes.constData()));
    if (!result.isEmpty()) {
        m_threadListRoom.clear();
        Q_EMIT threadListUpdated(roomId, {}, true, true);
    }
}

void RustSdkMatrixClient::closeThreadList()
{
    if (m_rustHandle && !m_threadListRoom.isEmpty())
        takeRustString(mx_rust_thread_list_close(m_rustHandle));
    m_threadListRoom.clear();
    m_threadListGeneration = 0;
}

void RustSdkMatrixClient::paginateThreadList(const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty() || roomId != m_threadListRoom)
        return;
    const QByteArray roomBytes = roomId.toUtf8();
    takeRustString(
        mx_rust_thread_list_paginate(m_rustHandle, roomBytes.constData()));
}

void RustSdkMatrixClient::markThreadRead(const QString &roomId,
                                         const QString &rootEventId)
{
    if (!m_rustHandle || roomId.isEmpty() || rootEventId.isEmpty())
        return;
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray rootBytes = rootEventId.toUtf8();
    takeRustString(mx_rust_thread_mark_read(
        m_rustHandle, roomBytes.constData(), rootBytes.constData()));
}

void RustSdkMatrixClient::queryThreadSubscription(const QString &roomId,
                                                  const QString &rootEventId)
{
    if (!m_rustHandle || roomId.isEmpty() || rootEventId.isEmpty())
        return;
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray rootBytes = rootEventId.toUtf8();
    takeRustString(mx_rust_thread_subscription_query(
        m_rustHandle, roomBytes.constData(), rootBytes.constData()));
}

void RustSdkMatrixClient::setThreadSubscribed(const QString &roomId,
                                              const QString &rootEventId,
                                              bool subscribed)
{
    if (!m_rustHandle || roomId.isEmpty() || rootEventId.isEmpty())
        return;
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray rootBytes = rootEventId.toUtf8();
    takeRustString(mx_rust_thread_set_subscribed(
        m_rustHandle, roomBytes.constData(), rootBytes.constData(),
        subscribed ? 1 : 0));
}

void RustSdkMatrixClient::handleThreadListReset(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    if (roomId != m_threadListRoom)
        return;   // stale: list view moved to another room (or closed)
    const auto generation = static_cast<quint64>(
        event.value(QStringLiteral("thread_list_generation")).toDouble(0));
    if (generation < m_threadListGeneration)
        return;
    m_threadListGeneration = generation;

    if (event.value(QStringLiteral("type")).toString()
        == QLatin1String("thread_list_error")) {
        Q_EMIT threadListUpdated(roomId, {}, true, true);
        return;
    }

    QVariantList threads;
    const QJsonArray items = event.value(QStringLiteral("items")).toArray();
    for (const auto &value : items) {
        const QJsonObject obj = value.toObject();
        QVariantMap entry;
        entry.insert(QStringLiteral("rootEventId"),
                     obj.value(QStringLiteral("root_event_id")).toString());
        entry.insert(QStringLiteral("rootSender"),
                     obj.value(QStringLiteral("root_sender")).toString());
        entry.insert(QStringLiteral("rootSenderName"),
                     obj.value(QStringLiteral("root_sender_name")).toString(
                         matrix::user_lookup::localpartOrUserId(
                             obj.value(QStringLiteral("root_sender"))
                                 .toString())));
        entry.insert(QStringLiteral("rootPreview"),
                     obj.value(QStringLiteral("root_preview")).toString());
        entry.insert(QStringLiteral("rootTimestamp"),
                     timestampFromMs(static_cast<qint64>(
                         obj.value(QStringLiteral("root_timestamp_ms"))
                             .toDouble(0))));
        entry.insert(QStringLiteral("replyCount"),
                     obj.value(QStringLiteral("reply_count")).toInt(0));
        entry.insert(QStringLiteral("latestSender"),
                     obj.value(QStringLiteral("latest_sender")).toString());
        entry.insert(QStringLiteral("latestSenderName"),
                     obj.value(QStringLiteral("latest_sender_name")).toString(
                         matrix::user_lookup::localpartOrUserId(
                             obj.value(QStringLiteral("latest_sender"))
                                 .toString())));
        entry.insert(QStringLiteral("latestPreview"),
                     obj.value(QStringLiteral("latest_preview")).toString());
        entry.insert(QStringLiteral("latestTimestamp"),
                     timestampFromMs(static_cast<qint64>(
                         obj.value(QStringLiteral("latest_timestamp_ms"))
                             .toDouble(0))));
        threads.append(entry);
    }
    Q_EMIT threadListUpdated(
        roomId, threads,
        event.value(QStringLiteral("end_reached")).toBool(false),
        event.value(QStringLiteral("failed")).toBool(false));
}

void RustSdkMatrixClient::handleThreadSubscriptionEvent(const QString &type,
                                                        const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const QString rootId =
        event.value(QStringLiteral("thread_root_id")).toString();
    if (type == QLatin1String("thread_subscription_state")) {
        Q_EMIT threadSubscriptionState(
            roomId, rootId,
            event.value(QStringLiteral("supported")).toBool(false),
            event.value(QStringLiteral("subscribed")).toBool(false),
            event.value(QStringLiteral("automatic")).toBool(false));
    } else {
        Q_EMIT threadSubscriptionResult(
            roomId, rootId, event.value(QStringLiteral("ok")).toBool(false),
            event.value(QStringLiteral("subscribed")).toBool(false));
    }
}

void RustSdkMatrixClient::clearThreadTimelineState()
{
    const QString requested = m_threadTracker.requestedRoom();
    const QString active = m_threadTracker.activeRoom();
    for (const QString &timelineId : { requested, active }) {
        if (timelineId.isEmpty())
            continue;
        m_timelines.remove(timelineId);
        m_pagination.remove(timelineId);
    }
    m_threadTracker.reset();
}

void RustSdkMatrixClient::handleThreadReset(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const QString rootId =
        event.value(QStringLiteral("thread_root_id")).toString();
    const QString timelineId = threadTimelineId(roomId, rootId);
    const auto generation = static_cast<quint64>(
        event.value(QStringLiteral("thread_generation")).toDouble(0));
    if (!m_threadTracker.adoptReset(timelineId, generation)) {
        qCInfo(lcRust) << "thread stale reset ignored generation="
                       << generation;
        return;
    }
    const QJsonArray items = event.value(QStringLiteral("items")).toArray();
    m_timelines[timelineId] =
        matrix::rust_timeline::eventsFromItemArray(items, timelineId);
    qCInfo(lcRust) << "thread subscription started"
                   << "thread_generation=" << generation
                   << "items=" << m_timelines[timelineId].size();
    Q_EMIT timelineReset(timelineId);
    Q_EMIT paginationStateChanged(timelineId);
}

void RustSdkMatrixClient::handleThreadDiff(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const QString rootId =
        event.value(QStringLiteral("thread_root_id")).toString();
    const QString timelineId = threadTimelineId(roomId, rootId);
    const auto generation = static_cast<quint64>(
        event.value(QStringLiteral("thread_generation")).toDouble(0));
    if (!m_threadTracker.accepts(timelineId, generation)) {
        qCInfo(lcRust) << "thread stale diff ignored generation="
                       << generation;
        return;
    }

    using matrix::rust_timeline::DiffOutcome;
    auto &mirror = m_timelines[timelineId];
    const DiffOutcome outcome =
        matrix::rust_timeline::applyTimelineDiff(mirror, event, timelineId);

    switch (outcome.kind) {
    case DiffOutcome::Appended:
        for (const auto &item : outcome.items)
            Q_EMIT eventAppended(timelineId, item);
        break;
    case DiffOutcome::Prepended:
        Q_EMIT eventsPrepended(timelineId, outcome.items);
        break;
    case DiffOutcome::Inserted:
        Q_EMIT eventInsertedAt(timelineId, outcome.index, outcome.items.first());
        break;
    case DiffOutcome::Changed:
        Q_EMIT eventChangedAt(timelineId, outcome.index, outcome.items.first());
        break;
    case DiffOutcome::Removed:
        Q_EMIT eventRemovedAt(timelineId, outcome.index);
        break;
    case DiffOutcome::Cleared:
    case DiffOutcome::Reset:
        Q_EMIT timelineReset(timelineId);
        break;
    case DiffOutcome::Truncated:
        Q_EMIT eventsTruncatedTo(timelineId, outcome.length);
        break;
    case DiffOutcome::Invalid:
        // Never apply a malformed/stale thread diff; recover with one fresh
        // snapshot of the same thread. No message bodies in this log line.
        qCWarning(lcRust) << "thread invalid diff rejected"
                          << "op=" << event.value(QStringLiteral("op")).toString()
                          << "mirror_size=" << mirror.size();
        openThread(roomId, rootId);
        break;
    }
}

void RustSdkMatrixClient::handleThreadPagination(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const QString rootId =
        event.value(QStringLiteral("thread_root_id")).toString();
    const QString timelineId = threadTimelineId(roomId, rootId);
    const auto generation = static_cast<quint64>(
        event.value(QStringLiteral("thread_generation")).toDouble(0));
    if (!m_threadTracker.accepts(timelineId, generation)) {
        qCInfo(lcRust) << "thread stale pagination ignored generation="
                       << generation;
        return;
    }
    auto &state = m_pagination[timelineId];
    const QString paginationState =
        event.value(QStringLiteral("state")).toString();
    if (paginationState == QLatin1String("loading")) {
        state.loading = true;
        state.failed = false;
        state.failureTransient = false;
    } else if (paginationState == QLatin1String("idle")) {
        state.loading = false;
        state.failed = false;
        state.failureTransient = false;
        state.reachedStart =
            event.value(QStringLiteral("reached_start")).toBool(false);
    } else if (paginationState == QLatin1String("failed")) {
        state.loading = false;
        state.failed = true;
        const QString category =
            event.value(QStringLiteral("category")).toString();
        state.failureTransient = category == QLatin1String("network")
            || category == QLatin1String("not_ready");
        qCWarning(lcRust) << "thread pagination failed category=" << category;
    }
    Q_EMIT paginationStateChanged(timelineId);
}

void RustSdkMatrixClient::handleThreadError(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const QString rootId =
        event.value(QStringLiteral("thread_root_id")).toString();
    const QString timelineId = threadTimelineId(roomId, rootId);
    const QString category = event.value(QStringLiteral("category"))
                                 .toString(QStringLiteral("unknown"));
    qCWarning(lcRust) << "thread error category=" << category;
    // Only the currently requested/active thread may surface the failure.
    if (m_threadTracker.requestedRoom() == timelineId
        || m_threadTracker.activeRoom() == timelineId) {
        clearThreadTimelineState();
        Q_EMIT threadTimelineFailed(roomId, rootId, category);
    }
}

void RustSdkMatrixClient::handleThreadClosed(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const QString rootId =
        event.value(QStringLiteral("thread_root_id")).toString();
    const QString timelineId = threadTimelineId(roomId, rootId);
    // Drop mirror state for the closed thread only; a newer thread may
    // already have been requested (its id differs, so it is untouched).
    m_timelines.remove(timelineId);
    m_pagination.remove(timelineId);
    if (m_threadTracker.activeRoom() == timelineId
        || m_threadTracker.requestedRoom() == timelineId)
        m_threadTracker.reset();
    qCInfo(lcRust) << "thread subscription stopped";
}

void RustSdkMatrixClient::closeRoomTimeline()
{
    // A thread panel / Threads view never survives its room: Rust closes
    // them as part of timeline close/open; the C++ mirrors drop immediately.
    clearThreadTimelineState();
    m_threadListRoom.clear();
    m_threadListGeneration = 0;
    if (m_rustHandle && m_timelineTracker.hasActiveTimeline()) {
        const QString room = m_timelineTracker.activeRoom();
        takeRustString(mx_rust_timeline_close(m_rustHandle));
        qCInfo(lcRust) << "timeline close room=" << room.right(12);
    }
    m_timelineTracker.reset();
}

void RustSdkMatrixClient::refuseSend(const char *op)
{
    Q_EMIT errorOccurred(tr("Rust SDK backend does not implement %1 yet.").arg(QLatin1String(op)));
}

void RustSdkMatrixClient::pollRustEvents()
{
    if (!m_rustHandle)
        return;

    const quint64 eventGeneration = m_handleGeneration;

    // v0.7 defense-in-depth: drain the TERMINAL command lane completely
    // before the bounded bulk batch, so media/GIF results can never be
    // starved (or dropped) behind a timeline-diff flood. The lane's
    // population is bounded by the C++ in-flight discipline, so "fully"
    // is a handful of events; 256 is a defensive iteration cap only.
    for (int i = 0; i < 256; ++i) {
        const QString raw =
            takeRustString(mx_rust_poll_command_event(m_rustHandle));
        if (raw.isEmpty())
            break;
        const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
        if (!doc.isObject()) {
            qCWarning(lcRust) << "discarding malformed Rust SDK command event";
            continue;
        }
        const QJsonObject event = doc.object();
        if (m_lifecycle.acceptsActive(eventGeneration)) {
            handleRustEvent(event, eventGeneration);
        } else {
            qCInfo(lcRust) << "ignored stale command callback"
                           << "type="
                           << event.value(QStringLiteral("type")).toString();
        }
    }

    for (int i = 0; i < 64; ++i) {
        const QString raw = takeRustString(mx_rust_poll_event(m_rustHandle));
        if (raw.isEmpty())
            break;

        const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
        if (!doc.isObject()) {
            qCWarning(lcRust) << "discarding malformed Rust SDK event";
            continue;
        }
        const QJsonObject event = doc.object();
        const QString type = event.value(QStringLiteral("type")).toString();
        if (m_lifecycle.acceptsActive(eventGeneration)) {
            handleRustEvent(event, eventGeneration);
            continue;
        }
        if (type == QLatin1String("logged_out")
            && m_lifecycle.acceptsShutdownCompletion(eventGeneration)) {
            handleRustEvent(event, eventGeneration);
            return; // finishSignOut releases the handle being polled.
        }

        qCInfo(lcRust) << "ignored stale callback"
                       << "type=" << type
                       << "generation=" << eventGeneration
                       << "active_generation=" << m_lifecycle.activeGeneration();
    }
}

void RustSdkMatrixClient::requireLocalReset(const QString &reasonCode)
{
    qCWarning(lcRust) << "local session reset required reason=" << reasonCode;
    Q_EMIT localSessionResetRequired(reasonCode);
}

void RustSdkMatrixClient::finishSignOut(const QString &serverResult,
                                        const QString &serverMessage)
{
    const auto identity = m_signOutIdentity;
    if (serverResult == QLatin1String("already_invalid")) {
        qCInfo(lcRust) << "rust server logout result=already_logged_out";
    } else if (serverResult == QLatin1String("failed")) {
        // Safe diagnostic only. Local cleanup remains authoritative and the
        // user is intentionally signing out, so this is not a fatal UI error.
        qCWarning(lcRust) << "rust server logout result=failed"
                          << "message=" << serverMessage;
    } else {
        qCInfo(lcRust) << "rust server logout result=" << serverResult;
    }

    releaseRustHandle();
    clearLocalState();
    const bool sessionOk = identity.isValid() && clearPersistedAccount(identity);
    const auto files = identity.isValid()
        ? matrix::app_data::removeAccountRustState(identity)
        : matrix::app_data::RemovalSummary{0, 0, 1};
    const bool ok = sessionOk && files.ok();

    qCInfo(lcRust) << "rust local sign-out cleanup"
                   << "slug=" << identity.slug
                   << "session=" << (sessionOk ? "ok" : "failed")
                   << "store_and_sidecars=" << (files.ok() ? "ok" : "failed")
                   << "deleted=" << files.deleted
                   << "missing=" << files.missing
                   << "failed=" << files.failed;

    m_storePath.clear();
    m_signOutIdentity = {};
    m_signOutDeviceId.clear();
    m_lifecycle.finishSignOut();

    Q_EMIT loggedOut();
    if (ok) {
        Q_EMIT localSessionCleanupFinished(
            true, tr("Local Lightning session reset. You can sign in again."));
    } else {
        requireLocalReset(QStringLiteral("cleanup_incomplete"));
        const QString failure = tr(
            "Lightning could not completely reset the local session for this "
            "account. Check the application logs and filesystem permissions, "
            "then try again.");
        Q_EMIT localSessionCleanupFinished(false, failure);
        Q_EMIT errorOccurred(failure);
    }
}

void RustSdkMatrixClient::handleRustEvent(const QJsonObject &event,
                                          quint64 eventGeneration)
{
    if (!m_lifecycle.acceptsActive(eventGeneration)
        && !m_lifecycle.acceptsShutdownCompletion(eventGeneration)) {
        qCInfo(lcRust) << "ignored stale callback"
                       << "generation=" << eventGeneration
                       << "active_generation=" << m_lifecycle.activeGeneration();
        return;
    }

    const QString type = event.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("status")) {
        const QString state = event.value(QStringLiteral("state")).toString();
        if (state == QLatin1String("connecting"))
            setState(Connecting);
        else if (state == QLatin1String("syncing"))
            setState(Syncing);
        else if (state == QLatin1String("disconnected"))
            setState(Disconnected);
        else if (state == QLatin1String("error"))
            setState(Error);
        return;
    }

    if (type == QLatin1String("login_ok")) {
        m_freshLoginIdentity = {};
        // SENSITIVE: this event object carries `access_token`. Never pass
        // `event` or the extracted `accessToken` to a log stream. The token
        // must flow only into SecretStore-backed SettingsManager::saveSession
        // and then be forgotten locally. No qCDebug / qCInfo of `event` here.
        matrix::app_data::AccountIdentity identity;
        if (matrix::app_data::resolveAccountIdentity(
                event.value(QStringLiteral("homeserver")).toString(m_homeserver),
                event.value(QStringLiteral("user_id")).toString(m_userId),
                &identity)) {
            m_homeserver = identity.homeserver;
        }
        m_userId = event.value(QStringLiteral("user_id")).toString(m_userId);
        m_deviceId = event.value(QStringLiteral("device_id")).toString(m_deviceId);
        m_loggedIn = !m_userId.isEmpty();
        const QString accessToken = event.value(QStringLiteral("access_token")).toString();
        if (m_loggedIn && m_settings && !accessToken.isEmpty()) {
            m_settings->saveSession(m_homeserver, m_userId, m_deviceId, accessToken);
            m_settings->setSyncToken({});
        }
        setState(Disconnected);
        if (m_loggedIn)
            Q_EMIT loginSucceeded(m_userId);
        else
            Q_EMIT loginFailed(tr("Rust SDK login response did not include a user id."));
        return;
    }

    if (type == QLatin1String("login_failed")) {
        m_loggedIn = false;
        setState(Error);
        // A failed fresh-store login must not leave a half-initialised
        // store directory behind — that is exactly what used to poison
        // every later attempt for this account. Release the handle first
        // so no SDK task still owns the store files.
        if (m_freshLoginIdentity.isValid()) {
            const auto identity = m_freshLoginIdentity;
            m_freshLoginIdentity = {};
            releaseRustHandle();
            const auto removed =
                matrix::app_data::removeAccountRustState(identity);
            qCInfo(lcRust) << "cleaned fresh store after failed login"
                           << "slug=" << identity.slug
                           << "deleted=" << removed.deleted
                           << "failed=" << removed.failed;
        }
        const QString message = event.value(QStringLiteral("message")).toString(
            tr("Rust SDK login failed."));
        if (matrix::rust_session::isStoreOwnershipMismatch(message)) {
            requireLocalReset(QStringLiteral("sdk_store_ownership_mismatch"));
            Q_EMIT loginFailed(tr(
                "This local Lightning Rust SDK store belongs to a different "
                "Matrix session or device. Reset the local Lightning session "
                "for this account, then sign in again. This does not delete "
                "server messages or Element data."));
        } else {
            Q_EMIT loginFailed(message);
        }
        return;
    }

    if (type == QLatin1String("logged_out")) {
        finishSignOut(event.value(QStringLiteral("result")).toString(
                          QStringLiteral("ok")),
                      event.value(QStringLiteral("message")).toString());
        return;
    }

    if (type == QLatin1String("rooms") || type == QLatin1String("room_list_reset")) {
        handleRoomsEvent(event.value(QStringLiteral("rooms")).toArray());
        return;
    }

    if (type.startsWith(QLatin1String("room_list_"))
        && type != QLatin1String("room_list_mode")
        && type != QLatin1String("room_list_sync_state")
        && type != QLatin1String("room_list_error")) {
        handleRoomListDiff(event);
        return;
    }

    if (type == QLatin1String("room_list_mode")) {
        const QString mode = event.value(QStringLiteral("mode")).toString();
        if (!mode.isEmpty() && mode != m_syncMode) {
            m_syncMode = mode;
            qCInfo(lcRust) << "room_list mode=" << mode;
            Q_EMIT syncModeChanged();
        }
        return;
    }

    if (type == QLatin1String("room_list_sync_state")) {
        const QString state = event.value(QStringLiteral("state")).toString();
        // v0.5.8: the classic path re-announces "running" on every /sync
        // callback. Collapse consecutive identical states so the log and
        // downstream handling see each transition once. setState() is
        // already idempotent; distinct transitions (running → offline →
        // retrying → running) are never coalesced, so reconnect is intact.
        if (state == m_lastSyncState)
            return;
        m_lastSyncState = state;
        qCInfo(lcRust) << "sync state=" << state;
        if (state == QLatin1String("offline")) setState(Offline);
        else if (state == QLatin1String("starting") || state == QLatin1String("retrying"))
            setState(Syncing);
        else if (state == QLatin1String("running")) setState(Syncing);
        return;
    }

    if (type == QLatin1String("room_list_error")) {
        const QString category = event.value(QStringLiteral("category")).toString();
        if (category == QLatin1String("authentication")) {
            setState(Error);
            Q_EMIT errorOccurred(tr("Matrix session is no longer authorized."));
        }
        return;
    }

    if (type == QLatin1String("space_list_reset")) {
        handleSpacesEvent(event.value(QStringLiteral("spaces")).toArray());
        return;
    }

    if (type == QLatin1String("typing_update")) {
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        auto room = m_rooms.find(roomId);
        if (room == m_rooms.end()) return;
        QStringList users;
        for (const auto &entry : event.value(QStringLiteral("users")).toArray()) {
            const auto object = entry.toObject();
            const QString userId = object.value(QStringLiteral("user_id")).toString();
            if (userId.isEmpty() || userId == m_userId) continue;
            users.append(userId);
            const QString displayName = object.value(QStringLiteral("display_name")).toString();
            if (!displayName.isEmpty()) {
                auto member = room->members.value(userId);
                member.userId = userId;
                member.displayName = displayName;
                room->members.insert(userId, member);
            }
        }
        room->typingUserIds = users;
        Q_EMIT typingChanged(roomId);
        return;
    }

    if (type == QLatin1String("invite_state_update")) {
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        auto room = m_rooms.find(roomId);
        if (room == m_rooms.end()) return;
        const QString state = event.value(QStringLiteral("state")).toString();
        room->invitePending = state == QLatin1String("pending");
        room->inviteError = state == QLatin1String("failed")
            ? tr("Invite action failed. Try again.") : QString{};
        Q_EMIT roomUpdated(roomId);
        return;
    }

    if (type == QLatin1String("room_action_error")) {
        const QString action = event.value(QStringLiteral("action")).toString();
        if (action == QLatin1String("read_receipt"))
            m_lastReceiptSent.remove(event.value(QStringLiteral("room_id")).toString());
        qCWarning(lcRust) << "room action failed category=" << action;
        return;
    }

    if (type == QLatin1String("initial_sync_done")) {
        setInitialSyncDone(true);
        // v0.5.9: fetch the server upload limit once per session so the
        // composer can enforce the real m.upload.size before dispatching.
        if (!m_uploadLimitRequested && m_rustHandle) {
            m_uploadLimitRequested = true;
            takeRustString(mx_rust_fetch_upload_limit(m_rustHandle));
        }
        return;
    }

    if (type == QLatin1String("timeline_event")) {
        handleTimelineEvent(event);
        return;
    }

    // v0.5.7 live SDK timeline events.
    if (type == QLatin1String("timeline_reset")) {
        handleTimelineReset(event);
        return;
    }
    if (type == QLatin1String("timeline_diff")) {
        handleTimelineDiff(event);
        return;
    }
    if (type == QLatin1String("timeline_pagination")) {
        handleTimelinePagination(event);
        return;
    }
    // v0.6.0: SDK-backed thread timeline events.
    if (type == QLatin1String("thread_reset")) {
        handleThreadReset(event);
        return;
    }
    if (type == QLatin1String("thread_diff")) {
        handleThreadDiff(event);
        return;
    }
    if (type == QLatin1String("thread_pagination")) {
        handleThreadPagination(event);
        return;
    }
    if (type == QLatin1String("thread_error")) {
        handleThreadError(event);
        return;
    }
    if (type == QLatin1String("thread_closed")) {
        handleThreadClosed(event);
        return;
    }
    if (type == QLatin1String("device_list")) {
        QVariantList devices;
        const QJsonArray items = event.value(QStringLiteral("devices")).toArray();
        for (const auto &value : items) {
            const QJsonObject obj = value.toObject();
            QVariantMap entry;
            entry.insert(QStringLiteral("deviceId"),
                         obj.value(QStringLiteral("device_id")).toString());
            entry.insert(QStringLiteral("displayName"),
                         obj.value(QStringLiteral("display_name")).toString());
            const auto ts = static_cast<qint64>(
                obj.value(QStringLiteral("last_seen_ts")).toDouble(0));
            entry.insert(QStringLiteral("lastSeen"),
                         ts > 0 ? QDateTime::fromMSecsSinceEpoch(ts, Qt::UTC)
                                : QDateTime{});
            entry.insert(QStringLiteral("lastSeenIp"),
                         obj.value(QStringLiteral("last_seen_ip")).toString());
            entry.insert(QStringLiteral("isCurrent"),
                         obj.value(QStringLiteral("is_current")).toBool(false));
            entry.insert(QStringLiteral("hasCryptoIdentity"),
                         obj.value(QStringLiteral("has_crypto_identity"))
                             .toBool(false));
            entry.insert(QStringLiteral("verified"),
                         obj.value(QStringLiteral("verified")).toBool(false));
            entry.insert(QStringLiteral("crossSigned"),
                         obj.value(QStringLiteral("cross_signed")).toBool(false));
            devices.append(entry);
        }
        Q_EMIT deviceListUpdated(
            event.value(QStringLiteral("ok")).toBool(false), devices);
        return;
    }
    if (type == QLatin1String("crypto_health")) {
        // Forward verbatim (already sanitized in Rust); AppController stamps
        // the generation before the model adopts it.
        QVariantMap snapshot = event.toVariantMap();
        snapshot.remove(QStringLiteral("type"));
        Q_EMIT cryptoHealthUpdated(snapshot);
        return;
    }
    if (type == QLatin1String("crypto_bootstrap")) {
        // Sanitized observer state (the poll layer already rejected stale
        // session handles; AppController resets the model per session).
        Q_EMIT cryptoBootstrapEvent(
            event.value(QStringLiteral("kind")).toString(),
            event.value(QStringLiteral("state")).toString(),
            static_cast<quint64>(
                event.value(QStringLiteral("count")).toDouble(0)));
        return;
    }
    if (type == QLatin1String("thread_list_reset")
        || type == QLatin1String("thread_list_error")) {
        handleThreadListReset(event);
        return;
    }
    if (type == QLatin1String("thread_subscription_state")
        || type == QLatin1String("thread_subscription_result")) {
        handleThreadSubscriptionEvent(type, event);
        return;
    }
    if (type == QLatin1String("thread_send_failed")) {
        qCWarning(lcRust) << "thread send state=failed category="
                          << event.value(QStringLiteral("category")).toString();
        Q_EMIT errorOccurred(tr("The thread reply could not be sent."));
        return;
    }
    if (type == QLatin1String("timeline_retry_decryption")) {
        handleTimelineRetryDecryption(event);
        return;
    }
    if (type == QLatin1String("timeline_send_failed")) {
        const QString category =
            event.value(QStringLiteral("category")).toString(
                QStringLiteral("rejected"));
        qCWarning(lcRust) << "timeline send state=failed category=" << category;
        Q_EMIT errorOccurred(tr("Message could not be sent. You can retry "
                                "from the message's Retry action."));
        return;
    }
    if (type == QLatin1String("timeline_error")) {
        const QString category =
            event.value(QStringLiteral("category")).toString(
                QStringLiteral("unknown"));
        qCWarning(lcRust) << "timeline error category=" << category;
        if (category != QLatin1String("unknown_room")) {
            Q_EMIT errorOccurred(tr("The room timeline could not be opened."));
        }
        return;
    }
    if (type == QLatin1String("timeline_closed")
        || type == QLatin1String("timeline_shutdown")) {
        qCInfo(lcRust) << "timeline subscription stopped"
                       << "kind=" << type;
        return;
    }

    if (type == QLatin1String("send_ok")) {
        handleSendOk(event);
        return;
    }

    if (type == QLatin1String("send_failed")) {
        handleSendFailed(event);
        return;
    }

    if (type == QLatin1String("encrypted_send_ok")) {
        handleEncryptedSendOk(event);
        return;
    }

    if (type == QLatin1String("encrypted_send_failed")) {
        handleEncryptedSendFailed(event);
        return;
    }

    if (type == QLatin1String("key_backup_status")) {
        const QString state = event.value(QStringLiteral("state")).toString();
        const QString message = event.value(QStringLiteral("message")).toString();
        Q_EMIT keyBackupResult(state, message);
        return;
    }

    if (type == QLatin1String("reload_timeline_done")) {
        Q_EMIT roomTimelineReloaded(
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("events")).toInt(0),
            event.value(QStringLiteral("decrypted")).toInt(0),
            event.value(QStringLiteral("undecryptable")).toInt(0));
        return;
    }

    if (type == QLatin1String("reload_timeline_failed")) {
        Q_EMIT errorOccurred(tr("Reload timeline failed: %1").arg(
            event.value(QStringLiteral("message")).toString(
                tr("Matrix Rust SDK error."))));
        return;
    }

    if (type == QLatin1String("verification_request_received")) {
        Q_EMIT verificationRequestReceived(
            event.value(QStringLiteral("flow_id")).toString(),
            event.value(QStringLiteral("other_user_id")).toString(),
            event.value(QStringLiteral("other_device_id")).toString(),
            event.value(QStringLiteral("is_self_verification")).toBool(false));
        return;
    }
    if (type == QLatin1String("verification_request_started")) {
        Q_EMIT verificationRequestStarted(
            event.value(QStringLiteral("flow_id")).toString(),
            event.value(QStringLiteral("other_user_id")).toString(),
            event.value(QStringLiteral("is_self_verification")).toBool(true));
        return;
    }
    if (type == QLatin1String("room_key_import_started")) {
        Q_EMIT roomKeyImportStarted();
        return;
    }
    if (type == QLatin1String("room_key_import_progress")) {
        Q_EMIT roomKeyImportProgress(
            event.value(QStringLiteral("imported")).toInt(0),
            event.value(QStringLiteral("total")).toInt(0));
        return;
    }
    if (type == QLatin1String("room_key_import_done")) {
        QStringList roomIds;
        for (const auto &v : event.value(QStringLiteral("room_ids")).toArray()) {
            const QString id = v.toString();
            if (!id.isEmpty()) roomIds.append(id);
        }
        const int imported = event.value(QStringLiteral("imported")).toInt(0);
        const int total = event.value(QStringLiteral("total")).toInt(0);
        const int affected = event.value(QStringLiteral("affected_rooms"))
                                 .toInt(roomIds.size());
        qCInfo(lcRust) << "room key import completed"
                       << "imported=" << imported
                       << "total=" << total
                       << "affected_rooms=" << affected;
        Q_EMIT roomKeyImportDone(imported, total, affected, roomIds);
        return;
    }
    if (type == QLatin1String("room_key_import_failed")) {
        const QString category =
            event.value(QStringLiteral("category")).toString(
                QStringLiteral("import_failed"));
        // Never log the raw message — categorized only.
        qCWarning(lcRust) << "room key import failed category=" << category;
        Q_EMIT roomKeyImportFailed(
            category,
            event.value(QStringLiteral("message")).toString(
                tr("Room-key import failed.")));
        return;
    }
    if (type == QLatin1String("verification_sas_ready")) {
        QVariantList emojis;
        for (const auto &v : event.value(QStringLiteral("emojis")).toArray()) {
            QVariantMap m;
            m.insert(QStringLiteral("symbol"),
                     v.toObject().value(QStringLiteral("symbol")).toString());
            m.insert(QStringLiteral("description"),
                     v.toObject().value(QStringLiteral("description")).toString());
            emojis.append(m);
        }
        QVariantList decimals;
        for (const auto &v : event.value(QStringLiteral("decimals")).toArray())
            decimals.append(v.toInt());
        Q_EMIT verificationSasReady(
            event.value(QStringLiteral("flow_id")).toString(),
            emojis, decimals);
        return;
    }
    if (type == QLatin1String("verification_sas_confirmed")) {
        // v0.7.1: our confirmation registered; waiting for the peer's.
        Q_EMIT verificationSasConfirmed(
            event.value(QStringLiteral("flow_id")).toString());
        return;
    }
    if (type == QLatin1String("verification_done")) {
        Q_EMIT verificationDone(
            event.value(QStringLiteral("flow_id")).toString());
        return;
    }
    if (type == QLatin1String("verification_cancelled")) {
        Q_EMIT verificationCancelled(
            event.value(QStringLiteral("flow_id")).toString(),
            event.value(QStringLiteral("message")).toString());
        return;
    }
    if (type == QLatin1String("verification_failed")) {
        Q_EMIT verificationFailed(
            event.value(QStringLiteral("flow_id")).toString(),
            event.value(QStringLiteral("message")).toString());
        return;
    }
    if (type == QLatin1String("verification_ready")) {
        // Surfacing this is what lets the UI distinguish "the peer has not
        // answered yet" from "the handshake is running". Dropping it meant
        // an accepted request looked identical to an unanswered one until
        // the emoji arrived — or, on a stall, forever.
        Q_EMIT verificationReady(
            event.value(QStringLiteral("flow_id")).toString());
        return;
    }
    // verification_sas_started is informational — the sas_ready / done /
    // cancelled path carries every state the UI acts on. Ignore.
    if (type == QLatin1String("verification_sas_started"))
        return;

    if (type == QLatin1String("sync_error")) {
        // This branch is reachable only for the active generation. Shutdown
        // callbacks were rejected in pollRustEvents, so M_UNKNOWN_TOKEN keeps
        // its real error semantics for a live signed-in session.
        setState(Error);
        Q_EMIT errorOccurred(event.value(QStringLiteral("message")).toString(
            tr("Rust SDK sync failed.")));
        return;
    }

    // v0.5.9 room-management / user-search / media command results.
    if (handleRoomCommandEvent(type, event))
        return;

    if (type == QLatin1String("error")) {
        Q_EMIT errorOccurred(event.value(QStringLiteral("message")).toString(
            tr("Rust SDK backend error.")));
        return;
    }

    if (type == QLatin1String("queue_overflow")) {
        // Rust dropped events because the poll timer stalled. Surface once
        // as an error banner so users know some events were lost; do not
        // treat as fatal.
        qCWarning(lcRust) << event.value(QStringLiteral("message")).toString();
        Q_EMIT errorOccurred(event.value(QStringLiteral("message")).toString(
            tr("Rust SDK event queue overflowed.")));
    }
}

void RustSdkMatrixClient::handleRoomsEvent(const QJsonArray &rooms)
{
    QHash<QString, RoomInfo> nextRooms;
    nextRooms.reserve(rooms.size());
    QStringList nextOrder;
    QSet<QString> seen;

    for (const auto &value : rooms) {
        const QJsonObject obj = value.toObject();
        RoomInfo room = roomInfoFromJson(obj);
        if (room.id.isEmpty() || seen.contains(room.id)) continue;
        seen.insert(room.id);
        nextRooms.insert(room.id, room);
        nextOrder.append(room.id);
    }
    m_rooms = nextRooms;
    m_roomOrder = nextOrder;
    Q_EMIT roomsChanged();
}

RoomInfo RustSdkMatrixClient::roomInfoFromJson(const QJsonObject &obj) const
{
    const QString id = obj.value(QStringLiteral("id")).toString();
    RoomInfo room = m_rooms.value(id);
    room.id = id;
    room.name = obj.value(QStringLiteral("name")).toString(room.name);
    if (room.name.isEmpty()) room.name = room.id;
    room.topic = obj.value(QStringLiteral("topic")).toString(room.topic);
    room.canonicalAlias = obj.value(QStringLiteral("canonical_alias")).toString(room.canonicalAlias);
    room.avatarUrl = obj.value(QStringLiteral("avatar_url")).toString(room.avatarUrl);
    // A present-but-empty preview must not clobber one we already learned
    // from the open timeline or a live event: Rust legitimately sends ""
    // whenever the SDK has no latest event for the room yet, and room-list
    // set/insert diffs arrive on every unread/order change — pre-0.7 this
    // raced previews back to empty until the room was reopened.
    {
        // The Rust latest-event path sends plain text (typed summaries are
        // built Rust-side); normalization still guards legacy multi-line
        // bodies and mention markdown.
        const QString incomingPreview = matrix::preview::normalizePreviewText(
            obj.value(QStringLiteral("last_message_preview")).toString());
        if (!incomingPreview.isEmpty())
            room.lastMessagePreview = incomingPreview;
    }
    const auto activity = timestampFromMs(static_cast<qint64>(
        obj.value(QStringLiteral("last_activity_ms")).toDouble(0)));
    if (activity.isValid()) room.lastActivity = activity;
    room.unreadCount = obj.value(QStringLiteral("unread_count")).toInt(room.unreadCount);
    room.highlightCount = obj.value(QStringLiteral("highlight_count")).toInt(room.highlightCount);
    room.markedUnread = obj.value(QStringLiteral("marked_unread")).toBool(room.markedUnread);
    room.hasUnreadMessages = obj.value(QStringLiteral("has_unread_messages"))
                                 .toBool(room.hasUnreadMessages || room.unreadCount > 0);
    room.encrypted = obj.value(QStringLiteral("encrypted")).toBool(room.encrypted);
    room.isSpace = obj.value(QStringLiteral("is_space")).toBool(room.isSpace);
    room.isDirect = obj.value(QStringLiteral("is_direct")).toBool(false);
    room.directUserId = obj.value(QStringLiteral("direct_user_id")).toString();
    room.directUserIds.clear();
    for (const auto &value : obj.value(QStringLiteral("direct_user_ids")).toArray())
        room.directUserIds.append(value.toString());
    room.roomType = obj.value(QStringLiteral("room_type")).toString();
    room.prevBatchToken = obj.value(QStringLiteral("prev_batch")).toString(room.prevBatchToken);
    room.inviterUserId = obj.value(QStringLiteral("inviter_user_id")).toString();
    room.inviterDisplayName = obj.value(QStringLiteral("inviter_display_name")).toString();
    const QString membership = obj.value(QStringLiteral("membership")).toString(
        QStringLiteral("joined"));
    room.membership = membership == QLatin1String("invited") ? RoomInfo::Invited
        : membership == QLatin1String("knocked") ? RoomInfo::Knocked
        : membership == QLatin1String("left") ? RoomInfo::Left : RoomInfo::Joined;
    return room;
}

void RustSdkMatrixClient::handleRoomListDiff(const QJsonObject &event)
{
    const QString type = event.value(QStringLiteral("type")).toString();
    auto reject = [this, &type] {
        // Never apply a malformed/out-of-range diff — that is what would
        // corrupt the ordered registry. Instead request a controlled fresh
        // snapshot from Rust so the model recovers to a complete, correct
        // room set rather than staying stale. (Well-formed dynamic-adapter
        // diffs should never reach here.)
        qCWarning(lcRust) << "room_list malformed diff rejected op=" << type
                          << "— requesting fresh room-list snapshot";
        if (m_rustHandle)
            takeRustString(mx_rust_resync_rooms(m_rustHandle));
    };
    auto addRoom = [this](int index, const QJsonObject &object) {
        RoomInfo room = roomInfoFromJson(object);
        if (room.id.isEmpty() || m_rooms.contains(room.id)
            || index < 0 || index > m_roomOrder.size()) return false;
        m_rooms.insert(room.id, room);
        m_roomOrder.insert(index, room.id);
        return true;
    };

    bool ok = true;
    if (type == QLatin1String("room_list_append")) {
        for (const auto &value : event.value(QStringLiteral("rooms")).toArray())
            ok = addRoom(m_roomOrder.size(), value.toObject()) && ok;
    } else if (type == QLatin1String("room_list_push_front")) {
        ok = addRoom(0, event.value(QStringLiteral("room")).toObject());
    } else if (type == QLatin1String("room_list_push_back")) {
        ok = addRoom(m_roomOrder.size(), event.value(QStringLiteral("room")).toObject());
    } else if (type == QLatin1String("room_list_insert")) {
        ok = addRoom(event.value(QStringLiteral("index")).toInt(-1),
                     event.value(QStringLiteral("room")).toObject());
    } else if (type == QLatin1String("room_list_set")) {
        const int index = event.value(QStringLiteral("index")).toInt(-1);
        const RoomInfo room = roomInfoFromJson(event.value(QStringLiteral("room")).toObject());
        if (index < 0 || index >= m_roomOrder.size() || room.id.isEmpty()) ok = false;
        else {
            const QString oldId = m_roomOrder.at(index);
            if (room.id != oldId && m_rooms.contains(room.id)) ok = false;
            else {
                m_rooms.remove(oldId); m_rooms.insert(room.id, room); m_roomOrder[index] = room.id;
            }
        }
    } else if (type == QLatin1String("room_list_remove")) {
        const int index = event.value(QStringLiteral("index")).toInt(-1);
        if (index < 0 || index >= m_roomOrder.size()) ok = false;
        else m_rooms.remove(m_roomOrder.takeAt(index));
    } else if (type == QLatin1String("room_list_pop_front")) {
        if (m_roomOrder.isEmpty()) ok = false; else m_rooms.remove(m_roomOrder.takeFirst());
    } else if (type == QLatin1String("room_list_pop_back")) {
        if (m_roomOrder.isEmpty()) ok = false; else m_rooms.remove(m_roomOrder.takeLast());
    } else if (type == QLatin1String("room_list_clear")) {
        m_rooms.clear(); m_roomOrder.clear();
    } else if (type == QLatin1String("room_list_truncate")) {
        const int length = event.value(QStringLiteral("length")).toInt(-1);
        if (length < 0 || length > m_roomOrder.size()) ok = false;
        else while (m_roomOrder.size() > length) m_rooms.remove(m_roomOrder.takeLast());
    } else {
        ok = false;
    }
    if (!ok) { reject(); return; }
    Q_EMIT roomsChanged();
}

void RustSdkMatrixClient::handleSpacesEvent(const QJsonArray &spaces)
{
    QSet<QString> present;
    for (const auto &value : spaces) {
        const auto object = value.toObject();
        const QString id = object.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) continue;
        present.insert(id);
        RoomInfo room = m_rooms.value(id);
        room.id = id; room.isSpace = true; room.membership = RoomInfo::Joined;
        room.name = object.value(QStringLiteral("name")).toString(room.name);
        room.avatarUrl = object.value(QStringLiteral("avatar_url")).toString(room.avatarUrl);
        room.childRoomIds.clear();
        for (const auto &child : object.value(QStringLiteral("descendants")).toArray()) {
            const QString childId = child.toString();
            if (!childId.isEmpty() && childId != id && !room.childRoomIds.contains(childId))
                room.childRoomIds.append(childId);
        }
        room.parentSpaceIds.clear();
        for (const auto &parent : object.value(QStringLiteral("parents")).toArray()) {
            const QString parentId = parent.toString();
            if (!parentId.isEmpty()) room.parentSpaceIds.append(parentId);
        }
        m_rooms.insert(id, room);
        if (!m_roomOrder.contains(id)) m_roomOrder.append(id);
    }
    for (auto it = m_rooms.begin(); it != m_rooms.end(); ++it) {
        if (it->isSpace && !present.contains(it.key())) {
            it->childRoomIds.clear(); it->parentSpaceIds.clear();
        }
    }
    Q_EMIT roomsChanged();
}

void RustSdkMatrixClient::handleTimelineEvent(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const QJsonObject obj = event.value(QStringLiteral("event")).toObject();
    const QString eventId = obj.value(QStringLiteral("event_id")).toString();
    if (roomId.isEmpty() || eventId.isEmpty())
        return;

    // v0.5.7: rooms with a live SDK timeline are fed exclusively through
    // timeline_reset / timeline_diff — appending the raw sync event here
    // would duplicate rows. Keep only the room-list preview update.
    if (m_timelineTracker.activeRoom() == roomId
        || m_timelineTracker.requestedRoom() == roomId) {
        auto roomIt = m_rooms.find(roomId);
        if (roomIt != m_rooms.end()) {
            // Raw sync bodies are free-form (poll fallbacks, mention
            // markdown, newlines); the live-timeline diff path follows up
            // with the typed summary, but this writer must be one-line too.
            const QString body = matrix::preview::normalizePreviewText(
                obj.value(QStringLiteral("body")).toString());
            if (!body.isEmpty())
                roomIt->lastMessagePreview = body;
            const QDateTime ts = timestampFromMs(static_cast<qint64>(
                obj.value(QStringLiteral("timestamp_ms")).toDouble(0)));
            if (ts.isValid())
                roomIt->lastActivity = ts;
            Q_EMIT roomUpdated(roomId);
        }
        return;
    }

    auto &timeline = m_timelines[roomId];
    for (const auto &existing : timeline) {
        if (existing.eventId == eventId)
            return;
    }

    TimelineEvent timelineEvent;
    timelineEvent.eventId = eventId;
    timelineEvent.roomId = roomId;
    timelineEvent.sender = obj.value(QStringLiteral("sender")).toString();
    timelineEvent.senderDisplayName = displayNameFor(roomId, timelineEvent.sender);
    timelineEvent.body = obj.value(QStringLiteral("body")).toString();
    // Untrusted sender HTML — carried through as-is; TimelineModel sanitizes
    // it before QML ever sees it.
    timelineEvent.formattedBody =
        obj.value(QStringLiteral("formatted_body")).toString();
    timelineEvent.timestamp = timestampFromMs(static_cast<qint64>(
        obj.value(QStringLiteral("timestamp_ms")).toDouble(0)));
    if (!timelineEvent.timestamp.isValid())
        timelineEvent.timestamp = QDateTime::currentDateTimeUtc();
    timelineEvent.type = typeFromString(obj.value(QStringLiteral("msgtype")).toString());
    timelineEvent.status = TimelineEvent::Sent;

    // v0.5.0-prep+6: propagate the encryption metadata the Rust bridge
    // emits (is_encrypted / is_decrypted / undecryptable / error_kind).
    // Fall back to the prep+5 `decrypted` boolean for backward
    // compatibility if the FFI is ever downgraded. Never derive
    // plaintext from these fields — they are metadata only.
    const bool undecryptable =
        obj.value(QStringLiteral("undecryptable")).toBool(false);
    const bool isDecrypted =
        obj.value(QStringLiteral("is_decrypted"))
           .toBool(obj.value(QStringLiteral("decrypted")).toBool(false));
    const bool isEncrypted =
        obj.value(QStringLiteral("is_encrypted"))
           .toBool(undecryptable || isDecrypted);
    timelineEvent.isEncrypted   = isEncrypted;
    timelineEvent.isDecrypted   = isDecrypted;
    timelineEvent.undecryptable = undecryptable;
    timelineEvent.errorKind     =
        obj.value(QStringLiteral("error_kind")).toString();
    // v0.6.0 checkpoint 12: mention/thread metadata for notification policy
    // in rooms without a live timeline.
    timelineEvent.mentionsMe =
        obj.value(QStringLiteral("mentions_me")).toBool(false);
    timelineEvent.mentionsRoom =
        obj.value(QStringLiteral("mentions_room")).toBool(false);
    timelineEvent.threadRootId =
        obj.value(QStringLiteral("thread_root_id")).toString();

    // v0.5-prep+3: Rust bridges undecryptable encrypted events with
    // `undecryptable = true` and an empty body. Render an honest
    // placeholder here instead of an empty bubble. The SDK will
    // upgrade the event later (via `event_replaced`) if / when keys
    // arrive; until then the user sees WHY the timeline is silent.
    if (undecryptable && timelineEvent.body.isEmpty()) {
        timelineEvent.body = tr("[unable to decrypt yet]");
        timelineEvent.type = TimelineEvent::Notice;
    }

    // Safe recovery-lifecycle diagnostics for encrypted events (redacted ids,
    // semantic error category — never bodies or ciphertext). Only encrypted
    // events are traced, so this stays quiet in unencrypted rooms.
    if (isEncrypted) {
        qCDebug(lcE2ee) << "encrypted-event"
                        << "room=" << matrix::e2ee::redactId(roomId)
                        << "event=" << matrix::e2ee::redactId(timelineEvent.eventId)
                        << (undecryptable ? "state=utd" : "state=decryptable")
                        << "error=" << (timelineEvent.errorKind.isEmpty()
                                            ? QStringLiteral("none")
                                            : timelineEvent.errorKind);
    }

    timeline.append(timelineEvent);
    Q_EMIT eventAppended(roomId, timelineEvent);

    auto roomIt = m_rooms.find(roomId);
    if (roomIt != m_rooms.end()) {
        roomIt->lastMessagePreview = previewFor(timelineEvent);
        roomIt->lastActivity = timelineEvent.timestamp;
        Q_EMIT roomUpdated(roomId);
    }
}

void RustSdkMatrixClient::updateRoomPreviewFrom(
    const QString &roomId, const QList<TimelineEvent> &newestFirstCandidates)
{
    auto roomIt = m_rooms.find(roomId);
    if (roomIt == m_rooms.end())
        return;
    for (const auto &event : newestFirstCandidates) {
        if (event.isVirtual())
            continue;
        roomIt->lastMessagePreview = previewFor(event);
        if (event.timestamp.isValid())
            roomIt->lastActivity = event.timestamp;
        Q_EMIT roomUpdated(roomId);
        return;
    }
}

void RustSdkMatrixClient::handleTimelineReset(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const auto generation = static_cast<quint64>(
        event.value(QStringLiteral("room_generation")).toDouble(0));
    if (!m_timelineTracker.adoptReset(roomId, generation)) {
        qCInfo(lcRust) << "timeline stale reset ignored"
                       << "generation=" << generation
                       << "adopted=" << m_timelineTracker.generation();
        return;
    }

    const QJsonArray items = event.value(QStringLiteral("items")).toArray();
    m_timelines[roomId] =
        matrix::rust_timeline::eventsFromItemArray(items, roomId);
    qCInfo(lcRust) << "timeline subscription started"
                   << "room_generation=" << generation
                   << "items=" << m_timelines[roomId].size();
    Q_EMIT timelineReset(roomId);
    Q_EMIT paginationStateChanged(roomId);

    QList<TimelineEvent> newestFirst = m_timelines[roomId];
    std::reverse(newestFirst.begin(), newestFirst.end());
    updateRoomPreviewFrom(roomId, newestFirst);
}

void RustSdkMatrixClient::handleTimelineDiff(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const auto generation = static_cast<quint64>(
        event.value(QStringLiteral("room_generation")).toDouble(0));
    if (!m_timelineTracker.accepts(roomId, generation)) {
        qCInfo(lcRust) << "timeline stale diff ignored"
                       << "generation=" << generation
                       << "adopted=" << m_timelineTracker.generation();
        return;
    }

    using matrix::rust_timeline::DiffOutcome;
    auto &mirror = m_timelines[roomId];
    const DiffOutcome outcome =
        matrix::rust_timeline::applyTimelineDiff(mirror, event, roomId);

    switch (outcome.kind) {
    case DiffOutcome::Appended:
        for (const auto &item : outcome.items)
            Q_EMIT eventAppended(roomId, item);
        {
            QList<TimelineEvent> newestFirst = outcome.items;
            std::reverse(newestFirst.begin(), newestFirst.end());
            updateRoomPreviewFrom(roomId, newestFirst);
        }
        break;
    case DiffOutcome::Prepended:
        Q_EMIT eventsPrepended(roomId, outcome.items);
        break;
    case DiffOutcome::Inserted:
        Q_EMIT eventInsertedAt(roomId, outcome.index, outcome.items.first());
        break;
    case DiffOutcome::Changed:
        Q_EMIT eventChangedAt(roomId, outcome.index, outcome.items.first());
        break;
    case DiffOutcome::Removed:
        Q_EMIT eventRemovedAt(roomId, outcome.index);
        break;
    case DiffOutcome::Cleared:
    case DiffOutcome::Reset:
        Q_EMIT timelineReset(roomId);
        break;
    case DiffOutcome::Truncated:
        Q_EMIT eventsTruncatedTo(roomId, outcome.length);
        break;
    case DiffOutcome::Invalid:
        // Never apply a malformed/stale diff. Recover with one fresh
        // snapshot instead of corrupting model state. No message bodies
        // in this log line.
        qCWarning(lcRust) << "timeline invalid diff rejected"
                          << "op=" << event.value(QStringLiteral("op")).toString()
                          << "index=" << event.value(QStringLiteral("index")).toInt(-1)
                          << "mirror_size=" << mirror.size();
        openRoomTimeline(roomId);
        break;
    }
}

void RustSdkMatrixClient::handleTimelinePagination(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const auto generation = static_cast<quint64>(
        event.value(QStringLiteral("room_generation")).toDouble(0));
    if (!m_timelineTracker.accepts(roomId, generation)) {
        qCInfo(lcRust) << "timeline stale pagination ignored"
                       << "generation=" << generation;
        return;
    }

    auto &state = m_pagination[roomId];
    const QString paginationState =
        event.value(QStringLiteral("state")).toString();
    if (paginationState == QLatin1String("loading")) {
        state.loading = true;
        state.failed = false;
        state.failureTransient = false;
        qCInfo(lcRust) << "timeline pagination started";
    } else if (paginationState == QLatin1String("idle")) {
        state.loading = false;
        state.failed = false;
        state.failureTransient = false;
        state.reachedStart =
            event.value(QStringLiteral("reached_start")).toBool(false);
        qCInfo(lcRust) << "timeline pagination complete reached_start="
                       << state.reachedStart;
    } else if (paginationState == QLatin1String("failed")) {
        state.loading = false;
        state.failed = true;
        const QString category =
            event.value(QStringLiteral("category")).toString();
        state.failureTransient = category == QLatin1String("network")
            || category == QLatin1String("not_ready");
        qCWarning(lcRust) << "timeline pagination failed category="
                          << category;
    }
    Q_EMIT paginationStateChanged(roomId);
}

void RustSdkMatrixClient::handleTimelineRetryDecryption(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const QString state = event.value(QStringLiteral("state")).toString();
    const int sessions = event.value(QStringLiteral("sessions")).toInt(0);
    qCInfo(lcRust) << "timeline retry decryption" << state
                   << "sessions=" << sessions;
    // Safe recovery-lifecycle diagnostics: redacted room id, semantic state,
    // and a session COUNT only — never session ids, keys, or bodies.
    qCDebug(lcE2ee) << "retry-decryption" << "room=" << matrix::e2ee::redactId(roomId)
                    << "state=" << state << "sessions=" << sessions;
    if (state == QLatin1String("done"))
        Q_EMIT roomKeysApplied(roomId, sessions);
}

void RustSdkMatrixClient::handleSendOk(const QJsonObject &event)
{
    const QString txnId = event.value(QStringLiteral("transaction_id")).toString();
    const QString realEventId = event.value(QStringLiteral("event_id")).toString();
    const auto pendingIt = m_pendingSends.find(txnId);
    if (pendingIt == m_pendingSends.end())
        return;

    const PendingSend pending = pendingIt.value();
    m_pendingSends.erase(pendingIt);

    auto &timeline = m_timelines[pending.roomId];
    for (auto &timelineEvent : timeline) {
        if (timelineEvent.eventId != pending.localEventId)
            continue;

        if (!realEventId.isEmpty()) {
            const QString oldId = timelineEvent.eventId;
            timelineEvent.eventId = realEventId;
            timelineEvent.status = TimelineEvent::Sent;
            Q_EMIT eventReplaced(pending.roomId, oldId, timelineEvent);
        } else {
            timelineEvent.status = TimelineEvent::Sent;
            Q_EMIT eventStatusChanged(pending.roomId,
                                      pending.localEventId,
                                      TimelineEvent::Sent);
        }
        return;
    }
}

void RustSdkMatrixClient::handleSendFailed(const QJsonObject &event)
{
    failPendingSend(event.value(QStringLiteral("transaction_id")).toString(),
                    event.value(QStringLiteral("message")).toString(
                        tr("Rust SDK send failed.")));
}

void RustSdkMatrixClient::recoverFromBackup(const QString &recoveryKey)
{
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT keyBackupResult(QStringLiteral("failed"),
                               tr("Not signed in."));
        return;
    }
    if (recoveryKey.isEmpty()) {
        Q_EMIT keyBackupResult(QStringLiteral("failed"),
                               tr("Recovery key is empty."));
        return;
    }
    QByteArray keyBytes = recoveryKey.toUtf8();
    const QString result = takeRustString(mx_rust_recover_from_backup(
        m_rustHandle, keyBytes.constData()));
    // Best-effort scrub of the recovery secret's transit buffer, mirroring
    // importRoomKeys (the QString original is owned by the caller, which
    // clears its field immediately after submitting).
    keyBytes.fill('\0');
    if (!result.isEmpty()) {
        Q_EMIT keyBackupResult(QStringLiteral("failed"),
            result.startsWith(QLatin1String("error: "))
                ? result.mid(7) : result);
    }
}

void RustSdkMatrixClient::reloadRoomTimeline(const QString &roomId, int limit)
{
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT errorOccurred(tr("Reload timeline: not signed in."));
        return;
    }
    if (roomId.isEmpty()) return;
    const QByteArray idBytes = roomId.toUtf8();
    const unsigned int clamped =
        limit <= 0 ? 30u
                   : static_cast<unsigned int>(std::min(limit, 200));
    qCInfo(lcRust) << "reload_timeline start room=" << roomId.right(12)
                   << "limit=" << clamped;
    const QString result = takeRustString(mx_rust_reload_room_timeline(
        m_rustHandle, idBytes.constData(), clamped));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(
            result.startsWith(QLatin1String("error: "))
                ? result.mid(7) : result);
    }
}

void RustSdkMatrixClient::acceptVerification(const QString &flowId)
{
    if (!m_rustHandle || flowId.isEmpty()) return;
    const QByteArray b = flowId.toUtf8();
    const QString r = takeRustString(mx_rust_accept_verification(m_rustHandle, b.constData()));
    if (!r.isEmpty()) Q_EMIT verificationFailed(flowId,
        r.startsWith(QLatin1String("error: ")) ? r.mid(7) : r);
}

void RustSdkMatrixClient::confirmVerification(const QString &flowId)
{
    if (!m_rustHandle || flowId.isEmpty()) return;
    const QByteArray b = flowId.toUtf8();
    const QString r = takeRustString(mx_rust_confirm_verification(m_rustHandle, b.constData()));
    if (!r.isEmpty()) Q_EMIT verificationFailed(flowId,
        r.startsWith(QLatin1String("error: ")) ? r.mid(7) : r);
}

void RustSdkMatrixClient::mismatchVerification(const QString &flowId)
{
    if (!m_rustHandle || flowId.isEmpty()) return;
    const QByteArray b = flowId.toUtf8();
    const QString r = takeRustString(mx_rust_mismatch_verification(m_rustHandle, b.constData()));
    if (!r.isEmpty()) Q_EMIT verificationFailed(flowId,
        r.startsWith(QLatin1String("error: ")) ? r.mid(7) : r);
}

void RustSdkMatrixClient::cancelVerification(const QString &flowId)
{
    if (!m_rustHandle || flowId.isEmpty()) return;
    const QByteArray b = flowId.toUtf8();
    const QString r = takeRustString(mx_rust_cancel_verification(m_rustHandle, b.constData()));
    if (!r.isEmpty()) Q_EMIT verificationFailed(flowId,
        r.startsWith(QLatin1String("error: ")) ? r.mid(7) : r);
}

void RustSdkMatrixClient::startOwnVerification()
{
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT verificationFailed(QString{}, tr("Not signed in."));
        return;
    }
    const QString r = takeRustString(mx_rust_start_own_verification(m_rustHandle));
    if (!r.isEmpty()) {
        Q_EMIT verificationFailed(QString{},
            r.startsWith(QLatin1String("error: ")) ? r.mid(7) : r);
    }
}

void RustSdkMatrixClient::requestMissingSecrets()
{
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT errorOccurred(tr("Not signed in."));
        return;
    }
    const QString r =
        takeRustString(mx_rust_request_missing_secrets(m_rustHandle));
    if (!r.isEmpty()) {
        qCWarning(lcRust) << "request_missing_secrets dispatch failed";
        Q_EMIT errorOccurred(
            r.startsWith(QLatin1String("error: ")) ? r.mid(7) : r);
    }
}

void RustSdkMatrixClient::refreshOwnDeviceStatus()
{
    if (!m_loggedIn || !m_rustHandle)
        return;
    const QString raw = takeRustString(mx_rust_query_own_device_status(m_rustHandle));
    if (raw.isEmpty() || raw.startsWith(QLatin1String("error: ")))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
    if (!doc.isObject()) return;
    const QJsonObject obj = doc.object();
    Q_EMIT ownDeviceStatusUpdated(
        obj.value(QStringLiteral("device_id")).toString(),
        obj.value(QStringLiteral("own_identity_available")).toBool(false),
        obj.value(QStringLiteral("own_identity_verified")).toBool(false),
        obj.value(QStringLiteral("device_cross_signed")).toBool(false),
        obj.value(QStringLiteral("has_master")).toBool(false),
        obj.value(QStringLiteral("has_self_signing")).toBool(false),
        obj.value(QStringLiteral("has_user_signing")).toBool(false));
}

void RustSdkMatrixClient::importRoomKeys(const QString &filePath,
                                         const QString &passphrase)
{
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT roomKeyImportFailed(QStringLiteral("not_signed_in"),
                                   tr("Not signed in."));
        return;
    }
    if (filePath.isEmpty()) {
        Q_EMIT roomKeyImportFailed(QStringLiteral("invalid_file"),
                                   tr("No file selected."));
        return;
    }
    // Convert once and pass through — do NOT keep a QString copy of the
    // passphrase alive in the C++ layer beyond this call.
    QByteArray pathBytes = filePath.toUtf8();
    QByteArray passphraseBytes = passphrase.toUtf8();
    const QString r = takeRustString(mx_rust_import_room_keys(
        m_rustHandle, pathBytes.constData(), passphraseBytes.constData()));
    // Best-effort scrub. QByteArray is not zeroizing but the buffers go
    // out of scope on return and the passphrase is not kept anywhere in
    // C++ after this line.
    for (int i = 0; i < passphraseBytes.size(); ++i)
        passphraseBytes[i] = 0;
    if (!r.isEmpty()) {
        Q_EMIT roomKeyImportFailed(QStringLiteral("import_failed"),
            r.startsWith(QLatin1String("error: ")) ? r.mid(7) : r);
    }
}

bool RustSdkMatrixClient::roomKeyImportActive() const
{
    if (!m_rustHandle) return false;
    return mx_rust_room_key_import_active(m_rustHandle) != 0;
}

void RustSdkMatrixClient::probeEncryptedSend(const QString &roomId,
                                             const QString &body,
                                             const QString &marker)
{
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT encryptedSendProbeResult(roomId, marker, false,
                                        QString(), tr("Not signed in."));
        return;
    }
    if (!m_rooms.contains(roomId)) {
        Q_EMIT encryptedSendProbeResult(roomId, marker, false,
                                        QString(),
                                        tr("Unknown room: %1").arg(roomId));
        return;
    }
    if (!isRoomEncrypted(roomId)) {
        Q_EMIT encryptedSendProbeResult(roomId, marker, false,
                                        QString(),
                                        tr("Probe refused: target room is not encrypted."));
        return;
    }

    const QString txnId = nextTxnId();
    m_pendingProbes.insert(txnId, PendingProbe{ roomId, marker });

    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray bodyBytes = body.toUtf8();
    const QByteArray txnBytes  = txnId.toUtf8();
    const QString result = takeRustString(mx_rust_probe_encrypted_send(
        m_rustHandle, roomBytes.constData(),
        bodyBytes.constData(), txnBytes.constData()));
    if (!result.isEmpty()) {
        m_pendingProbes.remove(txnId);
        Q_EMIT encryptedSendProbeResult(roomId, marker, false, QString(),
            result.startsWith(QLatin1String("error: "))
                ? result.mid(7) : result);
    }
}

void RustSdkMatrixClient::handleEncryptedSendOk(const QJsonObject &event)
{
    const QString txnId = event.value(QStringLiteral("transaction_id")).toString();
    const QString serverEventId = event.value(QStringLiteral("event_id")).toString();
    const auto it = m_pendingProbes.find(txnId);
    if (it == m_pendingProbes.end())
        return;
    const PendingProbe probe = it.value();
    m_pendingProbes.erase(it);
    Q_EMIT encryptedSendProbeResult(probe.roomId, probe.marker, true,
                                    serverEventId, QString());
}

void RustSdkMatrixClient::handleEncryptedSendFailed(const QJsonObject &event)
{
    const QString txnId = event.value(QStringLiteral("transaction_id")).toString();
    const QString message = event.value(QStringLiteral("message")).toString(
        tr("Rust SDK encrypted send probe failed."));
    const auto it = m_pendingProbes.find(txnId);
    if (it == m_pendingProbes.end()) {
        Q_EMIT errorOccurred(tr("Encrypted send probe failed: %1").arg(message));
        return;
    }
    const PendingProbe probe = it.value();
    m_pendingProbes.erase(it);
    Q_EMIT encryptedSendProbeResult(probe.roomId, probe.marker, false,
                                    QString(), message);
}

void RustSdkMatrixClient::failPendingSend(const QString &transactionId, const QString &message)
{
    const auto pendingIt = m_pendingSends.find(transactionId);
    if (pendingIt == m_pendingSends.end()) {
        if (!message.isEmpty())
            Q_EMIT errorOccurred(tr("Send failed: %1").arg(message));
        return;
    }

    const PendingSend pending = pendingIt.value();
    m_pendingSends.erase(pendingIt);

    auto &timeline = m_timelines[pending.roomId];
    for (auto &timelineEvent : timeline) {
        if (timelineEvent.eventId == pending.localEventId) {
            timelineEvent.status = TimelineEvent::Failed;
            Q_EMIT eventStatusChanged(pending.roomId,
                                      pending.localEventId,
                                      TimelineEvent::Failed);
            break;
        }
    }

    if (!message.isEmpty())
        Q_EMIT errorOccurred(tr("Send failed: %1").arg(message));
}

// ---------------------------------------------------------------------------
// v0.5.9 — conversation creation, membership, room editing, media bridge.
//
// Pattern shared by all commands: generate an op id, dispatch to Rust, and
// return the id on acceptance (0 on synchronous rejection). Results arrive
// on the poll queue; handleRustEvent has already rejected stale handle
// generations, and Rust stamps its lifecycle so a signed-out session can
// never complete into a new one.
// ---------------------------------------------------------------------------

quint64 RustSdkMatrixClient::searchUsers(const QString &query, int limit)
{
    if (!m_rustHandle || query.trimmed().isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray q = query.toUtf8();
    const QString result = takeRustString(mx_rust_search_users(
        m_rustHandle, q.constData(),
        static_cast<unsigned long long>(qBound(1, limit, 50)), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "user search rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::fetchUserProfile(const QString &userId)
{
    if (!m_rustHandle || userId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray user = userId.toUtf8();
    const QString result = takeRustString(mx_rust_get_user_profile(
        m_rustHandle, user.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "profile lookup rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::fetchUrlPreview(const QString &url)
{
    // Scheme allow-list is enforced again in Rust; this early check keeps
    // obviously unsafe schemes from ever crossing the FFI.
    const QString lowered = url.trimmed().toLower();
    if (!m_rustHandle
        || !lowered.startsWith(QLatin1String("https://")))
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray target = url.toUtf8();
    const QString result = takeRustString(mx_rust_get_url_preview(
        m_rustHandle, target.constData(), opId));
    if (!result.isEmpty()) {
        // No URL in the log — operation state only.
        qCWarning(lcRust) << "url preview rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::gifGet(const QString &url)
{
    // https-only guard before the FFI; the URL carries the provider key so it
    // is never logged, here or in Rust.
    if (!m_rustHandle || !url.trimmed().toLower().startsWith(QLatin1String("https://")))
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray target = url.toUtf8();
    const QString result =
        takeRustString(mx_rust_gif_get(m_rustHandle, target.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "gif request rejected"; // no URL
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::gifDownload(const QString &url)
{
    if (!m_rustHandle || !url.trimmed().toLower().startsWith(QLatin1String("https://")))
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray target = url.toUtf8();
    const QString result = takeRustString(
        mx_rust_gif_download(m_rustHandle, target.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "gif download rejected"; // no URL
        return 0;
    }
    return opId;
}

QVariantList RustSdkMatrixClient::existingDirectRooms(const QString &userId) const
{
    if (!m_rustHandle || userId.isEmpty())
        return {};
    const QByteArray user = userId.toUtf8();
    const QString payload =
        takeRustString(mx_rust_get_dm_rooms(m_rustHandle, user.constData()));
    if (payload.isEmpty() || payload.startsWith(QLatin1String("error:")))
        return {};
    const QJsonObject obj = QJsonDocument::fromJson(payload.toUtf8()).object();
    QVariantList out;
    const QJsonArray rooms = obj.value(QStringLiteral("rooms")).toArray();
    for (const QJsonValue &value : rooms) {
        const QJsonObject room = value.toObject();
        QVariantMap entry;
        entry.insert(QStringLiteral("roomId"),
                     room.value(QStringLiteral("room_id")).toString());
        entry.insert(QStringLiteral("name"),
                     room.value(QStringLiteral("name")).toString());
        out.append(entry);
    }
    return out;
}

quint64 RustSdkMatrixClient::createDirectChat(const QString &userId)
{
    if (!m_rustHandle || userId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray user = userId.toUtf8();
    const QString result =
        takeRustString(mx_rust_create_dm(m_rustHandle, user.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "create DM rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::createRoom(const QVariantMap &options)
{
    if (!m_rustHandle)
        return 0;
    QJsonObject payload;
    payload.insert(QStringLiteral("name"),
                   options.value(QStringLiteral("name")).toString());
    payload.insert(QStringLiteral("topic"),
                   options.value(QStringLiteral("topic")).toString());
    payload.insert(QStringLiteral("public"),
                   options.value(QStringLiteral("public")).toBool());
    payload.insert(QStringLiteral("encrypted"),
                   options.value(QStringLiteral("encrypted")).toBool());
    payload.insert(QStringLiteral("alias"),
                   options.value(QStringLiteral("alias")).toString());
    payload.insert(QStringLiteral("space_id"),
                   options.value(QStringLiteral("spaceId")).toString());
    payload.insert(QStringLiteral("is_space"),
                   options.value(QStringLiteral("isSpace")).toBool());
    payload.insert(QStringLiteral("invites"),
                   QJsonArray::fromStringList(
                       options.value(QStringLiteral("invites")).toStringList()));
    const quint64 opId = nextOpId();
    const QByteArray json =
        QJsonDocument(payload).toJson(QJsonDocument::Compact);
    const QString result = takeRustString(
        mx_rust_create_room(m_rustHandle, json.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "create room rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::inviteUsers(const QString &roomId,
                                         const QStringList &userIds)
{
    if (!m_rustHandle || roomId.isEmpty() || userIds.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray users =
        QJsonDocument(QJsonArray::fromStringList(userIds))
            .toJson(QJsonDocument::Compact);
    const QString result = takeRustString(mx_rust_invite_users(
        m_rustHandle, room.constData(), users.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "invite command rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::requestRoomMembers(const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_room_members(m_rustHandle, room.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "member snapshot rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::setRoomName(const QString &roomId, const QString &name)
{
    if (!m_rustHandle || roomId.isEmpty() || name.trimmed().isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray value = name.toUtf8();
    const QString result = takeRustString(mx_rust_set_room_name(
        m_rustHandle, room.constData(), value.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::setRoomTopic(const QString &roomId, const QString &topic)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray value = topic.toUtf8();
    const QString result = takeRustString(mx_rust_set_room_topic(
        m_rustHandle, room.constData(), value.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::setRoomAvatar(const QString &roomId,
                                           const QString &localPath)
{
    if (!m_rustHandle || roomId.isEmpty() || localPath.isEmpty())
        return 0;
    const QFileInfo info(localPath);
    if (!info.isFile() || !info.isReadable() || info.size() <= 0)
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray path = localPath.toUtf8();
    const QString result = takeRustString(mx_rust_set_room_avatar(
        m_rustHandle, room.constData(), path.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::removeRoomAvatar(const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_remove_room_avatar(m_rustHandle, room.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::leaveRoom(const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_leave_room(m_rustHandle, room.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::addRoomToSpace(const QString &spaceId,
                                            const QString &roomId)
{
    if (!m_rustHandle || spaceId.isEmpty() || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray space = spaceId.toUtf8();
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_add_room_to_space(
        m_rustHandle, space.constData(), room.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::removeRoomFromSpace(const QString &spaceId,
                                                 const QString &roomId)
{
    if (!m_rustHandle || spaceId.isEmpty() || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray space = spaceId.toUtf8();
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_remove_room_from_space(
        m_rustHandle, space.constData(), room.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::sendAttachment(const QString &roomId,
                                            const QString &localPath,
                                            const QString &mime,
                                            const QString &caption,
                                            int width, int height, bool animated)
{
    if (!m_rustHandle || roomId.isEmpty() || localPath.isEmpty() || mime.isEmpty())
        return 0;
    if (!timelineActiveFor(roomId)) {
        qCWarning(lcRust) << "attachment send requires the open room timeline";
        return 0;
    }
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray path = localPath.toUtf8();
    const QByteArray mimeBytes = mime.toUtf8();
    const QByteArray captionBytes = caption.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_send_attachment(
        m_rustHandle, room.constData(), path.constData(), mimeBytes.constData(),
        captionBytes.constData(),
        static_cast<unsigned long long>(qMax(0, width)),
        static_cast<unsigned long long>(qMax(0, height)),
        animated ? 1 : 0, opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "attachment send rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::sendAttachmentBytes(const QString &roomId,
                                                 const QByteArray &bytes,
                                                 const QString &filename,
                                                 const QString &mime,
                                                 int width, int height)
{
    if (!m_rustHandle || roomId.isEmpty() || bytes.isEmpty() || mime.isEmpty())
        return 0;
    if (!timelineActiveFor(roomId)) {
        qCWarning(lcRust) << "attachment send requires the open room timeline";
        return 0;
    }
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray name = filename.toUtf8();
    const QByteArray mimeBytes = mime.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_send_attachment_bytes(
        m_rustHandle, room.constData(),
        reinterpret_cast<const unsigned char *>(bytes.constData()),
        static_cast<size_t>(bytes.size()), name.constData(),
        mimeBytes.constData(),
        static_cast<unsigned long long>(qMax(0, width)),
        static_cast<unsigned long long>(qMax(0, height)), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "clipboard attachment send rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::sendThreadAttachment(const QString &roomId,
                                                  const QString &rootEventId,
                                                  const QString &localPath,
                                                  const QString &mime,
                                                  const QString &caption,
                                                  int width, int height,
                                                  bool animated)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty()
        || rootEventId.isEmpty() || localPath.isEmpty() || mime.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray root = rootEventId.toUtf8();
    const QByteArray path = localPath.toUtf8();
    const QByteArray mimeBytes = mime.toUtf8();
    const QByteArray captionBytes = caption.toUtf8();
    const QString result = takeRustString(mx_rust_thread_send_attachment(
        m_rustHandle, room.constData(), root.constData(), path.constData(),
        mimeBytes.constData(), captionBytes.constData(),
        static_cast<unsigned long long>(qMax(0, width)),
        static_cast<unsigned long long>(qMax(0, height)),
        animated ? 1 : 0, opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "thread attachment send rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::sendThreadAttachmentBytes(const QString &roomId,
                                                       const QString &rootEventId,
                                                       const QByteArray &bytes,
                                                       const QString &filename,
                                                       const QString &mime,
                                                       int width, int height)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty()
        || rootEventId.isEmpty() || bytes.isEmpty() || mime.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray root = rootEventId.toUtf8();
    const QByteArray name = filename.toUtf8();
    const QByteArray mimeBytes = mime.toUtf8();
    const QString result = takeRustString(mx_rust_thread_send_attachment_bytes(
        m_rustHandle, room.constData(), root.constData(),
        reinterpret_cast<const unsigned char *>(bytes.constData()),
        static_cast<size_t>(bytes.size()), name.constData(),
        mimeBytes.constData(),
        static_cast<unsigned long long>(qMax(0, width)),
        static_cast<unsigned long long>(qMax(0, height)), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "thread clipboard attachment send rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::fetchMedia(const QString &mediaKey, int kind,
                                        int timeoutClass)
{
    if (!m_rustHandle || mediaKey.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray key = mediaKey.toUtf8();
    const QString result = takeRustString(mx_rust_media_fetch(
        m_rustHandle, key.constData(),
        static_cast<unsigned int>(qBound(0, kind, 1)), opId,
        static_cast<unsigned int>(qBound(0, timeoutClass, 2))));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::fetchMxcThumbnail(const QString &mxc,
                                               int width, int height)
{
    if (!m_rustHandle || !mxc.startsWith(QLatin1String("mxc://")))
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray uri = mxc.toUtf8();
    const QString result = takeRustString(mx_rust_media_fetch_mxc(
        m_rustHandle, uri.constData(),
        static_cast<unsigned long long>(qMax(0, width)),
        static_cast<unsigned long long>(qMax(0, height)), opId));
    return result.isEmpty() ? opId : 0;
}

void RustSdkMatrixClient::handleMediaReady(const QJsonObject &event)
{
    const quint64 opId =
        static_cast<quint64>(event.value(QStringLiteral("op_id")).toDouble());
    size_t len = 0;
    unsigned char *raw = mx_rust_media_take(m_rustHandle, opId, &len);
    if (!raw || len == 0) {
        // Stale or already-taken payload; treat as a failed fetch.
        Q_EMIT mediaFailed(opId, event.value(QStringLiteral("key")).toString(),
                           event.value(QStringLiteral("kind")).toInt(),
                           QStringLiteral("gone"));
        return;
    }
    // One bounded copy into Qt-owned memory, then release the Rust buffer.
    QByteArray bytes(reinterpret_cast<const char *>(raw),
                     static_cast<qsizetype>(len));
    mx_rust_media_free(raw, len);
    Q_EMIT mediaReady(opId, event.value(QStringLiteral("key")).toString(),
                      event.value(QStringLiteral("kind")).toInt(), bytes,
                      event.value(QStringLiteral("mimetype")).toString(),
                      event.value(QStringLiteral("filename")).toString());
}

bool RustSdkMatrixClient::handleRoomCommandEvent(const QString &type,
                                                 const QJsonObject &event)
{
    const auto opId = [&event]() {
        return static_cast<quint64>(
            event.value(QStringLiteral("op_id")).toDouble());
    };

    if (type == QLatin1String("user_search_result")) {
        QVariantList results;
        const QJsonArray rows = event.value(QStringLiteral("results")).toArray();
        for (const QJsonValue &value : rows) {
            const QJsonObject row = value.toObject();
            QVariantMap entry;
            entry.insert(QStringLiteral("userId"),
                         row.value(QStringLiteral("user_id")).toString());
            entry.insert(QStringLiteral("displayName"),
                         row.value(QStringLiteral("display_name")).toString());
            entry.insert(QStringLiteral("avatarUrl"),
                         row.value(QStringLiteral("avatar_url")).toString());
            results.append(entry);
        }
        Q_EMIT userSearchFinished(opId(),
                                  event.value(QStringLiteral("ok")).toBool(),
                                  results,
                                  event.value(QStringLiteral("limited")).toBool(),
                                  event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("user_profile_result")) {
        Q_EMIT userProfileFinished(
            opId(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("user_id")).toString(),
            event.value(QStringLiteral("display_name")).toString(),
            event.value(QStringLiteral("avatar_url")).toString(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("url_preview_result")) {
        QVariantMap fields;
        const QJsonObject raw = event.value(QStringLiteral("fields")).toObject();
        fields.insert(QStringLiteral("title"),
                      raw.value(QStringLiteral("title")).toString());
        fields.insert(QStringLiteral("description"),
                      raw.value(QStringLiteral("description")).toString());
        fields.insert(QStringLiteral("siteName"),
                      raw.value(QStringLiteral("site_name")).toString());
        fields.insert(QStringLiteral("previewKind"),
                      raw.value(QStringLiteral("preview_kind")).toString());
        fields.insert(QStringLiteral("imageMxc"),
                      raw.value(QStringLiteral("image_mxc")).toString());
        fields.insert(QStringLiteral("imageSource"),
                      raw.value(QStringLiteral("image_source")).toString());
        fields.insert(QStringLiteral("imageMime"),
                      raw.value(QStringLiteral("image_mime")).toString());
        fields.insert(QStringLiteral("imageWidth"),
                      raw.value(QStringLiteral("image_width")).toInt());
        fields.insert(QStringLiteral("imageHeight"),
                      raw.value(QStringLiteral("image_height")).toInt());
        fields.insert(QStringLiteral("imageSize"),
                      static_cast<qint64>(
                          raw.value(QStringLiteral("image_size")).toDouble()));
        Q_EMIT urlPreviewFinished(
            opId(), event.value(QStringLiteral("ok")).toBool(), fields,
            event.value(QStringLiteral("category")).toString(),
            event.value(QStringLiteral("status")).toInt(),
            event.value(QStringLiteral("redirects")).toInt());
        return true;
    }

    if (type == QLatin1String("gif_response")) {
        // The bounded JSON body is provider data (no key, no Matrix ids); the
        // GIF controller parses it into safe structs. Never logged.
        Q_EMIT gifResponse(
            opId(), event.value(QStringLiteral("ok")).toBool(false),
            event.value(QStringLiteral("status")).toInt(),
            event.value(QStringLiteral("body")).toString().toUtf8(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("gif_download_result")) {
        const quint64 op = opId();
        const bool ok = event.value(QStringLiteral("ok")).toBool(false);
        if (!ok) {
            Q_EMIT gifDownloadFinished(
                op, false, QByteArray(), QString(), 0, 0, 0,
                event.value(QStringLiteral("category")).toString());
            return true;
        }
        // Take the parked GIF bytes into Qt-owned memory (one bounded copy),
        // then release the Rust buffer — mirrors media_ready.
        size_t len = 0;
        unsigned char *raw = mx_rust_media_take(m_rustHandle, op, &len);
        if (!raw || len == 0) {
            Q_EMIT gifDownloadFinished(op, false, QByteArray(), QString(), 0, 0,
                                       0, QStringLiteral("gone"));
            return true;
        }
        QByteArray bytes(reinterpret_cast<const char *>(raw),
                         static_cast<qsizetype>(len));
        mx_rust_media_free(raw, len);
        Q_EMIT gifDownloadFinished(
            op, true, bytes, event.value(QStringLiteral("mime")).toString(),
            event.value(QStringLiteral("width")).toInt(),
            event.value(QStringLiteral("height")).toInt(),
            static_cast<qint64>(event.value(QStringLiteral("size")).toDouble()),
            QString());
        return true;
    }

    if (type == QLatin1String("dm_create_result")) {
        Q_EMIT dmCreateFinished(opId(),
                                event.value(QStringLiteral("ok")).toBool(),
                                event.value(QStringLiteral("room_id")).toString(),
                                event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("room_create_result")) {
        Q_EMIT roomCreateFinished(opId(),
                                  event.value(QStringLiteral("ok")).toBool(),
                                  event.value(QStringLiteral("room_id")).toString(),
                                  event.value(QStringLiteral("category")).toString(),
                                  event.value(QStringLiteral("warning")).toString());
        return true;
    }

    if (type == QLatin1String("room_invite_result")) {
        Q_EMIT inviteUserFinished(opId(),
                                  event.value(QStringLiteral("room_id")).toString(),
                                  event.value(QStringLiteral("user_id")).toString(),
                                  event.value(QStringLiteral("ok")).toBool(),
                                  event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("room_invite_done")) {
        Q_EMIT inviteBatchFinished(opId(),
                                   event.value(QStringLiteral("room_id")).toString(),
                                   event.value(QStringLiteral("ok_count")).toInt(),
                                   event.value(QStringLiteral("fail_count")).toInt());
        return true;
    }

    if (type == QLatin1String("room_members")) {
        QVariantMap snapshot;
        snapshot.insert(QStringLiteral("ok"),
                        event.value(QStringLiteral("ok")).toBool());
        snapshot.insert(QStringLiteral("truncated"),
                        event.value(QStringLiteral("truncated")).toBool());
        snapshot.insert(QStringLiteral("joinedCount"),
                        event.value(QStringLiteral("joined_count")).toInt());
        snapshot.insert(QStringLiteral("invitedCount"),
                        event.value(QStringLiteral("invited_count")).toInt());
        snapshot.insert(QStringLiteral("canInvite"),
                        event.value(QStringLiteral("own_can_invite")).toBool());
        snapshot.insert(QStringLiteral("canEditName"),
                        event.value(QStringLiteral("own_can_edit_name")).toBool());
        snapshot.insert(QStringLiteral("canEditTopic"),
                        event.value(QStringLiteral("own_can_edit_topic")).toBool());
        snapshot.insert(QStringLiteral("canEditAvatar"),
                        event.value(QStringLiteral("own_can_edit_avatar")).toBool());
        snapshot.insert(QStringLiteral("category"),
                        event.value(QStringLiteral("category")).toString());
        QVariantList members;
        const QJsonArray rows = event.value(QStringLiteral("members")).toArray();
        for (const QJsonValue &value : rows) {
            const QJsonObject row = value.toObject();
            QVariantMap entry;
            entry.insert(QStringLiteral("userId"),
                         row.value(QStringLiteral("user_id")).toString());
            entry.insert(QStringLiteral("displayName"),
                         row.value(QStringLiteral("display_name")).toString());
            entry.insert(QStringLiteral("avatarUrl"),
                         row.value(QStringLiteral("avatar_url")).toString());
            entry.insert(QStringLiteral("membership"),
                         row.value(QStringLiteral("membership")).toString());
            entry.insert(QStringLiteral("role"),
                         row.value(QStringLiteral("role")).toString());
            entry.insert(QStringLiteral("ambiguous"),
                         row.value(QStringLiteral("ambiguous")).toBool());
            entry.insert(QStringLiteral("isOwn"),
                         row.value(QStringLiteral("is_own")).toBool());
            members.append(entry);
        }
        snapshot.insert(QStringLiteral("members"), members);
        Q_EMIT roomMembersReceived(
            opId(), event.value(QStringLiteral("room_id")).toString(), snapshot);
        return true;
    }

    if (type == QLatin1String("room_edit_result")) {
        Q_EMIT roomEditFinished(opId(),
                                event.value(QStringLiteral("room_id")).toString(),
                                event.value(QStringLiteral("field")).toString(),
                                event.value(QStringLiteral("ok")).toBool(),
                                event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("room_leave_result")) {
        Q_EMIT roomLeaveFinished(opId(),
                                 event.value(QStringLiteral("room_id")).toString(),
                                 event.value(QStringLiteral("ok")).toBool(),
                                 event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("space_child_result")) {
        Q_EMIT spaceChildFinished(opId(),
                                  event.value(QStringLiteral("space_id")).toString(),
                                  event.value(QStringLiteral("room_id")).toString(),
                                  event.value(QStringLiteral("ok")).toBool());
        return true;
    }

    if (type == QLatin1String("space_child_removed_result")) {
        Q_EMIT spaceChildRemoveFinished(
            opId(),
            event.value(QStringLiteral("space_id")).toString(),
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("ok")).toBool());
        return true;
    }

    if (type == QLatin1String("attachment_send_result")) {
        Q_EMIT attachmentQueueFinished(
            opId(), event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("media_ready")) {
        handleMediaReady(event);
        return true;
    }

    if (type == QLatin1String("media_failed")) {
        Q_EMIT mediaFailed(opId(),
                           event.value(QStringLiteral("key")).toString(),
                           event.value(QStringLiteral("kind")).toInt(),
                           event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("upload_limit")) {
        const qint64 bytes =
            static_cast<qint64>(event.value(QStringLiteral("bytes")).toDouble());
        if (bytes > 0 && bytes != m_maxUploadSize) {
            m_maxUploadSize = bytes;
            Q_EMIT maxUploadSizeChanged();
        }
        return true;
    }

    return false;
}
