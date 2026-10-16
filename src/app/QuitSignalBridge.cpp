#include "app/QuitSignalBridge.h"

#include <QLoggingCategory>
#include <QObject>

#ifdef Q_OS_UNIX
#include <QSocketNotifier>

#include <cerrno>
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

Q_LOGGING_CATEGORY(lcQuitSignals, "lightning.quitsignals")

namespace lightning::quitsignals {

#ifdef Q_OS_UNIX
namespace {

int g_pipe[2] = {-1, -1};
// The signal that started the quit (0: none yet), and when, in whole seconds
// of CLOCK_MONOTONIC.
volatile std::sig_atomic_t g_firstSignal = 0;
volatile std::sig_atomic_t g_firstSignalAt = 0;
// How long a DIFFERENT signal is taken as part of the same event.
constexpr int kForceGraceSeconds = 2;

int monotonicSeconds()
{
    timespec now {};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<int>(now.tv_sec);
}

// Async-signal-safe only: clock_gettime(2), write(2) and _exit(2).
void forwardSignal(int signo)
{
    const int savedErrno = errno;
    if (g_firstSignal != 0) {
        // The same signal again is someone asking twice (a second Ctrl+C, a
        // second kill): end now. A different one arriving with the first is
        // the same event — logind sends SIGTERM and then SIGHUP at once on a
        // logout (SendSIGHUP=yes), and a closing terminal can overlap them —
        // so it forces the exit only once the quit has had its grace period.
        if (signo == g_firstSignal
            || monotonicSeconds() - g_firstSignalAt >= kForceGraceSeconds)
            _exit(128 + signo);
        errno = savedErrno;
        return;
    }
    g_firstSignalAt = monotonicSeconds();
    g_firstSignal = signo;
    const unsigned char byte = static_cast<unsigned char>(signo);
    const ssize_t written = ::write(g_pipe[1], &byte, 1);
    (void)written;
    errno = savedErrno;
}

bool prepareEnd(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL);
    return flags != -1 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1
        && ::fcntl(fd, F_SETFD, FD_CLOEXEC) != -1;
}

} // namespace

bool install(QObject *context, std::function<void(int signo)> onSignal)
{
    if (!context || !onSignal || g_pipe[0] != -1)
        return false;
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, g_pipe) != 0) {
        qCWarning(lcQuitSignals) << "no quit-signal bridge: socketpair failed";
        g_pipe[0] = g_pipe[1] = -1;
        return false;
    }
    if (!prepareEnd(g_pipe[0]) || !prepareEnd(g_pipe[1])) {
        qCWarning(lcQuitSignals) << "no quit-signal bridge: fcntl failed";
        ::close(g_pipe[0]);
        ::close(g_pipe[1]);
        g_pipe[0] = g_pipe[1] = -1;
        return false;
    }

    auto *notifier = new QSocketNotifier(g_pipe[0], QSocketNotifier::Read, context);
    QObject::connect(notifier, &QSocketNotifier::activated, context,
                     [onSignal = std::move(onSignal)] {
                         unsigned char byte = 0;
                         // Drain; one quit is enough however many arrived.
                         int signo = 0;
                         while (::read(g_pipe[0], &byte, 1) == 1)
                             signo = byte;
                         if (signo == 0)
                             return;
                         qCInfo(lcQuitSignals) << "quitting on signal" << signo;
                         onSignal(signo);
                     });

    struct sigaction action {};
    action.sa_handler = forwardSignal;
    // One handler at a time, so a signal cannot land between the check and
    // the record of the first one.
    sigemptyset(&action.sa_mask);
    for (const int signo : {SIGTERM, SIGINT, SIGHUP})
        sigaddset(&action.sa_mask, signo);
    action.sa_flags = SA_RESTART;
    for (const int signo : {SIGTERM, SIGINT, SIGHUP}) {
        struct sigaction previous {};
        if (::sigaction(signo, nullptr, &previous) == 0
            && previous.sa_handler == SIG_IGN)
            continue;
        ::sigaction(signo, &action, nullptr);
    }
    return true;
}
#else
bool install(QObject *, std::function<void(int)>)
{
    return false;
}
#endif

} // namespace lightning::quitsignals
