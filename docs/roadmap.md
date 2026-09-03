# Roadmap

## Milestones

| Version | Theme | Backend |
|---|---|---|
| v0.1 | Native app shell | `MockMatrixClient` only |
| v0.2 | Basic real client | `CppHttpMatrixClient` (default) + `MockMatrixClient` (`--mock`) |
| v0.3 | Usable chat UX | `CppHttpMatrixClient` + local SQLite cache + `MockMatrixClient` (`--mock`) |
| v0.4 | Secure storage + Rust backend scaffold | HTTP default; `--mock`; optional `--backend=rust` behind `-DENABLE_RUST_SDK_BACKEND=ON` — scaffold only, refuses login/sends |
| v0.4.5-8 | HTTP polish (sync bring-up, cache repair, delegate anchor fix, Connected label) | HTTP feature-complete short of E2EE |
| v0.5.0 | E2EE via Matrix Rust SDK | Rust backend + encrypted send/receive + SAS verification + recovery key |
| v0.5.2 | Design-token foundation | `AppTheme` semantic tokens, spacing, radii, typography, light/dark palettes |
| v0.5.3 | Split Spaces + Rooms sidebar | `SpacesPanel` + `RoomsPanel` with independent scroll/search; bottom-left gear Settings |
| **v0.5.4 (current)** | 3-column navigation + room grouping | `SpacesRail` (56 px) + `RoomsPanel` with DM/Room sections + user footer; softer light palette |
| v0.5.x | UI polish + advanced features | Timeline/composer redesign; login redesign; multi-account; SSO/OIDC; authenticated media |
| v1.0 | Polished release | Rust SDK; hardware-backed secure storage, packaging, i18n complete |

## Next: identified gaps (2026-09-03) — ALL FOUR BUILT the same day

All four of the gaps below were implemented on 2026-09-03 at Rokas's
direction. They are kept here with their original reasoning because the
reasoning is what a later reader needs, and the contracts now live in
`docs/feature-contracts.md` under "Navigating a room's history" and "Export a
room". None of them is live-validated.



Four features a mainstream Matrix client has and Lightning does not, found by
grepping the tree rather than by reading this file. Ordered by how often a
user would feel the absence.

- **Jump to first unread.** The pieces exist and are not connected: the
  timeline already carries a `ReadMarker` row (`TimelineEvent::ReadMarker`,
  handled in `TimelineModel`), the room list already shows unread badges, and
  `RoomListModel::markRoomRead` already exists. What is missing is any
  affordance that scrolls to the marker — a grep for `jump.*unread` across
  `qml/` returns nothing. In a busy room this is daily friction, and it is the
  cheapest of the four.
- **Mark all rooms read.** `markRoomRead(roomId)` exists per room; there is no
  sweep. It pairs with the Activity Center's bell, which now clears per room
  from a read receipt, so "mark everything" would clear both in one action.
- **Jump to date (MSC3030 `timestamp_to_event`).** Absent entirely. Useful
  once a room has real history; Element has it.
- **Export a room.** Absent entirely. Element has it. Least felt of the four,
  and the one with the most design surface (format, range, media, and whether
  an encrypted room's plaintext may be written to disk at all — §6 says
  encrypted-room plaintext stays memory-only, so an export is an explicit,
  user-chosen exception that has to be argued rather than assumed).

Deliberately NOT proposed, with reasons, so they are not re-derived:

- **Widgets.** `docs/widgets-feasibility.md` names the blocker: a Qt WebEngine
  dependency, which is a large commitment for a surface few desktop users
  open.
- **Location sharing (`m.location`).** Genuinely absent, and the least used
  feature in most clients. Cheap to add if ever asked for.
- **Custom emoji and sticker packs.** NOT a gap — MSC2545 `im.ponies.room_emotes`
  is implemented, including upload to a user pack (`rust/src/stickers.rs`,
  `src/stickers/StickerPackManager.cpp`, `qml/StickerPicker.qml`). An older
  note in `docs/matrix-feature-status.md` still says otherwise; that table is
  stale in several rows and should be read against the source.

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
- Reactions via `m.reaction` / `m.annotation` with toggle-off through redacting the local `event_id`; since v0.5.10 the UI uses the complete local Unicode 17.0 picker shared with the composer.
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
  message and exit 2. `--backend=rust` refuses cleanly when the Rust backend
  is not compiled in.
- Initial Rust SDK backend scaffold behind `option(ENABLE_RUST_SDK_BACKEND OFF)`.
  - `rust/` crate producing `libmatrix_client_rust.a`; hand-authored C
    ABI at `rust/include/matrix_rust.h`.
  - `RustSdkMatrixClient` (C++) implements `MatrixClient`, links against
    the crate, and reports name/status/version through FFI. All send /
    login / sync operations refuse honestly — no fake E2EE.
- `CryptoManager` becomes a capability surface driven by the active
  backend. `supportsE2ee` is `false` for mock and http, and remains
  `false` for the rust backend until `RUST_SDK_E2EE_WIRED` is defined.
- Settings screen surfaces:
  - active secret backend name,
  - a red warning when the insecure fallback is active,
  - crypto backend description,
  - E2EE status string.

## Next milestones after v0.4

### v0.5.0-prep — Rust SDK foundation

- `matrix-sdk` v0.18 is in `rust/Cargo.toml`; `Cargo.lock` is
  committed and offline Rust builds work.
- FFI covers create/destroy/login/restore/logout, start/stop sync,
  event polling, E2EE capability, and plain text send.
- `RustSdkMatrixClient` polls Rust events and feeds the existing C++
  models/signals.
- Rust owns the SDK client/runtime/store at
  `${XDG_DATA_HOME}/matrix-client/<safeUserId>/matrix-rust-sdk-store/`.
- Login, restore, joined-room sync, room-list events, basic text
  timeline events, and unencrypted plain text sends are wired.
- `CryptoManager::supportsE2ee` remains false; interactive encrypted
  sends are blocked while smoke-only encrypted send/receive probes are
  used for verification.

### v0.5.0 — Verify and enable E2EE

- Manually verify Rust login/restore/sync/send against the real test
  homeserver and fix any SDK/runtime issues.
- Add encrypted read and encrypted send through the Matrix Rust SDK.
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
