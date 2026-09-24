#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class MatrixClient;

// User-Interactive Authentication flow and device sign-out, one operation at
// a time. The operation is attempted first; only a real UIA challenge opens the
// prompt. Only the password stage is supported; others are reported as such.
//
// The password passes through submitPassword() transiently: never stored,
// logged or emitted, and scrubbed by every layer below. Cancellation,
// completion, failure, sign-out and account switches clear the challenge.
//
// OAuth (MAS) accounts manage devices in the account web console instead;
// managementUrlReady hands its URL to the UI.
class UiaController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    // A UIA challenge is waiting for the user.
    Q_PROPERTY(bool challengeActive READ challengeActive NOTIFY stateChanged)
    // The challenge can be completed with the account password.
    Q_PROPERTY(bool passwordStage READ passwordStage NOTIFY stateChanged)
    // A previous password answer was rejected — offer retry.
    Q_PROPERTY(bool wrongPassword READ wrongPassword NOTIFY stateChanged)
    // Raw stage names, for the unsupported-stage message.
    Q_PROPERTY(QStringList stages READ stages NOTIFY stateChanged)

public:
    explicit UiaController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    bool supported() const;
    bool busy() const { return m_deleteOp != 0 || m_urlOp != 0; }
    bool challengeActive() const { return m_challengeActive; }
    bool passwordStage() const { return m_passwordStage; }
    bool wrongPassword() const { return m_wrongPassword; }
    QStringList stages() const { return m_stages; }

    // Sign out other devices of this account. The current device is refused:
    // that is the normal logout flow.
    Q_INVOKABLE void signOutDevices(const QStringList &deviceIds,
                                    const QString &currentDeviceId);

    // Answer the pending challenge. The QML field must wipe itself
    // immediately after calling this.
    Q_INVOKABLE void submitPassword(const QString &password);
    Q_INVOKABLE void cancel();

    // OAuth accounts: fetch the account-console URL (deviceId "" = sessions
    // list). The result arrives on managementUrlReady.
    Q_INVOKABLE void requestManagementUrl(const QString &deviceId);

Q_SIGNALS:
    void stateChanged();
    // Terminal outcome of a sign-out operation (after any UIA round).
    void signOutFinished(bool ok, const QString &message);
    void managementUrlReady(const QString &url);

private Q_SLOTS:
    void onUiaRequired(quint64 uiaId, bool hasPasswordStage,
                       bool wrongPassword, const QStringList &stages);
    void onDeleteFinished(quint64 opId, bool ok, const QString &category);
    void onManagementUrl(quint64 opId, bool ok, const QString &url);
    void onLoggedOut();

private:
    void clearChallenge();
    static QString describeCategory(const QString &category);

    MatrixClient *m_client = nullptr;
    quint64 m_deleteOp = 0;
    quint64 m_urlOp = 0;
    bool m_challengeActive = false;
    bool m_passwordStage = false;
    bool m_wrongPassword = false;
    QStringList m_stages;
};
