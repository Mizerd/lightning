#pragma once

#include <QString>
#include <QStringList>

// Lightning updater helper — argv contract (UPDATE-SPEC §11).
//
// The helper accepts NO command strings from anywhere. Its argument vector
// is fixed and fully enumerated here:
//
//   lightning-updater --mode <install-type> --artifact <path> --pid <n>
//                     --target <path> --status <path> --sha256 <64 hex>
//                     [--relaunch <path>] [--install-scope user|machine]
//
// Every option may appear at most once, every value is validated, and any
// unrecognised option, positional argument, duplicate, or malformed value is
// a terminal parse error. Nothing in this file ever constructs a shell
// command; the parsed struct only ever feeds a QProcess program + QStringList
// argument vector (see InstallStrategies).
//
// `--relaunch` is the helper's entire relaunch policy: present means start
// that program after a successful install. Lightning passes it only for
// installAndRestart(), so an install-on-quit does not reopen the application.
// Omission is the safe default.
//
// `--sha256` is the signed manifest's digest. The helper re-hashes the
// artifact right before acting (the path may sit for hours before an
// install-on-quit) and refuses on mismatch; see ArtifactDigest.h. Only the
// shape is validated here; main.cpp compares.
//
// `--install-scope` is valid only for windows-msi and windows-setup.
// `machine` runs the upgrade per-machine and elevated (InstallStrategies.h);
// absent means `user`. Omission can only fail to elevate, never elevate
// unprompted.
//
// Every path option refuses a symbolic link: Lightning passes resolved paths,
// and a link would redirect a chmod, replace or package-manager read.
//
// No global state, so it is testable with a plain QStringList.
//
// This translation unit is deliberately free of global state so it can be
// unit-tested by passing a QStringList directly.

namespace updater {

// Install-type identifiers from UPDATE-SPEC §5. All parse, so refusals are
// specific, but only the self-installable subset is dispatched.
enum class UpdaterMode {
    Invalid = 0,     // not a recognised identifier at all
    WindowsMsi,      // "windows-msi"
    WindowsSetup,    // "windows-setup"
    WindowsPortable, // "windows-portable"
    LinuxAppImage,   // "linux-appimage"
    LinuxDeb,        // "linux-deb"
    LinuxRpm,        // "linux-rpm"
    LinuxFlatpak,    // "linux-flatpak"     — ecosystem managed
    LinuxSnap,       // "linux-snap"        — ecosystem managed
    MacosDmg,        // "macos-dmg"         — no helper strategy yet
    Development,     // "development"       — never installs
    UnknownInstall,  // "unknown"           — never installs
};

namespace detail {

struct ModeName {
    UpdaterMode mode;
    const char *name;
};

// Inline so the application-side test can check, without linking the helper,
// that canInstallAutomatically() and isSelfInstallable() agree on every
// identifier.
inline constexpr ModeName kModeNames[] = {
    {UpdaterMode::WindowsMsi, "windows-msi"},
    {UpdaterMode::WindowsSetup, "windows-setup"},
    {UpdaterMode::WindowsPortable, "windows-portable"},
    {UpdaterMode::LinuxAppImage, "linux-appimage"},
    {UpdaterMode::LinuxDeb, "linux-deb"},
    {UpdaterMode::LinuxRpm, "linux-rpm"},
    {UpdaterMode::LinuxFlatpak, "linux-flatpak"},
    {UpdaterMode::LinuxSnap, "linux-snap"},
    {UpdaterMode::MacosDmg, "macos-dmg"},
    {UpdaterMode::Development, "development"},
    {UpdaterMode::UnknownInstall, "unknown"},
};

} // namespace detail

// Exact, case-sensitive match; anything else is UpdaterMode::Invalid.
inline UpdaterMode modeFromString(const QString &value)
{
    for (const detail::ModeName &entry : detail::kModeNames) {
        if (value == QLatin1String(entry.name))
            return entry.mode;
    }
    return UpdaterMode::Invalid;
}

QString modeToString(UpdaterMode mode);

// True only for the modes the helper is actually allowed to act on.
inline bool isSelfInstallable(UpdaterMode mode)
{
    switch (mode) {
    case UpdaterMode::WindowsMsi:
    case UpdaterMode::WindowsSetup:
    case UpdaterMode::WindowsPortable:
    case UpdaterMode::LinuxAppImage:
    case UpdaterMode::LinuxDeb:
    case UpdaterMode::LinuxRpm:
        return true;
    case UpdaterMode::Invalid:
    case UpdaterMode::LinuxFlatpak:
    case UpdaterMode::LinuxSnap:
    case UpdaterMode::MacosDmg:
    case UpdaterMode::Development:
    case UpdaterMode::UnknownInstall:
        return false;
    }
    return false;
}

// Modes installed in-process by the helper (extraction or atomic replace)
// rather than by an external installer.
bool isInProcessMode(UpdaterMode mode);

// Mirrors lightning::update::InstallScope; the helper links none of the
// application.
enum class InstallScope {
    User,     // "user"    -- the default, and every installation up to 0.9.9
    Machine,  // "machine" -- Program Files / HKLM; upgraded elevated
};

// Inline so the application-side test can compare it with installScopeId().
inline QString installScopeToString(InstallScope scope)
{
    return scope == InstallScope::Machine ? QStringLiteral("machine")
                                          : QStringLiteral("user");
}

enum class ArgsError {
    None = 0,
    EmptyArguments,
    UnknownOption,
    PositionalArgument,
    DuplicateOption,
    MissingValue,
    MissingRequiredOption,
    EmptyValue,
    UnsafeValue,      // embedded NUL / control character / over-long
    UnknownMode,
    ModeNotSelfInstallable,
    PathNotAbsolute,
    PathTraversal,    // contains a ".." component
    PathDoesNotExist,
    PathNotAFile,
    PathNotADirectory,
    PathIsSymlink,
    ArtifactEmpty,
    StatusParentMissing,
    InvalidPid,
    InvalidDigest,    // --sha256 is not exactly 64 lowercase hex characters
    InvalidInstallScope, // not "user"/"machine", or given for a mode it does not apply to
};

struct UpdaterArguments {
    UpdaterMode mode = UpdaterMode::Invalid;
    QString modeString;
    QString artifactPath;  // absolute, exists, regular file, non-empty
    qint64 pid = 0;        // > 1
    QString targetPath;    // absolute, exists (file for appimage, dir for portable)
    // Empty means do not relaunch; otherwise absolute, existing regular file.
    QString relaunchPath;
    QString statusPath;    // absolute, parent directory exists, not a dir/symlink
    // Manifest SHA-256, 64 lowercase hex characters; compared in main.cpp.
    QString expectedSha256;
    // User unless --install-scope machine was supplied (MSI / setup only).
    InstallScope installScope = InstallScope::User;

    // The helper relaunches ONLY when Lightning explicitly asked it to.
    bool relaunchRequested() const { return !relaunchPath.isEmpty(); }
};

struct ArgsParseResult {
    ArgsError error = ArgsError::None;
    QString message;           // human readable, never contains secrets
    QString offendingOption;   // e.g. "--pid"; empty when not option-specific
    UpdaterArguments args;

    bool ok() const { return error == ArgsError::None; }
};

// `arguments` excludes the program name.
ArgsParseResult parseUpdaterArgs(const QStringList &arguments);

// Also used by the pre-scan. Unsafe: empty, embedded NUL, any control
// character (e.g. a newline smuggling a second log line), or over-long.
bool valueIsUnsafe(const QString &value);
bool pathIsAbsolute(const QString &path);

// True when any component of `path` is "..". Lightning passes resolved paths,
// so this is never legitimate, and refusing it stops `/staging/../../etc/x`
// from normalizing into something that passes the existence checks.
bool pathContainsParentComponent(const QString &path);

QString parseErrorName(ArgsError error);

// Recovers --status from an otherwise invalid argument list: only when present
// once, safe, absolute, and its parent exists.
QString preScanStatusPath(const QStringList &arguments);

} // namespace updater
