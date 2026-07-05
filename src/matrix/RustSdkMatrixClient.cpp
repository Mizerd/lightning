#include "matrix/RustSdkMatrixClient.h"

#include "app/SettingsManager.h"
#include "matrix/MediaHelpers.h"
#include "matrix_rust.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QStandardPaths>
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

QString sanitizeHomeserver(QString homeserver)
{
    homeserver = homeserver.trimmed();
    while (homeserver.endsWith(QLatin1Char('/')))
        homeserver.chop(1);
    return homeserver;
}

QString appDataRoot()
{
    QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (root.isEmpty())
        root = QDir::homePath() + QLatin1String("/.local/share/matrix-client");
    return root;
}

QString safeUserSlug(QString userId)
{
    userId = userId.trimmed();
    if (userId.startsWith(QLatin1Char('@')))
        userId.remove(0, 1);
    userId.replace(QLatin1Char(':'), QLatin1Char('_'));
    userId.replace(QLatin1Char('/'), QLatin1Char('_'));
    userId.replace(QLatin1Char('\\'), QLatin1Char('_'));
    if (userId.isEmpty())
        userId = QStringLiteral("_unknown");
    return userId;
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
    if (m_rustHandle) {
        mx_rust_stop_sync(m_rustHandle);
        mx_rust_destroy(m_rustHandle);
        m_rustHandle = nullptr;
    }
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

void RustSdkMatrixClient::clearLocalState(bool clearPersisted)
{
    m_loggedIn = false;
    m_homeserver.clear();
    m_userId.clear();
    m_deviceId.clear();
    m_rooms.clear();
    m_timelines.clear();
    m_pendingSends.clear();
    setInitialSyncDone(false);
    if (clearPersisted && m_settings)
        m_settings->clearSession();
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
    return appDataRoot()
        + QLatin1Char('/')
        + safeUserSlug(userIdForStore)
        + QLatin1String("/matrix-rust-sdk-store");
}

bool RustSdkMatrixClient::ensureRustHandleForUser(const QString &userIdForStore)
{
    const QString storePath = rustStorePathForUser(userIdForStore);
    if (m_rustHandle && m_storePath == storePath) {
        ensurePollTimer();
        return true;
    }

    if (m_rustHandle) {
        mx_rust_stop_sync(m_rustHandle);
        mx_rust_destroy(m_rustHandle);
        m_rustHandle = nullptr;
    }

    if (!QDir().mkpath(storePath)) {
        Q_EMIT errorOccurred(tr("Failed to create Rust SDK store directory: %1").arg(storePath));
        return false;
    }

    const QByteArray path = QFileInfo(storePath).absoluteFilePath().toUtf8();
    m_rustHandle = mx_rust_create(path.constData());
    if (!m_rustHandle) {
        Q_EMIT errorOccurred(tr("Failed to create Rust SDK backend handle."));
        return false;
    }

    m_storePath = storePath;
    ensurePollTimer();
    return true;
}

void RustSdkMatrixClient::login(const QString &homeserver,
                                const QString &user,
                                const QString &password)
{
    const QString hs = sanitizeHomeserver(homeserver);
    const QString loginUser = user.trimmed();
    if (hs.isEmpty() || loginUser.isEmpty() || password.isEmpty()) {
        Q_EMIT loginFailed(tr("Homeserver, user, and password are required."));
        return;
    }

    if (!ensureRustHandleForUser(userIdForLoginStore(hs, loginUser))) {
        setState(Error);
        Q_EMIT loginFailed(tr("Rust SDK backend could not be initialized."));
        return;
    }

    stopSync();
    m_homeserver = hs;
    m_userId.clear();
    m_deviceId.clear();
    m_loggedIn = false;
    m_rooms.clear();
    m_timelines.clear();
    m_pendingSends.clear();
    setInitialSyncDone(false);
    Q_EMIT roomsChanged();
    setState(Connecting);

    const QByteArray hsBytes = hs.toUtf8();
    const QByteArray userBytes = loginUser.toUtf8();
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

    const QString hs = sanitizeHomeserver(m_settings->homeserverUrl());
    const QString userId = m_settings->userId();
    const QString deviceId = m_settings->deviceId();
    const QString accessToken = m_settings->accessToken();
    if (hs.isEmpty() || userId.isEmpty() || accessToken.isEmpty())
        return false;

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

void RustSdkMatrixClient::logout()
{
    if (m_rustHandle)
        mx_rust_logout(m_rustHandle);
    else
        clearLocalState(true);
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
    if (m_rustHandle)
        mx_rust_stop_sync(m_rustHandle);
    if (m_state == Syncing)
        setState(Disconnected);
}

QList<RoomInfo> RustSdkMatrixClient::rooms() const
{
    QList<RoomInfo> list;
    list.reserve(m_rooms.size());
    for (const auto &room : m_rooms)
        list.append(room);
    std::sort(list.begin(), list.end(), [](const RoomInfo &a, const RoomInfo &b) {
        if (a.lastActivity.isValid() && b.lastActivity.isValid())
            return a.lastActivity > b.lastActivity;
        if (a.lastActivity.isValid())
            return true;
        if (b.lastActivity.isValid())
            return false;
        return a.name.toLower() < b.name.toLower();
    });
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

void RustSdkMatrixClient::sendReply(const QString &, const QString &, const QString &)
{
    refuseSend("sendReply");
}

void RustSdkMatrixClient::editMessage(const QString &, const QString &, const QString &)
{
    refuseSend("editMessage");
}

void RustSdkMatrixClient::redactEvent(const QString &, const QString &, const QString &)
{
    refuseSend("redactEvent");
}

void RustSdkMatrixClient::toggleReaction(const QString &, const QString &, const QString &)
{
    refuseSend("toggleReaction");
}

void RustSdkMatrixClient::sendTyping(const QString &, bool, int)
{
}

void RustSdkMatrixClient::sendReadReceipt(const QString &, const QString &)
{
}

void RustSdkMatrixClient::sendImage(const QString &, const QString &)
{
    refuseSend("sendImage");
}

void RustSdkMatrixClient::sendFile(const QString &, const QString &)
{
    refuseSend("sendFile");
}

void RustSdkMatrixClient::loadOlderMessages(const QString &)
{
}

void RustSdkMatrixClient::refuseSend(const char *op)
{
    Q_EMIT errorOccurred(tr("Rust SDK backend does not implement %1 yet.").arg(QLatin1String(op)));
}

void RustSdkMatrixClient::pollRustEvents()
{
    if (!m_rustHandle)
        return;

    for (int i = 0; i < 64; ++i) {
        const QString raw = takeRustString(mx_rust_poll_event(m_rustHandle));
        if (raw.isEmpty())
            break;

        const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
        if (!doc.isObject()) {
            qCWarning(lcRust) << "discarding malformed Rust SDK event";
            continue;
        }
        handleRustEvent(doc.object());
    }
}

void RustSdkMatrixClient::handleRustEvent(const QJsonObject &event)
{
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
        m_homeserver = sanitizeHomeserver(event.value(QStringLiteral("homeserver")).toString(m_homeserver));
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
        Q_EMIT loginFailed(event.value(QStringLiteral("message")).toString(
            tr("Rust SDK login failed.")));
        return;
    }

    if (type == QLatin1String("logged_out")) {
        clearLocalState(true);
        Q_EMIT loggedOut();
        return;
    }

    if (type == QLatin1String("rooms")) {
        handleRoomsEvent(event.value(QStringLiteral("rooms")).toArray());
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

    if (type == QLatin1String("send_ok")) {
        handleSendOk(event);
        return;
    }

    if (type == QLatin1String("send_failed")) {
        handleSendFailed(event);
        return;
    }

    if (type == QLatin1String("sync_error")) {
        setState(Error);
        Q_EMIT errorOccurred(event.value(QStringLiteral("message")).toString(
            tr("Rust SDK sync failed.")));
        return;
    }

    if (type == QLatin1String("error")) {
        Q_EMIT errorOccurred(event.value(QStringLiteral("message")).toString(
            tr("Rust SDK backend error.")));
    }
}

void RustSdkMatrixClient::handleRoomsEvent(const QJsonArray &rooms)
{
    QHash<QString, RoomInfo> nextRooms;
    nextRooms.reserve(rooms.size());

    for (const auto &value : rooms) {
        const QJsonObject obj = value.toObject();
        RoomInfo room;
        room.id = obj.value(QStringLiteral("id")).toString();
        if (room.id.isEmpty())
            continue;

        const auto oldIt = m_rooms.constFind(room.id);
        if (oldIt != m_rooms.constEnd())
            room = *oldIt;

        room.id = obj.value(QStringLiteral("id")).toString(room.id);
        room.name = obj.value(QStringLiteral("name")).toString(room.name);
        if (room.name.isEmpty())
            room.name = room.id;
        room.topic = obj.value(QStringLiteral("topic")).toString(room.topic);
        room.avatarUrl = obj.value(QStringLiteral("avatar_url")).toString(room.avatarUrl);
        room.lastMessagePreview = obj.value(QStringLiteral("last_message_preview"))
                                      .toString(room.lastMessagePreview);
        const qint64 lastActivityMs = static_cast<qint64>(
            obj.value(QStringLiteral("last_activity_ms")).toDouble(0));
        const QDateTime activity = timestampFromMs(lastActivityMs);
        if (activity.isValid())
            room.lastActivity = activity;
        room.unreadCount = obj.value(QStringLiteral("unread_count")).toInt(room.unreadCount);
        room.encrypted = obj.value(QStringLiteral("encrypted")).toBool(room.encrypted);
        room.isSpace = obj.value(QStringLiteral("is_space")).toBool(room.isSpace);
        room.prevBatchToken = obj.value(QStringLiteral("prev_batch")).toString(room.prevBatchToken);
        room.childRoomIds.clear();
        for (const auto &child : obj.value(QStringLiteral("child_room_ids")).toArray()) {
            const QString childId = child.toString();
            if (!childId.isEmpty())
                room.childRoomIds.append(childId);
        }

        nextRooms.insert(room.id, room);
    }

    for (auto it = nextRooms.begin(); it != nextRooms.end(); ++it) {
        if (!it->isSpace)
            it->spaceId.clear();
    }
    for (auto it = nextRooms.constBegin(); it != nextRooms.constEnd(); ++it) {
        if (!it->isSpace)
            continue;
        for (const QString &childId : it->childRoomIds) {
            auto childIt = nextRooms.find(childId);
            if (childIt != nextRooms.end() && childIt->spaceId.isEmpty())
                childIt->spaceId = it->id;
        }
    }

    m_rooms = nextRooms;
    Q_EMIT roomsChanged();
}

void RustSdkMatrixClient::handleTimelineEvent(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const QJsonObject obj = event.value(QStringLiteral("event")).toObject();
    const QString eventId = obj.value(QStringLiteral("event_id")).toString();
    if (roomId.isEmpty() || eventId.isEmpty())
        return;

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

    timeline.append(timelineEvent);
    Q_EMIT eventAppended(roomId, timelineEvent);

    auto roomIt = m_rooms.find(roomId);
    if (roomIt != m_rooms.end()) {
        roomIt->lastMessagePreview = previewFor(timelineEvent);
        roomIt->lastActivity = timelineEvent.timestamp;
        Q_EMIT roomUpdated(roomId);
    }
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
