# lightning-deploy

Private, manually triggered Linux packaging pipelines for the Lightning Matrix
client. Every job independently fetches an explicitly selected revision from
the private `Mizerd/lightning` project, builds the production Rust SDK/E2EE
backend, validates the package, and publishes it only as a GitLab job artifact.

## Supported outputs

| Job | Runner | Output |
| --- | --- | --- |
| `package-deb` | `package-runner-apt` | Debian `.deb` |
| `package-rpm` | `package-runner-dnf` | Fedora/RHEL `.rpm` |
| `package-nix` | `package-runner-nix` | Portable file-based Nix cache archive |

Artifacts expire after 30 days. No job publishes a release or a public package
repository.

## Manual usage

1. Open `Mizerd/lightning-deploy` in GitLab.
2. Go to **Build → Pipelines** and select **New pipeline**.
3. Set `LIGHTNING_REF` to a branch, release tag, or full commit SHA.
4. Run the pipeline, then press the play button for one or more manual package jobs.
5. Download `dist/` from each successful job's artifacts.

Examples:

```text
LIGHTNING_REF=main
LIGHTNING_REF=v0.6.0
LIGHTNING_REF=2157194d2ddbfe57aa7a636f2ac5b188acc1bcc0
```

Only UI (`web`) and API pipelines are accepted. Package jobs never run merely
because this repository receives a push.

## Versioning

An exact `vX.Y.Z` source tag produces `X.Y.Z`. A branch or commit build uses
the version declared by Lightning's top-level CMake project plus date and SHA,
for example `0.6.0+git20260717.c197129`. RPM encodes the Git suffix in its
release field. `dist/version.json`, `source-info.json`, and `build-info.json`
record the complete resolution and provenance.

## Installation

```bash
sudo apt install ./lightning_<version>_amd64.deb
sudo dnf install ./lightning-<version>-1.x86_64.rpm
```

For Nix, extract the cache and copy the output path recorded in
`nix-path-info.json`:

```bash
tar --use-compress-program=zstd -xf lightning-<version>-x86_64-linux-nix-cache.tar.zst
nix copy --from "file://$PWD/nix-cache" /nix/store/<recorded-lightning-output>
```

## Security model

- Both projects remain private.
- `Mizerd/lightning` allows this project's short-lived `CI_JOB_TOKEN` through
  GitLab's inbound job-token allowlist. No long-lived source credential is
  committed or configured by default.
- A temporary `GIT_ASKPASS` helper supplies the token without putting it in a
  URL or trace; it is removed immediately and the clone remote is reset to the
  clean HTTPS URL.
- Jobs use only the exact designated tag triplets and pinned images.
- The runner managers control the host Docker socket. Therefore only trusted
  administrators should be allowed to change this private project's CI code.
- No signing key, Nix key, Matrix account, or decrypted Matrix content is used.

## Build implementation

Lightning requires CMake 3.21+, C++20, Qt 6.5+, Cargo, SQLite, and libsecret.
The pinned Matrix SDK 0.18 graph requires Rust 1.93; the Debian job uses an
ephemeral, explicitly selected official Rust 1.93 toolchain because Debian
13.6 provides Rust 1.85.
The pipeline enables `ENABLE_RUST_SDK_BACKEND=ON`, retains the source's E2EE
gate, honors `Cargo.lock`, fetches dependencies once, and lets CMake perform its
required `cargo build --offline --locked`. DEB and RPM packages use a staged
`cmake --install`; the Nix job vendors the same locked Cargo graph and builds a
derivation from the exact local checkout. Build parallelism is capped at two.

## Troubleshooting

- **No pipeline is created:** use **New pipeline** or the API; push pipelines
  are rejected by workflow rules.
- **Manual job remains pending / no matching runner:** verify all three job
  tags and confirm the corresponding package runner is online.
- **Private clone returns 403:** confirm this project is in Lightning's inbound
  CI job-token allowlist and the user starting the pipeline can read Lightning.
- **Source ref not found:** use an existing branch/tag or a full reachable SHA.
- **Out of memory / exit 137:** rerun with `BUILD_JOBS=1`; inspect runner and VM
  OOM events before changing resource limits.
- **Missing Qt dependency:** compare the CMake component error with the pinned
  image's Qt development package set; do not disable the Rust backend.
- **Rust dependency failure:** verify `Cargo.lock`, registry reachability during
  dependency resolution, and runner cache health. Locks are never auto-updated.
- **Installed package does not start:** inspect `dist/ldd.txt`, run
  `matrix-client --version`, then use `QT_QPA_PLATFORM=offscreen` for GUI smoke
  diagnostics without a Matrix account.
- **Nix store permission error:** verify the runner's persistent `/nix` bind is
  traversable and `sandbox = false`; privileged mode is not required.
- **Artifact too large:** measure it and the project limit. Prefer the private
  GitLab Generic Package Registry with `CI_JOB_TOKEN` rather than raising a
  global limit.

See [docs/package-layout.md](docs/package-layout.md) for installed paths.
