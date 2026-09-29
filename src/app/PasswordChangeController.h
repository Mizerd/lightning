#pragma once

#include <QObject>
#include <QString>

#include <functional>

class MatrixClient;

// Settings → Account → Change password, as Element offers it.
//
// The passwords enter C++ only as parameters of changePassword(): they are
// checked locally (confirmation matches, minimum length), handed to the
// backend and dropped. No member, property or signal ever holds one. The
// backend scrubs its own copies and reports a category, never the server's
// text.
//
// OAuth (MAS) accounts, and servers that turn the m.change_password
// capability off, get no form: the account page does it instead
// (managementUrl, from the server's auth metadata).
//
// Sign-out and account switches drop the pending operation, so a late answer
// can never land on the next account.
class PasswordChangeController : public QObject
{
    Q_OBJECT
    // The backend can change a password at all (Rust backend only).
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    // "form" | "external" (managed by the account page) | "unavailable"
    Q_PROPERTY(QString mode READ mode NOTIFY stateChanged)
    // The account page, when the server publishes one. Empty otherwise.
    Q_PROPERTY(QString managementUrl READ managementUrl NOTIFY stateChanged)
    // Outcome of the last attempt, shown in place.
    Q_PROPERTY(QString resultMessage READ resultMessage NOTIFY stateChanged)
    Q_PROPERTY(bool resultOk READ resultOk NOTIFY stateChanged)
    Q_PROPERTY(int minimumLength READ minimumLength CONSTANT)

public:
    static constexpr int kMinimumLength = 8;

    explicit PasswordChangeController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    // Whether the active account signed in through OAuth (MAS).
    void setOAuthAccountCheck(std::function<bool()> check);

    bool available() const;
    bool busy() const { return m_changeOp != 0; }
    QString mode() const;
    QString managementUrl() const { return m_managementUrl; }
    QString resultMessage() const { return m_resultMessage; }
    bool resultOk() const { return m_resultOk; }
    int minimumLength() const { return kMinimumLength; }

    // Ask the server again (capability and account page). Cheap; call when
    // the section is shown and when the active account changes.
    Q_INVOKABLE void refresh();

    // Checks the fields, then starts the change. Returns true when it was
    // sent; false with the reason in resultMessage when refused locally. The
    // QML fields must wipe themselves right after calling this.
    Q_INVOKABLE bool changePassword(const QString &currentPassword,
                                    const QString &newPassword,
                                    const QString &confirmPassword,
                                    bool signOutOtherDevices);
    Q_INVOKABLE void clearResult();

    // "" when the fields may be sent, else a category: "empty",
    // "mismatch" or "too_short".
    static QString localProblem(const QString &currentPassword,
                                const QString &newPassword,
                                const QString &confirmPassword);
    // The sentence shown for a category ("" is success).
    static QString describe(const QString &category, bool signedOutOthers);

Q_SIGNALS:
    void stateChanged();
    // Terminal outcome of a change that was sent.
    void finished(bool ok, const QString &message);

private Q_SLOTS:
    void onProbed(quint64 opId, bool known, bool canChange,
                  const QString &managementUrl);
    void onFinished(quint64 opId, bool ok, const QString &category);
    void onLoggedOut();
    void onLoggedIn();

private:
    void setResult(bool ok, const QString &message);

    MatrixClient *m_client = nullptr;
    std::function<bool()> m_oauthCheck;
    quint64 m_changeOp = 0;
    quint64 m_probeOp = 0;
    // The section has asked at least once: re-ask for each new session, so
    // an account switched to while it is open is not judged by the old one.
    bool m_probeWanted = false;
    bool m_signOutRequested = false;
    // Probe answer. Unknown means the form is offered and the server decides.
    bool m_probeKnown = false;
    bool m_canChange = true;
    QString m_managementUrl;
    QString m_resultMessage;
    bool m_resultOk = false;
};
