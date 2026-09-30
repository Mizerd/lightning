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
    //   failed     the server could not be asked, or the text is not an
    //              address (nothing is asked then)
    Q_PROPERTY(QString discoveryState READ discoveryState NOTIFY discoveryChanged)
    // The address being asked, as normalizedServerAddress() gives it.
    Q_PROPERTY(QString discoveredHomeserver READ discoveredHomeserver NOTIFY discoveryChanged)
    Q_PROPERTY(bool serverOffersPassword READ serverOffersPassword NOTIFY discoveryChanged)
    Q_PROPERTY(bool serverOffersBrowserLogin READ serverOffersBrowserLogin NOTIFY discoveryChanged)
    // The server offers legacy Matrix SSO (m.login.sso); see rust/src/sso.rs.
    Q_PROPERTY(bool serverOffersSso READ serverOffersSso NOTIFY discoveryChanged)
    // SSO identity providers as {id, name, icon, brand} maps. Empty is common
    // and means a single unnamed flow. `brand` is MSC2858's ("google", ...).
    Q_PROPERTY(QVariantList ssoProviders READ ssoProviders NOTIFY discoveryChanged)
    // Why the state is "failed" or offers nothing: "" | "not_an_address" |
    // "unreachable" | "unsupported" (answered, but no method this build has).
    Q_PROPERTY(QString discoveryProblem READ discoveryProblem NOTIFY discoveryChanged)
    // The client API base URL the SDK resolved is plain http and not on this
    // machine: a password and everything after it travel unencrypted. A
    // warning only; a local development server needs http.
    Q_PROPERTY(bool serverConnectionInsecure READ serverConnectionInsecure
                   NOTIFY discoveryChanged)
    // The server's own sign-in page can create an account (prompt=create).
    Q_PROPERTY(bool serverCanCreateAccount READ serverCanCreateAccount
                   NOTIFY discoveryChanged)
    // The server's sign-in page publishes an account page (where a password
    // is reset); openAccountPage() opens it.
    Q_PROPERTY(bool serverOffersAccountPage READ serverOffersAccountPage
                   NOTIFY discoveryChanged)
    // True from the moment a browser sign-in starts until it resolves.
    Q_PROPERTY(bool browserLoginInProgress READ browserLoginInProgress
                   NOTIFY browserLoginInProgressChanged)

public:
    explicit AuthManager(MatrixClient *client, QObject *parent = nullptr);

    // The homeserver field's text as the one form every sign-in path hands
    // the backend: "https://host[:port][/path]", or "http://..." only when
    // http:// was typed. Accepts "matrix.org", any scheme or host case, spaces,
    // a trailing slash, a pasted API path (/_matrix/..., /.well-known/...) and
    // a pasted Matrix ID (@you:matrix.org). Without a scheme the text must be
    // a server name as Matrix writes one, so a password typed here by mistake
    // is not looked up. It still goes through the SDK's discovery, which
    // strips the scheme again and asks the server name (with a path, it
    // checks the URL itself); nothing here guesses a client API URL. Empty for
    // text that cannot be a server address, including any with a user name or
    // password in it.
    static QString normalizedServerAddress(const QString &typed);

    bool isLoggingIn() const { return m_loggingIn; }
    // Whether the latest loginSucceeded came from a sign-in the user started
    // here (password, browser/OAuth, SSO) rather than a restored session.
    // Read from a loginSucceeded handler.
    bool lastSignInWasInteractive() const { return m_lastSignInInteractive; }
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
    QString discoveryProblem() const;
    bool serverConnectionInsecure() const { return m_serverInsecure; }
    bool serverCanCreateAccount() const { return m_serverOauth && m_serverCanCreate; }
    bool serverOffersAccountPage() const
    {
        return m_serverOauth && !m_accountManagementUrl.isEmpty();
    }
    bool browserLoginInProgress() const { return m_browserLoginInProgress; }

    // True for a plain-http URL whose host is not this machine.
    static bool isInsecureRemoteUrl(const QString &url);

    // Ask the homeserver what it offers. Answers through discoveryChanged.
    Q_INVOKABLE void discoverAuthMethods(const QString &homeserver);
    // Start an OAuth browser sign-in against the entered homeserver.
    Q_INVOKABLE void beginBrowserLogin(const QString &homeserver);
    // The same flow, asking the server's page to create an account. Refused
    // unless discovery said the server can.
    Q_INVOKABLE void beginBrowserSignUp(const QString &homeserver);
    // Open the discovered server's account page in the browser. False when
    // there is none.
    Q_INVOKABLE bool openAccountPage();
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
    // Runs a discovery asked for while a browser sign-in was in flight.
    void runDeferredDiscovery();
    // Refuses a sign-in whose server text is not an address, without asking
    // the backend.
    void refuseServerAddress();
    // False, with the reason shown, when the account record at the end of a
    // browser sign-in would refuse this address: said before the browser
    // opens, never after the user has signed in there.
    bool refuseBeforeTheBrowser(const QString &homeserver);

    MatrixClient *m_client = nullptr;
    bool m_loggingIn = false;
    bool m_lastSignInInteractive = false;
    QString m_lastError;
    QString m_loginStage = QStringLiteral("idle");
    QString m_discoveryState = QStringLiteral("idle");
    QString m_discoveredHomeserver;
    bool m_serverPassword = false;
    bool m_serverOauth = false;
    bool m_serverSso = false;
    QVariantList m_ssoProviders;
    bool m_browserLoginInProgress = false;
    // A discovery asked for during a browser sign-in, run when it ends.
    bool m_discoveryDeferred = false;
    QString m_deferredDiscovery;
    // The answer of a probe abandoned for text that asks nothing.
    bool m_dropNextDiscovery = false;
    // authDiscoveryDetails() for the answer that follows it.
    QString m_detailsFor;
    QVariantMap m_details;
    bool m_serverReachable = true;
    bool m_serverInsecure = false;
    bool m_serverCanCreate = false;
    QString m_accountManagementUrl;
};
