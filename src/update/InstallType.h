#pragma once

#include <QString>
#include <QStringList>
#include <QStringView>

#include <functional>
#include <optional>

// How this binary was installed. Decides whether Lightning may apply an update
// and which compiled-in helper strategy runs. Never taken from the network.
//
// Detection priority (detectInstall()):
//   1. Runtime ecosystem evidence (Flatpak / Snap / AppImage).
//   2. The Windows installer's `.lightning-install-type` marker, only on
//      Windows and only naming one of the three Windows package types: the
//      MSI, setup EXE and portable ZIP come from one build, so no compile-time
//      value can tell them apart. Elsewhere a stray marker is ignored, since
//      it could turn an AppImage into a "deb" and raise a wrong PolicyKit
//      prompt.
//   3. Compile-time LIGHTNING_INSTALL_TYPE naming a concrete package type.
//   4. LIGHTNING_INSTALL_TYPE_OVERRIDE, test/diagnostic only; it never enables
//      automatic installation.
//   5. Otherwise the compile-time value; `development` when unset.
namespace lightning::update {

enum class InstallType {
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
    Unknown,
};

enum class InstallScope {
    User,
    Machine,
};

QString installScopeId(InstallScope scope);

QString installTypeId(InstallType type);
QString installTypeLabel(InstallType type);
// Strict id -> enum. Unknown text yields nullopt (never Unknown-by-guess).
std::optional<InstallType> installTypeFromId(QStringView id);

// False for flatpak, snap, linux-rpm-repo, macos-dmg, development and unknown;
// a hard refusal with no override. macos-dmg is false because the helper has
// no strategy for it; a test keeps the two sides in step.
bool canInstallAutomatically(InstallType type);

bool isPackageManaged(InstallType type);

QString packageManagerName(InstallType type);

QString repositoryRpmManager(const std::function<bool(const QString &)> &exists = {});

struct InstallEnvironment {
    std::function<QString(const char *)> readEnv;
    std::function<bool(const QString &)> pathExists;
    QString compileTimeId;
    std::function<QString()> readInstallMarker;
    std::function<QString()> readInstallScopeMarker;
    std::function<QStringList()> readMachineInstallDirs;
    QString applicationDir;
    std::function<bool(const QString &)> looksLikeAppImage;
    std::function<bool()> portableMarkerPresent;

    bool windowsPlatform = false;
};

bool fileLooksLikeAppImage(const QString &path);

inline constexpr char kInstallMarkerFileName[] = ".lightning-install-type";
inline constexpr char kInstallScopeMarkerFileName[] = ".lightning-install-scope";

InstallEnvironment defaultInstallEnvironment();

bool sameInstallDirectory(const QString &registered, const QString &applicationDir);

struct InstallDetection {
    InstallType type = InstallType::Unknown;
    bool diagnosticOverride = false;
    bool automaticInstallAllowed = false;
    bool developmentCheckAllowed = false;
    InstallScope scope = InstallScope::User;
};

InstallDetection detectInstall(const InstallEnvironment &environment);
InstallDetection detectInstall();

InstallType detectInstallType(const InstallEnvironment &environment);
InstallType detectInstallType();

}
