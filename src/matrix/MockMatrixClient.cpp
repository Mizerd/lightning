#include "matrix/MockMatrixClient.h"

#include "app/SettingsManager.h"
#include "matrix/MediaHelpers.h"

#include <QDateTime>
#include <QTimeZone>
#include <QFileInfo>
#include <QTimer>
#include <QUrl>

#include <algorithm>

MockMatrixClient::MockMatrixClient(QObject *parent)
    : MatrixClient(parent)
{
    // v0.7 startup-lifecycle env hooks (test/demo backend only): the
    // AppController constructor may restore the saved session before a
    // test can reach this instance, so the hooks must pre-exist.
    bool ok = false;
    const int delay = qEnvironmentVariableIntValue(
        "LIGHTNING_MOCK_RESTORE_DELAY_MS", &ok);
    if (ok && delay >= 0)
        m_restoreDelayMs = delay;
    if (qEnvironmentVariableIsSet("LIGHTNING_MOCK_FAIL_RESTORE"))
        m_failNextRestore = true;
    seedMockData();
}

void MockMatrixClient::login(const QString &homeserver,
                             const QString &user,
                             const QString &password)
{
    setState(Connecting);
    // Mimic the Rust backend: starting a login releases the previous
    // session's runtime, so the client is not logged in while the attempt
    // is in flight (this is what the add-account resilience tests exercise).
    m_loggedIn = false;
    m_homeserver = homeserver.isEmpty()
        ? QStringLiteral("https://mock.local") : homeserver;
    const QString localpart = user.isEmpty() ? QStringLiteral("alice") : user;
    QString host = QUrl(m_homeserver).host();
    if (host.isEmpty())
        host = QStringLiteral("mock.local");

    // Deterministic failure hook for lifecycle tests: the magic password
    // "mock-fail" rejects the attempt like a wrong-password server reply.
    if (password == QLatin1String("mock-fail")) {
        m_userId.clear();
        QTimer::singleShot(60, this, [this] {
            setState(Error);
            Q_EMIT loginFailed(QStringLiteral("mock: invalid credentials"));
        });
        return;
    }

    m_userId = QStringLiteral("@%1:%2").arg(localpart, host);
    QTimer::singleShot(120, this, [this] {
        m_loggedIn = true;
        Q_EMIT loginSucceeded(m_userId);
        setState(Disconnected);
    });
}

void MockMatrixClient::logout()
{
    stopSync();
    closeThread();
    m_loggedIn = false;
    m_userId.clear();
    Q_EMIT loggedOut();
}

bool MockMatrixClient::restoreSession()
{
    // v0.7: restore the active account from the persisted registry so the
    // account-switch lifecycle is exercisable without a network backend.
    if (!m_settings)
        return false;
    const QString uid = m_settings->userId();
    if (uid.isEmpty())
        return false;
    setState(Connecting);
    if (m_failNextRestore) {
        m_failNextRestore = false;
        QTimer::singleShot(m_restoreDelayMs, this, [this] {
            setState(Error);
            Q_EMIT loginFailed(QStringLiteral("mock: restore rejected"));
        });
        return true;
    }
    m_homeserver = m_settings->homeserverUrl();
    m_userId = uid;
    QTimer::singleShot(m_restoreDelayMs, this, [this] {
        m_loggedIn = true;
        Q_EMIT loginSucceeded(m_userId);
        setState(Disconnected);
    });
    return true;
}

bool MockMatrixClient::detachSession()
{
    stopSync();
    closeThread();
    m_loggedIn = false;
    m_userId.clear();
    m_homeserver.clear();
    Q_EMIT loggedOut();
    return true;
}

void MockMatrixClient::startSync()
{
    if (!m_loggedIn)
        return;
    setState(Syncing);
    Q_EMIT roomsChanged();
    for (const auto &r : m_rooms)
        Q_EMIT timelineReset(r.id);
}

void MockMatrixClient::stopSync()
{
    setState(Disconnected);
}

QList<RoomInfo> MockMatrixClient::rooms() const
{
    return m_rooms;
}

QList<TimelineEvent> MockMatrixClient::timeline(const QString &roomId) const
{
    return m_timelines.value(roomId);
}

QString MockMatrixClient::displayNameFor(const QString &roomId,
                                         const QString &userId) const
{
    for (const auto &r : m_rooms) {
        if (r.id != roomId) continue;
        const auto it = r.members.constFind(userId);
        if (it != r.members.constEnd() && !it->displayName.isEmpty())
            return it->displayName;
    }
    return userId;
}

QString MockMatrixClient::avatarMxcFor(const QString &roomId,
                                       const QString &userId) const
{
    for (const auto &r : m_rooms) {
        if (r.id != roomId) continue;
        const auto it = r.members.constFind(userId);
        if (it != r.members.constEnd())
            return it->avatarMxcUrl;
    }
    return {};
}

QStringList MockMatrixClient::typingUsersFor(const QString &roomId) const
{
    for (const auto &r : m_rooms)
        if (r.id == roomId)
            return r.typingUserIds;
    return {};
}

QUrl MockMatrixClient::mediaDownloadUrl(const QString &mxcUrl) const
{
    return matrix::media::downloadUrl(m_homeserver, mxcUrl);
}

QUrl MockMatrixClient::mediaThumbnailUrl(const QString &mxcUrl,
                                         int width, int height,
                                         bool crop) const
{
    return matrix::media::thumbnailUrl(m_homeserver, mxcUrl, width, height, crop);
}

TimelineEvent *MockMatrixClient::findEvent(const QString &roomId,
                                           const QString &eventId)
{
    auto it = m_timelines.find(roomId);
    if (it == m_timelines.end())
        return nullptr;
    for (auto &e : *it) {
        if (e.eventId == eventId)
            return &e;
    }
    return nullptr;
}

void MockMatrixClient::ackAfter(int ms, const QString &roomId,
                                const QString &eventId)
{
    QTimer::singleShot(ms, this, [this, roomId, eventId] {
        if (auto *ev = findEvent(roomId, eventId)) {
            ev->status = TimelineEvent::Sent;
            Q_EMIT eventStatusChanged(roomId, eventId, TimelineEvent::Sent);
        }
    });
}

void MockMatrixClient::sendTextMessage(const QString &roomId, const QString &body)
{
    if (!m_timelines.contains(roomId)) {
        Q_EMIT errorOccurred(tr("Unknown room: %1").arg(roomId));
        return;
    }

    TimelineEvent ev;
    ev.eventId           = nextEventId();
    ev.roomId            = roomId;
    ev.sender            = m_userId;
    ev.senderDisplayName = QStringLiteral("You");
    ev.body              = body;
    ev.timestamp         = QDateTime::currentDateTimeUtc();
    ev.type              = TimelineEvent::TextMessage;
    ev.status            = TimelineEvent::Sending;

    m_timelines[roomId].append(ev);
    Q_EMIT eventAppended(roomId, ev);
    ackAfter(150, roomId, ev.eventId);

    for (auto &room : m_rooms) {
        if (room.id == roomId) {
            room.lastMessagePreview = matrix::media::previewSnippet(body);
            room.lastActivity = ev.timestamp;
            Q_EMIT roomUpdated(roomId);
            break;
        }
    }
}

void MockMatrixClient::sendReply(const QString &roomId,
                                  const QString &replyToEventId,
                                  const QString &body)
{
    if (!m_timelines.contains(roomId)) {
        Q_EMIT errorOccurred(tr("Unknown room: %1").arg(roomId));
        return;
    }
    TimelineEvent ev;
    ev.eventId           = nextEventId();
    ev.roomId            = roomId;
    ev.sender            = m_userId;
    ev.senderDisplayName = QStringLiteral("You");
    ev.body              = body;
    ev.timestamp         = QDateTime::currentDateTimeUtc();
    ev.type              = TimelineEvent::TextMessage;
    ev.status            = TimelineEvent::Sending;
    ev.replyToEventId    = replyToEventId;
    if (auto *target = findEvent(roomId, replyToEventId)) {
        ev.replyToSender  = target->senderDisplayName.isEmpty()
                              ? target->sender : target->senderDisplayName;
        ev.replyToPreview = matrix::media::previewSnippet(target->body);
    }
    m_timelines[roomId].append(ev);
    Q_EMIT eventAppended(roomId, ev);
    ackAfter(150, roomId, ev.eventId);
}

void MockMatrixClient::sendThreadReply(const QString &roomId,
                                        const QString &threadRootEventId,
                                        const QString &body)
{
    if (!m_timelines.contains(roomId)) {
        Q_EMIT errorOccurred(tr("Unknown room: %1").arg(roomId));
        return;
    }
    TimelineEvent ev;
    ev.eventId           = nextEventId();
    ev.roomId            = roomId;
    ev.sender            = m_userId;
    ev.senderDisplayName = QStringLiteral("You");
    ev.body              = body;
    ev.timestamp         = QDateTime::currentDateTimeUtc();
    ev.type              = TimelineEvent::TextMessage;
    ev.status            = TimelineEvent::Sending;
    ev.threadRootId      = threadRootEventId;
    m_timelines[roomId].append(ev);
    Q_EMIT eventAppended(roomId, ev);
    ackAfter(150, roomId, ev.eventId);

    // Keep the open mock thread timeline in sync, exactly like the SDK's
    // thread-focused timeline would receive the reply.
    const QString timelineId = threadTimelineId(roomId, threadRootEventId);
    if (m_openThreadTimelineId == timelineId) {
        TimelineEvent threadCopy = ev;
        threadCopy.roomId = timelineId;
        m_timelines[timelineId].append(threadCopy);
        Q_EMIT eventAppended(timelineId, threadCopy);
        ackAfter(150, timelineId, ev.eventId);
    }
    if (m_openThreadListRoom == roomId)
        emitThreadList(roomId);
}

void MockMatrixClient::sendThreadReplyTo(const QString &roomId,
                                         const QString &threadRootEventId,
                                         const QString &inReplyToEventId,
                                         const QString &body)
{
    if (inReplyToEventId.isEmpty()) {
        sendThreadReply(roomId, threadRootEventId, body);
        return;
    }
    if (!m_timelines.contains(roomId)) {
        Q_EMIT errorOccurred(tr("Unknown room: %1").arg(roomId));
        return;
    }
    TimelineEvent ev;
    ev.eventId           = nextEventId();
    ev.roomId            = roomId;
    ev.sender            = m_userId;
    ev.senderDisplayName = QStringLiteral("You");
    ev.body              = body;
    ev.timestamp         = QDateTime::currentDateTimeUtc();
    ev.type              = TimelineEvent::TextMessage;
    ev.status            = TimelineEvent::Sending;
    ev.threadRootId      = threadRootEventId;
    ev.replyToEventId    = inReplyToEventId;
    if (auto *target = findEvent(roomId, inReplyToEventId)) {
        ev.replyToSender  = target->senderDisplayName.isEmpty()
                              ? target->sender : target->senderDisplayName;
        ev.replyToPreview = matrix::media::previewSnippet(target->body);
    }
    m_timelines[roomId].append(ev);
    Q_EMIT eventAppended(roomId, ev);
    ackAfter(150, roomId, ev.eventId);

    const QString timelineId = threadTimelineId(roomId, threadRootEventId);
    if (m_openThreadTimelineId == timelineId) {
        TimelineEvent threadCopy = ev;
        threadCopy.roomId = timelineId;
        m_timelines[timelineId].append(threadCopy);
        Q_EMIT eventAppended(timelineId, threadCopy);
        ackAfter(150, timelineId, ev.eventId);
    }
    if (m_openThreadListRoom == roomId)
        emitThreadList(roomId);
}

// v0.7 outgoing @-mentions: record what the composer delivered (expanded body
// + deduped ids) for the composer tests, then reuse the existing send paths.
void MockMatrixClient::sendTextMessage(const QString &roomId,
                                       const QString &body,
                                       const QStringList &mentionUserIds)
{
    m_lastSentBody = body;
    m_lastMentionIds = mentionUserIds;
    sendTextMessage(roomId, body);
}

void MockMatrixClient::sendReply(const QString &roomId,
                                 const QString &replyToEventId,
                                 const QString &body,
                                 const QStringList &mentionUserIds)
{
    m_lastSentBody = body;
    m_lastMentionIds = mentionUserIds;
    sendReply(roomId, replyToEventId, body);
}

void MockMatrixClient::editMessage(const QString &roomId,
                                   const QString &targetEventId,
                                   const QString &newBody,
                                   const QStringList &mentionUserIds)
{
    m_lastSentBody = newBody;
    m_lastMentionIds = mentionUserIds;
    editMessage(roomId, targetEventId, newBody);
}

void MockMatrixClient::sendThreadReplyTo(const QString &roomId,
                                         const QString &threadRootEventId,
                                         const QString &inReplyToEventId,
                                         const QString &body,
                                         const QStringList &mentionUserIds)
{
    m_lastSentBody = body;
    m_lastMentionIds = mentionUserIds;
    sendThreadReplyTo(roomId, threadRootEventId, inReplyToEventId, body);
}

quint64 MockMatrixClient::requestRoomMembers(const QString &roomId)
{
    const quint64 op = ++m_opCounter;
    QVariantList members;
    for (const auto &room : m_rooms) {
        if (room.id != roomId)
            continue;
        for (auto it = room.members.constBegin(); it != room.members.constEnd();
             ++it) {
            QVariantMap entry;
            entry.insert(QStringLiteral("userId"), it.key());
            entry.insert(QStringLiteral("displayName"), it.value().displayName);
            entry.insert(QStringLiteral("avatarUrl"), it.value().avatarMxcUrl);
            entry.insert(QStringLiteral("membership"), QStringLiteral("join"));
            entry.insert(QStringLiteral("role"), QStringLiteral("default"));
            entry.insert(QStringLiteral("ambiguous"), false);
            entry.insert(QStringLiteral("isOwn"), it.key() == m_userId);
            members.append(entry);
        }
        break;
    }
    QVariantMap snapshot;
    snapshot.insert(QStringLiteral("ok"), true);
    snapshot.insert(QStringLiteral("members"), members);
    // Asynchronous like the real backend, so the model's op-id is stored before
    // the snapshot arrives.
    QTimer::singleShot(0, this, [this, op, roomId, snapshot] {
        Q_EMIT roomMembersReceived(op, roomId, snapshot);
    });
    return op;
}

quint64 MockMatrixClient::appendThreadAttachment(const QString &roomId,
                                                 const QString &rootEventId,
                                                 const QString &fileName,
                                                 const QString &mime)
{
    if (!m_timelines.contains(roomId) || rootEventId.isEmpty())
        return 0;
    if (m_failNextThreadAttachment) {
        m_failNextThreadAttachment = false;
        return 0;   // queue rejection
    }
    ++m_threadAttachmentCalls;
    const quint64 opId = ++m_opCounter;

    TimelineEvent ev;
    ev.eventId           = nextEventId();
    ev.roomId            = roomId;
    ev.sender            = m_userId;
    ev.senderDisplayName = QStringLiteral("You");
    ev.body              = fileName;
    ev.timestamp         = QDateTime::currentDateTimeUtc();
    ev.type              = mime.startsWith(QLatin1String("image/"))
                               ? TimelineEvent::Image
                               : TimelineEvent::File;
    ev.status            = TimelineEvent::Sending;
    ev.threadRootId      = rootEventId;
    m_timelines[roomId].append(ev);
    Q_EMIT eventAppended(roomId, ev);
    ackAfter(150, roomId, ev.eventId);

    const QString timelineId = threadTimelineId(roomId, rootEventId);
    if (m_openThreadTimelineId == timelineId) {
        TimelineEvent threadCopy = ev;
        threadCopy.roomId = timelineId;
        m_timelines[timelineId].append(threadCopy);
        Q_EMIT eventAppended(timelineId, threadCopy);
        ackAfter(150, timelineId, ev.eventId);
    }
    if (m_openThreadListRoom == roomId)
        emitThreadList(roomId);

    // The SDK send queue reports acceptance asynchronously; mirror that so the
    // composer tray reconciliation is exercised like the real backend.
    QTimer::singleShot(50, this, [this, opId, roomId] {
        Q_EMIT attachmentQueueFinished(opId, roomId, true, QString());
    });
    return opId;
}

quint64 MockMatrixClient::sendThreadAttachment(const QString &roomId,
                                               const QString &rootEventId,
                                               const QString &localPath,
                                               const QString &mime,
                                               const QString &caption,
                                               int width, int height,
                                               bool animated)
{
    Q_UNUSED(caption); Q_UNUSED(width); Q_UNUSED(height); Q_UNUSED(animated);
    return appendThreadAttachment(roomId, rootEventId,
                                  QFileInfo(localPath).fileName(), mime);
}

quint64 MockMatrixClient::sendThreadAttachmentBytes(const QString &roomId,
                                                    const QString &rootEventId,
                                                    const QByteArray &bytes,
                                                    const QString &filename,
                                                    const QString &mime,
                                                    int width, int height)
{
    Q_UNUSED(bytes); Q_UNUSED(width); Q_UNUSED(height);
    return appendThreadAttachment(roomId, rootEventId, filename, mime);
}

// ── v0.6.0: mock thread timelines ────────────────────────────────────────

void MockMatrixClient::rebuildOpenThreadTimeline()
{
    if (m_openThreadTimelineId.isEmpty())
        return;
    const QString roomId = threadTimelineRoomId(m_openThreadTimelineId);
    const QString rootId = threadTimelineRootId(m_openThreadTimelineId);
    QList<TimelineEvent> thread;
    for (const auto &event : m_timelines.value(roomId)) {
        if (event.eventId != rootId && event.threadRootId != rootId)
            continue;
        TimelineEvent copy = event;
        copy.roomId = m_openThreadTimelineId;
        if (event.eventId == rootId)
            thread.prepend(copy);      // root pinned first, never duplicated
        else
            thread.append(copy);
    }
    m_timelines[m_openThreadTimelineId] = thread;
}

void MockMatrixClient::openThread(const QString &roomId,
                                  const QString &rootEventId)
{
    if (!m_timelines.contains(roomId)) {
        Q_EMIT threadTimelineFailed(roomId, rootEventId,
                                    QStringLiteral("unknown_room"));
        return;
    }
    const auto &roomTimeline = m_timelines.value(roomId);
    const bool rootExists = std::any_of(
        roomTimeline.cbegin(), roomTimeline.cend(),
        [&](const TimelineEvent &e) { return e.eventId == rootEventId; });
    if (!rootExists) {
        Q_EMIT threadTimelineFailed(roomId, rootEventId,
                                    QStringLiteral("unknown_root"));
        return;
    }
    closeThread();
    m_openThreadTimelineId = threadTimelineId(roomId, rootEventId);
    rebuildOpenThreadTimeline();
    Q_EMIT timelineReset(m_openThreadTimelineId);
}

void MockMatrixClient::closeThread()
{
    if (m_openThreadTimelineId.isEmpty())
        return;
    m_timelines.remove(m_openThreadTimelineId);
    m_openThreadTimelineId.clear();
}

// ── v0.6.0 checkpoint 5: mock thread list + follow state ─────────────────

void MockMatrixClient::emitThreadList(const QString &roomId)
{
    QVariantList threads;
    QHash<QString, QVariantMap> byRoot;
    QStringList order;
    for (const auto &event : m_timelines.value(roomId)) {
        if (event.threadRootId.isEmpty())
            continue;
        auto &entry = byRoot[event.threadRootId];
        if (entry.isEmpty()) {
            order.append(event.threadRootId);
            entry.insert(QStringLiteral("rootEventId"), event.threadRootId);
            entry.insert(QStringLiteral("replyCount"), 0);
            for (const auto &root : m_timelines.value(roomId)) {
                if (root.eventId != event.threadRootId)
                    continue;
                entry.insert(QStringLiteral("rootSender"), root.sender);
                entry.insert(QStringLiteral("rootSenderName"),
                             root.senderDisplayName.isEmpty()
                                 ? root.sender : root.senderDisplayName);
                entry.insert(QStringLiteral("rootPreview"),
                             matrix::media::previewSnippet(root.body));
                entry.insert(QStringLiteral("rootTimestamp"), root.timestamp);
                break;
            }
        }
        entry.insert(QStringLiteral("replyCount"),
                     entry.value(QStringLiteral("replyCount")).toInt() + 1);
        entry.insert(QStringLiteral("latestSender"), event.sender);
        entry.insert(QStringLiteral("latestSenderName"),
                     event.senderDisplayName.isEmpty()
                         ? event.sender : event.senderDisplayName);
        entry.insert(QStringLiteral("latestPreview"),
                     matrix::media::previewSnippet(event.body));
        entry.insert(QStringLiteral("latestTimestamp"), event.timestamp);
    }
    for (const auto &rootId : order)
        threads.append(byRoot.value(rootId));
    Q_EMIT threadListUpdated(roomId, threads, /*endReached=*/true,
                             /*failed=*/false);
}

void MockMatrixClient::openThreadList(const QString &roomId)
{
    if (!m_timelines.contains(roomId)) {
        Q_EMIT threadListUpdated(roomId, {}, true, true);
        return;
    }
    m_openThreadListRoom = roomId;
    emitThreadList(roomId);
}

void MockMatrixClient::closeThreadList()
{
    m_openThreadListRoom.clear();
}

void MockMatrixClient::paginateThreadList(const QString &roomId)
{
    // One deterministic page; end is always reached.
    if (roomId == m_openThreadListRoom)
        emitThreadList(roomId);
}

void MockMatrixClient::queryThreadSubscription(const QString &roomId,
                                               const QString &rootEventId)
{
    const QString key = roomId + QChar(0x1f) + rootEventId;
    Q_EMIT threadSubscriptionState(roomId, rootEventId, /*supported=*/true,
                                   m_threadSubscriptions.value(key, false),
                                   /*automatic=*/false);
}

void MockMatrixClient::setThreadSubscribed(const QString &roomId,
                                           const QString &rootEventId,
                                           bool subscribed)
{
    const QString key = roomId + QChar(0x1f) + rootEventId;
    m_threadSubscriptions.insert(key, subscribed);
    Q_EMIT threadSubscriptionResult(roomId, rootEventId, true, subscribed);
    Q_EMIT threadSubscriptionState(roomId, rootEventId, true, subscribed,
                                   false);
}

void MockMatrixClient::editMessage(const QString &roomId,
                                    const QString &targetEventId,
                                    const QString &newBody)
{
    auto *ev = findEvent(roomId, targetEventId);
    if (!ev) {
        Q_EMIT errorOccurred(tr("Cannot edit: original message not found."));
        return;
    }
    ev->body   = newBody;
    ev->edited = true;
    Q_EMIT eventEdited(roomId, targetEventId);

    for (auto &room : m_rooms) {
        if (room.id == roomId && !m_timelines[roomId].isEmpty() &&
            m_timelines[roomId].last().eventId == targetEventId) {
            room.lastMessagePreview = matrix::media::previewSnippet(newBody);
            Q_EMIT roomUpdated(roomId);
            break;
        }
    }
}

void MockMatrixClient::redactEvent(const QString &roomId,
                                    const QString &eventId,
                                    const QString &reason)
{
    Q_UNUSED(reason);
    auto *ev = findEvent(roomId, eventId);
    if (!ev) return;
    ev->redacted = true;
    ev->body.clear();
    // If it was a reaction, remove from parent.
    // Mock keeps reactions on their target event, so no removal needed by id.
    Q_EMIT eventRedacted(roomId, eventId);
}

void MockMatrixClient::toggleReaction(const QString &roomId,
                                       const QString &targetEventId,
                                       const QString &key)
{
    auto *ev = findEvent(roomId, targetEventId);
    if (!ev) return;
    bool matched = false;
    for (auto &r : ev->reactions) {
        if (r.key == key) {
            matched = true;
            if (r.byMe) {
                r.count = qMax(0, r.count - 1);
                r.byMe  = false;
                r.myEventId.clear();
            } else {
                r.count += 1;
                r.byMe   = true;
                r.myEventId = QStringLiteral("$mock-rx-%1").arg(nextTxnId());
            }
            break;
        }
    }
    if (!matched) {
        Reaction r;
        r.key = key;
        r.count = 1;
        r.byMe = true;
        r.myEventId = QStringLiteral("$mock-rx-%1").arg(nextTxnId());
        ev->reactions.append(r);
    }
    // Drop zero-count reactions.
    QList<Reaction> filtered;
    filtered.reserve(ev->reactions.size());
    for (const auto &r : ev->reactions)
        if (r.count > 0) filtered.append(r);
    ev->reactions = filtered;
    Q_EMIT reactionsChanged(roomId, targetEventId);
}

void MockMatrixClient::sendTyping(const QString &roomId, bool isTyping, int)
{
    Q_UNUSED(roomId);
    Q_UNUSED(isTyping);
    // Mock: swallow (no-op).
}

void MockMatrixClient::sendReadReceipt(const QString &roomId, const QString &eventId)
{
    Q_UNUSED(roomId);
    Q_UNUSED(eventId);
    // Mock: swallow.
}

void MockMatrixClient::sendImage(const QString &roomId, const QString &localPath)
{
    if (!m_timelines.contains(roomId)) {
        Q_EMIT errorOccurred(tr("Unknown room: %1").arg(roomId));
        return;
    }
    QFileInfo fi(localPath);
    TimelineEvent ev;
    ev.eventId           = nextEventId();
    ev.roomId            = roomId;
    ev.sender            = m_userId;
    ev.senderDisplayName = QStringLiteral("You");
    ev.body              = fi.fileName().isEmpty() ? tr("Image") : fi.fileName();
    ev.timestamp         = QDateTime::currentDateTimeUtc();
    ev.type              = TimelineEvent::Image;
    ev.status            = TimelineEvent::Sending;
    ev.mediaMxcUrl       = QStringLiteral("mxc://mock.local/mock-image-%1")
                               .arg(m_eventCounter);
    ev.mediaMimetype     = QStringLiteral("image/png");
    ev.mediaFilename     = ev.body;
    ev.mediaSize         = fi.size();
    m_timelines[roomId].append(ev);
    Q_EMIT eventAppended(roomId, ev);
    ackAfter(200, roomId, ev.eventId);
}

void MockMatrixClient::sendFile(const QString &roomId, const QString &localPath)
{
    if (!m_timelines.contains(roomId)) {
        Q_EMIT errorOccurred(tr("Unknown room: %1").arg(roomId));
        return;
    }
    QFileInfo fi(localPath);
    TimelineEvent ev;
    ev.eventId           = nextEventId();
    ev.roomId            = roomId;
    ev.sender            = m_userId;
    ev.senderDisplayName = QStringLiteral("You");
    ev.body              = fi.fileName().isEmpty() ? tr("File") : fi.fileName();
    ev.timestamp         = QDateTime::currentDateTimeUtc();
    ev.type              = TimelineEvent::File;
    ev.status            = TimelineEvent::Sending;
    ev.mediaMxcUrl       = QStringLiteral("mxc://mock.local/mock-file-%1")
                               .arg(m_eventCounter);
    ev.mediaMimetype     = matrix::media::mimetypeForFile(localPath);
    ev.mediaFilename     = ev.body;
    ev.mediaSize         = fi.size();
    m_timelines[roomId].append(ev);
    Q_EMIT eventAppended(roomId, ev);
    ackAfter(200, roomId, ev.eventId);
}

void MockMatrixClient::loadOlderMessages(const QString &roomId)
{
    if (!m_paginationRemaining.contains(roomId)) return;
    if (m_paginating.contains(roomId)) return;
    m_paginationFailed.remove(roomId);
    m_transientPaginationFailures.remove(roomId);
    if (m_failNextPagination) {
        m_failNextPagination = false;
        m_paginationFailed.insert(roomId);
        if (m_nextPaginationFailureTransient)
            m_transientPaginationFailures.insert(roomId);
        m_nextPaginationFailureTransient = false;
        Q_EMIT paginationStateChanged(roomId);
        return;
    }
    if (m_paginationRemaining[roomId] <= 0) {
        for (auto &r : m_rooms)
            if (r.id == roomId) r.paginationExhausted = true;
        Q_EMIT paginationStateChanged(roomId);
        return;
    }
    m_paginating.insert(roomId);
    Q_EMIT paginationStateChanged(roomId);

    QTimer::singleShot(m_paginationDelayMs, this, [this, roomId] {
        m_paginating.remove(roomId);
        int &remaining = m_paginationRemaining[roomId];
        if (remaining <= 0) {
            for (auto &r : m_rooms)
                if (r.id == roomId) r.paginationExhausted = true;
            Q_EMIT paginationStateChanged(roomId);
            return;
        }
        // Prepend a small chunk of synthetic older events (or the staged
        // test chunk, so hydration tests control exactly what arrives).
        const auto &existing = m_timelines[roomId];
        QDateTime start = existing.isEmpty()
            ? QDateTime::currentDateTimeUtc().addSecs(-3600)
            : existing.first().timestamp.addSecs(-60);

        QList<TimelineEvent> chunk;
        if (!m_paginationChunkOverride.isEmpty()) {
            chunk = m_paginationChunkOverride;
            for (auto &e : chunk) {
                if (e.eventId.isEmpty())
                    e.eventId = nextEventId();
                e.roomId = roomId;
            }
        } else {
            for (int i = 0; i < 3; ++i) {
                TimelineEvent e;
                e.eventId           = nextEventId();
                e.roomId            = roomId;
                e.sender            = QStringLiteral("@history-bot:mock.local");
                e.senderDisplayName = QStringLiteral("History Bot");
                e.body              = tr("Older message #%1 (page %2)")
                                          .arg(3 - i).arg(remaining);
                e.timestamp         = start.addSecs(-i * 60);
                e.type              = TimelineEvent::TextMessage;
                e.status            = TimelineEvent::Sent;
                chunk.prepend(e);
            }
        }
        auto &tl = m_timelines[roomId];
        for (int i = chunk.size() - 1; i >= 0; --i)
            tl.prepend(chunk.at(i));
        Q_EMIT eventsPrepended(roomId, chunk);
        remaining -= 1;
        if (remaining <= 0) {
            for (auto &r : m_rooms)
                if (r.id == roomId) r.paginationExhausted = true;
        }
        Q_EMIT paginationStateChanged(roomId);
    });
}

bool MockMatrixClient::canPaginate(const QString &roomId) const
{
    return m_paginationRemaining.value(roomId, 0) > 0;
}

bool MockMatrixClient::paginating(const QString &roomId) const
{
    return m_paginating.contains(roomId);
}

void MockMatrixClient::seedMockData()
{
    const auto now = QDateTime::currentDateTimeUtc();

    auto member = [](const QString &id, const QString &name) {
        MemberInfo m; m.userId = id; m.displayName = name; return m;
    };

    RoomInfo general;
    general.id                 = QStringLiteral("!general:mock.local");
    general.name               = QStringLiteral("General");
    general.topic              = QStringLiteral("Casual chat");
    general.lastMessagePreview = QStringLiteral("Welcome to the mock backend!");
    general.lastActivity       = now.addSecs(-60);
    general.unreadCount        = 2;
    general.members.insert(QStringLiteral("@alice:mock.local"),
                           member(QStringLiteral("@alice:mock.local"),
                                  QStringLiteral("Alice")));
    general.members.insert(QStringLiteral("@bob:mock.local"),
                           member(QStringLiteral("@bob:mock.local"),
                                  QStringLiteral("Bob")));
    general.members.insert(QStringLiteral("@carol:mock.local"),
                           member(QStringLiteral("@carol:mock.local"),
                                  QStringLiteral("Carol")));
    general.typingUserIds << QStringLiteral("@bob:mock.local");

    RoomInfo devs;
    devs.id                 = QStringLiteral("!devs:mock.local");
    devs.name               = QStringLiteral("Developers");
    devs.topic              = QStringLiteral("Client development discussion");
    devs.lastMessagePreview = QStringLiteral("Timeline model wiring works.");
    devs.lastActivity       = now.addSecs(-300);
    devs.encrypted          = true;

    RoomInfo dm;
    dm.id                 = QStringLiteral("!dm-bob:mock.local");
    // Named explicitly (rather than left to compute from the other
    // member) so the mock backend also exercises "explicit room name must
    // not disable member-avatar derivation".
    dm.name               = QStringLiteral("Bob");
    dm.topic              = QStringLiteral("Direct message");
    dm.lastMessagePreview = QStringLiteral("See you tomorrow.");
    dm.lastActivity       = now.addSecs(-3600);
    dm.isDirect           = true;
    dm.directUserId       = QStringLiteral("@bob:mock.local");
    dm.directUserIds      = { dm.directUserId };
    dm.members.insert(QStringLiteral("@bob:mock.local"),
                      member(QStringLiteral("@bob:mock.local"),
                             QStringLiteral("Bob")));

    // v0.4.1: one Space grouping general + devs. `dm` stays outside any
    // Space so QML can also render an "Other rooms" row.
    RoomInfo team;
    team.id                 = QStringLiteral("!space-team:mock.local");
    team.name               = QStringLiteral("Team");
    team.topic              = QStringLiteral("Mock Space containing General and Developers");
    team.isSpace            = true;
    team.childRoomIds       = { general.id, devs.id };
    team.lastActivity       = now.addSecs(-60);
    general.spaceId         = team.id;
    devs.spaceId            = team.id;

    m_rooms = { team, general, devs, dm };
    m_paginationRemaining[general.id] = 2;
    m_paginationRemaining[devs.id]    = 1;
    m_paginationRemaining[dm.id]      = 0;

    auto makeEvent = [this, &now](const QString &roomId, const QString &sender,
                                   const QString &name, const QString &body,
                                   int secondsAgo) {
        TimelineEvent e;
        e.eventId           = nextEventId();
        e.roomId            = roomId;
        e.sender            = sender;
        e.senderDisplayName = name;
        e.body              = body;
        e.timestamp         = now.addSecs(-secondsAgo);
        e.type              = TimelineEvent::TextMessage;
        e.status            = TimelineEvent::Sent;
        return e;
    };

    auto ev1 = makeEvent(general.id, QStringLiteral("@alice:mock.local"), "Alice",
                         QStringLiteral("Welcome to the mock backend!"), 600);
    auto ev2 = makeEvent(general.id, QStringLiteral("@bob:mock.local"), "Bob",
                         QStringLiteral("Everything you see here is fake."), 540);
    auto ev3 = makeEvent(general.id, QStringLiteral("@carol:mock.local"), "Carol",
                         QStringLiteral("Real Matrix arrives in v0.2."), 480);
    // A reply
    auto ev4 = makeEvent(general.id, QStringLiteral("@bob:mock.local"), "Bob",
                         QStringLiteral("Nice — replies show up as threaded previews."),
                         420);
    ev4.replyToEventId = ev1.eventId;
    ev4.replyToSender  = QStringLiteral("Alice");
    ev4.replyToPreview = QStringLiteral("Welcome to the mock backend!");
    // An edited message
    auto ev5 = makeEvent(general.id, QStringLiteral("@alice:mock.local"), "Alice",
                         QStringLiteral("This message was edited (v0.3)."), 360);
    ev5.edited = true;
    // A redacted message
    auto ev6 = makeEvent(general.id, QStringLiteral("@carol:mock.local"), "Carol",
                         QStringLiteral(""), 300);
    ev6.redacted = true;
    // An image
    auto ev7 = makeEvent(general.id, QStringLiteral("@alice:mock.local"), "Alice",
                         QStringLiteral("cat.png"), 240);
    ev7.type          = TimelineEvent::Image;
    ev7.mediaMxcUrl   = QStringLiteral("mxc://mock.local/cat");
    ev7.mediaMimetype = QStringLiteral("image/png");
    ev7.mediaFilename = QStringLiteral("cat.png");
    ev7.mediaSize     = 45123;
    // A file
    auto ev8 = makeEvent(general.id, QStringLiteral("@bob:mock.local"), "Bob",
                         QStringLiteral("notes.pdf"), 180);
    ev8.type          = TimelineEvent::File;
    ev8.mediaMxcUrl   = QStringLiteral("mxc://mock.local/notes");
    ev8.mediaMimetype = QStringLiteral("application/pdf");
    ev8.mediaFilename = QStringLiteral("notes.pdf");
    ev8.mediaSize     = 82344;
    // Reactions on ev2
    Reaction rx1; rx1.key = QStringLiteral("👍"); rx1.count = 2;
    Reaction rx2; rx2.key = QStringLiteral("❤️"); rx2.count = 1;
    ev2.reactions = { rx1, rx2 };
    // A thread root + two mock replies inside the thread.
    auto evThreadRoot = makeEvent(general.id, QStringLiteral("@carol:mock.local"), "Carol",
                                  QStringLiteral("Anyone up for a mock deploy tomorrow?"), 220);
    auto evThreadReply1 = makeEvent(general.id, QStringLiteral("@alice:mock.local"), "Alice",
                                    QStringLiteral("Sure, morning works."), 190);
    evThreadReply1.threadRootId = evThreadRoot.eventId;
    auto evThreadReply2 = makeEvent(general.id, QStringLiteral("@bob:mock.local"), "Bob",
                                    QStringLiteral("I'll take the afternoon slot."), 160);
    evThreadReply2.threadRootId = evThreadRoot.eventId;
    // ── Long-message scrolling regression fixtures (0.6.0) ──────────────
    // Deterministic bodies that exercise wheel motion while the viewport is
    // fully inside ONE tall delegate, framed by normal short messages.
    //
    // 1. Exactly 3,764 characters with no hard line break, so the delegate's
    //    Text wrapping alone produces a body far taller than the viewport.
    const QString longSentence = QStringLiteral(
        "This deterministic long-message fixture exists so wheel scrolling "
        "can be exercised while the viewport sits entirely inside one tall "
        "wrapped text delegate, with no item boundary to mask stepping. ");
    QString longWrappedBody;
    while (longWrappedBody.size() < 3764)
        longWrappedBody += longSentence;
    longWrappedBody.truncate(3764);
    auto evShortBeforeLong = makeEvent(
        general.id, QStringLiteral("@bob:mock.local"), "Bob",
        QStringLiteral("Short message before the long fixture."), 152);
    auto evLongWrapped = makeEvent(
        general.id, QStringLiteral("@carol:mock.local"), "Carol",
        longWrappedBody, 150);
    // 2. A many-line body substantially taller than any realistic viewport.
    auto evVeryTall = makeEvent(
        general.id, QStringLiteral("@alice:mock.local"), "Alice",
        QStringLiteral("Tall fixture line for scroll continuity checks.\n")
            .repeated(80).trimmed(), 140);
    // 3. A multiline body with links (link-preview and rich-text layout in a
    //    tall delegate).
    auto evMultilineLinks = makeEvent(
        general.id, QStringLiteral("@bob:mock.local"), "Bob",
        QStringLiteral("Reading list for the scroll test:\n"
                       "https://example.org/first-long-article\n"
                       "https://example.org/second-long-article\n"
                       "Both stay deterministic and offline."), 130);
    auto evShortAfterLong = makeEvent(
        general.id, QStringLiteral("@carol:mock.local"), "Carol",
        QStringLiteral("Short message after the long fixture."), 120);

    // Latest message
    auto ev9 = makeEvent(general.id, QStringLiteral("@alice:mock.local"), "Alice",
                         QStringLiteral("Type something below and press Send."), 60);

    m_timelines[general.id] = { ev1, ev2, ev3, ev4, ev5, ev6, ev7, ev8,
                                evThreadRoot, evThreadReply1, evThreadReply2,
                                evShortBeforeLong, evLongWrapped, evVeryTall,
                                evMultilineLinks, evShortAfterLong, ev9 };

    // Two consecutive room-activity (state-change) events after the last
    // message — matches what the Rust bridge produces for membership/
    // profile/room-settings changes, and lets the compact Expand/Collapse
    // UI be exercised (clicked, keyboard-activated) without a live server.
    auto stateEvent = [this, &now](const QString &roomId, const QString &stateKind,
                                    const QString &body, int secondsAgo) {
        TimelineEvent e;
        e.eventId    = nextEventId();
        e.roomId     = roomId;
        e.type       = TimelineEvent::StateChange;
        e.stateKind  = stateKind;
        e.body       = body;
        e.timestamp  = now.addSecs(-secondsAgo);
        return e;
    };

    auto decrypted = makeEvent(
        devs.id, QStringLiteral("@dave:mock.local"), "Dave",
        QStringLiteral("This fixture represents decrypted encrypted text."), 900);
    decrypted.isEncrypted = true;
    decrypted.isDecrypted = true;

    auto undecryptable = makeEvent(
        devs.id, QStringLiteral("@unknown:mock.local"), QString{},
        QStringLiteral("Unable to decrypt this message"), 700);
    undecryptable.isEncrypted = true;
    undecryptable.undecryptable = true;
    undecryptable.errorKind = QStringLiteral("session_missing");

    auto missingReply = makeEvent(
        devs.id, QStringLiteral("@eve:mock.local"), "Eve",
        QStringLiteral("Reply target is intentionally unavailable."), 600);
    missingReply.isEncrypted = true;
    missingReply.isDecrypted = true;
    missingReply.replyToEventId = QStringLiteral("$missing:mock.local");

    auto pendingMedia = makeEvent(
        devs.id, QStringLiteral("@frank:mock.local"), "Frank",
        QStringLiteral("encrypted-image.png"), 500);
    pendingMedia.type = TimelineEvent::Image;
    pendingMedia.isEncrypted = true;
    pendingMedia.isDecrypted = true;
    pendingMedia.mediaMimetype = QStringLiteral("image/png");
    pendingMedia.mediaFilename = pendingMedia.body;
    pendingMedia.mediaKey = QStringLiteral("mock-pending-encrypted-media");
    pendingMedia.mediaSourceAvailable = false;

    auto longDecrypted = makeEvent(
        devs.id, QStringLiteral("@dave:mock.local"), "Dave",
        QStringLiteral("Large encrypted timeline fixture line.\n").repeated(128),
        100);
    longDecrypted.isEncrypted = true;
    longDecrypted.isDecrypted = true;

    // v0.6.0: an encrypted thread — decrypted root, one decrypted reply and
    // one undecryptable reply — so encrypted-thread handling is testable
    // without a homeserver.
    auto encThreadRoot = makeEvent(
        devs.id, QStringLiteral("@dave:mock.local"), "Dave",
        QStringLiteral("Encrypted thread root fixture."), 450);
    encThreadRoot.isEncrypted = true;
    encThreadRoot.isDecrypted = true;
    auto encThreadReply = makeEvent(
        devs.id, QStringLiteral("@eve:mock.local"), "Eve",
        QStringLiteral("Encrypted thread reply fixture."), 430);
    encThreadReply.isEncrypted = true;
    encThreadReply.isDecrypted = true;
    encThreadReply.threadRootId = encThreadRoot.eventId;
    auto encThreadUtd = makeEvent(
        devs.id, QStringLiteral("@unknown:mock.local"), QString{},
        QStringLiteral("Unable to decrypt this message"), 410);
    encThreadUtd.isEncrypted = true;
    encThreadUtd.undecryptable = true;
    encThreadUtd.errorKind = QStringLiteral("session_missing");
    encThreadUtd.threadRootId = encThreadRoot.eventId;

    m_timelines[devs.id] = {
        decrypted,
        undecryptable,
        missingReply,
        pendingMedia,
        encThreadRoot,
        encThreadReply,
        encThreadUtd,
        stateEvent(devs.id, QStringLiteral("membership"),
                   QStringLiteral("Grace joined the room."), 200),
        stateEvent(devs.id, QStringLiteral("member_profile"),
                   QStringLiteral("Grace changed their display name."), 190),
        longDecrypted,
    };

    m_timelines[dm.id] = {
        makeEvent(dm.id, QStringLiteral("@bob:mock.local"), "Bob",
                  QStringLiteral("Hey, are you around?"), 7200),
        makeEvent(dm.id, QStringLiteral("@alice:mock.local"), "Alice",
                  QStringLiteral("Just for a bit."), 6900),
        makeEvent(dm.id, QStringLiteral("@bob:mock.local"), "Bob",
                  QStringLiteral("See you tomorrow."), 3600),
    };
}

void MockMatrixClient::setScreenshotDemoMode(bool on)
{
    if (m_screenshotDemoMode == on)
        return;
    m_screenshotDemoMode = on;
    if (on)
        seedScreenshotDemoData();
}

// Development-only: a polished, fully deterministic scene for promotional
// screenshots. Overwrites the compact shared test fixtures (m_rooms /
// m_timelines) — never called by tests, so those fixtures are unchanged.
// Everything (timestamps, event ids, ordering, unread counts) is fixed, so
// screenshots are reproducible across launches. No network; no real stores.
void MockMatrixClient::seedScreenshotDemoData()
{
    // Deterministic clock (NOT wall-clock) so relative timestamps are stable.
    const QDateTime base(QDate(2026, 7, 23), QTime(10, 24), QTimeZone::UTC);
    m_eventCounter = 0;              // deterministic $mock-N event ids
    m_rooms.clear();
    m_timelines.clear();
    m_paginationRemaining.clear();

    const QString self = QStringLiteral("@alex:lightning.example");

    // ── Fictional cast (stable ids / names) ─────────────────────────────
    struct Person { QString id; QString name; };
    const Person maya  { QStringLiteral("@maya:lightning.example"),   QStringLiteral("Maya Chen") };
    const Person jordan{ QStringLiteral("@jordan:lightning.example"), QStringLiteral("Jordan Lee") };
    const Person sam   { QStringLiteral("@sam:lightning.example"),    QStringLiteral("Sam Rivera") };
    const Person aisha { QStringLiteral("@aisha:lightning.example"),  QStringLiteral("Aisha Khan") };
    const Person noah  { QStringLiteral("@noah:lightning.example"),   QStringLiteral("Noah Williams") };
    const Person priya { QStringLiteral("@priya:lightning.example"),  QStringLiteral("Priya Shah") };
    const Person leo   { QStringLiteral("@leo:lightning.example"),    QStringLiteral("Leo Novak") };
    const Person alex  { self,                                        QStringLiteral("Alex Morgan") };

    auto mem = [](QHash<QString, MemberInfo> &m, const Person &p) {
        MemberInfo mi; mi.userId = p.id; mi.displayName = p.name;
        m.insert(p.id, mi);
    };

    // Per-room deterministic timeline builder.
    auto text = [&](const QString &room, const Person &p, const QString &body,
                    int minsAgo) {
        TimelineEvent e;
        e.eventId = nextEventId();
        e.roomId = room;
        e.sender = p.id;
        e.senderDisplayName = p.name;
        e.body = body;
        e.timestamp = base.addSecs(-minsAgo * 60);
        e.type = TimelineEvent::TextMessage;
        return e;
    };

    // ── Spaces ──────────────────────────────────────────────────────────
    RoomInfo friends;
    friends.id = QStringLiteral("!space-friends:lightning.example");
    friends.name = QStringLiteral("Friends");
    friends.topic = QStringLiteral("People I actually know");
    friends.isSpace = true;
    friends.lastActivity = base.addSecs(-8 * 60);

    RoomInfo studio;
    studio.id = QStringLiteral("!space-studio:lightning.example");
    studio.name = QStringLiteral("Creative Studio");
    studio.topic = QStringLiteral("Design, photography and music");
    studio.isSpace = true;
    studio.lastActivity = base.addSecs(-2 * 60);

    RoomInfo community;
    community.id = QStringLiteral("!space-community:lightning.example");
    community.name = QStringLiteral("Lightning Community");
    community.topic = QStringLiteral("Building a native Matrix client");
    community.isSpace = true;
    community.lastActivity = base.addSecs(-30 * 60);

    // ── Design Lounge — the main polished group chat ────────────────────
    RoomInfo design;
    design.id = QStringLiteral("!design-lounge:lightning.example");
    design.name = QStringLiteral("Design Lounge");
    design.topic = QStringLiteral("Where the launch visuals come together");
    design.spaceId = studio.id;
    design.lastActivity = base.addSecs(-2 * 60);
    design.lastMessagePreview = QStringLiteral("The dark theme looks great for the hero shot.");
    design.unreadCount = 3;
    design.highlightCount = 1;
    design.hasUnreadMessages = true;
    mem(design.members, alex); mem(design.members, maya);
    mem(design.members, jordan); mem(design.members, sam);
    mem(design.members, aisha);
    design.typingUserIds << maya.id;

    {
        auto e1 = text(design.id, jordan, QStringLiteral(
            "Morning! Ready to lock the launch screenshots today?"), 34);
        auto e2 = text(design.id, maya, QStringLiteral(
            "Yes — I pulled the room list, timeline and thread views into a board so we can compare them side by side."), 31);
        auto e3 = text(design.id, sam, QStringLiteral(
            "The new layout feels much faster. I especially like how calm the room list is now."), 27);
        auto e4 = text(design.id, jordan, QStringLiteral(
            "Should we shoot the blue or the violet theme for the hero image?"), 22);
        e4.replyToEventId = e3.eventId;
        e4.replyToSender = sam.name;
        e4.replyToPreview = QStringLiteral("The new layout feels much faster…");
        auto e5 = text(design.id, maya, QStringLiteral(
            "The dark theme looks great for the hero shot — deep background, the accent really pops."), 18);
        e5.reactions = {
            { QStringLiteral("👍"), 3, true, nextEventId() },
            { QStringLiteral("🔥"), 2, false, QString() },
        };
        auto e6 = text(design.id, aisha, QStringLiteral(
            "Agreed. @alex can you export the timeline at 1440×900 so the composer is fully visible?"), 12);
        e6.mentionsMe = true;
        auto e7 = text(design.id, alex, QStringLiteral(
            "On it — I'll grab Modern layout with the accent theme."), 8);
        e7.edited = true;
        auto e8 = text(design.id, maya, QStringLiteral("shot-timeline-dark.png"), 4);
        e8.type = TimelineEvent::Image;
        e8.body = QStringLiteral("shot-timeline-dark.png");
        e8.mediaMimetype = QStringLiteral("image/png");
        e8.mediaFilename = QStringLiteral("shot-timeline-dark.png");
        e8.mediaMxcUrl = QStringLiteral("mxc://lightning.example/shot-timeline");
        e8.mediaWidth = 1280; e8.mediaHeight = 800; e8.mediaSize = 284000;
        m_timelines[design.id] = { e1, e2, e3, e4, e5, e6, e7, e8 };
    }
    m_paginationRemaining[design.id] = 2;

    // ── Maya Chen — encrypted direct message ────────────────────────────
    RoomInfo dmMaya;
    dmMaya.id = QStringLiteral("!dm-maya:lightning.example");
    dmMaya.name = maya.name;
    dmMaya.isDirect = true;
    dmMaya.directUserId = maya.id;
    dmMaya.directUserIds = { maya.id };
    dmMaya.encrypted = true;
    dmMaya.spaceId = friends.id;
    dmMaya.lastActivity = base.addSecs(-40 * 60);
    dmMaya.lastMessagePreview = QStringLiteral("Perfect, thank you! 🙌");
    mem(dmMaya.members, alex); mem(dmMaya.members, maya);
    {
        auto d1 = text(dmMaya.id, maya, QStringLiteral(
            "Hey! Did the reference board come through?"), 52);
        auto d2 = text(dmMaya.id, alex, QStringLiteral(
            "Just landed — the palette is exactly what we talked about."), 49);
        auto d3 = text(dmMaya.id, maya, QStringLiteral("palette.png"), 45);
        d3.type = TimelineEvent::Image;
        d3.body = QStringLiteral("palette.png");
        d3.mediaMimetype = QStringLiteral("image/png");
        d3.mediaFilename = QStringLiteral("palette.png");
        d3.mediaMxcUrl = QStringLiteral("mxc://lightning.example/palette");
        d3.mediaWidth = 900; d3.mediaHeight = 900; d3.mediaSize = 120000;
        auto d4 = text(dmMaya.id, alex, QStringLiteral(
            "Love it. I'll wire it into the theme presets tonight."), 42);
        d4.replyToEventId = d3.eventId;
        d4.replyToSender = maya.name;
        d4.replyToPreview = QStringLiteral("palette.png");
        d4.reactions = { { QStringLiteral("❤️"), 1, false, QString() } };
        auto d5 = text(dmMaya.id, maya, QStringLiteral("Perfect, thank you! 🙌"), 40);
        m_timelines[dmMaya.id] = { d1, d2, d3, d4, d5 };
    }
    m_paginationRemaining[dmMaya.id] = 0;

    // ── Lightning Development — code, file, and a thread ────────────────
    RoomInfo dev;
    dev.id = QStringLiteral("!dev:lightning.example");
    dev.name = QStringLiteral("Lightning Development");
    dev.topic = QStringLiteral("Native Qt/QML + Rust Matrix SDK");
    dev.spaceId = community.id;
    dev.lastActivity = base.addSecs(-30 * 60);
    dev.lastMessagePreview = QStringLiteral("Shipped: smooth touchpad scrolling.");
    dev.unreadCount = 5;
    dev.hasUnreadMessages = true;
    mem(dev.members, alex); mem(dev.members, sam);
    mem(dev.members, noah); mem(dev.members, priya); mem(dev.members, leo);
    QString threadRootId;
    {
        auto c1 = text(dev.id, noah, QStringLiteral(
            "The timeline anchor rewrite is in. Scrolling is 1:1 with the touchpad now."), 92);
        auto c2 = text(dev.id, priya, QStringLiteral(
            "Nice. For the release notes, the key change is:"), 88);
        auto c3 = text(dev.id, priya, QStringLiteral(
            "```\n- Defer anchor corrections until the gesture settles\n- Drop per-delta geometry scans\n```"), 87);
        c3.formattedBody = QStringLiteral(
            "<pre><code>- Defer anchor corrections until the gesture settles\n"
            "- Drop per-delta geometry scans</code></pre>");
        auto c4 = text(dev.id, sam, QStringLiteral(
            "Use `userScrollActive` as the gate — that's the load-bearing bit."), 80);
        c4.formattedBody = QStringLiteral(
            "Use <code>userScrollActive</code> as the gate — that's the load-bearing bit.");
        auto c5 = text(dev.id, noah, QStringLiteral("release-notes-0.6.4.md"), 74);
        c5.type = TimelineEvent::File;
        c5.body = QStringLiteral("release-notes-0.6.4.md");
        c5.mediaMimetype = QStringLiteral("text/markdown");
        c5.mediaFilename = QStringLiteral("release-notes-0.6.4.md");
        c5.mediaMxcUrl = QStringLiteral("mxc://lightning.example/relnotes");
        c5.mediaSize = 4210;
        // A thread root with replies.
        auto root = text(dev.id, leo, QStringLiteral(
            "Should we backport the scroll fix to 0.6.x or hold for 0.7?"), 64);
        threadRootId = root.eventId;
        root.isThreadRoot = true;
        root.threadReplyCount = 4;
        root.threadLatestPreview = QStringLiteral("Agreed — backport it.");
        root.threadLatestKind = QStringLiteral("text");
        root.threadLatestSender = sam.id;
        root.threadLatestSenderDisplayName = sam.name;
        root.threadLatestTimestamp = base.addSecs(-58 * 60);
        auto c7 = text(dev.id, sam, QStringLiteral(
            "Shipped: smooth touchpad scrolling. Tagging it for the changelog."), 30);
        c7.reactions = {
            { QStringLiteral("🚀"), 4, true, nextEventId() },
            { QStringLiteral("🎉"), 2, false, QString() },
        };
        // Thread replies live in the ROOM timeline with threadRootId set: the
        // model filters true m.thread replies out of the main view, and
        // openThread() rebuilds the thread panel from them (root pinned first).
        auto tr = [&](const Person &p, const QString &body, int minsAgo) {
            TimelineEvent e = text(dev.id, p, body, minsAgo);
            e.threadRootId = threadRootId;
            return e;
        };
        m_timelines[dev.id] = {
            c1, c2, c3, c4, c5, root, c7,
            tr(priya, QStringLiteral("It's low-risk and user-visible."), 62),
            tr(noah, QStringLiteral("Tests are green on both configs."), 61),
            tr(alex, QStringLiteral("I can cut 0.6.4 this week."), 60),
            tr(sam, QStringLiteral("Agreed — backport it."), 58),
        };
    }
    m_paginationRemaining[dev.id] = 1;

    // ── Product Feedback — a poll ───────────────────────────────────────
    RoomInfo feedback;
    feedback.id = QStringLiteral("!feedback:lightning.example");
    feedback.name = QStringLiteral("Product Feedback");
    feedback.topic = QStringLiteral("What should we build next?");
    feedback.spaceId = community.id;
    feedback.lastActivity = base.addSecs(-70 * 60);
    feedback.lastMessagePreview = QStringLiteral("Poll: Which theme for the release screenshots?");
    mem(feedback.members, alex); mem(feedback.members, maya);
    mem(feedback.members, sam); mem(feedback.members, priya);
    {
        auto p1 = text(feedback.id, priya, QStringLiteral(
            "Quick vote before we finalise the store listing:"), 75);
        TimelineEvent poll = text(feedback.id, priya, QString(), 74);
        poll.type = TimelineEvent::Poll;
        poll.pollQuestion = QStringLiteral(
            "Which theme should we use for the release screenshots?");
        poll.pollKind = QStringLiteral("disclosed");
        poll.pollMaxSelections = 1;
        poll.pollTotalVoters = 9;
        poll.pollAnswers = {
            { QStringLiteral("a1"), QStringLiteral("Midnight"), 4, true },
            { QStringLiteral("a2"), QStringLiteral("Ocean"),    3, false },
            { QStringLiteral("a3"), QStringLiteral("Violet"),   2, false },
            { QStringLiteral("a4"), QStringLiteral("Light"),    0, false },
        };
        m_timelines[feedback.id] = { p1, poll };
    }
    m_paginationRemaining[feedback.id] = 0;

    // ── Photography — a media-heavy room ────────────────────────────────
    RoomInfo photo;
    photo.id = QStringLiteral("!photography:lightning.example");
    photo.name = QStringLiteral("Photography");
    photo.topic = QStringLiteral("Shots from the weekend");
    photo.spaceId = studio.id;
    photo.lastActivity = base.addSecs(-3 * 60 * 60);
    photo.lastMessagePreview = QStringLiteral("Golden hour by the coast 🌅");
    mem(photo.members, alex); mem(photo.members, aisha); mem(photo.members, jordan);
    {
        auto ph = [&](const Person &p, const QString &caption,
                      const QString &key, int w, int h, int minsAgo) {
            TimelineEvent e = text(photo.id, p, caption, minsAgo);
            e.type = TimelineEvent::Image;
            e.mediaMimetype = QStringLiteral("image/jpeg");
            e.mediaFilename = key + QStringLiteral(".jpg");
            e.mediaMxcUrl = QStringLiteral("mxc://lightning.example/") + key;
            e.mediaWidth = w; e.mediaHeight = h; e.mediaSize = 320000;
            return e;
        };
        m_timelines[photo.id] = {
            ph(aisha, QStringLiteral("Golden hour by the coast 🌅"),
               QStringLiteral("coast"), 1600, 1000, 200),
            ph(jordan, QStringLiteral("Portrait test, natural light"),
               QStringLiteral("portrait"), 800, 1200, 190),
            ph(aisha, QStringLiteral("Album cover crop"),
               QStringLiteral("square"), 1000, 1000, 185),
        };
        m_timelines[photo.id][0].reactions = {
            { QStringLiteral("😍"), 3, false, QString() } };
    }
    m_paginationRemaining[photo.id] = 0;

    // ── Release Announcements — announcement room ───────────────────────
    RoomInfo announce;
    announce.id = QStringLiteral("!announce:lightning.example");
    announce.name = QStringLiteral("Release Announcements");
    announce.topic = QStringLiteral("What's new in Lightning");
    announce.spaceId = community.id;
    announce.lastActivity = base.addSecs(-6 * 60 * 60);
    announce.lastMessagePreview = QStringLiteral("Lightning 0.6.3 is out 🎉");
    mem(announce.members, alex); mem(announce.members, priya);
    {
        auto a1 = text(announce.id, priya, QStringLiteral(
            "Lightning 0.6.3 is out 🎉 Smoother scrolling, reliable video, and a refreshed room list."), 360);
        a1.reactions = { { QStringLiteral("🎉"), 8, true, nextEventId() },
                         { QStringLiteral("⚡"), 5, false, QString() } };
        m_timelines[announce.id] = { a1 };
    }
    m_paginationRemaining[announce.id] = 0;

    // ── Music Discovery — favourite room ────────────────────────────────
    RoomInfo music;
    music.id = QStringLiteral("!music:lightning.example");
    music.name = QStringLiteral("Music Discovery");
    music.topic = QStringLiteral("Share what you're listening to");
    music.spaceId = friends.id;
    music.lastActivity = base.addSecs(-5 * 60 * 60);
    music.lastMessagePreview = QStringLiteral("This album is on repeat all week.");
    mem(music.members, alex); mem(music.members, jordan); mem(music.members, noah);
    m_timelines[music.id] = {
        text(music.id, jordan, QStringLiteral("This album is on repeat all week."), 300),
    };
    m_paginationRemaining[music.id] = 0;

    // ── An invite ───────────────────────────────────────────────────────
    RoomInfo invite;
    invite.id = QStringLiteral("!invite-founders:lightning.example");
    invite.name = QStringLiteral("Founders Lounge");
    invite.topic = QStringLiteral("Private space for the founding team");
    invite.membership = RoomInfo::Invited;
    invite.invitePending = true;
    invite.inviterUserId = sam.id;
    invite.inviterDisplayName = sam.name;
    invite.lastActivity = base.addSecs(-9 * 60 * 60);

    // Space membership.
    friends.childRoomIds   = { dmMaya.id, music.id };
    studio.childRoomIds    = { design.id, photo.id };
    community.childRoomIds  = { dev.id, feedback.id, announce.id };

    // Room-list order: Spaces first, then rooms by recency, DM, invite.
    m_rooms = { friends, studio, community,
                design, dev, dmMaya, feedback, photo, announce, music,
                invite };
}

void MockMatrixClient::setState(ConnectionState s)
{
    if (m_state == s)
        return;
    m_state = s;
    Q_EMIT connectionStateChanged(m_state);
}

QString MockMatrixClient::nextEventId()
{
    return QStringLiteral("$mock-%1:mock.local").arg(++m_eventCounter);
}

QString MockMatrixClient::nextTxnId()
{
    return QStringLiteral("mock-txn-%1").arg(++m_txnCounter);
}

// ── v0.7 timeline-hydration test hooks ──────────────────────────────────

void MockMatrixClient::resetTimelineForTest(const QString &roomId,
                                            const QList<TimelineEvent> &events,
                                            int paginationPages)
{
    QList<TimelineEvent> stamped = events;
    for (auto &e : stamped) {
        if (e.eventId.isEmpty())
            e.eventId = nextEventId();
        e.roomId = roomId;
    }
    m_timelines[roomId] = stamped;
    m_paginationRemaining[roomId] = paginationPages;
    for (auto &r : m_rooms) {
        if (r.id == roomId)
            r.paginationExhausted = paginationPages <= 0;
    }
    Q_EMIT timelineReset(roomId);
    Q_EMIT paginationStateChanged(roomId);
}

void MockMatrixClient::changeEventAtForTest(const QString &roomId, int index,
                                            const TimelineEvent &event)
{
    auto it = m_timelines.find(roomId);
    if (it == m_timelines.end() || index < 0 || index >= it->size())
        return;
    TimelineEvent stamped = event;
    stamped.roomId = roomId;
    (*it)[index] = stamped;
    Q_EMIT eventChangedAt(roomId, index, stamped);
}

void MockMatrixClient::appendEventForTest(const QString &roomId,
                                          const TimelineEvent &event)
{
    TimelineEvent stamped = event;
    if (stamped.eventId.isEmpty())
        stamped.eventId = nextEventId();
    stamped.roomId = roomId;
    m_timelines[roomId].append(stamped);
    Q_EMIT eventAppended(roomId, stamped);
}
