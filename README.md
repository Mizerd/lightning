# lightning-deploy

Private Debian and RPM packaging for the Lightning Matrix client. This is
GitLab project 7; it fetches an immutable source revision from the canonical
Lightning source project (project 6), builds native packages, validates each
on a clean target system, and optionally publishes the two final files to
project 6. It is not a second Lightning source repository.

The canonical local checkout is `/home/roksme/git/lightning-deploy`. The
canonical source checkout is `/home/roksme/git/lightning`.

## Pipeline

Only manually created `web` and `api` pipelines are accepted. The active
stages and jobs are:

| Stage | Job | Image and runner tags |
| --- | --- | --- |
| resolve | `resolve-source` | `debian:13.6-slim`; `apt`, `deb`, `package` |
| build | `build-deb` | `debian:13.6-slim`; `apt`, `deb`, `package` |
| build | `build-rpm` | `fedora:44`; `dnf`, `rpm`, `package` |
| validate | `validate-deb` | clean Debian 13.6; `apt`, `deb`, `package` |
| validate | `validate-rpm` | clean Fedora 44; `dnf`, `rpm`, `package` |
| publish | `publish-packages` | both validated files, serialized publication |
| release | `attach-release-assets` | two project 6 package links |

`resolve-source` resolves the requested ref to a full 40-character commit SHA.
Both builders refetch and must resolve the same SHA, so the two formats cannot
silently package different source. Project 7 build and diagnostic artifacts
expire after seven days; publication metadata expires after one day. A shared
build resource group keeps the two memory-heavy native builds sequential on
the GitLab VM.

NixOS packaging is deferred. There is no active Nix build, validation,
publication, or release-link job.

## Starting a pipeline

Set these manual variables:

- `SOURCE_REF`: a project 6 tag, branch, or full commit SHA. Examples are
  `v0.6.1`, `main`, and a 40-character commit SHA.
- `PUBLISH_PACKAGES`: `false` by default. Set exactly `true` only for an
  existing project 6 `vX.Y.Z` release.

Build-only mode accepts branches and commits and never uploads packages.
Publication requires all of the following:

- the pipeline runs from project 7's default branch;
- `SOURCE_REF` is exactly `vX.Y.Z` and matches Lightning's CMake version;
- the tag in project 6 still targets the resolved SHA;
- `PUBLISH_PACKAGES=true`;
- both clean-system validation jobs passed;
- the matching project 6 release already exists.

The pipeline does not create or alter source tags or create a missing release.

## Packages and standalone validation

Release filenames are:

```text
lightning_<version>_amd64.deb
lightning-<version>-1.x86_64.rpm
```

Both install `/usr/bin/matrix-client`, a desktop file, AppStream metadata, the
upstream GPL-3.0-or-later licence and README, and the packaging copyright
notice. QML and application resources are compiled into the executable and
the Rust bridge is statically linked. Native system libraries remain
dynamically linked and are derived by `dpkg-shlibdeps` or RPM automatic
dependency generation.

Validation jobs receive only the final package and short-lived metadata, not a
source checkout or build tree. They install through APT/DNF, run the installed
executable's version command from `/tmp`, launch the mock backend with
`QT_QPA_PLATFORM=offscreen`, audit libraries and dynamic tags, and uninstall.
They reject missing libraries, any RPATH/RUNPATH, `/nix/store`, `/home/roksme`,
`/builds/`, credential markers, source/build products, world-writable files,
and unexpected setuid/setgid files. Nix must be absent from each validation
container.

Install downloaded packages with:

```bash
sudo apt install ./lightning_<version>_amd64.deb
sudo dnf install ./lightning-<version>-1.x86_64.rpm
```

No compiler, Cargo, CMake, Git, Nix, source checkout, or build directory is
needed after installation.

## Registry and release assets

The only permanent publication destination is project 6's Generic Package
Registry:

```text
/projects/6/packages/generic/lightning/<version>/<filename>
```

Exactly the `.deb` and `.rpm` are uploaded. Checksums, logs, JSON metadata,
reports, and other temporary files remain expiring project 7 job artifacts.
Nothing in this pipeline uploads final packages to project 7's registry.

Publication preflights both destinations. An identical existing file is
accepted; different content at the same version and filename fails. The
publisher never overwrites an immutable released file. If a later upload
fails, it attempts to remove only files that this pipeline proved absent
before uploading. A `resource_group` serializes release publication.

The release job idempotently adds these `package` links to the existing
project 6 `v<version>` release:

```text
Lightning <version> Debian amd64
Lightning <version> RPM x86_64
```

An identical link is retained. A reused name or URL with conflicting metadata
fails without deleting unrelated manually added assets.

## Authentication and security

The preferred credential is project 7's short-lived `CI_JOB_TOKEN`. Project 6
must allow project 7 through its job-token allowlist and grant only source
read, Generic Package Registry write, release read, and release-link create
operations supported by the installed GitLab version.

If cross-project writes are unsupported for job tokens, create an expiring
project 6 project access token named `lightning-deploy-publisher`, give it the
minimum API scope needed, and store it in project 7 as the masked and protected
`LIGHTNING_PUBLISH_TOKEN` variable. Do not use a personal token. Variable names
that may exist are `LIGHTNING_PUBLISH_TOKEN` and the legacy read-only
`LIGHTNING_DEPLOY_USER`/`LIGHTNING_DEPLOY_TOKEN` fallback; their values must
never be committed or documented.

Tokens are sent only in request headers. Scripts do not enable shell tracing,
embed credentials in URLs, or print token values. Only trusted maintainers
should be able to change the default-branch CI configuration or start a
publishing pipeline.

## Troubleshooting

- **Runner unavailable:** confirm the three expected tags and the matching
  Debian or Fedora package runner are online.
- **Source tag missing or moved:** use a project 6 ref that exists; rerun only
  after confirming its immutable target. Publication rejects a changed SHA.
- **Release missing:** create the legitimate project 6 source release first;
  the packaging pipeline intentionally stops before upload.
- **Cross-project 401/403:** check project 6's job-token allowlist and
  fine-grained endpoint permissions. Use the protected project-token fallback
  only if this GitLab version cannot authorize the job token.
- **Duplicate package conflict:** compare the existing file out of band and
  publish changed binaries under a new release version; never overwrite it.
- **Missing runtime library:** inspect `dist/*-ldd.txt` and native package
  requirements, then fix dependency generation rather than preinstalling a
  hidden runner dependency.
- **QML module or plugin missing:** inspect the headless log and add the native
  runtime dependency or required packaged resource.
- **RPATH or build-path contamination:** inspect `dist/*-readelf.txt`; retain
  prefix remapping and remove generated RPATH before packaging.
- **Release link already exists:** identical links are safe. A conflicting
  name or URL requires deliberate operator review.

Published package versions are immutable. Roll back by publishing a corrected
new Lightning version or by removing the release link after explicit operator
review; do not casually delete registry files consumers may already use.

See [docs/package-layout.md](docs/package-layout.md) for the installed layout.
