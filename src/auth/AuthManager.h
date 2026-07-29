#pragma once

#include <QObject>
#include <QString>

class MatrixClient;

class AuthManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool isLoggingIn READ isLoggingIn NOTIFY isLoggingInChanged)
    Q_PROPERTY(bool isLoggedIn READ isLoggedIn NOTIFY isLoggedInChanged)
    Q_PROPERTY(QString currentUserId READ currentUserId NOTIFY isLoggedInChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

    // Sign-in progress. A plain QString token (the convention already used by
    // verificationState / roomKeyImportState) rather than a translated status
    // string, so QML can branch on it without comparing user-visible prose.
    //
    // Only stages the backend can actually PROVE are reported. The bridge
    // cannot currently distinguish "creating a device" from "restoring
    // encryption state", so those labels are deliberately absent rather than
    // guessed: a progress step that is not observed is a lie about what the
    // client is doing.
    //
    //   idle           no sign-in in flight
    //   connecting     credentials accepted locally; SDK handle + local store
    //                  are being opened
    //   authenticating store is open, the homeserver request is in flight
    //   starting_sync  the server accepted the login; sync is being started
    //   ready          sync is running
    Q_PROPERTY(QString loginStage READ loginStage NOTIFY loginStageChanged)

    // v0.4.1: capability flags. Password login works on the HTTP backend.
    // SSO / OIDC are v0.5 targets — the properties exist so QML can render
    // the buttons in a disabled state without guessing.
    Q_PROPERTY(bool supportsPasswordLogin READ supportsPasswordLogin CONSTANT)
    Q_PROPERTY(bool supportsSsoLogin      READ supportsSsoLogin      CONSTANT)
    Q_PROPERTY(bool supportsOidcLogin     READ supportsOidcLogin     CONSTANT)

public:
    explicit AuthManager(MatrixClient *client, QObject *parent = nullptr);

    bool isLoggingIn() const { return m_loggingIn; }
    bool isLoggedIn() const;
    QString currentUserId() const;
    QString lastError() const { return m_lastError; }
    QString loginStage() const { return m_loginStage; }

    // Password login is available on all compiled backends. Rust may still
    // surface SDK-side login errors through MatrixClient::login().
    bool supportsPasswordLogin() const { return true; }
    // v0.4.1: capability advertisements only — no protocol implementation.
    // Real SSO / OIDC flow is v0.5. Setting these to false keeps the UI
    // honest.
    bool supportsSsoLogin()  const { return false; }
    bool supportsOidcLogin() const { return false; }

    Q_INVOKABLE void login(const QString &homeserver,
                           const QString &user,
                           const QString &password);
    Q_INVOKABLE void logout();
    Q_INVOKABLE void restoreSession();
    void clearLastError();

    // Placeholders for the SSO / OIDC flows. Both surface a controlled
    // "not implemented" error via lastError so the UI can react.
    Q_INVOKABLE void beginSsoLogin(const QString &homeserver);
    Q_INVOKABLE void beginOidcLogin(const QString &homeserver);

Q_SIGNALS:
    void isLoggingInChanged();
    void isLoggedInChanged();
    void lastErrorChanged();
    void loginStageChanged();
    void loginSucceeded();
    void loginFailed(const QString &reason);
    void loggedOut();

private:
    void setLoggingIn(bool v);
    void setLastError(const QString &err);
    void setLoginStage(const QString &stage);

    MatrixClient *m_client = nullptr;
    bool m_loggingIn = false;
    QString m_lastError;
    QString m_loginStage = QStringLiteral("idle");
};
