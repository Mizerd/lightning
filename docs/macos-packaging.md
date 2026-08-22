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
| OpenSSL 3 | Homebrew `openssl@3` (`/opt/homebrew/opt/openssl@3`) | **required** since the update-manifest verifier; keg-only, so it is passed explicitly (see below) |
| CMake, Ninja, pkg-config | Homebrew | |
| Rust 1.93.0 | rustup, in the `runner` account (`~/.cargo`) | matches the pinned toolchain the Linux `deb`/`rpm` builds use |
| clang + macOS SDK | Xcode / Command Line Tools | either works; see below |
| `jq`, `iconutil`, `sips`, `codesign`, `ditto`, `lipo`, `otool`, `spctl` | macOS 26 base system | `jq` ships with macOS 26 |

`build-macos.sh` fails fast with a named tool if any of these is missing, rather
than dying halfway through a long build.

### OpenSSL 3

Lightning verifies update-manifest signatures through OpenSSL 3's EVP API, and
its CMake does `find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)`. macOS
ships **no** OpenSSL 3 development headers — `/usr/bin/openssl` is LibreSSL and
has no `openssl/evp.h` — and Homebrew keg-onlys `openssl@3`, meaning it is
deliberately not symlinked into `/opt/homebrew`. So CMake cannot find it by
itself:

```sh
brew install openssl@3
```

`build-macos.sh` asserts the keg's `include/openssl/evp.h` and `lib/libcrypto.a`
(not `command -v openssl`, which finds Apple's LibreSSL) and passes
`-DOPENSSL_ROOT_DIR` plus `-DOPENSSL_USE_STATIC_LIBS=TRUE`.

**libcrypto is linked statically here on purpose.** A dynamic link against a
keg-only formula leaves an absolute `/opt/homebrew/opt/openssl@3/...` load
command in the binary. `macdeployqt` walks the Qt frameworks and their
dependencies; it would not bundle libcrypto, and the load-command repair pass
then fails closed with "needs …, which macdeployqt did not bundle" rather than
shipping a bundle that only runs on the build machine. Static linking removes
the question. If a future Homebrew stops shipping `libcrypto.a`, drop the static
flag and add libcrypto to the bundling pass — do not leave a host path in the
binary.

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

## Deployment target

`CMAKE_OSX_DEPLOYMENT_TARGET`, `MACOSX_DEPLOYMENT_TARGET`, and
`LSMinimumSystemVersion` all come from one derived value: the **highest `minos`
across the Qt frameworks the app links**, read with `otool -l` at build time.

It is derived rather than hardcoded because Homebrew's Qt does not have a single
floor:

| Framework | `minos` |
| --- | --- |
| QtCore, QtGui, QtNetwork, QtSql, QtWidgets | 14.0 |
| QtQml, QtQuick, QtQuickControls2, QtMultimedia | **26.0** |

An earlier revision read only `QtCore` and hardcoded `14.0`. That produced a
bundle whose `Info.plist` advertised macOS 14 support while linking frameworks
that require macOS 26 — it would not have loaded there at all. The linker warned
about exactly this:

```text
ld: warning: building for macOS-14.0, but linking with dylib
    '.../QtQuick.framework/...' which was built for newer version 26.0
```

Taking the maximum keeps the compatibility claim true and self-corrects whenever
Homebrew rebuilds Qt against a different SDK.

`MACOSX_DEPLOYMENT_TARGET` is exported for the same value so cargo/rustc and the
C sources the `cc` crate compiles (sha3, aes, jitterentropy, blake3) target the
same floor as the C++ link. Without it those objects are built against the host
SDK and the linker reports:

```text
ld: warning: object file (...libmatrix_client_rust.a[361](...sha3...o))
    was built for newer 'macOS' version (26.5) than being linked (14.0)
```

Consequence worth knowing: while Qt comes from Homebrew, the bundle's floor
tracks whatever macOS the build machine runs. Supporting genuinely older macOS
would mean supplying a Qt built against an older SDK (the official Qt installer
rather than Homebrew), not lowering this number.

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
(its per-target version resources). If a future SDK bump pulls in another Apple
framework, add it here rather than editing project 6.

Note that this global flag stays global on macOS on purpose: the bundle contains
only `matrix-client`, so there is no second executable it could wrongly apply
to. (Lightning's `lightning-updater` is not built here — the macOS bundle is an
unsigned, un-notarized TEST artifact that is never published, and its install
type stays `development`, which refuses automatic installation outright.)

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

### Pruned modules

`macdeployqt` deploys every plugin in its default categories, not only the ones
the app can reach. Here that pulls in the virtual-keyboard input context and,
through it, the VirtualKeyboard, Timeline, StateMachine and Pdf QML modules.
Lightning imports none of them — its complete import set is `QtQuick`,
`QtQuick.Controls`, `QtQuick.Controls.Basic`, `QtQuick.Dialogs`,
`QtQuick.Effects`, `QtQuick.Layouts`, `QtQuick.Window`, `QtMultimedia` and its
own `MatrixClient`.

They also arrive **broken**: their frameworks cannot be resolved (see
[Expected noise](#expected-noise-in-a-successful-build-log)), so the QML module
and its plugin are copied in while the framework it links is not. A bundle that
loaded one would fail at runtime.

So the build deletes them after macdeployqt and before signing:

```text
Contents/PlugIns/platforminputcontexts
Contents/Resources/qml/QtQuick/VirtualKeyboard
Contents/Resources/qml/QtQuick/Timeline
Contents/Resources/qml/QtQuick/Pdf
Contents/Resources/qml/QtQml/StateMachine
```

This removes the dead payload. It does **not** silence the errors: macdeployqt
prints them while deploying, before the prune runs, so they can only be
filtered — see below. If a future Lightning release does import one of these,
the module must be *fixed* (bundled with its framework), not merely un-pruned,
and the validation run at the end of the build is what would catch its absence.

### Console filtering

macdeployqt emits about 40 lines, in pairs, for dependencies it cannot resolve:

```text
ERROR: Cannot resolve rpath "@rpath/QtVirtualKeyboard.framework/..."
ERROR:  using QList("/opt/homebrew/opt/qtdeclarative/lib", ...)
```

They are all either for modules the build prunes moments later, or for dylibs
(`libwebp`, `libsharpyuv`, `libbrotlicommon`) that macdeployqt resolves on a
later pass. Only those two patterns are filtered from the console, and the build
prints how many it dropped. **Every line still lands verbatim in
`reports/macdeployqt.log`**, which is an artifact, and any other macdeployqt
error — a codesign failure, for instance — still prints.

### Load-command repair pass

`macdeployqt` drives `install_name_tool` through `QProcess` without always
waiting for it. Its own log carries:

```text
QProcess: Destroyed while process ("install_name_tool") is still running.
```

When that race bites, a binary keeps an absolute `/opt/homebrew` dependency and
the bundle only runs on the build machine. It is **intermittent**: the same
source commit produced a clean bundle in pipeline 90 and a broken
`PlugIns/quick/libqtquicktemplates2plugin.dylib` (still pointing at
`/opt/homebrew/opt/qtbase/lib/QtNetwork.framework`) in pipeline 91, which failed
validation.

Rather than rely on macdeployqt having finished, the build sweeps every Mach-O
in the bundle afterwards and rewrites any remaining host reference to the copy
already inside `Contents/Frameworks`. A binary's own install name (`LC_ID_DYLIB`)
is skipped — it is identity, not a dependency. A host dependency whose framework
is *not* bundled is a genuine missing dependency and stops the build.

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
- `LSMinimumSystemVersion` is **derived at build time** (currently **26.0**) —
  see [Deployment target](#deployment-target). The app must not claim support for
  an older macOS than the frameworks it links.
- **No GUI acceptance testing.** Validation exercises `--version` and
  `--build-info` on the bundled binary, which proves the frameworks load and the
  binary executes. Real window creation, notifications, media playback,
  microphone capture, Keychain behaviour, and Retina rendering are **NOT
  TESTED**.

The bundle always reports `build_kind: unsigned-test`, whatever the pipeline.

## Publication (0.7.5 onwards)

This artifact **is published**, as a download-only release asset, on an
explicit maintainer decision taken for 0.7.5. Everything in the section above
is still true of it — what changed is that the download page now tells people
how to open it and states the limits, instead of the project deciding on their
behalf that they should not have it.

What that means in practice:

- **`publish-packages` consumes this job's artifact** and uploads the zip
  alongside the Linux and Windows packages. Publication, verification, the
  release links and the GitHub mirror all follow from the publication manifest,
  so none of them needed a macOS special case.
- **The release never depends on the Mac.** The job is `allow_failure: true`
  and the `needs` entry is `optional: true`, so the Mac being offline, asleep
  or mid-macOS-update publishes a release *without* a macOS asset rather than
  failing eight good packages. `write-manifest.sh` prints which of the two
  happened.
- **It is NOT in the signed update manifest.** The client has no macOS install
  strategy — `InstallType::MacosDmg` is not self-installable and the updater
  helper returns `UnsupportedPlatform` — so an entry there would advertise an
  install the updater refuses to perform. macOS users update by downloading the
  next release.
- **Two limits are stated wherever it is offered**: Apple Silicon only, and
  macOS 26 or newer. Both are consequences of Homebrew's Qt, not choices.
- **Gatekeeper still blocks it**, and the walkthrough is the *Open Anyway*
  route. That works because the ad-hoc signature is structurally valid —
  `validate-macos-artifacts.sh` asserts `codesign --verify --deep --strict`
  and `ditto` preserves it — so macOS says "Apple could not verify…" and offers
  the button, rather than "damaged", which offers nothing.

`tests/test-pipeline-config.py` pins that shape: the need exists, it is
optional, the job is `allow_failure`, no macOS-tagged job carries a release
action, and the update manifest declares no macOS format. Each of those was
proven to fail against an injected defect before it was committed.

> Earlier revisions made `build-macos.sh` abort on `PUBLISH_PACKAGES=true`. That
> was dropped once macOS was allowed into full-fleet runs — it would have failed
> exactly the pipelines that build every other platform. The build script still
> uploads nothing; publication happens on Linux, from the artifact.

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

## Path to a SIGNED macOS build

Publication happened first, unsigned and disclosed (above). What follows is
what it would take for macOS users to stop having to click past Gatekeeper —
and for an Intel or older-macOS user to be able to run it at all.

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
7. `SecretStore` on the macOS Keychain, and a universal binary.

Separately from signing, the **deployment floor** is the other half of the
reach problem. `LSMinimumSystemVersion` is derived from the highest `minos`
across the linked Qt frameworks, and Homebrew's `qtdeclarative`/`qtmultimedia`
bottles put that at 26.0 — so the published bundle runs on macOS 26 and newer
only. Lowering it means a Qt built against an older SDK (from source, or a
different distribution channel), not a change in this repository. Do not simply
lower the number: an earlier revision hardcoded 14.0 and produced a bundle that
advertised macOS 14 while linking frameworks that require 26, which would not
have loaded there at all.

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

## Build times and the cargo cache

Cargo's target directory is not inside the build tree. `build-macos.sh` symlinks
`build/rust` to `$LIGHTNING_MACOS_CACHE/cargo-target`
(default `~/Library/Caches/lightning-ci/cargo-target`) so it survives the shell
executor wiping the workspace between jobs.

Two things invalidate the cache and force every crate to rebuild:

- **The deployment target changes.** `MACOSX_DEPLOYMENT_TARGET` is part of
  rustc's fingerprint, so a Qt upgrade that moves the derived floor (see
  [Deployment target](#deployment-target)) rebuilds everything exactly once.
- **The Rust toolchain changes.**

### The build host used to sleep mid-build

Job durations observed for the *same* source commit:

| Pipeline | Job wall clock | Cargo's own reported time |
| --- | --- | --- |
| 88 | 10 min | 5m 56s |
| 90 | 2 h 34 min | 6m 00s |
| 91 | 1 h 22 min | 6m 01s |

Cargo reported the same ~6 minutes every run and the C++ phase took under a
minute, so the variance was never in the build. **The Mac was falling asleep.**

`pmset -g` showed `sleep 1` — idle sleep after one minute — and `pmset -g log`
recorded 13,903 seconds of sleep across 30 events in a 90-minute window,
including a single 867-second sleep in the middle of a build. That is why
`rustc` accumulated only ~6 minutes of CPU across an hour of elapsed time while
sampling at a full core whenever it *was* awake.

The trap is that **macOS idle sleep is driven by user/HID activity and power
assertions, not CPU load.** A headless Mac with nobody at the keyboard sleeps
even at 100% CPU, and `gitlab-runner` takes no power assertion of its own. The
same mechanism delayed job pickup by 2–3 minutes and produced hundreds of
`Checking for jobs... failed` entries, because the machine was asleep when
GitLab had work for it.

Two independent fixes, deliberately both:

- **The job** wraps its steps in `caffeinate -dims`, so the machine cannot sleep
  while a build is running regardless of host configuration.
- **The host** should have `sudo pmset -a sleep 0 disksleep 0 powernap 0` set.
  This is admin-owned and can be undone by a macOS update or a settings change,
  which is exactly why the job does not rely on it.

The job `timeout` is `3h` — headroom, not an estimate. Re-measure a true
baseline once builds have run without sleeping.

`BUILD_JOBS` is `4`. It was briefly raised to 6, which coincided with a job that
hit the old 2h timeout, so it was put back. Given the variance documented above,
treat that as a precaution rather than a measured result — the fat-LTO pass
(`lto = true`, `codegen-units = 1`) is single-threaded regardless, so no value of
`BUILD_JOBS` affects the step that dominates the run.

## Expected noise in a successful build log

A green run still prints a lot of alarming-looking output. These are all
benign, and none of them fails the job:

**`ERROR: Cannot resolve rpath "@rpath/QtVirtualKeyboard.framework/..."`** (also
Qt3D*, QtPdf, QtStateMachine, QtQuickTimeline, QtSvg). Now filtered from the
console and kept in `reports/macdeployqt.log` — see
[Console filtering](#console-filtering) — but the mechanism is worth knowing.

The frameworks are **not** missing; they are all present under
`$(brew --prefix qt)/lib`. They fail to resolve because Homebrew's Qt binaries
carry rpaths naming *per-module* prefixes (`/opt/homebrew/opt/qtbase/lib`,
`/opt/homebrew/opt/qtdeclarative/lib`, `/opt/homebrew/opt/qtmultimedia/lib`),
and there is no corresponding prefix for qt3d, qtscxml or qtpdf. So macdeployqt
searches a list that cannot contain them, gives up, and copies the QML module in
*without* the framework it needs — dead payload, not just noise.

The remaining `libwebp`/`libsharpyuv`/`libbrotlicommon` lines are different: those
dylibs *are* bundled, and macdeployqt resolves them on a later pass.

The frameworks the app actually links are asserted individually by
`validate-macos-artifacts.sh`, which is the check that matters.

**`ERROR: codesign verification error: ... invalid signature (code or signature
have been modified) In subcomponent: .../libbrotlicommon.1.dylib`**. This is
`macdeployqt`'s own internal verification, and it is correct at the moment it
runs: macdeployqt has just rewritten those Mach-O load commands with
`install_name_tool`, which invalidates the signatures Homebrew shipped. The
build then re-signs everything inside-out and verifies, so the line immediately
after it in the log is `bundle signature verifies (ad-hoc)`. If the re-sign ever
failed, the script would `die` rather than continue.

**`ld: warning: object file ... was built for newer 'macOS' version`** and
**`ld: warning: building for macOS-X, but linking with dylib ... built for newer
version`**. These indicated a real bug and should no longer appear — see
[Deployment target](#deployment-target). If they come back, the derived target
has drifted from what Qt or the Rust objects were actually built against.

**Long single-threaded stretches.** Lightning's `[profile.release]` sets
`lto = true` and `codegen-units = 1`, so the final Rust crate and its link-time
optimisation are single-threaded by construction. `BUILD_JOBS` parallelises the
C++ phase and independent crates; it cannot split an LTO pass. Expect one busy
core for a long stretch, then `ninja -j$BUILD_JOBS` for the ~200 C++ files.

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
