#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include <functional>

class MatrixClient;

class AuthManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool isLoggingIn READ isLoggingIn NOTIFY isLoggingInChanged)
    Q_PROPERTY(bool isLoggedIn READ isLoggedIn NOTIFY isLoggedInChanged)
    Q_PROPERTY(bool logoutPending READ logoutPending NOTIFY logoutPendingChanged)
    Q_PROPERTY(QString currentUserId READ currentUserId NOTIFY isLoggedInChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

    Q_PROPERTY(QString loginStage READ loginStage NOTIFY loginStageChanged)

    Q_PROPERTY(bool supportsPasswordLogin READ supportsPasswordLogin CONSTANT)
    Q_PROPERTY(bool supportsSsoLogin      READ supportsSsoLogin      CONSTANT)
    Q_PROPERTY(bool supportsOidcLogin     READ supportsOidcLogin     CONSTANT)

    Q_PROPERTY(QString discoveryState READ discoveryState NOTIFY discoveryChanged)
    Q_PROPERTY(QString discoveredHomeserver READ discoveredHomeserver NOTIFY discoveryChanged)
    Q_PROPERTY(bool serverOffersPassword READ serverOffersPassword NOTIFY discoveryChanged)
    Q_PROPERTY(bool serverOffersBrowserLogin READ serverOffersBrowserLogin NOTIFY discoveryChanged)
    Q_PROPERTY(bool serverOffersSso READ serverOffersSso NOTIFY discoveryChanged)
    Q_PROPERTY(QVariantList ssoProviders READ ssoProviders NOTIFY discoveryChanged)
    Q_PROPERTY(QString discoveryProblem READ discoveryProblem NOTIFY discoveryChanged)
    Q_PROPERTY(bool serverConnectionInsecure READ serverConnectionInsecure
                   NOTIFY discoveryChanged)
    Q_PROPERTY(bool serverCanCreateAccount READ serverCanCreateAccount
                   NOTIFY discoveryChanged)
    Q_PROPERTY(bool serverOffersAccountPage READ serverOffersAccountPage
                   NOTIFY discoveryChanged)
    Q_PROPERTY(bool browserLoginInProgress READ browserLoginInProgress
                   NOTIFY browserLoginInProgressChanged)
    Q_PROPERTY(bool browserLoginUnavailable READ browserLoginUnavailable
                   NOTIFY discoveryChanged)

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
    bool lastSignInWasInteractive() const { return m_lastSignInInteractive; }
    bool isLoggedIn() const;
    QString currentUserId() const;
    QString lastError() const { return m_lastError; }
    QString loginStage() const { return m_loginStage; }

    bool supportsPasswordLogin() const { return true; }
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
    bool browserLoginUnavailable() const { return m_browserLoginUnavailable; }

    static bool isInsecureRemoteUrl(const QString &url);

    Q_INVOKABLE void discoverAuthMethods(const QString &homeserver, bool force = false);
    Q_INVOKABLE void beginBrowserLogin(const QString &homeserver);
    Q_INVOKABLE void beginBrowserSignUp(const QString &homeserver);
    Q_INVOKABLE bool openAccountPage();
    Q_INVOKABLE void cancelBrowserLogin();
    Q_INVOKABLE void cancelLogin();

    Q_INVOKABLE void login(const QString &homeserver,
                           const QString &user,
                           const QString &password);
    Q_INVOKABLE void logout();
    /// Runs before every sign-out, with a `proceed` it must call exactly once
    /// when the session may go. AppController leaves a live call there, so
    /// the MatrixRTC membership retraction is sent while the session can
    /// still send it (live 2026-10-07: the logout ran first, the retraction
    /// "could not be dispatched", and a ghost participant stayed ~5 min).
    /// A second logout() while one is waiting is ignored. The deferred
    /// logout applies only to the session it was asked for: if another
    /// session became current meanwhile (same user id AND the same session
    /// generation), it is dropped and logoutAbandoned() says so, because the
    /// client's logout() acts on whatever session it holds now and would
    /// sign out, and delete the store of, the wrong account.
    using BeforeLogout = std::function<void(std::function<void()> proceed)>;
    void setBeforeLogout(BeforeLogout hook) { m_beforeLogout = std::move(hook); }
    bool logoutPending() const { return m_logoutPending; }
    Q_INVOKABLE void restoreSession();
    Q_INVOKABLE void clearLastError();

    Q_INVOKABLE void beginSsoLogin(const QString &homeserver,
                                   const QString &idpId = QString());
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
    void logoutPendingChanged();
    void logoutAbandoned();

public:
    void setAbandonedLoginBoundMsForTest(qint64 ms) { m_abandonedLoginBoundMs = ms; }

private:
    void setLoggingIn(bool v);
    void setLastError(const QString &err);
    void setLoginStage(const QString &stage);

    void setBrowserLoginInProgress(bool v);
    QString browserLaunchFailureText() const;
    bool refuseWhileAbandonedLoginRuns();
    static QString abandonedWaitText();
    void runDeferredDiscovery();
    void refuseServerAddress();
    bool refuseBeforeTheBrowser(const QString &homeserver);

    MatrixClient *m_client = nullptr;
    void setLogoutPending(bool pending);

    BeforeLogout m_beforeLogout;
    bool m_logoutPending = false;
    quint64 m_sessionGeneration = 0;
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
    bool m_browserLaunchFailed = false;
    bool m_browserLoginUnavailable = false;
    bool m_errorSurvivesRestore = false;
    QElapsedTimer m_abandonedLogin;
    QTimer m_abandonedWaitNote;
    // How long a new sign-in waits for a cancelled one to answer. The request
    // gave up in under two minutes in the 2026-10-15 break-it run (/login held
    // for 130 s: "reason= timeout"); past the bound the new attempt goes ahead
    // and replaces the old handle, rather than locking sign-in for the session.
    qint64 m_abandonedLoginBoundMs = 150 * 1000;
    bool m_discoveryDeferred = false;
    QString m_deferredDiscovery;
    bool m_dropNextDiscovery = false;
    QString m_detailsFor;
    QVariantMap m_details;
    bool m_serverReachable = true;
    bool m_serverInsecure = false;
    bool m_serverCanCreate = false;
    QString m_accountManagementUrl;
};
