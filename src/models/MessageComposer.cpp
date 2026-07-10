#include "models/MessageComposer.h"

#include "matrix/MatrixClient.h"

namespace {
// matrix-sdk 0.18 advertises a four-second typing timeout and suppresses
// redundant calls itself. Renew just before expiry; text is never forwarded.
constexpr int kTypingRefreshMs = 3000;
constexpr int kTypingTimeoutMs = 4000;
}

MessageComposer::MessageComposer(QObject *parent)
    : QObject(parent)
{
    m_typingRefresh.setInterval(kTypingRefreshMs);
    connect(&m_typingRefresh, &QTimer::timeout, this, [this] {
        if (m_client && !m_roomId.isEmpty() && m_typingActive)
            m_client->sendTyping(m_roomId, true, kTypingTimeoutMs);
    });
}

void MessageComposer::setClient(MatrixClient *client)
{
    m_client = client;
    updateCanSend();
}

void MessageComposer::setText(const QString &t)
{
    if (m_text == t)
        return;
    m_text = t;
    Q_EMIT textChanged();
    updateCanSend();
    refreshTypingState();
}

void MessageComposer::setRoomId(const QString &r)
{
    if (m_roomId == r)
        return;
    // Cancel typing on the previous room and any pending reply/edit.
    if (!m_roomId.isEmpty()) stopTyping();
    cancelReplyOrEdit();
    m_roomId = r;
    m_text.clear();
    Q_EMIT textChanged();
    Q_EMIT roomIdChanged();
    updateCanSend();
}

bool MessageComposer::canSend() const
{
    return m_canSend;
}

void MessageComposer::send()
{
    if (!m_canSend || !m_client) return;
    const QString body = m_text.trimmed();
    if (!m_editingEventId.isEmpty()) {
        m_client->editMessage(m_roomId, m_editingEventId, body);
    } else if (!m_threadRootId.isEmpty()) {
        // v0.4.1: thread replies. Mock preserves thread grouping; HTTP
        // currently falls back to sendReply via the interface default.
        m_client->sendThreadReply(m_roomId, m_threadRootId, body);
    } else if (!m_replyingToEventId.isEmpty()) {
        m_client->sendReply(m_roomId, m_replyingToEventId, body);
    } else {
        m_client->sendTextMessage(m_roomId, body);
    }
    stopTyping();
    cancelReplyOrEdit();
    clear();
}

void MessageComposer::clear()
{
    if (m_text.isEmpty()) return;
    m_text.clear();
    Q_EMIT textChanged();
    updateCanSend();
    refreshTypingState();
}

void MessageComposer::beginReply(const QString &eventId,
                                  const QString &sender,
                                  const QString &preview)
{
    m_editingEventId.clear();
    Q_EMIT editStateChanged();
    m_replyingToEventId = eventId;
    m_replyingToSender  = sender;
    m_replyingToPreview = preview;
    Q_EMIT replyStateChanged();
}

void MessageComposer::beginEdit(const QString &eventId,
                                 const QString &currentBody)
{
    m_replyingToEventId.clear();
    m_replyingToSender.clear();
    m_replyingToPreview.clear();
    Q_EMIT replyStateChanged();
    m_threadRootId.clear();
    m_threadPreview.clear();
    Q_EMIT threadStateChanged();
    m_editingEventId = eventId;
    m_text = currentBody;
    Q_EMIT editStateChanged();
    Q_EMIT textChanged();
    updateCanSend();
}

void MessageComposer::beginThreadReply(const QString &rootEventId,
                                        const QString &preview)
{
    m_editingEventId.clear();
    Q_EMIT editStateChanged();
    m_replyingToEventId.clear();
    m_replyingToSender.clear();
    m_replyingToPreview.clear();
    Q_EMIT replyStateChanged();
    m_threadRootId  = rootEventId;
    m_threadPreview = preview;
    Q_EMIT threadStateChanged();
}

void MessageComposer::cancelReplyOrEdit()
{
    const bool wasReplying = !m_replyingToEventId.isEmpty();
    const bool wasEditing  = !m_editingEventId.isEmpty();
    const bool wasInThread = !m_threadRootId.isEmpty();
    m_replyingToEventId.clear();
    m_replyingToSender.clear();
    m_replyingToPreview.clear();
    m_editingEventId.clear();
    m_threadRootId.clear();
    m_threadPreview.clear();
    if (wasEditing) {
        m_text.clear();
        Q_EMIT textChanged();
        updateCanSend();
    }
    if (wasReplying) Q_EMIT replyStateChanged();
    if (wasEditing)  Q_EMIT editStateChanged();
    if (wasInThread) Q_EMIT threadStateChanged();
}

void MessageComposer::reactTo(const QString &targetEventId, const QString &key)
{
    if (!m_client || m_roomId.isEmpty()) return;
    m_client->toggleReaction(m_roomId, targetEventId, key);
}

void MessageComposer::redact(const QString &eventId)
{
    if (!m_client || m_roomId.isEmpty()) return;
    m_client->redactEvent(m_roomId, eventId, {});
}

void MessageComposer::sendImageFromPath(const QString &localPath)
{
    if (!m_client || m_roomId.isEmpty() || localPath.isEmpty()) return;
    m_client->sendImage(m_roomId, localPath);
}

void MessageComposer::sendFileFromPath(const QString &localPath)
{
    if (!m_client || m_roomId.isEmpty() || localPath.isEmpty()) return;
    m_client->sendFile(m_roomId, localPath);
}

void MessageComposer::updateCanSend()
{
    const bool next = m_client
                      && !m_roomId.isEmpty()
                      && !m_text.trimmed().isEmpty();
    if (next == m_canSend)
        return;
    m_canSend = next;
    Q_EMIT canSendChanged();
}

void MessageComposer::refreshTypingState()
{
    if (!m_client || m_roomId.isEmpty())
        return;
    const bool wantTyping = !m_text.trimmed().isEmpty();
    if (wantTyping && !m_typingActive) {
        m_typingActive = true;
        m_client->sendTyping(m_roomId, true, kTypingTimeoutMs);
        m_typingRefresh.start();
    } else if (!wantTyping && m_typingActive) {
        stopTyping();
    }
}

void MessageComposer::stopTyping()
{
    m_typingRefresh.stop();
    if (m_typingActive && m_client && !m_roomId.isEmpty()) {
        m_client->sendTyping(m_roomId, false, 0);
    }
    m_typingActive = false;
}
