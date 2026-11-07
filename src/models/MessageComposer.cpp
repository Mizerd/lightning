#include "models/MessageComposer.h"

#include <QRegularExpression>
#include <QSet>

#include "app/DraftStore.h"
#include "app/JoinErrorText.h"
#include "matrix/MatrixClient.h"
#include "media/ImageMetadata.h"
#include "models/MarkdownFormat.h"
#include "models/RichComposition.h"

#include <QBuffer>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>

namespace {
// The SDK advertises a four-second typing timeout and suppresses redundant
// calls itself; renew just before expiry. Text is never forwarded.
constexpr int kTypingRefreshMs = 3000;
constexpr int kTypingTimeoutMs = 4000;
// Clipboard images larger than this are scaled down before encoding, so a
// paste cannot cause an unbounded allocation or upload.
constexpr int kMaxPasteEdge = 4096;
constexpr int kDraftSaveMs = 1000;
// A Matrix event is capped at 65,536 bytes of canonical JSON (the server answers
// M_TOO_LARGE); the envelope (sender, room, hashes, signatures, auth and prev
// events) takes roughly 1.5 KB of it, so the content gets a little less.
constexpr qsizetype kMaxEventContentBytes = 64000;
}

MessageComposer::MessageComposer(QObject *parent)
    : QObject(parent)
    , m_attachments(new AttachmentQueueModel(this))
    , m_vim(new VimKeymap(this))
    , m_threadVim(new VimKeymap(this))
{
    m_typingRefresh.setInterval(kTypingRefreshMs);
    connect(&m_typingRefresh, &QTimer::timeout, this, [this] {
        if (m_client && !m_roomId.isEmpty() && m_typingActive
            && m_typingEnabled)
            m_client->sendTyping(m_roomId, true, kTypingTimeoutMs);
    });
    connect(m_attachments, &AttachmentQueueModel::countChanged, this, [this] {
        Q_EMIT attachmentsChanged();
        updateCanSend();
    });
    connect(m_attachments, &AttachmentQueueModel::entryPrepared,
            this, &MessageComposer::pumpAttachments);
    m_draftDebounce.setSingleShot(true);
    m_draftDebounce.setInterval(kDraftSaveMs);
    connect(&m_draftDebounce, &QTimer::timeout, this,
            [this] { saveDraftNow(); });
}

void MessageComposer::saveDraftNow()
{
    if (!m_drafts || m_roomId.isEmpty() || m_restoringDraft)
        return;
    if (!m_editingEventId.isEmpty())
        return;
    QVariantMap draft;
    draft.insert(QStringLiteral("text"), m_text);
    if (!m_replyingToEventId.isEmpty()) {
        draft.insert(QStringLiteral("replyToEventId"), m_replyingToEventId);
        draft.insert(QStringLiteral("replyToSender"), m_replyingToSender);
        draft.insert(QStringLiteral("replyToPreview"), m_replyingToPreview);
        if (!m_replyingToMediaKey.isEmpty())
            draft.insert(QStringLiteral("replyToMediaKey"),
                         m_replyingToMediaKey);
    }
    if (!m_mentionRefs.isEmpty()) {
        QVariantList refs;
        for (const mention::MentionRef &ref : m_mentionRefs) {
            refs.append(QVariantMap{
                { QStringLiteral("userId"), ref.userId },
                { QStringLiteral("displayText"), ref.displayText },
                { QStringLiteral("start"), ref.start },
                { QStringLiteral("length"), ref.length },
            });
        }
        draft.insert(QStringLiteral("mentions"), refs);
    }
    m_drafts->save(draftKey(), m_roomId, draft);
}

QString MessageComposer::draftKey() const
{
    if (m_roomId.isEmpty())
        return {};
    if (m_threadTargetRootId.isEmpty())
        return m_roomId;
    return MatrixClient::threadTimelineId(m_roomId, m_threadTargetRootId);
}

void MessageComposer::restoreDraft()
{
    if (!m_drafts || m_roomId.isEmpty())
        return;
    const QVariantMap draft = m_drafts->load(draftKey());
    if (DraftStore::draftIsEmpty(draft))
        return;
    m_restoringDraft = true;
    m_text = draft.value(QStringLiteral("text")).toString();
    m_mentionRefs.clear();
    m_emoticonRefs.clear();
    for (const EmoticonRef &ref : m_emoticonRefsByRoom.take(m_roomId)) {
        if (ref.start >= 0 && ref.start + ref.length <= m_text.size()
            && m_text.mid(ref.start, ref.length) == ref.displayText)
            m_emoticonRefs.append(ref);
    }
    const QVariantList refs = draft.value(QStringLiteral("mentions")).toList();
    for (const QVariant &value : refs) {
        const QVariantMap map = value.toMap();
        mention::MentionRef ref;
        ref.userId = map.value(QStringLiteral("userId")).toString();
        ref.displayText = map.value(QStringLiteral("displayText")).toString();
        ref.start = map.value(QStringLiteral("start")).toInt();
        ref.length = map.value(QStringLiteral("length")).toInt();
        if (!ref.userId.isEmpty() && ref.start >= 0 && ref.length > 0
            && ref.start + ref.length <= m_text.size()
            && m_text.mid(ref.start, ref.length) == ref.displayText) {
            m_mentionRefs.append(ref);
        }
    }
    m_replyingToEventId =
        draft.value(QStringLiteral("replyToEventId")).toString();
    m_replyingToSender =
        draft.value(QStringLiteral("replyToSender")).toString();
    m_replyingToPreview =
        draft.value(QStringLiteral("replyToPreview")).toString();
    m_replyingToMediaKey =
        draft.value(QStringLiteral("replyToMediaKey")).toString();
    m_restoringDraft = false;
    Q_EMIT textChanged();
    Q_EMIT mentionRangesChanged();
    Q_EMIT replyStateChanged();
    updateCanSend();
}

void MessageComposer::setClient(MatrixClient *client)
{
    if (m_client)
        m_client->disconnect(this);
    m_imagePrep.releaseAll();
    m_client = client;
    m_slashJoinOp = 0;
    ++m_dropGeneration;
    m_attachments->setClient(client);
    if (m_client) {
        connect(m_client, &MatrixClient::roomJoinFinished, this,
                [this](quint64 opId, bool ok, const QString &,
                       const QString &category) {
                    if (opId == 0 || opId != m_slashJoinOp)
                        return;
                    m_slashJoinOp = 0;
                    if (!ok) {
                        setCommandError(
                            joinerror::describe(category));
                    }
                });
        connect(m_client, &MatrixClient::attachmentQueueFinished,
                this, &MessageComposer::onAttachmentQueueFinished);
        connect(m_client, &MatrixClient::messageEditsRemoved, this,
                [this](const QString &roomId, const QString &eventId, bool ok,
                       int removed, int failed, bool truncated) {
                    if (roomId != m_roomId)
                        return;
                    Q_EMIT editsRemoved(eventId, ok, removed, failed,
                                        truncated);
                });
        connect(m_client, &MatrixClient::loggedOut, this, [this] {
            ++m_dropGeneration;
            m_imagePrep.releaseAll();
            m_attachments->clearAll();
            // Unresolved voice recordings must not outlive the session on disk.
            for (const VoiceOp &op : std::as_const(m_voiceOps))
                QFile::remove(op.localPath);
            m_voiceOps.clear();
            // A pending draft save must die with the session: firing after
            // DraftStore's loggedOut wipe would re-insert the signed-out
            // account's plaintext. The text goes too.
            m_draftDebounce.stop();
            m_roomId.clear();
            cancelReplyOrEdit();
            m_draftDebounce.stop();
            if (!m_text.isEmpty()) {
                m_text.clear();
                Q_EMIT textChanged();
            }
            m_mentionRefs.clear();
            m_emoticonRefs.clear();
            m_emoticonRefsByRoom.clear();
            m_threadEmoticonChoices.clear();
            Q_EMIT mentionRangesChanged();
            Q_EMIT roomIdChanged();
            updateCanSend();
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

void MessageComposer::addDroppedAttachments(const QList<QUrl> &urls,
                                            const QString &portalKey)
{
    if (!attachmentsSupported()) {
        Q_EMIT attachmentRejected(
            tr("Attachments are not supported on this backend."));
        return;
    }
    // The room id alone is not enough: room A, sign-out, the next account
    // opens room A. Session, client and account must all still match.
    const QString roomId = m_roomId;
    const quint64 generation = m_dropGeneration;
    const MatrixClient *client = m_client;
    const QString userId = m_client->currentUserId();
    AttachmentQueueModel::resolveDroppedUrls(
        urls, portalKey, this,
        [this, roomId, generation, client, userId](
            const QList<QUrl> &resolved) {
            if (m_roomId != roomId || m_dropGeneration != generation
                || m_client != client || !m_client
                || m_client->currentUserId() != userId)
                return;
            if (resolved.isEmpty()) {
                Q_EMIT attachmentRejected(tr("That file cannot be read."));
                return;
            }
            for (const QUrl &url : resolved)
                addAttachment(url);
            Q_EMIT droppedAttachmentsResolved();
        });
}

bool MessageComposer::pasteFromClipboard()
{
    if (!attachmentsSupported())
        return false;
    const QClipboard *clipboard = QGuiApplication::clipboard();
    const QMimeData *mime = clipboard ? clipboard->mimeData() : nullptr;
    if (!mime)
        return false;

    if (mime->hasUrls()) {
        QList<QUrl> local;
        const auto urls = mime->urls();
        for (const QUrl &url : urls) {
            if (url.isLocalFile())
                local.append(url);
        }
        if (!local.isEmpty()) {
            addDroppedAttachments(
                local, AttachmentQueueModel::portalKeyFromMimeData(mime));
            return true;
        }
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
            return true;
        }
        const QString reason = m_attachments->addImageData(
            bytes, QStringLiteral("image/png"), image.width(), image.height());
        if (!reason.isEmpty())
            Q_EMIT attachmentRejected(reason);
        return true;
    }

    return false;
}

void MessageComposer::setSendTextAsCaption(bool on)
{
    if (m_sendTextAsCaption == on) return;
    m_sendTextAsCaption = on;
    Q_EMIT sendTextAsCaptionChanged();
}

void MessageComposer::setImageMetadataMode(const QString &mode)
{
    const QString normalized =
        imagemeta::modeToString(imagemeta::modeFromString(mode));
    if (m_imageMetadataMode == normalized)
        return;
    m_imageMetadataMode = normalized;
    Q_EMIT imageMetadataModeChanged();
}

bool MessageComposer::takeTextAsCaption(const QString &body,
                                        const QStringList &mentionIds)
{
    if (!m_sendTextAsCaption || body.isEmpty() || !mentionIds.isEmpty()
        || !sendThreadRootId().isEmpty() || !m_attachments)
        return false;
    auto &entries = m_attachments->entries();
    for (int row = 0; row < entries.size(); ++row) {
        auto &entry = entries[row];
        if (entry.state != QLatin1String("queued") || entry.localPath.isEmpty())
            continue;
        entry.caption = body;
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
        if (entries[row].state != QLatin1String("queued"))
            continue;
        entries[row].sendRequested = true;
        if (!m_replyingToEventId.isEmpty())
            entries[row].replyToEventId = m_replyingToEventId;
        // The thread is captured with the reply: a row must not move between
        // the room and a thread if the filter changes before it is sent.
        const QString root = sendThreadRootId();
        if (!root.isEmpty()) {
            entries[row].threadRootId = root;
            if (entries[row].replyToEventId == root)
                entries[row].replyToEventId.clear();
        }
    }
    pumpAttachments();
}

// The send queue fixes its order when a file has been read and cached, which
// takes as long as the file is big, so dispatching every row at once lands
// them in completion order (a 20 MB file behind a 20 KB one arrives second).
// Rows go one at a time in tray order instead: the next one starts only when
// the previous result is back (it resolves once the SDK has queued the
// event), and a video still waiting for its poster holds the rows behind it.
void MessageComposer::pumpAttachments()
{
    if (!m_client || m_roomId.isEmpty())
        return;
    auto &entries = m_attachments->entries();
    for (int row = 0; row < entries.size(); ++row) {
        const QString state = entries[row].state;
        if (state == QLatin1String("dispatching"))
            return;
        if (state != QLatin1String("queued") || !entries[row].sendRequested)
            continue;
        if (entries[row].posterPending)
            return;
        dispatchAttachment(row);
        if (entries[row].state == QLatin1String("dispatching"))
            return;
    }
}

void MessageComposer::dispatchAttachment(int row)
{
    if (!m_client || m_roomId.isEmpty())
        return;
    auto &entries = m_attachments->entries();
    if (row < 0 || row >= entries.size())
        return;
    auto &entry = entries[row];
    if (entry.state != QLatin1String("queued") || !entry.sendRequested
        || entry.posterPending)
        return;
    const QString caption = entry.caption;
    quint64 opId = 0;
    QString sendPath = entry.localPath;
    QByteArray sendData = entry.data;
    QString strippedSub;
    if (!entry.isVideo && !entry.isSvg
        && entry.mime.startsWith(QLatin1String("image/"))
        && !m_imagePrep.prepare(m_imageMetadataMode, entry.mime, sendPath,
                                sendData, &strippedSub)) {
        entry.state = QStringLiteral("failed");
        entry.error = tr("Photo details could not be removed from this image, "
                         "so it was not sent. You can change this under "
                         "Settings, Privacy.");
        m_attachments->updateEntry(row);
        return;
    }
    if (entry.spoiler) {
        opId = entry.isSvg ? 0 : m_client->sendSpoilerMedia(
            m_roomId, entry.threadRootId,
            entry.localPath.isEmpty() ? QString() : sendPath, sendData,
            entry.fileName, entry.mime, caption, entry.width, entry.height,
            entry.durationMs, entry.animated, entry.poster, entry.posterWidth,
            entry.posterHeight, entry.replyToEventId);
    } else if (!entry.threadRootId.isEmpty()) {
        // Inline thread style: the SDK thread send path, never the room one
        // (CLAUDE.md §8). An SVG never reaches here converted or not (below).
        const QString &root = entry.threadRootId;
        if (entry.isSvg)
            opId = 0;
        else if (entry.localPath.isEmpty())
            opId = m_client->sendThreadAttachmentBytes(
                m_roomId, root, sendData, entry.fileName, entry.mime,
                entry.width, entry.height, entry.replyToEventId);
        else if (entry.isVideo)
            opId = m_client->sendThreadVideo(
                m_roomId, root, entry.localPath, entry.mime, caption,
                entry.width, entry.height, entry.durationMs, entry.poster,
                entry.posterWidth, entry.posterHeight, entry.replyToEventId);
        else
            opId = m_client->sendThreadAttachment(
                m_roomId, root, sendPath, entry.mime, caption, entry.width,
                entry.height, entry.animated, entry.durationMs,
                entry.replyToEventId);
    } else if (entry.localPath.isEmpty()) {
        opId = m_client->sendAttachmentBytes(m_roomId, sendData,
                                             entry.fileName, entry.mime,
                                             entry.width, entry.height,
                                             entry.replyToEventId);
    } else if (entry.isVideo) {
        opId = m_client->sendVideo(m_roomId, entry.localPath, entry.mime,
                                   caption, entry.width, entry.height,
                                   entry.durationMs, entry.poster,
                                   entry.posterWidth, entry.posterHeight,
                                   entry.replyToEventId);
    } else if (entry.isSvg) {
        opId = 0;
    } else {
        opId = m_client->sendAttachment(m_roomId, sendPath,
                                        entry.mime, caption, entry.width,
                                        entry.height, entry.animated,
                                        entry.durationMs,
                                        entry.replyToEventId);
    }
    if (opId == 0) {
        ImageSendPrep::discard(strippedSub);
        entry.state = QStringLiteral("failed");
        entry.error = tr("The attachment could not be queued.");
    } else {
        entry.state = QStringLiteral("dispatching");
        entry.opId = opId;
        m_imagePrep.track(opId, strippedSub);
    }
    m_attachments->updateEntry(row);
}

void MessageComposer::sendVoiceMessage(const QString &localPath,
                                       const QString &mime,
                                       qreal durationMs,
                                       const QVariantList &waveform)
{
    if (!m_client || m_roomId.isEmpty() || localPath.isEmpty()
        || mime.isEmpty() || durationMs <= 0) {
        QFile::remove(localPath);
        Q_EMIT attachmentRejected(tr("The voice message could not be sent."));
        return;
    }
    const qint64 recordedBytes = QFileInfo(localPath).size();
    if (m_attachments && m_attachments->exceedsUploadLimit(recordedBytes)) {
        QFile::remove(localPath);
        Q_EMIT attachmentRejected(
            tr("The voice message is larger than the server's upload "
               "limit (%1).")
                .arg(AttachmentQueueModel::humanSize(
                    m_attachments->uploadLimit())));
        return;
    }
    QList<int> amplitudes;
    amplitudes.reserve(waveform.size());
    for (const QVariant &value : waveform)
        amplitudes.append(value.toInt());
    const QString targetRoom = m_roomId;
    const QString threadRoot = sendThreadRootId();
    const QString replyTo =
        m_replyingToEventId == threadRoot ? QString() : m_replyingToEventId;
    const quint64 opId = threadRoot.isEmpty()
        ? m_client->sendVoiceMessage(
              targetRoom, localPath, mime, static_cast<qint64>(durationMs),
              amplitudes, m_replyingToEventId)
        : m_client->sendThreadVoiceMessage(
              targetRoom, threadRoot, localPath, mime,
              static_cast<qint64>(durationMs), amplitudes, replyTo);
    if (opId == 0) {
        QFile::remove(localPath);
        Q_EMIT attachmentRejected(tr("The voice message could not be sent."));
        return;
    }
    m_voiceOps.insert(opId, VoiceOp{localPath, targetRoom});
    clearReplyTarget();
}

void MessageComposer::clearReplyTarget()
{
    if (m_replyingToEventId.isEmpty())
        return;
    m_replyingToEventId.clear();
    m_replyingToSender.clear();
    m_replyingToPreview.clear();
    m_replyingToMediaKey.clear();
    Q_EMIT replyStateChanged();
    if (!m_replyThreadRootId.isEmpty()) {
        m_replyThreadRootId.clear();
        Q_EMIT threadStateChanged();
    }
    if (m_drafts && !m_restoringDraft && !m_roomId.isEmpty())
        m_draftDebounce.start();
}

void MessageComposer::setThreadTargetRootId(const QString &rootEventId)
{
    if (m_threadTargetRootId == rootEventId)
        return;
    m_draftDebounce.stop();
    if (!m_editingEventId.isEmpty())
        cancelReplyOrEdit();
    m_draftDebounce.stop();
    saveDraftNow();
    const bool hadReply = !m_replyingToEventId.isEmpty();
    m_replyingToEventId.clear();
    m_replyingToSender.clear();
    m_replyingToPreview.clear();
    m_replyingToMediaKey.clear();
    m_replyThreadRootId.clear();
    m_threadTargetRootId = rootEventId;
    m_slashJoinOp = 0;
    m_text.clear();
    m_mentionRefs.clear();
    restoreDraft();
    setCommandError(QString());
    if (hadReply || !m_replyingToEventId.isEmpty())
        Q_EMIT replyStateChanged();
    Q_EMIT threadStateChanged();
    Q_EMIT commandCompletionsChanged();
    Q_EMIT textChanged();
    Q_EMIT mentionRangesChanged();
    updateCanSend();
    refreshTypingState();
}

QString MessageComposer::sendThreadRootId() const
{
    if (!m_threadRootId.isEmpty())
        return m_threadRootId;
    if (!m_replyingToEventId.isEmpty() && !m_replyThreadRootId.isEmpty())
        return m_replyThreadRootId;
    return m_threadTargetRootId;
}

void MessageComposer::onAttachmentQueueFinished(quint64 opId,
                                                const QString &roomId,
                                                bool ok,
                                                const QString &category)
{
    Q_UNUSED(category);
    m_imagePrep.finished(opId);
    if (const auto voiceIt = m_voiceOps.constFind(opId);
        voiceIt != m_voiceOps.constEnd()) {
        const VoiceOp op = voiceIt.value();
        QFile::remove(op.localPath);
        m_voiceOps.erase(voiceIt);
        if (!ok && op.roomId == m_roomId)
            Q_EMIT attachmentRejected(
                tr("The voice message could not be sent."));
        return;
    }
    if (!roomId.isEmpty() && roomId != m_roomId)
        return;
    auto &entries = m_attachments->entries();
    for (int row = 0; row < entries.size(); ++row) {
        if (entries[row].opId != opId || opId == 0)
            continue;
        if (ok) {
            entries[row].state = QStringLiteral("sent");
            m_attachments->removeAt(row);
        } else {
            entries[row].state = QStringLiteral("failed");
            entries[row].error = tr("Upload failed. Retry or remove.");
            entries[row].opId = 0;
            m_attachments->updateEntry(row);
        }
        pumpAttachments();
        return;
    }
}

void MessageComposer::setText(const QString &t)
{
    if (m_text == t)
        return;
    m_mentionRefs = mention::shiftRefs(m_mentionRefs, m_text, t);
    m_emoticonRefs = shiftEmoticonRefs(m_emoticonRefs, m_text, t);
    m_text = t;
    Q_EMIT textChanged();
    Q_EMIT mentionRangesChanged();
    setCommandError(QString());
    Q_EMIT commandCompletionsChanged();
    updateCanSend();
    refreshTypingState();
    if (m_drafts && !m_restoringDraft && !m_roomId.isEmpty())
        m_draftDebounce.start();
}

void MessageComposer::setRoomId(const QString &r)
{
    if (m_roomId == r)
        return;
    m_draftDebounce.stop();
    saveDraftNow();
    if (!m_roomId.isEmpty()) stopTyping();
    cancelReplyOrEdit();
    m_vim->enterInsert();
    m_threadVim->enterInsert();
    if (!m_threadTargetRootId.isEmpty()) {
        m_threadTargetRootId.clear();
        Q_EMIT threadStateChanged();
    }
    m_draftDebounce.stop();
    if (m_client && !m_roomId.isEmpty()) {
        auto &pending = m_attachments->entries();
        for (int row = 0; row < pending.size(); ++row) {
            if (pending[row].state == QLatin1String("queued")
                && pending[row].sendRequested)
                dispatchAttachment(row);
        }
    }
    m_attachments->clearAll();
    if (!m_roomId.isEmpty() && !m_emoticonRefs.isEmpty())
        m_emoticonRefsByRoom.insert(m_roomId, m_emoticonRefs);
    m_emoticonRefs.clear();
    m_roomId = r;
    m_slashJoinOp = 0;
    m_text.clear();
    m_mentionRefs.clear();
    restoreDraft();
    setCommandError(QString());
    Q_EMIT commandCompletionsChanged();
    Q_EMIT textChanged();
    Q_EMIT mentionRangesChanged();
    Q_EMIT roomIdChanged();
    updateCanSend();
}

bool MessageComposer::canSend() const
{
    return m_canSend;
}

void MessageComposer::send()
{
    sendInternal(true);
}

void MessageComposer::sendBypassingCommands()
{
    sendInternal(false);
}

void MessageComposer::sendInternal(bool allowCommands)
{
    if (!m_canSend || !m_client) return;
    if (!m_editingEventId.isEmpty() && m_text == m_editSeedText) {
        finishSuccessfulSend();
        return;
    }
    const mention::Expansion expansion = mention::expand(m_text, m_mentionRefs);
    QString body = expansion.body.trimmed();
    if (m_emojiAutoConvert)
        body = convertUnicodeShortcodesForSend(body);
    const QStringList mentionIds = expansion.userIds;

    if (allowCommands && m_editingEventId.isEmpty()) {
        const SlashCommands::Parse parsed = SlashCommands::parse(body);
        switch (parsed.kind) {
        case SlashCommands::Parse::NotACommand:
            break;
        case SlashCommands::Parse::Escaped:
            body = parsed.literalText.trimmed();
            break;
        case SlashCommands::Parse::Unknown:
            break;
        case SlashCommands::Parse::Known:
            if (executeCommand(parsed, mentionIds))
                return;
            break;
        }
    }

    if (refuseIfTooLarge(body, QString()))
        return;
    if (!m_editingEventId.isEmpty()) {
        if (body.isEmpty()) return;
        QString editBody = body;
        QVariantMap editSpec;
        applyOutgoingEmojiWith(editBody, editSpec, selectedEmoticonMap());
        if (editSpec.isEmpty())
            m_client->editMessage(editTargetTimelineId(), m_editingEventId, body,
                                  mentionIds);
        else
            m_client->editMessage(editTargetTimelineId(), m_editingEventId,
                                  editBody, mentionIds, editSpec);
    } else {
        const bool captioned = takeTextAsCaption(body, mentionIds);
        dispatchAttachments();
        if (!body.isEmpty() && !captioned)
            sendComposed(body, mentionIds, QVariantMap());
        Q_EMIT ownMessageSent();
    }
    finishSuccessfulSend();
}

void MessageComposer::sendPrepared(const QString &body, const QString &html,
                                   const QStringList &mentionUserIds)
{
    if (!m_client || m_roomId.isEmpty() || body.trimmed().isEmpty())
        return;
    if (!m_editingEventId.isEmpty()
        && m_text.trimmed() == m_editSeedText.trimmed()) {
        finishSuccessfulSend();
        return;
    }
    if (refuseIfTooLarge(body, html))
        return;
    const QVariantMap spec = html.isEmpty()
        ? QVariantMap{ { QStringLiteral("format"), QStringLiteral("plain") } }
        : QVariantMap{ { QStringLiteral("format"), QStringLiteral("html") },
                       { QStringLiteral("html"), html } };
    if (!m_editingEventId.isEmpty()) {
        m_client->editMessage(editTargetTimelineId(), m_editingEventId, body,
                              mentionUserIds, spec);
    } else {
        dispatchAttachments();
        sendComposed(body, mentionUserIds, spec);
        Q_EMIT ownMessageSent();
    }
    finishSuccessfulSend();
}

void MessageComposer::setEmoticonResolver(
    std::function<QString(const QString &)> resolve)
{
    m_emoticonResolver = std::move(resolve);
}

namespace {
QVariantMap resolveShortcodes(
    const QString &body,
    const std::function<QString(const QString &)> &resolve)
{
    QVariantMap found;
    if (!resolve || !body.contains(QLatin1Char(':')))
        return found;
    static const QRegularExpression re(
        QStringLiteral(":([a-zA-Z0-9_+-]{1,100}):"));
    auto it = re.globalMatch(body);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString code = m.captured(0);
        if (found.contains(code))
            continue;
        const QString mxc = resolve(m.captured(1));
        if (mxc.startsWith(QLatin1String("mxc://")))
            found.insert(code, mxc);
    }
    return found;
}
bool insideCode(const QString &text, int cursor);
}

QVariantMap MessageComposer::composedMessage() const
{
    const mention::Expansion expansion = mention::expand(m_text, m_mentionRefs);
    QString body = expansion.body.trimmed();
    if (m_emojiAutoConvert)
        body = convertUnicodeShortcodesForSend(body);
    return QVariantMap{
        { QStringLiteral("body"), body },
        { QStringLiteral("mentionIds"), expansion.userIds },
        { QStringLiteral("bodySpec"), QVariantMap() },
        { QStringLiteral("roomId"), m_roomId },
        { QStringLiteral("threadRootId"), m_threadRootId },
        { QStringLiteral("replyToEventId"), m_replyingToEventId },
    };
}

QString MessageComposer::convertUnicodeShortcodesForSend(const QString &text) const
{
    if (!m_unicodeShortcodeResolver || !text.contains(QLatin1Char(':')))
        return text;
    static const QRegularExpression re(
        QStringLiteral(":([a-zA-Z0-9_+-]{1,100}):"));
    QString out;
    out.reserve(text.size());
    int last = 0;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        const int start = m.capturedStart(0);
        if (start < last)
            continue;
        out += text.mid(last, start - last);
        const QString code = m.captured(1);
        QString emoji;
        const bool shadowedByCustomPack = customShortcodeExists(code);
        if (!shadowedByCustomPack && !insideCode(text, start))
            emoji = m_unicodeShortcodeResolver(code);
        if (!emoji.isEmpty()) {
            out += emoji;
            if (m_unicodeEmojiUseRecorder)
                m_unicodeEmojiUseRecorder(emoji);
        } else {
            out += m.captured(0);
        }
        last = m.capturedEnd(0);
    }
    out += text.mid(last);
    return out;
}

void MessageComposer::sendComposed(const QString &body,
                                   const QStringList &mentionIds,
                                   const QVariantMap &bodySpecIn)
{
    if (!m_client || body.isEmpty())
        return;
    QString outBody = body;
    QVariantMap bodySpec = bodySpecIn;
    applyOutgoingEmojiWith(outBody, bodySpec, selectedEmoticonMap());
    if (m_customEmojiUseRecorder) {
        QSet<QString> recorded;
        for (const EmoticonRef &ref : std::as_const(m_emoticonRefs)) {
            if (recorded.contains(ref.url) || !outBody.contains(ref.displayText))
                continue;
            recorded.insert(ref.url);
            m_customEmojiUseRecorder(QVariantMap{
                { QStringLiteral("url"), ref.url },
                { QStringLiteral("key"), ref.key },
                { QStringLiteral("shortcode"), ref.key },
                { QStringLiteral("body"), ref.body },
                { QStringLiteral("isEmoticon"), true },
            });
        }
    }
    if (!m_threadRootId.isEmpty()) {
        m_client->sendThreadReplyTo(m_roomId, m_threadRootId, QString(), outBody,
                                    mentionIds, bodySpec);
    } else if (const QString root = sendThreadRootId(); !root.isEmpty()) {
        const QString inReplyTo =
            m_replyingToEventId == root ? QString() : m_replyingToEventId;
        m_client->sendThreadReplyTo(m_roomId, root, inReplyTo, outBody,
                                    mentionIds, bodySpec);
    } else if (!m_replyingToEventId.isEmpty()) {
        m_client->sendReply(m_roomId, m_replyingToEventId, outBody, mentionIds,
                            bodySpec);
    } else {
        m_client->sendTextMessage(m_roomId, outBody, mentionIds, bodySpec);
    }
}

// The size the event's content would serialize to, measured on real JSON and
// not on the character count (multi-byte text and escapes both widen it). An
// edit carries the text twice, as the fallback body and inside m.new_content.
qsizetype MessageComposer::serializedContentBytes(const QString &body,
                                                  const QString &html,
                                                  bool edit)
{
    QJsonObject content{ { QStringLiteral("msgtype"), QStringLiteral("m.text") },
                         { QStringLiteral("body"), body } };
    if (!html.isEmpty()) {
        content.insert(QStringLiteral("format"),
                       QStringLiteral("org.matrix.custom.html"));
        content.insert(QStringLiteral("formatted_body"), html);
    }
    const qsizetype once =
        QJsonDocument(content).toJson(QJsonDocument::Compact).size();
    return edit ? once * 2 : once;
}

// Refuses a message the server would reject, before anything is sent or
// cleared: the text stays in the composer with the reason beside it, where a
// failed echo would only offer Retry and Cancel and Cancel loses the text.
bool MessageComposer::refuseIfTooLarge(const QString &body, const QString &html)
{
    const qsizetype bytes = serializedContentBytes(
        body, html, !m_editingEventId.isEmpty());
    if (bytes <= kMaxEventContentBytes)
        return false;
    setCommandError(
        tr("This message is too long to send (%1 KB; the limit is about %2 KB). "
           "Shorten it or split it into several messages.")
            .arg((bytes + 1023) / 1024)
            .arg(kMaxEventContentBytes / 1024));
    return true;
}

void MessageComposer::finishSuccessfulSend()
{
    const bool wasEditing = !m_editingEventId.isEmpty();
    stopTyping();
    cancelReplyOrEdit();
    if (!wasEditing)
        clear();
}

bool MessageComposer::executeCommand(const SlashCommands::Parse &parsed,
                                     const QStringList &mentionIds)
{
    SlashCommands::Actions actions;
    actions.send = [this](const QString &body, const QStringList &ids,
                          const QVariantMap &spec) {
        sendComposed(body, ids, spec);
        Q_EMIT ownMessageSent();
    };
    actions.join = [this](const QString &target) {
        m_slashJoinOp = m_client->joinRoomByIdOrAlias(target, QStringList());
        if (m_slashJoinOp == 0) {
            setCommandError(joinerror::describe(
                m_client->supportsJoinByAddress() ? QStringLiteral("network")
                                                  : QStringLiteral("unsupported")));
        }
    };
    actions.invite = [this](const QString &userId) {
        m_client->inviteUsers(m_roomId, { userId });
    };
    actions.kick = [this](const QString &userId, const QString &reason) {
        m_client->kickUser(m_roomId, userId, reason);
    };
    actions.ban = [this](const QString &userId, const QString &reason) {
        m_client->banUser(m_roomId, userId, reason);
    };
    actions.unban = [this](const QString &userId, const QString &reason) {
        m_client->unbanUser(m_roomId, userId, reason);
    };
    actions.setTopic = [this](const QString &topic) {
        m_client->setRoomTopic(m_roomId, topic);
    };
    actions.setRoomName = [this](const QString &name) {
        m_client->setRoomName(m_roomId, name);
    };
    actions.setDisplayName = [this](const QString &name) {
        Q_EMIT displayNameChangeRequested(name);
    };
    actions.toggleComposerMode = [this] {
        Q_EMIT composerModeToggleRequested();
        clear();
    };
    actions.clearComposer = [this] {
        if (m_attachments)
            m_attachments->clearAll();
        stopTyping();
        cancelReplyOrEdit();
        clear();
    };

    const SlashCommands::Outcome outcome =
        SlashCommands::execute(parsed, mentionIds, actions);
    if (!outcome.error.isEmpty()) {
        setCommandError(outcome.error,true);
        return true;
    }
    if (outcome.retireDraft)
        finishSuccessfulSend();
    return true;
}

QVariantList MessageComposer::commandCompletions() const
{
    QVariantList out;
    if (!m_editingEventId.isEmpty())
        return out;
    const QList<SlashCommands::Command> matches =
        SlashCommands::completions(m_text);
    for (const SlashCommands::Command &command : matches) {
        const bool enabled = command.permissionKey.isEmpty()
            || m_commandPermissions.value(command.permissionKey, true).toBool();
        out.append(QVariantMap{
            { QStringLiteral("name"), command.name },
            { QStringLiteral("argsHint"), command.argsHint },
            { QStringLiteral("description"), command.description },
            { QStringLiteral("enabled"), enabled },
            { QStringLiteral("requiresArgs"), command.requiresArgs },
            { QStringLiteral("exact"),
              m_text.mid(1).compare(command.name, Qt::CaseInsensitive) == 0 },
        });
    }
    return out;
}

void MessageComposer::setCommandPermissions(const QVariantMap &permissions)
{
    if (m_commandPermissions == permissions)
        return;
    m_commandPermissions = permissions;
    Q_EMIT commandPermissionsChanged();
    Q_EMIT commandCompletionsChanged();
}

void MessageComposer::setCommandError(const QString &error, bool bypassable)
{
    if (m_commandError == error && m_commandErrorBypassable == bypassable)
        return;
    m_commandError = error;
    m_commandErrorBypassable = bypassable;
    Q_EMIT commandErrorChanged();
}

void MessageComposer::setEmoticonSearch(
    std::function<QVariantList(const QString &, int)> search)
{
    m_emoticonSearch = std::move(search);
}

void MessageComposer::setUnicodeEmojiSearch(
    std::function<QVariantList(const QString &, int)> search)
{
    m_unicodeEmojiSearch = std::move(search);
}

void MessageComposer::setUnicodeEmojiUseRecorder(
    std::function<void(const QString &)> record)
{
    m_unicodeEmojiUseRecorder = std::move(record);
}

void MessageComposer::setUnicodeShortcodeResolver(
    std::function<QString(const QString &)> resolve)
{
    m_unicodeShortcodeResolver = std::move(resolve);
}

void MessageComposer::setEmojiAutoConvertEnabled(bool enabled)
{
    m_emojiAutoConvert = enabled;
}

namespace {
QPair<int, int> emojiTokenAt(const QString &text, int cursor)
{
    if (cursor < 0 || cursor > text.size())
        return { -1, 0 };
    int i = cursor;
    while (i > 0) {
        const QChar c = text.at(i - 1);
        if (c == QLatin1Char(':'))
            break;
        if (!(c.isLetterOrNumber() || c == QLatin1Char('_')
              || c == QLatin1Char('-') || c == QLatin1Char('+')))
            return { -1, 0 };
        --i;
    }
    if (i == 0 || text.at(i - 1) != QLatin1Char(':'))
        return { -1, 0 };
    const int colon = i - 1;
    if (colon > 0) {
        const QChar before = text.at(colon - 1);
        if (!(before.isSpace() || before == QLatin1Char('(')
              || before == QLatin1Char('[') || before == QLatin1Char('{')))
            return { -1, 0 };
    }
    return { colon, cursor - colon };
}

bool insideCode(const QString &text, int cursor)
{
    int ticks = 0;
    for (int i = 0; i < cursor && i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('`'))
            ++ticks;
    }
    return ticks % 2 == 1;
}

QPair<int, int> completeShortcodeEndingAt(const QString &text, int end)
{
    if (end < 3 || end > text.size() || text.at(end - 1) != QLatin1Char(':'))
        return { -1, 0 };
    const int closeColon = end - 1;
    int i = closeColon;
    while (i > 0) {
        const QChar c = text.at(i - 1);
        if (c == QLatin1Char(':'))
            break;
        if (!(c.isLetterOrNumber() || c == QLatin1Char('_')
              || c == QLatin1Char('-') || c == QLatin1Char('+')))
            return { -1, 0 };
        --i;
    }
    if (i == 0 || i == closeColon || text.at(i - 1) != QLatin1Char(':'))
        return { -1, 0 };
    const int openColon = i - 1;
    if (openColon > 0) {
        const QChar before = text.at(openColon - 1);
        if (!(before.isSpace() || before == QLatin1Char('(')
              || before == QLatin1Char('[') || before == QLatin1Char('{')))
            return { -1, 0 };
    }
    return { openColon, end - openColon };
}
}

namespace {
constexpr int kUnicodeMinQueryChars = 2;
constexpr int kMaxEmojiCompletions = 12;
}

QVariantList MessageComposer::emojiCompletionsAt(int cursorPos) const
{
    if (!m_editingEventId.isEmpty())
        return {};
    return emojiCompletionsForText(m_text, cursorPos);
}

QVariantList MessageComposer::emojiCompletionsForText(const QString &text,
                                                      int cursorPos) const
{
    QVariantList out;
    if (!m_emoticonSearch && !m_unicodeEmojiSearch)
        return out;
    const auto token = emojiTokenAt(text, cursorPos);
    if (token.first < 0)
        return out;
    const QString prefix = text.mid(token.first + 1, token.second - 1);
    if (prefix.isEmpty() || insideCode(text, cursorPos))
        return out;
    if (m_emoticonSearch)
        out = m_emoticonSearch(prefix, kMaxEmojiCompletions);
    if (m_unicodeEmojiSearch && prefix.size() >= kUnicodeMinQueryChars
        && out.size() < kMaxEmojiCompletions) {
        const QVariantList unicodeMatches =
            m_unicodeEmojiSearch(prefix, kMaxEmojiCompletions - out.size());
        for (const QVariant &row : unicodeMatches)
            out.append(row);
    }
    return out;
}

QVariantMap MessageComposer::emojiCompletionEdit(const QString &text,
                                                int cursorPos,
                                                const QVariantMap &entry)
{
    const auto token = emojiTokenAt(text, cursorPos);
    const bool unicode =
        entry.value(QStringLiteral("kind")).toString() == QLatin1String("unicode");
    const QString value = unicode
        ? entry.value(QStringLiteral("emoji")).toString()
        : entry.value(QStringLiteral("shortcode")).toString();
    if (token.first < 0 || value.isEmpty())
        return {};
    const QString replacement = unicode
        ? value + QStringLiteral(" ")
        : QLatin1Char(':') + value + QStringLiteral(": ");
    if (unicode && m_unicodeEmojiUseRecorder)
        m_unicodeEmojiUseRecorder(value);
    const QString url = entry.value(QStringLiteral("url")).toString();
    if (!unicode && url.startsWith(QLatin1String("mxc://"))) {
        m_threadEmoticonChoices.insert(
            QLatin1Char(':') + value + QLatin1Char(':'),
            QVariantMap{ { QStringLiteral("url"), url },
                         { QStringLiteral("body"),
                           entry.value(QStringLiteral("body")).toString() } });
    }
    return QVariantMap{
        { QStringLiteral("start"), token.first },
        { QStringLiteral("length"), token.second },
        { QStringLiteral("replacement"), replacement },
        { QStringLiteral("cursor"), token.first + replacement.length() },
    };
}

QVariantMap MessageComposer::shortcodeAutoConvertEdit(const QString &text,
                                                     int cursorPos)
{
    if (!m_emojiAutoConvert || !m_unicodeShortcodeResolver)
        return {};
    if (cursorPos < 1 || cursorPos > text.size())
        return {};
    if (text.at(cursorPos - 1) != QLatin1Char(' '))
        return {};
    if (insideCode(text, cursorPos))
        return {};
    const auto token = completeShortcodeEndingAt(text, cursorPos - 1);
    if (token.first < 0)
        return {};
    const QString code = text.mid(token.first + 1, token.second - 2);
    if (customShortcodeExists(code))
        return {};
    const QString emoji = m_unicodeShortcodeResolver(code);
    if (emoji.isEmpty())
        return {};
    if (m_unicodeEmojiUseRecorder)
        m_unicodeEmojiUseRecorder(emoji);
    return QVariantMap{
        { QStringLiteral("start"), token.first },
        { QStringLiteral("length"), token.second },
        { QStringLiteral("replacement"), emoji },
        { QStringLiteral("cursor"), token.first + emoji.length() + 1 },
    };
}

void MessageComposer::applyOutgoingEmoji(QString &body,
                                         QVariantMap &bodySpec) const
{
    applyOutgoingEmojiWith(body, bodySpec, m_threadEmoticonChoices);
    m_threadEmoticonChoices.clear();
}

void MessageComposer::applyOutgoingEmojiWith(QString &body,
                                             QVariantMap &bodySpec,
                                             const QVariantMap &selected) const
{
    if (m_emojiAutoConvert && !bodySpec.contains(QStringLiteral("format")))
        body = convertUnicodeShortcodesForSend(body);
    QVariantMap emoticons = resolveShortcodes(body, m_emoticonResolver);
    for (auto it = selected.cbegin(); it != selected.cend(); ++it) {
        if (body.contains(it.key()))
            emoticons.insert(it.key(), it.value());
    }
    if (emoticons.isEmpty())
        return;
    if (!bodySpec.contains(QStringLiteral("format")))
        bodySpec.insert(QStringLiteral("format"), QStringLiteral("markdown"));
    bodySpec.insert(QStringLiteral("emoticons"), emoticons);
}

int MessageComposer::acceptEmojiCompletionAt(int cursorPos,
                                             const QString &shortcode)
{
    const auto token = emojiTokenAt(m_text, cursorPos);
    if (token.first < 0 || shortcode.isEmpty())
        return -1;
    const QString replacement =
        QLatin1Char(':') + shortcode + QStringLiteral(": ");
    QString next = m_text;
    next.replace(token.first, token.second, replacement);
    setText(next);
    return token.first + replacement.length();
}

int MessageComposer::acceptEmoticonCompletionAt(int cursorPos,
                                                const QVariantMap &entry)
{
    const QString key = entry.value(QStringLiteral("key"),
                                    entry.value(QStringLiteral("shortcode")))
                            .toString();
    const QString url = entry.value(QStringLiteral("url")).toString();
    const auto token = emojiTokenAt(m_text, cursorPos);
    if (token.first < 0 || key.isEmpty() || !url.startsWith(QLatin1String("mxc://")))
        return -1;
    const QString code = QLatin1Char(':') + key + QLatin1Char(':');
    QString next = m_text;
    next.replace(token.first, token.second, code + QLatin1Char(' '));
    setText(next);
    EmoticonRef ref;
    ref.displayText = code;
    ref.url = url;
    ref.body = entry.value(QStringLiteral("body")).toString();
    ref.key = key;
    ref.start = token.first;
    ref.length = code.length();
    for (EmoticonRef &existing : m_emoticonRefs) {
        if (existing.displayText == code) {
            existing.url = ref.url;
            existing.body = ref.body;
        }
    }
    m_emoticonRefs.append(ref);
    return token.first + code.length() + 1;
}

bool MessageComposer::bindEmoticonAt(int start, const QVariantMap &entry)
{
    const QString key = entry.value(QStringLiteral("key"),
                                    entry.value(QStringLiteral("shortcode")))
                            .toString();
    const QString url = entry.value(QStringLiteral("url")).toString();
    if (key.isEmpty() || !url.startsWith(QLatin1String("mxc://")))
        return false;
    const QString code = QLatin1Char(':') + key + QLatin1Char(':');
    if (start < 0 || m_text.mid(start, code.length()) != code)
        return false;
    for (EmoticonRef &existing : m_emoticonRefs) {
        if (existing.displayText == code) {
            existing.url = url;
            existing.body = entry.value(QStringLiteral("body")).toString();
        }
    }
    EmoticonRef ref;
    ref.displayText = code;
    ref.url = url;
    ref.body = entry.value(QStringLiteral("body")).toString();
    ref.key = key;
    ref.start = start;
    ref.length = code.length();
    m_emoticonRefs.append(ref);
    return true;
}

bool MessageComposer::customShortcodeExists(const QString &code) const
{
    const QString text = QLatin1Char(':') + code + QLatin1Char(':');
    for (const EmoticonRef &ref : m_emoticonRefs) {
        if (ref.displayText == text)
            return true;
    }
    if (m_emoticonResolver && !m_emoticonResolver(code).isEmpty())
        return true;
    if (m_emoticonSearch) {
        for (const QVariant &row : m_emoticonSearch(code, kMaxEmojiCompletions)) {
            const QVariantMap map = row.toMap();
            if (map.value(QStringLiteral("kind")).toString() == QLatin1String("unicode"))
                continue;
            const QString key = map.value(QStringLiteral("key"),
                                          map.value(QStringLiteral("shortcode"))).toString();
            if (key == code)
                return true;
        }
    }
    return false;
}

QVariantList MessageComposer::selectedEmoticons() const
{
    QVariantList out;
    for (const EmoticonRef &ref : m_emoticonRefs) {
        out.append(QVariantMap{
            { QStringLiteral("text"), ref.displayText },
            { QStringLiteral("url"), ref.url },
            { QStringLiteral("body"), ref.body },
            { QStringLiteral("key"), ref.key },
            { QStringLiteral("start"), ref.start },
            { QStringLiteral("length"), ref.length },
        });
    }
    return out;
}

QVariantMap MessageComposer::selectedEmoticonMap() const
{
    QVariantMap out;
    for (const EmoticonRef &ref : m_emoticonRefs) {
        out.insert(ref.displayText, QVariantMap{
            { QStringLiteral("url"), ref.url },
            { QStringLiteral("body"), ref.body },
        });
    }
    return out;
}

QList<MessageComposer::EmoticonRef>
MessageComposer::emoticonRefsFromHtml(const QString &text,
                                      const QString &sanitizedHtml)
{
    QList<EmoticonRef> refs;
    if (sanitizedHtml.isEmpty() || text.isEmpty())
        return refs;
    static const QRegularExpression img(
        QStringLiteral("<img data-mx-emoticon src=\"(mxc://[^\"]+)\"([^>]*)>"));
    static const QRegularExpression altAttr(QStringLiteral(" alt=\"([^\"]*)\""));
    static const QRegularExpression titleAttr(QStringLiteral(" title=\"([^\"]*)\""));
    const auto unescape = [](QString v) {
        v.replace(QLatin1String("&quot;"), QLatin1String("\""));
        v.replace(QLatin1String("&lt;"), QLatin1String("<"));
        v.replace(QLatin1String("&gt;"), QLatin1String(">"));
        v.replace(QLatin1String("&#39;"), QLatin1String("'"));
        v.replace(QLatin1String("&amp;"), QLatin1String("&"));
        return v;
    };
    qsizetype from = 0;
    auto it = img.globalMatch(sanitizedHtml);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString attrs = m.captured(2);
        QString key = unescape(titleAttr.match(attrs).captured(1));
        const QString alt = unescape(altAttr.match(attrs).captured(1));
        if (key.isEmpty())
            key = alt;
        if (key.size() > 2 && key.startsWith(QLatin1Char(':'))
            && key.endsWith(QLatin1Char(':')))
            key = key.mid(1, key.size() - 2);
        if (key.isEmpty())
            continue;
        const QString code = QLatin1Char(':') + key + QLatin1Char(':');
        const qsizetype at = text.indexOf(code, from);
        if (at < 0)
            continue;
        EmoticonRef ref;
        ref.displayText = code;
        ref.url = unescape(m.captured(1));
        ref.body = (alt == key) ? QString() : alt;
        ref.key = key;
        ref.start = int(at);
        ref.length = int(code.size());
        refs.append(ref);
        from = at + code.size();
    }
    return refs;
}

QList<MessageComposer::EmoticonRef>
MessageComposer::shiftEmoticonRefs(const QList<EmoticonRef> &refs,
                                   const QString &oldText,
                                   const QString &newText)
{
    if (refs.isEmpty())
        return {};
    QList<mention::MentionRef> ranges;
    ranges.reserve(refs.size());
    for (int i = 0; i < refs.size(); ++i) {
        mention::MentionRef r;
        r.userId = QString::number(i);
        r.displayText = refs.at(i).displayText;
        r.start = refs.at(i).start;
        r.length = refs.at(i).length;
        ranges.append(r);
    }
    QList<EmoticonRef> out;
    for (const mention::MentionRef &moved :
         mention::shiftRefs(ranges, oldText, newText)) {
        EmoticonRef ref = refs.at(moved.userId.toInt());
        ref.start = moved.start;
        out.append(ref);
    }
    return out;
}

int MessageComposer::acceptUnicodeEmojiCompletionAt(int cursorPos,
                                                    const QString &emoji)
{
    const auto token = emojiTokenAt(m_text, cursorPos);
    if (token.first < 0 || emoji.isEmpty())
        return -1;
    const QString replacement = emoji + QStringLiteral(" ");
    QString next = m_text;
    next.replace(token.first, token.second, replacement);
    setText(next);
    if (m_unicodeEmojiUseRecorder)
        m_unicodeEmojiUseRecorder(emoji);
    return token.first + replacement.length();
}

int MessageComposer::maybeAutoConvertShortcodeBeforeCursor(int cursorPos)
{
    if (!m_emojiAutoConvert || !m_unicodeShortcodeResolver)
        return -1;
    if (cursorPos < 1 || cursorPos > m_text.size())
        return -1;
    if (m_text.at(cursorPos - 1) != QLatin1Char(' '))
        return -1;
    if (insideCode(m_text, cursorPos))
        return -1;
    const auto token = completeShortcodeEndingAt(m_text, cursorPos - 1);
    if (token.first < 0)
        return -1;
    const QString code = m_text.mid(token.first + 1, token.second - 2);
    if (customShortcodeExists(code))
        return -1;
    const QString emoji = m_unicodeShortcodeResolver(code);
    if (emoji.isEmpty())
        return -1;
    QString next = m_text;
    next.replace(token.first, token.second, emoji);
    setText(next);
    if (m_unicodeEmojiUseRecorder)
        m_unicodeEmojiUseRecorder(emoji);
    return token.first + emoji.length() + 1;
}

int MessageComposer::acceptCommandCompletion(const QString &name)
{
    if (!SlashCommands::find(name))
        return -1;
    const QString newText = QLatin1Char('/') + name + QLatin1Char(' ');
    setText(newText);
    return newText.length();
}

void MessageComposer::clear()
{
    m_draftDebounce.stop();
    if (m_drafts && !m_roomId.isEmpty())
        m_drafts->clear(draftKey());
    m_mentionRefs.clear();
    m_emoticonRefs.clear();
    m_emoticonRefsByRoom.remove(m_roomId);
    Q_EMIT mentionRangesChanged();
    if (m_text.isEmpty()) return;
    m_text.clear();
    Q_EMIT textChanged();
    updateCanSend();
    refreshTypingState();
}

QVariantMap MessageComposer::mentionTokenAt(const QString &text,
                                            int cursorPos) const
{
    const mention::Token tok = mention::activeToken(text, cursorPos);
    QVariantMap out;
    if (!tok.active) {
        out.insert(QStringLiteral("active"), false);
        return out;
    }
    const int tokEnd = qBound(0, cursorPos, text.length());
    for (const mention::MentionRef &ref : m_mentionRefs) {
        const int rs = ref.start;
        const int re = ref.start + ref.length;
        if (tok.start < re && rs < tokEnd) {
            out.insert(QStringLiteral("active"), false);
            return out;
        }
    }
    out.insert(QStringLiteral("active"), true);
    out.insert(QStringLiteral("start"), tok.start);
    out.insert(QStringLiteral("query"), tok.query);
    return out;
}

int MessageComposer::insertMention(const QString &userId,
                                   const QString &displayName, int tokenStart,
                                   int cursorPos)
{
    if (userId.isEmpty())
        return cursorPos;
    const mention::InsertResult res = mention::buildInsertion(
        m_text, tokenStart, cursorPos, userId, displayName);
    m_mentionRefs = mention::shiftRefs(m_mentionRefs, m_text, res.text);
    m_mentionRefs.append(res.ref);
    m_text = res.text;
    Q_EMIT textChanged();
    Q_EMIT mentionRangesChanged();
    updateCanSend();
    refreshTypingState();
    return res.cursorPos;
}

void MessageComposer::beginReply(const QString &eventId,
                                  const QString &sender,
                                  const QString &preview,
                                  const QString &mediaKey,
                                  const QString &threadRootId)
{
    m_editingEventId.clear();
    m_editSeedText.clear();
    m_editSeedHtml.clear();
    m_editPreview.clear();
    m_editingTimelineId.clear();
    Q_EMIT editStateChanged();
    m_replyingToEventId = eventId;
    m_replyingToSender  = sender;
    m_replyingToPreview = preview;
    m_replyingToMediaKey = mediaKey;
    Q_EMIT replyStateChanged();
    if (m_replyThreadRootId != threadRootId) {
        m_replyThreadRootId = threadRootId;
        Q_EMIT threadStateChanged();
    }
    if (m_drafts && !m_restoringDraft && !m_roomId.isEmpty())
        m_draftDebounce.start();
}

void MessageComposer::beginEdit(const QString &eventId,
                                 const QString &currentBody,
                                 const QString &sanitizedHtml,
                                 const QString &timelineId)
{
    const bool wasEditing = !m_editingEventId.isEmpty();
    if (!wasEditing) {
        m_draftDebounce.stop();
        saveDraftNow();
    }
    m_editingTimelineId = timelineId;
    m_replyingToEventId.clear();
    m_replyingToSender.clear();
    m_replyingToPreview.clear();
    m_replyingToMediaKey.clear();
    m_replyThreadRootId.clear();
    Q_EMIT replyStateChanged();
    m_threadRootId.clear();
    m_threadPreview.clear();
    Q_EMIT threadStateChanged();
    m_editingEventId = eventId;
    const mention::Recovery recovered = mention::recoverFromBody(currentBody);
    const QString formatted = RichComposition::markdownFromSanitizedHtml(
        sanitizedHtml, recovered.text);
    m_text = formatted.isEmpty() ? recovered.text : formatted;
    m_editSeedText = m_text;
    m_editSeedHtml = sanitizedHtml;
    m_editPreview = sanitizedHtml.isEmpty()
        ? recovered.text
        : RichComposition::plainTextOfSanitizedHtml(sanitizedHtml);
    m_mentionRefs = recovered.refs;
    if (m_mentionRefs.isEmpty() && !sanitizedHtml.isEmpty()) {
        m_mentionRefs =
            mention::refsFromSanitizedHtml(m_text, sanitizedHtml);
    }
    if (!wasEditing && !m_roomId.isEmpty() && !m_emoticonRefs.isEmpty())
        m_emoticonRefsByRoom.insert(m_roomId, m_emoticonRefs);
    m_emoticonRefs = emoticonRefsFromHtml(m_text, sanitizedHtml);
    Q_EMIT editStateChanged();
    Q_EMIT mentionRangesChanged();
    Q_EMIT textChanged();
    updateCanSend();
}

void MessageComposer::beginThreadReply(const QString &rootEventId,
                                        const QString &preview)
{
    m_editingEventId.clear();
    m_editSeedText.clear();
    m_editSeedHtml.clear();
    m_editPreview.clear();
    m_editingTimelineId.clear();
    Q_EMIT editStateChanged();
    m_replyingToEventId.clear();
    m_replyingToSender.clear();
    m_replyingToPreview.clear();
    m_replyingToMediaKey.clear();
    m_replyThreadRootId.clear();
    Q_EMIT replyStateChanged();
    m_threadRootId  = rootEventId;
    m_threadPreview = preview;
    Q_EMIT threadStateChanged();
}

void MessageComposer::cancelReplyOrEdit()
{
    const bool wasReplying = !m_replyingToEventId.isEmpty();
    const bool wasEditing  = !m_editingEventId.isEmpty();
    const bool wasInThread = !m_threadRootId.isEmpty()
        || !m_replyThreadRootId.isEmpty();
    m_replyThreadRootId.clear();
    m_replyingToEventId.clear();
    m_replyingToSender.clear();
    m_replyingToPreview.clear();
    m_replyingToMediaKey.clear();
    m_editingEventId.clear();
    m_editSeedText.clear();
    m_editSeedHtml.clear();
    m_editPreview.clear();
    m_editingTimelineId.clear();
    m_threadRootId.clear();
    m_threadPreview.clear();
    m_mentionRefs.clear();
    Q_EMIT mentionRangesChanged();
    if (wasEditing) {
        m_text.clear();
        m_mentionRefs.clear();
        restoreDraft();
        Q_EMIT textChanged();
        Q_EMIT mentionRangesChanged();
        updateCanSend();
    }
    if (wasReplying) Q_EMIT replyStateChanged();
    if (wasEditing)  Q_EMIT editStateChanged();
    if (wasInThread) Q_EMIT threadStateChanged();
    if ((wasReplying || wasEditing) && m_drafts && !m_restoringDraft
        && !m_roomId.isEmpty()) {
        m_draftDebounce.start();
    }
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

bool MessageComposer::canRemoveEdits() const
{
    return m_client && m_client->supportsRemovingEdits();
}

void MessageComposer::removeEdits(const QString &eventId)
{
    if (!m_client || m_roomId.isEmpty() || eventId.isEmpty()) return;
    if (!m_client->supportsRemovingEdits()) return;
    m_client->removeMessageEdits(m_roomId, eventId);
}

bool MessageComposer::pollsSupported() const
{
    return m_client && m_client->supportsPolls();
}

void MessageComposer::votePoll(const QString &pollEventId,
                               const QStringList &answerIds,
                               const QString &threadRootId)
{
    if (!m_client || m_roomId.isEmpty() || pollEventId.isEmpty()) return;
    m_client->sendPollResponse(m_roomId, threadRootId, pollEventId, answerIds);
}

void MessageComposer::endPoll(const QString &pollEventId,
                              const QString &threadRootId)
{
    if (!m_client || m_roomId.isEmpty() || pollEventId.isEmpty()) return;
    m_client->endPoll(m_roomId, threadRootId, pollEventId);
}

void MessageComposer::createPoll(const QString &question,
                                 const QStringList &answers,
                                 bool undisclosed,
                                 int maxSelections)
{
    if (!m_client || m_roomId.isEmpty()) return;
    m_client->createPoll(m_roomId, sendThreadRootId(), question, answers,
                         undisclosed, maxSelections);
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
    const bool wantTyping = m_typingEnabled && !m_text.trimmed().isEmpty();
    if (wantTyping && !m_typingActive) {
        m_typingActive = true;
        m_client->sendTyping(m_roomId, true, kTypingTimeoutMs);
        m_typingRefresh.start();
    } else if (!wantTyping && m_typingActive) {
        stopTyping();
    }
}

void MessageComposer::setTypingNotificationsEnabled(bool enabled)
{
    if (m_typingEnabled == enabled)
        return;
    m_typingEnabled = enabled;
    if (!enabled)
        stopTyping();
    else
        refreshTypingState();
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
