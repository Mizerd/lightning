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

## Thread replies (v0.4.1)

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
- **HTTP** currently uses the default. Real `m.thread` relation content
  (rel_type + is_falling_back) is a v0.5 follow-up documented in
  `docs/next-prompts.md`.
- **Rust** currently refuses with `errorOccurred` — same as every other
  send.

If you add real `m.thread` to `CppHttpMatrixClient`, override this
method; do not touch `sendReply`.

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

## Spaces contract (v0.4.1)

`RoomInfo` carries Space membership:

- `isSpace == true` for a `m.room.create` with `type: m.space`. Space
  rooms are visible via `MatrixClient::rooms()` but the QML room list
  filters them out — they render as chips in `RoomListPane`.
- `childRoomIds` on a Space lists the room ids it contains (from
  `m.space.child` state events).
- `spaceId` on a normal room is a *primary parent* hint — Matrix
  actually allows a room in multiple spaces, but the app treats
  `spaceId` as informational only. `SpaceManager` computes membership
  strictly from `childRoomIds`.

The mock backend seeds one Space (`Team`) containing `General` and
`Developers`. `dm-bob` stays outside, so QML renders an "Other rooms"
row. HTTP backend does not yet parse these — see
`docs/next-prompts.md`.

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
