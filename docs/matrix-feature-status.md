# Matrix feature status (v0.4.1)

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
| **Spaces — recognise `m.room.create type:m.space`** | ✅ seeded | ❌ not parsed from sync | ❌ |
| **Spaces — `m.space.child` hierarchy** | ✅ seeded | ❌ | ❌ |
| **Spaces — filter room list by active Space** | ✅ (chip strip) | ✅ if a backend surfaces `RoomInfo.isSpace/childRoomIds` | ❌ |
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
- **HTTP — Spaces from sync**: `CppHttpMatrixClient::parseSyncResponse`
  does not currently look at `m.room.create` `type` or at
  `m.space.child` state events. As a result, `RoomInfo.isSpace` and
  `RoomInfo.childRoomIds` are always empty from HTTP, so the QML chip
  strip stays hidden. Mock is the reference for what needs to be
  populated.
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
