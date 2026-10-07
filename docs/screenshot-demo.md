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
builds `lightning-matrix`, points `XDG_{DATA,CONFIG,CACHE}_HOME` at an isolated
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
| `--appearance <light\|dark\|system\|system-dark>` | Force light/dark/match-system; `system-dark` is match-system on a desktop that asks for dark (a fresh install's dark look) |
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
`--demo-appearance`, `--demo-size`, and `--demo-hide-controls`, plus
`--demo-capture=PATH.png[,delayMs]`, which grabs the window's contents
(`QQuickWindow::grabWindow`, no decoration) after the delay and quits.

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
| `home-overview` | Alex | Design Lounge | Storm · 1440×900 |
| `main-chat` | Alex | Design Lounge (reply, reactions, mention, edit, image) | Storm · 1440×900 |
| `direct-message` | Alex | Maya Chen (encrypted DM) | Storm · 1280×800 |
| `development` | Alex | Lightning Development (code, file, thread root) | Storm · 1440×900 |
| `media-gallery` | Alex | Photography (landscape/portrait/square/artwork, video poster, GIF, audio, file) | Storm · 1440×900 |
| `thread-view` | Alex | Lightning Development + open thread panel | Storm · 1600×1000 |
| `poll` | Alex | Product Feedback (real poll widget) | Storm · 1280×800 |
| `settings-themes` | Alex | Settings → Appearance (real page) | Indigo Night · 1280×800 |
| `account-switching` | Alex | Real account switcher popover | Storm · 1280×800 |
| `security` | Alex | Settings → Privacy & security (real page) | Storm · 1280×800 |
| `invite` | Alex | Founders Lounge invite (real invite UI) | Storm · 1280×800 |
| `work-overview` | Taylor | Project Aurora | Storm · 1440×900 |
| `community-overview` | Nova | General | Storm · 1440×900 |
| `responsive-chat` | Alex | Maya Chen at narrow width | Storm · narrow (760×900) |
| `menu-message` | Alex | Design Lounge + message context menu | Storm · 1440×900 |
| `menu-room` | Alex | Design Lounge + room context menu | Storm · 1440×900 |
| `find-in-room` | Alex | Design Lounge + the floating find-in-room card, pre-filled (`poster`) | Storm · 1600×1000 |
| `quick-switcher` | Alex | Design Lounge + quick switcher (plain mode) | Storm · 1280×800 |
| `quick-switcher-command` | Alex | Design Lounge + quick switcher (command mode, `>theme`) | Deep Teal · 1280×800 |
| `emoji-picker` | Alex | Design Lounge + emoji picker (seeded recents) | Storm · 1280×800 |
| `gif-picker` | Alex | Weekend Plans + GIF picker (local catalogue) | Storm · 1280×800 |
| `member-profile` | Alex | Design Lounge + member profile popover | Storm · 1280×800 |
| `mention-popup` | Alex | Design Lounge + mention popup (`ma` → Maya Chen) | Storm · 1280×800 |
| `trust-card` | Alex | Settings → Sessions (own-account trust card) | Storm · 1280×800 |
| `new-conversation` | Alex | Design Lounge + New conversation dialog | Storm · 1280×800 |
| `settings-search` | Alex | Settings + search focused (`security`) | Storm · 1280×800 |
| `invite-people` | Alex | Design Lounge + Invite people dialog | Storm · 1280×800 |
| `create-poll` | Alex | Design Lounge + Create poll dialog | Storm · 1280×800 |
| `store-hero` | Alex | Design Lounge, every room in the Classic list (store default shot, README) | Indigo Night · 1920×1080 |
| `store-thread` | Alex | Design Lounge with its thread open | Indigo Night · 1920×1080 |
| `theme-editor` | Alex | Custom theme editor on a seeded gradient conversation background | Indigo Night · 1600×1000 |
| `settings-sound` | Alex | Settings → Sound & video (real page) | Indigo Night · 1440×900 |
| `verification` | Alex | Maya Chen DM + the verification dialog at its emoji step (staged: no SDK flow, no keys, no trust change) | Indigo Night · 1440×900 |

The `trust-card` row's theme is Indigo Night for the surrounding Settings
chrome only — the TrustCard itself is a brand-fixed design (its palette does
not follow the active theme), so the card looks the same regardless of which
theme the row pins.

### Demo-only popup signals

Thirteen of the rows above don't have a stable QML-global handle the controller
can call directly (a context menu, a picker, a popover, or a dialog — each
instantiated once per view, not once per app). For those, `ScreenshotDemoController`
exposes development-only signals that fire once the scenario's account/room/
section navigation has settled (the same pattern `accountSwitcherRequested`
already uses for the account switcher). Each surface owns wiring a `Connections`
block (`target: app.demo`, `enabled: app.screenshotDemoActive`) to its own
popup/dialog instance — the controller only says "open now"; it never reaches
into QML to pick which delegate/row/instance to target.

| Signal | Scenario | Opens |
|---|---|---|
| `demoOpenMessageContextMenu()` | `menu-message` | A message's context menu |
| `demoOpenRoomContextMenu()` | `menu-room` | A room-list row's context menu |
| `demoOpenFindBar(query)` | `find-in-room` | The in-room find card, pre-filled with `query` |
| `demoOpenQuickSwitcher(query)` | `quick-switcher`, `quick-switcher-command` | The quick switcher, pre-filled with `query` (`""` = plain mode, a leading `>` = command mode) |
| `demoOpenEmojiPicker()` | `emoji-picker` | The emoji picker (recents pre-seeded — see below) |
| `demoOpenGifPicker()` | `gif-picker` | The GIF picker (local catalogue pre-seeded — see below) |
| `demoOpenMemberProfile()` | `member-profile` | The member profile popover |
| `demoOpenMentionPopup(prefix)` | `mention-popup` | The mention popup, pre-filled with `prefix` |
| `demoOpenTrustCard()` | `trust-card` | (Settings → Sessions; a hook for scrolling/highlighting the card once embedded) |
| `demoOpenNewConversation()` | `new-conversation` | The New conversation dialog |
| `demoFocusSettingsSearch(query)` | `settings-search` | The Settings search field, focused and pre-filled with `query` |
| `demoOpenInvitePeople()` | `invite-people` | The Invite people dialog |
| `demoOpenCreatePoll()` | `create-poll` | The Create poll dialog |
| `demoOpenThemeEditor(role)` | `theme-editor` | The custom theme editor, opened on `role` (a seeded "Lakeside" theme with a gradient conversation background) |

Two rows seed local, demo-only, idempotent state before opening (SettingsManager
recents / the local GIF favorites store — both are ordinary application-local
persistence, never real Matrix or provider data):

- `emoji-picker` records a fixed, deterministic set of five recent emoji.
- `gif-picker` seeds a **browsable local catalogue** and one locally-saved GIF.

  Until v0.6.7 this was the one surface the demo could not photograph: with no
  network, no provider key and a mock transport reporting `available() ==
  false`, the picker could only ever render "GIFs are unavailable on this
  backend", and the single seeded favourite pointed at a `media.giphy.example`
  URL that never resolves. That broken-thumbnail state used to be documented
  here as accepted.

  Now `GifSearchController::seedDemoCatalogue()` (compiled only under
  `LIGHTNING_ENABLE_SCREENSHOT_DEMO`) hands the picker a fixed set of rows
  pointing at bundled animated fixtures, and reports the provider as ready and
  configured so no overlay covers the grid. Both provider tabs are browsable,
  every tile renders a real moving picture with a true source tag and byte
  size, and the Saved tab gets a real locally-saved GIF written through the
  normal `starBytes()` path into the isolated demo profile.

  **No request is ever issued** and no validation is relaxed: the rows live in
  the in-memory browse model only, and the persisted collections still accept
  `https` exclusively. Regenerate the fixtures with
  `scripts/generate-demo-gifs.sh` — they are derived from the stills already in
  `resources/screenshot-demo/`, so no new third-party asset enters the tree.

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

Small, deterministic images generated entirely by the scripts below: drawn
portraits for the avatars, flat illustrations for the scenes the conversations
share, and abstract gradients for the rest. No photographs, no real people, no
third-party/commercial artwork, no network. They live in
`resources/screenshot-demo/` and are bundled into the QML module **only** when
`LIGHTNING_ENABLE_SCREENSHOT_DEMO=ON`, so releases exclude them. The mock serves
them through the real `MediaBridge` → `MediaImageProvider` path (the same one the
Rust backend uses), so image, video-poster, GIF and avatar rows render through
the production delegates with no network, no mxc fetch and no token.

| Fixture | Type | Used for |
|---|---|---|
| `avatar-*.png` (10) | PNG 224×224 | Account + member avatars (drawn portraits) |
| `lake.png` | PNG 8:5 | The painting shared in Design Lounge |
| `forest-trail.png` | PNG 8:5 | The sketch shared in the Maya Chen DM; Weekend Plans' avatar |
| `fallen-leaf.png` | PNG 8:5 | Photography's avatar |
| `share-poster.jpg` | JPEG 16:9 | The staged call's shared screen |
| `lightning-icon.png` | PNG 224×224 | Lightning Community and Release Announcements avatars |
| `coast.png` | PNG 8:5 | Landscape image; the Friends Space avatar |
| `portrait.png` | PNG 2:3 | Portrait image |
| `square.png` | PNG 1:1 | Square image |
| `artwork.png` | PNG 1:1 | Abstract illustration; Music Discovery's avatar |
| `shot-timeline.png` | PNG 8:5 | (unused since the store screenshots of 0.10.1) |
| `palette.png` | PNG 1:1 | The Creative Studio Space avatar |
| `timelapse.png` | PNG 16:9 | Video poster |
| `loop.gif` | GIF 1:1 | Animated GIF preview |
| `release-notes.txt` | text | Document attachment |

Regenerate the abstract images with `scripts/generate-demo-media.sh`, the
avatars with `scripts/generate-demo-avatars.py`, the scenes with
`scripts/generate-demo-scenes.py` and the shared screen with
`scripts/generate-demo-share.py` (run after the scenes; it paints the lake onto
the poster). All need ImageMagick; the Python ones need its librsvg delegate, and
the shared screen Noto Sans.

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

## AppStream / Flathub captures (`docs/screenshots/flathub/`)

These are a **separate set** from `docs/screenshots/*.png`, which
`docs/features.md` uses. The README has its own picture,
`docs/screenshots/readme-hero.png` (below).

The metainfo (`packaging-ci/packaging/common/lightning.metainfo.xml`) references
this directory at an **immutable tag**, never a branch — Flathub requires that —
so the release commit re-points every URL at the tag it is about to receive, and
the files must be COMMITTED in that commit (`test-metainfo-consistency.py` checks
both). A file that is new in a release 404s until its tag exists.

### The 0.10.1 set (2026-10-07)

Six renders from this demo mode, captured the way Flathub's quality guidelines
ask: the app window only, with its native decoration, rounded corners and shadow
on a transparent background; a 1000x700 logical window (frame included) at scale
2, so 2000x1400 plus the shadow; and default settings, which on the platform are
Breeze and the light style, so Lightning's "Match system" resolves to Moss Light.

| File | Scenario | Shows |
|---|---|---|
| `01-conversation.png` | `store-hero` | Rail, room list, a conversation with a shared picture, a reply quoting it, reactions, a mention, a thread summary, receipts and typing |
| `02-call.png` | `call-screen-share` | A four-person call with Maya's screen share on the stage |
| `03-threads.png` | `store-thread` | The poster-copy thread, opened from Design Lounge |
| `04-themes.png` | `theme-editor` | The custom theme editor on a gradient conversation background |
| `05-settings.png` | `settings-themes` | Settings → Appearance with match-system on |
| `06-verification.png` | `verification` | The verification dialog at its emoji step, over an encrypted DM |

Recipe: a private, nested KWin on a virtual framebuffer inside `gui-slot`, so
nothing reaches a real desktop, with KDE's configuration redirected to a scratch
directory so it starts from Breeze defaults. Lightning runs as its Wayland client
and Spectacle grabs the active window with decoration and shadow:

```sh
# inside gui-slot (private XDG_RUNTIME_DIR and session bus);
# XDG_CONFIG_HOME/XDG_DATA_HOME/XDG_CACHE_HOME point at a scratch directory
kwin_wayland --virtual --width 1400 --height 1000 --no-lockscreen \
    --socket wayland-shots --exit-with-session ./session.sh
# session.sh:
kscreen-doctor output.Virtual-0.scale.2
QT_QPA_PLATFORM=wayland lightning-matrix --screenshot-demo \
    --demo-scenario=store-hero --demo-size=998x662 \
    --demo-hide-controls --demo-appearance=system &
sleep 15; QT_QPA_PLATFORM=wayland spectacle -b -n -a -o 01-conversation.png
```

`998x662` is the client area: Breeze adds a 1 px outline and a 36 px title bar,
which makes the frame exactly 1000x700. Pre-dismiss the "drawing on the CPU"
notice in the throwaway profile (`[ui] softwareRendererNoticeDismissedVersion`),
since the virtual output renders with llvmpipe. Use a fresh profile per shot.
Optimise losslessly with `oxipng -o 4 --strip safe`.

### The README picture (`docs/screenshots/readme-hero.png`)

3840x2160, window contents only, the default **dark** look
(`--demo-appearance=system-dark`): the `store-hero` scenario at 1920x1080
logical and device pixel ratio 2, grabbed by `--demo-capture` on a private Xvfb
larger than the window, so the pointer can be parked outside it (otherwise a row
is drawn hovered), with OpenGL through Mesa:

```sh
Xvfb :77 -screen 0 4096x2304x24 -nolisten tcp -noreset &
export DISPLAY=:77 QT_SCALE_FACTOR=2 QT_QPA_PLATFORM=xcb
xdotool mousemove 2 2
lightning-matrix --screenshot-demo --demo-scenario=store-hero \
    --demo-size=1920x1080 --demo-hide-controls --demo-appearance=system-dark \
    --demo-capture=readme-hero.png,10000
```

Wait about ten seconds before a capture: a scenario that opens a thread can
still show "Loading conversation…" at six. Keep the data fictional.
