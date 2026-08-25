#include "models/MessageComposer.h"

#include "app/DraftStore.h"
#include "matrix/MatrixClient.h"
#include "models/MarkdownFormat.h"

#include <QBuffer>
#include <QClipboard>
#include <QFile>
#include <QFileInfo>
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
// Draft-save debounce: long enough to coalesce a typing burst, short
// enough that a crash loses at most a second of text. Room switches save
// synchronously regardless.
constexpr int kDraftSaveMs = 1000;
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
    // A video queued before its poster finished decoding dispatches here,
    // the moment the poster resolves (or definitively fails).
    connect(m_attachments, &AttachmentQueueModel::entryPrepared,
            this, &MessageComposer::dispatchAttachment);
    m_draftDebounce.setSingleShot(true);
    m_draftDebounce.setInterval(kDraftSaveMs);
    connect(&m_draftDebounce, &QTimer::timeout, this,
            [this] { saveDraftNow(); });
}

void MessageComposer::saveDraftNow()
{
    if (!m_drafts || m_roomId.isEmpty() || m_restoringDraft)
        return;
    // Edit mode: the text is the edited event's body. Saving it would
    // resurrect an old message as a "draft".
    if (!m_editingEventId.isEmpty())
        return;
    QVariantMap draft;
    draft.insert(QStringLiteral("text"), m_text);
    if (!m_replyingToEventId.isEmpty()) {
        draft.insert(QStringLiteral("replyToEventId"), m_replyingToEventId);
        draft.insert(QStringLiteral("replyToSender"), m_replyingToSender);
        draft.insert(QStringLiteral("replyToPreview"), m_replyingToPreview);
        // The banner thumbnail's key (the reply target's event id — a
        // stable public identifier, never media bytes). Without it a
        // room switch restored the text preview but silently dropped
        // the thumbnail (review find, 2026-08-18).
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
    m_drafts->save(m_roomId, m_roomId, draft);
}

void MessageComposer::restoreDraft()
{
    if (!m_drafts || m_roomId.isEmpty())
        return;
    const QVariantMap draft = m_drafts->load(m_roomId);
    if (DraftStore::draftIsEmpty(draft))
        return;
    m_restoringDraft = true;
    m_text = draft.value(QStringLiteral("text")).toString();
    m_mentionRefs.clear();
    const QVariantList refs = draft.value(QStringLiteral("mentions")).toList();
    for (const QVariant &value : refs) {
        const QVariantMap map = value.toMap();
        mention::MentionRef ref;
        ref.userId = map.value(QStringLiteral("userId")).toString();
        ref.displayText = map.value(QStringLiteral("displayText")).toString();
        ref.start = map.value(QStringLiteral("start")).toInt();
        ref.length = map.value(QStringLiteral("length")).toInt();
        // Fail closed: a ref whose slice no longer matches its display
        // text contributes nothing rather than mis-tagging someone.
        if (!ref.userId.isEmpty() && ref.start >= 0 && ref.length > 0
            && ref.start + ref.length <= m_text.size()
            && m_text.mid(ref.start, ref.length) == ref.displayText) {
            m_mentionRefs.append(ref);
        }
    }
    // The reply target is restored tolerantly: if the referenced event was
    // redacted or is unavailable, the reply still sends (Matrix allows
    // replying to a redacted event) and the user can always cancel the
    // chip — a dangling target must never block editing or sending.
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
    m_client = client;
    m_attachments->setClient(client);
    if (m_client) {
        connect(m_client, &MatrixClient::attachmentQueueFinished,
                this, &MessageComposer::onAttachmentQueueFinished);
        // Scoped to the room the composer is in: a late answer for a room
        // the user has already left must not report over the new one (the
        // same rule the attachment-failure notice follows).
        connect(m_client, &MatrixClient::messageEditsRemoved, this,
                [this](const QString &roomId, const QString &eventId, bool ok,
                       int removed, int failed, bool truncated) {
                    if (roomId != m_roomId)
                        return;
                    Q_EMIT editsRemoved(eventId, ok, removed, failed,
                                        truncated);
                });
        connect(m_client, &MatrixClient::loggedOut, this, [this] {
            m_attachments->clearAll();
            // Unresolved voice recordings must not outlive the session on
            // disk; their ops can never resolve past this point.
            for (const VoiceOp &op : std::as_const(m_voiceOps))
                QFile::remove(op.localPath);
            m_voiceOps.clear();
            // Review M2: a pending draft save must die WITH the session —
            // firing after DraftStore's own loggedOut wipe would re-insert
            // the signed-out account's plaintext into the store the next
            // account inherits. The text goes too: it belongs to the
            // account that typed it.
            m_draftDebounce.stop();
            m_roomId.clear();
            cancelReplyOrEdit(); // may re-arm the debounce — stop it again
            m_draftDebounce.stop();
            if (!m_text.isEmpty()) {
                m_text.clear();
                Q_EMIT textChanged();
            }
            m_mentionRefs.clear();
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

void MessageComposer::setSendTextAsCaption(bool on)
{
    if (m_sendTextAsCaption == on) return;
    m_sendTextAsCaption = on;
    Q_EMIT sendTextAsCaptionChanged();
}

// Attach the typed text to ONE queued attachment as its caption, and report
// whether an attachment took it. A caption belongs to a single event — the
// setting promises "one event instead of two", not one caption per file — so
// the FIRST queued entry that can carry one takes it.
//
// Four cases deliberately fall back to the separate text message rather than
// dropping the text on the floor:
//   * the setting is off;
//   * nothing queued can carry a caption. A pasted image goes out through
//     sendAttachmentBytes, which has no caption parameter at any layer;
//   * the text carries @-mentions. A caption is a plain body: the m.mentions
//     list has nowhere to go and the expanded matrix.to markdown would be
//     shown literally, so mentions keep working by staying a real message;
//   * the composer is in thread-reply mode. The attachment dispatch targets
//     the ROOM, so a caption would move a thread reply into the main timeline
//     (CLAUDE.md §8) — the one failure here that is not merely cosmetic.
bool MessageComposer::takeTextAsCaption(const QString &body,
                                        const QStringList &mentionIds)
{
    if (!m_sendTextAsCaption || body.isEmpty() || !mentionIds.isEmpty()
        || !m_threadRootId.isEmpty() || !m_attachments)
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
        dispatchAttachment(row);
    }
}

// v0.7 video round: one entry's dispatch, separated out because a video
// waits for its locally extracted poster. The wait is bounded by the
// extractor's own timeout and resolves either way; a video whose poster
// could not be produced still sends, just without one.
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
    // The typed text rides along as the attachment's CAPTION when the user
    // has asked for that (Settings → Appearance → Message box). One event
    // instead of two. Empty when the setting is off, which is the previous
    // behaviour exactly — the argument was a literal QString() before.
    //
    // TAKEN, NOT COPIED: send() attaches the text to exactly one queued entry
    // and skips the separate text message under exactly the same condition,
    // or a subsequent plain send would repeat it and the user would see their
    // caption twice. A clipboard image never carries one: sendAttachmentBytes
    // has no caption parameter at any layer, which is why takeTextAsCaption
    // refuses an entry with no local path.
    const QString caption = entry.caption;
    quint64 opId = 0;
    if (entry.localPath.isEmpty()) {
        opId = m_client->sendAttachmentBytes(m_roomId, entry.data,
                                             entry.fileName, entry.mime,
                                             entry.width, entry.height);
    } else if (entry.isVideo) {
        opId = m_client->sendVideo(m_roomId, entry.localPath, entry.mime,
                                   caption, entry.width, entry.height,
                                   entry.durationMs, entry.poster,
                                   entry.posterWidth, entry.posterHeight);
    } else {
        opId = m_client->sendAttachment(m_roomId, entry.localPath,
                                        entry.mime, caption, entry.width,
                                        entry.height, entry.animated);
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
    // Preflight against the server's advertised upload limit, exactly like a
    // tray attachment. A recording is the one attachment the user cannot
    // resize, so failing it here — before an upload that the server would
    // reject — is the only useful moment to say so. Silent when the limit is
    // unknown: no invented ceiling refuses what the server would accept.
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
    const quint64 opId = m_client->sendVoiceMessage(
        targetRoom, localPath, mime, static_cast<qint64>(durationMs),
        amplitudes);
    if (opId == 0) {
        // Never queued: the recording is dead — reclaim it now.
        QFile::remove(localPath);
        Q_EMIT attachmentRejected(tr("The voice message could not be sent."));
        return;
    }
    m_voiceOps.insert(opId, VoiceOp{localPath, targetRoom});
}

void MessageComposer::onAttachmentQueueFinished(quint64 opId,
                                                const QString &roomId,
                                                bool ok,
                                                const QString &category)
{
    Q_UNUSED(category);
    if (const auto voiceIt = m_voiceOps.constFind(opId);
        voiceIt != m_voiceOps.constEnd()) {
        // The op owns its recording file; queued or failed, the SDK holds
        // the bytes (or nothing) — the path is no longer needed. Cleanup is
        // unconditional so a recording can never be orphaned on disk.
        const VoiceOp op = voiceIt.value();
        QFile::remove(op.localPath);
        m_voiceOps.erase(voiceIt);
        // Voice sends have no tray entry; only a failure needs surfacing —
        // success already shows as the SDK local echo in the timeline. The
        // notice is scoped to the room the recording was sent to: a failure
        // that resolves after the user has switched away belongs to that
        // conversation, not to whatever is on screen now, and there is no
        // banner to leave behind when they return.
        if (!ok && op.roomId == m_roomId)
            Q_EMIT attachmentRejected(
                tr("The voice message could not be sent."));
        return;
    }
    // Tray entries belong to the room they were prepared in — setRoomId
    // clears them — so a late result for another room matches no row. The
    // explicit guard keeps that true even if that ever changes.
    if (!roomId.isEmpty() && roomId != m_roomId)
        return;
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
    // Keep the mention ranges in sync with an ordinary edit (typing, deletion,
    // paste): a ref whose slice no longer matches is dropped fail-closed.
    m_mentionRefs = mention::shiftRefs(m_mentionRefs, m_text, t);
    m_text = t;
    Q_EMIT textChanged();
    Q_EMIT mentionRangesChanged();
    updateCanSend();
    refreshTypingState();
    // Coalesced draft persistence; never during a restore (the restore IS
    // the draft) and never in edit mode (saveDraftNow refuses it anyway).
    if (m_drafts && !m_restoringDraft && !m_roomId.isEmpty())
        m_draftDebounce.start();
}

void MessageComposer::setRoomId(const QString &r)
{
    if (m_roomId == r)
        return;
    // The old room's draft is saved BEFORE anything below mutates state:
    // the debounce is stopped so it cannot fire mid-switch, and the save
    // reads the still-current room. This also makes the Settings
    // round-trip (setRoomId("") then back) draft-preserving.
    m_draftDebounce.stop();
    saveDraftNow();
    // Cancel typing on the previous room and any pending reply/edit.
    if (!m_roomId.isEmpty()) stopTyping();
    cancelReplyOrEdit();
    // cancelReplyOrEdit may have re-armed the debounce; nothing may fire
    // between here and the restore below.
    m_draftDebounce.stop();
    // Queued attachments belong to the room they were prepared in; entries
    // already dispatched continue in the SDK send queue regardless.
    m_attachments->clearAll();
    m_roomId = r;
    m_text.clear();
    m_mentionRefs.clear();
    restoreDraft();
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
    if (!m_canSend || !m_client) return;
    // v0.7: expand inserted @-mentions into matrix.to markdown links and
    // collect the deduped MXIDs for m.mentions. With no mentions this is the
    // trimmed text and an empty id list, so the previous behaviour is exact.
    const mention::Expansion expansion = mention::expand(m_text, m_mentionRefs);
    const QString body = expansion.body.trimmed();
    const QStringList mentionIds = expansion.userIds;
    if (!m_editingEventId.isEmpty()) {
        // Edit mode edits text only; attachments stay queued.
        if (body.isEmpty()) return;
        m_client->editMessage(m_roomId, m_editingEventId, body, mentionIds);
    } else {
        // v0.5.9: attachments go first (each becomes its own SDK local
        // echo), then the text as a separate message — matching how other
        // Matrix clients compose "files + comment".
        //
        // …unless the user asked for the text to ride along as the first
        // attachment's caption, in which case there is no second message.
        // The caption is attached BEFORE dispatch: an entry held back for
        // its poster is dispatched later, and the caption has to be waiting
        // on it when it goes.
        const bool captioned = takeTextAsCaption(body, mentionIds);
        dispatchAttachments();
        if (!body.isEmpty() && !captioned) {
            if (!m_threadRootId.isEmpty()) {
                // v0.4.1: thread replies. Mock preserves thread grouping;
                // HTTP falls back to sendReply via the interface default.
                m_client->sendThreadReplyTo(m_roomId, m_threadRootId, QString(),
                                            body, mentionIds);
            } else if (!m_replyingToEventId.isEmpty()) {
                m_client->sendReply(m_roomId, m_replyingToEventId, body,
                                    mentionIds);
            } else {
                m_client->sendTextMessage(m_roomId, body, mentionIds);
            }
        }
    }
    stopTyping();
    cancelReplyOrEdit();
    clear();
}

void MessageComposer::clear()
{
    // A successful send and an explicit clear both retire the draft; a
    // pending debounce must not resurrect the text afterwards.
    m_draftDebounce.stop();
    if (m_drafts && !m_roomId.isEmpty())
        m_drafts->clear(m_roomId);
    m_mentionRefs.clear();
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
    // Suppress the popup when the detected token overlaps an already-inserted
    // mention (for example the trailing space right after "@Name ").
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
    // The insertion is one atomic edit: shift existing refs across it, then
    // record the new one (it never overlaps an existing ref).
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
                                  const QString &mediaKey)
{
    m_editingEventId.clear();
    Q_EMIT editStateChanged();
    m_replyingToEventId = eventId;
    m_replyingToSender  = sender;
    m_replyingToPreview = preview;
    m_replyingToMediaKey = mediaKey;
    Q_EMIT replyStateChanged();
    // The reply target is part of the draft.
    if (m_drafts && !m_restoringDraft && !m_roomId.isEmpty())
        m_draftDebounce.start();
}

void MessageComposer::beginEdit(const QString &eventId,
                                 const QString &currentBody,
                                 const QString &sanitizedHtml)
{
    m_replyingToEventId.clear();
    m_replyingToSender.clear();
    m_replyingToPreview.clear();
    m_replyingToMediaKey.clear();
    Q_EMIT replyStateChanged();
    m_threadRootId.clear();
    m_threadPreview.clear();
    Q_EMIT threadStateChanged();
    m_editingEventId = eventId;
    // Legacy sends carry raw [@x](https://matrix.to/…) markdown in the
    // body; newer sends carry display text with the mention identities
    // only in the formatted body's mention: anchors. Recover the semantic
    // refs from whichever form this event has, or the resend would
    // silently drop m.mentions.
    const mention::Recovery recovered = mention::recoverFromBody(currentBody);
    m_text = recovered.text;
    m_mentionRefs = recovered.refs;
    if (m_mentionRefs.isEmpty() && !sanitizedHtml.isEmpty()) {
        m_mentionRefs =
            mention::refsFromSanitizedHtml(m_text, sanitizedHtml);
    }
    Q_EMIT editStateChanged();
    Q_EMIT mentionRangesChanged();
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
    m_replyingToMediaKey.clear();
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
    m_replyingToMediaKey.clear();
    m_editingEventId.clear();
    m_threadRootId.clear();
    m_threadPreview.clear();
    m_mentionRefs.clear();
    Q_EMIT mentionRangesChanged();
    if (wasEditing) {
        m_text.clear();
        Q_EMIT textChanged();
        updateCanSend();
    }
    if (wasReplying) Q_EMIT replyStateChanged();
    if (wasEditing)  Q_EMIT editStateChanged();
    if (wasInThread) Q_EMIT threadStateChanged();
    // Dropping the reply chip changes the draft too.
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
    m_client->createPoll(m_roomId, m_threadRootId, question, answers,
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
