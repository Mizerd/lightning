#pragma once

#include <QString>
#include <QStringList>

namespace updater {

enum class UpdaterMode {
    Invalid = 0,
    WindowsMsi,
    WindowsSetup,
    WindowsPortable,
    LinuxAppImage,
    LinuxDeb,
    LinuxRpm,
    LinuxFlatpak,
    LinuxSnap,
    LinuxRpmRepo,
    MacosDmg,
    Development,
    UnknownInstall,
};

namespace detail {

struct ModeName {
    UpdaterMode mode;
    const char *name;
};

inline constexpr ModeName kModeNames[] = {
    {UpdaterMode::WindowsMsi, "windows-msi"},
    {UpdaterMode::WindowsSetup, "windows-setup"},
    {UpdaterMode::WindowsPortable, "windows-portable"},
    {UpdaterMode::LinuxAppImage, "linux-appimage"},
    {UpdaterMode::LinuxDeb, "linux-deb"},
    {UpdaterMode::LinuxRpm, "linux-rpm"},
    {UpdaterMode::LinuxFlatpak, "linux-flatpak"},
    {UpdaterMode::LinuxSnap, "linux-snap"},
    {UpdaterMode::LinuxRpmRepo, "linux-rpm-repo"},
    {UpdaterMode::MacosDmg, "macos-dmg"},
    {UpdaterMode::Development, "development"},
    {UpdaterMode::UnknownInstall, "unknown"},
};

}

inline UpdaterMode modeFromString(const QString &value)
{
    for (const detail::ModeName &entry : detail::kModeNames) {
        if (value == QLatin1String(entry.name))
            return entry.mode;
    }
    return UpdaterMode::Invalid;
}

QString modeToString(UpdaterMode mode);

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
    case UpdaterMode::LinuxRpmRepo:
    case UpdaterMode::MacosDmg:
    case UpdaterMode::Development:
    case UpdaterMode::UnknownInstall:
        return false;
    }
    return false;
}

bool isInProcessMode(UpdaterMode mode);

enum class InstallScope {
    User,
    Machine,
};

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
    UnsafeValue,
    UnknownMode,
    ModeNotSelfInstallable,
    PathNotAbsolute,
    PathTraversal,
    PathDoesNotExist,
    PathNotAFile,
    PathNotADirectory,
    PathIsSymlink,
    ArtifactEmpty,
    StatusParentMissing,
    InvalidPid,
    InvalidDigest,
    InvalidInstallScope,
};

struct UpdaterArguments {
    UpdaterMode mode = UpdaterMode::Invalid;
    QString modeString;
    QString artifactPath;
    qint64 pid = 0;
    QString targetPath;
    QString relaunchPath;
    QString statusPath;
    QString expectedSha256;
    InstallScope installScope = InstallScope::User;

    // The helper relaunches ONLY when Lightning explicitly asked it to.
    bool relaunchRequested() const { return !relaunchPath.isEmpty(); }
};

struct ArgsParseResult {
    ArgsError error = ArgsError::None;
    QString message;
    QString offendingOption;
    UpdaterArguments args;

    bool ok() const { return error == ArgsError::None; }
};

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

QString preScanStatusPath(const QStringList &arguments);

}
