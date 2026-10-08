#pragma once

#include <QObject>
#include <QString>

class QLocalServer;
class QLocalSocket;

namespace lightning {

// One Lightning process per data root. Two matrix-sdk clients on one store
// corrupt it (Olm sessions, one-time keys), and "keep running in the tray"
// made every relaunch a new process.
//
// The authority is a kernel lock on a file inside the data root: flock() on
// Unix, LockFileEx() on Windows. The kernel drops it when the holder dies,
// crash included, and it works across Flatpak's per-instance pid namespaces,
// where QLockFile's pid-based staleness check does not. The local socket only
// carries "show": a launch that finds the lock held asks the holder to raise
// its window and exits. Keyed by the data root, so profiles with different
// XDG/portable roots never collide.
//
// Needs a QCoreApplication (QLocalSocket), so main.cpp claims right after the
// QApplication, after every preflight flag that exits and before any store,
// secret store or client opens.
class SingleInstanceGuard : public QObject
{
    Q_OBJECT
public:
    enum class Claim {
        Primary,      // this process owns the data root
        Deferred,     // another instance owns it and was asked to show itself
        Unresponsive, // another instance owns it and did not answer in time
        Unguarded,    // the lock could not be used at all; run without it
    };

    // How long a second launch waits for the running instance, which may
    // still be starting (its event loop answers only after QML has loaded)
    // or shutting down. RustSdkMatrixClient::kStoreCloseBudgetMs (15000; not
    // referenced directly, this header must not depend on the Rust client)
    // is how long a real teardown may still be closing the store after
    // aboutToQuit, and the running instance keeps the lock for every bit of
    // that — see holdLockUntilProcessExit(). This budget must stay
    // comfortably above it, or a relaunch during a slow-but-healthy shutdown
    // is told the running instance is unresponsive.
    static constexpr int kDefaultBudgetMs = 20000;
    // Longest request line the server reads.
    static constexpr int kMaxRequestBytes = 512;
    // Longest activation token forwarded.
    static constexpr int kMaxTokenBytes = 256;

    SingleInstanceGuard(const QString &dataRoot, const QString &appName,
                        QObject *parent = nullptr);
    ~SingleInstanceGuard() override;

    // Takes the lock and starts serving, or hands off to the holder. Blocks
    // for at most `budgetMs` while another instance holds the lock.
    Claim claim(int budgetMs = kDefaultBudgetMs);

    // Closes the server; a relaunch during teardown then waits for the lock
    // instead of talking to an event loop that has stopped. For aboutToQuit.
    void stopListening();

    // Switches the destructor from releasing the lock (the default, which is
    // what lets a test simulate a quitting instance by destroying its guard)
    // to leaving it held: only the kernel then drops the fd/handle, at real
    // process exit, after every static destructor and background thread
    // (e.g. the Rust SDK's store-close retirement pool, which can outlive
    // main() returning) has had its chance to run. main.cpp calls this once,
    // right after constructing its one real, process-lifetime guard, so the
    // lock is never released early merely because main() unwound.
    void holdLockUntilProcessExit();

    bool isListening() const;
    QString serverName() const { return m_serverName; }
    QString lockFilePath() const { return m_lockPath; }

    // Outcome of a non-blocking attempt to take another data root's lock.
    enum class LockProbe {
        Acquired,    // this RootLock now holds it
        HeldByOther, // a live process holds it; nothing was acquired
        Unusable,    // the lock itself could not be used; fails open
    };

    // RAII probe-and-hold of another data root's lock, usable before a
    // QCoreApplication exists (raw flock()/LockFileEx, no QLocalSocket, so it
    // cannot also fall back to the socket the way claim()'s Lock::Error path
    // can). Releases on destruction unless held until process exit.
    // --reset-crypto-store holds one across
    // its whole scan-and-delete, so a starting instance waits in its own
    // claim() budget instead of opening the store mid-delete; probing and
    // releasing BEFORE deleting would leave exactly that window open.
    class RootLock
    {
    public:
        RootLock(const QString &dataRoot, const QString &appName);
        ~RootLock();
        RootLock(const RootLock &) = delete;
        RootLock &operator=(const RootLock &) = delete;

        LockProbe result() const { return m_result; }
        void holdLockUntilProcessExit() { m_holdLockUntilProcessExit = true; }

    private:
        LockProbe m_result = LockProbe::Unusable;
        bool m_holdLockUntilProcessExit = false;
#ifdef Q_OS_WIN
        void *m_handle = nullptr;
#else
        int m_fd = -1;
#endif
    };

    // Pure helpers, public for the tests.
    // Existing roots resolve symlinks; case folds on Windows and macOS.
    static QString canonicalRoot(const QString &dataRoot);
    // lightning-<32 hex>, under `socketDir` when it is set and short enough
    // for sun_path, otherwise a bare name (Qt then uses the temp directory).
    static QString serverNameFor(const QString &canonicalRoot,
                                 const QString &appName,
                                 const QString &user,
                                 const QString &socketDir);
    // Where every instance of this packaging can see the socket; empty means
    // Qt's default. Flatpak gives each instance a private /run/user/<uid> and
    // shares only $XDG_RUNTIME_DIR/app/$FLATPAK_ID.
    static QString defaultSocketDir();
    static QString currentUser();
    // Printable ASCII without spaces, at most kMaxTokenBytes.
    static bool isAcceptableActivationToken(const QByteArray &token);
    // "show" or "show <token>". An unacceptable token is dropped, not the
    // request. False for anything else.
    static bool parseRequest(const QByteArray &line, QString *activationToken);

Q_SIGNALS:
    // Another launch asked this instance to show its window. The token is the
    // launcher's XDG activation token (Wayland), already validated, or empty.
    void activationRequested(const QString &activationToken);

private:
    enum class Lock { Acquired, Busy, Error };
    enum class Notify { Delivered, NotListening, NoAnswer };

    Lock tryLock();
    void releaseLock();
    Notify notifyRunningInstance(int budgetMs);
    void startServer();
    void onNewConnection();
    void handleClient(QLocalSocket *socket);

    QString m_lockPath;
    QString m_serverName;
    QLocalServer *m_server = nullptr;
    // See holdLockUntilProcessExit(). Default false: the destructor releases
    // the lock, which is what every existing test relies on to simulate a
    // quitting instance by destroying its guard.
    bool m_holdLockUntilProcessExit = false;
#ifdef Q_OS_WIN
    void *m_lockHandle = nullptr;
#else
    int m_lockFd = -1;
#endif
};

} // namespace lightning
