#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class MatrixClient;

// v0.7.x: reusable User-Interactive Authentication flow + device sign-out.
//
// One privileged operation at a time. The server decides whether auth is
// needed: the operation is attempted first, and only a real UIA challenge
// opens the prompt (`challengeActive`). Only the password stage is rendered
// today; any other required stage surfaces honestly as unsupported.
//
// CREDENTIAL RULES (non-negotiable): the password passes through
// submitPassword() transiently — never stored in a member, never logged,
// never echoed into a signal, scrubbed at every layer below (C++ transit
// buffer + Rust). Cancellation, completion, failure, sign-out and account
// switches all clear the challenge state.
//
// MAS/OAuth accounts have no password to ask for: their device management
// lives in the account web console; managementUrlReady hands the URL to
// the UI, which opens it in the system browser.
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
    // Raw stage names, for the honest unsupported-stage message.
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

    // Sign out one/many of the account's own devices. The current device
    // is refused here as a guard — signing out THIS session is the normal
    // logout flow, not a device deletion.
    Q_INVOKABLE void signOutDevices(const QStringList &deviceIds,
                                    const QString &currentDeviceId);

    // Answer the pending challenge. The QML field must wipe itself
    // immediately after calling this.
    Q_INVOKABLE void submitPassword(const QString &password);
    Q_INVOKABLE void cancel();

    // OAuth accounts: fetch the account-console URL (deviceId "" = the
    // sessions list). Result arrives on managementUrlReady.
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
