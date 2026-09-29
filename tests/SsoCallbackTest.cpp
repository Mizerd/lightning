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

#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QtTest>

#include <memory>
#include <vector>

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
    void discoverAuthMethods(const QString &homeserver) override
    {
        discoveryRequests.append(homeserver);
    }
    // The backend ends a browser attempt with loginFailed: cancel, timeout,
    // a refused callback.
    void endAttemptWithFailure() { Q_EMIT loginFailed(QStringLiteral("Sign-in was cancelled.")); }

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
    QStringList discoveryRequests;
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
    // The listener's summary line is asserted below; a local qtlogging.ini
    // must not be able to switch it off.
    void initTestCase()
    {
        QLoggingCategory::setFilterRules(QStringLiteral("matrix.oauth.info=true"));
    }

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

    // Chromium-based browsers open a spare connection when a link to the
    // loopback is pressed and send the request on another one (measured on
    // 2026-09-29: Vivaldi 8.2 four runs of four, Chromium 154 two of three).
    // Synapse's SSO confirmation page is such a link. Serving only the first
    // connection reset the real callback, so the sign-in never arrived.
    void anIdleConnectionDoesNotHoldBackTheRealCallback()
    {
        OAuthCallbackServer server;
        server.setFlow(OAuthCallbackServer::Flow::Sso);
        QVERIFY(server.listen());
        const quint16 port = portOf(server);

        QTcpSocket spare;
        spare.connectToHost(QHostAddress::LocalHost, port);
        QVERIFY(spare.waitForConnected(3000));
        // Let the listener accept it first, as it would in the browser.
        QTRY_COMPARE_WITH_TIMEOUT(server.pendingConnectionCount(), 1, 3000);

        QSignalSpy received(&server, &OAuthCallbackServer::callbackReceived);
        QTest::ignoreMessage(QtInfoMsg,
                             QRegularExpression(QStringLiteral(
                                 "listener closed outcome= callback connections= 2 ")));
        QVERIFY(deliver(port, pathOf(server) + QStringLiteral("?loginToken=real")));
        QVERIFY(received.wait(3000));
        QCOMPARE(received.at(0).at(0).toString(), QStringLiteral("real"));
        // The spare connection is closed with the attempt, not left open.
        QTRY_COMPARE_WITH_TIMEOUT(spare.state(), QAbstractSocket::UnconnectedState, 3000);
    }

    // A connection that never sends a request is closed after a bounded wait
    // and does not consume the single shot.
    void aConnectionThatNeverSpeaksIsClosed()
    {
        OAuthCallbackServer server;
        server.setFlow(OAuthCallbackServer::Flow::Sso);
        server.setRequestDeadline(std::chrono::milliseconds(150));
        QVERIFY(server.listen());
        const quint16 port = portOf(server);

        QTcpSocket silent;
        silent.connectToHost(QHostAddress::LocalHost, port);
        QVERIFY(silent.waitForConnected(3000));
        QTRY_COMPARE_WITH_TIMEOUT(silent.state(), QAbstractSocket::UnconnectedState, 3000);
        QCOMPARE(server.pendingConnectionCount(), 0);
        QVERIFY(server.isListening());

        QSignalSpy received(&server, &OAuthCallbackServer::callbackReceived);
        QVERIFY(deliver(port, pathOf(server) + QStringLiteral("?loginToken=after")));
        QVERIFY(received.wait(3000));
    }

    // Many silent connections cannot grow without bound: past the cap a new
    // one is closed at once, and those already waiting are kept.
    void theNumberOfWaitingConnectionsIsBounded()
    {
        OAuthCallbackServer server;
        server.setFlow(OAuthCallbackServer::Flow::Sso);
        QVERIFY(server.listen());
        const quint16 port = portOf(server);

        std::vector<std::unique_ptr<QTcpSocket>> held;
        for (int i = 0; i < OAuthCallbackServer::kMaxPendingConnections; ++i) {
            auto socket = std::make_unique<QTcpSocket>();
            socket->connectToHost(QHostAddress::LocalHost, port);
            QVERIFY(socket->waitForConnected(3000));
            held.push_back(std::move(socket));
        }
        QTRY_COMPARE_WITH_TIMEOUT(server.pendingConnectionCount(),
                                  OAuthCallbackServer::kMaxPendingConnections, 3000);

        QTcpSocket extra;
        extra.connectToHost(QHostAddress::LocalHost, port);
        QVERIFY(extra.waitForConnected(3000));
        QTRY_COMPARE_WITH_TIMEOUT(extra.state(), QAbstractSocket::UnconnectedState, 3000);
        QCOMPARE(server.pendingConnectionCount(),
                 OAuthCallbackServer::kMaxPendingConnections);
        for (const auto &socket : held)
            QCOMPARE(socket->state(), QAbstractSocket::ConnectedState);
    }

    // The answer waits for the whole request head; a head that arrives in two
    // pieces is still one callback.
    void aRequestHeadSplitAcrossWritesIsStillReceived()
    {
        OAuthCallbackServer server;
        server.setFlow(OAuthCallbackServer::Flow::Sso);
        QVERIFY(server.listen());
        QSignalSpy received(&server, &OAuthCallbackServer::callbackReceived);

        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, portOf(server));
        QVERIFY(socket.waitForConnected(3000));
        socket.write("GET " + pathOf(server).toUtf8() + "?loginToken=split HTTP/1.1\r\n");
        QVERIFY(socket.waitForBytesWritten(3000));
        QTest::qWait(150);
        QCOMPARE(received.count(), 0);
        socket.write("Host: 127.0.0.1\r\n\r\n");
        QVERIFY(socket.waitForBytesWritten(3000));
        QVERIFY(received.wait(3000));
        QCOMPARE(received.at(0).at(0).toString(), QStringLiteral("split"));
    }

    // The page is shown before the SDK has redeemed anything, and redeeming
    // can still fail. It used to say "Signed in" over a failed sign-in.
    void theBrowserIsNotToldItIsSignedInBeforeLightningKnows()
    {
        OAuthCallbackServer server;
        QVERIFY(server.listen());
        QSignalSpy received(&server, &OAuthCallbackServer::callbackReceived);

        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, portOf(server));
        QVERIFY(socket.waitForConnected(3000));
        socket.write("GET " + pathOf(server).toUtf8()
                     + "?code=abc&state=xyz HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
        QVERIFY(socket.waitForBytesWritten(3000));
        QTRY_COMPARE_WITH_TIMEOUT(socket.state(), QAbstractSocket::UnconnectedState, 3000);
        QCOMPARE(received.count(), 1);
        const QByteArray page = socket.readAll();
        QVERIFY2(page.startsWith("HTTP/1.1 200 OK"), page.constData());
        QVERIFY2(!page.contains("Signed in"), page.constData());
        QVERIFY2(page.contains("Finishing sign-in"), page.constData());
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
        // Discovery asks for the providers itself, but not from inside the
        // discovery signal: the backend releases the answering handle right
        // after it, and a request started there died with it.
        QVERIFY(client.providerRequests.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(client.providerRequests,
                                  QStringList{ QStringLiteral("https://hs.example") }, 3000);

        // A build that cannot do SSO must not offer it however loudly the
        // server advertises.
        SsoMock incapable;
        incapable.ssoSupported = false;
        AuthManager auth2(&incapable);
        incapable.announce(QStringLiteral("https://hs.example"), false, false, true);
        QVERIFY(!auth2.serverOffersSso());
        QCoreApplication::processEvents();
        QVERIFY(incapable.providerRequests.isEmpty());
    }

    // The provider question is asked a turn later, so it must still be the
    // right question then: not for a server the user has typed away from, and
    // never once a browser sign-in holds the backend's bootstrap handle.
    void aLateProviderRequestIsDroppedWhenItNoLongerApplies()
    {
        SsoMock client;
        AuthManager auth(&client);
        client.announce(QStringLiteral("https://hs.example"), false, false, true);
        auth.beginSsoLogin(QStringLiteral("https://hs.example"), QString());
        QCoreApplication::processEvents();
        QVERIFY(client.providerRequests.isEmpty());

        SsoMock other;
        AuthManager auth2(&other);
        other.announce(QStringLiteral("https://first.example"), false, false, true);
        other.announce(QStringLiteral("https://second.example"), false, false, true);
        QTRY_COMPARE_WITH_TIMEOUT(other.providerRequests,
                                  QStringList{ QStringLiteral("https://second.example") }, 3000);
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

    // When the browser takes focus the homeserver field reports editing
    // finished and asks for discovery again. The backend refuses to probe
    // while a browser sign-in is running and never answers, so clearing the
    // offer then left the page with no browser buttons after the attempt
    // (measured live with a window manager on 2026-09-29).
    void aProbeDuringABrowserSignInDoesNotWithdrawTheBrowserButtons()
    {
        SsoMock client;
        AuthManager auth(&client);
        const QString hs = QStringLiteral("https://hs.example");
        client.announce(hs, true, true, true);
        QVERIFY(auth.serverOffersSso());
        QVERIFY(auth.serverOffersBrowserLogin());

        auth.beginSsoLogin(hs, QString());
        QVERIFY(auth.browserLoginInProgress());
        auth.discoverAuthMethods(hs);
        QVERIFY(auth.serverOffersSso());
        QVERIFY(auth.serverOffersBrowserLogin());
        QCOMPARE(auth.discoveryState(), QStringLiteral("done"));
        QVERIFY(client.discoveryRequests.isEmpty());

        // The attempt ends; the way to try again is still on screen, and the
        // same server is not asked again for nothing.
        client.endAttemptWithFailure();
        QVERIFY(!auth.browserLoginInProgress());
        QVERIFY(auth.serverOffersSso());
        QVERIFY(auth.serverOffersBrowserLogin());
        QCOMPARE(auth.discoveryState(), QStringLiteral("done"));
        QVERIFY(client.discoveryRequests.isEmpty());
    }

    // A different server typed during the attempt is probed once it ends.
    void aServerTypedDuringABrowserSignInIsProbedWhenItEnds()
    {
        SsoMock client;
        AuthManager auth(&client);
        const QString hs = QStringLiteral("https://hs.example");
        client.announce(hs, true, true, true);
        auth.beginBrowserLogin(hs);
        QVERIFY(auth.browserLoginInProgress());

        auth.discoverAuthMethods(QStringLiteral("https://other.example"));
        QVERIFY(client.discoveryRequests.isEmpty());

        client.endAttemptWithFailure();
        QCOMPARE(client.discoveryRequests,
                 QStringList{ QStringLiteral("https://other.example") });
        QCOMPARE(auth.discoveryState(), QStringLiteral("probing"));
        QCOMPARE(auth.discoveredHomeserver(), QStringLiteral("https://other.example"));
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
