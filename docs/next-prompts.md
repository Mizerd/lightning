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

The next safest single-feature step is Prompt 2 (multi-account
foundation). Everything above the multi-account bar (SSO,
matrix-sdk, authenticated media) is materially bigger and depends
on account scoping being clean.

---

## Prompt 1 (superseded — landed in v0.4.5)

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
longer the recommended next step — jump to Prompt 2.

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

## Prompt 2 — Multi-account foundation (data model + switcher UI)

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

## Prompt 3 — SSO login via system browser

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

## Prompt 4 — Small Rust step: wire matrix-sdk `Client::builder`

The Rust scaffold currently has no `matrix-sdk` dependency. Trying to
pull the whole SDK in one pass is unstable. Small step:

1. In `rust/Cargo.toml`, add `matrix-sdk = { version = "…", default-features = false, features = ["rustls-tls"] }`. Use the latest that builds in Nix cleanly.
2. In `rust/src/lib.rs`, add:
   - `mx_rust_login(homeserver: *const c_char, user: *const c_char, password: *const c_char) -> *mut c_char` — build a `Client`, log in, return the resulting `user_id` as a heap string. On failure, return an error string prefixed with `"error: "`. Do not panic — catch every `Result` and translate.
3. Extend `rust/include/matrix_rust.h`.
4. In `src/matrix/RustSdkMatrixClient.cpp`, call `mx_rust_login` from
   `login()`. Emit `loginSucceeded` on success, `loginFailed` with the
   error string otherwise. Do not emit `loggedOut` on failure.
5. Only if all of the above compiles and passes smoke tests in **both**
   `nix develop` build modes, commit. If the matrix-sdk build fails in
   Nix (missing OpenSSL headers, git deps, etc), stop and document
   what you learned — do not commit a half-wired FFI.

Do **not** flip `CryptoManager::supportsE2ee` or define
`RUST_SDK_E2EE_WIRED` in this step — login alone is not encryption.

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
