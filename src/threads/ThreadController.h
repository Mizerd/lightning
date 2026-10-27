#pragma once

#include <functional>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QtQmlIntegration/qqmlintegration.h>

#include "media/ImageSendPrep.h"
#include "models/AttachmentQueueModel.h"
#include "models/MentionTokenizer.h"
#include "models/PaginationController.h"
#include "models/TimelineModel.h"

#include <QVariantMap>

class MatrixClient;

// Lifecycle owner for the single open SDK-backed thread panel.
//
// The backend serves the thread as a normal timeline under a composite id
// (MatrixClient::threadTimelineId), so the rows live in an ordinary
// TimelineModel. This class owns only open/close, generation isolation across
// thread and room switches, state, and the thread send entry points.
//
// Never logs message bodies, tokens, or media URLs.
class ThreadController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    // Exposed only as app.threads; registered so QML can name the State enum.
    QML_UNCREATABLE("ThreadController is exposed via app.threads")
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    Q_PROPERTY(bool traceEnabled READ traceEnabled CONSTANT)
    /// Why the open room is still unread once its main timeline is read: 0 no
    /// thread holds it, 1 unread in a thread, 2 a mention in a thread. A
    /// conservative hint (never a count) that clears when the thread is opened
    /// and the server's counts drop. Drives the header's Threads dot.
    Q_PROPERTY(int roomThreadUnread READ roomThreadUnread
                   NOTIFY roomThreadUnreadChanged)
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    /// Whether the reader is looking at the open thread's newest reply right
    /// now. ThreadPanel writes `followLatest && panel visible && Window.active`;
    /// it reads true only while the thread is Ready. It gates the thread's
    /// read receipt (markRead) and the desktop-notification suppression for
    /// replies in this thread, so a panel left open behind another window
    /// marks nothing read. Becoming true marks the newest reply read.
    Q_PROPERTY(bool viewerAtLatest READ viewerAtLatest WRITE setViewerAtLatest
                   NOTIFY viewerAtLatestChanged)
    /// Photo metadata policy ("location" | "all" | "keep"), pushed in from
    /// qml/Main.qml beside the room composer's. Thread attachments are images
    /// leaving the client like any other.
    Q_PROPERTY(QString imageMetadataMode READ imageMetadataMode
                   WRITE setImageMetadataMode NOTIFY imageMetadataModeChanged)
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)
    Q_PROPERTY(QString roomId READ roomId NOTIFY stateChanged)
    Q_PROPERTY(QString rootEventId READ rootEventId NOTIFY stateChanged)
    // Coarse failure category ("unknown_root", "network", ...). Safe for
    // display; never carries server detail or message content.
    Q_PROPERTY(QString failureCategory READ failureCategory NOTIFY stateChanged)
    Q_PROPERTY(TimelineModel *model READ model CONSTANT)
    /// Number of replies, never negative: the larger of the SDK's
    /// `num_replies` (covers history beyond the lazily paginated window) and
    /// the loaded real events, excluding virtual rows and the root.
    Q_PROPERTY(int replyCount READ replyCount NOTIFY replyCountChanged)
    // -1 until the SDK supplies a total. Loaded rows are only a lower bound.
    Q_PROPERTY(int exactReplyCount READ exactReplyCount NOTIFY replyCountChanged)
    // Thread composer text, kept here so outgoing @-mentions can be tracked.
    // The panel two-way-binds its TextArea to this.
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    // Slash-command refusal, same contract as MessageComposer::commandError:
    // the draft stays and the panel offers sendTextBypassingCommands.
    Q_PROPERTY(QString commandError READ commandError NOTIFY commandErrorChanged)
    // Mention ranges [{start, length}] for the thread composer's chip
    // highlighter; mirrors MessageComposer::mentionRanges.
    Q_PROPERTY(QVariantList mentionRanges READ mentionRanges
                   NOTIFY mentionRangesChanged)
    // Rich reply within the thread. sendText targets it through the SDK
    // thread path.
    Q_PROPERTY(bool inReply READ inReply NOTIFY replyStateChanged)
    Q_PROPERTY(QString replyToEventId READ replyToEventId NOTIFY replyStateChanged)
    Q_PROPERTY(QString replyToSender READ replyToSender NOTIFY replyStateChanged)
    Q_PROPERTY(QString replyToPreview READ replyToPreview NOTIFY replyStateChanged)
    // MSC4306 follow state for the open thread. followSupported stays false
    // until the backend confirms server support; followBusy covers the
    // round-trips.
    Q_PROPERTY(bool followSupported READ followSupported NOTIFY followStateChanged)
    Q_PROPERTY(bool followed READ followed NOTIFY followStateChanged)
    Q_PROPERTY(bool followAutomatic READ followAutomatic NOTIFY followStateChanged)
    Q_PROPERTY(bool followBusy READ followBusy NOTIFY followStateChanged)
    // The room's Threads view (bounded to fetched pages, sorted by latest
    // activity, unread-first where known).
    Q_PROPERTY(bool listOpen READ listOpen NOTIFY listStateChanged)
    Q_PROPERTY(bool listLoading READ listLoading NOTIFY listStateChanged)
    Q_PROPERTY(bool listEndReached READ listEndReached NOTIFY listStateChanged)
    Q_PROPERTY(bool listFailed READ listFailed NOTIFY listStateChanged)
    Q_PROPERTY(QVariantList threadList READ threadList NOTIFY listStateChanged)
    // Thread composer attachment tray (SDK thread-focused send).
    Q_PROPERTY(AttachmentQueueModel *attachments READ attachments CONSTANT)
    Q_PROPERTY(bool hasAttachments READ hasAttachments NOTIFY attachmentsChanged)
    Q_PROPERTY(bool attachmentsSupported READ attachmentsSupported
                   NOTIFY stateChanged)
    // Thread-local reply navigation. Separate from PaginationController
    // because a reply inside a thread must never go through the room history
    // loader; see navigateToEvent(). Empty means nothing is highlighted.
    Q_PROPERTY(QString navigationHighlightEventId READ navigationHighlightEventId
                   NOTIFY navigationChanged)
    // Transient failure text; shares PaginationController's string.
    Q_PROPERTY(QString navigationMessage READ navigationMessage
                   NOTIFY navigationChanged)
    // True while a bounded backward search for an unloaded target runs.
    Q_PROPERTY(bool navigating READ navigating NOTIFY navigationChanged)

public:
    enum State { Closed, Opening, Ready, Failed };
    Q_ENUM(State)

    explicit ThreadController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    bool traceEnabled() const { return m_traceEnabled; }
    // Called only by the thread surface on placement/visibility transitions;
    // values are numeric geometry, with no QML text or identifiers.
    Q_INVOKABLE void tracePresentation(int preset, bool mainView, bool visible,
                                       int viewportWidth, int viewportHeight,
                                       int contentY);
    QString imageMetadataMode() const { return m_imageMetadataMode; }
    void setImageMetadataMode(const QString &mode);
    // Per-thread drafts, keyed on the internal composite id (never leaves the
    // process). Optional.
    void setDraftStore(class DraftStore *store) { m_drafts = store; }

    bool supported() const;
    State state() const { return m_state; }
    bool active() const { return m_state != Closed; }
    bool viewerAtLatest() const { return m_viewerAtLatest && m_state == Ready; }
    void setViewerAtLatest(bool atLatest);
    QString roomId() const { return m_roomId; }
    QString rootEventId() const { return m_rootEventId; }
    QString failureCategory() const { return m_failureCategory; }
    TimelineModel *model() { return &m_model; }
    QString text() const { return m_text; }
    QVariantList mentionRanges() const
    {
        QVariantList out;
        for (const mention::MentionRef &ref : m_mentionRefs) {
            out.append(QVariantMap{
                { QStringLiteral("start"), ref.start },
                { QStringLiteral("length"), ref.length },
            });
        }
        return out;
    }
    void setText(const QString &text);
    bool inReply() const { return !m_replyToEventId.isEmpty(); }
    QString replyToEventId() const { return m_replyToEventId; }
    QString replyToSender() const { return m_replyToSender; }
    QString replyToPreview() const { return m_replyToPreview; }
    bool followSupported() const { return m_followSupported; }
    bool followed() const { return m_followed; }
    bool followAutomatic() const { return m_followAutomatic; }
    bool followBusy() const { return m_followBusy; }
    bool listOpen() const { return m_listOpen; }
    bool listLoading() const { return m_listLoading; }
    bool listEndReached() const { return m_listEndReached; }
    bool listFailed() const { return m_listFailed; }
    QVariantList threadList() const { return m_threadList; }
    AttachmentQueueModel *attachments() const { return m_attachments; }
    bool hasAttachments() const
    { return m_attachments && !m_attachments->isEmpty(); }
    // Attachment sending is available only when the backend supports both
    // attachment upload and live thread timelines.
    bool attachmentsSupported() const;
    QString navigationHighlightEventId() const
    { return m_navigationHighlightEventId; }
    QString navigationMessage() const { return m_navigationMessage; }
    bool navigating() const { return !m_navigationEventId.isEmpty(); }

    // Open (or switch to) the thread rooted at `rootEventId`. Replaces any
    // open thread; stale results from the replaced thread are ignored by
    // composite-id identity.
    Q_INVOKABLE void openThread(const QString &roomId,
                                const QString &rootEventId);
    /// Re-subscribe the open thread's SDK timeline in place (the composer,
    /// draft and reply state are kept). For the moment the SDK's event cache
    /// was emptied under it; a no-op when no thread is open.
    Q_INVOKABLE void reopen();
    Q_INVOKABLE void close();
    // Send a text reply into the open thread through the backend's SDK
    // thread path (never as an ordinary room message). An active reply
    // target turns it into a rich reply within the thread and is cleared
    // after dispatch.
    // The room composer's send-time emoji pass (MessageComposer::
    // applyOutgoingEmoji), so `:shortcode:`s behave the same in a thread.
    // Set by AppController; absent, bodies go out as typed.
    void setOutgoingEmoji(
        std::function<void(QString &body, QVariantMap &bodySpec)> apply)
    { m_outgoingEmoji = std::move(apply); }
    Q_INVOKABLE void sendText(const QString &body);
    // "Send as a message" on the unknown-command refusal: the same send
    // with command parsing skipped.
    Q_INVOKABLE void sendTextBypassingCommands(const QString &body);
    // Sends a pre-composed (plainBody, html, mentions) triple through the
    // thread lane; see MessageComposer::sendPrepared.
    Q_INVOKABLE void sendPrepared(const QString &body, const QString &html,
                                  const QStringList &mentionUserIds);
    QString commandError() const { return m_commandError; }
    // Outgoing @-mentions; mirrors MessageComposer.
    Q_INVOKABLE QVariantMap mentionTokenAt(const QString &text,
                                           int cursorPos) const;
    Q_INVOKABLE int insertMention(const QString &userId,
                                  const QString &displayName,
                                  int tokenStart, int cursorPos);
    // Begin/cancel replying to a specific loaded thread event.
    Q_INVOKABLE void beginReply(const QString &eventId);
    Q_INVOKABLE void cancelReply();
    // Queues a file for the open thread; emits attachmentRejected(reason) on
    // validation failure. Never logs the path.
    Q_INVOKABLE void addAttachment(const QUrl &fileUrl);
    // Thread twin of MessageComposer::addDroppedAttachments: a drop's URLs and
    // its portal key, resolved to readable files, then queued. An answer that
    // arrives after the thread changed is dropped.
    Q_INVOKABLE void addDroppedAttachments(const QList<QUrl> &urls,
                                           const QString &portalKey);
    // Ctrl+V: queues a clipboard image or local file URLs. Returns true when
    // handled so the editor does not also paste.
    Q_INVOKABLE bool pasteFromClipboard();
    // MSC3245 voice message into the open thread (waveform entries 0..=100).
    // Always an m.thread reply via the SDK thread path; no room-send
    // fallback. Failures surface via attachmentRejected for that thread only.
    Q_INVOKABLE void sendVoiceMessage(const QString &localPath,
                                      const QString &mime,
                                      qreal durationMs,
                                      const QVariantList &waveform);
    // Follow/unfollow the open thread (server-side MSC4306 subscription).
    Q_INVOKABLE void setFollowed(bool followed);
    // Sends one threaded read receipt for the open thread's latest readable
    // event (deduplicated; never room-wide). A no-op unless viewerAtLatest,
    // and while a reply search is moving the reader to an older reply.
    Q_INVOKABLE void markRead();
    // The explicit "Mark this thread read" action: one threaded receipt for
    // the newest loaded reply of THIS thread, whether or not the reader is
    // scrolled to it -- the user asked. Never room-wide, deduplicated like
    // markRead, and announced (threadReadSent) only when the server accepts.
    Q_INVOKABLE void markReadNow();
    // Threads view lifecycle for the current room.
    Q_INVOKABLE void openList(const QString &roomId);
    Q_INVOKABLE void closeList();
    Q_INVOKABLE void paginateList();
    // De-duplicated sender MXIDs of the loaded thread events (root first
    // when loaded). Participants of unloaded history are not invented.
    Q_INVOKABLE QStringList participants() const;
    // Presentation data for the pinned root header, from the thread timeline
    // or else the room timeline. {loaded: false} when the root is loaded
    // nowhere. Safe fields only.
    Q_INVOKABLE QVariantMap rootInfo() const;
    int replyCount() const;
    int exactReplyCount() const;
    void notifyReplyCountIfChanged();

    // Reply navigation within the open thread. Paginates this thread's
    // history a bounded number of times and reports when the target cannot be
    // reached. Never touches the room timeline.
    Q_INVOKABLE void navigateToEvent(const QString &eventId);
    // Opens a thread and locates one of its replies once it is Ready: a
    // notification or Activity row for a reply lands on that reply, not
    // wherever the panel was. Runs after every stateChanged handler (the
    // panel's scroll restore included). Dropped if the thread fails, closes
    // or is replaced first. An empty or root `eventId` only opens the thread.
    Q_INVOKABLE void openThreadAt(const QString &roomId,
                                  const QString &rootEventId,
                                  const QString &eventId);

    // Test hook for the navigation policy. Non-positive arguments leave the
    // value unchanged.
    void setNavigationPolicyForTest(int maxBatches, int highlightDurationMs,
                                    int batchTimeoutMs);

    // The active room changed; a thread panel never survives into another
    // room. Called by AppController.
    void handleCurrentRoomChanged(const QString &currentRoomId);
    int roomThreadUnread() const { return m_roomThreadUnread; }

Q_SIGNALS:
    void imageMetadataModeChanged();
    /// The thread's own read receipt was sent: everything in it up to
    /// `timestampMs` is read. The Activity bell mirrors it.
    void threadReadSent(const QString &roomId, const QString &rootEventId,
                        qint64 timestampMs);
    void supportedChanged();
    void roomThreadUnreadChanged();
    void stateChanged();
    void viewerAtLatestChanged();
    void replyCountChanged();
    /// The root's own row changed in place (decrypted, edited, redacted, or
    /// its sender resolved). The panel renders the root from a `rootInfo()`
    /// snapshot, and in-place updates change no row count, so this is what
    /// refreshes it after a late key arrives.
    void rootInfoChanged();
    void textChanged();
    void commandErrorChanged();
    // /markdown and /nick only request the change, as in MessageComposer.
    void composerModeToggleRequested();
    void displayNameChangeRequested(const QString &name);
    void mentionRangesChanged();
    void replyStateChanged();
    void followStateChanged();
    void listStateChanged();
    void attachmentsChanged();
    void attachmentRejected(const QString &reason);
    void navigationChanged();
    // The navigation target resolved to `row` of model(). Like
    // PaginationController::targetLocated but without a pixel offset: the
    // thread panel keeps no saved scroll anchors.
    void navigationTargetLocated(int row);

private Q_SLOTS:
    void onAttachmentQueueFinished(quint64 opId, const QString &roomId,
                                   bool ok, const QString &category);

private:
    // Opt-in, bounded metadata only. The support-log exporter may include
    // these lines, so IDs are hashed before they reach the local log too.
    void traceState(const char *reason) const;
    void setState(State state, const QString &failureCategory = QString());
    // Announces a change of viewerAtLatest() and reads the newest reply on a
    // rising edge.
    void syncViewerAtLatest();
    QString timelineId() const;
    // Send every queued attachment through the SDK thread path. Each becomes
    // its own local echo in the thread timeline.
    // True when at least one queued attachment was handed to the client.
    bool dispatchAttachments();
    // One entry, once it is dispatchable (a video waits for its poster).
    void dispatchAttachment(int row);
    void clearAttachments();

    // Voice send ops in flight. The recording file is deleted when the op
    // resolves; a failure is surfaced only while the same thread is open.
    struct VoiceOp {
        QString localPath;
        QString roomId;
        QString rootEventId;
    };
    QHash<quint64, VoiceOp> m_voiceOps;
    QString m_imageMetadataMode = QStringLiteral("location");
    ImageSendPrep m_imagePrep;

    MatrixClient *m_client = nullptr;
    TimelineModel m_model;
    // De-duplication only; notifyReplyCountIfChanged() recomputes from live
    // state. Not `mutable`, so a const getter can never emit.
    int m_lastReplyCount = -1;
    int m_lastExactReplyCount = -1;
    AttachmentQueueModel *m_attachments = nullptr;
    QHash<QString, QList<AttachmentQueueModel::Entry>> m_committedAttachments;
    State m_state = Closed;
    bool m_traceEnabled = false;
    quint64 m_traceGeneration = 0;
    QString m_roomId;
    QString m_rootEventId;
    // Bumped on sign-out and client change; see MessageComposer's twin.
    quint64 m_dropGeneration = 0;
    QString m_failureCategory;
    QString m_replyToEventId;
    QString m_replyToSender;
    QString m_replyToPreview;
    QString m_text;
    QList<mention::MentionRef> m_mentionRefs;
    void clearComposerText();
    // The single send implementation, the shared thread lane, and the
    // draft-retire tail.
    void sendTextInternal(const QString &body, bool allowCommands);
    void sendThreadBody(const QString &body, const QStringList &mentionIds,
                        const QVariantMap &bodySpec);
    void retireComposerDraft();
    void setCommandError(const QString &error);
    QString m_commandError;
    // Drafts: the debounce stops before every thread change and the save
    // reads the current thread, so a stale timer cannot write across threads.
    void saveDraftNow();
    void restoreDraft();
    class DraftStore *m_drafts = nullptr;
    QTimer m_draftDebounce;
    bool m_restoringDraft = false;
    void resetFollowState();
    // Reply-navigation internals. All of them no-op when no navigation is
    // pending, so they are safe to call from the model signal handlers.
    // The open-time history fill: a thread opens with whatever the event
    // cache holds (often only the newest page), so with no scroll gesture the
    // rest, and the root, never load. Pages backwards, bounded, until the root
    // row is loaded or the start is reached.
    void startHistoryFill();
    void continueHistoryFill();
    void stopHistoryFill();
    void beginNavigationBatch();
    void continueNavigation();
    void locateNavigationTarget(int row);
    void failNavigation();
    void clearNavigation(bool clearMessage);
    void setNavigationHighlight(const QString &eventId);
    // The open thread is no longer the one the navigation started in. Identity
    // is the generation guard, as for the follow answers.
    bool navigationStale() const;
    // Conservative unread hint for a list entry: the loaded room timeline's
    // SDK thread summary for that root, when present.
    bool threadUnreadHint(const QString &rootEventId) const;
    // The open room's thread-unread kind, re-read from the client's room
    // record whenever the room changes or the record does.
    void refreshRoomThreadUnread();
    QString m_currentRoomId;
    int m_roomThreadUnread = 0;
    bool m_followSupported = false;
    bool m_followed = false;
    bool m_followAutomatic = false;
    bool m_followBusy = false;
    QString m_lastMarkedReadEventId;
    // What ThreadPanel last wrote; viewerAtLatest() also requires Ready.
    bool m_viewerAtLatest = false;
    // The value last announced through viewerAtLatestChanged.
    bool m_viewerAtLatestAnnounced = false;
    // openThreadAt(): the reply to locate once that thread is Ready.
    QString m_pendingNavigationEventId;
    QString m_pendingNavigationRoomId;
    QString m_pendingNavigationRootEventId;
    // Thread receipts sent and not yet answered by the server, by the request
    // id each was sent with (the answers echo it, because separate receipts
    // can complete out of order). Cleared on sign-out.
    struct PendingThreadRead {
        QString roomId;
        QString rootId;
        QString eventId;
        qint64 timestampMs = 0;
    };
    QHash<quint64, PendingThreadRead> m_pendingThreadReads;
    quint64 m_nextThreadReadId = 1;
    bool m_listOpen = false;
    bool m_listLoading = false;
    bool m_listEndReached = false;
    bool m_listFailed = false;
    QString m_listRoomId;
    QVariantList m_threadList;

    // Non-empty m_navigationEventId means a search is in flight; its room/root
    // pair is the staleness gate for every completion.
    QString m_navigationEventId;
    QString m_navigationRoomId;
    QString m_navigationRootEventId;
    int m_navigationBatches = 0;
    bool m_fillActive = false;
    int m_fillBatches = 0;
    int m_maxFillBatches = 8;
    int m_maxNavigationBatches = 8;   // matches kMaxNavigationBatches
    QString m_navigationHighlightEventId;
    QString m_navigationMessage;
    QTimer m_navigationHighlightTimer;
    QTimer m_navigationMessageTimer;
    // Per-batch watchdog, so a request the backend drops silently still ends
    // with a visible result.
    QTimer m_navigationBatchTimer;
    int m_navigationHighlightMs =
        PaginationController::kDefaultHighlightDurationMs;
    int m_navigationBatchTimeoutMs = 8000;
    // Mirrors TimelineModel::paginating() to detect the completion edge.
    bool m_modelPaginating = false;
    std::function<void(QString &, QVariantMap &)> m_outgoingEmoji;
};
