# Screenshot / demo mode (development only)

A development-only launch mode that boots the **real** Lightning UI on the
in-memory mock backend with three deterministic fictional accounts, so you can
take clean promotional screenshots without a Matrix account, a homeserver, or
the network.

It is **development only** and cannot exist in a shipped binary (see
[Production exclusion](#production-exclusion)).

## What it is

- The real application: real themes, Settings, room list, timeline, composer,
  threads, panels, responsive layout — populated by the deterministic
  `MockMatrixClient`, not a screenshot-only mock UI.
- Three fictional accounts in the **real account switcher**, each with its own
  Spaces, rooms, DMs, media and invites.
- Local, bundled, license-clear media fixtures so image / video-poster / GIF /
  avatar rows render as real pictures.
- An in-app **control panel** (scenario / account / room / theme / appearance /
  window-size selectors, one-click scenario activation, reset).
- No network, no login form, no Matrix credentials, no real homeserver, no
  crypto store, no real account store, no libsecret / production SecretStore.

## Quick start

```sh
scripts/run-screenshot-demo.sh
```

This configures a dedicated `build-demo/` tree with the demo compile option on,
builds `matrix-client`, points `XDG_{DATA,CONFIG,CACHE}_HOME` at an isolated
demo directory, and launches straight into the primary demo account.

Reset the isolated demo profile (safe, validated deletion — only ever removes an
absolute, marker-bearing `lightning-screenshot-demo` directory under `$HOME`):

```sh
scripts/run-screenshot-demo.sh --reset
```

## Command-line options

The launcher validates every value and rejects unknown arguments; it then passes
matching development-only `--demo-*` flags to the binary (a production binary has
no such flags — see [Production exclusion](#production-exclusion)).

| Launcher flag | Effect |
|---|---|
| `--scenario <id>` | Activate a screenshot scenario on launch (validated) |
| `--account <personal\|work\|community>` | Start on that fictional account |
| `--theme <name>` | Force a theme (e.g. `ocean`, `midnight`, `violet`, `dark`) |
| `--appearance <light\|dark\|system>` | Force light/dark/match-system |
| `--size <WxH\|narrow\|wide>` | Set the window size |
| `--hide-controls` | Start with the demo controls hidden (Ctrl+Shift+D restores) |
| `--reset` | Remove the isolated demo profile |
| `-- <args…>` | Pass extra arguments straight to the app |

Combine freely:

```sh
scripts/run-screenshot-demo.sh \
  --scenario main-chat --theme ocean --size 1440x900 --hide-controls
```

The equivalent application flags (development builds only; rejected in a normal
build) are `--demo-scenario`, `--demo-account`, `--demo-theme`,
`--demo-appearance`, `--demo-size`, and `--demo-hide-controls`.

## The demo accounts

All identities are fictional `*.example` and every timestamp is anchored to a
fixed clock (Thursday 23 July 2026), so screenshots reproduce across launches.
Switching accounts uses the **real** account switcher (the rail avatar → the
account popover, or the panel's Account selector); each account keeps its own
selected room and local mutations across a switch, and Reset returns all three
to their deterministic initial state.

### Alex Morgan — `@alex:lightning.example` (`lightning.example`) — personal

Spaces: **Friends**, **Creative Studio**, **Lightning Community**. Rooms include
Design Lounge, Weekend Plans, Photography, Music Discovery, Lightning
Development, Release Announcements, the Maya Chen (encrypted) and Jordan Lee DMs,
Product Feedback (poll), and a Founders Lounge invite. For the main promotional
screenshots.

### Taylor Reed — `@taylor:workplace.example` (`workplace.example`) — work

Spaces: **Product**, **Engineering**, **Company**. Rooms: Project Aurora,
Product Design, Engineering, Release Planning, Company Announcements, Team Lounge
(muted), the Sam Rivera encrypted DM (with a mention), Incident Review, and a
Leadership Sync invite. For professional workspace screenshots.

### Nova — `@nova:community.example` (`community.example`) — community

Spaces: **Open Source**, **Community**, **Support**. Rooms: General (public),
Development (thread-heavy), Support (support question), Showcase (media),
Feature Requests (poll), Off Topic, the Priya Shah DM, Maintainers (encrypted),
and a Translators invite. For open-source and support screenshots.

## Scenarios

Each scenario performs ALL of its navigation deterministically (account, Space,
room, thread/settings/switcher page, recommended theme/appearance/window size,
typing, and demo-controls state). Activate one from the panel's Scenario
selector or with `--scenario <id>`.

| id | Account | Opens | Recommended |
|---|---|---|---|
| `home-overview` | Alex | Design Lounge | Indigo Night · 1440×900 |
| `main-chat` | Alex | Design Lounge (reply, reactions, mention, edit, image) | Midnight · 1440×900 |
| `direct-message` | Alex | Maya Chen (encrypted DM) | Midnight · 1280×800 |
| `development` | Alex | Lightning Development (code, file, thread root) | Indigo Night · 1440×900 |
| `media-gallery` | Alex | Photography (landscape/portrait/square/artwork, video poster, GIF, audio, file) | Indigo Night · 1440×900 |
| `thread-view` | Alex | Lightning Development + open thread panel | Purple Dusk · 1600×1000 |
| `poll` | Alex | Product Feedback (real poll widget) | Indigo Night · 1280×800 |
| `settings-themes` | Alex | Settings → Appearance (real page) | Indigo Night · 1280×800 |
| `account-switching` | Alex | Real account switcher popover | Indigo Night · 1280×800 |
| `security` | Alex | Settings → Privacy & security (real page) | Indigo Night · 1280×800 |
| `invite` | Alex | Founders Lounge invite (real invite UI) | Indigo Night · 1280×800 |
| `work-overview` | Taylor | Project Aurora | Moss Light · 1440×900 |
| `community-overview` | Nova | General | Indigo Night · 1440×900 |
| `responsive-chat` | Alex | Maya Chen at narrow width | Indigo Night · narrow (760×900) |

## Control panel

The floating panel (top-right; an overlay, so hiding it leaves no gap) has:

- **Scenario / Account / Room / Theme / Appearance / Window-size** selectors.
- **Typing** and **Unread badges** toggles.
- **Open Settings**, **Account switcher**, **Open thread**, **Reset scenario**,
  **Reset all**, **Hide controls** actions.
- A status line: *Demo account · Scenario · Size*.
- A compact collapsed pill; **Expand/collapse**; **Hide entirely**;
  **Ctrl+Shift+D** restores.

Theme and window changes go through the SAME settings/controller paths as the
normal Settings interface — the panel does not duplicate Settings. It is present
only when `app.screenshotDemoActive` is true (a demo build), never in production.

### Window-size presets

`1024x768`, `1280x800`, `1440x900`, `1600x1000`, `1920x1080`, `900x900`,
`narrow` (760×900), `wide` (1720×960). The panel and `--size` both apply them to
the real application window; resizing afterwards still works.

## Media fixtures

Small, deterministic, **abstract** raster images (gradients + simple geometry)
generated entirely by `scripts/generate-demo-media.sh` — no photographs, no real
people, no third-party/commercial artwork, no network. They live in
`resources/screenshot-demo/` and are bundled into the QML module **only** when
`LIGHTNING_ENABLE_SCREENSHOT_DEMO=ON`, so releases exclude them. The mock serves
them through the real `MediaBridge` → `MediaImageProvider` path (the same one the
Rust backend uses), so image, video-poster, GIF and avatar rows render through
the production delegates with no network, no mxc fetch and no token.

| Fixture | Type | Used for |
|---|---|---|
| `avatar-*.png` (10) | PNG 224×224 | Account + member avatars |
| `coast.png` | PNG 8:5 | Landscape image |
| `portrait.png` | PNG 2:3 | Portrait image |
| `square.png` | PNG 1:1 | Square image |
| `artwork.png` | PNG 1:1 | Abstract illustration |
| `shot-timeline.png` | PNG 8:5 | In-chat "hero" image |
| `palette.png` | PNG 1:1 | Shared image in the DM |
| `timelapse.png` | PNG 16:9 | Video poster |
| `loop.gif` | GIF 1:1 | Animated GIF preview |
| `release-notes.txt` | text | Document attachment |

Regenerate with `scripts/generate-demo-media.sh` (requires ImageMagick).

## Local interactions

Everything is local and reset-restorable: account switching, Space/room
navigation, opening threads/profiles/room details, poll voting, reaction
toggles, invite accept/reject, mark read/unread, typing/unread toggles, search,
and typing/sending a local fake message. No action ever reaches a network
backend. **Reset scenario** restores the current account; **Reset all** restores
all three accounts and the panel toggles.

## Isolation guarantees

- **Storage.** A distinct `applicationName`
  (`matrix-client-screenshot-demo`) redirects every `QSettings` store to a
  separate file; the launcher additionally overrides
  `XDG_{DATA,CONFIG,CACHE}_HOME`. The mock touches no other store (no
  `cache.sqlite`, Rust SDK store, or crypto store).
- **SecretStore / libsecret.** In demo mode the app constructs an **in-memory**
  SecretStore instead of the production libsecret/keychain store, and
  `beginScreenshotDemo` asserts (fail-closed) that no secure store was
  initialized. The three demo accounts are registered as non-secret metadata
  only — no token is ever stored, and libsecret is never touched.
- **Network.** The mock backend performs zero network I/O.
- **Credentials.** Auto-login uses fictional accounts with no real token.

## Production exclusion

The mode is impossible to reach in a shipped binary:

- The CMake option `LIGHTNING_ENABLE_SCREENSHOT_DEMO` defaults **OFF**.
- Combining it with `LIGHTNING_RUST_ONLY` (the release configuration) is a
  **fatal CMake error**; a release binary also excludes the mock backend and the
  demo media resources entirely.
- When the option is off, `--screenshot-demo` (and every `--demo-*` flag) is
  **rejected in preflight** (exit 2) before any UI, network or store access, and
  `--build-info` reports `screenshot_demo_compiled: false`.
- The `screenshot-demo-exclusion` test runs the real binary and asserts all of
  the above.

## Screenshot recipes

Main chat:

```sh
scripts/run-screenshot-demo.sh \
  --scenario main-chat --theme ocean --size 1440x900 --hide-controls
```

Media:

```sh
scripts/run-screenshot-demo.sh \
  --scenario media-gallery --theme midnight --size 1440x900 --hide-controls
```

Thread:

```sh
scripts/run-screenshot-demo.sh \
  --scenario thread-view --theme violet --size 1600x1000 --hide-controls
```

Settings:

```sh
scripts/run-screenshot-demo.sh --scenario settings-themes --size 1280x800
```

Account switching:

```sh
scripts/run-screenshot-demo.sh --scenario account-switching --size 1280x800
```

Responsive:

```sh
scripts/run-screenshot-demo.sh --scenario responsive-chat --size narrow --hide-controls
```
