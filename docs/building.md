# Building Lightning from source

The verified workflow uses the repository's Nix flake on Linux; the dev shell
supplies Qt 6.5+ and a Rust toolchain.

```sh
git clone https://gitlab.smetonis.net/Mizerd/lightning.git
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
