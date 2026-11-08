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
        Primary,
        Deferred,
        Unresponsive,
        Unguarded,
    };

    static constexpr int kDefaultBudgetMs = 20000;
    static constexpr int kMaxRequestBytes = 1536;
    static constexpr int kMaxTokenBytes = 256;
    static constexpr int kMaxLinkBytes = 1024;

    SingleInstanceGuard(const QString &dataRoot, const QString &appName,
                        QObject *parent = nullptr);
    ~SingleInstanceGuard() override;

    Claim claim(int budgetMs = kDefaultBudgetMs);

    // A matrix: or matrix.to link to hand to the running instance on Deferred.
    void setLinkToOpen(const QString &link) { m_link = link; }

    void stopListening();

    void holdLockUntilProcessExit();

    bool isListening() const;
    QString serverName() const { return m_serverName; }
    QString lockFilePath() const { return m_lockPath; }

    enum class LockProbe {
        Acquired,
        HeldByOther,
        Unusable,
    };

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

    static QString canonicalRoot(const QString &dataRoot);
    static QString serverNameFor(const QString &canonicalRoot,
                                 const QString &appName,
                                 const QString &user,
                                 const QString &socketDir);
    // Where every instance of this packaging can see the socket; empty means
    // Qt's default. Flatpak gives each instance a private /run/user/<uid> and
    // shares only $XDG_RUNTIME_DIR/app/$FLATPAK_ID.
    static QString defaultSocketDir();
    static QString currentUser();
    static bool isAcceptableActivationToken(const QByteArray &token);
    static bool isOpenableLink(const QString &link);
    // "show", "show TOKEN", or "show TOKEN|- LINK"; an older instance reads the
    // last form as an unusable token and still raises its window.
    static bool parseRequest(const QByteArray &line, QString *activationToken,
                             QString *link = nullptr);

Q_SIGNALS:
    void activationRequested(const QString &activationToken,
                             const QString &link);

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
    QString m_link;
    QLocalServer *m_server = nullptr;
    bool m_holdLockUntilProcessExit = false;
#ifdef Q_OS_WIN
    void *m_lockHandle = nullptr;
#else
    int m_lockFd = -1;
#endif
};

}
