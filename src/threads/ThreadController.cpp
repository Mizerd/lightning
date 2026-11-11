#include "threads/ThreadController.h"

#include "models/SlashCommands.h"

#include "app/DraftStore.h"
#include "app/JoinErrorText.h"
#include "media/ImageMetadata.h"
#include "matrix/MatrixClient.h"
#include "models/UserLookup.h"
#include "crypto/E2eeDiagnostics.h"

#include <QBuffer>
#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QLoggingCategory>
#include <QMimeData>

#include <algorithm>

namespace {
constexpr int kMaxPasteEdge = 4096;
Q_LOGGING_CATEGORY(lcThread, "matrix.thread")
}

ThreadController::ThreadController(QObject *parent)
    : QObject(parent)
    , m_attachments(new AttachmentQueueModel(this))
{
    m_traceEnabled = qEnvironmentVariableIsSet("LIGHTNING_THREAD_TRACE");
    connect(m_attachments, &AttachmentQueueModel::countChanged, this,
            [this] { Q_EMIT attachmentsChanged(); });
    connect(m_attachments, &AttachmentQueueModel::entryPrepared,
            this, &ThreadController::dispatchAttachment);
    m_draftDebounce.setSingleShot(true);
    m_draftDebounce.setInterval(1000);
    connect(&m_draftDebounce, &QTimer::timeout, this,
            [this] { saveDraftNow(); });

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
        if (!m_navigationEventId.isEmpty())
            failNavigation();
    });
    connect(&m_model, &TimelineModel::olderPrepended, this, [this](int) {
        if (m_navigationEventId.isEmpty())
            return;
        if (navigationStale()) {
            clearNavigation(true);
            return;
        }
        const int row = m_model.rowForStableId(m_navigationEventId);
        if (row >= 0)
            locateNavigationTarget(row);
    });
    connect(&m_model, &TimelineModel::countChanged, this,
            &ThreadController::notifyReplyCountIfChanged);
    connect(this, &ThreadController::stateChanged, this,
            &ThreadController::notifyReplyCountIfChanged);
    connect(&m_model, &TimelineModel::dataChanged, this,
            [this](const QModelIndex &topLeft, const QModelIndex &bottomRight,
                   const QList<int> &roles) {
                if (!roles.isEmpty()
                    && !roles.contains(TimelineModel::ThreadReplyCountRole))
                    return;
                const auto &events = m_model.events();
                const int lo = qMax(0, topLeft.row());
                const int hi =
                    qMin(bottomRight.row(), static_cast<int>(events.size()) - 1);
                for (int row = lo; row <= hi; ++row) {
                    if (events.at(row).eventId == m_rootEventId) {
                        notifyReplyCountIfChanged();
                        Q_EMIT rootInfoChanged();
                        return;
                    }
                }
            });
    connect(&m_model, &TimelineModel::paginationChanged, this, [this] {
        const bool wasPaginating = m_modelPaginating;
        m_modelPaginating = m_model.paginating();
        if (m_fillActive && wasPaginating && !m_modelPaginating)
            continueHistoryFill();
        if (m_navigationEventId.isEmpty() || m_modelPaginating)
            return;
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

void ThreadController::setImageMetadataMode(const QString &mode)
{
    const QString normalized =
        imagemeta::modeToString(imagemeta::modeFromString(mode));
    if (m_imageMetadataMode == normalized)
        return;
    m_imageMetadataMode = normalized;
    Q_EMIT imageMetadataModeChanged();
}

void ThreadController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    ++m_dropGeneration;
    close();
    m_imagePrep.releaseAll();
    m_pendingThreadReads.clear();
    m_client = client;
    m_slashJoinOp = 0;
    refreshRoomThreadUnread();
    m_model.setClient(client);
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
        connect(m_client, &MatrixClient::attachmentQueueFinished, this,
                &ThreadController::onAttachmentQueueFinished);
        connect(m_client, &MatrixClient::roomsChanged, this,
                &ThreadController::refreshRoomThreadUnread);
        connect(m_client, &MatrixClient::roomUpdated, this,
                [this](const QString &roomId) {
                    if (roomId == m_currentRoomId)
                        refreshRoomThreadUnread();
                });
        connect(m_client, &MatrixClient::loggedOut, this, [this] {
            ++m_dropGeneration;
            // Unresolved thread recordings must not outlive the session on
            // disk. Mirrors MessageComposer's cleanup.
            for (const VoiceOp &op : std::as_const(m_voiceOps))
                QFile::remove(op.localPath);
            m_voiceOps.clear();
            m_imagePrep.releaseAll();
        });
        connect(m_client, &MatrixClient::timelineReset, this,
                [this](const QString &timelineId) {
                    if (timelineId != this->timelineId()) {
                        if (m_state != Closed)
                            traceState("stale_reset");
                        return;
                    }
                    if (m_state == Opening)
                        setState(Ready);
                    if (m_state == Ready)
                        startHistoryFill();
                });
        connect(m_client, &MatrixClient::threadTimelineFailed, this,
                [this](const QString &roomId, const QString &rootEventId,
                       const QString &category) {
                    if (m_state != Closed && roomId == m_roomId
                        && rootEventId == m_rootEventId)
                        setState(Failed, category);
                    else if (m_state != Closed)
                        traceState("stale_failure");
                });
        connect(m_client, &MatrixClient::threadSubscriptionState, this,
                [this](const QString &roomId, const QString &rootEventId,
                       bool supported, bool subscribed, bool automatic) {
                    if (m_state == Closed || roomId != m_roomId
                        || rootEventId != m_rootEventId)
                        return;
                    m_followSupported = supported;
                    m_followed = supported && subscribed;
                    m_followAutomatic = supported && automatic;
                    m_followBusy = false;
                    Q_EMIT followStateChanged();
                });
        connect(m_client, &MatrixClient::threadSubscriptionResult, this,
                [this](const QString &roomId, const QString &rootEventId,
                       bool ok, bool) {
                    if (m_state == Closed || roomId != m_roomId
                        || rootEventId != m_rootEventId)
                        return;
                    if (!ok) {
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
                        return;
                    QVariantList sorted = threads;
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
                [this] {
                    m_pendingThreadReads.clear();
                    close();
                    closeList();
                    discardAllAttachments();
                });
        connect(m_client, &MatrixClient::threadReadMarkerAdvanced, this,
                [this](const QString &, const QString &, quint64 opId) {
                    const auto it = m_pendingThreadReads.find(opId);
                    if (it == m_pendingThreadReads.end())
                        return;
                    const PendingThreadRead done = it.value();
                    m_pendingThreadReads.erase(it);
                    traceState("receipt_ack");
                    Q_EMIT threadReadSent(done.roomId, done.rootId,
                                          done.timestampMs);
                });
        connect(m_client, &MatrixClient::threadMarkReadFailed, this,
                [this](const QString &, const QString &, quint64 opId) {
                    const auto it = m_pendingThreadReads.find(opId);
                    if (it == m_pendingThreadReads.end())
                        return;
                    const PendingThreadRead failed = it.value();
                    m_pendingThreadReads.erase(it);
                    traceState("receipt_failed");
                    if (failed.roomId == m_roomId && failed.rootId == m_rootEventId
                        && m_lastMarkedReadEventId == failed.eventId)
                        m_lastMarkedReadEventId.clear();
                });
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
        return;

    m_draftDebounce.stop();
    saveDraftNow();

    parkAttachments();

    stopHistoryFill();
    ++m_traceGeneration;
    m_roomId = roomId;
    m_slashJoinOp = 0;
    m_rootEventId = rootEventId;
    m_failureCategory.clear();
    cancelReply();
    clearComposerText();
    restoreDraft();
    m_attachments->restoreEntries(m_parkedAttachments.take(timelineId()));
    clearNavigation(true);
    m_model.setRoomId(timelineId());
    resetFollowState();
    m_lastMarkedReadEventId.clear();
    setState(Opening);
    m_client->openThread(roomId, rootEventId);
    m_followBusy = true;
    Q_EMIT followStateChanged();
    m_client->queryThreadSubscription(roomId, rootEventId);
}

void ThreadController::reopen()
{
    if (!supported() || m_state == Closed || m_roomId.isEmpty()
        || m_rootEventId.isEmpty())
        return;
    setState(Opening);
    m_client->openThread(m_roomId, m_rootEventId);
}

void ThreadController::close()
{
    m_draftDebounce.stop();
    saveDraftNow();
    parkAttachments();
    stopHistoryFill();
    const bool wasActive = m_state != Closed;
    if (wasActive)
        traceState("close_requested");
    if (m_client && wasActive)
        m_client->closeThread();
    m_roomId.clear();
    m_slashJoinOp = 0;
    m_rootEventId.clear();
    m_failureCategory.clear();
    m_model.setRoomId(QString{});
    cancelReply();
    clearComposerText();
    clearNavigation(true);
    resetFollowState();
    m_lastMarkedReadEventId.clear();
    for (const VoiceOp &op : std::as_const(m_voiceOps))
        QFile::remove(op.localPath);
    m_voiceOps.clear();
    if (wasActive) {
        m_lastReplyCount = -1;
        setState(Closed);
    }
}

void ThreadController::sendText(const QString &body)
{
    sendTextInternal(body,true);
}

void ThreadController::sendTextBypassingCommands(const QString &body)
{
    sendTextInternal(body,false);
}

void ThreadController::sendTextInternal(const QString &body, bool allowCommands)
{
    if (!m_client || m_state == Closed || m_roomId.isEmpty()
        || m_rootEventId.isEmpty())
        return;
    const mention::Expansion expansion = mention::expand(m_text, m_mentionRefs);
    QString outBody = expansion.body.trimmed();
    if (outBody.isEmpty())
        outBody = body.trimmed();
    const QStringList mentionIds = expansion.userIds;

    if (allowCommands && !outBody.isEmpty()) {
        const SlashCommands::Parse parsed = SlashCommands::parse(outBody);
        switch (parsed.kind) {
        case SlashCommands::Parse::NotACommand:
            break;
        case SlashCommands::Parse::Escaped:
            outBody = parsed.literalText.trimmed();
            break;
        case SlashCommands::Parse::Unknown:
            break;
        case SlashCommands::Parse::Known: {
            SlashCommands::Actions actions;
            actions.send = [this](const QString &b, const QStringList &ids,
                                  const QVariantMap &spec) {
                sendThreadBody(b, ids, spec);
            };
            actions.join = [this](const QString &target) {
                m_slashJoinOp =
                    m_client->joinRoomByIdOrAlias(target, QStringList());
                if (m_slashJoinOp == 0) {
                    setCommandError(joinerror::describe(
                        m_client->supportsJoinByAddress()
                            ? QStringLiteral("network")
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

    const bool dispatched = dispatchAttachments();
    if (outBody.isEmpty()) {
        if (dispatched)
            cancelReply();
        return;
    }
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
    sendThreadBody(body, mentionUserIds,
                   html.isEmpty()
                       ? QVariantMap{ { QStringLiteral("format"),
                                        QStringLiteral("plain") } }
                       : QVariantMap{ { QStringLiteral("format"),
                                        QStringLiteral("html") },
                                      { QStringLiteral("html"), html } });
    retireComposerDraft();
}

void ThreadController::sendThreadBody(const QString &bodyIn,
                                      const QStringList &mentionIds,
                                      const QVariantMap &bodySpecIn)
{
    QString body = bodyIn;
    QVariantMap bodySpec = bodySpecIn;
    if (m_outgoingEmoji)
        m_outgoingEmoji(body, bodySpec);
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

void ThreadController::addDroppedAttachments(const QList<QUrl> &urls,
                                             const QString &portalKey)
{
    if (!attachmentsSupported()) {
        Q_EMIT attachmentRejected(
            tr("Attachments are not supported on this backend."));
        return;
    }
    // Room and root alone are not enough across a sign-out: session, client
    // and account must all still match.
    const QString roomId = m_roomId;
    const QString rootId = m_rootEventId;
    const quint64 generation = m_dropGeneration;
    const MatrixClient *client = m_client;
    const QString userId = m_client->currentUserId();
    AttachmentQueueModel::resolveDroppedUrls(
        urls, portalKey, this,
        [this, roomId, rootId, generation, client, userId](
            const QList<QUrl> &resolved) {
            if (m_roomId != roomId || m_rootEventId != rootId
                || m_dropGeneration != generation || m_client != client
                || !m_client || m_client->currentUserId() != userId)
                return;
            if (resolved.isEmpty()) {
                Q_EMIT attachmentRejected(tr("That file cannot be read."));
                return;
            }
            for (const QUrl &url : resolved)
                addAttachment(url);
        });
}

bool ThreadController::pasteFromClipboard()
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
            Q_EMIT attachmentRejected(
                tr("The clipboard image could not be read."));
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

bool ThreadController::dispatchAttachments()
{
    if (!m_client || m_state != Ready || m_roomId.isEmpty()
        || m_rootEventId.isEmpty())
        return false;
    bool any = false;
    auto &entries = m_attachments->entries();
    for (int row = 0; row < entries.size(); ++row) {
        if (entries[row].state != QLatin1String("queued"))
            continue;
        entries[row].sendRequested = true;
        if (!m_replyToEventId.isEmpty())
            entries[row].replyToEventId = m_replyToEventId;
        any = true;
        dispatchAttachment(row);
    }
    return any;
}

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
    // The same metadata policy as the room composer, at this send path's own
    // choke point (a thread image used to leave with its GPS block intact).
    QString sendPath = entry.localPath;
    QByteArray sendData = entry.data;
    QString strippedSub;
    if (!entry.isVideo && !entry.isSvg
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
            m_roomId, m_rootEventId,
            entry.localPath.isEmpty() ? QString() : sendPath, sendData,
            entry.fileName, entry.mime, QString(), entry.width, entry.height,
            entry.durationMs, entry.animated, entry.poster, entry.posterWidth,
            entry.posterHeight, entry.replyToEventId);
    } else if (entry.localPath.isEmpty()) {
        opId = m_client->sendThreadAttachmentBytes(
            m_roomId, m_rootEventId, sendData, entry.fileName, entry.mime,
            entry.width, entry.height, entry.replyToEventId);
    } else if (entry.isVideo) {
        opId = m_client->sendThreadVideo(
            m_roomId, m_rootEventId, entry.localPath, entry.mime, QString(),
            entry.width, entry.height, entry.durationMs, entry.poster,
            entry.posterWidth, entry.posterHeight, entry.replyToEventId);
    } else if (entry.isSvg) {
        opId = 0;
    } else {
        opId = m_client->sendThreadAttachment(
            m_roomId, m_rootEventId, sendPath, entry.mime, QString(),
            entry.width, entry.height, entry.animated, entry.durationMs,
            entry.replyToEventId);
    }
    if (opId == 0) {
        ImageSendPrep::discard(strippedSub);
        entry.state = QStringLiteral("failed");
        entry.error = tr("The attachment could not be queued.");
    } else {
        entry.state = QStringLiteral("dispatching");
        entry.opId = opId;
        entry.committed = true;
        entry.threadRootId = m_rootEventId;
        m_imagePrep.track(opId, strippedSub);
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
    const QString targetRoot = m_rootEventId;
    const quint64 opId = m_client->sendThreadVoiceMessage(
        targetRoom, targetRoot, localPath, mime,
        static_cast<qint64>(durationMs), amplitudes, m_replyToEventId);
    if (opId == 0) {
        // Never retried as a room send: a thread voice message must not land
        // in the main timeline.
        QFile::remove(localPath);
        Q_EMIT attachmentRejected(tr("The voice message could not be sent."));
        return;
    }
    m_voiceOps.insert(opId, VoiceOp{localPath, targetRoom, targetRoot});
    cancelReply();
}

void ThreadController::onAttachmentQueueFinished(quint64 opId,
                                                 const QString &roomId,
                                                 bool ok,
                                                 const QString &category)
{
    Q_UNUSED(category);
    if (opId == 0)
        return;
    m_imagePrep.finished(opId);
    if (const auto voiceIt = m_voiceOps.constFind(opId);
        voiceIt != m_voiceOps.constEnd()) {
        const VoiceOp op = voiceIt.value();
        QFile::remove(op.localPath);
        m_voiceOps.erase(voiceIt);
        if (!ok && op.roomId == m_roomId && op.rootEventId == m_rootEventId)
            Q_EMIT attachmentRejected(
                tr("The voice message could not be sent."));
        return;
    }
    auto &entries = m_attachments->entries();
    for (int row = 0; row < entries.size(); ++row) {
        if (entries[row].opId != opId
            || (!roomId.isEmpty() && roomId != m_roomId))
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
        return;
    }
    for (auto it = m_parkedAttachments.begin();
         it != m_parkedAttachments.end(); ++it) {
        for (int row = 0; row < it.value().size(); ++row) {
            auto &entry = it.value()[row];
            if (entry.opId != opId
                || (!roomId.isEmpty()
                    && MatrixClient::threadTimelineId(roomId, entry.threadRootId)
                           != it.key()))
                continue;
            if (ok) {
                m_attachments->discardEntries({entry});
                it.value().removeAt(row);
                if (it.value().isEmpty())
                    m_parkedAttachments.erase(it);
            } else {
                entry.state = QStringLiteral("failed");
                entry.error = tr("Upload failed. Retry or remove.");
                entry.opId = 0;
            }
            return;
        }
    }
}

void ThreadController::parkAttachments()
{
    const QList<AttachmentQueueModel::Entry> entries = m_attachments->takeAll();
    const QString key = timelineId();
    if (key.isEmpty())
        m_attachments->discardEntries(entries);
    else if (!entries.isEmpty())
        m_parkedAttachments[key].append(entries);
}

void ThreadController::discardAllAttachments()
{
    m_attachments->clearAll();
    for (const auto &entries : std::as_const(m_parkedAttachments))
        m_attachments->discardEntries(entries);
    m_parkedAttachments.clear();
}

void ThreadController::beginReply(const QString &eventId)
{
    if (m_state == Closed || eventId.isEmpty())
        return;
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
    m_replyToPreview = m_model.replyPreviewForEvent(eventId).left(80);
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
    const int known = exactReplyCount();
    const int rootRow = m_model.rowForStableId(m_rootEventId);
    const int loaded = qMax(0, m_model.realEventCount() - (rootRow >= 0 ? 1 : 0));
    return qMax(known, loaded);
}

int ThreadController::exactReplyCount() const
{
    const auto summaryFor = [](const QList<TimelineEvent> &events,
                               const QString &rootId) {
        for (const auto &e : events)
            if (e.eventId == rootId)
                return e.threadReplyCount;
        return -1;
    };

    const int rootRow = m_model.rowForStableId(m_rootEventId);
    int known = summaryFor(m_model.events(), m_rootEventId);
    if (rootRow < 0 && known < 0 && m_client)
        known = summaryFor(m_client->timeline(m_roomId), m_rootEventId);

    return known;
}

void ThreadController::notifyReplyCountIfChanged()
{
    const int now = replyCount();
    const int exact = exactReplyCount();
    if (now == m_lastReplyCount && exact == m_lastExactReplyCount)
        return;
    m_lastReplyCount = now;
    m_lastExactReplyCount = exact;
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
    for (const QVariant &entry : m_threadList) {
        const QVariantMap thread = entry.toMap();
        if (thread.value(QStringLiteral("rootEventId")).toString()
            != m_rootEventId)
            continue;
        info.insert(QStringLiteral("loaded"), true);
        info.insert(QStringLiteral("eventId"), m_rootEventId);
        info.insert(QStringLiteral("sender"),
                    thread.value(QStringLiteral("rootSender")));
        info.insert(QStringLiteral("senderDisplayName"),
                    thread.value(QStringLiteral("rootSenderName")));
        info.insert(QStringLiteral("body"),
                    thread.value(QStringLiteral("rootPreview")));
        info.insert(QStringLiteral("timestamp"),
                    thread.value(QStringLiteral("rootTimestamp")));
        return info;
    }
    info.insert(QStringLiteral("pending"), m_state == Opening || m_fillActive);
    return info;
}

void ThreadController::refreshRoomThreadUnread()
{
    int kind = 0;
    if (m_client && !m_currentRoomId.isEmpty()) {
        const RoomInfo room = m_client->roomInfo(m_currentRoomId);
        if (room.id == m_currentRoomId && room.membership == RoomInfo::Joined
            && room.unreadThreadsOnly)
            kind = room.mentionInThreads ? 2 : 1;
    }
    if (kind == m_roomThreadUnread)
        return;
    m_roomThreadUnread = kind;
    Q_EMIT roomThreadUnreadChanged();
}

void ThreadController::handleCurrentRoomChanged(const QString &currentRoomId)
{
    m_currentRoomId = currentRoomId;
    refreshRoomThreadUnread();
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

void ThreadController::setViewerAtLatest(bool atLatest)
{
    if (m_viewerAtLatest == atLatest)
        return;
    m_viewerAtLatest = atLatest;
    syncViewerAtLatest();
}

void ThreadController::syncViewerAtLatest()
{
    const bool now = viewerAtLatest();
    if (now == m_viewerAtLatestAnnounced)
        return;
    m_viewerAtLatestAnnounced = now;
    Q_EMIT viewerAtLatestChanged();
    if (now)
        markRead();
}

void ThreadController::markRead()
{
    if (!m_client || m_state != Ready) {
        traceState("receipt_not_ready");
        return;
    }
    if (!viewerAtLatest()) {
        traceState("receipt_view_hidden_or_old");
        return;
    }
    if (!m_pendingNavigationEventId.isEmpty() || !m_navigationEventId.isEmpty()) {
        traceState("receipt_navigation_pending");
        return;
    }
    qint64 latestMs = 0;
    const QString latest = m_model.latestReadableEventId(&latestMs);
    if (latest.isEmpty() || latest == m_lastMarkedReadEventId)
        return;
    m_lastMarkedReadEventId = latest;
    const quint64 opId = m_nextThreadReadId++;
    m_pendingThreadReads.insert(
        opId, {m_roomId, m_rootEventId, latest, latestMs});
    traceState("receipt_pending_ack");
    m_client->markThreadRead(m_roomId, m_rootEventId, opId);
}

void ThreadController::markReadNow()
{
    if (!m_client || m_state != Ready)
        return;
    qint64 latestMs = 0;
    const QString latest = m_model.latestReadableEventId(&latestMs);
    if (latest.isEmpty() || latest == m_lastMarkedReadEventId)
        return;
    m_lastMarkedReadEventId = latest;
    const quint64 opId = m_nextThreadReadId++;
    m_pendingThreadReads.insert(
        opId, {m_roomId, m_rootEventId, latest, latestMs});
    m_client->markThreadRead(m_roomId, m_rootEventId, opId);
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

void ThreadController::navigateToEvent(const QString &eventId)
{
    if (eventId.isEmpty() || m_state == Closed)
        return;
    if (eventId == m_rootEventId) {
        clearNavigation(true);
        setNavigationHighlight(eventId);
        return;
    }
    const int loadedRow = m_model.rowForStableId(eventId);
    if (loadedRow >= 0) {
        clearNavigation(true);
        setNavigationHighlight(eventId);
        Q_EMIT navigationTargetLocated(loadedRow);
        return;
    }
    if (m_navigationEventId == eventId)
        return;
    clearNavigation(true);
    m_navigationEventId = eventId;
    m_navigationRoomId = m_roomId;
    m_navigationRootEventId = m_rootEventId;
    m_navigationBatches = 0;
    Q_EMIT navigationChanged();
    beginNavigationBatch();
}

void ThreadController::startHistoryFill()
{
    m_fillActive = true;
    m_fillBatches = 0;
    Q_EMIT rootInfoChanged();
    continueHistoryFill();
}

void ThreadController::stopHistoryFill()
{
    const bool wasActive = m_fillActive;
    m_fillActive = false;
    m_fillBatches = 0;
    if (wasActive)
        Q_EMIT rootInfoChanged();
}

void ThreadController::continueHistoryFill()
{
    if (!m_fillActive)
        return;
    if (!m_client || m_state != Ready || !m_navigationEventId.isEmpty()
        || m_model.rowForStableId(m_rootEventId) >= 0) {
        traceState(m_model.rowForStableId(m_rootEventId) >= 0
                       ? "root_loaded" : "fill_paused");
        stopHistoryFill();
        return;
    }
    const QString id = timelineId();
    if (m_client->paginating(id))
        return;
    if (m_fillBatches >= m_maxFillBatches || !m_client->canPaginate(id)) {
        traceState(m_fillBatches >= m_maxFillBatches
                       ? "fill_limit" : "history_exhausted");
        stopHistoryFill();
        return;
    }
    ++m_fillBatches;
    traceState("page_requested");
    m_model.requestOlder();
    m_modelPaginating = m_client->paginating(id);
}

void ThreadController::beginNavigationBatch()
{
    if (m_navigationEventId.isEmpty())
        return;
    if (!m_client || navigationStale()) {
        clearNavigation(true);
        return;
    }
    const QString id = timelineId();
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
    m_modelPaginating = m_client->paginating(id);
}

void ThreadController::continueNavigation()
{
    if (m_navigationEventId.isEmpty())
        return;
    if (navigationStale()) {
        clearNavigation(true);
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
    clearNavigation(true);
    setNavigationHighlight(eventId);
    Q_EMIT navigationTargetLocated(row);
}

void ThreadController::failNavigation()
{
    clearNavigation(false);
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
    traceState(state == Failed ? "session_failed" : "state_changed");
    Q_EMIT stateChanged();
    bool landed = false;
    if (!m_pendingNavigationEventId.isEmpty()) {
        const bool sameThread = m_roomId == m_pendingNavigationRoomId
            && m_rootEventId == m_pendingNavigationRootEventId;
        if (!sameThread || m_state == Closed || m_state == Failed) {
            m_pendingNavigationEventId.clear();
        } else if (m_state == Ready) {
            const QString eventId = m_pendingNavigationEventId;
            m_pendingNavigationEventId.clear();
            navigateToEvent(eventId);
            landed = true;
        }
    }
    syncViewerAtLatest();
    if (landed && viewerAtLatest())
        markRead();
}

void ThreadController::openThreadAt(const QString &roomId,
                                    const QString &rootEventId,
                                    const QString &eventId)
{
    if (!supported() || roomId.isEmpty() || rootEventId.isEmpty())
        return;
    m_pendingNavigationEventId.clear();
    const bool alreadyOpen = m_state != Closed && m_state != Failed
        && m_roomId == roomId && m_rootEventId == rootEventId;
    if (!alreadyOpen)
        openThread(roomId, rootEventId);
    if (eventId.isEmpty() || eventId == rootEventId)
        return;
    if (m_roomId != roomId || m_rootEventId != rootEventId)
        return;
    if (m_state == Ready) {
        navigateToEvent(eventId);
        return;
    }
    m_pendingNavigationEventId = eventId;
    m_pendingNavigationRoomId = roomId;
    m_pendingNavigationRootEventId = rootEventId;
}

void ThreadController::traceState(const char *reason) const
{
    if (!m_traceEnabled)
        return;
    qCInfo(lcThread).nospace()
        << "thread_diag reason=" << reason
        << " generation=" << m_traceGeneration
        << " room=" << matrix::e2ee::redactId(m_roomId)
        << " root=" << matrix::e2ee::redactId(m_rootEventId)
        << " state=" << static_cast<int>(m_state)
        << " loaded_rows=" << m_model.rowCount()
        << " root_loaded=" << (m_model.rowForStableId(m_rootEventId) >= 0)
        << " paginating=" << m_model.paginating()
        << " pagination_failed=" << m_model.paginationFailed()
        << " fill_pages=" << m_fillBatches
        << " visible_read_owner=" << viewerAtLatest()
        << " receipt_scope=thread pending_acks=" << m_pendingThreadReads.size();
}

void ThreadController::tracePresentation(int preset, bool mainView, bool visible,
                                         int viewportWidth, int viewportHeight,
                                         int contentY)
{
    if (!m_traceEnabled)
        return;
    traceState("presentation");
    qCInfo(lcThread).nospace()
        << "thread_layout generation=" << m_traceGeneration
        << " preset=" << (preset == 1 ? 1 : 0)
        << " placement=" << (mainView ? "main" : "side")
        << " visible=" << visible
        << " viewport=" << qMax(0, viewportWidth) << 'x' << qMax(0, viewportHeight)
        << " content_y=" << contentY;
}
