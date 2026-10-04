#pragma once

#include <QObject>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

#include "models/TimelineModel.h"

class MatrixClient;

// Lifecycle owner for the read-only event context view: a message and the
// messages around it, fetched with /context, shown without walking history.
//
// The backend serves the view as a timeline under its own composite id
// (MatrixClient::contextTimelineId), so the rows live in an ordinary
// TimelineModel and render with the same delegate. This class owns only
// open/close, generation isolation across room, account and view changes, and
// bounded pagination at the two edges. It never sends, reacts or marks read.
//
// Never logs message bodies, tokens or media URLs.
class ContextController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    // Exposed only as app.eventContext; registered so QML can name the enum.
    QML_UNCREATABLE("ContextController is exposed via app.eventContext")
    Q_PROPERTY(bool supported READ supported NOTIFY stateChanged)
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)
    Q_PROPERTY(QString roomId READ roomId NOTIFY stateChanged)
    // The event the view was opened for; highlighted in the list.
    Q_PROPERTY(QString eventId READ eventId NOTIFY stateChanged)
    Q_PROPERTY(QString failureCategory READ failureCategory NOTIFY stateChanged)
    Q_PROPERTY(TimelineModel *model READ model CONSTANT)
    Q_PROPERTY(bool loadingOlder READ loadingOlder NOTIFY edgesChanged)
    Q_PROPERTY(bool loadingNewer READ loadingNewer NOTIFY edgesChanged)
    Q_PROPERTY(bool reachedStart READ reachedStart NOTIFY edgesChanged)
    Q_PROPERTY(bool reachedEnd READ reachedEnd NOTIFY edgesChanged)
    // The last edge load failed; the edge shows a retry.
    Q_PROPERTY(bool olderFailed READ olderFailed NOTIFY edgesChanged)
    Q_PROPERTY(bool newerFailed READ newerFailed NOTIFY edgesChanged)
    // Edge loads allowed so far per side; each is one bounded page.
    Q_PROPERTY(bool canLoadOlder READ canLoadOlder NOTIFY edgesChanged)
    Q_PROPERTY(bool canLoadNewer READ canLoadNewer NOTIFY edgesChanged)

public:
    enum State { Closed, Opening, Ready, Failed };
    Q_ENUM(State)

    // Upper bound on pages loaded per edge in one view, so a reader cannot
    // turn the view back into an unbounded history walk.
    static constexpr int kMaxEdgePages = 25;

    explicit ContextController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    bool supported() const;
    State state() const { return m_state; }
    bool active() const { return m_state != Closed; }
    QString roomId() const { return m_roomId; }
    QString eventId() const { return m_eventId; }
    QString failureCategory() const { return m_failureCategory; }
    TimelineModel *model() { return &m_model; }
    bool loadingOlder() const { return m_loadingOlder; }
    bool loadingNewer() const { return m_loadingNewer; }
    bool reachedStart() const { return m_reachedStart; }
    bool reachedEnd() const { return m_reachedEnd; }
    bool olderFailed() const { return m_olderFailed; }
    bool newerFailed() const { return m_newerFailed; }
    bool canLoadOlder() const;
    bool canLoadNewer() const;

    // Open (or replace) the view for `eventId` in `roomId`. Returns false,
    // changing nothing, when the backend has no event context or an argument
    // is empty; true once the request is dispatched.
    bool open(const QString &roomId, const QString &eventId);
    // Close the view. Safe when closed. Late results from the closed view are
    // ignored by identity and by the backend's generation.
    Q_INVOKABLE void close();
    // "Jump to latest": close, and let the live timeline take over.
    Q_INVOKABLE void jumpToLatest();
    Q_INVOKABLE void loadOlder();
    Q_INVOKABLE void loadNewer();
    // Row of the opened event in model(), or -1.
    Q_INVOKABLE int targetRow() const;

    // The active room changed; a view never survives into another room.
    void handleCurrentRoomChanged(const QString &currentRoomId);

Q_SIGNALS:
    void stateChanged();
    void edgesChanged();
    // The view reached Ready and the target is at `row`.
    void targetLocated(int row);
    // Opening failed (`/context` refused, no access, network). The router
    // falls back to the history walk for `eventId`.
    void openFailed(const QString &roomId, const QString &eventId,
                    const QString &category);
    // The target turned out to be a thread reply; the view closed itself and
    // the router opens the thread panel there instead.
    void threadReplyHit(const QString &roomId, const QString &threadRootId,
                        const QString &eventId);
    // jumpToLatest() was used.
    void latestRequested();

private:
    void setState(State state, const QString &failureCategory = QString());
    void resetEdges();
    QString timelineId() const;
    void onTimelineReset(const QString &timelineId);
    void onPagination(const QString &roomId, const QString &eventId,
                      bool forward, const QString &state, bool reachedEdge);

    MatrixClient *m_client = nullptr;
    TimelineModel m_model;
    State m_state = Closed;
    QString m_roomId;
    QString m_eventId;
    QString m_failureCategory;
    bool m_loadingOlder = false;
    bool m_loadingNewer = false;
    bool m_reachedStart = false;
    bool m_reachedEnd = false;
    bool m_olderFailed = false;
    bool m_newerFailed = false;
    int m_olderPages = 0;
    int m_newerPages = 0;
};
