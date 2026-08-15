#include "updater/ProcessWaiter.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QThread>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace updater {
namespace {

class Sleeper : public QThread
{
public:
    using QThread::msleep;
};

int clampTimeout(int timeoutMs)
{
    if (timeoutMs < 0)
        return 0;
    return qMin(timeoutMs, kMaxWaitTimeoutMs);
}

int clampPoll(int pollIntervalMs)
{
    return qBound(kMinPollIntervalMs, pollIntervalMs, kMaxPollIntervalMs);
}

} // namespace

const char *waitResultName(WaitResult result)
{
    switch (result) {
    case WaitResult::Exited: return "exited";
    case WaitResult::TimedOut: return "timed-out";
    case WaitResult::InvalidPid: return "invalid-pid";
    case WaitResult::Error: return "error";
    }
    return "unknown";
}

bool processIsRunning(qint64 pid)
{
    if (pid <= 1)
        return false;

#ifdef Q_OS_WIN
    const HANDLE handle = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                        FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr) {
        const DWORD err = ::GetLastError();
        // ERROR_INVALID_PARAMETER is what Windows reports for a pid that no
        // longer exists. ERROR_ACCESS_DENIED means it exists but we may not
        // open it — that still counts as running.
        return err == ERROR_ACCESS_DENIED;
    }
    DWORD exitCode = 0;
    const bool haveCode = ::GetExitCodeProcess(handle, &exitCode) != 0;
    ::CloseHandle(handle);
    if (!haveCode)
        return true; // cannot prove it is gone; assume alive
    return exitCode == STILL_ACTIVE;
#else
    if (::kill(static_cast<pid_t>(pid), 0) == 0)
        return true;
    if (errno == EPERM)
        return true; // alive, different owner
    return false;    // ESRCH (or anything else we cannot interpret as alive)
#endif
}

WaitResult waitForProcessExit(qint64 pid, int timeoutMs, int pollIntervalMs)
{
    if (pid <= 1)
        return WaitResult::InvalidPid;
    if (pid == QCoreApplication::applicationPid())
        return WaitResult::InvalidPid;

    const int bound = clampTimeout(timeoutMs);
    const int poll = clampPoll(pollIntervalMs);

#ifdef Q_OS_WIN
    const HANDLE handle = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            // We cannot wait on it, so degrade to the bounded poll below.
            QDeadlineTimer deadline(bound);
            while (!deadline.hasExpired()) {
                if (!processIsRunning(pid))
                    return WaitResult::Exited;
                Sleeper::msleep(static_cast<unsigned long>(poll));
            }
            return processIsRunning(pid) ? WaitResult::TimedOut : WaitResult::Exited;
        }
        // Anything else (notably ERROR_INVALID_PARAMETER) means it is gone.
        return WaitResult::Exited;
    }
    const DWORD waited = ::WaitForSingleObject(handle, static_cast<DWORD>(bound));
    ::CloseHandle(handle);
    if (waited == WAIT_OBJECT_0)
        return WaitResult::Exited;
    if (waited == WAIT_TIMEOUT)
        return WaitResult::TimedOut;
    return WaitResult::Error;
#else
    QDeadlineTimer deadline(bound);
    for (;;) {
        if (!processIsRunning(pid))
            return WaitResult::Exited;
        if (deadline.hasExpired())
            return WaitResult::TimedOut;
        const qint64 remaining = deadline.remainingTime();
        const unsigned long nap =
            static_cast<unsigned long>(remaining < 0 ? poll
                                                     : qMin<qint64>(poll, remaining));
        Sleeper::msleep(nap == 0 ? 1 : nap);
    }
#endif
}

} // namespace updater
