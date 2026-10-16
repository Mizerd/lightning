// Browser sign-in edge cases from docs/open-items.md ("Browser sign-in"):
// the loopback listener's per-connection deadline, and the bootstrap handle
// that drainAuthEvents() released AFTER emitting, which made it unsafe for a
// slot to start the next request from inside the signal.

#include "auth/OAuthCallbackServer.h"
#include "matrix/RustSdkMatrixClient.h"
#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AuthManager.h"
#include "storage/AppDataPaths.h"
#include "storage/InMemorySecretStore.h"

#include <QDir>
#include <QHash>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtTest/QtTest>

#include <chrono>

namespace {
// Answers discovery and nothing else: versions, password flows, and
// M_UNRECOGNIZED for every other path (no OAuth metadata, no well-known).
class DiscoveryHomeserver : public QObject
{
    Q_OBJECT
public:
    // Offer SSO with one named provider, answering /login after a delay.
    bool offerSso = false;
    int loginDelayMs = 0;
    int logouts = 0;

    quint16 listen()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this,
                        [this, socket] { serve(socket); });
                connect(socket, &QTcpSocket::disconnected, socket,
                        &QObject::deleteLater);
            }
        });
        return m_server.listen(QHostAddress::LocalHost, 0) ? m_server.serverPort() : 0;
    }

private:
    void reply(QTcpSocket *socket, int code, const QByteArray &body)
    {
        socket->write("HTTP/1.1 " + QByteArray::number(code) + " X\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: " + QByteArray::number(body.size())
                      + "\r\n\r\n" + body);
    }

    void serve(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        for (;;) {
            const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
            if (headerEnd < 0)
                return;
            qsizetype length = 0;
            const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
            for (const QByteArray &raw : lines) {
                const QByteArray line = raw.trimmed().toLower();
                if (line.startsWith("content-length:"))
                    length = line.mid(15).trimmed().toLongLong();
            }
            if (buffer.size() < headerEnd + 4 + length)
                return;
            const QByteArray path =
                lines.value(0).trimmed().split(' ').value(1).split('?').value(0);
            buffer.remove(0, headerEnd + 4 + length);
            if (path == "/_matrix/client/versions") {
                reply(socket, 200, R"({"versions":["v1.1","v1.11"],"unstable_features":{}})");
            } else if (path == "/_matrix/client/v3/login" && offerSso) {
                // Slow enough for a sign-in to start before the answer.
                QTimer::singleShot(loginDelayMs, socket, [this, socket] {
                    reply(socket, 200, R"({"flows":[{"type":"m.login.password"},)"
                                       R"({"type":"m.login.sso","identity_providers":)"
                                       R"([{"id":"oidc-example","name":"Example"}]}]})");
                });
            } else if (path == "/_matrix/client/v3/login") {
                reply(socket, 200, R"({"flows":[{"type":"m.login.password"}]})");
            } else if (path == "/_matrix/client/v3/logout") {
                ++logouts;
                reply(socket, 200, "{}");
            } else {
                reply(socket, 404, R"({"errcode":"M_UNRECOGNIZED","error":"not here"})");
            }
        }
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};
} // namespace

class BrowserSignInEdgeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_home.isValid());
        qputenv("XDG_CONFIG_HOME", (m_home.path() + QStringLiteral("/config")).toUtf8());
        qputenv("XDG_DATA_HOME", (m_home.path() + QStringLiteral("/data")).toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("browser-sign-in-edge-test"));
    }

    // A connection's deadline lives as long as its socket, and deleteLater()
    // keeps the socket until the next event-loop pass. stop() dropped the
    // socket without stopping its deadline, so a deadline already due fired
    // afterwards and counted that connection as a silent close of the NEXT
    // attempt, whose counters listen() had just reset.
    void aStoppedAttemptsDeadlineNeverCountsInTheNextOne()
    {
        OAuthCallbackServer server;
        server.setRequestDeadline(std::chrono::milliseconds(1));
        QVERIFY(server.listen());
        QTcpSocket silent;
        silent.connectToHost(server.serverAddress(), QUrl(server.redirectUri()).port());
        QTRY_COMPARE_WITH_TIMEOUT(server.pendingConnectionCount(), 1, 5000);
        // The deadline is overdue, and nothing has run it yet.
        QThread::msleep(30);
        server.stop();
        QVERIFY(server.listen());
        // Timers run; deferred deletes do not (processEvents never runs them),
        // which is the window between stop() and the socket really going.
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        QTest::ignoreMessage(
            QtInfoMsg,
            QRegularExpression(QStringLiteral(
                "sign-in callback listener closed outcome= stopped connections= 0 "
                "not-the-callback= 0 refused= 0 closed-silent= 0")));
        server.stop();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    // A slot answering authMethodsDiscovered by starting the next discovery
    // (the user changed the server field) lost that discovery: the release
    // after the emit destroyed the handle the slot had just created, so the
    // answer never came and the sign-in screen waited on it for ever.
    void aDiscoveryStartedFromTheAnswerToTheLastOneIsAnswered()
    {
        DiscoveryHomeserver homeserver;
        const quint16 port = homeserver.listen();
        QVERIFY(port != 0);
        const QString hs = QStringLiteral("http://127.0.0.1:%1").arg(port);

        // Never the host keyring.
        InMemorySecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        RustSdkMatrixClient client(&settings);
        QSignalSpy discovered(&client, &MatrixClient::authMethodsDiscovered);
        bool askedAgain = false;
        connect(&client, &MatrixClient::authMethodsDiscovered, this,
                [&](const QString &) {
                    if (askedAgain)
                        return;
                    askedAgain = true;
                    client.discoverAuthMethods(hs);
                });
        client.discoverAuthMethods(hs);
        QTRY_VERIFY_WITH_TIMEOUT(discovered.size() >= 1, 30000);
        QVERIFY(askedAgain);
        QTRY_VERIFY2_WITH_TIMEOUT(discovered.size() >= 2, "the second discovery was never answered",
                                  30000);
        // The server is reachable and offers password sign-in both times.
        for (const QList<QVariant> &answer : std::as_const(discovered))
            QVERIFY(answer.at(1).toBool());
    }

    // The named SSO providers were asked on the bootstrap handle a browser
    // sign-in also takes, so a sign-in started inside that round trip
    // replaced the handle and the answer never came: the login screen kept
    // only the generic button. They have a handle of their own now.
    void theNamedSsoProvidersSurviveASignInStartedBeforeTheyArrive()
    {
        DiscoveryHomeserver homeserver;
        homeserver.offerSso = true;
        homeserver.loginDelayMs = 1500;
        const quint16 port = homeserver.listen();
        QVERIFY(port != 0);
        // Plain http on loopback: the sign-in below is refused before any
        // browser could be opened (only https sign-in pages are handed on).
        const QString hs = QStringLiteral("http://127.0.0.1:%1").arg(port);

        InMemorySecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        RustSdkMatrixClient client(&settings);
        QSignalSpy providers(&client, &MatrixClient::ssoProvidersReceived);
        client.requestSsoProviders(hs);
        client.beginSsoLogin(hs, QString());
        QTRY_VERIFY2_WITH_TIMEOUT(!providers.isEmpty(),
                                  "the providers' answer was lost to the sign-in", 30000);
        QVERIFY(providers.first().at(1).toBool());
        const QVariantList named = providers.first().at(2).toList();
        QCOMPARE(named.size(), 1);
        QCOMPARE(named.first().toMap().value(QStringLiteral("id")).toString(),
                 QStringLiteral("oidc-example"));
        client.cancelSsoLogin();
    }

    // A browser sign-in the server completed and Phase B then refused here
    // (an account already on this device as another device) left that
    // session live at the server, a device nobody holds. It is ended now,
    // from the Phase A handle kept for the purpose. SSO, since an OAuth
    // revocation needs an https endpoint this loopback fixture cannot offer
    // (rust/src/oauth.rs covers that flow up to the request).
    void aRefusedBrowserSignInIsEndedAtTheServer()
    {
        DiscoveryHomeserver homeserver;
        const quint16 port = homeserver.listen();
        QVERIFY(port != 0);
        const QString hs = QStringLiteral("http://127.0.0.1:%1").arg(port);

        InMemorySecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        const QString kai = QStringLiteral("@kai:example.org");
        settings.saveSession(hs, kai, QStringLiteral("OLDDEVICE"),
                             QStringLiteral("old-access"));
        matrix::app_data::AccountIdentity identity;
        QVERIFY(matrix::app_data::resolveAccountIdentity(hs, kai, &identity));
        QVERIFY(QDir().mkpath(identity.rustStorePath));

        RustSdkMatrixClient client(&settings);
        QVERIFY(client.ensureOAuthBootstrapHandle());
        client.m_ssoHomeserver = hs;
        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        client.completeSsoLogin(kai, QStringLiteral("NEWDEVICE"),
                                QStringLiteral("new-access"), QString());
        QVERIFY(!failed.isEmpty());
        // Refused, and nothing of the refused sign-in was kept.
        QCOMPARE(settings.accountRecord(kai).value(QStringLiteral("deviceId")).toString(),
                 QStringLiteral("OLDDEVICE"));
        QTRY_COMPARE_WITH_TIMEOUT(homeserver.logouts, 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!client.m_revokeHandle, 10000);
    }

    // The browser could not be handed the server's sign-in page (plain http:
    // only https pages are ever handed on), and the attempt waited for its
    // fifteen-minute timeout under "Waiting for your browser..." beside the
    // error, because the backend reports the page right after refusing it.
    // Against the real backend: the attempt ends, nothing waits for the
    // browser, and one reason is shown. No browser is started: the refusal
    // comes before any launch.
    void aRefusedSignInPageEndsTheAttemptInsteadOfWaiting()
    {
        DiscoveryHomeserver homeserver;
        homeserver.offerSso = true;
        const quint16 port = homeserver.listen();
        QVERIFY(port != 0);
        const QString hs = QStringLiteral("http://127.0.0.1:%1").arg(port);

        InMemorySecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        RustSdkMatrixClient client(&settings);
        AuthManager auth(&client);
        QSignalSpy refused(&client, &MatrixClient::browserLaunchFailed);
        QSignalSpy failed(&auth, &AuthManager::loginFailed);
        QStringList stages;
        connect(&auth, &AuthManager::loginStageChanged, this,
                [&] { stages.append(auth.loginStage()); });

        auth.beginSsoLogin(hs, QString());
        QVERIFY(auth.browserLoginInProgress());
        QTRY_VERIFY_WITH_TIMEOUT(!refused.isEmpty(), 30000);
        QTRY_VERIFY2_WITH_TIMEOUT(!auth.browserLoginInProgress(),
                                  "the refused attempt was left waiting", 5000);
        QVERIFY(!auth.isLoggingIn());
        QVERIFY2(!stages.contains(QStringLiteral("waiting_for_browser")),
                 qPrintable(stages.join(QLatin1Char(','))));
        QCOMPARE(auth.loginStage(), QStringLiteral("idle"));
        QCOMPARE(failed.count(), 1);
        QVERIFY2(auth.lastError().contains(QStringLiteral("sign-in page")),
                 qPrintable(auth.lastError()));
        QVERIFY(auth.browserLoginUnavailable());
    }

    // Leaving the sign-in screen while a browser sign-in waits cancels it:
    // its listener and the server's page stayed live for up to fifteen
    // minutes, and finishing it later replaced the session the user had gone
    // back to.
    void leavingTheSignInScreenCancelsTheBrowserSignIn()
    {
        DiscoveryHomeserver homeserver;
        const quint16 port = homeserver.listen();
        QVERIFY(port != 0);
        // Plain http: no browser could be handed this sign-in page.
        const QString hs = QStringLiteral("http://127.0.0.1:%1").arg(port);
        QCoreApplication::setApplicationName(QStringLiteral("browser-sign-in-abandon"));
        AppController app(AppController::RustBackend, false,
                          [](QObject *parent) -> std::unique_ptr<SecretStore> {
                              return std::make_unique<InMemorySecretStore>(parent);
                          });
        app.showLogin();
        app.auth()->beginSsoLogin(hs, QString());
        QVERIFY(app.auth()->browserLoginInProgress());
        app.showMain();
        QVERIFY(!app.auth()->browserLoginInProgress());
        QCOMPARE(app.currentScreen(), AppController::MainScreen);
    }

private:
    QTemporaryDir m_home;
};

QTEST_MAIN(BrowserSignInEdgeTest)
#include "BrowserSignInEdgeTest.moc"
