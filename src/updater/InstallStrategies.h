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
// The installer declares `RequestExecutionLevel user` and installs into
// %LOCALAPPDATA%\Programs\Lightning with the uninstall entry under HKCU, so
// it is PER-USER: there is no UAC prompt, and the helper must not attempt to
// elevate. The same is true of the MSI (InstallScope="perUser").
//
// "/D=<dir>" would force the install directory. We deliberately do NOT pass
// it: the installer already remembers where it put itself, and overriding it
// from the helper would move a user's installation without being asked.
//
// If the installer technology ever changes, this constant and the comment
// above are the ONLY places that need editing.
extern const QStringList kNsisSilentSwitches;

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

// Absolute path to a Windows system executable, resolved from %SystemRoot%
// rather than from PATH. On non-Windows this returns the conventional
// C:\Windows\System32 form so the plan stays testable.
QString windowsSystemExecutable(const QString &executableName);

} // namespace updater
