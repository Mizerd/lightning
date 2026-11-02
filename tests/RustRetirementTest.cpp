// Retiring a Rust client (`mx_rust_shutdown_tasks`, and `mx_rust_destroy`
// dropping the tokio runtime) must not happen on the GUI thread.
//
// "An account switch is fast" cannot catch a regression: a small store closes
// in milliseconds either way. So this measures the property that the caller
// returns while the work is still outstanding, with a real Rust client,
// runtime and on-disk store.

#include "matrix/RustSdkMatrixClient.h"
#include "matrix_rust.h"
#include "app/SettingsManager.h"
#include "storage/AppDataPaths.h"
#include "storage/SecretStore.h"

#include <QDir>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest/QtTest>

#include <atomic>

namespace {
void *createRealClient(const QTemporaryDir &dir, const char *name)
{
    const QByteArray path = (dir.path() + QLatin1Char('/')
                             + QLatin1String(name)).toUtf8();
    return mx_rust_create(path.constData());
}

const QString kAccess0 = QStringLiteral("fixture-access-0");
const QString kRefresh0 = QStringLiteral("fixture-refresh-0");
const QString kUser = QStringLiteral("@rotation:localhost");
const QString kDevice = QStringLiteral("ROTATIONDEV");

class FixtureSecretStore final : public SecretStore
{
    Q_OBJECT
public:
    using SecretStore::SecretStore;
    bool isSecure() const override { return true; }
    bool isAvailable() const override { return true; }
    QString backendName() const override { return QStringLiteral("fixture"); }
    bool storeSecret(const QString &u, const QString &k, const QString &v) override
    {
        QMutexLocker lock(&m_mutex);
        if (m_failingWrites.contains(k))
            return false;
        m_values.insert(u + QLatin1Char('/') + k, v);
        return true;
    }
    void failWritesOf(const QString &key, bool fail)
    {
        QMutexLocker lock(&m_mutex);
        if (fail)
            m_failingWrites.insert(key);
        else
            m_failingWrites.remove(key);
    }
    QString readSecret(const QString &u, const QString &k) const override
    {
        QMutexLocker lock(&m_mutex);
        m_lastReadFailed = m_failingReads.contains(k);
        if (m_lastReadFailed)
            return {};
        return m_values.value(u + QLatin1Char('/') + k);
    }
    bool lastReadFailed() const override
    {
        QMutexLocker lock(&m_mutex);
        return m_lastReadFailed;
    }
    void failReadsOf(const QString &key, bool fail)
    {
        QMutexLocker lock(&m_mutex);
        if (fail)
            m_failingReads.insert(key);
        else
            m_failingReads.remove(key);
    }
    bool deleteSecret(const QString &u, const QString &k) override
    {
        QMutexLocker lock(&m_mutex);
        m_values.remove(u + QLatin1Char('/') + k);
        return true;
    }
    bool clearAccountSecrets(const QString &u) override
    {
        QMutexLocker lock(&m_mutex);
        const QString prefix = u + QLatin1Char('/');
        for (auto it = m_values.begin(); it != m_values.end();)
            it = it.key().startsWith(prefix) ? m_values.erase(it) : std::next(it);
        return true;
    }
    QString lastError() const override { return {}; }

private:
    mutable QMutex m_mutex;
    QHash<QString, QString> m_values;
    QSet<QString> m_failingReads;
    QSet<QString> m_failingWrites;
    mutable bool m_lastReadFailed = false;
};

class RotatingHomeserver : public QObject
{
    Q_OBJECT
public:
    std::atomic<int> logouts{0};
    std::atomic<int> refreshesAsked{0};
    QSemaphore refreshServed;

    QPair<QString, QString> latest() const
    {
        QMutexLocker lock(&m_mutex);
        return m_issued.last();
    }
    int rotations() const
    {
        QMutexLocker lock(&m_mutex);
        return int(m_issued.size()) - 1;
    }

public Q_SLOTS:
    void openRefreshGate()
    {
        m_gateOpen = true;
        const auto parked = std::exchange(m_parked, {});
        for (const auto &waiting : parked)
            answerRefresh(waiting.first, waiting.second);
    }

    quint16 listen()
    {
        m_server = new QTcpServer(this);
        connect(m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server->nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this,
                        [this, socket] { serve(socket); });
                connect(socket, &QTcpSocket::disconnected, socket,
                        &QObject::deleteLater);
            }
        });
        return m_server->listen(QHostAddress::LocalHost, 0) ? m_server->serverPort() : 0;
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
            const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
            qsizetype length = 0;
            QByteArray auth;
            for (const QByteArray &raw : lines) {
                const QByteArray line = raw.trimmed();
                const qsizetype colon = line.indexOf(':');
                if (colon < 0)
                    continue;
                const QByteArray name = line.left(colon).trimmed().toLower();
                const QByteArray value = line.mid(colon + 1).trimmed();
                if (name == "content-length")
                    length = value.toLongLong();
                else if (name == "authorization")
                    auth = value;
            }
            if (buffer.size() < headerEnd + 4 + length)
                return;
            const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
            const QByteArray path = requestLine.value(1).split('?').value(0);
            const QByteArray body = buffer.mid(headerEnd + 4, length);
            buffer.remove(0, headerEnd + 4 + length);
            respond(socket, path, auth, body);
        }
    }

    void respond(QTcpSocket *socket, const QByteArray &path, const QByteArray &auth,
                 const QByteArray &body)
    {
        if (qEnvironmentVariableIsSet("ROTATION_FIXTURE_TRACE"))
            fprintf(stderr, "fixture: %s auth=%s\n", path.constData(),
                    auth.isEmpty() ? "none" : auth.right(1).constData());
        if (path == "/_matrix/client/versions") {
            reply(socket, 200, R"({"versions":["v1.1","v1.2","v1.3","v1.4","v1.5","v1.6","v1.7","v1.8","v1.9","v1.10","v1.11"],"unstable_features":{}})");
            return;
        }
        if (path.endsWith("/refresh")) {
            ++refreshesAsked;
            const QString asked = QJsonDocument::fromJson(body).object()
                                      .value(QStringLiteral("refresh_token")).toString();
            if (!m_gateOpen && rotations() == 0 && asked == kRefresh0) {
                m_parked.append({ socket, asked });
                return;
            }
            answerRefresh(socket, asked);
            return;
        }
        if (auth.isEmpty()) {
            reply(socket, 404, R"({"errcode":"M_UNRECOGNIZED","error":"not here"})");
            return;
        }
        if (rotations() == 0 || auth != "Bearer " + latest().first.toUtf8()) {
            reply(socket, 401, R"({"errcode":"M_UNKNOWN_TOKEN","error":"expired","soft_logout":true})");
            return;
        }
        if (path.endsWith("/logout")) {
            ++logouts;
            reply(socket, 200, "{}");
        }
    }

    void answerRefresh(QTcpSocket *socket, const QString &asked)
    {
        QPair<QString, QString> pair;
        {
            QMutexLocker lock(&m_mutex);
            qsizetype at = -1;
            for (qsizetype i = 0; i < m_issued.size(); ++i) {
                if (m_issued.at(i).second == asked)
                    at = i;
            }
            if (at < 0) {
                lock.unlock();
                reply(socket, 401, R"({"errcode":"M_UNKNOWN_TOKEN","error":"unknown","soft_logout":false})");
                return;
            }
            if (at + 1 == m_issued.size()) {
                const QString n = QString::number(m_issued.size());
                m_issued.append({ QStringLiteral("fixture-access-") + n,
                                  QStringLiteral("fixture-refresh-") + n });
            }
            pair = m_issued.at(at + 1);
        }
        reply(socket, 200, QJsonDocument(QJsonObject{
            { QStringLiteral("access_token"), pair.first },
            { QStringLiteral("refresh_token"), pair.second },
            { QStringLiteral("expires_in_ms"), 3600000 },
        }).toJson(QJsonDocument::Compact));
        refreshServed.release();
    }

    mutable QMutex m_mutex;
    QList<QPair<QString, QString>> m_issued{ { kAccess0, kRefresh0 } };
    QTcpServer *m_server = nullptr;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    bool m_gateOpen = false;
    QList<QPair<QTcpSocket *, QString>> m_parked;
};

struct RotatedSession {
    QThread serverThread;
    RotatingHomeserver *server = nullptr;
    FixtureSecretStore secrets;
    SettingsManager settings;
    std::unique_ptr<RustSdkMatrixClient> client;
    QString homeserver;

    ~RotatedSession()
    {
        client.reset();
        RustSdkMatrixClient::waitForRustRetirement(30000);
        if (server)
            QMetaObject::invokeMethod(server, &QObject::deleteLater);
        serverThread.quit();
        serverThread.wait(5000);
    }

    bool start()
    {
        server = new RotatingHomeserver;
        server->moveToThread(&serverThread);
        serverThread.start();
        quint16 port = 0;
        QMetaObject::invokeMethod(server, "listen", Qt::BlockingQueuedConnection,
                                  Q_RETURN_ARG(quint16, port));
        if (port == 0)
            return false;
        homeserver = QStringLiteral("http://127.0.0.1:%1").arg(port);

        settings.setSecretStore(&secrets);
        settings.saveSession(homeserver, kUser, kDevice, kAccess0, kRefresh0);
        matrix::app_data::AccountIdentity identity;
        if (!matrix::app_data::resolveAccountIdentity(homeserver, kUser, &identity))
            return false;
        QDir().mkpath(identity.rustStorePath);

        client = std::make_unique<RustSdkMatrixClient>(&settings);
        QSignalSpy restored(client.get(), &MatrixClient::loginSucceeded);
        if (!client->restoreSession())
            return false;
        if (restored.isEmpty() && !restored.wait(30000))
            return false;
        client->startSync();
        QMetaObject::invokeMethod(server, "openRefreshGate", Qt::QueuedConnection);
        if (!server->refreshServed.tryAcquire(1, 30000))
            return false;
        QThread::msleep(1500);
        return true;
    }

    QString stored(const char *key) const { return secrets.readSecret(kUser, QLatin1String(key)); }
};
}

class RustRetirementTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_home.isValid());
        qputenv("XDG_CONFIG_HOME", (m_home.path() + QStringLiteral("/config")).toUtf8());
        qputenv("XDG_DATA_HOME", (m_home.path() + QStringLiteral("/data")).toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("rust-retirement-test"));
    }

    void passwordLoginWithoutSettingsFailsNormallyInsteadOfCrashing()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        {
            RustSdkMatrixClient client(nullptr);
            client.setStorePathOverride(dir.path() + QStringLiteral("/store"));
            QSignalSpy failed(&client, &MatrixClient::loginFailed);
            client.login(QStringLiteral("http://127.0.0.1:9"),
                         QStringLiteral("@fixture:127.0.0.1:9"),
                         QStringLiteral("synthetic-fixture-only"));
            QTRY_VERIFY_WITH_TIMEOUT(!failed.isEmpty(), 10000);
            QCOMPARE(client.connectionState(), MatrixClient::Error);
        }
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));
    }

    void retiringClientsLeavesTheWorkOutstanding()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));

        for (int i = 0; i < 6; ++i) {
            void *client = createRealClient(
                dir, qPrintable(QStringLiteral("outstanding-%1").arg(i)));
            QVERIFY(client);
            RustSdkMatrixClient::retireRustHandleAsync(client, QString());
        }

        QVERIFY2(!RustSdkMatrixClient::waitForRustRetirement(0),
                 "retirement had already finished when the caller returned, "
                 "so it ran on the caller's thread");

        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(60000));
    }

    void severalClientsCanBeRetiredAtOnce()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QElapsedTimer handOff;
        handOff.start();
        for (int i = 0; i < 4; ++i) {
            void *client = createRealClient(
                dir, qPrintable(QStringLiteral("store-%1").arg(i)));
            QVERIFY(client);
            RustSdkMatrixClient::retireRustHandleAsync(client, QString());
        }
        const qint64 callerMs = handOff.elapsed();
        QVERIFY2(callerMs < 500,
                 qPrintable(QStringLiteral("four hand-offs blocked for %1 ms")
                                .arg(callerMs)));

        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(60000));
    }

    // The deletion paths depend on this: waiting must actually mean the store
    // is closed, or removing the directory races an open SQLite connection.
    // After the wait, the store's files must be re-openable and removable.
    void afterWaitingTheStoreIsReallyClosed()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString storePath = dir.path() + QStringLiteral("/store-close");
        void *client = createRealClient(dir, "store-close");
        QVERIFY(client);

        RustSdkMatrixClient::retireRustHandleAsync(client, QString());
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));

        if (QFileInfo::exists(storePath))
            QVERIFY(QDir(storePath).removeRecursively());
    }

    void retiringNothingIsSafe()
    {
        RustSdkMatrixClient::retireRustHandleAsync(nullptr, QString());
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(5000));
    }

    void aRotationTheGuiNeverDrainedIsKeptWhenTheHandleRetires()
    {
        RotatedSession session;
        QVERIFY2(session.start(), "the fixture session did not reach a refresh");
        QVERIFY(session.server->rotations() >= 1);
        QCOMPARE(session.stored("refreshToken"), kRefresh0);

        QVERIFY(session.client->detachSession());
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));
        const auto latest = session.server->latest();
        QTRY_COMPARE_WITH_TIMEOUT(session.stored("refreshToken"), latest.second, 5000);
        QCOMPARE(session.stored("accessToken"), latest.first);
        QCOMPARE(session.settings.accountRecord(kUser)
                     .value(QStringLiteral("deviceId")).toString(), kDevice);
    }

    void aRemovedAccountNeverGetsTheRetiredTokensBack()
    {
        RotatedSession session;
        QVERIFY2(session.start(), "the fixture session did not reach a refresh");
        QVERIFY(session.client->detachSession());
        QVERIFY(session.settings.clearSessionForAccount(kUser));
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));
        QTest::qWait(500);
        QVERIFY(!session.settings.hasSavedAccount(kUser));
        QCOMPARE(session.stored("accessToken"), QString());
        QCOMPARE(session.stored("refreshToken"), QString());
    }

    void aStaleRetiredSignInNeverOverwritesANewerOne()
    {
        RotatedSession session;
        QVERIFY2(session.start(), "the fixture session did not reach a refresh");
        QVERIFY(session.client->detachSession());
        const QString newerAccess = QStringLiteral("fixture-access-newer");
        const QString newerRefresh = QStringLiteral("fixture-refresh-newer");
        QVERIFY(session.settings.updateSessionTokens(kUser, newerAccess, newerRefresh));
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));
        QTest::qWait(500);
        QCOMPARE(session.stored("accessToken"), newerAccess);
        QCOMPARE(session.stored("refreshToken"), newerRefresh);
    }

    void aSignOutLeavesNoSignInBehind()
    {
        RotatedSession session;
        QVERIFY2(session.start(), "the fixture session did not reach a refresh");
        QSignalSpy loggedOut(session.client.get(), &MatrixClient::loggedOut);
        session.client->logout();
        QVERIFY(loggedOut.wait(30000));
        QCOMPARE(session.server->logouts.load(), 1);
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));
        QTest::qWait(500);
        QVERIFY(!session.settings.hasSavedAccount(kUser));
        QCOMPARE(session.stored("accessToken"), QString());
        QCOMPARE(session.stored("refreshToken"), QString());
    }

    void aRotationTheKeyringRefusedIsSavedOnceItAnswers()
    {
        RotatedSession session;
        QVERIFY2(session.start(), "the fixture session did not reach a refresh");
        session.client->m_tokenWriteRetryMs = 200;
        session.secrets.failWritesOf(QStringLiteral("refreshToken"), true);
        session.secrets.failWritesOf(QStringLiteral("accessToken"), true);
        QTest::qWait(600);
        QCOMPARE(session.stored("refreshToken"), kRefresh0);
        session.secrets.failWritesOf(QStringLiteral("refreshToken"), false);
        session.secrets.failWritesOf(QStringLiteral("accessToken"), false);
        const auto latest = session.server->latest();
        QTRY_COMPARE_WITH_TIMEOUT(session.stored("refreshToken"), latest.second, 10000);
        QCOMPARE(session.stored("accessToken"), latest.first);
    }

    void aRefusedRefreshTokenIsStillWrittenBackWhenTheSessionEnds()
    {
        RotatedSession session;
        QVERIFY2(session.start(), "the fixture session did not reach a refresh");
        session.client->m_tokenWriteRetryMs = 600000;
        session.secrets.failWritesOf(QStringLiteral("refreshToken"), true);
        QTest::qWait(600);
        QCOMPARE(session.stored("refreshToken"), kRefresh0);
        session.secrets.failWritesOf(QStringLiteral("refreshToken"), false);

        QVERIFY(session.client->detachSession());
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));
        const auto latest = session.server->latest();
        QTRY_COMPARE_WITH_TIMEOUT(session.stored("refreshToken"), latest.second, 5000);
        QCOMPARE(session.stored("accessToken"), latest.first);
    }

    void aRefusedWriteIsRetriedForABoundedSpellAndAgainAtTheNextRotation()
    {
        RotatedSession session;
        QVERIFY2(session.start(), "the fixture session did not reach a refresh");
        auto *client = session.client.get();
        client->m_tokenWriteRetryMs = 100;
        client->m_tokenWriteGiveUpMs = 700;
        session.secrets.failWritesOf(QStringLiteral("refreshToken"), true);
        session.secrets.failWritesOf(QStringLiteral("accessToken"), true);
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression(QStringLiteral("still not saved after an hour")));
        QTRY_VERIFY_WITH_TIMEOUT(client->m_tokenWriteAttempts > 0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!client->m_tokenWriteRetry.isActive(), 10000);
        const int attempts = client->m_tokenWriteAttempts;
        QTest::qWait(600);
        QCOMPARE(client->m_tokenWriteAttempts, attempts);
        QCOMPARE(session.stored("refreshToken"), kRefresh0);

        const auto latest = session.server->latest();
        client->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("session_tokens_refreshed")},
            {QStringLiteral("access_token"), latest.first},
            {QStringLiteral("refresh_token"), latest.second},
        });
        QVERIFY(client->m_tokenWriteRetry.isActive());
        session.secrets.failWritesOf(QStringLiteral("refreshToken"), false);
        session.secrets.failWritesOf(QStringLiteral("accessToken"), false);
        QTRY_COMPARE_WITH_TIMEOUT(session.stored("refreshToken"), latest.second, 5000);
    }

    void anOlderRotationNeverOverwritesTheSessionsCurrentPair()
    {
        RotatedSession session;
        QVERIFY2(session.start(), "the fixture session did not reach a refresh");
        const auto latest = session.server->latest();
        QTRY_COMPARE_WITH_TIMEOUT(session.stored("refreshToken"), latest.second, 5000);
        session.client->handleRustEventForTest(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("session_tokens_refreshed")},
            {QStringLiteral("access_token"), kAccess0},
            {QStringLiteral("refresh_token"), kRefresh0},
        });
        QCOMPARE(session.stored("refreshToken"), latest.second);
        QCOMPARE(session.stored("accessToken"), latest.first);
    }

    void aRetryNeverWritesAfterASignOut()
    {
        RotatedSession session;
        QVERIFY2(session.start(), "the fixture session did not reach a refresh");
        session.client->m_tokenWriteRetryMs = 300;
        session.secrets.failWritesOf(QStringLiteral("refreshToken"), true);
        session.secrets.failWritesOf(QStringLiteral("accessToken"), true);
        QTRY_VERIFY_WITH_TIMEOUT(session.client->m_tokenWriteRetry.isActive(), 5000);
        QSignalSpy loggedOut(session.client.get(), &MatrixClient::loggedOut);
        session.client->logout();
        QVERIFY(loggedOut.wait(30000));
        session.secrets.failWritesOf(QStringLiteral("refreshToken"), false);
        session.secrets.failWritesOf(QStringLiteral("accessToken"), false);
        QTest::qWait(1500);
        QVERIFY(!session.settings.hasSavedAccount(kUser));
        QCOMPARE(session.stored("accessToken"), QString());
        QCOMPARE(session.stored("refreshToken"), QString());
    }

    void anUnreadableAccountNeverTearsDownTheRunningOne()
    {
        RotatedSession session;
        QVERIFY2(session.start(), "the fixture session did not reach a refresh");
        auto *client = session.client.get();
        void *running = client->m_rustHandle;
        QVERIFY(running);
        const QString other = QStringLiteral("@other:localhost");
        session.settings.saveSession(session.homeserver, other, QStringLiteral("OTHERDEV"),
                                     QStringLiteral("other-access"),
                                     QStringLiteral("other-refresh"));
        matrix::app_data::AccountIdentity identity;
        QVERIFY(matrix::app_data::resolveAccountIdentity(session.homeserver, other,
                                                         &identity));
        QDir().mkpath(identity.rustStorePath);
        session.secrets.failReadsOf(QStringLiteral("refreshToken"), true);
        QSignalSpy blocked(client, &RustSdkMatrixClient::localSessionBlocked);
        QVERIFY(!client->restoreSession());
        session.secrets.failReadsOf(QStringLiteral("refreshToken"), false);
        QCOMPARE(blocked.size(), 1);
        QCOMPARE(blocked.first().at(1).toString(), other);
        QCOMPARE(client->m_rustHandle, running);
        QVERIFY(client->isLoggedIn());
        QCOMPARE(client->currentUserId(), kUser);
    }

    void anUnreadableRefreshTokenNeverRestoresASessionWithoutOne()
    {
        RotatingHomeserver *server = new RotatingHomeserver;
        QThread serverThread;
        server->moveToThread(&serverThread);
        serverThread.start();
        const auto stopServer = qScopeGuard([&] {
            QMetaObject::invokeMethod(server, &QObject::deleteLater);
            serverThread.quit();
            serverThread.wait(5000);
        });
        quint16 port = 0;
        QMetaObject::invokeMethod(server, "listen", Qt::BlockingQueuedConnection,
                                  Q_RETURN_ARG(quint16, port));
        QVERIFY(port != 0);
        const QString homeserver = QStringLiteral("http://127.0.0.1:%1").arg(port);

        FixtureSecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        settings.saveSession(homeserver, kUser, kDevice, kAccess0, kRefresh0);
        matrix::app_data::AccountIdentity identity;
        QVERIFY(matrix::app_data::resolveAccountIdentity(homeserver, kUser, &identity));
        QDir().mkpath(identity.rustStorePath);

        secrets.failReadsOf(QStringLiteral("refreshToken"), true);
        auto client = std::make_unique<RustSdkMatrixClient>(&settings);
        const auto retire = qScopeGuard([&] {
            client.reset();
            RustSdkMatrixClient::waitForRustRetirement(30000);
        });
        QSignalSpy blocked(client.get(), &RustSdkMatrixClient::localSessionBlocked);
        QSignalSpy restoredSpy(client.get(), &MatrixClient::loginSucceeded);

        const bool restored = client->restoreSession();
        if (restored) {
            if (restoredSpy.isEmpty())
                restoredSpy.wait(30000);
            client->startSync();
            QTRY_VERIFY_WITH_TIMEOUT(!blocked.isEmpty(), 30000);
            QFAIL(qPrintable(QStringLiteral(
                "restored without its refresh token; the session ended as "
                "'%1' after %2 refresh request(s)")
                .arg(blocked.first().at(0).toString())
                .arg(server->refreshesAsked.load())));
        }
        QCOMPARE(blocked.size(), 1);
        QCOMPARE(blocked.first().at(0).toString(),
                 QStringLiteral("secret_backend_unavailable"));
        QCOMPARE(blocked.first().at(1).toString(), kUser);
        secrets.failReadsOf(QStringLiteral("refreshToken"), false);
        QCOMPARE(secrets.readSecret(kUser, QStringLiteral("refreshToken")), kRefresh0);
        QCOMPARE(settings.accountRecord(kUser).value(QStringLiteral("deviceId")).toString(),
                 kDevice);
        QVERIFY(QFileInfo::exists(identity.rustStorePath));

        QVERIFY(client->restoreSession());
        QVERIFY(restoredSpy.wait(30000));
        client->startSync();
        QMetaObject::invokeMethod(server, "openRefreshGate", Qt::QueuedConnection);
        QTRY_COMPARE_WITH_TIMEOUT(secrets.readSecret(kUser, QStringLiteral("refreshToken")),
                                  server->latest().second, 30000);
        QCOMPARE(blocked.size(), 1);
    }

private:
    QTemporaryDir m_home;
};

QTEST_MAIN(RustRetirementTest)
#include "RustRetirementTest.moc"
