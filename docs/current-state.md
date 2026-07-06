# Current state (v0.5.0-prep+10, GUI recovery + honest E2EE settings)

## v0.5.0-prep+10 — GUI E2EE controls

Reported symptoms after prep+9 landed:
- Rust GUI worked for sending, but the footer displayed
  "HTTP backend • Connected" even though the app was launched
  with `--backend=rust`.
- Timeline still showed many `[unable to decrypt yet]`
  placeholders for messages sent before the current Lightning
  device was created — expected, because the GUI had no way to
  restore the recovery key.
- No visible option in the GUI to verify the Lightning session or
  paste a recovery key.

Fixes in this pass:

- **Footer label.** `qml/Main.qml` now picks per backend:
  `Matrix Rust SDK • <status>`, `Mock backend • <status>`, or
  `HTTP backend • <status>`.
- **Settings E2EE panel.** New `Pane` in `qml/SettingsScreen.qml`,
  visible only when `app.backendName === "rust"`. Shows the
  redacted device id, a status line calling out what is / isn't
  implemented ("initial verified" for send + receive, "not
  implemented yet" for SAS emoji UI and cross-signing UI), and a
  recovery-key entry.
- **Recovery-key restore.** `AppController::requestRecoverFromBackup(QString)`
  invocable routes into `RustSdkMatrixClient::recoverFromBackup`.
  Recovery key TextField is `Password`-echo, wiped the moment the
  button is pressed, never held in a QML property beyond the
  invocation. Status flows back through
  `AppController::recoveryStateChanged(state, message)` which the
  Settings panel binds to via `Connections { target: app; … }`.
  States: `attempted` (button disables, shows "Recovery started"),
  `ok` (green "Recovery complete …" text), `failed` (red
  "Recovery failed: <safe reason>").
- **Local reset.** GUI reset button deliberately NOT implemented
  in this pass; the Settings panel points users at
  `matrix-client --reset-crypto-store` on the CLI, which already
  scans the correct roots (v0.5.0-prep+5). A dedicated GUI reset
  is a later step so we don't build a half-safe destroy path.
- **Undecryptable hint.** Settings panel now includes an inline
  note: "Some old messages may show '[unable to decrypt yet]'
  until you restore your recovery key here, or until another
  verified device shares the room keys." Placeholder rendering in
  the timeline is unchanged.

Not changed and preserved:
- `CryptoManager::supportsE2ee()` still returns `true` for Rust
  only. `RUST_SDK_E2EE_WIRED` still defined only under
  `ENABLE_RUST_SDK_BACKEND`.
- `CacheStore` still refuses encrypted `TimelineEvent` rows.
- Encrypted send/receive still route through matrix-sdk. C++
  never sees ciphertext or keys. Recovery key never logged.
- Smoke harness / persistent-store mode / `--reset-crypto-store`
  / encrypted-send probe FFI all untouched.

Known limitations (still):
- No SAS emoji verification UI (Settings panel says so).
- No GUI reset button (CLI works).
- No cross-signing management UI.
- No "Copy device ID" button (device id is displayed as
  redacted only; full id not yet exposed to QML).
- Interactive GUI shutdown still uses `mx_rust_destroy` — the
  deadpool-sqlite drop path from prep+8 is a follow-up.

# Current state (v0.5.0-prep+9, initial E2EE support enabled for Rust backend)

## v0.5.0-prep+9 — initial E2EE gate open

Both directions of the Rust SDK E2EE path have been verified live
against `matrix.smetonis.net`:

- **Element Classic → Lightning encrypted receive** (from the last
  smoke run):

  ```
  smoke: first_timeline_after_expect=yes
  smoke: expect_text=seen
  smoke: summary ...
         timeline_events_since_expect=1
         encrypted_events_since_expect=1
         decrypted_events_since_expect=1
         undecryptable_since_expect=0
         first_timeline_after_expect=yes
         supports_e2ee=false
  exit=0
  ```

- **Lightning → Element Classic encrypted send** (from the earlier
  prep+6 verification): `encrypted_send=ok marker=SMK-… event_id=$…`
  and Element Classic displayed the Lightning encrypted-send probe
  as readable text.

Concrete changes flipping the gate this pass:

- `CMakeLists.txt` defines `RUST_SDK_E2EE_WIRED=1` inside the
  `ENABLE_RUST_SDK_BACKEND` branch. HTTP and Mock builds do NOT
  define it.
- `CryptoManager::supportsE2ee()` returns `true` for the Rust
  backend only (both compile-time defines set AND the active
  backend name is `"rust"`).
- Rust FFI `mx_rust_supports_e2ee` returns `1`.
- Rust `mx_rust_send_text` no longer refuses encrypted rooms.
  matrix-sdk auto-encrypts via its `e2e-encryption + sqlite`
  features. C++ still gates the UI via
  `RustSdkMatrixClient::sendTextMessage`, which now passes the send
  through because `rustSupportsE2ee()` is true.
- `CryptoManager` status text / description updated to speak
  honestly: "Initial E2EE support (v0.5.0-prep+9): encrypted send
  + receive verified against Element Classic. SAS emoji UI, GUI
  recovery-key flow, cross-signing, and key backup management are
  not implemented yet."

`CacheStore` unchanged: encrypted `TimelineEvent` rows are still
refused, so decrypted encrypted-room plaintext remains memory-only.
`--reset-crypto-store`, persistent-store smoke mode, recovery-key
env var, and the encrypted-send probe are all preserved.

Known limitations (documented for honesty):
- No SAS emoji verification UI. Device verification currently
  happens externally through Element Classic ("Yes, it was me" +
  cross-signing propagation).
- No GUI recovery-key flow. Recovery is only exercised via
  `LIGHTNING_TEST_RECOVERY_KEY` in the smoke harness.
- No cross-signing management UI.
- No key backup management UI.
- Interactive GUI shutdown still uses the deadpool-sqlite drop
  path (only the smoke harness leaks). A clean GUI shutdown
  redesign is a follow-up.

# Current state (v0.5.0-prep+8, receive smoke reliability)

## v0.5.0-prep+8 additions

Fixes uncovered by the first live `EXPECT_TEXT` run against the
persistent store:

- **Dynamic global budget.** The old hard 60 s kill silently
  overrode `LIGHTNING_TEST_EXPECT_WAIT_SECONDS=180`, so the marker
  test never actually waited 3 minutes. Budget now scales:
  `max(60, expect_wait + 30)` when a marker is set, plus another
  30 s of headroom when a recovery key is set. Clamped to
  `[1, 3660]`. Printed once at startup as `smoke: budget timeout_s=N`,
  and the timeout emits `smoke: budget=exhausted timeout_s=N` before
  finalising cleanly.
- **Clean process exit / no deadpool panic.** matrix-sdk 0.18 uses
  `deadpool-sqlite` internally, whose async-drop paths require a
  live Tokio runtime when a `Client` is dropped. The smoke harness
  used per-call `current_thread` runtimes that were long gone by
  the time C++ tore down the wrapper — dropping the SDK Client from
  the main thread panicked with `there is no reactor running`
  AFTER the summary line had already been emitted. Fix: after
  `QCoreApplication::exec()` returns, the smoke harness now stops
  sync, sleeps 300 ms, prints `shutdown=leaked_for_process_exit`,
  and releases the unique_ptr without destroying it. The OS
  reclaims memory / FDs at process exit; no destructor path runs.
  Only smoke leaks — the interactive GUI still calls
  `mx_rust_destroy` normally (its own clean-shutdown story is
  documented as a known follow-up).
- **No more room-list spam.** The Rust SDK's sync callback fires
  many `rooms` events during a long wait; the harness now only
  prints `rooms joined=N encrypted=M spaces=S` when the counts
  actually change.
- **Duplicate `key_backup=attempted` gone.** The C++ side now
  relies on the `keyBackupResult` event handler to print the state
  (Rust bridge already emits `state=attempted` first, then `ok`/
  `failed`).
- **Long-wait heartbeat + since-counters.** During
  `expect_text=waiting`, a heartbeat every 30 s prints
  `sync=alive elapsed_s=N` plus the running
  `timeline_events_since_expect` / `encrypted_events_since_expect` /
  `decrypted_events_since_expect` / `undecryptable_since_expect`.
  New `timeline_events_since_expect` counter joins the existing
  three in the summary. `first_timeline_after_expect=yes` is
  printed the moment the first timeline event arrives during the
  wait.

## matrix-sdk 0.18 research findings (do not remove)

Confirmed by reading the locked source at
`~/.cargo/registry/src/index.crates.io-1949cf8c6b5b557f/matrix-sdk-0.18.0/`:

- `client.encryption().recovery().recover(key)` does exactly
  `secret_storage().open_secret_store(key)` → `import_secrets()` →
  `update_recovery_state()`. It imports **secrets** (including the
  backup recovery key + cross-signing seeds if present) into the
  local store. It does **not** synchronously download or import
  room keys.
- Room keys are then fetched **lazily** as encrypted events
  arrive on `/sync` — matrix-sdk detects a missing session and
  requests it from server-side backup / from other devices in
  the background. The smoke harness must therefore keep sync
  running long enough for that dance to complete.
- `client.encryption().backups().wait_for_steady_state()` is for
  **upload** progress after enabling backup, not for download.
  No public 0.18 API waits for room-key downloads to finish.
- `client.encryption().recovery().state()` returns a
  `RecoveryState` (Unknown / Disabled / Enabled / Incomplete).
  Useful to log post-recover, TODO in a future pass.
- No `recover_and_fix_backup` in 0.18 — that's newer. Available
  API used here is just `recover(&str)`.

Practical implication for `LIGHTNING_TEST_EXPECT_TEXT`: sending
the marker from Element **after** the Lightning smoke run has
called `recover()` is required. If the marker was sent long
before, the SDK will still try to fetch keys via backup + device
key requests, but only when a new sync response references those
events. Increase `LIGHTNING_TEST_EXPECT_WAIT_SECONDS` and/or send
a fresh marker after `key_backup=ok` prints.

## v0.5.0-prep+7 additions (retained)

# Current state (v0.5.0-prep+7, Rust SDK key backup probe + EXPECT_TEXT wait loop)

## v0.5.0-prep+7 additions

- New Rust FFI `mx_rust_recover_from_backup(recovery_key)` calling
  matrix-sdk 0.18 `client.encryption().recovery().recover(...)` to
  import backed-up room keys from server-side secret storage. FFI
  never logs the key or the imported material. Result flows through
  `key_backup_status` events on the poll queue.
- New C++ `RustSdkMatrixClient::recoverFromBackup(recoveryKey)` +
  signal `keyBackupResult(state, message)`.
- Smoke harness reads `LIGHTNING_TEST_RECOVERY_KEY` (base58 recovery
  key) after `initial_sync=done` and only when
  `LIGHTNING_TEST_PERSISTENT_STORE=1`. `LIGHTNING_TEST_RECOVERY_PASSPHRASE`
  is reserved but reports `passphrase_not_supported` — matrix-sdk
  0.18's fast-path recovery API takes a key.
- Smoke harness EXPECT_TEXT no longer finalises the moment
  `initial_sync=done` fires. When a marker is configured, the
  harness enters a bounded wait phase after any send/probe step
  finishes. Default 90 s, override via
  `LIGHTNING_TEST_EXPECT_WAIT_SECONDS`. New "since expect" counters
  (`encrypted_events_since_expect`, `decrypted_events_since_expect`,
  `undecryptable_since_expect`, `first_timeline_after_expect`) tell
  you whether new events arrived during the wait.
- SAS verification NOT implemented in this pass. Attempting it
  headlessly against matrix-sdk 0.18's async verification handshake
  risks unfinished state that the operator can't easily undo, so it
  is deferred to a session with a full token budget and interactive
  Element Classic driving.
- `CryptoManager::supportsE2ee()` unchanged: still `false`.
  `RUST_SDK_E2EE_WIRED` still undefined. Interactive UI encrypted
  sends remain blocked. `CacheStore` still refuses encrypted rows.


Last updated: 2026-07-05 (Rust SDK backend live-verified against
matrix.smetonis.net; encrypted-send probe verified one-way in Element
Classic; persistent Rust SDK smoke store/session support added so
encrypted receive can be tested with a stable device).

## Live verification status (v0.5.0-prep+6)

The maintainer ran the headless smoke harness twice against
`@test:matrix.smetonis.net` after the v0.5.0-prep+5 store-isolation
fix landed. Both runs exited 0 from a fresh QTemporaryDir SDK store.

Run 1 (no send):

```
smoke: rooms joined=2 encrypted=2 spaces=1
smoke: initial_sync=done
smoke: summary login=ok sync=ok rooms=2 encrypted_rooms=2 spaces=1
       timeline_events=4 undecryptable=4 send=n/a supports_e2ee=false
```

Run 2 (LIGHTNING_TEST_SEND=1):

```
smoke: send=skipped reason=no_unencrypted_room
smoke: summary … send=skipped(no_unencrypted_room) …
```

**What this proves:**

- Rust backend live login works (via matrix-sdk password login).
- Rust backend live joined-room sync works.
- Room list delivery works (2 rooms).
- Space detection works (1 Space).
- Timeline event delivery works (4 events observed).
- All observed events on this account are encrypted → the
  encrypted-timeline dispatch path is exercised.
- No decryption is possible from a fresh temp store — expected
  behaviour, not a bug.
- The plain-text send path safely skips when no unencrypted room
  exists.
- Smoke store isolation holds across back-to-back runs (no crypto
  store account/device mismatch).

Additional verified smoke result after prep+6:

```
smoke: encrypted_send=ok marker=SMK-1783280632 event_id=$NIP0ZhOlSs-NMUudtW_a3m45JmkxOeQZcsDks-mW3jQ
smoke: summary login=ok sync=ok rooms=2 encrypted_rooms=2 spaces=1
       timeline_events=4 encrypted_events=4 decrypted_events=0
       undecryptable=4 send=n/a encrypted_send=ok expect_text=n/a
       supports_e2ee=false
```

The maintainer confirmed in Element Classic that the Lightning
encrypted-send probe appeared as normal readable text. This proves
Lightning → Element Classic encrypted send one-way through matrix-sdk.

**What this does NOT prove yet:**

- Element Classic → Lightning encrypted receive. A fresh temp store
  still reports `expect_text=not_seen`, `decrypted_events=0`, and
  exits 14 when `LIGHTNING_TEST_REQUIRE_EXPECT=1`.
- Full E2EE support. Interactive UI encrypted sends remain blocked,
  SAS verification, key backup, and cross-signing are missing, and
  `RUST_SDK_E2EE_WIRED` remains undefined.

`CryptoManager::supportsE2ee()` remains **false**. Flipping it
requires Element Classic → Lightning `expect_text=seen` on a real
marker and Lightning → Element `encrypted_send=ok`. The second is now
verified one-way; the first is still pending.

This is the "where the repo actually is" doc. Treat it as ground truth
for a fresh LLM continuation session — read this before
`docs/matrix-feature-status.md`, before `README.md`, before touching
code.

## Repository

- Origin: `https://gitlab.smetonis.net/Mizerd/lightning.git`
- Local path this doc was written from: `/home/roksme/git/lightning/`
- Branch: `main`
- Most recent commits (newest last):
  - `c522f5d` Codex setup
  - `4310913` Fix Nix dev shell: drop stale qtquickcontrols2 attr, add .gitignore
  - `d251948` v0.4: SecretStore + backend CLI cleanup + Rust SDK backend scaffold
  - `13adf73` v0.4.1: Spaces + Threads foundations, SSO/OIDC flags, continuation docs
  - `1a5adba` v0.4.2: HTTP Spaces parsing + no-display preflight hardening
  - `2230bbe` v0.4.3: Nix Qt platform runtime fix + --http/--rust CLI hint
  - `da9f331` v0.4.4: real HTTP m.thread relation send + parse
  - `dbb28e0` v0.4.5: HTTP login transition fix + Lightning branding + Space/thread cache persistence
  - `31cbc22` v0.4.6: HTTP /sync bring-up polish + initialSyncDone capability + docs sweep
  - `50d4a6e` v0.4.7: HTTP restore recovers from stale Space-only cache
  - `41a9f69` v0.4.8: cache NOT NULL repair, delegate anchor warning, Connected status
  - `6f389aa` v0.5.0-prep: C++ groundwork for E2EE via matrix-sdk (crate not linked yet)
  - `9ade51b` v0.5.0-prep+1: --reset-crypto-store shows resolved paths; matrix-sdk still blocked at classifier layer
  - `8205606` rust: pull matrix-sdk deps for offline builds
  - `9eaa488` Wire Matrix Rust SDK backend foundation (Codex)
  - `4c9d4f5` Harden Matrix Rust SDK backend foundation (v0.5.0-prep+3)
  - `8d6f436` Harden Matrix Rust SDK backend testing (v0.5.0-prep+4)
  - `9bf3c83` Fix Rust SDK smoke store isolation (v0.5.0-prep+5)
  - HEAD after this pass: persistent Rust SDK smoke store for receive verification
  - Branch: `main`

## Layered architecture (unchanged from v0.4)

```
Qt/QML UI   →  qml/*.qml
App layer   →  src/app/{AppController,SettingsManager}
Auth        →  src/auth/{AuthManager,AccountManager}
UI models   →  src/models/{RoomListModel,TimelineModel,MessageComposer}
              src/spaces/SpaceManager        ← QAbstractListModel of Spaces
              src/threads/ThreadManager      ← thread aggregation helper
Backend iface: src/matrix/MatrixClient.h      ← the swap seam
Backends:     src/matrix/MockMatrixClient.{h,cpp}      --backend=mock
              src/matrix/CppHttpMatrixClient.{h,cpp}   --backend=http (default)
              src/matrix/RustSdkMatrixClient.{h,cpp}   --backend=rust
                                                       (only when compiled with
                                                        -DENABLE_RUST_SDK_BACKEND=ON)
Rust crate:  rust/                                    static lib + C ABI shim
Storage:     QSettings (prefs + non-secret session metadata)
             src/storage/SecretStore (LibSecret or InsecureFallback)
             src/storage/CacheStore  (SQLite: rooms/events/members)
Platform:    src/notifications/NotificationManager (stub)
             src/media/MediaManager (send/receive + open-external)
Crypto:      src/crypto/CryptoManager (capability surface only, no crypto)
```

## What is *implemented* right now

- **Product name**: **Lightning**. Window title / header / login-screen
  heading all say "Lightning". The executable is still `matrix-client`
  for build-system compatibility (Q_APPLICATION_NAME too — keeps the
  QSettings scope stable across the rename). The login-screen
  sub-heading is backend-aware (v0.4.5).
- **Persistent Rust SDK smoke store/session (this pass)**:
  `LIGHTNING_TEST_PERSISTENT_STORE=1` switches the headless Rust smoke
  harness from a fresh `QTemporaryDir` to the same account-specific
  Rust SDK store path used by the interactive Rust backend:
  `matrix::app_data::primaryRoot()/<safeUserId>/matrix-rust-sdk-store/`
  (normally
  `${XDG_DATA_HOME}/MatrixClient/matrix-client/<safeUserId>/matrix-rust-sdk-store/`).
  The default smoke mode is unchanged and still uses a temporary store.

  Persistent smoke mode configures a smoke-only MatrixSession sidecar:
  `matrix-rust-sdk-smoke-session.json` next to the account store. Rust
  writes it after password login and reads it through
  `mx_rust_restore_from_file` on the next run, so the SDK restores the
  same device without writing to the interactive QSettings/SecretStore
  session. The sidecar contains an access token, is 0600 on Unix, is
  never printed, and must not be committed.

  Smoke output now includes `restore=...`,
  `store_account_match=yes|no|unknown`, and a redacted `device_id`.
  If matrix-sdk reports the known "account in the store doesn't match
  the account in the constructor" error, the harness destroys the Rust
  handle, deletes only that account's `matrix-rust-sdk-store/` plus
  the smoke session sidecar, prints
  `store_reset=account_device_mismatch`, and retries password login
  once. It never deletes `cache.sqlite`, QSettings, or SecretStore
  entries.

  This implements the required workflow for encrypted receive
  verification:
  first persistent run creates/restores a stable Lightning SDK device;
  the user approves the new login in Element Classic if prompted; the
  user sends a harmless marker from Element Classic; the second
  persistent run uses `LIGHTNING_TEST_EXPECT_TEXT=<marker>` and
  `LIGHTNING_TEST_REQUIRE_EXPECT=1`. Success is `expect_text=seen`,
  `decrypted_events>0`, and exit 0. Until that happens, encrypted
  receive remains unverified.

  `CacheStore` now refuses to persist encrypted `TimelineEvent` rows:
  decrypted encrypted-room plaintext can be displayed in memory by the
  Rust backend, but is not written into `cache.sqlite` yet.
- **v0.5.0-prep+6 encrypted-receive diagnostics + encrypted-send
  probe (this pass)**: five connected changes wired end to end
  around real, safe E2EE plumbing — but no E2EE claim is made
  from the UI yet.

  1. `TimelineEvent` (`src/matrix/TimelineEvent.h`) gains
     `isEncrypted`, `isDecrypted`, `undecryptable`, `errorKind`.
     Defaults keep HTTP and Mock unchanged. All plumbing is
     metadata-only — the C++ layer never derives plaintext from
     these fields.
  2. `rust/src/lib.rs` `install_event_handlers` splits the two
     paths precisely:
     - `OriginalSyncRoomMessageEvent` (plaintext or SDK-decrypted)
       emits `is_encrypted = encryption_info.is_some()`,
       `is_decrypted = encryption_info.is_some()`,
       `undecryptable = false`.
     - `OriginalSyncRoomEncryptedEvent` (undecryptable) emits
       `is_encrypted = true, is_decrypted = false,
       undecryptable = true, error_kind = "no_key"`, empty body.
     The legacy `decrypted` field is still emitted for one
     release for backward compat.
  3. New Rust FFI `mx_rust_probe_encrypted_send` — mirror of
     `mx_rust_send_text` that ONLY accepts encrypted rooms
     (refuses non-encrypted with `encrypted_send_failed`).
     matrix-sdk does the encryption end-to-end via its
     `e2e-encryption + sqlite` features; the FFI never sees
     ciphertext, keys, or session material. On success the SDK
     returns a real server event id (safe to log).
  4. `RustSdkMatrixClient::probeEncryptedSend(room, body, marker)`
     wraps that FFI, tracks the txn id in a new `m_pendingProbes`
     map, and emits a new signal
     `encryptedSendProbeResult(room, marker, ok, serverEventId,
     message)`. Deliberately NOT wired into QML — the interactive
     UI send path stays gated on `CryptoManager::supportsE2ee()`.
     `handleEncryptedSendOk` / `handleEncryptedSendFailed` bridge
     the FFI events to that signal.
  5. Smoke harness (`src/smoke/RustSdkSmokeTest.cpp`) uses the new
     `TimelineEvent` flags to break `undecryptable` out of the
     total timeline event count and add `encrypted_events` and
     `decrypted_events`. Two new env vars land:
     - `LIGHTNING_TEST_EXPECT_TEXT` — a marker sent from Element
       Classic. The harness watches decrypted bodies for a match
       but never prints the marker itself; only `expect_text=seen`
       or `not_seen` is emitted. Combined with
       `LIGHTNING_TEST_REQUIRE_EXPECT=1` a missing marker becomes
       exit code 14.
     - `LIGHTNING_TEST_SEND_ENCRYPTED=1` — drives
       `probeEncryptedSend` against the first encrypted joined
       room (or `LIGHTNING_TEST_ROOM_ID`). Prints
       `encrypted_send=ok marker=<short-id> event_id=<id>` on
       success. Real send failures / timeouts return exit 15;
       "no encrypted room" and "target not encrypted" are
       `skipped` and remain exit 0.
     The summary line now includes `encrypted_events=N`,
     `decrypted_events=D`, `encrypted_send=<status>`,
     `expect_text=<status>`, and `supports_e2ee=<bool>`.

  `mx_rust_supports_e2ee()` still returns 0.
  `CryptoManager::supportsE2ee()` still returns false. The
  compile-time gate `RUST_SDK_E2EE_WIRED` is deliberately NOT
  defined this pass — flipping it requires both
  `expect_text=seen` AND `encrypted_send=ok` verified against
  Element Classic on a device with the room keys.

- **v0.5.0-prep+5 store-path consistency**: three
  connected fixes that closed the "SDK still opens an old crypto
  store after --reset-crypto-store" surprise reported after the
  headless smoke harness landed:
  1. New helper `matrix::app_data` at
     `src/storage/AppDataPaths.{h,cpp}` computes the same
     `QStandardPaths::AppLocalDataLocation`-style root as the
     runtime (`<XDG_DATA_HOME>/MatrixClient/matrix-client`) plus a
     list of legacy roots earlier v0.5.0-prep builds may have
     written to. Safe to call before `QCoreApplication` exists,
     which is exactly what `--reset-crypto-store` needs.
  2. `main.cpp --reset-crypto-store` now iterates
     `matrix::app_data::allRoots()` and lists every scanned root
     in its stdout, so users see exactly which layouts were
     inspected. It never touches `cache.sqlite`, QSettings, or
     the SecretStore. Bug fixed: pre-v0.5.0-prep+5 the scanner
     computed `<XDG_DATA_HOME>/matrix-client` directly, missing
     the `MatrixClient/` organisation-name segment Qt puts in
     `AppLocalDataLocation`. Any store the runtime created was
     invisible to reset, so `--reset-crypto-store` produced
     "No Rust SDK store directories found" while the SDK still
     opened the same (mismatched) store on the next login.
  3. `RustSdkMatrixClient` now uses the same helper for its
     per-account store, adds a `setStorePathOverride(QString)`
     testing hook for the smoke harness, exposes `rustStorePath()`
     and `rustStorePathIsOverride()` for diagnostics, and logs
     `base`, `slug`, `store`, `exists`, `mode` at INFO on the
     `matrix.rust` category. Path-only — no tokens, keys, or
     bodies.

  The smoke harness (`src/smoke/RustSdkSmokeTest.cpp`) now creates
  a `QTemporaryDir` under `/tmp/lightning-rust-sdk-smoke-XXXXXX/`
  and calls `setStorePathOverride` before login, so consecutive
  smoke runs never inherit a stale device id. The harness prints
  `smoke: store=temporary`, `smoke: store_path=<abs>`, and
  `smoke: store_exists=yes|no` up front and `supports_e2ee=…` in
  both the header and the summary line.

  Send outcome semantics also relaxed: `send=skipped` (no
  unencrypted room found) and `send=blocked` (target is
  encrypted / a Space / not in synced rooms) are now non-fatal
  and exit code stays 0. Only real send failures / timeouts
  return exit code 13.

- **v0.5.0-prep+4 verification harness**: a new headless
  smoke-test CLI mode for the Rust backend, gated to
  `ENABLE_RUST_SDK_BACKEND` and only accepted alongside
  `--backend=rust`. Sources at `src/smoke/RustSdkSmokeTest.{h,cpp}`;
  entry point invoked from `src/main.cpp` after preflight, before
  `QGuiApplication`.
  - Reads credentials only from environment variables
    (`LIGHTNING_TEST_HOMESERVER`, `LIGHTNING_TEST_USER`,
    `LIGHTNING_TEST_PASSWORD`, optional `LIGHTNING_TEST_SEND=1`,
    optional `LIGHTNING_TEST_ROOM_ID`). No creds ever land on a
    command line.
  - Constructs `RustSdkMatrixClient(nullptr, &app)` on purpose — a
    null `SettingsManager` prevents the smoke test from ever
    overwriting the interactive user's cached access token,
    syncToken, or homeserver. The Rust SDK store *is* still created
    under the test account's slug and can be wiped with
    `--reset-crypto-store`.
  - Prints `smoke: …` lines with counts (joined room count,
    encrypted room count, Space count, timeline event count,
    undecryptable event count) and statuses (`login=ok/failed`,
    `initial_sync=done`, `send=ok/failed/timeout`). Never prints
    message bodies, tokens, passwords, or crypto keys.
  - 60 s wall-clock budget with intermediate 30 s post-login sync
    guard and 15 s post-send confirmation guard. Exit codes: 0 on
    success, 10/11/12 for login/sync/room-count failures, 13 for
    send failure (only when `LIGHTNING_TEST_SEND=1`), 2 for
    missing env / wrong backend / wrong build.
  - `--help` output documents the flag. The preflight rejects it in
    a non-Rust build with exit 2 and a clean pointer to
    `-DENABLE_RUST_SDK_BACKEND=ON`.
  - See `docs/build-and-test.md` for exact usage and safety notes.

- **v0.5.0-prep+3 hardening**: three targeted fixes on
  top of Codex's `9eaa488` foundation.
  - **Bounded event queue.** The Rust-side `VecDeque<String>` used
    to accept unbounded pushes; a stalled C++ poll timer would
    grow it forever. Now capped at `EVENT_QUEUE_CAP = 4096` — on
    overflow we drop the oldest event and emit a single
    `queue_overflow` marker so the C++ side can log/surface it as
    a warning banner (`errorOccurred`).
  - **Sync-start race fixed.** `mx_rust_start_sync` used to check
    `sync_stop.is_some()`, release the lock, spawn the thread,
    then install the `stop` slot — two rapid `startSync()` calls
    could both see `None` and both spawn. Now the "already
    running?" check and slot reservation happen atomically under
    the same lock guard, before `thread::spawn`. No more leaked
    sync loops.
  - **Undecryptable encrypted events surface as a placeholder.**
    Codex's `install_event_handlers` only handled
    `OriginalSyncRoomMessageEvent` (plaintext or SDK-decrypted).
    Events the SDK could not decrypt silently disappeared,
    leaving encrypted rooms visually empty. Added a second
    handler for `OriginalSyncRoomEncryptedEvent` that emits a
    `timeline_event` with `undecryptable: true` and an empty
    body. `RustSdkMatrixClient::handleTimelineEvent` renders
    that as `[unable to decrypt yet]` (`TimelineEvent::Notice`).
    The ciphertext itself is deliberately NOT included in the
    FFI payload — C++ never needs it.
  - Comment on `login_ok` handling in
    `src/matrix/RustSdkMatrixClient.cpp` marks the payload
    sensitive: the `access_token` field must flow only into
    `SettingsManager::saveSession` (which routes to SecretStore)
    and must never appear in any log line.

- **v0.5.0-prep+2 foundation**: the optional Rust backend is no longer just a
  scaffold. `matrix-sdk` v0.18 is in `rust/Cargo.toml`, `Cargo.lock`
  is committed, and the Rust crate builds offline. The Rust FFI now
  owns a Matrix SDK client, SDK SQLite store, async work threads, and
  a JSON event queue drained by C++ on a `QTimer`.

  Implemented through the Rust path:
  - password login via `matrix_auth().login_username(...)`;
  - session restore via `MatrixSession` and `restore_session(...)`;
  - joined-room sync via `sync_with_callback(...)`;
  - room list events, including room name/topic/avatar/encrypted/Space
    flags where the SDK exposes them;
  - basic text/notice/emote timeline events through SDK event handlers;
  - plain text sends into unencrypted rooms, with C++ local echo
    reconciliation.

  Still not claimed:
  - E2EE is not enabled. `mx_rust_supports_e2ee()` returns 0 and
    `CryptoManager::supportsE2ee()` remains false.
  - Encrypted sends are blocked until encrypted read and send are
    verified end to end.
  - Rich timeline features in the Rust backend (pagination, replies,
    edits, reactions, media, typing, read receipts, Space child
    hierarchy) are still missing or partial.

  Store path: `${XDG_DATA_HOME}/MatrixClient/matrix-client/<safeUserId>/matrix-rust-sdk-store/`.
  This is separate from the C++ `cache.sqlite` and never stores access
  tokens; tokens remain in `SecretStore`.

- **v0.5.0-prep+1**: added --reset-crypto-store path resolution
  and documented the classifier block at the settings layer.
  Details in the git log.
- **v0.5.0-prep**: original C++ groundwork pass. `CMakeLists.txt`
  `PROJECT_VERSION` → `0.5.0`; `APP_VERSION_LABEL` → `"0.5.0-prep"`.
  `--reset-crypto-store` added as a pre-flight-recognised flag.
  Full write-up in `git show 6f389aa`.
- **Stabilisation (v0.4.8)**: three targeted fixes on top of v0.4.7:
  - **CacheStore NOT NULL repair**: previous versions bound null
    `QString` values to `rooms.child_room_ids` / `events.thread_root_id`
    (NOT NULL columns added in v0.4.5), which Qt's QSQLITE driver
    writes as SQL `NULL` and triggered `NOT NULL constraint failed`
    on every save. Fix: `textNonNull()` helper in
    `src/storage/CacheStore.cpp` coerces empty/null strings to a
    non-null empty QString at bind time, plus an idempotent repair
    (`UPDATE … SET col = '' WHERE col IS NULL`) on schema-ensure.
    No cache wipe.
  - **QML Column anchor warning**: `qml/MessageDelegate.qml`
    replaced an inner `MouseArea { anchors.fill: parent }` (invalid
    as a direct Column child) with a `HoverHandler`. Warning gone;
    hover-off behaviour unchanged.
  - **Status text `Connected`**: `AppController` reports
    `Connected` once the initial `/sync` response has been parsed
    and long-poll is the steady state, instead of continuing to
    say `Syncing`. Loading catch-up still says `Loading rooms…`.
- **HTTP `/sync` bring-up (v0.4.7)**: the initial `/sync` (no
  `since` token) uses `timeout=0&full_state=true`; the server
  returns current state immediately instead of long-polling.
  Follow-ups long-poll with `timeout=30000`. Request transfer
  timeout is 30s / 60s respectively — comfortably above the 30s
  server-side wait so we don't false-time-out.
  On session restore, a stored `syncToken` is discarded when the
  SQLite cache has no visible non-Space rooms, because an incremental
  token without visible cached room state cannot reconstruct the room
  list. This specifically handles the observed broken state where the
  cache held the Space room and `m.space.child` ids but no joined child
  room rows yet, causing the UI to render an empty list while
  incremental syncs had no reason to resend the full room snapshot.
  Fresh login also clears any persisted `syncToken` for the same MXID
  before starting sync.
  `MatrixClient::initialSyncDone()` is a new capability on the
  interface (default `true`; only `CppHttpMatrixClient` overrides
  and toggles it) so QML can distinguish "still waiting for the
  first response" from "sync loop live, no rooms". The room list
  header now shows the model count only after the first sync
  response lands, and the empty-state label under the list is
  state-aware: sign-in / loading / no joined rooms / no rooms in
  the selected Space.
  Non-secret sync diagnostics: `matrix.http:` log lines announce
  each request kind, HTTP status, response body size, and the
  joined / invited / left counts parsed. Tokens are never logged.
- **HTTP login → main-screen transition (v0.4.5)**: the `Loader` in
  `qml/Main.qml` used to pick the current page via
  `switch (app.currentScreen) { case app.LoginScreen: … }`. That
  pattern turned out to be fragile when the enum is exposed via
  `setContextProperty` (not registered as a QML type) and the QML
  files are AOT-compiled by the Qt Quick compiler — the case values
  could resolve to `undefined`, no case matched, and the switch fell
  through to `loginComponent`. Fresh HTTP login therefore logged
  "login ok" but the UI stayed on the login screen.
  Fix: integer-literal comparisons against the well-known
  `AppController::Screen` values in a `pickComponent()` helper, plus
  an explicit `Connections { onCurrentScreenChanged … }` re-trigger.
  Diagnostic `qCInfo(lcApp)` lines in
  `AppController::setCurrentScreen` and `onLoginSucceeded` make any
  future regression obvious in the terminal.
- **Backend selection**: `--backend={mock,http,rust}` plus legacy `--mock`.
  Pre-flight validation runs *before* `QGuiApplication` so bad args exit
  cleanly with exit code 2 even without a display. **v0.4.2**: a second
  preflight check refuses to construct `QGuiApplication` when neither
  `DISPLAY` nor `WAYLAND_DISPLAY` is set and `QT_QPA_PLATFORM` is not
  forced — exits 3 with a clear message instead of Qt's `qFatal`
  abort(). **v0.4.3**: `--http` and `--rust` are rejected pre-flight
  with a message pointing at `--backend=http` / `--backend=rust`.
- **Nix dev-shell runtime (v0.4.3)**: `flake.nix` / `shell.nix`
  `shellHook` now purges `QT_PLUGIN_PATH`, `QT_QPA_PLATFORM_PLUGIN_PATH`,
  `QML_IMPORT_PATH`, `QML2_IMPORT_PATH`, `QT_QUICK_CONTROLS_STYLE`,
  `QT_QUICK_CONTROLS_STYLE_PATH`, and `QT_QPA_PLATFORMTHEME` inherited
  from the outer KDE / GNOME session, then exports flake-consistent
  values against `${qt.qtbase}` and `${qt.qtwayland}` plus
  `QT_XKB_CONFIG_ROOT`. This fixes the reported crash where a KDE
  Plasma session's qtbase 6.11.0 helper plugin was being loaded into a
  6.11.1 executable and aborting at plugin init. Details in
  `docs/build-and-test.md`.
- **Mock backend** (`--backend=mock`): hardcoded rooms, one Space
  containing two rooms, one standalone room, one threaded conversation,
  synthetic reactions/edits/redactions/media/pagination.
- **HTTP backend** (`--backend=http`, default): password login,
  `/whoami` restore, long-poll `/sync`, room list, text messages,
  replies, edits, redactions, reactions, typing, read receipts,
  pagination via `/messages?dir=b`, member cache, media send/receive
  via legacy `/_matrix/media/v3/*`, local SQLite cache. **No E2EE.**
  Encrypted rooms are read-only placeholders; sends into encrypted
  rooms are blocked with a clear error.
- **Rust backend** (`--backend=rust`, only with
  `-DENABLE_RUST_SDK_BACKEND=ON`): Matrix Rust SDK backend
  foundation. Rust owns the SDK client/runtime/store and pushes JSON
  events through a C ABI queue. C++ `RustSdkMatrixClient` keeps the UI
  isolated from Rust and emits the existing `MatrixClient` signals.
  Login, restore, joined-room sync, basic text timeline events, and
  plain text send are wired. E2EE is still disabled; interactive
  encrypted sends are blocked honestly while the smoke-only encrypted
  send probe remains available for verification.
- **SecretStore**: libsecret (Secret Service via glib) backend when
  available; `InsecureFallbackSecretStore` (QSettings under `secrets/*`)
  when the session bus is unreachable. Legacy plaintext
  `session/accessToken` in QSettings is auto-migrated into the store
  on first launch of v0.4+.
- **SQLite cache**: `${XDG_DATA_HOME}/MatrixClient/matrix-client/<safeUserId>/cache.sqlite`.
  Rooms + last 200 non-encrypted events per room + members. Access
  tokens and decrypted encrypted-room bodies are **not** cached here.
  `SettingsManager::clearSession()` wipes both the QSettings session
  metadata and the SecretStore entry for that user.
- **Spaces (v0.4.1 + v0.4.2)**: `SpaceManager` is a `QAbstractListModel`
  bound to the active `MatrixClient`. Row 0 is a synthetic "All rooms";
  if any Space has children, an "Other rooms" row follows; then real
  Spaces. QML `RoomListPane` renders a chip strip when at least one
  real Space exists. `RoomListModel` applies the active-space filter.
  **v0.4.2**: `CppHttpMatrixClient::processStateEvent` now recognises
  `m.room.create` (`content.type == "m.space"` → `RoomInfo::isSpace`)
  and `m.space.child` (state key = child room id, `via[]` non-empty =
  active edge; empty = unlinked). Both events are handled from the
  `state.events` bucket AND from timeline state events. The
  `RoomInfo::spaceId` "primary parent" hint is intentionally not set on
  children — SpaceManager builds membership strictly from
  `Space.childRoomIds`, so rooms in multiple Spaces stay consistent.
  **v0.4.5**: `CacheStore` now persists `isSpace` and `childRoomIds`.
  On relaunch the Space chip strip renders immediately from cache —
  no more blank-until-first-`/sync` window.
- **Threads (v0.4.1 + v0.4.4)**: `MessageComposer` gains thread-reply
  mode via `beginThreadReply(rootId, preview)`. `TimelineModel`
  exposes `threadRootId`, `isThreadRoot`, `threadReplyCount` roles.
  Mock backend seeds a threaded conversation.
  `MatrixClient::sendThreadReply` is a virtual with a default that
  falls back to `sendReply`. **v0.4.4**: `CppHttpMatrixClient` now
  overrides `sendThreadReply` and emits a real `m.thread` relation:

  ```json
  { "m.relates_to": {
      "rel_type": "m.thread",
      "event_id": "$root",
      "is_falling_back": true,
      "m.in_reply_to": { "event_id": "$latest-or-root" }
  } }
  ```

  `processTimelineEvent` (live `/sync`) and the pagination path
  (`/messages?dir=b`) both recognise `rel_type == "m.thread"` and set
  `TimelineEvent::threadRootId`. When the same event carries an
  `m.in_reply_to` (fallback for non-thread-aware clients), the
  `replyToEventId` field is intentionally cleared — QML would
  otherwise render both the "in thread" chip AND the reply preview
  strip, which is noise. Local echo is set with `threadRootId` so the
  chip shows immediately; the existing txnId dedup + `eventReplaced`
  path already reconciles it with the server-confirmed event.

  Known limitations documented in `docs/matrix-feature-status.md`:
  * `unsigned["m.relations"]["m.thread"]` server aggregation (latest
    event, reply count) is not read yet. Reply counts are computed
    locally by scanning the loaded timeline (v0.4.1 behaviour).
  * Thread replies still appear inline in the main timeline (marked
    "in thread") — a dedicated thread side-panel is v0.5+.
  * `CacheStore` now persists `threadRootId` (v0.4.5) — the "in
    thread" chip renders immediately after restart for cached events.
- **SSO/OIDC capability flags (v0.4.1)**: `AuthManager` exposes
  `supportsPasswordLogin`, `supportsSsoLogin` (false), `supportsOidcLogin`
  (false), plus placeholder `beginSsoLogin` / `beginOidcLogin` that
  emit a clean "not implemented" error. QML is not wired to these yet
  (Settings screen is the natural spot in a follow-up).

## What is *stubbed or partial* — code exists but does not do the work

- `NotificationManager`: logs "notify" via QLoggingCategory. No tray, no
  native notify.
- `CryptoManager`: capability surface only. `supportsE2ee` is a pure
  compile-time expression — it becomes true only if
  `ENABLE_RUST_SDK_BACKEND` **and** `RUST_SDK_E2EE_WIRED` are both
  defined, AND the active backend is "rust". `RUST_SDK_E2EE_WIRED` is
  not defined in this pass.
- `RustSdkMatrixClient` rich operations: pagination, replies, edits,
  redactions, reactions, media send/receive, typing, read receipts,
  and Space child hierarchy are not wired through Rust yet.
- `AccountManager`: tracks the single active user id from the session.
  Multi-account (per-account SecretStore keyspace, cache path, sync
  loop) is not implemented — foundation described in
  `docs/next-prompts.md`.

## What is *intentionally missing*

- Verified E2EE. The Rust backend links the SDK but encrypted
  read/send have not been manually verified end to end, so Lightning
  still reports no E2EE support and will not hand-roll cryptography in
  C++.
- Authenticated media (`/_matrix/client/v1/media/*`) — v0.3/v0.4 use
  legacy `/_matrix/media/v3/*`.
- Full SSO / OIDC / MAS login flow.
- Multi-account UI switching. `AccountManager` API is single-active.
- Sliding sync.
- Windows / macOS SecretStore backends (they fall back to the insecure
  store with a warning until v0.5+).
- Own-profile lookup, other-user read-receipt display, member list UI.
- Thread panel / per-thread timeline model. Current thread UI is a
  chip on the root event + a composer mode.

## Build & smoke summary (see `docs/build-and-test.md` for details)

- Default: `nix develop -c cmake -S . -B build -G Ninja && nix develop -c cmake --build build`.
- With Rust: `nix develop -c cmake -S . -B build-rust -G Ninja -DENABLE_RUST_SDK_BACKEND=ON && nix develop -c cmake --build build-rust`.
- Rust-only offline: `cd rust && nix develop -c cargo build --release --offline`.
- Smoke: `QT_QPA_PLATFORM=offscreen timeout 3 ./build/matrix-client --mock`
  should exit 124 with no QML warnings and no crashes.
- Rejection: `QT_QPA_PLATFORM=offscreen ./build/matrix-client --backend=bogus`
  and `./build/matrix-client --backend=rust` (in the non-Rust build)
  both exit 2 with a clear stderr message. In the Rust build,
  `QT_QPA_PLATFORM=offscreen timeout 3 ./build-rust/matrix-client --backend=rust`
  should start without crashing.

## Rules for continuation

1. Do not rewrite from scratch. Keep the file layout.
2. Do not fake E2EE. `CryptoManager::supportsE2ee` is the single source
   of truth for the UI.
3. Do not remove the mock or HTTP backends.
4. Do not use Electron, Tauri, WebEngine chat UI, or Element Web.
5. Prefer C++ for anything that is not a Matrix cryptographic primitive
   or a place where the Matrix Rust SDK is the objectively correct
   dependency.
6. Any `Q_PROPERTY(T*)` in a header must have `T` fully defined in that
   header (moc reads `QMetaType::fromType<T>()` which requires
   completeness — we've been bitten by this twice, see the includes
   at the top of `src/app/AppController.h`).
7. Bad `--backend=…` values must never crash — pre-flight validation
   in `src/main.cpp` catches them before Qt starts.
