
#include "auth/AuthManager.h"
#include "matrix/MockMatrixClient.h"
#include "storage/AppDataPaths.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <utility>

namespace {

class RecordingClient : public MockMatrixClient
{
    Q_OBJECT
public:
    struct Login {
        QString homeserver;
        QString user;
        QString password;
    };

    bool supportsOAuthLogin() const override { return true; }
    bool supportsSsoLogin() const override { return true; }

    void login(const QString &homeserver, const QString &user,
               const QString &password) override
    {
        logins.append({ homeserver, user, password });
    }
    void discoverAuthMethods(const QString &homeserver) override
    {
        discoveries.append(homeserver);
    }
    void beginOAuthLogin(const QString &homeserver) override
    {
        oauthStarts.append(homeserver);
        oauthInFlight = true;
    }
    void beginOAuthSignUp(const QString &homeserver) override
    {
        signUps.append(homeserver);
        oauthInFlight = true;
    }
    void beginSsoLogin(const QString &homeserver, const QString &idpId) override
    {
        ssoStarts.append(qMakePair(homeserver, idpId));
        ssoInFlight = true;
    }
    void requestSsoProviders(const QString &homeserver) override
    {
        providerRequests.append(homeserver);
    }
    void cancelOAuthLogin() override
    {
        if (!std::exchange(oauthInFlight, false))
            return;
        ++oauthCancels;
        Q_EMIT loginFailed(QStringLiteral("Sign-in was cancelled."));
    }
    void cancelSsoLogin() override
    {
        if (!std::exchange(ssoInFlight, false))
            return;
        ++ssoCancels;
        Q_EMIT loginFailed(QStringLiteral("Sign-in was cancelled."));
    }

    void announce(const QString &homeserver)
    {
        Q_EMIT authMethodsDiscovered(homeserver, true, true, true);
    }
    void answer(const QString &homeserver, bool password, bool oauth, bool sso,
                const QVariantMap &details)
    {
        Q_EMIT authDiscoveryDetails(homeserver, details);
        Q_EMIT authMethodsDiscovered(homeserver, password, oauth, sso);
    }
    void detailsOnly(const QString &homeserver, const QVariantMap &details)
    {
        Q_EMIT authDiscoveryDetails(homeserver, details);
    }
    void methodsOnly(const QString &homeserver, bool password, bool oauth, bool sso)
    {
        Q_EMIT authMethodsDiscovered(homeserver, password, oauth, sso);
    }
    void endAttempt()
    {
        oauthInFlight = false;
        ssoInFlight = false;
        Q_EMIT loginFailed(QStringLiteral("Sign-in was cancelled."));
    }
    void refuseThePage(const QString &url)
    {
        Q_EMIT browserLaunchFailed();
        if (oauthInFlight)
            Q_EMIT oauthBrowserUrlReady(url);
        else
            Q_EMIT ssoBrowserUrlReady(url);
    }

    bool oauthInFlight = false;
    bool ssoInFlight = false;
    int oauthCancels = 0;
    int ssoCancels = 0;
    QList<Login> logins;
    QStringList discoveries;
    QStringList oauthStarts;
    QStringList signUps;
    QStringList providerRequests;
    QList<QPair<QString, QString>> ssoStarts;
};

// A backend with no browser sign-in at all (the mock, the HTTP backend).
class PasswordOnlyClient : public MockMatrixClient
{
    Q_OBJECT
public:
    void discoverAuthMethods(const QString &homeserver) override
    {
        discoveries.append(homeserver);
    }
    QStringList discoveries;
};

QVariantMap details(bool reachable, const QString &resolved = QString(),
                    bool canCreate = false, const QString &account = QString())
{
    return {
        { QStringLiteral("reachable"), reachable },
        { QStringLiteral("resolvedHomeserver"), resolved },
        { QStringLiteral("oauthCanCreate"), canCreate },
        { QStringLiteral("accountManagementUrl"), account },
    };
}

const QString kNotAnAddress =
    QStringLiteral("That is not a server address. Enter one like matrix.org.");
const QString kStoppedWaiting =
    QStringLiteral("Stopped waiting for the server. If it still completes this "
                   "sign-in, the account may open on its own.");

}

class ServerAddressInputTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dataHome;

private slots:
    void initTestCase()
    {
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        qputenv("HOME", m_dataHome.path().toUtf8());
    }

    void acceptedForms_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::addColumn<QString>("expected");

        const QString mo = QStringLiteral("https://matrix.org");
        QTest::newRow("bare") << QStringLiteral("matrix.org") << mo;
        QTest::newRow("https") << QStringLiteral("https://matrix.org") << mo;
        QTest::newRow("case, trailing slash")
            << QStringLiteral("HTTPS://Matrix.org/") << mo;
        QTest::newRow("spaces") << QStringLiteral("  matrix.org  ") << mo;
        QTest::newRow("tab, newline") << QStringLiteral("\tmatrix.org\n") << mo;
        QTest::newRow("slash") << QStringLiteral("https://matrix.org/") << mo;
        QTest::newRow("slashes") << QStringLiteral("matrix.org///") << mo;
        QTest::newRow("api path")
            << QStringLiteral("https://matrix.org/_matrix/client") << mo;
        QTest::newRow("endpoint with query")
            << QStringLiteral("https://matrix.org/_matrix/client/v3/login?x=1") << mo;
        QTest::newRow("well-known")
            << QStringLiteral("matrix.org/.well-known/matrix/client") << mo;
        QTest::newRow("fragment") << QStringLiteral("https://matrix.org/#/login") << mo;
        QTest::newRow("matrix id") << QStringLiteral("@user:matrix.org") << mo;
        QTest::newRow("matrix id, case, spaces")
            << QStringLiteral(" @Alice:Example.ORG ")
            << QStringLiteral("https://example.org");
        QTest::newRow("matrix id with port")
            << QStringLiteral("@alice:localhost:8008")
            << QStringLiteral("https://localhost:8008");
        QTest::newRow("bare with port")
            << QStringLiteral("localhost:8448")
            << QStringLiteral("https://localhost:8448");
        QTest::newRow("http local")
            << QStringLiteral("http://localhost:8008")
            << QStringLiteral("http://localhost:8008");
        QTest::newRow("http local, case, slash")
            << QStringLiteral("HTTP://LOCALHOST:8008/")
            << QStringLiteral("http://localhost:8008");
        QTest::newRow("http remote")
            << QStringLiteral("http://matrix.example.org")
            << QStringLiteral("http://matrix.example.org");
        QTest::newRow("ipv6")
            << QStringLiteral("https://[::1]:8448")
            << QStringLiteral("https://[::1]:8448");
        QTest::newRow("subpath")
            << QStringLiteral("https://example.com/matrix/")
            << QStringLiteral("https://example.com/matrix");
        QTest::newRow("typed api path, case")
            << QStringLiteral("matrix.org/_MATRIX/client") << mo;
        QTest::newRow("well-known, case")
            << QStringLiteral("https://matrix.org/.Well-Known/matrix/client") << mo;
        QTest::newRow("dot segments")
            << QStringLiteral("https://matrix.org/a/../_matrix/client") << mo;
        QTest::newRow("matrix id, localhost")
            << QStringLiteral("@alice:localhost")
            << QStringLiteral("https://localhost");
    }

    void acceptedForms()
    {
        QFETCH(QString, typed);
        QFETCH(QString, expected);
        const QString out = AuthManager::normalizedServerAddress(typed);
        QCOMPARE(out, expected);
        QCOMPARE(AuthManager::normalizedServerAddress(out), out);
        matrix::app_data::AccountIdentity identity;
        QVERIFY(matrix::app_data::resolveAccountIdentity(
            out, QStringLiteral("@alice:example.org"), &identity));
        QCOMPARE(identity.homeserver, out);
    }

    void refusedForms_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::newRow("empty") << QString();
        QTest::newRow("blank") << QStringLiteral("   ");
        QTest::newRow("ftp") << QStringLiteral("ftp://matrix.org");
        QTest::newRow("mxc") << QStringLiteral("mxc://matrix.org/abc");
        QTest::newRow("email-like") << QStringLiteral("alice@matrix.org");
        QTest::newRow("credentials") << QStringLiteral("alice:secret@matrix.org");
        QTest::newRow("credentials with scheme")
            << QStringLiteral("https://alice:secret@matrix.org");
        QTest::newRow("id without server") << QStringLiteral("@alice");
        QTest::newRow("id without user") << QStringLiteral("@:matrix.org");
        QTest::newRow("id with empty server") << QStringLiteral("@alice:");
        QTest::newRow("space in host") << QStringLiteral("matrix .org");
        QTest::newRow("scheme only") << QStringLiteral("https://");
        QTest::newRow("one slash") << QStringLiteral("https:/matrix.org");
        QTest::newRow("no slashes") << QStringLiteral("HTTP:matrix.org");
        QTest::newRow("password with ?") << QStringLiteral("Summer2026?");
        QTest::newRow("password with #") << QStringLiteral("Pass#1word");
        QTest::newRow("password with /") << QStringLiteral("Winter/2026");
        QTest::newRow("password like an id") << QStringLiteral("@lice:Secret99");
        QTest::newRow("fullwidth") << QStringLiteral("\uFF30assword");
        QTest::newRow("not an api path") << QStringLiteral("example.com/_matrixish");
        QTest::newRow("empty port") << QStringLiteral("matrix.org:");
        QTest::newRow("doubled scheme") << QStringLiteral("https://https://matrix.org");
        QTest::newRow("doubled, mixed") << QStringLiteral("http://https://matrix.org");
        QTest::newRow("doubled, one slash") << QStringLiteral("https://https:/matrix.org");
        QTest::newRow("doubled, no colon") << QStringLiteral("https://http//matrix.org");
        QTest::newRow("dots to a second host")
            << QStringLiteral("https://matrix.org/a/..//evil.com");
        QTest::newRow("dots out of an api path")
            << QStringLiteral("matrix.org/_matrix/../../evil");
        // Only a fixed point is an answer: an encoded slash would decode into
        // a new separator on the next pass.
        QTest::newRow("encoded slash to a second host")
            << QStringLiteral("https://matrix.org/a/..%2F/evil.com");
        QTest::newRow("encoded slashes climbing")
            << QStringLiteral("https://matrix.org/x/..%2F..%2F");
        QTest::newRow("too long")
            << QStringLiteral("matrix.org/_matrix/") + QString(2100, QLatin1Char('a'));
        QTest::newRow("bad port") << QStringLiteral("https://matrix.org:99999");
    }

    void refusedForms()
    {
        QFETCH(QString, typed);
        QCOMPARE(AuthManager::normalizedServerAddress(typed), QString());
    }

    void discoveryAsksForTheNormalizedAddress()
    {
        RecordingClient client;
        AuthManager auth(&client);
        auth.discoverAuthMethods(QStringLiteral(" HTTPS://Matrix.org/ "));
        QCOMPARE(client.discoveries, QStringList{ QStringLiteral("https://matrix.org") });
        QCOMPARE(auth.discoveredHomeserver(), QStringLiteral("https://matrix.org"));
        QCOMPARE(auth.discoveryState(), QStringLiteral("probing"));
    }

    void textThatIsNotAnAddressIsNeverProbed()
    {
        RecordingClient client;
        AuthManager auth(&client);
        auth.discoverAuthMethods(QStringLiteral("alice:secret@matrix.org"));
        QVERIFY(client.discoveries.isEmpty());
        QCOMPARE(auth.discoveryState(), QStringLiteral("failed"));
        QVERIFY(auth.discoveredHomeserver().isEmpty());

        auth.discoverAuthMethods(QStringLiteral("Summer2026?"));
        QVERIFY(client.discoveries.isEmpty());
        QCOMPARE(auth.discoveryState(), QStringLiteral("failed"));

        auth.discoverAuthMethods(QString());
        QVERIFY(client.discoveries.isEmpty());
        QCOMPARE(auth.discoveryState(), QStringLiteral("idle"));
    }

    void aLateAnswerForTheServerBeforeIsDropped()
    {
        RecordingClient client;
        AuthManager auth(&client);
        auth.discoverAuthMethods(QStringLiteral("matrix.org"));
        auth.discoverAuthMethods(QStringLiteral("matrix.org@"));
        client.announce(QStringLiteral("https://matrix.org"));
        QCOMPARE(auth.discoveryState(), QStringLiteral("failed"));
        QVERIFY(auth.discoveredHomeserver().isEmpty());
        QVERIFY(!auth.serverOffersPassword());
        QVERIFY(!auth.serverOffersBrowserLogin());
        QVERIFY(!auth.serverOffersSso());

        auth.discoverAuthMethods(QStringLiteral("example.org"));
        auth.discoverAuthMethods(QString());
        auth.discoverAuthMethods(QString());
        client.announce(QStringLiteral("https://example.org"));
        QCOMPARE(auth.discoveryState(), QStringLiteral("idle"));
        QVERIFY(!auth.serverOffersBrowserLogin());

        auth.discoverAuthMethods(QStringLiteral("example.org"));
        client.announce(QStringLiteral("https://example.org"));
        QCOMPARE(auth.discoveryState(), QStringLiteral("done"));
        QVERIFY(auth.serverOffersBrowserLogin());
    }

    void passwordSignInGetsTheNormalizedAddressAndUntouchedCredentials()
    {
        RecordingClient client;
        AuthManager auth(&client);
        auth.login(QStringLiteral("@alice:matrix.org"), QStringLiteral("bob"),
                   QStringLiteral("pw x"));
        QCOMPARE(client.logins.size(), 1);
        QCOMPARE(client.logins.at(0).homeserver, QStringLiteral("https://matrix.org"));
        QCOMPARE(client.logins.at(0).user, QStringLiteral("bob"));
        QCOMPARE(client.logins.at(0).password, QStringLiteral("pw x"));
    }

    void anAddressThatIsNotOneIsRefusedWithoutAskingTheBackend()
    {
        RecordingClient client;
        AuthManager auth(&client);
        QSignalSpy failed(&auth, &AuthManager::loginFailed);

        auth.login(QStringLiteral("alice:secret@matrix.org"), QStringLiteral("alice"),
                   QStringLiteral("pw"));
        auth.beginBrowserLogin(QStringLiteral("ftp://matrix.org"));
        auth.beginSsoLogin(QStringLiteral("matrix .org"), QStringLiteral("idp"));

        QVERIFY(client.logins.isEmpty());
        QVERIFY(client.oauthStarts.isEmpty());
        QVERIFY(client.ssoStarts.isEmpty());
        QCOMPARE(failed.count(), 3);
        QCOMPARE(auth.lastError(), kNotAnAddress);
        QVERIFY(!auth.isLoggingIn());
        QVERIFY(!auth.browserLoginInProgress());
    }

    void anEmptyAddressStillReachesTheBackend()
    {
        RecordingClient client;
        AuthManager auth(&client);
        auth.login(QStringLiteral("  "), QStringLiteral("alice"), QStringLiteral("pw"));
        QCOMPARE(client.logins.size(), 1);
        QCOMPARE(client.logins.at(0).homeserver, QString());
    }

    void browserSignInsGetTheNormalizedAddress()
    {
        RecordingClient client;
        AuthManager auth(&client);
        auth.beginBrowserLogin(QStringLiteral("@alice:matrix.org"));
        QCOMPARE(client.oauthStarts, QStringList{ QStringLiteral("https://matrix.org") });
        client.endAttempt();

        auth.beginSsoLogin(QStringLiteral("HTTP://LOCALHOST:8008/"), QStringLiteral("idp"));
        QCOMPARE(client.ssoStarts.size(), 1);
        QCOMPARE(client.ssoStarts.at(0).first, QStringLiteral("http://localhost:8008"));
        QCOMPARE(client.ssoStarts.at(0).second, QStringLiteral("idp"));
    }

    void aDeferredProbeOfTheSameServerSpelledDifferentlyIsNotRepeated()
    {
        RecordingClient client;
        AuthManager auth(&client);
        auth.discoverAuthMethods(QStringLiteral("matrix.org"));
        client.announce(QStringLiteral("https://matrix.org"));
        QCOMPARE(auth.discoveryState(), QStringLiteral("done"));
        client.discoveries.clear();

        auth.beginSsoLogin(QStringLiteral("matrix.org"), QString());
        auth.discoverAuthMethods(QStringLiteral("Matrix.org/"));
        client.endAttempt();
        QVERIFY(client.discoveries.isEmpty());
        QCOMPARE(auth.discoveryState(), QStringLiteral("done"));

        auth.beginSsoLogin(QStringLiteral("matrix.org"), QString());
        auth.discoverAuthMethods(QStringLiteral("example.org"));
        client.endAttempt();
        QCOMPARE(client.discoveries, QStringList{ QStringLiteral("https://example.org") });
    }

    void theServersOwnPageBringsSignUpAndItsAccountPage()
    {
        RecordingClient client;
        AuthManager auth(&client);
        auth.discoverAuthMethods(QStringLiteral("matrix.org"));
        client.answer(QStringLiteral("https://matrix.org"), true, true, true,
                      details(true, QStringLiteral("https://matrix-client.matrix.org/"), true,
                              QStringLiteral("https://account.matrix.org/account/")));
        QCOMPARE(auth.discoveryState(), QStringLiteral("done"));
        QVERIFY(auth.serverCanCreateAccount());
        QVERIFY(auth.serverOffersAccountPage());
        QVERIFY(!auth.serverConnectionInsecure());
        QCOMPARE(auth.discoveryProblem(), QString());

        QCoreApplication::processEvents();
        QVERIFY(client.providerRequests.isEmpty());

        auth.beginBrowserSignUp(QStringLiteral("https://matrix.org"));
        QCOMPARE(client.signUps, QStringList{ QStringLiteral("https://matrix.org") });
        QVERIFY(client.oauthStarts.isEmpty());
        QVERIFY(auth.browserLoginInProgress());
    }

    void signUpAndTheAccountPageNeedTheServerToOfferThem()
    {
        RecordingClient client;
        AuthManager auth(&client);
        QSignalSpy failed(&auth, &AuthManager::loginFailed);
        auth.discoverAuthMethods(QStringLiteral("example.org"));
        client.answer(QStringLiteral("https://example.org"), true, true, false,
                      details(true, QStringLiteral("https://example.org/"), false,
                              QStringLiteral("http://example.org/account/")));
        QVERIFY(!auth.serverCanCreateAccount());
        QVERIFY(!auth.serverOffersAccountPage());
        QVERIFY(!auth.openAccountPage());
        auth.beginBrowserSignUp(QStringLiteral("https://example.org"));
        QVERIFY(client.signUps.isEmpty());
        QCOMPARE(failed.count(), 1);
        QVERIFY(!auth.browserLoginInProgress());

        RecordingClient other;
        AuthManager auth2(&other);
        auth2.discoverAuthMethods(QStringLiteral("example.org"));
        other.answer(QStringLiteral("https://example.org"), true, false, true,
                     details(true, QString(), true,
                             QStringLiteral("https://example.org/account/")));
        QVERIFY(!auth2.serverCanCreateAccount());
        QVERIFY(!auth2.serverOffersAccountPage());
    }

    void theProblemSaysWhyNothingIsOffered()
    {
        RecordingClient client;
        AuthManager auth(&client);
        auth.discoverAuthMethods(QStringLiteral("example.org"));
        client.answer(QStringLiteral("https://example.org"), false, false, false,
                      details(false));
        QCOMPARE(auth.discoveryState(), QStringLiteral("failed"));
        QCOMPARE(auth.discoveryProblem(), QStringLiteral("unreachable"));

        auth.discoverAuthMethods(QStringLiteral("other.example"));
        client.answer(QStringLiteral("https://other.example"), false, false, false,
                      details(true, QStringLiteral("https://other.example/")));
        QCOMPARE(auth.discoveryProblem(), QStringLiteral("unsupported"));

        auth.discoverAuthMethods(QStringLiteral("Summer2026?"));
        QCOMPARE(auth.discoveryProblem(), QStringLiteral("not_an_address"));

        auth.discoverAuthMethods(QStringLiteral("third.example"));
        client.detailsOnly(QStringLiteral("https://elsewhere.example"), details(true));
        client.methodsOnly(QStringLiteral("https://third.example"), false, false, false);
        QCOMPARE(auth.discoveryProblem(), QStringLiteral("unreachable"));
    }

    void isInsecureRemoteUrl_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("insecure");
        QTest::newRow("https") << QStringLiteral("https://matrix.org/") << false;
        QTest::newRow("http remote") << QStringLiteral("http://matrix.example.org/") << true;
        QTest::newRow("http lan") << QStringLiteral("http://192.168.1.20:8008/") << true;
        QTest::newRow("HTTP remote") << QStringLiteral("HTTP://Matrix.Example.org") << true;
        QTest::newRow("localhost") << QStringLiteral("http://localhost:8008/") << false;
        QTest::newRow("dev.localhost") << QStringLiteral("http://matrix.localhost/") << false;
        QTest::newRow("127.0.0.1") << QStringLiteral("http://127.0.0.1:8008/") << false;
        QTest::newRow("127.x") << QStringLiteral("http://127.1.2.3/") << false;
        QTest::newRow("::1") << QStringLiteral("http://[::1]:8008/") << false;
        QTest::newRow("empty") << QString() << false;
    }

    void isInsecureRemoteUrl()
    {
        QFETCH(QString, url);
        QFETCH(bool, insecure);
        QCOMPARE(AuthManager::isInsecureRemoteUrl(url), insecure);
    }

    void theNotEncryptedWarningFollowsTheResolvedUrl()
    {
        RecordingClient client;
        AuthManager auth(&client);
        auth.discoverAuthMethods(QStringLiteral("example.org"));
        client.answer(QStringLiteral("https://example.org"), true, false, false,
                      details(true, QStringLiteral("http://matrix.example.org:8008/")));
        QVERIFY(auth.serverConnectionInsecure());

        auth.discoverAuthMethods(QStringLiteral("http://matrix.org"));
        client.answer(QStringLiteral("http://matrix.org"), true, false, false,
                      details(true, QStringLiteral("https://matrix-client.matrix.org/")));
        QVERIFY(!auth.serverConnectionInsecure());
    }

    void theSameServerIsNotAskedTwice()
    {
        RecordingClient client;
        AuthManager auth(&client);
        auth.discoverAuthMethods(QStringLiteral("matrix.org"));
        auth.discoverAuthMethods(QStringLiteral("https://matrix.org/"));
        QCOMPARE(client.discoveries.size(), 1);
        client.announce(QStringLiteral("https://matrix.org"));
        QSignalSpy changed(&auth, &AuthManager::discoveryChanged);
        auth.discoverAuthMethods(QStringLiteral("Matrix.org"));
        QCOMPARE(client.discoveries.size(), 1);
        QCOMPARE(changed.count(), 0);
        QVERIFY(auth.serverOffersBrowserLogin());

        auth.discoverAuthMethods(QStringLiteral("example.org"));
        client.answer(QStringLiteral("https://example.org"), false, false, false,
                      details(false));
        auth.discoverAuthMethods(QStringLiteral("example.org"));
        QCOMPARE(client.discoveries.size(), 3);
    }

    void aSignInTheAccountRecordWouldRefuseStopsBeforeTheBrowser()
    {
#ifdef Q_OS_WIN
        QSKIP("The data folder comes from LOCALAPPDATA there.");
#endif
        RecordingClient client;
        AuthManager auth(&client);
        auth.discoverAuthMethods(QStringLiteral("matrix.org"));
        client.announce(QStringLiteral("https://matrix.org"));

        const QByteArray home = qgetenv("HOME");
        const QByteArray data = qgetenv("XDG_DATA_HOME");
        qunsetenv("HOME");
        qunsetenv("XDG_DATA_HOME");
        auth.beginBrowserLogin(QStringLiteral("matrix.org"));
        auth.beginSsoLogin(QStringLiteral("matrix.org"), QString());
        const QString error = auth.lastError();
        qputenv("HOME", home);
        qputenv("XDG_DATA_HOME", data);

        QVERIFY(client.oauthStarts.isEmpty());
        QVERIFY(client.ssoStarts.isEmpty());
        QVERIFY2(error.contains(QStringLiteral("did not open your browser")), qPrintable(error));
        QVERIFY(!auth.browserLoginInProgress());

        auth.beginBrowserLogin(QStringLiteral("matrix.org"));
        QCOMPARE(client.oauthStarts, QStringList{ QStringLiteral("https://matrix.org") });
    }

    void anErrorGoesWhenTheServerChanges()
    {
        RecordingClient client;
        AuthManager auth(&client);
        auth.login(QStringLiteral("matrix .org"), QStringLiteral("a"), QStringLiteral("b"));
        QCOMPARE(auth.lastError(), kNotAnAddress);
        auth.discoverAuthMethods(QStringLiteral("example.org"));
        QVERIFY(auth.lastError().isEmpty());
    }

    void aBackendWithoutBrowserSignInOffersThePasswordAtOnce()
    {
        PasswordOnlyClient client;
        AuthManager auth(&client);
        auth.discoverAuthMethods(QStringLiteral("mock.local"));
        QVERIFY(client.discoveries.isEmpty());
        QCOMPARE(auth.discoveryState(), QStringLiteral("done"));
        QVERIFY(auth.serverOffersPassword());
        QVERIFY(!auth.serverOffersBrowserLogin());
        QCOMPARE(auth.discoveredHomeserver(), QStringLiteral("https://mock.local"));
    }

    void theBrowserButtonsActOnTheServerTheyName()
    {
        QFile file(QStringLiteral(QML_DIR "/LoginScreen.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString qml = QString::fromUtf8(file.readAll());
        QCOMPARE(qml.count(QStringLiteral("beginBrowserLogin(app.auth.discoveredHomeserver)")), 1);
        QCOMPARE(qml.count(QStringLiteral("beginSsoLogin(app.auth.discoveredHomeserver")), 2);
        QVERIFY(!qml.contains(QStringLiteral("beginBrowserLogin(homeserverField")));
        QVERIFY(!qml.contains(QStringLiteral("beginSsoLogin(homeserverField")));
        QCOMPARE(qml.count(QStringLiteral(
                     "enabled: !app.auth.isLoggingIn && !discoverDebounce.running")), 4);
    }

    void anExplicitCommitAsksAnAnsweredServerAgain()
    {
        RecordingClient client;
        AuthManager auth(&client);
        auth.discoverAuthMethods(QStringLiteral("matrix.org"));
        client.announce(QStringLiteral("https://matrix.org"));
        QCOMPARE(auth.discoveryState(), QStringLiteral("done"));
        QCOMPARE(client.discoveries.size(), 1);

        auth.discoverAuthMethods(QStringLiteral("matrix.org"));
        QCOMPARE(client.discoveries.size(), 1);

        auth.discoverAuthMethods(QStringLiteral("Matrix.org/"), true);
        QCOMPARE(client.discoveries,
                 QStringList({ QStringLiteral("https://matrix.org"),
                               QStringLiteral("https://matrix.org") }));
        QCOMPARE(auth.discoveryState(), QStringLiteral("probing"));
        QVERIFY(!auth.serverOffersBrowserLogin());

        auth.discoverAuthMethods(QStringLiteral("matrix.org"), true);
        QCOMPARE(client.discoveries.size(), 2);
    }

    void aFailedSignInHereSurvivesTheRestoreThatFollowsIt()
    {
        RecordingClient client;
        AuthManager auth(&client);
        const QString wrong = QStringLiteral("Wrong username or password.");
        auth.login(QStringLiteral("matrix.org"), QStringLiteral("bob"), QStringLiteral("pw"));
        QVERIFY(auth.isLoggingIn());
        Q_EMIT client.loginFailed(wrong);
        QCOMPARE(auth.lastError(), wrong);

        Q_EMIT client.loginSucceeded(QStringLiteral("@alice:example.org"));
        QCOMPARE(auth.lastError(), wrong);
        QVERIFY(!auth.lastSignInWasInteractive());

        Q_EMIT client.loginSucceeded(QStringLiteral("@alice:example.org"));
        QVERIFY(auth.lastError().isEmpty());

        auth.login(QStringLiteral("matrix.org"), QStringLiteral("bob"), QStringLiteral("pw"));
        Q_EMIT client.loginFailed(wrong);
        auth.login(QStringLiteral("matrix.org"), QStringLiteral("bob"), QStringLiteral("pw2"));
        Q_EMIT client.loginSucceeded(QStringLiteral("@bob:matrix.org"));
        QVERIFY(auth.lastError().isEmpty());
        QVERIFY(auth.lastSignInWasInteractive());
    }

    void clearingTheErrorIsAskable()
    {
        RecordingClient client;
        AuthManager auth(&client);
        auth.login(QStringLiteral("matrix .org"), QStringLiteral("a"), QStringLiteral("b"));
        QCOMPARE(auth.lastError(), kNotAnAddress);
        QVERIFY(QMetaObject::invokeMethod(&auth, "clearLastError"));
        QVERIFY(auth.lastError().isEmpty());
    }

    void cancellingAPasswordSignInGivesTheFormBack()
    {
        RecordingClient client;
        AuthManager auth(&client);
        QSignalSpy failed(&auth, &AuthManager::loginFailed);
        auth.cancelLogin();
        QCOMPARE(failed.count(), 0);

        auth.login(QStringLiteral("matrix.org"), QStringLiteral("bob"), QStringLiteral("pw"));
        QCOMPARE(client.logins.size(), 1);
        QVERIFY(auth.isLoggingIn());
        auth.cancelLogin();
        QVERIFY(!auth.isLoggingIn());
        QCOMPARE(auth.loginStage(), QStringLiteral("idle"));
        QCOMPARE(auth.lastError(), kStoppedWaiting);
        QCOMPARE(failed.count(), 1);

        const auto refusedForTheWait = [&](const char *what) {
            QVERIFY2(!auth.isLoggingIn(), what);
            QVERIFY2(!auth.browserLoginInProgress(), what);
            QVERIFY2(auth.lastError().contains(QStringLiteral("has not answered")),
                     qPrintable(QLatin1String(what) + QLatin1String(": ")
                                + auth.lastError()));
            QCOMPARE(failed.count(), 1);
        };
        auth.login(QStringLiteral("matrix.org"), QStringLiteral("bob"), QStringLiteral("pw2"));
        QCOMPARE(client.logins.size(), 1);
        refusedForTheWait("password");
        auth.beginBrowserLogin(QStringLiteral("matrix.org"));
        QVERIFY(client.oauthStarts.isEmpty());
        refusedForTheWait("browser");
        auth.beginSsoLogin(QStringLiteral("matrix.org"), QString());
        QVERIFY(client.ssoStarts.isEmpty());
        refusedForTheWait("sso");
        auth.beginBrowserSignUp(QStringLiteral("matrix.org"));
        QVERIFY(client.signUps.isEmpty());
        refusedForTheWait("sign-up");

        Q_EMIT client.loginFailed(QStringLiteral("Can't reach the server."));
        QCOMPARE(auth.lastError(), QStringLiteral("Can't reach the server."));
        auth.login(QStringLiteral("matrix.org"), QStringLiteral("bob"), QStringLiteral("pw2"));
        QCOMPARE(client.logins.size(), 2);
        QVERIFY(auth.isLoggingIn());

        auth.cancelLogin();
        Q_EMIT client.loginSucceeded(QStringLiteral("@alice:example.org"));
        QCOMPARE(auth.lastError(), kStoppedWaiting);
        auth.login(QStringLiteral("matrix.org"), QStringLiteral("bob"), QStringLiteral("pw3"));
        QCOMPARE(client.logins.size(), 3);
        auth.cancelLogin();
        Q_EMIT client.loginFailed(QStringLiteral("Can't reach the server."));
        auth.beginSsoLogin(QStringLiteral("matrix.org"), QString());
        QCOMPARE(client.ssoStarts.size(), 1);
        QVERIFY(auth.browserLoginInProgress());
    }

    void theCancelledSignInWaitLineEndsWithTheWait()
    {
        RecordingClient client;
        AuthManager auth(&client);
        auth.setAbandonedLoginBoundMsForTest(150);
        auth.login(QStringLiteral("matrix.org"), QStringLiteral("bob"), QStringLiteral("pw"));
        auth.cancelLogin();
        auth.login(QStringLiteral("matrix.org"), QStringLiteral("bob"), QStringLiteral("pw2"));
        QVERIFY(auth.lastError().contains(QStringLiteral("has not answered")));
        QTRY_VERIFY2(auth.lastError().isEmpty(), qPrintable(auth.lastError()));
        auth.login(QStringLiteral("matrix.org"), QStringLiteral("bob"), QStringLiteral("pw2"));
        QCOMPARE(client.logins.size(), 2);
    }

    void cancellingABrowserSignInCancelsItInTheBackend()
    {
        RecordingClient client;
        AuthManager auth(&client);
        auth.beginSsoLogin(QStringLiteral("matrix.org"), QString());
        QVERIFY(auth.browserLoginInProgress());
        auth.cancelLogin();
        QCOMPARE(client.ssoCancels, 1);
        QVERIFY(!auth.browserLoginInProgress());
        QVERIFY(!auth.isLoggingIn());
    }

    void aRefusedSignInPageEndsTheAttempt()
    {
        RecordingClient client;
        AuthManager auth(&client);
        QSignalSpy failed(&auth, &AuthManager::loginFailed);
        const QString hs = QStringLiteral("http://127.0.0.1:18008");
        auth.discoverAuthMethods(hs);
        client.announce(hs);
        QVERIFY(auth.serverOffersBrowserLogin());
        QVERIFY(auth.serverOffersPassword());
        QVERIFY(!auth.browserLoginUnavailable());

        auth.beginBrowserLogin(hs);
        QVERIFY(auth.browserLoginInProgress());
        client.refuseThePage(hs + QStringLiteral("/authorize"));
        QVERIFY(auth.loginStage() != QLatin1String("waiting_for_browser"));
        QVERIFY(auth.browserLoginUnavailable());
        QTRY_VERIFY(!auth.browserLoginInProgress());
        QCOMPARE(client.oauthCancels, 1);
        QVERIFY(!auth.isLoggingIn());
        QCOMPARE(auth.loginStage(), QStringLiteral("idle"));
        QCOMPARE(failed.count(), 1);
        QVERIFY2(auth.lastError().contains(QStringLiteral("password")),
                 qPrintable(auth.lastError()));
        QVERIFY(!auth.lastError().contains(QStringLiteral("cancelled")));

        const QString other = QStringLiteral("http://127.0.0.1:18009");
        auth.discoverAuthMethods(other);
        QVERIFY(!auth.browserLoginUnavailable());
        client.methodsOnly(other, false, true, false);
        auth.beginBrowserLogin(other);
        client.refuseThePage(other + QStringLiteral("/authorize"));
        QTRY_VERIFY(!auth.browserLoginInProgress());
        QVERIFY2(auth.lastError().contains(QStringLiteral("couldn't open")),
                 qPrintable(auth.lastError()));
        QVERIFY(!auth.lastError().contains(QStringLiteral("password")));

        QVERIFY(auth.browserLoginUnavailable());
        auth.discoverAuthMethods(other, true);
        QVERIFY(!auth.browserLoginUnavailable());
    }

    void aSignInMadeHereIsInteractiveAndARestoreIsNot()
    {
        MockMatrixClient client;
        AuthManager auth(&client);
        QSignalSpy signedIn(&auth, &AuthManager::loginSucceeded);
        auth.login(QStringLiteral("https://mock.local"), QStringLiteral("alice"),
                   QStringLiteral("x"));
        QVERIFY(signedIn.wait(3000));
        QVERIFY2(auth.lastSignInWasInteractive(),
                 "a password sign-in made here did not count as interactive");

        Q_EMIT client.loginSucceeded(QStringLiteral("@alice:mock.local"));
        QCOMPARE(signedIn.count(), 2);
        QVERIFY2(!auth.lastSignInWasInteractive(),
                 "a restored session counted as a sign-in made here");
    }
};

QTEST_MAIN(ServerAddressInputTest)
#include "ServerAddressInputTest.moc"
