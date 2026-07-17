#include "threads/ThreadController.h"

#include "matrix/MatrixClient.h"

#include <QBuffer>
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>

#include <algorithm>

namespace {
// Clipboard images beyond this edge are scaled down before encoding so a
// paste can never trigger an unbounded allocation or upload (mirrors the
// room composer's guard).
constexpr int kMaxPasteEdge = 4096;
}

ThreadController::ThreadController(QObject *parent)
    : QObject(parent)
    , m_attachments(new AttachmentQueueModel(this))
{
    connect(m_attachments, &AttachmentQueueModel::countChanged, this,
            [this] { Q_EMIT attachmentsChanged(); });
}

void ThreadController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    close();
    m_client = client;
    m_model.setClient(client);
    m_attachments->setClient(client);
    if (m_client) {
        connect(m_client, &MatrixClient::attachmentQueueFinished, this,
                &ThreadController::onAttachmentQueueFinished);
        connect(m_client, &MatrixClient::timelineReset, this,
                [this](const QString &timelineId) {
                    // Only the currently requested thread's reset promotes
                    // to Ready; anything else (rooms, stale threads) is not
                    // ours. Composite-id identity is the staleness gate.
                    if (m_state == Opening && timelineId == this->timelineId())
                        setState(Ready);
                });
        connect(m_client, &MatrixClient::threadTimelineFailed, this,
                [this](const QString &roomId, const QString &rootEventId,
                       const QString &category) {
                    if (m_state != Closed && roomId == m_roomId
                        && rootEventId == m_rootEventId)
                        setState(Failed, category);
                });
        connect(m_client, &MatrixClient::threadSubscriptionState, this,
                [this](const QString &roomId, const QString &rootEventId,
                       bool supported, bool subscribed, bool automatic) {
                    if (m_state == Closed || roomId != m_roomId
                        || rootEventId != m_rootEventId)
                        return;   // stale: another thread's answer
                    m_followSupported = supported;
                    m_followed = supported && subscribed;
                    m_followAutomatic = supported && automatic;
                    m_followBusy = false;
                    Q_EMIT followStateChanged();
                });
        connect(m_client, &MatrixClient::threadSubscriptionResult, this,
                [this](const QString &roomId, const QString &rootEventId,
                       bool ok, bool /*subscribed*/) {
                    if (m_state == Closed || roomId != m_roomId
                        || rootEventId != m_rootEventId)
                        return;
                    if (!ok) {
                        // The change did not apply; a fresh query restores
                        // the honest server state.
                        m_followBusy = false;
                        Q_EMIT followStateChanged();
                        m_client->queryThreadSubscription(m_roomId,
                                                          m_rootEventId);
                    }
                });
        connect(m_client, &MatrixClient::threadListUpdated, this,
                [this](const QString &roomId, const QVariantList &threads,
                       bool endReached, bool failed) {
                    if (!m_listOpen || roomId != m_listRoomId)
                        return;   // stale: list moved to another room
                    QVariantList sorted = threads;
                    // Latest activity first; unread-first where the loaded
                    // room timeline supplies a summary for the root.
                    std::sort(sorted.begin(), sorted.end(),
                              [this](const QVariant &a, const QVariant &b) {
                        const QVariantMap ma = a.toMap();
                        const QVariantMap mb = b.toMap();
                        const bool ua = threadUnreadHint(
                            ma.value(QStringLiteral("rootEventId")).toString());
                        const bool ub = threadUnreadHint(
                            mb.value(QStringLiteral("rootEventId")).toString());
                        if (ua != ub)
                            return ua;
                        const QDateTime ta =
                            ma.value(QStringLiteral("latestTimestamp"))
                                .toDateTime();
                        const QDateTime tb =
                            mb.value(QStringLiteral("latestTimestamp"))
                                .toDateTime();
                        return ta > tb;
                    });
                    for (auto &entry : sorted) {
                        QVariantMap map = entry.toMap();
                        map.insert(QStringLiteral("unread"),
                                   threadUnreadHint(
                                       map.value(QStringLiteral("rootEventId"))
                                           .toString()));
                        entry = map;
                    }
                    m_threadList = sorted;
                    m_listLoading = false;
                    m_listEndReached = endReached;
                    m_listFailed = failed;
                    Q_EMIT listStateChanged();
                });
        connect(m_client, &MatrixClient::loggedOut, this,
                [this] { close(); closeList(); });
    }
    Q_EMIT supportedChanged();
}

bool ThreadController::supported() const
{
    return m_client && m_client->supportsThreadTimelines();
}

QString ThreadController::timelineId() const
{
    if (m_roomId.isEmpty() || m_rootEventId.isEmpty())
        return {};
    return MatrixClient::threadTimelineId(m_roomId, m_rootEventId);
}

void ThreadController::openThread(const QString &roomId,
                                  const QString &rootEventId)
{
    if (!supported() || roomId.isEmpty() || rootEventId.isEmpty())
        return;
    if (m_state != Closed && roomId == m_roomId && rootEventId == m_rootEventId
        && m_state != Failed)
        return; // already open/opening — reopening would only reset scroll.

    m_roomId = roomId;
    m_rootEventId = rootEventId;
    m_failureCategory.clear();
    cancelReply();
    // Queued attachments belong to the thread they were prepared in; a thread
    // switch must never reroute them into the newly opened thread.
    clearAttachments();
    // Bind the model to the new composite id BEFORE dispatching so the
    // arriving snapshot reset is applied, never raced.
    m_model.setRoomId(timelineId());
    resetFollowState();
    m_lastMarkedReadEventId.clear();
    setState(Opening);
    m_client->openThread(roomId, rootEventId);
    m_followBusy = true;
    Q_EMIT followStateChanged();
    m_client->queryThreadSubscription(roomId, rootEventId);
}

void ThreadController::close()
{
    const bool wasActive = m_state != Closed;
    if (m_client && wasActive)
        m_client->closeThread();
    m_roomId.clear();
    m_rootEventId.clear();
    m_failureCategory.clear();
    m_model.setRoomId(QString{});
    cancelReply();
    clearAttachments();
    resetFollowState();
    m_lastMarkedReadEventId.clear();
    if (wasActive)
        setState(Closed);
}

void ThreadController::sendText(const QString &body)
{
    if (!m_client || m_state == Closed || m_roomId.isEmpty()
        || m_rootEventId.isEmpty())
        return;
    // Attachments go first — each becomes its own SDK local echo in the
    // thread — then the text as a separate thread message, matching the room
    // composer's "files + comment" behaviour.
    dispatchAttachments();
    const QString trimmed = body.trimmed();
    if (trimmed.isEmpty())
        return;   // attachment-only send is valid.
    // Always the backend's SDK thread path — never sendTextMessage, so a
    // thread reply can never land as an ordinary room message. An active
    // reply target makes it a rich reply within the thread.
    if (!m_replyToEventId.isEmpty()) {
        m_client->sendThreadReplyTo(m_roomId, m_rootEventId,
                                    m_replyToEventId, trimmed);
        cancelReply();
    } else {
        m_client->sendThreadReply(m_roomId, m_rootEventId, trimmed);
    }
}

bool ThreadController::attachmentsSupported() const
{
    return m_client && m_client->supportsAttachmentSend() && supported()
        && m_state != Closed;
}

void ThreadController::addAttachment(const QUrl &fileUrl)
{
    if (!attachmentsSupported()) {
        Q_EMIT attachmentRejected(
            tr("Attachments are not supported on this backend."));
        return;
    }
    const QString reason = m_attachments->addFile(fileUrl);
    if (!reason.isEmpty())
        Q_EMIT attachmentRejected(reason);
}

bool ThreadController::pasteFromClipboard()
{
    if (!attachmentsSupported())
        return false;
    const QClipboard *clipboard = QGuiApplication::clipboard();
    const QMimeData *mime = clipboard ? clipboard->mimeData() : nullptr;
    if (!mime)
        return false;

    // Real file MIME (text/uri-list) — plain text is never treated as paths.
    if (mime->hasUrls()) {
        bool any = false;
        const auto urls = mime->urls();
        for (const QUrl &url : urls) {
            if (!url.isLocalFile())
                continue;
            any = true;
            addAttachment(url);
        }
        if (any)
            return true;
    }

    if (mime->hasImage()) {
        QImage image = qvariant_cast<QImage>(mime->imageData());
        if (image.isNull())
            return false;
        if (image.width() > kMaxPasteEdge || image.height() > kMaxPasteEdge)
            image = image.scaled(kMaxPasteEdge, kMaxPasteEdge,
                                 Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG")) {
            Q_EMIT attachmentRejected(
                tr("The clipboard image could not be read."));
            return true;   // handled: never paste binary junk as text
        }
        const QString reason = m_attachments->addImageData(
            bytes, QStringLiteral("image/png"), image.width(), image.height());
        if (!reason.isEmpty())
            Q_EMIT attachmentRejected(reason);
        return true;
    }
    return false;
}

void ThreadController::dispatchAttachments()
{
    if (!m_client || m_state != Ready || m_roomId.isEmpty()
        || m_rootEventId.isEmpty())
        return;
    auto &entries = m_attachments->entries();
    for (int row = 0; row < entries.size(); ++row) {
        auto &entry = entries[row];
        if (entry.state != QLatin1String("queued"))
            continue;
        quint64 opId = 0;
        if (!entry.localPath.isEmpty()) {
            opId = m_client->sendThreadAttachment(
                m_roomId, m_rootEventId, entry.localPath, entry.mime, QString(),
                entry.width, entry.height, entry.animated);
        } else {
            opId = m_client->sendThreadAttachmentBytes(
                m_roomId, m_rootEventId, entry.data, entry.fileName, entry.mime,
                entry.width, entry.height);
        }
        if (opId == 0) {
            entry.state = QStringLiteral("failed");
            entry.error = tr("The attachment could not be queued.");
        } else {
            entry.state = QStringLiteral("dispatching");
            entry.opId = opId;
        }
        m_attachments->updateEntry(row);
    }
}

void ThreadController::onAttachmentQueueFinished(quint64 opId,
                                                 const QString &roomId,
                                                 bool ok,
                                                 const QString &category)
{
    Q_UNUSED(roomId);
    Q_UNUSED(category);
    if (opId == 0)
        return;
    auto &entries = m_attachments->entries();
    for (int row = 0; row < entries.size(); ++row) {
        if (entries[row].opId != opId)
            continue;   // not ours (the room composer owns other op ids).
        if (ok) {
            // The SDK local echo now owns this attachment's send state; the
            // tray entry has served its purpose.
            entries[row].state = QStringLiteral("sent");
            m_attachments->removeAt(row);
        } else {
            entries[row].state = QStringLiteral("failed");
            entries[row].error = tr("Upload failed. Retry or remove.");
            entries[row].opId = 0;
            m_attachments->updateEntry(row);
        }
        return;
    }
}

void ThreadController::clearAttachments()
{
    if (m_attachments && !m_attachments->isEmpty())
        m_attachments->clearAll();
}

void ThreadController::beginReply(const QString &eventId)
{
    if (m_state == Closed || eventId.isEmpty())
        return;
    // Only loaded thread events are valid reply targets (replying to the
    // root is a plain thread message, so it clears the target instead).
    if (eventId == m_rootEventId) {
        cancelReply();
        return;
    }
    const int row = m_model.rowForStableId(eventId);
    if (row < 0)
        return;
    const QModelIndex idx = m_model.index(row, 0);
    m_replyToEventId = eventId;
    m_replyToSender =
        m_model.data(idx, TimelineModel::SenderDisplayNameRole).toString();
    if (m_replyToSender.isEmpty())
        m_replyToSender =
            m_model.data(idx, TimelineModel::SenderRole).toString();
    m_replyToPreview = m_model.visibleTextForEvent(eventId).left(80);
    Q_EMIT replyStateChanged();
}

void ThreadController::cancelReply()
{
    if (m_replyToEventId.isEmpty() && m_replyToSender.isEmpty()
        && m_replyToPreview.isEmpty())
        return;
    m_replyToEventId.clear();
    m_replyToSender.clear();
    m_replyToPreview.clear();
    Q_EMIT replyStateChanged();
}

QStringList ThreadController::participants() const
{
    QStringList result;
    for (int row = 0; row < m_model.rowCount(); ++row) {
        const QModelIndex idx = m_model.index(row, 0);
        if (m_model.data(idx, TimelineModel::IsVirtualRole).toBool())
            continue;
        const QString sender =
            m_model.data(idx, TimelineModel::SenderRole).toString();
        if (!sender.isEmpty() && !result.contains(sender))
            result.append(sender);
    }
    return result;
}

QVariantMap ThreadController::rootInfo() const
{
    QVariantMap info;
    info.insert(QStringLiteral("loaded"), false);
    if (m_state == Closed || m_rootEventId.isEmpty())
        return info;

    auto fill = [&](auto data) {
        info.insert(QStringLiteral("loaded"), true);
        info.insert(QStringLiteral("eventId"), m_rootEventId);
        info.insert(QStringLiteral("sender"),
                    data(TimelineModel::SenderRole));
        info.insert(QStringLiteral("senderDisplayName"),
                    data(TimelineModel::SenderDisplayNameRole));
        info.insert(QStringLiteral("senderAvatarMxc"),
                    data(TimelineModel::SenderAvatarMxcRole));
        info.insert(QStringLiteral("body"), data(TimelineModel::BodyRole));
        info.insert(QStringLiteral("timestamp"),
                    data(TimelineModel::TimestampRole));
        info.insert(QStringLiteral("redacted"),
                    data(TimelineModel::RedactedRole));
        info.insert(QStringLiteral("undecryptable"),
                    data(TimelineModel::UndecryptableRole));
        info.insert(QStringLiteral("isEncrypted"),
                    data(TimelineModel::IsEncryptedRole));
        info.insert(QStringLiteral("isImage"),
                    data(TimelineModel::IsImageRole));
        info.insert(QStringLiteral("isFile"),
                    data(TimelineModel::IsFileRole));
    };

    // Prefer the loaded thread timeline; the room timeline is the fallback
    // while the thread snapshot is still arriving.
    const int threadRow = m_model.rowForStableId(m_rootEventId);
    if (threadRow >= 0) {
        const QModelIndex idx = m_model.index(threadRow, 0);
        fill([&](int role) { return m_model.data(idx, role); });
        return info;
    }
    if (m_client) {
        const auto roomEvents = m_client->timeline(m_roomId);
        for (const auto &event : roomEvents) {
            if (event.eventId != m_rootEventId)
                continue;
            info.insert(QStringLiteral("loaded"), true);
            info.insert(QStringLiteral("eventId"), event.eventId);
            info.insert(QStringLiteral("sender"), event.sender);
            info.insert(QStringLiteral("senderDisplayName"),
                        event.senderDisplayName.isEmpty()
                            ? event.sender
                            : event.senderDisplayName);
            info.insert(QStringLiteral("senderAvatarMxc"),
                        event.senderAvatarUrl);
            info.insert(QStringLiteral("body"), event.body);
            info.insert(QStringLiteral("timestamp"), event.timestamp);
            info.insert(QStringLiteral("redacted"), event.redacted);
            info.insert(QStringLiteral("undecryptable"), event.undecryptable);
            info.insert(QStringLiteral("isEncrypted"), event.isEncrypted);
            info.insert(QStringLiteral("isImage"),
                        event.type == TimelineEvent::Image);
            info.insert(QStringLiteral("isFile"),
                        event.type == TimelineEvent::File);
            return info;
        }
    }
    return info;
}

void ThreadController::handleCurrentRoomChanged(const QString &currentRoomId)
{
    if (m_listOpen && currentRoomId != m_listRoomId)
        closeList();
    if (m_state == Closed)
        return;
    if (currentRoomId != m_roomId)
        close();
}

void ThreadController::resetFollowState()
{
    if (!m_followSupported && !m_followed && !m_followAutomatic
        && !m_followBusy)
        return;
    m_followSupported = false;
    m_followed = false;
    m_followAutomatic = false;
    m_followBusy = false;
    Q_EMIT followStateChanged();
}

void ThreadController::setFollowed(bool followed)
{
    if (!m_client || m_state == Closed || !m_followSupported || m_followBusy
        || followed == m_followed)
        return;
    m_followBusy = true;
    Q_EMIT followStateChanged();
    m_client->setThreadSubscribed(m_roomId, m_rootEventId, followed);
}

void ThreadController::markRead()
{
    if (!m_client || m_state != Ready)
        return;
    const QString latest = m_model.latestReadableEventId();
    if (latest.isEmpty() || latest == m_lastMarkedReadEventId)
        return;   // deduplicated: one receipt per new latest reply
    m_lastMarkedReadEventId = latest;
    m_client->markThreadRead(m_roomId, m_rootEventId);
}

bool ThreadController::threadUnreadHint(const QString &rootEventId) const
{
    if (!m_client || rootEventId.isEmpty())
        return false;
    const QString roomId = m_listOpen ? m_listRoomId : m_roomId;
    const auto events = m_client->timeline(roomId);
    for (const auto &event : events) {
        if (event.eventId == rootEventId)
            return event.threadUnread;
    }
    return false;
}

void ThreadController::openList(const QString &roomId)
{
    if (!m_client || !m_client->supportsThreadList() || roomId.isEmpty())
        return;
    m_listRoomId = roomId;
    m_listOpen = true;
    m_listLoading = true;
    m_listFailed = false;
    m_listEndReached = false;
    m_threadList.clear();
    Q_EMIT listStateChanged();
    m_client->openThreadList(roomId);
}

void ThreadController::closeList()
{
    if (!m_listOpen)
        return;
    if (m_client)
        m_client->closeThreadList();
    m_listOpen = false;
    m_listLoading = false;
    m_listFailed = false;
    m_listEndReached = false;
    m_listRoomId.clear();
    m_threadList.clear();
    Q_EMIT listStateChanged();
}

void ThreadController::paginateList()
{
    if (!m_client || !m_listOpen || m_listLoading || m_listEndReached)
        return;
    m_listLoading = true;
    Q_EMIT listStateChanged();
    m_client->paginateThreadList(m_listRoomId);
}

void ThreadController::setState(State state, const QString &failureCategory)
{
    if (m_state == state && m_failureCategory == failureCategory)
        return;
    m_state = state;
    m_failureCategory = failureCategory;
    Q_EMIT stateChanged();
}
