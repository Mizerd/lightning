# Lightning development guide

This is the authoritative operating guide for Claude Code, Codex, and other
coding agents working in this repository. Before changing the repository,
inspect its current state; consult relevant source and path-scoped history as
needed because they override stale comments or assumptions.

## 1. Project identity

Lightning is an actively developed native desktop Matrix client. It is Linux
and NixOS first and uses Qt 6, QML, C++20, CMake, a Nix development
environment, and the official Rust Matrix SDK backend.

- Project working tree: `/home/roksme/git/lightning`
- Obsidian vault: `/home/roksme/Documents/LLM`
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

Release facts, verified on 2026-08-10:

- Latest published release: **Lightning 0.6.6** (`v0.6.6` -> `f35bc8c`)
- Previous releases: `v0.6.5` -> `4cdace3`, `v0.6.4` -> `e719bbe`,
  `v0.6.3` -> `97f10b7`, `v0.6.2` -> `fe3b85f`, `v0.6.1` -> `86d30b4`,
  `v0.6.0` -> `2157194` (all immutable, unchanged)
- Application version: **0.6.6** in `CMakeLists.txt`, `rust/Cargo.toml`, and
  the Rust/HTTP user agent

0.6.6 released the thirty commits that had accumulated since `v0.6.5`:
Element-style read receipts (`c060ef5`, `3afc2d0`, `30ee39b`), per-room
notification modes promoted to server push rules (`21d5fb8`), QR device
verification alongside SAS (`6fa6378`; the only dependency change in the
release), locally-saved GIFs reworked to one star with one destination
(`44c29aa`, `52cf6ca`, `e39439a`, `9531684`), pickers pinned to the composer
and made resizable (`dbbd484`, `467a391`, `e48fe8d`), find-in-timeline match
highlighting (`97cef4f`), and the room timeline rebuilt without height
virtualization (`1e50f6a`, `080c186`, `5dad0fd`, `e3a7d7a`, `8f84d18`,
`263268b`) after the staging/freeze mechanism from `225c7b3` was removed
entirely. `60f2c54` restored the README screenshots to native resolution.

**Carried into the release, and still open:** CTest was **85/87 on both
trees** at release time, not 87/87. `timeline-pane-qml` (36 passed / 27
failed) and `timeline-hydration-qml` (5 passed / 2 failed at release; 4/3 in
the current desktop environment — the extra case fails identically with the
release-era binaries, i.e. environment drift, not a code regression) have
failed continuously since the timeline was rebuilt in `1e50f6a`; `8f84d18`
ported seven other suites to the solid-timeline contract and recorded these
two as explicitly not addressed. They are stale assertions against the
previous virtualized contract rather than separately observed defects, but
the regression net for the timeline is incomplete and porting them is the
highest-value open work. This is disclosed in `docs/releases/v0.6.6.md`.

As of the 2026-08-12 video-thumbnail round the registered count is **93 per
tree** and the environment shows **91/93 on both trees** — ONLY the two
timeline suites above, at exactly their release-era sub-test totals
(`timeline-pane-qml` 36 passed / 27 failed, `timeline-hydration-qml` 5
passed / 2 failed). The three suites the 2026-08-11 round recorded as also
failing here — `settings-shell-qml`, `design-acceptance` and
`verification-qr-qml` — now PASS; that was environmental drift (offscreen
pixel sampling, a host KDE style leak) and it has cleared on its own, which
is exactly why those numbers were flagged as describing one desktop on one
day. Run the suites yourself rather than trusting these; the same caveat
still applies to this paragraph.

Run `git log --oneline v0.6.6..HEAD` rather than trusting this list; it will
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

**2026-08-11 history rewrite (one-time, maintainer-authorized):** every
commit SHA in this repository changed on 2026-08-11 when Rokas directed a
full message-only history rewrite (git filter-repo) stripping AI
co-author/session trailers so the GitHub mirror credits only him. Trees,
authors, dates and messages are otherwise identical; all 13 release tags
were recreated at the rewritten commits and the GitLab releases and GitHub
mirror follow them. Consequences: every commit SHA quoted in this file,
docs/, and release notes that predates the rewrite is a PRE-REWRITE
identifier (kept deliberately — they match the historical record); any old
clone must be re-cloned, never pulled; a full pre-rewrite backup bundle is
at /home/roksme/lightning-pre-rewrite-backup.bundle. This was a singular
exception — the rules below (never force-push, never rewrite, immutable
tags) remain in full force.

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

Before a task that may modify the repository, run the minimal local baseline:

```sh
cd /home/roksme/git/lightning
git status --short
git branch --show-current
git rev-parse --short HEAD
```

Do not run this baseline for a read-only question unless repository state is
needed to answer it. Inspect only relevant source and history: prefer a short
path-scoped log over a repository-wide `git log -40`. Fetch once before work
that depends on the current remote state, before an authorized push, or when
Rokas explicitly asks for synchronization. Release lists, tag inventories,
and binary version checks belong only to release/version tasks.

When remote state is relevant, compare `HEAD` with `origin/main`. If tracked
state is clean and local `main` is behind, use only:

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
  cross the bridge with a truthful uncapped total; ONLY the local user is
  excluded — since 2026-08-12 a user's marker renders even on their own
  message, matching real Element behavior; the earlier extra
  sender-exclusion made receipts vanish asymmetrically when the other side
  sent, see docs/receipt-semantics.md). Thread timeline builders
  deliberately keep receipt tracking Disabled — SDK receipts are not
  thread-aware
- Images, files, clipboard images, encrypted attachments, media viewing/saving,
  animated GIF attachments, and validated direct-raster inline previews
- Inline video/audio playback materializes the decrypted payload as a
  session-scoped 0600 temp file (wiped on sign-out/switch/exit). Since the
  2026-08-12 perf round this includes a BOUNDED speculative prefetch for
  on-screen video/audio rows (declared size ≤ 32 MiB, lowest priority,
  cancelled/dropped on room switch) governed by the SAME user preference as
  GIF autoplay ("never" disables all passive media downloads), plus a
  locally extracted first-frame poster for videos without a Matrix
  thumbnail (JPEG, RAM image cache only — never disk). In-flight fetches
  are cancellable end-to-end (QML card → MediaBridge → mx_rust_media_cancel
  aborts the download task), and the SDK media store runs a real retention
  policy (max_file_size 24 MiB) so large payloads no longer enter — or
  stall — matrix-sdk-media.sqlite3
- **Outgoing videos carry a real poster thumbnail** (2026-08-12). A video
  queued in the room or thread composer is postered the moment it is added:
  `AttachmentQueueModel` drives the SAME `VideoPosterExtractor` the receive
  side uses (offscreen `QMediaPlayer` + `QVideoSink`, black-lead-in skipping,
  640px JPEG, hard timeout, one job at a time), and the decoded frame is also
  the only honest source of the video's own width/height and duration on the
  send side. Dispatch of THAT entry waits for the poster and nothing else;
  extraction failure is not send failure — the video goes out without a
  poster, exactly as before. The bytes cross `mx_rust_timeline_send_video` /
  `mx_rust_thread_send_video`, are re-validated by magic sniffing
  (`rooms::PosterBytes`, ≤ 2 MiB, SVG and every non-raster refused; a refusal
  degrades to no thumbnail), and become `AttachmentConfig::thumbnail`. **The
  SDK owns everything after that**: it uploads the poster as its own media
  request, encrypts it alongside the payload in an encrypted room, and fills
  `thumbnail_url`/`thumbnail_file` + `thumbnail_info` on the `m.video` event.
  Nothing in C++ builds thumbnail content or encrypts anything. Live Element
  interoperability of the sent posters is NOT TESTED
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
  rooms, with local echoes, send state, and retry/failure handling. Thread
  video sends carry the same locally extracted poster as the room path
  (`mx_rust_thread_send_video`), still routed through the thread-focused SDK
  timeline so the `m.thread` relation and encryption stay SDK-owned
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

**Saving GIFs** is implemented — and since the 2026-08-11 media/UX round the
star accepts every safe static raster the timeline shows: GIF, PNG, JPEG and
WebP. Bytes are validated by magic sniffing (never a claimed MIME or file
name; SVG and everything else refused), stored in their ORIGINAL format as
`<sha256>.<ext>` (no transcoding), and re-sent with a truthful MIME and
dimensions. Legacy index entries without a format field load as GIF —
existing saved GIFs survive with no migration pass. The store stays
account-scoped and content-addressed, bounded at 200 items / 64 MiB
(refusal, never eviction — a full store must not silently discard what the
user asked to keep), and sends go from local bytes.

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

Validation is proportional to scope and risk:

- **Focused/local changes:** build the affected target and run the focused
  tests that cover the changed behavior. Do not build an unaffected backend.
- **Normal features:** run focused tests first, then the relevant registered
  subset in the affected build tree. Expand only when the change crosses a
  boundary or the focused evidence exposes a wider risk.
- **High-risk and release changes:** run complete applicable Rust tests plus
  Rust and non-Rust builds/CTest. High risk includes authentication, E2EE,
  credentials, persistence/deletion, lifecycle/concurrency, Rust/C++ FFI,
  dependencies, packaging, releases, and broad cross-cutting refactors.

Do not repeat a successful build or suite merely so another agent can run it.
Record the command and exact result once. Re-run affected validation after a
code correction; repeat a complete suite only when the correction can affect
it or the prior evidence is no longer trustworthy.

Completion reports must give exact totals for every executed suite: passed,
failed, skipped, and total. Do not say merely "tests passed."

## 13. Checkpoint workflow

Use the smallest workflow that produces trustworthy evidence:

1. For read-only analysis, inspect only what is needed and report the result;
   do not build, commit, push, or perform release checks.
2. For a repository change, inspect the minimal baseline, the relevant
   implementation, and short path-scoped history when history can answer a
   real question.
3. Reproduce or prove the defect or missing capability, then identify the
   root cause. Clearly label anything that remains a hypothesis.
4. Implement one coherent change without touching concurrent work and add
   focused tests where they provide meaningful regression coverage.
5. Run proportional validation from section 12, `git diff --check`, and one
   complete self-review of the exact diff and its security/privacy impact.
6. Apply the independent-review gate from section 18 only when its risk
   triggers are met.
7. Stage exact files and create a coherent checkpoint only when the task
   authorizes a commit. Push once after an authorized completed checkpoint or
   phase, not after every small edit. Verify remote equality after a push.

Split large work into ordered phases and separate commits. Do not create one
giant mixed commit spanning unrelated behavior, cleanup, dependencies, and
release work.

## 14. Release policy

Published tags and GitLab Releases are immutable. Never move, recreate, or
replace them. Do not bump a version, tag, or create a release unless Rokas
explicitly requests release work.

Version 0.6.6 is released and the synchronized CMake, Rust, and user-agent
version report 0.6.6. Any future version bump is a release checkpoint alone and
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

The latest published release is `v0.6.6` (`f35bc8c`), cut from its release
commit on `main` by project 7 pipeline **83** in `RELEASE_ACTION=create` mode
(all 16 jobs green; 9 assets published and hash-verified). All earlier
releases and tags (`v0.6.5` and older) remain immutable and unchanged.

One trigger note worth keeping: the pipeline's variables must be posted as a
**JSON body** (`glab api --method POST projects/7/pipeline --input file.json`
with a `variables` array). Passing them as form fields
(`-f "variables[0][key]=..."`) is silently ignored — GitLab accepts the
request, creates a pipeline with **zero** variables, and it runs as a
non-publishing snapshot build that reports success while publishing nothing.
Pipeline 82 was lost to exactly that. Always confirm with
`glab api projects/7/pipelines/<id>/variables` before trusting a release run.

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
  facepiles (needs participant data in the thread-summary bridge payload)
  and Matrix presence. Voice messages LANDED 2026-08-12 (VoiceRecorder:
  Qt Multimedia capture, OGG/Opus preferred with AAC/MP4 fallback, real
  QAudioDecoder-derived waveform; mx_rust_timeline_send_voice →
  AttachmentInfo::Voice, so the SDK emits the MSC3245 marker + MSC1767
  duration/waveform block via the normal encrypting attachment path; the
  hard duration cap DISCARDS, never auto-sends). Room composer only —
  the thread composer mic is a follow-up. Live mic capture and Element
  interop of sent voice events: NOT TESTED. Markdown sending (formatting
  toolbar + SDK text_markdown on interactive sends), message layout modes,
  and text-size scaling landed with the 2026-07-20 checkpoints; their live
  Element interoperability (formatted-body rendering) is still user-pending.
- Live-validate the redesigned Settings screen and the baked-mask avatar
  rendering interactively on a real desktop (automated suites cover both).
- Plan any post-0.6.5 work only through explicitly requested checkpoints.

Open items carried by the post-0.6.5 rounds (`4cdace3..e39439a`), in the
order a successor should pick them up:

- **Timeline scroll teleport during pagination — DID NOT REPRODUCE on
  2026-08-12; no longer a confirmed open defect.** A fresh physical
  `LIGHTNING_SCROLL_TRACE=1` capture (maintainer's real account, two rooms,
  11 gestures, ~28 real backward-pagination batches of 1–24 rows each, mixed
  text / images / GIFs to 10 MB / videos / voice, poster extraction running
  mid-scroll) shows **every** anchor outcome at zero on **every** line:
  `prependFirings=0 displacedFirings=0 displacedApplied=0
  displacedMaxAbsGrew=0 anchorCorrections=0 growthCorrections=0
  unresolvedId=0 evictedNoInsert=0`, with `materializedFirings` NON-zero and
  `materializedMaxAbsDelta=0` / `activeDeferredMaxAbs=0` — i.e. the
  mechanism engaged, measured, and found the anchor row's own y unmoved,
  which is the M2 "ran and had nothing to correct" reading, not "never ran".
  `originY=0` and `dOriginY=0` throughout.

  The reason is structural, not luck. The room timeline is a **rotated
  `Flickable` + `Column`** (`qml/TimelinePane.qml:619`), not a ListView:
  there is no delegate-height estimation left, so `contentHeight` is the
  exact sum of real rows and `originY` can never move. View row 0 is the
  NEWEST message at content y 0 (`sourceRowForViewRow = count-1-row`), so
  backward pagination lands at HIGHER view rows and HIGHER content y —
  *past* the reader. A prepend therefore does not change the anchor row's
  index or its y, `row > viewAnchorRow` is false, and the displaced branch
  is not reached at all. The `-3582` phantom shrink and the ±17000 px swings
  quoted by the previous version of this entry were artifacts of the
  pre-`1e50f6a` virtualized ListView and no longer exist.

  Consequence: **do not "fix" the positive-only guard.** In the current
  architecture the only insertion that displaces a scrolled-up reader is a
  LIVE message arriving at view row 0, which pushes every row down — a
  genuinely POSITIVE `grew` that the existing guard already applies
  correctly. Three past attempts failed here (two withdrawn in review, one —
  the staging/freeze window `225c7b3` — shipped, regressed, and was removed
  in `263268b`); a fourth needs a capture that actually names a failure.
  Keep the trace facility: it is what retired this entry. If a teleport is
  reported again, get a `LIGHTNING_SCROLL_TRACE=1` capture FIRST — a line
  with a non-zero `displacedApplied`, `anchorCorrections`, or
  `materializedMaxAbsDelta` is the evidence; all-zero lines are not.
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

The 2026-08-11 media/UX round (single commit on `main`) landed: big-emoji
rendering for 1-3 emoji-only messages (EmojiCatalog::emojiOnlySequenceCount,
grapheme-cluster + catalogue lookup; the loader also recovered the #️⃣
keycap a comment-prefix check silently dropped, so the catalogue is 3944
sequences now); MediaBridge request priorities (0 explicit playback/save,
1 avatars/thumbnails, 2 full static, 3 speculative GIF prefetch) with two
slots reserved for interactive classes, a 15s starvation bound, playable
temp-file pinning while a QMediaPlayer holds the file, queued-speculative
dropping on room switch, and byte-sniff rejection of A/V containers on
thumbnail-class results; offscreen player reclamation (audio engine unload
+ resume position after 45s, video card auto-close after 90s); the
VaapiLogGate bounding Qt's per-frame vaExportSurfaceHandle warning storm;
libpipewire made resolvable in the dev shell so Qt Multimedia uses native
PipeWire instead of the PulseAudio fallback that the captured FLAC crash
aborted in; a styled no-thumbnail video placeholder; the read-receipt
poll-drain fix (an SDK receipt move arrives as adjacent Set diffs; the
drain no longer splits the pair across 100 ms ticks) plus bounded
count-only receipt diagnostics under `matrix.receipts`; the new circular
default logo (scripts/generate-logo-source.sh -> lightning-source.png ->
generate-icons.sh); the user-selectable custom app icon
(Settings -> Appearance; appicon::normalizeIconBytes validates and
normalizes to the circular 512px presentation; window/task-switcher
surfaces only — launchers keep the packaged hicolor icon); the saved-media
generalization described in section 7; and the native QML Storm Band on
the About page (StormBandPainter tiles generated once per theme, reduced
motion honored). Known SDK-internal receipt-loss mechanisms that Lightning
cannot fix without patching matrix-sdk-ui 0.18 are documented in
`docs/receipt-semantics.md`. Live validation of all of the above:
**NOT TESTED** (see the round's completion report).

"Recovering never-backed-up Megolm keys" is **refused, not deferred**: a key
that was never backed up and never shared exists nowhere, every legitimate
recovery path is already implemented, and anything further would weaken E2EE.

Do not list the implemented GIF browser, favorites/recents, download/send path,
provider networking, thread summaries/attachments, notification sounds, or E2EE
generation isolation as unfinished. Do not turn possible future ideas into
commitments.

## 17. Agent completion-report requirements

Keep normal completion reports concise and evidence-based. Include:

- What changed and the confirmed root cause, separate from hypotheses
- Exact totals for tests actually run and the affected configurations
- Live validation as **PASS**, **FAIL**, or **NOT TESTED**
- Security/privacy impact, known limitations, and final working-tree status
- Commits and pushes only when any were actually authorized and performed

For release work, security/credential/E2EE/persistence changes, destructive
account-data behavior, dependency changes, multi-commit delivery, or when
Rokas requests a full audit, additionally include starting/final commits,
branch and fetched `origin/main`, exact checkpoint commits, dependency and
lock-file status, release/tag status, staged paths, and confirmation that
protected concurrent work and history were not altered.

Never imply a test happened when it did not. A concise honest report is more
valuable than a broad unsupported claim.

## 18. Multi-agent review protocol

Independent review is a risk gate, not a default tax on every feature. Require
one non-author review before committing changes involving authentication,
E2EE, credentials, persistence or deletion, data-loss risk, lifecycle or
concurrency isolation, Rust/C++ FFI, dependencies, packaging/releases, broad
cross-cutting refactors, a regression the harness cannot reproduce, or an
explicit review request from Rokas. A focused UI or isolated behavior change
with meaningful focused tests may use the lead's documented self-review.

The reusable role definitions live in `.claude/agents/`:

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
- Run the proportional validation required by section 12 **before** a required
  review, so the reviewer judges real evidence rather than intentions. Give
  the reviewer the exact commands and results. The reviewer does not repeat a
  trustworthy build or suite by default; it builds or tests only to resolve a
  specific evidentiary gap.
- When the risk gate applies, require one **non-author** independent review of
  the cumulative diff. The reviewer must be read-only: it may read, grep,
  inspect Git history, and run narrowly justified validation, but it has no
  `Edit` or `Write` and never authors the code it reviews. Corrections are made
  by the original author, and the reviewer then rechecks only the affected
  diff and validation invalidated by the correction.
- The reviewer reports every substantiated finding, grouped by severity, each
  with `file:line`, evidence, impact, and the requested correction. The lead
  classifies each finding as *must fix*, *accepted follow-up*, or *rejected
  with evidence*. All correctness, security, data-loss, interoperability, and
  regression findings are fixed before approval.
- A required review ends with exactly `APPROVED` or `CHANGES_REQUESTED`. When
  the gate applies, no commit or push happens before `APPROVED`.
- Stage exact files only — never `git add .` or `git add -A`.
- Never force-push, amend a pushed commit, rewrite history, `git reset --hard`,
  `git clean`, or stash another agent's work.
- Never create a release or tag, bump the version, or trigger packaging unless
  Rokas explicitly requests release work.

Runtime team state belongs to Claude Code itself and is never committed. Only
the portable role definitions above and this protocol are tracked; they must
contain no credentials, tokens, absolute user-specific paths, private
endpoints, or machine-specific values.

## 19. Autonomous long-running work and continuity

When Rokas asks Claude to work autonomously, finish a task, keep going, or
leaves the session unattended, continue making safe in-scope progress without
routine confirmation. Resolve discoverable questions from source, tests, Git
history, and existing documentation. Make and record reasonable assumptions;
ask only when a choice would materially change the requested outcome, needs
new authority, risks unrecoverable loss, or requires unavailable live input.

The Obsidian vault at `/home/roksme/Documents/LLM` is the durable local place
for long-running task notes and continuation handoffs. Use a clearly named,
task-specific note when work may span context compaction, a usage window, or
multiple sessions. Do not overwrite unrelated vault notes or treat the vault
as authoritative over repository source and Git history.

Treat context exhaustion, compaction, and the five-hour usage limit as an
interruption, never as completion or a blocker by themselves. Before an
anticipated interruption, leave a concise continuation record containing:

- Objective, current phase, and decisions already made
- Project path, branch, `HEAD`, working-tree state, and exact files being used
- Implemented changes and remaining work
- Commands and tests already run with exact results
- Any active process, reproducible failure, real blocker, and the next command

Use the existing local Claude Code scheduling/session-resume automation; do
not create or reconfigure automation unless Rokas explicitly asks. If a usage
limit stops work, do not busy-loop, repeatedly start sessions, or attempt to
bypass the limit. Let the existing automation resume the same task after the
allowance resets. On resume, read the continuation note, inspect current Git
state and any recorded process, then continue from the next unfinished action
instead of restarting the investigation.

If interruption occurs before a handoff can be written, reconstruct state on
resume from the existing task note, `git status`, the exact diff, recent
relevant history, and test artifacts. Do not discard or overwrite ambiguous
concurrent work. Continue until the requested outcome is achieved and verified
or a genuine blocker requiring Rokas is reached.
