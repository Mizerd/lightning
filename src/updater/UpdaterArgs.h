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
//                     [--relaunch <path>]
//
// Every option may appear at most once, every value is validated, and any
// unrecognised option, positional argument, duplicate, or malformed value is
// a terminal parse error. Nothing in this file ever constructs a shell
// command; the parsed struct only ever feeds a QProcess program + QStringList
// argument vector (see InstallStrategies).
//
// `--relaunch` is the ONE optional option, and it is the helper's entire
// relaunch policy: present means "start this program once the install
// succeeded", absent means "do not start anything". Lightning passes it only
// for installAndRestart(); a plain installUpdate() deliberately omits it, so
// an update applied on quit does not resurrect an application the user just
// closed. Omission is the safe default: forgetting the option can only ever
// skip a relaunch, never cause an unwanted one. When present it is validated
// exactly as strictly as every required path.
//
// `--sha256` is the digest the SIGNED MANIFEST gave for the artifact. The
// helper is connected to the application's verified download by nothing but
// a filesystem path, and that path can sit armed for hours before an
// install-on-quit fires -- so the helper re-hashes the artifact itself,
// right before it acts, and refuses on any mismatch (see ArtifactDigest.h).
// The parser only validates the SHAPE of the value here; the comparison
// happens in main.cpp after the application has exited, which is the last
// moment before the bytes are consumed.
//
// Every path option refuses a symbolic link. Lightning hands the helper
// fully resolved paths, and a link would redirect a chmod, a replace, or a
// package-manager read at whatever it points to at the moment of use.
//
// This translation unit is deliberately free of global state so it can be
// unit-tested by passing a QStringList directly.

namespace updater {

// The canonical install-type identifiers from UPDATE-SPEC §5. All of them
// parse (so the helper can report an honest, specific refusal), but only the
// self-installable subset is ever dispatched.
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

// The canonical table. Header-scoped and inline so that the application-side
// install-type test can assert, without linking the helper, that
// lightning::update::canInstallAutomatically() and isSelfInstallable() agree
// about every identifier. The two sides disagreeing is exactly the defect
// that assertion exists to catch.
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

// Exact, case-sensitive mapping against the canonical identifiers. An
// unrecognised string yields UpdaterMode::Invalid; there is no fuzzy match,
// no case folding, and no aliasing.
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

// True for the modes whose install is performed in-process by the helper
// (archive extraction / atomic replacement) rather than by launching an
// external installer.
bool isInProcessMode(UpdaterMode mode);

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
};

struct UpdaterArguments {
    UpdaterMode mode = UpdaterMode::Invalid;
    QString modeString;
    QString artifactPath;  // absolute, exists, regular file, non-empty
    qint64 pid = 0;        // > 1
    QString targetPath;    // absolute, exists (file for appimage, dir for portable)
    // Empty when --relaunch was not supplied, which means "do not relaunch".
    // When non-empty it is absolute, exists and is a regular file.
    QString relaunchPath;
    QString statusPath;    // absolute, parent directory exists, not a dir/symlink
    // The manifest's SHA-256 for the artifact, 64 lowercase hex characters.
    // Shape-validated here; compared against the file in main.cpp.
    QString expectedSha256;

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

// `arguments` must NOT contain the program name — main.cpp passes
// QCoreApplication::arguments().mid(1).
ArgsParseResult parseUpdaterArgs(const QStringList &arguments);

// Exposed for tests and for the pre-scan main.cpp uses to find a usable
// --status path even when the full parse failed.
//
// A value is unsafe when it is empty, contains an embedded NUL, contains any
// control character (including newline / carriage return, which is how a
// value would try to smuggle a second line into anything that logs it), or is
// unreasonably long.
bool valueIsUnsafe(const QString &value);
bool pathIsAbsolute(const QString &path);

// True when any component of `path` is "..". Lightning always hands the
// helper a fully resolved path, so a traversal component is never legitimate
// here — and refusing it keeps a `/staging/../../etc/x` value from quietly
// cleaning itself into something that passes the existence checks.
bool pathContainsParentComponent(const QString &path);

QString parseErrorName(ArgsError error);

// Best-effort recovery of the --status path from a raw argument list that may
// otherwise be invalid. Returns an empty string unless the value is present
// exactly once, safe, absolute, and its parent directory already exists.
QString preScanStatusPath(const QStringList &arguments);

} // namespace updater
