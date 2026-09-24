#include "updater/InstallStrategies.h"

#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace updater {

// NSIS MUI2. See the header for "/S" ordering and the omitted "/D=".
const QStringList kNsisSilentSwitches = {QStringLiteral("/S")};
const QString kNsisPerUserSwitch = QStringLiteral("/CURRENTUSER");
const QString kNsisPerMachineSwitch = QStringLiteral("/ALLUSERS");
const QString kMsiPerMachineProperty = QStringLiteral("ALLUSERS=1");

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
    // Absolute paths only, never PATH. The NixOS wrapper directory is where a
    // setuid pkexec lives there.
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
        // apt-get installs a local .deb by path and resolves its dependencies,
        // which dpkg cannot.
        return {managerPath, QStringLiteral("install"), QStringLiteral("-y"),
                QStringLiteral("--only-upgrade"), artifactPath};
    }
    if (tool == QLatin1String("dpkg")) {
        // No --force-*: a dpkg refusal is information.
        return {managerPath, QStringLiteral("-i"), artifactPath};
    }
    if (tool == QLatin1String("dnf5") || tool == QLatin1String("dnf")) {
        return {managerPath, QStringLiteral("install"), QStringLiteral("-y"),
                artifactPath};
    }
    if (tool == QLatin1String("rpm-ostree")) {
        // rpm-ostree layers into the next deployment and takes no -y.
        return {managerPath, QStringLiteral("install"), artifactPath};
    }
    if (tool == QLatin1String("rpm")) {
        // -U upgrades (installing if absent). Never --force, never --nodeps.
        return {managerPath, QStringLiteral("-U"), artifactPath};
    }
    return QStringList();
}

// msiexec reads '/' as the start of a switch, so a Qt-style
// `/i C:/Users/.../Lightning.msi` fails with 1619
// (ERROR_INSTALL_PACKAGE_OPEN_FAILED). Converted unconditionally: these plans
// only run on Windows, and this keeps them testable on Linux.
QString windowsNativePath(const QString &path)
{
    QString native = path;
    native.replace(QLatin1Char('/'), QLatin1Char('\\'));
    return native;
}

QString windowsSystemExecutable(const QString &executableName)
{
#ifdef Q_OS_WIN
    wchar_t system[MAX_PATH];
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
        return QString::fromWCharArray(system, int(length)) + QLatin1Char('\\')
            + executableName;
#endif
    QString root = qEnvironmentVariable("SystemRoot");
    if (root.isEmpty())
        root = qEnvironmentVariable("windir");
    if (root.isEmpty())
        root = QStringLiteral("C:\\Windows");
    while (root.endsWith(QLatin1Char('\\')) || root.endsWith(QLatin1Char('/')))
        root.chop(1);
    return root + QStringLiteral("\\System32\\") + executableName;
}

QString windowsCommandLine(const QStringList &arguments, bool *ok)
{
    if (ok)
        *ok = false;
    QStringList quoted;
    for (const QString &argument : arguments) {
        for (const QChar c : argument) {
            if (c == QLatin1Char('"') || c.unicode() < 0x20 || c.unicode() == 0x7f)
                return QString();
        }
        // Tabs are control characters, refused above.
        const bool needsQuotes = argument.isEmpty() || argument.contains(QLatin1Char(' '));
        if (!needsQuotes) {
            quoted << argument;
            continue;
        }
        // Inside quotes, only a backslash run right before the closing quote is
        // halved by CommandLineToArgvW; double just that run.
        QString element = argument;
        int trailing = 0;
        while (trailing < element.size()
               && element.at(element.size() - 1 - trailing) == QLatin1Char('\\'))
            ++trailing;
        element.append(QString(trailing, QLatin1Char('\\')));
        quoted << QLatin1Char('"') + element + QLatin1Char('"');
    }
    if (ok)
        *ok = true;
    return quoted.join(QLatin1Char(' '));
}

QString launchablePathFromFinal(const QString &finalPath)
{
    static const QString kUnc = QStringLiteral("\\\\?\\UNC\\");
    static const QString kLocal = QStringLiteral("\\\\?\\");
    QString path;
    if (finalPath.startsWith(kUnc, Qt::CaseInsensitive))
        path = QStringLiteral("\\\\") + finalPath.mid(kUnc.size());
    else if (finalPath.startsWith(kLocal))
        path = finalPath.mid(kLocal.size());
    else
        path = finalPath;
    // Must be a drive path ("C:\...") or a UNC path with server and share;
    // never a device or volume path, nor anything still prefixed "\\?\" or
    // "\\.\".
    const bool drive = path.size() >= 4 && path.at(0).isLetter()
        && path.at(1) == QLatin1Char(':') && path.at(2) == QLatin1Char('\\');
    const QStringList uncParts = path.mid(2).split(QLatin1Char('\\'));
    const bool unc = path.startsWith(QStringLiteral("\\\\"))
        && !path.startsWith(QStringLiteral("\\\\?"))
        && !path.startsWith(QStringLiteral("\\\\."))
        && uncParts.size() >= 3 && !uncParts.at(0).isEmpty() && !uncParts.at(1).isEmpty();
    if (!drive && !unc)
        return QString();
    if (path.contains(QLatin1Char('"')) || path.contains(QLatin1Char('/')))
        return QString();
    return path;
}

bool retargetPlanToLockedFile(InstallPlan &plan, const QString &launchablePath)
{
    if (plan.lockedArtifact.isEmpty() || launchablePath.isEmpty())
        return false;
    InstallPlan rewritten = plan;
    bool found = false;
    if (rewritten.program == plan.lockedArtifact) {
        rewritten.program = launchablePath;
        // The parent, still a directory: "C:\x.exe" -> "C:\".
        QString parent = launchablePath.left(launchablePath.lastIndexOf(QLatin1Char('\\')));
        if (parent.size() == 2 && parent.at(1) == QLatin1Char(':'))
            parent += QLatin1Char('\\');
        rewritten.workingDirectory = parent;
        found = true;
    }
    for (QString &argument : rewritten.arguments) {
        if (argument == plan.lockedArtifact) {
            argument = launchablePath;
            found = true;
        }
    }
    if (!found)
        return false;
    rewritten.lockedArtifact = launchablePath;
    plan = rewritten;
    return true;
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
    // /i installs or upgrades; /qb shows a basic progress UI (the user asked
    // for this and should see it); REINSTALLMODE=vomus re-caches every file so
    // a same-version repair replaces files. Per-machine: see the header.
    result.plan.arguments = {QStringLiteral("/i"),
                             windowsNativePath(args.artifactPath),
                             QStringLiteral("/qb"),
                             QStringLiteral("REINSTALLMODE=vomus")};
    if (args.installScope == InstallScope::Machine) {
        result.plan.arguments << kMsiPerMachineProperty;
        result.plan.elevateOnWindows = true;
        result.plan.lockedArtifact = windowsNativePath(args.artifactPath);
    }
    return result;
}

StrategyResult planWindowsSetup(const UpdaterArguments &args)
{
    if (args.artifactPath.isEmpty())
        return strategyFail(StrategyError::InvalidArtifact,
                            QStringLiteral("no artifact path"));

    StrategyResult result;
    result.plan.requiresExternalProcess = true;
    // The artifact is the program: an absolute path the argv validator proved
    // is a regular file. Made native for consistency with the MSI plan.
    result.plan.program = windowsNativePath(args.artifactPath);
    result.plan.arguments = kNsisSilentSwitches; // "/S" must come first
    // Always explicit: with both a per-user and a per-machine copy present,
    // only the helper knows which is being upgraded.
    if (args.installScope == InstallScope::Machine) {
        result.plan.arguments << kNsisPerMachineSwitch;
        result.plan.elevateOnWindows = true;
        // The program is the artifact, so the locked file is what Windows runs.
        result.plan.lockedArtifact = result.plan.program;
    } else {
        result.plan.arguments << kNsisPerUserSwitch;
    }
    result.plan.workingDirectory =
        windowsNativePath(QFileInfo(args.artifactPath).absolutePath());
    return result;
}

StrategyResult planWindowsPortable(const UpdaterArguments &)
{
    // In-process: extract into private staging, validate the layout, then swap
    // directories with rollback.
    StrategyResult result;
    result.plan.requiresExternalProcess = false;
    return result;
}

StrategyResult planLinuxAppImage(const UpdaterArguments &)
{
    // In-process: chmod +x the staged AppImage, then replace atomically with
    // rollback.
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
    // pkexec's argv: the program and its arguments. Never sudo or a shell.
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
