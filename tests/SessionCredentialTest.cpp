// What a session does with credentials it cannot fully use: an OAuth client
// id the keyring could not read at restore, an OAuth session the SDK ended
// without a refresh token to try, and a sign-in this install copied from an
// item saved before install scoping, checked with the server before use.
// Real RustSdkMatrixClient and SettingsManager; fixture keyrings; a loopback
// homeserver where one is needed. Never the host keyring.

#include "app/SettingsManager.h"
#include "matrix/RustSdkMatrixClient.h"
#include "matrix/RustSessionPolicy.h"
#include "matrix_rust.h"
#include "storage/AppDataPaths.h"
#include "storage/SecretStore.h"

#include <QDir>
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QFileInfo>
#include <QTcpServer>
#include <QTimer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <atomic>

namespace {
// A keyring shared between installs, as libsecret's is: this install's items
// under its scope, and the pre-scoping items any install can read. Reads of
// the keys in `failingReads` fail ("cannot tell").
class SharedFixtureStore final : public SecretStore
{
    Q_OBJECT
public:
    using SecretStore::SecretStore;
    bool isSecure() const override { return true; }
    bool isAvailable() const override { return true; }
    QString backendName() const override { return QStringLiteral("fixture"); }
    bool isSharedBetweenInstalls() const override { return true; }
    bool storeSecret(const QString &u, const QString &k, const QString &v) override
    {
        scoped.insert(key(installScope(), u, k), v);
        return true;
    }
    QString readSecret(const QString &u, const QString &k) const override
    {
        m_failed = failingReads.contains(k);
        return m_failed ? QString() : scoped.value(key(installScope(), u, k));
    }
    QString readLegacySecret(const QString &u, const QString &k) const override
    {
        m_failed = false;
        return legacy.value(key(QString(), u, k));
    }
    bool deleteSecret(const QString &u, const QString &k) override
    {
        scoped.remove(key(installScope(), u, k));
        return true;
    }
    bool clearAccountSecrets(const QString &u) override
    {
        const QString prefix = installScope() + QLatin1Char('|') + u + QLatin1Char('|');
        for (auto it = scoped.begin(); it != scoped.end();)
            it = it.key().startsWith(prefix) ? scoped.erase(it) : std::next(it);
        return true;
    }
    QString lastError() const override { return {}; }
    bool lastReadFailed() const override { return m_failed; }

    static QString key(const QString &scope, const QString &u, const QString &k)
    {
        return scope + QLatin1Char('|') + u + QLatin1Char('|') + k;
    }
    QHash<QString, QString> scoped;
    QHash<QString, QString> legacy;
    QSet<QString> failingReads;

private:
    mutable bool m_failed = false;
};

// Answers versions, and whoami by token: "mine" is MINEDEVICE, "theirs" is
// THEIRDEVICE, anything else 401. Everything else 404.
class WhoamiHomeserver : public QObject
{
    Q_OBJECT
public:
    std::atomic<int> whoamiAsked{0};
    std::atomic<int> whoamiAnswered{0};
    int whoamiDelayMs = 0;
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
            const qsizetype end = buffer.indexOf("\r\n\r\n");
            if (end < 0)
                return;
            qsizetype length = 0;
            QByteArray auth;
            const QList<QByteArray> lines = buffer.left(end).split('\n');
            for (const QByteArray &raw : lines) {
                const QByteArray line = raw.trimmed();
                if (line.toLower().startsWith("content-length:"))
                    length = line.mid(15).trimmed().toLongLong();
                else if (line.toLower().startsWith("authorization:"))
                    auth = line.mid(14).trimmed();
            }
            if (buffer.size() < end + 4 + length)
                return;
            const QByteArray path =
                lines.value(0).trimmed().split(' ').value(1).split('?').value(0);
            buffer.remove(0, end + 4 + length);
            if (path == "/_matrix/client/versions") {
                reply(socket, 200, R"({"versions":["v1.1","v1.11"],"unstable_features":{}})");
            } else if (path == "/_matrix/client/v3/account/whoami") {
                ++whoamiAsked;
                const auto answer = [this, socket, auth] {
                    ++whoamiAnswered;
                    if (auth == "Bearer mine")
                        reply(socket, 200, R"({"user_id":"@x:example.org","device_id":"MINEDEVICE"})");
                    else if (auth == "Bearer theirs")
                        reply(socket, 200, R"({"user_id":"@x:example.org","device_id":"THEIRDEVICE"})");
                    else
                        reply(socket, 401, R"({"errcode":"M_UNKNOWN_TOKEN","error":"x","soft_logout":false})");
                };
                if (whoamiDelayMs > 0)
                    QTimer::singleShot(whoamiDelayMs, socket, answer);
                else
                    answer();
            } else {
                reply(socket, 404, R"({"errcode":"M_UNRECOGNIZED","error":"not here"})");
            }
        }
    }
    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};
} // namespace

class SessionCredentialTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_home;

    static QString slugOf(const QString &userId)
    {
        return matrix::app_data::safeUserSlug(userId);
    }

    // A saved account whose store exists, its sign-in in this install's items.
    static void saveAccount(SettingsManager &settings, const QString &server,
                            const QString &userId, const QString &access,
                            const QString &authType = QStringLiteral("password"),
                            const QString &refresh = QString())
    {
        settings.saveSession(server, userId, QStringLiteral("MINEDEVICE"), access,
                             refresh, authType,
                             authType == QLatin1String("oauth")
                                 ? QStringLiteral("client-id") : QString());
        matrix::app_data::AccountIdentity identity;
        QVERIFY(matrix::app_data::resolveAccountIdentity(server, userId, &identity));
        QDir().mkpath(identity.rustStorePath);
    }

    // As adoptLegacyKeyringSession() leaves a copied sign-in.
    static void markAdopted(const QString &userId)
    {
        QSettings raw;
        raw.setValue(QStringLiteral("accounts/%1/keyringItems").arg(slugOf(userId)),
                     QStringLiteral("adopted-unverified"));
        raw.sync();
    }

    // The running session, as a restore leaves it, for the event handler.
    static void running(RustSdkMatrixClient &client, const QString &server,
                        const QString &userId)
    {
        client.m_homeserver = server;
        client.m_userId = userId;
        client.m_deviceId = QStringLiteral("MINEDEVICE");
        client.m_loggedIn = true;
    }

    static QJsonObject revoked(bool hadRefreshToken)
    {
        return QJsonObject{
            {QStringLiteral("type"), QStringLiteral("session_token_revoked")},
            {QStringLiteral("soft_logout"), false},
            {QStringLiteral("had_refresh_token"), hadRefreshToken},
        };
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_home.isValid());
        qputenv("XDG_CONFIG_HOME", (m_home.path() + QStringLiteral("/config")).toUtf8());
        qputenv("XDG_DATA_HOME", (m_home.path() + QStringLiteral("/data")).toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("session-credential-test"));
    }

    void init()
    {
        QSettings raw;
        raw.clear();
        raw.sync();
    }

    // An OAuth account whose client id the keyring cannot read at restore is
    // the keyring card, before any handle exists. It was the "missing session
    // metadata" message, which asks for a sign-in that would start a new
    // device over a session that is fine.
    void anUnreadableOAuthClientIdIsTheKeyringCard()
    {
        SharedFixtureStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        const QString uma = QStringLiteral("@uma:example.org");
        saveAccount(settings, QStringLiteral("https://example.org"), uma,
                    QStringLiteral("access"), QStringLiteral("oauth"),
                    QStringLiteral("refresh"));
        secrets.failingReads.insert(QStringLiteral("oauthClientId"));

        RustSdkMatrixClient client(&settings);
        QSignalSpy blocked(&client, &RustSdkMatrixClient::localSessionBlocked);
        QVERIFY(!client.restoreSession());
        QCOMPARE(blocked.size(), 1);
        QCOMPARE(blocked.first().at(0).toString(),
                 QStringLiteral("secret_backend_unavailable"));
        QCOMPARE(blocked.first().at(1).toString(), uma);
        QVERIFY(!client.m_rustHandle);
        secrets.failingReads.clear();
        QCOMPARE(settings.oauthClientIdFor(uma), QStringLiteral("client-id"));
    }

    // matrix-sdk reports UnknownToken without asking the server when the
    // session holds no refresh token. For OAuth that was always a HARD
    // revocation (the next sign-in a new device). Now, when the keyring holds
    // one (or cannot answer), the session merely never got it -> the keyring
    // card and no revocation. Conclusively none, or a refresh token the
    // server refused -> the hard revocation as before, whose next sign-in
    // moves the store aside instead of leaving "Remove" as the only way on.
    void anOAuthSessionWithoutARefreshTokenIsRevokedOnlyWhenTheKeyringHasNone()
    {
        const QString server = QStringLiteral("https://example.org");
        const QString vic = QStringLiteral("@vic:example.org");
        SharedFixtureStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        saveAccount(settings, server, vic, QStringLiteral("access"),
                    QStringLiteral("oauth"), QStringLiteral("refresh"));

        {
            RustSdkMatrixClient client(&settings);
            running(client, server, vic);
            QSignalSpy blocked(&client, &RustSdkMatrixClient::localSessionBlocked);
            client.handleRustEventForTest(revoked(false));
            QCOMPARE(blocked.size(), 1);
            QCOMPARE(blocked.first().at(0).toString(),
                     QStringLiteral("secret_backend_unavailable"));
            QVERIFY(client.m_revokedUserId.isEmpty());
        }
        {
            QVERIFY(settings.updateSessionTokens(vic, QStringLiteral("access"), QString()));
            RustSdkMatrixClient client(&settings);
            running(client, server, vic);
            QSignalSpy blocked(&client, &RustSdkMatrixClient::localSessionBlocked);
            client.handleRustEventForTest(revoked(false));
            QCOMPARE(blocked.size(), 1);
            QCOMPARE(blocked.first().at(0).toString(),
                     QStringLiteral("access_token_revoked"));
            QCOMPARE(client.m_revokedUserId, vic);
            QVERIFY(client.m_softLogoutUserId.isEmpty());
        }
        {
            // The server's soft_logout absent: still the hard end, with the
            // proof the next sign-in needs to move the store aside.
            RustSdkMatrixClient client(&settings);
            running(client, server, vic);
            QSignalSpy blocked(&client, &RustSdkMatrixClient::localSessionBlocked);
            QJsonObject event = revoked(false);
            event.remove(QStringLiteral("soft_logout"));
            client.handleRustEventForTest(event);
            QCOMPARE(blocked.size(), 1);
            QCOMPARE(blocked.first().at(0).toString(),
                     QStringLiteral("access_token_revoked"));
            QCOMPARE(client.m_revokedUserId, vic);
            QCOMPARE(client.m_revokedDeviceId, QStringLiteral("MINEDEVICE"));
        }
        {
            RustSdkMatrixClient client(&settings);
            running(client, server, vic);
            QSignalSpy blocked(&client, &RustSdkMatrixClient::localSessionBlocked);
            client.handleRustEventForTest(revoked(true));
            QCOMPARE(blocked.size(), 1);
            QCOMPARE(blocked.first().at(0).toString(),
                     QStringLiteral("access_token_revoked"));
            QCOMPARE(client.m_revokedUserId, vic);
        }
    }

    // A password sign-in copied from an item saved before install scoping is
    // another install's device: this install's copy goes, the shared item
    // stays, the card says so, and the next start never adopts it again.
    void anAdoptedSignInOfAnotherInstallsDeviceIsDeclined()
    {
        WhoamiHomeserver homeserver;
        const quint16 port = homeserver.listen();
        QVERIFY(port);
        const QString server = QStringLiteral("http://127.0.0.1:%1").arg(port);
        const QString wes = QStringLiteral("@wes:example.org");

        SharedFixtureStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        saveAccount(settings, server, wes, QStringLiteral("theirs"));
        secrets.legacy.insert(SharedFixtureStore::key(QString(), wes,
                                                      QStringLiteral("accessToken")),
                              QStringLiteral("theirs"));
        markAdopted(wes);
        QVERIFY(settings.keyringItemsAdoptedUnverified(wes));

        RustSdkMatrixClient client(&settings);
        QSignalSpy blocked(&client, &RustSdkMatrixClient::localSessionBlocked);
        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        QSignalSpy succeeded(&client, &MatrixClient::loginSucceeded);
        QVERIFY(client.restoreSession());
        QTRY_VERIFY_WITH_TIMEOUT(!failed.isEmpty(), 30000);
        QCOMPARE(homeserver.whoamiAsked.load(), 1);
        QVERIFY(succeeded.isEmpty());
        QCOMPARE(blocked.size(), 1);
        QCOMPARE(blocked.first().at(0).toString(), QStringLiteral("keyring_other_install"));
        QVERIFY(settings.keyringItemsDeclinedForOtherInstall(wes));
        // This install's copy is gone, the other install's item is untouched,
        // and it is not adopted again.
        QVERIFY(settings.accessTokenFor(wes).isEmpty());
        QVERIFY(!settings.secretBackendUnavailable());
        QCOMPARE(secrets.readLegacySecret(wes, QStringLiteral("accessToken")),
                 QStringLiteral("theirs"));
        QVERIFY(!settings.hasSession());
        QVERIFY(!client.m_rustHandle);

        // The card asks for a sign-in: a password sign-in continues as the
        // recorded device, into this store, and never reaches a reason whose
        // remedy deletes the store.
        matrix::app_data::AccountIdentity identity;
        QVERIFY(matrix::app_data::resolveAccountIdentity(server, wes, &identity));
        QSignalSpy reset(&client, &RustSdkMatrixClient::localSessionResetRequired);
        QSignalSpy blockedAgain(&client, &RustSdkMatrixClient::localSessionBlocked);
        client.login(server, wes, QStringLiteral("not-a-password"));
        QVERIFY(reset.isEmpty());
        for (const QList<QVariant> &b : std::as_const(blockedAgain))
            QVERIFY2(!matrix::rust_session::suggestsLocalResetForCode(b.at(0).toString()),
                     qPrintable(b.at(0).toString()));
        QCOMPARE(client.m_resumeDeviceId, QStringLiteral("MINEDEVICE"));
        QVERIFY(QFileInfo::exists(identity.rustStorePath));
    }

    // An answer that arrives after the account moved on changes nothing: a
    // newer sign-in written during the check, or a session of the account
    // running by then, keeps its tokens.
    void aLateAdoptionAnswerNeverRemovesANewerSignIn()
    {
        WhoamiHomeserver homeserver;
        homeserver.whoamiDelayMs = 1500;
        const quint16 port = homeserver.listen();
        QVERIFY(port);
        const QString server = QStringLiteral("http://127.0.0.1:%1").arg(port);
        SharedFixtureStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);

        // A newer sign-in during the check.
        const QString abe = QStringLiteral("@abe:example.org");
        saveAccount(settings, server, abe, QStringLiteral("theirs"));
        markAdopted(abe);
        {
            RustSdkMatrixClient client(&settings);
            QVERIFY(client.restoreSession());
            settings.saveSession(server, abe, QStringLiteral("NEWDEVICE"),
                                 QStringLiteral("newer-access"));
            QTRY_COMPARE_WITH_TIMEOUT(homeserver.whoamiAnswered.load(), 1, 30000);
            QTRY_VERIFY_WITH_TIMEOUT(!client.m_whoamiHandle, 10000);
            QCOMPARE(settings.accessTokenFor(abe), QStringLiteral("newer-access"));
            QVERIFY(!settings.keyringItemsDeclinedForOtherInstall(abe));
        }

        // A session of the account running by the time it answers.
        const QString bea = QStringLiteral("@bea:example.org");
        saveAccount(settings, server, bea, QStringLiteral("theirs"));
        markAdopted(bea);
        {
            RustSdkMatrixClient client(&settings);
            QVERIFY(client.restoreSession());
            client.m_userId = bea;
            client.m_loggedIn = true;
            QTRY_COMPARE_WITH_TIMEOUT(homeserver.whoamiAnswered.load(), 2, 30000);
            QTRY_VERIFY_WITH_TIMEOUT(!client.m_whoamiHandle, 10000);
            client.m_loggedIn = false;
            QCOMPARE(settings.accessTokenFor(bea), QStringLiteral("theirs"));
            QVERIFY(settings.keyringItemsAdoptedUnverified(bea));
        }
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));
    }

    // The same device confirms the adoption; a 401 decides nothing and the
    // restore goes on, unverified; an OAuth sign-in is never asked about.
    void anAdoptedSignInIsConfirmedOrRestoredUnverified()
    {
        WhoamiHomeserver homeserver;
        const quint16 port = homeserver.listen();
        QVERIFY(port);
        const QString server = QStringLiteral("http://127.0.0.1:%1").arg(port);
        SharedFixtureStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);

        struct Case { QString user; QString token; QString authType; bool confirmed;
                      int asked; };
        const QList<Case> cases{
            {QStringLiteral("@xia:example.org"), QStringLiteral("mine"),
             QStringLiteral("password"), true, 1},
            {QStringLiteral("@yan:example.org"), QStringLiteral("expired"),
             QStringLiteral("password"), false, 1},
            {QStringLiteral("@zed:example.org"), QStringLiteral("whatever"),
             QStringLiteral("oauth"), false, 0},
        };
        for (const Case &c : cases) {
            homeserver.whoamiAsked = 0;
            saveAccount(settings, server, c.user, c.token, c.authType,
                        QStringLiteral("refresh"));
            markAdopted(c.user);
            auto client = std::make_unique<RustSdkMatrixClient>(&settings);
            QSignalSpy succeeded(client.get(), &MatrixClient::loginSucceeded);
            QSignalSpy blocked(client.get(), &RustSdkMatrixClient::localSessionBlocked);
            QVERIFY2(client->restoreSession(), qPrintable(c.user));
            QVERIFY2(succeeded.wait(30000), qPrintable(c.user));
            QCOMPARE(homeserver.whoamiAsked.load(), c.asked);
            QVERIFY(blocked.isEmpty());
            QCOMPARE(settings.keyringItemsAdoptedUnverified(c.user), !c.confirmed);
            client.reset();
            QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));
        }
    }
};

QTEST_MAIN(SessionCredentialTest)
#include "SessionCredentialTest.moc"
