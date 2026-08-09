# Lightning development guide

This is the authoritative operating guide for Claude Code, Codex, and other
coding agents working in this repository. Inspect the repository before every
task: source and current Git history override stale comments or assumptions.

## 1. Project identity

Lightning is an actively developed native desktop Matrix client. It is Linux
and NixOS first and uses Qt 6, QML, C++20, CMake, a Nix development
environment, and the official Rust Matrix SDK backend.

- Repository: `/home/roksme/git/lightning`
- Remote: `https://gitlab.smetonis.net/Mizerd/lightning.git`
- Development branch: `main` only unless Rokas explicitly requests otherwise
- UI: Qt 6/QML
- Application and Qt bridge: C++
- Real Matrix backend: Rust Matrix SDK through the repository's FFI bridge
- Build system: CMake, with Cargo invoked for the Rust-enabled build

Lightning is not Electron, Tauri, Element Web, a fork of Element Web, or a
webview-based chat client. Do not replace native Qt/QML presentation with a web
frontend.

## 2. Current release and development state

Release facts, verified on 2026-08-04:

- Latest published release: **Lightning 0.6.5** (`v0.6.5` -> `4cdace3`)
- Previous releases: `v0.6.4` -> `e719bbe`, `v0.6.3` -> `97f10b7`,
  `v0.6.2` -> `fe3b85f`, `v0.6.1` -> `86d30b4`, `v0.6.0` -> `2157194`
  (all immutable, unchanged)
- Application version: **0.6.5** in `CMakeLists.txt`, `rust/Cargo.toml`, and
  the Rust/HTTP user agent

Post-release work since the tag, verified on 2026-08-08: `main` is
**thirteen commits past `v0.6.5`**, CTest **87/87 on both trees**, with **no
version bump pending** — none of it is released. In order: `326adff` (release
facts), `329a65c` (scroll regression guard), `c060ef5` (Element-style read
receipts), `21d5fb8` (per-room notification modes promoted to server push
rules), `6fa6378` (QR device verification alongside SAS; the only dependency
change since the release), `225c7b3` (scroll staging/freeze — **later removed
entirely**), `3afc2d0` (receipt-chip placement), `44c29aa` + `52cf6ca` +
`e39439a` (locally-saved GIFs: store, hover star + its own tab, durable
state), `2fe5cb0` (per-branch scroll-trace attribution), `30ee39b`
(receipt-avatar context-lookup fix), `263268b` (runaway prefetch chain and
the staging mechanism removed, −1463 net). Every one of these went through
independent review to `APPROVED`. Section 16 carries what is still open.

Run `git log --oneline v0.6.5..HEAD` rather than trusting this list; it will
go stale the same way the narrative below did.

The narrative below describes the 0.6.2-era checkpoints and has not been
rewritten for every release since. Treat it as background, not as the current
inventory: source and `git log` are authoritative, and this section will be
stale again before it is next read.
- The 2026-07-20 stability checkpoints (`790a75b`..`fe3b85f`) delivered:
  the initial-timeline presentation gate + persistent scroll anchoring
  (bottom-pinned and scrolled-up) with row-scoped Set-diff application;
  the shared Skeleton primitive with typed media placeholders and real
  video/audio/voice/sticker timeline rows; room-member profile hydration
  through `Timeline::fetch_members` with the localpart display-name
  fallback (never a bare MXID label) and explicit avatar states (a loaded
  transparent avatar never sits on the fallback colour); the
  verified-session Megolm bootstrap (`BackupDownloadStrategy::OneShot`, a
  sanitized per-session crypto-bootstrap observer, CryptoBootstrapModel
  status in Settings); the repeated account-switch fix (active-account
  changes re-notify the accounts list); the BootScreen startup state (the
  login form is never instantiated while a saved session restores); the
  favorite-GIF identity fix (choose() resolves the visible model);
  Lightning-styled AppMenu/AppMenuItem popovers for message/room/composer
  actions with ONE shared reaction picker + profile popover per view;
  emoji category buckets and a single shared tone popup; and five
  selectable bundled OFL UI fonts (per-account, Settings → Appearance).
- Older context: the 2026-07-19 checkpoints added
  the design-handoff UI shell (four-pane layout, Moss Light / Indigo Night /
  Deep Teal themes, bundled Manrope + JetBrains Mono), persistent
  multi-account support with safe in-app switching, the eager room-preview /
  avatar-readiness fixes, and the real application icon + desktop entry.
  The 2026-07-20 design-fidelity checkpoints consolidated the three-style
  button system (IconButton), rebuilt the composer to the correction spec
  (one card, formatting toolbar + markdown sending through the SDK),
  completed the Appearance page (featured theme cards, functional
  message-layout modes, text-size scaling — all per-account), and rebuilt
  threads as the exclusive 340px right-side panel. The same-day runtime
  correction pass then fixed GIF key discovery (the app itself reads
  lightning-gif.env: environment > env file > build key), switched the app
  to the flat Basic style with shared themed controls (AppButton,
  SegmentedControl, AppComboBox, AppTextField) across Room Information,
  Settings, the GIF picker, and invites, made Settings a FULL application
  view (rail/room list/timeline/composer hidden while open), and gave the
  right panel one authoritative state where closing a thread collapses to
  None — never back to Room Information. The application version stays
  0.6.2 until an explicitly requested release.
- `matrix-sdk`, `matrix-sdk-ui`, and `matrix-sdk-base` resolve to **0.18.0** in
  `rust/Cargo.lock`; UI and base are exact-pinned in `rust/Cargo.toml`
- Dependencies remain lock-file controlled. Do not update them incidentally.

Important checkpoints leading into 0.6.1, newest first:

- The `0.6.1` release completed the user-facing multi-provider GIF browser
  (GIPHY/KLIPY tabs, trending/search/categories, favorites, recents, autoplay
  and safe-search settings), the safe validated provider download pipeline, and
  room and real Matrix-thread GIF sending through the SDK media path.
- `fdd6c88` licensed the project as GPL-3.0-or-later and refreshed the README.
- `73ee4ed`, `4a01f11`, and `373087a` added GIPHY/KLIPY response parsing, a
  shared provider abstraction, bounded SDK-backed network transport, result
  models, and provider-agnostic search/trending/pagination control.
- `f25dfbd`, `5b9bf99`, `49b2708`, `01784af`, and `96ce316` added live thread
  summaries, summary cards, cold-cache thread loading, and correct removal of
  true thread replies from the main timeline.
- `580a1a1`, `20b33f6`, and `0ab7e33` strengthened thread E2EE recovery,
  account/room/thread generation isolation, and safe diagnostics.
- `c197129`, `0d05272`, `712b1e8`, and `f98e65d` added notification sounds and
  privacy controls, the room/thread quick switcher, loaded-timeline search,
  and thread attachment sending.
- `98d0bf0` through `cc2414f` hardened notification cold starts/click routing,
  stale thread failures, scrolling, and real room IDs in thread links/details.

Do not describe these systems as future-only work. Keep the version at 0.6.2
until an explicitly requested release checkpoint changes it. Live validation
still pending on a real desktop: verified-session Element interoperability of
the key bootstrap, repeated account switching, favorite-GIF sends, startup
restoration, message-action popovers, and the font options.

## 3. User and response preferences

The user and maintainer is Rokas Smetonis.

- Respond in English unless Rokas writes in Lithuanian first.
- Be practical, direct, and technically specific.
- Establish evidence and root cause before proposing a large fix.
- Clearly distinguish confirmed facts, hypotheses, and behavior that was not
  tested.
- Give commands in copyable fenced blocks.
- Do not generate large agent prompts unless explicitly requested. When asked
  for one, organize it into ordered phases and checkpoints.
- Never claim GUI behavior passed because the project compiled or launched.
- Report live validation as exactly **PASS**, **FAIL**, or **NOT TESTED**.

## 4. Git and repository safety

Work on `main`. As of 2026-08-01 Rokas has directed that development happens
on `main` and that **no new branches be created** — the 0.6.5 design/scroll
work was fast-forward merged into `main` (`1692e02..188a1bb`, 25 commits, no
squash, no merge commit) and `0.6.5` is retained only as history. Do not open
a topic branch for a round; commit the round's checkpoints straight onto
`main`. Use normal fast-forward pushes. Never force-push, rewrite history, amend a pushed commit,
move or recreate a published tag, destructively reset, or run `git clean`.
Never reset to an older commit merely because a prompt expected it. Inspect
both `HEAD` and `origin/main`; if origin advanced, inspect the real latest
state and continue from it safely.

Never use `git add .` or `git add -A`. Stage only explicit files. Existing
release tags are immutable.

Treat all unrelated tracked or untracked changes as protected concurrent work.
Do not format, stage, restore, delete, overwrite, stash, or otherwise alter
another agent's work. Do not run project-wide formatters or generators on a
dirty tree. Do not pull or rebase an unclean tree if doing so could disturb
local work. Stop and report the conflict if safe synchronization would require
touching it.

These protected untracked paths must never be modified, removed, staged, or
committed:

```text
FETCH_HEAD
main
.claude/
```

The one carve-out is `.claude/agents/*.md`. Those portable role
definitions are deliberately tracked (see section 18) and may be staged and
committed like any other source file. Everything else under `.claude/` —
`settings.local.json` and its backups, `scheduled_tasks.lock`, `worktrees/`,
and any runtime team or session state — stays protected and untracked, and
must never be staged.

Begin every task with this baseline inspection:

```sh
cd /home/roksme/git/lightning
git fetch origin
git status --short
git branch --show-current
git rev-parse --short HEAD
git rev-parse --short origin/main
git log -40 --oneline --decorate
git tag --list --sort=-version:refname | head -30
glab release list 2>/dev/null || true
./build-rust/matrix-client --version 2>/dev/null || true
./build/matrix-client --version 2>/dev/null || true
```

If tracked state is clean and local `main` is behind, use only:

```sh
git pull --ff-only origin main
```

Before committing, inspect `git status --short`, the exact diff, and the staged
diff. After committing, use `git diff-tree --no-commit-id --name-only -r HEAD`
to prove the commit contains only intended paths.

## 5. Architecture ownership

Maintain these boundaries.

**QML owns presentation:** layout, controls, interaction, dialogs, menus,
focus, accessibility, visual animation, and local visual state. QML must not
own Matrix protocol, credentials, cryptography, raw sync, or persistence.

**C++ owns the application and safe Qt-facing boundary:** application state,
models, controllers, settings, lifecycle, account/room/thread generation
isolation, routing, navigation, semantic presentation adapters, notification
policy, and safe bridge-facing state.

**The official Rust Matrix SDK owns Matrix behavior:** login/session behavior,
synchronization and Sliding Sync, rooms, room and thread timelines, event
relations, event cache, pagination, media upload/download, E2EE, Olm/Megolm,
room-key requests, key backup, verification, cross-signing, account data,
receipts, and push-rule evaluation where the SDK exposes it.

The Rust backend is the current real Matrix backend, not an optional future
idea. `RustSdkMatrixClient` and the Rust FFI are authoritative for real
networking, E2EE, SDK timelines, threads, and encrypted media.

The repository still supports non-Rust builds containing mock and experimental
C++ HTTP backends. Keep them buildable and testable, but do not infer feature
parity: they are development/fallback surfaces and are not authoritative for
modern Matrix, E2EE, or SDK-thread behavior.

## 6. Non-negotiable security rules

- Use official Matrix SDK behavior for all E2EE.
- Never implement custom Matrix cryptography, Olm/Megolm, SAS generation, or a
  custom key-transfer protocol.
- Never automatically trust a device or promote local UI confirmation to SDK
  trust.
- Never manipulate the crypto store directly or reset/delete it as a normal
  repair. An explicit, account-scoped destructive recovery action must remain
  a last resort with honest consequences.
- Never expose access or refresh tokens to QML.
- Never expose authenticated media URLs to external applications or use them
  as browser targets. Fetch/decrypt through the controlled media bridge.
- Never log decrypted private message bodies, recovery keys, room/session
  keys, secret-storage material, UIA passwords, provider keys, tokens, or raw
  cryptographic state.
- Never persist decrypted private-message plaintext in application caches.
  Encrypted-room plaintext remains memory-only; `CacheStore` must continue to
  reject encrypted timeline rows.
- Never put real credentials in tests or commit private stores/session data.
- Never render untrusted SVG as active content. Keep SVG excluded from inline
  preview/media paths unless a separately reviewed safe design lands.
- Never commit or log GIF-provider API keys.
- The store an account uses is **recorded**, never re-derived twice. A store
  path computed from a typed login name and a record persisted under the
  server-canonical user id will disagree, and the app then deletes, orphans,
  or fails to clean the wrong account's crypto store. Persist the mapping and
  read it everywhere: restore, logout, reset, removal, and the orphan check.
- Never treat "no readable access token" as "no account". A locked keyring or
  an unavailable session bus is a transient credential-backend failure, not
  evidence that a store is orphaned. Destructive cleanup keys on the *record*
  being absent, never on a secret being unreadable.
- Local debug logs may carry `safeUserSlug()` and account-scoped paths — that
  is the existing, deliberate practice, and those logs stay on the user's own
  machine. Anything the user is invited to **share** is held to a stricter
  bar: the support-diagnostics export carries hashed account identifiers and
  no paths at all, because a store path contains the Matrix localpart.
- Never report a cleanup as successful when it removed nothing. "Target
  absent" and "reset completed" are different outcomes, and conflating them
  hides a no-op repair behind a success message.
- Sign-out and account removal must delete the store that was actually in
  use. Leaving Megolm and device keys on disk after the user asked for the
  account to be gone is a data-at-rest defect.

Use sanitized categories, counts, stable public Matrix identifiers where
needed, and presentation-safe metadata at the Rust/C++ boundary. Do not weaken
SSRF, DNS/IP, redirect, MIME, scheme, response-size, or media-origin validation
to make a preview/provider test pass.

## 7. Implemented feature summary

Treat the following as implemented in the current repository, while preserving
backend capability checks and honest live-test status.

### Authentication and lifecycle

- Password login, persistent SDK session/store, session restoration, logout,
  sync/initial-sync state, and account-scoped local reset paths
- Persistent multi-account support: account records live under
  `accounts/<slug>/` in QSettings (SettingsManager), tokens stay per-user-id
  in the SecretStore, and the session accessors are views of the active
  account. `AppController::switchToAccount` detaches the local session
  (`MatrixClient::detachSession` — emits `loggedOut` for model cleanup
  WITHOUT server logout or store deletion), points settings at the target,
  and restores it through the normal restore path. Only the active account
  syncs. Removal/logout are scoped to one account; logout continues with the
  most recently added remaining account. The `accountSwitching` property
  guards the UI; a failed activation falls back once to the previous account
- Secret Service/libsecret token storage when available, with an explicit
  insecure QSettings fallback warning
- Rust-backed unified sync/Sliding Sync behavior with compatibility fallback

### Rooms and navigation

- Joined rooms, direct-message detection from `m.direct`, invites, Space
  hierarchy, room membership/actions, room information, and room creation
- Quick switching across rooms, direct messages, Spaces, invites, and threads
- Activity ordering, unread state/navigation, first-unread and latest jumps,
  threaded receipts, and local marked-unread behavior

### Timeline and media

- SDK-backed live timelines and local echoes
- Text, rich replies, edits, reactions, redactions, typing indicators, read
  receipts, mentions, and room-state activity rows
- Element-style read-receipt chips on live-room rows (newest 16 receipts
  cross the bridge with a truthful uncapped total; the row's own sender and
  the local user are excluded because the SDK synthesizes an implicit sender
  receipt). Thread timeline builders deliberately keep receipt tracking
  Disabled — SDK receipts are not thread-aware
- Images, files, clipboard images, encrypted attachments, media viewing/saving,
  animated GIF attachments, and validated direct-raster inline previews
- Backward pagination and retry, stable navigation, loaded-timeline search,
  message links/permalinks, message details, context menus, and sender profiles
- Link previews with encrypted-room privacy controls and security validation
- Smooth mouse-wheel motion, touchpad pixel scrolling, configurable wheel
  speed, keyboard scrolling, and per-room position preservation

### Threads

- SDK `TimelineFocus::Thread` timelines and `ThreadListService`
- Thread panel and per-room Threads view, real `m.thread` text/rich replies,
  follow/unfollow where MSC4306 is supported, threaded read receipts, and
  pagination
- Thread image/file/clipboard attachments through the SDK, including encrypted
  rooms, with local echoes, send state, and retry/failure handling
- Element-style root summary cards with server reply counts, latest metadata,
  live updates, and conservative unread indication
- True thread-reply filtering from the live main timeline, cold-cache initial
  loading, stable per-thread scrolling, quick-switch navigation, and in-place
  thread E2EE recovery

### E2EE

- SDK-owned encrypted sending/receiving and persistent crypto store
- Crypto readiness/health model and sanitized recovery diagnostics
- Automatic room-key requests and SDK backup download after decryption failure
- Late in-place decryption updates, manual bounded retry, key import, and
  recovery-key/passphrase backup restore controls
- SAS emoji device verification in both directions, show-QR verification
  (Lightning displays a code the other device scans; SDK-owned reciprocate
  flow, SAS fallback, never scans — live Element interop NOT TESTED),
  session/device trust UI,
  cross-signing/backup state, and generation-isolated callbacks

These mechanisms cannot guarantee recovery of historical messages whose keys
were never backed up or shared.

### Notifications

- Native freedesktop notifications when Qt DBus and a notification service are
  available
- SDK-derived mention metadata, direct-message and per-room local modes,
  privacy modes, active-room suppression, invite and verification notices
- Cold-start/backlog suppression, bounded click routing to room/event/thread,
  configurable sounds, and burst coalescing
- Per-room notification modes synchronize to server push rules on the Rust
  backend (SDK-managed; user-defined-rule reports reconcile a device-local
  cache that keeps policy working offline, and a failed write is disclosed
  in the UI as kept-on-this-device). Non-Rust backends remain device-local.
  Live homeserver/Element interoperability of the rules is NOT TESTED

### Settings, usability, and accessibility

- Eleven complete semantic themes (ids 1–11): Lightning Light, Lightning
  Dark, Graphite, Midnight, Nordic, Purple Dusk, Warm, the design-handoff
  Moss Light / Indigo Night / Deep Teal, and Storm (11) — the 0.6.5 brand
  theme (deep navy, bolt-yellow accent), first in the picker; System (0)
  resolves to Moss Light / Storm. AppTheme.qml is the sole token source;
  the theme test enforces palette completeness, routing, and WCAG AA pairs.
  The storm* token namespace (menus, popovers, Settings) is theme-ROUTED:
  Storm literals under theme 11, each legacy theme's own semantic tones
  otherwise. The trust card is the one deliberate invariant (raw _sto*
  literals). Ink on a bolt/accent fill uses boltInk, never stormPanel
- The four-pane design shell: 68 px spaces rail (home, Spaces, settings,
  account avatar + switcher popover), 300 px room list with workspace
  header and Ctrl-K hint, timeline with members/threads side panel, card
  composer; bundled Manrope/JetBrains Mono fonts; application icon and
  desktop entry installed by CMake (see data/ and scripts/generate-icons.sh)
- The full-view Settings screen: covers the entire application content
  area (the chat shell stays loaded but hidden — no rail, room list,
  timeline, composer, or right panel while open; closing restores the
  selected room with the right panel remaining None). 60px header
  ("Settings — <section>", accent section icon, bare close X) above the
  260 px internal navigation (Account, Appearance, Notifications, Privacy
  & security, Sessions, Labs; About pinned bottom; soft-accent active
  rows).
  Appearance carries the three featured design theme cards with fixed
  preview palettes plus a secondary row for the other presets, a custom
  match-system switch, a FUNCTIONAL message-layout selector (Modern /
  Bubbles for DMs / Compact) and a text-size slider (90-140%) — theme,
  layout and text scale persist per account with a global fallback. All
  prior security/session/recovery controls are preserved under Privacy &
  security and Sessions. Avatar shapes are baked into the cached bitmap by
  MediaImageProvider ("|shape:" suffix) instead of per-item MultiEffect
  masks. Headless/offscreen runs force stderr logging in main.cpp because
  Qt otherwise routes category logs to the journal when stderr is no TTY.
- Room-activity visibility, link/GIF preview policy, notification privacy and
  sound, per-room notification mode, and wheel-speed settings
- Unicode emoji picker with search, tones, and bounded local recents
- Keyboard quick switch/search/navigation, accessible labels/roles/actions,
  focus handling, and keyboard-operable message/thread actions

### GIF provider integration

The provider foundation and network clients are implemented: strict GIPHY and
KLIPY parsing, shared provider interface, provider-specific endpoint/key/rating
and pagination behavior, attribution, a provider-agnostic search controller,
result model, stale-response rejection, deduplication, trending/search modes,
and bounded redirect-validated HTTPS transport through the Rust backend.

The user-facing GIF browser is implemented: a shared room/thread picker with
GIPHY and KLIPY provider tabs, trending, debounced search, client-side category
shortcuts, pagination, per-provider attribution, favorites, bounded local
recents, safe-search rating, a configurable autoplay policy, and accessible
keyboard-navigable tiles. The safe validated download pipeline (HTTPS-only,
revalidated redirects, bounded size, GIF magic and dimension validation) and
the send path into a room or a real Matrix thread — uploading through the SDK
media path, with SDK media encryption in encrypted rooms — are implemented.
Existing GIF attachment/direct-media playback remains separate and implemented.
Live Element interoperability of provider GIF sends should still be tested
honestly rather than assumed.

**Saving GIFs** is implemented. A star appears on hover over a timeline GIF
and copies the bytes into an account-scoped, content-addressed store bounded
at 200 items / 64 MiB (refusal, never eviction — a full store must not
silently discard what the user asked to keep), and those send from local
bytes.

A star means exactly one thing everywhere — "save this GIF" — with one
destination: the picker's **Saved** tab. That tab renders `GifSavedModel`, a
presentation-only `QConcatenateTablesProxyModel` merge of the local byte store
and the provider favorites; the two **stores stay separate**, because only one
of them holds decrypted media. The picker's navigation is one row of peers:
the provider sources (GIPHY, KLIPY) and the two lists that were always
cross-provider (Saved, Recent). Each tile carries its own source tag
(GIPHY/KLIPY/LOCAL).

Saved and Recent issue **no provider API request** — no search, trending,
pagination, or category call is reachable from either. They are not offline,
though: a saved *provider bookmark* is a link, so its tile still loads its
preview from that provider's CDN, exactly as the Favorites list always did.
Only the locally-saved rows are pure device-local content. Do not describe the
Saved tab as having "no provider traffic".

Never read `GifResultModel::FavoriteRole` from a `GifStoredModel` as a
"is this saved" oracle: that role is a constant `true` for every stored
collection, which is honest for favorites and local-saved rows and a lie for
Recents. Ask the collection (`GifFavoritesModel::isFavorite`).

This is a deliberate, documented exception to the section 6 rule against
persisting decrypted media, on explicit-export semantics: the user is
choosing to save one image, exactly as Save-As already allows. It is only
defensible because deletion is real — the store is removed on sign-out and
on account removal through a shared path helper with tri-state
deleted/absent/failed reporting (an earlier version *claimed* this cleanup
and did not have it; decrypted media would have survived sign-out
indefinitely). Settings → Privacy & security discloses the store and offers
Clear All. The index records **no provenance**: no room, event, or sender.
Do not weaken any of that, and do not extend the exception to other media
without the same deletion guarantees.

## 8. Threads and main-timeline rules

Preserve these invariants:

- A true `m.thread` reply must not render as a standalone ordinary message in
  the main room timeline.
- Thread roots remain in the main timeline and may show compact summary cards.
- Thread replies belong to SDK `TimelineFocus::Thread` timelines.
- Normal rich replies that are not `m.thread` events remain visible in main.
- Thread local echoes, including GIF/media sends, must never leak into main.
- Thread text and attachments must use the SDK thread-focused send path; never
  fall back to an ordinary room send.
- Navigation uses real room IDs and root event IDs. The internal composite
  timeline ID (`room + unit separator + thread + unit separator + root`) must
  never leak into permalinks, details, notifications, or protocol calls.
- Do not invent exact per-thread unread counts when the SDK/server only
  provides enough information for a conservative unread dot.

The client builder currently enables:

```text
ThreadingSupport::Enabled { with_subscriptions: false }
```

This enables event-cache per-thread chunks and thread-aware receipts/unreads.
Lightning controls MSC4306 follow state directly; it does not use the MSC4308
Sliding Sync subscriptions extension. The live room timeline uses
`TimelineFocus::Live { hide_threaded_events: true }`; thread timelines use
`TimelineFocus::Thread { root_event_id }`.

## 9. E2EE synchronization and recovery rules

The intended and implemented late-decryption path is:

```text
encrypted event
  -> SDK cannot decrypt yet
  -> SDK room-key request, verified-session recovery, or trusted backup
  -> key imported by the SDK
  -> SDK/event cache retries decryption
  -> timeline emits a replacement/update
  -> the same stable event updates in place
```

This applies to main and thread timelines. Normal key arrival must require no
restart, room switch, crypto-store deletion, or repeated manual Retry. Manual
Retry remains a bounded diagnostic/recovery control, not the ordinary path.

Stable event identity is mandatory. Account/lifecycle, room-timeline, thread,
thread-list, and request generations must reject stale callbacks after logout,
room changes, or thread changes. Never let a late callback mutate the next
account or a newer timeline.

Verified-device recovery and trusted-backup recovery are distinct. Trust labels
must come from SDK state; importing keys does not verify a device. Recovery
from another session must preserve the SDK's verification/trust requirements,
and backup recovery must require a usable trusted backup. Never promise that
every historical key is recoverable.

Automated replacement tests prove local mechanics, not real interoperability.
Report Element-to-Lightning, multi-device, backup, and live homeserver recovery
as PASS only after actually exercising those paths.

## 10. GIF integration rules

Keep GIPHY and KLIPY behind the shared provider interface. Provider-specific
URLs, parsing, rating mapping, pagination, errors, and attribution belong in
provider code; lifecycle, stale-response rejection, and result state remain
provider-neutral.

Provider keys resolve in one authoritative path
(see `gif::resolveProviderKeyDetailed` in `src/gif/GifKeyConfig.*`):

1. a runtime override — `LIGHTNING_GIPHY_API_KEY` / `LIGHTNING_KLIPY_API_KEY`;
2. the local development env file, parsed safely by the app itself
   (`LIGHTNING_GIF_ENV_FILE` override, else `./lightning-gif.env` in the
   working directory) — so a direct binary launch works without the
   `run-dev.sh` wrapper;
3. an application key compiled into an official release build;
4. otherwise unconfigured (the existing missing-key state).

An empty value never overrides a valid lower-precedence source, and the
picker re-resolves on every open (`refreshProviderKeys`).

The runtime override always wins, so a source build (which has no embedded key)
works as soon as those variables are set. Rokas's local from-source workflow
keeps a private env file in the repository root (untracked, gitignored):

```text
/home/roksme/git/lightning/lightning-gif.env   # optional local dev convenience
```

It exports `LIGHTNING_GIPHY_API_KEY` / `LIGHTNING_KLIPY_API_KEY`. The
supported way to run a source build with the keys loaded is:

```sh
scripts/run-dev.sh
```

which sources the file (`set -a; . ./lightning-gif.env; set +a`) and launches
`build-rust/matrix-client --backend=rust` inside `nix develop`. The file (and
`*.env` generally) is gitignored and must never be read aloud, printed,
logged, or committed by tooling. It is an optional developer convenience, not
a build or release requirement — official packages do not depend on it.

Official release packages embed the keys at build time. The values come from the
project 7 (lightning-deploy) protected+masked CI variables `GIPHY_API_KEY` and
`KLIPY_API_KEY`, mapped into the build-only `LIGHTNING_BUILD_GIPHY_API_KEY` /
`LIGHTNING_BUILD_KLIPY_API_KEY` that a CMake generator writes into a build-tree
header (`<build>/generated/LightningGifBuildKeys.h`). That header is never
tracked, installed, packaged, logged, or emitted on a compiler command line;
enable `-DLIGHTNING_REQUIRE_GIF_KEYS=ON` to require both in official builds.
Clean-package validation runs `matrix-client --gif-status` (booleans only) and
`--gif-selftest` (bounded live request) with every key variable unset to prove
the embedded keys work.

Never print, log, commit, embed in tracked source, or pass key values through
QML, `--version`, settings, or diagnostics. A key compiled into a distributed
desktop binary is ultimately extractable; do not claim otherwise. These are
application/provider keys, not Matrix keys. Send only the user's GIF search
term to the explicitly selected external provider; never send Matrix IDs,
room IDs, event IDs, user IDs, message bodies, homeserver credentials, or
other Matrix context. Display the selected provider's required attribution.

Provider API search/trending fetching, downloading the selected provider media,
and sending it to Matrix are implemented. The download path validates scheme,
DNS/IP, redirects, MIME/type, size, and dimensions before handing bytes to the
existing Matrix attachment/media path. Preserve these checks. Encrypted-room and
encrypted-thread GIF sends use SDK media encryption exactly like other
attachments; never send a bare provider URL and never weaken these validations.

## 11. Build, test, and run commands

Use the Nix development environment. If build trees do not exist, configure
them explicitly:

```sh
nix develop -c cmake -S . -B build-rust -G Ninja \
  -DENABLE_RUST_SDK_BACKEND=ON
nix develop -c cmake -S . -B build -G Ninja
```

Rust tests:

```sh
nix develop -c cargo test --manifest-path rust/Cargo.toml
```

Rust-enabled build and CTest:

```sh
nix develop -c cmake --build build-rust
nix develop -c ctest \
  --test-dir build-rust \
  --output-on-failure
```

Non-Rust build and CTest:

```sh
nix develop -c cmake --build build
nix develop -c ctest \
  --test-dir build \
  --output-on-failure
```

Version checks:

```sh
./build-rust/matrix-client --version
./build/matrix-client --version
```

Real Rust-backed run:

```sh
nix develop -c ./build-rust/matrix-client --backend=rust
```

Run binaries inside `nix develop` when the host Qt environment is incompatible.
For live debugging, capture only bounded logs and filter by safe categories.
Never enable logging that exposes tokens, passwords, recovery material,
provider-key-bearing URLs, private bodies, raw events, stores, or secrets.

## 12. Testing and validation policy

Use the category that matches the evidence:

- **Unit tests:** focused pure C++/Qt behavior.
- **Rust tests:** SDK bridge, parser, timeline, recovery, and Rust behavior.
- **CTest:** registered C++/Qt/QML/controller/bridge tests. Five
  screenshot-demo suites are gated behind `LIGHTNING_ENABLE_SCREENSHOT_DEMO`
  and are absent from a default tree. Never quote a test count from this
  file — it goes stale the moment a suite is added, which it repeatedly has.
  Run `ctest --test-dir <tree> -N` and quote that. Note that `-N` counts
  *registered* tests and is not evidence that any of them passed; never
  report a registration count as a pass rate.
- **QML tests:** contract scans and real offscreen module/component loading.
- **Bridge/controller tests:** generation isolation, diff ingestion, media,
  thread, notification, and application policy.
- **Application launch:** proves startup only.
- **Real GUI interaction:** proves the exercised visual interaction only.
- **Physical mouse/touchpad tests:** required for claimed wheel/touchpad feel.
- **Real homeserver tests:** required for network behavior.
- **Element interoperability:** required for Element-to-Lightning claims.
- **Multi-device E2EE tests:** required for real key sharing/recovery claims.

Compilation is not a GUI PASS. Launch is not feature validation. Automated
tests are not live Matrix interoperability. Real Element-to-Lightning
decryption requires a live test. Physical scrolling requires physical input.
Desktop notification display, sound, and click routing require actual desktop
interaction. Mark any unavailable live test **NOT TESTED**.

Completion reports must give exact totals for every executed suite: passed,
failed, skipped, and total. Do not say merely "tests passed."

## 13. Checkpoint workflow

Use this sequence for every task:

1. Inspect repository state and compare `HEAD` with `origin/main`.
2. Inspect the current implementation and recent relevant history.
3. Reproduce or prove the defect or missing capability.
4. Identify and explain the root cause.
5. Implement one coherent change without touching concurrent work.
6. Add focused tests.
7. Build the affected configurations.
8. Run focused tests.
9. Run all relevant suites.
10. Run `git diff --check`.
11. Review status, full diff, and security/privacy impact.
12. Stage exact files only.
13. Commit one coherent checkpoint.
14. Push normally to `main`.
15. Fetch and verify `HEAD` equals `origin/main`.
16. Continue only from a clean, pushed checkpoint.

Split large work into ordered phases and separate commits. Do not create one
giant mixed commit spanning unrelated behavior, cleanup, dependencies, and
release work.

## 14. Release policy

Published tags and GitLab Releases are immutable. Never move, recreate, or
replace them. Do not bump a version, tag, or create a release unless Rokas
explicitly requests release work.

Version 0.6.5 is released and the synchronized CMake, Rust, and user-agent
version report 0.6.5. Any future version bump is a release checkpoint alone and
updates those same synchronized locations. Before release, run complete Rust
tests plus Rust and non-Rust builds/CTest, and report unavailable live
validation honestly.

Releases are **package-first**: the packaging pipeline (lightning-deploy,
project 7) creates the tag and GitLab Release only after it has built,
validated, published, and verified the installation packages. The tag and
release must not exist before package publication and verification pass. The
authoritative flow is:

1. Prepare the release commit on project 6 `main`.
2. Update the application version (CMake, Rust, user agent) and the release
   documentation (`docs/releases/v<version>.md` if used for notes).
3. Run complete source tests (Rust tests plus Rust and non-Rust builds/CTest),
   and report unavailable live validation honestly.
4. Push the release commit normally to `main`; never force.
5. Do **not** manually create the tag or GitLab Release yet.
6. Trigger the project 7 packaging pipeline in `RELEASE_ACTION=create` mode
   (`SOURCE_REF=<full release commit SHA>`, `RELEASE_VERSION=<X.Y.Z>`,
   `PUBLISH_PACKAGES=true`).
7. The pipeline builds and validates all supported packages on clean systems.
8. It publishes them to the project 6 Generic Package Registry under
   `lightning / <version>`.
9. It creates the tag and GitLab Release from the exact resolved commit only
   after publication verifies.
10. It attaches every package link (`link_type: package`) to the new release.
11. The release is complete only after source archives and package links
    verify.

For an existing release that is missing packages, use
`RELEASE_ACTION=attach-existing` (build, validate, publish, verify, then add
links to the existing release without altering its tag, notes, or source
archives). This was used to backfill `v0.6.1`.

The latest published release is `v0.6.5` (`4cdace3`), cut from its release
commit on `main` by the project 7 pipeline in `RELEASE_ACTION=create` mode.
All earlier releases and tags (`v0.6.4` and older) remain immutable and
unchanged.

## 15. Licensing and public repository state

Lightning is licensed **GPL-3.0-or-later**. `LICENSE` contains the GPLv3 text,
and README declares the later-version option and copyright notice.

The canonical GitLab source is publicly readable. Direct write access is
limited and controlled by the maintainer. Open-source licensing permits use,
study, modification, and redistribution under its terms; it does not grant
write access to the canonical repository. Public registration, forks, or
direct merge-request submission may not be enabled on this GitLab instance.

## 16. Current active development areas

Keep this list grounded in source and recent history:

- Continue GIF playback, cancellation, resource, cache, and malformed-media
  hardening now that the user-facing flow has landed.
- Perform real Element interoperability validation of provider GIF sends across
  plain and encrypted rooms and threads.
- Validate notification coverage/routing for thread replies now that true
  thread replies are excluded from the live main timeline.
- Perform real homeserver and Element interoperability validation for thread
  timelines, thread sending/attachments, late E2EE recovery, backup recovery,
  verification, notifications, and physical scrolling.
- Perform live multi-account validation on real homeservers: switching with
  two signed-in accounts (same and different homeservers), encrypted-room
  decryption after a switch, notification routing, restart restoration, and
  scoped removal. The offline lifecycle is CTest-covered; the live matrix is
  NOT TESTED.
- Live-validate the room-list latest-event previews and avatar readiness on a
  real account (the lazy Latest-Events registration landed with the 0.7 UI
  checkpoints), and the design shell on a real desktop (KDE Wayland taskbar
  icon association included).
- Deliberate follow-ups from the design handoff: thread participant
  facepiles (needs participant data in the thread-summary bridge payload),
  voice messages (the composer keeps the designed mic slot in the honest
  unavailable state), and Matrix presence. Markdown sending (formatting
  toolbar + SDK text_markdown on interactive sends), message layout modes,
  and text-size scaling landed with the 2026-07-20 checkpoints; their live
  Element interoperability (formatted-body rendering) is still user-pending.
- Live-validate the redesigned Settings screen and the baked-mask avatar
  rendering interactively on a real desktop (automated suites cover both).
- Plan any post-0.6.5 work only through explicitly requested checkpoints.

Open items carried by the post-0.6.5 rounds (`4cdace3..e39439a`), in the
order a successor should pick them up:

- **Timeline scroll teleport during pagination — OPEN, precisely located.**
  A live `LIGHTNING_SCROLL_TRACE=1` capture shows a real 20-row page landing
  while the content-height *estimate* reports a shrink
  (`displacedMaxAbsGrew=-3582 Rows=20 applied=0`), so `maintainViewAnchor()`'s
  positive-only guard skips the correction and the reader is moved. Do NOT
  simply apply negatives: the same trace shows ±17000 px estimate swings with
  no model change at all. Three attempts have failed — two withdrawn in
  review on disproved premises, one (the staging/freeze window, `225c7b3`)
  shipped and made it worse before being removed wholesale in `263268b`.
  **This subsystem cannot be diagnosed from the mock harness.** Land
  instrumentation, obtain a capture, then fix what the capture names. The
  next unverified candidate is deriving displacement from `originY` rather
  than `contentHeight` — measure before implementing.
- **GIF-favorite reopen crash** — still only `1502e6b`'s commit message as
  evidence; seven headless scenario families including an ASan build found
  nothing. Needs a real `coredumpctl`/`gdb` backtrace.
- **`app.` dereferences in creation-time bindings of other `Repeater`
  delegates** (`qml/EmojiPicker.qml`, `qml/SettingsScreen.qml` theme cards) —
  structurally exposed to the poisoned-context-lookup defect fixed in
  `30ee39b`, not observed failing.
- Live validation still outstanding for everything the post-release rounds
  added: read receipts, server push-rule notification modes, QR verification
  against Element / Element X, and saving GIFs. All **NOT TESTED**.

"Recovering never-backed-up Megolm keys" is **refused, not deferred**: a key
that was never backed up and never shared exists nowhere, every legitimate
recovery path is already implemented, and anything further would weaken E2EE.

Do not list the implemented GIF browser, favorites/recents, download/send path,
provider networking, thread summaries/attachments, notification sounds, or E2EE
generation isolation as unfinished. Do not turn possible future ideas into
commitments.

## 17. Agent completion-report requirements

Every completion report must include:

- Starting commit, final commit, branch, and fetched `origin/main`
- Exact checkpoint commits created and pushed
- Exact test totals and which configurations ran
- Dependency/lock-file changes, or explicit confirmation of none
- Release/tag status
- Confirmed root cause(s), separate from hypotheses
- Automated validation performed
- Live validation with **PASS**, **FAIL**, or **NOT TESTED**
- Security/privacy review and known limitations
- Final working-tree status
- Confirmation that only intended exact files were staged/committed
- Confirmation that no force-push, amend, reset, clean, stash, or history
  rewrite occurred
- Confirmation that protected untracked files and concurrent work were
  untouched

Never imply a test happened when it did not. A concise honest report is more
valuable than a broad unsupported claim.

## 18. Multi-agent review protocol

Substantive code changes in this repository go through one independent review
before they are committed. The reusable role definitions live in
`.claude/agents/`:

```text
.claude/agents/lightning-session-store-specialist.md
.claude/agents/lightning-verification-specialist.md
.claude/agents/lightning-account-security-ui-specialist.md
.claude/agents/lightning-integration-regression-specialist.md
.claude/agents/lightning-gif-thread-specialist.md
.claude/agents/lightning-touchpad-scroll-specialist.md
.claude/agents/lightning-menu-specialist.md
.claude/agents/lightning-layout-specialist.md
.claude/agents/lightning-storm-design-lead.md
.claude/agents/lightning-independent-reviewer.md
```

Rules:

- **Exactly one agent builds at a time.** `cmake --build`, `ctest`,
  `cargo build` and `cargo test` all write into the same build trees; two
  concurrent `ninja` runs in one tree race on object files and `.ninja_deps`
  and produce a result that looks like evidence but is not. The lead holds a
  build lock and hands it to one agent at a time. Writing code and tests
  needs no compiler — implement while waiting.

  This is not theoretical. Three concurrent `cmake --build build-rust` runs
  on one tree — orphans left by killed foreground timeouts — corrupted
  `.ninja_deps` and produced a phantom test failure that was nearly dismissed
  as pre-existing. A build overlapping a `ctest` run on the same tree caused
  two more flakes. Check for a live build (`pgrep -af "ninja|cmake"`) before
  starting one, and serialize build → test strictly.

- **Cap CPU-heavy work at 18 threads.** Rokas directed this on 2026-08-07:
  pass `-j18` explicitly to every `cmake --build`, `ctest`, and `cargo`
  invocation. The defaults use all 20 cores; he wants two left.

- **Implementation agents must run builds synchronously.** Agents that
  background a build and wait for a notification stall indefinitely at zero
  CPU — observed repeatedly across the post-0.6.5 rounds, and detected by the
  user rather than by the orchestrator. If an agent has only verification
  left, stand it down and let the lock-holder verify.

- **Instrument rather than guess when the harness cannot reproduce the
  report.** Two speculative scroll fixes were withdrawn in review on
  disproved premises and a third shipped and regressed the user's experience
  before this was learned. A plausible mechanism supported by code reading is
  a hypothesis; it becomes evidence when a measurement distinguishes it from
  the alternatives. Landing an opt-in trace and asking for a capture is
  faster than a third wrong fix.

- **A regression test that does not fail on the old code is decoration.**
  Prove the failure against the unfixed tree before claiming coverage.

- Use agents only for genuinely independent, substantial work. A single
  focused edit does not need a team.
- Keep the number of agents low. Do not spawn extra agents for redundant
  verification; one meaningful review gate beats a cycle of ceremonial
  double-checking.
- Assign **exclusive file ownership** before any implementation begins. Two
  agents must never edit the same file concurrently. When two workstreams need
  the same file, serialize them: one agent owns the file, the other supplies
  findings only. Shared integration files (for example `CMakeLists.txt`) are
  owned by the lead.
- Run the relevant tests **before** review, so the reviewer judges real
  evidence rather than intentions.
- Require one **non-author** independent review of the cumulative diff before
  committing substantive code. The reviewer must be read-only: it may read,
  grep, inspect Git history, build, and run tests, but it has no `Edit` or
  `Write` and never authors the code it reviews. Corrections are made by the
  original author, and the reviewer then rechecks only the affected diff.
- The reviewer reports every substantiated finding, grouped by severity, each
  with `file:line`, evidence, impact, and the requested correction. The lead
  classifies each finding as *must fix*, *accepted follow-up*, or *rejected
  with evidence*. All correctness, security, data-loss, interoperability, and
  regression findings are fixed before approval.
- The review ends with exactly `APPROVED` or `CHANGES_REQUESTED`. No commit or
  push happens before `APPROVED`.
- Stage exact files only — never `git add .` or `git add -A`.
- Never force-push, amend a pushed commit, rewrite history, `git reset --hard`,
  `git clean`, or stash another agent's work.
- Never create a release or tag, bump the version, or trigger packaging unless
  Rokas explicitly requests release work.

Runtime team state belongs to Claude Code itself and is never committed. Only
the portable role definitions above and this protocol are tracked; they must
contain no credentials, tokens, absolute user-specific paths, private
endpoints, or machine-specific values.
