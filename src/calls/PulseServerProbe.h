#pragma once

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QList>
#include <QtGlobal>

#include <functional>
#include <utility>

#ifdef Q_OS_LINUX
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace lightning::calls {

inline QList<QByteArray> pulseServerSockets(const QByteArray &pulseServer,
                                            const QByteArray &pulseRuntimePath,
                                            const QByteArray &xdgRuntimeDir)
{
    QList<QByteArray> paths;
    if (!pulseServer.trimmed().isEmpty()) {
        for (QByteArray entry : pulseServer.simplified().split(' ')) {
            if (entry.startsWith('{')) {
                const qsizetype close = entry.indexOf('}');
                entry = close < 0 ? QByteArray() : entry.mid(close + 1);
            }
            if (entry.startsWith("unix:"))
                entry = entry.mid(5);
            if (entry.startsWith('/'))
                paths << entry;
        }
        return paths;
    }
    if (!pulseRuntimePath.isEmpty())
        paths << pulseRuntimePath + "/native";
    else if (!xdgRuntimeDir.isEmpty())
        paths << xdgRuntimeDir + "/pulse/native";
    return paths;
}

inline bool unixSocketListening(const QByteArray &path)
{
#ifdef Q_OS_LINUX
    sockaddr_un address{};
    if (path.isEmpty() || path.size() >= qsizetype(sizeof address.sun_path))
        return false;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.constData(), size_t(path.size()));
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    const int rc = ::connect(fd, reinterpret_cast<const sockaddr *>(&address),
                             socklen_t(sizeof address));
    const bool listening = rc == 0 || errno == EAGAIN || errno == EINPROGRESS;
    ::close(fd);
    return listening;
#else
    Q_UNUSED(path);
    return false;
#endif
}

inline bool threadNamedRunning(const QByteArray &name)
{
#ifdef Q_OS_LINUX
    const QStringList tasks =
        QDir(QStringLiteral("/proc/self/task")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &task : tasks) {
        QFile comm(QStringLiteral("/proc/self/task/%1/comm").arg(task));
        if (comm.open(QIODevice::ReadOnly) && comm.readAll().trimmed() == name)
            return true;
    }
#else
    Q_UNUSED(name);
#endif
    return false;
}

inline bool qtPulseThreadRunning()
{
    return threadNamedRunning(QByteArrayLiteral("QPulseAudioEngi"));
}

/// The cue engine's gate on Qt's PulseAudio backend: Qt's connection FIRST
/// and the server's socket only then, so a backend that gave up (Qt does not
/// reconnect after a failed attempt, see the top) never costs a connection to
/// the server, and a socket-activated server stopped on purpose is not
/// started again by us. Says once, through `sayGone`, when Qt's connection
/// has stayed absent for `sayAfterMs` of asking.
class PulseGate
{
public:
    using Probe = std::function<bool()>;
    PulseGate(Probe qtConnected, Probe socketListening, std::function<qint64()> nowMs,
              std::function<void()> sayGone, qint64 sayAfterMs = 30000)
        : m_qtConnected(std::move(qtConnected))
        , m_socketListening(std::move(socketListening))
        , m_nowMs(std::move(nowMs))
        , m_sayGone(std::move(sayGone))
        , m_sayAfterMs(sayAfterMs)
    {
    }

    bool operator()()
    {
        if (!m_qtConnected()) {
            const qint64 now = m_nowMs();
            if (m_qtAbsentSinceMs < 0)
                m_qtAbsentSinceMs = now;
            if (!m_said && now - m_qtAbsentSinceMs >= m_sayAfterMs) {
                m_said = true;
                if (m_sayGone)
                    m_sayGone();
            }
            return false;
        }
        m_qtAbsentSinceMs = -1;
        return m_socketListening();
    }

private:
    Probe m_qtConnected;
    Probe m_socketListening;
    std::function<qint64()> m_nowMs;
    std::function<void()> m_sayGone;
    qint64 m_sayAfterMs;
    qint64 m_qtAbsentSinceMs = -1;
    bool m_said = false;
};

inline bool pulseServerReachable()
{
    const QList<QByteArray> paths =
        pulseServerSockets(qgetenv("PULSE_SERVER"), qgetenv("PULSE_RUNTIME_PATH"),
                           qgetenv("XDG_RUNTIME_DIR"));
    for (const QByteArray &path : paths) {
        if (unixSocketListening(path))
            return true;
    }
    return false;
}

}
