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

The corrected build (project 6 `c4450fb` onward) produces the production
`Lightning.exe` as a **GUI-subsystem** PE32+ x86-64 binary, so a normal
double-click never flashes or leaves a console; `--version` / `--help` /
`--build-info` still print when launched from a console (the binary attaches to
the parent console). The build defaults to the **Rust (E2EE) backend** and, on
Windows, stores tokens in the **Windows Credential Manager**. Validation asserts
the GUI subsystem, dependency closure, required QML imports and plugins,
path/secret scans, portable ZIP inspection, x64 MSI table inspection, and amd64
NSIS inspection. Separate clean Wine prefixes pass portable/MSI/NSIS
`--version`, install/run/uninstall (preserving simulated user data), and a
`--build-info` check that the packaged binary reports `default_backend=rust`
and `secret_store=windows-credential-manager`. Those are cross-platform and Wine
results only, not native Windows acceptance.

## The update helper

Beside the application the build stages `lightning-updater.exe`: a small
**console-subsystem** helper (Qt6::Core only — no network, no Matrix, no store)
that performs the one step of an update that cannot happen while Lightning is
running. All three Windows packages are produced from the one staged tree, so it
ships in the portable ZIP, the MSI and the setup EXE alike; without it the
in-app updater has nothing to hand a verified download to and the feature is a
silent no-op.

Two consequences worth knowing:

- **It carries its own version resource.** The build used to pass one compiled
  `VERSIONINFO` object through the global `CMAKE_EXE_LINKER_FLAGS`, which with a
  second executable would stamp the helper with `OriginalFilename
  "Lightning.exe"` — false metadata, of exactly the kind SignPath's file
  restrictions apply to. `build-windows.sh` now compiles one resource per
  executable and attaches them per target through
  `packaging/windows/version-resources.cmake` (injected with
  `-DCMAKE_PROJECT_INCLUDE`; the Lightning source is not patched).
  `verify-windows-metadata.py` rejects both failure directions.
- **It is Lightning-owned and is signed.** It appears in
  `packaging/windows/signing-inventory.json`, in `dist/windows/signing-payload/`
  with its own checksum, and is passed to `sign_windows_file` before the
  packages consume it. An unsigned helper beside a signed application would be
  the weakest link, not a detail — it is the process that replaces the
  application on disk.

Validation asserts the helper's presence in the stage, its console subsystem,
its row in the MSI `File` table, the portable ZIP entry, and a reference in the
NSIS payload; the Wine prefixes additionally assert that an MSI install and an
NSIS install each actually place it next to `Lightning.exe`.

An earlier pre-runner feasibility pass had built the source as a 62 MiB
console-subsystem application; that subsystem and the earlier HTTP/insecure
defaults were the defects this pass corrected.

## The call media engine (GStreamer)

Voice, camera and screen share go through `SfuMediaEngine`, which is built only
when CMake's `pkg_check_modules(GSTWEBRTC ... gstreamer-1.0 gstreamer-webrtc-1.0
gstreamer-sdp-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-rtp-1.0)`
succeeds. **When it does not, the build still succeeds** and the shipped client
answers every call attempt with the honest signaling-only refusal — a failure
that is invisible in a package listing, in a launch, and in a sync. Windows
shipped exactly that until this change: the build log carried
`Package 'gstreamer-webrtc-1.0' not found` and the .exe imported no GStreamer at
all.

**Where the plugins come from.** Fedora's `mingw64-gstreamer1*` RPMs are not
usable: they contain the `libgstwebrtc-1.0-0.dll` *library* but no `webrtcbin`
*plugin*, and no nice/ICE, srtp, opus, vpx or webrtcdsp elements. The image
installs a pinned, checksum-verified subset of the upstream **GStreamer 1.28.5
MinGW SDK** instead — the same `x86_64-w64-mingw32` target the rest of the build
uses. Its installer is Inno Setup 6.7 data, which `innoextract` cannot read and
7-Zip cannot open, so the image runs it under the Wine it already carries, in
the vendor's own silent mode. About 26 MB of the 2.4 GB extraction is kept.

**One runtime, chosen with evidence.** A process gets one `libglib-2.0-0.dll`,
one `libcrypto-3-x64.dll`, one `libstdc++-6.dll`. The SDK is built with GCC 14.2
and this image's MinGW with GCC 15/16, so eleven DLL names exist on both sides.
Every symbol the shipped SDK binaries import from each of those was compared
against the Fedora copy's export table with `objdump`: all present, so **no
Fedora runtime DLL is replaced** and the staging script keeps resolving
everything from one place. Two plugins failed that comparison —
`libgstmediafoundation.dll` (`mfvideosrc`) and `libgstd3d11.dll`
(`d3d11screencapturesrc`) import `std::codecvt<wchar_t, char, _Mbstatet>`
symbols that no longer exist since mingw-w64 changed `mbstate_t` — so they are
**not shipped**, and Windows capture uses `ksvideosrc` (libgstwinks) and
`gdiscreencapsrc` (libgstwinscreencap). The image build FAILS if any future
GStreamer runtime DLL collides with a Fedora one, rather than overwriting it.

**Staging is explicit, because nothing imports a plugin.** A GStreamer plugin is
dlopen'd, so the recursive PE-import walk cannot discover one. `GSTREAMER_PLUGINS`
in `scripts/stage-windows-runtime.py` copies 25 plugins into `gstreamer-1.0/`
beside `Lightning.exe` and SEEDS them into that walk, which is what pulls their
own runtime DLLs out of the sysroot. The directory name is a contract:
`SfuMediaEngine::runtimeAvailable()` points `GST_PLUGIN_PATH` at
`<exe dir>/gstreamer-1.0` and clears `GST_PLUGIN_SYSTEM_PATH` before `gst_init`,
because the compiled-in default plugin path is the builder's sysroot.

**What validation proves.** `scripts/validate-windows-artifacts.sh` asserts that
`Lightning.exe` imports `libgstreamer-1.0-0.dll`, `libgstwebrtc-1.0-0.dll`,
`libgstsdp-1.0-0.dll` and `libgstapp-1.0-0.dll` (the only evidence that the
engine was compiled in at all), that every declared plugin is in the stage, the
MSI File table and the extracted portable ZIP, and then runs
`gst-element-probe.exe` — built into the image, never shipped — from inside the
extracted tree under Wine, reproducing the engine's own registry probe and
requiring all 34 elements to be found. `smoke-windows-wine.sh` additionally
asserts the MSI and NSIS installs deliver the whole plugin directory. A Wine
pass is not native Windows acceptance and not a completed call: it proves the
plugins load and register against the bundled runtime.

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

## Native acceptance

`packaging/windows/native-windows-acceptance.ps1` is run by an operator on a
real Windows host. It verifies installed files, `--version`/`--build-info` (Rust
default + Credential Manager), the GUI PE subsystem, DISPLAY-free startup +
bounded liveness + clean close, and the captured log (qwindows / Rust / secure
store selected, cache opened, and none of: insecure fallback, invalid cache
path, "Invalid window handle", QML binding loop, pagination storm). It emits a
sanitized diagnostic ZIP with no credentials, account database, token, or
message content. Record its results as **PASS/FAIL/NOT TESTED**.

## Known limitations

The Windows build now uses a native Windows Credential Manager SecretStore
(`WinCredStore`, project 6); it no longer falls back to the insecure QSettings
store, and existing plaintext tokens are migrated on first launch. What that
store does at runtime is still **NOT TESTED** here — it is compiled by the MinGW
cross-build but only exercisable on native Windows.

There is no Authenticode certificate, so SmartScreen/Defender will warn on these
unsigned test artifacts (project 7 has a disabled signing hook that activates
only when a real signing credential is supplied via protected CI variables — no
fake/self-signed identity is ever created). Native Windows 10/11 installation,
Credential Manager behaviour, SmartScreen/Defender, GUI/Direct3D, multimedia,
notifications, tray behavior, DPI, long paths, non-ASCII profiles, the shutdown
race, MSI repair/upgrade, and Add/Remove Programs presentation remain **NOT
TESTED**.

A call that actually connects on native Windows is **NOT TESTED**. The bundled
GStreamer is proven only as far as Wine can prove it: the plugins load and every
required element registers. Real capture is untested in both directions —
`ksvideosrc` talks to the kernel-streaming camera stack, which does not expose
MediaFoundation-only (virtual, DRM) cameras, and `gdiscreencapsrc` is a GDI
desktop grab rather than a compositor capture. Audio device selection resolves
through `autoaudiosrc`/`autoaudiosink`, i.e. WASAPI2/WASAPI/DirectSound by rank,
and which one a given machine picks has not been observed. A real authorized Windows host (run the acceptance script above) is
still required for those checks and for any release claim.
