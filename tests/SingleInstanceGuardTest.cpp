// One Lightning process per data root (CLAUDE.md §6: two SDK clients on one
// store corrupt it). The in-process cases drive SingleInstanceGuard directly;
// the process cases run the real binary (LIGHTNING_BINARY) against a guard
// this test holds, because main.cpp cannot be linked into a test and only the
// binary can show where the guard sits relative to the preflight flags.
//
// Every guard here keys on a temporary root, and XDG_RUNTIME_DIR points at a
// private directory, so nothing reaches a real profile, session bus, display
// or sound server.
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

// QCoreApplication::applicationName() of a normal launch (src/main.cpp).
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

// A second launch, on its own thread, so the primary on this thread can
// answer through the event loop while that launch blocks in claim().
class Launch
{
public:
    Launch(const QString &root, int budgetMs)
    {
        m_thread.reset(QThread::create([this, root, budgetMs] {
            SingleInstanceGuard guard(root, kApp);
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

} // namespace

class SingleInstanceGuardTest : public QObject
{
    Q_OBJECT

    // Short, so a socket path under it fits in sun_path whatever TMPDIR is.
    std::unique_ptr<QTemporaryDir> m_runtime;
    std::vector<std::unique_ptr<QTemporaryDir>> m_dirs;

    QString freshDir()
    {
        m_dirs.push_back(std::make_unique<QTemporaryDir>());
        return m_dirs.back()->path();
    }
    // The shape of a real root, so the lock sits where the store would.
    QString freshRoot()
    {
        return freshDir() + QStringLiteral("/MatrixClient/matrix-client");
    }

    static QString binary() { return QStringLiteral(LIGHTNING_BINARY); }

    // The root the binary computes for XDG_DATA_HOME=dataHome, asked of the
    // production resolver rather than re-composed here.
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

    // Children never reach the real session: private runtime dir (inherited
    // from this process), no display, no bus, no sound server.
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

    // A whole profile for a full launch: home, config, cache, state, temp.
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
        // Every guard in this process and its children puts its socket here.
        qputenv("XDG_RUNTIME_DIR", QFile::encodeName(m_runtime->path()));
        qunsetenv("FLATPAK_ID");
        qunsetenv("XDG_ACTIVATION_TOKEN");
        // Installed layout, whatever sits beside this binary.
        qputenv("LIGHTNING_PORTABLE", "0");
    }

    void aSecondGuardOnTheSameRootDefersAndDeliversShow()
    {
        const QString root = freshRoot();
        SingleInstanceGuard primary(root, kApp);
        QCOMPARE(primary.claim(1000), Claim::Primary);
        QVERIFY(primary.isListening());
        QSignalSpy shown(&primary, &SingleInstanceGuard::activationRequested);

        // The launcher's activation token travels with the request.
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

        // The screenshot demo renames the application on the same root.
        const QString root = freshRoot();
        SingleInstanceGuard app(root, kApp);
        SingleInstanceGuard demo(root, QStringLiteral("matrix-client-screenshot-demo"));
        QCOMPARE(app.claim(1000), Claim::Primary);
        QCOMPARE(demo.claim(1000), Claim::Primary);
    }

#ifdef Q_OS_UNIX
    // A symlink to the root is the same store, so it must reach the same
    // server; the lock alone would leave it Unresponsive.
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

    // A crash leaves the socket file behind with nothing listening on it.
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

    // The lock decides who owns the root; a live server that is not ours
    // keeps its name.
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

    // A lock that cannot be opened must not blind us to an instance that IS
    // reachable: going straight to Unguarded on Lock::Error, without ever
    // trying the socket, would miss a perfectly healthy running instance and
    // let a second, corrupting client start on the same store.
    void aLockErrorStillTriesTheSocketBeforeGoingUnguarded()
    {
        const QString root = freshRoot();
        SingleInstanceGuard primary(root, kApp);
        QCOMPARE(primary.claim(1000), Claim::Primary);
        QVERIFY(primary.isListening());
        QSignalSpy shown(&primary, &SingleInstanceGuard::activationRequested);

        // Swap the lock file for a directory of the same name: a fresh
        // open(..., O_RDWR) on it fails with EISDIR (Lock::Error). `primary`
        // keeps its already-open fd (and its flock) on the removed inode,
        // none the wiser, and stays reachable on the socket.
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

    // A lock error paired with a socket that CONNECTS but never answers is
    // still proof that a live instance is there: a stale Unix socket refuses
    // the connection outright, and a Windows pipe exists only while a real
    // server is serving it. Only NotListening (nothing answers the connection
    // attempt at all) may run unguarded; a silent connection must be
    // Unresponsive, not Unguarded.
    void aLockErrorWithASilentConnectionIsUnresponsive()
    {
        const QString root = freshRoot();
        SingleInstanceGuard guard(root, kApp);

        // A live server on the guard's name that accepts connections and
        // answers nothing, as a hung process's event loop would look.
        QLocalServer silent;
        QVERIFY(silent.listen(guard.serverName()));
        connect(&silent, &QLocalServer::newConnection, &silent, [&silent] {
            while (silent.nextPendingConnection() != nullptr) {
                // Accepted; deliberately never answered.
            }
        });

        // Force tryLock() to Error the same way as the case above: a
        // directory sits where the lock file would be opened.
        QVERIFY(QDir().mkpath(guard.lockFilePath()));

        QElapsedTimer timer;
        timer.start();
        QCOMPARE(guard.claim(600), Claim::Unresponsive);
        QVERIFY(timer.elapsed() >= 500);
    }
#endif

    // Not a second instance, even when the first one never answers: a hung
    // process holding the store is still holding it.
    void aHungInstanceMakesTheLaunchUnresponsive()
    {
        const QString root = freshRoot();
        SingleInstanceGuard hung(root, kApp);
        QCOMPARE(hung.claim(1000), Claim::Primary);
        QVERIFY(hung.isListening());

        // Same thread: `hung` cannot answer while claim() blocks.
        SingleInstanceGuard second(root, kApp);
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(second.claim(600), Claim::Unresponsive);
        QVERIFY(timer.elapsed() >= 500);
        QVERIFY(!second.isListening());
    }

    // Quitting: the server is closed, the lock is held until exit. The next
    // launch waits for the lock instead of giving up.
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

    // main.cpp's real guard must hold the lock until the process truly
    // exits, not merely until this object is destroyed: RustSdkMatrixClient's
    // shutdown can still be closing the store on a background thread for up
    // to kStoreCloseBudgetMs after a plain destructor would otherwise have
    // let a relaunch in. holdLockUntilProcessExit() switches the destructor
    // to leave the lock held, observable here as the next launch, in the
    // SAME process, still finding it busy after the holder object is gone.
    void holdLockUntilProcessExitKeepsTheLockPastDestruction()
    {
        const QString root = freshRoot();
        auto holder = std::make_unique<SingleInstanceGuard>(root, kApp);
        holder->holdLockUntilProcessExit();
        QCOMPARE(holder->claim(1000), Claim::Primary);
        holder->stopListening();
        holder.reset(); // destroyed, as main()'s local would be at unwind

        SingleInstanceGuard next(root, kApp);
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(next.claim(600), Claim::Unresponsive);
        QVERIFY(timer.elapsed() >= 500);
    }

    // The default budget must comfortably outlast RustSdkMatrixClient's own
    // teardown budget (kStoreCloseBudgetMs, 15000 ms — duplicated here
    // deliberately, since this guard must not depend on the Rust matrix
    // client), or a relaunch during a slow-but-healthy shutdown is told the
    // running instance is unresponsive: exit 5, a Windows MessageBox, over a
    // shutdown that was never actually stuck.
    void theDefaultBudgetOutlastsTheStoreCloseBudget()
    {
        constexpr int kStoreCloseBudgetMs = 15000; // RustSdkMatrixClient.h
        QVERIFY(SingleInstanceGuard::kDefaultBudgetMs > kStoreCloseBudgetMs);
    }

    // --reset-crypto-store's refusal check: a RootLock on a root a running
    // instance already owns must report HeldByOther, never Acquired.
    void rootLockReportsHeldByOtherWhenAnInstanceOwnsTheRoot()
    {
        const QString root = freshRoot();
        SingleInstanceGuard primary(root, kApp);
        QCOMPARE(primary.claim(1000), Claim::Primary);

        SingleInstanceGuard::RootLock probe(root, kApp);
        QCOMPARE(probe.result(), SingleInstanceGuard::LockProbe::HeldByOther);
    }

    // The property --reset-crypto-store's fix depends on: a RootLock that
    // acquired the lock keeps holding it for as long as it lives, so a
    // starting instance waits in claim() rather than seeing the root as
    // free. Probing and releasing before deleting (the pre-fix shape) would
    // leave exactly the window this proves closed.
    void rootLockBlocksAConcurrentClaimUntilReleased()
    {
        const QString root = freshRoot();
        // Unlike SingleInstanceGuard's constructor, RootLock does not create
        // the root: main.cpp only ever constructs one against a root
        // allRoots() already named, which exists whenever anything could be
        // running against it. Create it here so open() has somewhere to
        // land instead of failing with ENOENT (Unusable).
        QVERIFY(QDir().mkpath(root));
        auto held = std::make_unique<SingleInstanceGuard::RootLock>(root, kApp);
        QCOMPARE(held->result(), SingleInstanceGuard::LockProbe::Acquired);

        Launch next(root, 2000);
        QTest::qWait(400);
        QVERIFY(!next.finished());
        held.reset(); // as main.cpp's `rootLock` local would at end of scope

        QTRY_VERIFY_WITH_TIMEOUT(next.finished(), 15000);
        QVERIFY2(Claim(next.result()) == Claim::Primary,
                 claimName(next.result()));
    }

    // A lock file outlives its process; only a live holder counts.
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

    // A client that sends nothing, garbage or an unknown command changes
    // nothing and does not block the next launch.
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

        // Well inside the idle timeout, so only the size bound can close it.
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
        // A bad token is dropped; the request stands.
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

        // Too long for sun_path: a bare name in Qt's temp directory instead.
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
        // Missing shared directory: never the per-instance runtime dir.
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

    // ---- The real binary ------------------------------------------------

    void theBinaryExists()
    {
        QVERIFY2(!binary().isEmpty(), "LIGHTNING_BINARY not defined");
        QVERIFY2(QFileInfo::exists(binary()), qUtf8Printable(binary()));
    }

    // The defect: with an instance running on this profile, a launch used to
    // start a second one on the same store.
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

    // Dev rigs and tests run many instances side by side, one per profile:
    // two real launches on two profiles both own their roots.
    void twoLaunchesOnTwoProfilesBothRun()
    {
#ifndef Q_OS_LINUX
        QSKIP("XDG profile isolation is Linux-only");
#else
        static const QByteArray kOwns("single-instance: this process owns its data root");
        // Before the processes: their destructors may still emit.
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
        // The first must still hold its root when the second claims, or the
        // second proves nothing.
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

    // Packaging validators run these against an installed app that may be
    // open. They exit in preflight and must never meet the guard.
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

    // --reset-crypto-store runs in preflight, before the guard, and used to
    // scan-and-delete every Rust SDK store under the data root
    // unconditionally — including one a running instance still has open.
    void resetCryptoStoreRefusesWhileAnInstanceOwnsTheRoot()
    {
#ifndef Q_OS_LINUX
        QSKIP("XDG profile isolation is Linux-only");
#else
        const QString dataHome = freshDir() + QStringLiteral("/data");
        const QString root = rootForDataHome(dataHome);
        SingleInstanceGuard running(root, kApp);
        QCOMPARE(running.claim(1000), Claim::Primary);

        // A store this run must not touch.
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
