# Installing Lightning

Packages are attached to the
[**Releases**](https://gitlab.smetonis.net/Mizerd/lightning/-/releases) page and
mirrored to [GitHub Releases](https://github.com/Mizerd/lightning/releases). Every
release ships a `SHA256SUMS` file; verifying is worth the one command, because no
package is code-signed yet:

```sh
sha256sum -c SHA256SUMS --ignore-missing
```

Replace `0.10.0` below with the version you downloaded.

- [Linux](#linux)
- [NixOS](#nixos)
- [Windows](#windows-x86-64-windows-10-or-later)
- [macOS](#macos-apple-silicon-macos-26-or-newer)
- [Afterwards](#afterwards)

## Linux

**On an older distribution, install from
[Flathub](https://flathub.org/apps/org.lightning_matrix.Lightning).** The
Flatpak brings its own Qt, GStreamer and C library, so the host's versions do
not matter.

The deb and the rpm are each built against a recent Qt, and their declared
dependencies say so. **If your distribution ships an older Qt the package will
refuse to install rather than half-work.** The AppImage carries its own Qt but
uses the host's C and C++ runtime, so it needs a recent distribution too.

There is one rpm, for **Fedora and openSUSE Tumbleweed** alike. It asks for its
GStreamer plugins, QML modules and image plugins by what they provide rather
than by Fedora's package names, and its QML is compiled to bytecode only, so it
uses nothing from Qt's private interface: Fedora and openSUSE version that
interface differently, and a single such symbol would make the binary refuse
to start on the other one.

There are two debs. `lightning_<version>_amd64.deb` is built on Debian 13 and
`lightning_<version>_ubuntu2604_amd64.deb` on Ubuntu 26.04: `dpkg-shlibdeps`
writes the build host's library versions into a deb's dependencies, so each one
installs only on the distribution it was built for, or a newer one.

| package | needs | known good | known to FAIL |
|---|---|---|---|
| Flatpak | flatpak, and Flathub for the KDE 6.11 runtime | Debian 12 (its own flatpak 1.14.10) | — |
| `.deb` | Qt >= 6.8.2, GStreamer >= 1.26.2, glibc >= 2.38, `QtQuick.Effects` (Qt 6.5+) | Debian 13 | **Debian 12** (Qt 6.4.2, glibc 2.36); **Ubuntu 24.04 LTS** (Qt 6.4.2), and its derivatives — Mint 22.x, Pop!_OS 24.04 |
| `.deb` (`_ubuntu2604_`) | the library versions of Ubuntu 26.04, which it is built on | Ubuntu 26.04 | not measured elsewhere |
| `.rpm` | Qt >= 6.11 | Fedora 44, Fedora 45, openSUSE Tumbleweed | **Fedora 43** (Qt 6.10.3); **openSUSE Leap 16.0** (Qt 6.9.1) |
| AppImage | glibc >= 2.39, libstdc++ from GCC 14 or newer | Debian 13, Ubuntu 24.04 | **Debian 12** (glibc 2.36: `GLIBC_2.38 not found`), and so Ubuntu 22.04 and Mint 21.x |
| snap | snapd | Debian 12 (snapd 2.57.6, which updates itself on first install) | — |

The deb and rpm rows are measured on a real installation of each distribution;
the Ubuntu 26.04 deb was installed with `apt` in a clean guest, signed in and
synced (2026-09-12, `docs/open-items.md`). The Debian 12 results, and the
AppImage's Ubuntu 24.04 result, are measured in containers of those releases:
the deb refused by `apt`, the AppImage stopping at the dynamic loader on
Debian 12, and the Flatpak and the snap starting and passing their built-in
checks. Ubuntu 22.04 and Mint 21.x are inferred from their glibc (2.35), not
run. The rpm on Fedora 45 and openSUSE Tumbleweed (snapshot 20260924) was
installed, started without a display and asked for its call engine and image
decoders in containers (2026-09-30), not used with an account; every release
pipeline now repeats that on Tumbleweed. openSUSE Leap 16.0 refuses it at
install, because its Qt is older than the one the rpm was built with: use the
Flatpak there. RHEL is **untested**; the Qt 6.11 floor makes any current RHEL
unlikely to satisfy it.

```sh
sudo apt install ./lightning_0.10.0_amd64.deb            # Debian 13+
sudo apt install ./lightning_0.10.0_ubuntu2604_amd64.deb # Ubuntu 26.04+
sudo dnf install ./lightning-0.10.0-1.x86_64.rpm         # Fedora 44+
sudo zypper install --allow-unsigned-rpm ./lightning-0.10.0-1.x86_64.rpm  # openSUSE Tumbleweed

# Keep the version in the pattern: only the suffix is globbed, because some
# browsers lower-case .AppImage. Lightning-* would match two downloads and run
# the older one with the newer as its argument.
chmod +x Lightning-0.10.0-x86_64.*pp[Ii]mage && ./Lightning-0.10.0-x86_64.*pp[Ii]mage

flatpak remote-add --if-not-exists --user flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user flathub org.lightning_matrix.Lightning   # from Flathub
flatpak install --user ./lightning_0.10.0_amd64.flatpak          # or the release file; it fetches its runtime itself
flatpak run org.lightning_matrix.Lightning

sudo snap install --dangerous ./lightning_0.10.0_amd64.snap
```

The leading `./` matters for `apt`, `dnf` and `zypper`, or they look for a
package by that name in your repositories. zypper asks for
`--allow-unsigned-rpm` because the rpm is not signed; check it against
`SHA256SUMS` first. The AppImage installs nothing — delete the file to
remove it; if it will not start you may need FUSE, or run it with
`--appimage-extract-and-run`. If it stops with `GLIBC_2.38' not found`, the
distribution is older than the AppImage supports: use the Flatpak. The snap is
not published to the Snap Store, so
`--dangerous` means "this file is not signed by the store", not that the snap is
unsafe; it is built with `strict` confinement.

## NixOS

If you want to use it without installing:

```sh
nix run github:Mizerd/lightning
```

**Installing using flakes**:

Add lightning-matrix-client as an input:

```nix
{
  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-unstable";
    lightning-matrix-client = {
      url = "github:Mizerd/lightning";
      #url = "github:Mizerd/lightning/v0.10.0"; # Use this if you want a specific version
    };
  };
  . . . # Your outputs config
}
```

Add the package from the lightning-matrix-client input:

```nix
{ inputs, pkgs, ... }:
{
  environment.systemPackages = [
    inputs.lightning-matrix-client.packages.${pkgs.stdenv.hostPlatform.system}.default
  ];
}
```

Optionally, the flake also provides a `homeManagerModules` output with settings
(you don't need to add the package to `environment.systemPackages` if using this method):

```nix
# This is a module imported inside a home manager (https://github.com/nix-community/home-manager) configuration
{ inputs, ... }:
{
  imports = [
    inputs.lightning-matrix-client.homeManagerModules.default
  ];
  lightning-matrix-client.enable = true;
}
```

## Windows (x86-64, Windows 10 or later)

Windows packages ship with every release from v0.6.3.

Three formats — **MSI**, **Setup EXE** and a **portable ZIP**. By default all
three are per-user and need no administrator rights; none modifies `PATH`, file
associations, URL protocols, services, scheduled tasks, firewall rules or
autostart. MSI and Setup EXE install to `%LOCALAPPDATA%\Programs\Lightning` with
a Start-menu shortcut and uninstall from Settings → Apps; the portable ZIP writes
no registry keys, so deleting the folder removes it.

**For all users** (Program Files, e.g. where policy only allows programs from
trusted locations): choose *For all users* in the Setup EXE, or deploy silently
from an elevated context — Intune, SCCM, WAPT and GPO all qualify:

```bat
Lightning-<version>-<sha>-windows-x86_64-setup.exe /S /ALLUSERS
msiexec /i Lightning-<version>-<sha>-windows-x86_64.msi ALLUSERS=1 /qn
```

Each person's settings and account stay in their own profile. An all-users copy
still updates itself, but Windows asks for administrator approval each time.
Details, uninstall switches and exit codes:
[Windows packaging](../packaging-ci/docs/windows-packaging.md#install-scope-just-me-or-all-users-github-issue-14).

Windows packages are **not code-signed**, so Windows shows an "unknown publisher"
SmartScreen warning. Check the hash first
(`Get-FileHash .\Lightning-0.10.0-<sha>-windows-x86_64.msi -Algorithm SHA256`),
then choose *More info → Run anyway*. Signing through
[SignPath Foundation](https://signpath.org/) is planned but has not been applied
for or granted — see the [code signing policy](code-signing-policy.md).

## macOS (Apple Silicon, macOS 26 or newer)

macOS packages have been published since v0.7.5. The macOS build never
blocks a release, so a release can occasionally ship without one.

Apple Silicon only, and macOS 26 or newer: both limits are derived from the Qt
frameworks the bundle links, not chosen. Unzip and drag **Lightning.app** into
`/Applications`.

The app is not signed with an Apple Developer ID and not notarized, so the first
launch is refused. Double-click it and let macOS refuse — the button only appears
after it has blocked the app once — then open **System Settings → Privacy &
Security**, click **Open Anyway** next to the message, and confirm. macOS
remembers the decision. Clearing the quarantine flag directly does the same thing:

```sh
xattr -dr com.apple.quarantine /Applications/Lightning.app
```

Two honest limits: the macOS build **does not update itself** — Lightning will
tell you a new version exists, but installing it means downloading the next zip —
and **nobody has clicked through it on a Mac**. The pipeline proves the bundle's
frameworks load and the binary runs; that is all. Please report what you find.

## Afterwards

Uninstalling removes the application and its shortcuts and deliberately leaves
your Matrix session, settings and message stores alone: those live in your user
profile, outside the install directory, and are removed by signing out of the
account inside the app. Once installed, Lightning can update itself — see
[Application updates](updates.md).

Packaging, cross-platform builds, publishing and verification live under
[`packaging-ci/`](../packaging-ci/README.md) in this repository. Every package
is built by CI from one exact, immutable source commit, never from a
developer's machine ([provenance](signpath-build-provenance.md)).
