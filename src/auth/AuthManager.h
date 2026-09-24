#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

class MatrixClient;

class AuthManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool isLoggingIn READ isLoggingIn NOTIFY isLoggingInChanged)
    Q_PROPERTY(bool isLoggedIn READ isLoggedIn NOTIFY isLoggedInChanged)
    Q_PROPERTY(QString currentUserId READ currentUserId NOTIFY isLoggedInChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

    // Sign-in progress as a stable token for QML to branch on. Only stages
    // the backend can actually observe are reported.
    //
    //   idle           no sign-in in flight
    //   connecting     credentials accepted locally; SDK handle + local store
    //                  are being opened
    //   authenticating store is open, the homeserver request is in flight
    //   starting_sync  the server accepted the login; sync is being started
    //   ready          sync is running
    Q_PROPERTY(QString loginStage READ loginStage NOTIFY loginStageChanged)

    // What this build can do, independent of any server.
    Q_PROPERTY(bool supportsPasswordLogin READ supportsPasswordLogin CONSTANT)
    Q_PROPERTY(bool supportsSsoLogin      READ supportsSsoLogin      CONSTANT)
    Q_PROPERTY(bool supportsOidcLogin     READ supportsOidcLogin     CONSTANT)

    // What the entered homeserver advertises; all false until discovery
    // completes.
    //
    //   idle       no homeserver resolved yet
    //   probing    discovery in flight
    //   done       the server answered
    //   failed     the server could not be asked
    Q_PROPERTY(QString discoveryState READ discoveryState NOTIFY discoveryChanged)
    Q_PROPERTY(QString discoveredHomeserver READ discoveredHomeserver NOTIFY discoveryChanged)
    Q_PROPERTY(bool serverOffersPassword READ serverOffersPassword NOTIFY discoveryChanged)
    Q_PROPERTY(bool serverOffersBrowserLogin READ serverOffersBrowserLogin NOTIFY discoveryChanged)
    // The server offers legacy Matrix SSO (m.login.sso); see rust/src/sso.rs.
    Q_PROPERTY(bool serverOffersSso READ serverOffersSso NOTIFY discoveryChanged)
    // SSO identity providers as {id, name, icon} maps. Empty is common and
    // means a single unnamed flow.
    Q_PROPERTY(QVariantList ssoProviders READ ssoProviders NOTIFY discoveryChanged)
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

    bool supportsPasswordLogin() const { return true; }
    // Legacy SSO is built on the ungated get_sso_login_url/login_token
    // primitives; the SDK's login_sso helper needs the unvendored axum feature.
    bool supportsSsoLogin() const;
    bool supportsOidcLogin() const;

    QString discoveryState() const { return m_discoveryState; }
    QString discoveredHomeserver() const { return m_discoveredHomeserver; }
    bool serverOffersPassword() const { return m_serverPassword; }
    bool serverOffersBrowserLogin() const { return m_serverOauth; }
    bool serverOffersSso() const { return m_serverSso; }
    QVariantList ssoProviders() const { return m_ssoProviders; }
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

    // Start a legacy Matrix SSO sign-in. `idpId` selects one advertised
    // identity provider; empty means the server's default single flow.
    Q_INVOKABLE void beginSsoLogin(const QString &homeserver,
                                   const QString &idpId = QString());
    // Retained name for the existing QML surface; OAuth/OIDC is one flow.
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
    QVariantList m_ssoProviders;
    bool m_browserLoginInProgress = false;
};
