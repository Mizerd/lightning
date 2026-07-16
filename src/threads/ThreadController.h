#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QtQmlIntegration/qqmlintegration.h>

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

private:
    void setState(State state, const QString &failureCategory = QString());
    QString timelineId() const;

    MatrixClient *m_client = nullptr;
    TimelineModel m_model;
    State m_state = Closed;
    QString m_roomId;
    QString m_rootEventId;
    QString m_failureCategory;
    QString m_replyToEventId;
    QString m_replyToSender;
    QString m_replyToPreview;
};
