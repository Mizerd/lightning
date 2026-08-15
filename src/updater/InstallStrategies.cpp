#include "updater/InstallStrategies.h"

#include <QDir>
#include <QFileInfo>

namespace updater {

// NSIS MUI2. See the header for why this is "/S", why it must be first, and
// why "/D=" is deliberately omitted.
const QStringList kNsisSilentSwitches = {QStringLiteral("/S")};

namespace {

StrategyResult strategyFail(StrategyError error, const QString &message)
{
    StrategyResult result;
    result.error = error;
    result.message = message;
    return result;
}

QString firstExisting(const QStringList &candidates, const ExecutableProbe &probe)
{
    for (const QString &candidate : candidates) {
        if (probe(candidate))
            return candidate;
    }
    return QString();
}

} // namespace

const char *strategyErrorName(StrategyError error)
{
    switch (error) {
    case StrategyError::None: return "none";
    case StrategyError::NotSelfInstallable: return "not-self-installable";
    case StrategyError::NoPackageManagerFound: return "no-package-manager-found";
    case StrategyError::ElevationHelperMissing: return "elevation-helper-missing";
    case StrategyError::UnsupportedPlatform: return "unsupported-platform";
    case StrategyError::InvalidArtifact: return "invalid-artifact";
    }
    return "unknown";
}

ExecutableProbe defaultExecutableProbe()
{
    return [](const QString &path) {
        const QFileInfo info(path);
        return info.exists() && info.isFile() && info.isExecutable();
    };
}

QStringList pkexecCandidates()
{
    // Absolute paths only — never PATH. The NixOS wrapper directory is listed
    // because that is where a setuid pkexec actually lives on the maintainer's
    // own system, and a missing entry there would silently disable deb/rpm
    // installs on exactly the platform Lightning targets first.
    return {
        QStringLiteral("/usr/bin/pkexec"),
        QStringLiteral("/run/wrappers/bin/pkexec"),
        QStringLiteral("/usr/local/bin/pkexec"),
        QStringLiteral("/bin/pkexec"),
    };
}

QStringList debFrontendCandidates()
{
    return {
        QStringLiteral("/usr/bin/apt-get"),
        QStringLiteral("/usr/bin/dpkg"),
        QStringLiteral("/bin/dpkg"),
    };
}

QStringList rpmFrontendCandidates()
{
    // Probe order per UPDATE-SPEC §10: dnf5, dnf, rpm-ostree, rpm.
    return {
        QStringLiteral("/usr/bin/dnf5"),
        QStringLiteral("/usr/bin/dnf"),
        QStringLiteral("/usr/bin/rpm-ostree"),
        QStringLiteral("/usr/bin/rpm"),
        QStringLiteral("/bin/rpm"),
    };
}

QStringList packageManagerArguments(const QString &managerPath,
                                    const QString &artifactPath)
{
    const QString tool = QFileInfo(managerPath).fileName();

    if (tool == QLatin1String("apt-get")) {
        // A local .deb is installed by giving apt-get the path. It resolves
        // dependencies from the configured repositories, which dpkg cannot do.
        return {managerPath, QStringLiteral("install"), QStringLiteral("-y"),
                QStringLiteral("--only-upgrade"), artifactPath};
    }
    if (tool == QLatin1String("dpkg")) {
        // No --force-* of any kind: if dpkg refuses the package, that refusal
        // is information, not an obstacle.
        return {managerPath, QStringLiteral("-i"), artifactPath};
    }
    if (tool == QLatin1String("dnf5") || tool == QLatin1String("dnf")) {
        return {managerPath, QStringLiteral("install"), QStringLiteral("-y"),
                artifactPath};
    }
    if (tool == QLatin1String("rpm-ostree")) {
        // rpm-ostree layers the package into the next deployment; it never
        // mutates the running root, and it takes no -y.
        return {managerPath, QStringLiteral("install"), artifactPath};
    }
    if (tool == QLatin1String("rpm")) {
        // -U upgrades (installing if absent). Never --force, never --nodeps.
        return {managerPath, QStringLiteral("-U"), artifactPath};
    }
    return QStringList();
}

QString windowsSystemExecutable(const QString &executableName)
{
    QString root = qEnvironmentVariable("SystemRoot");
    if (root.isEmpty())
        root = qEnvironmentVariable("windir");
    if (root.isEmpty())
        root = QStringLiteral("C:\\Windows");
    while (root.endsWith(QLatin1Char('\\')) || root.endsWith(QLatin1Char('/')))
        root.chop(1);
    return root + QStringLiteral("\\System32\\") + executableName;
}

// ---------------------------------------------------------------------------

StrategyResult planWindowsMsi(const UpdaterArguments &args)
{
    if (args.artifactPath.isEmpty())
        return strategyFail(StrategyError::InvalidArtifact,
                            QStringLiteral("no artifact path"));

    StrategyResult result;
    result.plan.requiresExternalProcess = true;
    result.plan.program = windowsSystemExecutable(QStringLiteral("msiexec.exe"));
    // /i install-or-upgrade, /qb basic UI with a progress bar (never fully
    // silent — the user asked for this and should see it happening),
    // REINSTALLMODE=vomus forces every file to be re-cached from the new
    // package, which is what makes a same-version repair actually replace
    // files. The MSI is perUser, so no elevation is requested or needed.
    result.plan.arguments = {QStringLiteral("/i"), args.artifactPath,
                             QStringLiteral("/qb"),
                             QStringLiteral("REINSTALLMODE=vomus")};
    return result;
}

StrategyResult planWindowsSetup(const UpdaterArguments &args)
{
    if (args.artifactPath.isEmpty())
        return strategyFail(StrategyError::InvalidArtifact,
                            QStringLiteral("no artifact path"));

    StrategyResult result;
    result.plan.requiresExternalProcess = true;
    // The verified artifact IS the program here. It is an absolute path that
    // the argv validator already proved exists and is a regular file.
    result.plan.program = args.artifactPath;
    result.plan.arguments = kNsisSilentSwitches; // "/S" must come first
    result.plan.workingDirectory = QFileInfo(args.artifactPath).absolutePath();
    return result;
}

StrategyResult planWindowsPortable(const UpdaterArguments &)
{
    // Handled in-process: extract the verified ZIP into a private staging
    // directory, validate the layout, then swap directories with rollback.
    StrategyResult result;
    result.plan.requiresExternalProcess = false;
    return result;
}

StrategyResult planLinuxAppImage(const UpdaterArguments &)
{
    // Handled in-process: chmod +x the staged AppImage, then atomically
    // replace the target path with rollback.
    StrategyResult result;
    result.plan.requiresExternalProcess = false;
    return result;
}

StrategyResult planLinuxDeb(const UpdaterArguments &args,
                            const ExecutableProbe &probeIn)
{
    const ExecutableProbe probe = probeIn ? probeIn : defaultExecutableProbe();

    const QString pkexec = firstExisting(pkexecCandidates(), probe);
    if (pkexec.isEmpty())
        return strategyFail(StrategyError::ElevationHelperMissing,
                            QStringLiteral("pkexec was not found; install the "
                                           "package manually"));

    const QString manager = firstExisting(debFrontendCandidates(), probe);
    if (manager.isEmpty())
        return strategyFail(StrategyError::NoPackageManagerFound,
                            QStringLiteral("neither apt-get nor dpkg was found"));

    StrategyResult result;
    result.plan.requiresExternalProcess = true;
    result.plan.elevates = true;
    result.plan.program = pkexec;
    // pkexec's own argv: the program to run followed by its arguments. Never
    // sudo, never a shell, never a single command string.
    result.plan.arguments = packageManagerArguments(manager, args.artifactPath);
    if (result.plan.arguments.isEmpty())
        return strategyFail(StrategyError::NoPackageManagerFound,
                            QStringLiteral("unsupported package manager"));
    return result;
}

StrategyResult planLinuxRpm(const UpdaterArguments &args,
                            const ExecutableProbe &probeIn)
{
    const ExecutableProbe probe = probeIn ? probeIn : defaultExecutableProbe();

    const QString pkexec = firstExisting(pkexecCandidates(), probe);
    if (pkexec.isEmpty())
        return strategyFail(StrategyError::ElevationHelperMissing,
                            QStringLiteral("pkexec was not found; install the "
                                           "package manually"));

    const QString manager = firstExisting(rpmFrontendCandidates(), probe);
    if (manager.isEmpty())
        return strategyFail(StrategyError::NoPackageManagerFound,
                            QStringLiteral("no supported RPM front-end was found"));

    StrategyResult result;
    result.plan.requiresExternalProcess = true;
    result.plan.elevates = true;
    result.plan.program = pkexec;
    result.plan.arguments = packageManagerArguments(manager, args.artifactPath);
    if (result.plan.arguments.isEmpty())
        return strategyFail(StrategyError::NoPackageManagerFound,
                            QStringLiteral("unsupported package manager"));
    return result;
}

StrategyResult planForMode(const UpdaterArguments &args,
                           const ExecutableProbe &probe)
{
    switch (args.mode) {
    case UpdaterMode::WindowsMsi:
        return planWindowsMsi(args);
    case UpdaterMode::WindowsSetup:
        return planWindowsSetup(args);
    case UpdaterMode::WindowsPortable:
        return planWindowsPortable(args);
    case UpdaterMode::LinuxAppImage:
        return planLinuxAppImage(args);
    case UpdaterMode::LinuxDeb:
        return planLinuxDeb(args, probe);
    case UpdaterMode::LinuxRpm:
        return planLinuxRpm(args, probe);
    case UpdaterMode::LinuxFlatpak:
        return strategyFail(StrategyError::NotSelfInstallable,
                            QStringLiteral("updates for this installation are "
                                           "managed by Flatpak"));
    case UpdaterMode::LinuxSnap:
        return strategyFail(StrategyError::NotSelfInstallable,
                            QStringLiteral("updates for this installation are "
                                           "managed by Snap"));
    case UpdaterMode::MacosDmg:
        return strategyFail(StrategyError::UnsupportedPlatform,
                            QStringLiteral("macOS disk images are not installed by "
                                           "the helper"));
    case UpdaterMode::Development:
    case UpdaterMode::UnknownInstall:
    case UpdaterMode::Invalid:
        return strategyFail(StrategyError::NotSelfInstallable,
                            QStringLiteral("this installation cannot update itself"));
    }
    return strategyFail(StrategyError::NotSelfInstallable,
                        QStringLiteral("unhandled install type"));
}

} // namespace updater
