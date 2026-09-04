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

Latest published release: **Lightning 0.8.3** (`v0.8.3` -> `24fbe9c`), tagged
2026-08-30; notes in `docs/releases/v0.8.3.md`. **0.8.4 is IN FLIGHT** as of
2026-09-02 at Rokas's explicit request: the version bump is prepared on `main`
and reads **0.8.4** in `CMakeLists.txt` (both `project()` and
`APP_VERSION_LABEL`), `rust/Cargo.toml`, `rust/Cargo.lock`, and the Rust/HTTP
user agent (derived from `CARGO_PKG_VERSION`). Any bump after it is a new
release checkpoint and only on Rokas's explicit request (§14).

The anonymous verification bar (§14) has NOT yet been run for 0.8.4. It was
run and passed for 0.8.0: the annotated tag peeled to `6f203be` on both
GitLab and GitHub; all 10 package links returned 200 under curl; the `latest`
manifest reported 0.8.0 / `v0.8.0` with `mirror_url` on all six artifacts; its
Ed25519 signature (`lightning-release-2026a`) VERIFIED and two separately
tampered copies were REJECTED; the GitHub mirror carried 10 assets and a deb
fetched from it matched the GitLab-signed SHA-256 exactly. Run the same bar
against every release, 0.8.4 included.

`matrix-sdk`, `matrix-sdk-ui`, and `matrix-sdk-base` resolve to
**0.18.0** in `rust/Cargo.lock`; UI and base are exact-pinned in
`rust/Cargo.toml`. Dependencies are lock-file controlled — never update
them incidentally.

**2026-09-04: `bundled-sqlite` is ON, and it is load-bearing.** The local
message index is SQLite FTS5, and FTS5 is a COMPILE-TIME option that
sqlite.org documents as disabled by default for the canonical source tree. On
the system path whether search works at all would be decided per platform,
thirty minutes into a release pipeline. Bundling makes it a build-time
constant (libsqlite3-sys sets `-DSQLITE_ENABLE_FTS5` explicitly) and raises
the feature floor to 3.50.2. Consequence to remember: the C++ side must NOT
also link `SQLite::SQLite3`, or two SQLite implementations end up in one
process. `rusqlite` and `unicode-normalization` became DIRECT dependencies in
the same round; both were already in the lock file, so the build stays
`--offline --locked`.

### Release inventory (all tags immutable)

| Version | Commit | Deploy pipeline | Notes file |
|---|---|---|---|
| 0.8.4 | IN FLIGHT 2026-09-02 | — | `docs/releases/v0.8.4.md` |
| 0.8.3 | `24fbe9c` | not recorded here | `docs/releases/v0.8.3.md` |
| 0.8.2 | `a8523b7` | not recorded here | `docs/releases/v0.8.2.md` |
| 0.8.1 | `b3d36ec` | not recorded here | `docs/releases/v0.8.1.md` |
| 0.8.0 | `6f203be` | 138, 20/20 green (137 lost `build-deb`) | `docs/releases/v0.8.0.md` |
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
- **`glab` PICKS ITS SERVER FROM THE CURRENT DIRECTORY, and says
  `Unauthenticated.` when it picks wrong.** `glab config get host` is
  **gitlab.com**; this project is on `gitlab.smetonis.net`, and glab only
  reaches it when it can infer the host from the cwd's git remote. Measured:
  the same `glab api projects/7/...` call succeeds from `~/git/lightning` or
  `~/git/lightning-deploy` (only the HOST is inferred — the project id is
  free) and fails from any directory that is not a git repository. So a call
  made after `cd`-ing to a scratchpad or `/tmp` to handle an artifact
  silently changes servers, and the error is a bare `Unauthenticated.` —
  indistinguishable from an expired token. It has cost a session twice: the
  second time a whole round of AppImage work was abandoned and handed off as
  "auth expired, run `glab auth login`" while the token was valid for another
  four months. **Always `export GITLAB_HOST=gitlab.smetonis.net`**, and
  before believing a token is dead, re-run the same call from inside
  `~/git/lightning-deploy`.
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
  `CacheStore` must continue to reject encrypted timeline rows.
  **ONE sanctioned exception, added 2026-09-04 at Rokas's explicit choice: the
  local search index** (`rust/src/localsearch.rs`). Searching encrypted rooms
  is the entire point of it — the server cannot read ciphertext, so a local
  index is the only search those rooms can ever have — and it introduces no
  new class of data to disk: matrix-sdk's own event cache ALREADY persists
  decrypted bodies unencrypted (`encode_event` serializes the `Decrypted`
  variant, `encode_value` is a no-op with no cypher, and Lightning opens
  `sqlite_store(path, None)`). It lives in the account's own store directory,
  is deleted with the account, and drops a redacted message outright. The full
  contract, and why a store passphrase is NOT a free fix — one config covers
  the crypto store too, so it would strand every existing install's account
  pickle — is in `docs/feature-contracts.md` under "Local message search".
  Encrypting the store at rest remains an OPEN decision, not a refused one.
  Do not read this exception as permission for the next cache.
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

**MOVED: the full text is `docs/feature-contracts.md`. READ IT before changing
any feature.** It was moved out on 2026-08-28 because this file had grown past
the 150,000-character limit and was being truncated, silently dropping its own
tail — sections 17 to 19 — from agent context. Nothing was deleted; the whole
section is in that file unchanged.

What it covers, so you know when you need it: authentication and lifecycle
(password, OAuth/OIDC, multi-account); rooms and navigation (both layouts, the
Spaces rail, its drag and folders, Space settings); timeline and media (pins,
receipts, images, video posters, hide-image, link previews); threads; E2EE;
calls, screen sharing and MatrixRTC; notifications; settings, themes and
accessibility; and the GIF provider integration.

Treat everything in it as implemented and as binding. Do not re-list any of it
as unfinished, and do not turn a possible future idea into a commitment.

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
`build-rust/lightning-matrix --backend=rust` inside `nix develop`. The file (and
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
Clean-package validation runs `lightning-matrix --gif-status` (booleans only) and
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
./build-rust/lightning-matrix --version
./build/lightning-matrix --version
```

Real Rust-backed run:

```sh
nix develop -c ./build-rust/lightning-matrix --backend=rust
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

The synchronized CMake, Rust, and user-agent version is authoritative over any
number quoted in this file; read it from `CMakeLists.txt` rather than from
here. Any version bump is a release checkpoint alone and updates those same
synchronized locations. Before release, run complete Rust tests plus Rust and
non-Rust builds/CTest, the `-DLIGHTNING_ENABLE_WEBRTC=OFF` build over EVERY
target (§16 — this is the configuration the Linux package jobs use, and 0.8.0
lost `build-deb` to it twice in one job), and report unavailable live
validation honestly.

**2026-09-02 audit, release checklist** (lightning-deploy
`docs/update-manifest.md`): the signed manifest carries `expires` (`released`
+ 120 days) — INFORMATIONAL only since 2026-09-03 at Rokas's direction, a
client past it keeps updating and merely says so; refresh `latest` without a
release to keep that line quiet (`RELEASE_ACTION=attach-existing`,
`UPDATE_RELEASED_AT=<original>`, `UPDATE_EXPIRES_AT=<now+120d>`,
`UPDATE_REFRESH_LATEST_ONLY=true`); `attach-existing` never moves `latest`
backwards without `UPDATE_ALLOW_LATEST_ROLLBACK=true`; refresh image digests
each round; the pipeline writes the GitHub `update-latest` slot after every
promotion and preflights the mirror token (rotate it when it warns; check the
source mirror with `glab api projects/6/remote_mirrors`). **OPERATOR ACTION PENDING:** scope `UPDATE_SIGNING_KEY_B64` to
environment `signing` and `GITHUB_MIRROR_TOKEN` to `mirror` in project 7,
or the private key keeps reaching all six build jobs (key id and public key
stay unscoped).

There is a WEBSITE, and it is a THIRD repository: `lightning-website`
(Cloudflare Workers, `https://www.lightning-matrix.org`). It is updated AFTER
a release exists, never before, because the Windows and macOS asset filenames
embed the release commit's short sha. Its whole procedure is to edit
`public/releases.json`; the page also prefers the worker's `GET /api/latest`,
which reports what GitHub has actually published, so a live page follows a new
release within a five-minute cache on its own. `index.html` carries baked-in
values for readers without JavaScript and those only change on a rebuild.

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

The latest published release is in §2, which is the ONE place this file
records it; do not add a second claim here. The trigger shape below is what
matters and it has not changed. The reference run is pipeline **111** (0.7.6,
`b13e346`), **20/20 green on the first attempt** in `RELEASE_ACTION=create`
mode — the first fully clean release run since the macOS lane was added, and
the proof that `86ec616` fixed the mirror. Its trigger used the same SIX
variables 110 used
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

**THIS FILE HAS A HARD 150,000-CHARACTER LIMIT AND IT TRUNCATES SILENTLY,
dropping its own TAIL — §§17-19 — from agent context.** It has now hit that
twice. §7 moved to `docs/feature-contracts.md` on 2026-08-28; §16's round
history moved to `docs/round-history.md` on 2026-09-03 at 150,397 characters.
Before adding a block here, run `wc -c CLAUDE.md`; past roughly 140,000 the
answer is a new file under `docs/` and a pointer, never a longer section.

### Standing warnings

**2026-09-02 SECURITY AUDIT — READ `docs/security-audit-2026-09-02.md`
before touching the updater, `src/calls/`, `rust/src/rtc.rs`, `rust/src/sfu.rs`,
the QML plain-text rule, or the pipeline's secrets.** It holds the refuted
hypotheses (a store passphrase; filtering the mock for §8; bounding RTC
expiry against `created_ts`) and the lessons (a comment is not an
`EncryptionInfo` extractor; content timestamps and `membershipID` are
attacker input; a verified path is not verified bytes; `textFormat` on a
`MenuItem` is a load-time error; one working tree, two sessions).

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

*The row window FIRES in production — first evidence, 2026-08-29.* A live
`LIGHTNING_SCROLL_TRACE=1` capture on the maintainer's desktop, scrolling
deep into a real room, produced `winApplies=1 winSkip=380 rows=255
srcRows=635` with `dContentH=-22111`. So the window does bound the
instantiated set on a real account, which had never been observed before —
`rows` really is much less than `srcRows`. What is STILL unproven is the
FRAME COST half: that capture carries `worstNotchMs` (0-1 ms throughout,
which times the handler and not the frame), not `QSG_RENDER_TIMING`, so it
says the mechanism engages and says nothing about what it saves. The judge
for that remains a `QSG_RENDER_TIMING` capture with `winApplies` > 0 and
median frame cost deep in history falling toward 3 ms.

*And the anchor machinery came back CLEAN in that same capture.* Eleven
gestures, up to 385 events each, 635 rows: `displacedApplied=0`,
`prependFirings=0`, `unresolvedId=0` and `evictedNoInsert=0` on every line.
`materializedMaxAbsDelta` was non-zero twice (84 and 221) — but
`activeDeferrals` EQUALS `materializedFirings` on every line and
`activeDeferredSum` equals the delta, so every one of those was deferred
and none reached an active gesture. The single `anchorCorrections=1` sits
on a gesture with `netY=0` and `stick=1`: an idle stuck-to-bottom restore,
which is the designed path. Non-zero counters are not automatically a
failure — read `activeDeferrals` beside them before concluding anything. The window
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

**`pipewiresrc min-buffers` IS PINNED, AND INHERITING ITS DEFAULT IS A BUG.**
`gst-plugin-pipewire`'s `DEFAULT_MIN_BUFFERS` was **8** through 1.4.x and is
**1** from 1.6. The element asks for `SPA_PARAM_Buffers` as
`RANGE(default, min-buffers, max-buffers)`; KWin 6.6 offers `RANGE(3, 2, 4)` —
at most FOUR — and PipeWire 1.6 added an explicit "reject impossible range"
`-EINVAL` when the minimum exceeds the source's maximum, which 1.4.x lacked. So
the BUNDLED 1.4.2 element against a 1.6 daemon asks for >= 8 where <= 4 exist,
and the daemon reports `error alloc buffers: Invalid argument`. A from-source
build on the same machine works, because it loads the HOST's 1.6 plugin —
which is exactly how it hid. Measured on a live 1.6.6 daemon: 8 and 5 fail,
4 and 1 allocate; raising the producer's ceiling to 8 makes 8 pass, pinning it
to the range intersection alone. RULED OUT in the same round, do not
re-propose: the appimage-run bwrap sandbox, the bundled libpipewire version,
and a missing SPA plugin. **GENERALISE: a GStreamer element property whose
DEFAULT changed between the version you develop against and the version you
bundle is invisible until a package meets a host that disagrees — pin it.**

**QML HAS NO `font.families`.** It is a C++ `QFont` API; the QML font value
type exposes `family` alone, so assigning a list is a LOAD-TIME error that
makes the component unavailable and cascades into every parent (it took four
QML suites down at once). `qmlformat` does NOT catch it — it parses syntax and
does not check that a property exists. Express a fallback by resolving ONE
family in C++ against `QFontDatabase::families()`, on a class that already
links **Qt6::Gui** (`AppController`, not `EmojiCatalog`, whose test target
links Qt6::Core alone — the same constraint that keeps `QScreen` out of
`SettingsManager`). Needed because **Qt's automatic per-character fallback is
version-dependent**: measured, Qt 6.8.2 (Debian's, bundled in the AppImage)
drew U+1F600 with colouredPx=0, preferring a MONOCHROME font that claims the
codepoint, where Qt 6.11.1 drew 2580; naming the family gave 4400 on both. That
is why emoji looked right locally and came out monochrome-or-tofu when packaged.

**WHERE A SCREEN SHARE'S CPU ACTUALLY GOES, measured 2026-08-30 — and it is
NOT where two rounds of "GPU scaling" work assumed.** Per 5 s of video, one
core-second is 20% of a core; `videotestsrc` at 4K into the real share stages,
machine otherwise idle:

| Stage (4K desktop shared at 1080p) | CPU / 5 s | Share |
|---|---|---|
| BGRA -> I420 convert, at 4K | 3.32 s | **57%** |
| `videoscale` 4K -> 1080p | 0.12 s | **2%** |
| `vp8enc` at 1080p | 2.35 s | **41%** |

**SCALING IS 2% OF THE COST.** A GPU path justified as "moving the scaling to
the GPU" would be worth almost nothing. What makes `glupload ! glcolorconvert
! glcolorscale ! gldownload` worth having is different and must be described
correctly or the next round will optimise the wrong stage: it moves the COLOUR
CONVERSION onto the GPU and downloads only the REDUCED frame, so the 57% is
what it removes, not the 2%.

Encoder alone, by rung (5 s of video): 1080p30 **2.80 s** (0.56 cores),
1440p30 **5.29 s** (1.06), 4K30 **9.53 s** (1.91), 4K60 **14.35 s** (2.87). A
4K share at 4K therefore costs convert+encode ~12.9 s per 5 s, ~2.6 cores
sustained, which is the measured shape of "4K is laggy".

**None of this explains an OS-WIDE stall on a 20-core machine** — 2.6 cores is
13%. Before blaming the encoder for that, measure the CAPTURE: Windows uses
`gdiscreencapsrc`, a GDI BitBlt that contends with the compositor every frame,
where Discord and OBS use DXGI Desktop Duplication (`d3d11screencapturesrc`,
currently blocked here by the mingw-w64 UCRT/msvcrt break recorded above).
That is a hypothesis, NOT a measurement — the numbers in this block are Linux
`videotestsrc` and say nothing about either capture element.

**THE LINUX PACKAGE JOBS BUILD WITHOUT THE MEDIA ENGINE, and no local tree
does.** Every machine here has GStreamer, so `HAVE_LIGHTNING_WEBRTC` is ON in
`build` and in `build-rust` alike. The deb/rpm/flatpak/appimage jobs build
WITHOUT it on purpose — a distribution with no GStreamer gets the honest
refusal rather than a hard dependency — so anything behind that guard is
compiled in that configuration for the first time thirty minutes into a
release pipeline. 0.8.0 lost `build-deb` to exactly that, twice over in one
job: an `#include` inside the guard whose REGISTRATION was outside it, and an
INLINE accessor in a header calling into a source file the build does not
compile (the `QPointer` lesson below, in a second costume — an inline accessor
in a header creates a link dependency in EVERY target that includes it).

Three minutes locally instead of thirty in CI:

```sh
nix develop -c cmake -S . -B /tmp/build-nowebrtc -G Ninja \
    -DLIGHTNING_ENABLE_WEBRTC=OFF
nix develop -c cmake --build /tmp/build-nowebrtc -j18
```

**Build EVERY target, not just `lightning-matrix`** — a passing app target is how
0.8.0's first attempt got through. Run it before a release and after touching
`src/calls/`. Out-of-tree, `presence-manager` fails because it walks up to
find the repository it scans; that is the harness, not the code.

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

**A plugin an ELEMENT loads for itself is invisible to every check we
have.** Windows shipped for months able to SEND audio and unable to
RECEIVE anything — no `pad-added`, no receive bin, not one
`received track attributed` line in a whole call — because
`libgstsctp.dll` was never staged. LiveKit's SUBSCRIBER offer puts a data
channel in media section 0 and Lightning sets `bundle-policy=max-bundle`,
so EVERY audio and video section is bundled onto that section's
transport; without sctp webrtcbin cannot build it
(`_get_or_create_data_channel_transports: code should not be reached`)
and the media has nowhere to ride. Sending still worked because our own
publisher offer is media-only, so its bundle owner is the audio section —
and that one-directional shape is what made it look like anything but a
missing plugin. macOS stages sctp and was never affected.
**Two reasons nothing caught it.** The Windows Dockerfile copies a
hand-written list of plugins out of the SDK's **267**, and the element
probe covers only elements Lightning NAMES in its own pipelines —
`sctpenc` appears nowhere in our source. It also copied
`libgstsctp-1.0-0.dll`, the SCTP LIBRARY, all along: a DLL of the right
name is not the element, the same distinction that bit the webrtc lane.
**And the SDP is no use for diagnosing it** — the answer is byte-identical
with and without the plugin, which refuted the theory once before a
single-variable control run watching for the WARNING brought it back. The
method that worked: cross-compile a tiny webrtcbin harness with the MinGW
SDK and run it UNDER WINE on the builder image, so the SHIPPED GStreamer
answers the question. Generalize: when a feature is assembled at package
time, the check must ask the shipped artifact whether the feature WORKS —
and the required-element list must include what the elements load, not
only what we call.

**GStreamer version differences, same trap, different library.** The dev
shell is now **1.28.6** (MEASURED 2026-08-31 via `--call-media-status`; it
was 1.26.11 when this was written, and the flake has moved since — check
before relying on the split); packaged Windows is **1.28.5** (upstream
MinGW SDK) and the macOS bundle **1.28.6**. So the dev shell no longer
differs from the packaged fleet the way it did, and a defect that needs
1.28 will now reproduce locally — but do not read that as "the versions
all match": Windows is still a different patch release built by a
different toolchain. Received-track attribution read the
`msid` PROPERTY off a webrtcbin src pad, and how much of it is populated
moved between those releases — so on a packaged build it came back empty,
the fallback took the transceiver **mid as the TRACK KEY**, and the ring
named `"1"` was one nobody had keyed. Reported as Linux→Windows audio
inaudible and a screen share Element could see and Lightning could not.
**RULE: do not depend on a webrtcbin pad or transceiver PROPERTY for
anything load-bearing.** The SDP text is identical everywhere and the
engine already parses it; take the participant AND the track sid from
that one pass, matched on the section's own mid — never a positional
index (LiveKit's subscriber offer carries a data channel in section 0).
Two things this round also proves: the track key is **NOT** the decrypt
key (the cryptor ring is per PARTICIPANT), so a wrong track key explains
missing VIDEO and not silent audio; and the three per-section maps
(`m_streamForMline`, `m_midForMline`, `m_trackForMline`) are ONE record
and must be cleared together, because section mids are small integers
that repeat across calls. `--call-media-status` now names the loaded
version so a tester's output identifies their runtime without a round
trip.

**Three QML/CTest suites are LOAD-SENSITIVE and will flake a full run.**
`timeline-pane-qml`, `timeline-hydration-qml` and `media-bridge` all pass
alone and fail intermittently under `-j14`/`-j18`. Measured 2026-08-27 on a diff that
touched none of them: `timeline-pane-qml` failed a full `-j14` run and failed
once more when re-run alone, then passed three isolated runs in a row; a
`build-rust` run at `-j14` that failed BOTH timeline suites passed each of
them alone and then went 157/157 at `-j8`.
The usual offender is
`topEdgePrependKeepsReaderOnTheSameRowMidGesture`, which is an ANCHOR case —
so before reading a failure as a scroll regression, re-run it alone and at
lower parallelism. §16's scrolling block is explicit that a fourth anchor fix
needs a `LIGHTNING_SCROLL_TRACE=1` capture naming a failure, and a flake is
not that capture.

**A PIPELINE'S CAPS ARE A CONTRACT BOTH ENDS MUST HONOUR, and three ways
this lane has broken it.** All three were live defects, all three were
invisible to every test that existed, and all three are cheap to re-create.

* **A source must produce the size it NEGOTIATED, not the size it measured.**
  `gst_video_frame_map` accepts an OVERSIZED buffer in silence and reads it at
  the caps' stride. Implement `set_caps`, size every buffer from what it
  recorded, and attach a `GstVideoMeta`.
* **`gst_caps_get_structure(caps, 0)` is not "the peer's caps".**
  videoconvertscale puts the DOWNSTREAM-RESTRICTED structure first because
  passthrough is cheaper. An element that fixates structure 0 blind clamps
  itself to a downstream ceiling and defeats the scaler that was there to do
  the work. An element reporting FIXED caps never meets this, which is why two
  sources in the SAME pipeline can disagree.
* **Any caps field a source's `fixate` leaves alone is taken to its MINIMUM by
  `gst_caps_fixate`.** For a `pixel-aspect-ratio` opened by a downstream pin,
  that minimum is 1/2147483647 and videoscale then dies of integer overflow.
  Fixate every field you constrain — and if you pin a field downstream, put a
  FIXED value in front of the source too, because a fixed value is not a range
  and cannot be fixated to a minimum.

**A REFUTATION IS ONLY AS WIDE AS WHAT IT WAS TESTED AGAINST.** `videorate
skip-to-first` sat on this file's do-not-retry list and WAS the fix for the
camera freeze. It had been refuted against the first-buffer hold, on a fresh
pipeline where the call age is ~0 and the property is a no-op by
construction — a true result about a different defect. When re-proposing
something from that list, do not argue it; state which claim was refuted, and
whether yours is the same claim.

**AND A PROBE IS EVIDENCE ONLY IF IT SHARES THE PROPERTY UNDER TEST.**
`videotestsrc` fixates its own PAR, so a probe built on it CANNOT see a PAR
defect in a source that does not — my measurement passed on code that would
have killed every window share larger than the publish ceiling, and my first
attempt to reproduce the review's finding passed too, because it omitted the
element's caps-reorder step. Before trusting a harness, ask what would make it
pass on broken code, and make it fail on purpose first.

**A preflight flag that does not EXIT must be registered TWICE.**
`src/main.cpp` parses its flags in a preflight pass before
`QGuiApplication` exists and again through `QCommandLineParser`. Most
preflight flags exit and never meet the second parser; the ones that let
the app run must be declared in both places or `process()` rejects them
as unknown and quits. `--console` shipped broken this way and reached a
tester as `matrix-client: Unknown option 'console'.` — the one flag whose
whole job is getting a log out of an installed build, and it had never
worked in any build. `--log-file PATH` (added the same day, initially
with the identical defect) mirrors the log to a file on every platform,
because `--console` reopens stdout ONTO the console so a shell redirect
captures nothing, and a macOS bundle from Finder has no terminal at all.
`DesktopIntegrationTest::parseTimeFlagsSurviveIntoTheQtParser` DERIVES
the set from preflight's own source, so a new flag is covered without
editing it.

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

**`QConcatenateTablesProxyModel::roleNames()` DOES NOT FORWARD ITS SOURCES'
ROLE NAMES ON QT 6.8.2**, and does on 6.11.1 — measured in both toolchains
2026-08-28. A `QQuickDelegateModel` silently REFUSES to build a delegate whose
`required property` the model cannot supply BY NAME, while `rowCount` and the
view's `count` stay correct: measured `roleNames PRESENT -> count=3
delegates=3`, `STRIPPED -> count=3 delegates=0`. That was the GIF picker's
blank Saved tab — zero tiles AND no empty-state copy, because the correct
non-zero count suppressed the overlay. Both halves of the screenshot from one
cause. Fix: the proxy answers `GifResultModel().roleNames()` itself.
GENERALISE: a proxy feeding required properties must define `roleNames()`
explicitly. The dev shell's newer Qt hides it; only the packaged build fails —
the same shape as the AppImage's missing image plugins below.

**A TEST FILE CAN BE COMMITTED AND NEVER REGISTERED.**
`tests/ShortcutRegistryTest.cpp` shipped in `cca3011` with the rebindable-
shortcuts feature and was never added to `CMakeLists.txt`; its 19 cases — the
conflict refusal, reserved sequences and global-vs-editor shadowing that
feature depends on — had never been built or run. On their first execution one
failed for real: it bound a GLOBAL action to bare `Escape`, but
`validationError` refuses a modifier-less sequence BEFORE consulting the
reserved table, so it got the modifier error and its `QVERIFY(!error.isEmpty())`
passed while testing nothing. The TEST was wrong. Two rules: the presence of a
test file proves nothing, grep CMakeLists for it; and an assertion that only
checks "an error came back" cannot say WHICH branch produced it — assert on
that branch's own words. Sibling of the moc trap (a case after a trailing
`private:` never registers).

**`expires` IS MEASURED FROM `created_ts`, SO A CONSTANT CANNOT REFRESH IT.**
Every client reads an RTC membership's deadline as `created_ts + expires`,
including `rust/src/rtc.rs`'s own parser. A refresh deliberately PRESERVES
`created_ts` (or the oldest-membership focus selection reorders under
everyone), so re-writing the same constant republishes the SAME ABSOLUTE
INSTANT: the membership dies a fixed period after the JOIN however often it is
refreshed. Reported as a Lightning participant dropping out of a call every
exactly 5 minutes and returning — `MEMBERSHIP_EXPIRY_NO_DELAYED_MS` is 5
minutes, and the 60 s re-publish cadence was running correctly the whole time
while writing a value that could not extend anything. It also explains the
LOPSIDED symptom, which is the part that misleads: peers aged the membership
out and rotated media keys WITHOUT that user, so they could still be HEARD
(their own media kept flowing to an SFU that had never disconnected them) and
could hear nobody. Fixed by `expires_for_refresh()` = `(now - created) +
period`, saturating both ways. NOT live-validated — needs a real call held
past five minutes.

**A RULE ENFORCED ON A FIELD THE ATTACKER CONTROLS IS NOT ENFORCED.** MSC2545
sticker packs live in `im.ponies.room_emotes` — ROOM STATE any member can
write — and the MSC lets an entry omit `mimetype`, which `stickers.rs` allows
on purpose so a pack from a future client stays visible. So the declared type
could not carry §6's "never render untrusted SVG", and `MediaBridge`'s byte
sniff refused A/V containers ONLY: SVG bytes under an absent or lying mimetype
reached the image decode path. Closed at the media CHOKE POINT so every
thumbnail and avatar path inherits it, and deliberately SHAPE-based rather than
a list of spellings — every raster format this client accepts opens with binary
magic, so refusing an image-class payload that begins with `<` (after BOM and
whitespace) or with gzip (SVGZ, which Qt's SVG handler decompresses) has no
false positives and nothing to evade. Listing `<svg`/`<?xml`/`<!DOCTYPE`
instead only tells an attacker what to avoid.

**A QML MUTATION CHECK THAT DOES NOT REBUILD PROVES NOTHING.** A contract
test that READS a `.qml` file sees the source; a test that
`engine.loadFromModule(...)` sees the COMPILED module in the build tree. Mutate
the source, run the test binary directly, and the engine happily loads the
stale good version — so the mutation "passes" and the gate looks vacuous when
it is fine. Measured twice in one session on `CallStage.qml`: without a
rebuild a root-level `font.families` passed; WITH one it failed exactly as
intended (`QQmlApplicationEngine failed to load component`). Rebuild the target
between mutating and running, or you are testing the previous build.

Worth having such a gate at all: every other case over `CallStage.qml` reads it
as TEXT, and a text scan cannot see a load-time error — the failure that took
four QML suites down at once over `font.families`. `qmlformat` cannot see it
either. `theCallStageComponentActuallyLoads` is that gate; note it only covers
the component it loads, since a failure inside a `Loader`'s `sourceComponent`
leaves the ROOT loading fine.

**A WAIT LOOP WHOSE PATTERN MATCHES ITS OWN COMMAND LINE NEVER TERMINATES.**
`while pgrep -f "ninja|ctest"; do sleep; done` matches the bash process running
it, so it waits on itself forever; two background shells deadlocked this way in
one session. Use `pgrep -x ninja`. Same family as the recorded
`$(pgrep -c x || echo 0)` trap.


### Round history (newest first)

**MOVED: the full text is `docs/round-history.md`. READ IT before proposing a
fix in an area it covers.** It was moved out on 2026-09-03 because this file
had reached 150,397 characters against a 150,000 limit and was being
truncated, silently dropping its own tail — sections 17 to 19 — from agent
context. That is the same failure that moved §7 out on 2026-08-28. Nothing was
deleted; the whole block is in that file unchanged.

What it covers, so you know when you need it, by THEME: capture, encoding and
the media pipeline; the voice-call constraints that must not soften; packaging,
platforms and toolchains; QML, layout and bindings; timeline, scrolling and
navigation; models, backends and derived data; Matrix protocol, privacy and
lifecycle decisions; testing and harness discipline; and performance, disk and
logging.

Its refutations are binding: a hypothesis recorded there as refuted must not be
re-proposed without stating which claim was refuted and whether yours is the
same claim.

### Live validation: what Rokas has actually confirmed

**2026-08-30 — THE GPU SCREEN-SHARE SCALE PATH WORKS ON FOUR ENVIRONMENTS,
AND TWO GPU VENDORS.** `screen share scaling on the GPU` confirmed on: NixOS
from source (NVIDIA), a packaged Windows portable build (NVIDIA), the Fedora
RPM (Intel), and the Flatpak (Intel) — the last two on a laptop, with share
audio and a real two-participant call carrying a distributed media key. It is
now the DEFAULT everywhere rather than opt-in.

The Windows numbers are the ones that justify it: a game held 225 of 240 fps
while sharing, and the capture fed the encoder 1:1 (1000 delivered, 1000
encrypted) where the CPU path at 60 fps delivered ~500 against ~1500 — i.e.
`videorate` tripling every real frame, so two thirds of the encode and
encryption was the same picture.

`gstreamer initialised bundled= false` is CORRECT for the RPM and the Flatpak
(system and runtime GStreamer respectively); only the AppImage bundles.

**AND THE APPIMAGE SHIPPED WITHOUT THE PLUGIN, which nothing could have
caught.** The 0.8.2 AppImage logged `screen share falling back to the CPU:
GStreamer element "glupload" is not available in this build` — `libgstopengl`
was simply not in `GST_REQUIRED_PLUGINS`. The engine probes for the element
and degrades, so a missing plugin can never fail a build or a call: GRACEFUL
FALLBACK AND SILENT ABSENCE ARE THE SAME OBSERVABLE unless something asserts
the payload. Third time this shape has bitten — sctp on Windows, ximagesrc,
now opengl — and `validate-appimage.sh` now names it, as it already named
those two. The same gap existed on Windows in a SECOND list:
packaging/windows/Dockerfile stages into the builder SYSROOT,
stage-windows-runtime.py stages into the shipped ZIP, and updating one is not
updating the other.

NOT COVERED: whether the GPU path survives on a machine with no usable GL —
the new `gpuShareChainUsable()` pre-flight is written for that case and has
never been observed declining. macOS is untested entirely.

**2026-08-29 — SCREEN SHARE AUDIO REACHES ELEMENT, on Linux.** Confirmed on
a real desktop into an ENCRYPTED room: Element hears what the sharing
computer is playing. First time share audio has ever left this client.

The log carries the whole path — `share audio published`, then
`negotiation needed: offering 2 track(s)` and an answer at **3 sections**
(the SFU accepting the added track), then TWO independent
`frames encrypted video=false` counters running side by side, which is the
microphone and the share audio as two separately encrypted Opus tracks.

NOT covered by that confirmation: Windows (the capture element differs and
has only been verified to EXIST and to carry a `loopback` Boolean, by
running the shipped SDK's own gst-inspect under Wine — Wine answers
metadata questions, not whether WASAPI captures); the RECEIVE direction,
i.e. whether Lightning plays share audio someone else sends; and whether a
receiver handles two audio tracks from one participant.

Seen in the same capture and NOT diagnosed: `frames dropped: no key in
video=false` climbing on the receive side, with `sfu joined others=2`
against `media key distributed targets=1`. The leading explanation is a
GHOST MEMBERSHIP from the evening's repeated Ctrl+C exits — the log says
`no MSC4140 delayed retraction armed — an unclean exit will leave this
membership until it expires` — which is the mechanism recorded under
"media key targets=0". Not established, and not attributable to the share
audio change either way.

**2026-08-27 — THE WINDOWS CAMERA AND THE WINDOWS SCREEN SHARE BOTH WORK.**
Confirmed on a real packaged Windows build (project 7 pipeline 135, a
NON-PUBLISHING snapshot from `9f829a3`): the camera sends live video instead
of one frozen frame, and screen sharing works. Earlier the same day, from the
same tester on the pipeline 134 artifact: selecting a MONITOR works with two
monitors attached, and a window share of File Explorer is correct.

CONFIRMED LATER THE SAME DAY, on the pipeline 136 build: the camera works,
and a WINDOW share is correct — right aspect ratio, and resizing the window
mid-share does not break it. So the three headline defects of this lane are
all closed on Windows.

WHAT IT STILL DOES NOT COVER: closing a shared window while sharing, the
picker's grid rework, the call-UI layout fixes, and anything at all on macOS.
Do not promote those to tested.

STILL WRONG at the time of that confirmation, reported with a screenshot and
fixed afterwards in `008ccfd` (so ITSELF not yet re-validated): with a share
running, dragging the call panel small collapsed the picture to a sliver and
the spotlight's overlay controls drew across its top edge in half.

**2026-08-26 (later still) — RAISED HANDS INTEROPERATE WITH ELEMENT.**
Confirmed working on a real desktop: a hand raised in Lightning is seen in
Element and vice versa. The wire format was read out of element-call's own
source rather than guessed at (§16), and it was right first time — which is
the whole argument for reading the reference implementation.

Two things from the same session log, neither of them the feature under test:

* **The screen-share startup hold is 77 ms**, measured rather than reported:
  `publish first encoded frame screenShare=true afterPublishMs=135
  firstCaptureMs=58 rateStageHoldMs=77`. The open defect below describes
  "5-10 s previously, 1-2 s on a restart" — this is the first NUMBER anyone
  has had for it, and it says the `videorate` hold is no longer the cost on
  this machine. It is ONE capture on ONE desktop and the cause is unchanged;
  it is evidence about that share, not a fix.
* **Switching accounts mid-call stranded the membership** (`retraction could
  not be dispatched`), because the teardown ran after the Rust client was
  released. Fixed the same day; live re-validation NOT TESTED.

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

- ~~**The camera does not work at all**~~ — **FIXED and LIVE-CONFIRMED on
  Windows 2026-08-27** (`31e6048`), as is the window share's aspect ratio and
  resizing a shared window mid-share. It was never the camera, and "screen share
  works on the same publish path" was the clue rather than the puzzle: the
  window share worked because Lightning's OWN capture element stamps PTS from
  ZERO, and every other source stamps the pipeline's RUNNING TIME.
  `videorate` starts its output clock at SEGMENT START, so a camera switched
  on three minutes into a call handed it a first PTS of three minutes and it
  owed thirty duplicate frames for every second of that — emitted as fast as
  the encoder would take them. ONE picture at full rate, every counter
  healthy. See `videorate skip-to-first` below and §16's rate-stage note.
  **The 10 fps ceiling is a SEPARATE and still-open item**: `ksvideosrc`
  negotiated YUY2 1280x720@10 because `libgstjpeg.dll` is not staged, so an
  MJPG mode cannot negotiate and raw YUY2 at 720p saturates USB 2.0. Fixing
  the freeze did not raise the rate. The control lag is also unclosed
  (`firstCaptureMs` 794-811 ms, the KS device open running on the GUI thread
  inside `gst_element_sync_state_with_parent`); it needs a
  `LIGHTNING_GUI_STALL_TRACE` capture, not a theory.
- **An account switch FREEZES the UI for 3-5 seconds**, reproducibly, on the
  SECOND switch (A→B→A) and not the first. Distinct from the unbounded
  profile-fetch loop (`be195f7`) and from the double-polled JoinHandle
  (`e50eff6`); this is a synchronous block, and `shutdown_managed_tasks`
  does `block_on` on the GUI thread with a 15 s budget over a pool that
  includes ~170 avatar fetches.
  **A 2026-08-26 log makes the leading suspect look WRONG.** The same A→B→A
  round trip reported `teardown_total_ms= 23` on the first switch and
  `teardown_total_ms= 308` on the second — so the second switch IS ~13x
  slower, and it is 0.3 s, not 3-5. Whatever costs seconds is somewhere else,
  and the next round should stop reading `shutdown_managed_tasks` and get a
  `LIGHTNING_GUI_STALL_TRACE` capture across a switch instead. One capture
  beats another theory; that is the standing rule in this file and it applies
  to the theory this file itself wrote down.
  **2026-09-04: THAT CAPTURE EXISTS NOW, AND IT DID NOT REPRODUCE.** A real
  A→B→A round trip on `matrix.smetonis.net`, driven through the GUI with
  `LIGHTNING_GUI_STALL_TRACE=1`:

      switch 1  begin 16:56:20.674 -> login succeeded 16:56:20.917   243 ms
      switch 2  begin 16:57:10.656 -> login succeeded 16:57:10.914   258 ms
      teardown_total_ms = 3 and 1

  NO GUI stall was recorded during either switch. The only stalls in the whole
  session were at startup (271 ms unattributed, 1730 ms rust-poll-drain) and
  one 622 ms rust-poll-drain when the second account was first ADDED — not a
  switch. `shutdown_managed_tasks` is now definitively not the cost: 1-3 ms.
  **This does NOT close the defect.** Both test accounts are small — twelve
  rooms, two members, no avatars — and the recorded suspicion involves ~170
  avatar fetches, so the load is nowhere near the reported case. What is
  established is that the switch MACHINERY is fast and the cost scales with
  something the fixture does not have. The next capture needs a real account
  with real rooms and real avatars; it will not be found on a fixture.
- **The screen share's startup is still VARIABLE**, though far less so, and
  there is now a MEASUREMENT: one live share on 2026-08-26 reported
  `afterPublishMs=135 firstCaptureMs=58 rateStageHoldMs=77` — 77 ms of
  `videorate` hold, not seconds. That is ONE capture on ONE desktop and the
  cause is unchanged, so it bounds the problem rather than closing it; a
  desktop that is genuinely still (no damage) still has nothing to deliver.
  Previously live-confirmed as near-instant on the first share and 1-2 s on a
  restart, against the 5-10 s originally reported. The cause is unchanged and
  unfixed — `videorate` emits nothing until a SECOND input buffer arrives and
  a desktop capture delivers ON DAMAGE, so the wait is "how long until
  something on the screen changes". THREE properties have now been shipped
  against it without measurement and all three made it worse: `min-buffers=8`
  and `keepalive-time=100` each killed the capture outright (and `min-buffers=8`
  is now UNDERSTOOD — see the pipewiresrc note in this section: 8 exceeds the
  buffer ceiling a compositor offers, so on a PipeWire >= 1.6 daemon it cannot
  negotiate at all; `min-buffers=1` is required and shipped), and `compositor`
  as the rate stage cropped a 3840x2160 desktop to its top-left quarter
  (compositor is NOT a scaler — it paints each input at native size on its
  output canvas). A fourth guess is not acceptable.
  **The open lead, measured but NOT shipped:** putting the SIZE ceiling
  BEFORE the rate stage makes `compositor` usable without the crop —
  `sink 1920 / src 1920` against a 4K input, where caps-after gave
  `sink 3840 / src 1920`. What is still unmeasured is the other half, that it
  keeps the instant first frame, and that must go through the suite's own
  `framesFromASingleCaptureBuffer` harness before anything ships.
- ~~**The incoming-call prompt's Accept does nothing.**~~ — **FIXED and
  SHIPPED in 0.8.3** (`87aafd9`, `6e9bd9a`). The card had ONE Accept button
  serving two unrelated lanes: the legacy 1:1 `m.call.*` lane, answered by
  `app.calls.answer()`, and a MatrixRTC ring, which announces a SESSION and is
  answered by JOINING it (`app.groupCall.join()`, gated on
  `app.rtc.joinBlockReason()`). A MatrixRTC ring therefore showed an Accept
  that `CallController::answer()` refused at its own front door. The prompt now
  offers each lane its own control, and `qml/IncomingCallPrompt.qml` carries
  the whole reasoning at the top of the file. Live re-validation of an ANSWERED
  call is still **NOT TESTED** — the button reaches the right code now, which
  is not the same claim.
- **Windows camera runs at 10 fps**, and the fix for the freeze did not touch
  it. `ksvideohelpers.c` exposes `image/jpeg` for MJPG media types, the
  publish bin links `capsrc ! queue ! videoconvert` with no decoder, and
  `libgstjpeg.dll` is absent from the staged plugin list — so an MJPG mode
  cannot negotiate and the camera falls back to raw YUY2, which at 1280x720
  is 18.4 MB/s and hits a USB 2.0 ceiling at 10 fps. Staging the jpeg plugin
  and adding a decoder to the camera branch is the lead; it is a packaging
  change, so it must go through the shipped-artifact check (§16).
  **PACKAGING HALF DONE 2026-09-02** (lightning-deploy): `libgstjpeg.dll` is
  now staged in BOTH Windows lists — the builder sysroot in
  `packaging/windows/Dockerfile` and the shipped zip in
  `stage-windows-runtime.py` — and `jpegdec:libgstjpeg` joins the Wine element
  probe. Note `libjpeg-8.dll` had been staged all along as a LIBRARY
  dependency of Qt and libgstopengl: a DLL of the right name is not the
  element, the same distinction that shipped Windows for months with
  `libgstsctp-1.0-0` present and `sctpenc` missing.
  **THE APP HALF IS STILL OPEN, and the exact blocking line is now known.**
  It is not `videoconvert`: `captureEntryFilter(false)`
  (`src/calls/SfuMediaEngine.cpp`) is
  `capsfilter caps="video/x-raw,pixel-aspect-ratio=(fraction)1/1"` and it sits
  DIRECTLY after `%1 name=capsrc`, so `image/jpeg` cannot satisfy the very
  first element downstream of the source and no MJPG mode can ever negotiate.
  **`decodebin` there is REFUTED, with evidence — do not re-propose it.**
  Inserting `decodebin ! ` in front of that capsfilter builds, but the bin
  logs `element="decodebin0" ... "Delayed linking failed."` and then
  `element="capsrc" ... "Internal data stream error."`, and
  `aBusErrorFromALiveOrUnknownBinIsNotAPublishFailure` fails because the
  capture is retired. It is NOT a latency cost: measured one-buffer wall time
  is 497 ms without the decoder, 478 ms through `decodebin`, 486 ms through
  `jpegenc ! decodebin`. A bare `gst-launch` probe of the same three chains
  PASSES, because it negotiates a different format than the engine does
  (the engine's capture came up `A444_16LE`) — the recorded "a probe is
  evidence only if it shares the property under test" trap, third occurrence.
  What is left to try, in order: build the camera chain for `image/jpeg`
  explicitly and FALL BACK to today's raw chain when it will not build, which
  is the idiom the GPU share path already uses and logs; or resolve the
  device's caps first and choose. Either needs a real webcam, so it is not
  landing from this machine.
- ~~**Raise hand is invisible to Element**~~ — **FIXED and LIVE-CONFIRMED
  2026-08-26** in both directions. The wire representation was established by
  READING element-call's own source rather than guessing: an `m.reaction`
  annotating the raiser's OWN `m.call.member` state event with
  `\u{1F590}\u{FE0F}`, lowered by redacting it. Three lanes (our send, two
  sync handlers, one bounded join-time sweep for hands raised before we
  arrived).
- **Full screen opens on the primary monitor**, not the one the app is on.

**NOT TESTED, 2026-09-02 audit:** six items in
`docs/security-audit-2026-09-02.md`.

**2026-09-04, local search and widgets.** Both are LIVE-VALIDATED against
`matrix.smetonis.net` with throwaway fixture accounts (credentials in the
vault, `Lightning/Testing/Test Accounts.md`, never in this repository):
local search finds a Megolm-encrypted message this client sent, and the widget
list returns one openable widget plus three refused with the right reason and
excludes the tombstone.

**The widget QML is now LIVE-VALIDATED TOO, on a packaged AppImage** (later the
same day, 0.8.4+git 6989623, driven through the GUI against
`matrix.smetonis.net`). Four real `im.vector.modular.widgets` state events were
written to a fixture room: the https one lists with an ENABLED Open, the
`http://` and `javascript:` ones list with a DISABLED Open and the not-HTTPS
refusal, and the one with no `url` is dropped from the list entirely. The
consent sheet has been seen: it names the widget, its URL and who added it,
says the site receives the user's IP, and states that Lightning opens widgets
in the browser so the page cannot reach the account, keys or messages. One
inaccuracy worth fixing eventually and not a defect: a `javascript:` URL is
refused with the not-HTTPS wording rather than a scheme-specific one.

STILL NOT SEEN: the find bar's source strip and coverage line. The room-header
magnifier opens the SERVER-side search panel (it says so, and correctly finds
nothing in an encrypted room); whatever surface exposes the local index was not
reached from the GUI this round, so local search remains validated at the Rust
layer only.

Keyboard automation now works — a `ydotool` uinput device plus KWin scripting
for closed-loop pointer positioning, with a focus guard that refuses to type
unless KWin reports the intended window active. The guard exists because
without it a login went into a browser window instead of the client.

Two open decisions those rounds created, both recorded rather than taken:
- **The SDK store is not encrypted at rest**, and it holds DECRYPTED
  encrypted-room bodies (`encode_event` serializes the `Decrypted` variant,
  `encode_value` is a no-op with no cypher, Lightning opens
  `sqlite_store(path, None)`). §6's memory-only rule binds Lightning's own
  `CacheStore` and is intact; the property it was protecting is not held by
  the installation. The search index is a second copy of what is already
  there. The audit's route to fixing it is in
  `docs/security-audit-2026-09-02.md`; a naive passphrase bricks every install.
- **Widgets are LISTED and opened externally, never embedded**
  (`docs/widgets.md`). Revisiting that needs a Windows Qt story with WebEngine,
  a Flatpak answer that is not "disable the sandbox", and a decision about
  `QtWebEngineQuick::initialize()` forcing the whole scenegraph to OpenGL.

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
