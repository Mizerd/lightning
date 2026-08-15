#pragma once

#include <QString>
#include <QStringView>

#include <functional>
#include <optional>

// Lightning secure update system — how THIS binary was installed.
//
// The install type decides whether an update can be applied by Lightning at
// all, and which compiled-in strategy the updater helper is asked to run.
// It is never taken from the network: the manifest cannot name a strategy,
// and nothing here reads a remote value.
//
// Detection priority, as implemented in detectInstall():
//   1. Runtime ecosystem evidence (Flatpak / Snap / AppImage) — the same
//      binary really IS running under that runtime, so it overrides the
//      compile-time value.
//   2. The Windows installer's `.lightning-install-type` marker, and ONLY on
//      Windows, and ONLY when it names one of the three Windows package
//      types. That marker exists for exactly one reason: the MSI, the setup
//      EXE and the portable ZIP are produced from ONE Windows build, so no
//      compile-time value can tell them apart. Everywhere else a concrete
//      compile-time value wins and a stray marker file is ignored — a marker
//      that could downgrade a genuine `linux-appimage` install to
//      `linux-deb` would provoke an unexpected PolicyKit prompt and install
//      the wrong package.
//   3. Compile-time LIGHTNING_INSTALL_TYPE when it names a concrete package
//      type (set by the packaging pipeline per package job).
//   4. LIGHTNING_INSTALL_TYPE_OVERRIDE env — TEST/DIAGNOSTIC ONLY. It never
//      enables automatic installation (the detection carries a flag saying
//      so, and installs are refused exactly as for a development build).
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

// Canonical wire ids (spec §5). These are the artifact keys in the manifest.
QString installTypeId(InstallType type);
// Human-readable label for the UI. Never used as a wire value.
QString installTypeLabel(InstallType type);
// Strict id -> enum. Unknown text yields nullopt (never Unknown-by-guess).
std::optional<InstallType> installTypeFromId(QStringView id);

// False for flatpak, snap, macos-dmg, development and unknown. A false here
// is a hard refusal: there is no override anywhere in the update API.
//
// macos-dmg is false because the updater helper has NO strategy for it
// (updater::isSelfInstallable refuses UpdaterMode::MacosDmg and planForMode
// returns UnsupportedPlatform). Advertising an automatic install that the
// helper would then refuse is a promise the product cannot keep, so the two
// sides are kept in step — and there is a test asserting exactly that.
bool canInstallAutomatically(InstallType type);

// True where another package manager owns updates for this installation.
// Repo-managed .deb/.rpm installs are NOT detected here — claiming that
// without evidence would be a guess, so those report false.
bool isPackageManaged(InstallType type);

// Injectable inputs so every detection branch is testable without setenv
// races. The defaults read the real process environment / filesystem.
struct InstallEnvironment {
    // Returns a null QString when the variable is unset.
    std::function<QString(const char *)> readEnv;
    std::function<bool(const QString &)> pathExists;
    // Compile-time LIGHTNING_INSTALL_TYPE; empty means "unset".
    QString compileTimeId;
    // Contents of the install marker written next to the executable by the
    // installer, or a null QString when there is none. This exists because the
    // MSI, the setup EXE and the portable ZIP are all produced from ONE
    // Windows build, so no compile-time value can tell them apart -- only the
    // thing that installed the files knows which of the three it was.
    //
    // That is its ONLY justification, so that is its only scope: the marker is
    // consulted only when `windowsPlatform` is true and only when it names one
    // of the three Windows package types. On every other platform the
    // compile-time value wins outright, because a marker is a file and files
    // travel -- one copied into an AppImage's directory must never turn it
    // into a .deb install and raise a PolicyKit prompt for the wrong package.
    std::function<QString()> readInstallMarker;

    // True when the running platform is Windows. Injectable so both the
    // marker-accepted and the marker-ignored branch are testable on one host.
    // defaultInstallEnvironment() sets the real value; a hand-built
    // environment defaults to false, i.e. "do not consult the marker".
    bool windowsPlatform = false;
};

// File name of that marker, written by the Windows installers into the
// installation directory. One line, one canonical install-type id.
inline constexpr char kInstallMarkerFileName[] = ".lightning-install-type";

InstallEnvironment defaultInstallEnvironment();

struct InstallDetection {
    InstallType type = InstallType::Unknown;
    // The diagnostic env override supplied the type. Automatic installation
    // stays refused in that case, exactly as for a development build.
    bool diagnosticOverride = false;
    // Convenience: canInstallAutomatically(type) && !diagnosticOverride.
    bool automaticInstallAllowed = false;
    // A development build may only CHECK when the opt-in env var is set.
    bool developmentCheckAllowed = false;
};

InstallDetection detectInstall(const InstallEnvironment &environment);
InstallDetection detectInstall();

InstallType detectInstallType(const InstallEnvironment &environment);
InstallType detectInstallType();

} // namespace lightning::update
