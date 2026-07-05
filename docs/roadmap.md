# Roadmap

## Milestones

| Version | Theme | Backend |
|---|---|---|
| v0.1 | Native app shell | `MockMatrixClient` only |
| v0.2 | Basic real client | `CppHttpMatrixClient` (default) + `MockMatrixClient` (`--mock`) |
| v0.3 | Usable chat UX | `CppHttpMatrixClient` + local SQLite cache + `MockMatrixClient` (`--mock`) |
| v0.4 | Secure storage + Rust backend scaffold | HTTP default; `--mock`; optional `--backend=rust` behind `-DENABLE_RUST_SDK_BACKEND=ON` — scaffold only, refuses login/sends |
| v0.4.5-8 | HTTP polish (sync bring-up, cache repair, delegate anchor fix, Connected label) | HTTP feature-complete short of E2EE |
| **v0.5.0-prep (current)** | C++ groundwork for matrix-sdk (version bump, `--reset-crypto-store` CLI, doc recipe) | HTTP default; Rust scaffold still honest |
| v0.5.0 | E2EE via Matrix Rust SDK | Rust backend wires SDK login/sync/crypto; matrix-sdk crate linked |
| v0.5.x | Advanced Matrix UX | Rust SDK; sliding sync, multi-account, SSO/OIDC, authenticated media, key backup |
| v1.0 | Polished release | Rust SDK; hardware-backed secure storage, packaging, i18n complete |

## Feature classification

### Easy in C++

- Native app shell (Qt/QML window, navigation, focus)
- Room list model
- Timeline rendering
- Settings screen and persistence
- Themes (light/dark/system)
- Translations wiring
- Mock backend
- Message composer
- Basic notifications (once tray is wired)

### Possible in C++ but time-consuming

- Password login and access token flow
- `/sync` long-poll loop and delta application
- Send/receive text messages
- Pagination via `/messages`
- Media upload/download (multipart, `mxc://` resolution)
- Replies, edits, redactions
- Reactions
- Read receipts
- Typing indicators
- Mentions
- Multi-account session isolation
- Basic space navigation UI (once room hierarchy is exposed by backend)
- Basic thread UI

### Better delegated to Matrix Rust SDK

- End-to-end encryption (Olm/Megolm session management)
- Device verification, cross-signing, secret storage
- Sliding sync
- Long-term Matrix protocol compatibility
- Complex `/sync` state resolution
- "Invisible cryptography" UX backend logic

### Not recommended to implement manually

- Custom cryptography or new libolm-based bindings
- Full Matrix E2EE from scratch
- A full calls / VoIP stack at v0.1
- A complete Element replacement in a single milestone

## Completed in v0.2

- `CppHttpMatrixClient` implementing `MatrixClient` (default). `MockMatrixClient` still available via `--mock`.
- Password login (`m.login.password`), returning access_token / user_id / device_id.
- Session persistence in `SettingsManager` (plaintext QSettings, warning visible in Settings).
- Session restore on startup via `GET /account/whoami`.
- Long-poll `/sync` loop with `since` cursor.
- Room list built from `m.room.name` / `m.room.topic` / `m.room.encryption` / `m.room.avatar` / notification counts.
- Timeline for `m.room.message` (`m.text`, `m.notice`, `m.emote`).
- Encrypted-room read-only placeholders; sends into encrypted rooms blocked with a clear error.
- Send text messages with local echo and `unsigned.transaction_id`-based dedup on `/sync`.
- `POST /logout` and local session clear.

## Completed in v0.3

- Backfill pagination (`GET /rooms/{id}/messages?dir=b`) with per-room `prev_batch` tokens and dedup against the in-memory timeline; UI trigger on scroll to top.
- Local echo resolution: PUT-response `event_id` replaces `local:<txn>` and is propagated to the model via `eventReplaced`; `unsigned.transaction_id` dedup on `/sync` still holds.
- Room-scoped member cache (display name + avatar mxc) from `m.room.member` state; sender names replace bare MXIDs in the timeline.
- Media receive for `m.image` / `m.file`; `mxc://` → HTTP via legacy `/_matrix/media/v3/{download,thumbnail}`. Encrypted-media envelopes render placeholder text.
- Media send: `POST /_matrix/media/v3/upload` followed by `PUT .../send/m.room.message/{txnId}` with `msgtype: m.image` or `m.file`, `info { mimetype, size, w, h }`. Blocked on encrypted rooms with a clear error.
- Replies via `m.in_reply_to`; the composer surfaces a "Replying to …" banner; the target's preview is captured on both send and receive.
- Edits via `m.replace`; the wrapper event is suppressed and the target's body updates in place with an `edited` marker. Limitation: original body is not retained (edit history UI is v0.5+).
- Redactions via `PUT /rooms/{id}/redact/{eventId}/{txnId}`; local timeline flips to `[message deleted]` on `/sync`.
- Reactions via `m.reaction` / `m.annotation` with toggle-off through redacting the local `event_id`; UI is a 5-emoji palette.
- Typing indicator: composer sends `PUT /typing/{userId}` with 15s keep-alive and false on clear / room change / send; timeline header renders "*Alice* is typing…".
- Read receipts: latest visible event is acked via `POST /receipt/m.read/{eventId}`, debounced per room.
- Local SQLite cache (`${XDG_DATA_HOME}/matrix-client/<safeUserId>/cache.sqlite`) with rooms, last 200 events per room, and members. Loaded on session restore before `/whoami` completes. Access tokens are **not** stored here.
- Mock backend updated: pre-seeded reply, edit, redaction, image, file, reactions, typing user, and a two-page synthetic pagination.

## Completed in v0.4

- Secure token storage via `SecretStore` abstraction.
  - `LibSecretStore` (Freedesktop Secret Service via libsecret) — active
    on Linux hosts where the session bus + a Secret Service provider are
    reachable.
  - `InsecureFallbackSecretStore` — plaintext QSettings under a dedicated
    `secrets/*` group, loudly reports insecure, Settings screen shows a
    red warning when active.
  - `SettingsManager::setSecretStore()` migrates any legacy
    `session/accessToken` plaintext QSettings key on first read and
    deletes it after a successful write.
  - `clearSession()` clears QSettings session keys + SecretStore entry
    for the user (in addition to the existing SQLite cache wipe).
- Backend selection CLI cleanup: `--backend={mock,http,rust}`, with old
  `--mock` kept as an alias. Unknown values reject with a clean stderr
  message and exit 2. `--backend=rust` refuses cleanly when Rust scaffold
  is not compiled in.
- Rust SDK backend scaffold behind `option(ENABLE_RUST_SDK_BACKEND OFF)`.
  - `rust/` crate producing `libmatrix_client_rust.a`; hand-authored C
    ABI at `rust/include/matrix_rust.h`.
  - `RustSdkMatrixClient` (C++) implements `MatrixClient`, links against
    the crate, and reports name/status/version through FFI. All send /
    login / sync operations refuse honestly — no fake E2EE.
- `CryptoManager` becomes a capability surface driven by the active
  backend. `supportsE2ee` is `false` for mock and http, and remains
  `false` for the rust scaffold until `RUST_SDK_E2EE_WIRED` is defined.
- Settings screen surfaces:
  - active secret backend name,
  - a red warning when the insecure fallback is active,
  - crypto backend description,
  - E2EE status string.

## Next milestones after v0.4

### v0.4.x — Wire the Rust SDK

- Depend on `matrix-sdk` in `rust/Cargo.toml`; grow the FFI to cover
  login, sync, timeline, and send.
- Introduce `cbindgen` once the C ABI outgrows a hand-authored header.
- Route Rust panics through a `MatrixClient::errorOccurred` signal so
  they surface in the UI status bar instead of aborting.
- Once real crypto is available, define `RUST_SDK_E2EE_WIRED` in the
  build and let `CryptoManager::supportsE2ee` flip to `true` for the
  rust backend only.

### v0.5 — Advanced Matrix UX

- Spaces, threads (backend already exposes hooks).
- Multi-account: per-account `MatrixClient` instance, per-account SQLite/
  session bundle, account switcher in the toolbar.
- SSO / OIDC / Matrix Authentication Service. QtWebEngine used only for
  the login redirect flow, if needed.
- Sliding sync via Rust SDK.
- Lithuanian translation completed.

### v1.0 — Polish

- Offline behavior, retry policies.
- Tray + native notifications.
- Cross-platform packaging (Linux → Windows → macOS).
- Accessibility pass.
- Full app branding, stable release.
