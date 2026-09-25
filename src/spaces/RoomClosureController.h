#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

class MatrixClient;
class QTimer;

// Closing a room or a Space, and deleting one from the homeserver when the
// account is a server administrator.
//
// Matrix has no client-side room deletion, so "close" is what an owner can
// honestly do: make the room invite-only, take it out of the public directory
// (and, for one room, out of the Spaces it is listed in), remove everyone the
// viewer outranks, then leave. History stays on every server that took part
// and anyone the viewer does not outrank stays. It is never called a delete.
//
// "delete" is Synapse's admin API: offered only after the server has said the
// account is an administrator, and only after the user types the name. It
// removes the room from this homeserver only.
//
// A Space cascades to the rooms beneath it (the scope resolver, Space first).
// The backend's plan decides what close may attempt; a room it does not offer
// is never dispatched. Steps run one at a time, rooms before the Space, and
// each reports its own outcome.
class RoomClosureController : public QObject
{
    Q_OBJECT
    // "close" | "delete".
    Q_PROPERTY(QString mode READ mode NOTIFY stateChanged)
    Q_PROPERTY(QString targetId READ targetId NOTIFY stateChanged)
    Q_PROPERTY(QString targetName READ targetName NOTIFY stateChanged)
    Q_PROPERTY(bool targetIsSpace READ targetIsSpace NOTIFY stateChanged)
    // "idle" | "planning" | "ready" | "running" | "done" | "failed". "failed"
    // is a plan that could not be read; nothing was sent.
    Q_PROPERTY(QString phase READ phase NOTIFY stateChanged)
    // "unknown" | "checking" | "yes" | "no". Only "yes" offers a delete.
    Q_PROPERTY(QString serverAdmin READ serverAdmin NOTIFY stateChanged)
    // The target's own row and the rooms beneath a Space. Row shape: { roomId,
    // name, isSpace, eligible, reason, reasonText, summary, alsoElsewhere,
    // selected, status, message, progress }. `status` is "" before
    // confirming, then "pending", "running", "ok", "partial", "failed",
    // "following" (a delete the server had not finished when we stopped
    // following it), "unknown" (no answer within the step watchdog) or
    // "skipped".
    Q_PROPERTY(QVariantMap targetRow READ targetRow NOTIFY stateChanged)
    Q_PROPERTY(QVariantList rooms READ rooms NOTIFY stateChanged)
    Q_PROPERTY(bool targetEligible READ targetEligible NOTIFY stateChanged)
    Q_PROPERTY(int eligibleRoomCount READ eligibleRoomCount NOTIFY stateChanged)
    Q_PROPERTY(int selectedRoomCount READ selectedRoomCount NOTIFY stateChanged)
    Q_PROPERTY(int uncheckedRoomCount READ uncheckedRoomCount
                   NOTIFY stateChanged)
    Q_PROPERTY(bool planTruncated READ planTruncated NOTIFY stateChanged)
    Q_PROPERTY(bool cascade READ cascade WRITE setCascade NOTIFY stateChanged)
    // Close: leave each room once it is fully closed.
    Q_PROPERTY(bool leaveAfter READ leaveAfter WRITE setLeaveAfter
                   NOTIFY stateChanged)
    // Close of one room: also take it out of these Spaces (the parents where
    // the viewer may change the children, from the plan).
    Q_PROPERTY(QStringList parentSpaceNames READ parentSpaceNames
                   NOTIFY stateChanged)
    // Parents that list the room but whose children the viewer may not
    // change: it stays listed there.
    Q_PROPERTY(QStringList lockedParentNames READ lockedParentNames
                   NOTIFY stateChanged)
    Q_PROPERTY(bool unlistFromParents READ unlistFromParents
                   WRITE setUnlistFromParents NOTIFY stateChanged)
    // Delete: stop users of this server joining it again.
    Q_PROPERTY(bool block READ block WRITE setBlock NOTIFY stateChanged)
    // Delete: what must be typed to confirm, and whether it has been.
    Q_PROPERTY(QString confirmationPhrase READ confirmationPhrase
                   NOTIFY stateChanged)
    Q_PROPERTY(bool typedConfirmationMatches READ typedConfirmationMatches
                   NOTIFY stateChanged)
    // False when the name has characters that cannot be typed; the phrase is
    // then the room id. The id is always accepted too.
    Q_PROPERTY(bool confirmationNameUsable READ confirmationNameUsable
                   NOTIFY stateChanged)
    Q_PROPERTY(bool canConfirm READ canConfirm NOTIFY stateChanged)
    Q_PROPERTY(int succeededCount READ succeededCount NOTIFY stateChanged)
    Q_PROPERTY(int partialCount READ partialCount NOTIFY stateChanged)
    Q_PROPERTY(int failedCount READ failedCount NOTIFY stateChanged)
    // Steps that did not answer within the step watchdog.
    Q_PROPERTY(int unknownCount READ unknownCount NOTIFY stateChanged)
    Q_PROPERTY(int stepCount READ stepCount NOTIFY stateChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY stateChanged)
    Q_PROPERTY(QString title READ title NOTIFY stateChanged)
    Q_PROPERTY(QString consequenceText READ consequenceText NOTIFY stateChanged)
    Q_PROPERTY(QString cascadeLabel READ cascadeLabel NOTIFY stateChanged)
    Q_PROPERTY(QString confirmLabel READ confirmLabel NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)

public:
    explicit RoomClosureController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    // Space id -> the Space first, then every joined room and subspace
    // beneath it (SpaceManager::moderationScopeRoomIds in the app).
    void setScopeResolver(std::function<QStringList(const QString &)> resolver);
    // Room id -> the joined Spaces that list it as a direct child.
    void setParentResolver(std::function<QStringList(const QString &)> resolver);
    // Room id -> { name, isSpace }, for the rows of a delete, which reads no
    // plan.
    void setRoomInfoResolver(
        std::function<QVariantMap(const QString &)> resolver);

    QString mode() const { return m_mode; }
    QString targetId() const { return m_targetId; }
    QString targetName() const { return m_targetName; }
    bool targetIsSpace() const { return m_targetIsSpace; }
    QString phase() const { return m_phase; }
    // "unknown" whenever the answer was about another account than the one
    // the client now speaks for, whatever reset did or did not run.
    QString serverAdmin() const;
    QVariantMap targetRow() const { return m_targetRow; }
    QVariantList rooms() const { return m_rooms; }
    bool targetEligible() const;
    int eligibleRoomCount() const;
    int selectedRoomCount() const;
    int uncheckedRoomCount() const;
    bool planTruncated() const { return m_planTruncated; }
    bool cascade() const { return m_cascade; }
    void setCascade(bool cascade);
    bool leaveAfter() const { return m_leaveAfter; }
    void setLeaveAfter(bool leave);
    QStringList parentSpaceNames() const;
    QStringList lockedParentNames() const;
    bool unlistFromParents() const { return m_unlistFromParents; }
    void setUnlistFromParents(bool unlist);
    bool block() const { return m_block; }
    void setBlock(bool block);
    QString confirmationPhrase() const;
    bool confirmationNameUsable() const;
    bool typedConfirmationMatches() const;
    bool canConfirm() const;
    int succeededCount() const { return m_succeeded; }
    int partialCount() const { return m_partial; }
    int failedCount() const { return m_failed; }
    int unknownCount() const { return m_unknown; }
    int stepCount() const { return m_steps.size(); }
    QString notice() const { return m_notice; }

    QString title() const;
    QString consequenceText() const;
    QString cascadeLabel() const;
    QString confirmLabel() const;
    QString statusText() const;

    // Asks the server once per session whether the account is a server
    // administrator. Never asked in the background: a view calls this when
    // it is about to show a delete.
    Q_INVOKABLE void checkServerAdmin();
    // Starts a flow. `mode` is "close" or "delete"; a delete needs
    // serverAdmin == "yes". Refused while a flow is running, which sets
    // `notice`. Returns whether a flow started.
    Q_INVOKABLE bool begin(const QString &roomId, const QString &name,
                           bool isSpace, const QString &mode);
    Q_INVOKABLE void setRoomSelected(const QString &roomId, bool selected);
    Q_INVOKABLE void setAllRoomsSelected(bool selected);
    Q_INVOKABLE void setTypedConfirmation(const QString &text);
    // Runs the selected rooms, then the target. `reason` (close only, may be
    // empty) goes on each removal.
    Q_INVOKABLE void confirm(const QString &reason);
    // Forgets the flow. A step already sent still completes on the server.
    Q_INVOKABLE void reset();
    // Another account became active without this one signing out (add
    // account): forget the flow and the administrator answer, which belong
    // to the previous account.
    void resetForAccountChange();
    // The dialog was closed. A running flow keeps going, and its result is
    // shown the next time a dialog asks to begin; anything else is reset.
    Q_INVOKABLE void viewClosed();

    static QString reasonText(const QString &reason);
    // One plan row's "what closing does here" line.
    static QString closeSummary(const QVariantMap &planRow);

    static constexpr int kPlanTimeoutMs = 40000;
    // Silence allowed within one step before its row reads "not known yet"
    // and the flow moves on. Longer than every backend bound: a close
    // reports after each step and each removal (60 s each); a delete is
    // followed for 10 minutes.
    static constexpr int kStepSilenceMs = 15 * 60 * 1000;
    // An "unavailable" administrator answer is asked again after this.
    static constexpr int kAdminRetryMs = 10 * 60 * 1000;
    void setPlanTimeoutForTest(int ms) { m_planTimeoutMs = ms; }
    void setStepSilenceForTest(int ms) { m_stepSilenceMs = ms; }
    // The step watchdog without waiting on the wall clock.
    bool stepWatchdogArmedForTest() const;
    int stepWatchdogIntervalForTest() const;
    void expireStepForTest() { onStepTimedOut(); }
    void setAdminRetryForTest(int ms) { m_adminRetryMs = ms; }

Q_SIGNALS:
    void stateChanged();

private:
    void onPlanReceived(quint64 opId, bool truncated, const QVariantList &rows);
    void onClosureProgress(quint64 opId, const QString &roomId, int done,
                           int total);
    void onClosureFinished(quint64 opId, const QString &roomId,
                           const QVariantMap &result);
    void onAdminStatus(quint64 opId, bool admin, const QString &detail);
    void onDeleteProgress(quint64 opId, const QString &roomId,
                          const QString &status);
    void onDeleteFinished(quint64 opId, const QString &roomId,
                          const QVariantMap &result);
    void onPlanTimedOut();
    void onStepTimedOut();
    QStringList descendantsOf(const QString &spaceId) const;
    bool anyDescendantUnfinished(const QString &spaceId) const;
    void forgetAccount();
    // Whether the client now speaks for another account than `pinned`.
    bool accountMoved(const QString &pinned) const;

    QVariantMap rowFromPlan(const QVariantMap &plan) const;
    QVariantMap deleteRow(const QString &roomId) const;
    int alsoElsewhere(const QString &roomId) const;
    QString serverName() const;
    void setPhase(const QString &phase);
    void dispatchNext();
    void markRow(const QString &roomId, const QString &status,
                 const QString &message);
    void setRowProgress(const QString &roomId, const QString &progress);
    void stepAnswered(const QString &roomId, const QString &status,
                      const QString &message);
    static QString closeResultText(const QVariantMap &result);
    static QString deleteResultText(const QVariantMap &result);

    MatrixClient *m_client = nullptr;
    std::function<QStringList(const QString &)> m_scopeResolver;
    std::function<QStringList(const QString &)> m_parentResolver;
    std::function<QVariantMap(const QString &)> m_infoResolver;

    QString m_mode;
    QString m_targetId;
    QString m_targetName;
    bool m_targetIsSpace = false;
    QString m_phase = QStringLiteral("idle");
    QString m_serverAdmin = QStringLiteral("unknown");
    quint64 m_adminOp = 0;
    QString m_adminUser;
    // The account the running flow belongs to, pinned at begin().
    QString m_flowUser;
    QVariantMap m_targetRow;
    QVariantList m_rooms;
    QStringList m_scope;
    QStringList m_parentCandidates;
    QStringList m_parents;
    QStringList m_lockedParents;
    QSet<QString> m_leaveHeld;
    bool m_resultPending = false;
    bool m_adminUnavailable = false;
    QElapsedTimer m_adminAnswered;
    int m_adminRetryMs = kAdminRetryMs;
    QTimer *m_stepTimer = nullptr;
    int m_stepSilenceMs = kStepSilenceMs;
    bool m_cascade = true;
    bool m_leaveAfter = true;
    bool m_unlistFromParents = true;
    bool m_block = true;
    bool m_planTruncated = false;
    QString m_typed;
    QString m_reason;
    QString m_notice;

    quint64 m_planOp = 0;
    QTimer *m_planTimer = nullptr;
    int m_planTimeoutMs = kPlanTimeoutMs;
    QStringList m_steps;
    int m_nextStep = 0;
    quint64 m_stepOp = 0;
    QString m_stepRoom;
    int m_succeeded = 0;
    int m_partial = 0;
    int m_failed = 0;
    int m_unknown = 0;
};
