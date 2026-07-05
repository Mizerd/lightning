# matrix-client

A native desktop Matrix client written in C++20 / Qt 6 / QML.

**Not** Electron. **Not** Tauri. **Not** a webview wrapping a browser. The chat UI is real Qt Quick.

## Status

**v0.4** — Secure token storage + optional Matrix Rust SDK backend scaffold.

New in v0.4:

- `SecretStore` abstraction. On Linux, `LibSecretStore` talks to the Freedesktop Secret Service via libsecret (works with gnome-keyring, KWallet with libsecret support). If no session bus / Secret Service is reachable, an `InsecureFallbackSecretStore` keeps the app working and the Settings screen shows a red warning.
- Access tokens are stored in the `SecretStore`, not in QSettings. On first launch of v0.4 any legacy `session/accessToken` value in QSettings is migrated into the store and the plaintext key is deleted.
- Backend selection cleanup: `--backend={mock,http,rust}` (old `--mock` still works). Default remains `http`. Requesting `--backend=rust` when the Rust scaffold was not compiled in exits 2 with a clean message.
- Optional Rust SDK backend scaffold under `-DENABLE_RUST_SDK_BACKEND=ON`. Adds a static library from `rust/`, links it into the C++ binary, and reports backend name/version/status through a small C ABI. Login, sync, and E2EE are not wired yet — the client refuses those operations honestly.
- `CryptoManager` becomes a capability surface driven by the active backend. `supportsE2ee` is `false` for mock and http, and remains `false` for the Rust scaffold until the SDK is actually wired (`RUST_SDK_E2EE_WIRED`).

Still carried over from v0.3:

- Backfill pagination, local echo resolution, replies, edits, redactions, reactions, typing, read receipts, media send/receive, per-room member cache, local SQLite cache under `${XDG_DATA_HOME}/matrix-client/<safeUserId>/cache.sqlite`.
- Password login (`m.login.password`), session restore via `/account/whoami`, long-poll `/sync`, `POST /logout`.
- `--mock` / `--backend=mock` for offline UI work.

Still intentionally missing:

- Real E2EE (v0.4.x follow-up). The Rust backend scaffold compiles and links but does not perform any Matrix cryptography yet. `--backend=rust` will not sign you in.
- SSO / OIDC, spaces, threads, multi-account, sliding sync (v0.5).
- Authenticated media endpoints (`/_matrix/client/v1/media/*`); v0.3/v0.4 use legacy `/_matrix/media/v3/*`.
- Read-receipt display of other users, avatars rendered as images, own-profile lookup, room member list UI.

See [`docs/roadmap.md`](docs/roadmap.md) for the milestone plan, [`docs/architecture.md`](docs/architecture.md) for the layering, and [`docs/threat-model.md`](docs/threat-model.md) for what is stored where.

## Build (Linux)

Requires Qt 6.5+ (Core, Gui, Qml, Quick, QuickControls2, Network, Sql, Widgets), CMake 3.21+, a C++20 compiler, Ninja, and optionally libsecret-1 + cargo/rustc.

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/matrix-client
```

If libsecret-1 is detected via pkg-config, secure storage is enabled automatically. Otherwise the app falls back to insecure QSettings storage with a visible warning.

### With the Rust SDK backend scaffold

```bash
cmake -S . -B build-rust -G Ninja -DENABLE_RUST_SDK_BACKEND=ON
cmake --build build-rust
./build-rust/matrix-client --backend=rust
```

This requires `cargo` in `PATH`. The Rust static library is built via `cargo build` invoked from CMake and linked into the C++ binary. Turning the flag back off is a clean `cmake -B build -G Ninja` — no Rust dependencies remain.

### NixOS

The dev shell bundles Qt 6, CMake, Ninja, gcc, libsecret, glib,
xkeyboard_config, cargo, and rustc so both build modes work out of the
box:

```bash
nix develop            # or: nix-shell
cmake -S . -B build -G Ninja
cmake --build build
./build/matrix-client
```

**Important on KDE / GNOME sessions**: always launch the built binary
from *inside* the dev shell (either the interactive `nix develop`
subshell or `nix develop -c ./build/matrix-client`). The shellHook
purges the Qt env variables (`QT_PLUGIN_PATH`,
`QT_QPA_PLATFORM_PLUGIN_PATH`, `QML_IMPORT_PATH`, …) that a running
KDE Plasma or GNOME session exports at a different qtbase version, and
sets them consistently against the flake's Qt. Launching outside the
dev shell can pick up the session's `QT_PLUGIN_PATH` pointing at a
different qtbase and abort at plugin load with the message
`"Could not load the Qt platform plugin"`. If that happens, re-enter
`nix develop` first. This is documented in
[`docs/build-and-test.md`](docs/build-and-test.md#troubleshooting).

### Windows / macOS

Not yet targeted. The CMake project is portable; SecretStore has no Windows/macOS backends yet (they fall back to insecure storage with a warning until v0.5+). Packaging follows in v1.0.

## Try it

### Real homeserver (default)

```bash
./build/matrix-client                    # same as --backend=http
./build/matrix-client --backend=http
```

Enter your homeserver URL (e.g. `https://matrix.org`), your username (localpart or full MXID), and your password. On successful login the app persists the session (access token via SecretStore, everything else in QSettings) and starts syncing. Restart the app — you'll be signed back in automatically. Sign out from the toolbar to clear both the SecretStore entry and the QSettings session metadata.

### Mock backend (for UI work)

```bash
./build/matrix-client --mock
./build/matrix-client --backend=mock     # equivalent
```

Any credentials succeed. Hard-coded rooms and messages appear. Nothing hits the network.

### Rust backend scaffold

```bash
./build-rust/matrix-client --backend=rust
```

Available only when built with `-DENABLE_RUST_SDK_BACKEND=ON`. The app launches, the Settings screen reports the Rust backend, but login is refused with a clear message pointing you back to `--backend=http`.

## Layering

```
Qt/QML UI      →  qml/*.qml
App layer      →  src/app, src/auth
UI models      →  src/models
Backend iface  →  src/matrix/MatrixClient.h
Backends       →  src/matrix/MockMatrixClient.{h,cpp}    ← --backend=mock
                  src/matrix/CppHttpMatrixClient.{h,cpp} ← --backend=http (default)
                  src/matrix/RustSdkMatrixClient.{h,cpp} ← --backend=rust
                                                          (only when
                                                          ENABLE_RUST_SDK_BACKEND)
Rust crate     →  rust/                                  ← static lib linked in
Storage        →  QSettings (prefs + non-secret session)
                  SecretStore: LibSecretStore or InsecureFallbackSecretStore
                  SQLite (rooms/timeline/members cache)
Platform       →  src/notifications, src/media (stubs / partial)
Crypto         →  src/crypto/CryptoManager (capability surface only)
```

The `MatrixClient` interface is the swap seam. UI, models, and the app layer never depend on a concrete backend.

## What v0.4 does *not* do

- No real E2EE. The Rust backend scaffold compiles and links but does not talk to Matrix. Encrypted rooms still show `[encrypted message - E2EE not implemented yet]`; encrypted media still shows a placeholder; sends into encrypted rooms are still blocked.
- No hardware-backed secret storage. `LibSecretStore` uses the Freedesktop Secret Service, whose backend is whatever the user's session provides.
- No Windows / macOS secret backend. Windows and macOS fall back to the insecure store with a warning.
- No spaces, threads, multi-account, sliding sync, SSO/OIDC. Those are v0.5.
- No display of other users' read receipts. Avatars are cached but not rendered as images yet.

## Contributing

Prefer many small files over one large one. Keep protocol logic behind the `MatrixClient` interface. Do not import protocol types into QML files.
