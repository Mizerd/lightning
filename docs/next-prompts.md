# Next prompts

Pre-written prompts a future session can copy verbatim. Each is
scoped small enough that one pass should keep the build green.

Read `docs/current-state.md`, `docs/backend-contract.md`, and
`docs/matrix-feature-status.md` **before** running any of these.

---

## Prompt 1 — Parse Spaces from the HTTP backend

Right now Spaces work end-to-end on Mock only. HTTP does not read
`m.room.create` `type` and does not read `m.space.child` state events
from `/sync`. Task:

1. In `src/matrix/CppHttpMatrixClient.cpp`:
   - When processing `m.room.create`, if `content.type == "m.space"`,
     set `RoomInfo::isSpace = true`.
   - When processing state events, watch for `m.space.child` events.
     The state-key is the child room id. If content has `via` (a
     non-empty array), treat the child as present. If content is `{}`
     the child was removed — pop it from `childRoomIds`.
   - Keep `RoomInfo::spaceId` in sync with the first Space that lists
     the room as a child, as a "primary parent" hint.
2. Emit `roomUpdated(roomId)` for every affected room so
   `SpaceManager::rebuild` runs.
3. On first sync, seed `childRoomIds` from any accumulated state, then
   emit `roomsChanged()`.
4. No QML change needed — `RoomListPane` will surface the chip strip
   automatically once `SpaceManager::hasSpaces` becomes true.

Verify with a real homeserver that has at least one Space you're in.
Do not touch the mock; do not touch the interface.

---

## Prompt 2 — Real `m.thread` relation for HTTP sends

`MessageComposer::send()` routes thread replies to
`MatrixClient::sendThreadReply`, which the interface defaults to
`sendReply`. HTTP still emits `m.in_reply_to`. Task:

1. Override `sendThreadReply` in `CppHttpMatrixClient`.
2. Content should be `msgtype: m.text`, `body`, plus:
   ```json
   "m.relates_to": {
     "rel_type": "m.thread",
     "event_id": "<threadRootEventId>",
     "is_falling_back": true,
     "m.in_reply_to": { "event_id": "<lastEventInThread>" }
   }
   ```
   Use the newest event in the local timeline whose `threadRootId`
   matches, else the root itself, as `<lastEventInThread>`.
3. Parse incoming events with `m.relates_to.rel_type == "m.thread"`
   and set `TimelineEvent::threadRootId`. Preserve the existing reply
   parsing (`m.in_reply_to`) — a thread reply *also* carries an in-reply
   for backwards compatibility; the presence of `rel_type == "m.thread"`
   is what makes it a thread event.
4. Do not change the interface.

Verify against a homeserver that supports threads (matrix.org does).

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

## Prompt 5 — Small Rust step: wire matrix-sdk `Client::builder`

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

## Prompt 6 — Authenticated media (`/_matrix/client/v1/media/*`)

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
