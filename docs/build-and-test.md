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
8. **Logout**. Toolbar → Sign out. Settings entry is cleared; the
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
