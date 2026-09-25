#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

class MatrixClient;
class QTimer;

// Kick, ban or unban one member of a Space, optionally cascading to the rooms
// beneath it.
//
// A Space is a room, so each step is the ordinary room moderation call
// (MatrixClient::kickUser / banUser / unbanUser); nothing here talks to the
// server another way. What a step may attempt comes from the backend's
// moderation plan, which reads both members' levels and each room's own
// threshold from the SDK. A room the plan does not offer is never dispatched,
// whatever the view asks. Steps run one at a time and each reports its own
// outcome, so a partial result is shown as partial.
class SpaceModerationController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString spaceId READ spaceId NOTIFY stateChanged)
    Q_PROPERTY(QString userId READ userId NOTIFY stateChanged)
    Q_PROPERTY(QString op READ op NOTIFY stateChanged)
    // "idle" | "planning" | "ready" | "running" | "done" | "failed".
    // "failed" is a plan that could not be read; nothing was sent.
    Q_PROPERTY(QString phase READ phase NOTIFY stateChanged)
    // The Space's own row, in the `rooms` row shape.
    Q_PROPERTY(QVariantMap spaceRow READ spaceRow NOTIFY stateChanged)
    // The rooms beneath the Space. Row shape: { roomId, name, isSpace,
    // eligible, reason, reasonText, selected, status, message }. `status` is
    // "" before confirming, then "pending", "running", "ok", "failed",
    // "unknown" (no answer within the step watchdog) or "skipped" (not
    // selected, or not offered).
    Q_PROPERTY(QVariantList rooms READ rooms NOTIFY stateChanged)
    Q_PROPERTY(bool spaceEligible READ spaceEligible NOTIFY stateChanged)
    Q_PROPERTY(int eligibleRoomCount READ eligibleRoomCount
                   NOTIFY stateChanged)
    Q_PROPERTY(int selectedRoomCount READ selectedRoomCount
                   NOTIFY stateChanged)
    Q_PROPERTY(int skippedRoomCount READ skippedRoomCount NOTIFY stateChanged)
    // Whether the selected rooms are included. Only meaningful while
    // eligibleRoomCount > 0.
    Q_PROPERTY(bool cascade READ cascade WRITE setCascade NOTIFY stateChanged)
    Q_PROPERTY(bool planTruncated READ planTruncated NOTIFY stateChanged)
    // Rooms the plan ran out of time for. Never offered.
    Q_PROPERTY(int uncheckedRoomCount READ uncheckedRoomCount
                   NOTIFY stateChanged)
    // Set when a new flow was asked for while this one was still running;
    // the view shows it beside the running flow. Cleared when it finishes.
    Q_PROPERTY(QString notice READ notice NOTIFY stateChanged)
    Q_PROPERTY(bool canConfirm READ canConfirm NOTIFY stateChanged)
    Q_PROPERTY(int succeededCount READ succeededCount NOTIFY stateChanged)
    Q_PROPERTY(int failedCount READ failedCount NOTIFY stateChanged)
    Q_PROPERTY(int unknownCount READ unknownCount NOTIFY stateChanged)
    Q_PROPERTY(int stepCount READ stepCount NOTIFY stateChanged)
    // Presentation text, built here so tests can read exactly what the user is
    // asked to confirm.
    Q_PROPERTY(QString title READ title NOTIFY stateChanged)
    Q_PROPERTY(QString consequenceText READ consequenceText
                   NOTIFY stateChanged)
    Q_PROPERTY(QString cascadeLabel READ cascadeLabel NOTIFY stateChanged)
    Q_PROPERTY(QString confirmLabel READ confirmLabel NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)

public:
    explicit SpaceModerationController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    // The rooms a Space-level action may cascade to, the Space first
    // (SpaceManager::moderationScopeRoomIds in the app).
    void setScopeResolver(std::function<QStringList(const QString &)> resolver);

    QString spaceId() const { return m_spaceId; }
    QString userId() const { return m_userId; }
    QString op() const { return m_op; }
    QString phase() const { return m_phase; }
    QVariantMap spaceRow() const { return m_spaceRow; }
    QVariantList rooms() const { return m_rooms; }
    bool spaceEligible() const;
    /// The plan arrived and refused the Space itself.
    bool spaceSkipped() const;
    int eligibleRoomCount() const;
    int selectedRoomCount() const;
    int skippedRoomCount() const;
    bool cascade() const { return m_cascade; }
    void setCascade(bool cascade);
    bool planTruncated() const { return m_planTruncated; }
    int uncheckedRoomCount() const;
    QString notice() const { return m_notice; }
    bool canConfirm() const;
    int succeededCount() const { return m_succeeded; }
    int failedCount() const { return m_failed; }
    int unknownCount() const { return m_unknown; }
    int stepCount() const { return m_steps.size(); }

    QString title() const;
    QString consequenceText() const;
    QString cascadeLabel() const;
    QString confirmLabel() const;
    QString statusText() const;

    // Starts a new flow and asks the backend for the plan. `op` is "kick",
    // "ban" or "unban"; the names are presentation only. Refused while a flow
    // is running, which sets `notice`. Returns whether a flow started.
    Q_INVOKABLE bool begin(const QString &spaceId, const QString &spaceName,
                           const QString &userId, const QString &displayName,
                           const QString &op);
    // Rooms the plan did not offer cannot be selected.
    Q_INVOKABLE void setRoomSelected(const QString &roomId, bool selected);
    Q_INVOKABLE void setAllRoomsSelected(bool selected);
    // Runs the Space step (when offered) and, with `cascade`, each selected
    // room, one after another. `reason` may be empty.
    Q_INVOKABLE void confirm(const QString &reason);
    // Forgets the flow. A step already sent still completes on the server;
    // its answer is ignored.
    Q_INVOKABLE void reset();
    // Another account became active without this one signing out (add
    // account): the flow belongs to the previous account.
    void resetForAccountChange() { reset(); }

    // Plain text for a plan reason code, for the row that is not offered.
    static QString reasonText(const QString &reason);

    // How long to wait for a plan before calling it failed. The backend
    // answers within its own budget (rooms::MODERATION_PLAN_BUDGET, 25 s);
    // this only catches an answer that never comes.
    static constexpr int kPlanTimeoutMs = 40000;
    void setPlanTimeoutForTest(int ms) { m_planTimeoutMs = ms; }
    // One kick, ban or unban that never answers (its answer lost with a
    // switched handle, say) would keep the modal dialog up for the session.
    // Longer than any single SDK request with its own retries.
    static constexpr int kStepTimeoutMs = 3 * 60 * 1000;
    bool stepWatchdogArmedForTest() const;
    int stepWatchdogIntervalForTest() const;
    void expireStepForTest() { onStepTimedOut(); }

Q_SIGNALS:
    void stateChanged();
    // Every step has answered. The Space's roster is not refreshed here.
    void finished(const QString &spaceId, const QString &userId,
                  const QString &op, int succeeded, int failed);

private Q_SLOTS:
    void onPlanReceived(quint64 opId, const QString &userId,
                        const QString &op, bool truncated,
                        const QVariantList &rooms);
    void onModerationFinished(quint64 opId, const QString &roomId,
                              const QString &userId, const QString &op,
                              bool ok, const QString &category);

private:
    static QVariantMap rowFromPlan(const QVariantMap &plan);
    static QString failureText(const QString &category);
    void setPhase(const QString &phase);
    void onPlanTimedOut();
    void onStepTimedOut();
    void dispatchNext();
    void markRow(const QString &roomId, const QString &status,
                 const QString &message);

    MatrixClient *m_client = nullptr;
    std::function<QStringList(const QString &)> m_scopeResolver;

    QString m_spaceId;
    QString m_spaceName;
    QString m_userId;
    QString m_displayName;
    QString m_op;
    QString m_phase = QStringLiteral("idle");
    QString m_reason;
    QVariantMap m_spaceRow;
    QVariantList m_rooms;
    bool m_cascade = true;
    bool m_planTruncated = false;

    quint64 m_planOp = 0;
    QTimer *m_planTimer = nullptr;
    int m_planTimeoutMs = kPlanTimeoutMs;
    QString m_notice;
    // The account the flow belongs to, pinned at begin().
    QString m_flowUser;
    // The rooms to act on, in order, and the one in flight.
    QStringList m_steps;
    int m_nextStep = 0;
    quint64 m_stepOp = 0;
    QString m_stepRoom;
    int m_succeeded = 0;
    int m_failed = 0;
    int m_unknown = 0;
    QTimer *m_stepTimer = nullptr;
};
