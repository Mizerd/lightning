# Matrix feature status (v0.5.0-prep+3)

Honest per-feature status per backend. Ground truth for anything the UI
claims about support. Do not check anything as "done" here that is not
actually done in code.

Legend:

- ✅ Implemented and used in production paths.
- 🟡 Partial — code path exists but is limited (documented after the
  table).
- ⏳ Stubbed / placeholder in code, not usable end-to-end.
- ❌ Not present.

| Feature | Mock | HTTP | Rust SDK |
|---|---|---|---|
| Password login (`m.login.password`) | ✅ (any creds) | ✅ | 🟡 wired via SDK; needs manual homeserver verification |
| Session persistence + restore | 🟡 in-mem | ✅ (`/whoami`) | 🟡 restore via SDK session token; needs manual verification |
| Long-poll `/sync` | ⏳ | ✅ (v0.4.7: initial call uses timeout=0 + full_state; stale token is discarded if cache has no visible rooms) | 🟡 joined-room SDK sync wired |
| Initial-sync UX (`initialSyncDone` capability) | n/a | ✅ (v0.4.6) | ✅ flips after first SDK sync callback |
| Text message send / receive | ✅ | ✅ | 🟡 basic text/notice/emote only; unencrypted send only |
| Backfill pagination | ✅ | ✅ | ❌ |
| Read receipts (self → server) | ✅ | ✅ | ❌ |
| Typing indicator (send + display) | ✅ | ✅ | ❌ |
| Replies (`m.in_reply_to`) | ✅ | ✅ | ❌ |
| Edits (`m.replace`) | ✅ | ✅ | ❌ |
| Redactions | ✅ | ✅ | ❌ |
| Reactions (`m.reaction` / `m.annotation`) | ✅ | ✅ | ❌ |
| Media send (image/file, legacy `/media/v3/upload`) | 🟡 no-op | ✅ | ❌ |
| Media receive (image/file, legacy `/media/v3/download`) | ✅ | ✅ | ❌ |
| Authenticated media (`/client/v1/media/*`) | ❌ | ❌ | ❌ |
| Local SQLite cache | n/a | ✅ | 🟡 Rust SDK store only; no C++ CacheStore timeline cache |
| **Spaces — recognise `m.room.create type:m.space`** | ✅ seeded | ✅ (v0.4.2) | 🟡 SDK `room.is_space()` surfaced |
| **Spaces — `m.space.child` hierarchy** | ✅ seeded | ✅ (v0.4.2) | ❌ |
| **Spaces — filter room list by active Space** | ✅ (chip strip) | ✅ (v0.4.2) | 🟡 works only for surfaced Space flags/children |
| **Spaces — persistence across restart (before /sync)** | n/a | ✅ (v0.4.5) | ❌ |
| **Threads — detect `m.thread` relation** | ✅ seeded | ✅ (v0.4.4) | ❌ |
| **Threads — compose thread reply** | ✅ (Mock preserves grouping) | ✅ (v0.4.4, real m.thread relation) | ❌ refuses |
| **Threads — indicator on root** | ✅ | ✅ (v0.4.4, locally computed count) | ❌ |
| **Threads — server-side aggregation (`unsigned["m.relations"]["m.thread"]`)** | n/a | ❌ (v0.5) | ❌ |
| **Threads — dedicated thread panel / per-thread timeline model** | ❌ | ❌ (v0.5+) | ❌ |
| **Threads — persistence across restart (before /sync)** | n/a | ✅ (v0.4.5) | ❌ |
| Encrypted room read | ❌ placeholder | ❌ placeholder | ❌ not claimed; only SDK-decrypted text events would be shown |
| Encrypted send | ❌ blocked | ❌ blocked | ❌ blocked |
| Device verification / cross-signing | ❌ | ❌ | ❌ |
| Encrypted media | ❌ placeholder | ❌ placeholder | ❌ |
| SSO login | ❌ capability flag `false` | ❌ capability flag `false` | ❌ |
| OIDC / MAS login | ❌ capability flag `false` | ❌ capability flag `false` | ❌ |
| Multi-account switching | ❌ single-active | ❌ single-active | ❌ |
| Sliding sync | ❌ | ❌ | ❌ |
| Notifications (native / tray) | ❌ (log only) | ❌ (log only) | ❌ (log only) |

## Partial-status notes

- **Mock — session persistence**: the mock user id is set on login and
  cleared on logout, but nothing is written to `SettingsManager` — this
  is by design (Mock has no server-side state to restore).
- **Mock — sync**: `startSync()` just emits `roomsChanged` and
  `timelineReset` from the seeded data; no timers, no network.
- **Mock — media send**: `sendImage` / `sendFile` are wired to the
  `MediaManager` on the C++ side; they append a synthetic Sending event
  but do not actually deliver a payload (no bytes are uploaded).
- **HTTP — threads (v0.4.4)**: `CppHttpMatrixClient` now overrides
  `sendThreadReply` and emits a real `m.thread` relation. Content
  shape:

  ```json
  { "msgtype": "m.text", "body": "…",
    "m.relates_to": {
      "rel_type": "m.thread",
      "event_id": "$root",
      "is_falling_back": true,
      "m.in_reply_to": { "event_id": "$latest-or-root" }
    } }
  ```

  `is_falling_back: true` + `m.in_reply_to` makes non-thread-aware
  clients (older Element, matrix-commander, etc.) still render the
  message as a normal reply chain. The fallback target is the newest
  server-confirmed event whose local `threadRootId` matches the root;
  if none is loaded yet, we use the root itself.

  Incoming events: both `processTimelineEvent` (live `/sync`) and the
  `/messages?dir=b` backfill path recognise `rel_type == "m.thread"`
  and populate `TimelineEvent::threadRootId`. When such an event also
  carries `m.in_reply_to` (the spec fallback), `replyToEventId` is
  cleared so QML doesn't stack the "in thread" chip and the reply
  preview strip.

  Reply count: `TimelineModel` still computes counts locally by
  scanning the loaded timeline (v0.4.1 behaviour). Server-side
  aggregation (`unsigned["m.relations"]["m.thread"]`) is a v0.5
  follow-up documented in `docs/next-prompts.md`.

  Encrypted rooms remain blocked at the composer boundary — thread
  replies into encrypted rooms are refused with a clear error until
  E2EE arrives via Rust SDK.
- **HTTP — Spaces from sync (v0.4.2)**:
  `CppHttpMatrixClient::processStateEvent` handles two additional
  types:
  - `m.room.create` sets `RoomInfo.isSpace = (content.type == "m.space")`.
  - `m.space.child` uses the `state_key` as the child room id, an
    active edge is a non-empty `content.via` array, an empty content /
    missing via removes the edge from `RoomInfo.childRoomIds`.
  These are processed in both `rooms.join[roomId].state.events` and in
  timeline state events (Matrix delivers Space edges in either
  bucket). `RoomInfo.spaceId` is intentionally not set on children —
  SpaceManager builds membership from `Space.childRoomIds`, which
  handles the multi-parent case correctly.
- **HTTP — Spaces persistence**: `src/storage/CacheStore.cpp` does not
  yet serialise `isSpace` / `childRoomIds`. On session restore rooms
  come back from SQLite without their Space flags, so the chip strip
  stays hidden until the first `/sync` reply arrives (usually seconds).
  Extending the schema is the top follow-up if this becomes annoying;
  see `docs/next-prompts.md`.
- **HTTP — child rooms you're not joined to**: `SpaceManager::rebuild`
  skips any child room id that is not present in the current
  `MatrixClient::rooms()` result (see the `byId.constFind` guard). So
  if a Space references rooms you haven't joined, they simply do not
  appear in the filtered list — the app never crashes on a dangling
  reference.
- **Rust SDK backend**: foundation is wired, but still narrower than
  HTTP. Rust owns the SDK client/runtime/store and C++ polls JSON
  events. Login/restore/sync/plain text send are implemented, but not
  manually verified against credentials in this pass. It does not use
  the C++ `CacheStore`, does not implement pagination/rich relations,
  and still blocks encrypted sends. E2EE remains unsupported until
  encrypted read and encrypted send are both verified end to end.
- **Rust SDK — v0.5.0-prep+3 hardening.** Three foundation bugs were
  fixed in this pass, none of them changing the surface API:
  1. The Rust → C++ event queue is now bounded at 4096 entries. On
     overflow the oldest entries are dropped and a single
     `queue_overflow` event is emitted; C++ raises a non-fatal
     `errorOccurred` and keeps polling. Prevents the client from
     eating memory if the UI thread stalls.
  2. `mx_rust_start_sync` now atomically reserves the running flag
     before spawning its worker thread, closing a small window where
     two calls could race and start two sync loops.
  3. Encrypted timeline rows the SDK could not decrypt now propagate
     through the FFI as `undecryptable: true` with an empty body,
     and C++ renders them with the localised
     `[unable to decrypt yet]` placeholder. Ciphertext is not
     forwarded through the FFI in any form.
  See `docs/backend-contract.md` for the full FFI event schema.

## Where the honest E2EE flag lives

`src/crypto/CryptoManager.cpp`:

```cpp
bool CryptoManager::supportsE2ee() const
{
#ifdef ENABLE_RUST_SDK_BACKEND
#  ifdef RUST_SDK_E2EE_WIRED
    return m_backendName == QLatin1String("rust");
#  else
    return false;
#  endif
#else
    return false;
#endif
}
```

`RUST_SDK_E2EE_WIRED` is not defined anywhere yet. Do not flip this
without verified encrypted read and encrypted send through the Matrix
Rust SDK.

## SecretStore honesty

`SettingsManager::secretsAreSecure` returns:

```cpp
m_secretStore && m_secretStore->isSecure() && m_secretStore->isAvailable();
```

- `LibSecretStore::isSecure() == true` and `isAvailable()` depends on
  a runtime probe of `secret_service_get_sync`. When the session bus
  isn't reachable (headless CI, container without a session), the
  factory falls back to `InsecureFallbackSecretStore` — which reports
  `isSecure() == false`. The Settings screen renders a red warning
  banner whenever `secretsAreSecure == false`.
