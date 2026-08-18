// Opt-in GUI-thread stall tracing (LIGHTNING_GUI_STALL_TRACE).
//
// The 2026-08-18 tester report includes a whole-app freeze right after
// hammering reactions that no harness has reproduced. This repository's
// hard-won rule is to instrument rather than guess (three scroll fixes were
// withdrawn or regressed on disproved premises; the video-poster GUI stall
// was found by a measured 937 ms heartbeat gap, not a theory). This facility
// is the shippable form of that heartbeat: a timer beats on the GUI thread,
// a detached watchdog thread notices when the beats stop for longer than a
// threshold (default 250 ms), and when they resume it logs ONE line with the
// stall duration and a coarse category naming the section that was running.
//
// Privacy: the log line carries a duration and a compile-time category
// literal — never message content, identifiers, file paths, or event data.
//
// Enabling: set LIGHTNING_GUI_STALL_TRACE=1 (threshold 250 ms) or
// LIGHTNING_GUI_STALL_TRACE=<ms> with <ms> >= 50 to override the threshold.
// Disabled (unset/0/invalid) the facility installs nothing and Scope is a
// cheap no-op, so shipping it costs an idle build nothing.
#pragma once

#include <QByteArray>
#include <QtGlobal>

namespace stalltrace {

// True when the environment enables tracing. Stable for the process
// lifetime (read once).
bool enabled();

// Install on the GUI thread (requires a running QCoreApplication). No-op
// when disabled or already installed. thresholdMsOverride < 0 means "from
// the environment"; tests pass an explicit threshold.
void install(int thresholdMsOverride = -1);

// Stop the watchdog and the beat timer. Installed automatically as a child
// of the application object, so an explicit call is only needed by tests.
void shutdown();

// Test/diagnostic accessors: number of stalls detected, the last stall's
// duration and category. Counts only.
int stallCount();
qint64 lastStallMs();
QByteArray lastStallCategory();

// RAII category marker for known-heavy GUI-thread sections. The watchdog
// samples the innermost active category when it detects a stall, so the
// logged line names the section that was running when the thread stopped
// beating. GUI-thread only; the argument must be a string literal (it is
// stored, not copied).
class Scope
{
public:
    explicit Scope(const char *category);
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

private:
    const char *m_previous = nullptr;
    bool m_active = false;
};

} // namespace stalltrace
