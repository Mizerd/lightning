#include "matrix/MockMatrixClient.h"

#include "app/SettingsManager.h"
#include "matrix/MediaHelpers.h"

#include <QDateTime>
#include <QTimeZone>
#include <QByteArray>
#include <QFile>
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
    // Demo mode: point the live dataset at this account's scene before sync.
    if (m_screenshotDemoMode)
        activateDemoAccount(m_userId);
    QTimer::singleShot(120, this, [this] {
        m_loggedIn = true;
        Q_EMIT loginSucceeded(m_userId);
        // startSync() (triggered synchronously by loginSucceeded) set Syncing;
        // the demo presents as a healthy "Connected" client (so the status
        // footer stays quiet for clean screenshots) rather than dropping to the
        // mock's usual idle state.
        setState(m_screenshotDemoMode ? Syncing : Disconnected);
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
    // Demo mode: swap the live dataset to this account's scene BEFORE the
    // async loginSucceeded so startSync rebuilds the correct rooms/timelines.
    // Preserves the previous account's local mutations (snapshot on switch).
    if (m_screenshotDemoMode)
        activateDemoAccount(uid);
    QTimer::singleShot(m_restoreDelayMs, this, [this] {
        m_loggedIn = true;
        Q_EMIT loginSucceeded(m_userId);
        // Demo presents as "Connected" (see login()); other backends idle.
        setState(m_screenshotDemoMode ? Syncing : Disconnected);
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
    if (m_screenshotDemoMode && m_demoHideUnread) {
        // Panel "hide unread badges": a read-time filter, fully reversible.
        QList<RoomInfo> out = m_rooms;
        for (RoomInfo &r : out) {
            r.unreadCount = 0;
            r.highlightCount = 0;
            r.hasUnreadMessages = false;
            r.markedUnread = false;
        }
        return out;
    }
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
    if (m_screenshotDemoMode && m_demoTypingSuppressed)
        return {};   // panel "hide typing indicators"
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

// ── Development-only screenshot-demo media bridge ────────────────────────
// Resolve a demo media key ("coast", "loop", "avatar-alex", …) to bundled
// fixture bytes. The fixtures are QRC resources present only in a
// LIGHTNING_ENABLE_SCREENSHOT_DEMO build; a miss (or any non-demo build) simply
// returns false and the caller reports the fetch unavailable.
bool MockMatrixClient::loadDemoFixture(const QString &key, QByteArray *bytes,
                                       QString *mime)
{
    if (key.isEmpty())
        return false;
    QString file;
    QString mimeType = QStringLiteral("image/png");
    if (key == QLatin1String("loop")) {
        file = QStringLiteral("loop.gif");
        mimeType = QStringLiteral("image/gif");
    } else if (key == QLatin1String("relnotes")) {
        file = QStringLiteral("release-notes.txt");
        mimeType = QStringLiteral("text/plain");
    } else {
        // Avatars ("avatar-<name>") and images ("coast"/"portrait"/… and the
        // video poster "timelapse") are all PNG fixtures named by their key.
        file = key + QStringLiteral(".png");
    }
    QFile f(QStringLiteral(
                ":/qt/qml/MatrixClient/resources/screenshot-demo/") + file);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    if (bytes)
        *bytes = f.readAll();
    if (mime)
        *mime = mimeType;
    return true;
}

quint64 MockMatrixClient::fetchMedia(const QString &mediaKey, int kind,
                                     int timeoutClass)
{
    Q_UNUSED(timeoutClass);
    if (!m_screenshotDemoMode)
        return 0;
    QByteArray bytes;
    QString mime;
    if (!loadDemoFixture(mediaKey, &bytes, &mime))
        return 0;   // unknown key → MediaBridge marks a transient "unavailable"
    const quint64 op = ++m_mediaOpCounter;
    const QString filename = mediaKey;
    // Deliver on the event loop so the bridge's dispatch bookkeeping (which
    // inserts the op AFTER fetchMedia returns) has recorded this op first.
    QTimer::singleShot(0, this, [this, op, mediaKey, kind, bytes, mime, filename] {
        Q_EMIT mediaReady(op, mediaKey, kind, bytes, mime, filename);
    });
    return op;
}

quint64 MockMatrixClient::fetchMxcThumbnail(const QString &mxc, int width,
                                            int height)
{
    Q_UNUSED(width);
    Q_UNUSED(height);
    if (!m_screenshotDemoMode) {
        // Test-only avatar bytes (receipt-chip avatar suite): resolve a
        // registered mxc through the SAME async op-id contract the Rust
        // backend uses. Unregistered keys keep the honest "unsupported"
        // rejection.
        const auto it = m_avatarBytesForTest.constFind(mxc);
        if (!m_mediaBridgeSupportedForTest
            || it == m_avatarBytesForTest.constEnd())
            return 0;
        const quint64 op = ++m_mediaOpCounter;
        const QByteArray bytes = it->bytes;
        const QString mime = it->mime;
        QTimer::singleShot(0, this, [this, op, mxc, bytes, mime] {
            Q_EMIT mediaReady(op, mxc, 2, bytes, mime, mxc);
        });
        return op;
    }
    // The avatar key is the mxc's last path segment ("avatar-alex").
    const QString key = mxc.section(QLatin1Char('/'), -1);
    QByteArray bytes;
    QString mime;
    if (!loadDemoFixture(key, &bytes, &mime))
        return 0;
    const quint64 op = ++m_mediaOpCounter;
    QTimer::singleShot(0, this, [this, op, mxc, bytes, mime] {
        // kind 2 == mxc thumbnail; only the op id is matched by the bridge.
        Q_EMIT mediaReady(op, mxc, 2, bytes, mime, mxc);
    });
    return op;
}

void MockMatrixClient::finalizeDemoMedia(DemoAccount &acct)
{
    for (auto it = acct.timelines.begin(); it != acct.timelines.end(); ++it) {
        for (TimelineEvent &e : it.value()) {
            const bool imageLike = e.type == TimelineEvent::Image
                || e.type == TimelineEvent::Video
                || e.type == TimelineEvent::Sticker;
            if (!imageLike || e.mediaMxcUrl.isEmpty())
                continue;
            // Route image/video/GIF rows through the demo media bridge: the key
            // is the fixture name (mxc's last segment). File/audio rows are left
            // metadata-only — their cards render without fetching bytes.
            e.mediaKey = e.mediaMxcUrl.section(QLatin1Char('/'), -1);
            e.mediaSourceAvailable = true;
            e.mediaThumbAvailable = true;
        }
    }
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

namespace {
// A fictional demo participant. `avatarMxc` is a stable opaque id resolved to a
// bundled fixture by the demo media path; empty until an avatar is assigned.
struct DemoPerson {
    QString id;
    QString name;
    QString avatarMxc;
};
void demoMem(QHash<QString, MemberInfo> &m, const DemoPerson &p)
{
    MemberInfo mi;
    mi.userId = p.id;
    mi.displayName = p.name;
    mi.avatarMxcUrl = p.avatarMxc;
    m.insert(p.id, mi);
}
QString demoAvatar(const QString &localpart)
{
    return QStringLiteral("mxc://lightning.example/avatar-") + localpart;
}
}

// Development-only: three polished, fully deterministic fictional accounts for
// promotional screenshots. Each account owns a complete scene; the active one is
// mirrored into the live working copy (m_rooms/m_timelines). Never called by
// tests, so the shared mock fixtures they assert on are unchanged. Everything
// (timestamps, event ids, ordering, unread counts) is fixed, so screenshots are
// reproducible across launches. No network; no real stores.
void MockMatrixClient::seedScreenshotDemoData()
{
    m_demoAccounts.clear();
    m_demoAccountOrder.clear();
    auto add = [&](DemoAccount a) {
        finalizeDemoMedia(a);   // tag media rows for the demo bridge
        // No backward-pagination in the demo: scrolling up must never reveal the
        // mock's generic "Older message #N (page N)" filler in a screenshot.
        a.paginationRemaining.clear();
        m_demoAccountOrder << a.userId;
        m_demoAccounts.insert(a.userId, a);
    };
    add(buildDemoAccountAlex());
    add(buildDemoAccountTaylor());
    add(buildDemoAccountNova());
    // Activate the primary account so tests that enable demo mode and read
    // rooms()/timeline() straight away (with no login) still see Alex's scene.
    m_activeDemoUser.clear();
    activateDemoAccount(QStringLiteral("@alex:lightning.example"));
}

MockMatrixClient::DemoAccount MockMatrixClient::buildDemoAccountAlex()
{
    const QDateTime base(QDate(2026, 7, 23), QTime(10, 24), QTimeZone::UTC);
    const QString hs = QStringLiteral("lightning.example");
    DemoAccount acct;
    acct.userId = QStringLiteral("@alex:lightning.example");
    acct.homeserver = hs;
    acct.displayName = QStringLiteral("Alex Morgan");
    acct.avatarMxc = demoAvatar(QStringLiteral("alex"));
    acct.defaultRoomId = QStringLiteral("!design-lounge:lightning.example");

    int n = 0;
    auto eid = [&]() {
        return QStringLiteral("$demo-alex-%1:%2").arg(++n).arg(hs);
    };

    const DemoPerson maya  { QStringLiteral("@maya:lightning.example"),   QStringLiteral("Maya Chen"),     demoAvatar(QStringLiteral("maya")) };
    const DemoPerson jordan{ QStringLiteral("@jordan:lightning.example"), QStringLiteral("Jordan Lee"),    demoAvatar(QStringLiteral("jordan")) };
    const DemoPerson sam   { QStringLiteral("@sam:lightning.example"),    QStringLiteral("Sam Rivera"),    demoAvatar(QStringLiteral("sam")) };
    const DemoPerson aisha { QStringLiteral("@aisha:lightning.example"),  QStringLiteral("Aisha Khan"),    demoAvatar(QStringLiteral("aisha")) };
    const DemoPerson noah  { QStringLiteral("@noah:lightning.example"),   QStringLiteral("Noah Williams"), demoAvatar(QStringLiteral("noah")) };
    const DemoPerson priya { QStringLiteral("@priya:lightning.example"),  QStringLiteral("Priya Shah"),    demoAvatar(QStringLiteral("priya")) };
    const DemoPerson leo   { QStringLiteral("@leo:lightning.example"),    QStringLiteral("Leo Novak"),     demoAvatar(QStringLiteral("leo")) };
    const DemoPerson alex  { acct.userId,                                 acct.displayName,                acct.avatarMxc };

    auto text = [&](const QString &room, const DemoPerson &p,
                    const QString &body, int minsAgo) {
        TimelineEvent e;
        e.eventId = eid();
        e.roomId = room;
        e.sender = p.id;
        e.senderDisplayName = p.name;
        e.senderAvatarUrl = p.avatarMxc;
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
    demoMem(design.members, alex); demoMem(design.members, maya);
    demoMem(design.members, jordan); demoMem(design.members, sam);
    demoMem(design.members, aisha);
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
            { QStringLiteral("👍"), 3, true, eid() },
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
        auto e9 = text(design.id, sam, QStringLiteral(
            "That crop is perfect. The composer being visible really sells the density."), 3);
        e9.replyToEventId = e8.eventId;
        e9.replyToSender = maya.name;
        e9.replyToPreview = QStringLiteral("shot-timeline-dark.png");
        auto e10 = text(design.id, jordan, QStringLiteral(
            "One nit — can we get a shot with the thread panel open too? "
            "It is the feature people ask about most."), 3);
        auto e11 = text(design.id, maya, QStringLiteral("Already queued 👍"), 2);
        e11.reactions = {
            { QStringLiteral("🎉"), 4, true, eid() },
            { QStringLiteral("👏"), 2, false, QString() },
        };
        acct.timelines[design.id] = { e1, e2, e3, e4, e5, e6, e7, e8,
                                      e9, e10, e11 };
    }
    acct.paginationRemaining[design.id] = 2;

    // ── Weekend Plans — a light casual group chat ───────────────────────
    RoomInfo weekend;
    weekend.id = QStringLiteral("!weekend:lightning.example");
    weekend.name = QStringLiteral("Weekend Plans");
    weekend.topic = QStringLiteral("Where are we going this weekend?");
    weekend.spaceId = friends.id;
    weekend.lastActivity = base.addSecs(-90 * 60);
    weekend.lastMessagePreview = QStringLiteral("The coast trail it is 🥾");
    demoMem(weekend.members, alex); demoMem(weekend.members, jordan);
    demoMem(weekend.members, aisha);
    {
        auto w1 = text(weekend.id, jordan, QStringLiteral(
            "Coast trail or the lake this Saturday?"), 140);
        auto w2 = text(weekend.id, aisha, QStringLiteral(
            "Coast — the light is perfect for photos in the morning."), 132);
        w2.reactions = { { QStringLiteral("📸"), 2, true, eid() } };
        auto w3 = text(weekend.id, jordan, QStringLiteral(
            "How early are we talking? I need to know how much coffee to make."), 128);
        w3.replyToEventId = w2.eventId;
        w3.replyToSender = aisha.name;
        w3.replyToPreview = QStringLiteral("Coast — the light is perfect…");
        auto w4 = text(weekend.id, aisha, QStringLiteral("Sunrise is 06:12. Leave at 5?"), 126);
        auto w5 = text(weekend.id, jordan, QStringLiteral("😴"), 125);
        w5.reactions = {
            { QStringLiteral("😂"), 3, true, eid() },
            { QStringLiteral("💯"), 1, false, QString() },
        };
        auto w6 = text(weekend.id, alex, QStringLiteral(
            "I can drive — there is room for four plus camera bags."), 118);
        auto w7 = text(weekend.id, aisha, QStringLiteral(
            "Perfect. I will bring the wide lens and the tripod."), 114);
        auto w8 = text(weekend.id, jordan, QStringLiteral("loop.gif"), 108);
        w8.type = TimelineEvent::Image;
        w8.body = QStringLiteral("loop.gif");
        w8.mediaMimetype = QStringLiteral("image/gif");
        w8.mediaFilename = QStringLiteral("loop.gif");
        w8.mediaMxcUrl = QStringLiteral("mxc://lightning.example/loop");
        w8.mediaWidth = 480; w8.mediaHeight = 480; w8.mediaSize = 66172;
        w8.reactions = { { QStringLiteral("🔥"), 4, true, eid() } };
        auto w9 = text(weekend.id, aisha, QStringLiteral(
            "That is exactly the energy I need at five in the morning."), 104);
        auto w10 = text(weekend.id, alex, QStringLiteral(
            "Packing list: water, snacks, rain shell. Anything else?"), 100);
        auto w11 = text(weekend.id, jordan, QStringLiteral("Bug spray. Learned that one the hard way."), 98);
        w11.edited = true;
        auto w12 = text(weekend.id, alex, QStringLiteral("The coast trail it is 🥾"), 96);
        w12.reactions = {
            { QStringLiteral("🥾"), 3, true, eid() },
            { QStringLiteral("🌊"), 2, false, QString() },
        };
        acct.timelines[weekend.id] = { w1, w2, w3, w4, w5, w6, w7,
                                       w8, w9, w10, w11, w12 };
    }
    acct.paginationRemaining[weekend.id] = 0;

    // ── Maya Chen — encrypted direct message ────────────────────────────
    RoomInfo dmMaya;
    dmMaya.id = QStringLiteral("!dm-maya:lightning.example");
    dmMaya.name = maya.name;
    dmMaya.avatarUrl = maya.avatarMxc;
    dmMaya.isDirect = true;
    dmMaya.directUserId = maya.id;
    dmMaya.directUserIds = { maya.id };
    dmMaya.encrypted = true;
    dmMaya.spaceId = friends.id;
    dmMaya.lastActivity = base.addSecs(-40 * 60);
    dmMaya.lastMessagePreview = QStringLiteral("Perfect, thank you! 🙌");
    demoMem(dmMaya.members, alex); demoMem(dmMaya.members, maya);
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
        auto d6 = text(dmMaya.id, maya, QStringLiteral(
            "Also — did you see the saved GIFs tab? Way easier to find things now."), 6);
        auto d7 = text(dmMaya.id, alex, QStringLiteral(
            "Yes! One star, one place. I stopped losing them."), 5);
        d7.reactions = { { QStringLiteral("⭐"), 1, false, QString() } };
        auto d8 = text(dmMaya.id, maya, QStringLiteral("Ship it 🚀"), 4);
        acct.timelines[dmMaya.id] = { d1, d2, d3, d4, d5, d6, d7, d8 };
    }
    acct.paginationRemaining[dmMaya.id] = 0;

    // ── Jordan Lee — a plain (unencrypted) direct message ───────────────
    RoomInfo dmJordan;
    dmJordan.id = QStringLiteral("!dm-jordan:lightning.example");
    dmJordan.name = jordan.name;
    dmJordan.avatarUrl = jordan.avatarMxc;
    dmJordan.isDirect = true;
    dmJordan.directUserId = jordan.id;
    dmJordan.directUserIds = { jordan.id };
    dmJordan.spaceId = friends.id;
    dmJordan.lastActivity = base.addSecs(-150 * 60);
    dmJordan.lastMessagePreview = QStringLiteral("See you at the studio 👋");
    demoMem(dmJordan.members, alex); demoMem(dmJordan.members, jordan);
    {
        auto j1 = text(dmJordan.id, jordan, QStringLiteral(
            "Are we still on for the studio review at 3?"), 170);
        auto j2 = text(dmJordan.id, alex, QStringLiteral("Yep — bringing the new mockups."), 165);
        auto j3 = text(dmJordan.id, jordan, QStringLiteral("See you at the studio 👋"), 150);
        acct.timelines[dmJordan.id] = { j1, j2, j3 };
    }
    acct.paginationRemaining[dmJordan.id] = 0;

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
    demoMem(dev.members, alex); demoMem(dev.members, sam);
    demoMem(dev.members, noah); demoMem(dev.members, priya); demoMem(dev.members, leo);
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
            { QStringLiteral("🚀"), 4, true, eid() },
            { QStringLiteral("🎉"), 2, false, QString() },
        };
        // Thread replies live in the ROOM timeline with threadRootId set: the
        // model filters true m.thread replies out of the main view, and
        // openThread() rebuilds the thread panel from them (root pinned first).
        auto tr = [&](const DemoPerson &p, const QString &body, int minsAgo) {
            TimelineEvent e = text(dev.id, p, body, minsAgo);
            e.threadRootId = threadRootId;
            return e;
        };
        acct.timelines[dev.id] = {
            c1, c2, c3, c4, c5, root, c7,
            tr(priya, QStringLiteral("It's low-risk and user-visible."), 62),
            tr(noah, QStringLiteral("Tests are green on both configs."), 61),
            tr(alex, QStringLiteral("I can cut 0.6.4 this week."), 60),
            tr(sam, QStringLiteral("Agreed — backport it."), 58),
        };
    }
    acct.paginationRemaining[dev.id] = 1;

    // ── Product Feedback — a poll ───────────────────────────────────────
    RoomInfo feedback;
    feedback.id = QStringLiteral("!feedback:lightning.example");
    feedback.name = QStringLiteral("Product Feedback");
    feedback.topic = QStringLiteral("What should we build next?");
    feedback.spaceId = community.id;
    feedback.lastActivity = base.addSecs(-70 * 60);
    feedback.lastMessagePreview = QStringLiteral("Poll: Which theme for the release screenshots?");
    demoMem(feedback.members, alex); demoMem(feedback.members, maya);
    demoMem(feedback.members, sam); demoMem(feedback.members, priya);
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
        acct.timelines[feedback.id] = { p1, poll };
    }
    acct.paginationRemaining[feedback.id] = 0;

    // ── Photography — a media-heavy room (landscape/portrait/square/…) ──
    RoomInfo photo;
    photo.id = QStringLiteral("!photography:lightning.example");
    photo.name = QStringLiteral("Photography");
    photo.topic = QStringLiteral("Shots from the weekend");
    photo.spaceId = studio.id;
    photo.lastActivity = base.addSecs(-3 * 60 * 60);
    photo.lastMessagePreview = QStringLiteral("Golden hour by the coast 🌅");
    demoMem(photo.members, alex); demoMem(photo.members, aisha);
    demoMem(photo.members, jordan);
    {
        auto ph = [&](const DemoPerson &p, const QString &caption,
                      const QString &key, int w, int h, int minsAgo) {
            TimelineEvent e = text(photo.id, p, caption, minsAgo);
            e.type = TimelineEvent::Image;
            e.mediaMimetype = QStringLiteral("image/jpeg");
            e.mediaFilename = key + QStringLiteral(".jpg");
            e.mediaMxcUrl = QStringLiteral("mxc://lightning.example/") + key;
            e.mediaWidth = w; e.mediaHeight = h; e.mediaSize = 320000;
            return e;
        };
        auto coast = ph(aisha, QStringLiteral("Golden hour by the coast 🌅"),
                        QStringLiteral("coast"), 1600, 1000, 200);
        coast.reactions = { { QStringLiteral("😍"), 3, false, QString() } };
        auto portrait = ph(jordan, QStringLiteral("Portrait test, natural light"),
                           QStringLiteral("portrait"), 800, 1200, 190);
        auto square = ph(aisha, QStringLiteral("Album cover crop"),
                         QStringLiteral("square"), 1000, 1000, 185);
        // A GIF, a video (poster), an audio clip and a document round out the
        // media gallery so every media row type renders.
        auto artwork = ph(aisha, QStringLiteral("New wallpaper artwork"),
                          QStringLiteral("artwork"), 1200, 1200, 178);
        artwork.mediaMimetype = QStringLiteral("image/png");
        artwork.mediaFilename = QStringLiteral("artwork.png");
        auto gif = text(photo.id, jordan, QStringLiteral("loop-preview.gif"), 172);
        gif.type = TimelineEvent::Image;
        gif.body = QStringLiteral("loop-preview.gif");
        gif.mediaMimetype = QStringLiteral("image/gif");
        gif.mediaFilename = QStringLiteral("loop-preview.gif");
        gif.mediaMxcUrl = QStringLiteral("mxc://lightning.example/loop");
        gif.mediaWidth = 480; gif.mediaHeight = 480; gif.mediaSize = 90000;
        auto video = text(photo.id, aisha, QStringLiteral("timelapse.mp4"), 166);
        video.type = TimelineEvent::Video;
        video.body = QStringLiteral("timelapse.mp4");
        video.mediaMimetype = QStringLiteral("video/mp4");
        video.mediaFilename = QStringLiteral("timelapse.mp4");
        video.mediaMxcUrl = QStringLiteral("mxc://lightning.example/timelapse");
        video.mediaWidth = 1280; video.mediaHeight = 720; video.mediaSize = 2400000;
        video.mediaDurationMs = 12000;
        auto audio = text(photo.id, jordan, QStringLiteral("field-recording.ogg"), 160);
        audio.type = TimelineEvent::Audio;
        audio.body = QStringLiteral("field-recording.ogg");
        audio.mediaMimetype = QStringLiteral("audio/ogg");
        audio.mediaFilename = QStringLiteral("field-recording.ogg");
        audio.mediaMxcUrl = QStringLiteral("mxc://lightning.example/audio");
        audio.mediaSize = 180000;
        audio.mediaDurationMs = 9000;
        audio.mediaWaveform = { 8, 22, 40, 55, 70, 62, 48, 66, 82, 74, 58, 44,
                                60, 78, 90, 72, 50, 34, 46, 64, 80, 56, 30, 18 };
        auto doc = text(photo.id, aisha, QStringLiteral("shot-list.pdf"), 154);
        doc.type = TimelineEvent::File;
        doc.body = QStringLiteral("shot-list.pdf");
        doc.mediaMimetype = QStringLiteral("application/pdf");
        doc.mediaFilename = QStringLiteral("shot-list.pdf");
        doc.mediaMxcUrl = QStringLiteral("mxc://lightning.example/shotlist");
        doc.mediaSize = 30240;
        acct.timelines[photo.id] = { coast, portrait, square, artwork,
                                     gif, video, audio, doc };
    }
    acct.paginationRemaining[photo.id] = 0;

    // ── Release Announcements — announcement room ───────────────────────
    RoomInfo announce;
    announce.id = QStringLiteral("!announce:lightning.example");
    announce.name = QStringLiteral("Release Announcements");
    announce.topic = QStringLiteral("What's new in Lightning");
    announce.spaceId = community.id;
    announce.lastActivity = base.addSecs(-6 * 60 * 60);
    announce.lastMessagePreview = QStringLiteral("Lightning 0.6.3 is out 🎉");
    demoMem(announce.members, alex); demoMem(announce.members, priya);
    {
        auto a1 = text(announce.id, priya, QStringLiteral(
            "Lightning 0.6.3 is out 🎉 Smoother scrolling, reliable video, and a refreshed room list."), 360);
        a1.reactions = { { QStringLiteral("🎉"), 8, true, eid() },
                         { QStringLiteral("⚡"), 5, false, QString() } };
        acct.timelines[announce.id] = { a1 };
    }
    acct.paginationRemaining[announce.id] = 0;

    // ── Music Discovery — favourite room ────────────────────────────────
    RoomInfo music;
    music.id = QStringLiteral("!music:lightning.example");
    music.name = QStringLiteral("Music Discovery");
    music.topic = QStringLiteral("Share what you're listening to");
    music.spaceId = friends.id;
    music.lastActivity = base.addSecs(-5 * 60 * 60);
    music.lastMessagePreview = QStringLiteral("This album is on repeat all week.");
    demoMem(music.members, alex); demoMem(music.members, jordan);
    demoMem(music.members, noah);
    acct.timelines[music.id] = {
        text(music.id, jordan, QStringLiteral("This album is on repeat all week."), 300),
    };
    acct.paginationRemaining[music.id] = 0;

    // ── An invite ───────────────────────────────────────────────────────
    RoomInfo invite;
    invite.id = QStringLiteral("!invite-founders:lightning.example");
    invite.name = QStringLiteral("Founders Lounge");
    invite.topic = QStringLiteral("Private space for the founding team");
    invite.membership = RoomInfo::Invited;
    invite.inviterUserId = sam.id;
    invite.inviterDisplayName = sam.name;
    invite.lastActivity = base.addSecs(-9 * 60 * 60);

    // Space membership.
    friends.childRoomIds   = { dmMaya.id, dmJordan.id, music.id, weekend.id };
    studio.childRoomIds    = { design.id, photo.id };
    community.childRoomIds  = { dev.id, feedback.id, announce.id };

    // Room-list order: Spaces first, then rooms by recency, DMs, invite.
    acct.rooms = { friends, studio, community,
                   design, dev, weekend, dmMaya, dmJordan, feedback,
                   photo, announce, music, invite };
    return acct;
}

MockMatrixClient::DemoAccount MockMatrixClient::buildDemoAccountTaylor()
{
    const QDateTime base(QDate(2026, 7, 23), QTime(10, 24), QTimeZone::UTC);
    const QString hs = QStringLiteral("workplace.example");
    DemoAccount acct;
    acct.userId = QStringLiteral("@taylor:workplace.example");
    acct.homeserver = hs;
    acct.displayName = QStringLiteral("Taylor Reed");
    acct.avatarMxc = demoAvatar(QStringLiteral("taylor"));
    acct.defaultRoomId = QStringLiteral("!aurora:workplace.example");

    int n = 0;
    auto eid = [&]() {
        return QStringLiteral("$demo-taylor-%1:%2").arg(++n).arg(hs);
    };

    const DemoPerson taylor{ acct.userId,                                  acct.displayName,                acct.avatarMxc };
    const DemoPerson sam   { QStringLiteral("@sam:workplace.example"),     QStringLiteral("Sam Rivera"),    demoAvatar(QStringLiteral("sam")) };
    const DemoPerson priya { QStringLiteral("@priya:workplace.example"),   QStringLiteral("Priya Shah"),    demoAvatar(QStringLiteral("priya")) };
    const DemoPerson noah  { QStringLiteral("@noah:workplace.example"),    QStringLiteral("Noah Williams"), demoAvatar(QStringLiteral("noah")) };
    const DemoPerson maya  { QStringLiteral("@maya:workplace.example"),    QStringLiteral("Maya Chen"),     demoAvatar(QStringLiteral("maya")) };

    auto text = [&](const QString &room, const DemoPerson &p,
                    const QString &body, int minsAgo) {
        TimelineEvent e;
        e.eventId = eid();
        e.roomId = room;
        e.sender = p.id;
        e.senderDisplayName = p.name;
        e.senderAvatarUrl = p.avatarMxc;
        e.body = body;
        e.timestamp = base.addSecs(-minsAgo * 60);
        e.type = TimelineEvent::TextMessage;
        return e;
    };

    // ── Spaces ──────────────────────────────────────────────────────────
    RoomInfo product;
    product.id = QStringLiteral("!space-product:workplace.example");
    product.name = QStringLiteral("Product");
    product.topic = QStringLiteral("Roadmap, design and delivery");
    product.isSpace = true;
    product.lastActivity = base.addSecs(-5 * 60);

    RoomInfo engineering;
    engineering.id = QStringLiteral("!space-engineering:workplace.example");
    engineering.name = QStringLiteral("Engineering");
    engineering.topic = QStringLiteral("Services, infra and releases");
    engineering.isSpace = true;
    engineering.lastActivity = base.addSecs(-12 * 60);

    RoomInfo companySpace;
    companySpace.id = QStringLiteral("!space-company:workplace.example");
    companySpace.name = QStringLiteral("Company");
    companySpace.topic = QStringLiteral("Everyone at Aurora");
    companySpace.isSpace = true;
    companySpace.lastActivity = base.addSecs(-45 * 60);

    // ── Project Aurora — the main professional conversation ─────────────
    RoomInfo aurora;
    aurora.id = QStringLiteral("!aurora:workplace.example");
    aurora.name = QStringLiteral("Project Aurora");
    aurora.topic = QStringLiteral("Flagship Q3 launch");
    aurora.spaceId = product.id;
    aurora.lastActivity = base.addSecs(-5 * 60);
    aurora.lastMessagePreview = QStringLiteral("Design sign-off is in — shipping Thursday.");
    aurora.unreadCount = 2;
    aurora.highlightCount = 1;
    aurora.hasUnreadMessages = true;
    demoMem(aurora.members, taylor); demoMem(aurora.members, sam);
    demoMem(aurora.members, priya); demoMem(aurora.members, noah);
    aurora.typingUserIds << sam.id;
    {
        auto a1 = text(aurora.id, priya, QStringLiteral(
            "Status for the Aurora launch: design is sign-off pending, backend is green."), 46);
        auto a2 = text(aurora.id, noah, QStringLiteral(
            "QA finished the regression pass — no blockers, two cosmetic tickets filed."), 40);
        auto a3 = text(aurora.id, taylor, QStringLiteral(
            "Great. @sam can you confirm the rollout window with infra?"), 22);
        a3.mentionsMe = false;
        auto a4 = text(aurora.id, sam, QStringLiteral(
            "Confirmed — Thursday 09:00, staged behind the feature flag."), 12);
        a4.replyToEventId = a3.eventId;
        a4.replyToSender = taylor.name;
        a4.replyToPreview = QStringLiteral("can you confirm the rollout window…");
        a4.reactions = { { QStringLiteral("✅"), 3, true, eid() } };
        auto a5 = text(aurora.id, priya, QStringLiteral(
            "Design sign-off is in — shipping Thursday."), 5);
        acct.timelines[aurora.id] = { a1, a2, a3, a4, a5 };
    }
    acct.paginationRemaining[aurora.id] = 2;

    // ── Product Design — technical/design room with an image ────────────
    RoomInfo productDesign;
    productDesign.id = QStringLiteral("!product-design:workplace.example");
    productDesign.name = QStringLiteral("Product Design");
    productDesign.topic = QStringLiteral("Design system and specs");
    productDesign.spaceId = product.id;
    productDesign.lastActivity = base.addSecs(-80 * 60);
    productDesign.lastMessagePreview = QStringLiteral("Updated the spacing tokens.");
    demoMem(productDesign.members, taylor); demoMem(productDesign.members, maya);
    demoMem(productDesign.members, priya);
    {
        auto p1 = text(productDesign.id, maya, QStringLiteral(
            "Pushed the refreshed component sheet:"), 84);
        auto p2 = text(productDesign.id, maya, QStringLiteral("components.png"), 82);
        p2.type = TimelineEvent::Image;
        p2.body = QStringLiteral("components.png");
        p2.mediaMimetype = QStringLiteral("image/png");
        p2.mediaFilename = QStringLiteral("components.png");
        p2.mediaMxcUrl = QStringLiteral("mxc://lightning.example/square");
        p2.mediaWidth = 1000; p2.mediaHeight = 1000; p2.mediaSize = 210000;
        auto p3 = text(productDesign.id, taylor, QStringLiteral("Updated the spacing tokens."), 80);
        acct.timelines[productDesign.id] = { p1, p2, p3 };
    }
    acct.paginationRemaining[productDesign.id] = 0;

    // ── Engineering — technical room with a code block ──────────────────
    RoomInfo eng;
    eng.id = QStringLiteral("!engineering:workplace.example");
    eng.name = QStringLiteral("Engineering");
    eng.topic = QStringLiteral("Backend and platform");
    eng.spaceId = engineering.id;
    eng.lastActivity = base.addSecs(-35 * 60);
    eng.lastMessagePreview = QStringLiteral("Deploy is green on staging.");
    eng.unreadCount = 4;
    eng.hasUnreadMessages = true;
    demoMem(eng.members, taylor); demoMem(eng.members, sam); demoMem(eng.members, noah);
    {
        auto e1 = text(eng.id, noah, QStringLiteral(
            "Rolling the migration behind a flag. Config:"), 44);
        auto e2 = text(eng.id, noah, QStringLiteral(
            "```yaml\nrollout:\n  strategy: canary\n  percent: 10\n```"), 43);
        e2.formattedBody = QStringLiteral(
            "<pre><code>rollout:\n  strategy: canary\n  percent: 10</code></pre>");
        auto e3 = text(eng.id, sam, QStringLiteral(
            "Use `canary` first, then widen once error rate holds."), 36);
        e3.formattedBody = QStringLiteral(
            "Use <code>canary</code> first, then widen once error rate holds.");
        auto e4 = text(eng.id, sam, QStringLiteral("Deploy is green on staging."), 30);
        e4.reactions = { { QStringLiteral("🟢"), 2, false, QString() } };
        acct.timelines[eng.id] = { e1, e2, e3, e4 };
    }
    acct.paginationRemaining[eng.id] = 1;

    // ── Release Planning — favourite-style room ─────────────────────────
    RoomInfo releasePlanning;
    releasePlanning.id = QStringLiteral("!release-planning:workplace.example");
    releasePlanning.name = QStringLiteral("Release Planning");
    releasePlanning.topic = QStringLiteral("Cut dates and checklists");
    releasePlanning.spaceId = engineering.id;
    releasePlanning.lastActivity = base.addSecs(-2 * 60 * 60);
    releasePlanning.lastMessagePreview = QStringLiteral("Cut is Thursday, freeze Wednesday EOD.");
    demoMem(releasePlanning.members, taylor); demoMem(releasePlanning.members, priya);
    acct.timelines[releasePlanning.id] = {
        text(releasePlanning.id, priya,
             QStringLiteral("Cut is Thursday, freeze Wednesday EOD."), 130),
    };
    acct.paginationRemaining[releasePlanning.id] = 0;

    // ── Company Announcements — announcement room ───────────────────────
    RoomInfo companyAnnounce;
    companyAnnounce.id = QStringLiteral("!company-announce:workplace.example");
    companyAnnounce.name = QStringLiteral("Company Announcements");
    companyAnnounce.topic = QStringLiteral("Company-wide news");
    companyAnnounce.spaceId = companySpace.id;
    companyAnnounce.lastActivity = base.addSecs(-4 * 60 * 60);
    companyAnnounce.lastMessagePreview = QStringLiteral("All-hands moved to Friday 10:00.");
    demoMem(companyAnnounce.members, taylor); demoMem(companyAnnounce.members, priya);
    {
        auto a1 = text(companyAnnounce.id, priya, QStringLiteral(
            "All-hands moved to Friday 10:00. Agenda in the doc."), 240);
        a1.reactions = { { QStringLiteral("👍"), 12, false, QString() } };
        acct.timelines[companyAnnounce.id] = { a1 };
    }
    acct.paginationRemaining[companyAnnounce.id] = 0;

    // ── Team Lounge — muted, quiet room ─────────────────────────────────
    RoomInfo teamLounge;
    teamLounge.id = QStringLiteral("!team-lounge:workplace.example");
    teamLounge.name = QStringLiteral("Team Lounge");
    teamLounge.topic = QStringLiteral("Off-topic and coffee");
    teamLounge.spaceId = companySpace.id;
    teamLounge.lastActivity = base.addSecs(-8 * 60 * 60);
    teamLounge.lastMessagePreview = QStringLiteral("New espresso machine works 🎉");
    demoMem(teamLounge.members, taylor); demoMem(teamLounge.members, noah);
    acct.timelines[teamLounge.id] = {
        text(teamLounge.id, noah, QStringLiteral("New espresso machine works 🎉"), 480),
    };
    acct.paginationRemaining[teamLounge.id] = 0;

    // ── Sam Rivera — an encrypted 1:1 DM (with a mention) ───────────────
    RoomInfo dmSam;
    dmSam.id = QStringLiteral("!dm-sam:workplace.example");
    dmSam.name = sam.name;
    dmSam.avatarUrl = sam.avatarMxc;
    dmSam.isDirect = true;
    dmSam.directUserId = sam.id;
    dmSam.directUserIds = { sam.id };
    dmSam.encrypted = true;
    dmSam.spaceId = engineering.id;
    dmSam.lastActivity = base.addSecs(-25 * 60);
    dmSam.lastMessagePreview = QStringLiteral("Thanks — reviewing now.");
    dmSam.unreadCount = 1;
    dmSam.highlightCount = 1;
    dmSam.hasUnreadMessages = true;
    demoMem(dmSam.members, taylor); demoMem(dmSam.members, sam);
    {
        auto s1 = text(dmSam.id, sam, QStringLiteral(
            "Can you review the infra PR before the freeze?"), 32);
        auto s2 = text(dmSam.id, taylor, QStringLiteral("On it now."), 28);
        auto s3 = text(dmSam.id, sam, QStringLiteral(
            "@taylor thanks — reviewing now."), 25);
        s3.mentionsMe = true;
        acct.timelines[dmSam.id] = { s1, s2, s3 };
    }
    acct.paginationRemaining[dmSam.id] = 0;

    // ── Incident Review — technical incident room ───────────────────────
    RoomInfo incident;
    incident.id = QStringLiteral("!incident-review:workplace.example");
    incident.name = QStringLiteral("Incident Review");
    incident.topic = QStringLiteral("Post-incident notes");
    incident.spaceId = engineering.id;
    incident.lastActivity = base.addSecs(-10 * 60 * 60);
    incident.lastMessagePreview = QStringLiteral("Root cause: expired cert. Action items filed.");
    demoMem(incident.members, taylor); demoMem(incident.members, sam);
    demoMem(incident.members, noah);
    acct.timelines[incident.id] = {
        text(incident.id, noah,
             QStringLiteral("Root cause: expired cert. Action items filed."), 600),
    };
    acct.paginationRemaining[incident.id] = 0;

    // ── An invite ───────────────────────────────────────────────────────
    RoomInfo invite;
    invite.id = QStringLiteral("!invite-leadership:workplace.example");
    invite.name = QStringLiteral("Leadership Sync");
    invite.topic = QStringLiteral("Weekly leadership review");
    invite.membership = RoomInfo::Invited;
    invite.inviterUserId = priya.id;
    invite.inviterDisplayName = priya.name;
    invite.lastActivity = base.addSecs(-11 * 60 * 60);

    product.childRoomIds     = { aurora.id, productDesign.id };
    engineering.childRoomIds = { eng.id, releasePlanning.id, incident.id, dmSam.id };
    companySpace.childRoomIds = { companyAnnounce.id, teamLounge.id };

    acct.rooms = { product, engineering, companySpace,
                   aurora, eng, dmSam, productDesign, releasePlanning,
                   companyAnnounce, teamLounge, incident, invite };
    return acct;
}

MockMatrixClient::DemoAccount MockMatrixClient::buildDemoAccountNova()
{
    const QDateTime base(QDate(2026, 7, 23), QTime(10, 24), QTimeZone::UTC);
    const QString hs = QStringLiteral("community.example");
    DemoAccount acct;
    acct.userId = QStringLiteral("@nova:community.example");
    acct.homeserver = hs;
    acct.displayName = QStringLiteral("Nova");
    acct.avatarMxc = demoAvatar(QStringLiteral("nova"));
    acct.defaultRoomId = QStringLiteral("!general:community.example");

    int n = 0;
    auto eid = [&]() {
        return QStringLiteral("$demo-nova-%1:%2").arg(++n).arg(hs);
    };

    const DemoPerson nova  { acct.userId,                                 acct.displayName,                acct.avatarMxc };
    const DemoPerson priya { QStringLiteral("@priya:community.example"),  QStringLiteral("Priya Shah"),    demoAvatar(QStringLiteral("priya")) };
    const DemoPerson leo   { QStringLiteral("@leo:community.example"),    QStringLiteral("Leo Novak"),     demoAvatar(QStringLiteral("leo")) };
    const DemoPerson maya  { QStringLiteral("@maya:community.example"),   QStringLiteral("Maya Chen"),     demoAvatar(QStringLiteral("maya")) };
    const DemoPerson jordan{ QStringLiteral("@jordan:community.example"), QStringLiteral("Jordan Lee"),    demoAvatar(QStringLiteral("jordan")) };

    auto text = [&](const QString &room, const DemoPerson &p,
                    const QString &body, int minsAgo) {
        TimelineEvent e;
        e.eventId = eid();
        e.roomId = room;
        e.sender = p.id;
        e.senderDisplayName = p.name;
        e.senderAvatarUrl = p.avatarMxc;
        e.body = body;
        e.timestamp = base.addSecs(-minsAgo * 60);
        e.type = TimelineEvent::TextMessage;
        return e;
    };

    // ── Spaces ──────────────────────────────────────────────────────────
    RoomInfo openSource;
    openSource.id = QStringLiteral("!space-oss:community.example");
    openSource.name = QStringLiteral("Open Source");
    openSource.topic = QStringLiteral("The project and its contributors");
    openSource.isSpace = true;
    openSource.lastActivity = base.addSecs(-6 * 60);

    RoomInfo communitySpace;
    communitySpace.id = QStringLiteral("!space-community:community.example");
    communitySpace.name = QStringLiteral("Community");
    communitySpace.topic = QStringLiteral("Everyone welcome");
    communitySpace.isSpace = true;
    communitySpace.lastActivity = base.addSecs(-15 * 60);

    RoomInfo supportSpace;
    supportSpace.id = QStringLiteral("!space-support:community.example");
    supportSpace.name = QStringLiteral("Support");
    supportSpace.topic = QStringLiteral("Get help and give help");
    supportSpace.isSpace = true;
    supportSpace.lastActivity = base.addSecs(-20 * 60);

    // ── General — the main public room ──────────────────────────────────
    RoomInfo general;
    general.id = QStringLiteral("!general:community.example");
    general.name = QStringLiteral("General");
    general.topic = QStringLiteral("Public lobby — say hello 👋");
    general.spaceId = communitySpace.id;
    general.lastActivity = base.addSecs(-6 * 60);
    general.lastMessagePreview = QStringLiteral("Welcome to all the new folks!");
    general.unreadCount = 7;
    general.highlightCount = 1;
    general.hasUnreadMessages = true;
    demoMem(general.members, nova); demoMem(general.members, priya);
    demoMem(general.members, leo); demoMem(general.members, maya);
    demoMem(general.members, jordan);
    general.typingUserIds << leo.id;
    {
        auto g1 = text(general.id, priya, QStringLiteral(
            "Welcome to all the new folks! Introduce yourself here."), 30);
        auto g2 = text(general.id, maya, QStringLiteral(
            "Hi everyone — designer, happy to help with UI reviews."), 24);
        g2.reactions = { { QStringLiteral("👋"), 6, true, eid() } };
        auto g3 = text(general.id, jordan, QStringLiteral(
            "Long-time lurker, first-time contributor 😄"), 18);
        auto g4 = text(general.id, nova, QStringLiteral(
            "Welcome! The Development room is a good place to start."), 10);
        acct.timelines[general.id] = { g1, g2, g3, g4 };
    }
    acct.paginationRemaining[general.id] = 2;

    // ── Development — thread-heavy technical room ───────────────────────
    RoomInfo devel;
    devel.id = QStringLiteral("!development:community.example");
    devel.name = QStringLiteral("Development");
    devel.topic = QStringLiteral("Contributing and internals");
    devel.spaceId = openSource.id;
    devel.lastActivity = base.addSecs(-22 * 60);
    devel.lastMessagePreview = QStringLiteral("Merged — thanks for the review!");
    devel.unreadCount = 3;
    devel.hasUnreadMessages = true;
    demoMem(devel.members, nova); demoMem(devel.members, leo);
    demoMem(devel.members, priya); demoMem(devel.members, jordan);
    QString develRoot;
    {
        auto d1 = text(devel.id, leo, QStringLiteral(
            "Opened a PR for the plugin API. Feedback welcome."), 120);
        auto d2 = text(devel.id, leo, QStringLiteral(
            "```rust\npub fn register(plugin: Plugin) -> Result<()> {\n    registry().add(plugin)\n}\n```"), 118);
        d2.formattedBody = QStringLiteral(
            "<pre><code>pub fn register(plugin: Plugin) -&gt; Result&lt;()&gt; {\n"
            "    registry().add(plugin)\n}</code></pre>");
        auto root = text(devel.id, priya, QStringLiteral(
            "Should the plugin registry be global or per-session?"), 100);
        develRoot = root.eventId;
        root.isThreadRoot = true;
        root.threadReplyCount = 5;
        root.threadLatestPreview = QStringLiteral("Merged — thanks for the review!");
        root.threadLatestKind = QStringLiteral("text");
        root.threadLatestSender = leo.id;
        root.threadLatestSenderDisplayName = leo.name;
        root.threadLatestTimestamp = base.addSecs(-22 * 60);
        auto tr = [&](const DemoPerson &p, const QString &body, int minsAgo) {
            TimelineEvent e = text(devel.id, p, body, minsAgo);
            e.threadRootId = develRoot;
            return e;
        };
        acct.timelines[devel.id] = {
            d1, d2, root,
            tr(nova, QStringLiteral("Per-session is safer for isolation."), 92),
            tr(jordan, QStringLiteral("Agreed, global state bit us last time."), 88),
            tr(leo, QStringLiteral("Per-session it is. Updating the PR."), 80),
            tr(priya, QStringLiteral("LGTM once tests pass."), 40),
            tr(leo, QStringLiteral("Merged — thanks for the review!"), 22),
        };
    }
    acct.paginationRemaining[devel.id] = 1;

    // ── Feature Requests — a poll ───────────────────────────────────────
    RoomInfo features;
    features.id = QStringLiteral("!feature-requests:community.example");
    features.name = QStringLiteral("Feature Requests");
    features.topic = QStringLiteral("Vote on what's next");
    features.spaceId = communitySpace.id;
    features.lastActivity = base.addSecs(-55 * 60);
    features.lastMessagePreview = QStringLiteral("Poll: what should we prioritise next?");
    demoMem(features.members, nova); demoMem(features.members, priya);
    demoMem(features.members, jordan);
    {
        auto f1 = text(features.id, priya, QStringLiteral(
            "Community poll — what should we prioritise next quarter?"), 58);
        TimelineEvent poll = text(features.id, priya, QString(), 57);
        poll.type = TimelineEvent::Poll;
        poll.pollQuestion = QStringLiteral("What should we prioritise next?");
        poll.pollKind = QStringLiteral("disclosed");
        poll.pollMaxSelections = 1;
        poll.pollTotalVoters = 24;
        poll.pollAnswers = {
            { QStringLiteral("b1"), QStringLiteral("Plugins"),       11, true },
            { QStringLiteral("b2"), QStringLiteral("Themes"),         7, false },
            { QStringLiteral("b3"), QStringLiteral("Mobile"),         4, false },
            { QStringLiteral("b4"), QStringLiteral("Localization"),   2, false },
        };
        acct.timelines[features.id] = { f1, poll };
    }
    acct.paginationRemaining[features.id] = 0;

    // ── Support — a support question room ───────────────────────────────
    RoomInfo support;
    support.id = QStringLiteral("!support:community.example");
    support.name = QStringLiteral("Support");
    support.topic = QStringLiteral("Ask for help here");
    support.spaceId = supportSpace.id;
    support.lastActivity = base.addSecs(-70 * 60);
    support.lastMessagePreview = QStringLiteral("That fixed it — thank you! 🙏");
    support.unreadCount = 2;
    support.hasUnreadMessages = true;
    demoMem(support.members, nova); demoMem(support.members, jordan);
    demoMem(support.members, priya);
    {
        auto s1 = text(support.id, jordan, QStringLiteral(
            "Build fails on Wayland with a missing plugin. Any ideas?"), 78);
        auto s2 = text(support.id, nova, QStringLiteral(
            "Install the platform plugin and set QT_QPA_PLATFORM=wayland."), 70);
        s2.replyToEventId = s1.eventId;
        s2.replyToSender = jordan.name;
        s2.replyToPreview = QStringLiteral("Build fails on Wayland…");
        auto s3 = text(support.id, jordan, QStringLiteral("That fixed it — thank you! 🙏"), 66);
        s3.reactions = { { QStringLiteral("🙏"), 2, false, QString() } };
        acct.timelines[support.id] = { s1, s2, s3 };
    }
    acct.paginationRemaining[support.id] = 0;

    // ── Showcase — a media showcase room ────────────────────────────────
    RoomInfo showcase;
    showcase.id = QStringLiteral("!showcase:community.example");
    showcase.name = QStringLiteral("Showcase");
    showcase.topic = QStringLiteral("Show what you built");
    showcase.spaceId = communitySpace.id;
    showcase.lastActivity = base.addSecs(-2 * 60 * 60);
    showcase.lastMessagePreview = QStringLiteral("My custom theme 🎨");
    demoMem(showcase.members, nova); demoMem(showcase.members, maya);
    demoMem(showcase.members, leo);
    {
        auto sh1 = text(showcase.id, maya, QStringLiteral("My custom theme 🎨"), 130);
        auto sh2 = text(showcase.id, maya, QStringLiteral("theme.png"), 128);
        sh2.type = TimelineEvent::Image;
        sh2.body = QStringLiteral("theme.png");
        sh2.mediaMimetype = QStringLiteral("image/png");
        sh2.mediaFilename = QStringLiteral("theme.png");
        sh2.mediaMxcUrl = QStringLiteral("mxc://lightning.example/coast");
        sh2.mediaWidth = 1600; sh2.mediaHeight = 1000; sh2.mediaSize = 300000;
        sh2.reactions = { { QStringLiteral("🎨"), 5, false, QString() } };
        auto sh3 = text(showcase.id, leo, QStringLiteral("looks-great.gif"), 124);
        sh3.type = TimelineEvent::Image;
        sh3.body = QStringLiteral("looks-great.gif");
        sh3.mediaMimetype = QStringLiteral("image/gif");
        sh3.mediaFilename = QStringLiteral("looks-great.gif");
        sh3.mediaMxcUrl = QStringLiteral("mxc://lightning.example/loop");
        sh3.mediaWidth = 480; sh3.mediaHeight = 480; sh3.mediaSize = 90000;
        acct.timelines[showcase.id] = { sh1, sh2, sh3 };
    }
    acct.paginationRemaining[showcase.id] = 0;

    // ── Off Topic — quiet casual room ───────────────────────────────────
    RoomInfo offTopic;
    offTopic.id = QStringLiteral("!off-topic:community.example");
    offTopic.name = QStringLiteral("Off Topic");
    offTopic.topic = QStringLiteral("Anything goes");
    offTopic.spaceId = communitySpace.id;
    offTopic.lastActivity = base.addSecs(-5 * 60 * 60);
    offTopic.lastMessagePreview = QStringLiteral("Coffee recommendations? ☕");
    demoMem(offTopic.members, nova); demoMem(offTopic.members, jordan);
    acct.timelines[offTopic.id] = {
        text(offTopic.id, jordan, QStringLiteral("Coffee recommendations? ☕"), 300),
    };
    acct.paginationRemaining[offTopic.id] = 0;

    // ── Priya Shah — a direct message ───────────────────────────────────
    RoomInfo dmPriya;
    dmPriya.id = QStringLiteral("!dm-priya:community.example");
    dmPriya.name = priya.name;
    dmPriya.avatarUrl = priya.avatarMxc;
    dmPriya.isDirect = true;
    dmPriya.directUserId = priya.id;
    dmPriya.directUserIds = { priya.id };
    dmPriya.spaceId = communitySpace.id;
    dmPriya.lastActivity = base.addSecs(-48 * 60);
    dmPriya.lastMessagePreview = QStringLiteral("Sounds good — I'll draft it.");
    demoMem(dmPriya.members, nova); demoMem(dmPriya.members, priya);
    {
        auto p1 = text(dmPriya.id, priya, QStringLiteral(
            "Want to co-write the release blog post?"), 60);
        auto p2 = text(dmPriya.id, nova, QStringLiteral("Sounds good — I'll draft it."), 48);
        acct.timelines[dmPriya.id] = { p1, p2 };
    }
    acct.paginationRemaining[dmPriya.id] = 0;

    // ── Maintainers — private maintainer room ───────────────────────────
    RoomInfo maintainers;
    maintainers.id = QStringLiteral("!maintainers:community.example");
    maintainers.name = QStringLiteral("Maintainers");
    maintainers.topic = QStringLiteral("Core team only");
    maintainers.spaceId = openSource.id;
    maintainers.encrypted = true;
    maintainers.lastActivity = base.addSecs(-3 * 60 * 60);
    maintainers.lastMessagePreview = QStringLiteral("Let's tag the release Friday.");
    demoMem(maintainers.members, nova); demoMem(maintainers.members, priya);
    demoMem(maintainers.members, leo);
    acct.timelines[maintainers.id] = {
        text(maintainers.id, priya, QStringLiteral("Let's tag the release Friday."), 190),
    };
    acct.paginationRemaining[maintainers.id] = 0;

    // ── An invite ───────────────────────────────────────────────────────
    RoomInfo invite;
    invite.id = QStringLiteral("!invite-translators:community.example");
    invite.name = QStringLiteral("Translators");
    invite.topic = QStringLiteral("Localization working group");
    invite.membership = RoomInfo::Invited;
    invite.inviterUserId = maya.id;
    invite.inviterDisplayName = maya.name;
    invite.lastActivity = base.addSecs(-12 * 60 * 60);

    openSource.childRoomIds     = { devel.id, maintainers.id };
    communitySpace.childRoomIds = { general.id, features.id, showcase.id,
                                    offTopic.id, dmPriya.id };
    supportSpace.childRoomIds   = { support.id };

    acct.rooms = { openSource, communitySpace, supportSpace,
                   general, devel, features, support, showcase, offTopic,
                   dmPriya, maintainers, invite };
    return acct;
}

QString MockMatrixClient::demoDefaultRoom(const QString &userId) const
{
    const auto it = m_demoAccounts.constFind(userId);
    return it != m_demoAccounts.constEnd() ? it->defaultRoomId : QString{};
}

void MockMatrixClient::snapshotWorkingSetToActiveDemoAccount()
{
    if (m_activeDemoUser.isEmpty())
        return;
    const auto it = m_demoAccounts.find(m_activeDemoUser);
    if (it == m_demoAccounts.end())
        return;
    it->rooms = m_rooms;
    it->timelines = m_timelines;
    it->paginationRemaining = m_paginationRemaining;
}

void MockMatrixClient::loadDemoAccountIntoWorkingSet(const DemoAccount &acct)
{
    m_rooms = acct.rooms;
    m_timelines = acct.timelines;
    m_paginationRemaining = acct.paginationRemaining;
    // Anything that pointed into the previous account's timelines is invalid.
    m_openThreadTimelineId.clear();
    m_openThreadListRoom.clear();
    m_paginating.clear();
    m_paginationFailed.clear();
    m_transientPaginationFailures.clear();
}

void MockMatrixClient::activateDemoAccount(const QString &userId)
{
    if (!m_screenshotDemoMode)
        return;
    const auto it = m_demoAccounts.constFind(userId);
    if (it == m_demoAccounts.constEnd())
        return;
    if (m_activeDemoUser == userId)
        return;
    snapshotWorkingSetToActiveDemoAccount();  // preserve local mutations
    loadDemoAccountIntoWorkingSet(it.value());
    m_activeDemoUser = userId;
}

void MockMatrixClient::resetDemoData()
{
    if (!m_screenshotDemoMode)
        return;
    const QString active = m_activeDemoUser;
    seedScreenshotDemoData();          // rebuilds all three, activates Alex
    m_activeDemoUser.clear();
    activateDemoAccount(active.isEmpty()
                            ? QStringLiteral("@alex:lightning.example")
                            : active);
    Q_EMIT roomsChanged();
    for (const auto &r : m_rooms)
        Q_EMIT timelineReset(r.id);
}

void MockMatrixClient::resetDemoAccount(const QString &userId)
{
    if (!m_screenshotDemoMode || !m_demoAccounts.contains(userId))
        return;
    DemoAccount fresh;
    if (userId == QStringLiteral("@alex:lightning.example"))
        fresh = buildDemoAccountAlex();
    else if (userId == QStringLiteral("@taylor:workplace.example"))
        fresh = buildDemoAccountTaylor();
    else if (userId == QStringLiteral("@nova:community.example"))
        fresh = buildDemoAccountNova();
    else
        return;
    finalizeDemoMedia(fresh);
    fresh.paginationRemaining.clear();
    m_demoAccounts[userId] = fresh;
    if (m_activeDemoUser == userId) {
        loadDemoAccountIntoWorkingSet(fresh);
        Q_EMIT roomsChanged();
        for (const auto &r : m_rooms)
            Q_EMIT timelineReset(r.id);
    }
}

QString MockMatrixClient::demoThreadRoot(const QString &roomId) const
{
    const auto it = m_timelines.constFind(roomId);
    if (it == m_timelines.constEnd())
        return {};
    for (const TimelineEvent &e : it.value())
        if (e.isThreadRoot)
            return e.eventId;
    return {};
}

void MockMatrixClient::setDemoTypingSuppressed(bool suppressed)
{
    if (m_demoTypingSuppressed == suppressed)
        return;
    m_demoTypingSuppressed = suppressed;
    for (const auto &r : m_rooms)
        if (!r.typingUserIds.isEmpty())
            Q_EMIT typingChanged(r.id);
}

void MockMatrixClient::setDemoUnreadHidden(bool hidden)
{
    if (m_demoHideUnread == hidden)
        return;
    m_demoHideUnread = hidden;
    Q_EMIT roomsChanged();
}

// ── Development-only local interactions ──────────────────────────────────

void MockMatrixClient::sendPollResponse(const QString &roomId,
                                        const QString &threadRootId,
                                        const QString &pollStartEventId,
                                        const QStringList &answerIds)
{
    Q_UNUSED(threadRootId);
    if (!m_screenshotDemoMode)
        return;
    auto it = m_timelines.find(roomId);
    if (it == m_timelines.end())
        return;
    for (int i = 0; i < it->size(); ++i) {
        TimelineEvent &e = (*it)[i];
        if (e.eventId != pollStartEventId || e.type != TimelineEvent::Poll)
            continue;
        // Move the local vote: decrement the previously-selected answer(s),
        // increment the newly-selected. An empty answerIds retracts the vote.
        for (PollAnswer &a : e.pollAnswers) {
            const bool nowSelected = answerIds.contains(a.id);
            if (nowSelected && !a.byMe) { a.count += 1; a.byMe = true; }
            else if (!nowSelected && a.byMe) { a.count = qMax(0, a.count - 1);
                                               a.byMe = false; }
        }
        Q_EMIT eventChangedAt(roomId, i, e);
        return;
    }
}

void MockMatrixClient::acceptInvite(const QString &roomId)
{
    if (!m_screenshotDemoMode)
        return;
    for (RoomInfo &r : m_rooms) {
        if (r.id != roomId)
            continue;
        r.membership = RoomInfo::Joined;
        r.invitePending = false;
        if (r.lastMessagePreview.isEmpty())
            r.lastMessagePreview = tr("You joined the room.");
        if (!m_timelines.contains(roomId))
            m_timelines[roomId] = {};
        Q_EMIT roomUpdated(roomId);
        Q_EMIT roomsChanged();
        Q_EMIT timelineReset(roomId);
        return;
    }
}

void MockMatrixClient::rejectInvite(const QString &roomId)
{
    if (!m_screenshotDemoMode)
        return;
    for (int i = 0; i < m_rooms.size(); ++i) {
        if (m_rooms[i].id != roomId)
            continue;
        m_rooms.removeAt(i);
        m_timelines.remove(roomId);
        Q_EMIT roomsChanged();
        return;
    }
}

void MockMatrixClient::setRoomMarkedUnread(const QString &roomId, bool unread)
{
    if (!m_screenshotDemoMode)
        return;
    for (RoomInfo &r : m_rooms) {
        if (r.id != roomId)
            continue;
        r.markedUnread = unread;
        r.hasUnreadMessages = unread || r.unreadCount > 0;
        Q_EMIT roomUpdated(roomId);
        return;
    }
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
    // Demo mode must never emit the mock.local domain (the safety test rejects
    // it); the deterministic seeded rows use their own $demo-<slug>-N ids, so
    // this only stamps runtime demo sends.
    if (m_screenshotDemoMode)
        return QStringLiteral("$demo-live-%1:lightning.example").arg(++m_eventCounter);
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

void MockMatrixClient::setRoomMemberForTest(const QString &roomId,
                                            const MemberInfo &member)
{
    // Mirror of the Rust backend's room_members merge: never clobber known
    // data with empty fields, then announce membersChanged so every
    // member-derived surface (names, receipt chips) re-resolves.
    for (auto &r : m_rooms) {
        if (r.id != roomId)
            continue;
        MemberInfo &slot = r.members[member.userId];
        slot.userId = member.userId;
        if (!member.displayName.isEmpty())
            slot.displayName = member.displayName;
        if (!member.avatarMxcUrl.isEmpty())
            slot.avatarMxcUrl = member.avatarMxcUrl;
        Q_EMIT membersChanged(roomId);
        return;
    }
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
