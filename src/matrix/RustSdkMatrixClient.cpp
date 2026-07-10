#include "matrix/RustSdkMatrixClient.h"

#include "app/SettingsManager.h"
#include "matrix/MediaHelpers.h"
#include "matrix/RustSessionPolicy.h"
#include "matrix_rust.h"
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
    if (event.type == TimelineEvent::Image)
        return event.mediaFilename.isEmpty() ? QStringLiteral("Image") : event.mediaFilename;
    if (event.type == TimelineEvent::File)
        return event.mediaFilename.isEmpty() ? QStringLiteral("File") : event.mediaFilename;
    return event.body;
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
    m_pagination.clear();
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

    const bool storeExists = pathExistsOrIsLink(identity.rustStorePath);
    const auto block = matrix::rust_session::passwordLoginBlockReason(
        identity,
        storeExists,
        m_settings && m_settings->hasSession(),
        m_settings ? m_settings->homeserverUrl() : QString{},
        m_settings ? m_settings->userId() : QString{},
        m_settings ? m_settings->deviceId() : QString{});
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
            m_rustHandle, roomBytes.constData(), bodyBytes.constData()));
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
        bodyBytes.constData()));
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
        bodyBytes.constData()));
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
    const QByteArray roomBytes = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_paginate_back(
        m_rustHandle, roomBytes.constData(), kPaginationBatch));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "timeline pagination dispatch failed";
        state.failed = true;
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

void RustSdkMatrixClient::retryFailedSend(const QString &roomId,
                                          const QString &transactionId)
{
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
    return m_rustHandle && !roomId.isEmpty()
        && (m_timelineTracker.activeRoom() == roomId
            || m_timelineTracker.requestedRoom() == roomId);
}

void RustSdkMatrixClient::openRoomTimeline(const QString &roomId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty())
        return;
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

void RustSdkMatrixClient::closeRoomTimeline()
{
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
    // verification_ready / verification_sas_started are informational —
    // handled by the sas_ready / done / cancelled path. Ignore.
    if (type == QLatin1String("verification_ready")
        || type == QLatin1String("verification_sas_started"))
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
    room.lastMessagePreview = obj.value(QStringLiteral("last_message_preview"))
                                  .toString(room.lastMessagePreview);
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
            const QString body = obj.value(QStringLiteral("body")).toString();
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

    // v0.5-prep+3: Rust bridges undecryptable encrypted events with
    // `undecryptable = true` and an empty body. Render an honest
    // placeholder here instead of an empty bubble. The SDK will
    // upgrade the event later (via `event_replaced`) if / when keys
    // arrive; until then the user sees WHY the timeline is silent.
    if (undecryptable && timelineEvent.body.isEmpty()) {
        timelineEvent.body = tr("[unable to decrypt yet]");
        timelineEvent.type = TimelineEvent::Notice;
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
        qCInfo(lcRust) << "timeline pagination started";
    } else if (paginationState == QLatin1String("idle")) {
        state.loading = false;
        state.failed = false;
        state.reachedStart =
            event.value(QStringLiteral("reached_start")).toBool(false);
        qCInfo(lcRust) << "timeline pagination complete reached_start="
                       << state.reachedStart;
    } else if (paginationState == QLatin1String("failed")) {
        state.loading = false;
        state.failed = true;
        qCWarning(lcRust) << "timeline pagination failed category="
                          << event.value(QStringLiteral("category")).toString();
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
    const QByteArray keyBytes = recoveryKey.toUtf8();
    const QString result = takeRustString(mx_rust_recover_from_backup(
        m_rustHandle, keyBytes.constData()));
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
