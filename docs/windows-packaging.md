# Windows test packaging architecture decision (2026-07-22)

This pass selects a **containerized Linux cross-build runner** (Option A). It
does not claim to be a native Windows runner. The dedicated GitLab Runner
manager and every build tool remain in containers on the main GitLab VM
(`10.195.35.2`). Nothing is installed or changed on the runner-only VM
(`10.195.35.6`). Artifacts are unsigned, manually requested test artifacts
with seven-day CI retention; they are never published or attached to a
release.

## Evidence and feasibility

The first verified source was immutable project-6 commit
`55ce43798583fe36fc6af8d9e42745e3e18f6e51`, fetched into a separate
temporary source tree rather than built from a mutable developer checkout.
Every artifact filename and `build-info.json` records that commit. The source
requires Qt Core, Gui, Qml, Quick, Quick Controls 2, Network, Sql, Widgets, and
Multimedia
(minimum Qt 6.5). Qt DBus and libsecret are optional. The current development
shell uses Qt 6.11.1 and Rust 1.95.0. Fedora 44 provides a coherent Qt 6.11.1
MinGW cross-toolchain, so this image does not downgrade Qt. Rust officially
supports cross-compiling the Tier-1 `x86_64-pc-windows-gnu` target, and the
source's Unix-only permission APIs are protected by `cfg(unix)`.

The Rust-enabled build is required. Project 7 supplies only a Cargo invocation
adapter: Cargo builds the unchanged project-6 manifest for the Windows GNU
target and mirrors its static-library output into the location that the
unchanged project-6 CMake file imports. SQLite, Qt, MinGW, and Rust all target
the same x86-64 Windows GNU ABI. A real build, PE import audit, and Wine smoke
test must still pass before the application artifact can be called successful.

Fedora's MinGW QtMultimedia package has the target library and Windows Media
Foundation backend but omits the `QtMultimedia` QML import used by Lightning.
The builder verifies the official Qt 6.11.1 qtmultimedia source archive by
SHA-256 and cross-builds that matching QML module. Runtime staging then copies
the QML imports, explicit Qt plugins, and the recursively closed PE DLL graph;
it does not copy the Qt SDK.

The EXE installer uses NSIS and the MSI uses `wixl`/msitools. PE and MSI table
inspection are structural cross-platform validation. Clean Wine prefixes add
portable launch and silent install/uninstall smoke coverage, but Wine is not
native Windows acceptance.

## Verified cross-build result

The pre-runner feasibility pass built the unchanged Rust-enabled source as a
62 MiB PE32+ x86-64 console application. The final deployment contained 145
x86-64 PE executables/DLLs; dependency closure, required QML imports and
plugins, path/secret scans, portable ZIP inspection, x64 MSI table inspection,
and amd64 NSIS inspection passed. Separate clean Wine prefixes passed portable
`--version`, silent MSI install/run/uninstall, and silent NSIS
install/run/uninstall while preserving simulated user data. Those are
cross-platform and Wine results only, not native Windows acceptance.

## Security decision

The Docker socket is required by the runner manager's Docker executor and is
therefore a root-equivalent boundary. The runner must be project-7-only,
protected, locked, tagged only `windows-cross` and `windows-package`, refuse
untagged jobs, and have concurrency one. The Windows job exists only for a
web/API pipeline on project 7's protected default branch when
`BUILD_WINDOWS_PACKAGES=true`, `PUBLISH_PACKAGES=false`, and `SOURCE_REF` is a
full source commit. The job uses a prebuilt local image; it does not build a
Dockerfile from a user-selected source ref and does not receive the host socket.
Large artifact uploads use GitLab's host-internal coordinator endpoint because
the public proxy rejects request bodies above approximately 100 MiB. Source
checkout remains on the canonical HTTPS origin with certificate verification;
the internal coordinator route is a deliberately documented trusted-network
boundary, not a TLS-verification bypass.

## Known limitations

Project 6 still has no Windows Credential Manager/DPAPI SecretStore; the
Windows build falls back to the application's explicitly warned insecure
QSettings store. That makes these artifacts unsuitable for production and is
a concrete upstream requirement before release. There is no Authenticode
certificate. Native Windows 10/11 installation, Credential Manager,
SmartScreen/Defender, GUI/Direct3D, multimedia, notifications, tray behavior,
DPI, long paths, non-ASCII profiles, MSI repair/upgrade, and Add/Remove Programs
presentation remain **NOT TESTED**. A real authorized Windows host is still
required for those checks and for any release claim.
