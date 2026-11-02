
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
    socket.close();
    return true;
}

quint16 portOf(const OAuthCallbackServer &server)
{
    const QString uri = server.redirectUri();
    const int colon = uri.lastIndexOf(QLatin1Char(':'));
    const int slash = uri.indexOf(QLatin1Char('/'), colon);
    return uri.mid(colon + 1, slash - colon - 1).toUShort();
}

QString pathOf(const OAuthCallbackServer &server)
{
    const QString uri = server.redirectUri();
    const int colon = uri.lastIndexOf(QLatin1Char(':'));
    const int slash = uri.indexOf(QLatin1Char('/'), colon);
    return slash < 0 ? QString() : uri.mid(slash);
}

}

class SsoCallbackTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QLoggingCategory::setFilterRules(QStringLiteral("matrix.oauth.info=true"));
    }

    void aTokenDeliveredWithoutThisAttemptsSecretIsRefused()
    {
        OAuthCallbackServer server;
        server.setFlow(OAuthCallbackServer::Flow::Sso);
        QVERIFY(server.listen());
        QVERIFY(pathOf(server).startsWith(QStringLiteral("/callback/")));
        QVERIFY(pathOf(server).size() > QStringLiteral("/callback/").size() + 16);

        QSignalSpy received(&server, &OAuthCallbackServer::callbackReceived);
        QSignalSpy failed(&server, &OAuthCallbackServer::callbackFailed);
        const quint16 port = portOf(server);

        QVERIFY(deliver(port, QStringLiteral("/callback?loginToken=attacker")));
        QVERIFY(deliver(port,
                        QStringLiteral("/callback/0000000000000000?loginToken=attacker")));
        QTest::qWait(200);
        QCOMPARE(received.count(), 0);
        QCOMPARE(failed.count(), 0);

        QVERIFY(deliver(port, pathOf(server) + QStringLiteral("?loginToken=real")));
        QVERIFY(received.wait(3000));
        QCOMPARE(received.count(), 1);
        QCOMPARE(received.at(0).at(0).toString(), QStringLiteral("real"));
    }

    void twoAttemptsNeverAdvertiseTheSamePath()
    {
        OAuthCallbackServer first;
        first.setFlow(OAuthCallbackServer::Flow::Sso);
        QVERIFY(first.listen());
        const QString firstPath = pathOf(first);
        first.stop();
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
        QCOMPARE(received.at(0).at(0).toString(), QStringLiteral("syt_abc123"));
        QVERIFY(!server.isListening());
    }

    void aMissingOrEmptyTokenIsAFailureNotAnEmptySuccess()
    {
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

        deliver(port, pathOf(server) + QStringLiteral("?loginToken=second"));
        QTest::qWait(200);
        QCOMPARE(received.count(), 1);
        QCOMPARE(received.at(0).at(0).toString(), QStringLiteral("first"));
    }

    void anUnrelatedRequestDoesNotConsumeTheSingleShot()
    {
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

        QVERIFY(deliver(port, pathOf(server) + QStringLiteral("?loginToken=real")));
        QVERIFY(received.wait(3000));
        QCOMPARE(received.at(0).at(0).toString(), QStringLiteral("real"));
    }

    void anIdleConnectionDoesNotHoldBackTheRealCallback()
    {
        OAuthCallbackServer server;
        server.setFlow(OAuthCallbackServer::Flow::Sso);
        QVERIFY(server.listen());
        const quint16 port = portOf(server);

        QTcpSocket spare;
        spare.connectToHost(QHostAddress::LocalHost, port);
        QVERIFY(spare.waitForConnected(3000));
        QTRY_COMPARE_WITH_TIMEOUT(server.pendingConnectionCount(), 1, 3000);

        QSignalSpy received(&server, &OAuthCallbackServer::callbackReceived);
        QTest::ignoreMessage(QtInfoMsg,
                             QRegularExpression(QStringLiteral(
                                 "listener closed outcome= callback connections= 2 ")));
        QVERIFY(deliver(port, pathOf(server) + QStringLiteral("?loginToken=real")));
        QVERIFY(received.wait(3000));
        QCOMPARE(received.at(0).at(0).toString(), QStringLiteral("real"));
        QTRY_COMPARE_WITH_TIMEOUT(spare.state(), QAbstractSocket::UnconnectedState, 3000);
    }

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

        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, port);
        QVERIFY(socket.waitForConnected(3000));
        socket.write("not-http\r\n\r\n");
        socket.waitForBytesWritten(3000);
        QTest::qWait(200);
        socket.close();
        QCOMPARE(received.count(), 0);

        QTcpSocket flood;
        flood.connectToHost(QHostAddress::LocalHost, port);
        QVERIFY(flood.waitForConnected(3000));
        flood.write("GET /callback?loginToken=" + QByteArray(64 * 1024, 'x'));
        flood.waitForBytesWritten(5000);
        QTest::qWait(300);
        flood.close();
        QCOMPARE(received.count(), 0);
    }

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

        QVERIFY(deliver(port, path + QStringLiteral("?loginToken=syt_real")));
        QTRY_COMPARE_WITH_TIMEOUT(received.count(), 1, 3000);
    }

    void theListenerBindsLoopbackOnlyAndAnEphemeralPort()
    {
        OAuthCallbackServer server;
        server.setFlow(OAuthCallbackServer::Flow::Sso);
        QVERIFY(server.listen());

        QCOMPARE(server.serverAddress(), QHostAddress(QHostAddress::LocalHost));
        QVERIFY(!server.serverAddress().isEqual(QHostAddress::Any));
        QVERIFY(portOf(server) != 0);

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
        server.stop();

        QSignalSpy received(&server, &OAuthCallbackServer::callbackReceived);
        deliver(port, QStringLiteral("/callback?loginToken=late"));
        QTest::qWait(200);
        QCOMPARE(received.count(), 0);
    }

    void aStaleCallbackFromAnEarlierAttemptCannotCompleteTheNewOne()
    {
        OAuthCallbackServer first;
        first.setFlow(OAuthCallbackServer::Flow::Sso);
        QVERIFY(first.listen());
        const quint16 firstPort = portOf(first);
        first.stop();

        OAuthCallbackServer second;
        second.setFlow(OAuthCallbackServer::Flow::Sso);
        QVERIFY(second.listen());
        QSignalSpy secondReceived(&second, &OAuthCallbackServer::callbackReceived);

        deliver(firstPort, QStringLiteral("/callback?loginToken=stale"));
        QTest::qWait(200);
        QCOMPARE(secondReceived.count(), 0);
        QVERIFY(deliver(portOf(second),
                        pathOf(second) + QStringLiteral("?loginToken=fresh")));
        QVERIFY(secondReceived.wait(3000));
        QCOMPARE(secondReceived.at(0).at(0).toString(), QStringLiteral("fresh"));
    }

    void oauthCallbackParsingIsUnchangedByTheSsoFlow()
    {
        OAuthCallbackServer server;
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
        QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("access_denied"));
    }

    void discoveryOffersSsoOnlyWhenTheServerAndTheBuildBothHaveIt()
    {
        SsoMock client;
        AuthManager auth(&client);
        QVERIFY(!auth.serverOffersSso());

        client.announce(QStringLiteral("https://hs.example"), false, false, true);
        QVERIFY(auth.serverOffersSso());
        QCOMPARE(auth.discoveryState(), QStringLiteral("done"));
        QVERIFY(client.providerRequests.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(client.providerRequests,
                                  QStringList{ QStringLiteral("https://hs.example") }, 3000);

        SsoMock incapable;
        incapable.ssoSupported = false;
        AuthManager auth2(&incapable);
        incapable.announce(QStringLiteral("https://hs.example"), false, false, true);
        QVERIFY(!auth2.serverOffersSso());
        QCoreApplication::processEvents();
        QVERIFY(incapable.providerRequests.isEmpty());
    }

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

        client.announceProviders(QStringLiteral("https://other.example"), true,
                                 { provider(QStringLiteral("x"), QStringLiteral("X")) });
        QVERIFY(auth.ssoProviders().isEmpty());

        client.announceProviders(QStringLiteral("https://hs.example"), true,
                                 { provider(QStringLiteral("oidc-a"), QStringLiteral("Alpha")),
                                   provider(QStringLiteral("oidc-b"), QStringLiteral("Beta")) });
        QCOMPARE(auth.ssoProviders().size(), 2);
        QCOMPARE(auth.ssoProviders().at(0).toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Alpha"));

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
        QVERIFY(auth.isLoggingIn());
        QVERIFY(auth.browserLoginInProgress());

        auth.cancelBrowserLogin();
        QCOMPARE(client.ssoCancels, 1);
        QCOMPARE(client.oauthCancels, 1);
    }

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

        client.endAttemptWithFailure();
        QVERIFY(!auth.browserLoginInProgress());
        QVERIFY(auth.serverOffersSso());
        QVERIFY(auth.serverOffersBrowserLogin());
        QCOMPARE(auth.discoveryState(), QStringLiteral("done"));
        QVERIFY(client.discoveryRequests.isEmpty());
    }

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
        QVERIFY(!auth.browserLoginInProgress());
    }
};

QTEST_MAIN(SsoCallbackTest)
#include "SsoCallbackTest.moc"
