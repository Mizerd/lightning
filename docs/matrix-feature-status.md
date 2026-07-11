# Matrix feature status (v0.5.9)

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
| Password login (`m.login.password`) | ✅ (any creds) | ✅ | ✅ verified live against matrix.smetonis.net (v0.5.0-prep+6) |
| Session persistence + restore | 🟡 in-mem | ✅ (`/whoami`) | 🟡 interactive restore via SettingsManager/SecretStore is wired; persistent smoke restore now uses a smoke-only MatrixSession sidecar and stable SDK store |
| Long-poll `/sync` | ⏳ | ✅ | ✅ classic compatibility mode; modern mode uses unified Sliding Sync |
| Initial-sync UX (`initialSyncDone` capability) | n/a | ✅ (v0.4.6) | ✅ flips after first SDK sync callback |
| Text message send / receive | ✅ | ✅ | 🟡 basic text/notice/emote only; unencrypted send only |
| Backfill pagination | ✅ | ✅ | ✅ SDK Timeline |
| Read receipts (self → server) | ✅ | ✅ | ✅ public receipt + fully-read marker |
| Typing indicator (send + display) | ✅ | ✅ | ✅ SDK replacement stream + throttled notice |
| Replies (`m.in_reply_to`) | ✅ | ✅ | ✅ SDK Timeline |
| Edits (`m.replace`) | ✅ | ✅ | ✅ SDK Timeline |
| Redactions | ✅ | ✅ | ✅ SDK Timeline |
| Reactions (`m.reaction` / `m.annotation`) | ✅ | ✅ | ✅ SDK Timeline |
| Media send (image/file, legacy `/media/v3/upload`) | 🟡 no-op | ✅ | ✅ (v0.5.9) `Timeline::send_attachment().use_send_queue()` — composer tray (picker/drag-drop/clipboard paste), SDK local echo + retry, MIME from content, server upload-limit gate |
| Media receive (image/file, legacy `/media/v3/download`) | ✅ | ✅ | ✅ (v0.5.9) media bridge: Rust-side `MediaSource` registry + `Media::get_media_content` (decrypts) + binary take/free FFI + C++ LRU cache/image provider; timeline thumbnails, in-app image viewer (zoom/pan/prev/next, GIF), explicit Save As (atomic, sanitized, never auto-opened) |
| User directory search (`/user_directory/search`) | ❌ | ❌ | ✅ (v0.5.9) debounced, stale-result-safe `UserSearchModel` |
| Create DM (`create_dm` + locked `m.direct` merge) | ❌ | ❌ | ✅ (v0.5.9) with existing-DM reuse offer |
| Create room (`create_room`, encryption initial state) | ❌ | ❌ | ✅ (v0.5.9) private/public, optional alias, initial invites, optional Space placement |
| Invite users to joined room | ❌ | ❌ | ✅ (v0.5.9) permission-checked (`RoomMember::can_invite`), per-user results |
| Room member list + roles | ❌ | ❌ | ✅ (v0.5.9) bounded snapshot (500), joined/invited, role labels, ambiguity flags |
| Room profile edit (name/topic/avatar) | ❌ | ❌ | ✅ (v0.5.9) gated on `can_send_state`; avatar via SDK `upload_avatar` |
| Leave joined room | ❌ | ❌ | ✅ (v0.5.9) with confirmation; list updates via authoritative sync |
| Authenticated media (`/client/v1/media/*`) | ❌ | ❌ | ❌ |
| Local SQLite cache | n/a | ✅ | 🟡 Rust SDK store only; no C++ CacheStore timeline cache; encrypted `TimelineEvent` rows are skipped so decrypted encrypted-room plaintext is not cached in `cache.sqlite` |
| **Spaces — recognise `m.room.create type:m.space`** | ✅ seeded | ✅ (v0.4.2) | 🟡 SDK `room.is_space()` surfaced |
| **Spaces — `m.space.child` hierarchy** | ✅ seeded | ✅ (v0.4.2) | ✅ SpaceService nested/cycle-safe filters |
| **Spaces — filter room list by active Space** | ✅ (chip strip) | ✅ (v0.4.2) | ✅ transitive descendants, multiple parents |
| **Spaces — persistence across restart (before /sync)** | n/a | ✅ (v0.4.5) | ❌ |
| **Threads — detect `m.thread` relation** | ✅ seeded | ✅ (v0.4.4) | ❌ |
| **Threads — compose thread reply** | ✅ (Mock preserves grouping) | ✅ (v0.4.4, real m.thread relation) | ❌ refuses |
| **Threads — indicator on root** | ✅ | ✅ (v0.4.4, locally computed count) | ❌ |
| **Threads — server-side aggregation (`unsigned["m.relations"]["m.thread"]`)** | n/a | ❌ (v0.5) | ❌ |
| **Threads — dedicated thread panel / per-thread timeline model** | ❌ | ❌ (v0.5+) | ❌ |
| **Threads — persistence across restart (before /sync)** | n/a | ✅ (v0.4.5) | ❌ |
| Encrypted room read | ❌ placeholder | ❌ placeholder | ✅ initial E2EE support (v0.5.0-prep+9): verified live against Element Classic on a persistent SDK store — `expect_text=seen`, `decrypted_events_since_expect=1`, `undecryptable_since_expect=0`. Undecryptable events still render as `[unable to decrypt yet]` |
| Encrypted send | ❌ blocked | ❌ blocked | ✅ initial E2EE support (v0.5.0-prep+9): matrix-sdk auto-encrypts on the interactive UI path; the encrypted-send probe was displayed as readable text in Element Classic |
| Device verification / cross-signing | ❌ | ❌ | ✅ SAS + trust snapshot |
| Encrypted media | ❌ placeholder | ❌ placeholder | ✅ (v0.5.9) sent via SDK-encrypted attachments; received via decrypt-on-fetch in the media bridge (sources/keys stay inside Rust; plaintext memory-only, `LIGHTNING_MEDIA_CACHE_TEST_059` proves nothing reaches cache.sqlite) |
| SSO login | ❌ capability flag `false` | ❌ capability flag `false` | ❌ |
| OIDC / MAS login | ❌ capability flag `false` | ❌ capability flag `false` | ❌ |
| Multi-account switching | ❌ single-active | ❌ single-active | ❌ |
| Sliding sync | ❌ | ❌ | ✅ capability-probed SyncService with classic fallback |
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
- **Rust SDK — v0.5.0-prep+4 verification harness.**
  `--rust-sdk-smoke-test` is a new headless entry (Rust-enabled
  build only) that logs in against a real homeserver, drives
  `startSync()`, counts joined / encrypted / Space rooms, counts
  timeline events (including the `undecryptable` placeholders), and
  optionally sends one probe text via `LIGHTNING_TEST_SEND=1`. It
  runs under `QCoreApplication` with a 60 s budget and reads
  credentials only from environment variables. It never prints
  bodies / tokens / passwords / crypto material, and constructs
  `RustSdkMatrixClient` with a null `SettingsManager` so it cannot
  overwrite the interactive user's cached session. Full usage +
  exit-code table in `docs/build-and-test.md`.
- **Rust SDK — persistent smoke store/session.**
  `LIGHTNING_TEST_PERSISTENT_STORE=1` reuses the account's normal
  Rust SDK store and a smoke-only MatrixSession sidecar so repeated
  smoke runs keep the same Matrix device. This is only for encrypted
  receive verification. It does not flip `supportsE2ee`, does not
  enable UI encrypted sends, and does not write the interactive
  QSettings/SecretStore session. On the known account/device mismatch
  it deletes only that account's Rust SDK store plus the smoke sidecar
  and retries once.
- **Cache policy for encrypted events.** `CacheStore` skips
  `TimelineEvent` rows marked `isEncrypted`, including decrypted
  encrypted-room events, so encrypted-room plaintext is memory-only
  until a deliberate encrypted-cache design exists.

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
