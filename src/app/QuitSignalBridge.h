#pragma once

#include <functional>

class QObject;

namespace lightning::quitsignals {

// SIGTERM, SIGINT and SIGHUP (a desktop logout, `kill`, Ctrl+C in a terminal)
// had their default disposition, so the kernel ended the process at once: no
// aboutToQuit, no sync stop, no store close, exactly like kill -9 for the SDK
// store. This routes them into the event loop instead. The handler only writes
// the signal number to a socket pair (async-signal-safe); a QSocketNotifier
// owned by `context` reads it on the GUI thread and calls `onSignal`, which
// should start the same orderly quit as Ctrl+Q. While that quit runs (it can
// wait up to the store-close budget), the SAME signal again ends the process
// at once with 128 + signal, the shell convention. A different signal does so
// only after a two-second grace: a desktop logout delivers SIGTERM and SIGHUP
// together, and that pair must not cut the orderly quit short.
//
// A signal the process was started with ignored (nohup's SIGHUP) stays
// ignored. Unix only; elsewhere this returns false and installs nothing.
// Installs once per process; a later call returns false.
bool install(QObject *context, std::function<void(int signo)> onSignal);

} // namespace lightning::quitsignals
