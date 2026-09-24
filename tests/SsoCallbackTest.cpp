// Legacy Matrix SSO (m.login.sso) at the two boundaries on this side of the
// FFI: the loopback callback listener, and the login-flow policy in
// AuthManager.
//
// The listener is driven over real loopback sockets, since its socket
// handling (single-shot consumption, bounded reads, teardown) carries the
// security properties. The SDK half (get_sso_login_url(), login_token()) needs
// a homeserver and is covered by rust/src/sso.rs.

#include "auth/AuthManager.h"
#include "auth/OAuthCallbackServer.h"
#include "matrix/MockMatrixClient.h"

#include <QSignalSpy>
#include <QTcpSocket>
#include <QtTest>

namespace {

// A backend that reports SSO support and records what it was asked to do,
// without a homeserver anywhere in sight.
class SsoMock : public MockMatrixClient
{
    Q_OBJECT
public:
    bool supportsSsoLogin() const override { return ssoSupported; }
    bool supportsOAuthLogin() const override { return oauthSupported; }

    void requestSsoProviders(const QString &homeserver) override
    {
        providerRequests.append(homeserver);
    }
    void beginSsoLogin(const QString &homeserver, const QString &idpId) override
    {
        ssoStarts.append(qMakePair(homeserver, idpId));
    }
    void cancelSsoLogin() override { ++ssoCancels; }
    void cancelOAuthLogin() override { ++oauthCancels; }

    void announce(const QString &homeserver, bool password, bool oauth, bool sso)
    {
        Q_EMIT authMethodsDiscovered(homeserver, password, oauth, sso);
    }
    void announceProviders(const QString &homeserver, bool sso,
                           const QVariantList &providers)
    {
        Q_EMIT ssoProvidersReceived(homeserver, sso, providers);
    }

    bool ssoSupported = true;
    bool oauthSupported = true;
    QStringList providerRequests;
    QList<QPair<QString, QString>> ssoStarts;
    int ssoCancels = 0;
    int oauthCancels = 0;
};

QVariantMap provider(const QString &id, const QString &name,
                     const QString &icon = QString())
{
    QVariantMap m;
    m.insert(QStringLiteral("id"), id);
    m.insert(QStringLiteral("name"), name);
    m.insert(QStringLiteral("icon"), icon);
    return m;
}

// Send one raw HTTP request to the listener. Returns once the bytes are on
// their way; the CALLER waits for the outcome it cares about (see below).
bool deliver(quint16 port, const QString &target)
{
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    if (!socket.waitForConnected(3000))
        return false;
    const QByteArray request = "GET " + target.toUtf8()
        + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    socket.write(request);
    if (!socket.waitForBytesWritten(3000))
        return false;
    // Deliberately not waiting for the reply: the listener lives on this
    // thread, so blocking here stops it accepting. Pumping the loop here would
    // let the server answer before the caller arms its QSignalSpy. The bytes
    // are already written and the close is a graceful FIN, so the caller's
    // own wait drives the server.
    socket.close();
    return true;
}

quint16 portOf(const OAuthCallbackServer &server)
{
    // "http://127.0.0.1:PORT/callback/<nonce>"
    const QString uri = server.redirectUri();
    const int colon = uri.lastIndexOf(QLatin1Char(':'));
    const int slash = uri.indexOf(QLatin1Char('/'), colon);
    return uri.mid(colon + 1, slash - colon - 1).toUShort();
}

// The path this attempt actually advertised. It carries a per-attempt secret
// now, so a test may not hard-code "/callback": that is precisely the request
// the server must refuse.
QString pathOf(const OAuthCallbackServer &server)
{
    const QString uri = server.redirectUri();
    const int colon = uri.lastIndexOf(QLatin1Char(':'));
    const int slash = uri.indexOf(QLatin1Char('/'), colon);
    return slash < 0 ? QString() : uri.mid(slash);
}

} // namespace

class SsoCallbackTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // ── The listener, in SSO mode ────────────────────────────────────────
    // Login CSRF: m.login.sso has no `state`, so nothing binds the returned
    // `loginToken` to this attempt. The redirect URI carries 128 bits of
    // entropy in its path, and a token delivered to the bare path is refused.
    void aTokenDeliveredWithoutThisAttemptsSecretIsRefused()
    {
        OAuthCallbackServer server;
        server.setFlow(OAuthCallbackServer::Flow::Sso);
        QVERIFY(server.listen());
        // The advertised path is not guessable and is not the bare one.
        QVERIFY(pathOf(server).startsWith(QStringLiteral("/callback/")));
        QVERIFY(pathOf(server).size() > QStringLiteral("/callback/").size() + 16);

        QSignalSpy received(&server, &OAuthCallbackServer::callbackReceived);
        QSignalSpy failed(&server, &OAuthCallbackServer::callbackFailed);
        const quint16 port = portOf(server);

        // The bare path, and a wrong secret, are both strangers.
        QVERIFY(deliver(port, QStringLiteral("/callback?loginToken=attacker")));
        QVERIFY(deliver(port,
                        QStringLiteral("/callback/0000000000000000?loginToken=attacker")));
        QTest::qWait(200);
        QCOMPARE(received.count(), 0);
        QCOMPARE(failed.count(), 0);

        // And the single shot was NOT consumed: the real answer still works.
        QVERIFY(deliver(port, pathOf(server) + QStringLiteral("?loginToken=real")));
        QVERIFY(received.wait(3000));
        QCOMPARE(received.count(), 1);
        QCOMPARE(received.at(0).at(0).toString(), QStringLiteral("real"));
    }

    // Two attempts never share a secret, so a token meant for an abandoned
    // sign-in cannot be replayed into the next one.
    void twoAttemptsNeverAdvertiseTheSamePath()
    {
        OAuthCallbackServer first;
        first.setFlow(OAuthCallbackServer::Flow::Sso);
        QVERIFY(first.listen());
        const QString firstPath = pathOf(first);
        first.stop();
        // Cleared with the attempt: a stale secret must not outlive it.
        QVERIFY(pathOf(first).isEmpty());

        OAuthCallbackServer second;
        second.setFlow(OAuthCallbackServer::Flow::Sso);
        QVERIFY(second.listen());
        QVERIFY(!pathOf(second).isEmpty());
        QVERIFY(pathOf(second) != firstPath);
    }

    void aValidLoginTokenIsExtractedAndNothingElseIsForwarded()
    {
        OAuthCallbackServer server;
        server.setFlow(OAuthCallbackServer::Flow::Sso);
        QVERIFY(server.listen());
        QVERIFY(server.redirectUri().startsWith(QStringLiteral("http://127.0.0.1:")));

        QSignalSpy received(&server, &OAuthCallbackServer::callbackReceived);
        QVERIFY(deliver(portOf(server),
                        pathOf(server) + QStringLiteral("?loginToken=syt_abc123")));
        QVERIFY(received.wait(3000));
        QCOMPARE(received.count(), 1);
        // The TOKEN alone — not the URL. login_token() takes the token, and
        // forwarding the whole redirect would carry it further than it needs
        // to go.
        QCOMPARE(received.at(0).at(0).toString(), QStringLiteral("syt_abc123"));
        // Single-shot: the listener is down the moment it is consumed.
        QVERIFY(!server.isListening());
    }

    void aMissingOrEmptyTokenIsAFailureNotAnEmptySuccess()
    {
        // The SUFFIX is the fixture; the path itself must come from the
        // server built inside the loop, because each attempt advertises its
        // own secret path.
        for (const QString &suffix : { QString(),
                                       QStringLiteral("?loginToken="),
                                       QStringLiteral("?code=oauthish") }) {
            OAuthCallbackServer server;
            server.setFlow(OAuthCallbackServer::Flow::Sso);
            QVERIFY(server.listen());
            const QString target = pathOf(server) + suffix;
            QSignalSpy received(&server, &OAuthCallbackServer::callbackReceived);
            QSignalSpy failed(&server, &OAuthCallbackServer::callbackFailed);
            QVERIFY(deliver(portOf(server), target));
            QVERIFY2(failed.wait(3000), qPrintable(target));
            QCOMPARE(received.count(), 0);
            QCOMPARE(failed.at(0).at(0).toString(),
                     QStringLiteral("invalid_response"));
        }
    }

    void aDuplicateCallbackCannotBeDeliveredTwice()
    {
        OAuthCallbackServer server;
        server.setFlow(OAuthCallbackServer::Flow::Sso);
        QVERIFY(server.listen());
        const quint16 port = portOf(server);
        QSignalSpy received(&server, &OAuthCallbackServer::callbackReceived);

        QVERIFY(deliver(port, pathOf(server) + QStringLiteral("?loginToken=first")));
        QVERIFY(received.wait(3000));
        QCOMPARE(received.count(), 1);

        // The listener is gone, so a replay cannot even connect — and
        // crucially it does not deliver a second token.
        deliver(port, pathOf(server) + QStringLiteral("?loginToken=second"));
        QTest::qWait(200);
        QCOMPARE(received.count(), 1);
        QCOMPARE(received.at(0).at(0).toString(), QStringLiteral("first"));
    }

    void anUnrelatedRequestDoesNotConsumeTheSingleShot()
    {
        // A browser asking for /favicon.ico must not burn the one callback we
        // are allowed to receive.
        OAuthCallbackServer server;
        server.setFlow(OAuthCallbackServer::Flow::Sso);
        QVERIFY(server.listen());
        const quint16 port = portOf(server);
        QSignalSpy received(&server, &OAuthCallbackServer::callbackReceived);
        QSignalSpy failed(&server, &OAuthCallbackServer::callbackFailed);

        QVERIFY(deliver(port, QStringLiteral("/favicon.ico")));
        QTest::qWait(150);
        QCOMPARE(received.count(), 0);
        QCOMPARE(failed.count(), 0);
        QVERIFY(server.isListening());

        // ...and the real callback still works afterwards.
        QVERIFY(deliver(port, pathOf(server) + QStringLiteral("?loginToken=real")));
        QVERIFY(received.wait(3000));
        QCOMPARE(received.at(0).at(0).toString(), QStringLiteral("real"));
    }

    void aMalformedRequestIsRefusedRatherThanParsed()
    {
        OAuthCallbackServer server;
        server.setFlow(OAuthCallbackServer::Flow::Sso);
        QVERIFY(server.listen());
        const quint16 port = portOf(server);
        QSignalSpy received(&server, &OAuthCallbackServer::callbackReceived);

        // Garbage that is not an HTTP request line at all.
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, port);
        QVERIFY(socket.waitForConnected(3000));
        socket.write("not-http\r\n\r\n");
        socket.waitForBytesWritten(3000);
        QTest::qWait(200);
        socket.close();
        QCOMPARE(received.count(), 0);

        // An oversized request is dropped rather than buffered.
        QTcpSocket flood;
        flood.connectToHost(QHostAddress::LocalHost, port);
        QVERIFY(flood.waitForConnected(3000));
        flood.write("GET /callback?loginToken=" + QByteArray(64 * 1024, 'x'));
        flood.waitForBytesWritten(5000);
        QTest::qWait(300);
        flood.close();
        QCOMPARE(received.count(), 0);
    }

    // A POST carrying the right secret must still be refused: a cross-origin
    // form can POST to a loopback URL blind. The method is checked before the
    // path, proven here with a request correct in every other respect.
    void aPostToTheRealCallbackPathIsRefused()
    {
        OAuthCallbackServer server;
        server.setFlow(OAuthCallbackServer::Flow::Sso);
        QVERIFY(server.listen());
        const quint16 port = portOf(server);
        const QString path = pathOf(server);
        QSignalSpy received(&server, &OAuthCallbackServer::callbackReceived);
        QSignalSpy failed(&server, &OAuthCallbackServer::callbackFailed);

        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, port);
        QVERIFY(socket.waitForConnected(3000));
        const QByteArray request =
            "POST " + path.toUtf8() + "?loginToken=syt_stolen HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n\r\n";
        socket.write(request);
        socket.waitForBytesWritten(3000);
        QTest::qWait(300);
        socket.close();

        QCOMPARE(received.count(), 0);
        QCOMPARE(failed.count(), 0);

        // ...and it did not burn the single shot: the real callback arriving
        // afterwards still works.
        QVERIFY(deliver(port, path + QStringLiteral("?loginToken=syt_real")));
        QTRY_COMPARE_WITH_TIMEOUT(received.count(), 1, 3000);
    }

    // Loopback only: the listener speaks plain HTTP and carries a credential,
    // and QTcpServer binds every interface by default. Every other test here
    // connects to 127.0.0.1 and would pass either way.
    void theListenerBindsLoopbackOnlyAndAnEphemeralPort()
    {
        OAuthCallbackServer server;
        server.setFlow(OAuthCallbackServer::Flow::Sso);
        QVERIFY(server.listen());

        QCOMPARE(server.serverAddress(), QHostAddress(QHostAddress::LocalHost));
        QVERIFY(!server.serverAddress().isEqual(QHostAddress::Any));
        // Ephemeral: never a fixed, predictable port another process could
        // camp on before the sign-in starts.
        QVERIFY(portOf(server) != 0);

        // The advertised redirect URI must name loopback too — a URI that
        // pointed at a routable address would send the credential there.
        QVERIFY(server.redirectUri().startsWith(
            QStringLiteral("http://127.0.0.1:")));
    }

    void theWaitIsFiniteAndReportsATimeout()
    {
        OAuthCallbackServer server;
        server.setFlow(OAuthCallbackServer::Flow::Sso);
        server.setTimeout(std::chrono::milliseconds(150));
        QVERIFY(server.listen());
        QSignalSpy timedOut(&server, &OAuthCallbackServer::timedOut);
        QVERIFY(timedOut.wait(3000));
        // A timeout also releases the port; a sign-in nobody finished must not
        // hold a listener open for the rest of the session.
        QVERIFY(!server.isListening());
    }

    void cancellationReleasesTheListenerImmediately()
    {
        OAuthCallbackServer server;
        server.setFlow(OAuthCallbackServer::Flow::Sso);
        QVERIFY(server.listen());
        const quint16 port = portOf(server);
        QVERIFY(server.isListening());
        server.stop();
        QVERIFY(!server.isListening());
        // stop() is idempotent — cancel paths call it alongside teardown.
        server.stop();

        // And a callback arriving after cancellation reaches nobody.
        QSignalSpy received(&server, &OAuthCallbackServer::callbackReceived);
        // The port is dead, so the path does not matter here.
        deliver(port, QStringLiteral("/callback?loginToken=late"));
        QTest::qWait(200);
        QCOMPARE(received.count(), 0);
    }

    void aStaleCallbackFromAnEarlierAttemptCannotCompleteTheNewOne()
    {
        // A token from an abandoned attempt must not complete the next one:
        // each attempt owns its own listener, and the abandoned one is closed.
        OAuthCallbackServer first;
        first.setFlow(OAuthCallbackServer::Flow::Sso);
        QVERIFY(first.listen());
        const quint16 firstPort = portOf(first);
        first.stop();

        OAuthCallbackServer second;
        second.setFlow(OAuthCallbackServer::Flow::Sso);
        QVERIFY(second.listen());
        QSignalSpy secondReceived(&second, &OAuthCallbackServer::callbackReceived);

        // The old port is dead, so the stale callback goes nowhere...
        // Dead port; the path is irrelevant.
        deliver(firstPort, QStringLiteral("/callback?loginToken=stale"));
        QTest::qWait(200);
        QCOMPARE(secondReceived.count(), 0);
        // ...and the new attempt is still armed for its own.
        QVERIFY(deliver(portOf(second),
                        pathOf(second) + QStringLiteral("?loginToken=fresh")));
        QVERIFY(secondReceived.wait(3000));
        QCOMPARE(secondReceived.at(0).at(0).toString(), QStringLiteral("fresh"));
    }

    // ── OAuth must keep working independently ────────────────────────────
    void oauthCallbackParsingIsUnchangedByTheSsoFlow()
    {
        OAuthCallbackServer server;   // Flow::OAuth is the default.
        QCOMPARE(server.flow(), OAuthCallbackServer::Flow::OAuth);
        QVERIFY(server.listen());
        QSignalSpy received(&server, &OAuthCallbackServer::callbackReceived);
        QVERIFY(deliver(portOf(server),
                        pathOf(server) + QStringLiteral("?code=abc&state=xyz")));
        QVERIFY(received.wait(3000));
        // OAuth still forwards the WHOLE redirect URL: the SDK validates
        // `state` from it, so handing over the code alone would break it.
        const QString url = received.at(0).at(0).toString();
        QVERIFY(url.startsWith(QStringLiteral("http://127.0.0.1:")));
        QVERIFY(url.contains(QStringLiteral("code=abc")));
        QVERIFY(url.contains(QStringLiteral("state=xyz")));

        // And an OAuth callback carrying only a loginToken is NOT a valid
        // OAuth response.
        OAuthCallbackServer other;
        QVERIFY(other.listen());
        QSignalSpy failed(&other, &OAuthCallbackServer::callbackFailed);
        QVERIFY(deliver(portOf(other),
                        pathOf(other) + QStringLiteral("?loginToken=x")));
        QVERIFY(failed.wait(3000));
    }

    void anOAuthErrorResponseStillReportsItsProtocolCode()
    {
        OAuthCallbackServer server;
        QVERIFY(server.listen());
        QSignalSpy failed(&server, &OAuthCallbackServer::callbackFailed);
        QVERIFY(deliver(portOf(server),
                        pathOf(server) + QStringLiteral("?error=access_denied"
                                                        "&error_description=nope")));
        QVERIFY(failed.wait(3000));
        // The fixed protocol token is passed on; the server's free-text
        // description deliberately is not.
        QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("access_denied"));
    }

    // ── Discovery and login-flow policy ──────────────────────────────────
    void discoveryOffersSsoOnlyWhenTheServerAndTheBuildBothHaveIt()
    {
        SsoMock client;
        AuthManager auth(&client);
        QVERIFY(!auth.serverOffersSso());

        client.announce(QStringLiteral("https://hs.example"), false, false, true);
        QVERIFY(auth.serverOffersSso());
        QCOMPARE(auth.discoveryState(), QStringLiteral("done"));
        // Discovery asks for the providers itself.
        QCOMPARE(client.providerRequests, QStringList{ QStringLiteral("https://hs.example") });

        // A build that cannot do SSO must not offer it however loudly the
        // server advertises.
        SsoMock incapable;
        incapable.ssoSupported = false;
        AuthManager auth2(&incapable);
        incapable.announce(QStringLiteral("https://hs.example"), false, false, true);
        QVERIFY(!auth2.serverOffersSso());
        QVERIFY(incapable.providerRequests.isEmpty());
    }

    void providersArriveSeparatelyAndAreScopedToTheServerOnScreen()
    {
        SsoMock client;
        AuthManager auth(&client);
        client.announce(QStringLiteral("https://hs.example"), false, false, true);
        QVERIFY(auth.ssoProviders().isEmpty());

        // A late answer for a DIFFERENT homeserver — the user typed on — must
        // not populate the chooser.
        client.announceProviders(QStringLiteral("https://other.example"), true,
                                 { provider(QStringLiteral("x"), QStringLiteral("X")) });
        QVERIFY(auth.ssoProviders().isEmpty());

        client.announceProviders(QStringLiteral("https://hs.example"), true,
                                 { provider(QStringLiteral("oidc-a"), QStringLiteral("Alpha")),
                                   provider(QStringLiteral("oidc-b"), QStringLiteral("Beta")) });
        QCOMPARE(auth.ssoProviders().size(), 2);
        QCOMPARE(auth.ssoProviders().at(0).toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Alpha"));

        // Switching servers clears the previous one's providers rather than
        // leaving a chooser that belongs to a homeserver nobody is looking at.
        client.announce(QStringLiteral("https://fresh.example"), true, false, true);
        QVERIFY(auth.ssoProviders().isEmpty());
    }

    void startingSsoPassesTheChosenProviderAndResolvesOnCancel()
    {
        SsoMock client;
        AuthManager auth(&client);
        client.announce(QStringLiteral("https://hs.example"), false, false, true);

        auth.beginSsoLogin(QStringLiteral("https://hs.example"),
                           QStringLiteral("oidc-b"));
        QCOMPARE(client.ssoStarts.size(), 1);
        QCOMPARE(client.ssoStarts.first().first, QStringLiteral("https://hs.example"));
        QCOMPARE(client.ssoStarts.first().second, QStringLiteral("oidc-b"));
        // The UI must be able to show a waiting state and a way out.
        QVERIFY(auth.isLoggingIn());
        QVERIFY(auth.browserLoginInProgress());

        // Cancel reaches BOTH flows: only one can be live, each backend call
        // is a no-op for the other, and guessing wrong would strand the UI.
        auth.cancelBrowserLogin();
        QCOMPARE(client.ssoCancels, 1);
        QCOMPARE(client.oauthCancels, 1);
    }

    void aBuildWithoutSsoRefusesToStartOneAndSaysSo()
    {
        SsoMock client;
        client.ssoSupported = false;
        AuthManager auth(&client);
        QSignalSpy failed(&auth, &AuthManager::loginFailed);

        auth.beginSsoLogin(QStringLiteral("https://hs.example"), QString());
        QCOMPARE(client.ssoStarts.size(), 0);
        QCOMPARE(failed.count(), 1);
        QVERIFY(!auth.lastError().isEmpty());
        // Resolved, never stuck in a waiting state.
        QVERIFY(!auth.browserLoginInProgress());
    }
};

QTEST_MAIN(SsoCallbackTest)
#include "SsoCallbackTest.moc"
