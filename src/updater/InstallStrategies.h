#pragma once

#include "updater/UpdaterArgs.h"

#include <QString>
#include <QStringList>

#include <functional>

// Per-install-type strategies (UPDATE-SPEC §10).
//
// Every function here returns a PLAN — a program path plus an argument
// VECTOR — instead of executing anything. That keeps the whole decision
// surface unit-testable without launching a process, and it makes the
// no-shell rule structural rather than a convention: there is no string a
// caller could concatenate into, because a plan never holds a command line.
//
// Nothing in this file reads the manifest, the network, or any user input.
// The only inputs are the validated argv struct and the filesystem probe.

namespace updater {

// ---------------------------------------------------------------------------
// Windows setup executable
// ---------------------------------------------------------------------------
//
// CONFIRMED against lightning-deploy (2026-08-15): the Windows setup
// executable is built by `makensis` from packaging/windows/installer.nsi —
// it is NSIS with the MUI2 UI, NOT Inno Setup. The NSIS silent switch is a
// single capital "/S", and NSIS requires it to appear FIRST in the argument
// vector; NSIS parses its own switches positionally and ignores anything it
// does not recognise, so a later "/S" is silently dropped.
//
// The installer declares `RequestExecutionLevel user`. After 0.9.9 both it
// and the MSI can install in either of two scopes (GitHub issue #14):
//
//   * PER-USER, the default and every installation made by 0.9.9 or older:
//     %LOCALAPPDATA%\Programs\Lightning, registration under HKCU, no UAC
//     prompt, and the helper must not attempt to elevate. The setup EXE gets
//     "/CURRENTUSER" after "/S"; the MSI's argument vector is unchanged.
//   * PER-MACHINE: Program Files, registration under HKLM. The setup EXE gets
//     "/ALLUSERS", the MSI "ALLUSERS=1" -- without it Windows Installer looks
//     for the old version only in the per-user context, finds nothing, and
//     installs a SECOND copy -- and the plan is marked `elevateOnWindows`, so
//     the helper starts it through ShellExecuteEx "runas": one UAC prompt,
//     for an update the person just asked for. Declining it installs nothing.
//
// "/D=<dir>" would force the install directory. We deliberately do NOT pass
// it: the installer already remembers where it put itself, and overriding it
// from the helper would move a user's installation without being asked.
//
// If the installer technology ever changes, this constant and the comment
// above are the ONLY places that need editing.
extern const QStringList kNsisSilentSwitches;
// The scope switches installer.nsi understands. They always follow "/S".
extern const QString kNsisPerUserSwitch;     // "/CURRENTUSER"
extern const QString kNsisPerMachineSwitch;  // "/ALLUSERS"
// The Windows Installer property that selects the MSI's per-machine context
// (packaging-ci/scripts/generate-windows-wix.py re-points its install
// directory at Program Files when it is set).
extern const QString kMsiPerMachineProperty; // "ALLUSERS=1"

// ---------------------------------------------------------------------------
// Plans and errors
// ---------------------------------------------------------------------------

struct InstallPlan {
    // False for the modes the helper performs itself (portable directory
    // swap, AppImage replacement). When false, `program` and `arguments` are
    // empty and the caller must run the in-process routine.
    bool requiresExternalProcess = false;
    QString program;             // absolute path, never resolved from PATH
    QStringList arguments;       // one element per argument, never a command line
    QString workingDirectory;    // optional
    bool elevates = false;       // true only when `program` is pkexec
    // Windows only: start `program` through ShellExecuteEx with the "runas"
    // verb (a UAC prompt) instead of CreateProcess. Set for a per-machine MSI
    // or setup upgrade and for nothing else.
    bool elevateOnWindows = false;
    // Set together with elevateOnWindows: the verified artifact the elevated
    // process will read. The helper opens it with READ sharing only (no
    // write, no delete), re-hashes it FROM THAT HANDLE and holds the handle
    // until the elevated process exits. The UAC prompt can wait for minutes,
    // and the artifact sits in a user-writable staging directory, so without
    // this same-user malware could swap it after the path-based re-hash and
    // have the person's own consent run it as administrator.
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

// Injectable filesystem probe so the package-manager search order is testable
// without depending on what happens to be installed on the build machine.
// Defaults to "this absolute path exists and is executable".
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

// Dispatches on args.mode. Non-self-installable modes never reach here — the
// argv parser refuses them — but the switch still handles them explicitly so
// a future mode cannot fall through into silence.
StrategyResult planForMode(const UpdaterArguments &args,
                           const ExecutableProbe &probe = ExecutableProbe());

// Candidate absolute locations, in probe order. Exposed for tests.
QStringList pkexecCandidates();
QStringList debFrontendCandidates();  // apt-get, then dpkg
QStringList rpmFrontendCandidates();  // dnf5, dnf, rpm-ostree, rpm

// Builds the argument vector for a resolved package-manager binary. The
// artifact path is always the last element, always a single element, and is
// never quoted or escaped — a QStringList argument is passed through verbatim
// by QProcess, so spaces, Unicode and parentheses need no special handling
// and MUST NOT be given any.
QStringList packageManagerArguments(const QString &managerPath,
                                    const QString &artifactPath);

// Absolute path to a Windows system executable. On Windows it comes from
// GetSystemDirectoryW(), never from %SystemRoot% / %windir%: a user can set
// those in HKCU\Environment (the classic "windir" UAC bypass), and the MSI
// plan hands this path to an ELEVATED launch. Off Windows the environment
// form is kept, with a C:\Windows fallback, so the plan stays testable.
QString windowsSystemExecutable(const QString &executableName);

// ShellExecuteEx takes ONE parameter string where CreateProcess (QProcess)
// takes a vector, so the elevated path is the one place a plan has to become
// a command line. It is built with the CommandLineToArgvW / C runtime quoting
// convention: an element is wrapped in double quotes when it is empty or holds
// a space, and backslashes are only doubled where they precede that closing
// quote. msiexec and NSIS have parsers of their own, not CommandLineToArgvW;
// both accept a double-quoted path, which is the only quoting these plans
// ever produce (a fixed switch, a property, or a quoted path). An element containing a double quote or a control
// character is REFUSED (*ok = false) rather than escaped: no Windows path can
// contain one, every value here is a path or a fixed switch, so one appearing
// means something upstream is wrong and escaping it would only hide that.
QString windowsCommandLine(const QStringList &arguments, bool *ok);

// The path an elevated installer is launched with must be the path of the
// file the helper LOCKED, not the string it was given: a junction anywhere in
// the artifact's parent directories can be re-pointed after the lock is taken
// (nothing is open under the junction entry itself), and the elevated process
// would then open a different file by the same string. So the helper asks
// Windows for the locked handle's final path (GetFinalPathNameByHandleW,
// VOLUME_NAME_DOS) and rewrites the plan to it.
//
// launchablePathFromFinal() turns that result into a path msiexec and the
// loader accept: "\\?\C:\..." -> "C:\...", "\\?\UNC\server\share\..." ->
// "\\server\share\...". Anything else -- a volume GUID path, a device path,
// an empty or relative string -- returns an empty string and the install is
// refused.
QString launchablePathFromFinal(const QString &finalPath);

// Rewrites every place `plan` names plan.lockedArtifact -- the program and
// its working directory for the setup EXE, the /i argument for the MSI -- to
// `launchablePath`. Returns false, leaving `plan` untouched, when the plan
// names no locked artifact or does not contain it anywhere.
bool retargetPlanToLockedFile(InstallPlan &plan, const QString &launchablePath);

} // namespace updater
