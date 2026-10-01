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

#include <QElapsedTimer>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QSemaphore>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest/QtTest>

#include <atomic>

namespace {
// A real client, with its own store directory. mx_rust_create builds the
// bridge and its tokio runtime; it performs no network I/O, so this is
// deterministic and offline.
void *createRealClient(const QTemporaryDir &dir, const char *name)
{
    const QByteArray path = (dir.path() + QLatin1Char('/')
                             + QLatin1String(name)).toUtf8();
    return mx_rust_create(path.constData());
}

// Test fixtures only; never real credentials.
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
        m_values.insert(u + QLatin1Char('/') + k, v);
        return true;
    }
    QString readSecret(const QString &u, const QString &k) const override
    {
        QMutexLocker lock(&m_mutex);
        return m_values.value(u + QLatin1Char('/') + k);
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
};

// A homeserver with MAS's rotation rules: the first access token has expired
// and only the newest one is accepted (anything else is refused as unknown,
// which makes the SDK refresh);
// the newest refresh token buys a new pair; an already used one gets back the
// pair it bought (MAS's double-refresh grace) and anything else is refused.
// The FIRST rotation is held until the test opens the gate, so the test
// decides when it lands. Requests with the newest token wait, as a long-poll
// sync does, except a logout. Runs on its own thread, so it answers while the
// test thread deliberately does not spin its event loop.
class RotatingHomeserver : public QObject
{
    Q_OBJECT
public:
    std::atomic<int> logouts{0};
    QSemaphore refreshServed;

    // The pair the server last issued: what the client must end up with.
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
        // The first access token has expired; only a newer one is accepted.
        if (rotations() == 0 || auth != "Bearer " + latest().first.toUtf8()) {
            reply(socket, 401, R"({"errcode":"M_UNKNOWN_TOKEN","error":"expired","soft_logout":true})");
            return;
        }
        if (path.endsWith("/logout")) {
            ++logouts;
            reply(socket, 200, "{}");
        }
        // Anything else waits, as a long-poll sync does.
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

// One account restored against the rotating homeserver, its token refreshed
// by the SDK, and the test thread then kept OFF its event loop, so the
// `session_tokens_refreshed` event sits undrained in the handle: the window
// between the last drain and the handle's release.
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

    // False on any setup failure; the caller QVERIFYs it.
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
        // The account's store, as a previous run leaves it.
        QDir().mkpath(identity.rustStorePath);

        client = std::make_unique<RustSdkMatrixClient>(&settings);
        QSignalSpy restored(client.get(), &MatrixClient::loginSucceeded);
        if (!client->restoreSession())
            return false;
        if (restored.isEmpty() && !restored.wait(30000))
            return false;
        client->startSync();
        // Not spinning the event loop from here on: nothing drains the handle.
        QMetaObject::invokeMethod(server, "openRefreshGate", Qt::QueuedConnection);
        if (!server->refreshServed.tryAcquire(1, 30000))
            return false;
        // Long enough for the SDK to adopt the new pair and announce it.
        QThread::msleep(1500);
        return true;
    }

    QString stored(const char *key) const { return secrets.readSecret(kUser, QLatin1String(key)); }
};
} // namespace

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

    // The work is still outstanding when the caller returns: done inline, the
    // pool would be empty and `waitForRustRetirement(0)` would report drained.
    // Several clients are handed over so the answer does not depend on
    // winning a race against one fast close.
    void retiringClientsLeavesTheWorkOutstanding()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        // Drain anything a previous case left behind, so "not drained" below
        // can only be about the clients handed over in this one.
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

        // ...and it really does finish, rather than being dropped.
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(60000));
    }

    // Rapid switching hands over several clients; each owns its own store and
    // all must close.
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

        // The whole point of the wait: the directory can now be removed
        // without racing anything.
        if (QFileInfo::exists(storePath))
            QVERIFY(QDir(storePath).removeRecursively());
    }

    // A null handle is a no-op rather than a crash: releaseRustHandle() can
    // reach the hand-off with nothing to retire on a client that never
    // logged in.
    void retiringNothingIsSafe()
    {
        RustSdkMatrixClient::retireRustHandleAsync(nullptr, QString());
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(5000));
    }

    // Reported 2026-10-01 as an OAuth account "signed out remotely". The SDK
    // rotates a refresh token and announces it through an event C++ drains
    // every 100 ms; a handle released before that drain (account switch, a
    // revoked session ended, quit) took the event with it, and the keyring
    // kept the USED refresh token. MAS refuses a used one once its successor
    // has been used, so the next start was refused as signed out. The
    // retiring worker now reads the session's final tokens and they are
    // written back under the account captured at release.
    void aRotationTheGuiNeverDrainedIsKeptWhenTheHandleRetires()
    {
        RotatedSession session;
        QVERIFY2(session.start(), "the fixture session did not reach a refresh");
        QVERIFY(session.server->rotations() >= 1);
        // Undrained: the keyring still holds the pair the server used up.
        QCOMPARE(session.stored("refreshToken"), kRefresh0);

        QVERIFY(session.client->detachSession());
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));
        const auto latest = session.server->latest();
        QTRY_COMPARE_WITH_TIMEOUT(session.stored("refreshToken"), latest.second, 5000);
        QCOMPARE(session.stored("accessToken"), latest.first);
        // The record is untouched: same device, same account.
        QCOMPARE(session.settings.accountRecord(kUser)
                     .value(QStringLiteral("deviceId")).toString(), kDevice);
    }

    // What a removal deletes stays deleted (CLAUDE.md §6): the account goes
    // while its retiring handle still holds a rotation nobody wrote down, and
    // that rotation must not bring its sign-in back.
    void aRemovedAccountNeverGetsTheRetiredTokensBack()
    {
        RotatedSession session;
        QVERIFY2(session.start(), "the fixture session did not reach a refresh");
        QVERIFY(session.client->detachSession());
        // Removed before the retiring worker hands anything back.
        QVERIFY(session.settings.clearSessionForAccount(kUser));
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));
        // Anything the worker queued runs here.
        QTest::qWait(500);
        QVERIFY(!session.settings.hasSavedAccount(kUser));
        QCOMPARE(session.stored("accessToken"), QString());
        QCOMPARE(session.stored("refreshToken"), QString());
    }

    // A retirement can land after a newer pair is already stored (one that
    // overran the restore's settle wait, or any later write): the retired
    // pair is older and must not replace it. The write-back is a compare and
    // swap against the pair that handle itself last wrote.
    void aStaleRetiredSignInNeverOverwritesANewerOne()
    {
        RotatedSession session;
        QVERIFY2(session.start(), "the fixture session did not reach a refresh");
        QVERIFY(session.client->detachSession());
        // Stored before the retiring worker hands its pair back.
        const QString newerAccess = QStringLiteral("fixture-access-newer");
        const QString newerRefresh = QStringLiteral("fixture-refresh-newer");
        QVERIFY(session.settings.updateSessionTokens(kUser, newerAccess, newerRefresh));
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));
        QTest::qWait(500);
        QCOMPARE(session.stored("accessToken"), newerAccess);
        QCOMPARE(session.stored("refreshToken"), newerRefresh);
    }

    // A real sign-out leaves no sign-in behind, retirement included. This
    // pins the end state and is NOT a regression test of the write-back
    // guards: it passes on the code before them, and with them removed,
    // because the Rust logout (`mx_rust_logout`, `mx_rust_oauth_logout`)
    // empties the bridge's client slot, so a signed-out handle has no final
    // tokens at all. It guards that slot clearing, and fails only if it goes
    // AND `persist` stops honouring the sign-out. The guards themselves are
    // tested by aRemovedAccountNeverGetsTheRetiredTokensBack.
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

private:
    QTemporaryDir m_home;
};

QTEST_MAIN(RustRetirementTest)
#include "RustRetirementTest.moc"
