# Release operations

Moved out of `CLAUDE.md` §2 on 2026-09-10, when that file stood at 146,262
characters against a hard 150,000 limit that truncates it SILENTLY — dropping
its own tail, §§17-19 (the completion-report requirements and the multi-agent
review protocol), out of every agent's context. That has now happened twice,
and §16 records the rule this move follows: past roughly 140,000 the answer is
a new file under `docs/` and a pointer, never a longer section. It is the same
move that took §7 to `docs/feature-contracts.md` on 2026-08-28 and §16's round
history to `docs/round-history.md` on 2026-09-03. Nothing was deleted; both
blocks are below unchanged.

READ THIS FILE before any release, packaging, or pipeline work. The POLICY —
what a release is allowed to do, the package-first flow, the trigger shape, the
anonymous verification bar — stays in `CLAUDE.md` §14. What lives here is the
inventory of what has shipped, and the operational traps that have cost real
pipelines.

### Release inventory (all tags immutable)

| Version | Commit | Deploy pipeline | Notes file |
|---|---|---|---|
| 0.9.4 | `bcea599` | **project 6** 186, **20/20 green first attempt** | `docs/releases/v0.9.4.md` |
| 0.9.3 | `7306dde` | **project 6** 183, 21/22 (the red one is the allow_failure manifest mirror, a CDN race, not the release) | `docs/releases/v0.9.3.md` |
| 0.9.2 | `2545391` | **project 6** 180, 22/22 (177/178/179 lost to runner memory; 176 to the migration's own path bug) | `docs/releases/v0.9.2.md` |
| 0.9.1 | `d2e343b` | 175, 22/22 (174 failed validate-appimage) | `docs/releases/v0.9.1.md` |
| 0.9.0 | `9dc6a07` | 173, 22/22 green (171 lost `build-windows` + macOS to the QtDBus guard, 172 `build-windows` to the builder image) | `docs/releases/v0.9.0.md` |
| 0.8.4 | `49249e8` | 170 | `docs/releases/v0.8.4.md` |
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

- **THE LOCAL PACKAGE RUNNERS CANNOT LINK THIS PROJECT, and the failures look
  random because both hosts carry identical tags.** Measured across pipelines
  177/178/179 on 2026-09-06: jobs that landed on the LOCAL package runners
  (ids 3,4,6,7,8) failed 6 times and succeeded 0; jobs on the REMOTE
  xcp-ng-1 runners (10-14) succeeded 3 of 4, and the single failure was the
  one time two heavy jobs ran there together. The local host cannot finish
  even ONE fat-LTO link — two failures in one pipeline did not even overlap
  (rpm 14:31-14:37 died, appimage started 14:37 and died at 14:45).
  So "run fewer jobs at once on both hosts" does not help; the local host
  needs more RAM or must not take these jobs at all.
  **WHAT CHANGED, since 0.8.4 built green first try:** NOT the dependency
  graph — the lock file is 471 crates in 0.8.4, 0.9.0, 0.9.1 and today. OUR
  crate grew: `rust/src` went 20 files / 1400 KB at 0.8.4 to 27 files /
  1740 KB at 0.9.0, and `lto = true` with `codegen-units = 1` merges
  everything into ONE compilation whose peak tracks total code. A 24% jump
  crossed the local host's ceiling.
  **The 0.9.2 workaround, still in place: runners 3,4,6,7,8 are PAUSED** so
  every package job goes to the host that can finish it. Un-pause with
  `glab api --method PUT runners/<id> --raw-field paused=false` once that
  host has more memory. Do NOT "fix" this by weakening LTO: every release
  since 0.6.x shipped fat LTO and 0.9.2 would become the odd one out.
- **The GitHub update slot is a tag that exists ONLY on GitHub, and the
  mirror was deleting it.** `update-latest` carries the signed manifest that
  installed clients read when GitLab is unreachable. The push mirror ran with
  `keep_divergent_refs=false`, which removes refs the source does not have —
  so the tag vanished after every release, a GitHub release whose tag is gone
  reverts to a DRAFT, and a draft's assets are NOT publicly downloadable. The
  fallback answered **404 for every installed client**, and because
  `/releases/tags/<tag>` cannot see drafts the job created a NEW one each
  release (two identical drafts by 0.9.1). Fixed 2026-09-06 by setting
  `keep_divergent_refs=true` on the mirror. If duplicates reappear, check
  that setting FIRST.
- **A freshly uploaded GitHub release asset is not instantly readable.**
  Its API reports `state=uploaded` with the right size while an anonymous GET
  answers 404 BlobNotFound for minutes. Any read-back check must POLL, not
  ask once — this failed 0.9.2's manifest mirror twice with correct bytes.

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
