<div align="center">

# ⚡ lightning-deploy

**Packaging and release orchestration for the [Lightning](https://gitlab.smetonis.net/Mizerd/lightning) Matrix client.**

[![Linux formats](https://img.shields.io/badge/linux-deb%20%7C%20rpm%20%7C%20flatpak%20%7C%20appimage%20%7C%20snap-2f6be0.svg)](#packages-and-clean-system-validation)
[![Windows: cross-built test](https://img.shields.io/badge/windows-cross--built%20test-4c8fdc.svg)](#windows-unsigned-test-packaging)
[![macOS: native arm64 test](https://img.shields.io/badge/macOS-native%20arm64%20test-000000.svg)](#macos-unsigned-test-packaging)
[![Pipelines: manual only](https://img.shields.io/badge/pipelines-web%20%2F%20api%20only-orange.svg)](#modes)
[![Signing: unsigned](https://img.shields.io/badge/signing-unsigned-lightgrey.svg)](#code-signing)

</div>

This is GitLab **project 7**. It fetches an immutable source revision from the
canonical Lightning source project (**project 6**), builds native packages,
validates each on a clean target system, publishes the validated files to
project 6's Generic Package Registry, verifies them, and finally creates or
updates the matching GitLab Release with package asset links. It is **not** a
second Lightning source repository — it holds no application code.

|  |  |
|---|---|
| **Five Linux formats** | `deb`, `rpm`, `flatpak`, `appimage`, `snap` — each built and installed on a clean system before it can be published |
| **Immutable input** | Every publishing pipeline pins one full 40-character project 6 SHA; both builders refetch it and must agree |
| **Publish, then verify** | Files are re-downloaded from the registry and checked before the release is created — the release is the *last* action, never the first |
| **Two test-only paths** | Cross-built Windows and natively built macOS arm64 artifacts that can never reach the registry, enforced by tests |
| **No accidental runs** | `web`/`api` pipelines only; publication additionally requires the protected default branch and full validation |
| **Truthful metadata** | One switch governs whether artifacts may claim a signature, so no file can advertise one it does not carry |

> **Scope:** build infrastructure for a self-managed GitLab instance. Nix
> packaging is deferred — no Nix package is built, validated, published, or
> linked. Windows and macOS artifacts are **test builds only** and are not part
> of any release.

## Contents

- [Architecture](#architecture)
- [Pipeline stages and jobs](#pipeline-stages-and-jobs)
- [Windows unsigned test packaging](#windows-unsigned-test-packaging)
- [macOS unsigned test packaging](#macos-unsigned-test-packaging)
- [Modes](#modes)
- [Variables](#variables)
- [Publication manifest](#publication-manifest)
- [Signed update manifest](#signed-update-manifest)
- [GitHub binary mirror](#github-binary-mirror)
- [Registry, verification, and immutability](#registry-verification-and-immutability)
- [Packages and clean-system validation](#packages-and-clean-system-validation)
- [Authentication and security](#authentication-and-security)
- [GIF provider keys](#gif-provider-keys)
- [Tests](#tests)
- [Code signing](#code-signing)
- [Release ordering (authoritative)](#release-ordering-authoritative)

The canonical local checkout is `/home/roksme/git/lightning-deploy`. The
canonical source checkout is `/home/roksme/git/lightning`.

## Architecture

```text
Lightning project 6
├── source repository
├── tag  (vX.Y.Z)
├── GitLab Release  (Lightning X.Y.Z)
└── Generic Package Registry
    └── lightning
        └── <version>
            ├── lightning_<version>_amd64.deb
            ├── lightning-<version>-1.x86_64.rpm
            ├── lightning_<version>_amd64.flatpak
            ├── Lightning-<version>-x86_64.AppImage
            ├── lightning_<version>_amd64.snap
            └── future explicitly approved formats

lightning-deploy project 7
├── source resolution     (resolve-source)
├── builds                (build-deb, build-rpm, build-flatpak,
│                          build-appimage, build-snap)
├── clean-system validation (validate-deb, validate-rpm, validate-flatpak,
│                          validate-appimage, validate-snap)
├── publication           (publish-packages)
├── registry verification (verify-published-packages)
├── final release action  (finalize-release)
└── non-publishing test paths
    ├── windows-package-test  (Linux cross-build, unsigned)
    └── macos-package-test    (native arm64 on the Mac mini, unsigned)
```

The two test paths are opt-in, never part of a release, and produce only
expiring CI artifacts. Neither feeds `publish-packages`.

The permanent home of every released package is project 6's Generic Package
Registry. Project 7 keeps only expiring CI artifacts for diagnostics; nothing
consumers download depends on a project 7 job artifact.

## Pipeline stages and jobs

Only manually created `web` and `api` pipelines are accepted.

| Stage | Job | Purpose |
| --- | --- | --- |
| test | `config-tests` | Shell + pipeline-config tests; gates the rest |
| resolve | `resolve-source` | Validate the request, pin the source SHA, resolve the version |
| build | `build-deb` | Debian 13.6 build |
| build | `build-rpm` | Fedora 44 build |
| build | `build-flatpak` | KDE-runtime sandbox build → single-file bundle |
| build | `build-appimage` | Debian staged build → self-contained AppImage |
| build | `build-snap` | Snap packed from the AppImage job's AppDir |
| build | `windows-package-test` | Opt-in unsigned Windows cross-build (never publishes) |
| build | `macos-package-test` | Opt-in unsigned macOS arm64 `.app` on the Mac mini (never publishes) |
| validate | `validate-deb` | Clean Debian install/run/uninstall audit |
| validate | `validate-rpm` | Clean Fedora install/run/uninstall audit |
| validate | `validate-flatpak` | Bundle install into a disposable installation, run, uninstall |
| validate | `validate-appimage` | extract-and-run on a Qt-less image, payload audit |
| validate | `validate-snap` | Structural + payload audit, launcher run (no snapd in fleet) |
| publish | `publish-packages` | Build the manifest and upload to project 6 |
| verify | `verify-published-packages` | Re-download and check the registry files |
| sign | `sign-update-manifest` | Build and Ed25519-sign the client update manifest |
| release | `finalize-release` | Attach links / create the release |
| mirror | `mirror-release-to-github` | Copy the published binaries to the GitHub mirror and verify them anonymously |
| update | `publish-update-manifest` | Publish the signed manifest and move the `latest` pointer (final action) |

The graph no longer ends after validation. When `PUBLISH_PACKAGES=true` the
publish, verify, sign, release, mirror, and update jobs are always included for
**both** release actions; a build-only pipeline stops after validation by design.

The update manifest is deliberately **signed before** the release and
**published after** it: a broken signing key must fail while nothing
irreversible has happened, and the `latest` pointer that every installed
Lightning polls must never advertise a release that was not finalized. The
GitHub mirror sits between the two: it can only run once the real release exists,
and the `latest` pointer must not hand out a mirror URL that has not been proved
to serve the right bytes. See
[`docs/update-manifest.md`](docs/update-manifest.md).

`resolve-source` resolves the requested ref to a full 40-character commit SHA.
Every builder refetches and must resolve the same SHA, so no two formats can
package different source (the snap consumes the AppImage job's already-built
AppDir artifact instead of compiling a third time). A shared build
`resource_group` keeps the memory-heavy builds sequential; a second
`resource_group` serializes all project-6 registry/release writes.

Each format builds and validates via one unique selector tag: `apt`, `dnf`,
`flatpak`, `appimage`, `snap`. Two runner fleets carry those tags — the
original one on the GitLab VM and a mirrored fleet on a dedicated VM
(10.195.35.6, xcp-ng-1) — so whichever matching runner is free takes a job
(see the "GitLab Package Build Runners" infrastructure note). Build
concurrency is **bounded, not free**: the build jobs are split across exactly
two resource groups (`lightning-package-build-a`: deb/flatpak/snap,
`lightning-package-build-b`: rpm/appimage), so at most two package builds run
at once anywhere. Since 2026-07-20 those two lanes are also guaranteed to run
on **different hosts**: each host runs at most one package job at a time
(global `concurrent = 1` on both the mirror VM and the consolidated
`package-runner-packages` manager on the GitLab VM), so when both lanes are
active the second is forced onto the other host — the mirror is no longer
idle during parallel builds. Job containers stay capped at 4 CPU / 6 GiB
(GitLab VM) / 4 CPU / 8 GiB (mirror) so a single build always leaves GitLab EE
headroom. Do not add a third resource group or raise `concurrent`/the caps
without revisiting host capacity (raising `concurrent` would let both lanes
collapse back onto one host).

The flatpak runners (on both hosts) are the one deliberate confinement
exception: their job containers run **privileged**. flatpak-builder's bwrap
must mount a fresh procfs inside its user namespace, which Docker's default
masked /proc forbids; the Docker daemon rejects a raw
`systempaths=unconfined` security-opt (it is a CLI-only alias), and GitLab
Runner exposes no masked-paths knob, so privileged is the only available
mechanism. Windows EXE/MSI work uses a separate project-7-only runner on the
main GitLab VM and never uses these Linux package pools.

## Windows unsigned test packaging

`windows-package-test` is an explicitly enabled **Linux cross-build**, not a
native Windows job. Its dedicated runner has only the tags `windows-cross` and
`windows-package`, resides on `10.195.35.2`, and accepts no untagged or
unprotected work. Build tools remain inside the pinned
`lightning-windows-builder:fedora44-qt6.11.1-ffmpeg7.1.1-gst1.28.5-rust1.95.0-v6`
image; the job does not receive the host Docker socket.

The runner manager uses GitLab's host-internal coordinator endpoint for polling
and the combined artifact upload because the public proxy rejects request
bodies above approximately 100 MiB. Project source fetches remain on the
canonical HTTPS URL with certificate verification. The internal route is a
trusted-network boundary; it is not exposed to job containers and no TLS
verification setting is disabled.

Start a new pipeline from project 7's protected default branch with exactly:

```text
BUILD_WINDOWS_PACKAGES=true
BUILD_FORMATS=none
PUBLISH_PACKAGES=false
SOURCE_REF=<full 40-character project-6 commit SHA>
RELEASE_VERSION=
RELEASE_NOTES_B64=
```

Resolve the source SHA before starting. A running pipeline never follows
project-6 `main` and must not use a mutable local project-6 checkout; the same
full SHA is rechecked by source resolution and the Windows builder, then
recorded in filenames and `build-info.json`.

Web and API pipeline sources are accepted. A branch, tag, short SHA, merge
request, non-default project-7 ref, publication input, release input, or Linux
format selection excludes the Windows job. `BUILD_FORMATS=none` also excludes
every Linux package build, so an intended Windows-only test cannot consume the
regular fleet accidentally.

The job produces seven-day, developer-visible CI artifacts only:

- `Lightning-<version>-<source-short-sha>-windows-x86_64.msi`
- `Lightning-<version>-<source-short-sha>-windows-x86_64-setup.exe`
- `Lightning-<version>-<source-short-sha>-windows-x86_64-portable.zip`
- `SHA256SUMS-windows.txt`, the staged deployment tree, and validation reports

They are unsigned test artifacts, not a release. The job has no package upload,
release, release-link, or tag action. It builds the project-6 source with the
real Rust backend for `x86_64-pc-windows-gnu`, producing a **GUI-subsystem**
`Lightning.exe` that **defaults to the Rust (E2EE) backend** and uses the
**Windows Credential Manager** for token storage. It stages a dependency-closed
Qt 6.11.1 runtime, creates a per-user NSIS installer and x64 MSI, inspects every
PE and the MSI tables (asserting the GUI subsystem), scans for
credentials/private build paths, and runs portable plus silent install/uninstall
tests in disposable Wine prefixes — including a `--build-info` check that the
binary reports `default_backend=rust` and
`secret_store=windows-credential-manager`. Wine smoke coverage is supplemental
and is never described as native Windows acceptance;
`packaging/windows/native-windows-acceptance.ps1` is the operator-run native
check.

Native Windows 10/11 install, graphics, multimedia, Credential Manager
behaviour, SmartScreen/Defender, DPI, tray/notification, long-path/non-ASCII
profile, the shutdown race, MSI repair/upgrade, and Add/Remove Programs behavior
remain **NOT TESTED**. The Windows build now uses a native Credential Manager
SecretStore (project 6) instead of the insecure QSettings fallback, but its
runtime behaviour is only exercisable on native Windows. Code signing is absent
(a disabled hook activates only with a real credential — no fake identity), so
SmartScreen will warn. Those are release blockers, not properties hidden by the
packaging pipeline. See
[`docs/windows-packaging.md`](docs/windows-packaging.md) for the architecture
decision and [`docs/windows-runner-operations.md`](docs/windows-runner-operations.md)
for operations, rollback, cache, and troubleshooting.

## macOS unsigned test packaging

`macos-package-test` is a **native** build on real Apple hardware — the physical
M1 Mac mini at `10.195.35.7` (tags `macos`, `arm64`, `ios`), added 2026-08-13.
Unlike the Windows target it is not a cross-build: there is no supported way to
cross-compile a Qt/QML macOS app, and Apple's SDK and `codesign` cannot run in a
Linux container. It is therefore the only job in this project that uses a
**shell executor on bare metal** rather than a pinned image.

That has consequences the pipeline is explicit about. The toolchain is installed
on the host (Homebrew Qt 6.11.1, CMake, Ninja; rustup 1.93.0 in the `runner`
account; clang and the macOS SDK from Xcode/CLT), so it is not pinned by an
image digest — `build-macos.sh` therefore *records* the versions it actually
used into `build-info.json`. The working directory persists between jobs, so
every output path is explicitly removed rather than assumed clean. The `runner`
account is non-admin and cannot `brew install`, so a CI job cannot alter the
toolchain it builds against.

For a **macOS-only** pipeline, start one from project 7's protected default
branch with:

```text
BUILD_MACOS_PACKAGES=true
BUILD_FORMATS=none
PUBLISH_PACKAGES=false
SOURCE_REF=<full 40-character project-6 commit SHA>
```

`BUILD_FORMATS=none` excludes every Linux package build, so the pipeline is
exactly `config-tests` → `resolve-source` → `macos-package-test`.

The gate is deliberately **not** constrained on `BUILD_FORMATS`,
`BUILD_WINDOWS_PACKAGES`, or `PUBLISH_PACKAGES` — only on the default branch, a
web/api pipeline, a pinned 40-character SHA, and the explicit
`BUILD_MACOS_PACKAGES=true` opt-in. So the same flag added to a **full-fleet**
run (`BUILD_FORMATS=all`, with or without publication) builds macOS *alongside*
Linux rather than instead of it. The Mac is a dedicated host that shares no
capacity with the Linux or Windows pools, so there is no reason to exclude it —
and excluding it would mean macOS silently missing from exactly the pipelines
that build every other platform.

It starts as soon as `resolve-source` finishes: the job depends only on
`resolve-source`, never on a Linux build, and sits in its own
`lightning-macos-package` resource group rather than either of the two bounded
Linux build lanes, so it never queues behind them. Both properties are asserted
in `tests/test-pipeline-config.py`.

A branch, tag, short SHA, or merge-request pipeline still excludes the macOS
job.

The same commit once took 10 minutes and 2.5 hours on this host: the Mac was
idle-sleeping mid-build (macOS sleeps on lack of user activity, not lack of CPU
load). The job now runs under `caffeinate`. The `timeout` is `3h` as headroom.
See [The build host used to sleep mid-build](docs/macos-packaging.md#the-build-host-used-to-sleep-mid-build).

The job produces seven-day, developer-visible CI artifacts only:

- `Lightning-<version>-<source-short-sha>-macos-arm64.zip` and its `.sha256`
- `dist/macos/build-info.json`
- `dist/macos/reports/` — macdeployqt log, codesign output, `spctl` assessment,
  `otool` dependency dumps, and `macos-validation.json`

It builds the project-6 source with the real Rust backend for
`aarch64-apple-darwin`, asserts the Rust-only invariant on the built binary
(same fail-closed check as the Linux path), assembles a `Lightning.app` —
Lightning's CMake never sets `MACOSX_BUNDLE`, so the bundle is built by the
script, not by `cmake --install` — bundles the Qt frameworks and QML modules
with `macdeployqt`, re-signs ad-hoc, and validates the finished bundle before it
is archived. Validation checks bundle structure, `arm64` Mach-O, that no
bundled binary still references `/opt/homebrew` (which would make the app run
only on the build machine), the presence of every linked Qt framework and the
cocoa platform plugin, signature validity, and that the bundled binary actually
executes `--version` and `--build-info`.

**`NSMicrophoneUsageDescription` is mandatory, not decorative.** Lightning
records voice messages via `QMediaCaptureSession`/`QAudioInput`, and macOS
terminates any process that touches the microphone without a usage string, so a
missing key is a hard validation failure.

These are unsigned test artifacts, not a release:

- ad-hoc signed (`-`), **not** Developer ID signed, and **not notarized**, so
  Gatekeeper refuses them on any machine but the builder. Validation runs
  `spctl --assess` and records the *expected* rejection as evidence, so a green
  pipeline is never mistaken for a distributable build;
- **arm64 only** — Homebrew's Qt bottle is arm64-only, so this is not a
  universal binary;
- `LSMinimumSystemVersion` is **derived** from the highest `minos` across the Qt
  frameworks the app links, not hardcoded. Homebrew's Qt has a split floor —
  qtbase modules are built `minos 14.0` but QtQml/QtQuick/QtQuickControls2/
  QtMultimedia are `26.0` — so the real floor today is **macOS 26.0**;
- **no GUI acceptance testing** — window creation, notifications, media
  playback, microphone capture, Keychain behaviour, and Retina rendering remain
  **NOT TESTED**.

When the project's `GIPHY_API_KEY` / `KLIPY_API_KEY` variables are available the
build **deliberately embeds** them, exactly as the Windows test path does, so the
GIF picker works without local configuration. A key compiled into a binary is
ultimately extractable — these artifacts are developer-scoped and expire in
seven days. Validation records that the keys were embedded on purpose; the check
it *does* enforce is the inverse, that a build reporting itself keyless contains
no key.

There is deliberately **no publishing macOS job**. The bundle always reports
`build_kind: unsigned-test` regardless of pipeline, and
`tests/test-pipeline-config.py` asserts that no publishing macOS job exists,
that `publish-packages` does not consume the bundle, and that no macOS-tagged
job carries a release action: publishing an artifact Gatekeeper blocks would be
worse than shipping nothing.
See [`docs/macos-packaging.md`](docs/macos-packaging.md) for the architecture
decision, the bundle layout, and the ordered path to a releasable signed +
notarized build.

### Build caching

Every package runner bind-mounts a private host directory at `/cache` into its
job containers. `configure-build.sh` uses it for a persistent cargo home
(registry + crate sources), a persistent cargo target dir (symlinked over the
source-pinned `<build>/rust` location), and — for build-only pipelines — a
ccache. `build-flatpak.sh` persists the flatpak runtimes and, for build-only
pipelines, the flatpak-builder state dir and its ccache. Official
(key-embedding) builds deliberately disable the C++ compile caches so no
object compiled from the generated GIF-key header ever persists outside the
job; the Rust tree never sees the keys, so cargo caching stays on. Caches are
per-runner and contain only material derived from public sources. A cold
runner (or wiped cache) simply rebuilds at full cost.

### Publish-path routing

The publish/verify/release jobs route their API requests through the internal
GitLab endpoint (`PUBLISH_API_BASE`, `http://10.195.35.2/api/v4`) — the same
convention the runner fleet uses for polling and clone traffic — because the
public hostname is Cloudflare-proxied with a ~100 MB request-body cap that the
Flatpak/AppImage/snap files exceed. The durable URLs recorded in the manifest
and in release links are always built from the canonical public
`CI_API_V4_URL`; credentials are sent only in request headers on both paths.

Every `image:` in `.gitlab-ci.yml` (and the runner manager in
`infrastructure/windows-runner/compose.yml`) is pinned by digest, which
freezes the base layer against upstream CVE rebuilds — every job still runs
`apt-get update` / `apk add` at build time, so only the base layer is frozen.
Refresh the digests each release round with
`skopeo inspect --format '{{.Digest}}' docker://docker.io/<image:tag>` (or
`docker buildx imagetools inspect <image:tag>`) and update the trailing
`# <tag>` comment beside each one. The hand-built `lightning-windows-builder`
image is the one exception: it is never pulled from a registry, and its own
`FROM` is digest-pinned in `packaging/windows/Dockerfile`.

The GitHub mirror is now the full fallback, not only for bytes: the
`update-latest` release slot carries the current signed manifest pair
(written after every GitLab promotion, replaced in place, never GitHub's
"latest release"), so installed clients keep checking for and installing
updates when GitLab is unreachable. A preflight job proves the mirror token
is usable — and not about to expire — before a publishing pipeline writes
anything. See `docs/update-manifest.md`, "The GitHub update slot".

Two consequences of the plaintext internal path are worth knowing. The API
client (`scripts/gitlab-api.sh`) sends the token with `--max-redirs 0`: curl
re-sends a custom `JOB-TOKEN:` header to a redirect target on another host,
so a redirect is treated as the anomaly it is rather than followed. And the
publish-then-verify round trip runs over the same cleartext channel, so it
proves the registry holds the bytes that were sent, not that nothing on the
segment could have altered them in transit; the anonymous **https** checks
run by hand after every release (CLAUDE.md §14 in the application repository)
are what cover that. Terminating TLS on the internal endpoint and putting
`PUBLISH_API_BASE` back on `https://` is the follow-up that closes it.

## Modes

### Build-only (default, non-publishing)

`BUILD_FORMATS` selects which formats a build-only pipeline runs: `all`
(default) or a comma list from `deb,rpm,flatpak,appimage,snap` — e.g.
`BUILD_FORMATS=flatpak` iterates on one format without rebuilding the rest.
`snap` additionally requires `appimage` in the list (it repackages the
AppImage job's AppDir). Publishing pipelines ignore the selection and always
build every format.

```text
PUBLISH_PACKAGES=false
SOURCE_REF=main          # or any branch / tag / SHA
```

Resolves, builds, and validates. Snapshot metadata carries Git provenance
(`X.Y.Z+gitYYYYMMDD.<short>` for Debian). Nothing is uploaded and no release is
touched.

### create — a new release (vX.Y.Z+)

Publishes packages, then creates the tag and GitLab Release as the final action.

```text
PUBLISH_PACKAGES=true
RELEASE_ACTION=create
RELEASE_VERSION=0.7.0
SOURCE_REF=<full 40-char commit SHA on project 6 main>
RELEASE_NOTES_B64=<base64 Markdown>     # optional; see Release notes
```

Requires: the SHA is reachable from project 6 `main`; the source's CMake version
equals `RELEASE_VERSION`; no `vRELEASE_VERSION` tag or release exists yet; the
registry has no conflicting binaries.

### attach-existing — backfill an existing release

Publishes packages and adds package links to an already-published release,
without touching the tag, notes, or source archives.

```text
PUBLISH_PACKAGES=true
RELEASE_ACTION=attach-existing
RELEASE_VERSION=0.6.1
SOURCE_REF=v0.6.1        # or the exact commit SHA
```

Requires: the tag exists and still points at the resolved SHA; the release
exists with a matching version.

## Variables

| Variable | Meaning |
| --- | --- |
| `SOURCE_REF` | Build ref. Publication requires a full 40-char SHA (`create`) or `vX.Y.Z`/SHA (`attach-existing`). Never a bare branch for publication. |
| `RELEASE_VERSION` | Application version without a leading `v`, e.g. `0.6.1`. Required to publish; must equal the source's CMake version. |
| `RELEASE_ACTION` | `create` or `attach-existing`. Any other value is rejected. |
| `PUBLISH_PACKAGES` | `false` by default (safe, non-publishing). `true` enables publish/verify/release. |
| `BUILD_FORMATS` | Build-only format selection: `all`, `none`, or a comma list from `deb,rpm,flatpak,appimage,snap`. `none` is required for a Windows- or macOS-only test pipeline. |
| `BUILD_WINDOWS_PACKAGES` | `false` by default. `true` enables the unsigned Windows cross-package test job. |
| `BUILD_MACOS_PACKAGES` | `false` by default. `true` enables the unsigned macOS arm64 `.app` test job on the Mac mini runner. |
| `RELEASE_NOTES_B64` | `create` only: base64-encoded Markdown release notes. |
| `TARGET_PROJECT_ID` / `LIGHTNING_PROJECT_ID` | Fixed to `6`; scripts reject any other value so publication cannot be redirected. |
| `PACKAGE_NAME` | `lightning`. |
| `LIGHTNING_REPOSITORY` | Canonical project 6 clone URL, verified in-script. |
| `UPDATE_SIGNING_KEY_B64` | **Protected + masked, environment scope `signing`.** Base64 PKCS#8 PEM of the Ed25519 update-signing **private** key. Present only in `resolve-source` (the fail-fast key check; runs no project-6 code) and `sign-update-manifest`. Never in a build job, never logged, never in argv, never in an artifact. The scope is a project-7 setting; until it is set the key is injected into every job. |
| `UPDATE_SIGNING_KEY_ID` | **Protected, scope `*`.** Key id written into the signature envelope and matched against Lightning's compiled-in trust table. Not secret; every job derives the public-key variable name from it, so it must NOT be scoped. |
| `UPDATE_SIGNING_PUBKEY_2026A` | **Protected, not masked** (it is not secret). Base64 raw 32-byte **public** key for key id `lightning-release-2026a`. Every build job compiles it into the package as `-DLIGHTNING_UPDATE_PUBKEY_2026A`; it is the update trust root of the shipped binary. The name is key-id specific — a new key id needs its own variable *and* a new trust-table row in the application source. |
| `GITHUB_MIRROR_REPO` | **Optional. Protected**, not masked. `<owner>/<repo>` of the GitHub binary mirror, e.g. `Mizerd/lightning`. Unset ⇒ no mirroring and no `mirror_url` in the update manifest. This is the single on/off switch. |
| `GITHUB_MIRROR_TOKEN` | **Optional. Protected + masked, environment scope `mirror`.** GitHub token that may create a release and upload assets on `GITHUB_MIRROR_REPO`, and nothing else. Required when `GITHUB_MIRROR_REPO` is set. Only `mirror-release-to-github` declares that environment. Never logged, never in argv, never in an artifact. |

`UPDATE_SIGNING_KEY_*` and `UPDATE_SIGNING_PUBKEY_<id>` are required for a
publishing pipeline, and `scripts/check-update-signing-keys.sh` (run first, in
`resolve-source`, and again in `sign-update-manifest`) refuses to publish unless
the public variable is set **and** is the public half of the configured private
key. Without that check a pipeline can succeed while publishing a correctly
signed manifest that every package it just built must reject — a failure nobody
sees until a user clicks "check for updates", and one that cannot be corrected
without a new release. The optional `UPDATE_CHANNEL_*`, `UPDATE_RELEASED_AT`,
`UPDATE_MIN_UPDATER_VERSION`, `UPDATE_INCLUDE_RELEASE_NOTES`, and
`LIGHTNING_RELEASE_BASE_URL` variables, the rotation procedure, and the
compromise procedure are in
[`docs/update-manifest.md`](docs/update-manifest.md).

A publishing pipeline with wrong or missing inputs fails **loudly and early** in
`resolve-source` (see `scripts/validate-release-request.sh`) rather than
silently building without publishing.

### Release notes

For `RELEASE_ACTION=create`, notes come from **either**:

- `RELEASE_NOTES_B64` — base64-encoded Markdown, decoded to a temporary file
  inside the job (never `eval`'d, never executed, never fully logged); **or**
- a tracked file in the source ref: `docs/releases/v<version>.md`, captured
  during `resolve-source`.

`RELEASE_NOTES_B64` takes precedence. `attach-existing` never edits notes.

## Publication manifest

`scripts/write-manifest.sh` produces `dist/manifest.json`, the single
authoritative list of files approved for permanent publication. Entries are
**declared explicitly** (never discovered by a `dist/` wildcard). Each entry
records local path, filename, format, architecture, application version, source
SHA, SHA-256, size, release asset display name, direct asset path, and the full
registry URL.

For the current task the manifest contains exactly two entries (the `.deb` and
the `.rpm`). A future package format is added by appending one more explicit
entry in `write-manifest.sh`; the registry version layout (`lightning/<version>`)
does not change. Checksums, logs, `source-info.json`, `version.json`,
`build-info.json`, `.sha256` sidecars, reports, and build directories are never
uploaded.

## Signed update manifest

Separately from the publication manifest above, the pipeline builds, signs, and
publishes a small **client-facing** document that Lightning's in-app updater
polls: `update-manifest-v1.json` plus a detached Ed25519 signature envelope
`update-manifest-v1.json.sig`, under the generic package `lightning-update`.

```text
${CI_API_V4_URL}/projects/6/packages/generic/lightning-update/latest/update-manifest-v1.json
${CI_API_V4_URL}/projects/6/packages/generic/lightning-update/<version>/update-manifest-v1.json
```

- Every filename, size, SHA-256, and URL is **copied** from `dist/manifest.json`
  after `verify-published-packages` has already re-downloaded and hash-checked
  those exact bytes. Nothing is re-hashed locally.
- It advertises only the six **directly downloadable** install types
  (`windows-msi`, `windows-setup`, `windows-portable`, `linux-appimage`,
  `linux-deb`, `linux-rpm`). The Flatpak and Snap bundles are published as
  release files but appear in a `channels` block as `available: false` — a
  Flatpak or Snap install is updated by its own ecosystem, and offering those
  users a download would be telling them an action is available that they must
  not take. There is no Flathub publication, no Snap Store publication, and no
  APT or DNF repository, and the manifest says so in plain text.
- The signing key is an Ed25519 private key held only as a protected+masked CI
  variable; the matching public key is compiled into Lightning — from the
  `UPDATE_SIGNING_PUBKEY_<id>` variable, by every build job — so **the server
  can never introduce a trusted key**. That the two really are halves of one
  keypair, and that the key id is one a shipped client trusts, is asserted
  before any package is built (`scripts/check-update-signing-keys.sh`).
- The per-release copy is immutable like every other published file. The
  `latest/` slot is the pipeline's one deliberate, scoped exception, and it is
  written only after the versioned copy has been read back and verified.

**Windows Authenticode signing is still not active** (see
[Code signing](#code-signing)), and there is no signed APT/DNF repository, so
today this manifest is the actual cryptographic integrity guarantee for an
in-app update.

Format, signature envelope, key generation, CI variables, the rotation
procedure, and the emergency compromise procedure are documented in
[`docs/update-manifest.md`](docs/update-manifest.md).

## GitHub binary mirror

**GitLab decides what Lightning may install; GitHub is only a faster place to
get the bytes GitLab already decided on.**

`scripts/mirror-release-to-github.sh` (stage `mirror`, between `release` and
`update`) creates a GitHub Release at the **same tag** and uploads the **exact
bytes already published** — the local `dist/` files recorded in
`dist/manifest.json`, which `verify-published-packages` had already
re-downloaded and hash-checked. It rebuilds nothing, fetches from nowhere else,
and alters nothing. It then re-downloads every uploaded asset **anonymously**
(no token — what a client actually does) and compares SHA-256 against
`dist/manifest.json`; any mismatch, missing asset, or size difference fails the
job. The signed update manifest gains one optional field per artifact,
`mirror_url`, pointing at the immutable
`https://github.com/<repo>/releases/download/<tag>/<filename>`; the canonical
GitLab `url` stays required and stays the fallback.

Properties worth stating plainly:

- **A compromise of the GitHub mirror alone cannot ship a trusted update.** The
  manifest is fetched only from GitLab and is signed by an Ed25519 key GitHub
  never holds, and the SHA-256 each download is verified against is fixed before
  any byte is fetched. Replacing a mirrored file yields a failed hash check and a
  fallback to GitLab — a denial of service on the fast path, not an install.
- **Nothing GitHub says is an input to a decision.** No `api.github.com` lookup,
  no GitHub release metadata, no tags, no `/releases/latest` — not in the client,
  and not in the URL the manifest carries.
- **GitLab remains the release authority.** The mirror job runs only after
  `finalize-release`, waits (bounded, default 300 s) for GitLab's push mirror to
  deliver the tag, and refuses unless that tag peels to the released commit — so
  GitHub can never invent a tag of its own. Nothing is uploaded and no GitHub
  release is created when that check fails.
- **It never overwrites.** An already-present byte-identical asset is accepted so
  a retry converges; one that differs in bytes or size is a hard failure.
- **Optional.** With `GITHUB_MIRROR_REPO` unset, the job is a clean no-op and the
  manifest carries no `mirror_url` — never a URL that will not resolve.

**Operator step:** create `GITHUB_MIRROR_REPO` (protected) and
`GITHUB_MIRROR_TOKEN` (protected + masked) on project 7. **This replaces the
manual `gh release create v<version> --verify-tag …` step** that used to follow
every pipeline: the GitHub Release, its assets, and their verification are now
part of the pipeline. Details, the trust argument, and the tag-wait knobs are in
[`docs/update-manifest.md`](docs/update-manifest.md#github-bandwidth-mirror).

## Registry, verification, and immutability

Uploads target project 6's Generic Package Registry:

```text
${CI_API_V4_URL}/projects/6/packages/generic/lightning/<version>/<filename>
```

Published versions are immutable. Before each upload the publisher preflights the
destination: an identical existing file is accepted; a different file at the same
path fails; a released file is never overwritten or deleted. Partial failures
roll back only the files this pipeline proved absent, so a retry converges (e.g.
after a `.deb`-uploaded / `.rpm`-failed run, a rerun confirms the `.deb`, uploads
the missing `.rpm`, and continues).

`verify-published-packages` independently queries the Packages API and confirms
the package is `lightning`, the version matches, the type is generic, exactly the
manifest's files exist (no unexpected extras), stored sizes and (where GitLab
exposes them) SHA-256 values match, and every file **downloads** with the
expected checksum. The release job depends on this verification.

The bytes uploaded are the exact bytes the clean-install jobs validated: the
manifest checksum is compared against the on-disk file before upload, and the
same file flows build → validate → publish through artifact dependencies.

## Release asset links

Each published package becomes a release asset link with `link_type: package`:

```text
Lightning <version> — Debian amd64   ->  .../packages/generic/lightning/<version>/lightning_<version>_amd64.deb
Lightning <version> — RPM x86_64     ->  .../packages/generic/lightning/<version>/lightning-<version>-1.x86_64.rpm
```

Links use a stable `direct_asset_path` (`/packages/<version>/<filename>`), point
to the registry (never a project 7 artifact), and never expire with a job.
`attach-existing` adds links idempotently: an identical link is kept; a reused
name or URL with conflicting metadata fails without deleting unrelated links.
`create` includes the links atomically in the Releases API request.

## Packages and clean-system validation

Release filenames:

```text
lightning_<version>_amd64.deb
lightning-<version>-1.x86_64.rpm
lightning_<version>_amd64.flatpak
Lightning-<version>-x86_64.AppImage
lightning_<version>_amd64.snap
```

The deb/rpm install natively; the Flatpak is a single-file bundle
(`flatpak install ./lightning_<version>_amd64.flatpak`, runtimes come from
Flathub); the AppImage is self-contained and unsandboxed (`chmod +x`, run);
the snap installs with `sudo snap install --dangerous ./lightning_<v>_amd64.snap`
(built with a snap-pack-equivalent squashfs; strict confinement is declared
but not exercised against a live snapd in CI — the fleet cannot run snapd).
Flathub and the Snap Store remain future decisions, not targets of this
pipeline.

Both install `/usr/bin/matrix-client`, `/usr/bin/lightning-updater` (the small
Qt6::Core-only update helper — see
[`docs/package-layout.md`](docs/package-layout.md)), a desktop file, AppStream
metadata, the GPL-3.0-or-later licence and README, and the packaging copyright.
QML and app resources are compiled into the application executable and the Rust
bridge is statically linked; native system libraries are dynamically linked and
declared via `dpkg-shlibdeps` (run over both executables) / RPM automatic
dependencies. Each package records its exact
source SHA in build metadata (Debian changelog, RPM changelog) without changing
the public version.

Validation jobs receive only the final package and short-lived metadata, not a
source checkout or build tree. They install through APT/DNF, run the installed
binary's `--version` from `/tmp`, launch the mock backend with
`QT_QPA_PLATFORM=offscreen`, audit libraries and dynamic tags, and uninstall.
They reject missing libraries, any RPATH/RUNPATH, `/nix/store`, `/home/roksme`,
`/builds/`, credential markers, source/build products, world-writable files, and
unexpected setuid/setgid files. Nix must be absent. Publication and release
depend on both validators; nothing is published from a failed or skipped
validator.

Install downloaded packages with:

```bash
sudo apt install ./lightning_<version>_amd64.deb
sudo dnf install ./lightning-<version>-1.x86_64.rpm
```

### The call media engine (GStreamer)

Lightning 0.8.0 shipped **every** Linux package with calling, screen sharing
and the camera compiled out. The published deb answered
`call media engine built in: no` and `ldd` named no GStreamer at all. Nothing
caught it because an engine-less build installs, launches, syncs and behaves
perfectly, and only refuses calls.

The cause was silence, not a wrong value. The source's `LIGHTNING_ENABLE_WEBRTC`
defaults to `ON`, but it is only honoured when a pkg-config probe finds
`gstreamer-1.0`, `gstreamer-webrtc-1.0`, `gstreamer-sdp-1.0`,
`gstreamer-app-1.0`, `gstreamer-video-1.0` and `gstreamer-rtp-1.0`. No Linux
build job installed any of them, so CMake set `HAVE_LIGHTNING_WEBRTC` `OFF`,
said so in one `STATUS` line among hundreds, and every check downstream passed.
Windows and macOS shipped the same way for months in 2026 for the same reason.

Three things now have to hold, and `tests/test-pipeline-config.py` pins all
three.

**Build.** Every job that compiles installs the development files:
`libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
libgstreamer-plugins-bad1.0-dev` on Debian/Ubuntu (deb, AppImage) and
`gstreamer1-devel gstreamer1-plugins-base-devel
gstreamer1-plugins-bad-free-devel` on Fedora (rpm). The Flatpak compiles inside
`org.kde.Sdk//6.9`, which supplies all six modules; the snap compiles nothing —
it repacks the AppImage job's AppDir.

**Assert.** `configure-build.sh` passes `-DLIGHTNING_ENABLE_WEBRTC=ON` and then
runs `--call-media-status` on the *staged binary*, failing the build unless it
reports `call media engine built in: yes`. Naming the option changes nothing
CMake does; it changes what the script is entitled to assert. The Flatpak
manifest carries the same guard inline, because it is the one packaging build
that does not run that script.

**Prove.** Every `validate-<format>.sh` runs `--call-media-status` against the
*shipped artifact* and judges it through one shared helper
(`assert_call_media_engine` in `scripts/lib.sh`), requiring both the
compiled-in line and `RESULT: calls can be placed and answered.` The engine's
own element probe — the same function `AppController` calls — decides that,
which is why it also proves each format's plugin story:

| Format | How the plugins arrive |
|---|---|
| deb | `Depends:` — `gstreamer1.0-plugins-{base,good,bad}`, `gstreamer1.0-nice`, `gstreamer1.0-pipewire`, `gstreamer1.0-alsa` (`CALL_DEPENDS` in `build-deb.sh`) |
| rpm | `Requires:` — `gstreamer1-plugins-{base,good,bad-free}`, `libnice-gstreamer1`, `pipewire-gstreamer` (`packaging/rpm/lightning.spec`) |
| flatpak | the `org.kde.Platform//6.9` runtime, which carries all of them |
| AppImage | **bundled** into `usr/lib/gstreamer-1.0` plus an AppRun hook setting `GST_PLUGIN_SYSTEM_PATH_1_0` — an AppImage has nobody to depend on |
| snap | inherits the AppImage's AppDir; `bin/lightning-launch` sets the same variables, because the snap takes only `usr/` and linuxdeploy's AppRun stays behind |

`dpkg-shlibdeps`, RPM's automatic generator and linuxdeploy can see **none** of
this: GStreamer plugins are `dlopen`'d from a plugin path and appear in no ELF
`NEEDED` entry. The lists are therefore explicit, and the AppImage's is fatal
when a name is missing rather than best-effort — it used to be wrapped in an
`if [ -d ... ]` that skipped silently on every build.

Two elements are load-bearing and invisible to that check, because neither is
in the engine's required-element list. **`ximagesrc`** is the X11 screen-share
fallback used when no xdg portal answers; `SfuCallController` probes the
*running registry* for it, and the AppImage hook and snap launcher **replace**
the system plugin path — so an unbundled `ximagesrc` is invisible inside those
two formats, the route refuses, and the user is told to install
`gst-plugins-good`, which they probably already have and which would change
nothing. **An audio sink** is the other: `autoaudiosink` resolves to
`pipewiresink`, `pulsesink` or `alsasink`, but the engine only probes the
`autodetect` *factories*, which exist whether or not any sink is installed — so
`--call-media-status` is green on a package that cannot make a sound. Debian
splits ALSA into `gstreamer1.0-alsa`; Fedora does not (base carries
`libgstalsa`, good carries `libgstpulseaudio`), which is why only the deb needs
the extra name.

Two entries in the bundled lists are worth knowing about. `libgstsctp` is named
nowhere in Lightning: `webrtcbin` loads it itself for the data channel, and
LiveKit's subscriber offer puts one in media section 0, which under
`bundle-policy=max-bundle` owns the transport every audio and video section
rides on — Windows shipped for months able to send and unable to receive
because of that one file. And `libgstvolume`, `libgstaudiotestsrc`,
`libgstvideotestsrc` and `libgstapp` are in the engine's own required-element
list, so an AppImage without them reports the engine unavailable even when it
is compiled in.

Two limits are deliberate and stay honest. A **Flatpak has no camera**:
`SfuMediaEngine` opens `v4l2src` on Linux, Flatpak has no camera-only device
permission, and the alternatives are `--device=all` (every device on the
machine) or a client change to take a PipeWire node from the portal's Camera
interface. Audio calls and screen sharing are unaffected. And a **snap needs
`audio-record`** connected; it is declared, but it is not auto-connected
everywhere, so a user may have to run `snap connect lightning:audio-record`.

## Authentication and security

The preferred credential is project 7's short-lived `CI_JOB_TOKEN`. Project 6's
inbound CI/CD job-token allowlist already includes project 7, and the triggering
maintainer's project 6 role authorizes Generic Package Registry writes, Packages
API reads, and Releases/Release-Links writes.

If a future GitLab version rejects the required cross-project writes for job
tokens, create an expiring project 6 project access token named
`lightning-deploy-publisher` with the minimum scope, store it in project 7 as the
**masked and protected** `LIGHTNING_PUBLISH_TOKEN`, and the scripts will use it
as `PRIVATE-TOKEN`. Do not use a personal token. Tokens are sent only in request
headers; scripts never enable shell tracing around credentials, embed tokens in
URLs, or print token values.

Project 6 is public with the package registry enabled, so anyone who can view the
release can download the linked `.deb`/`.rpm`. Note: project 6's
`releases_access_level` is currently **private**; if the Releases page should be
publicly visible, raise it to `enabled` in project 6 → Settings → General →
Visibility. This is a deliberate project-6 setting and is not changed by the
pipeline.

## GIF provider keys

Official packages embed application-level GIPHY/KLIPY keys so an installed client
opens the GIF browser without the user creating keys or setting any environment
variable. The keys are supplied only by protected CI, never committed, and never
printed.

Required project 7 CI/CD variables (values never appear in this repo or docs):

| Variable | Requirement |
| --- | --- |
| `GIPHY_API_KEY` | Protected + masked. Consumed only by the build jobs. |
| `KLIPY_API_KEY` | Protected + masked. Consumed only by the build jobs. |

Because they are **protected**, they are exposed only to pipelines on the
**protected** `main` branch. Project 7 `main` must remain a protected branch; if
it is not, protect it rather than unprotecting the variables.

Flow: `resolve-source` presence-checks both variables when `PUBLISH_PACKAGES=true`
(printing only `GIPHY_API_KEY is configured` / `KLIPY_API_KEY is configured`) so a
keyless release fails before the expensive build. `configure-build.sh` maps them
to the build-only `LIGHTNING_BUILD_GIPHY_API_KEY` / `LIGHTNING_BUILD_KLIPY_API_KEY`
in the build job's environment and configures the source with
`-DLIGHTNING_REQUIRE_GIF_KEYS=ON`; the source generator writes them into a
build-tree header the compiler embeds, then the script scrubs that header. Values
never reach a command line, `CMakeCache.txt`, ninja files, a dotenv report, an
artifact, or a log. Build-only pipelines (`PUBLISH_PACKAGES=false`) require no
keys and ship the keyless missing-key state.

Clean-package validation runs, with **every** provider-key variable unset, the
installed binary's `--gif-status` (booleans only) and, for publishing pipelines,
`--gif-selftest` (a bounded real GIPHY and KLIPY trending request using only the
embedded keys). It also confirms the generated header is not inside the package
and that no key or authenticated URL appears in the diagnostics. The release job
is downstream of both validators, so a release is never created when a key is
missing, was not embedded, provider status is unconfigured, a live request fails,
or leakage is detected.

An application key compiled into a distributed desktop binary is inherently
extractable; this scheme spares normal users manual configuration and protects
the keys from accidental plaintext disclosure, not from a determined extractor.

### Key rotation

A compiled key cannot be changed in packages already released. To rotate:

1. Replace the protected `GIPHY_API_KEY` / `KLIPY_API_KEY` CI variable.
2. Cut a new Lightning release version (`RELEASE_ACTION=create`).
3. Build and validate both providers with the new key.
4. Publish the new immutable package version and finalize the release.

Never overwrite an older released package file, and never attempt to replace a
key inside an already-distributed binary.

## v0.6.1 backfill example

```text
SOURCE_REF=v0.6.1
RELEASE_VERSION=0.6.1
RELEASE_ACTION=attach-existing
PUBLISH_PACKAGES=true
```

Resolves `v0.6.1` to `86d30b4`, rebuilds and revalidates both packages,
publishes `lightning/0.6.1` (two files), verifies them, and adds the two package
links to the existing `Lightning 0.6.1` release without altering the tag, notes,
or source archives.

## Tests

- `tests/test-version.sh` — version resolution (snapshot, exact tag, publishing).
- `tests/test-publication.sh` — request gate, manifest, idempotent uploads,
  conflicts, partial-upload rollback/retry, verification, and both release
  actions against a stateful mock GitLab API.
- `tests/test-update-manifest.sh` — the signed update manifest: generation from
  a verified publication manifest, the exact artifact key set and metadata
  passthrough, refusal without `dist/verification.json`, flatpak/snap absence
  from the directly-updatable artifacts, channel honesty, deterministic output,
  an Ed25519 sign→verify round trip with a key generated inside the test, tamper
  and wrong-key rejection, absence of key material from every captured log,
  per-release immutability, and the guarantee that the `latest` slot is written
  only after the versioned copy. It also covers the signing-key consistency
  gate: a public key that is not the private key's half, an empty or malformed
  `UPDATE_SIGNING_PUBKEY_<id>`, and a key id no shipped Lightning trusts are
  each rejected — in the gate itself and through `validate-release-request.sh` —
  while a build-only pipeline stays unaffected. No key material is committed.
  It also covers the **GitHub binary mirror**: the deterministic `mirror_url`
  when mirroring is enabled and its complete absence when it is not, preserved
  determinism, rejection of a repository value that could steer a URL, a
  configured repository with no token failing hard instead of skipping, an
  unmirrored tag and a tag on the wrong commit both failing with **no** GitHub
  release created, byte-identical upload of every publication-manifest artifact,
  anonymous read-back verification (while API calls stay authenticated), an
  idempotent re-run that uploads nothing, an already-present asset differing in
  bytes or size being a hard failure that overwrites nothing, and the mirror
  token appearing in no log, no URL, and no artifact.
- `tests/test-gif-key-injection.sh` — the publish-time GIF-key presence gate and
  the safe mapping of CI variables into the build (env, not command line; never
  logged), using synthetic canary values only; plus the mandatory, validated
  `LIGHTNING_INSTALL_TYPE` and the update trust root reaching the build.
- `tests/test-pipeline-config.py` — required stages/jobs, publish/verify/sign/
  release/mirror/update gating for both actions, dependency wiring (including
  that the release waits for a signable manifest, that the mirror waits for the
  release, and that the `latest` promotion waits for the mirror), and the two
  platform test gates:
  that a macOS-only request creates no Linux or Windows build job, and that no
  macOS job can ever reach the publication chain.
- `tests/test-windows-metadata.py` — the Windows product-metadata gate, against
  synthetic PE files with real version resources: correct metadata for both
  Lightning-owned binaries passes, and a version mismatch, a wrong product name,
  a missing copyright, a wrong publisher, a missing Lightning-owned binary, a
  missing update helper, a helper carrying the application's `OriginalFilename`,
  and an upstream DLL claiming `ProductName=Lightning` each fail.
- `tests/test-windows-version-resources.sh` — the per-target Windows version
  resource: driven against a two-executable stand-in CMake project, it asserts
  that each executable receives its **own** resource, that the update helper
  never inherits the application's, and that a missing resource object or a
  renamed target fails the configure by name instead of silently producing
  binaries with no version resource.
- `tests/test-release-notes-policy.py` — every created release description
  carries a **Code signing policy** link, idempotently, with the correct
  signed/unsigned wording.

`config-tests` runs all eight on every pipeline.

## Code signing

One thing **is** signed: the update manifest. Ed25519, with the public key
compiled into Lightning — see
[Signed update manifest](#signed-update-manifest) and
[`docs/update-manifest.md`](docs/update-manifest.md). That covers the in-app
update path only; it does nothing for the OS-level trust prompts below.

Windows artifacts are **not signed**. The preparation for SignPath Foundation
signing — what is already in place, what is blocked on SignPath onboarding for
this self-managed GitLab instance, and where the signing pipeline must run for
its provenance to be verifiable — is documented in
[`docs/signpath-integration.md`](docs/signpath-integration.md).

One switch decides the signed/unsigned story everywhere: `windows_signed` in
`scripts/lib.sh`, driven by `LIGHTNING_WINDOWS_SIGNED` (default `false`). It
governs PE and MSI metadata, published asset names, and the release description,
so none of them can claim a signature that does not exist. Flip it only in the
change that actually signs.

macOS artifacts are **not signed for distribution** either. The bundle carries
an **ad-hoc** signature because Apple Silicon will not execute unsigned pages at
all — that makes it runnable, not distributable. It is not Developer ID signed
and not notarized, so Gatekeeper blocks it everywhere except the build machine,
which is why there is no publishing macOS job. Note that the Mac runner has
FileVault disabled and automatic GUI login (so the runner LaunchAgent starts
unattended), so importing a Developer ID key there needs a deliberate keychain
model decided first — see [`docs/macos-packaging.md`](docs/macos-packaging.md).

## Release ordering (authoritative)

1. Prepare and push the release commit on project 6 `main` (version + notes).
2. Do **not** manually create the tag or release.
3. Trigger this pipeline in `RELEASE_ACTION=create` mode.
4. Resolve and verify the exact project 6 commit.
5. Build and clean-system-validate every package.
6. Publish them to `lightning/<version>` and verify they download.
7. Build and Ed25519-sign the update manifest, and self-verify the signature —
   **before** anything irreversible happens, so a bad signing key costs nothing.
8. Create the tag + release, with package links attached.
9. Mirror the published binaries to the GitHub Release at the same tag (only
   after that tag has been push-mirrored and proved to be the released commit),
   and verify every asset by downloading it anonymously and comparing SHA-256.
   Skipped cleanly when `GITHUB_MIRROR_REPO` is unset.
10. Publish the signed update manifest to `lightning-update/<version>`
    (immutable) and only then move the `lightning-update/latest` pointer, as the
    final action.
11. The release is complete only after source archives, package links, the
    mirrored assets, and both update-manifest slots verify.

Step 9 replaces the manual `gh release create v<version> --verify-tag …` that
used to follow a release; no hand-made GitHub Release is needed any more.

For an existing release missing packages, use `RELEASE_ACTION=attach-existing`.

Windows artifacts remain unsigned; the signed update manifest from steps 7 and 9
is what actually protects an in-app update today. See
[`docs/update-manifest.md`](docs/update-manifest.md).

See [docs/package-layout.md](docs/package-layout.md) for the installed layout.
