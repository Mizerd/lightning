// Whether the PulseAudio server Qt's PulseAudio backend talks to accepts a
// connection right now, asked of its socket without libpulse (CueEngine's
// reopen gate; see CallSoundMixer.h for why a new stream must wait for it).
//
// The socket is found as libpulse finds it for a local server: $PULSE_SERVER
// (each of its space-separated entries that is a unix socket: "unix:/path",
// "/path", either with a "{machine-id}" prefix), else
// $PULSE_RUNTIME_PATH/native, else $XDG_RUNTIME_DIR/pulse/native. A server
// reached any other way (tcp, a client.conf default-server, an X11 property)
// is never reported reachable: the caller then never reopens, which is silent
// and safe.
//
// In a Flatpak $PULSE_SERVER names the host socket FILE bind-mounted into the
// sandbox, so this asks exactly what Qt's own reconnect asks: a server that
// restarts behind a systemd socket keeps that inode and answers, one that
// creates a new socket never reaches the sandbox, and neither does Qt.
//
// A listening socket is not enough on its own (measured 2026-10-16, Qt
// 6.11.2): Qt retries its connection every 3 s only after a connection that
// FAILED; a retry whose pa_context_connect() fails outright (no socket file
// yet) schedules nothing more, and Qt's PulseAudio backend then stays without
// a connection for the life of the process, so a server that comes back later
// is reachable while every new stream still aborts. qtPulseThreadRunning()
// tells those apart: Qt's PulseAudio mainloop thread ("QPulseAudioEngi") is
// created with its connection and stopped with it (release() stops the
// mainloop whenever it drops the connection, and every failed attempt drops
// both), so outside a reconnect attempt, which runs on the GUI thread like
// the cue engine, it runs exactly while Qt holds a connection: the condition
// QAudioSink::start() needs not to abort. Read from /proc: a Qt that names
// the thread differently makes it false, and nothing is reopened (silent,
// safe).
//
// Header-only and free of Qt Multimedia so it is testable anywhere.
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

/// The unix socket paths a local PulseAudio client would try, in order.
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
        return paths; // set: libpulse tries nothing else
    }
    if (!pulseRuntimePath.isEmpty())
        paths << pulseRuntimePath + "/native";
    else if (!xdgRuntimeDir.isEmpty())
        paths << xdgRuntimeDir + "/pulse/native";
    return paths;
}

/// Whether something listens on the unix socket `path` now. Never blocks: a
/// unix-domain connect completes or fails at once, and a full backlog (a
/// listener that has not accepted yet) still counts as listening.
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

/// Whether a thread of this process is named `name` (Linux; false elsewhere).
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

/// Whether Qt's PulseAudio backend has its mainloop thread (see the top).
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

/// Whether the PulseAudio server this process would use accepts a
/// connection now.
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

} // namespace lightning::calls
