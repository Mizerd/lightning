#include "app/SingleInstanceGuard.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QThread>
#include <QTimer>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace lightning {

namespace {

constexpr int kClientTimeoutMs = 5000;
constexpr int kMaxSocketPathBytes = 100;

QString fileSafe(const QString &name)
{
    QString out;
    out.reserve(name.size());
    for (const QChar c : name) {
        const bool ok = (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
                     || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
                     || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                     || c == QLatin1Char('-') || c == QLatin1Char('_')
                     || c == QLatin1Char('.');
        out += ok ? c : QLatin1Char('_');
    }
    return out;
}

}

SingleInstanceGuard::SingleInstanceGuard(const QString &dataRoot,
                                         const QString &appName,
                                         QObject *parent)
    : QObject(parent)
{
    if (dataRoot.isEmpty())
        return;
    if (!QFileInfo::exists(dataRoot) && QDir().mkpath(dataRoot)) {
        QFile::setPermissions(dataRoot, QFileDevice::ReadOwner
                                            | QFileDevice::WriteOwner
                                            | QFileDevice::ExeOwner);
    }
    m_lockPath = QDir(dataRoot).filePath(
        QStringLiteral("instance-%1.lock").arg(fileSafe(appName)));
    m_serverName = serverNameFor(canonicalRoot(dataRoot), appName,
                                 currentUser(), defaultSocketDir());
}

SingleInstanceGuard::~SingleInstanceGuard()
{
    stopListening();
    if (!m_holdLockUntilProcessExit)
        releaseLock();
}

void SingleInstanceGuard::holdLockUntilProcessExit()
{
    m_holdLockUntilProcessExit = true;
}

SingleInstanceGuard::Claim SingleInstanceGuard::claim(int budgetMs)
{
    if (m_lockPath.isEmpty()) {
        qWarning("single-instance: no data root; running without the guard");
        return Claim::Unguarded;
    }
#ifdef Q_OS_WIN
    if (m_lockHandle)
        return Claim::Primary;
#else
    if (m_lockFd >= 0)
        return Claim::Primary;
#endif

    QElapsedTimer timer;
    timer.start();
    for (;;) {
        switch (tryLock()) {
        case Lock::Acquired:
            startServer();
            qInfo("single-instance: this process owns its data root");
            return Claim::Primary;
        case Lock::Error: {
            const qint64 leftForNotify = budgetMs - timer.elapsed();
            const int notifyBudget = leftForNotify > 0 ? int(leftForNotify) : 0;
            switch (notifyRunningInstance(notifyBudget)) {
            case Notify::Delivered:
                qInfo("single-instance: another instance owns this data root; "
                      "asked it to show its window");
                return Claim::Deferred;
            case Notify::NoAnswer:
                qWarning("single-instance: another instance owns this data root "
                         "and did not answer");
                return Claim::Unresponsive;
            case Notify::NotListening:
                break;
            }
            qWarning("single-instance: the instance lock is unusable; "
                     "running without the guard");
            return Claim::Unguarded;
        }
        case Lock::Busy:
            break;
        }

        const qint64 left = budgetMs - timer.elapsed();
        if (left <= 0)
            break;
        switch (notifyRunningInstance(int(left))) {
        case Notify::Delivered:
            qInfo("single-instance: another instance owns this data root; "
                  "asked it to show its window");
            return Claim::Deferred;
        case Notify::NoAnswer:
            qWarning("single-instance: another instance owns this data root "
                     "and did not answer");
            return Claim::Unresponsive;
        case Notify::NotListening:
            break;
        }
        const qint64 rest = budgetMs - timer.elapsed();
        if (rest <= 0)
            break;
        QThread::msleep(quint64(qMin<qint64>(100, rest)));
    }
    qWarning("single-instance: another instance owns this data root and did "
             "not answer");
    return Claim::Unresponsive;
}

void SingleInstanceGuard::stopListening()
{
    if (!m_server)
        return;
    m_server->close();
    delete m_server;
    m_server = nullptr;
}

bool SingleInstanceGuard::isListening() const
{
    return m_server && m_server->isListening();
}

QString SingleInstanceGuard::canonicalRoot(const QString &dataRoot)
{
    if (dataRoot.isEmpty())
        return {};
    QString root = QFileInfo(dataRoot).canonicalFilePath();
    if (root.isEmpty())
        root = QDir::cleanPath(QFileInfo(dataRoot).absoluteFilePath());
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    root = root.toLower();
#endif
    return root;
}

QString SingleInstanceGuard::serverNameFor(const QString &canonicalRoot,
                                           const QString &appName,
                                           const QString &user,
                                           const QString &socketDir)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(canonicalRoot.toUtf8());
    hash.addData(QByteArrayView("\n"));
    hash.addData(appName.toUtf8());
    hash.addData(QByteArrayView("\n"));
    hash.addData(user.toUtf8());
    const QString name = QStringLiteral("lightning-")
        + QString::fromLatin1(hash.result().toHex().left(32));
    if (!socketDir.isEmpty()) {
        const QString full = QDir(socketDir).filePath(name);
        if (QFile::encodeName(full).size() < kMaxSocketPathBytes)
            return full;
    }
    return name;
}

QString SingleInstanceGuard::defaultSocketDir()
{
#ifdef Q_OS_WIN
    return {};
#else
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty())
        return {};
    const QString flatpakId = qEnvironmentVariable("FLATPAK_ID");
    if (!flatpakId.isEmpty()) {
        const QString shared =
            runtime + QStringLiteral("/app/") + flatpakId;
        return QFileInfo(shared).isDir() ? shared : QString();
    }
    return QFileInfo(runtime).isDir() ? runtime : QString();
#endif
}

QString SingleInstanceGuard::currentUser()
{
#ifdef Q_OS_WIN
    return qEnvironmentVariable("USERDOMAIN") + QLatin1Char('\\')
         + qEnvironmentVariable("USERNAME");
#else
    return QString::number(::getuid());
#endif
}

bool SingleInstanceGuard::isAcceptableActivationToken(const QByteArray &token)
{
    if (token.isEmpty() || token.size() > kMaxTokenBytes)
        return false;
    for (const char c : token) {
        if (c < 0x21 || c > 0x7e)
            return false;
    }
    return true;
}

bool SingleInstanceGuard::isOpenableLink(const QString &link)
{
    if (link.isEmpty() || link.size() > kMaxLinkBytes)
        return false;
    for (const QChar c : link) {
        if (c.unicode() < 0x21 || c.unicode() > 0x7e)
            return false;
    }
    return link.startsWith(QLatin1String("matrix:"))
        || link.startsWith(QLatin1String("https://matrix.to/#/"));
}

bool SingleInstanceGuard::parseRequest(const QByteArray &line,
                                       QString *activationToken,
                                       QString *link)
{
    activationToken->clear();
    if (link)
        link->clear();
    if (line == "show")
        return true;
    if (!line.startsWith("show "))
        return false;
    QByteArray token = line.mid(5);
    const QList<QByteArray> parts = token.split(' ');
    if (parts.size() == 2 && isOpenableLink(QString::fromLatin1(parts.at(1)))) {
        if (link)
            *link = QString::fromLatin1(parts.at(1));
        token = parts.at(0) == "-" ? QByteArray() : parts.at(0);
    }
    if (isAcceptableActivationToken(token))
        *activationToken = QString::fromLatin1(token);
    return true;
}

SingleInstanceGuard::Lock SingleInstanceGuard::tryLock()
{
#ifdef Q_OS_WIN
    const QString native = QDir::toNativeSeparators(m_lockPath);
    HANDLE handle = CreateFileW(reinterpret_cast<const wchar_t *>(native.utf16()),
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        qWarning("single-instance: cannot open the instance lock (error %lu)",
                 GetLastError());
        return Lock::Error;
    }
    OVERLAPPED overlapped = {};
    if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                   0, 1, 0, &overlapped)) {
        m_lockHandle = handle;
        return Lock::Acquired;
    }
    const DWORD error = GetLastError();
    CloseHandle(handle);
    if (error == ERROR_LOCK_VIOLATION)
        return Lock::Busy;
    qWarning("single-instance: cannot take the instance lock (error %lu)", error);
    return Lock::Error;
#else
    const QByteArray path = QFile::encodeName(m_lockPath);
    const int fd = ::open(path.constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        qWarning("single-instance: cannot open the instance lock: %s",
                 std::strerror(errno));
        return Lock::Error;
    }
    // flock, not fcntl: fcntl locks never conflict within one process and
    // are dropped when any descriptor of the file closes.
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
        m_lockFd = fd;
        return Lock::Acquired;
    }
    const int error = errno;
    ::close(fd);
    if (error == EWOULDBLOCK)
        return Lock::Busy;
    qWarning("single-instance: cannot take the instance lock: %s",
             std::strerror(error));
    return Lock::Error;
#endif
}

SingleInstanceGuard::RootLock::RootLock(const QString &dataRoot,
                                        const QString &appName)
{
    if (dataRoot.isEmpty()) {
        m_result = LockProbe::Unusable;
        return;
    }
    const QString lockPath = QDir(dataRoot).filePath(
        QStringLiteral("instance-%1.lock").arg(fileSafe(appName)));
#ifdef Q_OS_WIN
    const QString native = QDir::toNativeSeparators(lockPath);
    HANDLE handle = CreateFileW(reinterpret_cast<const wchar_t *>(native.utf16()),
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        m_result = LockProbe::Unusable;
        return;
    }
    OVERLAPPED overlapped = {};
    if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                   0, 1, 0, &overlapped)) {
        m_handle = handle;
        m_result = LockProbe::Acquired;
        return;
    }
    const DWORD error = GetLastError();
    CloseHandle(handle);
    m_result = error == ERROR_LOCK_VIOLATION ? LockProbe::HeldByOther
                                              : LockProbe::Unusable;
#else
    const QByteArray path = QFile::encodeName(lockPath);
    const int fd = ::open(path.constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        m_result = LockProbe::Unusable;
        return;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
        m_fd = fd;
        m_result = LockProbe::Acquired;
        return;
    }
    const int error = errno;
    ::close(fd);
    m_result = error == EWOULDBLOCK ? LockProbe::HeldByOther
                                     : LockProbe::Unusable;
#endif
}

SingleInstanceGuard::RootLock::~RootLock()
{
    if (m_holdLockUntilProcessExit)
        return;
#ifdef Q_OS_WIN
    if (m_handle)
        CloseHandle(static_cast<HANDLE>(m_handle));
#else
    if (m_fd >= 0)
        ::close(m_fd);
#endif
}

void SingleInstanceGuard::releaseLock()
{
    // The file stays: unlinking a lock file lets two processes lock two
    // different files of the same name.
#ifdef Q_OS_WIN
    if (m_lockHandle) {
        CloseHandle(static_cast<HANDLE>(m_lockHandle));
        m_lockHandle = nullptr;
    }
#else
    if (m_lockFd >= 0) {
        ::close(m_lockFd);
        m_lockFd = -1;
    }
#endif
}

SingleInstanceGuard::Notify SingleInstanceGuard::notifyRunningInstance(int budgetMs)
{
    QElapsedTimer timer;
    timer.start();
    QLocalSocket socket;
    socket.connectToServer(m_serverName);
    if (!socket.waitForConnected(qMin(budgetMs, 1000)))
        return Notify::NotListening;

#ifdef Q_OS_WIN
    AllowSetForegroundWindow(ASFW_ANY);
#endif

    QByteArray request("show");
    const QByteArray token = qgetenv("XDG_ACTIVATION_TOKEN");
    const bool hasToken = isAcceptableActivationToken(token);
    if (isOpenableLink(m_link))
        request += ' ' + (hasToken ? token : QByteArray("-")) + ' '
            + m_link.toLatin1();
    else if (hasToken)
        request += ' ' + token;
    request += '\n';
    socket.write(request);
    socket.flush();

    QByteArray reply;
    while (!reply.contains('\n')) {
        const qint64 left = budgetMs - timer.elapsed();
        if (left <= 0)
            return Notify::NoAnswer;
        if (!socket.waitForReadyRead(int(left))) {
            reply += socket.readAll();
            if (reply.contains('\n'))
                break;
            return socket.state() == QLocalSocket::ConnectedState
                ? Notify::NoAnswer : Notify::NotListening;
        }
        reply += socket.readAll();
        if (reply.size() > 64)
            return Notify::NoAnswer;
    }
    return reply.startsWith("ok\n") ? Notify::Delivered : Notify::NoAnswer;
}

void SingleInstanceGuard::startServer()
{
    {
        QLocalSocket probe;
        probe.connectToServer(m_serverName);
        if (probe.waitForConnected(500)) {
            probe.abort();
            qWarning("single-instance: another process answers on this "
                     "instance's name; not accepting activations");
            return;
        }
    }
    auto *server = new QLocalServer(this);
    server->setSocketOptions(QLocalServer::UserAccessOption);
    bool listening = server->listen(m_serverName);
    if (!listening
        && server->serverError() == QAbstractSocket::AddressInUseError) {
        QLocalServer::removeServer(m_serverName);
        listening = server->listen(m_serverName);
    }
    if (!listening) {
        qWarning("single-instance: cannot accept activations: %s",
                 qUtf8Printable(server->errorString()));
        delete server;
        return;
    }
    m_server = server;
    connect(m_server, &QLocalServer::newConnection,
            this, &SingleInstanceGuard::onNewConnection);
}

void SingleInstanceGuard::onNewConnection()
{
    while (QLocalSocket *socket = m_server->nextPendingConnection()) {
        connect(socket, &QLocalSocket::disconnected,
                socket, &QObject::deleteLater);
        connect(socket, &QLocalSocket::readyRead,
                this, [this, socket] { handleClient(socket); });
        QTimer::singleShot(kClientTimeoutMs, socket, [socket] {
            socket->abort();
            socket->deleteLater();
        });
        if (socket->bytesAvailable() > 0)
            handleClient(socket);
    }
}

void SingleInstanceGuard::handleClient(QLocalSocket *socket)
{
    if (!socket->canReadLine()) {
        if (socket->bytesAvailable() > kMaxRequestBytes) {
            socket->abort();
            socket->deleteLater();
        }
        return;
    }
    disconnect(socket, &QLocalSocket::readyRead, this, nullptr);
    const QByteArray line = socket->readLine(kMaxRequestBytes + 1).trimmed();
    QString token;
    QString link;
    if (!parseRequest(line, &token, &link)) {
        socket->abort();
        socket->deleteLater();
        return;
    }
    socket->write("ok\n");
    socket->flush();
    socket->disconnectFromServer();
    Q_EMIT activationRequested(token, link);
}

}
