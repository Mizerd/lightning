#include "models/MessageComposer.h"

#include "matrix/MatrixClient.h"
#include "models/MarkdownFormat.h"

#include <QBuffer>
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>

namespace {
// matrix-sdk 0.18 advertises a four-second typing timeout and suppresses
// redundant calls itself. Renew just before expiry; text is never forwarded.
constexpr int kTypingRefreshMs = 3000;
constexpr int kTypingTimeoutMs = 4000;
// Clipboard images beyond this edge are scaled down before encoding so a
// paste can never trigger an unbounded allocation or upload.
constexpr int kMaxPasteEdge = 4096;
}

MessageComposer::MessageComposer(QObject *parent)
    : QObject(parent)
    , m_attachments(new AttachmentQueueModel(this))
{
    m_typingRefresh.setInterval(kTypingRefreshMs);
    connect(&m_typingRefresh, &QTimer::timeout, this, [this] {
        if (m_client && !m_roomId.isEmpty() && m_typingActive)
            m_client->sendTyping(m_roomId, true, kTypingTimeoutMs);
    });
    connect(m_attachments, &AttachmentQueueModel::countChanged, this, [this] {
        Q_EMIT attachmentsChanged();
        updateCanSend();
    });
}

void MessageComposer::setClient(MatrixClient *client)
{
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    m_attachments->setClient(client);
    if (m_client) {
        connect(m_client, &MatrixClient::attachmentQueueFinished,
                this, &MessageComposer::onAttachmentQueueFinished);
        connect(m_client, &MatrixClient::loggedOut, this, [this] {
            m_attachments->clearAll();
        });
    }
    updateCanSend();
}

bool MessageComposer::attachmentsSupported() const
{
    return m_client && m_client->supportsAttachmentSend() && !m_roomId.isEmpty();
}

void MessageComposer::addAttachment(const QUrl &fileUrl)
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

bool MessageComposer::pasteFromClipboard()
{
    if (!attachmentsSupported())
        return false;
    const QClipboard *clipboard = QGuiApplication::clipboard();
    const QMimeData *mime = clipboard ? clipboard->mimeData() : nullptr;
    if (!mime)
        return false;

    // Real file MIME data (text/uri-list) — e.g. copy from a file manager.
    // Plain text is deliberately NOT interpreted as paths.
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
            Q_EMIT attachmentRejected(tr("The clipboard image could not be read."));
            return true; // handled: do not paste binary junk as text
        }
        const QString reason = m_attachments->addImageData(
            bytes, QStringLiteral("image/png"), image.width(), image.height());
        if (!reason.isEmpty())
            Q_EMIT attachmentRejected(reason);
        return true;
    }

    return false;
}

void MessageComposer::dispatchAttachments()
{
    if (!m_client || m_roomId.isEmpty())
        return;
    auto &entries = m_attachments->entries();
    for (int row = 0; row < entries.size(); ++row) {
        auto &entry = entries[row];
        if (entry.state != QLatin1String("queued"))
            continue;
        quint64 opId = 0;
        if (!entry.localPath.isEmpty()) {
            opId = m_client->sendAttachment(m_roomId, entry.localPath,
                                            entry.mime, QString(), entry.width,
                                            entry.height, entry.animated);
        } else {
            opId = m_client->sendAttachmentBytes(m_roomId, entry.data,
                                                 entry.fileName, entry.mime,
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

void MessageComposer::onAttachmentQueueFinished(quint64 opId,
                                                const QString &roomId,
                                                bool ok,
                                                const QString &category)
{
    Q_UNUSED(roomId);
    Q_UNUSED(category);
    auto &entries = m_attachments->entries();
    for (int row = 0; row < entries.size(); ++row) {
        if (entries[row].opId != opId || opId == 0)
            continue;
        if (ok) {
            // The SDK local echo now owns this attachment's send state; the
            // tray entry has served its purpose. (State leaves "dispatching"
            // first so removeAt's mid-dispatch guard does not apply.)
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
    // Queued attachments belong to the room they were prepared in; entries
    // already dispatched continue in the SDK send queue regardless.
    m_attachments->clearAll();
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
        // Edit mode edits text only; attachments stay queued.
        if (body.isEmpty()) return;
        m_client->editMessage(m_roomId, m_editingEventId, body);
    } else {
        // v0.5.9: attachments go first (each becomes its own SDK local
        // echo), then the text as a separate message — matching how other
        // Matrix clients compose "files + comment".
        dispatchAttachments();
        if (!body.isEmpty()) {
            if (!m_threadRootId.isEmpty()) {
                // v0.4.1: thread replies. Mock preserves thread grouping;
                // HTTP falls back to sendReply via the interface default.
                m_client->sendThreadReply(m_roomId, m_threadRootId, body);
            } else if (!m_replyingToEventId.isEmpty()) {
                m_client->sendReply(m_roomId, m_replyingToEventId, body);
            } else {
                m_client->sendTextMessage(m_roomId, body);
            }
        }
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
    // Sendable with text, or with queued attachments (outside edit mode).
    const bool hasQueuedAttachment =
        hasAttachments() && m_editingEventId.isEmpty();
    const bool next = m_client
                      && !m_roomId.isEmpty()
                      && (!m_text.trimmed().isEmpty() || hasQueuedAttachment);
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

QVariantMap MessageComposer::toggleFormat(const QString &format,
                                          const QString &text,
                                          int selectionStart,
                                          int selectionEnd) const
{
    const auto result = MarkdownFormat::toggle(format, text,
                                               selectionStart, selectionEnd);
    return {
        { QStringLiteral("text"), result.text },
        { QStringLiteral("selectionStart"), result.selectionStart },
        { QStringLiteral("selectionEnd"), result.selectionEnd },
    };
}

QVariantMap MessageComposer::formatState(const QString &text,
                                         int selectionStart,
                                         int selectionEnd) const
{
    return MarkdownFormat::state(text, selectionStart, selectionEnd);
}
