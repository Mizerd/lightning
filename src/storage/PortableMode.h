#pragma once

#include <QString>

// Portable-installation mode — the single authority for "is this copy of
// Lightning self-contained?".
//
// ─────────────────────────────────────────────────────────────────────────
//  What portable means here
// ─────────────────────────────────────────────────────────────────────────
// A portable installation keeps EVERYTHING it owns inside one directory tree
// beside the executable: settings, the SDK/crypto store, caches, logs and the
// saved session. The user extracts the ZIP, signs in, closes Lightning, copies
// that one directory to a different path on a different PC, and continues with
// the same Matrix session and the SAME Matrix device — no registry, no
// %LOCALAPPDATA%, no Credential Manager, no second login.
//
// The signal is a POSITIVE marker file (`portable.marker`) that the packaging
// pipeline writes into the portable ZIP and into no other package. It is not
// inferred from the install-type build define, from a writability probe, or
// from where the executable happens to live: all three of those are guesses,
// and guessing wrong in either direction is a data-location bug that surfaces
// as "my account vanished".
//
// ─────────────────────────────────────────────────────────────────────────
//  Why there is no #ifdef Q_OS_WIN around the POLICY
// ─────────────────────────────────────────────────────────────────────────
// Only executableDir() is platform-specific. Everything else — the marker
// lookup, the caching, the data-root composition — is platform-neutral, which
// is what makes the whole mechanism testable on Linux. CI never runs Windows,
// so a Windows-only policy would be a policy nothing ever exercises.
//
// ─────────────────────────────────────────────────────────────────────────
//  The ordering constraint (do not re-derive this)
// ─────────────────────────────────────────────────────────────────────────
// The decision has to be made in src/main.cpp between setApplicationName() and
// the FIRST default-constructed QSettings, because on Windows that QSettings
// is registry-backed and there is no undoing it afterwards. At that point
// there is no QCoreApplication instance, so QCoreApplication::applicationDirPath()
// is unavailable — hence executableDir() goes to the platform API directly.
// argv[0] is NOT acceptable: it is launcher-controlled and is wrong whenever
// the binary is found via PATH.
//
// Nothing here logs a resolved path at info level: a data root under a user
// profile carries the account's identity in the path.
namespace lightning::portable {

// The marker the packaging pipeline puts in the portable ZIP and in no other
// package. Its presence beside the executable is the ONLY signal.
inline constexpr char kMarkerFileName[] = "portable.marker";

// The single portable data directory name, hoisted so the UPDATER can name it
// without linking this translation unit. lightning-updater is a separate
// Qt6::Core-only executable, and it must exclude this directory from the
// install-directory swap or an in-app update destroys the user's session and
// crypto store — see swapDirectory's `preserveNames` note.
inline constexpr char kDataDirName[] = "data";

// Development-only override, read by isPortable(). "1"/"true"/"on" forces
// portable mode using the real executable directory; "0"/"false"/"off" forces
// installed mode. Anything else (including unset) leaves the marker in charge.
//
// This is an ENVIRONMENT variable rather than a --portable command-line flag
// on purpose: the flag would have to be parsed inside preflightParse(), whose
// loop already ACTS on options as it walks argv (--reset-crypto-store deletes
// store directories mid-loop), so `--reset-crypto-store --portable` and
// `--portable --reset-crypto-store` would target different roots. An
// environment variable is order-free and is equally available to the preflight
// paths that run before any parsing at all.
inline constexpr char kPortableEnvVar[] = "LIGHTNING_PORTABLE";

// Directory containing the running executable, resolved WITHOUT Qt's
// application instance (GetModuleFileNameW / /proc/self/exe /
// _NSGetExecutablePath). Uses '/' separators, no trailing slash. Empty on
// failure — callers must treat empty as "not portable", never as "cwd".
//
// While a test override is installed this returns the injected directory, so
// dataRoot() composes from it.
QString executableDir();

// True when this installation is portable. Decided ONCE per process on the
// first call and cached, so no two callers can observe different answers and
// so a marker file appearing (or being deleted) mid-run cannot move a running
// process's storage out from under it.
bool isPortable();

// <executableDir>/data — the one portable data root. Empty when not portable.
// NEVER falls back to %LOCALAPPDATA%/$XDG_DATA_HOME: a portable app that
// quietly starts writing to AppData is the exact defect this module exists to
// fix, and a silent fallback would hide it.
QString dataRoot();

// The four directories the portable tree is made of, each empty when not
// portable. They are named here rather than concatenated at call sites so the
// literals cannot drift between the code that writes and the code that cleans
// up (the same reasoning as matrix::app_data::starredGifsDir).
//   config  — QSettings INI files (see main.cpp's QSettings::setPath)
//   secrets — PortableSecretStore's sealed document + key
//   cache   — everything QStandardPaths::CacheLocation would have held
//   logs    — reserved for opt-in file logging
// The app-data root itself is matrix::app_data::primaryRoot(), which resolves
// to <dataRoot>/matrix in portable mode.
QString configDir();
QString secretsDir();
QString cacheDir();
QString logsDir();

// PURE helpers, for tests and for anyone who needs the decision over an
// injected directory rather than the running process's own.
bool markerPresentIn(const QString &dir);
QString dataRootFor(const QString &executableDir);

// Test seam: force the answer for one process, and inject the directory
// executableDir() reports. Used ONLY by tests.
//
// The override sits IN FRONT of the cached real decision rather than seeding
// it, so clearPortableOverrideForTest() genuinely restores the real answer
// instead of leaving a test's fiction latched for the rest of the process.
void setPortableOverrideForTest(bool portable, const QString &executableDir);
void clearPortableOverrideForTest();

// Create the portable tree and PROVE it is writable (create, write, fsync-less
// flush, read back, delete a probe file — an mkpath that succeeds on a
// read-only mount is not evidence). Returns an empty string on success, or a
// human-readable reason on failure.
//
// MUST NOT fall back anywhere. The caller's only correct responses are to
// proceed portable or to exit non-zero telling the user what to fix.
QString prepareDataRoot();

} // namespace lightning::portable
