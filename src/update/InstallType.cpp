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

// Canonical table. The ids are the artifact keys in the update manifest and
// the --mode values in the updater helper's argv contract.
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

// The three Windows packages that share ONE build. They are the entire
// reason the install marker exists, and therefore the entire set of values
// the marker is allowed to select.
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

// The long form of an existing path, so an 8.3 short name ("PROGRA~1") on
// either side of the HKLM comparison cannot make a genuine per-machine copy
// read as per-user. Unchanged when it cannot be resolved or off Windows.
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
    // The updater helper has no macOS strategy: planForMode(macos-dmg)
    // returns UnsupportedPlatform and isSelfInstallable() is false. Offering
    // an automatic install here would advertise something the helper refuses.
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
        // Beside the running executable, never a search path: the marker
        // describes THIS installation or it is not consulted at all.
        const QString path = QCoreApplication::applicationDirPath()
                + QLatin1Char('/') + QLatin1String(kInstallMarkerFileName);
        QFile marker(path);
        if (!marker.open(QIODevice::ReadOnly | QIODevice::Text))
            return {};
        // A canonical id is short; refuse to read an arbitrarily large file.
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
        // The 64-bit view explicitly: both installers are x64 and write there.
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

    // 1. Runtime ecosystem evidence wins: the binary genuinely runs there.
    std::optional<InstallType> runtimeType;
    if (envIsSet(environment, "FLATPAK_ID") || pathExists(QStringLiteral("/.flatpak-info"))) {
        runtimeType = InstallType::LinuxFlatpak;
    } else if (envIsSet(environment, "SNAP") && envIsSet(environment, "SNAP_NAME")) {
        runtimeType = InstallType::LinuxSnap;
    } else {
        // The AppImage runtime sets $APPIMAGE to the image it mounted. The
        // variable is also settable by anything that starts the process, and
        // it becomes the updater's --target: the file that gets chmod +x and
        // replaced. So the claim is accepted only for a file that at least
        // IS an AppImage. Same-user only either way; this keeps a stray
        // environment from turning an update into "replace that file".
        const QString appImage = envValue(environment, "APPIMAGE");
        if (!appImage.isEmpty() && pathExists(appImage) && environment.looksLikeAppImage
            && environment.looksLikeAppImage(appImage)) {
            runtimeType = InstallType::LinuxAppImage;
        }
    }

    const std::optional<InstallType> compileTime =
        environment.compileTimeId.isEmpty() ? std::optional<InstallType>(InstallType::Development)
                                            : installTypeFromId(environment.compileTimeId);

    // 2. The Windows installer's own marker -- and nothing else. It is
    // consulted only when no runtime ecosystem claimed the process, only on
    // Windows, and only for the three Windows package types it exists to
    // distinguish. On Linux and macOS a concrete compile-time value wins, so a
    // stray or copied marker file cannot flip a genuine linux-appimage install
    // to linux-deb and hand the user an unexpected PolicyKit prompt for a
    // package that was never installed.
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
        // 4. Diagnostic override — never an installation authorisation.
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
            // A compile-time value we do not recognise is reported as
            // unknown rather than assumed to be a development build.
            detection.type = InstallType::Unknown;
        }
    }

    // PORTABLE MUST BE PROVEN, NEVER REACHED BY FALLBACK.
    //
    // windows-portable is the compiled-in value for all three Windows
    // packages, so it is what an installed copy lands on whenever its
    // `.lightning-install-type` marker is missing -- and the NSIS script
    // writes that marker without checking whether the write succeeded. The
    // portable strategy would then swap the directory an installer owns,
    // moving `.lightning-install-root` into the backup (so the uninstaller
    // refuses forever) and relocating the user's data root (so they appear
    // signed out). The other two strategies hand the work to an installer
    // and cannot do that.
    //
    // A portable copy always carries `portable.marker`; the installed
    // packages never do. Require it, and when it is absent report Unknown,
    // which offers the update and declines to APPLY it rather than applying
    // the wrong one.
    if (detection.type == InstallType::WindowsPortable
        && environment.windowsPlatform
        && environment.portableMarkerPresent
        && !environment.portableMarkerPresent()) {
        detection.type = InstallType::Unknown;
    }

    // THE SCOPE, for the two installer-owned Windows types only. Portable has
    // no installer to own a context, and on any other platform the marker is
    // a stray file (the same reasoning as the install-type marker above).
    // "machine" must be backed by HKLM naming this directory: the marker is
    // user-writable in a per-user installation (see InstallType.h).
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
