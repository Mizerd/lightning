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
    // The server offers legacy Matrix SSO (m.login.sso). Since 0.7.6+ this is
    // a USABLE flow — see rust/src/sso.rs — so the UI renders a real action
    // rather than the dead end this property used to describe. The old name is
    // gone deliberately: leaving "Unsupported" in it would have kept every
    // reader believing the feature was still absent.
    Q_PROPERTY(bool serverOffersSso READ serverOffersSso NOTIFY discoveryChanged)
    // Identity providers the server advertises for SSO: a list of
    // {id, name, icon} maps. EMPTY while the server offers SSO is normal and
    // common — it means one unnamed flow, and the UI offers a single generic
    // action rather than a chooser. Populated by discovery, so nothing
    // speculative is shown.
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

    // Password login is available on all compiled backends. Rust may still
    // surface SDK-side login errors through MatrixClient::login().
    bool supportsPasswordLogin() const { return true; }
    // Legacy Matrix SSO, answered by the backend rather than hardcoded. The
    // SDK's login_sso CONVENIENCE helper is still unavailable (it needs the
    // sso-login feature's axum dependency), but the two primitives underneath
    // it — get_sso_login_url and login_token — are not feature-gated, so the
    // flow is implemented on those plus Lightning's existing loopback listener.
    bool supportsSsoLogin() const;
    // OAuth 2.0 / OIDC, answered by the backend rather than hardcoded.
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
