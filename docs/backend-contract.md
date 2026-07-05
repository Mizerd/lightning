# Backend contract

`src/matrix/MatrixClient.h` is the swap seam. UI, models, `AppController`
and everything above them depend only on this interface — never on a
concrete backend. This document describes exactly what a concrete
backend must do to slot in cleanly.

## Rules for any `MatrixClient` implementation

1. **Never block.** Every method returns immediately; work happens on
   internal queues / threads / QNetworkAccessManager callbacks, results
   arrive via signals.
2. **Signal-first state.** `AppController` reacts to `loginSucceeded`,
   `loggedOut`, `connectionStateChanged`, `roomsChanged`, `roomUpdated`,
   `timelineReset`, `eventAppended`, `eventReplaced`, `eventEdited`,
   `eventRedacted`, `reactionsChanged`, `eventsPrepended`,
   `paginationStateChanged`, `typingChanged`, `membersChanged`,
   `errorOccurred`. Do not rely on method return values to communicate
   state changes to the UI.
3. **Ordering.** Emit `loginSucceeded(userId)` before touching the room
   set. `AppController::onLoginSucceeded` calls `startSync()` — do not
   auto-start sync inside `login()`.
4. **Do not lie about E2EE.** If your backend does not decrypt
   `m.room.encrypted`, report the event as a placeholder (see how
   `CppHttpMatrixClient` renders them) and reject sends into encrypted
   rooms with `errorOccurred`. `CryptoManager::supportsE2ee` is the
   single source of truth for the UI badge — do not attempt to override
   it from a backend.
5. **Session lifecycle.** `restoreSession()` may return true (a session
   was restored, expect a `/whoami`-like verification later) or false.
   `logout()` must eventually emit `loggedOut` even if the network call
   fails.
6. **Media URLs.** `mediaDownloadUrl` / `mediaThumbnailUrl` return
   `QUrl`s the UI can hand to `QImage` / `QDesktopServices`. If the
   backend has no media pipeline yet, return `QUrl()` — the UI already
   handles that.

## Send / edit / redact / react

All are fire-and-forget. On success:

- `sendTextMessage` / `sendReply` / `sendImage` / `sendFile`:
  emit `eventAppended` with a local echo (`eventId = "local:<txn>"`,
  `status = Sending`) *immediately*. When the server confirms, emit
  `eventReplaced` with the real `event_id` OR `eventStatusChanged`
  flipping the row to `Sent`. Never leave a "Sending…" row behind.
- `editMessage`: emit `eventEdited` after the server acks.
- `redactEvent`: emit `eventRedacted` after the server acks.
- `toggleReaction`: emit `reactionsChanged`.

## Thread replies (v0.4.1 / v0.4.4)

`MatrixClient::sendThreadReply` is a *non-pure* virtual:

```cpp
virtual void sendThreadReply(const QString &roomId,
                             const QString &threadRootEventId,
                             const QString &body)
{
    sendReply(roomId, threadRootEventId, body);
}
```

- **Mock** overrides to preserve `TimelineEvent::threadRootId` so
  `ThreadManager` and QML can see the thread grouping.
- **HTTP (v0.4.4)** overrides with a real `m.thread` relation:
  ```json
  { "m.relates_to": {
      "rel_type": "m.thread",
      "event_id": "$root",
      "is_falling_back": true,
      "m.in_reply_to": { "event_id": "$latest-or-root" }
  } }
  ```
  Local echo carries `threadRootId` (not `replyToEventId`) so QML
  renders the "in thread" chip immediately. The interface's txnId
  dedup + `eventReplaced` path handles the /sync round-trip
  unchanged. `processTimelineEvent` and the `/messages` pagination
  path both set `TimelineEvent::threadRootId` from
  `rel_type == "m.thread"`; when a thread event also carries a
  fallback `m.in_reply_to`, `replyToEventId` is cleared to avoid
  double-decoration in QML.
- **Rust** currently refuses with `errorOccurred` — same as every other
  send.

If you add server-side thread aggregation (unsigned["m.relations"]
["m.thread"] latest_event / count), do it inside
`CppHttpMatrixClient::processTimelineEvent` — no interface change
needed. Do not touch `sendReply` for thread work.

## Signal cheat sheet

| Signal | When to emit |
|---|---|
| `loginSucceeded(userId)` | After a POST /login (or /whoami restore) returns success. |
| `loginFailed(reason)` | Reasons should be user-facing strings; the UI shows them verbatim in Settings + Login. |
| `loggedOut()` | Fires exactly once per session teardown. |
| `connectionStateChanged(state)` | `Disconnected`, `Connecting`, `Syncing`, `Error`. |
| `roomsChanged()` | Full room set or ordering may have changed. |
| `roomUpdated(roomId)` | Single-room summary update (name, last preview, unread, encrypted flag, isSpace, childRoomIds). |
| `timelineReset(roomId)` | Room's timeline was replaced (e.g. on sync-since re-run). |
| `eventAppended(roomId, event)` | New event at the end of a room's timeline. Includes local echoes. |
| `eventReplaced(roomId, oldEventId, newEvent)` | Local echo (`local:<txn>`) resolved to a real event id. |
| `eventStatusChanged(roomId, eventId, status)` | `Sending` → `Sent` or `Failed` for local echoes when server response is empty. |
| `eventEdited(roomId, eventId)` | Body of that event has been replaced (`m.replace`). |
| `eventRedacted(roomId, eventId)` | Event was redacted. Timeline should render `[message deleted]`. |
| `reactionsChanged(roomId, eventId)` | Reactions bucket for a target event changed. |
| `eventsPrepended(roomId, events)` | Backfill pagination result. |
| `paginationStateChanged(roomId)` | `canPaginate` / `paginating` might have changed. |
| `typingChanged(roomId)` | `typingUsersFor(roomId)` returns different data now. |
| `membersChanged(roomId)` | `displayNameFor` / `avatarMxcFor` might return different data now. |
| `errorOccurred(message)` | User-facing error. AppController relays to `errorReported` and QML surfaces it in the status bar. |

## Spaces contract (v0.4.1 / v0.4.2)

`RoomInfo` carries Space membership:

- `isSpace == true` for a `m.room.create` with `type: m.space`. Space
  rooms are visible via `MatrixClient::rooms()` but the QML room list
  filters them out — they render as chips in `RoomListPane`.
- `childRoomIds` on a Space lists the room ids it contains (from
  `m.space.child` state events).
- `spaceId` on a normal room is a *primary parent* hint — Matrix
  actually allows a room in multiple Spaces, but the app treats
  `spaceId` as informational only. `SpaceManager` computes membership
  strictly from `childRoomIds`.

Concrete backends must populate `isSpace` and `childRoomIds` from
whatever sync data they see:

- **Mock (v0.4.1)** hardcodes one Space (`Team`) containing `General`
  and `Developers`; `dm-bob` stays outside.
- **HTTP (v0.4.2)** parses `m.room.create` (`content.type == "m.space"`)
  and `m.space.child` (state_key = child room id, `via[]` non-empty =
  active edge, empty = removed) inside
  `CppHttpMatrixClient::processStateEvent`. Both events are picked up
  from `rooms.join[roomId].state.events` **and** from timeline state
  events — Matrix delivers them in either bucket.
- **Rust** does not yet talk to a server.

If a backend adds or removes a Space edge later, emit
`roomUpdated(roomId)` on the Space room; `SpaceManager` rebuilds and
QML picks up the change automatically.

Robustness: `SpaceManager::rebuild` skips child room ids that are not
present in `MatrixClient::rooms()`. Backends do not need to filter
out unjoined children before returning them.

Cache persistence (v0.4.5): `CacheStore` persists both
`RoomInfo::isSpace` and `RoomInfo::childRoomIds` (comma-joined,
Matrix room ids never contain commas) as well as
`TimelineEvent::threadRootId`. Backends should populate these on
their `RoomInfo` / `TimelineEvent` instances and call the usual
`saveRoom` / `updateEvent` cache helpers; the schema migrates in
place via `ALTER TABLE ADD COLUMN` for pre-v0.4.5 databases.

## Rust SDK backend (v0.5.0-prep foundation)

`RustSdkMatrixClient` is compiled iff `ENABLE_RUST_SDK_BACKEND=ON`.
It is the C++ `QObject` backend wrapper; QML never calls Rust. Rust
owns the Matrix SDK client, async work, SDK SQLite store, and a JSON
event queue drained by C++ on a short `QTimer`.

Current Rust backend scope:

- password login through matrix-sdk;
- session restore from `SettingsManager`/`SecretStore` access token;
- joined-room sync;
- room-list events;
- basic text/notice/emote timeline events;
- plain text sends into unencrypted rooms.

Current Rust backend non-scope:

- no E2EE support claim yet;
- encrypted sends are blocked;
- pagination, replies, edits, redactions, reactions, media, typing,
  read receipts, Space child hierarchy, key backup, and verification
  are not wired through Rust yet.

These rules must hold:

1. **Crypto store isolation.** The SDK owns its own SQLite (or
   sled) store at `${XDG_DATA_HOME}/matrix-client/<safeUserId>/matrix-rust-sdk-store/`.
   That directory is per-account and disjoint from the C++
   `CacheStore` (`cache.sqlite`). Access tokens are still owned by
   `SecretStore`, never by either SQLite file. `--reset-crypto-store`
   removes only the Rust SDK directory; it never touches
   `cache.sqlite` or the SecretStore.
2. **Thread model.** The Rust side spins up a Tokio runtime on its
   own thread. C++ never blocks on Rust. Prefer a queue-and-poll
   pattern: Rust enqueues serialised event JSON, C++ drains via a
   short `QTimer` on the main thread. No cross-thread callbacks.
3. **Panic isolation.** FFI entry points should convert recoverable
   failures into error strings or queued error events. Do not rely on
   C++ to recover from Rust panics.
4. **Encrypted send remains blocked until verified.** The composer may
   allow encrypted sends only after the Rust backend has real encrypted
   read/send and `CryptoManager::supportsE2ee()` returns true.
5. **Historical undecryptable messages still render placeholders.**
   Cross-signing, key backup, and secret storage are NOT scope for
   the initial matrix-sdk landing. Messages from before the
   current device joined a room may never decrypt on this device
   until key backup lands. QML shows `[unable to decrypt]` for
   those, distinct from the current
   `[encrypted message - E2EE not implemented yet]`.
6. **`CryptoManager::supportsE2ee` gate.** The compile-time gate
   `RUST_SDK_E2EE_WIRED` is defined only by the CMake path that
   has verified encrypted read/send. When both
   `ENABLE_RUST_SDK_BACKEND` and `RUST_SDK_E2EE_WIRED` are defined
   AND the active backend is `rust`, `supportsE2ee` returns true. Any
   other combination returns false. Do not add a runtime override that
   flips this to true without recompiling.

## Initial sync capability (v0.4.6)

`MatrixClient::initialSyncDone()` is a *non-pure* virtual that
defaults to `true`. Backends that synthesise state immediately
(the Mock backend has all its rooms at construction time) can
leave the default. Backends that need to talk to a homeserver
before rooms are known (`CppHttpMatrixClient` — and later the Rust
SDK backend) must override:

- Return `false` after login / restoreSession, before the first
  `/sync` response is parsed.
- Flip to `true` when the first response is fully processed, even
  if the response contained zero rooms.
- Reset to `false` on `clearLocalSession` / logout.
- Emit `initialSyncDoneChanged()` on every transition.

`AppController::initialSyncDone` re-exposes this to QML so the
room list can say "Loading rooms…" while it's `false` and switch
to a proper empty state (or the list itself) after.

## Login lifecycle & the screen transition

`AppController::onLoginSucceeded` is the single point that flips
`currentScreen` from Login to Main and starts sync. It is connected
to `AuthManager::loginSucceeded`, which is a re-emit of
`MatrixClient::loginSucceeded(userId)` after `AuthManager` clears its
own "logging in" state. Any backend that emits `loginSucceeded`
should have `isLoggedIn()` return `true` **before** the emit, since
`AppController::onLoginSucceeded` may inspect it during startSync
routing (v0.4.5 makes the transition robust with an explicit
`Connections` re-trigger in QML, but keeping this invariant simple
saves debugging time).

## What backends do NOT own

- Screen navigation, current room, current screen.
- Composer draft state.
- Room ordering rules (the model can sort).
- Notification presentation.
- Space filter state (`SpaceManager::activeSpaceId`).
- Which SecretStore backend is used.

## Multi-account contract (documented; not implemented yet)

When multi-account lands (see `docs/next-prompts.md` for the exact
prompt), a backend instance is bound to *one* Matrix account. Switching
account means:

1. `stopSync()` on the current client.
2. Persist the current sync token via `SettingsManager::setSyncToken`.
3. Swap the `MatrixClient*` inside `AppController` for the target
   account's instance (or reconstruct it), reconnecting all signals.
4. `restoreSession()` on the new client, then `startSync()`.

Backends must therefore not hold static / global state that assumes a
single account. `CppHttpMatrixClient` uses a per-instance
`QNetworkAccessManager`, which is fine.
