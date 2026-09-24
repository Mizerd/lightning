#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QtQmlIntegration/qqmlintegration.h>

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
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
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
    // Per-thread drafts, keyed on the internal composite id (never leaves the
    // process). Optional.
    void setDraftStore(class DraftStore *store) { m_drafts = store; }

    bool supported() const;
    State state() const { return m_state; }
    bool active() const { return m_state != Closed; }
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
    Q_INVOKABLE void close();
    // Send a text reply into the open thread through the backend's SDK
    // thread path (never as an ordinary room message). An active reply
    // target turns it into a rich reply within the thread and is cleared
    // after dispatch.
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
    // event (deduplicated; never room-wide).
    Q_INVOKABLE void markRead();
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
    void notifyReplyCountIfChanged();

    // Reply navigation within the open thread. Paginates this thread's
    // history a bounded number of times and reports when the target cannot be
    // reached. Never touches the room timeline.
    Q_INVOKABLE void navigateToEvent(const QString &eventId);

    // Test hook for the navigation policy. Non-positive arguments leave the
    // value unchanged.
    void setNavigationPolicyForTest(int maxBatches, int highlightDurationMs,
                                    int batchTimeoutMs);

    // The active room changed; a thread panel never survives into another
    // room. Called by AppController.
    void handleCurrentRoomChanged(const QString &currentRoomId);

Q_SIGNALS:
    void supportedChanged();
    void stateChanged();
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
    void setState(State state, const QString &failureCategory = QString());
    QString timelineId() const;
    // Send every queued attachment through the SDK thread path. Each becomes
    // its own local echo in the thread timeline.
    void dispatchAttachments();
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

    MatrixClient *m_client = nullptr;
    TimelineModel m_model;
    // De-duplication only; notifyReplyCountIfChanged() recomputes from live
    // state. Not `mutable`, so a const getter can never emit.
    int m_lastReplyCount = -1;
    AttachmentQueueModel *m_attachments = nullptr;
    State m_state = Closed;
    QString m_roomId;
    QString m_rootEventId;
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
    bool m_followSupported = false;
    bool m_followed = false;
    bool m_followAutomatic = false;
    bool m_followBusy = false;
    QString m_lastMarkedReadEventId;
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
};
