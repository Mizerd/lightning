// The homeserver field accepts a server however it is typed: with or without
// https://, any case, spaces, a trailing slash, a pasted API path or a pasted
// Matrix ID. AuthManager turns it into one form for every sign-in path, and
// that form must stay a server NAME for matrix-sdk's discovery, never a
// guessed client API URL (https://matrix.org serves its API elsewhere).
//
// Before this, "matrix.org" was probed fine but refused by the password path
// ("not a full URL"), and a browser sign-in with it completed in the browser
// and was then refused at the end, when the session was saved.

#include "auth/AuthManager.h"
#include "matrix/MockMatrixClient.h"
#include "storage/AppDataPaths.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {

// Records what each sign-in path hands the backend, and never signs in.
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
    }
    void beginOAuthSignUp(const QString &homeserver) override
    {
        signUps.append(homeserver);
    }
    void beginSsoLogin(const QString &homeserver, const QString &idpId) override
    {
        ssoStarts.append(qMakePair(homeserver, idpId));
    }
    void requestSsoProviders(const QString &homeserver) override
    {
        providerRequests.append(homeserver);
    }
    void cancelOAuthLogin() override {}
    void cancelSsoLogin() override {}

    void announce(const QString &homeserver)
    {
        Q_EMIT authMethodsDiscovered(homeserver, true, true, true);
    }
    // As RustSdkMatrixClient does: the details, then the methods.
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
    void endAttempt() { Q_EMIT loginFailed(QStringLiteral("Sign-in was cancelled.")); }

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

} // namespace

class ServerAddressInputTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dataHome;

private slots:
    void initTestCase()
    {
        // resolveAccountIdentity() derives store paths; keep them here.
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
        // No scheme means https, even for a local name: http is never guessed.
        QTest::newRow("matrix id with port")
            << QStringLiteral("@alice:localhost:8008")
            << QStringLiteral("https://localhost:8008");
        QTest::newRow("bare with port")
            << QStringLiteral("localhost:8448")
            << QStringLiteral("https://localhost:8448");
        // A typed http:// is kept exactly, never upgraded.
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
        // A homeserver can live under a path; only API paths are cut.
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
        // Stable: normalising the answer changes nothing.
        QCOMPARE(AuthManager::normalizedServerAddress(out), out);
        // The account record accepts it as a browser sign-in's final step
        // passes it, with the server's full user ID. That step refused a bare
        // name after the browser had already signed the user in.
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
        // A password typed into this field by mistake is not looked up.
        QTest::newRow("password with ?") << QStringLiteral("Summer2026?");
        QTest::newRow("password with #") << QStringLiteral("Pass#1word");
        QTest::newRow("password with /") << QStringLiteral("Winter/2026");
        QTest::newRow("password like an id") << QStringLiteral("@lice:Secret99");
        QTest::newRow("fullwidth") << QStringLiteral("\uFF30assword");
        QTest::newRow("not an api path") << QStringLiteral("example.com/_matrixish");
        QTest::newRow("empty port") << QStringLiteral("matrix.org:");
        // A second scheme would make the host "https".
        QTest::newRow("doubled scheme") << QStringLiteral("https://https://matrix.org");
        QTest::newRow("doubled, mixed") << QStringLiteral("http://https://matrix.org");
        QTest::newRow("doubled, one slash") << QStringLiteral("https://https:/matrix.org");
        QTest::newRow("doubled, no colon") << QStringLiteral("https://http//matrix.org");
        // Checked on the path "." and ".." resolve to.
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

    // Nothing is asked for such text, so the backend's probe of the previous
    // server is still running; its answer must not bring that server's
    // sign-in buttons back.
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

        // A cleared field, asked twice (debounce, then editing finished).
        auth.discoverAuthMethods(QStringLiteral("example.org"));
        auth.discoverAuthMethods(QString());
        auth.discoverAuthMethods(QString());
        client.announce(QStringLiteral("https://example.org"));
        QCOMPARE(auth.discoveryState(), QStringLiteral("idle"));
        QVERIFY(!auth.serverOffersBrowserLogin());

        // Only that one answer is dropped: the next server's is shown.
        auth.discoverAuthMethods(QStringLiteral("example.org"));
        client.announce(QStringLiteral("https://example.org"));
        QCOMPARE(auth.discoveryState(), QStringLiteral("done"));
        QVERIFY(auth.serverOffersBrowserLogin());
    }

    // The server text changes form; the user name and password go through
    // untouched and in their own places. A pasted Matrix ID gives only its
    // server, and never fills the user.
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
        // Resolved, never left waiting.
        QVERIFY(!auth.isLoggingIn());
        QVERIFY(!auth.browserLoginInProgress());
    }

    // Empty text still reaches the backend, whose own message asks for a
    // homeserver.
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

        // An explicit http:// stays http.
        auth.beginSsoLogin(QStringLiteral("HTTP://LOCALHOST:8008/"), QStringLiteral("idp"));
        QCOMPARE(client.ssoStarts.size(), 1);
        QCOMPARE(client.ssoStarts.at(0).first, QStringLiteral("http://localhost:8008"));
        QCOMPARE(client.ssoStarts.at(0).second, QStringLiteral("idp"));
    }

    // A probe asked for during a browser sign-in runs when it ends, unless it
    // is the server already on screen, however it was spelled.
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

        // A different server is still probed.
        auth.beginSsoLogin(QStringLiteral("matrix.org"), QString());
        auth.discoverAuthMethods(QStringLiteral("example.org"));
        client.endAttempt();
        QCOMPARE(client.discoveries, QStringList{ QStringLiteral("https://example.org") });
    }

    // What the server's answer decides beyond its methods: a server with its
    // own sign-in page that can create accounts and has an account page.
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

        // Such a server is signed in to on its page, as Element does, so its
        // compatibility single sign-on is not asked about.
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
        // No prompt=create, and an account page that is not https.
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

        // prompt=create means nothing without the server's own page.
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

        // Details for another server decide nothing about this one; without
        // them a server offering nothing reads as unreachable.
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

    // The warning follows the RESOLVED base URL, not the typed scheme: a
    // server name's well-known may point at plain http, and a typed http://
    // may redirect to https.
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

    // Asking about the server already answered (or being asked) changes
    // nothing: the choices do not blink when the field loses focus.
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

        // A failed one is asked again ("Try again").
        auth.discoverAuthMethods(QStringLiteral("example.org"));
        client.answer(QStringLiteral("https://example.org"), false, false, false,
                      details(false));
        auth.discoverAuthMethods(QStringLiteral("example.org"));
        QCOMPARE(client.discoveries.size(), 3);
    }

    // What the account record at the end of a browser sign-in would refuse is
    // refused before the browser opens, never after the user signed in there
    // and the server issued a device (sk.community typed without https://,
    // reported 2026-09-29). Here: no data folder to keep an account in.
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

        // With somewhere to keep it, the same sign-in goes ahead.
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

    // A backend without browser sign-in has nothing to ask: the password form
    // shows at once, as it always did there.
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

    // A browser button names the discovered server ("Continue with
    // matrix.org"), so it must sign in there, not wherever the field's text
    // has moved on to since.
    void theBrowserButtonsActOnTheServerTheyName()
    {
        QFile file(QStringLiteral(QML_DIR "/LoginScreen.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString qml = QString::fromUtf8(file.readAll());
        QCOMPARE(qml.count(QStringLiteral("beginBrowserLogin(app.auth.discoveredHomeserver)")), 1);
        QCOMPARE(qml.count(QStringLiteral("beginSsoLogin(app.auth.discoveredHomeserver")), 2);
        QVERIFY(!qml.contains(QStringLiteral("beginBrowserLogin(homeserverField")));
        QVERIFY(!qml.contains(QStringLiteral("beginSsoLogin(homeserverField")));
        // A click does not end an edit of the field, so for the debounce's
        // 700 ms the buttons would still belong to the last server.
        // Continue with <server>, the unnamed and the named single sign-on
        // buttons, and Create account.
        QCOMPARE(qml.count(QStringLiteral(
                     "enabled: !app.auth.isLoggingIn && !discoverDebounce.running")), 4);
    }

    // The one-time "index all messages now?" offer is made after a sign-in the
    // user made here, never after a restore, and AuthManager is where the two
    // are told apart. Old code had no such flag; a flag that simply read
    // isLoggedIn() (or was never cleared) fails the restore half.
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

        // A restore (launch, account switch) reaches the client directly.
        Q_EMIT client.loginSucceeded(QStringLiteral("@alice:mock.local"));
        QCOMPARE(signedIn.count(), 2);
        QVERIFY2(!auth.lastSignInWasInteractive(),
                 "a restored session counted as a sign-in made here");
    }
};

QTEST_MAIN(ServerAddressInputTest)
#include "ServerAddressInputTest.moc"
