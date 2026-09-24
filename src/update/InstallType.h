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
    MacosDmg,
    Development,
    Unknown,
};

// Who a Windows MSI/setup installation belongs to; User for everything else.
// A Machine installation (Program Files, HKLM) must be upgraded elevated in
// the same context, or the MSI installs a second copy (FindRelatedProducts
// only sees its own context).
enum class InstallScope {
    User,
    Machine,
};

// "user" / "machine": the scope marker value and the helper's
// --install-scope value.
QString installScopeId(InstallScope scope);

// Canonical wire ids (spec §5). These are the artifact keys in the manifest.
QString installTypeId(InstallType type);
// Human-readable label for the UI. Never used as a wire value.
QString installTypeLabel(InstallType type);
// Strict id -> enum. Unknown text yields nullopt (never Unknown-by-guess).
std::optional<InstallType> installTypeFromId(QStringView id);

// False for flatpak, snap, macos-dmg, development and unknown; a hard refusal
// with no override. macos-dmg is false because the helper has no strategy for
// it; a test keeps the two sides in step.
bool canInstallAutomatically(InstallType type);

// True where another package manager owns updates. Repo-managed .deb/.rpm
// installs cannot be detected and report false.
bool isPackageManaged(InstallType type);

// Injectable inputs so every branch is testable without setenv races.
// Defaults read the real environment and filesystem.
struct InstallEnvironment {
    // Returns a null QString when the variable is unset.
    std::function<QString(const char *)> readEnv;
    std::function<bool(const QString &)> pathExists;
    // Compile-time LIGHTNING_INSTALL_TYPE; empty means "unset".
    QString compileTimeId;
    // Contents of the install marker beside the executable, or null. Only
    // consulted on Windows and only when it names a Windows package type (see
    // detection step 2).
    std::function<QString()> readInstallMarker;
    // Contents of `.lightning-install-scope` beside the executable, or null.
    // Consulted only on Windows for MSI/setup; anything but "machine"
    // (including no marker) means User. "machine" also requires
    // readMachineInstallDirs() to name this directory: the marker is
    // user-writable in a per-user install, and a spoofed value would make every
    // update raise a UAC prompt for a program malware chose.
    std::function<QString()> readInstallScopeMarker;
    // Per-machine directories recorded under HKLM\Software\Mizerd\Lightning
    // ("InstallDir" for setup, "MsiInstallDir" for MSI). Empty off Windows or
    // when unset.
    std::function<QStringList()> readMachineInstallDirs;
    // The directory the running executable is in, compared against the above.
    QString applicationDir;
    // Whether `path` carries the AppImage magic (ELF with "AI" + type 1 or 2 at
    // offset 8). $APPIMAGE becomes the file the strategy chmods and replaces,
    // so it is accepted only for an actual AppImage. Unset accepts none.
    std::function<bool(const QString &)> looksLikeAppImage;
    // Whether `portable.marker` is beside the executable. Portable is the
    // compiled-in default for all Windows packages, and its strategy swaps the
    // installation directory; an installed copy missing its type marker must
    // not fall through to it and break its uninstaller.
    std::function<bool()> portableMarkerPresent;

    // Injectable so both marker branches are testable on one host. Unset means
    // "do not consult the marker".
    bool windowsPlatform = false;
};

// Reports whether `path` starts with the AppImage signature. Public for tests.
bool fileLooksLikeAppImage(const QString &path);

// Written by the Windows installers into the installation directory: one line,
// one install-type id.
inline constexpr char kInstallMarkerFileName[] = ".lightning-install-type";
// Written by the NSIS script and the WiX generator.
inline constexpr char kInstallScopeMarkerFileName[] = ".lightning-install-scope";

InstallEnvironment defaultInstallEnvironment();

// True when a registry-recorded directory names `applicationDir`, ignoring
// separators, a trailing separator and case. Empty never matches.
bool sameInstallDirectory(const QString &registered, const QString &applicationDir);

struct InstallDetection {
    InstallType type = InstallType::Unknown;
    // The type came from the diagnostic override; installation stays refused.
    bool diagnosticOverride = false;
    // Convenience: canInstallAutomatically(type) && !diagnosticOverride.
    bool automaticInstallAllowed = false;
    // A development build may only CHECK when the opt-in env var is set.
    bool developmentCheckAllowed = false;
    // Machine only when the scope marker says so and HKLM registers this
    // directory.
    InstallScope scope = InstallScope::User;
};

InstallDetection detectInstall(const InstallEnvironment &environment);
InstallDetection detectInstall();

InstallType detectInstallType(const InstallEnvironment &environment);
InstallType detectInstallType();

} // namespace lightning::update
