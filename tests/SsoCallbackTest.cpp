// Legacy Matrix SSO (m.login.sso), at the two boundaries that can actually go
// wrong on this side of the FFI: the loopback callback listener, and the
// login-flow policy in AuthManager.
//
// The listener is driven over REAL loopback sockets rather than by calling its
// parser directly — the socket handling (single-shot consumption, bounded
// reads, teardown) is the part with the security properties, and a test that
// bypasses it would prove nothing about them.
//
// The SDK half — get_sso_login_url() and login_token() — is not reachable
// here: it needs a homeserver. Those are covered by rust/src/sso.rs's own
// tests and, ultimately, by a live server. Nothing in this file pretends
// otherwise.

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
    // Deliberately NOT waiting for the reply here.
    //
    // This used to end in socket.waitForReadyRead(3000), which could never
    // make progress: the listener lives on this same thread, so blocking the
    // caller is exactly what stops the server accepting the connection. Every
    // delivery therefore burned the whole 3 s bound and the request was only
    // handled once the CALLER started spinning the event loop — about 33 s of
    // this suite's 34 s spent waiting for something that could not happen
    // until the wait gave up.
    //
    // Pumping the loop here instead is worse than useless: it lets the server
    // answer BEFORE the caller arms its QSignalSpy, and QSignalSpy::wait()
    // waits for a NEW signal, so seven cases started failing. The bytes are
    // already in the kernel buffer (waitForBytesWritten above) and the close
    // is a graceful FIN, so the server still reads the full request when it
    // accepts — the caller's own wait is what drives that, which is where the
    // waiting belongs.
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
    // LOGIN CSRF. The legacy m.login.sso flow has no `state` of its own: the
    // homeserver echoes back only `loginToken`, so nothing in the protocol
    // binds the answer to the attempt that asked for it. This endpoint used
    // to accept any token that landed on a FIXED `/callback` for the whole
    // five-minute window, which meant any other local process — and,
    // depending on the browser's private-network rules, a web page sweeping
    // the ephemeral port range — could sign the user into the ATTACKER'S
    // account. Everything typed afterwards would go to an identity someone
    // else controls.
    //
    // The redirect URI now carries 128 bits of system entropy in its path.
    // On the unfixed server this case fails: the bare path was the callback.
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

    // A POST CARRYING THE RIGHT SECRET MUST STILL BE REFUSED.
    //
    // The nonce lives in the PATH, and a path is not a secret from a page
    // that can guess or observe it — but more to the point, a cross-origin
    // form can POST to a loopback URL without reading anything back. If the
    // listener took the method for granted, that is a login-CSRF route which
    // the nonce does not close on its own: the browser would happily deliver
    // an attacker's loginToken to the exact advertised URL.
    //
    // So the method is checked BEFORE the path, and this proves the check is
    // load-bearing by using a request that is correct in every other respect.
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

        // ...and it did not burn the single shot either: the real callback,
        // arriving afterwards on the same listener, still works. A refusal
        // that disarmed the attempt would be a denial of service on every
        // sign-in a stray request touched.
        QVERIFY(deliver(port, path + QStringLiteral("?loginToken=syt_real")));
        QTRY_COMPARE_WITH_TIMEOUT(received.count(), 1, 3000);
    }

    // LOOPBACK ONLY. The listener speaks plain HTTP and carries a credential,
    // so it must never be reachable from off the machine. QTcpServer binds
    // every interface by default, which is exactly the mistake to guard
    // against — and it is invisible in every other test here, because they
    // all connect to 127.0.0.1 and would pass either way.
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
        // Attempt 1 starts, then is abandoned. Attempt 2 starts on its own
        // port. The token from attempt 1 must not complete attempt 2 — which
        // is guaranteed structurally: each attempt owns its own listener, and
        // the abandoned one is closed.
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
