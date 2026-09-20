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

Latest published release: **Lightning 0.9.8** (`v0.9.8` -> `c04ea54`), tagged
2026-09-17 by **project 6** pipeline **240, 25/25 — fully green, every job,
first attempt**. Notes in `docs/releases/v0.9.8.md`. The tree and the published
release are the same thing again; "latest published" and "what the tree says"
are different facts and this sentence has stated the wrong one before.
`tests/VersionConsistencyTest.cpp` compares five locations so a bump cannot
half-land — **and there is a SIXTH it does NOT compare, the AppStream
metainfo; see §14.** The root Flathub manifest is a SEVENTH and is re-pinned
to `v0.9.8` / `c04ea54f…`, which is the post-release step
`test-flathub-manifest-pin.py` deliberately waits for.

**THE ANONYMOUS VERIFICATION BAR PASSED IN FULL for 0.9.8** on 2026-09-17: all
**eleven** package links 200 with the count asserted, the signed manifest
reading 0.9.8 / `v0.9.8` with six artifacts all carrying `mirror_url` and macOS
correctly ABSENT, the Ed25519 signature VERIFIED against the key extracted from
the shipped `.deb` with a one-field-changed copy REJECTED, the GitHub tag
peeling to `c04ea54f5cae…`, 11 mirror assets, and the `.deb` fetched FROM
GITHUB matching the GitLab-signed SHA-256.

**IT WAS CUT AFTER A DRESS REHEARSAL, AND THE REHEARSAL IS WHY IT WAS CLEAN.**
The review approved and then observed that three checks added that day had
never executed. Two failed on their first run — `validate-appimage` and
`validate-snap`, both `error: world-writable content` — because git records
only the executable bit, so a CI checkout under `umask 0000` gives 666 files
and `cp -a` copied that into the payload. Fixed with `install -m 0644` and
re-rehearsed. It would have died in the release pipeline after building every
format. **A job that exists and looks right is not a job that has run**, for
the fourth time here and the first time it cost only rehearsal minutes.

**A CALLS RELEASE, AND THE FIRST WHERE THE CALL PATH IS INSTRUMENTED.** Six
defects, all found on 2026-09-16 from one report ("i hear myself from element
to lighting but not from ligthing to element"): a multi-input capture device
averaged its live input with three dead ones and lost 12.04 dB; a default
`queue` held one second and never leaked it; media keys delivered before the
call went active were discarded; a call could join an encrypted room in the
clear; a media key was adopted though it reached nobody; and a room Lightning
created would not let its own members join a call. Full account in
`docs/round-history.md`, 2026-09-16 (afternoon).

**AND THAT `queue` FIX BOUNDED ONE OF THE ELEVEN QUEUES THIS PROJECT SHIPS.**
An independent review found six more in `SfuMediaEngine.cpp`; a SECOND round
found four more OUTSIDE it, in `ShareAudioSources.cpp` and across the whole
1:1 lane in `GstCallMediaBackend.cpp` — while the commit that fixed the first
batch claimed "every queue on a live path" and its sweep read ONE FILE of
three. All eleven are bounded now, and leaky except the one carrying RTP into
a video depayloader (leaking there corrupts the bitstream downstream of
webrtcbin, which then sends no PLI; its latency protection is the appsink's
own `drop=true`). The sweep asserts a PER-FILE count across all three sources.
**A sweep is only as wide as what it reads, and a count assertion over too few
files is a confident wrong answer.** Full account in `docs/round-history.md`,
2026-09-17.

**VOICE DELAY IS NOW ASKABLE OF A PACKAGE: `--call-queue-selftest`.** It had
only ever been measured acoustically, which needs two machines, a sound card
and a rig — so it existed for Linux alone, and the Windows guest (no sound
card; RDP playback drifting 291 -> 545 ms, larger than the effect) could not be
measured at all. The command starves the CONSUMER of every queue the publish
pipeline actually builds, runs a plain `queue` beside them as a CONTROL, and
reports what each still held once the consumer was back at REAL TIME — all a
live encoder ever gets. Measured: shipped 90-100 ms, **default 1000 ms and
still 1000 ms**. That 900 ms is what this file has asserted since 2026-09-16
and never demonstrated. **`SIGSTOP` cannot show it** — freezing stops producer
and consumer together, so no backlog forms and a flat result means nothing.

**MEASURED ON SEVEN ENVIRONMENTS, FIVE GSTREAMER VERSIONS, TWO OPERATING
SYSTEMS, AND THE NUMBERS DO NOT MOVE**: all six Linux formats (pipeline 230,
14/14 green, each asked of the artifact its own job built), the dev shell, and
a Windows 11 guest running the shipped 0.9.8 portable. Shipped queues 40 and
90-100 ms; the default 1000 ms and still 1000 ms. It is wired into every
package validator through ONE helper in `packaging-ci/scripts/lib.sh` and is
deliberately NOT a hard gate yet — a transcript with no verdict, or one saying
`VERDICT: unmeasurable`, fails the build; a `VERDICT: fail` only warns. The
promotion procedure is written at that declaration and requires the newer
command to report `pass` on all seven in ONE pipeline first. **A required
entry is the half that must come second** (§16, pipeline 224).

**LIVE-VALIDATED PASS, AND NO LONGER NARROWLY — this paragraph was written on
release day and was overtaken the same night.** What it originally recorded
stands: Lightning <-> Element Web, audio both ways, latency "almost instant",
on ONE device (a Roland Rubix44 on PipeWire). Added 2026-09-16 evening, all
measured with a tone through a Goertzel detector rather than reported, and all
in `docs/live-validation.md`: the **published Windows portable** against
**Sable 1.21.0**, against **Element Web's Element Call**, and against the
**published Linux AppImage** — audio both ways and screen share both ways in
every one of the three. So Windows, Sable, Element Call and cross-platform
Lightning<->Lightning are TESTED. **macOS is still NOT TESTED**, and so is any
MatrixRTC client other than those three.

Two limits that are real, and one of them is now closed. **NO published 0.9.7
package has the capture level meter** — measured, `level= false` in a real call
from the Windows portable AND from the AppImage. On Windows the hand-built
builder image does not carry `libgstlevel.dll` (builder **v7** adds it, §16);
on the AppImage the plugin entered `GST_REQUIRED_PLUGINS` in `8e744c7`, which
is AFTER the release commit. So 0.9.7's headline diagnostic, and the
mic-silence badge that depends on it, cannot fire in anything shipped. Both
halves are fixed on `main` and neither is released
(`docs/open-items.md`). **THE WINDOWS CAMERA DID NOT REPRODUCE ON
2026-09-17, and that refutes two hypotheses including my own next one.** Same
published 0.9.7 package, same guest, same camera: the self-view drew a real
picture on the compressed chain AND on the raw one (forced by moving
`libgstjpeg.dll` out of the package, which needs no new build), and the far end
logged `frames decrypted ... video= true count= 1000 dropped= 0` with no
`NOT rendered` warnings. Nothing in the code changed in between, so this is
NOT a fix — it means the fault is not a property of the package, the chain, the
camera or the renderer, and the next step is to find what DIFFERED between the
two sessions rather than to guess at the pipeline again. **NOT REPRODUCED, NOT
EXPLAINED**; nobody may say it works and nobody may say the picture never
arrives. Same run quantified the ceiling the JPEG plugin removes: the raw chain
negotiated **5 fps** at 1080p where the compressed chain negotiated 30. **Do not repeat
"encrypted camera and screen-share sending does not carry"**: §16 records that
as stale since `RtpVp8Payloader.cpp`, and tonight's runs carry encrypted screen
share to Sable, to Element and to Lightning on another platform.

**TWO PIPELINES DIED BEFORE 225 AND BOTH WERE THE RELEASE COMMIT'S OWN
MISTAKES, caught by gates with nothing published and no tag created**: 223 on
the metainfo (§14's sixth location) and 224 on `build-windows`, where a
plugin was added to the REQUIRED list before the hand-built image carried it
(§16). Cancel a doomed pipeline immediately — it holds the runners.

Previous release: **Lightning 0.9.7** (`v0.9.7` -> `bc5dcd5`), tagged
2026-09-16 by pipeline **225, 25/25**; notes in `docs/releases/v0.9.7.md`. Its
own bar passed in full on 2026-09-16 and its result is in
`docs/release-operations.md`. It is the release whose three headline
promises could not fire in any shipped package, which is what 0.9.8 is for.

**0.9.6's RECORD HAS MOVED to `docs/release-operations.md`,** under "0.9.6,
and the first macOS release since the 413". Its one red job, the fix that
proved it, its macOS asset and its source-validation numbers are all there.
What stays here is the LESSON that came out of it, immediately below: the
verification bar had been silently checking one link fewer than every release
had.

**AND THE BAR ITSELF HAD BEEN UNDER-REPORTING BY ONE LINK ON EVERY PREVIOUS
RUN.** `verify-release.sh` wrote its link list with `"\n".join(...)` and read
it with `while read -r u`, which DROPS a final unterminated line — so the last
package link was never fetched, on 0.9.6 and on every release before it. That
is why this section used to say "nine package links" for a 0.9.5 that had ten.
Fixed two ways, because the silent-short-count was the real defect: the file
gets its trailing newline, and the script now ASSERTS that the number of links
it checked equals the number the release reports. A durable copy lives in the
vault at `Lightning/Tasks/verify-release.sh` — it used to exist only in a
session scratchpad, which is how a harness this load-bearing stayed unreviewed.

**0.9.5 AND THE macOS 413 SAGA HAVE MOVED to `docs/release-operations.md`,
under "0.9.5, and the 413 that kept macOS out of it".** That is where the
release inventory already lives, and none of it is a lesson this file needs in
front of an agent: it is the record of one release, the artifact-size cap that
kept macOS out of it, the loopback relay that fixed it, the endpoint change
that does NOT work on that host, and the two-`.deb` filename hazard. Read it
before any macOS or artifact-size work.

**RECOVERY AND KEY BACKUP SHIPPED NOT TESTED, AND DELIBERATELY SO.** §6 forbids
capturing a recovery key and the setup flow displays one, so the feature was
audited by READING it — QML to controller to FFI to matrix-sdk 0.18.0's own
sources. Six defects fixed, two of them able to destroy an account's recovery,
and both of those are visible only by reading the SDK. Four more are known and
NOT fixed; none can destroy a key. `docs/round-history.md` (2026-09-13 night)
and `docs/open-items.md`. Do not promote this feature to tested on the strength
of the audit.

**AppImage validated as an artifact, not just as a job** (measured at 0.9.4;
the same checks pass in 215). It runs, reports
`Lightning 0.9.4`, its media engine is built in, both call engines are
available, and the recorded 0.9.0 gaps stay closed — Qt's TLS backends
(`libqopensslbackend.so`) and the Wayland shell integration
(`libxdg-shell.so`) are both in the payload. It cannot be launched BARE on
this NixOS host (`libEGL.so.1`: graphics libraries come from the host and
NixOS does not put them on the standard path — the host, not the package);
`nix-shell -p appimage-run` runs it. And the `gst-plugin-scanner`
gap this section used to record is **CLOSED, and proven on the artifact**:
project 6 pipeline **187** builds and validates an AppImage that stages the
helper, reports its path at launch and logs zero plugin-loader warnings.

Before those: **Lightning 0.9.4** (`v0.9.4` -> `bcea599`), tagged
2026-09-10 by pipeline **186, 20/20 green on the first attempt**; notes in
`docs/releases/v0.9.4.md`. Its verification bar passed in full on 2026-09-10
(ten package links, 10 mirror assets, the AppImage fetched from the mirror
matching the signed SHA-256).

Before it: **Lightning 0.9.3** (`v0.9.3` -> `7306dde`), tagged
2026-09-08 by **project 6** pipeline **183, 21/22** (the one red job is
`mirror-update-manifest-to-github`, `allow_failure`, and it was a false
negative: see below); notes in `docs/releases/v0.9.3.md`. The synchronized
version reads **0.9.3** in
`CMakeLists.txt` (both `project()` and `APP_VERSION_LABEL`), `rust/Cargo.toml`,
`rust/Cargo.lock`, and the Rust/HTTP user agent (derived from
`CARGO_PKG_VERSION`). Any bump after it is a new release checkpoint and only on
Rokas's explicit request (§14).

The same bar PASSED in full for **0.9.3** on 2026-09-08 (release at
`7306dde`, nine package links, 10 mirror assets), and the website (third repo,
§14) passed both of its checks against it. 0.9.4's run above is the current
one; the shape of the bar is in §14.

**0.9.3's ONE red job was a verification racing GitHub's CDN, not a bad
upload** — a stale-but-200 read-back of its own upload, fixed in `6149337` by
comparing the digest INSIDE the retry loop. Both it and its sibling (a 404
while the API says `state=uploaded`) are in `docs/release-operations.md`.

**ON `main` ABOVE 0.9.4 (2026-09-10, NOT a release).** Nothing tagged, no
version bumped: the post-0.9.4 audit debt (nine defects 0.9.4 shipped
without) and four items that were degrading every session, including this
file's size limit and the AppImage's `gst-plugin-scanner`. Both batches, their
accepted follow-ups and one refutation are in `docs/round-history.md` under
2026-09-10. Validation: Rust 413 passed, `WEBRTC=OFF` over every target, both
CTest trees green. NOTHING in it is live-validated.

**The 2026-09-08/09 four-audit round SHIPPED IN 0.9.4** (`5d9fa37..820d368`;
`820d368` is an ancestor of `bcea599`). See `docs/round-history.md`,
2026-09-08. Not live-validated.

**THE PIPELINE NOW LIVES IN THIS REPOSITORY.** 0.9.2 is the first release cut
from project 6: the packaging project was folded in under `packaging-ci/` on
2026-09-06 with all 125 of its commits, and `.gitlab-ci.yml` sits at the root
because GitLab reads it nowhere else. Project 7 still exists and is NOT
deleted. Trigger with `glab api --method POST projects/6/pipeline` — the id
in every older note here is 7 and is now wrong. See
[[packaging-moved-into-the-app-repo]] for the one mistake that layout
creates: both repos have `scripts/`, `tests/` and `docs/`, so a path that
reaches the packaging tree from the repository root finds a REAL directory
with none of those files in it, and that cost three separate fixes and two
dead pipelines.

0.9.1 (`v0.9.1` -> `d2e343b`, project 7 pipeline 175, 2026-09-06) and 0.8.4
(`v0.8.4` -> `49249e8`, pipeline 170, 2026-09-02) preceded it.

The same bar passed in full for **0.9.2** (`2545391`), **0.9.0** (`9dc6a07`;
pipelines 171 and 172 were CANCELLED on the way, §16: the QtDBus guard and
the Windows builder image) and **0.8.0** (`6f203be`). **Two of its lines FAIL
for a reason that is not the release**: openssl is not on the bare shell's
PATH, so the signature check cannot run there — use
`nix shell nixpkgs#openssl`, and note the signature file's field is `sig`,
not `signature`. The script is `verify-release.sh` in the session
scratchpad; the recipe is in §14. Run the same bar against every release.

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

### Release inventory and operational traps

**MOVED: the full text is `docs/release-operations.md`.** It holds the release
inventory table (every version, its commit, its deploy pipeline and its notes
file) and the traps release rounds have learned — the local package runners
that cannot link this project, the GitHub `update-latest` slot the mirror was
deleting, CDN read-back races, the JSON-body trigger requirement, `glab`
picking its host from the cwd, `needs` wiring for published bytes, verifying
anonymously rather than from job status, and the Debian-container check.

READ IT before any release, packaging or pipeline work. The latest published
release and its verification status stay above, in this section; the release
POLICY stays in §14.

### Update / upgrade live-validation truth

**MOVED: the full text is `docs/release-operations.md`,** beside the release
inventory and traps it belongs with. It records which update paths have been
exercised for real, which are structurally unreachable from the release
before them, and that Windows packages remain unsigned.

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
  -> verified-session gossip, or Lightning's own backup download pass
  -> key imported by the SDK
  -> SDK/event cache retries decryption
  -> timeline emits a replacement/update
  -> the same stable event updates in place
```

**TWO OF THE THREE MECHANISMS THIS DIAGRAM USED TO NAME ARE NOT WIRED, AND
THE LINE ABOVE CLAIMED THEM UNTIL 2026-09-15.** Verified against the tree, not
inferred: `automatic-room-key-forwarding` is NOT among the features
`rust/Cargo.toml` requests and `matrix-sdk` is `default-features = false`, so
`create_outgoing_key_request` is `#[cfg]`'d out and **Lightning has never sent
an `m.room_key_request` on a decryption failure**; and
`BackupDownloadStrategy::OneShot` (`rust/src/lib.rs`) makes matrix-sdk install
neither the UTD event handler nor the `BackupDownloadTask`, so a decryption
failure triggers no backup fetch either. What remains is Lightning's own
`download_backup_keys_for_room`, which is deduplicated per room per lifecycle
and so runs AT MOST ONCE per room per session. Whether those SDK mechanisms
SHOULD be on is an open decision (upstream made key forwarding opt-in
deliberately); this paragraph records only that they are off.

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

**READ `docs/release-operations.md` FIRST.** This section is the policy — what
a release may do and in what order. That file is the record: the inventory of
every published version, and the operational traps that have cost real
pipelines. Neither is complete without the other.

Published tags and GitLab Releases are immutable. Never move, recreate, or
replace them. Do not bump a version, tag, or create a release unless Rokas
explicitly requests release work.

The synchronized CMake, Rust, and user-agent version is authoritative over any
number quoted in this file; read it from `CMakeLists.txt` rather than from
here. Any version bump is a release checkpoint alone and updates those same
synchronized locations.

**AND THERE IS A SIXTH LOCATION `VersionConsistencyTest` DOES NOT COVER: the
AppStream metainfo.** `packaging-ci/packaging/common/lightning.metainfo.xml`
carries both the newest `<release>` and four screenshot URLs pinned to a TAG,
and 0.9.7's first pipeline (223) died in `config-tests` on exactly that —
after the release commit was already pushed, because nothing local had asked.
Bump it in the release commit:

```sh
packaging-ci/scripts/update-metainfo-release.sh write <version>
# then repoint the screenshot refs from v<old> to v<new>
```

Run `packaging-ci/tests/test-metainfo-consistency.py` before triggering; it
needs `nix-shell -p python3Packages.pyyaml`, which is why it is easy to skip.
The screenshot check is release-commit aware: the refs must name the tag that
does NOT exist yet, and it verifies the files are COMMITTED in the tree that
will become it. Before release, run complete Rust tests plus Rust and
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
source mirror with `glab api projects/6/remote_mirrors`). **RESOLVED
2026-09-04:** scoping `UPDATE_SIGNING_KEY_B64` to environment `signing` and
`GITHUB_MIRROR_TOKEN` to `mirror` in project 7 was addressed OPERATOR-SIDE, in
GitLab's variable/environment settings. Nothing in either repository
implements it and nothing here needs changing — do not reopen it or "fix" it
in CI config. The key id and public key stay unscoped, which is correct: they
are not secret.

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
dropping its own TAIL — §§17-19, the completion-report requirements and the
multi-agent review protocol — from agent context.** FOUR moves so far: §7 to
`docs/feature-contracts.md` on 2026-08-28; §16's round history to
`docs/round-history.md` on 2026-09-03 at 150,397 characters; §2's release
inventory and operational traps to `docs/release-operations.md` on 2026-09-10
at 146,262; and §16's open-items and NOT TESTED inventory to
`docs/open-items.md` on 2026-09-11 at 140,752. Before adding a block here, run
`wc -c CLAUDE.md`; past roughly 140,000 the answer is a new file under `docs/`
and a pointer, never a longer section.

**IT BINDS THE ROUND YOU ARE WORKING ON NOW**, not some future editor: one
round entry written the old way took this file to 148,643, 1,357 CHARACTERS
FROM THE CLIFF. A round's own record belongs in `docs/round-history.md` with a
POINTER here, and a pointer is three or four lines, not a summary. If adding
yours pushes this file past 140,000, move a section out in the SAME commit.

### Standing warnings

**2026-09-02 SECURITY AUDIT — READ `docs/security-audit-2026-09-02.md`
before touching the updater, `src/calls/`, `rust/src/rtc.rs`, `rust/src/sfu.rs`,
the QML plain-text rule, or the pipeline's secrets.** It holds the refuted
hypotheses (a store passphrase; filtering the mock for §8; bounding RTC
expiry against `created_ts`) and the lessons (a comment is not an
`EncryptionInfo` extractor; content timestamps and `membershipID` are
attacker input; a verified path is not verified bytes; `textFormat` on a
`MenuItem` is a load-time error; one working tree, two sessions).

**Timeline scrolling — MOVED: the full text is `docs/timeline-scrolling.md`.
READ IT before touching the timeline's scrolling, anchoring, pagination or row
window.** Five rounds, three reverted fixes, and a refuted-hypothesis table
that is binding: state-events-are-the-cost, GPU fill-rate, clipping, the
de-layouting of MessageDelegate, the anchoring machinery, the pagination
teleport, `worstNotchMs` as a frame cost, and offscreen numbers transferring to
hardware are all refuted THERE with the measurement that killed each one. It
also holds the five shipped fixes, the positive-only anchor guard nobody may
"fix" without a capture naming a failure, and why Element's unfilling does not
port here.

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

**THE WEBRTC=OFF BUILD IS SOURCE HYGIENE, NOT A RELEASE GATE — CORRECTED
2026-09-12, and this block said the opposite for months.** It used to read
"the Linux package jobs build WITHOUT the media engine, and no local tree
does". That is **no longer true of any lane**, verified by reading them:
`packaging-ci/scripts/configure-build.sh` passes
`-DLIGHTNING_ENABLE_WEBRTC=ON -DLIGHTNING_REQUIRE_WEBRTC=ON`; `.gitlab-ci.yml`
installs the GStreamer dev packages in `build-deb`, `build-deb-ubuntu`, rpm and
appimage; both Flatpak manifests pass `=ON`; `build-snap` compiles nothing at
all (it repacks the AppImage); and `configure-build.sh` asserts
`call media engine built in: yes` **against the staged binary**. So the failure
mode moved from "silently compiled out, discovered in CI" to "configure fails
fast". Do not quote the old claim at a release.

It is still worth running — `SfuCallController.cpp` alone carries ~40 `#ifdef`s
and nothing else compiles that half — and the 0.8.0 lesson it was written for
stands on its own: that release lost `build-deb` twice over in one job: an
`#include` inside the guard whose REGISTRATION was outside it, and an INLINE
accessor in a header calling into a source file the build does not compile
(the `QPointer` lesson below, in a second costume — an inline accessor in a
header creates a link dependency in EVERY target that includes it).

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

**WINDOWS AND macOS BUILD WITHOUT QtDBus, AND NO LOCAL TREE DOES.** The
0.9.0 release lost pipeline 171 to `NotificationManager.cpp:536: 'QDBusReply'
does not name a type`: `bodyForServer()` — the daemon capability probe that
gained the inline-reply query in the desktop-integration round — was defined
outside every `HAVE_QT_DBUS` block, and every Linux tree has the bus so none
of them could show it. Same class as the no-WebRTC lesson above, third
costume. THREE MINUTES LOCALLY: take the unit's own compile command from
`ninja -C build -t commands CMakeFiles/lightning-matrix.dir/<src>.o`, strip
`-DHAVE_QT_DBUS`, add `-fsyntax-only` — it reproduced the exact Windows and
macOS error before the fix and passed after; run it over every source that
names `QDBus` before a release.

**A BUILD WITHOUT QtDBus HAD NO NOTIFICATIONS AT ALL (2026-09-05, "no
notifications on windows at all").** `deliverNow` had one path, the
freedesktop daemon; the `#else` logged a line. The tray icon's balloon is
Qt's only other delivery and it needs a VISIBLE icon, so on those platforms
`refreshTrayState` shows the icon while notifications are enabled, the
manager keeps the one payload a balloon carries, and a click routes to its
room. **LIVE-VALIDATED PASS ON WINDOWS, 2026-09-12**: a real toast with the
room avatar, and a click that raises the app from minimised and opens that
room (`docs/open-items.md`). macOS remains NOT LIVE-TESTED. A
balloon cannot be withdrawn through Qt — read-dismissal there needs the
native toast APIs (WinRT `ToastNotificationHistory.Remove`,
`UNUserNotificationCenter removeDeliveredNotifications`), a follow-up.
And on KDE the read-withdrawal never reached the history because an EXPIRED
popup (reason 1) forgot its payload; it keeps it now. Also not yet seen live.

**PACKAGING TOOLCHAINS AND BUILDER IMAGES — MOVED: the full text is
`docs/packaging-traps.md`. READ IT before rebuilding a builder image,
changing a required-plugin list, pinning a toolchain version, or diagnosing a
packaging job that hangs.** It holds the hand-built Windows image and why a
Dockerfile change alone changes nothing; the required-plugin entry that must
come SECOND; Fedora withdrawing every Qt package the image pinned, and the
broken probe that said all 31 were gone; a download with no timeout, and why a
SLOW one looks identical to a stall; and the 0.9.0 AppImage that shipped
without Qt's TLS backend and without the Wayland shell integration, which
nothing could have caught because graceful fallback and silent absence are the
same observable.

**A ROOM OPEN COSTS THE HISTORY FILL'S PAGE COUNT TIMES ~400 MS, AND
`maxInvisibleFillRetries` IS THAT COUNT'S CEILING (measured 2026-09-05).**
`a5e64a6` raised the no-progress budget 8 -> 60; in a call room whose tail is
RTC membership churn that meant 9 fill pages on a first open (4.2 s) and 18 on
a re-open (11 s) — each page ~70 ms dispatch + network + 100-250 ms of ingest
and the fill's own timers. Reported as "ten seconds to load a room". It is 12
now, and a wheel towards older history on content too short to scroll asks for
the next page, so the reader is never stuck behind a collapsed group either.
The fill also decides how many rows a room holds after open, which IS the
scroll frame cost (~14 ms at 900 rows): a bigger budget makes scrolling worse
too. Detail in `docs/round-history.md`.

**PAGE DOUBLING WAS TRIED THE SAME DAY AND REFUTED BY THE SAME LOG.** Asking
for 100 events after an invisible page: Synapse answered a 100-event
/messages in 1.5-1.8 s (17 ms per event) against 110 ms for 20 (5.5 ms
per event), the fill overshot to ~600 rows, and the re-open went from 4 s
to 11 s. Do not re-propose bigger pages as the answer to the fill; fewer
pages is.

**MATRIXRTC MEMBERSHIP STATE IS FILTERED OUT OF EVERY TIMELINE AT THE SDK
(`lightning_event_filter`, rust/src/timeline.rs).** A call re-publishes one
`m.call.member` (msc3401 / msc4143 / stable) state event per participant per
MINUTE, so a room that hosts calls carries thousands; every one was a timeline
item — paginated twenty at a time, ingested as a hidden activity row,
instantiated as a delegate, counted by the fill. The maintainer's key
observation (2026-09-05): ONE room lagged and every other opened instantly.
Nothing on screen needs them (the "started a call" row is the notification
event; the call UI reads membership from room STATE). The controller's
no-progress budget is 12 empty pages (was 2) so the fill can walk a churn
run; the pane's row cap bounds what is inserted.

**THE FILL IS BOUNDED BY ROWS, NOT ONLY BY INVISIBLE PAGES
(`maxViewportFillRows`, 240).** The invisible-page budget cannot bound a room
whose every page adds a little height — a collapsed activity run growing by a
line per page, one visible message per twenty hidden ones — because each such
page counts as progress and resets it. Measured 2026-09-05: a re-open ran 32
pages and 600+ rows in 6.4 s with the page budget never tripping. Every row
is a delegate in the un-virtualized Column: that instantiation is the
"freezes for five seconds", and the same 600 rows are the one-second stall
when the row window releases them all at the live edge. 0.8.3 stopped at
eight pages, ~160 rows; the cap restores that scale and the reader's scroll
loads the rest a page at a time.

**THE FILL LOOP MUST FOLLOW THE CONTROLLER, NOT ITS RETRY TIMER.** Rows
land before `PaginationController` finishes a batch, so the fill check they
trigger finds `busy` true and waits on the 250 ms retry timer — once per
page. With every page served from the event cache in ~1 ms, that timer WAS
the room-open time (14 pages, 4.4 s, timestamped log 2026-09-05). The pane
re-checks the fill on the controller's `stateChanged` when it is idle.

**THE MEDIA BAND IS AN INDEX RANGE THE ROW LOADER ASSIGNS, exactly like
`rowOnScreen`, AND IT NEVER CLOSES ON THE NEWEST SIDE.** A picture loading
late in a row between the reader and the live edge grows below the reader
and moves them; rows older than the reader grow away from them. Only history
beyond 2.5 viewports waits. Its first cut compared the delegate's own `y` against
content-coordinate bounds — and inside the per-row Loader a delegate's y is
always 0, so every row was "in band" at the newest end (no saving) and no row
was deep in history (no picture ever loaded there). The pane computes
`mediaBandFirstRow/LastRow` with `viewRowAtContentY` at discrete moments and
the Loader sets `mediaInBand` on the delegate; the delegate's own default is
permissive for hosts without a band.

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

**ENCRYPTED VIDEO SEND WORKS, AND THE NOTE SAYING IT DOES NOT IS STALE — live
tested 2026-09-16.** The `rtpvp8pay` entry below is still true about
`rtpvp8pay`, and it stopped being true about LIGHTNING when
`src/calls/RtpVp8Payloader.cpp` landed: this client ships its own non-parsing
VP8 payloader. Measured, laptop, encrypted room, shipped 0.9.7 AppImage:
`screen share publishing ... encrypted= true` on the sender and
`frames decrypted stream= ... video= true count= 500 dropped= 0` at the far
end, both Lightning-to-Lightning and Lightning-to-Sable, and Sable-to-Lightning
in reverse. Do not repeat "encrypted video send does not carry"; it cost this
release's notes a false limitation.

**A plugin an ELEMENT loads for itself is invisible to every check we have.**
Windows shipped for months able to SEND audio and unable to RECEIVE anything,
because `libgstsctp.dll` was never staged: LiveKit's subscriber offer bundles
every section onto a data channel's transport, and without sctp webrtcbin
cannot build it. Sending still worked (our publisher offer is media-only), and
that one-directional shape is what disguised it. The SDP is byte-identical
with and without the plugin, so only running the SHIPPED GStreamer answers the
question — a MinGW webrtcbin harness under Wine on the builder image.
GENERALISE: when a feature is assembled at package time, the check must ask
the shipped artifact whether the feature WORKS, and the required-element list
must include what the elements LOAD, not only what we call. Full account in
`docs/round-history.md`.

**GStreamer version differences, same trap, different library.** The dev
shell is **1.26.11** again (MEASURED 2026-09-08 via `--call-media-status`; it
read 1.28.6 on 2026-08-31, so the flake has moved BACK — the lesson is the
one this sentence has always carried: measure it, never quote it); packaged Windows is **1.28.5** (upstream
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

**Four CTest suites are LOAD-SENSITIVE and will flake a full run.**
`timeline-pane-qml`, `timeline-hydration-qml` and `media-bridge` all pass
alone and fail intermittently; `message-html` joined them on 2026-09-10, when
the quadratic-scan fix added two WALL-CLOCK assertions (complexity is the
property under test and there is no branch to assert on; the headroom is 24x
and documented in-source).

**AND IT IS NOT ONLY `-j14`/`-j18`.** Measured 2026-09-10 with a game running:
`timeline-pane-qml` failed at `-j4` and again at `-j2`, in BOTH trees, on two
DIFFERENT cases — and passed alone at 229-230 s each time. Different cases
failing on different runs is the signature of timing, not of a defect; a
regression fails the same case every time. Before reading one as a regression,
check `ps -eo pcpu,comm --sort=-pcpu | head` and see
[[timeline-neartop-anchor-flake]] for the three-grep proof that beats a rate
comparison. Measured 2026-08-27 on a diff that touched none of them: `timeline-pane-qml` failed a full `-j14` run and failed
once more when re-run alone, then passed three isolated runs in a row; a
`build-rust` run at `-j14` that failed BOTH timeline suites passed each of
them alone and then went 157/157 at `-j8`.
The usual offender is
`topEdgePrependKeepsReaderOnTheSameRowMidGesture`, which is an ANCHOR case —
so before reading a failure as a scroll regression, re-run it alone and at
lower parallelism. `docs/timeline-scrolling.md` is explicit that a fourth anchor fix
needs a `LIGHTNING_SCROLL_TRACE=1` capture naming a failure, and a flake is
not that capture. **The anchor flake itself is FIXED as of 2026-09-11 and was
the FIXTURE** — see `docs/timeline-scrolling.md` — so a failure of those three is now
news. Their failure text carries the offsets, the live anchor counters, and
which of three things went wrong: the row was never built, the wrong row was
measured, or the reader moved.

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
**AND `gesturePolicy: WithinBounds` IS WHAT CLOSES IT.** On the default
`DragThreshold` a TapHandler takes only a PASSIVE grab and the ancestor's
handler fires on the same press; `WithinBounds` takes the exclusive grab and
it does not. The image viewer's thumbnail strip shipped without it and a
click on a thumbnail closed the viewer, while `imageTap` two hundred lines
above — which has always asked for it — zooms the picture without closing.
**This block said the opposite for a few hours**, on a measurement taken
through a fixture whose target was `visible: opacity > 0` behind a 180ms
fade, so the click never reached the handler under test: the same failure,
a different cause. Make the fixture prove it can HIT the thing before
concluding anything about what the thing does.

**THREE WAYS A VALUE CAN BE SILENTLY ABSENT, all on 2026-09-18.** A BINDING
THAT REACHES STATE THROUGH A FUNCTION CALL IS NOT BOUND TO IT: a rail leaf's
chevron opened and revealed no rooms because `revealed` asked a Q_INVOKABLE
for the expansion state — reported as a room that "did not appear until a
restart", and the sync hypothesis written for it was tested and REFUTED. A
PUSHED RECORD IS ONLY AS COMPLETE AS THE CALLERS THAT REMEMBERED TO PUSH: a
call in an unencrypted room carried audio ONE WAY because
`RtcController::roomEncrypted()` read a map whose two writers both required
the room to be OPEN, and the incoming-call card opens none. Both are PULLED
now. And **AN UNDECLARED PROPERTY ON A DELEGATE IS `undefined`, AND QML SAYS
NOTHING**: the rail's group field read two model roles it had never declared
as `required property`, so `!undefined` drew every run square at both ends
and the gap between groups was never added — invisible in a capture, caught
only by a geometric case comparing derived row tops against the delegates.
Full account in `docs/round-history.md`, 2026-09-18.

**TWO NUMBERS THAT MUST MOVE TOGETHER, AND THE BINDINGS READ CORRECTLY IN
EVERY ONE.** Three defects in one 2026-09-18 round, all in the Spaces rail,
all invisible to a source scan: a seam moved a child's BAND down while the
tile drawn on it was still positioned from the ROW's top, so the tile was
drawn through the top edge of its own region; `Avatar.size` is the mask's
permille DENOMINATOR, so a 41px tile rendered with a mask baked from 48 came
out rounder than the band containing it while the `Rectangle` beneath it was
right; and a delegate height that `rowBand()` does not mirror is a mis-drop,
because that function is what a drag maps the pointer through. **The cure is
geometric and nothing else finds it**: assert that every child is inside the
thing that contains it, on real delegates, in x and in y. Same family as the
2026-09-15 right-rail collisions. Full account in `docs/round-history.md`,
2026-09-18 (evening).

**A SLICE-AND-SPLICE EDIT REMOVES WHAT IS BETWEEN THE BOUNDARIES, NOT WHAT
YOU MEANT.** One such edit deleted a `Column`'s `visible`, `y`, `width` and
`spacing` along with the block above them, and produced THREE live failures
that each looked like a different bug — no Space revealed any room anywhere, a
row height the drag arithmetic did not share, and a drop landing a row and a
half from the pointer. A full CTest run stayed green because the mock reveals
no rooms, so that Column is empty there and the divergence is exactly zero.
**Diff what the edit actually removed.**

**A RAIL IS CHROME AND MUST RECEDE, AND A TINT LADDER WITH NO CEILING WILL
NOT.** Derived from `text` at escalating alpha it reached L*62 in a theme
whose base is L*6 — 5.25:1 brighter than the room-list column beside it, where
every comparable product makes the leftmost rail the DARKEST surface. A light
slab behind a run of tiles also reads as SELECTION, and one rung landed within
0.2 L* of the chat pane's selected row. **Capping the ladder's internal ratio
is not the same as moving its anchor**, and the first fix did only the first.
Discord's ENTIRE three-plane chrome spans 9.46 ΔL*; that is the budget.

**A CHANGE HANDLER ON A LOCAL BINDING RUNS INSIDE A STRANGER'S EVALUATION.**
`HomePane` logged `Binding loop detected for property "displayName"` on every
launch. A QML binding is evaluated LAZILY, on its first READ — and
`activeUserId`'s first read happened INSIDE `displayName`'s own evaluation
(which reads it for the localpart fallback), so the id moved from "" to the
real one mid-binding, `onActiveUserIdChanged` ran synchronously, and it wrote
`activeAccount`, a dependency `displayName` had already captured. Qt abandons
an evaluation whose dependencies move under it. The rail's account tile and the
Settings identity card were always right: drive the refresh from `Connections`
on the MANAGER's own signals and read `app.accounts.activeUserId` directly,
with no local binding in between. A binding loop is a LOAD-TIME fact no source
scan can see — the component loads, the root is non-null, and the property
keeps whatever the aborted pass left — so the gate is in
`QmlComponentLoadTest`, which asserts every listed component loads loop-free.

**SOLVE FOR THE SEQUENCE THAT IS PAINTED, NOT THE ONE THE DATA HOLDS.** The
rail's region ladder was re-solved for an even step per rung and made the
reported problem WORSE: rung 0 is the FOLDER container and hierarchy regions
index from 1, so the chain a reader sees is `rail -> r1 -> r2 -> r3` and the
solve covered a ladder whose first link is never drawn — the loudest boundary
got 53% louder and every inner one ~20% fainter. Caught by computing the drawn
boundaries in review, not by reading the numbers. Related and equally
expensive: **equal alpha steps are not equal LIGHTNESS steps**, and one
hand-picked list cannot serve a light rail and a dark one.

**AND THE SUITE THAT CERTIFIED ELEVEN THEMES WAS MEASURING ONE.**
`theRegionLadderIsEvenOnEveryTheme` set `settings.theme` 1..11, but
`AppTheme.mode` is written only by a `Binding` in `Main.qml`, which that suite
never loads — so eleven iterations read ONE palette while `themesChecked`
counted to eleven and the case passed. It writes `mode` on the singleton now
and asserts the palettes are DISTINCT. Assert the COUNT of what actually
varied, never the count of loop iterations.

**A THING DRAWN OUTSIDE ITS OWN BOUNDS MAKES EVERY NEIGHBOUR'S BUDGET WRONG.**
Reported as a "clipped" chevron: nothing was clipped — the active ring is drawn
OUTSIDE its tile, so a selected tile's visible edge is not `tileColumnX`, and
the expander's gap was measured to the tile. They shared pixels, and only on
the tile the user had just clicked, so every audit capture looked fine. Two
siblings carried the same literal. Corollary, from the same round: **place the
INK, never the box** — `expand_more`'s ink is 12x6 device px and
`chevron_right`'s is 7x12, transposes of each other, so one plate around both
reads as two different boxes unless it is square.

Full account in `docs/round-history.md`, 2026-09-19 (night).

**A KEEP-ALIVE SLOWER THAN THE EXPIRY IT EXISTS TO BEAT IS NOT A KEEP-ALIVE.**
Lightning published presence every FOUR MINUTES on the strength of a comment
reading "servers expire presence after a few minutes without activity".
Measured from a second account against this project's own Synapse, the window
is **33 to 63 seconds** (`SYNC_ONLINE_TIMEOUT` plus activity granularity), so
the account read OFFLINE to everyone else for about three quarters of every
live, continuously syncing session. **AND THE PUT IS THE ONLY LEVER**: a client
normally stays online because its `/sync` carries `set_presence`, and
simplified sliding sync has no such parameter — `set_presence::v3` in
`rust/src/presence.rs` is the only call that touches presence at all. It is
25 s now, and JITTERED to 21-25 s, because every session on one account
publishes on the same period and Synapse's `rc_presence` burst is 1: measured,
3 of 38 PUTs came back 429 with four clients open, on the steady keep-alive and
not at session start. Generalise both halves — measure a refresh against the
server's real expiry rather than against a comment about it, and a FIXED period
makes a user's devices collide for ever rather than once.

**A FIX WRITTEN FROM RE-READING AN INVARIANT IS NOT A FIX FOR THE PHOTOGRAPH.**
A square corner was reported in the rail from a screenshot. The first answer
rounded the cap backdrop, on a sound reading of a comment that contradicted the
element's own visibility condition — and its own commit message said outright
that it came from the invariant and NOT from reproducing the report. It shipped
and regressed: that rectangle exists to be the PARENT's colour in the notch a
child's rounded corner opens at a cap, so a radius rounds it away from the
notch and the GRANDPARENT shows through instead, a rung too light under a hard
full-width edge (measured at the same junction on a Windows guest before and
after, and the notch unfilled again on a Linux sweep). The real cause was
`folderLast`, stamped by the store over a folder's TOP-LEVEL members only with
every nested row hard-coded false — one wrong flag that the view read three
ways. §18 already says to instrument rather than guess; this is the cost of
not: a fix whose own justification admits it never reproduced the report is a
hypothesis, and shipping it buys a regression plus the round that undoes it.

**A PROBE DELIVERED THROUGH THE CHANNEL IT IS TESTING ANSWERS NOTHING.** The
`lt-windows` job runner stopped producing output, and four diagnostic jobs were
written to decide whether the SMB share had gone read-only or the guest's disk
had filled — `net use`, `fsutil volume diskfree`, five separate write tests,
`ren`/`md`. All four were delivered as `job.bat` THROUGH the runner, so all
four produced the same nothing and none of them said anything about SMB; it was
settled by typing into the guest's Run box over RDP instead. The fault: a job
launched `Lightning.exe` without `start ""`, so the GUI app INHERITED the
runner's redirected stdout handle and kept running, and **a failed redirect
makes `cmd` SKIP the command entirely** — `j.bat` never ran while
`echo %RC% > job.done` still worked, so every job looked like it had run and
returned an rc with no output at all. Sibling of "a probe is evidence only if
it shares the property under test": it must NOT share the fault.

**AN IMPERATIVE WRITE TO A BOUND PROPERTY IS A ONE-WAY DOOR.** Assigning to a
property that carries a binding destroys that binding for the life of the
object and nothing warns. Five media-cache handlers assigned `Image.source`, so
the first image that arrived froze the row; the follow checkbox and the rail's
saved width went the same way on 2026-09-18; and the theme editor's audit
throttle assigned the result it was meant to publish, so the import notice
counted the PREVIOUS theme. Put the binding back, or drive the value from a
signal and never assign it.

**A COLLAPSED ROW SETS `active: false`, NEVER `visible: false`.** Every media
fetch in `MessageDelegate` lives inside a Loader's component, so an attachment
that is merely HIDDEN is still instantiated and still fetches, decodes and
prefetches — identical on screen, with the whole point of the setting lost. One
`collapsed-embeds-qml` case exists purely to tell those two implementations
apart.

**A GATE THAT CANNOT TELL "NOTHING YET" FROM "NOTHING THERE" PAYS FOR BOTH.**
`read_membership_events` escalated to a full `/state` whenever the store held
no LIVE membership — and an EMPTY store failed that test identically to a stale
one. So besides the ~60 MatrixRTC pokes that collapsed into TEN `/state` calls
on initial sync, every room the user OPENED paid for one too, through
`setCurrentRoomId`, under a comment reading "a read is cheap (state store, no
request)". That half appears in no poke trace, which is why a round spent
reading the poke path could not find it. The escalation now needs a positive
reason to believe a session exists, and the log says `store-no-session` versus
`store-cooling-*`, because otherwise "we did not need to ask" and "we were not
allowed to ask" are the same observable.

**A `ctest` KILLED MID-LOOP LEAVES ITS SETTINGS ON DISK.** `ComposerQmlTest`
shares one `QSettings` file across every case, and its hidden-button case
measures first, restores, then asserts — which defends against a FAILING
ASSERTION and not against the process being killed. A killed run left a
composer button hidden on disk and three cases failed the NEXT run on a tree
that was fine. Read a `composer-qml` failure against that file before reading
it as a regression.

Full account in `docs/round-history.md`, 2026-09-19 — presence, the RTC
`/state` storm, collapsed embeds, the rail's chroma ladder, self badge and
`folderLast`, and the theme editor's readability audit.

**`std::clamp` IS FOR QUANTITIES; AN ENUM NEEDS A FALLBACK.** The rail's new
depth-style setting clamped its stored value to `[0, 1]`, and
`std::clamp(2, 0, 1)` is **1** — so a value written by a NEWER build with a
third style would have landed this one on the LAST style rather than the
default, silently switching a returning beta user's rail to a look they never
chose, as far from their real choice as the range allows. A width clamps
because 4000 and 260 are the same intent at different magnitudes; a style name
has no such ordering. Caught by the test written for it, before it shipped.

**A SETTING THAT REMOVES ROWS IS A MODEL STATE, NOT A PAINT STATE.** The
Classic rail (`RailEntryModel::setFlat`) drops the hierarchy walk so a
subspace is not in the row list at all. Hiding those rows in QML instead would
have left them in the list the drag arithmetic, the group bands and every drop
target index into — all of them measuring rows nobody can see. Same family as
the slice-and-splice lesson: what bites is never the pixels, it is everything
that counts rows. Corollary: a `Repeater` whose `model` goes to 0 instantiates
nothing, where a `visible` binding on each delegate builds them all and hides
them.

**A POSITION FLOORS, A TOTAL ROUNDS, AND SHARING ONE FORMATTER IS WHY THEY
COULD NEVER AGREE.** At 25.7 s you have not reached 0:26, so an elapsed clock
floors; 25.7 s of audio IS 26 seconds to the nearest second, so a length
rounds. Every player in this tree formatted both with one function, and
"fixing" it by rounding both broke the position clock (it reaches the total
before the audio ends). `formatPosition` / `formatDuration`, and the two
RECORDING counters stay floored because a counter running while you speak is a
position. **The fix that caused this cited "the video card next door already
rounds" — it floors, and the function quoted had NO CALLER ANYWHERE.** When
two surfaces disagree and one is "obviously" right, check that the one you are
copying does what you think.

**A DOC COMMENT SPLICED INTO ANOTHER ONE STEALS ITS SUMMARY LINE, AND IT
COMPILES.** Inserting a helper's `///` block after `send_sticker`'s summary
left the HELPER documented as the sending function and `send_sticker` with no
summary at all — two wrong doc comments from one insertion, both rendered by
`cargo doc`, nothing failing anywhere.

**PIN EVERY INPUT A CROSS-PLATFORM COMPARISON DOES NOT MEAN TO TEST.** A
Windows-vs-Linux rail capture was taken with the THEME unpinned: the guest had
no `[ui]` section and rendered light where Linux rendered dark. That would have
been a confident false FAIL about a renderer difference that does not exist.
With `theme=2` pinned both sides the rails are pixel-identical — 0 differing
pixels across the band columns, 2 pixels at 1/255 in one channel at the edge.

Full account in `docs/round-history.md`, 2026-09-19 (evening).

**A TEST THAT ASSERTS A CONSEQUENCE CANNOT SEE PAST ANYTHING ELSE THAT
PRODUCES IT — three vacuous assertions in one commit, found by review.** The
retry chain's bound was asserted by COUNTING PUBLISHES, and
`m_publishRetryTimer` is one single-shot timer whose `start()` RESTARTS it:
at most one retry is ever pending however many rejections arrive, so the
publish count in any window is identical with the cap, without the cap, and
without the backoff. Two other assertions in the same case could not fail
either — one checked state with NO event-loop iteration between the emit and
the check, so the timer could not have fired for any interval including
zero; the other waited 1200 ms where the broken path arms 4000. Ask what
ELSE produces the consequence you are asserting, and assert the state.

**AND THE SEAM THAT FIXES ALL THREE HAD BEEN WRITTEN AND NEVER CALLED**, in
the same commit that made them vacuous, with a comment describing a different
member ("bounded in milliseconds" on something returning a count). Fifth
occurrence of this shape: `refreshIndexStats()`, `fetch_details_for_event`,
the unregistered `ShortcutRegistryTest.cpp`, `sfuTrackPublished`, now this.

**A PROBE IS ONLY AS GOOD AS ITS RESEMBLANCE, AND THE MOCK IS WHERE IT WILL
AGREE.** The account switcher sized its list from an off-layout probe of
`IdentityCard` times the row count — the workaround for a real deadlock
(`contentHeight` is 0 until delegates exist; delegates need a nonzero
viewport) — and the probe omitted the trust meter the ACTIVE card draws: 136
px against a real 159, **23 px short at every count including one**, which is
the reported "it scrolls with one account". Invisible in the harness because
the mock reports no crypto state, so probe and card agree there exactly. The
cure is not a better probe: the menu owns ONE row height and hands it to
every delegate, so content and viewport are the same arithmetic and no
property added later can make them disagree.

**`rc_presence` IS PER USER, SO JITTER CANNOT FIX AN AGGREGATE.** Spreading a
user's devices apart stops them colliding and does nothing at all once their
TOTAL offered rate is over the limit. Measured live: 62% of publishes
rejected, a run of 29 consecutive rejections, **eleven minutes of an account
reading offline while its process was healthy** — with a keep-alive that was
working perfectly and publishing into a wall. A rejected publish now retries
on the server's own `retry_after_ms`, bypassing `kMinPublishGapMs`, which
exists to suppress a duplicate of an ACCEPTED state and has nothing to
suppress after a rejection. **NOT fully explained and not live-confirmed
fixed**: one device offers 0.04 PUT/s against a 0.1/s limit and four offer
0.16, predicting ~37% and not 62% — that needs six or seven publishers, or
ONE device whose connection is flapping, because the Syncing-edge publish is
gap-limited to one per 10 s, exactly the limiter's rate. Those need opposite
fixes. `publishAttempts()` and the `LIGHTNING_PRESENCE_TRACE`
`presence-publish` line exist to settle it with the OFFERED RATE; a healthy
single client measures ~2.6 attempts/min.

**AN INK DESIGNED TO BE UNREADABLE MUST NOT CARRY A DISAMBIGUATOR.** The
account switcher's inactive MXID used `stormTextFaint`, which falls back to
`textDisabled` outside Storm — the ONE role the readability table refuses to
grade, because it is supposed to be low contrast. **1.60:1** on the light
popover, on the only line separating two accounts that share a display name.

**A STATUS LINE THAT ALWAYS SAYS "FINE" CANNOT SAY "NOT FINE".** The
switcher's strip read "Connected · 1 space(s)" and was reported as
unreadable. The space count was trivia — but "Idle" is this app's word for
DISCONNECTED-WHILE-LOGGED-IN, so the same line was reporting a real fault in
a word that reads as benign, next to a number that reads as noise. It speaks
only when the connection is unhealthy now. Same family as the readability
badge that fires on a stock theme.

**`localization` GATES EVERY `qsTr` AND PROPORTIONAL VALIDATION WILL MISS
IT.** It stayed red for a whole day across three commits whose entire content
was new user-visible strings, because each ran "the affected suites" and none
of those was this one. Run it after any commit that adds or removes a `qsTr`.
Resync with `cmake --build <tree> --target update-translations` — a
project-wide generator, so ONLY on a clean tree and in its own commit (§4).

Full account in `docs/round-history.md`, 2026-09-19 (late).

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
period`, saturating both ways. **LIVE-VALIDATED PASS 2026-09-13**: a
two-party call between the packaged flatpak and an AppImage held 19m30s,
~4x the window, both clients still reporting two participants and both
clear-frame receive counters climbing at the end. See the live-validation
entry below.

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

**THE PROCESS IS A `QApplication`, NOT A `QGuiApplication` (2026-09-05), AND
IT MUST STAY ONE.** `QSystemTrayIcon` on X11 has two backends: a
StatusNotifierItem over D-Bus when a watcher is on the session bus, and the
legacy XEmbed icon otherwise — and the XEmbed one is a `QWidget`. A NixOS user
on 0.9.0 enabled "keep running in the tray" under a bar that speaks XEmbed
only (i3bar, polybar, tint2 …) and the process aborted with `QWidget: Cannot
create a QWidget without QApplication`. No local desktop can show it: KDE and
GNOME-with-AppIndicator both have a watcher. `QtWidgets` was linked all along
for the tray; a QML application under `QApplication` renders identically.

**MENTION PILLS: THE ROOM'S MEMBER NAME, THEN A GLOBAL PROFILE, AND THE
LOCALPART LAST — NEVER THE SENDER'S ANCHOR TEXT.** `MessageHtml::sanitize`
replaces the anchor text of every `matrix.to` user link and used to fall
straight to the localpart when the member snapshot had nothing — so a
completed "@dim" was sent correctly and rendered as "@obscurus" on the
reader's own screen, and the pill's profile card opened with no name and no
picture (`openMessageLink` passed empty strings). Now the
`UserProfileResolver` (`app.userProfiles`) asks `/profile` once per unknown
user per session for the pill AND the popover, and each cached render records
the names it used (`m_htmlMemberDeps`) so an answer re-renders only the rows
that used it. Honouring the label the sender wrote was tried in the same
round and REFUSED: "@admin" linking to `@attacker:evil` must read as
`@attacker`, which is what the localpart fallback guarantees and why Element
ignores the anchor text too.

**MEMBER HYDRATION NO LONGER RE-RENDERS THE WHOLE ROOM.** `onMembersChanged`
used to rebuild every message body a second time, on the GUI thread, seconds
after each room's first open (the `/members` fetch lands then). It re-resolves
the recorded name pairs and forgets only the rows whose answer moved. LIVE
EFFECT ON THE ROOM-LOAD LAG: **NOT TESTED**.

**MEDIA ROWS FETCH ONLY NEAR THE VIEWPORT.** Every row is instantiated, so
every image in the loaded history used to ask the bridge for its payload at
room open — and in an encrypted room without server thumbnails, the FULL
files. `TimelinePane` publishes `mediaBandMinY/MaxY` (2.5 viewports towards
the newest end, 1.5 the other way) moved ONLY at load, reset, resize and the
settle after a gesture — never bound to `contentY`, which would re-run a
comparison in every row on every frame — and `MessageDelegate.mediaInBand`
gates `refreshBridgeSource()`. Thumbnails included. NOT live-measured.

**A RESTORE MUST NOT NEED A LIVE SERVER, AND UNTIL 2026-09-14 IT DID.**
`Client::builder().server_name_or_homeserver_url()` — the method issue #5's
delegation fix introduced, and the right one for a field a HUMAN typed —
performs well-known discovery AND a homeserver verification over HTTP. Every
restore went through it, so a homeserver that went down put the user on the
LOGIN PAGE with a complete local store on disk. `restore_session()` itself only
reads the store. `build_client_for_restore()` now tries discovery first
(bounded, so a delegation change is still followed) and falls back to the URL
the last successful build recorded beside the store. **A typed string can NEVER
be promoted to a homeserver URL by inspection**: `https://matrix.org` is a
server NAME whose client API is `https://matrix-client.matrix.org`, so the
obvious shortcut 404s the largest homeserver there is. Only a URL the SDK
itself resolved may be used. Detail in `docs/round-history.md`, 2026-09-14.

**THE TIMELINE ROW'S RIGHT RAIL IS SHARED, AND NOTHING ARBITRATED IT.** The
hover action bar anchors to the row's top-right; the read-receipt facepile
paints UPWARD from the row's bottom edge at the same margin. On a TALL row
they never meet, which is why it went unnoticed; on a SHORT one they are the
same pixels, and the facepile WINS (equal z, later in the document), so the
Edit and overflow buttons could not be pressed at all. Reported 2026-09-14,
reproduced live, measured at 48px of overlap. Auditing the three layouts for
it found three more of the same family, all in code whose comments claim the
collision is handled: **in Bubbles the sender header rendered OUTSIDE its
bubble** (its cap was `bubble.width - 112`, and the bubble is SIZED FROM the
column the header is in — Qt resolves that loop by pinning the header to ONE
PIXEL of contributed width; derive from `bubbleRow` instead, the escape
`segmentCap` already uses), **the facepile clipped an own bubble's corner**
(the width cap reserves a 40px rail that the PLACEMENT ignored), and **local
search claimed "Nothing is indexed yet." over results it had just returned**
because `refreshIndexStats()` is a `Q_INVOKABLE` with NO CALLER anywhere in
the tree. **GENERALISE: two things anchored to one edge from opposite ends of
a row is a collision waiting for a short row, and a geometric assertion is the
only thing that can see it — every one of these passed a source scan.**
Detail in `docs/round-history.md`, 2026-09-15.

**A LOG LINE THAT CANNOT TELL "NOTHING HAPPENED" FROM "WE THREW EVERYTHING
AWAY" IS NOT A LOG LINE.** A room open made fourteen back-paginations that each
reported `added= 0`, and nothing anywhere could say whether the server returned
nothing or the timeline filter had discarded a full page — `paginate_backwards`
returns a bare `bool` and matrix-sdk-ui drops `BackPaginationOutcome.events` on
the line that tests it. Lightning's own filter is the only place that sees
every raw event AND knows why it said no; it counts now, and one instrumented
run answered it outright (`filterOffered= 240 droppedRtc= 240` — twelve pages,
100% MatrixRTC churn). **Four handling defects hid behind that silence**: a
room asserted its own emptiness after ONE empty page
(`m_initialHistoryHasSucceeded` had no `inserted > 0` test, so
`timelineEmptyState` rendered "No messages here yet" over full history); the
fill gave up at 8 because a page that adds NO rows spends
`maxViewportFillRetries`, never the `maxInvisibleFillRetries` the 2026-09-05
round raised for exactly this case; every empty page paid a 250 ms settle for
rows that could not arrive; and `requestNearTop()`'s redirect swallowed the
user's gesture once the fill had stopped. Detail in `docs/round-history.md`,
2026-09-15 (afternoon).

**AND A PAGE-SIZE ESCALATION IS NEARLY INERT — do not record it as the fix.**
`matrix-sdk`'s `load_more_events_backwards` returns ONE STORED CHUNK at a time
and never consults `batch_size`; that parameter only reaches the wire when the
walk hits a network gap. A filtered run is therefore local disk reads, which is
why the 250 ms settle dominated and not the fetch. (It is also NOT the page
doubling §16 refutes: that measured unconditional 100-event pages on rooms
whose pages ADD ROWS, and both harms it found need rows.) Related: the
sliding-sync room list runs at `DEFAULT_LIST_TIMELINE_LIMIT = 1` and any
`limited` response shrinks a room's cache to its last chunk, so `items= 0`
versus a healthy room's `items= 2` (one event plus its date divider) is that
residue, not an empty room.

**AND THE BOUND THAT REPLACED THAT SILENCE WAS WRONG BY EXACTLY ONE MESSAGE
(2026-09-16).** The fix above keyed "keep walking a filtered run" on
`eventCount() == 0`. That is a PROXY for the reader's actual condition — *is
the viewport full?* — and the maintainer's next report was the same room one
message later: "in this room only a single image loads and I have to scroll up
for anything else to appear." One loaded image made `eventCount()` non-zero, so
the room got the ordinary twelve. **A bound keyed on a proxy for the user's
condition is wrong by exactly the difference between them**; when the honest
criterion cannot be read where the decision is made, derive it and say so
rather than taking the nearest readable thing.

**The terminator was in QML, and no previous round had it in frame.** The log
showed NINE dispatches and stopped; neither controller bound can produce a nine
(both are twelve), and `TimelinePane.qml`'s `maxViewportFillRetries` is EIGHT,
plus one from `requestNearTop()`'s redirect, which does not spend the pane's
counter. That counter's real subject is "the dispatch went nowhere" — and a
page the filter emptied looks identical to it from QML (zero rows, zero pixels)
while meaning the opposite: the cursor walked twenty real events towards the
first message beyond the churn. **Two observations that are identical at the
point of measurement are not one event.** Filtered pages now spend their own
budget (`viewportFillEmptyPages` / 60, matching `kMaxFilteredRunStrikes`),
which is affordable precisely because a page that inserts nothing instantiates
no delegates; `maxViewportFillRows` (240) still bounds everything the fill puts
on screen.

**AND THE MOCK COULD NOT EXPRESS A FILTERED PAGE AT ALL, WHICH IS WHY IT
SHIPPED TWICE.** `setPaginationChunkForTest({})` falls through to three
synthetic events (`if (!m_paginationChunkOverride.isEmpty())`), so EVERY mock
page had always added rows — the one pagination shape that matters most here
had no reachable fixture at the QML layer. `setFilteredPaginationPagesForTest`
is that fixture now. GENERALISE: before concluding a defect is untestable,
check whether the harness can even REPRESENT the input. Detail in
`docs/round-history.md`, 2026-09-16.

**`Timeline::fetch_details_for_event` HAD NEVER BEEN CALLED IN THIS
REPOSITORY.** `InReplyToDetails::event` is a field on the REPLYING event, not a
lookup into the loaded timeline, and it starts `Unavailable` — so a reply quote
read "(original message not loaded)" for ever unless the homeserver happened to
bundle the target. Reported against a message three rows above, on screen;
being on screen was never relevant. Same family as `refreshIndexStats()` and
the unregistered test file: code that exists, looks right, and is never
reached — **grep for the caller, not just the definition.**

**A STATE SET IMMEDIATELY BEFORE EMITTING A SIGNAL IS NOT A STATE THE USER
SEES.** Qt's default connection on one thread is DIRECT, so the whole
downstream chain runs before the setter returns and anything in it that writes
the same field wins. `setState(Offline)` at `login_ok` was overwritten by
`loginSucceeded` -> `AuthManager` -> `AppController::onLoginSucceeded` ->
`startSync()` -> `setState(Syncing)`, with no event-loop iteration anywhere in
between; the offline-restore label was never rendered and the claim had
already been written into this file as fact. Put the rule where the field is
written, or prove no handler downstream touches it. **Same mechanism, second
costume:** `publishShareAudio()` emits `failed()` synchronously, so
`onEngineFailed` re-enters `startScreenShare()` from INSIDE that call — before
`m_shareAudioCid` is assigned — so a cleanup branch keyed on that cid was a
no-op on the exact failure it was written for. Record the id BEFORE the call
that can fail.

**A TEST THAT COMPOSES SOMETHING *RESEMBLING* WHAT PRODUCTION COMPOSES PROVES
NOTHING — third occurrence, and this one shipped a feature that had never
worked.** `publishShareAudio()` built `"%1 name=sharesrc ! queue ! …"`, which
is valid only while `%1` is a single element; `mixedSourceDescription()` ends
in a PAD REFERENCE and GStreamer takes no assignment after one, so every
per-application share died at parse time with `unexpected reference
"shareaudiomix"` and the echo fix could never have run on any PipeWire desktop.
The test appended `" ! fakesink"` instead and passed. The composition is one
function now (`shareaudio::encodedTrackDescription`) and the test parses what
the engine actually hands GStreamer. Same round: **an optional track's failure
must never end the call** — `onEngineFailed` tore the session down for
`share_audio_failed`, which is how a missing feature became a lost call.

**`gst_device_monitor_start()` IS SYNCHRONOUS AND UNBOUNDED, AND IT WAS ON THE
GUI THREAD IN THE CALL-JOIN PATH.** Each provider decides when it has an
answer; PulseAudio's connects to the sound server and waits on its own mainloop
for the initial device list with no timeout anywhere. Added in 0.9.4 with the
device-preference work (`7e6bcb6`) — 0.9.3 contains neither `monitorCandidates`
nor that call — which is exactly the boundary GitHub issue #12 names ("joining
a call freezes it; 0.9.3 is fine"). NOT reproduced here. It is now skipped
entirely when no device preference exists (arguments are evaluated before
`chooseCaptureElement` can return early for an empty id, so every join paid for
it) and bounded at 2.5 s with a per-klass latch otherwise. MEASURED on a
healthy PipeWire desktop: 218 ms first `Audio/Source`, 8 ms after, 4 ms
`Video/Source`. **THERE ARE TWO SUCH CALLS IN THAT PATH AND THE OTHER IS
OLDER**: `perApplicationCaptureAvailable()`'s monitor installs NO FILTER (see
its comment — a provider filter matches none), so it starts EVERY provider on
the machine, and it is read from two `CONSTANT` properties the call header's
share menu binds when `groupCall.active` flips true. Present unchanged in
0.9.3, so it is not issue #12's boundary; bounded the same way regardless.

**A JOB THAT EXISTS AND LOOKS RIGHT IS NOT A JOB THAT HAS RUN — three in one
night, 2026-09-16.** `report-optional-assets` failed on its FIRST ever
execution, in the 0.9.6 release it was written to protect, on
`RELEASE_TAG: unbound variable`: it called `gitlab_api_init` and never
`release_contract_env`. It runs only in a PUBLISHING pipeline and no release
happened between the commit that added it and 0.9.6, so nothing could have
found out. The verification bar had been silently checking ONE LINK FEWER than
each release has, for every release ever, because `"\n".join(...)` writes no
trailing newline and `while read` drops an unterminated final line. And the
live-sync handler had never forwarded a media message at all, so an image sent
to a room with no timeline open produced no notification and no Activity row.
Same family as the unregistered test file, the ten-days-unbuildable Windows
Dockerfile, and `refreshIndexStats()` with no caller. **The cure is the same
every time: assert the COUNT, not just the items** — a check that can come back
silently short is the defect, not its symptom.

**SILENCE ENCRYPTS EXACTLY LIKE SPEECH, AND EVERY COUNTER IN THIS ENGINE SAID
THE CALL WAS HEALTHY FOR A WHOLE DAY (2026-09-16).** A call was reported
inaudible in one direction and the crypto path was searched first and
exhaustively — key indices, `targets=`, the adopt guard, resolved target
devices, the Olm identity compared byte-for-byte against the server, RED
wrapping, the to-device payload shape, room power levels, widget capabilities.
Four real defects were found and fixed on the way and **none of them was the
report**. `opusenc` turns a silent buffer into a real frame, the encrypt probe
authenticates it and the far end decrypts it, so `frames encrypted ... count=
1500 dropped= 0` with a green padlock and a connected transport is exactly what
a call carrying nothing looks like. The capture chain now carries a `level`
meter (`microphone level peak= N dBFS`, every 5 s, good news or bad), a
sustained-silence warning and a call-header badge.

**AND THAT COUNTER WAS MEASURED IN THE WRONG PLACE — it was quoted as proof of
transmission for six hours and it is not.** `frames encrypted` sits on the
ENCODER's src pad, upstream of the payloader, the capsfilter and webrtcbin. A
second counter now sits on the publishing bin's own src pad — `rtp packets
handed to webrtcbin`, the last point we own. **GENERALISE: a counter upstream
of the transport says what was PRODUCED, never what was SENT**, and the two are
one payloader apart. Three more instruments were missing beside it, and each
had made the fault unaskable: `sfuTrackPublished` (LiveKit's own answer to "did
you accept my track?") **had no consumer anywhere in the tree**, so a track
declared and never published looked identical to one carrying audio to
everyone; the mute VALVE had never been logged, so a muted capture and a
stalled one were the same silence in every log this client writes; and a
capture that produces nothing posts nothing, so a stall was invisible until a
watchdog was given three seconds and a warning.

**A MULTI-INPUT INTERFACE IS NOT A MICROPHONE, AND `channels=1` AVERAGES ITS
DEAD INPUTS INTO YOUR VOICE.** A Roland Rubix44 presents FOUR unpositioned
channels; the mic is on input 1. Asking the chain for mono propagated all the
way back to the source, so PipeWire averaged four channels before a sample
reached us: measured with the maintainer speaking, input 1 at **-21 dBFS**,
inputs 2/3/4 at -86/-67/-92, chain output **-33 dBFS** — 20*log10(1/4) to two
decimals. Fixed with a pinned channel-count capsfilter (`channel-mask=0`,
UNPOSITIONED — without it `audioconvert` refuses the graph with
`not-negotiated`) plus a mix-matrix taking input 1. **Stereo stays averaged on
purpose**: see the Windows one-live-channel mic above. Honouring a device
preference is itself a regression surface — before 0.9.4 every call used
`autoaudiosrc` and followed the system default, which is why "it worked before
0.9.0" was literally true.

**A DEFAULT `queue` HOLDS ONE SECOND AND NEVER LEAKS IT.** `max-size-time`
defaults to 1000000000 with `leaky=no`, so a live capture whose encoder falls
behind once fills it and the backlog is permanent latency for the rest of the
call. Reported as ~1 s Lightning->Element against ~0.2 s the other way, on the
same SFU over the same network — the asymmetry was ours and it matched the
queue's capacity. Any queue on a LIVE path needs an explicit bound and
`leaky=downstream`.

**A MEDIA KEY THAT ARRIVES BEFORE THE CALL IS ACTIVE IS NOT A KEY TO DISCARD.**
The peer already in the room sends its key the moment it sees our membership,
which can precede our own SFU session going active; `onMediaKeyReceived`
returned on `!active()` and nothing re-sends. Measured: THREE keys dropped per
call, and the badge that then said "media from someone here cannot be
decrypted" was reporting a fault we had caused ourselves. Parked and replayed
on join now, bounded and cleared on teardown, exactly as matrix-js-sdk does
with `keysWithoutMatchingRTCMembership`.

Full account in `docs/round-history.md`, 2026-09-16 (afternoon).

**A VALUE THAT LOOKS IMPOSSIBLE FOR THE SIGNAL MAY BE EXACTLY WHAT THE
INSTRUMENT EMITS AT ITS LIMIT.** A published Windows package logged
`microphone level peak= -350 dBFS`. 16-bit audio floors near -96, and -350 is
also the bus handler's own starting value, so it was read as "the parser
failed" and guarded out of the silence detector. Measured afterwards:
`gst-launch-1.0 audiotestsrc wave=silence ! level` posts
`peak=(GValueArray)< -349.99999992181608 >` — it is the ELEMENT'S floor for
digital silence, the handler starts there to match it, and the guard would
have made "your microphone is capturing nothing" unreachable for a genuinely
dead microphone while leaving it working for a quiet room. The suite already
said so in a comment and four fixtures were changed away from it instead.
Withdrawn before it shipped (`7a9fbef4`). **Ask the instrument before calling
its output a bug.**

**`git checkout --` TO UNDO A MUTATION TEST ALSO DISCARDS THE REAL WORK IN
THAT FILE.** A two-constant mutation was reverted that way and took a
just-written header with it. Mutate a COPY: `cp` the file aside, mutate, build,
run, `cp` it back. Never `git checkout` a file that also holds uncommitted
work — the mutation is one hunk and the restore is the whole file.

**A SOURCE SWEEP CAN MATCH YOUR OWN C++.** `everyLiveQueueIsBoundedAndLeaky`
looked for `\bqueue\s` and began reporting `queue = gst_bin_get_by_name(...)`
as an unbounded pipeline queue the moment the same file gained a variable of
that name. A sweep over source text has no idea what is a string literal; give
it something only the thing under test can satisfy — a pipeline queue is
followed by a pad separator or by one of its OWN properties.

**A FLATPAK-BUILDER RUNNING INSIDE `org.flatpak.Builder` NEEDS A SESSION BUS,
AND SAYS SOMETHING ELSE WHEN IT HAS NONE.** It resolves its sdk by running
`flatpak info` ON THE HOST through the spawn portal, so with no bus it dies at
init on `Unable to find sdk org.kde.Sdk version 6.11` — while
`flatpak info org.kde.Sdk//6.11` in the same shell prints the ref. Three
Flathub repo-lint attempts produced nothing on that. `dbus-run-session` is the
whole fix; D-Bus activates the portal itself. Detail in `docs/open-items.md`.

**AND `cmd | tail` MAKES `$?` THE STATUS OF `tail`.** It reported
`builder rc=0` over a build that had never started, three times. `PIPESTATUS`
is the command's own status.

**A WAIT LOOP WHOSE PATTERN MATCHES ITS OWN COMMAND LINE NEVER TERMINATES.**
`while pgrep -f "ninja|ctest"; do sleep; done` matches the bash process running
it, so it waits on itself forever; two background shells deadlocked this way in
one session. Use `pgrep -x ninja`. Same family as the recorded
`$(pgrep -c x || echo 0)` trap.

**A THEME-DEPENDENT CLAIM MEASURED IN ONE THEME IS A CLAIM ABOUT THAT THEME.**
"Ctrl+A doesn't work in the text fields" was reported, and I measured it
WORKING and said NOT REPRODUCED — in Storm, the one theme where the token
resolved to something visible. Selection worked everywhere; it was invisible
in the two DEFAULT themes, because `selectionColor` read `accentSoft`, a TILE
FILL that only three of eleven palettes define. Sibling of the settings cards
painting `stormCanvas`, which IS the page colour, on the same day: **the
semantically obvious token is often the broken one, and it renders correctly
on whichever theme you happen to test.** Both are gated now by
`ThemeTokensTest` cases that assert an L* floor across all eleven AND assert
the COUNT of palettes they checked.

**A BRIDGE THAT CARRIES TWO NEAR-SYNONYMS WILL HAVE THE UI READING THE WRONG
ONE.** matrix-sdk's `is_verified()` (locally trusted OR cross-signing trusted)
and `is_cross_signed_by_owner()` (merely SIGNED by the owner's key, with no
requirement that we trust that identity) both reach QML. The Sessions chip,
both filters and the account rollup key on the second and label it
"Verified" — wrong in BOTH directions, including a green badge for a device
matrix-sdk does not trust. `verified` is read in one line of the file. §6:
trust labels come from SDK state. NOT FIXED, §18 review pending.

Full account in `docs/round-history.md`, 2026-09-20 — also the ToolTip that is
centred on its anchor, a Qt border painting inside the bounds under an Avatar,
a parity bug in a derived tile size, a correct drop refusal delivered a whole
subtree away from the pointer, and a `qmlformat -i` that rewrites the file.


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

**MOVED: the full text is `docs/live-validation.md`. READ IT before claiming
any behaviour is confirmed working.** It was moved out on 2026-09-15 at
139,949 characters — the fifth move, for the reason all five happened, and it
went because §16 is a LESSON INDEX and a chronological record of confirmations
is an inventory. Nothing was deleted.

It holds every live PASS this project has, with what each one does and does
NOT cover: the offline restore and the layout audit (2026-09-15), the
still-window share and the snap's NSS gap (2026-09-13), the MatrixRTC
membership expiry and the packaged-flatpak sweep, the Windows camera and
renderer, calls and per-participant volume with their two withdrawn claims,
the send path, threads, the GPU share path across four environments, share
audio reaching Element, and the 2026-08-26 round that remains the largest
single confirmation event here.

### Open items and NOT TESTED inventory

**MOVED: the full text is `docs/open-items.md`. READ IT before claiming
anything is fixed or promoting anything to tested.** It was moved out on
2026-09-11 at 140,752 characters — the fourth move, for the reason all four
happened: past roughly 140,000 this file's own tail starts heading for the
150,000 cliff, where it truncates SILENTLY and §§17-19 vanish from agent
context. It was the section chosen because §16 says outright that it is a
LESSON INDEX and not an inventory, and an inventory is exactly what that block
was.

It holds every OPEN DEFECT reported live and not yet confirmed fixed (the
Windows camera's 10 fps ceiling, the account-switch freeze on a large account,
the screen share's variable startup, the `room_list` storm's outstanding live
confirmation, room-load lag, the share blur, full-screen on the wrong
monitor), the NOT TESTED inventory, the live-validated local-search and widget
results, the two open decisions those rounds created, and the accepted
follow-ups.

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
