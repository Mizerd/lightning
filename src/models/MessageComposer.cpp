#include "models/MessageComposer.h"

#include <QRegularExpression>

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
// The SDK advertises a four-second typing timeout and suppresses redundant
// calls itself; renew just before expiry. Text is never forwarded.
constexpr int kTypingRefreshMs = 3000;
constexpr int kTypingTimeoutMs = 4000;
// Clipboard images larger than this are scaled down before encoding, so a
// paste cannot cause an unbounded allocation or upload.
constexpr int kMaxPasteEdge = 4096;
// Coalesces a typing burst while losing at most a second of text on a crash.
// Room switches save synchronously.
constexpr int kDraftSaveMs = 1000;
}

MessageComposer::MessageComposer(QObject *parent)
    : QObject(parent)
    , m_attachments(new AttachmentQueueModel(this))
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
    // A video queued before its poster was ready dispatches once the poster
    // resolves or fails.
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
    // Edit mode: the text is the edited event's body, not a draft.
    if (!m_editingEventId.isEmpty())
        return;
    QVariantMap draft;
    draft.insert(QStringLiteral("text"), m_text);
    if (!m_replyingToEventId.isEmpty()) {
        draft.insert(QStringLiteral("replyToEventId"), m_replyingToEventId);
        draft.insert(QStringLiteral("replyToSender"), m_replyingToSender);
        draft.insert(QStringLiteral("replyToPreview"), m_replyingToPreview);
        // The banner thumbnail's key (the reply target's event id), so a room
        // switch restores the thumbnail with the text.
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
        // Fail closed: a ref whose slice no longer matches its text contributes
        // nothing.
        if (!ref.userId.isEmpty() && ref.start >= 0 && ref.length > 0
            && ref.start + ref.length <= m_text.size()
            && m_text.mid(ref.start, ref.length) == ref.displayText) {
            m_mentionRefs.append(ref);
        }
    }
    // Restored tolerantly: replying to a redacted or unavailable event is
    // allowed, and a dangling target must never block editing or sending.
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
        // Scoped to the current room: a late answer for a room the user left
        // must not report over the new one.
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
            // Unresolved voice recordings must not outlive the session on disk.
            for (const VoiceOp &op : std::as_const(m_voiceOps))
                QFile::remove(op.localPath);
            m_voiceOps.clear();
            // A pending draft save must die with the session: firing after
            // DraftStore's loggedOut wipe would re-insert the signed-out
            // account's plaintext. The text goes too.
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

    // Real file MIME data (text/uri-list), e.g. from a file manager. Plain text
    // is never interpreted as paths.
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

// Attach the typed text as the caption of one queued attachment (the first
// that can carry one) and report whether it was taken. Falls back to a
// separate text message when:
//   * the setting is off;
//   * nothing queued can carry a caption (sendAttachmentBytes, used for
//     pasted images, has no caption parameter);
//   * the text has @-mentions, which a plain caption body cannot carry;
//   * the composer is in thread-reply mode: attachments target the room, so a
//     caption would move a thread reply into the main timeline.
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

// One entry's dispatch, separate because a video waits for its extracted
// poster. The wait is bounded and resolves either way; without a poster the
// video still sends.
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
    // The typed text as caption when the user enabled that (one event instead
    // of two); empty otherwise. Taken, not copied: send() skips the separate
    // text message under the same condition, so the caption never appears
    // twice.
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
    } else if (entry.isSvg) {
        // The PNG rendered from the file becomes thumbnail_info, so receivers
        // show a preview without decoding SVG. Empty when none could be made.
        opId = m_client->sendImageWithThumbnail(
            m_roomId, entry.localPath, entry.mime, caption, entry.width,
            entry.height, entry.poster, entry.posterWidth, entry.posterHeight);
    } else {
        // Duration is 0 for non-timed media or an undecodable length; both are
        // sent as absent, never as a literal zero.
        opId = m_client->sendAttachment(m_roomId, entry.localPath,
                                        entry.mime, caption, entry.width,
                                        entry.height, entry.animated,
                                        entry.durationMs);
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
    // Preflight against the server's upload limit: a recording cannot be
    // resized, so fail before an upload the server would reject. Silent when
    // the limit is unknown.
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
        // Never queued: reclaim the recording now.
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
        // The op owns its recording file; once queued or failed the path is no
        // longer needed, so cleanup is unconditional.
        const VoiceOp op = voiceIt.value();
        QFile::remove(op.localPath);
        m_voiceOps.erase(voiceIt);
        // Voice sends have no tray entry; success shows as the local echo, so
        // only a failure is surfaced, and only in the room the recording was
        // sent to.
        if (!ok && op.roomId == m_roomId)
            Q_EMIT attachmentRejected(
                tr("The voice message could not be sent."));
        return;
    }
    // Tray entries belong to the room they were prepared in; setRoomId clears
    // them. The explicit guard keeps a late result from matching another room.
    if (!roomId.isEmpty() && roomId != m_roomId)
        return;
    auto &entries = m_attachments->entries();
    for (int row = 0; row < entries.size(); ++row) {
        if (entries[row].opId != opId || opId == 0)
            continue;
        if (ok) {
            // The SDK local echo now owns the send state. Leave "dispatching"
            // first so removeAt's mid-dispatch guard does not apply.
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
    // Keep mention ranges in sync with ordinary edits; a ref whose slice no
    // longer matches is dropped.
    m_mentionRefs = mention::shiftRefs(m_mentionRefs, m_text, t);
    m_text = t;
    Q_EMIT textChanged();
    Q_EMIT mentionRangesChanged();
    // Any edit invalidates a command refusal and can change completion matches.
    setCommandError(QString());
    Q_EMIT commandCompletionsChanged();
    updateCanSend();
    refreshTypingState();
    // Coalesced draft persistence; never during a restore or in edit mode.
    if (m_drafts && !m_restoringDraft && !m_roomId.isEmpty())
        m_draftDebounce.start();
}

void MessageComposer::setRoomId(const QString &r)
{
    if (m_roomId == r)
        return;
    // Save the old room's draft before anything changes: stop the debounce and
    // save against the still-current room. Keeps a setRoomId("") round trip
    // draft-preserving.
    m_draftDebounce.stop();
    saveDraftNow();
    // Cancel typing on the previous room and any pending reply/edit.
    if (!m_roomId.isEmpty()) stopTyping();
    cancelReplyOrEdit();
    // cancelReplyOrEdit may have re-armed the debounce.
    m_draftDebounce.stop();
    // Queued attachments belong to their room; dispatched ones continue in the
    // SDK send queue.
    m_attachments->clearAll();
    m_roomId = r;
    m_text.clear();
    m_mentionRefs.clear();
    restoreDraft();
    // A command refusal belongs to the room it was issued in.
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
    sendInternal(/*allowCommands=*/true);
}

void MessageComposer::sendBypassingCommands()
{
    sendInternal(/*allowCommands=*/false);
}

void MessageComposer::sendInternal(bool allowCommands)
{
    if (!m_canSend || !m_client) return;
    // Expand inserted @-mentions into matrix.to markdown links and collect the
    // MXIDs for m.mentions. Without mentions this is just the trimmed text.
    const mention::Expansion expansion = mention::expand(m_text, m_mentionRefs);
    QString body = expansion.body.trimmed();
    const QStringList mentionIds = expansion.userIds;

    // Slash commands apply only to fresh messages, never to an edit. Parsing
    // the expanded body is safe: a mention ref can never cover the leading
    // slash.
    if (allowCommands && m_editingEventId.isEmpty()) {
        const SlashCommands::Parse parsed = SlashCommands::parse(body);
        switch (parsed.kind) {
        case SlashCommands::Parse::NotACommand:
            break;
        case SlashCommands::Parse::Escaped:
            // "//hello" sends "/hello" as an ordinary message.
            body = parsed.literalText.trimmed();
            break;
        case SlashCommands::Parse::Unknown:
            // Unknown commands are sent as text, not refused: a client cannot
            // tell a bot's command from a typo, and bots are the common case. A
            // known command with bad arguments still refuses; `//text` escapes
            // to a literal.
            break;
        case SlashCommands::Parse::Known:
            if (executeCommand(parsed, mentionIds))
                return;
            break;
        }
    }

    if (!m_editingEventId.isEmpty()) {
        // Edit mode edits text only; attachments stay queued.
        if (body.isEmpty()) return;
        m_client->editMessage(editTargetTimelineId(), m_editingEventId, body,
                              mentionIds);
    } else {
        // Attachments go first (each its own local echo), then the text as a
        // separate message, unless the text rides as a caption. The caption is
        // set before dispatch because an entry waiting for its poster
        // dispatches later.
        const bool captioned = takeTextAsCaption(body, mentionIds);
        dispatchAttachments();
        if (!body.isEmpty() && !captioned)
            sendComposed(body, mentionIds, QVariantMap());
    }
    finishSuccessfulSend();
}

void MessageComposer::sendPrepared(const QString &body, const QString &html,
                                   const QStringList &mentionUserIds)
{
    if (!m_client || m_roomId.isEmpty() || body.trimmed().isEmpty())
        return;
    // An unformatted rich-mode message uses the plain lane: the body is sent
    // verbatim, never re-read as markdown.
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
    }
    finishSuccessfulSend();
}

void MessageComposer::setEmoticonResolver(
    std::function<QString(const QString &)> resolve)
{
    m_emoticonResolver = std::move(resolve);
}

namespace {
/// The `:shortcode:` runs in a body that resolve against the user's installed
/// packs; ordinary colons produce nothing. Not an emoji parser: code spans and
/// URLs are excluded later in Rust, over the formatted body.
QVariantMap resolveShortcodes(
    const QString &body,
    const std::function<QString(const QString &)> &resolve)
{
    QVariantMap found;
    if (!resolve || !body.contains(QLatin1Char(':')))
        return found;
    static const QRegularExpression re(
        QStringLiteral(":([a-zA-Z0-9_+-]{1,64}):"));
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
} // namespace

QVariantMap MessageComposer::composedMessage() const
{
    const mention::Expansion expansion = mention::expand(m_text, m_mentionRefs);
    return QVariantMap{
        { QStringLiteral("body"), expansion.body.trimmed() },
        { QStringLiteral("mentionIds"), expansion.userIds },
        { QStringLiteral("bodySpec"), QVariantMap() },
        { QStringLiteral("roomId"), m_roomId },
        { QStringLiteral("threadRootId"), m_threadRootId },
        { QStringLiteral("replyToEventId"), m_replyingToEventId },
    };
}

void MessageComposer::sendComposed(const QString &body,
                                   const QStringList &mentionIds,
                                   const QVariantMap &bodySpecIn)
{
    if (!m_client || body.isEmpty())
        return;
    // MSC2545 inline custom emoji, added here because every send path passes
    // through. The shortcode map crosses to Rust rather than finished HTML, so
    // markdown still applies; Rust substitutes after markdown, where code spans
    // and URLs are distinguishable.
    QVariantMap bodySpec = bodySpecIn;
    const QVariantMap emoticons = resolveShortcodes(body, m_emoticonResolver);
    if (!emoticons.isEmpty()) {
        if (!bodySpec.contains(QStringLiteral("format"))) {
            bodySpec.insert(QStringLiteral("format"),
                            QStringLiteral("markdown"));
        }
        bodySpec.insert(QStringLiteral("emoticons"), emoticons);
    }
    if (!m_threadRootId.isEmpty()) {
        // Thread replies. The HTTP backend falls back to sendReply.
        m_client->sendThreadReplyTo(m_roomId, m_threadRootId, QString(), body,
                                    mentionIds, bodySpec);
    } else if (!m_replyingToEventId.isEmpty()) {
        m_client->sendReply(m_roomId, m_replyingToEventId, body, mentionIds,
                            bodySpec);
    } else {
        m_client->sendTextMessage(m_roomId, body, mentionIds, bodySpec);
    }
}

void MessageComposer::finishSuccessfulSend()
{
    stopTyping();
    cancelReplyOrEdit();
    clear();
}

bool MessageComposer::executeCommand(const SlashCommands::Parse &parsed,
                                     const QStringList &mentionIds)
{
    // Command semantics live in SlashCommands::execute (shared with the thread
    // composer); this adapter supplies this composer's lanes. Each action is a
    // real backend verb with an explicit room id, except mode (a QML setting)
    // and display-name changes (AppController's op bookkeeping), which only
    // ask.
    SlashCommands::Actions actions;
    actions.send = [this](const QString &body, const QStringList &ids,
                          const QVariantMap &spec) {
        sendComposed(body, ids, spec);
    };
    actions.join = [this](const QString &target) {
        m_client->joinRoomByIdOrAlias(target, QStringList());
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
        // The draft is kept across a mode switch; only the command text is
        // removed.
        Q_EMIT composerModeToggleRequested();
        clear();
    };
    actions.clearComposer = [this] {
        // /clear empties the composer (draft and queued attachments) only. It
        // never touches the timeline, local cache or server; clearing the SDK
        // event cache would force a refetch.
        if (m_attachments)
            m_attachments->clearAll();
        stopTyping();
        cancelReplyOrEdit();
        clear();
    };

    const SlashCommands::Outcome outcome =
        SlashCommands::execute(parsed, mentionIds, actions);
    if (!outcome.error.isEmpty()) {
        setCommandError(outcome.error);
        return true;
    }
    if (outcome.retireDraft)
        finishSuccessfulSend();
    return true;
}

QVariantList MessageComposer::commandCompletions() const
{
    QVariantList out;
    // Completion only while typing a command word in a fresh message.
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

void MessageComposer::setCommandError(const QString &error)
{
    if (m_commandError == error)
        return;
    m_commandError = error;
    Q_EMIT commandErrorChanged();
}

void MessageComposer::setEmoticonSearch(
    std::function<QVariantList(const QString &, int)> search)
{
    m_emoticonSearch = std::move(search);
}

namespace {
/// The `:token` the caret is inside, as [start, length], or {-1, 0}. The
/// colon must start a word, so `http://host:8080` and `10:30` never complete.
QPair<int, int> emojiTokenAt(const QString &text, int cursor)
{
    if (cursor < 0 || cursor > text.size())
        return { -1, 0 };
    int i = cursor;
    // Walk back over shortcode characters to the colon.
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
    // The colon must start a word: preceded by nothing, whitespace or an
    // opening bracket.
    if (colon > 0) {
        const QChar before = text.at(colon - 1);
        if (!(before.isSpace() || before == QLatin1Char('(')
              || before == QLatin1Char('[') || before == QLatin1Char('{')))
            return { -1, 0 };
    }
    return { colon, cursor - colon };
}

/// True when the caret is inside a fenced or inline code run, where a
/// shortcode is literal.
bool insideCode(const QString &text, int cursor)
{
    int ticks = 0;
    for (int i = 0; i < cursor && i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('`'))
            ++ticks;
    }
    return ticks % 2 == 1;
}
} // namespace

QVariantList MessageComposer::emojiCompletionsAt(int cursorPos) const
{
    QVariantList out;
    if (!m_emoticonSearch || !m_editingEventId.isEmpty())
        return out;
    const auto token = emojiTokenAt(m_text, cursorPos);
    if (token.first < 0)
        return out;
    // At least one query character, so typing a colon does not open a popup.
    const QString prefix = m_text.mid(token.first + 1, token.second - 1);
    if (prefix.isEmpty() || insideCode(m_text, cursorPos))
        return out;
    return m_emoticonSearch(prefix, 12);
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
    // A send or explicit clear retires the draft; a pending debounce must not
    // resurrect it.
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
    // Suppress the popup when the token overlaps an inserted mention (e.g. the
    // space right after "@Name ").
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
    // One atomic edit: shift existing refs across it, then record the new one.
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
    m_editingTimelineId.clear();
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
                                 const QString &sanitizedHtml,
                                 const QString &timelineId)
{
    m_editingTimelineId = timelineId;
    m_replyingToEventId.clear();
    m_replyingToSender.clear();
    m_replyingToPreview.clear();
    m_replyingToMediaKey.clear();
    Q_EMIT replyStateChanged();
    m_threadRootId.clear();
    m_threadPreview.clear();
    Q_EMIT threadStateChanged();
    m_editingEventId = eventId;
    // Older sends carry raw [@x](https://matrix.to/…) markdown in the body;
    // newer ones carry display text with mention: anchors in the formatted
    // body. Recover the refs from either, or the edit would drop m.mentions.
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
    m_editingTimelineId.clear();
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
    m_editingTimelineId.clear();
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
    // Sendable with text, or with queued attachments outside edit mode.
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
