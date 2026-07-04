# Architecture

The client is layered so the Matrix backend is a swappable dependency of the UI
and never the other way around.

```
┌──────────────────────────────────────────────────────┐
│  UI Layer — QML                                      │  qml/*.qml
│  Login • RoomList • Timeline • Composer • Settings   │
└─────────────────────┬────────────────────────────────┘
                      │ Q_PROPERTY / Q_INVOKABLE / signals
┌─────────────────────▼────────────────────────────────┐
│  Application Layer — C++                             │  src/app, src/auth
│  AppController • AuthManager • AccountManager        │
│  SettingsManager                                     │
└─────────────────────┬────────────────────────────────┘
                      │
┌─────────────────────▼────────────────────────────────┐
│  UI Models — C++                                     │  src/models
│  RoomListModel • TimelineModel • MessageComposer     │
│  (QAbstractListModel subscribers to MatrixClient)    │
└─────────────────────┬────────────────────────────────┘
                      │
┌─────────────────────▼────────────────────────────────┐
│  MatrixClient Interface — pure C++ (QObject)         │  src/matrix
└──┬──────────────────┬──────────────────────┬─────────┘
   │                  │                      │
┌──▼──────────┐   ┌───▼──────────────┐   ┌───▼───────────────────────┐
│ Mock (v0.1) │   │ CppHttp (v0.2/3) │   │ RustSdk scaffold (v0.4)   │
│ --backend=  │   │ --backend=http   │   │ --backend=rust            │
│   mock      │   │  (default)       │   │ (requires -DENABLE_RUST_  │
│ (alias:     │   │                  │   │   SDK_BACKEND=ON)         │
│  --mock)    │   │                  │   │                           │
└─────────────┘   └──────────────────┘   └───────────────────────────┘

Storage layer (v0.4):
  QSettings                      – prefs + non-secret session metadata
                                   (homeserver, userId, deviceId, syncToken)
  SecretStore                    – access tokens (libsecret when reachable;
                                   InsecureFallbackSecretStore otherwise
                                   with a visible warning)
  CacheStore (SQLite)            – rooms + last N events + members
                                   at ${XDG_DATA_HOME}/matrix-client/<userId>/

Cross-cutting:
  Storage Layer (QSettings + SecretStore + SQLite)
  Platform Layer (NotificationManager, MediaManager, tray, keychain)
  Crypto Layer (interface + honest capability surface;
                 real crypto lands in v0.4.x when the Rust SDK is wired)
```

## Ownership and lifetimes

- `main.cpp` owns a single `AppController` on the stack.
- `AppController` owns every other subsystem via `std::unique_ptr` so
  destruction is deterministic and testable.
- `AuthManager` and the UI models hold a raw pointer to the current
  `MatrixClient`. They do not own it. Swapping backends means replacing the
  pointer and reconnecting signals — no code above that seam knows the
  concrete backend class.

## Signal flow

1. QML calls `app.auth.login(...)`.
2. `AuthManager` forwards to `MatrixClient::login`.
3. Mock backend emits `loginSucceeded` → `AuthManager` re-emits →
   `AppController::onLoginSucceeded` starts sync and switches to `MainScreen`.
4. Mock backend emits `roomsChanged` / `timelineReset` / `eventAppended`.
5. `RoomListModel` and `TimelineModel` update their `QAbstractListModel` rows.
6. QML `ListView`s repaint.

## Backend swap contract

Any concrete `MatrixClient` implementation must:

- Emit `loginSucceeded(userId)` after a successful auth. `AppController`
  uses that signal — not a return value — to transition screens.
- Emit `roomsChanged()` whenever the room set or ordering changes.
- Emit `roomUpdated(roomId)` for in-place updates to a single room summary.
- Emit `eventAppended(roomId, event)` for each new timeline event and
  `eventStatusChanged(roomId, eventId, status)` for local echoes / retries.
- Never block. Every method returns immediately; work happens on internal
  queues/threads and results arrive via signals.

## What lives above the seam vs below

| Concern | Above (C++/QML) | Below (`MatrixClient` impl) |
| --- | --- | --- |
| Screen navigation | ✅ | ❌ |
| Draft state, composer UX | ✅ | ❌ |
| Room ordering rules | ✅ (sort in model) | delivers unsorted list |
| Sync protocol details | ❌ | ✅ |
| E2EE key material | ❌ | ✅ (v0.4 via Rust SDK) |
| Media transport | ❌ | ✅ |
| Notification presentation | ✅ | delivers events |

## What is intentionally *not* done in v0.1

- No network. Only the `MockMatrixClient` is compiled.
- No secure storage. Tokens go through `QSettings`. Settings screen warns.
- No crypto. `CryptoManager::supportsE2ee()` returns `false` and QML shows
  a warning banner in Settings.
- No thread/space UI beyond the interface stubs.

## What is intentionally *not* done in v0.2

- No crypto. `m.room.encrypted` events render as `[encrypted message - E2EE not implemented yet]`. Sends into encrypted rooms are blocked with a clear error. `CryptoManager::supportsE2ee()` still returns `false`. Real E2EE ships in v0.4 via the Matrix Rust SDK.
- No secure storage. `SettingsManager::saveSession` writes `access_token` / `user_id` / `device_id` / `sync_token` to QSettings in plaintext. The Settings screen warns. Keychain integration ships in v0.4.
- No profile lookup. Sender display names default to the MXID.
- No media, replies, edits, redactions, reactions, receipts, typing indicators, mentions, or pagination. All ship in v0.3.
- No SSO / OIDC, spaces, threads, multi-account, sliding sync. All ship in v0.5.

## Send-echo dedup contract (v0.3)

`CppHttpMatrixClient` appends a local `TimelineEvent` with `eventId = "local:<txnId>"` and status `Sending` when the composer calls `sendTextMessage()` / `sendReply()` / `sendImage()` / `sendFile()`. On PUT success:
- If the response includes an `event_id`, the local echo's `eventId` is upgraded to that value and `eventReplaced(roomId, oldId, newEvent)` is emitted so `TimelineModel` can rewrite the row in place. `m_pendingSends[txnId]` is updated with the new id so `/sync`-side dedup keeps working.
- If the response is empty (older homeservers), only status flips to `Sent` and `eventStatusChanged` is emitted.

`/sync`-side dedup: any event carrying `unsigned.transaction_id` that matches a live entry in `m_pendingSends` is dropped, and the pending entry is removed.

This is a real fix for the v0.2 "eventId frozen at `local:*` forever" limitation. Edits and redactions of just-sent messages now target the real `event_id` because the model row was updated in place.

## v0.3 signal set

`MatrixClient` adds these signals on top of v0.2's:

- `eventReplaced(roomId, oldEventId, newEvent)` — local echo → real event.
- `eventEdited(roomId, eventId)` — target's body was replaced via `m.replace`.
- `eventRedacted(roomId, eventId)` — target was redacted.
- `reactionsChanged(roomId, eventId)` — target's reactions list changed.
- `eventsPrepended(roomId, events)` — pagination backfill.
- `paginationStateChanged(roomId)` — `canPaginate` / `paginating` might have changed.
- `typingChanged(roomId)` — typing user set changed.
- `membersChanged(roomId)` — a member's display name / avatar changed.

`TimelineModel` maps each to model updates (`dataChanged` / `beginInsert…End` / etc.).

## v0.4 additions

### Backend selection

`main.cpp` parses `--backend=` (values: `mock`, `http`, `rust`) plus the
`--mock` alias. `AppController::isBackendCompiled(Backend)` reports whether
this build actually contains the requested backend — `--backend=rust`
without `-DENABLE_RUST_SDK_BACKEND=ON` exits 2 with a clean message before
any window opens.

### SecretStore seam

```
AppController
  └── SecretStore (SecretStore::createDefault(this))
        ├── LibSecretStore              (Freedesktop Secret Service)
        └── InsecureFallbackSecretStore (QSettings under 'secrets/*')

  └── SettingsManager
        └── setSecretStore(SecretStore*)  ← wired before makeClient()
              ├── migratePlaintextTokenIfPresent()  (v0.2/v0.3 → v0.4)
              └── accessToken() / saveSession() / clearSession() route
                  the token through the SecretStore
```

The factory picks libsecret when both build-time (`HAVE_LIBSECRET`) and
runtime (`secret_service_get_sync`) succeed, else the insecure fallback.
The Settings screen reads `settings.secretsAreSecure` and shows a red
warning when the fallback is active.

### Rust backend seam

`RustSdkMatrixClient` (in `src/matrix/`, compiled only when
`ENABLE_RUST_SDK_BACKEND` is defined) implements `MatrixClient` and links
against the `matrix_client_rust` static library from the `rust/` crate.
The Rust surface today is intentionally tiny (`mx_rust_backend_name`,
`mx_rust_status_string`, `mx_rust_supports_e2ee`, `mx_rust_version`,
`mx_rust_free_cstring`). Every `MatrixClient` write op emits
`errorOccurred` or `loginFailed` with an honest reason — nothing
pretends E2EE works.

### CryptoManager as capability surface

`CryptoManager::supportsE2ee()` returns `true` only when both:
1. `ENABLE_RUST_SDK_BACKEND` was defined at build time, AND
2. `RUST_SDK_E2EE_WIRED` is defined (added later when the SDK is wired),
AND
3. the active backend is `"rust"`.

Every other layer defers to this single source of truth for the UI.

