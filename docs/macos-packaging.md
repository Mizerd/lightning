# macOS packaging — architecture and limits

Covers `macos-package-test`, the native macOS arm64 build path added
2026-08-13. Read this before changing anything under `scripts/build-macos.sh`,
`scripts/validate-macos-artifacts.sh`, or `scripts/prepare-pinned-source-macos.sh`.

## Why this target is different

Every other target in this project builds inside a pinned Docker image. macOS
cannot:

- Apple's SDK, frameworks, `codesign`, and `macdeployqt` are not distributable
  in a Linux container, and Apple's licence does not permit macOS in a
  non-Apple-hardware VM.
- There is no supported cross-compiler for a Qt/QML macOS app. The Windows
  target is a Linux **cross**-build (MinGW); no equivalent exists here.

So `macos-package-test` runs on a **shell executor on real hardware** — the
physical M1 Mac mini at `10.195.35.7`, tags `macos` / `arm64` / `ios`. Three
consequences drive the design:

| Container jobs | This job |
| --- | --- |
| Toolchain pinned by image digest | Toolchain installed on the host; versions **recorded** into `build-info.json` at build time |
| Fresh filesystem per job | Working directory **persists**; every output path is explicitly removed |
| Job isolated from the host | Job runs as the `runner` user with its normal privileges |

The runner account is deliberately non-admin and has no sudo, so a CI job cannot
modify the toolchain it builds against. Installing or upgrading Qt/CMake/Rust is
a separate, human, admin-account action.

## Toolchain

Installed on the Mac, not by this pipeline:

| Component | Source | Notes |
| --- | --- | --- |
| Qt 6.11.1 | Homebrew `qt` (`/opt/homebrew/opt/qt`) | arm64-only bottle; sets the deployment floor |
| CMake, Ninja, pkg-config | Homebrew | |
| Rust 1.93.0 | rustup, in the `runner` account (`~/.cargo`) | matches the pinned toolchain the Linux `deb`/`rpm` builds use |
| clang + macOS SDK | Xcode / Command Line Tools | either works; see below |
| `jq`, `iconutil`, `sips`, `codesign`, `ditto`, `lipo`, `otool`, `spctl` | macOS 26 base system | `jq` ships with macOS 26 |

`build-macos.sh` fails fast with a named tool if any of these is missing, rather
than dying halfway through a long build.

### Xcode vs Command Line Tools

`xcodebuild` is **not** required and is not in the tool check. A Ninja/clang
build needs only a compiler and an SDK, both of which the Command Line Tools
provide — and `xcodebuild` exits non-zero whenever `xcode-select` points at a
CLT instance instead of `Xcode.app`, which is exactly what the Homebrew
installer leaves behind. Requiring it would fail a build that can proceed
perfectly well.

The script records whichever it used (`toolchain.xcode`,
`toolchain.developer_dir`, `toolchain.macos_sdk`) so `build-info.json` never
claims an Xcode build that the CLT actually produced.

> Full Xcode **is** required for the future iOS target. Point the system at it
> with `sudo xcode-select --switch /Applications/Xcode.app/Contents/Developer`
> before adding an iOS job.

## Apple framework linking

The build passes `-framework Security -framework CoreFoundation` via
`CMAKE_EXE_LINKER_FLAGS`. This is required, not defensive.

matrix-sdk reaches TLS through rustls, and on Apple targets rustls' platform
verifier calls into Security.framework. When cargo links a binary itself it
honours the crate's `cargo:rustc-link-lib=framework=Security` directive — but
here CMake links `libmatrix_client_rust.a` into a C++ executable, so that
directive is never seen. Without the flag the link fails with ten undefined
symbols:

```text
"_SecTrustEvaluateWithError", "_SecPolicyCreateSSL",
"_SecCertificateCreateWithData", "_SecTrustCreateWithCertificates", …
ld: symbol(s) not found for architecture arm64
```

This is packaging configuration, not a source patch — the Lightning source is
unmodified, exactly as the Windows cross-build supplies its own linker inputs
(`lightning-version.o`). If a future SDK bump pulls in another Apple framework,
add it here rather than editing project 6.

## Bundle assembly

Lightning's `CMakeLists.txt` targets Linux and Windows layouts: it installs into
`bin/`, sets `WIN32_EXECUTABLE` on Windows, and never sets `MACOSX_BUNDLE`. The
`.app` is therefore assembled by `build-macos.sh` from the built binary rather
than by `cmake --install`. Keeping it out of the source tree means macOS
packaging cannot regress the Linux install rules.

```text
Lightning.app/Contents/
├── Info.plist                 generated (identity, version, usage strings)
├── PkgInfo
├── MacOS/Lightning            the matrix-client binary
├── Resources/
│   ├── Lightning.icns         built from the source hicolor PNGs via iconutil
│   ├── build-info.json
│   └── qml/                   Qt QML modules (macdeployqt)
├── Frameworks/                Qt frameworks (macdeployqt)
└── PlugIns/                   platform/imageformats/multimedia plugins
```

Three details that are load-bearing:

**`NSMicrophoneUsageDescription` is mandatory.** Lightning records voice
messages through `QMediaCaptureSession` / `QAudioInput`. macOS terminates any
process that touches the microphone without a usage string in `Info.plist`, so
omitting it produces a bundle that crashes the first time a user holds the
record button. `validate-macos-artifacts.sh` treats a missing key as a hard
failure.

**`macdeployqt` needs `-qmldir`.** Lightning's own QML is compiled into the
binary as Qt resources, so there is nothing on disk for macdeployqt to scan
unless it is pointed at the source tree. Without it, the Qt QML modules the app
imports (QtQuick, QtQuick.Controls, QtMultimedia, …) are not bundled and the app
dies when it tries to create its first window.

**The bundle must be re-signed after macdeployqt.** On Apple Silicon the kernel
refuses to execute any page without a valid signature. `macdeployqt` rewrites
Mach-O load commands with `install_name_tool`, which invalidates the linker's
original ad-hoc signature, so the bundle is unrunnable until
`codesign --force --deep --sign -` re-signs it. This is what makes the artifact
run **at all**; it is not distribution signing.

The archive is produced with `ditto -c -k --sequesterRsrc --keepParent`, not
`zip -r`: a plain zip mangles framework symlink layout and breaks the signature.

## Bundle identity

```text
CFBundleIdentifier   net.smetonis.lightning
```

Gatekeeper, TCC (microphone consent), and any future notarization ticket bind to
this string. Once artifacts exist outside the build machine it must stay stable —
changing it re-prompts every user for permissions and invalidates notarization.
It is also the identifier a future Apple Developer App ID must match.

## What this does NOT produce

The artifact is an **unsigned, un-notarized developer test bundle**, and the
pipeline is honest about it rather than implying otherwise:

- Signed with an **ad-hoc** signature (`-`), not a Developer ID identity.
- **Not notarized**, so Gatekeeper refuses to open it on any machine other than
  the one that built it. `validate-macos-artifacts.sh` runs
  `spctl --assess` and records the expected rejection as evidence in
  `reports/spctl.txt` — a green pipeline must not be mistaken for a
  distributable build.
- **arm64 only**, not a universal binary: Homebrew's Qt bottle is arm64-only, so
  an Intel or universal build would need a separate Qt.
- `LSMinimumSystemVersion` is **14.0**, inherited from Homebrew's Qt
  (`minos 14.0`). The app cannot claim support for an older macOS than the
  frameworks it links.
- **No GUI acceptance testing.** Validation exercises `--version` and
  `--build-info` on the bundled binary, which proves the frameworks load and the
  binary executes. Real window creation, notifications, media playback,
  microphone capture, Keychain behaviour, and Retina rendering are **NOT
  TESTED**.

The bundle always reports `build_kind: unsigned-test`, whatever the pipeline, and
`tests/test-pipeline-config.py` asserts that no publishing macOS job exists, that
`publish-packages` does not consume the bundle, and that no macOS-tagged job has
a release action. Publishing an artifact Gatekeeper blocks would be worse than
shipping nothing.

> Earlier revisions made `build-macos.sh` abort on `PUBLISH_PACKAGES=true`. That
> was dropped once macOS was allowed into full-fleet runs — it would have failed
> exactly the pipelines that build every other platform. The guarantee is
> structural (nothing consumes or releases the artifact), not a refusal to build.

## Running with the fleet

The gate constrains only the default branch, a web/api pipeline, a pinned
40-character SHA, and `BUILD_MACOS_PACKAGES=true`. It is deliberately **not**
constrained on `BUILD_FORMATS`, `BUILD_WINDOWS_PACKAGES`, or
`PUBLISH_PACKAGES`, because the Mac shares no capacity with the Linux or Windows
pools:

| Request | Result |
| --- | --- |
| `BUILD_MACOS_PACKAGES=true`, `BUILD_FORMATS=none` | macOS-only pipeline |
| `BUILD_MACOS_PACKAGES=true`, `BUILD_FORMATS=all` | macOS builds in parallel with the whole Linux fleet |
| `BUILD_MACOS_PACKAGES=true`, `PUBLISH_PACKAGES=true` | macOS builds, Linux publishes; the macOS bundle is not published |
| `BUILD_MACOS_PACKAGES=false` | no macOS job at all |

It starts as soon as `resolve-source` completes. Two properties make that true
and both are asserted in the config tests:

- `needs` is exactly `[resolve-source]` — never a Linux build job;
- `resource_group` is `lightning-macos-package`, distinct from the two bounded
  Linux lanes (`lightning-package-build-a` / `-b`), so it never waits on them.

## GIF provider keys

When `GIPHY_API_KEY` / `KLIPY_API_KEY` are available the build embeds them
through the same generator mechanism as every other target, so the picker works
with no local configuration. `build-info.json` records `gif_keys_embedded`.

The validator's leak scan reads that flag rather than scanning unconditionally.
An embedded key in a key-embedding build is intended, not a leak — flagging it
would fail every normal run. What the scan enforces is the inverse: a build that
reported itself **keyless** must not contain a key. It also always scans for
runner tokens, SSH private keys, and builder paths.

Note that a key compiled into a binary is ultimately extractable. These
artifacts are developer-scoped and expire in seven days.

## Path to a releasable macOS build

In order:

1. Apple Developer Program membership; create the App ID for
   `net.smetonis.lightning`.
2. Developer ID Application certificate, stored in a dedicated Keychain on the
   Mac. **This is the security-sensitive step** — the host has FileVault
   disabled and automatic GUI login so the runner can start unattended, so a
   signing key sitting in the login keychain is materially exposed. Decide the
   keychain/unlock model before importing anything.
3. Replace the ad-hoc `codesign` call with the Developer ID identity plus a
   hardened-runtime entitlements file.
4. Notarize with `notarytool`, then `xcrun stapler staple` the bundle.
5. Re-check `spctl --assess` — it must now *accept*; flip the validator's
   expectation accordingly.
6. Package as a `.dmg` rather than a zip, if a drag-to-Applications installer is
   wanted.
7. Only then consider a publishing job, `SecretStore` on the macOS Keychain, and
   a universal binary.

Until step 5 passes, this stays a test-only path.

## Version resolution

`macos-package-test` runs `prepare-pinned-source-macos.sh`, not
`prepare-pinned-source.sh`. The difference: it does **not** re-run
`resolve-version.sh`, which derives the snapshot version from the commit's UTC
date using GNU `date -u -d` — a flag BSD date does not implement.

Porting it would mean a second, independent version resolution on a different
platform, and if the two ever disagreed (a commit near a UTC day boundary is
enough) the macOS artifact would carry a different version string than the Linux
artifacts built from the identical commit. Instead the macOS script consumes the
`dist/version.env` that `resolve-source` already produced on Linux and asserts
the pinned SHA matches, keeping one authoritative resolution.

`lib.sh` gained a `sha256_of` helper for the same reason: macOS has no
`sha256sum`, and `shasum -a 256` emits a byte-identical format. Linux behaviour
is unchanged — the GNU tool is still preferred where it exists.

## Running it

From project 7's protected default branch, a **web** or **api** pipeline with:

```text
BUILD_MACOS_PACKAGES=true
BUILD_FORMATS=none
PUBLISH_PACKAGES=false
SOURCE_REF=<full 40-character project-6 commit SHA>
```

`BUILD_FORMATS=none` excludes every Linux package build, so the pipeline is
`config-tests` → `resolve-source` → `macos-package-test` and nothing else.
(`config-tests` and `resolve-source` are Linux container jobs — they run the
test suite and pin the source SHA; they do not build any Linux package.)

See [Running with the fleet](#running-with-the-fleet) for combining it with a
full-fleet run.

Artifacts, developer-visible, seven-day expiry:

- `Lightning-<version>-<short-sha>-macos-arm64.zip` and its `.sha256`
- `dist/macos/build-info.json`
- `dist/macos/reports/` — macdeployqt log, codesign output, `spctl` assessment,
  `otool` dependency dumps, validation JSON

## Troubleshooting

**`required tool not found on the runner`** — the host toolchain changed. Only
an admin (`roksme`) can reinstall it; `runner` cannot `brew install`.

**`no macOS SDK is available`** — `xcode-select` points at a directory with no
SDK. Repoint it at `Xcode.app` or reinstall the Command Line Tools.

**Bundle references `/opt/homebrew`** — `macdeployqt` missed a library. The
bundle would only run on this machine, so validation fails. Check
`reports/otool-all.txt` for the offender.

**App builds but the validator's `--version` step fails** — usually a missing Qt
framework or the cocoa platform plugin; check `reports/macdeployqt.log`.

**`code signature verifies` fails** — something modified the bundle after
`codesign`. Nothing may touch the tree between signing and `ditto`.

**Source clone fails** — the job needs `CI_JOB_TOKEN`; it is not available
outside a pipeline. For a manual trial, seed `work/lightning` from a local
bundle instead of putting a token on the Mac.
