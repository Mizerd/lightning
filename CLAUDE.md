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

Release facts, verified on 2026-08-18:

- Latest published release: **Lightning 0.7.3** (`v0.7.3` -> `8da2e81`),
  cut by lightning-deploy pipeline **104** in `RELEASE_ACTION=create`
  mode: all 19 jobs green on the first attempt, 9 assets published. The
  release that makes UPDATING work: 0.7.2 was the first that could be
  installed as an update, and doing it for real found msiexec rejecting
  Qt's forward-slash path (1619) and the portable swap asking Windows to
  rename a directory holding the running helper's mapped DLLs.
  Verified ANONYMOUSLY rather than from job status: all 9 GitLab package
  links 200 under curl; the `latest` manifest reports 0.7.3, names tag
  v0.7.3 and carries `mirror_url` on all 6 artifacts; its Ed25519
  signature (`lightning-release-2026a`) VERIFIES and a one-field-changed
  copy is REJECTED; the GitHub release has 9 assets, its annotated tag
  peels to the same commit, and a package fetched from the mirror matches
  the GitLab-signed SHA-256 exactly. Release notes:
  `docs/releases/v0.7.3.md`.
  First release carrying the Flatpak ID **org.lightning_matrix.Lightning**
  (was `net.smetonis.Lightning`; lightning-deploy `7e84170`) — a 0.7.2
  Flatpak BUNDLE is not upgraded in place and must be reinstalled.
  NOT live-validated: the Windows MSI and portable fixes CANNOT be reached
  by updating from 0.7.2, because the updater that performs an install is
  the one already on disk. Those two need one manual install of 0.7.3;
  the first genuine proof of them is the upgrade INTO 0.7.4. The Setup EXE
  path was never affected and updates normally.
- Previous release: **Lightning 0.7.2** (`v0.7.2` -> `7c736c3`),
  cut by lightning-deploy pipeline **103** in `RELEASE_ACTION=create`
  mode: all 19 jobs green on the first attempt, 9 assets published.
  Verified ANONYMOUSLY rather than from job status: all 9 GitLab package
  links 200 under curl; the `latest` update manifest fetches, reports
  0.7.2, carries `mirror_url` on every one of its 6 artifacts, and its
  Ed25519 signature (`key_id: lightning-release-2026a`) VERIFIES against
  the real public key — and a manifest with one byte changed is REJECTED,
  so that check is not vacuous; the GitHub release has 9 assets, its
  annotated tag peels to the same commit as the GitLab release, and a
  package downloaded from the mirror matches the GitLab-signed SHA-256
  exactly. Release notes: `docs/releases/v0.7.2.md`.
  NOT live-validated: **no real upgrade has still ever been performed.**
  0.7.2 is the first release that can be installed as an UPDATE (0.7.1
  was the first that could be updated from), so that test is now
  available and is the highest-value thing to do next — see
  `docs/updates.md`. Nothing in the 0.7.2 round has been checked against
  Element for interoperability either.
- Previous release: **Lightning 0.7.1** (`v0.7.1` -> `25a01f1`),
  cut by lightning-deploy pipeline **102** in `RELEASE_ACTION=create`
  mode: all 19 jobs green, 9 assets published. First release carrying the
  secure updater, and the first ever run of `sign-update-manifest` and
  `mirror-release-to-github`. Verified ANONYMOUSLY rather than from job
  status: all 9 GitLab package links 200; the `latest` update manifest
  fetches, reports 0.7.1, carries `mirror_url`, and its Ed25519 signature
  VERIFIES against the embedded key (`lightning-release-2026a`); the
  GitHub release has 9 assets and its tag peels to the same commit; and
  GitHub's bytes match the signed SHA-256. Release notes:
  `docs/releases/v0.7.1.md`.
  Three pipelines were burned first (99/100/101), all in CI plumbing and
  none reaching publication: no `make`; bare `gcc` without libc6-dev; and
  an NSIS payload assertion using `strings`, which cannot work under
  `SetCompressor /SOLID lzma`. VERIFY CI JOB SCRIPTS IN A REAL
  `docker run debian:13.6-slim` — the nix dev shell supplies a toolchain
  through stdenv and hid two of those. Also: a doomed pipeline keeps
  running its other jobs and HOLDS the runners; cancel it before
  retriggering or the retry sits pending.
  NOT live-validated: no real MSI/EXE/portable/AppImage/DEB/RPM upgrade
  has been performed. 0.7.1 is the first release that can be updated
  FROM, so the first true end-to-end upgrade is the one into 0.7.2.
- Earlier release: **Lightning 0.7.0** (`v0.7.0` -> `cd91b9c`),
  cut by lightning-deploy pipeline **98** in `RELEASE_ACTION=create` mode:
  all 17 jobs green (the fleet + the macOS arm64 test bundle via
  `BUILD_MACOS_PACKAGES=true` — built and validated on the Mac mini
  runner but never published, pending code signing), 9 assets published,
  hash-verified, and every link confirmed anonymously downloadable.
  Pipeline 97 failed only `build-rpm` (the RPM spec missed the new
  scalable SVG icon; fixed in lightning-deploy `ca24f16`). Release notes:
  `docs/releases/v0.7.0.md`. OAuth is fully live-validated (see §7).
  NOTE: anonymous probes of package links 403 under Python's default
  user-agent (reverse-proxy bot filter) — test with curl, it is not an
  access failure.
- Previous releases: `v0.6.6` -> `f35bc8c`, `v0.6.5` -> `4cdace3`,
  `v0.6.4` -> `e719bbe`, `v0.6.3` -> `97f10b7`, `v0.6.2` -> `fe3b85f`,
  `v0.6.1` -> `86d30b4`, `v0.6.0` -> `2157194` (all immutable, unchanged)
- Application version: **0.7.3** in `CMakeLists.txt`, `rust/Cargo.toml`, and
  the Rust/HTTP user agent — released, so the next bump is a new checkpoint

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

As of the 2026-08-14 user-report round the registered count is **95 per
tree** (the `room-info-moderation` suite was added) and the tree measures
**93/95 on both trees** — ONLY the two timeline suites above, with
`timeline-pane-qml` improved to 37 passed / 26 failed (the read-receipt
placement test was ported to the delegate-level fixture and the new
right-edge-rail contract) and `timeline-hydration-qml` at 5 passed / 2
failed (the release-era number). Earlier revisions of this paragraph
claimed 85/87, then 86/91, then 92/94; the pessimistic drift entries
(`settings-shell-qml`, `design-acceptance`, `verification-qr-qml`) were
offscreen pixel sampling and a host KDE style leak — exactly why these
numbers are flagged as describing one desktop on one day. Run the suites
yourself rather than trusting this paragraph. UPDATE 2026-08-18: both
timeline suites were repaired (one real gate defect, one fixture bug, two
tests inverted — see §16) and the registered tree measured fully green
offscreen on that day.

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

There is no carve-out: **all** of `.claude/` is protected and untracked —
`settings.local.json` and its backups, `scheduled_tasks.lock`, `worktrees/`,
and any runtime agent or session state. None of it may be staged. The
tracked `.claude/agents/*.md` role definitions were REMOVED on 2026-08-17 at
Rokas's request, along with the root `AGENTS.md` pointer; do not recreate
either.

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
- **OAuth 2.0 / OIDC browser sign-in** through `Client::oauth()` on
  matrix-sdk 0.18 (`rust/src/oauth.rs`). PKCE, the CSRF `state`, the code
  exchange and the token-refresh REQUEST are SDK-owned; Lightning implements
  no OAuth primitive. Refresh needs two things the SDK does not do by itself:
  `ClientBuilder::handle_refresh_tokens()` (it defaults to FALSE, and without
  it a 401 is forwarded rather than renewed, so a saved refresh token is
  inert), and writing the ROTATED pair back — `oauth::spawn_token_persistence`
  subscribes to `SessionChange::TokensRefreshed` and persists through
  `SettingsManager::updateSessionTokens`. Skipping that leaves a CONSUMED
  refresh token in the store, and an OAuth 2.1 server treats its reuse as
  compromise. `SessionChange::UnknownToken` surfaces as the existing
  `AccessTokenRevoked` state instead of an endless sync-failure loop. It adds only the system-browser launch and
  `src/auth/OAuthCallbackServer.*` — loopback-only (127.0.0.1), ephemeral
  port, single-shot, size-bounded, with a timeout, because matrix-sdk's own
  `local-server` helper is gated behind `sso-login`, whose `axum` dependency
  is not vendored in this offline `--locked` build. Costs NO dependency
  change: `oauth2`/`oauth2-reqwest` are already non-optional deps of
  matrix-sdk 0.18.
  **Two-phase store lifecycle, and it is mandatory.** Password login knows the
  account before it contacts the server; OAuth does not — the user id arrives
  only from `whoami` after the code exchange. So phase A authenticates on a
  bootstrap handle with NO persistent store (the builder's in-memory default;
  it must never sync, or it would upload device keys that phase B would then
  contradict), and phase B derives the normal `AccountIdentity`, applies
  `rust_session::oauthLoginBlockReason()`, and only then opens the account's
  sqlite store and restores through `oauth().restore_session()`. That policy
  is the OAuth counterpart of `passwordLoginBlockReason`: a device the server
  just issued must never adopt a store belonging to a different device. Note
  it deliberately does NOT suggest a local reset — that store belongs to a
  live device whose keys are still valid.
  Sessions carry an `authType` discriminator (QSettings, not the SecretStore,
  so restore routes correctly even when the keyring is locked): `password`
  restores via `matrix_auth()`, `oauth` via `oauth()`. Refresh tokens and the
  dynamic-registration client id are CREDENTIALS in the SecretStore, never in
  QSettings, never exposed to QML, never logged. Legacy Matrix SSO is
  detected and disclosed as unsupported, never offered.
  Live validation (2026-08-15): the browser sign-in flow **PASSED against
  matrix.org** (MAS/OIDC) — Rokas logged in end-to-end, and separately
  **registered a brand-new account through the Google upstream IdP** and
  logged in with it, exercising the full multi-step MAS journey and the
  two-phase store bootstrap for a never-seen account. Refresh, restart
  restoration and sign-out were then confirmed working live with his
  original test account — the OAuth path is fully live-validated. The same
  session surfaced a discovery-UX defect — the method choices only
  appeared after Enter in the homeserver field — fixed by probing the
  prefilled server on open and debounce-probing while typing.
- `restore_client()` previously hardcoded `refresh_token: None`, silently
  discarding a saved refresh token on every restore, so an expired access
  token surfaced as `M_UNKNOWN_TOKEN` instead of being renewed. Fixed for
  password sessions as well as OAuth
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
- Matrix presence indicators (2026-08-15) on unambiguous 1:1 DM rows, the
  People list and the member profile popover, via bounded client polling —
  Sliding Sync has no presence extension. See §16 for the full mechanism
  and honesty rules; live validation NOT TESTED
- **Member power levels** (2026-08-15) through the SDK's own
  `Room::update_power_levels`, which rewrites `m.room.power_levels`
  preserving every other user's value — including arbitrary custom numbers.
  Offered from the member profile popover; the OFFER policy lives in
  `RoomInfoController::canSetPowerLevel` (architecture §5), which applies
  the rules the server will apply anyway: never above the viewer's own
  level, never against a peer at or above it, self-DEMOTION only, and an
  unknown target FAILS CLOSED (levels may legitimately be negative — Element's
  "Restricted" is -1 — so absence of the roster row, never a sentinel, is
  the unknown state). `roleLabelForLevel` renders 100/50/users_default as
  Administrator/Moderator/Member and **anything else as its number**: a room
  using 42 must not be relabelled 50, and must not be SAVED as 50 either.
  Nothing is applied optimistically — the write completes and the roster is
  re-read, so a rejection cannot leave a value the room does not have.
  `own_can_change_power_levels` is the SDK's `can_send_state`, never a role
  label. Live homeserver validation NOT TESTED
- **Join rule and canonical alias** (2026-08-15) in Room Information →
  Overview, each gated on the room's REAL required level for that state
  event. Only `invite` / `public` / `knock` are settable: the restricted
  rules carry an allow-rule list this surface cannot build, and sending one
  with an empty list would silently lock the room to invite-only while
  claiming otherwise — a restricted room is displayed honestly and left
  alone. The alias path publishes the directory mapping first
  (`Client::create_room_alias`) when the alias does not already resolve to
  this room, because a server rejects a canonical alias it cannot resolve;
  clearing sends the state event with no alias and deliberately does NOT
  delete the directory mapping. Both ride the MEMBER snapshot, so a
  successful write asks for a roster refresh explicitly — nothing else
  refetches it for a non-membership state change. Live validation NOT TESTED
- **Room upgrades / tombstones** (2026-08-16), banner-and-link and
  deliberately NOT auto-follow. Matrix leaves an upgraded room in place
  and creates a replacement; Lightning keeps the old room open and
  readable and OFFERS the successor. That is a security decision as much
  as a UX one: a room transition discards navigation context and can
  discard draft state, and `m.room.tombstone` is state anyone with the
  power level can send — it NAMES the room you would be moved into. No
  code path changes the current room, joins, or leaves except as the
  direct result of the user pressing the banner.
  Room ids come only from the SDK's typed `Room::successor_room()` /
  `predecessor_room()` (ruma `OwnedRoomId`) — that IS the "parse
  replacement_room as a real room id" requirement, and nothing
  hand-parses `m.room.tombstone` or `m.room.create`. The tombstone's
  `body` NEVER crosses the FFI: free text chosen by whoever sent the
  event, on a control the user is invited to click, so the banner uses
  Lightning's own wording.
  Joined successor -> navigate, no join. Invited or UNKNOWN -> join
  through `RoomDiscoveryController::join` (so error categories and
  wait-for-room settling cannot drift from Discover), navigate only once
  settled. Refused -> stay in the old room with the reason shown inline
  in the banner. A successor we HOLD but cannot enter is the one case
  reported inaccessible; one we have never heard of is **Unknown**,
  because we cannot show the user is unable to join it.
  `chainVerified` requires the successor's predecessor to point BACK;
  false-because-unknown means "not established yet", not "bad", and only
  a CONTRADICTED chain withholds the room list's de-emphasis. That
  de-emphasis is a demotion WITHIN the room's own category (a fourth
  top-level sort group would fragment RoomsPanel's `category` sections
  and demote a superseded invite out of the top block), never a filter.
  Permalinks are untouched — an old-event permalink still resolves to the
  old room.
  Two defects were caught in review, both worth remembering:
  `navigateRequested` passed a MEMBER over a direct connection to
  `openRoom`, whose parameter aliased it — `openRoom` re-enters this
  controller, whose `refresh()` clears that member, so `openRoomTimeline`
  was never issued and Continue opened the room but not its timeline;
  and the guard that stops an abandoned join navigating a user who has
  moved on was retired only by a SUCCESSFUL join, so an abandoned join
  that FAILED left a token that swallowed a later successful Continue.
  Live validation NOT TESTED
- **Unverified-session prompts** (2026-08-15): `sessionVerificationNeeded`
  is true for exactly one actionable state — signed in, crypto-capable
  backend, `sessionTrustState == "Not verified"`. "Unknown" (not yet
  determined) and "Cross-signing unavailable" (no identity to verify
  against) deliberately do NOT prompt. `sessionVerificationWarning` adds the
  per-account dismissal, and ONLY the badges (rail cog, Sessions nav dot,
  corner prompt) read it — the Sessions page states the fact from the
  undismissible property, so silencing the reminder never hides the truth.
  The dismissal is strictly account-scoped (NOT `appearanceValue`, which
  mirrors into a shared global fallback) and is cleared automatically when
  the session verifies, so it can never silence a later unverified session

### Timeline and media

- **Pinned messages** (`m.room.pinned_events`, 2026-08-15). Lightning invents
  NO storage format: the list IS the state event, read through
  `Room::pinned_event_ids()` with `Room::load_pinned_events()` as the
  `/state` fallback, and written through `Room::pin_event()` /
  `unpin_event()`, which do the read-modify-send themselves — so a
  concurrent change can never be clobbered by a stale list of ours.
  A pinned event is usually NOT in the loaded timeline; each id resolves
  through `Room::load_or_fetch_event()` (cache-first, one bounded `/event`
  on a miss, written back to the event cache, decrypted by the SDK in an
  encrypted room). Fan-out is bounded twice: at most
  `PINNED_RESOLVE_CAP` (32) resolutions, sequential, 10 s no-retry each; a
  longer list reports `truncated` rather than issuing hundreds of GETs.
  The COMPLETE id list crosses uncapped, because that is what answers "is
  this pinned?" for the message menu — a capped answer there would be a
  WRONG answer, not a partial one.
  `PinnedMessagesController` tracks the ACTIVE room (not the Room
  Information panel's room, which may be a Space home). It never applies a
  pin optimistically: the write completes, then the authoritative list is
  re-read — on success AND on rejection. A failed READ keeps the last known
  list (a flaky connection must not read as "nothing is pinned any more").
  A remote change arrives as a payload-free `room_pinned_changed` poke and
  is answered by re-reading, so remote and local pins converge on one path.
  The `/state` fallback probe is spent once per room per session.
  Entry previews are decrypted message text in an encrypted room: MEMORY
  ONLY, never CacheStore. Live validation NOT TESTED
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
- **Thread voice messages** (2026-08-13). The thread composer has the same
  mic, pill, waveform, cancel and send as the room composer, reusing the ONE
  shared `VoiceRecorder`. `rooms::send_thread_voice_path` builds the SAME
  `AttachmentInfo::Voice` as the room path and routes through
  `mx_rust_thread_send_voice` → the thread-focused SDK timeline, so the
  `m.thread` relation and encryption stay SDK-owned. There is deliberately
  NO room-send fallback: a thread voice message that cannot reach its thread
  must fail, never land in the main timeline. It hands over BYTES, not a
  path — the SDK resolves `AttachmentSource::File` with `fs::read` INSIDE
  its spawned task, so reclaiming the recording when the panel closes (one
  click after Send) could delete it before it was read, and the advanced
  thread generation would suppress the failure report. Do not switch this
  back to `File`.
  Ownership of the shared recorder is ONE authoritative value
  (`AppController::voiceOwner`), never two per-composer flags: with two,
  recording in the room composer and then in a thread (opening a thread does
  not change `currentRoomId`, so cancel-on-room-change never fires) left
  both armed and one `ready()` sent the same file to BOTH. Ownership is taken
  only AFTER a successful start and is NEVER stolen from a live recorder —
  `VoiceRecorder::start()` refuses while Recording/Processing and returns
  false WITHOUT emitting `failed()`, so moving ownership first orphaned the
  microphone with no pill and no owner, for up to 15 minutes and across
  sign-out. Live mic capture and Element interop: NOT TESTED
- **Thread participant facepiles** (2026-08-13). matrix-sdk-ui 0.18 exposes
  NO participant list: `ThreadSummary` and `ThreadListItem` both carry only
  the root sender, the latest reply's sender and a count of REPLIES — never
  of people, and `num_replies` is not a participant count. So participants
  come from the thread's own events via
  `Room::load_or_fetch_event_with_relations` (cache-first, network only on a
  miss, writes back to the event cache), deduplicated by user id in Rust,
  root sender first then first-appearance order. Only user id, display name
  and avatar mxc cross the FFI — never event content. `ThreadManager` caches
  per (roomId, rootEventId), cleared on sign-out; requests are idempotent per
  root, and an unanswered one is released after 60s so a root never becomes
  permanently un-retryable. An empty list means UNKNOWN, never "nobody" — a
  FAILED lookup is deliberately not cached, and the card falls back to the
  latest sender's avatar. No "+N" badge: the distinct total beyond the cap is
  not known.
  **The fan-out is BOUNDED as of 2026-08-15** (this was the accepted
  follow-up). The timeline is not virtualized, so every loaded root's card
  calls `requestParticipants` on the same frame; `ThreadManager` now runs at
  most `kMaxConcurrentParticipantFetches` (4) at a time with the rest in a
  FIFO queue (capped at 64 — beyond that a root is DROPPED, which keeps it
  genuinely retryable, rather than queued forever). A slot is released by the
  answer **or** by the 60 s timeout, and — importantly — a FAILED (empty)
  answer releases it too, or one failure per round would shrink the pool
  permanently. Deduplication now covers cached, in-flight AND queued roots.
  `setActiveRoom()` (driven from `AppController::setCurrentRoomId`) discards
  QUEUED work for other rooms but deliberately leaves IN-FLIGHT work running:
  the cache key is `(roomId, rootEventId)`, so a late answer can only ever
  populate its own room, and cancelling a fetch already paid for would just
  make a return visit slower
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
- **"Follow account default" and retry on reconnect** (2026-08-13). Matrix
  has no follow-default rule — it has the ABSENCE of a room override — so
  mode 3 routes to `clearRoomNotificationMode` →
  `mx_rust_clear_room_notification_mode` → the SDK's
  `delete_user_defined_room_rules`, and `setRoomNotificationMode` still
  refuses 3 toward the FFI so an invalid `RoomNotificationMode` can never
  cross. Success reports on its own `roomNotificationModeCleared` signal:
  the absence of a rule is not a rule's value, and routing it through
  `roomNotificationModeChanged` meant a successful clear was DROPPED, so a
  clear that failed once claimed "couldn't save" for the whole session and
  was re-issued on every reconnect. Mode 3 is stored EXPLICITLY, not as a
  missing key — an absent key already reads back as 0, so absence cannot
  distinguish "following the default" from "never configured". Clamps are
  0..3 in `SettingsManager` only; the other mode settings stay 0..2.
  `NotificationManager` branches only on Muted/MentionsOnly, so mode 3
  falls through to notify locally — the UI discloses that the SERVER applies
  the account default while THIS DEVICE notifies for all messages, because
  the resolved default is not known here and is not fabricated. The option
  is offered only on a backend that owns server push rules. A write that
  fails offline is retried on the EDGE into Syncing (not on every status
  change), and a room leaves the failed set ONLY when the server
  acknowledges it — never merely because a retry was attempted. Live
  homeserver validation of the deletion and the retry: NOT TESTED

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
- The media autoplay control is labelled **"Autoplay and prefetch media"**
  (2026-08-13) because that is what it governs since the perf round: GIF
  animation, the picker's autoplay, AND the speculative video/audio
  prefetch. The stored key stays `gif/autoplay` and the property stays
  `gifAutoplay` ON PURPOSE — renaming the key would silently reset every
  existing user's preference, which is worse than a stale identifier
- **Pre-send upload-limit preflight** (2026-08-13) against the homeserver's
  advertised `m.upload.size` ONLY. Both fabricated 100 MiB ceilings are
  gone; the Rust one was the worse, reporting an invented value as though
  the server had advertised it whenever the capability lookup failed. 0 now
  means UNKNOWN — never "unlimited", never replaced by a client default —
  and suppresses local rejection entirely rather than refusing files the
  server would have accepted. Voice messages had NO preflight at all and
  now share `AttachmentQueueModel::exceedsUploadLimit`, so the check cannot
  drift between composers. Exactly-at-limit is allowed (`>`, not `>=`):
  `m.upload.size` is the largest ACCEPTED payload. Consequence to keep in
  mind: with no advertised limit there is no client-side ceiling at all, so
  an arbitrarily large file enters the SDK send queue. Re-adding a bound
  would need to be worded plainly as a CLIENT safety limit, never presented
  as the server's
- **Send failures are scoped to where they happened** (2026-08-13).
  `onAttachmentQueueFinished` received a `roomId` and discarded it, so a
  late voice-send failure surfaced over whatever room the composer had since
  moved to. Ops now carry their target room (and thread root). Cleanup of
  the recording stays UNCONDITIONAL so nothing is orphaned on disk; only the
  NOTICE is scoped — those are deliberately not the same decision
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

Version 0.7.3 is released and the synchronized CMake, Rust, and user-agent
version report 0.7.3. Any future version bump is a release checkpoint alone and
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

**The GitHub Release is now created by the pipeline, not by hand
(2026-08-16).** The `mirror-release-to-github` job runs after
`finalize-release` and before the update manifest is promoted: it uploads
the exact published bytes, then re-downloads each asset ANONYMOUSLY and
compares SHA-256 against `dist/manifest.json`. The manual
`gh release create v<version> --verify-tag …` step used for v0.7.0 is no
longer needed and must not be run alongside it.

Two properties that job depends on. GitLab push-mirrors REFS
asynchronously, so the tag may not be on GitHub yet — the job polls for it
(default 300 s) and requires it to peel to the same commit as the GitLab
release; a different commit is a hard failure and nothing is created. And
it passes `tag_name` only, never `target_commitish`, so GitHub cannot
invent the tag against the default branch.

GitHub is a BINARY MIRROR, never a release authority. It decides no
version, holds no signing key, and Lightning reads no GitHub metadata:
clients download artifacts from it first only because the signed manifest
names it, and every byte is checked against a hash GitLab signed. Requires
the project 7 variables `GITHUB_MIRROR_REPO` (protected) and
`GITHUB_MIRROR_TOKEN` (protected + masked); with `GITHUB_MIRROR_REPO`
unset the job is a no-op and no `mirror_url` is emitted.

For an existing release that is missing packages, use
`RELEASE_ACTION=attach-existing` (build, validate, publish, verify, then add
links to the existing release without altering its tag, notes, or source
archives). This was used to backfill `v0.6.1`.

The latest published release is `v0.7.3` (`8da2e81`), cut from its release
commit on `main` by project 7 pipeline **104** in `RELEASE_ACTION=create` mode
(all 19 jobs green on the first attempt; 9 assets published and
hash-verified). Its trigger used exactly the five variables pipelines 102
and 103 used, with `SOURCE_REF` set to the full release SHA, posted as a
JSON body with an explicit `-H "Content-Type: application/json"` — `glab
api --input` without that header returns HTTP 415. All earlier releases and
tags (`v0.7.2` and older) remain immutable and unchanged.

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

**2026-08-18 post-0.7.3 round (handoff top tasks + call pipes).** Landed
after the tester-report fixes shipped as 0.7.3:
- **Both stale timeline suites are GREEN for the first time since
  `1e50f6a`**: `timeline-hydration-qml` 8/0, `timeline-pane-qml` 63/0
  (offscreen, one desktop, one day — run them yourself).
  Two distinct root causes, neither what the fixtures assumed:
  * `initialHydrationGateHoldsThenOpensAtLatest` exposed a REAL production
    defect: after a timeline reset, `fillsViewport` trusted a `contentHeight`
    still reading the OUTGOING content's height (old delegates linger until
    deferred destruction) — measured `count=1 ch=3601 h=404` at gate-open —
    and `contentHeight >= height-1` is degenerately true while `height==0`
    pre-layout. Fixed in TimelinePane.qml: `presentationGeometryStale` (set
    on model reset, cleared by the first Column relayout) plus a `height>0`
    guard; the settled/guard paths still open the gate if geometry never
    moves.
  * `scrolledUpAnchorHoldsThroughGrowthAndAppends` was a FIXTURE bug: view
    rows count from the newest message, so a live append shifts every
    event's view row by one and the test kept measuring view row 15 — a
    different event after the append. The production anchor held within
    2px once the test tracked the event. The two dead eviction tests were
    INVERTED per the standing note (delegates are never evicted in the
    un-virtualized Column): they now pin "the evicted/displaced branches
    must not fire while the delegate is alive" — if eviction is ever
    reintroduced, they fail and the pre-8f84d18 fixtures in history are
    the re-porting start point.
- **GUI stall tracing** (`LIGHTNING_GUI_STALL_TRACE`, src/app/GuiStallTracer)
  for the still-unreproduced tester freeze after hammering reactions:
  heartbeat + watchdog thread, logs one line per stall > threshold
  (default 250 ms, env value >= 50 overrides) with a coarse category from
  RAII scopes (`rust-poll-drain`, `playable-write`; literal strings only,
  never content). New suite `gui-stall-trace` (6 cases). Hand the tester a
  build with it enabled — one capture beats three theories.
- **Element interop checklist**: `docs/element-interop-checklist.md` — the
  scripted PASS/FAIL pass (encrypted both directions, threads, voice,
  video+poster, reactions incl. the D3 hammer test, pins, edit, redaction,
  key-recovery cycle). Running it live is the highest-value next block.
- **Voice-call signaling pipes** (backend only, NO UI): MSC2746 `m.call.*`
  v1 + `m.rtc.notification`/`m.rtc.decline` lane. `rust/src/calls.rs`
  (sends via ruma version-1 constructors + SDK `make_decline_call_event`;
  typed event handlers behind `EventHandlerDropGuard`s bound to
  `run_authoritative_sync`), `mx_rust_calls_*` FFI, SDP-free `CallSignal`,
  `src/calls/CallController` state machine (glare = smaller call_id wins,
  party-id locking + single `select_answer`, cross-device settlement,
  clamped lifetime timers, BOUNDED busy auto-reject with idempotent
  re-delivery, per-call-scoped send-op results, bounded ended-call LRU,
  ignored-sender drop before any state or send, ring-policy/ring-state
  separation, backlog suppression defaults CLOSED; inbound call/party ids
  are sender-chosen text — bounded in Rust, never logged).
  placeCall() REFUSES (`no_media_backend`); `placeCallWithOffer` is the
  future media backend's entry. SDP never crosses the FFI/logs — see
  `docs/voice-calls.md` for the full contract and the deliberate absences
  (no candidates/negotiate, no `m.call.member`, no answering, no media).
  New suite `call-controller` (20 cases) + `calls::tests` in Rust (9). A
  four-lens independent review (§18) ran before commit; its must-fix
  findings (bounded busy auto-reject, live own-user filter, sender-chosen
  id bounding, idempotent re-delivery, per-call op scoping, tracer
  lifecycle) are in. The full registered CTest measured **128/128 on both
  trees** after this round — the first fully green complete run since the
  timeline rebuild. Live interop of ANY of it: **NOT TESTED**.

**2026-08-17/18 post-0.7.2 round** (`4f74eb4..8da2e81`) — **shipped as
0.7.3.** The first REAL upgrade test drove all of it. Windows MSI failed
with 1619 because msiexec has its own argument parser and rejects Qt's
forward-slash path (proven by hand: `/` errored, `\` installed); Windows
portable failed with backup-failed because the swap renamed the install
DIRECTORY while the running helper and its loaded Qt DLLs were mapped
inside it — now an entry-by-entry move, since renaming in-use FILES is
permitted on Windows while deleting them is not (so the backup directory
survives, and a stale one must be cleared or update #2 fails). AppImage
relaunched the MOUNTED binary rather than the .AppImage it had replaced.
Notifications: opening a room notified for its own backlog, because
opening a room subscribes it in sliding sync and the backlog arrives as
live appends while `roomVisibleAtLatest` is false (the view is still
hydrating); a sender with no avatar got the daemon's generic glyph
instead of an initials disc; and the app icon was passed only as a theme
name, which resolves in an installed deb/rpm and nowhere else — a source
build, an AppImage and a Flatpak all fell through to a placeholder.
Updates now announce themselves (corner card + gear badge, dismissal per
VERSION, badge survives dismissal), automatic checks default ON with the
default restated in all four places that claim it, and update DISCOVERY
survives the release server being unreachable via a canonical-first,
mirror-fallback manifest fetch — inert until lightning-deploy publishes
the pair to the `update-latest` tag.

**2026-08-16/17 post-0.7.1 round** (`ea1fd40..7c736c3`) — **shipped as
0.7.2.** Everything below is now released; live validation status is
unchanged by that, and most of it is still NOT TESTED.
Twenty-four commits addressing user-report batches. Newest first:
- **Video poster extraction froze the GUI thread** (`68cb82c`). The
  reported "massive lag spike when scrolling up and videos come up" was a
  SECOND cause, unrelated to the image decode below. `VideoPosterExtractor`
  built its `QMediaPlayer`/`QVideoSink` inline on the GUI thread, and the
  **first `QVideoSink` in a process costs ~931 ms** — lazy Qt Multimedia
  backend init including a hardware-decoder probe that fails without VAAPI
  — plus ~68 ms `~QMediaPlayer` and ~232 ms of `frame.toImage()` per job.
  A GUI-thread heartbeat measured a 937 ms stall against a 1 ms idle
  baseline; on a private worker thread the same extraction measures 1 ms.
  The intuitive per-frame theory was WRONG: `toImage()` is 0.24 ms.
  Two traps this shape invites, both caught in prototype and worth
  remembering: a plain `moveToThread` leaves a MEMBER `QTimer` on the
  creating thread, where Qt refuses to start it, silently disarming the
  6 s watchdog (make it a CHILD); and the reply becomes QUEUED, so
  `disconnect()` no longer reliably cancels one already posted — session
  isolation therefore keys on `m_posterExtracting`, which `clear()`
  empties, not on the connection. `MediaBridge::warmMultimediaBackend()`
  additionally pre-pays the init off-thread for the first inline PLAYBACK,
  whose sink QML builds on the GUI thread and cannot move; it is skipped
  under a guiless app so the media suites still construct no decoder.
  New suite `video-poster-threading`, proven to fail on the old tree
  (891 ms). Residual, unmeasured: `writePlayableFile` still writes up to
  32 MiB synchronously on the GUI thread.
- **Message action bar clipped on a thin row** (`4db1a18`). The hover
  toolbar was anchored INSIDE bubbleRow with a -3px overhang and root
  clips (`clip: ListView.view === null`, load-bearing), so a one-line row
  truncated it. Escaping the clip is right; the FIRST attempt at it
  crashed — a per-row Loader's loaded Rectangle setting
  `parent: Overlay.overlay` keeps the Loader as its destruction owner, so
  delegate churn dereferenced a dangling pointer (bisected against
  timeline-pane-qml). The `detailsDialogComponent` precedent does NOT
  transfer: a Dialog is a Popup and manages its own overlay lifetime.
  Fixed with ONE shared bar declared statically in TimelinePane.qml
  (same lifetime as sharedReactionPicker), into which rows publish only
  PRIMITIVES via claimActionBar/releaseActionBar — never a QObject
  reference, so a destroyed row cannot dangle anything.
  `forceReleaseActionBar` exists because the ordinary release refuses
  while the pointer is on the bar: correct for a live row, WRONG for a
  dying one, where a room switch left the bar on screen over the next
  room still holding the previous room's event id.
- **Scroll consistency** (`5429ab0`): shared `qml/SmoothWheelArea.qml`
  applied to Settings and eight other panes; contract test lists the
  twelve still unconverted rather than hiding them. It uses only
  ScrollTuning's STATELESS `notchDistance()` — `wheelTargetY()` mutates
  controller state owned by the timeline's anchoring.
- **State-flood scroll death: still NOT reproduced** (`970bc75`,
  `ff5dcfe`). The user reports scrolling dying in rooms with many state
  events. Two harnesses say the opposite of the hypothesis: state rows
  cost ~0 ms per wheel notch vs 12-20 ms for the same count of ordinary
  messages, and page inserts stay flat (~6 ms) out to 1063 loaded rows.
  The proxy-suppression fix sketched in `970bc75`'s message was therefore
  NOT shipped — it would be a fourth speculative scroll change, and the
  three before it were withdrawn in review or shipped and regressed. The
  trace now carries `gestureMs`, `worstNotchMs`, `stateRows` and
  `stateGroups` (all behind LIGHTNING_SCROLL_TRACE, counts only).
  **A real capture is the blocker**: a high `worstNotchMs` next to a high
  `stateRows` is the evidence that would justify the suppression work.
  Note the real inefficiency that IS confirmed: a collapsed group drawing
  ONE summary line still instantiates one delegate per member row
  (`modelRows=100 viewRows=100`).
- **Room upgrades / tombstones** (`de05091`) — see §7.
- **Space avatar** (`74319b1`): Space Home already edited name and topic;
  the avatar controls did not exist, so a Space could be renamed but
  never given a picture from inside Lightning. Same permission-gated
  `setRoomAvatar`/`removeRoomAvatar` a room uses — a Space IS a Matrix
  room and this is `m.room.avatar` either way.
- **"Mark as read" was a silent no-op for any room but the open one**
  (`05b2384`). `RoomListModel::markRoomRead` resolved its target by
  walking `MatrixClient::timeline(roomId)`, which on the Rust backend
  only ever holds the ACTIVE room — empty for every other room, so it
  sent nothing and said nothing. `mx_rust_mark_room_read` takes the
  target from the SDK's `Room::latest_event()`, sends the public receipt
  AND `m.fully_read` together, and always clears the manual unread flag.
  Worth recording: the read-marker plumbing was otherwise already
  correct — Lightning sends `m.fully_read` on every in-room advance and
  the SDK derives its own ReadMarker row from account data, so a marker
  set by another client already arrived. Only this entry point was broken.
- **Message forwarding**: content re-sent as a NEW, unrelated event (no
  Matrix forward primitive, no SDK helper). Media is RE-UPLOADED, never
  mxc-copied — under authenticated media the target's members may not be
  entitled to the source mxc, and an encrypted source's `file` block
  carries per-event keys that must not be planted in a room that never
  negotiated them. Carries NO relation, so a forwarded thread reply lands
  as an ordinary message (§8). Filename and MIME are sender-chosen and
  RE-ORIGINATED under this account, so both are sanitized: leaf-only
  filename, and type identified from MAGIC BYTES using the same five
  signatures as `rooms::sniff_image_mime` (NOT `QImageReader::format()`,
  which is plugin-backed — WebP lives in qtimageformats and the packaged
  fleet need not carry it; and NOT `gif::validateRasterBytes`, which
  imports the saved-GIF store's 4096px / 25 MiB caps and would refuse a
  5K screenshot). Four review-caught defects worth remembering: every
  image forward would have written decrypted bytes into the saved-media
  store (the star handler acted on EVERY `mediaBytesForStar`, safe only
  while `starChatGif` was its sole caller); media forwarding to any room
  but the OPEN one failed 100% of the time (`sendAttachmentBytes` gates
  on the live timeline, so `Room::send_attachment` was added); a server
  refusal after dispatch was SILENT (a direct upload has no send queue
  and the target timeline is not open, so nothing fails visibly).
- **`wantsSharedActionBar` binding loop** (regression from the action-bar
  fix, caught by a LIVE CAPTURE not by any test): the binding read
  `activeActionsKey` and its handler called `claimActionBar`, which
  writes it. Fired hundreds of times per pagination run. Keeping the bar
  alive under the pointer never needed that term — `releaseActionBar`
  already refuses while hovered. A binding loop is a WARNING, so the
  suites asserting on QML warnings missed it: they load MessageDelegate
  standalone, and the loop needs a claim to fire at all.
- **Stale timeline suites ported**: `timeline-pane-qml` 52/11 -> **61/2**.
  Not flakiness — six cases asserted `maintainViewAnchor`'s materialized
  branch APPLYING its delta, which was deliberately reversed to NO WRITE
  after physical testing rejected it twice. Also removed
  `diagMaterializedAppliedSum`, which was printed in the scroll trace but
  never incremented since that reversal, so `materializedApplied=0` in
  every capture read as "no growth measured". The 2 remaining failures
  abort on their own fixture precondition (no cache eviction exists in
  the un-virtualized Column) and are deliberately left.
- **First-upgrade validation procedure** (`082d4d0`) in `docs/updates.md`.
Everything user-visible in this round is **NOT TESTED** live.

**#10 RESOLVED as diagnosis, 2026-08-16. Read this before touching
timeline scrolling.** Four hypotheses were proposed and every one was
falsified by a live capture from Rokas's machine. Do not re-propose them:

| Hypothesis | Falsified by |
|---|---|
| State events are the cost | state rows measured ~8x CHEAPER than messages |
| Row count is the cost | `worstNotchMs` flat at 1-2 ms regardless of rows |
| GPU fill-rate at 4K | `render` = 1-4 ms on every slow frame |
| Clipping breaking batching | same — render is never the cost |

**What the evidence says.** `QSG_RENDER_TIMING=1` during a hard scroll at
4K fullscreen:

```
polish=28 sync=18 render=3 swap=2  -> 53ms
polish=10 sync=32 render=4 swap=1  -> 48ms
```

The cost is **polish (Qt item layout) + sync (scene-graph node updates)**,
both CPU-side on the GUI thread. `render` and `swap` are negligible. That
is why the lag tracked WINDOW SIZE rather than row count: a taller viewport
puts more rows inside the 3-viewport `rowOnScreen` band, and every one of
those activates media loaders and layouts.

**A bounded retained window was implemented and REVERTED.** It released
far-offscreen rows while pinning their measured height. It did not produce
a felt improvement over two rounds of testing, a follow-up cap on the
on-screen band made the app FREEZE, and the width-invalidation it required
called `captureViewAnchor()`/`maintainViewAnchorCoalesced()` on every
resize — injecting anchor operations into the machinery three previous
fixes were reverted from, which three anchor-counter tests detected.
Reverting restored "fine, only lags a bit when messages load". **The change
was making it worse**, consistent with the review finding that it added a
build-then-destroy pass to the pagination path.

Residual, accepted: ~60-140 ms per page while backfilling (`perRowMs` 3-7,
~18-20 rows a page). ReverseListProxyModel already paces this with a 3 ms
budget per tick.

**A REAL, MEASURED cause of the "lags when images load" spike was found on
2026-08-17 and fixed (`6ca9d99`) — it is not the polish story above.**
`MediaImageProvider` ignored the QML `sourceSize` on every timeline image.
The rows ask for `sourceSize.width: 640` with the height left 0 (QML's
documented keep-the-aspect idiom), and the provider gated on
`requestedSize.isValid() && !requestedSize.isEmpty()` — but
`QSize::isEmpty()` is true when EITHER axis is below 1, so a width-only
request read as "no size asked for" and the decode fell back to FULL
resolution, bounded only by the 4096 safety edge. A user capture of a
scroll-up shows ~20 paginations each pulling six to sixteen images at
1.7 MB / 1.3 MB / 920 KB, decoded at full size for a 348px box. Measured on
the test fixture: 3.84 Mpx -> 0.27 Mpx, 14x fewer pixels per image.
Upscaling is still honoured when a shape is BAKED IN (an avatar mask
rasterizes once, so baking small then showing large aliases the edge) and
refused otherwise. Whether this removes the felt spike is **NOT TESTED**
live. Note what this does and does not explain: it accounts for the spike
while media loads, and for part of the `sync` cost (texture upload), but the
`polish` finding above stands on its own for scrolling through already
loaded rows.

**If this is picked up again**: profile what `polish` is spending time on
before changing anything. MessageDelegate is built from nested
ColumnLayout/RowLayout and Qt Quick Layouts propagate size hints on every
polish; replacing the hot ones with anchored Items is the mechanical
candidate. Use `perf record` on the GUI thread, not more env-var
experiments. The scroll trace (`gestureMs`, `worstNotchMs`, `stateRows`,
`stateGroups`) and `row-reveal` (`perRowMs`) are already in place and are
what retired all four wrong hypotheses.

**2026-08-15 discovery / search / UIA / moderation / drafts round.** Landed
in one pass after the pins/admin round:
- **Active-room sliding-sync subscription** (user-report fix): sliding sync
  delivers `m.room.pinned_events` ONLY inside a room SUBSCRIPTION's
  required state, and Lightning never subscribed — so a pin (local or from
  another client) stayed invisible until a restart re-ran the once-per-room
  `/state` probe. `mx_rust_timeline_open` now records the open room as THE
  one subscription (`RoomListService::subscribe_to_rooms`, replacing the
  previous set), the modern sync loop applies it on start for restored
  sessions and publishes the service behind an RAII guard, and
  `stop_sync_and_wait` forgets the room so a later account can never
  inherit it. Bonus: the active room gets subscription timeline batches
  (20) instead of the list's 1. Also from that report: a `push_pin` header
  shortcut appears when the room has pins (opens Room Info → Pinned), and
  the profile role buttons' labels are vertically centred.
- **Discover / Join Room** (`discover.rs`, `RoomDiscoveryController`,
  `RoomDirectorySearchModel`, `DiscoverJoinDialog`): directory browse/
  search with `next_batch` paging, identifier/link resolution through ruma
  `MatrixUri`/`MatrixToUri` + `get_room_preview` (a refused preview still
  resolves — Join stays offered), joins via `join_room_by_id_or_alias`
  (+ via servers), knocking via `Client::knock` with optional reason,
  knock withdrawal (a Knocked-state `Room::leave` — the normal leave path
  deliberately filters to Joined), the room-list knocked row with
  Withdraw, Space Home "More rooms in this space" from the SDK's
  `/hierarchy`-backed `SpaceRoomList` (bounded 10 pages/200 rows), room
  matrix.to/`matrix:` links in messages open IN-APP through the dialog
  (joined rooms auto-open and jump to the linked event; user links keep
  the browser), and Quick Switcher commands. Join errors map to honest
  categories — banned / invite-only / restricted (`restricted_denied` is
  classified separately, never presented as plain invite-only).
- **Message history search** (`search.rs`, `MessageSearchController`,
  `MessageSearchDialog`, find-bar History segment): the server `/search`
  endpoint through raw `Client::send` (matrix-sdk has no wrapper), Recent
  order, `content.body` key only, zero-context + historic profiles,
  `next_batch` paging. E2EE POLICY (deliberate): the server cannot search
  ciphertext, so server search covers UNENCRYPTED rooms only and every
  surface says so; inside an encrypted room the loaded-timeline find is
  the only search, and the find bar offers no History segment there. The
  only content sent is the typed search term. Result navigation reuses
  `PaginationController::jumpToEvent` unchanged (bounded; deep history
  reports its honest unavailable message). Global search: Ctrl+Shift+F.
- **Reusable UIA + session sign-out** (`uia.rs`, `UiaController`,
  `UiaPromptDialog`, Sessions page): the privileged call runs WITHOUT auth
  first; a real 401 challenge (`as_uiaa_response`) parks the operation in
  the bridge's single UIA slot (the SDK's own `CrossSigningResetHandle`
  shape), surfaces sanitized stage NAMES only, and the password answer's
  transit buffers are scrubbed BEST-EFFORT (QML field wiped on dispatch,
  C++ QByteArray + Rust String zeroed with volatile writes). Honesty
  (review L1): on the success path the String moves into ruma's
  `uiaa::Password`, which serializes and drops it without zeroing — that
  memory is not scrubbable without patching ruma, so this is transit
  hygiene, never a guarantee. Wrong password re-parks with the refreshed
  session and offers retry; unsupported stages surface honestly. Sessions
  tiles get per-device Sign out + "Sign out all other sessions"; the
  current device is guarded out (that is the logout flow's job); tiles
  only disappear on the authoritative refetch. OAuth/MAS accounts have NO
  password stage: their buttons open the account console
  (`account_management_url_with_action` DeviceDelete/DevicesList) in the
  browser instead — never a fake password prompt. The old "not supported
  yet" disclaimer is gone. The login path's password transit buffer is now
  scrubbed too (it never was), and the login form wipes its field when the
  screen is left — deliberately not on a failed attempt.
- **Ignore + report** (`ignore.rs`, `ModerationController`,
  `ReportMessageDialog`): SDK `Account::ignore_user`/`unignore_user`
  (m.ignored_user_list read-modify-write — never a Lightning-local
  database), list read from account data (0.18 has no accessor), remote
  AND local changes forwarded from the sync loop's
  `subscribe_to_ignore_user_list_changes` arm so everything converges on
  one path. The SDK clears the whole event cache on a list change —
  timelines reset and refetch; that is expected. NotificationManager takes
  `senderIsIgnored` to close the race window before the server stops
  sending an ignored user's events. Report = `Room::report_content`
  (stable /v3, requires Joined, reason optional, no score field in 0.18);
  `report_room` (unstable MSC4151) and `report_user` (absent from the SDK)
  are deliberately NOT offered. Surfaces: profile popover Ignore row
  (account-wide, below room moderation), message menu "Report message"
  (own messages excluded; real room id via
  `TimelineModel::realRoomIdForEvent`, never the thread composite),
  Settings → Privacy "Ignored users" card.
- **Drafts** (`DraftStore`, SettingsManager `roomDraft`/`setRoomDraft`,
  composer/thread hooks): POLICY (maintainer-confirmed 2026-08-15) —
  unencrypted rooms persist drafts locally (strictly account-scoped
  QSettings keys `accounts/<slug>/drafts/<sha16>`, LRU cap 256, wiped with
  the account group); ENCRYPTED rooms are memory-only (survive switches
  and the Settings round-trip, never restart; a room with UNKNOWN
  encryption state fails closed to memory). Payload = text + mention refs
  (restored fail-closed against the text slice) + reply target (restored
  tolerantly — a dangling target never blocks sending); edit state and
  attachments deliberately excluded. Saves are 1 s debounced; the debounce
  is STOPPED before every room/thread change and the save reads the
  still-current key, so a stale timer can never write across rooms;
  send-success and explicit clear retire the draft and stop the timer.
  Memory drafts clear in `clearCrossAccountCaches` and on `loggedOut`.
  The old `Main.qml` "drafts survive" comment is now actually true.
- **Authenticated media hygiene**: the audit confirmed every Rust-backend
  media byte already flows through the SDK Media API (which negotiates
  `/_matrix/client/v1/media` itself); the ONLY reachable legacy surface
  was `RustSdkMatrixClient::mediaDownloadUrl`/`mediaThumbnailUrl` handing
  unauthenticated `/media/v3` links to the browser. Both now return empty
  on the Rust backend.
Everything user-visible in this round is **NOT TESTED** live until the
round's own live-validation pass says otherwise; see the completion report.

**2026-08-15 pins / admin / verification-UX round.** Landed: pinned
messages (§7 Timeline and media), member power levels + join rule +
canonical alias (§7 Rooms and navigation), the bounded thread-participant
fan-out (§7 Threads), the verification move to a focused dialog with
dismissible prompts (§7 Rooms and navigation), and the stale timeline-test
repair below. New suites: `pinned-messages`, `room-power-levels`,
`thread-facepile-bound`. Everything user-visible in it is **NOT TESTED**
live: real `m.room.pinned_events` round trips and Element interop, a
homeserver accepting/rejecting a power-level or join-rule write, alias
publication, and the on-screen look of the pinned tab, role buttons,
verification dialog and corner prompt.

**Stale timeline suites — root-caused, not "flaky".** `timeline-pane-qml`
and `timeline-hydration-qml` had been failing since the timeline was rebuilt
in `1e50f6a`. Three distinct causes, none of them one bug:
  1. **Obsolete ListView API.** `positionViewAtIndex`,
     `positionViewAtBeginning` and `itemAtIndex` do not exist on the rotated
     Flickable + Column; `QMetaObject::invokeMethod` merely returned false.
     Ported to the pane's real view-row API behind three helpers in the test
     file. NOTE the index convention: the pre-rewrite ListView bound
     `model: app.timeline` directly, so its indices were SOURCE rows
     (row 0 = oldest) — the helpers convert, the call sites keep their
     original meaning.
  2. **Direction and end-of-range flips.** The rotation makes physically
     UPWARD mean *increasing* contentY, and the earliest loaded position
     `wheelMaxY()` rather than `wheelMinY()`. Two assertions encoded the
     pre-rotation directions. Production is correct in both cases —
     `goToEarliestLoaded()` is literally `contentY = wheelMaxY()`.
  3. **A mock-fixture QML warning.** On the mock backend `mediaThumbUrl` is
     a plain http URL (the media bridge is the Rust path), so any image row
     asks Qt to resolve `mock.local` and logs one host-not-found warning.
     Six cases asserted `warnings == {}`. Filtered NARROWLY by
     `realWarnings()` — pinned to `mock.local` specifically, so a fixture
     that reached a REAL host still fails; every other warning still fails.
  4. **One genuine flake, now deterministic.**
     `diagUnresolvedIdFallbackCountsGenuinelyUnresolvableAnchor` asserted
     the ABSOLUTE branch counters were zero, but its own touchpad setup
     loop drives real geometry and can legitimately fire the materialized
     branch first. Measured 4 pass / 4 fail in isolation. Rewritten to
     baseline the counters immediately before the call under test and
     assert the DELTA — which is the invariant the test actually names
     ("this call falls straight to the capture fallback"). 8/8 after.
Do not "re-fix" these by reintroducing ListView semantics.

Result: `timeline-pane-qml` **37 passed / 26 failed -> 52 passed / 11
failed**; `timeline-hydration-qml` unchanged at 5/2. The remaining 13 cases
were root-caused and deliberately NOT forced green — they are not one bug:
  * **Unreachable branches (4+).** `diagEvictedNoInsertFallback…` and
    `diagDisplacedBranchCounters…` both abort on their own fixture
    precondition with "fixture no longer evicts the anchor's delegate —
    this test would pass on broken code". Every row is instantiated now, so
    there is no cache eviction and no displaced-anchor branch to enter.
    This agrees exactly with the physical capture already recorded in this
    section (`displacedFirings=0`, `evictedNoInsert=0`). Porting them means
    rewriting the fixture around the ONE displacement that can still occur
    (a live message arriving at view row 0) or inverting them into "this
    branch must not fire" — the latter would preserve a real invariant.
  * **Pre-rotation geometry in the fixture.**
    `maintainViewAnchorAppliesGrowthDeltaMidGestureWithoutGlide` reports
    `before=12 after=12 expectedGrowth=270`: the anchor did not move, so
    nothing needed compensating. On the rotated view, growth at an OLDER
    row sits at higher content y and cannot displace the reader.
  * **Paced row release.** `viewportFillRunCompensatesEveryBatchImmediately`
    fails at `positionAtSourceRow(timeline, 5)` because the proxy has not
    released that row yet; the navigation paths call `releasePendingRows()`
    first for exactly this reason.
  * `initialHydrationGateHoldsThenOpensAtLatest` (hydration) opens the
    presentation gate with one row. NOT diagnosed — it needs its own pass,
    and it is unrelated to the port (it failed identically before).
These are an honest open item, not a claim of completion.

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
- Matrix presence LANDED 2026-08-15, closing the design-handoff follow-up
  list. Sliding Sync delivers NO presence events (MSC4186 has no presence
  extension), so it is a bounded polling loop, stateless on the Rust side:
  `rust/src/presence.rs` answers one `presence_batch` poll event per round
  (raw ruma `get_presence` through `Client::send`, ≤40 users, 10 s
  no-retry timeout so sign-out's task join cannot stall) and publishes own
  state via `set_presence`. ALL policy lives in
  `src/presence/PresenceManager`: only watch()ed on-screen users are ever
  polled (DM rows via the identityColorKey 1:1 gate, People rows, an OPEN
  profile popover — one shared `qml/PresenceDot.qml` owns the
  watch/unwatch lifecycle), 30 s rounds with rotation past the cap plus a
  400 ms debounced burst for new unknowns, transient failures KEEP the
  last known state, forbidden/not_found erase it, and two consecutive
  all-forbidden batches of at least two distinct users each latch "server
  has presence disabled" for the session (reset on sign-out/switch; a
  single user's 403 never latches — smaller batches neither advance nor
  reset the count). Unknown renders NOTHING — never a
  fabricated offline. Own presence (online, or unavailable after 10 min in
  the background; 4 min keep-alive PUT) is gated by the APPLICATION-WIDE
  Privacy & security setting `sharePresence` (default ON, ecosystem norm;
  global like the link-preview switches, not per-account; disabling
  publishes ONE final offline — deferred to the next Syncing edge when
  the session is not live). The popover contract test
  flipped from "presence absent" to "presence via the shared component".
  Live homeserver validation (real dots, latch behavior on a
  presence-disabled server, Element interop): NOT TESTED.
  Prior text for this entry follows. Thread participant
  facepiles LANDED 2026-08-13 (see §7 Threads) — the entry that said they
  "need participant data in the thread-summary bridge payload" was
  misleading: the SDK has no such payload to extend. Voice messages LANDED
  2026-08-12 (VoiceRecorder: Qt Multimedia capture, OGG/Opus preferred with
  AAC/MP4 fallback, real QAudioDecoder-derived waveform;
  mx_rust_timeline_send_voice → AttachmentInfo::Voice, so the SDK emits the
  MSC3245 marker + MSC1767 duration/waveform block via the normal
  encrypting attachment path; the hard duration cap DISCARDS, never
  auto-sends). The thread composer mic LANDED 2026-08-13. Live mic capture
  and Element interop of sent voice events: NOT TESTED. Markdown sending (formatting
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
- From the 2026-08-13 thread-parity round, all **NOT TESTED**: microphone
  capture and Element interop of thread voice events; real-homeserver
  deletion of a room's push rules ("follow account default") and the
  reconnect retry; facepile rendering and real `/relations` cost on a live
  account. Accepted follow-ups from its review, none blocking: ~~bound the
  per-loaded-root participant fetch fan-out~~ — **DONE 2026-08-15**, see the
  facepile entry in §7 Threads (concurrency cap 4 + FIFO queue +
  room-switch invalidation, `thread-facepile-bound` suite); decide whether a
  client-side sanity ceiling should apply when the server advertises no
  upload limit (deliberately absent — see §7); `setVoiceRecorderForTest`
  would be better taking a `unique_ptr`; `voice_info` computes `info.size`
  from the stat size rather than the uploaded bytes; and `FakeRecorder` is
  a PARTIAL double — `stop()`/`durationMs()` are not virtual, so anyone
  needing `stop() → ready()` must extend the seam consciously.

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

The ten tracked `.claude/agents/*.md` role definitions were removed on
2026-08-17 at Rokas's request, and so was the root `AGENTS.md` pointer at
this file. Do not recreate them. The protocol below still applies to any
delegated work — describe the role in the delegation itself rather than
committing a role file. CLAUDE.md is the single tracked guide for every
agent, Claude Code and Codex alike.

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

Runtime team state belongs to Claude Code itself and is never committed.
This protocol is the only tracked part of it, and it must contain no
credentials, tokens, absolute user-specific paths, private endpoints, or
machine-specific values.

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
