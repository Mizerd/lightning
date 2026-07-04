# Current state (v0.4.2)

Last updated: 2026-07-05 (v0.4.2 pass).

This is the "where the repo actually is" doc. Treat it as ground truth
for a fresh LLM continuation session — read this before
`docs/matrix-feature-status.md`, before `README.md`, before touching
code.

## Repository

- Origin: `https://gitlab.smetonis.net/Mizerd/lightning.git`
- Local path this doc was written from: `/home/roksme/git/lightning/`
- Branch: `main`
- Most recent commits (newest last):
  - `c522f5d` Codex setup
  - `4310913` Fix Nix dev shell: drop stale qtquickcontrols2 attr, add .gitignore
  - `d251948` v0.4: SecretStore + backend CLI cleanup + Rust SDK backend scaffold
  - `13adf73` v0.4.1: Spaces + Threads foundations, SSO/OIDC flags, continuation docs
  - HEAD after this pass: `v0.4.2: HTTP Spaces parsing + no-display preflight hardening`

## Layered architecture (unchanged from v0.4)

```
Qt/QML UI   →  qml/*.qml
App layer   →  src/app/{AppController,SettingsManager}
Auth        →  src/auth/{AuthManager,AccountManager}
UI models   →  src/models/{RoomListModel,TimelineModel,MessageComposer}
              src/spaces/SpaceManager        ← QAbstractListModel of Spaces
              src/threads/ThreadManager      ← thread aggregation helper
Backend iface: src/matrix/MatrixClient.h      ← the swap seam
Backends:     src/matrix/MockMatrixClient.{h,cpp}      --backend=mock
              src/matrix/CppHttpMatrixClient.{h,cpp}   --backend=http (default)
              src/matrix/RustSdkMatrixClient.{h,cpp}   --backend=rust
                                                       (only when compiled with
                                                        -DENABLE_RUST_SDK_BACKEND=ON)
Rust crate:  rust/                                    static lib + C ABI shim
Storage:     QSettings (prefs + non-secret session metadata)
             src/storage/SecretStore (LibSecret or InsecureFallback)
             src/storage/CacheStore  (SQLite: rooms/events/members)
Platform:    src/notifications/NotificationManager (stub)
             src/media/MediaManager (send/receive + open-external)
Crypto:      src/crypto/CryptoManager (capability surface only, no crypto)
```

## What is *implemented* right now

- **Backend selection**: `--backend={mock,http,rust}` plus legacy `--mock`.
  Pre-flight validation runs *before* `QGuiApplication` so bad args exit
  cleanly with exit code 2 even without a display. **v0.4.2**: a second
  preflight check refuses to construct `QGuiApplication` when neither
  `DISPLAY` nor `WAYLAND_DISPLAY` is set and `QT_QPA_PLATFORM` is not
  forced — exits 3 with a clear message instead of Qt's `qFatal`
  abort() (that is what caused the reported v0.4.0 coredump).
- **Mock backend** (`--backend=mock`): hardcoded rooms, one Space
  containing two rooms, one standalone room, one threaded conversation,
  synthetic reactions/edits/redactions/media/pagination.
- **HTTP backend** (`--backend=http`, default): password login,
  `/whoami` restore, long-poll `/sync`, room list, text messages,
  replies, edits, redactions, reactions, typing, read receipts,
  pagination via `/messages?dir=b`, member cache, media send/receive
  via legacy `/_matrix/media/v3/*`, local SQLite cache. **No E2EE.**
  Encrypted rooms are read-only placeholders; sends into encrypted
  rooms are blocked with a clear error.
- **Rust backend** (`--backend=rust`, only with
  `-DENABLE_RUST_SDK_BACKEND=ON`): scaffold that links against
  `rust/`. Reports backend name/status/version through a small C ABI.
  Refuses login and all sends honestly. No `matrix-sdk` dependency yet.
- **SecretStore**: libsecret (Secret Service via glib) backend when
  available; `InsecureFallbackSecretStore` (QSettings under `secrets/*`)
  when the session bus is unreachable. Legacy plaintext
  `session/accessToken` in QSettings is auto-migrated into the store
  on first launch of v0.4+.
- **SQLite cache**: `${XDG_DATA_HOME}/matrix-client/<safeUserId>/cache.sqlite`.
  Rooms + last 200 events per room + members. Access tokens are **not**
  cached here. `SettingsManager::clearSession()` wipes both the
  QSettings session metadata and the SecretStore entry for that user.
- **Spaces (v0.4.1 + v0.4.2)**: `SpaceManager` is a `QAbstractListModel`
  bound to the active `MatrixClient`. Row 0 is a synthetic "All rooms";
  if any Space has children, an "Other rooms" row follows; then real
  Spaces. QML `RoomListPane` renders a chip strip when at least one
  real Space exists. `RoomListModel` applies the active-space filter.
  **v0.4.2**: `CppHttpMatrixClient::processStateEvent` now recognises
  `m.room.create` (`content.type == "m.space"` → `RoomInfo::isSpace`)
  and `m.space.child` (state key = child room id, `via[]` non-empty =
  active edge; empty = unlinked). Both events are handled from the
  `state.events` bucket AND from timeline state events. The
  `RoomInfo::spaceId` "primary parent" hint is intentionally not set on
  children — SpaceManager builds membership strictly from
  `Space.childRoomIds`, so rooms in multiple Spaces stay consistent.
  Known limitation: `CacheStore` does not persist `isSpace` /
  `childRoomIds` yet, so on relaunch the chip strip is hidden until
  the first `/sync` completes.
- **Threads (v0.4.1)**: `MessageComposer` gains thread-reply mode via
  `beginThreadReply(rootId, preview)`. `TimelineModel` exposes
  `threadRootId`, `isThreadRoot`, `threadReplyCount` roles. Mock
  backend seeds a threaded conversation. `MatrixClient::sendThreadReply`
  is a virtual with a default that falls back to `sendReply` — the
  Mock backend overrides to preserve `threadRootId`, HTTP still uses
  the reply-relation default.
- **SSO/OIDC capability flags (v0.4.1)**: `AuthManager` exposes
  `supportsPasswordLogin`, `supportsSsoLogin` (false), `supportsOidcLogin`
  (false), plus placeholder `beginSsoLogin` / `beginOidcLogin` that
  emit a clean "not implemented" error. QML is not wired to these yet
  (Settings screen is the natural spot in a follow-up).

## What is *stubbed* — code exists but does not do the work

- `RustSdkMatrixClient`: compiles and links; every send / login / paginate
  operation refuses with a "not implemented" `errorOccurred`.
- `NotificationManager`: logs "notify" via QLoggingCategory. No tray, no
  native notify.
- `CryptoManager`: capability surface only. `supportsE2ee` is a pure
  compile-time expression — it becomes true only if
  `ENABLE_RUST_SDK_BACKEND` **and** `RUST_SDK_E2EE_WIRED` are both
  defined, AND the active backend is "rust". Neither is defined right
  now.
- `AccountManager`: tracks the single active user id from the session.
  Multi-account (per-account SecretStore keyspace, cache path, sync
  loop) is not implemented — foundation described in
  `docs/next-prompts.md`.

## What is *intentionally missing*

- Real E2EE. The Rust backend does not implement Olm/Megolm yet, and
  we will not hand-roll cryptography in C++.
- Authenticated media (`/_matrix/client/v1/media/*`) — v0.3/v0.4 use
  legacy `/_matrix/media/v3/*`.
- Full SSO / OIDC / MAS login flow.
- Multi-account UI switching. `AccountManager` API is single-active.
- Sliding sync.
- Windows / macOS SecretStore backends (they fall back to the insecure
  store with a warning until v0.5+).
- Own-profile lookup, other-user read-receipt display, member list UI.
- Thread panel / per-thread timeline model. Current thread UI is a
  chip on the root event + a composer mode.

## Build & smoke summary (see `docs/build-and-test.md` for details)

- Default: `nix develop -c cmake -S . -B build -G Ninja && nix develop -c cmake --build build`.
- With Rust: `nix develop -c cmake -S . -B build-rust -G Ninja -DENABLE_RUST_SDK_BACKEND=ON && nix develop -c cmake --build build-rust`.
- Smoke: `QT_QPA_PLATFORM=offscreen timeout 3 ./build/matrix-client --mock`
  should exit 124 with no QML warnings and no crashes.
- Rejection: `QT_QPA_PLATFORM=offscreen ./build/matrix-client --backend=bogus`
  and `./build/matrix-client --backend=rust` (in the non-Rust build) both
  exit 2 with a clear stderr message.

## Rules for continuation

1. Do not rewrite from scratch. Keep the file layout.
2. Do not fake E2EE. `CryptoManager::supportsE2ee` is the single source
   of truth for the UI.
3. Do not remove the mock or HTTP backends.
4. Do not use Electron, Tauri, WebEngine chat UI, or Element Web.
5. Prefer C++ for anything that is not a Matrix cryptographic primitive
   or a place where the Matrix Rust SDK is the objectively correct
   dependency.
6. Any `Q_PROPERTY(T*)` in a header must have `T` fully defined in that
   header (moc reads `QMetaType::fromType<T>()` which requires
   completeness — we've been bitten by this twice, see the includes
   at the top of `src/app/AppController.h`).
7. Bad `--backend=…` values must never crash — pre-flight validation
   in `src/main.cpp` catches them before Qt starts.
