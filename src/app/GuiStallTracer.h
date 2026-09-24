// Opt-in GUI-thread stall tracing (LIGHTNING_GUI_STALL_TRACE).
//
// A timer beats on the GUI thread and a watchdog thread notices when beats stop
// for longer than a threshold. When they resume it logs one line with the stall
// duration and the category of the section that was running.
//
// Privacy: only a duration and a compile-time category literal are logged.
//
// Enabling: set LIGHTNING_GUI_STALL_TRACE=1 (threshold 250 ms) or
// LIGHTNING_GUI_STALL_TRACE=<ms> with <ms> >= 50 to override the threshold.
// When disabled nothing is installed and Scope is a cheap no-op.
#pragma once

#include <QByteArray>
#include <QtGlobal>

namespace stalltrace {

// Read once; stable for the process lifetime.
bool enabled();

// Install on the GUI thread (requires a QCoreApplication). No-op when disabled
// or already installed. thresholdMsOverride < 0 reads the environment.
void install(int thresholdMsOverride = -1);

// Stop the watchdog and beat timer. Runs automatically at application
// teardown; tests call it explicitly.
void shutdown();

// Test accessors.
int stallCount();
qint64 lastStallMs();
QByteArray lastStallCategory();

// RAII category marker for heavy GUI-thread sections; the innermost active one
// is logged with a stall. Inert off the GUI thread. The argument must be a
// string literal (it is stored, not copied).
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
