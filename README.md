# matrix-client

A native desktop Matrix client written in C++20 / Qt 6 / QML.

**Not** Electron. **Not** Tauri. **Not** a webview wrapping a browser. The chat UI is real Qt Quick.

## Status

**v0.3** — Richer unencrypted chat UX on top of the C++ HTTP backend.

New in v0.3:

- Backfill pagination via `GET /rooms/{id}/messages?dir=b` — scroll to the top of a room to load older history.
- Local echo resolution: the `event_id` from the PUT response replaces the `local:<txn>` placeholder so edits/redactions of just-sent messages work.
- Sender display names + avatars sourced from `m.room.member` state, cached per room (Matrix display names are room-scoped).
- Media receive: `m.image` and `m.file` are rendered inline, with `mxc://` resolved to HTTPS via `/_matrix/media/v3/{download,thumbnail}`.
- Media send: file picker in QML → `POST /_matrix/media/v3/upload` → send `m.image` / `m.file` with `info`.
- Replies: right-click / hover ↰ → composer shows a "Replying to …" bar, message sent with `m.in_reply_to`.
- Edits: hover ⋯ → Edit. Composer loads the current body; sending emits `m.replace`; the wrapper is suppressed and the target is updated in place with an `edited` marker.
- Redactions: hover ⋯ → Delete on your own message. Timeline shows `[message deleted]`.
- Reactions: hover 😊 → pick from a 5-emoji palette. Click your own reaction pill to remove it (redacts your `m.reaction`).
- Typing: composer sends `PUT /rooms/{id}/typing/{userId}` while text is non-empty, with a 15-second keep-alive and false on clear / room change / send.
- Read receipts: whenever the timeline gains events, the latest is acked via `POST /rooms/{id}/receipt/m.read/{eventId}` (debounced).
- Local SQLite cache under `${XDG_DATA_HOME}/matrix-client/<safeUserId>/cache.sqlite` — rooms, last 200 events per room, and members. Loaded before `/whoami` completes so the room list appears instantly on restart.

Still carried over from v0.2:

- Password login (`m.login.password`), session restore via `/account/whoami`, long-poll `/sync`, `POST /logout`.
- Room list with `m.room.name` / `m.room.topic` / `m.room.encryption` / `m.room.avatar` / notification counts.
- Text messages (`m.text` / `m.notice` / `m.emote`).
- Encrypted rooms remain read-only placeholders. Sends (text, media, reactions, edits) into encrypted rooms are blocked with a clear error.
- `--mock` for UI work, now with reply / edit / redaction / reactions / media / pagination demo data.

Still intentionally missing:

- E2EE (v0.4 via Matrix Rust SDK) — encrypted rooms show placeholders, encrypted media is not fetched.
- Secure token storage — access tokens are still written to QSettings in plaintext with a visible warning in Settings.
- SSO / OIDC, spaces, threads, multi-account, sliding sync (v0.5).
- Authenticated media endpoints (`/_matrix/client/v1/media/*`); v0.3 uses legacy `/_matrix/media/v3/*`.
- Read-receipt display of *other* users, avatars rendered as actual images (avatars are captured in cache but not shown yet), user's own profile lookup, room member list UI.

See [`docs/roadmap.md`](docs/roadmap.md) for the milestone plan and [`docs/architecture.md`](docs/architecture.md) for the layering.

## Build (Linux)

Requires Qt 6.5+ (Core, Gui, Qml, Quick, QuickControls2, Network, Sql, Widgets), CMake 3.21+, a C++20 compiler, and Ninja.

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/matrix-client
```

### NixOS

```bash
nix develop            # or: nix-shell
cmake -S . -B build -G Ninja
cmake --build build
./build/matrix-client
```

### Windows / macOS

Not yet targeted. The CMake project is portable; packaging follows in v1.0.

## Try it

### Real homeserver (default)

```bash
./build/matrix-client
```

Enter your homeserver URL (e.g. `https://matrix.org`), your username (localpart or full MXID),
and your password. On successful login the app persists the session and starts syncing.
Restart the app — you'll be signed back in automatically. Sign out from the toolbar to
clear the session.

### Mock backend (for UI work)

```bash
./build/matrix-client --mock
```

Any credentials succeed. Hard-coded rooms and messages appear. Nothing hits the network.

## Layering

```
Qt/QML UI      →  qml/*.qml
App layer      →  src/app, src/auth
UI models      →  src/models
Backend iface  →  src/matrix/MatrixClient.h
Backends       →  src/matrix/MockMatrixClient.{h,cpp}    ← v0.1 (still available via --mock)
                  src/matrix/CppHttpMatrixClient.{h,cpp} ← v0.2 (default)
                  (RustSdkMatrixClient via FFI)          ← v0.4+
Storage        →  QSettings (v0.1), SQLite + keyring (v0.3+)
Platform       →  src/notifications, src/media (stubs in v0.1)
```

The `MatrixClient` interface is the swap seam. UI, models, and the app layer never depend on a concrete backend.

## What v0.3 does *not* do

- No encryption. Encrypted rooms show `[encrypted message - E2EE not implemented yet]`. `m.image`/`m.file` events with an encrypted `file` envelope show `[encrypted media - E2EE not implemented yet]`. Sends into encrypted rooms are blocked. E2EE arrives in v0.4 via the Matrix Rust SDK — we do not hand-roll crypto.
- No secure token storage. Access tokens are stored in `QSettings` in plaintext with a visible warning in Settings. The SQLite cache under `${XDG_DATA_HOME}/matrix-client/<safeUserId>/` does not contain access tokens. Keychain integration ships in v0.4.
- No spaces, threads, multi-account, sliding sync, SSO/OIDC. Those are v0.5.
- No display of other users' read receipts. Avatars are cached but not rendered as images yet.

## Contributing

Prefer many small files over one large one. Keep protocol logic behind the `MatrixClient` interface. Do not import protocol types into QML files.
