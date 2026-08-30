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

Latest published release: **Lightning 0.8.0** (`v0.8.0` -> `6f203be`), cut by
project 7 pipeline **138** on 2026-08-27; notes in
`docs/releases/v0.8.0.md`. Pipeline **137** was the first attempt and lost
`build-deb` to two symbols used outside their WebRTC guard — no tag was
created, which is the package-first design working. The
application version reads **0.8.0** in `CMakeLists.txt` (both `project()`
and `APP_VERSION_LABEL`), `rust/Cargo.toml`, `rust/Cargo.lock`, and the
Rust/HTTP user agent (derived from `CARGO_PKG_VERSION`). The next bump is a
new release checkpoint and only on Rokas's explicit request (§14).

VERIFIED ANONYMOUSLY, not from job status: the annotated tag peels to
`6f203be` on both GitLab and GitHub; all 10 package links return 200 under
curl; the `latest` manifest reports 0.8.0 / `v0.8.0` with `mirror_url` on all
six artifacts; its Ed25519 signature (`lightning-release-2026a`) VERIFIES and
two separately tampered copies are REJECTED; the GitHub mirror carries 10
assets and a deb fetched from it matches the GitLab-signed SHA-256 exactly.

`matrix-sdk`, `matrix-sdk-ui`, and `matrix-sdk-base` resolve to
**0.18.0** in `rust/Cargo.lock`; UI and base are exact-pinned in
`rust/Cargo.toml`. Dependencies are lock-file controlled — never update
them incidentally.

### Release inventory (all tags immutable)

| Version | Commit | Deploy pipeline | Notes file |
|---|---|---|---|
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

**Build EVERY target, not just `matrix-client`** — a passing app target is how
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
shell is **1.26.11**; packaged Windows is **1.28.5** (upstream MinGW SDK)
and the macOS bundle **1.28.6**. Received-track attribution read the
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

By THEME, not chronology, and reduced to rules, refutations, deliberate
decisions, measured numbers and live status. Features are §7; the caps
contract, the refutation rule and the probe rule are in the standing warnings.

#### Capture, encoding and the media pipeline

- **Caps evidence.** `WindowCaptureSrc` fixated 1920x1080 against a 3840x2100
  window: half-row shear, top quarter only — under 1920 wide the stride matched
  and only bottom rows were lost. `gdiscreencapsrc` reports FIXED caps, so the
  restricted structure drops out: that is why a MONITOR share got 3840x2160 and
  a WINDOW share did not. With PAR opened downstream and the source's fixate
  silent, 3840x2100 and 3840x2160 ERROR while 1557x1213 passes carrying
  1/2147483647 — every window over the ceiling would have published NOTHING.
  The element fixates 1/1 itself AND a FIXED 1/1 capsfilter sits before the
  source, because `avfvideosrc` declares no PAR and `pipewiresrc` is untestable
  outside a portal.
- **PARs VP8 discards** — videoscale's "keeps the aspect ratio" is false; a
  libwebrtc receiver draws the literal size. 3840x2100 -> 36/35; 1920x1200 ->
  9/10 (11% stretch); 3440x1440 -> 43/32 (34% squash); `ximagesrc` -> 2/1.
  Unreported for months because the maintainer's monitors are 16:9.
- **`videorate` clocks from SEGMENT START, not the first buffer's PTS**, and
  capture sources stamp pipeline RUNNING TIME, so a source started mid-call
  back-fills 30 duplicates per second of CALL AGE — one picture at full rate,
  counters healthy. That was the "camera does not work". First PTS 0/10/174 s
  -> 27/327/5247 out; `skip-to-first=true` -> 27 always. Lightning's own
  element stamps from ZERO and must KEEP that.
- **`videorate` also HOLDS the first buffer until a second arrives**, and a
  PipeWire capture delivers ON DAMAGE, so the wait is "until the screen moves".
  REFUTED here: `skip-to-first` does nothing; `max-duplication-time` keeps the
  hold AND starves the encoder below 30 fps. `keepalive-time=100` re-pushes the
  held buffer, is NOT `min-buffers` renamed, and 100 ms is DIAGNOSTIC (a dead
  capture reports ~10/s vs a live one's up-to-30/s). **UNRECONCILED: open items
  record `keepalive-time=100` as having KILLED the capture.**
- **`min-buffers=8` was an unmeasured guess that made things worse and was
  banned; ban RE-SCOPED 2026-08-28** — it could not negotiate against a
  compositor offering at most 4 buffers on PipeWire >= 1.6, so "no frame
  arrived" was an allocation failure, not proof the property is forbidden.
  `min-buffers=1` is REQUIRED.
- **A counter downstream of `videorate` cannot prove the capture is alive** —
  it repeats the last picture, so a dead capture still encodes, encrypts and
  sends at full rate. Count the capture's own buffers, before it.
- **A desktop capture is VARIABLE RATE** — PipeWire negotiates `framerate=0/1`
  at native size (3840x2160 BGRA), and a range including `0/1` leaves `vp8enc`
  no rate to plan against. Pin `30/1`; sizes stay ranges, being ceilings.
- **`rtpvp8pay` parses the VP8 bitstream and cannot payload an encrypted
  frame** — libwebrtc takes the descriptor from the encoder as METADATA, hence
  `RtpVp8Payloader`, which reads nothing.
- **element-call mints a 16-byte media key, livekit-client 32** — requiring 32
  rejected every Element key for its LENGTH, so an Element peer was inaudible
  while our own media reached them.
- **Identify a received track by TRACK SID from the msid, never a `mid`** — a
  `TrackInfo.mid` belongs to the PUBLISHER's connection.
- **A capture that ends itself must be heard** — closing a shared window
  answers EOS and nothing listened, leaving the track declared and the far end
  frozen. It retires through Stop's path, which sets the transceiver INACTIVE.
- **Verify against something that is not Lightning** — two Lightning clients
  agree on streams a libwebrtc receiver rejects; `livekit-cli` (pion) and an
  independent frame-crypto implementation each refuted a confident wrong
  theory. Full table: `docs/matrixrtc.md`.

#### Voice-call constraints that must not soften

Contract in `docs/voice-calls.md`. Inbound call/party ids are sender-chosen
text: bounded in Rust, never logged; remotely triggered work is BOUNDED and
idempotent; ignored senders drop before any state change or send; backlog
suppression defaults CLOSED. SDP transport is OPT-IN end to end, bounded
128 KiB, in the single-shot memory-only `calls::SdpStore` (cap 8, wiped on
sign-out/detach/teardown/reset), never on CallSignal, never logged, never in
QML. TURN comes from `/voip/turnServer` only — credentials cross once,
engine-only, never logged, no third-party STUN. `startVoiceCallButton` is
contract-enforced 1:1-DM-only (a legacy invite rings every room member) and
`enabled: false`, contract-pinned so re-enabling is a decision, because no
answered call has been live-validated. Session-identity tokens ride every
GStreamer callback so a reused engine cannot attribute a closed call's queued
event to the next, and registration sits behind an explicit
`enableCallMediaEngine()` so the test fleet never gains an engine it did not
ask for. Pre-answer candidate buffering and RFC 3264 answer-side Opus pt reuse
came from reading GStreamer sources; `m.call.negotiate` is deliberately
unhandled. Suites: `call-controller` 35, `call-ring-policy` 10,
`call-ui-contract` 6, `call-media-loopback` (SKIPs without plugins),
`calls::tests` 10 — loopback proves the engine, not the network.

#### Packaging, platforms and toolchains

- **`GST_PLUGIN_PATH` is read DURING `gst_init`, once**, and two backends each
  ran their own `gst_init_check` while only one set the bundled path — so a
  package with 25 correct plugins and zero unresolved symbols refused every
  call, and every check passed it, because they proved the payload's SHAPE and
  none proved the app could FIND it. One entry point now does path-then-init
  and both backends are BANNED from `gst_init`. GENERALISE: a feature assembled
  at package time needs a check that runs the SHIPPED artifact and asks whether
  it works. Windows and macOS had shipped for months with no media engine, and
  the honest refusal kept anyone from suspecting packaging.
- **Fedora's mingw GStreamer is a trap** — `gstreamer1-plugins-bad-free` ships
  `libgstwebrtc-1.0-0.dll` (the LIBRARY), not the `webrtcbin` PLUGIN, and no
  nice/srtp/opus/vpx. A `.pc` file and a DLL of the right name are not the
  element; use the upstream MinGW SDK.
- **Capture elements AND their property names are per-platform** —
  `v4l2src`/`pipewiresrc` Linux-only, Windows `ksvideosrc`/`gdiscreencapsrc`,
  macOS `avfvideosrc` (± `capture-screen=true`); `gdiscreencapsrc` takes
  `monitor`/`cursor` where `d3d11screencapturesrc` takes `monitor-index`/
  `show-cursor`, and `gst_parse_launch` fails outright on an unknown property.
  Read them from the shipped plugin's own help strings.
- **A UCRT/msvcrt CRT split, and the probe that cannot see it** — mingw-w64's
  `wchar.h` makes `mbstate_t` a struct under `_UCRT` and an `int` otherwise, so
  `libgstd3d11`/`libgstmediafoundation` import a `std::codecvt` symbol absent
  from the staged libstdc++ (12018 exports; that symbol 0 times, the msvcrt
  spelling once). Windows fails a missing NORMAL import at LoadLibrary — **but
  Wine loads the module anyway**, so a Wine element probe passes a feature dead
  on its target platform. Only a symbol-level walk over the staged closure sees
  it (ZERO unresolved across the 24 shipped plugins). A CRT CHOICE, not version
  drift: a GStreamer bump will not fix it.
- **macOS codesign refuses a plain directory of dylibs inside the bundle** —
  the working shape is a SYMLINK from `Contents/MacOS/gstreamer-1.0` to
  `../PlugIns/gstreamer-plugins`. `macdeployqt` also rewrites the app's
  GStreamer glib/gobject/intl deps into `Contents/Frameworks` out of HOMEBREW,
  splitting the GObject type system, so validation asserts every GStreamer
  library the executable loads is the staged copy.
- **Compile-checking a `Q_OS_WIN`-only TU**:
  `x86_64-w64-mingw32-g++-posix -fsyntax-only` in `debian:13.6-slim` against
  Linux Qt/GStreamer headers plus a stub for `QtGui/qwindowdefs_win.h`, leaving
  three glib LP64/LLP64 `static_assert`s. **Prove it reached the end of the
  file** with a probe TU plus a deliberate undeclared identifier — "no errors"
  can mean "gave up early". nixpkgs' `pkgsCross.mingwW64` gcc does NOT work: it
  wants `mcfgthread/gthr.h`.
- **Windows update paths (fixed in 0.7.3).** MSI failed with **1619** because
  msiexec has its own argument parser and rejects Qt's forward-slash path (`/`
  errored, `\` installed). The portable swap renamed the install DIRECTORY
  while the running helper and its mapped Qt DLLs lived inside it — now
  entry-by-entry, since renaming in-use FILES is permitted on Windows while
  deleting them is not, so a stale backup directory must be cleared or update
  #2 fails. AppImage relaunched the MOUNTED binary, not the `.AppImage` it
  replaced; the app icon was passed only as a theme NAME, which resolves in an
  installed deb/rpm and nowhere else.

#### QML, layout and bindings

- **An imperative write to a bound property destroys the binding** — five media
  cache handlers assigned `Image.source` directly, so the first image that
  loaded was the last that Image showed. Use a `resolveTick` the binding READS
  and the handler bumps (an unused local does create the dependency in Qt
  6.11); with an intermediate `readonly property` it must live in THAT binding
  or it is a silent no-op; key handlers on the cache key.
- **A Popup does NOT consume a press landing on it** — `blockInput()` is FALSE
  when `popupItem == item`, so `modal: true` blocks OUTSIDE presses only; the
  2026-08-18 emoji fix assumed the opposite and was INERT. Sink with an
  all-buttons `MouseArea` in `background:`, never with `z`.
- **`visible: running` on a shared busy indicator is a permanent latch** —
  hosts use `running: visible`, together they cycle, and `visible` is EFFECTIVE
  visibility, so one created under a hidden ancestor latches off silently. A
  component owns its animation; the HOST owns visibility.
- **A defaulted C++ parameter QML must pass fails silently** — `setMentionStyle`
  gained `linkColor`, nothing passed it, and every URL and non-self mention
  rendered in the accent for a round. Pin the arity in a test.
- **In a Qt Quick Layout a child's size constraint may only read a width the
  layout does not compute** — `Layout.maximumWidth: parent.width * 0.7` under
  its RowLayout, and a segment sized against `bubble.width` which in Bubbles
  mode IS the segments' own implicit width, were the whole binding-loop log.
- **An invisible `MenuSeparator` still reserves its height** — QQuickMenu's
  ListView honours each item's height, and MenuSeparator's comes from
  contentItem plus padding regardless of `visible`.
- **A JS array bound to a ListView is a model RESET on every change**, so a
  reorder cannot animate and the delegate holding a live drag is destroyed by
  any refresh: if rows must MOVE, the model must be able to say so. A model
  early-returning on identical rows also announces nothing when only your
  per-row PRESENTATION FLAGS changed, so whoever clears such a flag announces it.
- **`QObject::findChild` cannot reach a `Repeater`'s delegates** — proven with
  a CONSTANT objectName absent from a full `findChildren` dump, ruling out a
  failed binding. Walk `childItems()`.
- **A change handler can run BEFORE the bindings depending on the same
  property** — `onTabChanged` read a binding on `tab`, got the tab being LEFT,
  and moved the selection into the tab just left.
- **Window geometry must be restored in BINDINGS, not `Component.onCompleted`**
  — Qt shows the window during `componentComplete()`, which runs first, so the
  user watches it jump. Read through a CONSTANT property, because a notifying
  one feeds the save back into the binding that produced it. A size below the
  window's minimum is REFUSED on write, since Qt reports transient 0x0/1x1
  while a window is shown, hidden to tray or restored from minimized; only the
  WINDOWED state is stored, maximized as its own flag; `QWindow::show()` forces
  NORMAL, so restoring from the tray sets `visible = true`. `QScreen` stays OUT
  of `SettingsManager` (~20 test targets link it against `Qt6::Core` alone), so
  the still-on-screen test is a BAND along the top of the frame in
  AppController — a window spanned across two monitors is not refused.
- **A guard suppressing a signal for a whole gesture needs something firing at
  the END of it** — `onWidthChanged: if (!SplitView.view.resizing) save()`
  never fired again, because the RELEASE moves nothing.
- **`Qt.quit()` is a REQUEST** — QGuiApplication closes every top-level window
  first and ignores the quit if one refuses, so close-to-tray's
  `close.accepted = false` ate Ctrl+Q, the only way out of that mode. Still
  `Qt.quit()`, not `Qt.exit()`: teardown and apply-on-quit hang off
  `aboutToQuit`.
- **A per-row Loader's item parented to `Overlay.overlay` keeps the Loader as
  its destruction owner**, so delegate churn dereferenced a dangling pointer;
  the `detailsDialogComponent` precedent does NOT transfer, a Dialog being a
  Popup that owns its overlay lifetime. Fixed with ONE shared action bar into
  which rows publish only PRIMITIVES, never a QObject reference;
  `forceReleaseActionBar` exists because the ordinary release refuses while the
  pointer is on the bar — right for a live row, wrong for a dying one.
- **Delegates reach the timeline pane only through their `timelineView`** (the
  rotated Flickable), so a pane-root `openReceiptList` was silently swallowed
  by the delegate's existence guard. Such entry points must be
  property-functions ON the Flickable.
- **The layout faults were one shape: a fixed band in a viewport that got
  smaller**, biting Windows at 125-150% scaling and not Linux, since every
  number is unchanged and two thirds as many fit. `CallHeaderBar` declared no
  `implicitWidth`, so its control row laid out at width 0; the spotlight
  strip's flat 96 px made it the bigger half of a short stage; the call panel's
  flat 45% floor bought the header, the dock and ten pixels of picture. The
  floor now asks the STAGE for `minimumUsefulHeight`, and overlay controls are
  ABSENT rather than squeezed.
- **The hidden-image contract is GEOMETRY, not visibility** — a text row in
  place of a 360x270 picture jumps every message above it, so the placeholder
  fills the media box and contributes no implicit size. An `Image` whose
  `visible` is false still holds its decoded pixmap (clear the SOURCE), and an
  `AnimatedImage` behind an opaque placeholder keeps decoding for nobody.
- **`QScreen::geometry()` is device-independent** — a 4K display at 125% listed
  as 3072x1728, a LABEL defect and not a share defect. Resolve a display by
  DEVICE NAME: Qt's screen order, `EnumDisplayMonitors`' order and
  `gdiscreencapsrc`'s `monitor` index are three unrelated enumerations. A
  Chromium window's caption is the TAB's title, so the owning application comes
  from the executable's VERSIONINFO.
- **The bundled Material Symbols font is a SUBSET** — an unmapped name renders
  as tofu and regenerating needs the network, so pick from the mapped set;
  `IconChromeTest` catches it. A brand mark in the raw accent reads as a status
  light, so `AppTheme.wordmarkBolt` keeps Storm's yellow and blends toward the
  header's secondary ink elsewhere.
- **Colour: measure before believing the symptom.** "Needs more colour" was
  SEPARATION — Storm is the most saturated shell (Lab chroma 27.1 vs Moss Light
  0.8), but every surface step was below 1.25:1 and four elevation roles were
  one literal. Hard ceiling: dark identity inks must clear 4.5:1 on four
  surfaces, capping them at luminance 0.0757, which four 1.25 rungs reach
  exactly. Contrast is NOT sufficient for identity colours — nine sender inks
  were really seven (closest pair dE 5.6/7.4) with every one passing AA — and
  an ink used as the base of its own 14% chip fill is checked against THAT.

#### Timeline, scrolling and navigation

- **The scroll teleport was FOUR paths and the reported one was not the obvious
  one.** (1) A CONVERGENCE-based landing budget re-armed forever, because
  `count`/`layoutRowsAtLastPass` change constantly during a scroll; now an
  absolute ~2 s ceiling. (2) **The actual "about 10 seconds" is a scroll-anchor
  RESTORE** — up to `kMaxNavigationBatches` (8) REAL network paginations before
  `targetLocated`, and cancelling in the VIEW cannot help because the landing
  does not exist yet when the reader starts scrolling; hence
  `PaginationController::cancelNavigation()` from `noteReaderTookControl()`.
  (3) Middle-click autoscroll left `userScrollActive` FALSE for the whole
  gesture (it writes contentY directly, so `moving` stays false), so
  `maintainViewAnchor()` took its IDLE branch and ABSOLUTELY restored contentY;
  pre-existing since v0.7.4. (4) Keyboard paging retired nothing. GENERALISE: a
  convergence budget needs an absolute ceiling, and the reader taking the view
  must reach EVERY layer that can move it.
- **After a model reset `contentHeight` still reads the OUTGOING content's
  height** (old delegates linger until deferred destruction), and
  `contentHeight >= height-1` is degenerately true while `height == 0`
  pre-layout, so the hydration gate opened early. Fixed with
  `presentationGeometryStale` plus a `height > 0` guard; both stale suites then
  went green (`timeline-hydration-qml` 8/0, `timeline-pane-qml` 63/0).
- **`SmoothWheelArea` may use only ScrollTuning's STATELESS `notchDistance()`**
  — `wheelTargetY()` mutates controller state owned by the timeline's
  anchoring. Its `parent as Flickable` was NULL in nine panes, leaving the
  shared area inert; the contract test LISTS unconverted panes.
- **State-flood scroll death is still NOT reproduced.** The proxy-suppression
  fix sketched in its commit message was deliberately NOT shipped — it would be
  a fourth speculative scroll change. The blocker is a real capture: a high
  `worstNotchMs` beside a high `stateRows`. Confirmed inefficiency: a collapsed
  state group drawing ONE summary line still instantiates a delegate per member.
- **GUI stall tracing** (`LIGHTNING_GUI_STALL_TRACE`, `src/app/GuiStallTracer`;
  default 250 ms, env value >= 50 overrides): one line per stall, coarse
  RAII-scope category, literal strings only, never content. `stalltrace::Scope`
  writes a single GLOBAL category, so it is inert off the GUI thread — a
  confidently wrong category is worse than `unattributed`.
- **The rail's drop gesture never once grouped, through THREE rules and two
  rounds that each believed they had fixed it.** All three shared one shape: *a
  reading that moves things while the user is still aiming.* Retired, do not
  re-propose: (a) "the middle 24 px of a row is the group zone" — reaching that
  middle means crossing the near edge first, which reorders, so the row under
  the pointer becomes the DRAGGED entry, never a group target; (b) a 320 ms
  dwell plus a 12 px dead zone; (c) `updateDrag(row, !dwellTimer.running)`,
  where `running` is TRUE for the whole 250 ms the dwell is served, so the
  second sample reordered and then stopped the dwell it was waiting for. The
  rule now is Discord's: the TILE is the group target, the GAP between tiles is
  the reorder target, nothing moves while the pointer is on a tile, and there
  is NO dwell because the geometry carries what the dwell stood in for.
  `updateDrag` was REMOVED rather than shimmed for three exclusive verbs
  `hoverGroup`/`hoverGap`/`clearDropTarget`, and the reorder destination
  derives from a GAP index with the `g > dragRow ? g - length : g` conversion
  the row-index version never had — separately why a one-row hover oscillated.
  **§7's rail paragraph still describes the 250 ms dwell as a second guard;
  this entry is the later record.** GENERALISE (third time): fifteen model
  cases passed through every broken rule because they hand the model a state
  production could not produce. `RailDragQmlTest` drives real mouse events at
  tile centres from real delegate geometry and asserts on the STORE; all six
  cases FAILED on the reverted tree.

#### Models, backends and derived data

- **The mock backend being RIGHT is how a backend defect survives** —
  `RoomInfo::childRoomIds` is contractually a Space's DIRECT children and was
  that on the mock and HTTP backends, while the Rust backend filled it from
  `descendants` (the transitive closure), so Channels listed a subspace's rooms
  twice and fifteen model tests passed against the mock throughout. GENERALISE:
  when a field's contract is enforced only by the testable backend, the others
  are undefended. Fixed by reading each Space's own `m.space.child` with the
  spec comparator (`order` first, room id tiebreak, empty-`via` skipped).
- **A design where every view is the same list narrowed by a scope cannot
  express a tab** — Channels collapsed every non-`!` scope to `""`, so the rail
  had one way to say anything that was not a Space and DMs had to ride inside
  EVERY view to stay reachable. Keeping the selection VERBATIM and CLASSIFYING
  it made three real views possible. GENERALISE: when a fix must make every
  surface carry something so it stays reachable, what is missing is a PLACE for
  it to be.
- **A layout that becomes the other layout is not a layout** — Channels scoped
  itself to the active Space, so at Home the host rendered Classic and the user
  silently got the layout they had not chosen. The fix removed the premise: no
  `spaceId` at all, and rooms from the CLIENT rather than the Space-scoped,
  chip-filtered `RoomListModel`.
- **A DM is never scoped by a Space, in any filter** — Matrix gives no way for
  a DM to be a Space's child, and a scoped Space dropped the account-wide
  "Rooms" group, the only place a DM could live. The column can now say a
  filter matched nothing (`matchCount`) without claiming the ACCOUNT is empty.
- **`level = parentSpaceIds.isEmpty() ? 0 : 1` is a two-level approximation
  that looks like a hierarchy** — a three-deep tree rendered as a flat pair of
  indents. Real depth is a breadth-first walk with assign-once semantics, which
  is also what makes it cycle-safe and stable under multiple parents; a Space
  the walk never reaches becomes a ROOT rather than being dropped.
- **Announce only what you actually learned** — `DirectAvatarResolver` cached a
  profile answer only when it carried a NON-EMPTY avatar but announced EVERY
  answer, and its owner rebuilds on that signal and re-resolves, so every
  avatar-less peer and every 404 ran rebuild -> fetch -> answer -> rebuild
  forever: one `/profile` and one full rebuild per round trip, per peer. That
  was the slow account switch, a switch clearing the caches and re-arming it,
  and the comment claiming "this cannot feed itself" was false for the two
  commonest answers. Fixed by caching the NEGATIVE and announcing only a face
  learned; the rebuild is coalesced per event-loop turn and resolves children
  against the map it already built, not `directChildRoomsDetailed`, which
  materialised the whole room list and a fresh hash PER SPACE. No test saw it:
  the fixture's `fetchUserProfile()` returns 0, and the resolver skips its
  pending bookkeeping on op 0.
- **A derivation living privately in one model will be wrong in the next** —
  `RoomInfo::avatarUrl` is empty for most DMs, so the Channels column drew
  initials beside a Home strip showing real faces. A late answer must run a
  `rebuild()`, not a bare `dataChanged`: the rows hold a SNAPSHOT.
- **A room-list indicator must not be allowed to ask** — `read_membership_events`
  falls back to a full `/state` whenever the store holds no live membership,
  the normal state of every idle room, so a self-refreshing call glyph would
  issue one `/state` PER ROOM per rebuild. `RoomCallGlyph` reads only what the
  controller knows, `app.rtc.refresh` is banned by contract test, and the
  honest cost is that a call in a room nothing has poked shows nothing.
- **When a row stops being a `StateChange`, grep every branch testing for
  one** — a new `TimelineEvent::CallEvent` silently un-suppressed call events
  in `NotificationManager` (an EMPTY notification per call) and in the Rust
  backend's activity test (blanking the room-list preview).
- **A collapsed folder cannot be reported on, so it must not be written over** —
  `applyArrangement` takes the whole arrangement in one write and a folder LEFT
  OUT keeps its members; without that, a drag past one would empty it.
- **Per-row state cannot live in the delegate** — a timeline row is destroyed
  the moment it leaves the cache buffer. `MediaVisibilityStore` keys by media
  identity, bounded at 4096, and the cap releases the OLDEST rather than
  refusing the newest: refusing to hide what the user just asked to hide is the
  worse failure.
- **A `json!` past serde_json's macro recursion limit is a compile error naming
  no key** — it points at the macro, not the addition. Hoist any nested object
  into its own `let` first.
- **"Mark as read" was a silent no-op for any room but the open one** —
  `markRoomRead` walked the client's timeline, which on the Rust backend holds
  only the ACTIVE room. `mx_rust_mark_room_read` takes the target from
  `Room::latest_event()` and sends the public receipt AND `m.fully_read`.

#### Matrix protocol, privacy and lifecycle decisions

- **Read the reference implementation; do not infer a wire format.** Raised
  hands: three things would have been wrong by inference — the target is the
  sender's OWN `m.call.member` STATE event, not a timeline message (that scopes
  a hand to one call, since rejoining publishes a new membership); the key is
  TWO code points (U+1F590 + U+FE0F, visually identical to the one-code-point
  form in every editor, so the test asserts the seven UTF-8 bytes); and the
  sender must OWN the membership they annotate, or one user could raise
  everybody's hand. A redaction names only what it removed, so "whose hand went
  down" comes from a locally held `reaction id -> identity` map.
- **Message forwarding** re-sends a NEW, unrelated event with NO relation (no
  Matrix forward primitive), so a forwarded thread reply lands as an ordinary
  message. **Media is RE-UPLOADED, never mxc-copied** — the target's members
  may not be entitled to the source mxc under authenticated media, and an
  encrypted source's `file` block carries per-event keys that must not be
  planted in a room that never negotiated them. Filename and MIME are
  re-originated and sanitized: leaf-only filename, type from MAGIC BYTES — NOT
  `QImageReader::format()` (plugin-backed; WebP lives in qtimageformats, which
  the packaged fleet need not carry) and NOT `gif::validateRasterBytes` (whose
  4096 px / 25 MiB caps would refuse a 5K screenshot). Review caught three
  defects: every image forward would have written decrypted bytes into the
  saved-media store; forwarding to any room but the OPEN one failed 100% of the
  time; a server refusal after dispatch was SILENT.
- **Sliding sync delivers `m.room.pinned_events` ONLY inside a room
  SUBSCRIPTION's required state** — the open room is THE one subscription,
  replacing the previous set, and `stop_sync_and_wait` forgets it so a later
  account cannot inherit it. Relatedly, opening a room notified for its own
  backlog, which arrives as live appends while `roomVisibleAtLatest` is false.
- **Server search covers UNENCRYPTED rooms only, and every surface says so** —
  the server cannot search ciphertext, so in an encrypted room the
  loaded-timeline find is the only search and the find bar offers no History
  segment. The only content sent is the typed term.
- **UIA scrubbing is transit hygiene, never a guarantee** — buffers are zeroed
  best-effort, but on the success path the String moves into ruma's
  `uiaa::Password`, which serializes and drops it without zeroing. A real 401
  surfaces sanitized stage NAMES only, the current device is guarded out of
  per-device sign-out, and **OAuth/MAS accounts have NO password stage**, so
  their buttons open the account-management URL, never a fake prompt.
- **The ignore list is the SDK's read-modify-write of `m.ignored_user_list`,
  never a Lightning-local database** — the SDK clears the whole event cache on
  a list change (timelines reset and refetch; expected), and `senderIsIgnored`
  closes the notification race before the server stops sending. Report is
  `Room::report_content` (requires Joined); `report_room` (MSC4151) and
  `report_user` (absent from the SDK) are deliberately NOT offered, and the
  message menu uses the real room id, never the thread composite.
- **Drafts: encrypted rooms are memory-only, and an UNKNOWN encryption state
  fails closed to memory.** Unencrypted rooms persist account-scoped (LRU 256);
  saves are 1 s debounced, and the debounce is STOPPED before every room/thread
  change with the save reading the still-current key.
- **Smaller protocol decisions.** A refused `get_room_preview` still resolves,
  so Join stays offered; knock withdrawal is a Knocked-state `Room::leave`,
  because the normal leave path filters to Joined; the `/hierarchy`-backed list
  is bounded to 10 pages / 200 rows; `restricted_denied` is classified
  separately, never presented as plain invite-only.
  `mx_rust_set_space_child_suggested` reads the CURRENT `m.space.child`,
  preserves via/order, flips only `suggested`, REFUSES a non-child (empty-via
  included) and never promotes one, and "Suggested" shows only when the
  hierarchy KNOWS. `mediaDownloadUrl`/`mediaThumbnailUrl` were the last surface
  handing unauthenticated `/media/v3` links to the browser and now return empty
  on the Rust backend. A `%n` source string renders its "(s)" literally without
  a loaded translation, so "Seen by N people" is branched explicitly, and
  `tsMs` 0 renders nothing rather than a fabricated time.
- **Rail / Space Home** — a SINGLE tap on a real Space opens Space Home (which
  REPLACES the chat view), there is deliberately NO double-tap, and the ONLY
  expansion trigger is the chevron disc, whose band is excluded from the tile's
  tap. `openSpaceHome` is ordered teardown-first, activation-last because the
  loader instantiates SYNCHRONOUSLY and its handlers point RoomInfoController
  at the Space, and the old order wiped the canInvite/canManageSpaceChildren
  gates afterwards. `spaceJoined` drill-in had been an UNFILTERED listener.
- **A keyed dedup must service ALL claimants** — a saved-media star and a Copy
  image racing on the same uncached image left the star stranded forever. Both
  fetch through MediaBridge with pending-key discipline and magic sniffing (SVG
  refused). Reply-to-image thumbnails register the embedded reply event's media
  under the reply target's event id; the media KEY crosses the FFI, never bytes.
- **Presence is a bounded poll because Sliding Sync delivers NO presence
  events** (MSC4186 has no presence extension): one batch per round (raw ruma
  `get_presence`, <= 40 users, 10 s no-retry timeout so sign-out's task join
  cannot stall), 30 s rounds with rotation past the cap. Transient failures
  KEEP the last known state, forbidden/not_found erase it, and two consecutive
  all-forbidden batches of at least two distinct users each latch "server has
  presence disabled" for the session — a single user's 403 never latches.
  **Unknown renders NOTHING, never a fabricated offline.** Own presence is
  gated by the application-wide `sharePresence` (default ON, global not
  per-account; disabling publishes ONE final offline).
- **Login button naming follows Element classic's actual strings** — both
  "Continue in browser" and "Sign in with SSO" open a browser, so naming the
  MECHANISM told the user nothing; what differs is which authority
  authenticates them. Element's order is `["oauthNativeFlow",
  "m.login.password", "m.login.sso"]`, SSO is primary only when there is no
  password flow, and the homeserver host on the browser button derives from
  what the USER typed, because a server must not choose the words on
  Lightning's own button. **The i18n catalogs are deliberately NOT
  regenerated** — already ~27 strings behind, and `lupdate` rewrote all 10
  files (53k lines) warning "Removed plural forms as the target language has
  less forms", a real plural-damage risk, so those labels render in English on
  non-English UIs until a dedicated localization refresh.
- **MediaBridge request priorities** (0 explicit playback/save, 1
  avatars/thumbnails, 2 full static, 3 speculative GIF prefetch), two slots
  reserved for interactive classes, a 15 s starvation bound, temp-file pinning
  while a QMediaPlayer holds the file, queued-speculative dropping on room
  switch, byte-sniff rejection of A/V containers on thumbnail-class results,
  offscreen player reclamation (45 s audio, 90 s video). An SDK receipt MOVE
  arrives as adjacent Set diffs, so the poll drain must not split the pair
  across 100 ms ticks. libpipewire was made resolvable in the dev shell so Qt
  Multimedia uses native PipeWire, not the PulseAudio fallback a captured FLAC
  crash aborted in. Receipt-loss mechanisms Lightning cannot fix without
  patching matrix-sdk-ui 0.18: `docs/receipt-semantics.md`.

#### Testing and harness discipline

- **A policy test that invokes the policy directly proves nothing about whether
  production ever reaches it** — recorded three times: the row window shipped
  as a permanent no-op, and the rail drop passed fifteen model cases through
  two successive broken rules. **A regression test that does not fail on the
  old code is decoration**; prove it against the unfixed tree.
- **Mutation-check every new sweep and give it a `found > 0` guard.**
  `everyRuntimeChosenIconNameIsMapped`'s C++ half tried to pattern-match its
  call sites, matched NONE of them, and passed on a deliberately broken tree;
  fixed by moving the names into a `kIcon…` block the sweep finds by prefix and
  BANNING the literal form. The Channels suite was checked the same way against
  two mutations of the FIXED tree (a Space view carrying the DM group again: 4
  failures; a Home repeating every Space: 2).
- **Anchor a source scan on the EXPRESSION, never on a fixed window after a
  name** — fourth occurrence. A case read 700 chars after `function
  clampCallPanelHeight`, and the explanatory comment inside pushed the code to
  offset 1016, so it failed on the FIXED tree. Mutation-check both halves.
- **A negated character class matches newlines** — a comment stripper using
  `(?m)\s//[^"']*$` let `[^"']*` cross newlines, so a trailing `//` comment
  consumed every following line until one ended in a quote, silently weakening
  **every scan in that file positioned after a trailing comment**. GENERALISE:
  a "strip comments" regex is a parser; assert something you KNOW is present
  and watch it fail.
- **An offscreen pixel is evidence only once every animation touching it has
  finished.** Four rounds of probes "proved" the Channels column marked the
  wrong row; every reading came from a `--demo-capture` at the default 1400 ms
  settle, and the rows' 90 ms `Behavior on color` had not advanced, so the grab
  held each row's CREATION-time colour. At 6000 ms every row was correct and
  always had been: NOTHING was wrong with the code, and one speculative fix was
  made on that false reading and reverted. A property probe rendered into a
  LABEL can disagree with the pixel for exactly this reason, which is what
  makes the contradiction diagnosable.
- **Suspect the harness first when a measurement indicts something distant** —
  `startSync()` returns silently before login completes, publishing before
  `Connected` puts no track on the wire, and sampling the SFU before a share
  starts looks like a forwarding failure.
- **Ask an agent what it OBSERVED, not what it concluded**, before writing its
  conclusion into a commit message. A "d3d11 and mediafoundation cannot load"
  finding was a static symbol comparison presented as an observed load failure;
  re-run, both plugins loaded under Wine. The DECISION survived (the absent
  import is real and Wine cannot adjudicate it); the reason did not.
- **Six of seven test failures in one round were bad tests, not bad code** — a
  ban regex matching a token named in a COMMENT, an icon regex matching `State
  { name: }`, three fixed-window source scans defeated by added comments, a
  click helper that never scrolled (Qt DROPS a press outside the window), and a
  reflow guard measuring scene coordinates so a scroll read as a reflow. Ask
  what an assertion meant to measure before deciding who is wrong, and repoint
  it with teeth rather than deleting it. `qmlformat` over `qml/*.qml` is a
  seconds-long parse gate worth running before any build.
- **`QAbstractSocket::waitForReadyRead()` cannot work against a server on the
  SAME thread** — blocking the caller is what stops the listener accepting.
  `SsoCallbackTest::deliver()` ended in `waitForReadyRead(3000)`, so each of
  eleven deliveries burned the full bound: **34.5 s of a 34.5 s suite**, and
  **1.4 s** with the wait removed. Pumping the loop in the helper is WORSE —
  the server then answers before the caller arms its `QSignalSpy`, and
  `QSignalSpy::wait()` waits for a NEW signal, so seven cases fail.
- **Contract-suite duplication detection is mechanical** — extract every
  `contains(QStringLiteral("…"))` needle per suite and rank suite PAIRS by
  intersection. One GIF-picker case had grown to 190 lines, 170 of them
  internals, with **26 of its 41 needles asserted again** in a second suite.
  Left alone deliberately: 23 suites each declare their own `MatrixClient`
  subclass with ~13 identical `override {}` stubs (~300 lines; a shared double
  would be a 23-file change).

#### Performance, disk and logging

- **The first `QVideoSink` in a process costs ~931 ms** (lazy Qt Multimedia
  init including a hardware-decoder probe that fails without VAAPI); the same
  extraction on a worker thread is 1 ms, and the per-frame theory was WRONG —
  `toImage()` is 0.24 ms. Two traps: a plain `moveToThread` leaves a MEMBER
  `QTimer` on the creating thread where Qt refuses to start it, silently
  disarming the 6 s watchdog (make it a CHILD); and the reply becomes QUEUED,
  so `disconnect()` no longer reliably cancels one already posted — session
  isolation keys on `m_posterExtracting`, not on the connection.
  `warmMultimediaBackend()` pre-pays the init off-thread for the first inline
  PLAYBACK, whose sink QML builds on the GUI thread and cannot move.
- **A log line that fires per CALLER does not belong in a default-on category;
  only state transitions do.** `avatarSource()` was the only one of five
  `alreadyPending()` branches that logged, and that branch is reached once per
  caller — O(callers), unbounded in a list. Twelve per-request lines moved to
  `lightning.media.trace`, with one counts-only burst summary once activity
  goes quiet. Separately `Avatar.qml` called the bridge from three triggers per
  instance, one of which (`onSizeChanged`) could not change the request at all,
  because `avatarSource` opens with `Q_UNUSED(size)`.
- **Where the disk goes.** A debug `libmatrix_client_rust.a` is **2.1 GB** and
  every one of the ~146 test binaries links it: `matrix-client` alone is 906 MB
  in `build-rust` against 124 MB in `build`, and the test binaries total
  **35 GB** there versus 5.1 GB in the non-Rust tree. With the two
  `incremental` caches (25 GB and 13 GB) the repo was 157 GB; those are pure
  caches, costing only the next build's incremental state. `nix store gc` freed
  **63 GB** — pin the dev shell FIRST (`nix develop --profile <path> -c true`,
  registering a root under `/nix/var/nix/gcroots/auto/`) or the GC takes the
  whole Qt/Rust toolchain with it. `split-debuginfo = "unpacked"` in
  `[profile.dev]` is **NOT applied**: a build-config decision for Rokas, and
  `[profile.release]` (which packaging uses) is unaffected either way.

#### Live status for these rounds

**NOT TESTED** live, and do not promote any of them: the Sable-parity round
(three Channels views, member column, call glyph); the 2026-08-19
design-deficit pass (CTest 134/134 both trees is a build result, not a GUI
one); the Element-parity round (`space-child-suggest` 4,
`element-parity-contract` 5); discovery / search / UIA / moderation / drafts;
pins / power levels / join rule / alias (real `m.room.pinned_events` round
trips, Element interop, a homeserver accepting or refusing a write, alias
publication, and the on-screen look of any of it); Matrix presence; the
2026-08-11 media/UX round; the tester report #2 round on Windows; any call
PLACED from a Windows or macOS package; an ANSWERED legacy 1:1 call.
**Live-validated**: MatrixRTC audio, camera and screen share both directions
against Element, and the Windows camera and window share on a packaged build —
the next subsection carries the full confirmed list.

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
- **The incoming-call prompt's Accept does nothing.**
- **Windows camera runs at 10 fps**, and the fix for the freeze did not touch
  it. `ksvideohelpers.c` exposes `image/jpeg` for MJPG media types, the
  publish bin links `capsrc ! queue ! videoconvert` with no decoder, and
  `libgstjpeg.dll` is absent from the staged plugin list — so an MJPG mode
  cannot negotiate and the camera falls back to raw YUY2, which at 1280x720
  is 18.4 MB/s and hits a USB 2.0 ceiling at 10 fps. Staging the jpeg plugin
  and adding a decoder to the camera branch is the lead; it is a packaging
  change, so it must go through the shipped-artifact check (§16).
- ~~**Raise hand is invisible to Element**~~ — **FIXED and LIVE-CONFIRMED
  2026-08-26** in both directions. The wire representation was established by
  READING element-call's own source rather than guessing: an `m.reaction`
  annotating the raiser's OWN `m.call.member` state event with
  `\u{1F590}\u{FE0F}`, lowered by redacting it. Three lanes (our send, two
  sync handlers, one bounded join-time sweep for hands raised before we
  arrived).
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
