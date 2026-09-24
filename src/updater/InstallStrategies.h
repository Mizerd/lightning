#pragma once

#include "updater/UpdaterArgs.h"

#include <QString>
#include <QStringList>

#include <functional>

// Per-install-type strategies (UPDATE-SPEC §10).
//
// Each function returns a plan (program path plus argument vector) instead of
// executing anything, so decisions are unit-testable and the no-shell rule is
// structural. Inputs are only the validated argv struct and the filesystem
// probe; never the manifest, network or user input.

namespace updater {

// ---------------------------------------------------------------------------
// Windows setup executable
// ---------------------------------------------------------------------------
//
// The Windows setup is NSIS (MUI2), built by `makensis` from
// packaging/windows/installer.nsi. Its silent switch "/S" must come first:
// NSIS parses switches positionally and ignores a later "/S".
//
// The installer declares `RequestExecutionLevel user`. It and the MSI can
// install in two scopes (GitHub issue #14):
//
//   * Per-user (the default; every installation up to 0.9.9):
//     %LOCALAPPDATA%\Programs\Lightning, HKCU, no UAC. The setup EXE gets
//     "/CURRENTUSER" after "/S"; the MSI arguments are unchanged.
//   * Per-machine: Program Files, HKLM. The setup EXE gets "/ALLUSERS", the
//     MSI "ALLUSERS=1" (without it Windows Installer misses the old version
//     and installs a second copy), and the plan sets `elevateOnWindows`, so
//     the helper uses ShellExecuteEx "runas": one UAC prompt. Declining it
//     installs nothing.
//
// "/D=<dir>" is deliberately not passed: the installer remembers its own
// location, and forcing one would move the installation unasked.
extern const QStringList kNsisSilentSwitches;
// The scope switches installer.nsi understands. They always follow "/S".
extern const QString kNsisPerUserSwitch;     // "/CURRENTUSER"
extern const QString kNsisPerMachineSwitch;  // "/ALLUSERS"
// Windows Installer property selecting the MSI's per-machine context
// (generate-windows-wix.py points the install directory at Program Files).
extern const QString kMsiPerMachineProperty; // "ALLUSERS=1"

// ---------------------------------------------------------------------------
// Plans and errors
// ---------------------------------------------------------------------------

struct InstallPlan {
    // False for modes the helper performs itself (portable swap, AppImage
    // replacement); `program` and `arguments` are then empty.
    bool requiresExternalProcess = false;
    QString program;             // absolute path, never resolved from PATH
    QStringList arguments;       // one element per argument, never a command line
    QString workingDirectory;    // optional
    bool elevates = false;       // true only when `program` is pkexec
    // Windows only: launch through ShellExecuteEx "runas" (UAC) instead of
    // CreateProcess. Set only for per-machine MSI/setup upgrades.
    bool elevateOnWindows = false;
    // Set with elevateOnWindows: the artifact the elevated process reads. The
    // helper opens it with read sharing only, re-hashes it through that handle
    // and holds it until the process exits, so same-user malware cannot swap
    // the file while the UAC prompt waits and have it run as administrator.
    QString lockedArtifact;
};

enum class StrategyError {
    None = 0,
    NotSelfInstallable,   // flatpak / snap / development / unknown / dmg
    NoPackageManagerFound,
    ElevationHelperMissing,
    UnsupportedPlatform,
    InvalidArtifact,
};

const char *strategyErrorName(StrategyError error);

struct StrategyResult {
    StrategyError error = StrategyError::None;
    QString message;
    InstallPlan plan;

    bool ok() const { return error == StrategyError::None; }
};

// Injectable probe so the package-manager search order is testable. Defaults
// to "absolute path exists and is executable".
using ExecutableProbe = std::function<bool(const QString &)>;
ExecutableProbe defaultExecutableProbe();

// ---------------------------------------------------------------------------
// Strategies
// ---------------------------------------------------------------------------

StrategyResult planWindowsMsi(const UpdaterArguments &args);
StrategyResult planWindowsSetup(const UpdaterArguments &args);
StrategyResult planWindowsPortable(const UpdaterArguments &args);
StrategyResult planLinuxAppImage(const UpdaterArguments &args);
StrategyResult planLinuxDeb(const UpdaterArguments &args,
                            const ExecutableProbe &probe = ExecutableProbe());
StrategyResult planLinuxRpm(const UpdaterArguments &args,
                            const ExecutableProbe &probe = ExecutableProbe());

// Dispatches on args.mode. The argv parser refuses non-self-installable modes,
// but they are still handled explicitly so none can fall through.
StrategyResult planForMode(const UpdaterArguments &args,
                           const ExecutableProbe &probe = ExecutableProbe());

// Candidate absolute locations, in probe order. Exposed for tests.
QStringList pkexecCandidates();
QStringList debFrontendCandidates();  // apt-get, then dpkg
QStringList rpmFrontendCandidates();  // dnf5, dnf, rpm-ostree, rpm

// Argument vector for a resolved package manager. The artifact path is always
// the last, single element and is never quoted or escaped: QProcess passes it
// verbatim.
QStringList packageManagerArguments(const QString &managerPath,
                                    const QString &artifactPath);

// Absolute path to a Windows system executable, from GetSystemDirectoryW(),
// never %SystemRoot%/%windir% (user-settable in HKCU\Environment, a known UAC
// bypass, and this path is launched elevated). Off Windows the environment
// form with a C:\Windows fallback keeps the plan testable.
QString windowsSystemExecutable(const QString &executableName);

// ShellExecuteEx takes one parameter string, so the elevated path is the one
// place a plan becomes a command line. Uses the CommandLineToArgvW quoting
// convention: elements that are empty or contain a space are quoted, and
// backslashes are doubled only before the closing quote. msiexec and NSIS
// accept a quoted path, the only quoting produced. An element with a double
// quote or control character is refused (*ok = false) rather than escaped: no
// Windows path contains one.
QString windowsCommandLine(const QStringList &arguments, bool *ok);

// The elevated installer must be launched with the locked file's own path, not
// the original string: a junction in a parent directory could be re-pointed
// after locking. The helper gets the handle's final path
// (GetFinalPathNameByHandleW, VOLUME_NAME_DOS) and this converts it:
// "\\?\C:\..." -> "C:\...", "\\?\UNC\server\share\..." -> "\\server\share\...".
// Anything else (volume GUID, device path, empty, relative) returns "" and the
// install is refused.
QString launchablePathFromFinal(const QString &finalPath);

// Rewrites every reference to plan.lockedArtifact (setup EXE program and
// working directory, MSI /i argument) to `launchablePath`. Returns false and
// leaves `plan` untouched when there is none.
bool retargetPlanToLockedFile(InstallPlan &plan, const QString &launchablePath);

} // namespace updater
