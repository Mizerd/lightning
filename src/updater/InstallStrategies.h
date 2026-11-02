#pragma once

#include "updater/UpdaterArgs.h"

#include <QString>
#include <QStringList>

#include <functional>

namespace updater {

extern const QStringList kNsisSilentSwitches;
extern const QString kNsisPerUserSwitch;
extern const QString kNsisPerMachineSwitch;
extern const QString kMsiPerMachineProperty;

struct InstallPlan {
    bool requiresExternalProcess = false;
    QString program;
    QStringList arguments;
    QString workingDirectory;
    bool elevates = false;
    bool elevateOnWindows = false;
    // Set with elevateOnWindows: the artifact the elevated process reads. The
    // helper opens it with read sharing only, re-hashes it through that handle
    // and holds it until the process exits, so same-user malware cannot swap
    // the file while the UAC prompt waits and have it run as administrator.
    QString lockedArtifact;
};

enum class StrategyError {
    None = 0,
    NotSelfInstallable,
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

using ExecutableProbe = std::function<bool(const QString &)>;
ExecutableProbe defaultExecutableProbe();

StrategyResult planWindowsMsi(const UpdaterArguments &args);
StrategyResult planWindowsSetup(const UpdaterArguments &args);
StrategyResult planWindowsPortable(const UpdaterArguments &args);
StrategyResult planLinuxAppImage(const UpdaterArguments &args);
StrategyResult planLinuxDeb(const UpdaterArguments &args,
                            const ExecutableProbe &probe = ExecutableProbe());
StrategyResult planLinuxRpm(const UpdaterArguments &args,
                            const ExecutableProbe &probe = ExecutableProbe());

StrategyResult planForMode(const UpdaterArguments &args,
                           const ExecutableProbe &probe = ExecutableProbe());

QStringList pkexecCandidates();
QStringList debFrontendCandidates();
QStringList rpmFrontendCandidates();

QStringList packageManagerArguments(const QString &managerPath,
                                    const QString &artifactPath);

QString windowsSystemExecutable(const QString &executableName);

// ShellExecuteEx takes one parameter string, so the elevated path is the one
// place a plan becomes a command line. Uses the CommandLineToArgvW quoting
// convention: elements that are empty or contain a space are quoted, and
// backslashes are doubled only before the closing quote. msiexec and NSIS
// accept a quoted path, the only quoting produced. An element with a double
// quote or control character is refused (*ok = false) rather than escaped: no
// Windows path contains one.
QString windowsCommandLine(const QStringList &arguments, bool *ok);

QString launchablePathFromFinal(const QString &finalPath);

bool retargetPlanToLockedFile(InstallPlan &plan, const QString &launchablePath);

}
