# Matrix feature status (v0.4.2)

Honest per-feature status per backend. Ground truth for anything the UI
claims about support. Do not check anything as "done" here that is not
actually done in code.

Legend:

- ✅ Implemented and used in production paths.
- 🟡 Partial — code path exists but is limited (documented after the
  table).
- ⏳ Stubbed / placeholder in code, not usable end-to-end.
- ❌ Not present.

| Feature | Mock | HTTP | Rust (scaffold) |
|---|---|---|---|
| Password login (`m.login.password`) | ✅ (any creds) | ✅ | ❌ refuses |
| Session persistence + `/whoami` restore | 🟡 in-mem | ✅ | ❌ |
| Long-poll `/sync` | ⏳ | ✅ | ❌ |
| Text message send / receive | ✅ | ✅ | ❌ |
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
| Local SQLite cache | n/a | ✅ | ❌ |
| **Spaces — recognise `m.room.create type:m.space`** | ✅ seeded | ✅ (v0.4.2) | ❌ |
| **Spaces — `m.space.child` hierarchy** | ✅ seeded | ✅ (v0.4.2) | ❌ |
| **Spaces — filter room list by active Space** | ✅ (chip strip) | ✅ (v0.4.2) | ❌ |
| **Spaces — persistence across restart (before /sync)** | n/a | 🟡 rooms restore but Space flags do not (see notes) | ❌ |
| **Threads — detect `m.thread` relation** | ✅ seeded | 🟡 receives replies but does not tag them as thread relations | ❌ |
| **Threads — compose thread reply** | ✅ (Mock preserves grouping) | 🟡 sent as normal reply (fallback) | ❌ refuses |
| **Threads — indicator on root** | ✅ | 🟡 only visible after Mock-seeded threads | ❌ |
| Encrypted room read | ❌ placeholder | ❌ placeholder | ❌ |
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
- **HTTP — threads**: `MessageComposer::send()` calls
  `sendThreadReply(roomId, rootId, body)`. `MatrixClient`'s default
  implementation routes that to `sendReply`, so the message is
  delivered as an `m.in_reply_to` — the recipient sees a normal reply,
  not a proper `m.thread` grouping. Full `m.thread` support in HTTP is
  a v0.5 target (see `docs/next-prompts.md`).
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
- **Everything on Rust**: scaffold refuses. The Rust FFI reports name /
  status / version; login and every send emit `errorOccurred` /
  `loginFailed`.

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
without a real matrix-sdk integration that decrypts and encrypts.

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
