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

The state verified on 2026-07-19 is:

- Latest published release: **Lightning 0.6.1**
- Published tag: `v0.6.1`, cut from the release commit on `main`
- Previous release: `v0.6.0` -> `2157194` (immutable, unchanged)
- Application version: **0.6.1** in `CMakeLists.txt`, `rust/Cargo.toml`, and
  the Rust/HTTP user agent
- `main` is at or just past the 0.6.1 release commit
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

Do not describe these systems as future-only work. Keep the version at 0.6.1
until an explicitly requested release checkpoint changes it.

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

Work on `main` only unless Rokas explicitly requests another branch. Use normal
fast-forward pushes. Never force-push, rewrite history, amend a pushed commit,
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
- SAS emoji device verification in both directions, session/device trust UI,
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
- Per-room notification modes are currently device-local, not synchronized
  server push rules

### Settings, usability, and accessibility

- System, light, Graphite, Midnight Blue, Nord, and Purple Dusk themes
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

Rokas's local development key file is:

```text
/home/roksme/Documents/API/lightning-gif.env
```

It is local configuration, never a repository file. The supported environment
variables are:

```text
LIGHTNING_GIPHY_API_KEY
LIGHTNING_KLIPY_API_KEY
```

Never print, log, commit, embed, or pass key values through QML. These are
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
- **CTest:** registered C++/Qt/QML/controller/bridge tests. The current CMake
  registers 31 tests in each configured build tree.
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

Version 0.6.1 is released and the synchronized CMake, Rust, and user-agent
version report 0.6.1. Any future version bump is a release checkpoint alone and
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

The latest published release is `v0.6.1`, cut from its release commit on `main`.
The previous release `v0.6.0` at `2157194` remains immutable and unchanged.
Publishing packages for `v0.6.1` did not move or recreate its tag or release.

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
- Plan any post-0.6.1 work only through explicitly requested checkpoints.

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
