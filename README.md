# Lightning

A native desktop Matrix client written in **C++20 / Qt 6 / QML**.

**Not** Electron. **Not** Tauri. **Not** a webview wrapping a browser.
The chat UI is real Qt Quick, drawn natively.

The executable file is named `matrix-client` for build-system stability
across the rename. The product name is **Lightning**.

---

## Status

**v0.5.7** — Live SDK timeline and immediate decryption retry.
The Rust backend's room history now runs on persistent
`matrix-sdk-ui 0.18.0` **Timeline** objects instead of one-shot
`Room::messages` snapshots. Opening a room builds a live timeline
(atomic initial snapshot + `VectorDiff` subscription); every SDK
update — new events, edits, reactions, redactions, read-state, local
echoes — is applied to the Qt `TimelineModel` incrementally, in place,
with stable item identity. The headline fix: importing an encrypted
room-key export now retries decryption **immediately** on the open
timeline (`Timeline::retry_decryption` with the imported Megolm
session IDs, which never leave Rust) — already-visible
"[unable to decrypt yet]" rows update in place with no restart, no
room switch, and no manual refresh. Also new: backward pagination
(scroll to the top to load older history, with loading / retry /
end-of-history states), SDK-owned local echoes with sending → sent /
failed transitions and a per-message **Retry** action, and
replies/edits/reactions/redactions on the Rust backend via the
official timeline APIs. Sign-out now performs a deterministic
managed-task shutdown (timeline subscriptions and any in-flight key
import are joined before the crypto store is released) instead of the
old 5-second flag poll. Stale timeline callbacks are rejected by
room + lifecycle generations on both sides of the FFI. The Settings
**Security & Recovery** section no longer clips at narrow widths.
`CacheStore` still refuses decrypted encrypted-room plaintext — now
locked in by a dedicated regression test.

**v0.5.6** — Session verification and encrypted room-key import.
Lightning can now (a) initiate a Matrix SAS emoji verification of the
current session against another session belonging to the same Matrix
account, such as Element, reusing the existing receive-first UI and
`active_request` / `active_sas` slots so exactly one flow is in flight
at any moment, and (b) import a passphrase-encrypted Megolm room-key
export via `matrix-sdk::Encryption::import_room_keys` — decryption and
import happen entirely inside Rust, the passphrase is wrapped in
`zeroize::Zeroizing` and never persisted, and only aggregate counts
plus (already-public) affected room IDs cross the FFI. Settings ships
a new **Security & Recovery** section that shows the current session's
device ID and cross-signing state, exposes **Verify this session**,
keeps the existing recovery-key restore, and adds **Import room keys**
with a file picker + password-echo passphrase field + progress bar +
aggregate result summary. The UI enforces the conceptual separation:
SAS verification does not import keys; key import does not verify a
session; Secure Backup restoration is neither. The **Verified** label
is only shown when the SDK's cross-signing state reports the current
device as cross-signed by the account owner. Sign-out waits up to ~5 s
for an active import to finish before releasing the crypto store.
`CacheStore` still refuses decrypted encrypted-room plaintext.

**v0.5.5** — Rust sign-out/session-store lifecycle fix. Explicit
Rust sign-out now captures the current account, invalidates the active
callback generation, cancels and joins sync, attempts Matrix logout,
releases the SDK client, clears that account's saved Lightning session,
and removes only its `matrix-rust-sdk-store/` plus the existing persistent
smoke-session sidecar (including an interrupted-write `.tmp`). A late
callback from the old handle cannot repopulate rooms/timelines or turn a
clean sign-out into an `M_UNKNOWN_TOKEN` error. Active-session 401s remain
real errors.

Password login is blocked when an existing store cannot be paired safely
with saved account/device metadata. The Login-screen action **Reset local
Lightning session** derives the same canonical account slug from its current
homeserver and full MXID/localpart fields, so it also works while signed out.
It preserves `cache.sqlite`, other files in the account directory, every
other Lightning account, Element data, and all server messages. A later
password login may create a new Matrix device; Secure Backup recovery and/or
SAS verification may therefore be required again. A recovery key unlocks
backed-up Megolm room keys only—it cannot decrypt events whose room keys were
never backed up or shared. Decrypted encrypted-room plaintext remains
memory-only and is still rejected by `CacheStore`.

**v0.5.2** — Design-token foundation pass. First slice of the UI
redesign: `qml/AppTheme.qml` now exposes the full light + dark
palette from the redesign spec (background / sidebar / card /
hover / selection / accent / semantic-text / border), the full
spacing scale (`spacing2..spacing24`), radii (`radiusSm/Md/Lg/Pill`),
font-size scale (`fontSizeXS..fontSizePageTitle`, incl.
`fontSizeRoom` and `fontSizeHeader`), and font-family stacks
(`uiFontFamilies` = Inter + SF Pro Display + Segoe UI Variable
+ system sans; `monoFontFamilies` = JetBrains Mono + fallbacks).
Legacy aliases (`text`, `textMuted`, `surfaceAlt`, `spacingXS..XL`,
`radius`, `fontSizeL/XL`, `error`, `ownBubble`, `selectedBg`) are
preserved so no existing QML file breaks. **No backend, no Rust,
no E2EE, no SAS, no recovery-key behavior changed.** The larger
layout / split Spaces+Rooms navigation / bottom-left gear rework
is the next design pass.

**v0.5.1** — Post-verification retry decryption. When SAS
verification completes and the current Lightning room already has
`[unable to decrypt yet]` placeholders, AppController now
automatically calls `reloadCurrentRoomTimeline(50)` — matrix-sdk
0.18 has no public per-event "request room key" API (the internal
`event_cache/redecryptor.rs` re-runs automatically as keys arrive
on sync), so a `Room::messages` reload is the correct public
action after cross-signing propagates. Settings card status
updated to set accurate expectations about historical keys.

**v0.5.0** — Matrix SAS emoji verification landed for the Rust
backend (receive-first). Lightning can now accept a verification
request initiated from Element Classic, display the seven emojis
with descriptions, and confirm / mismatch / cancel the flow — all
via matrix-sdk 0.18's own state machine. The prep series ended at
`0.5.0-prep+13`; future bugfix / polish commits will use `0.5.1`,
`0.5.2`, etc.

Initiating verification from Lightning is a follow-up; the current
pass covers accept + confirm + mismatch + cancel as documented in
`docs/next-prompts.md`. `CryptoManager::supportsE2ee()` still `true`
for Rust only; `CacheStore` still refuses encrypted rows.

**v0.5.0-prep+13** — SAS emoji verification API research pass. No
functional SAS wiring shipped this cycle — rather than land
half-wired scaffolding that could leave a real verification
request stranded, the exact locked matrix-sdk 0.18 API surface has
been documented in `docs/next-prompts.md` for the next agent to
implement cleanly. Settings still says "Session (SAS emoji)
verification UI: not implemented yet". All prep+12 functionality
(E2EE gate for Rust, GUI recovery-key restore, timeline reload,
store mismatch reset, desktop polish) preserved.

**v0.5.0-prep+11** — Two Rust-GUI bug fixes:
1. **Encrypted rooms no longer look empty after restart.** New Rust
   FFI `mx_rust_reload_room_timeline` calls matrix-sdk 0.18
   `Room::messages` and re-emits recent events through the normal
   `timeline_event` path (deduped by `event_id`). AppController
   triggers it automatically on room selection and after a
   successful recovery-key restore. Settings gains a "Refresh
   current room" button. `CacheStore` still refuses encrypted rows;
   plaintext stays memory-only.
2. **Rust store/device mismatch is now recoverable in the GUI.**
   When matrix-sdk returns the SDK's "account in the store doesn't
   match" login error, `AppController::storeDeviceMismatchDetected`
   fires and both LoginScreen and SettingsScreen show a "Reset
   local Lightning session" button. It removes only that account's
   `matrix-rust-sdk-store/` + smoke session sidecar; server data,
   other accounts, and Element are untouched.

**v0.5.0-prep+10** — GUI recovery-key restore and honest E2EE
Settings section for the Rust backend, plus the footer/backend label
fix. Settings now shows the Lightning device id (redacted), a
recovery-key TextField backed by `AppController::requestRecoverFromBackup`,
and clear labelling of what is and isn't in the GUI yet (no SAS
emoji UI, no GUI cross-signing UI). Footer no longer misreports
`HTTP backend` on the Rust build: it now reads
`Matrix Rust SDK • Connected` when launched with
`--backend=rust`, `Mock backend • …` for the mock, `HTTP backend • …`
for HTTP. Recovery key never leaves the invocation: TextField is
`Password`-echo, cleared immediately after the button is pressed,
routed straight into the Rust FFI without a QML property retaining
it. Undecryptable messages still render as `[unable to decrypt yet]`
with an inline hint in Settings about how to restore keys.

**v0.5.0-prep+9** — **Initial E2EE support enabled for the Rust
backend.** Both directions have been round-tripped live against
Element Classic via the smoke harness:
`expect_text=seen decrypted_events_since_expect=1` (Element → Lightning)
and `encrypted_send=ok` displayed as readable text in Element
(Lightning → Element). `CryptoManager::supportsE2ee()` now returns
`true` for the Rust backend only. The C++ `RustSdkMatrixClient` no
longer refuses encrypted-room sends on the interactive UI path.
matrix-sdk does the encryption end-to-end; C++ never sees ciphertext
or keys. `CacheStore` still refuses encrypted `TimelineEvent` rows,
so decrypted plaintext is memory-only. HTTP and Mock backends are
unchanged: still no E2EE. No SAS emoji UI, no GUI recovery-key
flow, no cross-signing UI yet.

**v0.5.0-prep+8** — Fixes the smoke harness so `EXPECT_TEXT` waits
can actually complete. Dynamic global budget replaces the hard 60 s
kill (scales with `LIGHTNING_TEST_EXPECT_WAIT_SECONDS` and adds
headroom when `LIGHTNING_TEST_RECOVERY_KEY` is set). Room-list "joined
= N encrypted = M spaces = S" line no longer spams; only prints on
change. Clean process exit — the deadpool-sqlite reactor panic on
shutdown is avoided by intentionally leaking the Rust handle at
smoke exit (safe: OS reclaims immediately). Sync heartbeat every 30
s during long expect waits so a stalled sync is obvious. Duplicate
`key_backup=attempted` line eliminated. `CryptoManager::supportsE2ee()`
still returns `false`.

**v0.5.0-prep+7** — Adds a key-backup recovery probe via matrix-sdk
0.18 and a bounded EXPECT_TEXT wait loop for the smoke harness so
Element Classic → Lightning encrypted receive can actually be tested.
No E2EE claim change: `CryptoManager::supportsE2ee()` still returns
`false`. Interactive UI encrypted sends still blocked.

**v0.5.0-prep+6** — Matrix Rust SDK backend foundation, hardened,
verified live against `matrix.smetonis.net` (login + sync + rooms +
Space detection + timeline delivery), plus encrypted-receive
diagnostics and an encrypted-send probe. No E2EE claim yet:
`CryptoManager::supportsE2ee()` remains `false` until the receive +
send round-trip is verified against Element Classic.

Verified live (v0.5.0-prep+6 smoke run 1):

```
smoke: rooms joined=2 encrypted=2 spaces=1
smoke: initial_sync=done
smoke: summary login=ok sync=ok rooms=2 encrypted_rooms=2 spaces=1
       timeline_events=4 undecryptable=4 send=n/a supports_e2ee=false
exit=0
```

All observed timeline events on the test account were encrypted and
undecryptable from a fresh temp SDK store — expected for a device
that just logged in with no historical room keys. A later
encrypted-send probe succeeded and was visually confirmed in Element
Classic as readable text, so Lightning → Element encrypted send is
verified one-way. Element → Lightning encrypted receive is still not
verified until `LIGHTNING_TEST_EXPECT_TEXT` reports `expect_text=seen`
from a persistent SDK store/device.

The default HTTP backend remains the working production path. The
optional Rust backend now links `matrix-sdk` v0.18 and wires a real
Rust-owned SDK client behind the existing C++ `MatrixClient` seam:
login, session restore, joined-room sync, room-list events, basic text
timeline events, and plain text sends for unencrypted rooms. QML still
talks only to C++ models and signals.

**New after v0.5.0-prep+6 (persistent smoke store for receive verification):**

- `LIGHTNING_TEST_PERSISTENT_STORE=1` makes `--rust-sdk-smoke-test`
  use the same account-specific Rust SDK store path as the interactive
  Rust backend:
  `${XDG_DATA_HOME}/MatrixClient/matrix-client/<safeUserId>/matrix-rust-sdk-store/`.
  Default smoke mode still uses a fresh `QTemporaryDir` and does not
  pollute persistent state.
- Persistent smoke mode writes a smoke-only MatrixSession sidecar next
  to the account store so the second run can restore the same Matrix
  device without writing to interactive QSettings or SecretStore
  entries. The sidecar contains an access token; it is never printed
  and is created outside the repository.
- Smoke output now reports `restore=...`,
  `store_account_match=yes|no|unknown`, and a redacted `device_id`.
  If the SDK reports the known account/device mismatch, the harness
  deletes only that account's Rust SDK store and sidecar, prints
  `store_reset=account_device_mismatch`, and retries once.
- `CacheStore` skips encrypted `TimelineEvent` rows entirely. Decrypted
  encrypted-room plaintext may be displayed in memory by the Rust
  backend, but it is not written into `cache.sqlite` yet.

**New in v0.5.0-prep+6 (encrypted receive diagnostics + encrypted send probe):**

- `TimelineEvent` gains four encryption-metadata fields:
  `isEncrypted`, `isDecrypted`, `undecryptable`, `errorKind`. HTTP and
  Mock backends leave everything at defaults; the Rust bridge parses
  these out of matrix-sdk's encryption-info signals.
- Rust bridge event schema is now precise: `is_encrypted`,
  `is_decrypted`, `undecryptable`, `error_kind` on every
  `timeline_event`. The prep+5 `decrypted` field is still emitted for
  one release for backward compat.
- New Rust FFI `mx_rust_probe_encrypted_send` — mirror of
  `mx_rust_send_text` that only accepts encrypted rooms. matrix-sdk
  performs the encryption end-to-end via its `e2e-encryption +
  sqlite` features; the FFI never sees ciphertext or keys.
- New C++ hook `RustSdkMatrixClient::probeEncryptedSend(room, body,
  marker)` + signal `encryptedSendProbeResult(room, marker, ok,
  serverEventId, message)`. Only wired into the smoke harness; the
  interactive UI send path stays gated on `supportsE2ee()`.
- Smoke harness:
  * counts `encrypted_events`, `decrypted_events`, `undecryptable`
    from `TimelineEvent` flags instead of body-string matching;
  * `LIGHTNING_TEST_EXPECT_TEXT` — watch for a marker in decrypted
    bodies without ever printing it. Reports only `expect_text=seen`
    or `expect_text=not_seen`. Optional
    `LIGHTNING_TEST_REQUIRE_EXPECT=1` makes an un-seen marker
    non-zero (exit 14);
  * `LIGHTNING_TEST_SEND_ENCRYPTED=1` — probe encrypted send.
    Reports `encrypted_send=ok marker=<short-id> event_id=<id>` on
    success. Never prints the probe body. Exit 15 on real failure
    (skipped / non-encrypted-target are non-fatal).
- `mx_rust_supports_e2ee()` still returns 0; `CryptoManager::
  supportsE2ee()` still returns false. Flip only after both
  `expect_text=seen` and `encrypted_send=ok` are verified against
  Element Classic. The encrypted-send probe has been verified
  one-way; encrypted receive has not.

**Fixed in v0.5.0-prep+5 (store-path consistency):**

- `--reset-crypto-store` now scans the SAME app-data root the Rust
  backend writes to (`~/.local/share/MatrixClient/matrix-client/…`)
  plus the pre-fix "no org prefix" legacy root
  (`~/.local/share/matrix-client/…`), via the new
  `matrix::app_data::allRoots()` helper. Previously it scanned only
  the legacy layout and silently missed real stores, which caused
  the SDK's "account in the store doesn't match the account in the
  constructor" login failure to persist across resets.
- `RustSdkMatrixClient` uses that same helper for its per-account
  store path — no more silent divergence between the runtime path
  and the reset scanner.
- The smoke harness now runs against a fresh `QTemporaryDir` crypto
  store per invocation, via a new `setStorePathOverride` hook, so
  back-to-back password logins can't inherit a stale device id
  from a previous run.
- Rust store path is logged at INFO (base, slug, absolute store
  path, exists?, persistent-vs-temporary) — paths only, never
  tokens or keys.

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
  `${XDG_DATA_HOME}/MatrixClient/matrix-client/<safeUserId>/matrix-rust-sdk-store/`.
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
| `rust` | Optional Matrix Rust SDK backend foundation. Rust owns the SDK client/runtime/store; C++ polls FFI events and exposes them through existing models. Login/restore/sync/plain text send are wired. A smoke-only encrypted-send probe is verified one-way, but UI encrypted sends stay blocked and E2EE still reports unsupported until encrypted receive is verified too. Only built when `-DENABLE_RUST_SDK_BACKEND=ON`. |

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
  `${XDG_DATA_HOME}/MatrixClient/matrix-client/<safeUserId>/cache.sqlite`
  and let the next `/sync` refill it.
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
