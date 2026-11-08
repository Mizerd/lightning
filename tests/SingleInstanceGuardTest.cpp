#include "app/SingleInstanceGuard.h"
#include "storage/AppDataPaths.h"

#include <QtTest/QtTest>

#include <QElapsedTimer>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <memory>
#include <vector>

#ifdef Q_OS_UNIX
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#ifndef LIGHTNING_BINARY
#define LIGHTNING_BINARY ""
#endif

using lightning::SingleInstanceGuard;
using Claim = SingleInstanceGuard::Claim;

namespace {

const QString kApp = QStringLiteral("matrix-client");

const char *claimName(int claim)
{
    switch (Claim(claim)) {
    case Claim::Primary: return "Primary";
    case Claim::Deferred: return "Deferred";
    case Claim::Unresponsive: return "Unresponsive";
    case Claim::Unguarded: return "Unguarded";
    }
    return "none";
}

class Launch
{
public:
    Launch(const QString &root, int budgetMs, const QString &link = {})
    {
        m_thread.reset(QThread::create([this, root, budgetMs, link] {
            SingleInstanceGuard guard(root, kApp);
            guard.setLinkToOpen(link);
            m_result.store(int(guard.claim(budgetMs)));
        }));
        m_thread->start();
    }
    ~Launch() { m_thread->wait(); }
    bool finished() const { return m_thread->isFinished(); }
    int result() const { return m_result.load(); }

private:
    std::unique_ptr<QThread> m_thread;
    std::atomic<int> m_result{ -1 };
};

}

class SingleInstanceGuardTest : public QObject
{
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> m_runtime;
    std::vector<std::unique_ptr<QTemporaryDir>> m_dirs;

    QString freshDir()
    {
        m_dirs.push_back(std::make_unique<QTemporaryDir>());
        return m_dirs.back()->path();
    }
    QString freshRoot()
    {
        return freshDir() + QStringLiteral("/MatrixClient/matrix-client");
    }

    static QString binary() { return QStringLiteral(LIGHTNING_BINARY); }

    static QString rootForDataHome(const QString &dataHome)
    {
        const QByteArray saved = qgetenv("XDG_DATA_HOME");
        const bool had = qEnvironmentVariableIsSet("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", QFile::encodeName(dataHome));
        const QString root = matrix::app_data::primaryRoot();
        if (had)
            qputenv("XDG_DATA_HOME", saved);
        else
            qunsetenv("XDG_DATA_HOME");
        return root;
    }

    QProcessEnvironment isolatedEnv(const QString &dataHome) const
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        const QString runtime = m_runtime->path();
        env.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime);
        env.insert(QStringLiteral("XDG_DATA_HOME"), dataHome);
        env.insert(QStringLiteral("LIGHTNING_PORTABLE"), QStringLiteral("0"));
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        env.insert(QStringLiteral("QT_FORCE_STDERR_LOGGING"), QStringLiteral("1"));
        env.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
                   QStringLiteral("unix:path=") + runtime + QStringLiteral("/no-bus"));
        env.insert(QStringLiteral("PULSE_SERVER"),
                   QStringLiteral("unix:") + runtime + QStringLiteral("/no-pulse"));
        env.insert(QStringLiteral("PIPEWIRE_RUNTIME_DIR"),
                   runtime + QStringLiteral("/no-pipewire"));
        env.remove(QStringLiteral("QT_LOGGING_RULES"));
        env.remove(QStringLiteral("DISPLAY"));
        env.remove(QStringLiteral("WAYLAND_DISPLAY"));
        env.remove(QStringLiteral("FLATPAK_ID"));
        env.remove(QStringLiteral("XDG_ACTIVATION_TOKEN"));
        return env;
    }

    QProcessEnvironment profileEnv(const QString &profile) const
    {
        QProcessEnvironment env = isolatedEnv(profile + QStringLiteral("/data"));
        for (const char *sub : { "home", "config", "cache", "state", "tmp" })
            QDir().mkpath(profile + QLatin1Char('/') + QLatin1String(sub));
        env.insert(QStringLiteral("HOME"), profile + QStringLiteral("/home"));
        env.insert(QStringLiteral("XDG_CONFIG_HOME"), profile + QStringLiteral("/config"));
        env.insert(QStringLiteral("XDG_CACHE_HOME"), profile + QStringLiteral("/cache"));
        env.insert(QStringLiteral("XDG_STATE_HOME"), profile + QStringLiteral("/state"));
        env.insert(QStringLiteral("TMPDIR"), profile + QStringLiteral("/tmp"));
        return env;
    }

private Q_SLOTS:
    void initTestCase()
    {
#ifdef Q_OS_UNIX
        m_runtime = std::make_unique<QTemporaryDir>(QStringLiteral("/tmp/lt-si-XXXXXX"));
#else
        m_runtime = std::make_unique<QTemporaryDir>();
#endif
        QVERIFY(m_runtime->isValid());
        qputenv("XDG_RUNTIME_DIR", QFile::encodeName(m_runtime->path()));
        qunsetenv("FLATPAK_ID");
        qunsetenv("XDG_ACTIVATION_TOKEN");
        qputenv("LIGHTNING_PORTABLE", "0");
    }

    void aSecondGuardOnTheSameRootDefersAndDeliversShow()
    {
        const QString root = freshRoot();
        SingleInstanceGuard primary(root, kApp);
        QCOMPARE(primary.claim(1000), Claim::Primary);
        QVERIFY(primary.isListening());
        QSignalSpy shown(&primary, &SingleInstanceGuard::activationRequested);

        qputenv("XDG_ACTIVATION_TOKEN", "kwin-42_ab.c");
        Launch second(root, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(second.finished(), 15000);
        qunsetenv("XDG_ACTIVATION_TOKEN");

        QVERIFY2(Claim(second.result()) == Claim::Deferred,
                 claimName(second.result()));
        QTRY_COMPARE(shown.count(), 1);
        QCOMPARE(shown.at(0).at(0).toString(), QStringLiteral("kwin-42_ab.c"));
        QVERIFY(primary.isListening());
    }

    void aDifferentRootDoesNotCollide()
    {
        SingleInstanceGuard a(freshRoot(), kApp);
        SingleInstanceGuard b(freshRoot(), kApp);
        QCOMPARE(a.claim(1000), Claim::Primary);
        QCOMPARE(b.claim(1000), Claim::Primary);
        QVERIFY(a.isListening());
        QVERIFY(b.isListening());
        QVERIFY(a.serverName() != b.serverName());

        const QString root = freshRoot();
        SingleInstanceGuard app(root, kApp);
        SingleInstanceGuard demo(root, QStringLiteral("matrix-client-screenshot-demo"));
        QCOMPARE(app.claim(1000), Claim::Primary);
        QCOMPARE(demo.claim(1000), Claim::Primary);
    }

#ifdef Q_OS_UNIX
    void aSymlinkedRootIsTheSameRoot()
    {
        const QString root = freshRoot();
        QVERIFY(QDir().mkpath(root));
        const QString link = freshDir() + QStringLiteral("/link");
        QVERIFY(QFile::link(root, link));

        SingleInstanceGuard primary(root, kApp);
        QCOMPARE(primary.claim(1000), Claim::Primary);
        QSignalSpy shown(&primary, &SingleInstanceGuard::activationRequested);

        Launch second(link, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(second.finished(), 15000);
        QVERIFY2(Claim(second.result()) == Claim::Deferred,
                 claimName(second.result()));
        QTRY_COMPARE(shown.count(), 1);
    }

    void aStaleSocketIsRecovered()
    {
        const QString root = freshRoot();
        SingleInstanceGuard guard(root, kApp);
        const QByteArray path = QFile::encodeName(guard.serverName());
        QVERIFY2(path.startsWith(QFile::encodeName(m_runtime->path())), path.constData());

        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        QVERIFY(fd >= 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        QVERIFY(size_t(path.size()) < sizeof(addr.sun_path));
        std::memcpy(addr.sun_path, path.constData(), size_t(path.size()));
        QCOMPARE(::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0);
        ::close(fd);
        QVERIFY(QFileInfo::exists(guard.serverName()));

        QCOMPARE(guard.claim(1000), Claim::Primary);
        QVERIFY(guard.isListening());
        QSignalSpy shown(&guard, &SingleInstanceGuard::activationRequested);

        Launch second(root, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(second.finished(), 15000);
        QVERIFY2(Claim(second.result()) == Claim::Deferred,
                 claimName(second.result()));
        QTRY_COMPARE(shown.count(), 1);
    }

    void aLiveServerOnTheNameIsNeverTakenOver()
    {
        const QString root = freshRoot();
        SingleInstanceGuard guard(root, kApp);
        QLocalServer other;
        QVERIFY(other.listen(guard.serverName()));
        connect(&other, &QLocalServer::newConnection, &other, [&other] {
            while (QLocalSocket *s = other.nextPendingConnection())
                s->write("other\n");
        });

        QCOMPARE(guard.claim(1000), Claim::Primary);
        QVERIFY(!guard.isListening());

        QLocalSocket client;
        client.connectToServer(guard.serverName());
        QVERIFY(client.waitForConnected(2000));
        QTRY_VERIFY_WITH_TIMEOUT(client.canReadLine(), 5000);
        QCOMPARE(client.readLine(), QByteArray("other\n"));
    }

    // The app starts browsers and xdg-open; none of them may keep the lock
    // after Lightning exits.
    void aChildProcessDoesNotInheritTheLock()
    {
        const QString root = freshRoot();
        auto holder = std::make_unique<SingleInstanceGuard>(root, kApp);
        QCOMPARE(holder->claim(1000), Claim::Primary);

        QProcess child;
        child.start(QStringLiteral("/bin/sh"), { QStringLiteral("-c"),
                                                 QStringLiteral("read x") });
        QVERIFY(child.waitForStarted(5000));
        holder.reset();

        SingleInstanceGuard next(root, kApp);
        const Claim claim = next.claim(500);
        child.closeWriteChannel();
        child.waitForFinished(5000);
        QVERIFY2(claim == Claim::Primary, claimName(int(claim)));
    }

    void aLockErrorStillTriesTheSocketBeforeGoingUnguarded()
    {
        const QString root = freshRoot();
        SingleInstanceGuard primary(root, kApp);
        QCOMPARE(primary.claim(1000), Claim::Primary);
        QVERIFY(primary.isListening());
        QSignalSpy shown(&primary, &SingleInstanceGuard::activationRequested);

        QVERIFY(QFile::remove(primary.lockFilePath()));
        QVERIFY(QDir().mkpath(primary.lockFilePath()));

        // claim() blocks synchronously, and QLocalSocket's blocking waits do
        // not service sibling sockets on the SAME thread — `primary` could
        // never answer if the erroring claim() ran here too. Claim from its
        // own thread instead, exactly like the sibling tests above.
        Launch second(root, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(second.finished(), 15000);
        QVERIFY2(Claim(second.result()) == Claim::Deferred,
                 claimName(second.result()));
        QTRY_COMPARE(shown.count(), 1);
    }

    void aLockErrorWithASilentConnectionIsUnresponsive()
    {
        const QString root = freshRoot();
        SingleInstanceGuard guard(root, kApp);

        QLocalServer silent;
        QVERIFY(silent.listen(guard.serverName()));
        connect(&silent, &QLocalServer::newConnection, &silent, [&silent] {
            while (silent.nextPendingConnection() != nullptr) {
            }
        });

        QVERIFY(QDir().mkpath(guard.lockFilePath()));

        QElapsedTimer timer;
        timer.start();
        QCOMPARE(guard.claim(600), Claim::Unresponsive);
        QVERIFY(timer.elapsed() >= 500);
    }
#endif

    void aHungInstanceMakesTheLaunchUnresponsive()
    {
        const QString root = freshRoot();
        SingleInstanceGuard hung(root, kApp);
        QCOMPARE(hung.claim(1000), Claim::Primary);
        QVERIFY(hung.isListening());

        SingleInstanceGuard second(root, kApp);
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(second.claim(600), Claim::Unresponsive);
        QVERIFY(timer.elapsed() >= 500);
        QVERIFY(!second.isListening());
    }

    void aQuittingInstanceHandsOverWhenItExits()
    {
        const QString root = freshRoot();
        auto quitting = std::make_unique<SingleInstanceGuard>(root, kApp);
        QCOMPARE(quitting->claim(1000), Claim::Primary);
        quitting->stopListening();
        QVERIFY(!quitting->isListening());

        Launch next(root, 8000);
        QTest::qWait(400);
        QVERIFY(!next.finished());
        quitting.reset();

        QTRY_VERIFY_WITH_TIMEOUT(next.finished(), 15000);
        QVERIFY2(Claim(next.result()) == Claim::Primary,
                 claimName(next.result()));
    }

    void holdLockUntilProcessExitKeepsTheLockPastDestruction()
    {
        const QString root = freshRoot();
        auto holder = std::make_unique<SingleInstanceGuard>(root, kApp);
        holder->holdLockUntilProcessExit();
        QCOMPARE(holder->claim(1000), Claim::Primary);
        holder->stopListening();
        holder.reset();

        SingleInstanceGuard next(root, kApp);
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(next.claim(600), Claim::Unresponsive);
        QVERIFY(timer.elapsed() >= 500);
    }

    void theDefaultBudgetOutlastsTheStoreCloseBudget()
    {
        constexpr int kStoreCloseBudgetMs = 15000;
        QVERIFY(SingleInstanceGuard::kDefaultBudgetMs > kStoreCloseBudgetMs);
    }

    void rootLockReportsHeldByOtherWhenAnInstanceOwnsTheRoot()
    {
        const QString root = freshRoot();
        SingleInstanceGuard primary(root, kApp);
        QCOMPARE(primary.claim(1000), Claim::Primary);

        SingleInstanceGuard::RootLock probe(root, kApp);
        QCOMPARE(probe.result(), SingleInstanceGuard::LockProbe::HeldByOther);
    }

    void rootLockCanRemainHeldPastDestructionUntilProcessExit()
    {
        const QString root = freshRoot();
        QVERIFY(QDir().mkpath(root));
        auto held = std::make_unique<SingleInstanceGuard::RootLock>(root, kApp);
        QCOMPARE(held->result(), SingleInstanceGuard::LockProbe::Acquired);
        held->holdLockUntilProcessExit();
        held.reset();
        SingleInstanceGuard::RootLock next(root, kApp);
        QCOMPARE(next.result(), SingleInstanceGuard::LockProbe::HeldByOther);
    }

    void persistentSmokeCannotOpenAProfileWhoseLockIsHeld()
    {
#ifndef Q_OS_LINUX
        QSKIP("XDG profile isolation is Linux-only");
#else
        const QString dataHome = freshDir();
        const QString root = rootForDataHome(dataHome);
        SingleInstanceGuard primary(root, kApp);
        QCOMPARE(primary.claim(1000), Claim::Primary);
        QProcessEnvironment env = isolatedEnv(dataHome);
        env.insert(QStringLiteral("XDG_CONFIG_HOME"), freshDir());
        env.insert(QStringLiteral("XDG_CACHE_HOME"), freshDir());
        env.insert(QStringLiteral("LIGHTNING_TEST_PERSISTENT_STORE"), QStringLiteral("1"));
        env.insert(QStringLiteral("LIGHTNING_TEST_HOMESERVER"), QStringLiteral("http://127.0.0.1:9"));
        env.insert(QStringLiteral("LIGHTNING_TEST_USER"), QStringLiteral("@fixture:127.0.0.1:9"));
        env.insert(QStringLiteral("LIGHTNING_TEST_PASSWORD"), QStringLiteral("synthetic-fixture-only"));
        QProcess child;
        child.setProcessEnvironment(env);
        child.start(binary(), {QStringLiteral("--backend=rust"), QStringLiteral("--rust-sdk-smoke-test")});
        QVERIFY(child.waitForStarted(10000));
        QVERIFY(child.waitForFinished(10000));
        const QByteArray err = child.readAllStandardError();
        if (err.contains("was not compiled into this build"))
            QSKIP("Rust backend not compiled into this test's application");
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 5);
        QVERIFY2(err.contains("profile lock unavailable or held"), err.constData());
        QCOMPARE(QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
#endif
    }

    void rootLockBlocksAConcurrentClaimUntilReleased()
    {
        const QString root = freshRoot();
        QVERIFY(QDir().mkpath(root));
        auto held = std::make_unique<SingleInstanceGuard::RootLock>(root, kApp);
        QCOMPARE(held->result(), SingleInstanceGuard::LockProbe::Acquired);

        Launch next(root, 2000);
        QTest::qWait(400);
        QVERIFY(!next.finished());
        held.reset();

        QTRY_VERIFY_WITH_TIMEOUT(next.finished(), 15000);
        QVERIFY2(Claim(next.result()) == Claim::Primary,
                 claimName(next.result()));
    }

    void aLeftoverLockFileDoesNotBlock()
    {
        const QString root = freshRoot();
        SingleInstanceGuard guard(root, kApp);
        QFile leftover(guard.lockFilePath());
        QVERIFY(leftover.open(QIODevice::WriteOnly));
        leftover.write("4242\nlightning-matrix\nsomehost\n");
        leftover.close();

        QCOMPARE(guard.claim(1000), Claim::Primary);
    }

    void aBadClientDoesNotBlockTheNextLaunch()
    {
        const QString root = freshRoot();
        SingleInstanceGuard primary(root, kApp);
        QCOMPARE(primary.claim(1000), Claim::Primary);
        QSignalSpy shown(&primary, &SingleInstanceGuard::activationRequested);

        QLocalSocket silent;
        silent.connectToServer(primary.serverName());
        QVERIFY(silent.waitForConnected(2000));

        QLocalSocket flood;
        flood.connectToServer(primary.serverName());
        QVERIFY(flood.waitForConnected(2000));
        flood.write(QByteArray(SingleInstanceGuard::kMaxRequestBytes + 100, 'x'));

        QLocalSocket unknown;
        unknown.connectToServer(primary.serverName());
        QVERIFY(unknown.waitForConnected(2000));
        unknown.write("raise\n");

        QTRY_COMPARE_WITH_TIMEOUT(flood.state(), QLocalSocket::UnconnectedState, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(unknown.state(), QLocalSocket::UnconnectedState, 3000);
        QCOMPARE(shown.count(), 0);

        Launch second(root, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(second.finished(), 15000);
        QVERIFY2(Claim(second.result()) == Claim::Deferred,
                 claimName(second.result()));
        QTRY_COMPARE(shown.count(), 1);
        QCOMPARE(shown.at(0).at(0).toString(), QString());
    }

    void requestsAreParsedStrictly()
    {
        QString token = QStringLiteral("stale");
        QVERIFY(SingleInstanceGuard::parseRequest("show", &token));
        QVERIFY(token.isEmpty());
        QVERIFY(SingleInstanceGuard::parseRequest("show kwin-42", &token));
        QCOMPARE(token, QStringLiteral("kwin-42"));
        QVERIFY(SingleInstanceGuard::parseRequest("show two words", &token));
        QVERIFY(token.isEmpty());
        QVERIFY(SingleInstanceGuard::parseRequest("show tab\there", &token));
        QVERIFY(token.isEmpty());
        QVERIFY(SingleInstanceGuard::parseRequest(
            "show " + QByteArray(SingleInstanceGuard::kMaxTokenBytes + 1, 'a'), &token));
        QVERIFY(token.isEmpty());
        QVERIFY(SingleInstanceGuard::parseRequest(
            "show " + QByteArray(SingleInstanceGuard::kMaxTokenBytes, 'a'), &token));
        QCOMPARE(int(token.size()), SingleInstanceGuard::kMaxTokenBytes);
        QVERIFY(SingleInstanceGuard::parseRequest("show \xc3\xa9", &token));
        QVERIFY(token.isEmpty());

        QVERIFY(!SingleInstanceGuard::parseRequest("", &token));
        QVERIFY(!SingleInstanceGuard::parseRequest("raise", &token));
        QVERIFY(!SingleInstanceGuard::parseRequest("showx", &token));
        QVERIFY(!SingleInstanceGuard::parseRequest("SHOW", &token));
    }

    void aLinkRidesAlongWithShowAndAnOldParserStillRaises()
    {
        QString token, link;
        QVERIFY(SingleInstanceGuard::parseRequest(
            "show kwin-42 matrix:r/room:example.org", &token, &link));
        QCOMPARE(token, QStringLiteral("kwin-42"));
        QCOMPARE(link, QStringLiteral("matrix:r/room:example.org"));
        QVERIFY(SingleInstanceGuard::parseRequest(
            "show - https://matrix.to/#/%23room:example.org", &token, &link));
        QVERIFY(token.isEmpty());
        QCOMPARE(link, QStringLiteral("https://matrix.to/#/%23room:example.org"));
        QVERIFY(SingleInstanceGuard::parseRequest(
            "show - https://evil.example/x", &token, &link));
        QVERIFY(token.isEmpty());
        QVERIFY(link.isEmpty());
        QVERIFY(SingleInstanceGuard::parseRequest("show kwin-42", &token, &link));
        QVERIFY(link.isEmpty());
        // The pre-link parser (no link out-parameter) still accepts the line.
        QVERIFY(SingleInstanceGuard::parseRequest(
            "show - matrix:r/room:example.org", &token));
    }

    void onlyMatrixLinksAreOpenable()
    {
        QVERIFY(SingleInstanceGuard::isOpenableLink(
            QStringLiteral("matrix:roomid/abc:example.org?via=example.org")));
        QVERIFY(SingleInstanceGuard::isOpenableLink(
            QStringLiteral("https://matrix.to/#/!abc:example.org")));
        QVERIFY(!SingleInstanceGuard::isOpenableLink(QStringLiteral("--backend=rust")));
        QVERIFY(!SingleInstanceGuard::isOpenableLink(QStringLiteral("MATRIX:r/a:example.org")));
        QVERIFY(!SingleInstanceGuard::isOpenableLink(QStringLiteral("HTTPS://MATRIX.TO/#/!a:example.org")));
        QVERIFY(!SingleInstanceGuard::isOpenableLink(QStringLiteral("https://example.org/")));
        QVERIFY(!SingleInstanceGuard::isOpenableLink(QStringLiteral("matrix:r/a b")));
        QVERIFY(!SingleInstanceGuard::isOpenableLink(
            QStringLiteral("matrix:r/") + QString(SingleInstanceGuard::kMaxLinkBytes, QLatin1Char('a'))));
    }

    void aSecondLaunchForwardsItsLink()
    {
        const QString root = freshRoot();
        SingleInstanceGuard primary(root, kApp);
        QCOMPARE(primary.claim(1000), Claim::Primary);
        QSignalSpy shown(&primary, &SingleInstanceGuard::activationRequested);

        Launch second(root, 5000, QStringLiteral("matrix:r/room:example.org"));
        QTRY_VERIFY_WITH_TIMEOUT(second.finished(), 15000);
        QVERIFY2(Claim(second.result()) == Claim::Deferred,
                 claimName(second.result()));
        QTRY_COMPARE(shown.count(), 1);
        QCOMPARE(shown.at(0).at(1).toString(),
                 QStringLiteral("matrix:r/room:example.org"));
    }

    void theServerNameIsKeyedByRootAppAndUser()
    {
        const QString dir = QStringLiteral("/run/user/1000");
        const QString base = SingleInstanceGuard::serverNameFor(
            QStringLiteral("/home/a/.local/share/MatrixClient/matrix-client"),
            kApp, QStringLiteral("1000"), dir);
        QCOMPARE(base, SingleInstanceGuard::serverNameFor(
            QStringLiteral("/home/a/.local/share/MatrixClient/matrix-client"),
            kApp, QStringLiteral("1000"), dir));
        QVERIFY(base.startsWith(dir + QStringLiteral("/lightning-")));
        QCOMPARE(int(base.size()), int(dir.size()) + 1 + 10 + 32);

        QVERIFY(base != SingleInstanceGuard::serverNameFor(
            QStringLiteral("/home/b/.local/share/MatrixClient/matrix-client"),
            kApp, QStringLiteral("1000"), dir));
        QVERIFY(base != SingleInstanceGuard::serverNameFor(
            QStringLiteral("/home/a/.local/share/MatrixClient/matrix-client"),
            QStringLiteral("matrix-client-screenshot-demo"), QStringLiteral("1000"), dir));
        QVERIFY(base != SingleInstanceGuard::serverNameFor(
            QStringLiteral("/home/a/.local/share/MatrixClient/matrix-client"),
            kApp, QStringLiteral("1001"), dir));

        const QString longDir = QStringLiteral("/tmp/") + QString(120, QLatin1Char('d'));
        const QString bare = SingleInstanceGuard::serverNameFor(
            QStringLiteral("/r"), kApp, QStringLiteral("1000"), longDir);
        QVERIFY(bare.startsWith(QStringLiteral("lightning-")));
        QVERIFY(!bare.contains(QLatin1Char('/')));
    }

    void flatpakInstancesShareTheAppRuntimeDir()
    {
#ifdef Q_OS_UNIX
        const QString runtime = freshDir();
        const QByteArray savedRuntime = qgetenv("XDG_RUNTIME_DIR");
        qputenv("XDG_RUNTIME_DIR", QFile::encodeName(runtime));
        qputenv("FLATPAK_ID", "org.example.Lightning");
        const QString missing = SingleInstanceGuard::defaultSocketDir();
        QVERIFY(QDir().mkpath(runtime + QStringLiteral("/app/org.example.Lightning")));
        const QString shared = SingleInstanceGuard::defaultSocketDir();
        qunsetenv("FLATPAK_ID");
        const QString native = SingleInstanceGuard::defaultSocketDir();
        qputenv("XDG_RUNTIME_DIR", savedRuntime);

        QVERIFY2(missing.isEmpty(), qUtf8Printable(missing));
        QCOMPARE(shared, runtime + QStringLiteral("/app/org.example.Lightning"));
        QCOMPARE(native, runtime);
#else
        QSKIP("Unix socket directories only");
#endif
    }

    void theBinaryExists()
    {
        QVERIFY2(!binary().isEmpty(), "LIGHTNING_BINARY not defined");
        QVERIFY2(QFileInfo::exists(binary()), qUtf8Printable(binary()));
    }

    void theBinaryHandsOffToTheRunningInstance()
    {
#ifndef Q_OS_LINUX
        QSKIP("XDG profile isolation is Linux-only");
#else
        const QString profile = freshDir();
        const QString root = rootForDataHome(profile + QStringLiteral("/data"));
        SingleInstanceGuard running(root, kApp);
        QCOMPARE(running.claim(1000), Claim::Primary);
        QSignalSpy shown(&running, &SingleInstanceGuard::activationRequested);

        QProcess launch;
        launch.setProcessEnvironment(profileEnv(profile));
        launch.setWorkingDirectory(profile);
        launch.start(binary(), {});
        QVERIFY(launch.waitForStarted(10000));
        QTRY_VERIFY2_WITH_TIMEOUT(launch.state() == QProcess::NotRunning,
                                  "a second instance kept running on the same data root",
                                  60000);
        const QString err = QString::fromUtf8(launch.readAllStandardError());
        QCOMPARE(launch.exitStatus(), QProcess::NormalExit);
        QVERIFY2(launch.exitCode() == 0, qUtf8Printable(err));
        QTRY_COMPARE(shown.count(), 1);
        QVERIFY2(err.contains(QStringLiteral("asked it to show its window")),
                 qUtf8Printable(err));
#endif
    }

    void theBinaryForwardsALinkFromItsArguments()
    {
#ifndef Q_OS_LINUX
        QSKIP("XDG profile isolation is Linux-only");
#else
        const QString profile = freshDir();
        const QString root = rootForDataHome(profile + QStringLiteral("/data"));
        SingleInstanceGuard running(root, kApp);
        QCOMPARE(running.claim(1000), Claim::Primary);
        QSignalSpy shown(&running, &SingleInstanceGuard::activationRequested);

        const QString link = QStringLiteral("https://matrix.to/#/%23room:example.org");
        QProcess launch;
        launch.setProcessEnvironment(profileEnv(profile));
        launch.setWorkingDirectory(profile);
        launch.start(binary(), { link });
        QVERIFY(launch.waitForStarted(10000));
        QTRY_VERIFY_WITH_TIMEOUT(launch.state() == QProcess::NotRunning, 60000);
        QVERIFY2(launch.exitCode() == 0, launch.readAllStandardError().constData());
        QTRY_COMPARE(shown.count(), 1);
        QCOMPARE(shown.at(0).at(1).toString(), link);
#endif
    }

    void twoLaunchesOnTwoProfilesBothRun()
    {
#ifndef Q_OS_LINUX
        QSKIP("XDG profile isolation is Linux-only");
#else
        static const QByteArray kOwns("single-instance: this process owns its data root");
        QByteArray errA, errB;
        QProcess a, b;
        const auto launch = [this](QProcess &p, QByteArray &err) {
            const QString profile = freshDir();
            p.setProcessEnvironment(profileEnv(profile));
            p.setWorkingDirectory(profile);
            connect(&p, &QProcess::readyReadStandardError, &p,
                    [&p, &err] { err += p.readAllStandardError(); });
            p.start(binary(), {});
            return p.waitForStarted(10000);
        };
        QVERIFY(launch(a, errA));
        QTRY_VERIFY2_WITH_TIMEOUT(errA.contains(kOwns) || a.state() == QProcess::NotRunning,
                                  "the first launch never reported owning its root", 60000);
        QVERIFY(launch(b, errB));
        QTRY_VERIFY2_WITH_TIMEOUT(errB.contains(kOwns) || b.state() == QProcess::NotRunning,
                                  "the second launch never reported owning its root", 60000);
        const bool firstStillRunning = a.state() == QProcess::Running;
        a.kill();
        b.kill();
        a.waitForFinished(10000);
        b.waitForFinished(10000);

        QVERIFY2(errA.contains(kOwns), errA.constData());
        QVERIFY2(errB.contains(kOwns), errB.constData());
        QVERIFY2(!errB.contains("asked it to show its window"), errB.constData());
        QVERIFY2(firstStillRunning, errA.constData());
#endif
    }

    void diagnosticFlagsRunWhileAnInstanceOwnsTheRoot()
    {
#ifndef Q_OS_LINUX
        QSKIP("XDG profile isolation is Linux-only");
#else
        const QString dataHome = freshDir() + QStringLiteral("/data");
        SingleInstanceGuard running(rootForDataHome(dataHome), kApp);
        QCOMPARE(running.claim(1000), Claim::Primary);
        QSignalSpy shown(&running, &SingleInstanceGuard::activationRequested);

        const QStringList flags = {
            QStringLiteral("--version"),
            QStringLiteral("--build-info"),
            QStringLiteral("--gif-status"),
            QStringLiteral("--image-format-status"),
            QStringLiteral("--desktop-status"),
            QStringLiteral("--spell-status"),
            QStringLiteral("--spell-selftest"),
            QStringLiteral("--call-media-status"),
        };
        int ran = 0;
        for (const QString &flag : flags) {
            QProcess run;
            run.setProcessEnvironment(isolatedEnv(dataHome));
            run.setWorkingDirectory(freshDir());
            run.start(binary(), { flag });
            QVERIFY2(run.waitForStarted(10000), qUtf8Printable(flag));
            QTRY_VERIFY2_WITH_TIMEOUT(run.state() == QProcess::NotRunning,
                                      qUtf8Printable(flag + QStringLiteral(" did not exit")),
                                      120000);
            const QString out = QString::fromUtf8(run.readAllStandardOutput());
            const QString err = QString::fromUtf8(run.readAllStandardError());
            QVERIFY2(run.exitStatus() == QProcess::NormalExit, qUtf8Printable(flag));
            QVERIFY2(!out.trimmed().isEmpty(),
                     qUtf8Printable(flag + QStringLiteral(" printed nothing: ") + err));
            QVERIFY2(!err.contains(QStringLiteral("single-instance:")),
                     qUtf8Printable(flag + QStringLiteral(" reached the guard: ") + err));
            if (flag == QStringLiteral("--version")) {
                QCOMPARE(run.exitCode(), 0);
                QVERIFY2(out.contains(QStringLiteral("Lightning")), qUtf8Printable(out));
            }
            ++ran;
        }
        QCOMPARE(ran, int(flags.size()));
        QCOMPARE(shown.count(), 0);
#endif
    }

    void resetCryptoStoreRefusesWhileAnInstanceOwnsTheRoot()
    {
#ifndef Q_OS_LINUX
        QSKIP("XDG profile isolation is Linux-only");
#else
        const QString dataHome = freshDir() + QStringLiteral("/data");
        const QString root = rootForDataHome(dataHome);
        SingleInstanceGuard running(root, kApp);
        QCOMPARE(running.claim(1000), Claim::Primary);

        const QString storeDir =
            root + QStringLiteral("/deadbeef/matrix-rust-sdk-store");
        QVERIFY(QDir().mkpath(storeDir));
        QFile sentinel(storeDir + QStringLiteral("/sentinel"));
        QVERIFY(sentinel.open(QIODevice::WriteOnly));
        sentinel.write("keep");
        sentinel.close();

        QProcess run;
        run.setProcessEnvironment(isolatedEnv(dataHome));
        run.setWorkingDirectory(freshDir());
        run.start(binary(), { QStringLiteral("--reset-crypto-store") });
        QVERIFY(run.waitForStarted(10000));
        QTRY_VERIFY2_WITH_TIMEOUT(run.state() == QProcess::NotRunning,
                                  "--reset-crypto-store did not exit", 60000);
        const QString err = QString::fromUtf8(run.readAllStandardError());
        QVERIFY2(run.exitStatus() == QProcess::NormalExit, qUtf8Printable(err));
        QVERIFY2(run.exitCode() != 0, qUtf8Printable(err));
        QVERIFY2(err.contains(QStringLiteral("already running")),
                 qUtf8Printable(err));
        QVERIFY2(QFileInfo::exists(sentinel.fileName()),
                 "the live store was deleted while an instance owned it");
#endif
    }
};

QTEST_GUILESS_MAIN(SingleInstanceGuardTest)
#include "SingleInstanceGuardTest.moc"
