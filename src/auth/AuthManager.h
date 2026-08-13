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

    // Backend CAPABILITY: what this build can do at all, regardless of any
    // particular server. Distinct from discovery below, which is what a given
    // homeserver actually offers.
    Q_PROPERTY(bool supportsPasswordLogin READ supportsPasswordLogin CONSTANT)
    Q_PROPERTY(bool supportsSsoLogin      READ supportsSsoLogin      CONSTANT)
    Q_PROPERTY(bool supportsOidcLogin     READ supportsOidcLogin     CONSTANT)

    // SERVER DISCOVERY: what the entered homeserver advertises. All false
    // until a discovery completes, so the UI shows nothing speculative.
    //
    //   idle       no homeserver resolved yet
    //   probing    discovery in flight
    //   done       the server answered
    //   failed     the server could not be asked
    Q_PROPERTY(QString discoveryState READ discoveryState NOTIFY discoveryChanged)
    Q_PROPERTY(QString discoveredHomeserver READ discoveredHomeserver NOTIFY discoveryChanged)
    Q_PROPERTY(bool serverOffersPassword READ serverOffersPassword NOTIFY discoveryChanged)
    Q_PROPERTY(bool serverOffersBrowserLogin READ serverOffersBrowserLogin NOTIFY discoveryChanged)
    // The server offers legacy Matrix SSO but Lightning CANNOT perform it (the
    // SDK helper needs the sso-login/local-server features, whose axum
    // dependency is not vendored in this offline build). Exposed only so the
    // UI can say so honestly — never render it as a usable button.
    Q_PROPERTY(bool serverOffersUnsupportedSso READ serverOffersUnsupportedSso
                   NOTIFY discoveryChanged)
    // True from the moment a browser sign-in starts until it resolves.
    Q_PROPERTY(bool browserLoginInProgress READ browserLoginInProgress
                   NOTIFY browserLoginInProgressChanged)

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
    // Legacy Matrix SSO is NOT implemented and is not merely unfinished: the
    // SDK's login_sso helper is gated behind the sso-login/local-server
    // features, whose axum dependency is not vendored in this offline
    // --locked build. Reporting false keeps the UI honest.
    bool supportsSsoLogin()  const { return false; }
    // OAuth 2.0 / OIDC, answered by the backend rather than hardcoded.
    bool supportsOidcLogin() const;

    QString discoveryState() const { return m_discoveryState; }
    QString discoveredHomeserver() const { return m_discoveredHomeserver; }
    bool serverOffersPassword() const { return m_serverPassword; }
    bool serverOffersBrowserLogin() const { return m_serverOauth; }
    bool serverOffersUnsupportedSso() const { return m_serverSso; }
    bool browserLoginInProgress() const { return m_browserLoginInProgress; }

    // Ask the homeserver what it offers. Answers through discoveryChanged.
    Q_INVOKABLE void discoverAuthMethods(const QString &homeserver);
    // Start an OAuth browser sign-in against the entered homeserver.
    Q_INVOKABLE void beginBrowserLogin(const QString &homeserver);
    // User pressed Cancel, or closed the browser. Always resolves the UI.
    Q_INVOKABLE void cancelBrowserLogin();

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
    void discoveryChanged();
    void browserLoginInProgressChanged();
    void loginSucceeded();
    void loginFailed(const QString &reason);
    void loggedOut();

private:
    void setLoggingIn(bool v);
    void setLastError(const QString &err);
    void setLoginStage(const QString &stage);

    void setBrowserLoginInProgress(bool v);

    MatrixClient *m_client = nullptr;
    bool m_loggingIn = false;
    QString m_lastError;
    QString m_loginStage = QStringLiteral("idle");
    QString m_discoveryState = QStringLiteral("idle");
    QString m_discoveredHomeserver;
    bool m_serverPassword = false;
    bool m_serverOauth = false;
    bool m_serverSso = false;
    bool m_browserLoginInProgress = false;
};
