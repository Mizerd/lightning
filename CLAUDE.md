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

Latest published release: **Lightning 0.7.6** (`v0.7.6` -> `b13e346`),
notes in `docs/releases/v0.7.6.md`. The application version reads
**0.7.6** in `CMakeLists.txt` (`APP_VERSION_LABEL`), `rust/Cargo.toml`,
and the Rust/HTTP user agent (derived from `CARGO_PKG_VERSION`). It is released, so the next bump is a new
release checkpoint and only on Rokas's explicit request (§14).

`matrix-sdk`, `matrix-sdk-ui`, and `matrix-sdk-base` resolve to
**0.18.0** in `rust/Cargo.lock`; UI and base are exact-pinned in
`rust/Cargo.toml`. Dependencies are lock-file controlled — never update
them incidentally.

### Release inventory (all tags immutable)

| Version | Commit | Deploy pipeline | Notes file |
|---|---|---|---|
| 0.7.6 | `b13e346` | 111, **20/20 green first attempt**, 10 assets | `docs/releases/v0.7.6.md` |
| 0.7.5 | `848a29e` | 110, 18/20 green — mirror wired wrong, mirrored by hand (see below) | `docs/releases/v0.7.5.md` |
| 0.7.4 | `e8139ed` | not recorded here (105 FAILED, see below) | `docs/releases/v0.7.4.md` |
| 0.7.3 | `8da2e81` | 104, 19/19 green first attempt, 9 assets | `docs/releases/v0.7.3.md` |
| 0.7.2 | `7c736c3` | 103, 19/19 green first attempt, 9 assets | `docs/releases/v0.7.2.md` |
| 0.7.1 | `25a01f1` | 102, 19/19 green, 9 assets | `docs/releases/v0.7.1.md` |
| 0.7.0 | `cd91b9c` | 98, 17/17 green, 9 assets | `docs/releases/v0.7.0.md` |
| 0.6.6 | `f35bc8c` | — | `docs/releases/v0.6.6.md` |
| 0.6.5 | `4cdace3` | — | `docs/releases/v0.6.5.md` |
| 0.6.4 | `e719bbe` | — | `docs/releases/v0.6.4.md` |
| 0.6.3 | `97f10b7` | — | `docs/releases/v0.6.3.md` |
| 0.6.2 | `fe3b85f` | — | `docs/releases/v0.6.2.md` |
| 0.6.1 | `86d30b4` | attach-existing backfill | — |
| 0.6.0 | `2157194` | — | — |

Every SHA above predating 2026-08-11 is a **pre-rewrite** identifier
(§4). Run `git log --oneline v0.7.6..HEAD` rather than trusting any
narrative in this file; it goes stale immediately. Never quote a CTest
count from here either — run the suites yourself (§12).

0.7.1 was the first release carrying the secure updater and the first
ever run of `sign-update-manifest` and `mirror-release-to-github`. The
0.7.0 round also built and validated a macOS arm64 bundle on the Mac
mini runner (`BUILD_MACOS_PACKAGES=true`) but deliberately **never
published it**, pending code signing. OAuth/OIDC sign-in is the one
feature block that IS fully live-validated (0.7.0; see §7) — nothing
else in the 0.7.x rounds is.

Releases are package-first: the tag and GitLab Release are created by
the lightning-deploy pipeline only after packages publish and verify
(§14). Never create a tag or release by hand, and never move one.

### What release rounds have learned (operational traps)

- **Trigger variables must be a JSON body.** `glab api --input` without
  an explicit `-H "Content-Type: application/json"` returns **HTTP 415**.
  And passing them as form fields (`-f "variables[0][key]=..."`) is
  **silently ignored**: GitLab creates a pipeline with **zero**
  variables, which then runs as a non-publishing snapshot build and
  reports success while publishing nothing. Pipeline **82** was lost to
  exactly that. Always confirm with
  `glab api projects/7/pipelines/<id>/variables` before trusting a run.
- **A job that consumes a published byte must `needs` its producer.**
  Pipeline **110** published 0.7.5 correctly, created the tag and the
  GitLab release, and then died in `mirror-release-to-github` on
  `mirror input missing` — the macOS zip was in the publication manifest
  but not in the mirror's workspace, because the new `needs` went on
  `publish-packages` alone. The mirror uploads the PUBLISHED BYTES and
  refuses to rebuild them, which is the whole point of it. It failed at
  the most expensive moment in a run: after publication and after the tag
  existed. 0.7.5 was completed BY HAND (mirror + update-manifest
  promotion, both verified anonymously); deploy `86ec616` fixes the wiring
  and asserts the invariant generally — "the mirror consumes every
  artifact source publish-packages does" — so the next format inherits it.
- **Verify anonymously, never from job status.** The bar used for 0.7.1
  through 0.7.3: every GitLab package link returns 200; the `latest`
  manifest fetches, reports the right version, names the right tag, and
  carries `mirror_url` on all 6 artifacts; its Ed25519 signature
  (`key_id: lightning-release-2026a`) VERIFIES against the real public
  key **and a one-field-changed copy is REJECTED** (otherwise the check
  is vacuous); the GitHub release has its 9 assets and its annotated tag
  peels to the same commit as the GitLab release; and a package fetched
  from the mirror matches the GitLab-signed SHA-256 exactly.
- **Anonymous probes 403 under Python's default user-agent** (a
  reverse-proxy bot filter). Test package links with **curl** — a 403
  there is not an access failure.
- **Verify CI job scripts in a real `docker run debian:13.6-slim`.** The
  nix dev shell supplies a toolchain through stdenv and hid two
  publication-blocking failures (no `make`; bare `gcc` without
  libc6-dev). A third burned pipeline asserted an NSIS payload with
  `strings`, which cannot work under `SetCompressor /SOLID lzma`.
  Pipelines 99/100/101 were all lost to CI plumbing before 0.7.1
  published; pipeline 97 lost only `build-rpm` (the spec missed the new
  scalable SVG icon, fixed in lightning-deploy `ca24f16`). Pipeline
  **105** lost `build-deb` to a Qt version difference the dev shell
  cannot show you (Qt 6.11 vs Debian's 6.8.2 — see §16); the same
  container plus `-fsyntax-only` reproduced it and swept all 104
  translation units, instead of finding the rest one 30-minute pipeline
  at a time.
  **A bare configure plus `-fsyntax-only` is worth NOTHING, and will not
  tell you so.** A configure runs no AUTOMOC, no `rcc`, no `qmlcachegen`,
  so every TU including a `.moc`, `qrc_*.cpp`, a qmlcache source or
  `*_qmltyperegistrations.cpp` dies on "No such file or directory". Before
  0.7.6 that produced **466 "failures"** and zero real findings. Run a real
  `ninja` in the container instead — the full non-Rust tree builds there in
  minutes and answers the actual question (0.7.6: exit 0, 1560/1560, zero
  errors). It configures WITHOUT the Rust backend, so
  `RustSdkMatrixClient.cpp` is not covered; judge that file separately.
- **Cancel a doomed pipeline immediately.** It keeps running its other
  jobs and **HOLDS the runners**, so the retry sits pending.
- **The Flatpak application ID changed in 0.7.3** to
  `org.lightning_matrix.Lightning` (was `net.smetonis.Lightning`;
  lightning-deploy `7e84170`). A 0.7.2 Flatpak **bundle is not upgraded
  in place and must be reinstalled**.

### Update / upgrade live-validation truth

0.7.1 was the first release that could be updated **FROM**; 0.7.2 the
first that could be installed **AS** an update. The procedure is in
`docs/updates.md`.

- **Exercised for real** (the 0.7.2 -> 0.7.3 round): the upgrade found
  the Windows MSI failing with **1619** because msiexec has its own
  argument parser and rejects Qt's forward-slash path (proven by hand:
  `/` errored, `\` installed), and the Windows portable swap failing
  because it renamed the install DIRECTORY while the running helper and
  its mapped Qt DLLs lived inside it. Both fixed in 0.7.3.
- **NOT live-validated:** those two Windows fixes cannot be reached by
  updating *from* 0.7.2, because the updater that performs an install is
  the one already on disk. Their first genuine proof is the upgrade
  **INTO 0.7.4**, and there is no record here that it was performed. The
  Setup EXE path was never affected and updates normally.
- **NOT TESTED:** any real AppImage / DEB / RPM / MSI / portable upgrade
  beyond the above; Element interoperability of anything in the 0.7.2,
  0.7.3 or 0.7.4 rounds.
- Windows packages remain **unsigned**; the signed update manifest is
  the integrity guarantee on every platform. Do not describe the
  packages as signed.

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
  matrix-sdk 0.18 (`rust/src/oauth.rs`). PKCE, CSRF `state`, code exchange
  and the refresh REQUEST are SDK-owned; Lightning implements no OAuth
  primitive. Two things the SDK does NOT do itself and must not be dropped:
  * `ClientBuilder::handle_refresh_tokens()` **defaults to FALSE** — without
    it a 401 is forwarded rather than renewed and a saved refresh token is
    inert.
  * The ROTATED pair must be written back (`oauth::spawn_token_persistence`
    on `SessionChange::TokensRefreshed` →
    `SettingsManager::updateSessionTokens`). Skipping it leaves a CONSUMED
    refresh token in the store, and an OAuth 2.1 server treats its reuse as
    compromise.
  `SessionChange::UnknownToken` surfaces as the existing `AccessTokenRevoked`
  state, not an endless sync-failure loop. Added surface is only the
  system-browser launch and `src/auth/OAuthCallbackServer.*` — loopback-only
  (127.0.0.1), ephemeral port, single-shot, size-bounded, timed out; hand-
  rolled because matrix-sdk's `local-server` helper needs `sso-login`/`axum`,
  not vendored in this offline `--locked` build. Costs no dependency change.
  **Two-phase store lifecycle, mandatory.** OAuth learns the user id only
  from `whoami` after the code exchange, so phase A authenticates on a
  bootstrap handle with NO persistent store (in-memory default; it must
  never sync, or it would upload device keys phase B would contradict), and
  phase B derives `AccountIdentity`, applies
  `rust_session::oauthLoginBlockReason()`, then opens the account's sqlite
  store and `oauth().restore_session()`. Rule: a device the server just
  issued must never adopt a store belonging to a different device. That
  block deliberately does NOT suggest a local reset — the store belongs to a
  live device whose keys are still valid.
  Sessions carry an `authType` discriminator in **QSettings, not the
  SecretStore**, so restore routes correctly even with a locked keyring:
  `password` → `matrix_auth()`, `oauth` → `oauth()`. Refresh tokens and the
  dynamic-registration client id are CREDENTIALS in the SecretStore, never
  in QSettings, never exposed to QML, never logged. Legacy Matrix SSO is
  detected and disclosed as unsupported, never offered. Live validation
  (2026-08-15): **PASSED against matrix.org** (MAS/OIDC) end-to-end incl.
  Google-IdP registration, refresh, restart restoration and sign-out — the
  OAuth path is fully live-validated.
- `restore_client()` previously hardcoded `refresh_token: None`, discarding
  a saved refresh token on every restore so an expired access token surfaced
  as `M_UNKNOWN_TOKEN`. Fixed for password sessions as well as OAuth
- Persistent multi-account support: records under `accounts/<slug>/` in
  QSettings, tokens per-user-id in the SecretStore, accessors are views of
  the active account. `AppController::switchToAccount` detaches the local
  session (`MatrixClient::detachSession` emits `loggedOut` for model cleanup
  WITHOUT server logout or store deletion), repoints settings, restores
  normally. Only the active account syncs; removal/logout are scoped to one
  account and logout continues with the most recently added remaining one. A
  failed activation falls back once
- Secret Service/libsecret token storage when available, with an explicit
  insecure QSettings fallback warning
- Rust-backed unified sync/Sliding Sync behavior with compatibility fallback

### Rooms and navigation

- Joined rooms, DM detection from `m.direct`, invites, Space hierarchy, room
  membership/actions, room information, and room creation
- **Two navigation layouts, chosen per account** (Settings → Appearance →
  Conversation list), plus the Spaces rail above both. Full contract in
  `docs/navigation-layouts.md`; read it before touching any of this. The
  load-bearing parts:
  * **Classic** — one activity-ordered conversation list. The default and the
    clamp target for an out-of-range stored value, because it works in an
    account with no Spaces at all.
  * **Channels** — Sable's model: Lobby, Message Search, an Invites group, a
    "Rooms" group for every joined room no Space folder lists (DMs included),
    then EVERY joined Space as a flat collapsible folder of its DIRECT child
    rooms. It no longer falls back to Classic at Home — a layout that becomes
    the other layout depending on where you are is not a layout. The rail's
    selection NARROWS it (`scopeSpaceId`: that Space and its subspaces, with
    the account-wide groups dropped) rather than deciding whether it renders.
    **Lobby is the HEAD of whatever the column shows** — the scoped Space's own
    overview, or Home when nothing is scoped — never a "clear the scope"
    control wearing the wrong name; `lobbyActive` is therefore just "no room
    open". Subspaces are NOT nested; a subspace is a Space folder at the same
    level. A room in two Spaces appears under both. Rows carry the room's
    AVATAR (the lock/DM glyph is a corner badge, not a replacement), and a DM's
    face comes from `DirectAvatarResolver` — ONE derivation shared with the
    Classic list, because `RoomInfo::avatarUrl` is empty on most DMs and this
    column drew initials next to a Home strip showing the real pictures. Order
    is the rail's arrangement for Spaces and `m.space.child` for rooms — never
    activity.
  * **The rail's drag** lives in `RailEntryModel`, a real QAbstractListModel
    emitting `beginMoveRows`, so a preview reorder ANIMATES and the delegate
    holding the gesture survives a refresh. A JS array rebuilt per change is a
    model reset and could do neither. Nothing is written until release. THE
    TILE ITSELF MOVES — full opacity, following the pointer, neighbours
    animating around it; the first revision's dimmed gap, insertion line and
    floating proxy were all cut on testing ("spaces should always be their
    normal image and move freely without a line appearing between them").
    `endDrag` ANNOUNCES the cleared drag flags: `refresh()` may find the rows
    identical and emit nothing, which left a released tile dimmed until an
    unrelated room update happened along.
    **REORDER vs GROUP is measured from the side the pointer arrived from.**
    Short of a row's MIDPOINT the pointer is RESTING on that tile — nothing
    moves, and it is what a release groups with; past the midpoint it has
    PUSHED THROUGH and the dragged block takes the row. The previous rule
    ("the middle 24 px is the group zone") could never fire: reaching that
    middle means crossing the near edge first, which reordered, so the tile
    being aimed at stepped aside and the row under the pointer became the
    DRAGGED entry — never a group target. **No drop ever created a folder, and
    every model test passed throughout**, because they hand the model the
    target's row directly. Resting needs its own verb (`clearDropTarget()`);
    `updateDrag(row, false)` reorders. The 250 ms dwell is now a SECOND guard,
    not the only one. On a group the target lights accent with a 3 px ring and
    the dragged tile PARKS on it at 0.56 scale, so a full-size tile no longer
    covers the ring that says where it would land.
  * **The rail's Space menu** carries Sable's set and names its Space in
    AppMenu's context header: Mark as read, Mute/Unmute, Invite, Copy link,
    Share link…, Space settings. Matrix has no "mark a Space read" and no "mute
    a Space" primitive — a Space is a room with no timeline — so both do what a
    person would do by hand to each room inside it, bounded by the Space's own
    transitive membership, through the ONE per-room path. Unmute restores
    FOLLOW THE ACCOUNT DEFAULT, never "all messages"; Mark as read routes to
    `RoomListModel::markRoomRead`. Links are the PUBLIC `matrix.to` permalink.
    **Invite is deliberately NOT gated on `canInvite`**: that reads
    `app.roomInfo`, which follows whatever surface last pointed it somewhere,
    so gating would grey the row out because nobody LOOKED — a worse lie than
    offering something the server may refuse.
  * **`SpaceSettingsDialog`** (General / Members / Permissions / Developer
    tools) is `RoomInfoController` behind a Space-shaped surface: a Space IS a
    Matrix room, so name/topic/avatar/join rule/alias/power levels are ordinary
    room state, gated on the room's REAL required level and never applied
    optimistically. Fields are EXPLICIT MIRRORS (a keystroke destroys a
    binding; a rejection must snap back; the dialog reopens on other Spaces),
    and it restores `app.roomInfo` to wherever it was pointing on close.
    Sable's Cosmetics / Abbreviations / Emojis & Stickers / Appearance pages
    are deliberately ABSENT — none is Matrix state, so they would be private
    storage only Lightning could read presented as part of the Space. Four dead
    tabs are worse than four missing ones; a contract test bans `app.settings`
    and `app.railLayout` from the file.
  * **A hidden `AppMenuSeparator` now takes NO height.** QQuickMenu lays rows
    out in a ListView that honours each item's height, and a separator's height
    comes from its contentItem plus padding whether it is visible or not — so
    the rail's Space menu opened with a 13 px band above its first row, left by
    the divider belonging to the folder-only rows. AppMenuItem already did this.
  * **Local Space folders** are device-local organisation and touch NO Matrix
    state — banned by contract test, not by convention. Dropping one Space
    onto another creates a folder where the target was; folders never nest.
    The stored format is ADDITIVE, so a 0.7.6 layout loads with its folders,
    membership, order and collapse intact.
  * **Matrix subspaces** are the real hierarchy: only ROOT Spaces sit at the
    rail's top level, a subspace nests under its expanded parent at its REAL
    depth (was a hardcoded 0-or-1 approximation), and a subspace row is not
    draggable because its position is Matrix's. Several parents → nested under
    exactly one, deterministically; cycles → every Space stays reachable as a
    root; parent links only one side reports → resolved from the union.
- **`RoomInfo::childRoomIds` is DIRECT children in `m.space.child` order** on
  every backend. The Rust backend used to fill it from its payload's
  `descendants` (the TRANSITIVE closure), so everything that needed the
  admin's structure saw one flat run of the whole tree — the mock and HTTP
  backends were right, which is exactly why no test caught it.
  `enqueue_spaces` now emits `children` read from each Space's own state,
  ordered by the spec's comparator; `descendants` remains a fallback.
- Quick switching across rooms, direct messages, Spaces, invites, threads
- Activity ordering, unread state/navigation, first-unread and latest jumps,
  threaded receipts, and local marked-unread behavior
- Matrix presence indicators on unambiguous 1:1 DM rows, the People list and
  the member profile popover, via bounded client polling — Sliding Sync has
  no presence extension. §16 carries the mechanism and honesty rules; live
  validation NOT TESTED
- **Member power levels** via `Room::update_power_levels`, which preserves
  every other user's value including arbitrary custom numbers. OFFER policy
  is `RoomInfoController::canSetPowerLevel` (§5), applying what the server
  applies anyway: never above the viewer's own level, never against a peer
  at or above it, self-DEMOTION only, and an unknown target **FAILS CLOSED**
  — levels may legitimately be NEGATIVE (Element's "Restricted" is -1), so
  absence of the roster row, never a sentinel, is the unknown state.
  `roleLabelForLevel` renders 100/50/users_default as
  Administrator/Moderator/Member and **anything else as its number**: a room
  using 42 must not be relabelled 50 and must not be SAVED as 50. Nothing is
  applied optimistically — the write completes, the roster is re-read, so a
  rejection cannot leave a value the room does not have.
  `own_can_change_power_levels` is the SDK's `can_send_state`, never a role
  label. Live homeserver validation NOT TESTED
- **Join rule and canonical alias** in Room Information → Overview, each
  gated on the room's REAL required level for that state event. Only
  `invite`/`public`/`knock` are settable: restricted rules carry an
  allow-rule list this surface cannot build, and sending one with an empty
  list would silently lock the room to invite-only while claiming otherwise
  — a restricted room is displayed honestly and left alone. The alias path
  publishes the directory mapping first (`Client::create_room_alias`) when
  the alias does not already resolve to this room, because a server rejects
  a canonical alias it cannot resolve; clearing sends the state event with
  no alias and deliberately does NOT delete the directory mapping. Both ride
  the MEMBER snapshot, so a successful write must ask for a roster refresh
  explicitly. NOT TESTED
- **Room upgrades / tombstones**: banner-and-link, deliberately **NOT
  auto-follow**. The old room stays open and readable; the successor is
  OFFERED. Security reason: a transition discards navigation and draft
  context, and `m.room.tombstone` is state anyone with the power level can
  send — it NAMES the room you would be moved into. No code path changes the
  current room, joins, or leaves except as the direct result of the user
  pressing the banner. Room ids come ONLY from the SDK's
  `Room::successor_room()` / `predecessor_room()` (ruma `OwnedRoomId`);
  nothing hand-parses `m.room.tombstone` or `m.room.create`. The tombstone's
  `body` **NEVER crosses the FFI** (free text chosen by whoever sent the
  event, on a control the user is invited to click), so the banner uses
  Lightning's own wording. Joined successor → navigate, no join. Invited or
  UNKNOWN → join through `RoomDiscoveryController::join` (so error
  categories and wait-for-room settling cannot drift from Discover),
  navigate once settled; refused → stay put, reason inline. A successor we
  HOLD but cannot enter is the one case reported inaccessible; one never
  heard of is **Unknown**. `chainVerified` requires the successor's
  predecessor to point BACK; false-because-unknown means "not established
  yet", and only a CONTRADICTED chain withholds the room list's de-emphasis
  — a demotion WITHIN the room's own category, never a filter. Permalinks
  untouched. NOT TESTED
- **Unverified-session prompts**: `sessionVerificationNeeded` is true for
  exactly one actionable state — signed in, crypto-capable backend,
  `sessionTrustState == "Not verified"`. "Unknown" and "Cross-signing
  unavailable" deliberately do NOT prompt. `sessionVerificationWarning` adds
  the per-account dismissal and ONLY the badges read it — the Sessions page
  states the fact from the undismissible property, so silencing the reminder
  never hides the truth. The dismissal is strictly account-scoped (NOT
  `appearanceValue`, which mirrors into a shared global fallback) and clears
  on verification, so it can never silence a later unverified session

### Timeline and media

- **Pinned messages** (`m.room.pinned_events`). Lightning invents NO storage
  format: **the list IS the state event**, read via
  `Room::pinned_event_ids()` with `Room::load_pinned_events()` as the
  `/state` fallback (probe spent once per room per session), written via
  `Room::pin_event()`/`unpin_event()`, **which do the read-modify-send
  themselves** — a concurrent change can never be clobbered by a stale list
  of ours. Each pinned id resolves through `Room::load_or_fetch_event()`
  (cache-first, one bounded `/event` on a miss, SDK-decrypted), fan-out
  bounded at `PINNED_RESOLVE_CAP` (32) sequential resolutions, 10 s no-retry
  each; longer lists report `truncated`. **The COMPLETE id list crosses
  uncapped**, because it answers "is this pinned?" for the message menu — a
  capped answer there would be a WRONG answer, not a partial one.
  `PinnedMessagesController` tracks the ACTIVE room (not the Room
  Information panel's room, which may be a Space home), never applies a pin
  optimistically (re-reads the authoritative list on success AND rejection),
  and a failed READ keeps the last known list — a flaky connection must not
  read as "nothing is pinned any more". A remote change arrives as a
  payload-free `room_pinned_changed` poke answered by re-reading, so remote
  and local converge on one path. Entry previews are decrypted text in an
  encrypted room: **MEMORY ONLY, never CacheStore**. NOT TESTED
- SDK-backed live timelines and local echoes
- Text, rich replies, edits, reactions, redactions, typing indicators, read
  receipts, mentions, and room-state activity rows
- Element-style read-receipt chips on live-room rows: newest 16 receipts
  cross the bridge with a truthful uncapped total, and **ONLY the local user
  is excluded** — a user's marker renders even on their own message, as in
  real Element; the earlier extra sender-exclusion made receipts vanish
  asymmetrically when the other side sent (docs/receipt-semantics.md).
  Thread timeline builders deliberately keep receipt tracking **Disabled** —
  SDK receipts are not thread-aware
- Images, files, clipboard images, encrypted attachments, media
  viewing/saving, animated GIF attachments, validated direct-raster previews
- Inline video/audio playback materializes the decrypted payload as a
  session-scoped 0600 temp file (wiped on sign-out/switch/exit); a BOUNDED
  speculative prefetch for on-screen video/audio rows (≤ 32 MiB declared,
  lowest priority, dropped on room switch) governed by the SAME preference
  as GIF autoplay ("never" disables all passive media downloads); and a
  locally extracted first-frame poster for videos without a Matrix thumbnail
  (JPEG, RAM image cache only — never disk). In-flight fetches are
  cancellable end-to-end (QML card → MediaBridge → `mx_rust_media_cancel`),
  and the SDK media store runs a retention policy (max_file_size 24 MiB) so
  large payloads no longer enter or stall matrix-sdk-media.sqlite3
- **Outgoing videos carry a real poster thumbnail.** `AttachmentQueueModel`
  drives the same `VideoPosterExtractor` the receive side uses; the decoded
  frame is also the only honest source of the video's width/height and
  duration on the send side. Dispatch waits for the poster and nothing else;
  extraction failure is NOT send failure. Bytes cross
  `mx_rust_timeline_send_video`/`mx_rust_thread_send_video`, are re-validated
  by magic sniffing (`rooms::PosterBytes`, ≤ 2 MiB, SVG and every non-raster
  refused; a refusal degrades to no thumbnail), and become
  `AttachmentConfig::thumbnail`. **The SDK owns everything after that** —
  upload, encryption alongside the payload, the thumbnail fields on the
  `m.video` event. Nothing in C++ builds thumbnail content or encrypts
  anything. Live Element interop of sent posters: NOT TESTED
- **Element-style Hide image / Show image**, on image and sticker rows only.
  PURELY LOCAL: nothing is redacted, edited, deleted or sent, no other client
  sees anything, and `MediaVisibilityStore` reaches no MatrixClient, no
  SettingsManager and no QSettings (asserted). THE contract is GEOMETRY —
  `MediaHiddenPlaceholder` fills the media box and contributes no implicit
  size, so the row keeps the exact rectangle the picture reserved and the
  timeline does not move; a text row in its place would jump every message
  above it. State is keyed by media identity in the STORE, never in the
  delegate (a timeline row is destroyed the moment it leaves the cache
  buffer). **Session-only, deliberately**: no Matrix standard exists, a hidden
  image the user has forgotten is content they cannot find, and there is no
  hidden-media list to un-hide from — bounded at 4096 keys, and the cap
  releases the OLDEST rather than refusing the newest. Hiding starts NO fetch
  and removes nothing from the cache; the `Image` source is CLEARED (an Image
  with a source still holds the decoded pixmap) and a hidden GIF stops
  animating. Hide is on the action bar and in the menu; once hidden the
  placeholder's Show image is the only control, because a second control
  offering to hide what is already hidden is noise. Live validation NOT TESTED
- Backward pagination and retry, stable navigation, loaded-timeline search,
  message links/permalinks, message details, context menus, sender profiles
- Link previews with encrypted-room privacy controls and security validation
- Smooth mouse-wheel motion, touchpad pixel scrolling, configurable wheel
  speed, keyboard scrolling, and per-room position preservation

### Threads

- SDK `TimelineFocus::Thread` timelines and `ThreadListService`
- Thread panel and per-room Threads view, real `m.thread` text/rich replies,
  follow/unfollow where MSC4306 is supported, threaded read receipts, and
  pagination
- Thread image/file/clipboard attachments through the SDK including
  encrypted rooms, with local echoes, send state, retry/failure handling.
  Thread video sends carry the same locally extracted poster
  (`mx_rust_thread_send_video`), still routed through the thread-focused SDK
  timeline so the `m.thread` relation and encryption stay SDK-owned
- Element-style root summary cards with server reply counts, latest
  metadata, live updates, conservative unread indication
- **Thread voice messages.** Same mic, pill, waveform, cancel and send as
  the room composer, reusing the ONE shared `VoiceRecorder`;
  `rooms::send_thread_voice_path` builds the same `AttachmentInfo::Voice`
  and routes through `mx_rust_thread_send_voice`. Invariants:
  * **NO room-send fallback, ever** — a thread voice message that cannot
    reach its thread must fail, never land in the main timeline.
  * It hands over **BYTES, not a path**. The SDK resolves
    `AttachmentSource::File` with `fs::read` INSIDE its spawned task, so
    reclaiming the recording when the panel closes (one click after Send)
    could delete it before it was read, and the advanced thread generation
    would suppress the failure report. Do not switch back to `File`.
  * Recorder ownership is ONE authoritative value
    (`AppController::voiceOwner`), never two per-composer flags: with two,
    recording in the room composer and then in a thread (opening a thread
    does not change `currentRoomId`, so cancel-on-room-change never fires)
    left both armed and one `ready()` sent the same file to BOTH.
  * Ownership is taken only AFTER a successful start and is NEVER stolen
    from a live recorder — `VoiceRecorder::start()` refuses while
    Recording/Processing and returns false WITHOUT emitting `failed()`, so
    moving ownership first orphaned the microphone with no pill and no
    owner, for up to 15 minutes and across sign-out.
  Live mic capture and Element interop: NOT TESTED
- **Thread participant facepiles.** matrix-sdk-ui 0.18 exposes NO
  participant list: `ThreadSummary`/`ThreadListItem` carry only the root
  sender, the latest reply's sender and a count of REPLIES — `num_replies`
  is not a participant count. Participants therefore come from the thread's
  own events via `Room::load_or_fetch_event_with_relations` (cache-first),
  deduped by user id in Rust, root sender first then first-appearance order.
  Only user id, display name and avatar mxc cross the FFI — never event
  content. `ThreadManager` caches per (roomId, rootEventId), cleared on
  sign-out; requests are idempotent per root and an unanswered one is
  released after 60 s so a root never becomes permanently un-retryable.
  **An empty list means UNKNOWN, never "nobody"** — a FAILED lookup is
  deliberately NOT cached, and the card falls back to the latest sender's
  avatar. No "+N" badge: the distinct total beyond the cap is not known.
  Fan-out is BOUNDED (the timeline is not virtualized, so every root's card
  calls `requestParticipants` on the same frame):
  `kMaxConcurrentParticipantFetches` (4) concurrent + a FIFO queue capped at
  64, beyond which a root is DROPPED, keeping it genuinely retryable rather
  than queued forever. A slot is released by the answer, by the 60 s
  timeout, **and by a FAILED (empty) answer** — otherwise one failure per
  round would shrink the pool permanently. Dedup covers cached, in-flight
  AND queued roots. `setActiveRoom()` discards QUEUED work for other rooms
  but deliberately leaves IN-FLIGHT work running: the cache key is
  `(roomId, rootEventId)`, so a late answer can only populate its own room
- True thread-reply filtering from the live main timeline, cold-cache
  initial loading, stable per-thread scrolling, quick-switch navigation, and
  in-place thread E2EE recovery

### E2EE

- SDK-owned encrypted sending/receiving and persistent crypto store
- Crypto readiness/health model and sanitized recovery diagnostics
- Automatic room-key requests and SDK backup download after decryption failure
- Late in-place decryption updates, manual bounded retry, key import, and
  recovery-key/passphrase backup restore controls
- SAS emoji device verification in both directions, show-QR verification
  (Lightning displays a code the other device scans; SDK-owned reciprocate
  flow, SAS fallback, **never scans** — live Element interop NOT TESTED),
  session/device trust UI, cross-signing/backup state, and
  generation-isolated callbacks

These mechanisms cannot guarantee recovery of historical messages whose keys
were never backed up or shared.

### Notifications

- Native freedesktop notifications when Qt DBus and a notification service
  are available
- SDK-derived mention metadata, direct-message and per-room local modes,
  privacy modes, active-room suppression, invite and verification notices
- Cold-start/backlog suppression, bounded click routing to room/event/thread,
  configurable sounds, and burst coalescing
- Per-room notification modes synchronize to server push rules on the Rust
  backend (SDK-managed; user-defined-rule reports reconcile a device-local
  cache that keeps policy working offline, and a failed write is disclosed
  in the UI as kept-on-this-device). Non-Rust backends remain device-local.
  Live homeserver/Element interoperability of the rules: NOT TESTED
- **"Follow account default" and retry on reconnect.** Matrix has no
  follow-default rule — it has the ABSENCE of a room override — so mode 3
  routes to `clearRoomNotificationMode` →
  `mx_rust_clear_room_notification_mode` → the SDK's
  `delete_user_defined_room_rules`, and `setRoomNotificationMode` still
  refuses 3 toward the FFI so an invalid `RoomNotificationMode` can never
  cross. Success reports on its OWN `roomNotificationModeCleared` signal:
  the absence of a rule is not a rule's value, and routing it through
  `roomNotificationModeChanged` DROPPED a successful clear, so a clear that
  failed once claimed "couldn't save" for the whole session and was
  re-issued on every reconnect. Mode 3 is stored EXPLICITLY, not as a
  missing key — an absent key already reads back as 0, so absence cannot
  distinguish "following the default" from "never configured". Clamps are
  0..3 in `SettingsManager` only; other mode settings stay 0..2.
  `NotificationManager` branches only on Muted/MentionsOnly, so mode 3
  notifies locally, and the UI discloses that the SERVER applies the account
  default while THIS DEVICE notifies for all messages — the resolved default
  is not known here and is not fabricated. Offered only on a backend that
  owns server push rules. A failed offline write is retried on the EDGE into
  Syncing (not on every status change), and a room leaves the failed set
  ONLY when the server acknowledges it, never merely because a retry was
  attempted. Live homeserver validation: NOT TESTED

### Settings, usability, and accessibility

- Eleven complete semantic themes (ids 1–11): Lightning Light, Lightning
  Dark, Graphite, Midnight, Nordic, Purple Dusk, Warm, the design-handoff
  Moss Light / Indigo Night / Deep Teal, and Storm (11), the brand theme.
  **Indigo Night is the flagship** (2026-08-25, maintainer's call): it leads
  the featured cards and System (0) resolves to Moss Light / Indigo Night.
  Storm stays a featured card, fourth, and stays the shell's own chrome. An
  explicitly persisted id is never rerouted, so changing what System means
  touches nobody's stored choice. The identity discs damp the magenta wedge
  (290-350 degrees, saturation x0.55) so a cool accent stops producing pink
  fallback avatars; hues are unmoved, so per-theme families and the dE 19.7
  all-pairs separation are unchanged.
  AppTheme.qml is the sole token source; the theme test enforces palette
  completeness, routing, and WCAG AA pairs. The storm* namespace (menus,
  popovers, Settings) is theme-ROUTED: Storm literals under theme 11, each
  legacy theme's own semantic tones otherwise. There is NO invariant
  exception left — the Sessions trust card was the last one, and 2026-08-26
  deleted its ten pinned `trust*` tokens and routed it here too, because a
  brand-navy card sitting between themed SettingsCards was the one surface
  on the page that read as foreign. Ink on a bolt/accent fill uses boltInk,
  never stormPanel — the trust card's complete-node glyph was the case that
  proves it: it only looked right as `trustNavy` because the pinned fill
  happened to be navy, and routed unchanged it would have painted the page
  ground onto a yellow disc
- The four-pane design shell: 68 px spaces rail (home, Spaces, settings,
  account avatar + switcher popover), 300 px room list with workspace header
  and Ctrl-K hint, timeline with members/threads side panel, card composer;
  bundled Manrope/JetBrains Mono fonts; application icon and desktop entry
  installed by CMake (data/, scripts/generate-icons.sh)
- The full-view Settings screen covers the whole content area (chat shell
  loaded but hidden — no rail, room list, timeline, composer or right panel
  while open; closing restores the selected room with the right panel
  remaining None), 60 px header above 260 px navigation (Account,
  Appearance, Notifications, Privacy & security, Sessions, Labs; About
  pinned bottom). Appearance carries featured theme cards, a match-system
  switch, a FUNCTIONAL message-layout selector (Modern / Bubbles for DMs /
  Compact) and a text-size slider (90-140%) — theme, layout and text scale
  persist per account with a global fallback. Avatar shapes are baked into
  the cached bitmap by MediaImageProvider ("|shape:" suffix) instead of
  per-item MultiEffect masks. Headless/offscreen runs force stderr logging
  in main.cpp because Qt otherwise routes category logs to the journal when
  stderr is no TTY
- Room-activity visibility, link/GIF preview policy, notification privacy
  and sound, per-room notification mode, and wheel-speed settings
- The media autoplay control is labelled **"Autoplay and prefetch media"**
  because that is what it governs: GIF animation, the picker's autoplay, AND
  the speculative video/audio prefetch. The stored key stays `gif/autoplay`
  and the property stays `gifAutoplay` **ON PURPOSE** — renaming the key
  would silently reset every existing user's preference, which is worse than
  a stale identifier
- **Pre-send upload-limit preflight** against the homeserver's advertised
  `m.upload.size` ONLY; both fabricated 100 MiB ceilings are gone (the Rust
  one reported an invented value as though the server had advertised it).
  **0 means UNKNOWN** — never "unlimited", never replaced by a client
  default — and suppresses local rejection entirely rather than refusing
  files the server would have accepted. Voice messages share
  `AttachmentQueueModel::exceedsUploadLimit` so the check cannot drift
  between composers. **Exactly-at-limit is allowed** (`>`, not `>=`):
  `m.upload.size` is the largest ACCEPTED payload. Consequence: with no
  advertised limit there is no client-side ceiling at all; re-adding a bound
  would have to be worded plainly as a CLIENT safety limit, never presented
  as the server's
- **Send failures are scoped to where they happened.**
  `onAttachmentQueueFinished` received a `roomId` and discarded it, so a
  late voice-send failure surfaced over whatever room the composer had since
  moved to. Ops now carry their target room (and thread root). Cleanup of
  the recording stays UNCONDITIONAL so nothing is orphaned on disk; only the
  NOTICE is scoped — deliberately not the same decision
- Unicode emoji picker with search, tones, and bounded local recents
- Keyboard quick switch/search/navigation, accessible labels/roles/actions,
  focus handling, and keyboard-operable message/thread actions

### GIF provider integration

Implemented: strict GIPHY and KLIPY parsing behind a shared provider
interface (provider-specific endpoint/key/rating/pagination, attribution), a
provider-agnostic search controller and result model with stale-response
rejection and deduplication, and bounded redirect-validated HTTPS transport
through the Rust backend. The user-facing browser is implemented too: shared
room/thread picker with provider tabs, trending, debounced search,
client-side categories, pagination, attribution, favorites, bounded local
recents, safe-search rating, configurable autoplay, accessible
keyboard-navigable tiles. The safe validated download pipeline (HTTPS-only,
revalidated redirects, bounded size, GIF magic and dimension validation) and
the send path into a room or a real Matrix thread — uploading through the SDK
media path, with SDK media encryption in encrypted rooms — are implemented.
Existing GIF attachment/direct-media playback remains separate and
implemented. Live Element interop of provider GIF sends: to be tested
honestly rather than assumed.

**Saving GIFs** is implemented; the star accepts every safe static raster the
timeline shows (GIF, PNG, JPEG, WebP). Bytes are validated by magic sniffing
— never a claimed MIME or file name; SVG and everything else refused —
stored in their ORIGINAL format as `<sha256>.<ext>` (no transcoding), and
re-sent with a truthful MIME and dimensions. Legacy index entries without a
format field load as GIF, so existing saved GIFs survive with no migration.
The store is account-scoped and content-addressed, bounded at 200 items /
64 MiB by **refusal, never eviction** — a full store must not silently
discard what the user asked to keep. Sends go from local bytes.

A star means exactly one thing everywhere — "save this GIF" — with one
destination: the picker's **Saved** tab, which renders `GifSavedModel`, a
presentation-only `QConcatenateTablesProxyModel` merge of the local byte
store and the provider favorites. The two **stores stay separate**, because
only one of them holds decrypted media. Each tile carries its own source tag
(GIPHY/KLIPY/LOCAL). Saved and Recent issue **no provider API request** — no
search, trending, pagination or category call is reachable from either — but
they are not offline: a saved *provider bookmark* is a link, so its tile
still loads its preview from that provider's CDN. Only locally-saved rows
are pure device-local content; do not describe the Saved tab as having "no
provider traffic".

Never read `GifResultModel::FavoriteRole` from a `GifStoredModel` as an "is
this saved" oracle: that role is a constant `true` for every stored
collection — honest for favorites and local-saved rows, a lie for Recents.
Ask the collection (`GifFavoritesModel::isFavorite`).

This is a deliberate, documented exception to the §6 rule against persisting
decrypted media, on explicit-export semantics: the user chooses to save one
image, exactly as Save-As already allows. It is only defensible because
deletion is REAL — the store is removed on sign-out and on account removal
through a shared path helper with tri-state deleted/absent/failed reporting
(an earlier version *claimed* this cleanup and did not have it; decrypted
media would have survived sign-out indefinitely). Settings → Privacy &
security discloses the store and offers Clear All. The index records **no
provenance**: no room, event, or sender. Do not weaken any of that, and do
not extend the exception to other media without the same guarantees.

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

Version 0.7.6 is released and the synchronized CMake, Rust, and user-agent
version report 0.7.6. Any future version bump is a release checkpoint alone and
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

The latest published release is `v0.7.6` (`b13e346`), cut from its release
commit on `main` by project 7 pipeline **111, 20/20 green on the first
attempt** in `RELEASE_ACTION=create` mode — the first fully clean release
run since the macOS lane was added, and the proof that `86ec616` fixed the
mirror. Its trigger used the same SIX variables 110 used
(`RELEASE_ACTION=create`, `RELEASE_VERSION`, `SOURCE_REF`,
`PUBLISH_PACKAGES=true`, `BUILD_FORMATS=all`, `BUILD_MACOS_PACKAGES=true`)
— posted as a JSON body with an explicit
`-H "Content-Type: application/json"`; `glab api --input` without that
header returns HTTP 415. All earlier releases and tags (`v0.7.4` and older)
remain immutable and unchanged.

**macOS is published from 0.7.5**, as a download-only asset, on Rokas's
explicit decision. Apple Silicon only and macOS 26 or newer — both derived
from the Qt build the bundle links, not chosen — ad-hoc signed and
un-notarized, so the download page carries the Open Anyway walkthrough.
Two invariants keep it safe and both are asserted in project 7's
`tests/test-pipeline-config.py`: the release never DEPENDS on the Mac (the
job is `allow_failure` and the `needs` entry `optional`, so one host being
asleep publishes without the asset), and the bundle never enters the signed
update manifest (the client has no macOS install strategy, so an entry
would advertise an install the updater refuses). See
lightning-deploy `docs/macos-packaging.md`.

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

Source and `git log` are authoritative. This section is a LESSON INDEX,
not an inventory: it exists so an agent does not repeat a past mistake.
Narrative and chronology have been cut; rules, refuted hypotheses,
deliberate decisions and validation status have not.

### Standing warnings

**Timeline scrolling — read this whole block before touching it.** Five
rounds, three reverted fixes, several measurement errors of my own.

*Refuted hypotheses. Do not re-propose any of these.*

| Hypothesis | Refuted by |
|---|---|
| State events are the cost | state rows ~8x CHEAPER per notch than ordinary messages; page inserts flat (~6 ms) out to 1063 rows |
| GPU fill-rate at 4K | `render` = 1-4 ms on every slow frame |
| Clipping breaks batching | same capture — render is never the cost |
| De-layouting MessageDelegate's nested ColumnLayout/RowLayouts | `perf record` named `QQuickItemPrivate::transformChanged` at 19.2% of cycles and `polishItems` at 1.0%. Buried twice; do not revive |
| The anchoring machinery displaces readers | every anchor counter zero on every line of every live capture (`anchorCorrections=0 displacedFirings=0 prependFirings=0 unresolvedId=0 evictedNoInsert=0`) |
| Pagination teleport | did not reproduce 2026-08-12; structurally impossible now (positive-only guard, below) |
| `worstNotchMs` measures frame cost | it times the wheel HANDLER, never the frame: 0-2 ms live while frames cost 14 ms. It once wrongly retired the row-window idea |
| Offscreen per-notch cost transfers to hardware | offscreen uses the software rasterizer where `syncSceneGraph`/`updateDirtyNode` dominates: 10.65 ms/notch offscreen at 1000 rows vs 0-2 ms on the GPU |

**Offscreen perf numbers are not the user's numbers.** Any scale-with-N
result measured offscreen must be confirmed on hardware before anything
is built on it.

*Measured.* Frame cost tracks TOTAL instantiated rows, not what is on
screen. `QSG_RENDER_TIMING=1` on Rokas's GPU, pagination frames
excluded: ~108 rows = 3 ms median frame (1% over 16 ms); ~916 rows =
14 ms (46% over, polish 5.6 / render 8.3). Cost is CPU-side polish+sync
on the GUI thread; `render`/`swap` are negligible. Residual accepted:
~60-140 ms per pagination page (`perRowMs` 3-7, ~18-20 rows a page),
paced by `ReverseListProxyModel` at 3 ms per tick.

*Shipped 1 — never-laid-out empty `Text` items (`d1ddc2f`).* Every
`QQuickText` is BORN carrying `ItemObservesViewport`
(`QQuickTextPrivate::init`). The ONLY code that clears it is
`QQuickText::setText`, which opens with `if (d->text == n) return;` —
*before* its `setFlag(ItemObservesViewport, n.size() > 10000)` line, so
a binding that keeps producing the same empty string never clears it.
**Visibility is never consulted** (an earlier revision wrongly blamed
invisibility; correlated, not causal).
`QQuickItemPrivate::transformChanged` only switches off its per-subtree
walk once NO descendant observes the viewport, so a few such Labels per
row made Qt walk the whole instantiated tree on every `contentY` change:
3000 observers across 1000 rows. Fixed with seven `Loader`s in
MessageDelegate.qml and ThreadSummaryCard.qml (whose `timeLabel()`
returns `""` without an SDK timestamp, so the hazard lives inside a live
card too). Observers 3000 → 0; offscreen per-notch 33.89 → 10.39 ms at
n=1000. Felt improvement on a real desktop: **NOT TESTED**.
**GENERALIZE: in a per-row delegate, a `Label` whose text can be `""` in
the state it is created in belongs in a `Loader`** — including labels
reading message fields, which are ALL empty on a virtual
date-divider/read-marker row. Single most expensive QML mistake known
here. `timelineRowsCarryNoPermanentViewportObservers` walks the real
item tree and requires ZERO (139 on the pre-fix tree). Unmeasured
follow-up: `continuationTimestamp` churns one Label per row crossed on
hover; the alternative costs a text layout per row at load.

*Shipped 2 — decoded image size (`6ca9d99`).* `MediaImageProvider`
ignored `sourceSize` on every timeline image: rows ask for
`sourceSize.width: 640` with height 0 (the documented keep-the-aspect
idiom) and the provider gated on
`requestedSize.isValid() && !requestedSize.isEmpty()` — but
**`QSize::isEmpty()` is true when EITHER axis is below 1**, so a
width-only request read as "no size asked for" and decoded at FULL
resolution. Fixture: 3.84 Mpx → 0.27 Mpx. Upscaling is still honoured
when a shape is BAKED IN (an avatar mask rasterizes once) and refused
otherwise. **NOT TESTED** live.

*Shipped 3 — speculative media waits for a settle.* A live capture of
one 15 s upward gesture (442 wheel events, ~45 pagination pages, 19 →
813 rows) showed ~120 MB of video pulled because every row that merely
SWEPT THROUGH the on-screen band armed a full-payload prefetch, each
completion writing its temp file synchronously on the GUI thread. ONE
gate, `speculativeMediaAllowed: !userScrollActive`, consulted by the
video payload prefetch, the video POSTER path (which prefetches
internally via `videoPosterSource` → `prefetchPlayable`, so gating only
the obvious call site leaves half the traffic) and the audio card.
**Thumbnails are deliberately NOT gated**: small, and they are what the
reader is looking at.

*Shipped 4 — jump-to-live history trim (`f40da33`).* The un-virtualized
Column instantiates every paginated event permanently. **Lightning
implements no unloading of its own**: matrix-sdk 0.18 already does it —
`RoomEventCache::subscribe()` bumps a `subscriber_count`, and at zero
`auto_shrink_linked_chunk_task` calls `shrink_to_last_chunk()`; this
round adds only ORDERING. **`abort()` only REQUESTS cancellation**, so
the old task still owns the `Arc<Timeline>` holding the count up;
`await_event_cache_shrink` awaits that handle (a `Cancelled` join IS the
success signal), bounded so a slow task degrades to no-trim. **Never
`RoomEventCache::clear()`** — it wipes PERSISTED events, forcing even
the live tail to be refetched. Policy is a PURE predicate
(`AppController::historyTrimAllowed`) so every clause is testable: Rust
backend, room open, not mid-pagination, **no thread panel or Threads
view open**, >400 loaded rows. The thread clause is load-bearing twice:
a thread timeline holds its OWN event-cache subscriber (so the shrink
could not fire) and the reload would tear its live subscription out from
under an on-screen panel. ONE call site, contract-pinned: the FAR branch
of `goToLatest()` — wiring it to scrolling or pagination would reset a
reader's timeline out from under them — committing (`stickToBottom`,
`saveFollowingLatest`) ONLY on a real dispatch success. Deliberate side
effect: `onModelReset` now closes the row-anchored surfaces on EVERY
reset, including the same-room recovery reload. **LIVE-VALIDATED PASS**
(`cachedBefore= 1083 released= true reloadedItems= 19`). The payload
carries both `trimmed_from` and `trim_shrunk` so a timed-out wait cannot
look like a successful trim, and the baseline must be sampled BEFORE the
release or a fast shrink makes a genuine trim report `released=false`.
`await_event_cache_shrink` has NO automated coverage at any layer (no
mock-room harness in `rust/`).

*Shipped 5 — the row window (`b74b518`, made live by `7092eab`).*
`ReverseListProxyModel` carries a window: `windowSkip` (how many of the
NEWEST source rows are excluded) plus the exposed count, every
transition a single insert-or-remove at ONE end — never a reset, never a
mid-list renumbering. **`windowSkip == 0` is the only state in which
proxy row 0 is the live edge**, so the pane must return to 0 before the
reader can reach the bottom. Policy (`applyRowWindow`,
TimelinePane.qml): `windowRunwayRows` 220 below the reader (≈30
viewports vs a largest observed gesture of ≈7.5 — that runway is the
strand-prevention), `windowMarginRows` 120 above, never below
`windowMinRows` 320, move only for a change of 40+ rows. **Applied ONLY
from the scroll-settle timer** — no structural change mid-gesture, which
is what sank the reverted bounded retained window. With a window active
`atBottomEdge()` returns FALSE so follow-latest cannot latch onto a
false newest message. Correction on a newest-end release is ONE exact
write: sum the MEASURED heights of the released rows (the Column has no
spacing, so a plain sum is exact) and subtract from contentY. A deferred
`Qt.callLater` snap-by-anchor-id was tried and REMOVED — it runs BEFORE
the Column relayout, reads the anchor's stale y and clamps against the
new shorter content, dumping the reader at the top. Do not add a second
correction path alongside `maintainViewAnchor`. Silent failure modes:
- **A skip change renumbers every view row.** View-row helpers must
  subtract `rowWindowSkip` or every jump/search/anchor-restore resolves
  to "no such row" and does nothing. Four instances; none threw.
- `releasePendingRows()` must clear the WINDOW, not just the pacing cap
  (`releaseAll()` lifts `m_windowCap`, leaves `m_windowSkip`).
- Live-edge paths must RESTORE the live edge: `wheelMinY()` under a
  window is a synthetic newest edge, so `goToLatest()` glided to a
  message that was not the latest. It now refuses the glide while a
  window is held, and `settleAtLatest()` calls `releasePendingRows()`
  itself rather than trusting the trim's model reset.
- `revealNextChunk()` bounded its release loop on `sourceRowTotal()`
  instead of `revealTarget()` (`9adcdc9`): one tick released straight
  through the cap — 230 rows against a cap of 60. Only bites once the
  exposed count drops below the cap.
- **Thrash guard, thresholded on the ENTER band.** Trimming the OLDEST
  end shrinks `contentHeight` and so `wheelMaxY()`, and
  `distanceFromTop()` is `wheelMaxY() - contentY`, so a trim moves the
  reader's measured distance from the top with no visible movement while
  `applyRowWindow()` ends in `updateStickAndPaginate()` — dispatching a
  backfill that regrows what was released. TALL-VIEWPORT only (fixed
  ~4020 px kept margin vs a `2.5 * height` enter band): a test at 420 or
  1400 px passes on broken code, so the suite uses 2160 px. Thresholding
  on `nearTopExitDistance` OVER-fires — that is hysteresis for a reader
  already in the band; what dispatches is crossing INTO it.
- **The window's old edge re-exposes LOCAL rows; it does not ask the
  server**, or the window creates a stall where none existed. It rides
  the proxy's paced reveal (`extendWindowAtOldEnd`); a synchronous
  `setWindow(skip, count+120)` would build 120 delegates at once,
  360-840 ms. It deliberately does NOT consume `nearTopArmed`.
- Acceptance: `rowWindowBoundsRowsWithoutMovingTheReadersMessage`
  (900 → 376 rows, the reader's own event moves 0 px, bar 2 px, both
  directions) plus three review-driven cases including
  `rowWindowTrimNeverFeedsTheNearTopPaginationBand`.

*It shipped as a PERMANENT NO-OP and a live capture caught it.*
`userScrollActive: moving || wheelAnimating || scrollSettleTimer.running`
and `applyRowWindow()`'s only caller is `scrollSettleTimer.onTriggered`,
where that timer still reads as running — so `if (userScrollActive)
return` was UNSATISFIABLE at the one call site that exists. Fixed with
`viewportMotionActive` (`moving || wheelAnimating`); `userScrollActive`
is left alone because the speculative-media gate deliberately includes
the settle tail. **GENERALIZE: a policy test that invokes the policy
function directly proves nothing about whether production ever reaches
it.** The policy was covered six ways and the trigger not at all;
`wheelScrollingIntoHistoryEventuallyBoundsRowsThroughTheSettleTimer` now
drives real wheel notches and waits, calling nothing. The window had
ZERO observability, which is how it shipped unnoticed: the gesture trace
now carries `srcRows`, `winSkip`, `winApplies`, and `rows == srcRows`
with a deep reader, or `winApplies=0`, is the signature.

*Still unproven.* **No production frame-cost improvement has ever been
observed from the row window** — the felt "better by a lot" in the live
capture belongs to the speculative-media gating. The judge is a fresh
`QSG_RENDER_TIMING` capture with `winApplies` > 0, `rows` ≪ `srcRows`
and median frame cost deep in history falling toward 3 ms. The window
only acts when SETTLED, so it does not help during the long upward
scroll itself. Also open: which stall category (`row-reveal`,
`image-decode`, `timeline-diff`, `timeline-reset`) owns the logged
333/369/1062 ms GUI stalls — do not guess a fix before that line exists.
`writePlayableFile` still writes up to 32 MiB synchronously on the GUI
thread (unmeasured).

*The positive-only anchor guard: do not "fix" it.* The timeline is a
rotated `Flickable` + `Column` (`qml/TimelinePane.qml`), not a ListView:
`contentHeight` is the exact sum of real rows and `originY` can never
move. View row 0 is the NEWEST message at content y 0
(`sourceRowForViewRow = count-1-row`), so backward pagination lands at
HIGHER view rows and higher content y, past the reader; a prepend does
not change the anchor row's index or y and the displaced branch is never
reached. The only insertion that displaces a scrolled-up reader is a
LIVE message at view row 0 — a genuinely positive `grew` the guard
already applies. Three attempts failed here (two withdrawn in review;
the staging/freeze window `225c7b3` shipped, regressed and was removed
in `263268b`). A fourth needs a `LIGHTNING_SCROLL_TRACE=1` capture
naming a failure: a non-zero `displacedApplied`, `anchorCorrections` or
`materializedMaxAbsDelta`. All-zero lines are not evidence.

*Element (classic) was read for this and does NOT animate.*
`ScrollPanel.scrollToBottom()` is a bare `scrollTop = scrollHeight`;
`TimelinePanel.jumpToLiveTimeline()` builds a NEW `TimelineWindow` at
the live edge and DISCARDS everything paginated. Its height-based
unfilling (`UNPAGINATION_PADDING = 6000`,
`UNFILL_REQUEST_DEBOUNCE_MS = 200`, relative `scrollBy` from a tracked
node's `offsetTop`) works because DOM removal is nearly free — exactly
why Lightning's bounded-retained-window attempt was reverted (no felt
improvement, a follow-up cap made the app FREEZE, and its
width-invalidation injected anchor calls on every resize into the
machinery three fixes were reverted from). Incremental unfilling while
scrolling remains deliberately NOT done.

**Qt version differences the dev shell cannot show you.** Pipeline 105's
`build-deb` died on `CallController.h` holding
`return m_mediaBackend != nullptr;` INLINE where `m_mediaBackend` is a
`QPointer<CallMediaBackend>` and that class is only forward-declared:
comparing a `QPointer<T>` against `nullptr` instantiates
`QPointer<T>::data()`, whose `static_cast<T*>` requires T COMPLETE.
**Qt 6.11 (nix dev shell) never reaches that path; Qt 6.8.2 (Debian —
every deb/rpm/AppImage job) does.** Fixed by moving the accessor into
the `.cpp` (`e8139ed`). Generalize: a `QPointer<T>` MEMBER of an
incomplete type is fine; any inline comparison or dereference of it in a
header is not. Raw `T *p = nullptr` comparisons are always fine.
Reusable method: `docker run debian:13.6-slim` + `qt6-base-dev` +
`-fsyntax-only` reproduced the exact error, and a sweep of all 104
translation units proved it the only occurrence. Check CMake
conditionals before believing a sweep hit (one apparent hit compiled
only under `LIGHTNING_ENABLE_SCREENSHOT_DEMO`; two were missing dev
packages). **Cancel a doomed pipeline immediately — it keeps running its
other jobs and HOLDS the runners, so a retry sits pending.** (Third time
that note has earned its place.)

**TapHandlers are non-exclusive across subtrees**, and TapHandler points
are PARENT-local. A right-click on a non-modal popup's tile also reached
the message context menu beneath it (fixed by making the emoji picker
MODAL with `dim:false`); a facepile tap also pinned the bubble's action
toolbar; a receipt popover opened displaced because its handler lives in
`receiptRow`, not the strip. Any overlaid affordance needs an explicit
band exclusion in the handler beneath it. Recurred in three rounds.

**Timeline test conventions — do not "re-fix" these.** The rotated
Flickable + Column has no `positionViewAtIndex`,
`positionViewAtBeginning` or `itemAtIndex`; `QMetaObject::invokeMethod`
merely returns false. Physically UPWARD means *increasing* contentY, and
the earliest loaded position is `wheelMaxY()`, not `wheelMinY()`
(`goToEarliestLoaded()` is literally `contentY = wheelMaxY()`). On the
mock backend `mediaThumbUrl` is a plain http URL, so image rows log one
`mock.local` host-not-found warning — filtered NARROWLY by
`realWarnings()`, so a fixture reaching a REAL host still fails. Assert
DELTAS of branch counters, not absolutes, when the fixture's own setup
can legitimately fire a branch. There is no delegate eviction in the
un-virtualized Column, so the two eviction tests are INVERTED to pin
"these branches must not fire while the delegate is alive"; if eviction
returns they fail and the pre-`8f84d18` fixtures are the re-porting
start point. View rows count from the newest message, so a live append
shifts every event's view row by one: a test measuring a fixed view row
is measuring a different event afterwards (a FIXTURE bug once misread as
an anchor defect). A binding loop is a WARNING, and suites that load
MessageDelegate standalone will miss one that needs a claim to fire.

**`indexOfValue()` is -1 at creation time** (evaluates before
valueRole/model settle; -1 was masked by `Math.max`). Combos must sync
their index explicitly.

### Round history (newest first)

Lessons only; features are described in §7, SHAs point into `git log`.

**2026-08-26 tester round: the rail drop (again), the slow account switch,
the People chip, the log, the settings, and the Discord call surface.**

*The rail drop had never worked, and this is the SECOND round that thought it
had fixed it.* The "resting on a tile" branch of `updateTileDrag` ended in
`updateDrag(row, !dwellTimer.running)` — and `running` is TRUE for the whole
250 ms the dwell is being served, so the second pointer sample inside the
target's near half REORDERED, the target stepped aside, the row under the
pointer became the dragged block, and the branch that then fired called
`dwellTimer.stop()` on the very dwell it was waiting for. Grouping needed a
frozen mouse. Both retired rules had the same shape: **a reading that moves
things while the user is still aiming.** The rule now is Discord's — the TILE
is the group target, the GAP between tiles is the reorder target, nothing
moves while the pointer is on a tile, and there is no dwell because the
geometry carries what the dwell was standing in for. One flag became three
exclusive verbs (`hoverGroup` / `hoverGap` / `clearDropTarget`); `updateDrag`
was REMOVED rather than shimmed, because its premise is the defect. The
reorder destination is now derived from a GAP index with the
`g > dragRow ? g - length : g` conversion the row-index version never had —
which is separately why a one-row hover used to park the block under the
pointer and oscillate. `tests/RailDragQmlTest.cpp` drives REAL
`QTest::mousePress`/`mouseMove`/`mouseRelease` at tile centres resolved from
real delegate geometry and asserts on the STORE; **all six cases were run
against the reverted tree and all six failed**, case 2 reporting the
mechanism in words ("target row moved from 3 to 2 on pointer sample 6").
GENERALISE, third time: the fifteen model cases passed through both broken
rules because they hand the model a state production could not produce.

*The slow account switch was an unbounded profile-fetch loop introduced by
`0b38f8c` itself.* `DirectAvatarResolver` cached a profile answer only when
it carried a NON-EMPTY avatar, but announced EVERY answer; `SpaceChannelModel`
was wired to that signal and its `rebuild()` calls `resolveMissing()`. So for
every DM peer with no avatar — and every 404 — rebuild → fetch → answer →
rebuild ran forever: one `/profile` request and one full model rebuild per
network round trip, per such peer, for the whole session. A switch clears the
caches, which is exactly what re-armed it. The in-source comment asserting
"resolveMissing() is guarded by its own cache, so this cannot feed itself"
was false for the two commonest answers. Fixed by caching the negative
(`m_noAvatar`, session-scoped, cleared on sign-out) and announcing ONLY a
face actually learned. Two further costs removed in the same lane: the
rebuild is coalesced to one per event-loop turn, and it resolves child rooms
through `SpaceManager::directChildRoomIds(spaceId, byId)` against the map it
already built instead of `directChildRoomsDetailed`, which materialised the
whole room list and a fresh hash PER SPACE. Why no test saw it: the fixture's
`FakeClient` inherited `fetchUserProfile() { return 0; }`, and the resolver
skips its pending bookkeeping entirely on op 0, so the failure mode was
structurally unreachable in the harness.

*The People chip was not inert — the layout had nowhere to put a person.*
The Binding reached `setFilterMode` and `rebuild()` ran correctly; a SCOPED
Space dropped the account-wide "Rooms" group, and that group was the only
place a DM could live, so People produced exactly two navigation rows over
blank space with no wording at all. **A DM is never scoped by a Space, in any
filter** — Matrix gives no way for a DM to be a Space's child — which is the
rule Classic had already reached. DMs also gained a group of their own:
showing nothing but people under a heading that says "Rooms" looks like the
filter did not take. And the column can now say a filter matched nothing
without claiming the ACCOUNT is empty (`matchCount`, distinct from `empty`).

*One log line fired once per DUPLICATE CALLER.* `avatarSource()` was the only
one of five `alreadyPending()` branches that logged, and that branch is
reached once per caller, so its volume is O(callers) — unbounded in a list.
Twelve per-request lines moved to `lightning.media.trace`; one counts-only
burst summary lands on the default category once activity goes quiet;
failures keep their own line. Separately `qml/Avatar.qml` called the bridge
from three triggers per instance, one of which (`onSizeChanged`) could not
change the request at all because `avatarSource` opens with `Q_UNUSED(size)`.
GENERALISE: a line that fires per CALLER does not belong in a default-on
category; only state transitions do.

*The screen share opened on one frozen picture, and `videorate` is why.*
Measured in the dev shell: with input `framerate=(fraction)0/1` and output
pinned `30/1`, videorate emits NOTHING for the first buffer — it holds it
until a second arrives — then back-fills the whole gap in one sub-millisecond
burst of duplicates timestamped across it. A PipeWire capture delivers ON
DAMAGE, so that gap is "how long until the screen moves", and a libwebrtc
receiver renders on the frame timeline. `pipewiresrc keepalive-time=100`
re-pushes the buffer the element already holds. **It is not `min-buffers`
wearing a new name** — that changed the pool negotiated with PipeWire and
killed the capture; this touches no caps, no pool and no negotiation. 100 ms
and not one frame period is a DIAGNOSTIC choice: a keepalive resend counts as
a delivered frame, so at this floor a dead capture still reports ~10/s
against a live capture's up-to-30/s and the counters keep diverging. Two
alternatives measured and refuted: `skip-to-first` changes nothing on this
input shape, and `max-duplication-time` keeps the hold AND starves the
encoder below the pinned 30 fps.

*A `json!` that grows past serde_json's macro recursion limit is a compile
error that names no key.* Adding a nested `power_levels` object to the member
snapshot produced "recursion limit reached while expanding
`$crate::json_internal!`" pointing at the macro, not at the addition. Hoist
any nested object out into its own `let` before the outer `json!`.

*Two `type == StateChange` guards had to learn a new enum value.* Giving call
events their own `TimelineEvent::CallEvent` silently un-suppressed them in
`NotificationManager` (an EMPTY desktop notification per call, since the
sentence is built in `TimelineModel`) and in `RustSdkMatrixClient`'s activity
test (blanking the room-list preview). Both were found by reading, not by a
test. GENERALISE: when a row stops being a `StateChange`, grep every branch
that tests for one.

**2026-08-26 rail-drop / Space-menu / log-noise round.** Contract in
`docs/navigation-layouts.md` §2 and §4b.

*The gesture the whole feature exists for had never once worked, and fifteen
model tests passed the entire time.* Dropping a Space onto a Space always
reordered and never made a folder. The model was correct; the VIEW could not
reach it. The band rule was "the middle 24 px of a row is the group zone", and
reaching that middle means crossing the row's near edge first — which reordered,
so the dragged block took the row, the target stepped aside, and by the time the
dwell elapsed the row under the pointer held the DRAGGED entry, which is never a
group target. The tests passed because they call `updateDrag(rowOf(target),
true)` — they hand the model the row production could never produce. **This is
the row-window lesson again: a policy test that invokes the policy directly
proves nothing about whether production ever reaches it.** GENERALISE further:
when a pointer gesture has two readings a few pixels apart, the reading has to
be measured from the side the pointer ARRIVED from, and "resting on" needs its
own verb — reusing the one that also mutates ("reorder") is what closed the door.

*Two `Layout` bindings that read their own layout's output.* The log was a
wall of `Detected recursive rearrange` (one per pass, per row, in a Space's
list) plus `Binding loop detected for property "implicitWidth"` on every
message carrying a fenced code block. Same shape both times: a cap read from
something the layout PRODUCES. `Layout.maximumWidth: parent.width * 0.7` where
`parent` is the enclosing RowLayout; and a segment sized against `bubble.width`,
which in Bubbles mode IS `bubbleContent.implicitWidth`, which is the segments'.
Both fixed by measuring against a row whose width comes from ABOVE and which
reports no implicit width of its own. GENERALISE: in a Qt Quick Layout, a
child's size constraint may only read a width that the layout does not compute.

*An invisible `MenuSeparator` still reserves its height.* QQuickMenu lays rows
out in a ListView that honours each item's height, and MenuSeparator's comes
from its contentItem plus padding regardless of `visible`. That was the "empty
space at the top" of the rail's Space menu: the divider belonging to the
folder-only rows. AppMenuItem had `implicitHeight: visible ? … : 0` already —
the separator did not.

*A field's derivation that lives privately in one model will be wrong in the
next one.* `RoomInfo::avatarUrl` is empty for most DMs; the room list had
carried the whole peer-avatar derivation privately since 0.6.x, and the Channels
column — written later, deliberately NOT reading RoomListModel — drew initials
for every DM next to a Home strip showing the real faces. Extracted to
`DirectAvatarResolver`, owned by both. Its late answer runs a `rebuild()`, not a
bare `dataChanged`: the rows hold a SNAPSHOT, so repainting them would repaint
the same initials.

*A brand mark painted in the raw accent reads as a status light.* The
wordmark's trailing bolt sat in primary-action blue next to plain header text
and was reported as "the blue lightning session status". `AppTheme.wordmarkBolt`
keeps Storm's brand yellow and blends the accent most of the way to the header's
own secondary ink everywhere else.

*The bundled Material Symbols font is a SUBSET.* A name that is not in
`Icon.qml`'s map renders as tofu, and regenerating the subset needs the network.
`IconChromeTest::everyReferencedIconNameIsMapped` catches it; pick from the
mapped set rather than reaching for the "right" glyph.

**2026-08-25 Spaces / Channels / hide-image round.** Full contract in
`docs/navigation-layouts.md`; read it before touching any of this lane. The
lessons worth carrying:

*A JS array bound to a ListView is a model RESET on every change, and that
costs two things at once.* The rail's rows were `railEntries = arrange(...)`, a
plain array reassigned whenever the model or the layout changed. So a reorder
could not animate (no `move`, no `displaced` — a reset has neither) AND the
delegate holding a live drag was destroyed the moment anything refreshed. Both
halves of the report — "it works, but it's kinda hard to tell exactly where you
are moving them" — have that one cause, and no amount of feedback drawn on top
would have fixed either. **GENERALISE: if rows must MOVE, the model has to be
able to say so.** `RailEntryModel` emits a real `beginMoveRows`, defers any
refresh that arrives mid-gesture, and writes settings exactly once, on release.

*Two gestures a few pixels apart need a dead zone AND a dwell.* "Between two
Spaces" (reorder) and "onto a Space" (make a folder) are separated by about
12 px. A dead zone alone is not enough: dragging slowly through a tile's centre
on the way somewhere else still arms the group gesture. A 320 ms dwell in ONE
tile's centre band is what makes an accidental folder impossible. The band is
measured against the TILE, not the row — an expanded Space's row is much taller,
and centring on that puts the group band over its revealed rooms.

*The mock backend being RIGHT is how a backend defect survives.*
`RoomInfo::childRoomIds` is documented as the Space's DIRECT children, and it
was that on the mock and the HTTP backend. The Rust backend filled it from its
payload's `descendants` — the transitive closure — so
`directChildRoomsDetailed` was not direct, and the Channels layout listed a
subspace's rooms twice with no structure visible. Fifteen model tests passed
throughout, because they all ran against the mock. **GENERALISE: when a field's
contract is enforced only by the backend that happens to be testable, the other
backends are undefended.** Fixed by reading each Space's own `m.space.child`
state in `enqueue_spaces` (`direct_children_of`, spec comparator: `order` key
first, room id as the tiebreak, empty-`via` removals skipped) and emitting it as
`children`, with `descendants` kept as a fallback.

*A layout that becomes the other layout is not a layout.* Channels used to scope
itself to the active Space, so at Home there was no hierarchy and the host
rendered Classic instead. The user chose a navigation layout and silently got
the other one. The fix was not a better empty state — it was removing the
premise: `SpaceChannelModel` has no `spaceId`, the rooms come from the CLIENT
rather than from `RoomListModel` (which is scoped to the active Space and
filtered by the chips, so the global "Rooms" group would have vanished the
moment a Space was picked), and `channelsUsable` is the user's choice alone.

*`level = parentSpaceIds.isEmpty() ? 0 : 1` is a two-level approximation that
looks like a hierarchy.* A three-deep tree rendered as a flat pair of indents.
Real depth comes from a breadth-first walk with assign-once semantics, which is
also what makes it cycle-safe and stable under multiple parents — a Space the
walk never reaches becomes a ROOT rather than being dropped, because a Space the
user has joined must stay reachable whatever its state says.

*The hidden-image contract is GEOMETRY, not visibility.* Replacing a 360×270
picture with a text row jumps every message above it, which for a
hide-this-image control is a worse outcome than the picture.
`MediaHiddenPlaceholder` fills the media box and contributes no implicit size at
all; the suite measures the box and the row before and after. Two smaller traps
in the same feature: an `Image` whose `visible` is false still holds its decoded
pixmap (clear the SOURCE), and an `AnimatedImage` behind an opaque placeholder
keeps decoding a frame at a time for something nobody can see.

*An offscreen capture reads a `Behavior` animation that never advanced.* Four
rounds of probes "proved" that the Channels column marked the wrong row —
Lobby highlighted with a room plainly open, and the open room's own row not
marked. Every reading came from a `--demo-capture` at the default 1400 ms
settle. The rows' `Behavior on color` starts a 90 ms ColorAnimation when the
delegate's state flips, and offscreen that animation had not advanced by the
capture, so the grab held each row's CREATION-time colour. At a 6000 ms settle
every row was correct and always had been. **Nothing was wrong with the code**;
one speculative "fix" was made on that false reading and reverted. GENERALISE:
a pixel from an offscreen grab is only evidence once every animation that
touches it has had time to finish — and a property probe rendered into a LABEL
can disagree with the pixel for exactly this reason, which is what makes the
contradiction diagnosable.

*A model that emits nothing when nothing changed will also emit nothing when
your FLAGS changed.* `RailEntryModel::endDrag` cleared the drag state and left
`refresh()` to announce it — and `applyRows` legitimately early-returns when
the rows are identical, which after a drag they usually are. So a released
tile kept rendering as dragged (dimmed, with the insertion line still under
it) until some unrelated room update happened along: "their icons get darkened
after moved and let go and only clear up after entering a room". Per-row
PRESENTATION flags that live outside the row data have to be announced by
whoever clears them.

*Where the state cannot live.* Not in the delegate — a timeline row is destroyed
the moment it leaves the cache buffer, so the flag would be gone by the time the
reader scrolls back. `MediaVisibilityStore` is keyed by media identity, bounded
at 4096, and the cap releases the OLDEST rather than refusing the newest:
refusing to hide something the user just asked to hide is the worse failure.

*A negated character class matches newlines, and it ate two lines of source.*
`NavigationLayoutContractTest`'s comment stripper removed trailing comments
with `(?m)\s//[^"']*$`. `[^"']*` happily crosses newlines, so a trailing `//`
comment consumed every following line until one ended in a quote — the ban
assertion that then read `setSpaceMuted`'s body simply reported the code
absent. **Every scan in that file positioned AFTER a trailing comment was
silently weakened by it.** The class needs `\n` in it. GENERALISE: a
"strip comments" regex is a parser, and the cheapest way to find out whether
yours works is to assert something you KNOW is present and watch it fail.

*A collapsed folder cannot be reported on, so it must not be written over.*
`applyArrangement` takes the whole arrangement in one write, and a folder LEFT
OUT of the call keeps its members. The rail only renders an open folder's
members, so without that rule a drag past a collapsed folder would silently
empty it.

**2026-08-24/25 MatrixRTC interop round — calls carry media at last.**
Audio, camera and screen share now work in both directions against Element,
live-validated. Sixteen separate defects; the full table, the REFUTED
theories and the harness traps are in `docs/matrixrtc.md` ("what was tried,
and what actually worked") and must be read before touching this lane again.
The load-bearing lessons:

*A counter downstream of `videorate` cannot tell you the capture is alive.*
`videorate` repeats the last picture to hold the output rate, so a DEAD
screen capture still produces full-rate encoded, encrypted and sent frames —
every counter healthy, both ends frozen on one image. Count the capture's own
buffers, before it. This masked the real fault for several rounds.

*A desktop capture is VARIABLE RATE.* PipeWire negotiates
`framerate=(fraction)0/1` (delivery on damage, not on a clock) and the panel's
native size — measured 3840x2160 BGRA. A publish caps framerate RANGE
including `0/1` propagates that to `vp8enc`, which then has no rate to plan
against. Fixing the framerate at `30/1` is what finally made Element render;
sizes stay ranges because they are ceilings.

*`rtpvp8pay` parses the VP8 bitstream and therefore cannot payload an
encrypted frame.* It reads partition0's size, a keyframe's `0x9d 01 2a` start
code, then bool-decodes segmentation fields out of the compressed partition.
libwebrtc's packetizer takes that from the encoder as METADATA and never
reads the payload. Hence `src/calls/RtpVp8Payloader.*` — descriptor plus MTU
fragmentation, reading nothing.

*element-call mints a 16-byte media key*, livekit-client 32. Requiring 32
rejected every key Element ever sent, for its LENGTH, so an Element peer
could never be heard — while our own media reached them normally.

*Identify a received track by its TRACK SID from the msid, never by a `mid`.*
A `TrackInfo.mid` belongs to the PUBLISHER's connection; our subscriber
transceiver's mid is assigned independently.

*Verify against something that is not Lightning.* Two Lightning clients agree
on streams a libwebrtc receiver rejects. `livekit-cli` (pion) as a subscriber,
and an independent implementation of LiveKit's frame crypto, each refuted a
confident wrong theory. `tests/CallLiveDiagnostic.cpp` drives a real call
headlessly and SKIPs without `LIGHTNING_LIVE_*`.

*Harness bugs produced false findings repeatedly* — `startSync()` returns
silently before login completes, publishing before `Connected` puts no track
on the wire, sampling the SFU before a share starts looks like a forwarding
failure. Suspect the harness first when a measurement indicts something
distant. One guess (`min-buffers=8`) was shipped without measurement, made
things strictly worse, and is now pinned against by a test.

**2026-08-23 disk + test audit.**

*`QAbstractSocket::waitForReadyRead()` cannot work against a server on
the SAME thread* — blocking the caller is precisely what stops the
listener accepting the connection. `SsoCallbackTest::deliver()` ended in
`waitForReadyRead(3000)`, so every one of its eleven deliveries burned
the full bound and the request was only handled once the CALLER started
spinning the loop: **34.5 s of a 34.5 s suite**. Removing the wait
outright takes it to **1.4 s**. Pumping the loop in the helper instead is
WORSE: the server then answers before the caller arms its `QSignalSpy`,
and `QSignalSpy::wait()` waits for a NEW signal, so seven cases fail.
The bytes are already in the kernel buffer after
`waitForBytesWritten()`, and `close()` is a graceful FIN, so the server
still reads the whole request — the caller's own wait is what drives it.

*Contract-suite duplication is the redundancy this repo actually
accumulates.* `QmlBindingContractTest::gifPickerWiredIntoBothComposers`
had grown to 190 lines, 170 of them GifPicker INTERNALS under a name
that promises only the composer wiring; **26 of its 41 needles were
asserted a second time** in `GifPickerRedesignContractTest`, which its
own comments already pointed at. Detection is mechanical: extract every
`contains(QStringLiteral("…"))` needle per suite and rank the suite
PAIRS by intersection. Nothing else in the tree came close (next
highest overlaps are shared FIXTURE strings, not assertions). Two other
findings from the same sweep, both left alone deliberately: 23 suites
each declare their own `MatrixClient` subclass with ~13 identical
`override {}` stubs (~300 lines; a shared test double would be a
23-file cross-cutting change), and `media-bridge` failed ONCE under
`-j18` while passing alone every time — load-sensitive like
`timeline-pane-qml`, not a regression, and not yet pinned.

*Where the disk goes.* A debug `libmatrix_client_rust.a` is **2.1 GB**,
and every one of the ~146 test binaries links it: `matrix-client` alone
is 906 MB in `build-rust` against 124 MB in `build`, and the test
binaries total **35 GB** there versus 5.1 GB in the non-Rust tree.
Add `rust/target/debug/incremental` (25 GB) and
`build-rust/rust/debug/incremental` (13 GB) and the repo was 157 GB.
The incremental caches are pure caches — deleting them costs only the
next build's incremental state, not the compiled `deps/`. Separately,
`nix store gc` freed **63 GB**; pin the dev shell FIRST
(`nix develop --profile <path> -c true`, which registers a root under
`/nix/var/nix/gcroots/auto/`) or the GC takes the whole Qt/Rust
toolchain with it. A durable fix for the binary size —
`split-debuginfo = "unpacked"` in `[profile.dev]`, so debug info stops
being copied into every binary — is NOT applied: it is a build-config
decision for Rokas, and `[profile.release]` (which packaging uses) is
unaffected either way.

**2026-08-23 tester round (banners, quit, window state, login labels).**

*An imperative write to a bound property destroys the binding — five
places had it.* Every media-cache completion handler that did
`img.source = bridge.someSource(key)` unbound that `Image` from its own
key for the rest of the session, so the FIRST image that ever finished
loading through that path was the last one it ever showed. Reported as
"I click my own profile, it loads the banner; then I click anyone else's
and it replaces whatever they had with mine", plus the same for Space
banners; the composer reply thumbnail, the Settings own-banner card and
the link-preview thumbnail had it too. Fix is a `resolveTick` counter the
binding READS and the handler bumps — the binding survives, so a new key
still replaces the image and an absent one still clears it. Empirically
confirmed in Qt 6.11 that an unused local (`var _t = resolveTick`) does
create the dependency. Where the source came through an intermediate
`readonly property bridgeSource`, the tick has to live in THAT binding,
not in `source` — a tick in the wrong binding is a silent no-op.
`mediaCacheHandlersNeverAssignABoundSource` scans every QML file.
Also: the handler must key on the cache key
(`mxc:<edge>:<uri>` / `thumb:<mediaKey>`), or one completion re-resolves
every such Image in the app.

*`Qt.quit()` is a REQUEST, and close-to-tray was refusing it.*
`QGuiApplication::event(QEvent::Quit)` closes every top-level window
first and `e->ignore()`s the quit if one refuses. `onClosing` set
`close.accepted = false` to hide into the tray, so Ctrl+Q hid the window
— in the one mode where the tray icon deliberately has no menu and
Ctrl+Q is the ONLY documented way out. Fixed with a `quitRequested` flag
the shortcut sets before calling `Qt.quit()`; still `Qt.quit()` and not
`Qt.exit()`, because AppController's teardown and UpdateManager's
apply-on-quit hang off `aboutToQuit`.

*Window geometry must be restored in BINDINGS, not `Component.onCompleted`.*
Qt shows the window during its own `componentComplete()`, which runs
BEFORE any `Component.onCompleted`, so geometry applied from a completion
handler lands after the window is on screen and the user watches it jump.
The stored value is therefore read through a CONSTANT property
(`AppController::restorableWindowGeometry`) — a notifying one would feed
the save back into the binding that produced it — and Qt breaking those
bindings when the user drags the frame is the desired behaviour, not a
bug. Two invariants: a size below the window's own minimum is REFUSED on
write, because Qt reports transient 0x0/1x1 geometry while a window is
shown, hidden into the tray or restored from minimized, and the tray path
fires exactly when the last good value must survive; and only the
WINDOWED state is stored, with maximized as its own flag. `QScreen` stays
out of `SettingsManager` (~20 test targets link it against `Qt6::Core`
alone), so the "is this still on a connected screen?" half lives in
AppController — tested as a BAND along the top of the frame, so a window
spanned across two monitors is not refused. `QWindow::show()` forces the
NORMAL state, so restoring from the tray must set `visible = true`
instead or a maximized window comes back un-maximized (and now that the
flag persists, that would be written back as the user's choice).

*A `SplitView` width was never saved at all.* `onWidthChanged: if
(!SplitView.view.resizing) save()` skips every intermediate pixel
correctly and then never fires again — the RELEASE moves nothing, so it
produces no `widthChanged`. The falling edge of `SplitView.resizing` is
the one moment that matters. GENERALISE: when a guard suppresses a signal
for the whole duration of a gesture, something has to fire at the END of
it.

*Login button naming, from Element classic's actual strings.* Both
"Continue in browser" (OAuth/OIDC) and "Sign in with SSO" (legacy
`m.login.sso`) open a browser, so naming the MECHANISM told the user
nothing; what differs is which authority authenticates them. Element's
order is `["oauthNativeFlow", "m.login.password", "m.login.sso"]`, its
labels are `Continue with %(provider)s`, `Sign in with single sign-on`
and a bare `Continue` for a delegated-auth flow, and SSO is primary only
when there is no password flow. Lightning now uses the same words, names
the homeserver host on the browser button (derived from what the USER
typed — a server must not choose the words on Lightning's own button),
and separates the password form from the browser buttons with "Or".
**The i18n catalogs are NOT updated**: they were already ~27 source
strings behind before this round, and `lupdate` rewrote all 10 files
(53k lines) while warning "Removed plural forms as the target language
has less forms" — a real plural-damage risk. Those labels render in
English on non-English UIs until a dedicated localization refresh.

**2026-08-21 UI rework round (`c3f2393..17e269e`, 13 commits).** Emoji
input, the scroll teleport, mentions, and a design-system rebuild.

*The scroll teleport — FOUR paths, and the reported one was not the
obvious one.* (1) The navigation-landing budget added in `f4e6525` was
CONVERGENCE-based: it reset whenever `count`/`layoutRowsAtLastPass`
changed, and during a scroll BOTH change constantly (pagination, the row
window's settle, every Column pass), so it re-armed forever and fired
whenever the target became measurable. Now has an absolute ~2 s ceiling.
(2) **The actual "about 10 seconds"** is a scroll-anchor RESTORE:
`restoreScrollAnchor` can spend up to `kMaxNavigationBatches` (8) REAL
network paginations before emitting `targetLocated`, and its target IS
"a spot I was at before". Cancelling the landing in the VIEW cannot help
— the landing does not exist yet when the reader starts scrolling — so
`PaginationController::cancelNavigation()` exists and
`noteReaderTookControl()` calls it. (3) Middle-click autoscroll left
`userScrollActive` FALSE for the whole gesture (it writes contentY
directly so `moving` stays false, and `updateStickAndPaginate` never
restarted the settle timer), so `maintainViewAnchor()` took its IDLE
branch and ABSOLUTELY restored contentY on the next content-height
change. Pre-existing since v0.7.4. (4) Keyboard paging retired nothing.
GENERALISE: a convergence budget needs an absolute ceiling, and the
reader taking the view must reach EVERY layer that can move it.

*A Popup does NOT consume a press that lands on it.*
`QQuickPopup::mousePressEvent` sets `accepted = blockInput()`, and
`blockInput()` returns FALSE when `popupItem == item` — so delivery keeps
walking down to items behind the overlay. **`modal: true` blocks presses
OUTSIDE a popup only.** The 2026-08-18 emoji fix rested on the opposite
premise and was inert. Left presses survived by ACCIDENT (GridView is a
Flickable, and Flickable is constructed `LeftButton`-only); right presses
reached MessageDelegate's `Qt.RightButton` TapHandler. Fix: an
all-buttons `MouseArea` sink in the popup's `background:` (bottom-most
hit-testable item, below `contentItem`). Do NOT fix with `z`.

*`visible: running` on a shared busy indicator is a permanent latch.*
Hosts use the inverse idiom `running: visible`; together they cycle, and
`QQuickItem::visible` is EFFECTIVE visibility, so an indicator created
under a hidden ancestor writes `running=false`, then `explicitVisible=false`,
and nothing re-triggers either. Silent (no `visibleChanged`, so no
binding-loop warning) and order-independent. A component owns its
animation; the HOST owns visibility.

*A defaulted C++ parameter that QML must pass fails silently.*
`setMentionStyle` gained `linkColor` and NOTHING passed it, so every URL
and every non-self mention rendered in the accent for the whole round.
Pin the arity in a test.

*Colour: measure before believing the symptom.* "Needs more colour"
turned out to be SEPARATION — Storm is the app's MOST saturated shell
(Lab chroma 27.1 vs Moss Light 0.8), but every surface step was below
1.25:1 and `cardElevated`/`hover`/`selected`/`reactionBackground` were one
literal. The ladder has a hard ceiling written into AppTheme: dark
identity inks must clear 4.5:1 on four surfaces, capping them at
luminance 0.0757, which four 1.25 rungs reach exactly. Separately,
contrast is NOT sufficient for identity colours — nine sender inks were
really seven (closest pair dE 5.6/7.4) while every one passed AA, because
legibility was never the failing property. And an ink used as the base of
its own 14% chip fill must be checked against THAT, not against the
surface.

*Six of seven test failures were bad tests, not bad code* — a ban regex
matching a token named in a COMMENT, an icon regex matching
`State { name: }`, three fixed-window source scans defeated by added
comments, a click helper that never scrolled (Qt DROPS a press outside the
window), and a reflow guard measuring scene coordinates so a scroll read
as a reflow. Ask what an assertion meant to measure before deciding who is
wrong; repoint it with teeth rather than deleting it. `qmlformat` over
`qml/*.qml` is a seconds-long parse gate worth running before any build.

**2026-08-19 design-deficit pass.** The reader popover's click was DEAD:
delegates reach the pane only through their `timelineView` (the rotated
Flickable), and `openReceiptList` was a pane-root function, so the
delegate's existence guard silently swallowed every click — it is now a
property-function ON the Flickable (the `openSenderProfile` pattern).
~18 consistency fixes from a three-lens design audit. CTest 134/134 both
trees. **NOT TESTED** live.

**2026-08-19 Element-parity round.** `mx_rust_set_space_child_suggested`
reads the CURRENT `m.space.child`, preserves via/order, flips only
`suggested`, and REFUSES a non-child (empty-via included) — it never
promotes one; nothing applies optimistically. "Suggested" is shown only
when the hierarchy KNOWS, never fabricated. Rail: a SINGLE tap on a real
space opens Space Home (which REPLACES the chat view), there is
deliberately NO double-tap, and the ONLY expansion trigger is the
chevron disc, whose band is excluded from the tile's tap.
`openSpaceHome` was reordered teardown-first, activation-last because
the Space Home loader instantiates SYNCHRONOUSLY and its handlers point
RoomInfoController at the space — the old order wiped the
canInvite/canManageSpaceChildren gates afterwards, rendering the
controls permission-less. A `%n` source string renders its "(s)"
literally without a loaded translation, so "Seen by N people" is
branched explicitly; per-reader time comes from the receipt's own
`tsMs`, and `tsMs` 0 renders nothing, never a fabricated time. Suites:
`space-child-suggest` (4), `element-parity-contract` (5). **NOT
TESTED**.

**2026-08-18 post-0.7.3 round.** Both stale timeline suites GREEN for
the first time since `1e50f6a` (`timeline-hydration-qml` 8/0,
`timeline-pane-qml` 63/0; offscreen, one desktop, one day).
`initialHydrationGateHoldsThenOpensAtLatest` exposed a REAL production
defect: after a reset, `fillsViewport` trusted a `contentHeight` still
reading the OUTGOING content's height (old delegates linger until
deferred destruction), and `contentHeight >= height-1` is degenerately
true while `height==0` pre-layout — fixed with
`presentationGeometryStale` (set on model reset, cleared by the first
Column relayout) plus a `height>0` guard. **GUI stall tracing**
(`LIGHTNING_GUI_STALL_TRACE`, `src/app/GuiStallTracer`; default 250 ms,
env value >= 50 overrides): one line per stall with a coarse RAII-scope
category — literal strings only, never content. `stalltrace::Scope`
writes a single GLOBAL category, so it is inert off the GUI thread: a
confidently wrong category is worse than `unattributed`.

**2026-08-18 voice calls (rounds 1-3).** MSC2746 `m.call.*` v1 plus the
`m.rtc.notification`/`m.rtc.decline` lane (`rust/src/calls.rs`,
`mx_rust_calls_*`, SDP-free `CallSignal`, `src/calls/CallController`)
and `GstCallMediaBackend` (GStreamer webrtcbin, ICE/libnice, DTLS-SRTP,
Opus, audio-only) behind a seam — optional at build time
(`LIGHTNING_ENABLE_WEBRTC`), re-probed at runtime,
`LIGHTNING_DISABLE_WEBRTC=1` kill switch; without it the honest
signaling-only refusal stands. Full contract in `docs/voice-calls.md`.
Constraints that must not soften:
- **Inbound call/party ids are sender-chosen text — bounded in Rust,
  never logged.** Anything remotely triggered (busy auto-reject,
  re-delivery) is BOUNDED and idempotent, ignored senders are dropped
  before any state change or send, and backlog suppression defaults
  CLOSED.
- **SDP transport is OPT-IN end to end**
  (`mx_rust_calls_set_media_capable`), bounded 128 KiB at the Rust edge,
  held in the single-shot memory-only `calls::SdpStore` (cap 8, wiped on
  sign-out/detach/teardown/local reset), never on CallSignal, never
  logged, never in QML.
- **TURN comes from `/voip/turnServer` only**; credentials cross once,
  engine-only, never logged; no third-party STUN.
- `startVoiceCallButton` is contract-enforced 1:1-DM-only (a legacy
  invite rings every room member) and currently `enabled: false`
  ("coming soon", contract-pinned so re-enabling is a decision) because
  no answered call has ever been live-validated.
- Session-identity tokens on every GStreamer callback: a reused engine
  must never attribute a closed call's queued event to the next call.
  Engine registration sits behind main.cpp's explicit
  `enableCallMediaEngine()` so the test fleet never gains an engine it
  did not ask for.
- Pre-answer candidate buffering (callers trickle immediately, humans
  answer slowly — those candidates were being dropped) and RFC 3264
  answer-side Opus pt reuse came from reading GStreamer sources.
Deliberate gaps: packages do not declare the GStreamer/libnice runtime
deps (packaged builds stay signaling-only), no video, no MatrixRTC/group
calls, `m.call.negotiate` unhandled. Suites: `call-controller` (35),
`call-ring-policy` (10), `call-ui-contract` (6), `call-media-loopback`
(a real in-process WebRTC call; SKIPs without plugins), `calls::tests`
(10). **Live network or Element interop of an ANSWERED call: NOT
TESTED** — the loopback suite proves the engine, not the network.

**2026-08-18 tester report #2 round (`1aa89f5`).** Copy image and the
saved-media star both fetch through the MediaBridge with pending-key
discipline and magic sniffing (SVG refused); **a keyed dedup must
service ALL claimants** — a star and a copy racing on the same uncached
image left the star stranded forever. Reply-to-image thumbnails register
the embedded reply event's media in the Rust media registry under the
reply target's event id (the row mechanism, so encrypted rooms work
identically); `reply_to_media_key` crosses, never media bytes. The
read-by popover shows the delivered 16 newest plus a truthful "…and N
more (names not loaded)" — the bridge caps at 16 by design and names are
never fabricated. Space Home's `spaceJoined` drill-in was an UNFILTERED
global listener. Deferred with reasons in
`docs/tester-report-2026-08-18-2.md`: spellcheck (the MentionHighlighter
`QSyntaxHighlighter` hook is the proven attach point), rail
reorder/folders, an update dev-channel (needs a second signed manifest
slot in lightning-deploy first), Win11 emoji tofu (bundling Noto Color
Emoji is a size decision for Rokas). **NOT TESTED** live on Windows.

**2026-08-17/18 post-0.7.2 round (`4f74eb4..8da2e81`, shipped as
0.7.3).** The first REAL upgrade test drove all of it. Windows MSI
failed with 1619 because msiexec has its own argument parser and rejects
Qt's forward-slash path (proven by hand: `/` errored, `\` installed).
Windows portable failed because the swap renamed the install DIRECTORY
while the running helper and its mapped Qt DLLs lived inside it — now an
entry-by-entry move, since renaming in-use FILES is permitted on Windows
while deleting them is not (so the backup directory survives, and a
stale one must be cleared or update #2 fails). AppImage relaunched the
MOUNTED binary rather than the .AppImage it had replaced. Opening a room
notified for its own backlog: opening a room subscribes it in sliding
sync and the backlog arrives as live appends while `roomVisibleAtLatest`
is false. The app icon was passed only as a theme name, which resolves
in an installed deb/rpm and nowhere else. Update discovery survives the
release server being unreachable via a canonical-first, mirror-fallback
manifest fetch. First-upgrade procedure: `docs/updates.md` (`082d4d0`).

**2026-08-16/17 post-0.7.1 round (`ea1fd40..7c736c3`, shipped as
0.7.2).**
- **Video poster extraction froze the GUI thread (`68cb82c`).** The
  **first `QVideoSink` in a process costs ~931 ms** (lazy Qt Multimedia
  init incl. a hardware-decoder probe that fails without VAAPI); on a
  worker thread the same extraction is 1 ms. The per-frame theory was
  WRONG: `toImage()` is 0.24 ms. Two traps: a plain `moveToThread`
  leaves a MEMBER `QTimer` on the creating thread where Qt refuses to
  start it, silently disarming the 6 s watchdog (make it a CHILD); and
  the reply becomes QUEUED, so `disconnect()` no longer reliably cancels
  one already posted — session isolation keys on `m_posterExtracting`,
  not on the connection. `MediaBridge::warmMultimediaBackend()` pre-pays
  the init off-thread for the first inline PLAYBACK, whose sink QML
  builds on the GUI thread and cannot move.
- **Message action bar (`4db1a18`).** The FIRST attempt crashed: a
  per-row Loader's loaded Rectangle setting `parent: Overlay.overlay`
  keeps the Loader as its destruction owner, so delegate churn
  dereferenced a dangling pointer (the `detailsDialogComponent`
  precedent does NOT transfer — a Dialog is a Popup and owns its overlay
  lifetime). Fixed with ONE shared bar declared statically in
  TimelinePane.qml into which rows publish only PRIMITIVES, never a
  QObject reference. `forceReleaseActionBar` exists because the ordinary
  release refuses while the pointer is on the bar: right for a live row,
  wrong for a dying one.
- **Scroll consistency (`5429ab0`):** shared `qml/SmoothWheelArea.qml`
  using only ScrollTuning's STATELESS `notchDistance()` —
  `wheelTargetY()` mutates controller state owned by the timeline's
  anchoring. Its `parent as Flickable` was NULL in nine panes, so the
  shared area was inert there; the contract test LISTS the unconverted
  panes rather than hiding them.
- **"Mark as read" was a silent no-op for any room but the open one
  (`05b2384`)**: `RoomListModel::markRoomRead` walked
  `MatrixClient::timeline(roomId)`, which on the Rust backend only ever
  holds the ACTIVE room. `mx_rust_mark_room_read` takes the target from
  `Room::latest_event()` and sends the public receipt AND `m.fully_read`
  together. Only this entry point was broken.
- **Message forwarding**: re-sent as a NEW, unrelated event (no Matrix
  forward primitive), carrying NO relation, so a forwarded thread reply
  lands as an ordinary message (§8). **Media is RE-UPLOADED, never
  mxc-copied** — under authenticated media the target's members may not
  be entitled to the source mxc, and an encrypted source's `file` block
  carries per-event keys that must not be planted in a room that never
  negotiated them. Filename and MIME are RE-ORIGINATED under this
  account and therefore sanitized: leaf-only filename, type from MAGIC
  BYTES using `rooms::sniff_image_mime`'s five signatures — NOT
  `QImageReader::format()` (plugin-backed; WebP lives in qtimageformats,
  which the packaged fleet need not carry) and NOT
  `gif::validateRasterBytes` (its 4096px / 25 MiB caps would refuse a 5K
  screenshot). Review caught: every image forward would have written
  decrypted bytes into the saved-media store (the star handler acted on
  EVERY `mediaBytesForStar`); forwarding to any room but the OPEN one
  failed 100% of the time (`sendAttachmentBytes` gates on the live
  timeline, so `Room::send_attachment` was added); a server refusal
  after dispatch was SILENT.
- **Space avatar (`74319b1`)**: a Space IS a Matrix room, so the same
  permission-gated `setRoomAvatar`/`removeRoomAvatar` applies.
- **State-flood scroll death: still NOT reproduced** (`970bc75`,
  `ff5dcfe`). The proxy-suppression fix sketched in `970bc75`'s message
  was deliberately NOT shipped — it would be a fourth speculative scroll
  change. A real capture is the blocker: a high `worstNotchMs` next to a
  high `stateRows`. Confirmed inefficiency: a collapsed state group
  drawing ONE summary line still instantiates one delegate per member
  row.

**2026-08-15 discovery / search / UIA / moderation / drafts round.**
- **Active-room sliding-sync subscription** (user-report fix): sliding
  sync delivers `m.room.pinned_events` ONLY inside a room
  SUBSCRIPTION's required state and Lightning never subscribed, so a pin
  stayed invisible until a restart re-ran the once-per-room `/state`
  probe. `mx_rust_timeline_open` records the open room as THE one
  subscription (`RoomListService::subscribe_to_rooms`, replacing the
  previous set); `stop_sync_and_wait` forgets it so a later account
  cannot inherit it.
- **Discover / Join** (`discover.rs`, `RoomDiscoveryController`): a
  refused `get_room_preview` still resolves — Join stays offered; knock
  withdrawal is a Knocked-state `Room::leave` because the normal leave
  path filters to Joined; the `/hierarchy`-backed `SpaceRoomList` is
  bounded to 10 pages / 200 rows; `restricted_denied` is classified
  separately, never presented as plain invite-only.
- **Message history search** (`search.rs`, `MessageSearchController`):
  the server `/search` endpoint via raw `Client::send` (matrix-sdk has
  no wrapper). **E2EE POLICY, deliberate: the server cannot search
  ciphertext, so server search covers UNENCRYPTED rooms only and every
  surface says so**; in an encrypted room the loaded-timeline find is
  the only search and the find bar offers no History segment. The only
  content sent is the typed search term.
- **Reusable UIA** (`uia.rs`, `UiaController`): the privileged call runs
  WITHOUT auth first; a real 401 (`as_uiaa_response`) parks it in the
  bridge's single UIA slot and surfaces sanitized stage NAMES only.
  Password transit buffers are scrubbed BEST-EFFORT (QML field wiped on
  dispatch, C++ QByteArray + Rust String zeroed with volatile writes).
  **Honesty: on the success path the String moves into ruma's
  `uiaa::Password`, which serializes and drops it without zeroing —
  transit hygiene, never a guarantee.** The current device is guarded
  out of per-device sign-out and tiles only disappear on the
  authoritative refetch. **OAuth/MAS accounts have NO password stage** —
  their buttons open `account_management_url_with_action` in the
  browser, never a fake password prompt.
- **Ignore + report** (`ignore.rs`, `ModerationController`): SDK
  `Account::ignore_user`/`unignore_user` read-modify-write of
  `m.ignored_user_list` — **never a Lightning-local database**. Remote
  and local changes both arrive via the sync loop's
  `subscribe_to_ignore_user_list_changes`. The SDK clears the whole
  event cache on a list change (timelines reset and refetch; expected).
  `NotificationManager` takes `senderIsIgnored` to close the race before
  the server stops sending. Report = `Room::report_content` (requires
  Joined); `report_room` (unstable MSC4151) and `report_user` (absent
  from the SDK) are deliberately NOT offered. The message menu uses
  `TimelineModel::realRoomIdForEvent`, never the thread composite.
- **Drafts** (`DraftStore`): unencrypted rooms persist drafts locally
  (account-scoped QSettings `accounts/<slug>/drafts/<sha16>`, LRU 256,
  wiped with the account group); **ENCRYPTED rooms are memory-only**
  (never restart), and UNKNOWN encryption state fails closed to memory.
  Payload = text + mention refs (restored fail-closed against the text
  slice) + reply target (restored tolerantly); edit state and
  attachments excluded. Saves are 1 s debounced; the debounce is STOPPED
  before every room/thread change and the save reads the still-current
  key, so a stale timer cannot write across rooms.
- **Authenticated media hygiene**: the only reachable legacy surface was
  `RustSdkMatrixClient::mediaDownloadUrl`/`mediaThumbnailUrl` handing
  unauthenticated `/media/v3` links to the browser; both now return
  empty on the Rust backend.
All **NOT TESTED** live.

**2026-08-15 pins / admin / verification-UX round.** Pinned messages,
member power levels, join rule and canonical alias, the bounded
thread-participant fan-out and the focused verification dialog — all in
§7. Suites: `pinned-messages`, `room-power-levels`,
`thread-facepile-bound`. **NOT TESTED** live: real
`m.room.pinned_events` round trips and Element interop, a homeserver
accepting or rejecting a power-level or join-rule write, alias
publication, and the on-screen look of any of it.

**2026-08-15 Matrix presence.** Sliding Sync delivers NO presence events
(MSC4186 has no presence extension), so this is a bounded polling loop,
stateless on the Rust side: `rust/src/presence.rs` answers one
`presence_batch` poll per round (raw ruma `get_presence` via
`Client::send`, ≤40 users, 10 s no-retry timeout so sign-out's task join
cannot stall). ALL policy is in `src/presence/PresenceManager`: only
watch()ed on-screen users are polled (`qml/PresenceDot.qml` owns that
lifecycle), 30 s rounds with rotation past the cap; transient failures
KEEP the last known state, forbidden/not_found erase it; two consecutive
all-forbidden batches of at least two distinct users each latch "server
has presence disabled" for the session (a single user's 403 never
latches). **Unknown renders NOTHING — never a fabricated offline.** Own
presence is gated by the APPLICATION-WIDE `sharePresence` setting
(default ON, global not per-account; disabling publishes ONE final
offline). **NOT TESTED** live.

**2026-08-11 media/UX round.** MediaBridge request priorities (0
explicit playback/save, 1 avatars/thumbnails, 2 full static, 3
speculative GIF prefetch), two slots reserved for interactive classes, a
15 s starvation bound, temp-file pinning while a QMediaPlayer holds the
file, queued-speculative dropping on room switch, byte-sniff rejection
of A/V containers on thumbnail-class results, offscreen player
reclamation (45 s audio, 90 s video). libpipewire was made resolvable in
the dev shell so Qt Multimedia uses native PipeWire instead of the
PulseAudio fallback a captured FLAC crash aborted in. An SDK receipt
move arrives as adjacent Set diffs, so the poll drain must not split the
pair across 100 ms ticks. The custom app icon
(`appicon::normalizeIconBytes`) covers window/task-switcher surfaces
only — launchers keep the packaged hicolor icon. SDK-internal
receipt-loss mechanisms Lightning cannot fix without patching
matrix-sdk-ui 0.18 are in `docs/receipt-semantics.md`. **NOT TESTED**.

### Live validation: what Rokas has actually confirmed

**2026-08-26 (later) — screen share STOP and RESTART, against Element.**
Confirmed working on a real desktop: stopping a share genuinely stops it —
the far end's tile clears instead of freezing on a grey box — and starting a
new share afterwards works, replacing rather than landing beside the old one.
First share near-instant, restart 1-2 s. Microphone loudness (the `webrtcdsp`
AGC) and the per-participant volume curve were confirmed in the same session.

This lane took FOUR rounds and the first three each fixed something real
without fixing the report, which is the lesson worth keeping:

1. `unpublish()` deadlocked the GUI thread against its own streaming thread
   (core dump: `gst_pad_set_active` wanting the stream lock, the encoder
   thread holding it in `do_probe_callbacks`). Fixed with an IDLE pad probe
   plus `gst_element_call_async`.
2. That probe then never fired, because a pad pushing into a webrtcbin that
   is not draining never becomes idle — so the teardown did not deadlock, it
   simply never ran. A leak wearing a fix's clothes. Instrumented: "probe
   installed", silence for three seconds, "probe fired" during teardown.
3. Releasing the request pad dropped the msid and left the section
   `a=sendrecv` — the far end still told it was being sent to, with nothing
   behind it. THAT was the grey box.
4. Setting the transceiver direction to INACTIVE is what the far end obeys.
   Section count must stay stable across renegotiation (an m= section may
   never be removed), so `a=inactive` is the only correct shape, not merely
   the tidy one.

Checked rather than assumed at step 4: livekit-protocol 0.7.12 has NO
unpublish verb for media tracks — SignalRequest carries AddTrack, Mute and an
UnpublishDataTrackRequest for DATA only. Renegotiation is the mechanism, which
is why a MUTE could never do the job: a mute removes nothing, so the stopped
track stayed in the participant list and was rendered forever.

GENERALISE: Lightning's own self-view is tee'd off the CAPTURE, upstream of
encryption and of the SFU entirely. It looking correct says nothing whatever
about what any other client receives, and it looked correct through all four
rounds. When a share is reported broken remotely and fine locally, the
preview is not evidence.

**2026-08-26 — the largest live-validation event this project has had.** Rokas
tested and confirmed WORKING, on a real desktop against real homeservers:

1. **The rail's drag, including drop-to-make-a-folder.** This is the headline:
   the gesture was structurally unreachable through THREE rules and two rounds
   that each believed they had fixed it. It works. Also confirmed: the
   folder-name dialog, the auto-scroll near the rail's ends, and a drop
   BETWEEN tiles reordering rather than grouping.
2. **Space settings and the rail's Space menu — every write.** Name, topic,
   avatar, join rule, canonical alias, the full power-level matrix, Publish to
   Directory, Local Addresses, Mark as read, Mute, Invite, Copy/Share link.
   Every one of those is a real state event and none had ever been sent.
3. **Keyboard shortcuts**, including the design's load-bearing case: Ctrl+B is
   Bold inside the message box and still toggles the room list everywhere
   else, rebinding, and the conflict refusal.
4. **Multi-account against real homeservers** — switching, encrypted-room
   decryption after a switch, notification routing, restart restoration,
   scoped removal. (The switch's 3-5 s FREEZE is a separate open defect below;
   the behaviour is correct, the latency is not.)
5. **Element interoperability** per `docs/element-interop-checklist.md`:
   encrypted both directions, threads, voice messages, video posters,
   reactions, pins, edits, redactions, the key-recovery cycle, and QR and SAS
   verification against Element.
6. **Notifications** — thread replies, server push-rule modes including
   "follow account default", and the retry after reconnect.
7. **The Channels column, the 2026-08-21 UI round across all 11 themes,** and
   the smaller items: hide/show an image without moving the timeline, read
   receipts, presence dots, saving GIFs, the compact link-preview consent box,
   reduced motion, the 24-hour clock, attachment captions.

WHAT THAT CONFIRMATION IS AND IS NOT. It is Rokas exercising each feature and
reporting it works — the only evidence that has ever counted here. It is not a
claim that every FAILURE branch was reached: a write that the server REFUSES,
a power level a homeserver rejects, and a reconnect retry all need a server
that says no, and those paths remain unexercised. Do not re-list the seven
areas above as untested; do not upgrade their failure branches to tested
either.

### Open items and NOT TESTED inventory

OPEN DEFECTS, reported live and not yet confirmed fixed. These are the list.

- **The camera does not work at all**, and the control lags when pressed. It
  survived one fix (the video router's detach-by-key becoming sink ownership,
  `91acd25`), so the route was not the cause or not the only one. Screen share
  works on the same publish path, which makes DIFFING THE TWO BRANCHES the
  highest-value comparison available.
- **An account switch FREEZES the UI for 3-5 seconds**, reproducibly, on the
  SECOND switch (A→B→A) and not the first. Distinct from the unbounded
  profile-fetch loop (`be195f7`) and from the double-polled JoinHandle
  (`e50eff6`); this is a synchronous block, and `shutdown_managed_tasks`
  does `block_on` on the GUI thread with a 15 s budget over a pool that
  includes ~170 avatar fetches.
- **The screen share's startup is still VARIABLE**, though far less so:
  live-confirmed 2026-08-26 as near-instant on the first share and 1-2 s on a
  restart, against the 5-10 s previously reported. The cause is unchanged and
  unfixed — `videorate` emits nothing until a SECOND input buffer arrives and
  a desktop capture delivers ON DAMAGE, so the wait is "how long until
  something on the screen changes". THREE properties have now been shipped
  against it without measurement and all three made it worse: `min-buffers=8`
  and `keepalive-time=100` each killed the capture outright, and `compositor`
  as the rate stage cropped a 3840x2160 desktop to its top-left quarter
  (compositor is NOT a scaler — it paints each input at native size on its
  output canvas). A fourth guess is not acceptable.
  **The open lead, measured but NOT shipped:** putting the SIZE ceiling
  BEFORE the rate stage makes `compositor` usable without the crop —
  `sink 1920 / src 1920` against a 4K input, where caps-after gave
  `sink 3840 / src 1920`. What is still unmeasured is the other half, that it
  keeps the instant first frame, and that must go through the suite's own
  `framesFromASingleCaptureBuffer` harness before anything ships.
- **The incoming-call prompt's Accept does nothing.**
- **Raise hand is invisible to Element** — it is local-only, and either the
  wire representation gets established from element-call or the control goes.
- **Full screen opens on the primary monitor**, not the one the app is on.

STILL UNPROVEN, not reported broken:

- **A fresh `QSG_RENDER_TIMING` capture** proving the row window does anything
  in production (`winApplies` > 0, `rows` ≪ `srcRows`). No production frame-cost
  improvement has ever been observed from it.
- **The still-unreproduced freeze after hammering reactions** — hand over a
  build with `LIGHTNING_GUI_STALL_TRACE` enabled. One capture beats three
  theories.
- **GIF-favorite reopen crash** — still only `1502e6b`'s commit message as
  evidence; seven headless scenario families including an ASan build found
  nothing. Needs a real `coredumpctl`/`gdb` backtrace.
- **The Rust `children` payload against a real homeserver** — that a real
  `m.space.child` order arrives in the order its admin set. Adjacent to item 2
  above but not the same claim.
- **`app.` dereferences in creation-time bindings of other `Repeater`
  delegates** (`qml/EmojiPicker.qml`, `qml/SettingsScreen.qml` theme cards) —
  structurally exposed to the poisoned-context-lookup defect fixed in
  `30ee39b`, not observed failing.
- Continue GIF playback, cancellation, resource, cache and malformed-media
  hardening.

**Accepted follow-ups, none blocking:** decide whether a client-side
sanity ceiling should apply when the server advertises no upload limit
(deliberately absent — see §7); `setVoiceRecorderForTest` would be
better taking a `unique_ptr`; `voice_info` computes `info.size` from the
stat size rather than the uploaded bytes; `FakeRecorder` is a PARTIAL
double (`stop()`/`durationMs()` are not virtual, so anyone needing
`stop() → ready()` must extend the seam consciously); the pre-existing
invite/verification notification bodies carry unescaped member-chosen
text.

"Recovering never-backed-up Megolm keys" is **refused, not deferred**: a
key that was never backed up and never shared exists nowhere, every
legitimate recovery path is already implemented, and anything further
would weaken E2EE.

Do not list the implemented GIF browser, favorites/recents, download and
send path, provider networking, thread summaries/attachments,
notification sounds, or E2EE generation isolation as unfinished. Do not
turn possible future ideas into commitments.

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
