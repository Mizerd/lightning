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
    QML_UNCREATABLE("ThreadController is exposed via app.threads")
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    Q_PROPERTY(bool traceEnabled READ traceEnabled CONSTANT)
    Q_PROPERTY(int roomThreadUnread READ roomThreadUnread
                   NOTIFY roomThreadUnreadChanged)
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool viewerAtLatest READ viewerAtLatest WRITE setViewerAtLatest
                   NOTIFY viewerAtLatestChanged)
    Q_PROPERTY(QString imageMetadataMode READ imageMetadataMode
                   WRITE setImageMetadataMode NOTIFY imageMetadataModeChanged)
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)
    Q_PROPERTY(QString roomId READ roomId NOTIFY stateChanged)
    Q_PROPERTY(QString rootEventId READ rootEventId NOTIFY stateChanged)
    Q_PROPERTY(QString failureCategory READ failureCategory NOTIFY stateChanged)
    Q_PROPERTY(TimelineModel *model READ model CONSTANT)
    Q_PROPERTY(int replyCount READ replyCount NOTIFY replyCountChanged)
    Q_PROPERTY(int exactReplyCount READ exactReplyCount NOTIFY replyCountChanged)
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    Q_PROPERTY(QString commandError READ commandError NOTIFY commandErrorChanged)
    Q_PROPERTY(QVariantList mentionRanges READ mentionRanges
                   NOTIFY mentionRangesChanged)
    Q_PROPERTY(bool inReply READ inReply NOTIFY replyStateChanged)
    Q_PROPERTY(QString replyToEventId READ replyToEventId NOTIFY replyStateChanged)
    Q_PROPERTY(QString replyToSender READ replyToSender NOTIFY replyStateChanged)
    Q_PROPERTY(QString replyToPreview READ replyToPreview NOTIFY replyStateChanged)
    Q_PROPERTY(bool followSupported READ followSupported NOTIFY followStateChanged)
    Q_PROPERTY(bool followed READ followed NOTIFY followStateChanged)
    Q_PROPERTY(bool followAutomatic READ followAutomatic NOTIFY followStateChanged)
    Q_PROPERTY(bool followBusy READ followBusy NOTIFY followStateChanged)
    Q_PROPERTY(bool listOpen READ listOpen NOTIFY listStateChanged)
    Q_PROPERTY(bool listLoading READ listLoading NOTIFY listStateChanged)
    Q_PROPERTY(bool listEndReached READ listEndReached NOTIFY listStateChanged)
    Q_PROPERTY(bool listFailed READ listFailed NOTIFY listStateChanged)
    Q_PROPERTY(QVariantList threadList READ threadList NOTIFY listStateChanged)
    Q_PROPERTY(AttachmentQueueModel *attachments READ attachments CONSTANT)
    Q_PROPERTY(bool hasAttachments READ hasAttachments NOTIFY attachmentsChanged)
    Q_PROPERTY(bool attachmentsSupported READ attachmentsSupported
                   NOTIFY stateChanged)
    Q_PROPERTY(QString navigationHighlightEventId READ navigationHighlightEventId
                   NOTIFY navigationChanged)
    Q_PROPERTY(QString navigationMessage READ navigationMessage
                   NOTIFY navigationChanged)
    Q_PROPERTY(bool navigating READ navigating NOTIFY navigationChanged)

public:
    enum State { Closed, Opening, Ready, Failed };
    Q_ENUM(State)

    explicit ThreadController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    bool traceEnabled() const { return m_traceEnabled; }
    Q_INVOKABLE void tracePresentation(int preset, bool mainView, bool visible,
                                       int viewportWidth, int viewportHeight,
                                       int contentY);
    QString imageMetadataMode() const { return m_imageMetadataMode; }
    void setImageMetadataMode(const QString &mode);
    void setDraftStore(class DraftStore *store) { m_drafts = store; }
    void discardAllAttachments();

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
    bool attachmentsSupported() const;
    QString navigationHighlightEventId() const
    { return m_navigationHighlightEventId; }
    QString navigationMessage() const { return m_navigationMessage; }
    bool navigating() const { return !m_navigationEventId.isEmpty(); }

    Q_INVOKABLE void openThread(const QString &roomId,
                                const QString &rootEventId);
    Q_INVOKABLE void reopen();
    Q_INVOKABLE void close();
    void setOutgoingEmoji(
        std::function<void(QString &body, QVariantMap &bodySpec)> apply)
    { m_outgoingEmoji = std::move(apply); }
    Q_INVOKABLE void sendText(const QString &body);
    Q_INVOKABLE void sendTextBypassingCommands(const QString &body);
    Q_INVOKABLE void sendPrepared(const QString &body, const QString &html,
                                  const QStringList &mentionUserIds);
    QString commandError() const { return m_commandError; }
    Q_INVOKABLE QVariantMap mentionTokenAt(const QString &text,
                                           int cursorPos) const;
    Q_INVOKABLE int insertMention(const QString &userId,
                                  const QString &displayName,
                                  int tokenStart, int cursorPos);
    Q_INVOKABLE void beginReply(const QString &eventId);
    Q_INVOKABLE void cancelReply();
    Q_INVOKABLE void addAttachment(const QUrl &fileUrl);
    Q_INVOKABLE void addDroppedAttachments(const QList<QUrl> &urls,
                                           const QString &portalKey);
    Q_INVOKABLE bool pasteFromClipboard();
    // MSC3245 voice message into the open thread (waveform entries 0..=100).
    // Always an m.thread reply via the SDK thread path; no room-send
    // fallback. Failures surface via attachmentRejected for that thread only.
    Q_INVOKABLE void sendVoiceMessage(const QString &localPath,
                                      const QString &mime,
                                      qreal durationMs,
                                      const QVariantList &waveform);
    Q_INVOKABLE void setFollowed(bool followed);
    // Sends one threaded read receipt for the open thread's latest readable
    // event (deduplicated; never room-wide). A no-op unless viewerAtLatest,
    // and while a reply search is moving the reader to an older reply.
    Q_INVOKABLE void markRead();
    Q_INVOKABLE void markReadNow();
    Q_INVOKABLE void openList(const QString &roomId);
    Q_INVOKABLE void closeList();
    Q_INVOKABLE void paginateList();
    Q_INVOKABLE QStringList participants() const;
    Q_INVOKABLE QVariantMap rootInfo() const;
    int replyCount() const;
    int exactReplyCount() const;
    void notifyReplyCountIfChanged();

    Q_INVOKABLE void navigateToEvent(const QString &eventId);
    Q_INVOKABLE void openThreadAt(const QString &roomId,
                                  const QString &rootEventId,
                                  const QString &eventId);

    void setNavigationPolicyForTest(int maxBatches, int highlightDurationMs,
                                    int batchTimeoutMs);

    void handleCurrentRoomChanged(const QString &currentRoomId);
    int roomThreadUnread() const { return m_roomThreadUnread; }

Q_SIGNALS:
    void imageMetadataModeChanged();
    void threadReadSent(const QString &roomId, const QString &rootEventId,
                        qint64 timestampMs);
    void supportedChanged();
    void roomThreadUnreadChanged();
    void stateChanged();
    void viewerAtLatestChanged();
    void replyCountChanged();
    void rootInfoChanged();
    void textChanged();
    void commandErrorChanged();
    void composerModeToggleRequested();
    void displayNameChangeRequested(const QString &name);
    void mentionRangesChanged();
    void replyStateChanged();
    void followStateChanged();
    void listStateChanged();
    void attachmentsChanged();
    void attachmentRejected(const QString &reason);
    void navigationChanged();
    void navigationTargetLocated(int row);

private Q_SLOTS:
    void onAttachmentQueueFinished(quint64 opId, const QString &roomId,
                                   bool ok, const QString &category);

private:
    void traceState(const char *reason) const;
    void setState(State state, const QString &failureCategory = QString());
    void syncViewerAtLatest();
    QString timelineId() const;
    bool dispatchAttachments();
    void dispatchAttachment(int row);
    void parkAttachments();

    struct VoiceOp {
        QString localPath;
        QString roomId;
        QString rootEventId;
    };
    QHash<quint64, VoiceOp> m_voiceOps;
    QString m_imageMetadataMode = QStringLiteral("location");
    ImageSendPrep m_imagePrep;

    MatrixClient *m_client = nullptr;
    quint64 m_slashJoinOp = 0;
    TimelineModel m_model;
    int m_lastReplyCount = -1;
    int m_lastExactReplyCount = -1;
    AttachmentQueueModel *m_attachments = nullptr;
    // Trays of the threads not on screen, by timelineId(); memory only.
    QHash<QString, QList<AttachmentQueueModel::Entry>> m_parkedAttachments;
    State m_state = Closed;
    bool m_traceEnabled = false;
    quint64 m_traceGeneration = 0;
    QString m_roomId;
    QString m_rootEventId;
    quint64 m_dropGeneration = 0;
    QString m_failureCategory;
    QString m_replyToEventId;
    QString m_replyToSender;
    QString m_replyToPreview;
    QString m_text;
    QList<mention::MentionRef> m_mentionRefs;
    void clearComposerText();
    void sendTextInternal(const QString &body, bool allowCommands);
    void sendThreadBody(const QString &body, const QStringList &mentionIds,
                        const QVariantMap &bodySpec);
    void retireComposerDraft();
    void setCommandError(const QString &error);
    QString m_commandError;
    void saveDraftNow();
    void restoreDraft();
    class DraftStore *m_drafts = nullptr;
    QTimer m_draftDebounce;
    bool m_restoringDraft = false;
    void resetFollowState();
    void startHistoryFill();
    void continueHistoryFill();
    void stopHistoryFill();
    void beginNavigationBatch();
    void continueNavigation();
    void locateNavigationTarget(int row);
    void failNavigation();
    void clearNavigation(bool clearMessage);
    void setNavigationHighlight(const QString &eventId);
    bool navigationStale() const;
    bool threadUnreadHint(const QString &rootEventId) const;
    void refreshRoomThreadUnread();
    QString m_currentRoomId;
    int m_roomThreadUnread = 0;
    bool m_followSupported = false;
    bool m_followed = false;
    bool m_followAutomatic = false;
    bool m_followBusy = false;
    QString m_lastMarkedReadEventId;
    bool m_viewerAtLatest = false;
    bool m_viewerAtLatestAnnounced = false;
    QString m_pendingNavigationEventId;
    QString m_pendingNavigationRoomId;
    QString m_pendingNavigationRootEventId;
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

    QString m_navigationEventId;
    QString m_navigationRoomId;
    QString m_navigationRootEventId;
    int m_navigationBatches = 0;
    bool m_fillActive = false;
    int m_fillBatches = 0;
    int m_maxFillBatches = 8;
    int m_maxNavigationBatches = 8;
    QString m_navigationHighlightEventId;
    QString m_navigationMessage;
    QTimer m_navigationHighlightTimer;
    QTimer m_navigationMessageTimer;
    QTimer m_navigationBatchTimer;
    int m_navigationHighlightMs =
        PaginationController::kDefaultHighlightDurationMs;
    int m_navigationBatchTimeoutMs = 8000;
    bool m_modelPaginating = false;
    std::function<void(QString &, QVariantMap &)> m_outgoingEmoji;
};
