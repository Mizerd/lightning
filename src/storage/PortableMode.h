#pragma once

#include <QDateTime>
#include <QString>

// Portable-installation mode: the single authority for whether this copy of
// Lightning is self-contained.
//
// A portable installation keeps everything it owns in one directory tree
// beside the executable (settings, SDK/crypto store, caches, logs, saved
// session). Copying that directory to another path or PC continues the same
// session as the same Matrix device, with no registry, %LOCALAPPDATA% or
// Credential Manager involved.
//
// The only signal is a positive marker file (`portable.marker`) that packaging
// writes into the portable ZIP alone. It is never inferred from build defines,
// writability or location; a wrong guess makes an account "vanish".
//
// Only executableDir() is platform-specific, so the policy is testable on
// Linux.
//
// Ordering: the decision is made in src/main.cpp between setApplicationName()
// and the first default-constructed QSettings (registry-backed on Windows, and
// not undoable). No QCoreApplication exists yet, so executableDir() uses the
// platform API; argv[0] is launcher-controlled and unusable.
//
// Resolved paths are not logged at info level: they can contain the account's
// identity.
namespace lightning::portable {

// The marker packaging puts in the portable ZIP only; its presence beside the
// executable is the only signal.
inline constexpr char kMarkerFileName[] = "portable.marker";

// The portable data directory name, here so lightning-updater (Qt6::Core only)
// can name it without linking this unit. The updater must exclude it from the
// directory swap or an update destroys the session and crypto store.
inline constexpr char kDataDirName[] = "data";

// Development override read by isPortable(): "1"/"true"/"on" forces portable
// mode with the real executable directory, "0"/"false"/"off" forces installed
// mode, anything else defers to the marker. An environment variable rather
// than a flag because preflightParse() acts on options in argv order
// (--reset-crypto-store deletes mid-loop), so a flag's position would matter.
inline constexpr char kPortableEnvVar[] = "LIGHTNING_PORTABLE";

// Directory of the running executable, resolved without a Qt application
// instance. '/' separators, no trailing slash. Empty on failure, which callers
// must treat as "not portable", never as "cwd". Returns the injected directory
// while a test override is installed.
QString executableDir();

// Decided once per process and cached, so callers always agree and a marker
// appearing or disappearing mid-run cannot move storage.
bool isPortable();

// <executableDir>/data, or empty when not portable. Never falls back to
// %LOCALAPPDATA%/$XDG_DATA_HOME.
QString dataRoot();

// The portable tree's directories, each empty when not portable. Named here so
// writers and cleanup cannot drift.
//   config  - QSettings INI files (see main.cpp's QSettings::setPath)
//   secrets - PortableSecretStore's sealed document and key
//   cache   - what QStandardPaths::CacheLocation would hold
//   logs    - reserved for opt-in file logging
// primaryRoot() resolves to <dataRoot>/matrix in portable mode.
QString configDir();
QString secretsDir();
QString cacheDir();
QString logsDir();
// Scratch space for decrypted playable media and voice recordings, inside the
// tree rather than the OS temp directory. Files are session-scoped, 0600 and
// wiped on sign-out, switch and exit, but a crash leaves them on the portable
// medium. That does not widen the threat model: the folder already holds the
// sealed session and crypto store (see PortableSecretStore).
QString tempDir();
// Root for session-scoped decrypted media: tempDir() when portable,
// QDir::tempPath() otherwise. Here so the voice recorder need not link the
// media bridge.
QString mediaScratchRoot();
// Test seam: keeps sweep tests away from the real temp directory, where a
// running Lightning keeps live scratch. Empty restores the real decision.
void setMediaScratchRootOverrideForTest(const QString &root);
// Marks a scratch directory live for the life of the process by holding a
// lock file in it. The sweep skips directories whose lock is held, so a second
// instance cannot delete files a player is using (the sweep matches by
// prefix).
void holdScratchDirLive(const QString &dir);
// Drops the lock for `dir`. Call before destroying the QTemporaryDir; the next
// hold prunes entries whose directory is gone.
void releaseScratchDir(const QString &dir);
inline constexpr char kScratchLiveLockName[] = ".lightning-live.lock";

// Removes stale `lightning-*` scratch directories under mediaScratchRoot()
// that belong to this user (left by a crash). Runs at startup on every install
// type because they hold decrypted media. Stale means the live lock is absent
// and old, or present but no longer held.
int cleanStaleTempDirs();
int cleanStaleTempDirs(const QDateTime &now);
// The updater's extraction staging and displaced previous version. Inside
// `data`, the one name the swap preserves, so an update never writes into
// the folder's parent.
QString updateWorkDir();

// Pure helpers over an injected directory.
bool markerPresentIn(const QString &dir);
QString dataRootFor(const QString &executableDir);

// Test seam: forces the answer and the directory executableDir() reports. It
// sits in front of the cached real decision, so clearing it restores the real
// answer.
void setPortableOverrideForTest(bool portable, const QString &executableDir);
void clearPortableOverrideForTest();

// Creates the portable tree and proves it writable with a probe file (mkpath
// succeeds on read-only mounts). Returns "" on success or a reason. Never
// falls back: the caller proceeds portable or exits with the reason.
QString prepareDataRoot();

} // namespace lightning::portable
