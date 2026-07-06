# Next prompts

Pre-written prompts a future session can copy verbatim. Each is
scoped small enough that one pass should keep the build green.

Read `docs/current-state.md`, `docs/backend-contract.md`, and
`docs/matrix-feature-status.md` **before** running any of these.

Order matters — prompts higher in the list unlock features that later
prompts depend on. Do not skip ahead.

**As of v0.4.3, runtime is stable**: the reported KDE-session Qt
platform plugin crash was fixed in `flake.nix` / `shell.nix` (see the
`Troubleshooting` section in `docs/build-and-test.md`). Feature work
can resume with Prompt 1 below.

**As of v0.4.4, real HTTP `m.thread` send + parse are live**.

**As of v0.4.5, the HTTP login → main-screen transition bug is
fixed** (Loader in Main.qml now uses integer literals + explicit
Connections re-trigger, and CacheStore persists `isSpace`,
`childRoomIds`, `threadRootId` so cached Spaces and thread markers
show up immediately after relaunch — no more "briefly hidden until
first /sync" window).

**As of v0.4.6, HTTP `/sync` bring-up is snappy**: initial sync
uses `timeout=0`, `MatrixClient::initialSyncDone()` capability is
exposed to QML, and the room list has a state-aware empty label
(loading / no joined / no in Space). The reported "Syncing forever
with 0 rooms" bug is fixed. Full docs sweep landed.

**As of v0.4.7, HTTP session restore recovers from stale visible-room
cache state**: fresh login clears persisted `syncToken`, initial sync
uses `timeout=0&full_state=true`, and restore discards a stored
`syncToken` if the SQLite cache has no visible non-Space rooms.

**As of v0.4.8, the v0.4.x stabilisation window is closed**:
`CacheStore` no longer emits `NOT NULL constraint failed`
(coerce-to-empty on bind + idempotent NULL repair on schema-ensure),
the `MessageDelegate` no longer prints `QML Column: Cannot specify
… anchors` warnings on every message, and the connection status
says `Connected` once initial sync is done + long-poll is the
steady state.

**As of the current v0.5.0-prep foundation pass, matrix-sdk is linked
and the optional Rust backend has real login/restore/sync/plain text
send plumbing.** E2EE is still deliberately disabled:
`CryptoManager::supportsE2ee()` returns false, encrypted sends are
blocked, and encrypted read/send must be verified against a real
homeserver before the UI may claim support.

**As of v0.5.0-prep+3, the Rust FFI foundation has been hardened**:
- bounded event queue (4096, drop-oldest + one `queue_overflow`
  marker) so a stalled UI thread cannot OOM the process;
- atomic reserve of the sync-running flag inside
  `mx_rust_start_sync`, closing a double-start race;
- undecryptable timeline events are surfaced as
  `undecryptable: true`, empty body, `msgtype: "encrypted"` — C++
  renders them as `[unable to decrypt yet]`; ciphertext is never
  forwarded through the FFI.
Full FFI event schema is documented in `docs/backend-contract.md`.

**As of v0.5.0-prep+4, a headless verification harness ships**:
`--rust-sdk-smoke-test` (Rust-enabled build only). Reads credentials
from env vars, runs under `QCoreApplication`, has a 60 s budget,
prints counts/statuses only. See `docs/build-and-test.md`. The
harness runs Prompt 1 below in a repeatable non-interactive way —
CI-friendly and safe to script into a `.envrc`-guarded shell.

**As of v0.5.0-prep+6, Prompt 1 is verified live**: login + sync +
rooms + Space detection + timeline delivery all work against
`matrix.smetonis.net` (2 rooms, 1 Space, 4 undecryptable events from
a fresh temp SDK store). Encrypted receive diagnostics and an
encrypted-send probe FFI are wired.

**Current follow-up state:** the encrypted-send probe succeeded and
Element Classic displayed the Lightning probe as readable text, so
Lightning → Element Classic encrypted send is verified one-way.
Persistent smoke store/session support now exists via
`LIGHTNING_TEST_PERSISTENT_STORE=1`, using the interactive Rust SDK
store path plus a smoke-only MatrixSession sidecar. Element Classic →
Lightning encrypted receive is still not verified until a persistent
`LIGHTNING_TEST_EXPECT_TEXT` run reports `expect_text=seen`.

Ordering rationale: first harden the Rust backend against the test
homeserver with unencrypted rooms and session restore. Then add
encrypted read/send and flip the E2EE gate only after real encrypted
round trips work.

---

## Prompt 1 — Verify and harden the Rust backend foundation

Continue from `main`. Do not create a branch. Do not refetch crates
unless a real build error proves the committed lockfile/cache is
insufficient.

Start with:

```bash
cd /home/roksme/git/lightning
git status
git pull --ff-only origin main
cd rust
nix develop -c cargo build --release --offline
cd ..
```

Read:

```bash
cat CLAUDE.md 2>/dev/null || true
cat README.md
cat docs/current-state.md
cat docs/backend-contract.md
cat docs/matrix-feature-status.md
cat docs/build-and-test.md
cat docs/architecture.md
cat docs/threat-model.md
```

Task:

1. Build clean default and Rust-enabled trees:
   ```bash
   rm -rf build build-rust
   nix develop -c cmake -S . -B build -G Ninja
   nix develop -c cmake --build build
   nix develop -c cmake -S . -B build-rust -G Ninja -DENABLE_RUST_SDK_BACKEND=ON
   nix develop -c cmake --build build-rust
   ```
2. Run all smoke tests from `docs/build-and-test.md`.
3. Manually test `./build-rust/matrix-client --backend=rust` against
   the disposable homeserver account documented in
   `docs/build-and-test.md`:
   - password login succeeds or fails with a useful Matrix SDK error;
   - session restore works after relaunch;
   - joined room list appears;
   - basic text timeline events appear;
   - unencrypted text send creates local echo then server event id;
   - encrypted room send is blocked;
   - E2EE status remains false.
4. Fix the smallest concrete bugs found. Likely areas:
   - store path when login input is a localpart instead of a full MXID;
   - sync-loop cancellation and restart;
   - duplicate timeline events after local echo replacement;
   - room display names/avatars/unread counts from SDK metadata;
   - useful status/error propagation to C++ without logging tokens or
     message bodies.
5. Keep HTTP and mock untouched except for build break fixes.
6. Update docs with verified manual results.
7. Commit and push:
   ```bash
   git status
   git add .
   git commit -m "Harden Matrix Rust SDK backend foundation"
   git push origin main
   ```

Do not flip `CryptoManager::supportsE2ee()` in this prompt.

## Prompt 2 — Verify persistent encrypted receive, then enable E2EE only if both directions pass

Precondition: persistent Rust smoke store/session support is present
and `LIGHTNING_TEST_PERSISTENT_STORE=1` can restore the same redacted
device id on a second run.

Goal: verify Element Classic → Lightning encrypted receive on a stable
Rust SDK device. Lightning → Element Classic encrypted send is already
verified one-way by the smoke probe; do not enable E2EE until receive
also passes.

Task:

1. Build the Rust-enabled tree.
2. Run a first persistent smoke run:
   ```bash
   LIGHTNING_TEST_HOMESERVER=https://matrix.smetonis.net \
   LIGHTNING_TEST_USER='@test:matrix.smetonis.net' \
   LIGHTNING_TEST_PASSWORD='<password-from-env-only>' \
   LIGHTNING_TEST_PERSISTENT_STORE=1 \
   nix develop -c ./build-rust/matrix-client --backend=rust --rust-sdk-smoke-test
   ```
   Expected: `store=persistent`, `restore=not_available` on a first
   run or `restore=ok` on an existing sidecar, `login=ok`,
   `sync=ok`, `supports_e2ee=false`.
3. Run a second persistent smoke run and confirm it does not hit the
   account/device mismatch. Expected: `store_account_match=yes`,
   `restore=attempted`, `restore=ok`, same redacted `device_id`.
4. In Element Classic, approve the new Lightning login if prompted.
   Send a harmless marker from Element Classic into the encrypted test
   room.
5. Run:
   ```bash
   LIGHTNING_TEST_HOMESERVER=https://matrix.smetonis.net \
   LIGHTNING_TEST_USER='@test:matrix.smetonis.net' \
   LIGHTNING_TEST_PASSWORD='<password-from-env-only>' \
   LIGHTNING_TEST_PERSISTENT_STORE=1 \
   LIGHTNING_TEST_EXPECT_TEXT='<marker>' \
   LIGHTNING_TEST_REQUIRE_EXPECT=1 \
   nix develop -c ./build-rust/matrix-client --backend=rust --rust-sdk-smoke-test
   ```
6. If the result is `expect_text=not_seen`, do not enable E2EE. Record
   counts and investigate key sharing, device verification, room key
   requests, and backup/cross-signing gaps.
7. If the result is `expect_text=seen` and `decrypted_events>0`, rerun
   the encrypted-send probe once more and confirm Element Classic still
   displays the Lightning marker as readable text.
8. Only after both directions pass, define
   `RUST_SDK_E2EE_WIRED` in the Rust-enabled CMake path and let
   `CryptoManager::supportsE2ee()` return true for active backend
   `rust`.
9. Only then consider wiring the interactive UI encrypted send path.
   Keep SAS verification, key backup, cross-signing, and secret-storage
   UX documented as missing unless implemented.
10. Update README and docs. Commit:
   ```bash
   git commit -m "Add initial E2EE support via Matrix Rust SDK"
   ```

Do not add SAS verification, key backup, cross-signing UI, sliding
sync, multi-account, authenticated media, or QML redesign in this pass.

---

## Archived prompt — Space/thread cache persistence landed in v0.4.5

Formerly "Persist Space + thread metadata in CacheStore". Landed in
v0.4.5:

- `CacheStore` schema gained `rooms.is_space`, `rooms.child_room_ids`,
  `events.thread_root_id`, all with `ALTER TABLE ADD COLUMN` guarded
  by `PRAGMA table_info` probes so existing user databases upgrade in
  place.
- `loadRooms` / `saveRoom` / `loadTimeline` / `updateEvent` all round-trip
  the new fields.
- No interface change, no QML change.

The full task text is preserved below for archival reference. It is no
longer a recommended next step.

<details>
<summary>Archived (do not run — already applied)</summary>

Two related quality-of-life follow-ups, both scoped to
`src/storage/CacheStore.{h,cpp}` — no interface change, no QML
change, no backend change.

On relaunch today, `CppHttpMatrixClient::loadCachedState` rebuilds
`m_rooms` and `m_timelines` from SQLite before the first `/sync`
completes. The cache schema pre-dates v0.4.2 Spaces and v0.4.4
threads, so:

- `RoomInfo::isSpace` and `RoomInfo::childRoomIds` are not persisted
  → the Space chip strip is briefly hidden after each relaunch.
- `TimelineEvent::threadRootId` is not persisted → threaded events
  reload as plain messages; the "in thread" chip only reappears
  after the next `/sync` reaches those events.

Task:

1. In `src/storage/CacheStore.{h,cpp}`, extend the schema:
   - `rooms` gets `is_space INTEGER NOT NULL DEFAULT 0` and
     `child_room_ids TEXT NOT NULL DEFAULT ''` (comma-separated).
   - `events` gets `thread_root_id TEXT NOT NULL DEFAULT ''`.
   - Bump the schema version if the file has one; else do
     `ALTER TABLE … ADD COLUMN` with `IF NOT EXISTS` probing so
     existing databases upgrade in place.
2. `saveRoom` / `loadRooms` write and read the new fields.
3. `appendEvent` / `updateEvent` / `loadEvents` write and read
   `thread_root_id`.
4. No new signals; existing `roomsChanged` / `timelineReset` fire
   after the cache warms up, which will trigger `SpaceManager` and
   `TimelineModel` to re-render with the persisted fields.

Verify by:

- Logging in against a real homeserver where you're in a Space and
  a threaded room.
- Quitting and relaunching.
- Chip strip should render *before* the first `/sync` reply arrives.
- Thread chip on the root event should render immediately for
  events already in the cache.

Do NOT touch the interface. Do NOT touch QML. Do NOT change
`MatrixClient` or the models.

</details>

---

## Prompt — Implement Matrix SAS emoji verification UI

Precondition: v0.5.0-prep+10 shipped GUI recovery-key restore for
the Rust backend and Rust E2EE send + receive are verified live.
The Settings panel already says "Session (SAS emoji) verification
UI: not implemented yet".

Goal: let a user complete SAS emoji verification of the Lightning
device from Element Classic without leaving Lightning. Do not
manually implement crypto in C++; use matrix-sdk 0.18 APIs
identified via the locked source at
`~/.cargo/registry/src/index.crates.io-1949cf8c6b5b557f/matrix-sdk-0.18.0/`:

- `client.encryption().recv_verification_requests()` — a Stream of
  incoming verification requests.
- `VerificationRequest::accept()` / `cancel()`.
- Transition to `SasVerification` state machine on accept.
- `SasVerification::accept_with_settings(AcceptSettings)` (choose
  emoji short-authentication-string).
- `emoji()` / `decimals()` — SAS the user must compare.
- `confirm()` when the user says the emoji match, `mismatch()`
  when they don't, `cancel()` for abort.
- Watch state via `changes()` / `state()` stream until Done or
  Cancelled.

Scope:
1. Rust FFI additions: `mx_rust_start_verification_listener`,
   `mx_rust_accept_verification`, `mx_rust_sas_emoji`,
   `mx_rust_sas_confirm`, `mx_rust_sas_cancel`. Never log the
   full SAS emoji list to matrix.rust; emoji themselves are fine
   to print in the UI (SAS design), but the incoming verification
   request's flow id / other user id should not appear in logs.
2. C++ wrapper on `RustSdkMatrixClient`: signals
   `verificationRequestReceived(flowId, otherUserId, otherDeviceId)`,
   `verificationSasReady(flowId, emojiList)`, `verificationDone(flowId)`,
   `verificationCancelled(flowId, reason)`. Methods `acceptVerification(flowId)`,
   `confirmSas(flowId)`, `cancelVerification(flowId)`.
3. QML: a modal `VerificationDialog.qml` in `qml/`. Shows the 7 emoji
   plus their spec-defined English names. "They match" button →
   `confirm`. "They don't match" → `cancel`. Also visible from
   Settings for outgoing verification (secondary path via
   `client.encryption().request_verification()` for the current
   device).
4. Smoke harness envs:
   `LIGHTNING_TEST_SAS_VERIFY=1` — auto-accept incoming request,
   log emoji list on stdout as `smoke: verification_emojis=<comma-sep>`,
   then wait for `LIGHTNING_TEST_SAS_CONFIRM=1` (else `cancel`).
5. Preserve all v0.5.0-prep+9 / +10 invariants:
   `CacheStore` still refuses encrypted rows; recovery-key path
   unchanged; footer/backend label logic unchanged; interactive
   encrypted send remains allowed.

Do NOT wire cross-signing / key backup management UI in the same
pass. Those are separate.

Commit message when done: `Add Rust SDK SAS emoji verification UI`.

---

## Prompt 3 — Multi-account foundation (data model + switcher UI)

Currently `AccountManager` tracks the single active user id. To land
multi-account safely in one pass:

1. Extend `AccountManager` with per-account metadata: `homeserverUrl`,
   `deviceId`, `cachePath`, `secretKeyNamespace`. Persist as a
   QSettings array under `accounts/`. Add:
   - `Q_INVOKABLE void addAccount(userId, homeserver)` (rejects
     duplicates)
   - `Q_INVOKABLE void switchTo(userId)` — currently a no-op that
     emits `accountSwitchRequested(userId)` for `AppController` to
     react to.
   - `Q_INVOKABLE void removeAccount(userId)` — clears the SecretStore
     entry via `SecretStore::clearAccountSecrets(userId)`, wipes the
     account's SQLite cache directory, deletes the QSettings entry.
2. `AppController` grows a `switchAccount(userId)` slot that:
   - `stopSync()` on current client;
   - persists `syncToken` via `SettingsManager::setSyncToken`;
   - reconstructs the MatrixClient for the target account (calling
     `makeClient` with the new user's metadata);
   - reconnects all signals;
   - calls `restoreSession()` then `startSync()`.
3. `SecretStore::readSecret / storeSecret / deleteSecret` already
   accept `userId`; `SettingsManager` already scopes token access by
   userId. What is missing: `cachePath` and `syncToken` per userId.
   Add a QSettings key layout like `accounts/<safeUserId>/syncToken`.
4. Add a toolbar dropdown in `qml/Main.qml`. Bind to
   `app.accounts.knownUserIds`. Selecting an item calls
   `app.accounts.switchTo(userId)`. "Add account" opens the existing
   `LoginScreen` in "add" mode (a new `AppController::Screen` value
   like `AddAccountScreen` is the clean move).

Do not implement per-account `matrix-client-cache/` migration for
existing single-account users — treat the current user as
"default account 0" and namespace only new accounts.

Verify by adding a second account against a different homeserver and
switching between them; both room lists should be independent.

---

## Prompt 4 — SSO login via system browser

Password-only auth is limiting for homeservers that require SSO. Task:

1. `AuthManager::beginSsoLogin(homeserver)`:
   - `GET /_matrix/client/v3/login`. If the response's `flows` array
     contains `{"type": "m.login.sso"}`, continue; else set
     `lastError` to a clean explanation and emit `loginFailed`.
   - Start a `QTcpServer` bound to `127.0.0.1:0`. Note the port.
   - `QDesktopServices::openUrl(homeserver + "/_matrix/client/v3/login/sso/redirect?redirectUrl=http://127.0.0.1:<port>/callback")`.
   - When the browser hits the local socket, parse `loginToken` from
     the query string, respond with a small "You may close this tab"
     HTML.
   - `POST /login` with `type: m.login.token`, receive `access_token`,
     drive the normal login flow.
2. Never use QtWebEngine. The system browser is the correct place for
   an OAuth-style redirect flow.
3. Set `AuthManager::supportsSsoLogin` to true.
4. Add a "Sign in with SSO" button on `LoginScreen`, visible only when
   `app.auth.supportsSsoLogin`.

Do **not** flip `CryptoManager::supportsE2ee`. SSO does not imply E2EE.

---

## Prompt 5 — Authenticated media (`/_matrix/client/v1/media/*`)

Legacy `/_matrix/media/v3/*` still works but is deprecated. Task:

1. In `src/matrix/MediaHelpers.cpp` (or wherever the URL builders
   live), add branching: if the homeserver advertises the
   authenticated endpoints via `GET /_matrix/client/versions`
   (`unstable_features["org.matrix.msc3916"]` or the equivalent
   stable capability), route through `/client/v1/media/download/…`
   and `/client/v1/media/thumbnail/…` with the `Authorization`
   header. Else keep legacy.
2. Cache the capability probe per homeserver in `SettingsManager`
   (non-secret data).
3. Verify with a homeserver that has authenticated media enabled
   (Synapse ≥ 1.100 with the `enable_authenticated_media` config).

---

## Non-goals for the next few passes

- Do not implement Olm/Megolm in C++.
- Do not add QtWebEngine.
- Do not implement a full Space hierarchy editor.
- Do not implement power-level management.
- Do not add per-message signatures / mentions parsing beyond what the
  UI already renders.
- Do not add Windows / macOS SecretStore backends until Linux path is
  stable.

Each of these has a discrete follow-up prompt to be written once the
prompts above land.
