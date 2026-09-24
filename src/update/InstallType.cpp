#include "update/InstallType.h"
#include "storage/PortableMode.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLatin1String>
#include <QSettings>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifndef LIGHTNING_INSTALL_TYPE
// Nothing was baked in: this is a source/development build.
#define LIGHTNING_INSTALL_TYPE "development"
#endif

namespace lightning::update {
namespace {

struct InstallTypeEntry {
    InstallType type;
    const char *id;
    const char *label;
};

// Ids are the manifest's artifact keys and the helper's --mode values.
constexpr InstallTypeEntry kInstallTypes[] = {
    { InstallType::WindowsMsi, "windows-msi", "Windows installer (MSI)" },
    { InstallType::WindowsSetup, "windows-setup", "Windows setup" },
    { InstallType::WindowsPortable, "windows-portable", "Windows portable" },
    { InstallType::LinuxAppImage, "linux-appimage", "AppImage" },
    { InstallType::LinuxDeb, "linux-deb", "Debian package" },
    { InstallType::LinuxRpm, "linux-rpm", "RPM package" },
    { InstallType::LinuxFlatpak, "linux-flatpak", "Flatpak" },
    { InstallType::LinuxSnap, "linux-snap", "Snap" },
    { InstallType::MacosDmg, "macos-dmg", "macOS disk image" },
    { InstallType::Development, "development", "Development build" },
    { InstallType::Unknown, "unknown", "Unknown installation" },
};

const InstallTypeEntry *entryFor(InstallType type)
{
    for (const InstallTypeEntry &entry : kInstallTypes) {
        if (entry.type == type)
            return &entry;
    }
    return nullptr;
}

bool isConcretePackageType(InstallType type)
{
    return type != InstallType::Development && type != InstallType::Unknown;
}

// The three Windows packages built from one tree: the only values the install
// marker may select.
bool isWindowsPackageType(InstallType type)
{
    return type == InstallType::WindowsMsi || type == InstallType::WindowsSetup
        || type == InstallType::WindowsPortable;
}

QString envValue(const InstallEnvironment &environment, const char *name)
{
    if (!environment.readEnv)
        return {};
    return environment.readEnv(name);
}

// Long form of a path, so an 8.3 short name cannot defeat the HKLM comparison.
// Unchanged when unresolvable or off Windows.
QString longPathName(const QString &path)
{
#ifdef Q_OS_WIN
    const std::wstring in = QDir::toNativeSeparators(path).toStdWString();
    const DWORD needed = GetLongPathNameW(in.c_str(), nullptr, 0);
    if (needed > 0) {
        std::wstring out(needed, L'\0');
        const DWORD written = GetLongPathNameW(in.c_str(), out.data(), needed);
        if (written > 0 && written < needed)
            return QString::fromWCharArray(out.data(), int(written));
    }
#endif
    return path;
}

bool envIsSet(const InstallEnvironment &environment, const char *name)
{
    const QString value = envValue(environment, name);
    return !value.isEmpty();
}

} // namespace

QString installScopeId(InstallScope scope)
{
    return scope == InstallScope::Machine ? QStringLiteral("machine")
                                          : QStringLiteral("user");
}

QString installTypeId(InstallType type)
{
    if (const InstallTypeEntry *entry = entryFor(type))
        return QString::fromLatin1(entry->id);
    return QStringLiteral("unknown");
}

QString installTypeLabel(InstallType type)
{
    if (const InstallTypeEntry *entry = entryFor(type))
        return QString::fromLatin1(entry->label);
    return QStringLiteral("Unknown installation");
}

std::optional<InstallType> installTypeFromId(QStringView id)
{
    for (const InstallTypeEntry &entry : kInstallTypes) {
        if (id == QLatin1String(entry.id))
            return entry.type;
    }
    return std::nullopt;
}

bool canInstallAutomatically(InstallType type)
{
    switch (type) {
    case InstallType::LinuxFlatpak:
    case InstallType::LinuxSnap:
    case InstallType::Development:
    case InstallType::Unknown:
        return false;
    // The helper has no macOS strategy.
    case InstallType::MacosDmg:
        return false;
    case InstallType::WindowsMsi:
    case InstallType::WindowsSetup:
    case InstallType::WindowsPortable:
    case InstallType::LinuxAppImage:
    case InstallType::LinuxDeb:
    case InstallType::LinuxRpm:
        return true;
    }
    return false;
}

bool isPackageManaged(InstallType type)
{
    return type == InstallType::LinuxFlatpak || type == InstallType::LinuxSnap;
}

bool fileLooksLikeAppImage(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = file.read(11);
    if (head.size() < 11)
        return false;
    // ELF magic, then the AppImage signature at offset 8: 'A' 'I' <type>.
    return head.startsWith(QByteArrayLiteral("\x7f" "ELF"))
        && head.at(8) == 'A' && head.at(9) == 'I'
        && (head.at(10) == 0x01 || head.at(10) == 0x02);
}

InstallEnvironment defaultInstallEnvironment()
{
    InstallEnvironment environment;
    environment.readEnv = [](const char *name) -> QString {
        if (!qEnvironmentVariableIsSet(name))
            return {};
        return qEnvironmentVariable(name);
    };
    environment.pathExists = [](const QString &path) { return QFileInfo::exists(path); };
    environment.looksLikeAppImage = [](const QString &path) {
        return fileLooksLikeAppImage(path);
    };
    environment.compileTimeId = QString::fromLatin1(LIGHTNING_INSTALL_TYPE);
    environment.portableMarkerPresent = []() {
        return QFileInfo::exists(QCoreApplication::applicationDirPath()
                                 + QLatin1Char('/')
                                 + QLatin1String(lightning::portable::kMarkerFileName));
    };
    environment.readInstallMarker = []() -> QString {
        // Only beside the running executable, never a search path.
        const QString path = QCoreApplication::applicationDirPath()
                + QLatin1Char('/') + QLatin1String(kInstallMarkerFileName);
        QFile marker(path);
        if (!marker.open(QIODevice::ReadOnly | QIODevice::Text))
            return {};
        // Canonical ids are short; never read a large file.
        return QString::fromLatin1(marker.read(64)).trimmed();
    };
    environment.readInstallScopeMarker = []() -> QString {
        const QString path = QCoreApplication::applicationDirPath()
                + QLatin1Char('/') + QLatin1String(kInstallScopeMarkerFileName);
        QFile marker(path);
        if (!marker.open(QIODevice::ReadOnly | QIODevice::Text))
            return {};
        return QString::fromLatin1(marker.read(64)).trimmed();
    };
    environment.applicationDir = longPathName(QCoreApplication::applicationDirPath());
    environment.readMachineInstallDirs = []() -> QStringList {
        QStringList dirs;
#ifdef Q_OS_WIN
        // 64-bit view: both installers are x64.
        const QSettings machine(QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\Mizerd\\Lightning"),
                                QSettings::Registry64Format);
        for (const char *name : {"InstallDir", "MsiInstallDir"}) {
            const QString dir = machine.value(QLatin1String(name)).toString();
            if (!dir.isEmpty())
                dirs << longPathName(dir);
        }
#endif
        return dirs;
    };
#ifdef Q_OS_WIN
    environment.windowsPlatform = true;
#else
    environment.windowsPlatform = false;
#endif
    return environment;
}

bool sameInstallDirectory(const QString &registered, const QString &applicationDir)
{
    const auto normalise = [](QString path) {
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));
        path = QDir::cleanPath(path);
        while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
            path.chop(1);
        return path;
    };
    if (registered.trimmed().isEmpty() || applicationDir.isEmpty())
        return false;
    return normalise(registered.trimmed()).compare(normalise(applicationDir),
                                                   Qt::CaseInsensitive) == 0;
}

InstallDetection detectInstall(const InstallEnvironment &environment)
{
    InstallDetection detection;

    const auto pathExists = [&environment](const QString &path) {
        return environment.pathExists ? environment.pathExists(path) : false;
    };

    // 1. Runtime ecosystem evidence.
    std::optional<InstallType> runtimeType;
    if (envIsSet(environment, "FLATPAK_ID") || pathExists(QStringLiteral("/.flatpak-info"))) {
        runtimeType = InstallType::LinuxFlatpak;
    } else if (envIsSet(environment, "SNAP") && envIsSet(environment, "SNAP_NAME")) {
        runtimeType = InstallType::LinuxSnap;
    } else {
        // $APPIMAGE can be set by whatever starts the process, and it becomes
        // the file the updater replaces, so accept it only for an actual
        // AppImage.
        const QString appImage = envValue(environment, "APPIMAGE");
        if (!appImage.isEmpty() && pathExists(appImage) && environment.looksLikeAppImage
            && environment.looksLikeAppImage(appImage)) {
            runtimeType = InstallType::LinuxAppImage;
        }
    }

    const std::optional<InstallType> compileTime =
        environment.compileTimeId.isEmpty() ? std::optional<InstallType>(InstallType::Development)
                                            : installTypeFromId(environment.compileTimeId);

    // 2. The Windows installer's marker: only when no runtime claimed the
    // process, only on Windows, and only for the Windows package types.
    std::optional<InstallType> markerType;
    if (!runtimeType && environment.windowsPlatform && environment.readInstallMarker) {
        const QString marker = environment.readInstallMarker().trimmed();
        if (!marker.isEmpty()) {
            if (const std::optional<InstallType> parsed = installTypeFromId(marker);
                parsed && isConcretePackageType(*parsed) && isWindowsPackageType(*parsed)) {
                markerType = parsed;
            }
        }
    }

    if (runtimeType) {
        detection.type = *runtimeType;
    } else if (markerType) {
        detection.type = *markerType;
    } else if (compileTime && isConcretePackageType(*compileTime)) {
        // 3. Explicit build metadata from the packaging pipeline.
        detection.type = *compileTime;
    } else {
        // 4. Diagnostic override; never authorizes installation.
        const QString overrideId = envValue(environment, "LIGHTNING_INSTALL_TYPE_OVERRIDE");
        const std::optional<InstallType> overrideType =
            overrideId.isEmpty() ? std::nullopt : installTypeFromId(overrideId);
        if (overrideType) {
            detection.type = *overrideType;
            detection.diagnosticOverride = true;
        } else if (compileTime) {
            // 5. Compile-time fallback (development when unset).
            detection.type = *compileTime;
        } else {
            // An unrecognised compile-time value is Unknown, not development.
            detection.type = InstallType::Unknown;
        }
    }

    // Portable must be proven, never reached by fallback. An installed copy
    // whose type marker is missing (NSIS writes it unchecked) would otherwise
    // get the portable strategy, which swaps the installer-owned directory,
    // breaking the uninstaller and relocating the data root. Without
    // `portable.marker`, report Unknown: offer the update but do not apply it.
    if (detection.type == InstallType::WindowsPortable
        && environment.windowsPlatform
        && environment.portableMarkerPresent
        && !environment.portableMarkerPresent()) {
        detection.type = InstallType::Unknown;
    }

    // Scope, for the two installer-owned types only. "machine" requires HKLM to
    // name this directory (the marker is user-writable; see InstallType.h).
    if (environment.windowsPlatform && environment.readInstallScopeMarker
        && (detection.type == InstallType::WindowsMsi
            || detection.type == InstallType::WindowsSetup)
        && environment.readInstallScopeMarker().trimmed() == QLatin1String("machine")
        && environment.readMachineInstallDirs) {
        for (const QString &registered : environment.readMachineInstallDirs()) {
            if (sameInstallDirectory(registered, environment.applicationDir)) {
                detection.scope = InstallScope::Machine;
                break;
            }
        }
    }

    detection.automaticInstallAllowed =
        canInstallAutomatically(detection.type) && !detection.diagnosticOverride;
    detection.developmentCheckAllowed =
        envValue(environment, "LIGHTNING_ALLOW_DEV_UPDATE_CHECK") == QLatin1String("1");
    return detection;
}

InstallDetection detectInstall()
{
    return detectInstall(defaultInstallEnvironment());
}

InstallType detectInstallType(const InstallEnvironment &environment)
{
    return detectInstall(environment).type;
}

InstallType detectInstallType()
{
    return detectInstall().type;
}

} // namespace lightning::update
