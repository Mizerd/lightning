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
-- Found SQLite3: …
-- Rust SDK backend enabled (cargo profile: debug)
```

The Rust crate at `rust/` builds a `libmatrix_client_rust.a` static
library which is linked into the C++ binary. CMake invokes Cargo with
`--offline --locked`, so the Rust-enabled build uses the committed
`Cargo.lock` and the already-fetched local crate cache.

Rust-only offline check:

```bash
cd rust
nix develop -c cargo build --release --offline
```

Unit tests (no live homeserver or credentials required):

```bash
nix develop -c ctest --test-dir build --output-on-failure
nix develop -c ctest --test-dir build-rust --output-on-failure
```

The suites cover canonical account derivation and traversal rejection,
account-scoped/idempotent Rust-state removal, preservation of cache/other
accounts, partial cleanup failure, lifecycle generations, stale callback
rejection, active-versus-shutdown 401 semantics, and store mismatch policy.

Rust-side unit tests (import-error classifier, no network, no credentials):

```bash
cd rust
nix develop /home/roksme/git/lightning -c cargo test --offline --locked
```

## Manual v0.5.6 verification tests

None of the following require credentials to appear on stdout or in a
log. Do not commit real key exports.

**Element → Lightning receive-first verification (regression):**
Sign in on Lightning, initiate verification from Element for Lightning's
device. Accept in Lightning; confirm the seven emojis match in both apps;
confirm from both sides. Settings should show **Status: Verified** after
the SDK trust snapshot refreshes.

**Lightning → Element outbound verification (new):** Sign in on Lightning
with a session Settings currently reports as **Not verified**. Click
**Verify this session**. Confirm Element receives the request, accept it
there. Lightning transitions to the seven-emoji view; confirm they match
on both sides. Settings should update to **Verified** only after the
SDK cross-signing snapshot completes.

**Wrong-passphrase key-import test:** From Element, export room keys with
a temporary strong passphrase. In Lightning, choose the file and enter a
deliberately wrong passphrase; confirm the failure message is
"The passphrase is incorrect or the key export is corrupted." and that
no passphrase appears in any log line. Repeat with the correct
passphrase; the completion summary should show `Imported sessions: N`
and `Affected rooms: M`.

**Import-alone-does-not-verify test (mandatory acceptance):** Start with
Settings showing **Not verified**. Import a valid encrypted room-key
export as above. Confirm Settings still reports **Not verified** after
the import completes. Then run SAS verification; confirm Settings
transitions independently to **Verified**.

**Sign-out during import:** Start an import against a large export (or
one that can be arranged to take at least a couple of seconds), and
click Sign out before it finishes. Lightning must not crash or hang,
must not surface a success toast on the Login screen, must clear the
passphrase field, and must not leave a detached completion callback
against the released client.

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
nix develop -c bash -lc './build/matrix-client --http || true'
nix develop -c bash -lc './build/matrix-client --rust || true'

# Rust build
nix develop -c bash -lc 'QT_QPA_PLATFORM=offscreen timeout 3 ./build-rust/matrix-client --backend=rust'
nix develop -c bash -lc './build-rust/matrix-client --reset-crypto-store || true'
nix develop -c bash -lc './build-rust/matrix-client --version'
```

### Headless Rust SDK smoke test (v0.5.0-prep+4)

The Rust-enabled build ships a `--rust-sdk-smoke-test` verification
harness. It talks to a real homeserver, reads credentials **only**
from environment variables (never CLI arguments), and prints
counts/statuses — never bodies, tokens, passwords, or crypto material.

```bash
LIGHTNING_TEST_HOMESERVER=https://your-homeserver \
LIGHTNING_TEST_USER='@your-user:your-homeserver' \
LIGHTNING_TEST_PASSWORD='<paste-only-into-your-shell-history-scrubber>' \
nix develop -c ./build-rust/matrix-client --backend=rust --rust-sdk-smoke-test
```

Optional environment:

- `LIGHTNING_TEST_PERSISTENT_STORE=1` — use the account-specific
  persistent Rust SDK store instead of a fresh temp store. Path:
  `${XDG_DATA_HOME}/MatrixClient/matrix-client/<safeUserId>/matrix-rust-sdk-store/`.
  The harness also uses a smoke-only MatrixSession sidecar next to the
  account directory so later runs can restore the same Matrix device.
  This sidecar contains an access token but is never printed and is
  not QSettings / SecretStore state.
- `LIGHTNING_TEST_SEND=1` — after login + initial sync, send one
  probe text `"Lightning smoke-test <epoch-seconds>"` to a
  non-encrypted joined room. Never sends into encrypted rooms.
- `LIGHTNING_TEST_ROOM_ID='!abc:server'` — pin the target room for
  either the plain or the encrypted probe send. `SEND` refuses an
  encrypted target; `SEND_ENCRYPTED` refuses a non-encrypted target.
- `LIGHTNING_TEST_EXPECT_TEXT='<marker>'` (v0.5.0-prep+6) — watch
  decrypted timeline bodies for `<marker>` and report only whether
  it was seen. The marker itself is **never** printed by the
  harness. Typical use: send `<marker>` from Element Classic into an
  encrypted room, then run the smoke test to verify Lightning can
  actually decrypt it. Combine with `LIGHTNING_TEST_REQUIRE_EXPECT=1`
  to make an un-seen marker return non-zero (exit 14) instead of
  the default advisory `expect_text=not_seen`.
- `LIGHTNING_TEST_EXPECT_WAIT_SECONDS=<n>` (v0.5.0-prep+7) — override
  the post-send wait window when `EXPECT_TEXT` is configured. Default
  is 90 s, clamped to [1, 3600]. During the wait the harness keeps
  syncing and watching decrypted bodies for the marker; the moment it
  arrives, the wait is cancelled and the harness exits. As of
  v0.5.0-prep+8 the harness's global budget scales with this value
  (`max(60, expect_wait + 30)`, plus 30 s more when
  `LIGHTNING_TEST_RECOVERY_KEY` is set) so the wait actually runs to
  completion. The old hard 60 s kill is gone.
- `LIGHTNING_TEST_RECOVERY_KEY='<base58-recovery-key>'` (v0.5.0-prep+7)
  — after `initial_sync=done` and while the send/probe phase runs,
  call matrix-sdk's `client.encryption().recovery().recover(...)` to
  import backed-up room keys from server-side secret storage. Runs
  only when `LIGHTNING_TEST_PERSISTENT_STORE=1` (a fresh temp store
  can't retain the keys). The recovery key is never printed and never
  crosses a log line. Status is emitted as
  `smoke: key_backup=attempted` → `key_backup=ok` or
  `key_backup=failed reason=<safe>`.
- `LIGHTNING_TEST_RECOVERY_PASSPHRASE='<phrase>'` (v0.5.0-prep+7) —
  reserved. matrix-sdk 0.18's clean recovery API takes a recovery
  *key*, not a passphrase, so the harness reports
  `key_backup=failed reason=passphrase_not_supported` when this is
  set without a key. Convert your passphrase to a recovery key in
  Element first.
- `LIGHTNING_TEST_SEND_ENCRYPTED=1` (v0.5.0-prep+6) — probe encrypted
  send. Picks the first encrypted joined room (or the room named by
  `LIGHTNING_TEST_ROOM_ID`) and asks matrix-sdk to encrypt + send a
  short probe. Reports `encrypted_send=ok marker=<short-id>
  event_id=<id>` on success. Body content is not printed. The
  matrix-sdk `e2e-encryption + sqlite` features do the encryption
  end-to-end; the C++ side never sees ciphertext or keys.

Current verified crypto status: the encrypted-send probe succeeded and
Element Classic displayed the Lightning probe as readable text. That
proves Lightning → Element encrypted send one-way. Element → Lightning
encrypted receive is not verified until the persistent
`LIGHTNING_TEST_EXPECT_TEXT` workflow reports `expect_text=seen`.
`supports_e2ee=false` remains correct.

Output format (all lines prefixed `smoke: `):

```
smoke: store=temporary
smoke: store_path=/tmp/lightning-rust-sdk-smoke-XXXXXX/matrix-rust-sdk-store
smoke: store_exists=no
smoke: store_account_match=unknown
smoke: device_id=unknown
smoke: supports_e2ee=false
smoke: expect_text=configured require=true|false     # only with EXPECT_TEXT
smoke: start homeserver=<hs>
smoke: restore=skipped                               # or attempted/ok/not_available/failed
smoke: state=connecting
smoke: login=ok
smoke: device_id=ABCD...WXYZ
smoke: state=syncing
smoke: rooms joined=<N> encrypted=<M> spaces=<S>
smoke: initial_sync=done
smoke: expect_text=seen                              # first decrypted match
smoke: send=start room=!…                            # LIGHTNING_TEST_SEND=1
smoke: send=ok                                       # or send=failed/timeout
smoke: encrypted_send=start room=!… marker=SMK-…    # SEND_ENCRYPTED=1
smoke: encrypted_send=ok marker=SMK-… event_id=$…   # or =failed / =skipped
smoke: summary restore=<r> login=<r> sync=<r> rooms=<N>
       encrypted_rooms=<M> spaces=<S> timeline_events=<T>
       encrypted_events=<E> decrypted_events=<D> undecryptable=<U>
       send=<r> encrypted_send=<r> expect_text=<seen|not_seen|n/a>
       supports_e2ee=<bool>
```

Exit codes:

- `0` — login ok, initial sync ok, at least one joined room, plus
  `send=ok`/`skipped`/`blocked` when `LIGHTNING_TEST_SEND=1`, plus
  `encrypted_send=ok`/`skipped` when `LIGHTNING_TEST_SEND_ENCRYPTED=1`,
  plus `expect_text=seen` when `LIGHTNING_TEST_REQUIRE_EXPECT=1`.
  `key_backup=not_configured` / `attempted` / `ok` / `failed` are
  advisory and never affect the exit code by themselves.
- `2` — missing env vars OR `--rust-sdk-smoke-test` used without
  `--backend=rust`, OR the current build has no Rust backend.
- `10` — login failed.
- `11` — initial sync did not complete within 30 s.
- `12` — zero joined rooms observed.
- `13` — `LIGHTNING_TEST_SEND=1` was set and the probe send
  actually failed (`send=failed`) or timed out (`send=timeout`).
  `send=skipped` (no unencrypted room found) and `send=blocked`
  (target is encrypted / a Space / not in synced rooms) are
  reported honestly but do NOT trigger a non-zero exit — they are
  the expected outcome on an all-encrypted account.
- `14` — `LIGHTNING_TEST_REQUIRE_EXPECT=1` was set and the expected
  marker was not seen in any decrypted timeline body.
- `15` — `LIGHTNING_TEST_SEND_ENCRYPTED=1` was set and the encrypted
  send probe actually failed (`encrypted_send=failed`) or timed out
  (`encrypted_send=timeout`). Skipped outcomes stay exit 0.

The harness runs headless via `QCoreApplication`, so no display is
needed and it never prompts. Total wall-clock budget is 60 s.

**Session-safety guarantee.** The harness constructs
`RustSdkMatrixClient(nullptr, nullptr)` — passing a null
`SettingsManager` so the SDK's login response can never overwrite
the interactive user's cached session in QSettings/SecretStore
(access token, syncToken, homeserver). In persistent smoke mode,
the only persistent credential is the smoke-only MatrixSession sidecar
under the account app-data directory; it exists solely to restore the
same SDK device for encrypted receive testing.

**Default temporary store.** Without
`LIGHTNING_TEST_PERSISTENT_STORE=1`, each smoke run gets its own
`QTemporaryDir` under `/tmp/lightning-rust-sdk-smoke-XXXXXX/` and
passes that path into `RustSdkMatrixClient::setStorePathOverride`.
Back-to-back temporary password logins therefore cannot inherit a
stale device id. The temp directory is removed on exit (best-effort —
SDK background threads may still be writing when the process shuts
down, but the OS cleans up on next boot). Temporary mode never reads
or writes the persistent Rust SDK store.

Temporary mode announces:

```
smoke: store=temporary
smoke: store_path=/tmp/lightning-rust-sdk-smoke-XXXXXX/matrix-rust-sdk-store
smoke: store_exists=no
smoke: store_account_match=unknown
smoke: device_id=unknown
smoke: supports_e2ee=false
smoke: restore=skipped
```

Fresh temp store means `store_exists=no` on every run; the SDK
creates the directory as part of login.

**Persistent store mode.** With `LIGHTNING_TEST_PERSISTENT_STORE=1`,
the harness uses the same store path as the interactive Rust backend:

```
${XDG_DATA_HOME}/MatrixClient/matrix-client/<safeUserId>/matrix-rust-sdk-store/
```

It also configures Rust to save and restore a smoke-only session file:

```
${XDG_DATA_HOME}/MatrixClient/matrix-client/<safeUserId>/matrix-rust-sdk-smoke-session.json
```

The session file contains a Matrix access token and is created with
0600 permissions on Unix. It is never printed, never committed, and
never routed through the interactive SecretStore. Persistent mode
prints:

```
smoke: store=persistent
smoke: store_path=<account-store>
smoke: store_exists=yes|no
smoke: store_account_match=yes|no|unknown
smoke: device_id=<redacted-or-unknown>
smoke: restore=attempted|ok|not_available|failed
```

First persistent run usually prints `restore=not_available`, performs
password login, creates the SDK store, and writes the smoke session
sidecar. Second persistent run should print `store_account_match=yes`,
`restore=attempted`, then `restore=ok`, and use the same redacted
device id.

If matrix-sdk reports the known account/device-store mismatch, the
harness prints `store_reset=account_device_mismatch`, deletes only
that account's `matrix-rust-sdk-store/` plus the smoke session sidecar,
and retries password login once. It never deletes `cache.sqlite`,
QSettings, or SecretStore entries.

Persistent encrypted receive workflow:

```bash
# 1. Create or restore a stable Lightning SDK device.
LIGHTNING_TEST_HOMESERVER=https://matrix.smetonis.net \
LIGHTNING_TEST_USER='@test:matrix.smetonis.net' \
LIGHTNING_TEST_PASSWORD='<password-from-env-only>' \
LIGHTNING_TEST_PERSISTENT_STORE=1 \
nix develop -c ./build-rust/matrix-client --backend=rust --rust-sdk-smoke-test

# 2. In Element Classic, approve the new login if prompted, then send a
#    harmless marker into the encrypted test room.

# 3. Verify Element Classic -> Lightning encrypted receive.
LIGHTNING_TEST_HOMESERVER=https://matrix.smetonis.net \
LIGHTNING_TEST_USER='@test:matrix.smetonis.net' \
LIGHTNING_TEST_PASSWORD='<password-from-env-only>' \
LIGHTNING_TEST_PERSISTENT_STORE=1 \
LIGHTNING_TEST_EXPECT_TEXT='<marker-sent-from-element>' \
LIGHTNING_TEST_REQUIRE_EXPECT=1 \
nix develop -c ./build-rust/matrix-client --backend=rust --rust-sdk-smoke-test
```

Expected success is `expect_text=seen`, `decrypted_events>0`, and
exit 0. If keys are still unavailable, the run reports
`expect_text=not_seen`; with `LIGHTNING_TEST_REQUIRE_EXPECT=1` that
exits 14. Undecryptable history alone is not fatal.

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
   password. Watch the status bar:
   `Not connected` → `Connecting…` → `Loading rooms…` → `Connected`
   (v0.4.8).
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
9. **Logout**. Rooms footer or Settings → Sign out. With the Rust backend,
   sync is cancelled/joined, Matrix logout is attempted, the old callback
   generation is invalidated, the client handle is released, and only this
   account's saved Lightning session plus Rust SDK store are removed. Verify
   the Login screen remains clean: no late `M_UNKNOWN_TOKEN`, no repopulated
   room/timeline state, and a later password login does not reuse the old
   device store. HTTP behavior remains its existing server-logout + local
   session/cache clear flow.

### Reproducible test account

The project maintainer keeps a **disposable** test account on
`https://matrix.smetonis.net` with MXID `@test:matrix.smetonis.net`.
It's pre-seeded with two joined rooms — one inside a private Space,
one outside — and messages already sent by the account. Use it to
exercise Space filtering, the "Other rooms" fallback, and session
restore end to end.

The password is **not** in this repo, in scripts, in shell history,
or in any commit — treat it as user-supplied secrets. Enter it into
the login field interactively, log out at the end of the session,
and rotate it if you suspect exposure. `matrix.http:` diagnostic
logs never print tokens or passwords.

Rooms expected in the room list after login as `@test`:

- "All rooms" chip shows **2** rooms.
- The private-Space chip shows **1** room.
- "Other rooms" shows the remaining **1** room.

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

- Settings shows the Rust backend with initial E2EE support enabled through
  matrix-sdk. Lightning does not implement Matrix cryptography itself.
- Login against a real homeserver. Expected path:
  `Not connected` → `Connecting…` → `Loading rooms…` → `Connected`.
- The Rust SDK store is created at
  `${XDG_DATA_HOME}/MatrixClient/matrix-client/<safeUserId>/matrix-rust-sdk-store/`.
  This directory is separate from the C++ `cache.sqlite`.
- Joined rooms should appear after the first SDK sync callback.
- Basic text/notice/emote timeline events should appear as SDK events
  arrive. If the SDK emits a decrypted encrypted text event, Lightning
  shows the decrypted body; otherwise E2EE is not claimed and no fake
  placeholder is converted into plaintext.
- Sending plain text in an unencrypted room should create a local echo
  and then replace it with the server event id.
- Sending into an encrypted room is delegated to matrix-sdk. Decrypted bodies
  may be displayed in memory, but encrypted `TimelineEvent` rows remain
  excluded from `cache.sqlite`.
- Replies, edits, redactions, reactions, media, pagination, typing,
  read receipts, and Space child hierarchy are still HTTP-only or
  missing in Rust.

### `--reset-crypto-store`

```bash
./build/matrix-client --reset-crypto-store
```

Recognised in the pre-flight parser (works with or without the
`build-rust` binary — no display needed). It deletes only per-account
Rust SDK store directories. As of v0.5.0-prep+5 the scanner uses the
same `matrix::app_data::allRoots()` helper the runtime backend uses,
so it inspects **both** the current
`QStandardPaths::AppLocalDataLocation` layout AND the pre-fix
"no-org-prefix" layout:

```
matrix-client --reset-crypto-store

Scanning: /home/…/.local/share/MatrixClient/matrix-client
Scanning: /home/…/.local/share/matrix-client

Deleted:
    /home/…/.local/share/MatrixClient/matrix-client/<safeUserId>/matrix-rust-sdk-store
```

If neither root contains a store, the output ends with
`No Rust SDK store directories found.` and exit code 0.

`--reset-crypto-store` NEVER removes `${safeUserId}/cache.sqlite`,
never touches QSettings session metadata, and never touches
SecretStore access tokens. Exit code 0 when nothing is found or
deletion succeeds; exit code 3 if a store directory cannot be
removed.

This CLI flag is a bulk legacy repair tool: it scans every Lightning account
slug under the known current and legacy roots and removes Rust store
directories only. It does **not** know the Login form identity and does not
clear QSettings/SecretStore metadata. For the normal v0.5.5 mismatch flow,
prefer the Login-screen **Reset local Lightning session** action. That action
derives one canonical account from the entered homeserver and MXID/localpart,
releases a lingering Rust handle, removes that account's SDK store and
existing smoke-session sidecar, and clears only that account's saved
Lightning session. It never removes `cache.sqlite` or unrelated account data.

**Crypto-store account/device mismatch.** If the Rust SDK login
fails with

```
failed to read or write to the crypto store the account in the
store doesn't match the account in the constructor: expected
@user:hs:DEV1, got @user:hs:DEV2
```

you have a stale SDK store from a previous login for that MXID. On the Login
screen, enter the homeserver and full MXID/localpart, choose **Reset local
Lightning session**, and retry. The fields remain populated. Use the bulk
`--reset-crypto-store` tool only for legacy/headless repair.
Pre-v0.5.0-prep+5 the reset scanned the wrong root and could leave
a stale store behind — that specific bug is fixed.

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

### `matrix.cache: NOT NULL constraint failed: …` after login

Fixed in v0.4.8. Root cause: Qt's QSQLITE driver binds a default-
constructed / null `QString` as SQL `NULL`. Two columns added in
v0.4.5 — `rooms.child_room_ids` and `events.thread_root_id` — carry
`NOT NULL` constraints, so any save for a non-thread event or a
non-Space room fired the constraint on every write.

Fixes shipped:

- `textNonNull(const QString&)` helper in `src/storage/CacheStore.cpp`
  coerces empty / null strings to a non-null empty `QString` before
  binding.
- Idempotent repair on schema-ensure: `UPDATE rooms SET
  child_room_ids='' WHERE child_room_ids IS NULL` (same for
  `events.thread_root_id`). No user data loss; no cache wipe.

If the constraint errors somehow persist after this pass, delete
`${XDG_DATA_HOME}/MatrixClient/matrix-client/<safeUserId>/cache.sqlite`
and let the next `/sync` refill the cache from scratch.

### `QML Column: Cannot specify top, bottom, verticalCenter, fill or centerIn anchors for items inside Column.`

Fixed in v0.4.8. `qml/MessageDelegate.qml` used to place a
`MouseArea { anchors.fill: parent }` as a direct child of a
`Column`. Column controls its children's positions and forbids
those five anchor names — QML logged the warning on every message.
The catcher is now a `HoverHandler` (`actionsHover`), which works
inside `Column` without needing explicit geometry.

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
