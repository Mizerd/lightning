# lightning-deploy

Private Debian and RPM packaging **and release orchestration** for the Lightning
Matrix client. This is GitLab **project 7**. It fetches an immutable source
revision from the canonical Lightning source project (**project 6**), builds
native packages, validates each on a clean target system, publishes the
validated files to project 6's Generic Package Registry, verifies them, and
finally creates or updates the matching GitLab Release with package asset links.
It is not a second Lightning source repository.

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
            └── future explicitly approved formats

lightning-deploy project 7
├── source resolution     (resolve-source)
├── builds                (build-deb, build-rpm)
├── clean-system validation (validate-deb, validate-rpm)
├── publication           (publish-packages)
├── registry verification (verify-published-packages)
└── final release action  (finalize-release)
```

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
| validate | `validate-deb` | Clean Debian install/run/uninstall audit |
| validate | `validate-rpm` | Clean Fedora install/run/uninstall audit |
| publish | `publish-packages` | Build the manifest and upload to project 6 |
| verify | `verify-published-packages` | Re-download and check the registry files |
| release | `finalize-release` | Attach links / create the release (final action) |

The graph no longer ends after validation. When `PUBLISH_PACKAGES=true` the
publish, verify, and release jobs are always included for **both** release
actions; a build-only pipeline stops after validation by design.

`resolve-source` resolves the requested ref to a full 40-character commit SHA.
Both builders refetch and must resolve the same SHA, so the two formats cannot
package different source. A shared build `resource_group` keeps the two
memory-heavy native builds sequential; a second `resource_group` serializes all
project-6 registry/release writes.

## Modes

### Build-only (default, non-publishing)

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
| `RELEASE_NOTES_B64` | `create` only: base64-encoded Markdown release notes. |
| `TARGET_PROJECT_ID` / `LIGHTNING_PROJECT_ID` | Fixed to `6`; scripts reject any other value so publication cannot be redirected. |
| `PACKAGE_NAME` | `lightning`. |
| `LIGHTNING_REPOSITORY` | Canonical project 6 clone URL, verified in-script. |

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
```

Both install `/usr/bin/matrix-client`, a desktop file, AppStream metadata, the
GPL-3.0-or-later licence and README, and the packaging copyright. QML and app
resources are compiled into the executable and the Rust bridge is statically
linked; native system libraries are dynamically linked and declared via
`dpkg-shlibdeps` / RPM automatic dependencies. Each package records its exact
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
- `tests/test-pipeline-config.py` — required stages/jobs, publish/verify/release
  gating for both actions, and dependency wiring.

`config-tests` runs all three on every pipeline.

## Release ordering (authoritative)

1. Prepare and push the release commit on project 6 `main` (version + notes).
2. Do **not** manually create the tag or release.
3. Trigger this pipeline in `RELEASE_ACTION=create` mode.
4. Resolve and verify the exact project 6 commit.
5. Build and clean-system-validate both packages.
6. Publish both to `lightning/<version>` and verify they download.
7. Create the tag + release as the final action, with package links attached.
8. The release is complete only after source archives and package links verify.

For an existing release missing packages, use `RELEASE_ACTION=attach-existing`.

See [docs/package-layout.md](docs/package-layout.md) for the installed layout.
