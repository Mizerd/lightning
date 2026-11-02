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

class RoomClosureController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString mode READ mode NOTIFY stateChanged)
    Q_PROPERTY(QString targetId READ targetId NOTIFY stateChanged)
    Q_PROPERTY(QString targetName READ targetName NOTIFY stateChanged)
    Q_PROPERTY(bool targetIsSpace READ targetIsSpace NOTIFY stateChanged)
    Q_PROPERTY(QString phase READ phase NOTIFY stateChanged)
    Q_PROPERTY(QString serverAdmin READ serverAdmin NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap targetRow READ targetRow NOTIFY stateChanged)
    Q_PROPERTY(QVariantList rooms READ rooms NOTIFY stateChanged)
    Q_PROPERTY(bool targetEligible READ targetEligible NOTIFY stateChanged)
    Q_PROPERTY(int eligibleRoomCount READ eligibleRoomCount NOTIFY stateChanged)
    Q_PROPERTY(int selectedRoomCount READ selectedRoomCount NOTIFY stateChanged)
    Q_PROPERTY(int uncheckedRoomCount READ uncheckedRoomCount
                   NOTIFY stateChanged)
    Q_PROPERTY(bool planTruncated READ planTruncated NOTIFY stateChanged)
    Q_PROPERTY(bool cascade READ cascade WRITE setCascade NOTIFY stateChanged)
    Q_PROPERTY(bool leaveAfter READ leaveAfter WRITE setLeaveAfter
                   NOTIFY stateChanged)
    Q_PROPERTY(QStringList parentSpaceNames READ parentSpaceNames
                   NOTIFY stateChanged)
    Q_PROPERTY(QStringList lockedParentNames READ lockedParentNames
                   NOTIFY stateChanged)
    Q_PROPERTY(bool unlistFromParents READ unlistFromParents
                   WRITE setUnlistFromParents NOTIFY stateChanged)
    Q_PROPERTY(bool block READ block WRITE setBlock NOTIFY stateChanged)
    Q_PROPERTY(QString confirmationPhrase READ confirmationPhrase
                   NOTIFY stateChanged)
    Q_PROPERTY(bool typedConfirmationMatches READ typedConfirmationMatches
                   NOTIFY stateChanged)
    Q_PROPERTY(bool confirmationNameUsable READ confirmationNameUsable
                   NOTIFY stateChanged)
    Q_PROPERTY(bool canConfirm READ canConfirm NOTIFY stateChanged)
    Q_PROPERTY(int succeededCount READ succeededCount NOTIFY stateChanged)
    Q_PROPERTY(int partialCount READ partialCount NOTIFY stateChanged)
    Q_PROPERTY(int failedCount READ failedCount NOTIFY stateChanged)
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
    void setScopeResolver(std::function<QStringList(const QString &)> resolver);
    void setParentResolver(std::function<QStringList(const QString &)> resolver);
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

    Q_INVOKABLE void checkServerAdmin();
    Q_INVOKABLE bool begin(const QString &roomId, const QString &name,
                           bool isSpace, const QString &mode);
    Q_INVOKABLE void setRoomSelected(const QString &roomId, bool selected);
    Q_INVOKABLE void setAllRoomsSelected(bool selected);
    Q_INVOKABLE void setTypedConfirmation(const QString &text);
    Q_INVOKABLE void confirm(const QString &reason);
    // Forgets the flow. A step already sent still completes on the server.
    Q_INVOKABLE void reset();
    void resetForAccountChange();
    Q_INVOKABLE void viewClosed();

    static QString reasonText(const QString &reason);
    static QString closeSummary(const QVariantMap &planRow);

    static constexpr int kPlanTimeoutMs = 40000;
    static constexpr int kStepSilenceMs = 15 * 60 * 1000;
    static constexpr int kAdminRetryMs = 10 * 60 * 1000;
    void setPlanTimeoutForTest(int ms) { m_planTimeoutMs = ms; }
    void setStepSilenceForTest(int ms) { m_stepSilenceMs = ms; }
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
