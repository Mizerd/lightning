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

## Decided 2026-10-18, planned after 0.10.2

Decided for Rokas under his standing rule: copy what Element/Element X,
Discord, Telegram, Signal and Slack do, and add no niche settings. The
reasoning, decision by decision, is in the vault note `Tasks/2026-10-18-dec.md`,
and each decision carries a "DECIDED 2026-10-18" line in `docs/open-items.md`.
None of the work below goes into 0.10.2. Order: **1** keyring finish, **2**
Flatpak audio backend, **3** event-focused timeline, **4** live accounts
Phase 1, then **5** the unified inbox, which needs 4 and its Phase 2. Items
1-3 are independent of each other and can run in parallel with separate file
ownership. Item 4 owns `AppController.*` and `RustSdkMatrixClient.*` while it
lands, so nothing else edits those files in that window.

### 1. Keyring phase 2, the remainder (open-items "Keyring: install scoping")

Already landed in `83aa66f0`: the whoami check for password adoptions and
recording the backend. Four slices, one commit each, each with a §18 review
(credentials and sign-in):

1. **"Try again" from the signed-out path.** `onLoggedOut`
   (`src/app/AppController.cpp`, the "restart Lightning" message) calls
   `refreshKeyringUnavailable()` and offers the existing `retryKeyring()`.
   Test (`StartupSessionTest` / `LoginRepairQmlTest`): sign out with the other
   accounts' secrets unreadable, then assert that the notice shows and that
   `retryKeyring()` restores them without a restart.
2. **A miss under the other libsecret backend is "cannot tell".** Compare the
   recorded backend (`SettingsManager`, written by `LibSecretStore`) with the
   one this launch reads. On a mismatch, `secretMissesAreInconclusive` holds
   and no `keyring_lost_session` card is shown. Test in `KeyringScopeTest`:
   a record says `file`, this launch reads `service` and misses, so the
   account stays and no repair is offered.
3. **An OAuth account whose keyring lost its sign-in resumes on the recorded
   device.** Pass the recorded device id into the OAuth login (`rust/src/oauth.rs`,
   which passes `device_id None` today). `oauthLoginBlockReason`
   (`src/matrix/RustSessionPolicy.cpp`) already accepts `saved == fresh`.
   Needs a MAS capture first (matrix.org or the local homeserver with MAS).
   Tests: `RustSessionPolicyTest` and a Rust unit test on the request.
4. **The OAuth adoption check** after the first token refresh and before the
   first sync, with the same three outcomes as the password check. It is the
   riskiest slice and comes last.

Constraints that bind every slice: a restore never needs a live server; an
unreadable token never means "no account"; destructive cleanup keys on the
record only (CLAUDE.md §6). Live: a real Flatpak with the Secret portal coming
and going, and KWallet.

### 2. Flatpak: Qt Multimedia on its PulseAudio backend

Decided because every Chromium client and Telegram Desktop reach PipeWire
through the PulseAudio API, and Qt's PipeWire sink is what spins the GUI
thread (open-items 2026-10-07).

1. **Measure first, change nothing.** In the Flatpak with
   `QT_AUDIO_BACKEND=pulseaudio` set by hand, check three things. (a) Are the
   stored call device ids (Qt device ids) the same as on the PipeWire backend?
   If not, write a one-time mapping by description, so nobody's chosen
   microphone is reset. (b) Run the QSoundEffect voice path (ringer, chimes)
   for 10 minutes of cues. (c) Does a QAudioSink open block the GUI thread
   when pipewire-pulse is stopped?
2. **Change.** Add `--env=QT_AUDIO_BACKEND=pulseaudio` to `finish-args` in both
   Flatpak manifests (the root one for Flathub, and `packaging-ci`'s), plus
   the mapping from step 1 if it was needed. Gate: `test-flathub-manifest-pin.py`
   and the packaging config tests.
3. **Live.** Reproduce the orphaned stream (unplug the output, restart
   PipeWire) and require that the GUI thread does not spin. Then run a
   two-party call, a voice message and the ringer inside the Flatpak.
   Self-review is enough (packaging, but one env var); §18 if a device-id
   mapping is needed.

### 3. Event-focused timeline (jumps to old messages)

Today every jump goes through `PaginationController::jumpToEvent`. A loaded
target lands in place. Otherwise the read-only `ContextView` overlay opens:
`rust/src/timeline.rs` `open_context_task` already builds a matrix-sdk-ui
`TimelineFocus::Event` timeline, but `ContextController` never sends, reacts or
marks read. If `/context` fails, `walkToEvent` pages up to 12 batches and
freezes past about 600 rows. Element Web and Element X open a normal,
interactive timeline around the event, with "Jump to latest".

1. **Rust (§18: FFI and lifecycle).** Let a room's main timeline be either
   Live or Event-focused under one timeline id and generation, and add a
   `switch_to_live` call that keeps the event cache. Every callback stays
   stamped with the focus generation, so a late Live diff cannot reach the
   focused view or the other way round. Files: `rust/src/timeline.rs`,
   `rust/src/lib.rs`, `rust/include/matrix_rust.h`. Tests: Rust, focusing a
   cached event and an uncached one, then going back to Live after new
   messages, with a stale-generation diff dropped.
2. **C++.** `PaginationController::jumpToEvent` asks for the focus instead of
   opening the overlay. `TimelineModel` resets on focus change, and sending
   from a focused timeline first returns to Live (Element's behaviour).
   Receipts are not sent while focused away from the newest message. Files:
   `src/models/{PaginationController,TimelineModel,ContextController}`,
   `src/matrix/{MatrixClient,RustSdkMatrixClient,MockMatrixClient}`,
   `src/app/AppController.cpp` (the context wiring around `m_contextOpener`).
   Tests: `PaginationControllerTest`, `ContextControllerTest` (rewritten for
   focus) and a mock focus in `MockMatrixClient`.
3. **QML.** `TimelinePane.qml` gets a "Jump to latest" pill while focused,
   and `ContextView.qml` is retired or kept only as the failure fallback.
   Landing uses `beginNavigationLanding`, as find matches already do. Tests:
   the jump cases in `TimelinePaneQmlTest` (reply, search, pin, Activity,
   jump-to-date). They must fail when the focus is replaced by the old walk.
4. **Parked-at-oldest shift (open-items 2026-10-16).** Only after a live
   `LIGHTNING_SCROLL_TRACE=1` capture shows the shift (docs/timeline-scrolling.md
   forbids a fourth anchor fix without one). The decision is already taken:
   hold the reader on their row.

Review gates: §18 on slices 1 and 2. `timeline-pane-qml` is load-sensitive,
so run it alone at low parallelism before reading a failure as a regression.
Live: physical scrolling around a jumped-to event, a jump in an encrypted
room, and a thread root.

### 4. Live accounts Phase 1 (Labs), hidden behind `LIGHTNING_LABS_LIVE_ACCOUNTS`

State: Phase 0 is on `main` (`0b168b9d`). Phase 1 is integrated in
`/mnt/storage/scratch/la1-partial-integration.patch` (1.2 MB, 79 files, base
`02693a52`, fixes G1-G11 included). Its last ctest was 322/325: localization
(expected), background-host and notification-dbus, the last two probably the
D-Bus environment, which is unconfirmed. cargo was not re-run after G6.
`main` is now 21 commits past that base, and 10 of them touch Phase 1 files.
Rokas's DECISIONS block in the design note settles §14: card hidden until
Phase 2, no background presence, one notification per event. Removal also
signs out on the server.

Land it as five slices onto post-0.10.2 `main`, in this order, with the flag
off throughout and `FlagOffParityTest` green after every slice:

1. **R (Rust):** `labs.rs`, `bgsession.rs`, the store reservation in
   `storeclose.rs`. Re-run the full cargo suite, including
   `every_type_literal_is_classified`, which must classify every event
   `"type"` added on `main` since the base (for example
   `room_membership_lost`, `room_own_permissions` and any account stamp
   wp2 adds). §18.
2. **S (session machinery):** `src/accounts/*` plus the claim and proof code
   in `RustSdkMatrixClient.*`, conflict C1 in `login()` re-merged against
   `main`. Then G3 (`beginQuitRetirement`) and G5. §18 (lifecycle, store,
   E2EE).
3. **M (settings and models):** `SettingsManager`, `SettingsSchemaTable.inc`,
   `AccountActivityModel`, `AccountListModel`. Self-review plus focused tests.
4. **A (app wiring):** `AppController.*` (about +1000 lines) and the
   `NotificationManager` refactor. This slice changes the default path even
   with the flag off, so `FlagOffParityTest`, `NotificationFlood`,
   `RemovalOrder` and `QuitParallel` must all pass. §18.
5. **Q (QML):** `AccountActivityBadge`, `AccountActivityFeed`, and the Labs
   card hidden without the env var. Badge contrast across all 11 palettes is
   checked here (a `ThemeTokensTest` case asserting the count).

Before slice 1, close the open §18 rechecks G2-G6, and decide or implement
`SessionPool::abandonRelease` and the Labs switcher's lost "Needs sign-in" for
a lost token (record that one in `docs/open-items.md`). Risks named by the
design: a store opened twice (crypto store corruption), a crypto or
verification event misclassified as UI-only, a refresh token lost to a stale
keyring read, and quit and retirement ordering (NOT TESTED live). Before it is
called Phase 1 done, it needs a live run on the local homeserver with two
throwaway accounts plus `lightningtest5`/`6`: background mention, switch,
quit while syncing, and removal.

### 5. #35 unified inbox

Rokas chose inbox design B (all accounts live) over design A (a read-only
poll per account), so the inbox is design B phase 3. It needs live accounts
Phase 1 (above) and Phase 2 (instant switch through `ActiveClientProxy`).
v1 matches what Discord, Slack and Telegram show:

1. **Switcher badges and rail dot** from every live session. Encrypted rooms
   show a dot, not a count (CLAUDE.md §8 rule on invented counts). Files:
   `AccountMenu.qml`, `IdentityCard.qml`, `SpacesRail.qml`,
   `AccountActivityModel`.
2. **"From your other accounts" list**: a `UnifiedInboxModel` over the live
   sessions' Activity rows, and `UnifiedInboxPanel.qml`. A click switches
   account and opens the room (cross-account pending open). Plain text only.
   Tests: generation guards across an account switch, and a stale row from a
   removed account.
3. Notifications for other accounts follow the existing per-account
   notification settings. No new setting.

Review: §18 for slice 2 (cross-account routing). Live: two accounts on
different homeservers, a mention in the background one, and opening it from
the list.

## Planned (2026-10-17) — BOTH BUILT for 0.10.2

- **German translation (`de`).** Requested by Rokas, 2026-10-17. BUILT in
  `759f211b`: the full catalogue, in the language picker and the
  `localization` suite. A native speaker's review of the longer explanatory
  strings is still welcome. Not live-checked in the app.

- **"Playing X" game activity (Discord-style), opt-in.** Idea from Rokas,
  2026-10-17. BUILT in `d46520b3` as a Labs feature, off by default: while a
  game runs and the user has no status of their own, its name becomes the
  presence `status_msg` (Discord's order since 2026-10-20: a status the user
  set always comes first), worded in the app's language. Detection reads the process table and knows
  Steam libraries, a built-in list and user-added entries, and on Windows
  prefers the foreground full-screen game; a per-game hide list; only the
  name is ever sent. Flatpak and Snap cannot see host processes, and macOS
  has no detection in this build. NOT live-tested.

## Planned (2026-10-20) — BUILT, not committed yet

- **"Listening to Spotify" music activity (Discord-style), opt-in.** BUILT as
  a Labs feature next to the game activity, off by default: while Spotify
  plays on this computer and the user has neither a status of their own nor
  a detected game (Discord's order), the line reads "Listening to <song> by
  <artist>" in the app's language. Only the title and the artist are sent.
  Linux reads Spotify's MPRIS name (the Flatpak gains
  `--talk-name=org.mpris.MediaPlayer2.spotify`); Windows reads the Spotify
  window's caption; macOS and the Snap have no detection. A track change rides
  the 25 s keep-alive (no extra PUT); sign-out, account switch and quit
  withdraw the game and music lines in one PUT (sign-out waits up to 11 s for
  the answer, quit 3 s, a switch not at all). Contract in
  `docs/feature-contracts.md`, "Activity status: game and music (Labs)".
  NOT live-tested.

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
