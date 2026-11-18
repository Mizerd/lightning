# Building Lightning from source

The verified workflow uses the repository's Nix flake on Linux; the dev shell
supplies Qt 6.5+ and a Rust toolchain.

```sh
git clone https://github.com/lightning-matrix/lightning.git
cd lightning

nix develop -c cmake -S . -B build-rust -G Ninja -DENABLE_RUST_SDK_BACKEND=ON
nix develop -c cmake --build build-rust
scripts/run-dev.sh
```

That is the real client: Rust SDK backend, real Matrix, E2EE, threads and calls.
Release binaries are Rust-only (`-DLIGHTNING_RUST_ONLY=ON`).

There is also a lighter tree with the development-only mock and experimental HTTP
backends, for UI work and tests without a homeserver — it is compiled out of
release builds:

```sh
nix develop -c cmake -S . -B build -G Ninja
nix develop -c cmake --build build
```

## Building for a distribution

Distro packages that update through their own package manager should configure
with:

```sh
cmake -S . -B build -DLIGHTNING_ENABLE_UPDATER=OFF
```

That compiles out the in-app updater: no update checks or network access for
updates, no `lightning-updater` helper (not built, not installed), no update
notifications, and Settings > Updates shows one line saying updates are handled
by the package manager. Version and install-type detection stay (About still
reports the package kind). The default is ON; `LIGHTNING_ENABLE_UPDATER=OFF`
also leaves out the updater test suites.

## Tests

```sh
nix develop -c cargo test --manifest-path rust/Cargo.toml
nix develop -c ctest --test-dir build-rust --output-on-failure
nix develop -c ctest --test-dir build       --output-on-failure
```

Compilation and launch are not feature validation: live Matrix behaviour —
interoperability, decryption, notifications, calls, physical scrolling — has to be
tested against a real homeserver and reported honestly. See
[`build-and-test.md`](build-and-test.md) for the long form (manual test
procedures and troubleshooting), [`CONTRIBUTING.md`](../CONTRIBUTING.md) for
which suites to run for a change, and [`screenshot-demo.md`](screenshot-demo.md)
for the demo mode the screenshots come from.

## Faster local builds

`ccache` and `mold` are in the dev shell. A source build uses ccache
automatically when it is on `PATH` and no compiler launcher was set
(`-DLIGHTNING_DISABLE_CCACHE=ON` turns that off); mold is opt-in per build tree
(`-DCMAKE_LINKER_TYPE=MOLD`). Neither changes the produced binaries, and
official packages are built without them.
[`build-performance.md`](build-performance.md) has the measurements.

## How it is put together

```text
Qt 6 / QML  ──  presentation, interaction, theming, layout
     │
C++         ──  application state, Qt-facing models and controllers, lifecycle,
     │          account/room/thread isolation, navigation, notification policy
Rust bridge ──  FFI to…
     │
matrix-rust-sdk  ──  login/sync, timelines, threads, event cache, media,
                     Olm/Megolm E2EE, verification, key backup, receipts
```

QML owns presentation and interaction only — never protocol, credentials, crypto
or persistence. C++ owns the safe Qt-facing boundary and application state. The
official Rust Matrix SDK owns all Matrix protocol and cryptography. See
[`architecture.md`](architecture.md).
