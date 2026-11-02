
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
class DiscoveryHomeserver : public QObject
{
    Q_OBJECT
public:
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
}

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

    void aStoppedAttemptsDeadlineNeverCountsInTheNextOne()
    {
        OAuthCallbackServer server;
        server.setRequestDeadline(std::chrono::milliseconds(1));
        QVERIFY(server.listen());
        QTcpSocket silent;
        silent.connectToHost(server.serverAddress(), QUrl(server.redirectUri()).port());
        QTRY_COMPARE_WITH_TIMEOUT(server.pendingConnectionCount(), 1, 5000);
        QThread::msleep(30);
        server.stop();
        QVERIFY(server.listen());
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

    void aDiscoveryStartedFromTheAnswerToTheLastOneIsAnswered()
    {
        DiscoveryHomeserver homeserver;
        const quint16 port = homeserver.listen();
        QVERIFY(port != 0);
        const QString hs = QStringLiteral("http://127.0.0.1:%1").arg(port);

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
        for (const QList<QVariant> &answer : std::as_const(discovered))
            QVERIFY(answer.at(1).toBool());
    }

    void theNamedSsoProvidersSurviveASignInStartedBeforeTheyArrive()
    {
        DiscoveryHomeserver homeserver;
        homeserver.offerSso = true;
        homeserver.loginDelayMs = 1500;
        const quint16 port = homeserver.listen();
        QVERIFY(port != 0);
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
        QVERIFY(client.ensureOAuthBootstrapHandle(hs));
        client.m_ssoHomeserver = hs;
        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        client.completeSsoLogin(kai, QStringLiteral("NEWDEVICE"),
                                QStringLiteral("new-access"), QString());
        QVERIFY(!failed.isEmpty());
        QCOMPARE(settings.accountRecord(kai).value(QStringLiteral("deviceId")).toString(),
                 QStringLiteral("OLDDEVICE"));
        QTRY_COMPARE_WITH_TIMEOUT(homeserver.logouts, 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!client.m_revokeHandle, 10000);
    }

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

    void leavingTheSignInScreenCancelsTheBrowserSignIn()
    {
        DiscoveryHomeserver homeserver;
        const quint16 port = homeserver.listen();
        QVERIFY(port != 0);
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
