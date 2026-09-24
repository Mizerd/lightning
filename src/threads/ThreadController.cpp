#include "threads/ThreadController.h"

#include "models/SlashCommands.h"

#include "app/DraftStore.h"
#include "matrix/MatrixClient.h"
#include "models/UserLookup.h"

#include <QBuffer>
#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>

#include <algorithm>

namespace {
// Clipboard images beyond this edge are scaled down before encoding, as in
// the room composer, to bound allocation and upload size.
constexpr int kMaxPasteEdge = 4096;
}

ThreadController::ThreadController(QObject *parent)
    : QObject(parent)
    , m_attachments(new AttachmentQueueModel(this))
{
    connect(m_attachments, &AttachmentQueueModel::countChanged, this,
            [this] { Q_EMIT attachmentsChanged(); });
    // A video queued before its poster decoded dispatches once the poster
    // resolves or fails.
    connect(m_attachments, &AttachmentQueueModel::entryPrepared,
            this, &ThreadController::dispatchAttachment);
    m_draftDebounce.setSingleShot(true);
    m_draftDebounce.setInterval(1000);
    connect(&m_draftDebounce, &QTimer::timeout, this,
            [this] { saveDraftNow(); });

    // ── Reply navigation inside the open thread (contract C5) ───────────
    m_navigationHighlightTimer.setSingleShot(true);
    m_navigationMessageTimer.setSingleShot(true);
    m_navigationBatchTimer.setSingleShot(true);
    connect(&m_navigationHighlightTimer, &QTimer::timeout, this, [this] {
        m_navigationHighlightEventId.clear();
        Q_EMIT navigationChanged();
    });
    connect(&m_navigationMessageTimer, &QTimer::timeout, this, [this] {
        m_navigationMessage.clear();
        Q_EMIT navigationChanged();
    });
    connect(&m_navigationBatchTimer, &QTimer::timeout, this, [this] {
        // Nothing answered the batch; report failure. The batch counter is
        // already spent, so this cannot spin.
        if (!m_navigationEventId.isEmpty())
            failNavigation();
    });
    // Rows landed: check now so the panel lands on the same frame. Requesting
    // another page waits for the completion edge, since a request made while
    // still paginating would be dropped and waste an attempt.
    connect(&m_model, &TimelineModel::olderPrepended, this, [this](int) {
        if (m_navigationEventId.isEmpty())
            return;
        if (navigationStale()) {
            clearNavigation(/*clearMessage=*/true);
            return;
        }
        const int row = m_model.rowForStableId(m_navigationEventId);
        if (row >= 0)
            locateNavigationTarget(row);
    });
    // The SDK summary arrives as an in-place Set on the root row, which does
    // not emit countChanged, so dataChanged is watched too. Everything goes
    // through notifyReplyCountIfChanged(), which only announces when the
    // number actually changes.
    connect(&m_model, &TimelineModel::countChanged, this,
            &ThreadController::notifyReplyCountIfChanged);
    connect(this, &ThreadController::stateChanged, this,
            &ThreadController::notifyReplyCountIfChanged);
    connect(&m_model, &TimelineModel::dataChanged, this,
            [this](const QModelIndex &topLeft, const QModelIndex &bottomRight,
                   const QList<int> &roles) {
                // onEventChangedAt announces all roles (an empty list), so
                // empty must count as a match.
                if (!roles.isEmpty()
                    && !roles.contains(TimelineModel::ThreadReplyCountRole))
                    return;
                // Compare ids over the announced range instead of resolving
                // the root's row: rowForStableId is a linear scan, and this
                // runs on every Set.
                const auto &events = m_model.events();
                const int lo = qMax(0, topLeft.row());
                const int hi =
                    qMin(bottomRight.row(), static_cast<int>(events.size()) - 1);
                for (int row = lo; row <= hi; ++row) {
                    if (events.at(row).eventId == m_rootEventId) {
                        notifyReplyCountIfChanged();
                        // The panel's root card is a snapshot of this row.
                        Q_EMIT rootInfoChanged();
                        return;
                    }
                }
            });
    connect(&m_model, &TimelineModel::paginationChanged, this, [this] {
        const bool wasPaginating = m_modelPaginating;
        m_modelPaginating = m_model.paginating();
        if (m_navigationEventId.isEmpty() || m_modelPaginating)
            return;
        // Only a busy -> idle transition is an answer; other emissions must
        // not consume a bounded attempt.
        if (!wasPaginating)
            return;
        continueNavigation();
    });
}

void ThreadController::saveDraftNow()
{
    if (!m_drafts || m_restoringDraft)
        return;
    const QString key = timelineId();
    if (key.isEmpty())
        return;
    QVariantMap draft;
    draft.insert(QStringLiteral("text"), m_text);
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
    m_drafts->save(key, m_roomId, draft);
}

void ThreadController::restoreDraft()
{
    if (!m_drafts)
        return;
    const QString key = timelineId();
    if (key.isEmpty())
        return;
    const QVariantMap draft = m_drafts->load(key);
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
        if (!ref.userId.isEmpty() && ref.start >= 0 && ref.length > 0
            && ref.start + ref.length <= m_text.size()
            && m_text.mid(ref.start, ref.length) == ref.displayText) {
            m_mentionRefs.append(ref);
        }
    }
    m_restoringDraft = false;
    Q_EMIT textChanged();
    Q_EMIT mentionRangesChanged();
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
        connect(m_client, &MatrixClient::loggedOut, this, [this] {
            // Unresolved thread recordings must not outlive the session on
            // disk. Mirrors MessageComposer's cleanup.
            for (const VoiceOp &op : std::as_const(m_voiceOps))
                QFile::remove(op.localPath);
            m_voiceOps.clear();
        });
        connect(m_client, &MatrixClient::timelineReset, this,
                [this](const QString &timelineId) {
                    // Only the requested thread's reset promotes to Ready;
                    // composite-id identity is the staleness gate.
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
                        // The change did not apply; re-query the server state.
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

    // Save the previous thread's draft before the ids its key derives from
    // change, and stop the debounce so a stale timer cannot write across
    // threads.
    m_draftDebounce.stop();
    saveDraftNow();

    m_roomId = roomId;
    m_rootEventId = rootEventId;
    m_failureCategory.clear();
    cancelReply();
    // Drop the previous thread's composer text (its draft was saved above).
    clearComposerText();
    restoreDraft();
    // Queued attachments belong to the thread they were prepared in.
    clearAttachments();
    // Abandon any reply search in the previous thread silently;
    // navigationStale() handles batches already in flight.
    clearNavigation(/*clearMessage=*/true);
    // Bind the model to the new composite id before dispatching so the
    // snapshot reset is applied, never raced.
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
    // Closing keeps the draft; it restores when the thread is reopened.
    m_draftDebounce.stop();
    saveDraftNow();
    const bool wasActive = m_state != Closed;
    if (m_client && wasActive)
        m_client->closeThread();
    m_roomId.clear();
    m_rootEventId.clear();
    m_failureCategory.clear();
    m_model.setRoomId(QString{});
    cancelReply();
    clearComposerText();
    clearAttachments();
    clearNavigation(/*clearMessage=*/true);
    resetFollowState();
    m_lastMarkedReadEventId.clear();
    // Reclaim voice recordings whose send may never report back: the result
    // is gated on the thread generation, which advances here. Deleting is
    // safe because rooms::send_thread_voice_path reads the bytes up front
    // (AttachmentSource::Data), so nothing touches the path afterwards. Do
    // not switch it to the File variant, which reads inside the spawned task.
    for (const VoiceOp &op : std::as_const(m_voiceOps))
        QFile::remove(op.localPath);
    m_voiceOps.clear();
    if (wasActive) {
        // The next thread announces its own count. Keep the braces: close()
        // must not announce on an already-closed controller.
        m_lastReplyCount = -1;
        setState(Closed);
    }
}

void ThreadController::sendText(const QString &body)
{
    sendTextInternal(body, /*allowCommands=*/true);
}

void ThreadController::sendTextBypassingCommands(const QString &body)
{
    sendTextInternal(body, /*allowCommands=*/false);
}

void ThreadController::sendTextInternal(const QString &body, bool allowCommands)
{
    if (!m_client || m_state == Closed || m_roomId.isEmpty()
        || m_rootEventId.isEmpty())
        return;
    // Expand inserted @-mentions into matrix.to links and the deduped MXID
    // list. Refs index into m_text; without tracked text, fall back to the
    // trimmed body.
    const mention::Expansion expansion = mention::expand(m_text, m_mentionRefs);
    QString outBody = expansion.body.trimmed();
    if (outBody.isEmpty())
        outBody = body.trimmed();
    const QStringList mentionIds = expansion.userIds;

    // Slash commands follow the room composer's rules: parsed before
    // attachments dispatch, so a refused command leaves the tray untouched.
    // Content commands go through the thread lane; room administration acts
    // on the thread's room.
    if (allowCommands && !outBody.isEmpty()) {
        const SlashCommands::Parse parsed = SlashCommands::parse(outBody);
        switch (parsed.kind) {
        case SlashCommands::Parse::NotACommand:
            break;
        case SlashCommands::Parse::Escaped:
            outBody = parsed.literalText.trimmed();
            break;
        case SlashCommands::Parse::Unknown:
            // Unknown commands are sent as text (issue #11): bots have
            // command sets Lightning cannot know. A known command with bad
            // arguments still refuses; `//text` still escapes to a literal.
            break;
        case SlashCommands::Parse::Known: {
            SlashCommands::Actions actions;
            actions.send = [this](const QString &b, const QStringList &ids,
                                  const QVariantMap &spec) {
                sendThreadBody(b, ids, spec);
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
                Q_EMIT composerModeToggleRequested();
                clearComposerText();
            };
            actions.clearComposer = [this] {
                m_attachments->clearAll();
                cancelReply();
                clearComposerText();
            };
            const SlashCommands::Outcome outcome =
                SlashCommands::execute(parsed, mentionIds, actions);
            if (!outcome.error.isEmpty()) {
                setCommandError(outcome.error);
                return;
            }
            if (outcome.retireDraft)
                retireComposerDraft();
            return;
        }
        }
    }

    // Attachments first, each its own SDK local echo, then the text as a
    // separate thread message, matching the room composer.
    dispatchAttachments();
    if (outBody.isEmpty())
        return;   // attachment-only send is valid.
    sendThreadBody(outBody, mentionIds, QVariantMap());
    retireComposerDraft();
}

void ThreadController::sendPrepared(const QString &body, const QString &html,
                                    const QStringList &mentionUserIds)
{
    if (!m_client || m_state == Closed || m_roomId.isEmpty()
        || m_rootEventId.isEmpty() || body.trimmed().isEmpty())
        return;
    dispatchAttachments();
    // Empty html is an unformatted rich-mode message: send verbatim on the
    // plain lane, not as markdown.
    sendThreadBody(body, mentionUserIds,
                   html.isEmpty()
                       ? QVariantMap{ { QStringLiteral("format"),
                                        QStringLiteral("plain") } }
                       : QVariantMap{ { QStringLiteral("format"),
                                        QStringLiteral("html") },
                                      { QStringLiteral("html"), html } });
    retireComposerDraft();
}

void ThreadController::sendThreadBody(const QString &body,
                                      const QStringList &mentionIds,
                                      const QVariantMap &bodySpec)
{
    // Always the SDK thread path (sendThreadReplyTo), never sendTextMessage,
    // so a thread reply cannot land as a room message. An empty in-reply-to
    // is a plain thread reply.
    if (!m_replyToEventId.isEmpty()) {
        m_client->sendThreadReplyTo(m_roomId, m_rootEventId, m_replyToEventId,
                                    body, mentionIds, bodySpec);
        cancelReply();
    } else {
        m_client->sendThreadReplyTo(m_roomId, m_rootEventId, QString(), body,
                                    mentionIds, bodySpec);
    }
}

void ThreadController::retireComposerDraft()
{
    // Retire the draft; a pending debounce must not resurrect the text.
    m_draftDebounce.stop();
    if (m_drafts && !timelineId().isEmpty())
        m_drafts->clear(timelineId());
    clearComposerText();
}

void ThreadController::setCommandError(const QString &error)
{
    if (m_commandError == error)
        return;
    m_commandError = error;
    Q_EMIT commandErrorChanged();
}

void ThreadController::setText(const QString &text)
{
    if (m_text == text)
        return;
    m_mentionRefs = mention::shiftRefs(m_mentionRefs, m_text, text);
    m_text = text;
    // Any edit invalidates a standing command refusal.
    setCommandError(QString());
    Q_EMIT textChanged();
    Q_EMIT mentionRangesChanged();
    if (m_drafts && !m_restoringDraft && !timelineId().isEmpty())
        m_draftDebounce.start();
}

void ThreadController::clearComposerText()
{
    m_mentionRefs.clear();
    Q_EMIT mentionRangesChanged();
    if (m_text.isEmpty())
        return;
    m_text.clear();
    Q_EMIT textChanged();
}

QVariantMap ThreadController::mentionTokenAt(const QString &text,
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

int ThreadController::insertMention(const QString &userId,
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
    return res.cursorPos;
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
        if (entries[row].state != QLatin1String("queued"))
            continue;
        entries[row].sendRequested = true;
        dispatchAttachment(row);
    }
}

// See MessageComposer::dispatchAttachment: a queued video waits for its local
// poster, then sends through the SDK's thread-focused path.
void ThreadController::dispatchAttachment(int row)
{
    if (!m_client || m_state != Ready || m_roomId.isEmpty()
        || m_rootEventId.isEmpty())
        return;
    auto &entries = m_attachments->entries();
    if (row < 0 || row >= entries.size())
        return;
    auto &entry = entries[row];
    if (entry.state != QLatin1String("queued") || !entry.sendRequested
        || entry.posterPending)
        return;
    quint64 opId = 0;
    if (entry.localPath.isEmpty()) {
        opId = m_client->sendThreadAttachmentBytes(
            m_roomId, m_rootEventId, entry.data, entry.fileName, entry.mime,
            entry.width, entry.height);
    } else if (entry.isVideo) {
        opId = m_client->sendThreadVideo(
            m_roomId, m_rootEventId, entry.localPath, entry.mime, QString(),
            entry.width, entry.height, entry.durationMs, entry.poster,
            entry.posterWidth, entry.posterHeight);
    } else if (entry.isSvg) {
        opId = m_client->sendThreadImageWithThumbnail(
            m_roomId, m_rootEventId, entry.localPath, entry.mime, QString(),
            entry.width, entry.height, entry.poster, entry.posterWidth,
            entry.posterHeight);
    } else {
        opId = m_client->sendThreadAttachment(
            m_roomId, m_rootEventId, entry.localPath, entry.mime, QString(),
            entry.width, entry.height, entry.animated, entry.durationMs);
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

void ThreadController::sendVoiceMessage(const QString &localPath,
                                        const QString &mime,
                                        qreal durationMs,
                                        const QVariantList &waveform)
{
    if (!m_client || m_state != Ready || m_roomId.isEmpty()
        || m_rootEventId.isEmpty() || localPath.isEmpty() || mime.isEmpty()
        || durationMs <= 0) {
        QFile::remove(localPath);
        Q_EMIT attachmentRejected(tr("The voice message could not be sent."));
        return;
    }
    // Same preflight as the room composer; silent when the limit is unknown.
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
    // Capture the target thread now; the panel may move before the send
    // resolves.
    const QString targetRoom = m_roomId;
    const QString targetRoot = m_rootEventId;
    const quint64 opId = m_client->sendThreadVoiceMessage(
        targetRoom, targetRoot, localPath, mime,
        static_cast<qint64>(durationMs), amplitudes);
    if (opId == 0) {
        // Never retried as a room send: a thread voice message must not land
        // in the main timeline.
        QFile::remove(localPath);
        Q_EMIT attachmentRejected(tr("The voice message could not be sent."));
        return;
    }
    m_voiceOps.insert(opId, VoiceOp{localPath, targetRoom, targetRoot});
}

void ThreadController::onAttachmentQueueFinished(quint64 opId,
                                                 const QString &roomId,
                                                 bool ok,
                                                 const QString &category)
{
    Q_UNUSED(category);
    if (opId == 0)
        return;
    if (const auto voiceIt = m_voiceOps.constFind(opId);
        voiceIt != m_voiceOps.constEnd()) {
        // Cleanup is unconditional; a failure is reported only while the same
        // room and root are still open.
        const VoiceOp op = voiceIt.value();
        QFile::remove(op.localPath);
        m_voiceOps.erase(voiceIt);
        if (!ok && op.roomId == m_roomId && op.rootEventId == m_rootEventId)
            Q_EMIT attachmentRejected(
                tr("The voice message could not be sent."));
        return;
    }
    if (!roomId.isEmpty() && roomId != m_roomId)
        return;
    auto &entries = m_attachments->entries();
    for (int row = 0; row < entries.size(); ++row) {
        if (entries[row].opId != opId)
            continue;   // not ours (the room composer owns other op ids).
        if (ok) {
            // The SDK local echo now owns this attachment's send state.
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
    // Only loaded thread events are valid reply targets; replying to the
    // root is a plain thread message, so it clears the target.
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

int ThreadController::replyCount() const
{
    // Read the event, not ThreadReplyCountRole: the role reports 0 when the
    // summary is absent, while TimelineEvent::threadReplyCount is -1 exactly
    // when unknown.
    const auto summaryFor = [](const QList<TimelineEvent> &events,
                               const QString &rootId) {
        for (const auto &e : events)
            if (e.eventId == rootId)
                return e.threadReplyCount;   // -1 when the SDK has not said
        return -1;
    };

    const int rootRow = m_model.rowForStableId(m_rootEventId);
    int known = summaryFor(m_model.events(), m_rootEventId);
    // The root may not be a row of the thread model yet (same fallback as
    // rootInfo()); only then scan the room timeline.
    if (rootRow < 0 && known < 0 && m_client)
        known = summaryFor(m_client->timeline(m_roomId), m_rootEventId);

    // Real loaded events (virtual rows are not replies), less the root.
    const int loaded = qMax(0, m_model.realEventCount() - (rootRow >= 0 ? 1 : 0));

    // The divider sits above the replies, so loaded replies are a floor: the
    // server summary can lag behind a just-sent reply. The SDK number covers
    // history beyond the loaded window. As a result the label may exceed the
    // room card by one after a redaction (the redacted row stays) or while a
    // local echo is pending; it describes the rows beneath it.
    return qMax(known, loaded);
}

void ThreadController::notifyReplyCountIfChanged()
{
    const int now = replyCount();
    if (now == m_lastReplyCount)
        return;
    m_lastReplyCount = now;
    Q_EMIT replyCountChanged();
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
                            ? matrix::user_lookup::localpartOrUserId(
                                  event.sender)
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

// ── Reply navigation inside the open thread (contract C5) ───────────────
//
// Separate from PaginationController::jumpToEvent on purpose. The SDK's
// thread timeline keeps only in-thread relations, so a reply target is always
// another thread event or the root; the room history loader must never be
// used. "Open in room" is the only way out of the thread.
void ThreadController::navigateToEvent(const QString &eventId)
{
    if (eventId.isEmpty() || m_state == Closed)
        return;
    if (eventId == m_rootEventId) {
        // The root is pinned as a card above the replies, so navigating to
        // it pulses the card.
        clearNavigation(/*clearMessage=*/true);
        setNavigationHighlight(eventId);
        return;
    }
    const int loadedRow = m_model.rowForStableId(eventId);
    if (loadedRow >= 0) {
        clearNavigation(/*clearMessage=*/true);
        setNavigationHighlight(eventId);
        Q_EMIT navigationTargetLocated(loadedRow);
        return;
    }
    if (m_navigationEventId == eventId)
        return;   // coalesce repeated activation of the same reply preview
    clearNavigation(/*clearMessage=*/true);
    m_navigationEventId = eventId;
    m_navigationRoomId = m_roomId;
    m_navigationRootEventId = m_rootEventId;
    m_navigationBatches = 0;
    Q_EMIT navigationChanged();
    beginNavigationBatch();
}

void ThreadController::beginNavigationBatch()
{
    if (m_navigationEventId.isEmpty())
        return;
    if (!m_client || navigationStale()) {
        clearNavigation(/*clearMessage=*/true);
        return;
    }
    const QString id = timelineId();
    // canPaginate() is also false while a batch is loading; ride that batch,
    // its completion edge continues the search.
    const bool busy = m_client->paginating(id);
    if (m_navigationBatches >= m_maxNavigationBatches
        || (!busy && !m_client->canPaginate(id))) {
        failNavigation();
        return;
    }
    ++m_navigationBatches;
    m_navigationBatchTimer.start(m_navigationBatchTimeoutMs);
    if (!busy)
        m_model.requestOlder();
    // Re-read so the busy -> idle edge below can be recognised.
    m_modelPaginating = m_client->paginating(id);
}

void ThreadController::continueNavigation()
{
    if (m_navigationEventId.isEmpty())
        return;
    if (navigationStale()) {
        clearNavigation(/*clearMessage=*/true);
        return;
    }
    const int row = m_model.rowForStableId(m_navigationEventId);
    if (row >= 0) {
        locateNavigationTarget(row);
        return;
    }
    if (m_model.paginationFailed()) {
        failNavigation();
        return;
    }
    beginNavigationBatch();
}

void ThreadController::locateNavigationTarget(int row)
{
    const QString eventId = m_navigationEventId;
    clearNavigation(/*clearMessage=*/true);
    setNavigationHighlight(eventId);
    Q_EMIT navigationTargetLocated(row);
}

void ThreadController::failNavigation()
{
    clearNavigation(/*clearMessage=*/false);
    // Reuses the room timeline's message so both surfaces read the same.
    m_navigationMessage = PaginationController::unavailableTargetMessage();
    m_navigationMessageTimer.start(
        PaginationController::kNavigationMessageDurationMs);
    Q_EMIT navigationChanged();
}

void ThreadController::clearNavigation(bool clearMessage)
{
    bool changed = !m_navigationEventId.isEmpty();
    m_navigationEventId.clear();
    m_navigationRoomId.clear();
    m_navigationRootEventId.clear();
    m_navigationBatches = 0;
    m_navigationBatchTimer.stop();
    if (!m_navigationHighlightEventId.isEmpty()) {
        m_navigationHighlightTimer.stop();
        m_navigationHighlightEventId.clear();
        changed = true;
    }
    if (clearMessage && !m_navigationMessage.isEmpty()) {
        m_navigationMessageTimer.stop();
        m_navigationMessage.clear();
        changed = true;
    }
    if (changed)
        Q_EMIT navigationChanged();
}

void ThreadController::setNavigationHighlight(const QString &eventId)
{
    m_navigationHighlightEventId = eventId;
    m_navigationHighlightTimer.start(m_navigationHighlightMs);
    Q_EMIT navigationChanged();
}

bool ThreadController::navigationStale() const
{
    return m_state == Closed || m_roomId != m_navigationRoomId
           || m_rootEventId != m_navigationRootEventId;
}

void ThreadController::setNavigationPolicyForTest(int maxBatches,
                                                  int highlightDurationMs,
                                                  int batchTimeoutMs)
{
    if (maxBatches > 0)
        m_maxNavigationBatches = maxBatches;
    if (highlightDurationMs > 0)
        m_navigationHighlightMs = highlightDurationMs;
    if (batchTimeoutMs > 0)
        m_navigationBatchTimeoutMs = batchTimeoutMs;
}

void ThreadController::setState(State state, const QString &failureCategory)
{
    if (m_state == state && m_failureCategory == failureCategory)
        return;
    m_state = state;
    m_failureCategory = failureCategory;
    Q_EMIT stateChanged();
}
