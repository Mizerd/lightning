#pragma once

#include <QtGlobal>

// Bounded wait for another process to exit.
//
// The helper must never touch the installation while Lightning is still
// running, and it must equally never hang forever if the parent refuses to
// die — a stuck updater with a half-open window is worse than a failed
// update. Every entry point here is bounded.

namespace updater {

enum class WaitResult {
    Exited = 0,   // the process is gone
    TimedOut,     // still alive when the bound expired
    InvalidPid,   // pid <= 1, or our own pid
    Error,        // the platform refused to answer
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

// Polls (POSIX) or waits on the process handle (Windows) until the process
// exits or `timeoutMs` elapses. `timeoutMs` is clamped to
// kMaxWaitTimeoutMs. Returns immediately when the process is already gone.
WaitResult waitForProcessExit(qint64 pid, int timeoutMs,
                              int pollIntervalMs = 100);

// Hard ceilings. Nothing in this module can block longer than this.
constexpr int kMaxWaitTimeoutMs = 5 * 60 * 1000;   // 5 minutes
constexpr int kDefaultWaitTimeoutMs = 2 * 60 * 1000; // 2 minutes
constexpr int kMinPollIntervalMs = 10;
constexpr int kMaxPollIntervalMs = 2000;

const char *waitResultName(WaitResult result);

} // namespace updater
