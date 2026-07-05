# Build and test

Everything below assumes NixOS. On non-Nix Linux with system Qt 6.5+
you can drop the `nix develop -c` wrapper and the same commands work.
Windows / macOS are not targeted yet.

## Enter the dev shell

```bash
nix develop            # or `nix-shell` (see shell.nix)
```

The shell provides: cmake, ninja, gcc, pkg-config, Qt 6 (qtbase +
qtdeclarative + qtsvg + qtwayland + qttools), libsecret, glib, rustc,
cargo, wrapQtAppsHook.

The first time you enter, Nix downloads ~1 GB of Qt/glib artefacts.
Subsequent enters are ~instant.

## Non-Rust build (default)

```bash
nix develop -c cmake -S . -B build -G Ninja
nix develop -c cmake --build build
```

Expected configure output includes:

```
-- libsecret-1 found (…) — enabling native secure storage
-- Rust SDK backend disabled (use -DENABLE_RUST_SDK_BACKEND=ON to build it)
```

If libsecret is not found you will still get a working binary — it just
falls back to the insecure QSettings store with a red banner in
Settings.

## Rust build (optional)

```bash
nix develop -c cmake -S . -B build-rust -G Ninja -DENABLE_RUST_SDK_BACKEND=ON
nix develop -c cmake --build build-rust
```

Expected configure output includes:

```
-- libsecret-1 found (…) — enabling native secure storage
-- Rust SDK backend enabled (cargo profile: debug)
```

The Rust crate at `rust/` builds a `libmatrix_client_rust.a` static
library which is linked into the C++ binary. There is no matrix-sdk
dependency yet — the crate is a small C ABI shim.

## Smoke tests

All smoke tests run under offscreen QPA. Exit code 124 (timeout) with
no QML warnings and no crash = success. Non-zero non-124 exit = fail.

```bash
# non-Rust build
nix develop -c bash -lc 'QT_QPA_PLATFORM=offscreen timeout 3 ./build/matrix-client --mock'
nix develop -c bash -lc 'QT_QPA_PLATFORM=offscreen timeout 3 ./build/matrix-client --backend=mock'
nix develop -c bash -lc 'QT_QPA_PLATFORM=offscreen timeout 3 ./build/matrix-client --backend=http'
nix develop -c bash -lc 'QT_QPA_PLATFORM=offscreen ./build/matrix-client --backend=rust'   # exits 2 with a clean message
nix develop -c bash -lc 'QT_QPA_PLATFORM=offscreen ./build/matrix-client --backend=bogus'  # exits 2 with a clean message

# Rust build
nix develop -c bash -lc 'QT_QPA_PLATFORM=offscreen timeout 3 ./build-rust/matrix-client --backend=rust'
```

Pre-flight validation runs before QGuiApplication in `src/main.cpp`,
so bad `--backend=` values and unknown flags give the same clean exit-2
message even without offscreen QPA.

v0.4.2 adds a second preflight: if neither `DISPLAY` nor
`WAYLAND_DISPLAY` is set and `QT_QPA_PLATFORM` is not forced, the app
exits **3** with a clear message instead of Qt's platform-plugin
`qFatal` abort. Test:

```bash
nix develop -c bash -lc 'env -u DISPLAY -u WAYLAND_DISPLAY ./build/matrix-client --backend=mock'
# → exit 3
#   matrix-client: no graphical display available (DISPLAY / WAYLAND_DISPLAY unset).
#   Run this app inside a graphical session, or export QT_QPA_PLATFORM=offscreen …
```

Rejection paths (exit 2) always take priority over the display check
(exit 3), so `--backend=bogus` without a display still exits 2, not 3.

## Manual test on a real homeserver

Prerequisite: a running graphical session, or an XDG runtime dir that
Qt Wayland/X11 can attach to. On NixOS with a Wayland compositor, just
run inside the shell:

```bash
nix develop -c ./build/matrix-client        # equivalent to --backend=http
```

1. **First-time login**: enter a homeserver URL (e.g.
   `https://matrix.org`), your MXID localpart or full MXID, and your
   password. Watch the status bar: "Not connected" → "Connecting…" →
   "Syncing".
2. **Session restore**: quit and relaunch. You should skip past the
   login screen and land on the room list. Check the Settings screen:
   "Secret backend: libsecret (Secret Service)" and the green banner.
3. **Send a text message**. It should appear immediately with
   "sending…" and flip to a plain timestamp within a second.
4. **Reply / edit / redact / react**. Hover on your own message → the
   `…` menu offers Edit / Delete / Reply in thread. Hover on a
   received message → the emoji menu opens a 5-emoji palette.
5. **Media**. `+` in the composer opens an image / file picker; upload
   progress is visible in the status bar; the image renders inline.
6. **Pagination**. Scroll to the top of the timeline. Older messages
   are loaded via `/messages?dir=b`.
7. **Spaces (v0.4.2)**. If your account is in at least one Space, a
   chip strip appears above the room list: "All rooms · N", "Other
   rooms · M" (if any rooms sit outside every Space), and one chip per
   Space. Click a Space chip → only that Space's joined children are
   listed. Click "All rooms" to restore. Rooms you're not joined to
   that a Space references are silently ignored. **Known limitation**:
   on relaunch the chip strip is briefly hidden until `/sync` completes
   (the SQLite cache does not yet persist Space fields).
8. **Threads (v0.4.4)**. Hover any message → `…` menu → "Reply in
   thread". The composer banner shows "Replying in thread: …". Send;
   your echo appears immediately marked "· in thread" and, on
   round-trip, the root's chip flips to "· 1 reply in thread". A
   second client (Element / matrix-commander / another Lightning
   instance) should see the message as a proper thread reply — the
   payload carries `m.relates_to.rel_type == "m.thread"` and the
   spec-compliant `is_falling_back` + `m.in_reply_to` fallback so
   non-thread-aware clients still see a reply chain. Server-side
   aggregation counts (`unsigned["m.relations"]["m.thread"]`) are
   not consumed yet — v0.4.4 counts by scanning the loaded timeline.
9. **Logout**. Toolbar → Sign out. Settings entry is cleared; the
   SecretStore entry is deleted (verify by re-launching; you should
   be back at the login screen).

## Manual test with the mock backend

```bash
nix develop -c ./build/matrix-client --mock
```

Any credentials work. Mock rooms appear immediately. Verify:

- **Spaces chip strip**: at the top of the room list, a chip labeled
  "All rooms" is selected. A chip labeled "Team · 2" appears next to
  it, and a chip labeled "Other rooms · 1" appears after. Clicking
  "Team" filters the list to General + Developers; clicking "Other
  rooms" shows Bob. Clicking "All rooms" restores everything.
- **Threads**: open "General". Scroll to find the message
  "Anyone up for a mock deploy tomorrow?" — it has a "· 2 replies in
  thread" indicator. Click it; the composer shows a
  "Replying in thread: …" banner. Type + Enter; the new message shows
  a "· in thread" marker.
- **Reply**: hover any message, click the ↰ button, composer shows
  "Replying to …". Send; the reply appears with a preview line above
  the body.
- **Edit / Redact / React**: same flow as the HTTP manual test.

## Manual test with the Rust backend

```bash
nix develop -c ./build-rust/matrix-client --backend=rust
```

- Settings screen shows "Crypto backend: Matrix Rust SDK backend
  (scaffold). Compiled in but not yet feature-complete — login/sync/
  crypto are not wired."
- Attempt login: "Rust SDK backend scaffold present but login is not
  wired in v0.4. Use --backend=http for a working session."
- All send operations refuse cleanly.

## Troubleshooting

### Qt platform plugin fails to load on NixOS / KDE / GNOME

Symptom (from the v0.4.2 report):

```
qt.qpa.plugin: Could not load the Qt platform plugin "wayland" ...
qt.qpa.plugin: Could not load the Qt platform plugin "xcb" ...
This application failed to start because no Qt platform plugin could be initialized.
Aborted (core dumped)
```

Stack trace ended in
`QGuiApplicationPrivate::createPlatformIntegration → qFatal → abort`.

**Root cause (v0.4.3)**: a running KDE Plasma / GNOME session on
NixOS exports `QT_PLUGIN_PATH` pointing at the system-wide qtbase
(e.g. 6.11.0). The dev-shell's Qt from `nixos-unstable` may be a
different patch version (e.g. 6.11.1), and the executable is linked
against *that*. When Qt initialises its platform plugin, it walks
`QT_PLUGIN_PATH` first, loads a helper plugin from the 6.11.0 tree
into the 6.11.1 process, and aborts on the version check *before* it
prints any error — which is why `QT_DEBUG_PLUGINS=1` produced no
output in the wild.

**Fix (v0.4.3)**: `flake.nix` and `shell.nix` shellHooks now
`unset` `QT_PLUGIN_PATH`, `QT_QPA_PLATFORM_PLUGIN_PATH`,
`QML_IMPORT_PATH`, `QML2_IMPORT_PATH`, `QT_QUICK_CONTROLS_STYLE`,
`QT_QUICK_CONTROLS_STYLE_PATH`, `QT_QPA_PLATFORMTHEME` before
setting flake-consistent values against `${qt.qtbase}`. Also adds
`xkeyboard_config` to `buildInputs` and exports
`QT_XKB_CONFIG_ROOT` so the xcb platform plugin can find keymaps.

**Verification**:

```bash
nix develop -c bash -lc 'QT_QPA_PLATFORM=wayland timeout 2 ./build/matrix-client --mock'
# → exit 124 (timeout, event loop running)

nix develop -c bash -lc 'QT_QPA_PLATFORM=xcb     timeout 2 ./build/matrix-client --mock'
# → exit 124
```

Both work now. `nix develop -c ./build/matrix-client --mock` (auto
platform selection on a graphical session) also succeeds — the app
opens.

If you still see the crash, you are probably running the binary
*outside* the dev shell (bare shell, tmux without `nix develop -c`,
IDE terminal without env inheritance). Re-enter `nix develop` and
retry — the shellHook is what makes the fix take effect.

### `--http` / `--rust` are not accepted

Use `--backend=http` / `--backend=rust`. The intentionally-shortcut
flags are rejected in the pre-flight parser (exit 2) with a hint,
before `QGuiApplication` is constructed. This keeps a typo from
being interpreted by Qt's own parser and potentially aborting on a
platform-plugin issue instead of surfacing the CLI error.

### HTTP login succeeds but the room list shows "Syncing" forever with 0 rooms

Fixed in v0.4.7. Root cause: the initial `/sync` request was using
`timeout=30000`, so a fresh login had to wait for the long-poll to
finish (up to 30s) before any response arrived. During that wait
the room list stayed empty and the footer said "Syncing" — visually
indistinguishable from a stuck app.

Follow-up root cause found against `matrix.smetonis.net`: after a
Space-only cache restore, QSettings could still hold a `syncToken`
while SQLite had the Space room and `m.space.child` ids but no visible
joined child-room rows. Continuing with `/sync?since=<token>` can
correctly return no joined-room objects, leaving the UI empty forever.

Fixes shipped:

- Initial `/sync` (no `since` token) uses `timeout=0` per Matrix
  spec, plus `full_state=true` to make the full-snapshot intent
  explicit. The server returns current state immediately. Follow-up
  syncs long-poll with `timeout=30000` as intended.
- Session restore discards a stored `syncToken` when the SQLite cache
  has no visible non-Space rooms (empty, unavailable, or only Space
  rooms). A resume token without visible cached room state can only
  produce deltas, so the client forces a since-less initial sync
  instead of showing zero rooms forever.
- Fresh login clears any persisted `syncToken` for the same MXID before
  starting sync.
- `MatrixClient::initialSyncDone()` — new capability on the
  interface. `false` until the first `/sync` response is parsed,
  then `true` for the rest of the session (reset on login /
  logout / clearLocalSession).
- `AppController::connectionStatus` shows `"Loading rooms…"` while
  sync is running AND `initialSyncDone` is false, then flips to
  `"Syncing"` after the first response.
- `qml/RoomListPane.qml` empty label reflects the same state:
  - not signed in → "Sign in to see rooms"
  - loading → "Loading rooms…"
  - real Space filter active + empty → "No rooms in this Space"
  - "All rooms" + empty → "No joined rooms"

Diagnostic log lines to look for in the terminal:

```
matrix.http: sync request: INITIAL (timeout=0) url= https://…/sync?timeout=0&full_state=true
matrix.http: sync response ok (initial) status= 200 size= <N>
matrix.http: sync parse: joined= <N> invited= 0 left= 0 next_batch_len= <N>
matrix.http: initial sync complete; rooms in memory = <N>
```

If you see the `sync request:` line but no `sync response ok`, the
request is stalling — check firewall / DNS / TLS to the homeserver
and any HTTP proxy in front of it. On session restore, this line:

```
matrix.http: discarding stored sync token because cache has no visible rooms; forcing initial sync
```

means the client detected a stale resume-token / empty-cache state and
will recover by requesting a full room snapshot. If you see
`sync parse: joined=0` on that initial request against an account that
clearly has joined rooms, the server side believes the authenticated
device is not joined to them.

### HTTP login logs "login ok" but the UI stays on the login screen

Fixed in v0.4.5. Root cause: `Main.qml`'s `Loader.sourceComponent`
picker was a JavaScript `switch (app.currentScreen) { case app.LoginScreen: … }`.
`app` is exposed via `setContextProperty`, not registered as a QML
type, so the enum values on the instance can resolve to `undefined`
under some Qt Quick compiler configurations — every case then fails
to match and the fallthrough returned `loginComponent` regardless of
the actual screen state.

Fix: `pickComponent()` compares against `AppController::Screen`
integer literals (0=Login, 1=Main, 2=Settings), plus an explicit
`Connections { onCurrentScreenChanged … }` re-triggers the picker on
the notify signal.

Diagnostic aid: `AppController::onLoginSucceeded` and
`setCurrentScreen` now emit `qCInfo(lcApp)` lines. When troubleshooting
you should see:

```
matrix.app: login succeeded for "@…" — switching to main + starting sync
matrix.app: screen change 0 -> 1 (0=Login, 1=Main, 2=Settings)
```

If you only see the first line and not the second, `setCurrentScreen`
was called with the current screen already equal to `MainScreen` —
which would mean the app was somehow already showing Main. If you see
neither line, the `MatrixClient::loginSucceeded` signal isn't reaching
`AuthManager`; verify that `AuthManager` was constructed against the
current `MatrixClient*` (see `AppController::AppController`).
