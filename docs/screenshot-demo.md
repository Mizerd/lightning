# Screenshot / demo mode (development only)

A development-only launch mode that boots the **real** Lightning UI on the
in-memory mock backend with deterministic fake data, so you can take clean
promotional screenshots without a Matrix account, a homeserver, or the network.

It is **development only** and cannot exist in a shipped binary (see
[Production exclusion](#production-exclusion)).

## What it is

- The real application: real themes, Settings, room list, timeline, composer,
  threads, panels, responsive layout — populated by the deterministic
  `MockMatrixClient`, not a screenshot-only mock UI.
- No network, no login form, no Matrix credentials, no real homeserver, no
  crypto store, no real account store.
- Storage is isolated: it never reads or writes your real Lightning config.

## Quick start

```sh
scripts/run-screenshot-demo.sh
```

This configures a dedicated `build-demo/` tree with the demo compile option on,
builds `matrix-client`, points `XDG_{DATA,CONFIG,CACHE}_HOME` at an isolated
demo directory, and launches straight into the mock account.

Reset the isolated demo profile (safe, validated deletion):

```sh
scripts/run-screenshot-demo.sh --reset
```

Pass extra application arguments after a literal `--`:

```sh
scripts/run-screenshot-demo.sh -- --backend=mock
```

## Manual build & launch

```sh
nix develop -c cmake -S . -B build-demo -G Ninja -DLIGHTNING_ENABLE_SCREENSHOT_DEMO=ON
nix develop -c cmake --build build-demo --target matrix-client
nix develop -c ./build-demo/matrix-client --screenshot-demo
```

`--build-info` reports the mode for that build:

```
screenshot_demo_compiled: true
```

## Demo controls

- The window title shows **“Lightning — Screenshot Demo.”**
- A floating **“● Screenshot Demo — fake data”** badge sits at the top.
- **Hide** the badge (its Hide button, or `Ctrl+Shift+D`) for a clean final
  screenshot; `Ctrl+Shift+D` restores it. Hiding leaves no layout gap (it is an
  overlay, not part of any layout).

Everything else is the normal application: switch themes and appearance through
**Settings → Appearance**, navigate Spaces/rooms, open threads and profiles,
resize the window for narrow/wide layouts — all against the fake data.

## Isolation guarantees

- **Storage.** The app runs with a distinct `applicationName`
  (`matrix-client-screenshot-demo`), which redirects every `QSettings` store
  (theme/appearance/account registry, GIF favourites, insecure token fallback)
  to a separate file. The launcher additionally overrides
  `XDG_{DATA,CONFIG,CACHE}_HOME`. The mock backend touches no other store
  (no `cache.sqlite`, Rust SDK store, or SecretStore/libsecret).
- **Network.** The mock backend performs zero network I/O; there is no
  homeserver URL and no Matrix client that connects.
- **Credentials.** Auto-login uses a fictional account
  (`@alex:lightning.chat`) with no real token.

## Production exclusion

The mode is impossible to reach in a shipped binary:

- The CMake option `LIGHTNING_ENABLE_SCREENSHOT_DEMO` defaults **OFF**.
- Combining it with `LIGHTNING_RUST_ONLY` (the release/packaging configuration)
  is a **fatal CMake error**. A release binary also excludes the mock backend
  entirely, so the demo’s data source is not even compiled in.
- When the option is off, `--screenshot-demo` is **rejected in preflight**
  (exit code 2) before any UI, network, or store access, and `--build-info`
  reports `screenshot_demo_compiled: false`.
- The `screenshot-demo-exclusion` test runs the real binary and asserts all of
  the above; CI can additionally grep `--build-info` for
  `screenshot_demo_compiled: false` on every release artifact.

## Adding fictional scenarios and assets

The fake scene is produced by `MockMatrixClient::seedMockData()`. When enriching
it for the demo, keep new content behind the demo path so the shared mock
fixtures the tests assert on stay unchanged, use clearly fictional domains
(`lightning.chat`, `workplace.example`, `matrix.example` — never a real
homeserver), deterministic timestamps, and only locally-bundled, clearly-licensed
demo assets that release packages exclude.

## Roadmap (not yet implemented)

The current foundation launches an isolated, production-excluded demo on the
existing mock scene. Planned follow-ups: an enriched multi-account/room/media
dataset, an in-app scenario/theme/window-size control panel, and bundled demo
media fixtures.
