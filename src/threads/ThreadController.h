#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QtQmlIntegration/qqmlintegration.h>

#include "models/AttachmentQueueModel.h"
#include "models/TimelineModel.h"

class MatrixClient;

// v0.6.0: lifecycle owner for the single open SDK-backed thread panel.
//
// The heavy lifting is deliberately NOT here: the backend serves the thread
// as a normal timeline under a composite thread timeline id
// (MatrixClient::threadTimelineId), so this controller owns only the
// LIFECYCLE — open/close, generation isolation across rapid thread and room
// switches, state presentation, and the thread send entry point. The
// thread's rows live in an ordinary TimelineModel bound to the composite
// id, reusing the exact diff application, roles, grouping, and pagination
// plumbing of the room timeline (per the architecture rule: no duplicated
// timeline implementation, no independent sync).
//
// Never logs message bodies, tokens, or media URLs.
class ThreadController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    // Instantiated in C++ and exposed to QML only as the "app.threads"
    // context-property instance; registration exists so QML can name the
    // State enum as ThreadController.Ready etc.
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
    // v0.6.0 checkpoint 4: rich-reply-within-thread compose state. The panel
    // composer shows the banner; sendText targets the reply through the SDK
    // thread path.
    Q_PROPERTY(bool inReply READ inReply NOTIFY replyStateChanged)
    Q_PROPERTY(QString replyToEventId READ replyToEventId NOTIFY replyStateChanged)
    Q_PROPERTY(QString replyToSender READ replyToSender NOTIFY replyStateChanged)
    Q_PROPERTY(QString replyToPreview READ replyToPreview NOTIFY replyStateChanged)
    // v0.6.0 checkpoint 5: MSC4306 follow state for the OPEN thread.
    // followSupported is false until the backend confirms the homeserver
    // supports thread subscriptions — the UI hides the control rather than
    // pretending. followBusy covers the query and the change round-trips.
    Q_PROPERTY(bool followSupported READ followSupported NOTIFY followStateChanged)
    Q_PROPERTY(bool followed READ followed NOTIFY followStateChanged)
    Q_PROPERTY(bool followAutomatic READ followAutomatic NOTIFY followStateChanged)
    Q_PROPERTY(bool followBusy READ followBusy NOTIFY followStateChanged)
    // v0.6.0 checkpoint 5: the room's Threads view (bounded to fetched
    // pages, sorted by latest activity, unread-first where known).
    Q_PROPERTY(bool listOpen READ listOpen NOTIFY listStateChanged)
    Q_PROPERTY(bool listLoading READ listLoading NOTIFY listStateChanged)
    Q_PROPERTY(bool listEndReached READ listEndReached NOTIFY listStateChanged)
    Q_PROPERTY(bool listFailed READ listFailed NOTIFY listStateChanged)
    Q_PROPERTY(QVariantList threadList READ threadList NOTIFY listStateChanged)
    // v0.6.1: thread composer attachment tray (SDK thread-focused send).
    Q_PROPERTY(AttachmentQueueModel *attachments READ attachments CONSTANT)
    Q_PROPERTY(bool hasAttachments READ hasAttachments NOTIFY attachmentsChanged)
    Q_PROPERTY(bool attachmentsSupported READ attachmentsSupported
                   NOTIFY stateChanged)

public:
    enum State { Closed, Opening, Ready, Failed };
    Q_ENUM(State)

    explicit ThreadController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    bool supported() const;
    State state() const { return m_state; }
    bool active() const { return m_state != Closed; }
    QString roomId() const { return m_roomId; }
    QString rootEventId() const { return m_rootEventId; }
    QString failureCategory() const { return m_failureCategory; }
    TimelineModel *model() { return &m_model; }
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
    // Begin/cancel replying to a specific loaded thread event.
    Q_INVOKABLE void beginReply(const QString &eventId);
    Q_INVOKABLE void cancelReply();
    // v0.6.1: queue a picked/dropped file for the open thread; emits
    // attachmentRejected(reason) on validation failure. Never logs the path.
    Q_INVOKABLE void addAttachment(const QUrl &fileUrl);
    // Intercept Ctrl+V: queue a clipboard image or local file URLs; returns
    // true when handled (so the editor does not also paste), false to let the
    // text editor paste normally.
    Q_INVOKABLE bool pasteFromClipboard();
    // Follow/unfollow the open thread (server-side MSC4306 subscription).
    Q_INVOKABLE void setFollowed(bool followed);
    // Send ONE threaded read receipt for the open thread's latest readable
    // event (deduplicated; never a room-wide receipt). QML calls this when
    // the panel is at the latest reply.
    Q_INVOKABLE void markRead();
    // Threads view lifecycle for the current room.
    Q_INVOKABLE void openList(const QString &roomId);
    Q_INVOKABLE void closeList();
    Q_INVOKABLE void paginateList();
    // De-duplicated sender MXIDs of the loaded thread events (root first
    // when loaded). Participants of unloaded history are not invented.
    Q_INVOKABLE QStringList participants() const;
    // Presentation data for the pinned root header. Resolved from the
    // loaded thread timeline first, then from the room timeline; when the
    // root is not loaded anywhere, {loaded: false} lets QML show the
    // honest "original message unavailable" state. Safe fields only.
    Q_INVOKABLE QVariantMap rootInfo() const;

    // The active room changed; a thread panel never survives into another
    // room. Called by AppController.
    void handleCurrentRoomChanged(const QString &currentRoomId);

Q_SIGNALS:
    void supportedChanged();
    void stateChanged();
    void replyStateChanged();
    void followStateChanged();
    void listStateChanged();
    void attachmentsChanged();
    void attachmentRejected(const QString &reason);

private Q_SLOTS:
    void onAttachmentQueueFinished(quint64 opId, const QString &roomId,
                                   bool ok, const QString &category);

private:
    void setState(State state, const QString &failureCategory = QString());
    QString timelineId() const;
    // Send every queued attachment through the SDK thread path. Each becomes
    // its own local echo in the thread timeline.
    void dispatchAttachments();
    void clearAttachments();

    MatrixClient *m_client = nullptr;
    TimelineModel m_model;
    AttachmentQueueModel *m_attachments = nullptr;
    State m_state = Closed;
    QString m_roomId;
    QString m_rootEventId;
    QString m_failureCategory;
    QString m_replyToEventId;
    QString m_replyToSender;
    QString m_replyToPreview;
    void resetFollowState();
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
};
