#pragma once

#include <QtGlobal>

namespace updater {

enum class WaitResult {
    Exited = 0,
    TimedOut,
    InvalidPid,
    Error,
};

// Best-effort liveness probe.
//
// POSIX: kill(pid, 0). ESRCH means gone; EPERM means alive but owned by
// another user, which still counts as running. Windows: OpenProcess.
//
// PID REUSE is a real, unavoidable limitation of any pid-based wait: if the
// parent exits and the operating system hands its number to an unrelated
// process before we look, we will keep waiting until the timeout. That is the
// safe direction to fail — we delay rather than install over a live process.
bool processIsRunning(qint64 pid);

WaitResult waitForProcessExit(qint64 pid, int timeoutMs,
                              int pollIntervalMs = 100);

constexpr int kMaxWaitTimeoutMs = 5 * 60 * 1000;
constexpr int kDefaultWaitTimeoutMs = 2 * 60 * 1000;
constexpr int kMinPollIntervalMs = 10;
constexpr int kMaxPollIntervalMs = 2000;

const char *waitResultName(WaitResult result);

}
