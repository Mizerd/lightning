# Lightning

A native desktop Matrix client written in **C++20 / Qt 6 / QML**.

**Not** Electron. **Not** Tauri. **Not** a webview wrapping a browser.
The chat UI is real Qt Quick, drawn natively.

The executable file is named `matrix-client` for build-system stability
across the rename. The product name is **Lightning**.

---

## Status

**v0.5.0-prep+4** — Matrix Rust SDK backend foundation, hardened, plus
a headless verification harness. No E2EE claim yet.

The default HTTP backend remains the working production path. The
optional Rust backend now links `matrix-sdk` v0.18 and wires a real
Rust-owned SDK client behind the existing C++ `MatrixClient` seam:
login, session restore, joined-room sync, room-list events, basic text
timeline events, and plain text sends for unencrypted rooms. QML still
talks only to C++ models and signals.

**New in v0.5.0-prep+4 (verification harness):**

- `--rust-sdk-smoke-test` in the Rust-enabled build. Reads
  credentials from `LIGHTNING_TEST_HOMESERVER` /
  `LIGHTNING_TEST_USER` / `LIGHTNING_TEST_PASSWORD` (never CLI args),
  runs headless via `QCoreApplication`, has a 60 s budget, and prints
  only counts / statuses — never bodies, tokens, passwords, or
  crypto material. Optional `LIGHTNING_TEST_SEND=1` (with optional
  `LIGHTNING_TEST_ROOM_ID`) sends one probe message to a
  non-encrypted room. Constructed with a `nullptr` `SettingsManager`
  so it cannot overwrite the interactive user's cached session.
  Full usage in [`docs/build-and-test.md`](docs/build-and-test.md#headless-rust-sdk-smoke-test-v050-prep4).

**Hardened in v0.5.0-prep+3 (foundation bug-fix pass):**

- Bounded Rust → C++ event queue at 4096 entries with drop-oldest +
  single `queue_overflow` marker on overflow, so a stalled UI thread
  can no longer OOM the process.
- Atomic reserve of the sync-running flag inside `mx_rust_start_sync`;
  closes a race where two rapid callers could each spawn a sync loop.
- Undecryptable timeline rows now propagate through the FFI as
  `{ undecryptable: true, body: "", msgtype: "encrypted" }` and are
  rendered by C++ as the localised `[unable to decrypt yet]`
  placeholder — never as raw ciphertext, which is deliberately not
  forwarded through the FFI.
- Full FFI event schema (including the new `queue_overflow` event
  and `undecryptable` timeline flag) documented in
  [`docs/backend-contract.md`](docs/backend-contract.md).

**Previously in v0.5.0-prep (foundation land):**

- `rust/` builds offline against the committed `Cargo.lock` and
  `matrix-sdk` dependency.
- `RustSdkMatrixClient` owns the C++ wrapper state and polls a Rust
  event queue via `QTimer`.
- Rust owns the SDK client, async work, and SDK SQLite store under
  `${XDG_DATA_HOME}/matrix-client/<safeUserId>/matrix-rust-sdk-store/`.
- `--reset-crypto-store` deletes only those Rust SDK store
  directories. It never touches `cache.sqlite` or SecretStore tokens.
- `CryptoManager::supportsE2ee()` still returns `false`. Encrypted
  room sends remain blocked until encrypted read and send are verified
  end to end.

See [`docs/current-state.md`](docs/current-state.md) for the
ground-truth snapshot.

**Previously in v0.4.8** — Cache constraint repair, timeline-delegate anchor
warning fixed, `Connected` status text, docs sweep.

**New in v0.4.8:**

- `CacheStore` no longer emits `NOT NULL constraint failed:
  events.thread_root_id` / `rooms.child_room_ids`. Root cause: Qt's
  QSQLITE driver binds a default-constructed `QString` as SQL NULL,
  which violated the v0.4.5 NOT NULL columns. Fix: coerce to a
  non-null empty QString via a `textNonNull()` helper at bind time,
  plus an idempotent `UPDATE … SET col = '' WHERE col IS NULL`
  repair pass on schema-ensure to clean up any rows historical
  code paths may have left NULL. No cache wipe.
- `qml/MessageDelegate.qml` no longer prints
  `QML Column: Cannot specify … anchors for items inside Column`
  on every message. The inner `MouseArea { anchors.fill: parent }`
  used to catch hover-off transitions on the actions column is
  replaced with a `HoverHandler`, which works inside Column without
  needing explicit geometry.
- Connection status flips to `Connected` (not `Syncing`) once the
  initial sync response has been parsed and the long-poll is the
  normal steady-state. `Loading rooms…` still shows during the
  initial catch-up.

**New in v0.4.7:**

- `/sync` initial fetch uses `timeout=0&full_state=true` so the server
  returns current state immediately instead of long-polling. Follow-up
  syncs long-poll with `timeout=30000` as before. The transfer timeout
  is 30s on the initial call and 60s on subsequent long-polls (safely
  above the 30s server-side wait).
- Fresh login clears any persisted `syncToken`, and session restore
  discards a stored token if the SQLite cache has no visible non-Space
  rooms. This recovers from stale Space-only cache state where the UI
  would otherwise have no room rows to render and incremental sync
  would not resend the full snapshot.
- `MatrixClient::initialSyncDone()` — new capability the interface
  advertises so the UI can distinguish *"still loading the initial
  sync"* from *"sync loop is live, there are just no rooms"*.
- Room list header stops showing a bogus "0" before the first sync
  response lands; the empty label under the list is now state-aware
  (loading / no joined rooms / no rooms in selected Space / not signed
  in).
- Connection status label reflects the same distinction:
  `Connecting…` → `Loading rooms…` → `Connected` (v0.4.8; was
  `Syncing`) → `Idle`.
- Non-secret sync diagnostics: `matrix.http:` log lines announce sync
  requests (initial vs. continuation), HTTP status, response body size,
  and joined / invited / left room counts. Access tokens are never
  logged.

Everything below carried over from earlier passes. See
[`docs/current-state.md`](docs/current-state.md) for the ground-truth
snapshot and [`docs/next-prompts.md`](docs/next-prompts.md) for the
next safe follow-up.

---

## Quick start on NixOS

```bash
# 1. Enter the dev shell. The shellHook purges any inherited KDE /
#    GNOME Qt env vars and sets flake-consistent paths.
nix develop

# 2. Configure + build (default: mock + http backends).
cmake -S . -B build -G Ninja
cmake --build build

# 3. Run. Default backend is http; use --mock for offline UI work.
./build/matrix-client
```

The dev shell brings in Qt 6 (qtbase, qtdeclarative, qtsvg, qtwayland,
qttools), CMake, Ninja, gcc, pkg-config, libsecret, glib,
xkeyboard_config, rustc, cargo.

Optional Rust SDK backend foundation:

```bash
cmake -S . -B build-rust -G Ninja -DENABLE_RUST_SDK_BACKEND=ON
cmake --build build-rust
./build-rust/matrix-client --backend=rust
```

**Always launch from inside `nix develop`.** Running the binary in a
bare terminal on a KDE Plasma / GNOME Wayland session picks up a
different `QT_PLUGIN_PATH` from the outer session and aborts on plugin
init. See [`docs/build-and-test.md`](docs/build-and-test.md#troubleshooting)
for the full story.

---

## Known good build commands

```bash
# Full clean build (default: mock + http).
rm -rf build
nix develop -c cmake -S . -B build -G Ninja
nix develop -c cmake --build build

# Full clean build with Rust SDK backend linked in.
rm -rf build-rust
nix develop -c cmake -S . -B build-rust -G Ninja -DENABLE_RUST_SDK_BACKEND=ON
nix develop -c cmake --build build-rust

# Rust-only offline dependency check.
cd rust
nix develop -c cargo build --release --offline

# Smoke tests (offscreen QPA — exit 124 = healthy).
nix develop -c bash -lc 'QT_QPA_PLATFORM=offscreen timeout 3 ./build/matrix-client --mock'
nix develop -c bash -lc 'QT_QPA_PLATFORM=offscreen timeout 3 ./build/matrix-client --backend=http'
nix develop -c bash -lc 'QT_QPA_PLATFORM=offscreen timeout 3 ./build-rust/matrix-client --backend=rust'

# CLI rejection sanity (exit 2 with a clean message).
nix develop -c bash -lc './build/matrix-client --backend=bogus || true'
nix develop -c bash -lc './build/matrix-client --http || true'
```

---

## Backend modes

Lightning ships three backends behind a single `MatrixClient` interface
(`src/matrix/MatrixClient.h`). Selection happens once at process
start; no live switching.

| `--backend=` | What it does |
|---|---|
| `mock` | In-memory hardcoded rooms, Space + threaded conversation demo, no network. `--mock` is an alias. Useful for offline UI work. |
| `http` | Talks Matrix Client-Server API directly with `QNetworkAccessManager`. Password login, `/sync` long-poll, replies / edits / redactions / reactions / media / typing / receipts, Spaces + real `m.thread`. **Default.** No E2EE. |
| `rust` | Optional Matrix Rust SDK backend foundation. Rust owns the SDK client/runtime/store; C++ polls FFI events and exposes them through existing models. Login/restore/sync/plain text send are wired; E2EE still reports unsupported and encrypted sends are blocked. Only built when `-DENABLE_RUST_SDK_BACKEND=ON`. |

---

## Current feature status

Full honest matrix in [`docs/matrix-feature-status.md`](docs/matrix-feature-status.md).
Highlights:

- ✅ Password login, session restore via `/whoami`, long-poll `/sync`.
- ✅ Text send/receive, replies, edits, redactions, reactions, typing,
  read receipts, backfill pagination, media send/receive
  (legacy `/media/v3/*`), per-room member cache, local SQLite cache.
- ✅ Matrix Spaces: parse from `/sync` (`m.room.create type:m.space` +
  `m.space.child`), chip strip filter in the room list, cache
  persistence.
- ✅ Real Matrix threads: `m.thread` relation on outgoing sends + parsed
  on incoming, "in thread" chip on root events, thread-reply composer
  mode, cache persistence for `threadRootId`.
- ✅ Secure token storage via libsecret (Freedesktop Secret Service);
  insecure QSettings fallback with a red warning banner.
- ✅ Nix dev shell that resolves the KDE/Gnome Qt plugin conflict.

---

## What is intentionally missing

- Verified E2EE — encrypted-room read/send, device verification,
  cross-signing, key backup, secret storage. Lightning will not
  hand-roll cryptography in C++.
- Full Rust SDK parity — pagination, replies, edits, reactions, media,
  Spaces hierarchy, typing, receipts, and sliding sync are still missing
  from the Rust backend.
- SSO / OIDC / Matrix Authentication Service login. Password login
  works; SSO/OIDC placeholders are wired but do nothing.
- Multi-account sync — `AccountManager` tracks a single active user.
- Sliding sync — v0.5+ via Rust SDK.
- Authenticated media (`/_matrix/client/v1/media/*`).
- Own-profile lookup, other-user read-receipt display, avatar image
  rendering (avatars are cached but shown as an initial-letter chip),
  member list UI, dedicated thread side-panel.
- Windows / macOS SecretStore backends.

---

## Do not break these invariants

1. **C++ owns UI + app logic + models + HTTP backend + cache + docs
   + tests.** Every screen, model, and backend that has a C++
   implementation stays C++.
2. **Rust is only for the Matrix SDK / E2EE / crypto path**, and only
   behind the `MatrixClient` interface. QML never talks to Rust
   directly.
3. **Access tokens live in `SecretStore`, never in the SQLite cache**.
   Non-secret session metadata (homeserver, userId, deviceId,
   syncToken) stays in `QSettings`.
4. **The SQLite cache is not secret storage.** Room summaries, last
   ~200 events per room, and per-room members. Never a token.
5. **The Mock / HTTP / Rust backend seams stay independent.** No
   backend imports another; UI/models only depend on
   `MatrixClient.h`.
6. **`CryptoManager::supportsE2ee` is the single source of truth for
   the E2EE badge.** Flip it to `true` only when real crypto works.
7. **CLI validation runs before `QGuiApplication`.** Bad `--backend=…`
   values and `--http` / `--rust` shortcuts exit 2 with a clean
   message; a missing display exits 3 with a hint about
   `QT_QPA_PLATFORM=offscreen`. Never let a typo abort inside Qt's
   platform plugin loader.

---

## Runtime troubleshooting

See [`docs/build-and-test.md`](docs/build-and-test.md#troubleshooting)
for the full list. Common ones:

- **Qt platform plugin fails to load on NixOS/KDE/GNOME** → launch
  from inside `nix develop`. The shellHook purges the outer session's
  `QT_PLUGIN_PATH` and sets flake-consistent Qt paths.
- **HTTP login succeeds but stays on login screen** — fixed in v0.4.5.
  Look for `matrix.app: screen change 0 -> 1` in the terminal to
  confirm the transition fired.
- **"Syncing" forever with 0 rooms** — fixed in v0.4.7. Look for
  `matrix.http: sync request:` and `matrix.http: initial sync
  complete; rooms in memory = N` in the terminal.
- **`matrix.cache: NOT NULL constraint failed …`** — fixed in v0.4.8.
  Existing cache DBs are repaired in place on next open. If the
  errors still appear, delete
  `${XDG_DATA_HOME}/matrix-client/<safeUserId>/cache.sqlite` and let
  the next `/sync` refill it.
- **`QML Column: Cannot specify … anchors for items inside Column`**
  spam on every message — fixed in v0.4.8. The `MessageDelegate`
  hover-catcher is now a `HoverHandler`, which is Column-safe.
- **`--backend=rust` doesn't work in the default build** — Rust
  backend is opt-in. Configure with
  `-DENABLE_RUST_SDK_BACKEND=ON` and rebuild into `build-rust/`.

---

## Next safe prompts

Full list in [`docs/next-prompts.md`](docs/next-prompts.md). Recommended
next single-pass step: **verify and harden Rust backend login/sync
against the real test homeserver**, then move to encrypted read/send.

After that, in order:

1. Rust backend manual login/restore/sync/send verification and fixes.
2. Initial encrypted read/send through Matrix Rust SDK; flip E2EE only
   after real encrypted round trips work.
3. Multi-account foundation.
4. SSO / OIDC login (system browser + local loopback callback).
5. Authenticated media (`/_matrix/client/v1/media/*`).

---

## Layering (concise)

```
Qt/QML UI              qml/*.qml
App layer              src/app,  src/auth
UI models              src/models   (+ src/spaces, src/threads)
Backend interface      src/matrix/MatrixClient.h
Backends               MockMatrixClient      (--backend=mock)
                       CppHttpMatrixClient   (--backend=http, default)
                       RustSdkMatrixClient   (--backend=rust,
                                              gated on ENABLE_RUST_SDK_BACKEND)
Rust crate             rust/                 (linked as static lib)
Storage                QSettings (prefs + non-secret session)
                       SecretStore (libsecret / insecure fallback)
                       CacheStore (SQLite; rooms + last N events + members)
Platform               src/notifications, src/media  (stubs / partial)
Crypto                 src/crypto/CryptoManager  (capability surface only)
```

Full write-up in [`docs/architecture.md`](docs/architecture.md).

---

## Contributing

Small files over one large one. Protocol logic stays behind
`MatrixClient`. Protocol types do not appear in QML files. Read
[`docs/current-state.md`](docs/current-state.md) before making changes
larger than a bug fix — it's kept honest with what is and is not
implemented.
