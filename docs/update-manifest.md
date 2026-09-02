# Signed update manifest

Lightning's in-app updater does not scrape a release page, parse HTML, or trust
a redirect. It fetches one small JSON document and one detached Ed25519
signature from a fixed URL, verifies the signature against a public key that is
**compiled into the application**, and only then reads a single field from it.
This document describes the deploy-side half of that: how the manifest is built,
signed, published, and rotated.

**Read this first, because it is the honest summary:** Windows Authenticode
signing is still **not active**. Every Windows artifact this pipeline publishes
is unsigned and will produce a SmartScreen "unknown publisher" warning
(`windows_signed` in `scripts/lib.sh` is `false`, and
[`docs/signpath-integration.md`](signpath-integration.md) explains why). There
is no GPG-signed APT or DNF repository, no Flathub publication, and no Snap
Store publication. Today, **the signed update manifest is the actual integrity
guarantee for an in-app update** — it is the only cryptographic link between "a
release Rokas cut" and "the bytes a user's Lightning installs". Treat the
signing key accordingly.

## Contents

- [What gets published](#what-gets-published)
- [Manifest format](#manifest-format)
- [Signature format](#signature-format)
- [Endpoints](#endpoints)
- [GitHub bandwidth mirror](#github-bandwidth-mirror)
- [Pipeline placement and ordering](#pipeline-placement-and-ordering)
- [Generating the signing key](#generating-the-signing-key)
- [CI variables](#ci-variables)
- [Where the public key lives](#where-the-public-key-lives)
- [Key rotation](#key-rotation)
- [Emergency: key compromise](#emergency-key-compromise)
- [Operational notes](#operational-notes)
- [Tests](#tests)

## What gets published

Two files, per release, to project 6's Generic Package Registry under the
generic package name `lightning-update`:

| File | Purpose |
| --- | --- |
| `update-manifest-v1.json` | The manifest: version, artifacts, checksums, URLs |
| `update-manifest-v1.json.sig` | Detached Ed25519 signature envelope over the manifest's exact bytes |

Both are written to **two** locations: an immutable per-release copy under
`<version>/`, and the mutable `latest/` slot that installed clients poll.

### Freshness: `expires`

Every manifest carries a signed `expires` instant, `released` plus
`UPDATE_MANIFEST_VALIDITY_DAYS` (default 120, range 1-366). Lightning 0.8.4
and later **refuse** a manifest without it and treat one past it as "update
information expired; check manually" — a failure, never "up to date". The
signature proves who produced the document; the expiry is what bounds how long
a captured `latest` pair can be replayed to freeze installations on a
vulnerable version.

**Refreshing without a release.** If a release lull outlasts the window, the
`latest` slot is refreshed in place — the immutable per-release copy cannot
be re-published (different bytes), so the refresh touches `latest` alone.
Trigger the pipeline against the CURRENT release with
`RELEASE_ACTION=attach-existing`, `SOURCE_REF=v<version>`,
`PUBLISH_PACKAGES=true`, plus three variables: `UPDATE_RELEASED_AT=<the
original released instant>`, `UPDATE_EXPIRES_AT=<a new instant, e.g. today
plus 120 days>` and `UPDATE_REFRESH_LATEST_ONLY=true`. The packages
converge (identical bytes are accepted), the manifest is regenerated with the
new expiry and signed, and `publish-update-manifest.sh` re-promotes only the
`latest` pair, refusing any version other than the one already there.
**Put "refresh `latest` before day 120" on the calendar** — nothing reminds
you, and on day 121 every installation reports that its update information
has expired.

### The `latest` slot never rolls back by accident

`publish-update-manifest.sh` reads the manifest currently in `latest/` and
refuses to promote a LOWER version. `RELEASE_ACTION=attach-existing` on an old
release runs the same script, and before this guard it re-pointed `latest` at
that old version: the client refuses the downgrade, so the effect was a
**freeze** — every current installation told it is up to date, indefinitely.
Yanking a bad release is the one legitimate backwards move; set
`UPDATE_ALLOW_LATEST_ROLLBACK=true` on that pipeline and the rollback is
performed and announced in the job log.

This is a **separate** document from `dist/manifest.json`. That file is the
*publication* manifest — the internal list of the nine files this pipeline is
allowed to upload. The update manifest is a *client-facing* document derived
from it, and it deliberately describes fewer things.

## Manifest format

`scripts/generate-update-manifest.sh` writes `dist/update-manifest-v1.json`:

```json
{
  "schema": 1,
  "version": "0.8.0",
  "channel": "stable",
  "tag": "v0.8.0",
  "released": "2026-08-16T12:00:00Z",
  "expires": "2026-12-14T12:00:00Z",
  "min_updater_version": 1,
  "release_notes_url": "https://gitlab.smetonis.net/Mizerd/lightning/-/releases/v0.8.0",
  "release_notes": "markdown, may be empty",
  "artifacts": {
    "linux-appimage":   { "filename": "…", "size": 0, "sha256": "…",
                          "url": "https://gitlab.smetonis.net/api/v4/projects/6/packages/generic/lightning/0.8.0/…",
                          "mirror_url": "https://github.com/Mizerd/lightning/releases/download/v0.8.0/…" },
    "linux-deb":        { … },
    "linux-rpm":        { … },
    "windows-msi":      { … },
    "windows-portable": { … },
    "windows-setup":    { … }
  },
  "channels": {
    "linux-flatpak":  { "available": false, "version": null, "note": "…" },
    "linux-snap":     { "available": false, "version": null, "note": "…" },
    "linux-deb-repo": { "available": false, "version": null, "note": "…" },
    "linux-rpm-repo": { "available": false, "version": null, "note": "…" }
  }
}
```

### `artifacts` — direct downloads only

Exactly six install-type keys, and only ever those that a user can actually
update by downloading a file. Every value is **copied** from the entry in
`dist/manifest.json` that `verify-published-packages` already re-downloaded and
hash-checked. Nothing is re-hashed from a local file here: re-hashing could
honestly describe bytes that were never the ones published, which is precisely
the failure the client's hash check exists to catch.

An install type that is not in the publication manifest is **omitted**, never
invented. Absence means "no direct download for this install type", which the
client treats as "check manually".

`url` is the **canonical** GitLab download and is always required.
`mirror_url` is optional and is present only when the GitHub bandwidth mirror is
configured — see [GitHub bandwidth mirror](#github-bandwidth-mirror). An absent
`mirror_url` means "no mirror for this artifact", which is exactly the behaviour
that existed before the mirror did. `schema` stays `1`: adding an optional field
is compatible in both directions.

### `channels` — ecosystem-managed installs, and why they are all false

Flatpak and Snap bundles **are** published as release files, but they are not
listed in `artifacts`, and this is a deliberate correctness decision rather than
an oversight. A Flatpak install is updated by Flatpak; handing that user a
`.flatpak` download and calling it an update would be telling them an action is
available that they must not take (and that would not update their installation
if they took it). The same is true of Snap. They belong in `channels`, which
describes ecosystem state without offering a download.

All four channels are `available: false` today, and each carries a `note` saying
plainly why:

- **`linux-flatpak`** — there is no Flathub publication. The `.flatpak` on the
  release page is a manual download.
- **`linux-snap`** — there is no Snap Store publication. Same situation.
- **`linux-deb-repo`** — there is no APT repository. The `.deb` is a direct
  download, which is why it *is* in `artifacts`.
- **`linux-rpm-repo`** — there is no DNF/YUM repository, same reasoning.

Every one of these is driven by a CI variable
(`UPDATE_CHANNEL_<NAME>_AVAILABLE` / `_VERSION` / `_NOTE`), so publishing to
Flathub later flips a variable rather than editing pipeline logic. A channel
that claims `available: true` **must** state a version; the generator refuses
otherwise, because a client comparing against `null` would have nothing to
compare.

### Determinism

Output is byte-reproducible from the same inputs: `jq -S` sorts every object's
keys, and the `released` timestamp comes from `UPDATE_RELEASED_AT` or
`CI_PIPELINE_CREATED_AT` rather than the wall clock. This is not cosmetic. The
per-release copy is published **immutably**, so a retried job that produced
different bytes would be refused at publication time for a reason that looks
exactly like tampering.

## Signature format

`scripts/sign-update-manifest.sh` writes `dist/update-manifest-v1.json.sig`:

```json
{ "alg": "ed25519", "key_id": "lightning-release-2026a", "sig": "<base64 of the 64-byte raw signature>" }
```

- **Algorithm:** Ed25519, one-shot over the manifest's **exact bytes**
  (`openssl pkeyutl -sign -rawin`). No digest step, no hand-rolled encoding, no
  canonicalisation on either side — the client verifies the bytes it received,
  not a re-serialisation of them.
- **`key_id`** exists so rotation is possible. The client looks it up in a
  compiled-in table; an unknown id is a hard failure. **The server can never
  introduce a key.**
- The envelope is capped at 4 KiB and the manifest at 256 KiB in the pipeline
  (the client's own bounds are 4 KiB / 1 MiB).

The signing script verifies its own output **twice** before anything is written
to a publishable path: once against the public key derived from the private key,
and once more after round-tripping through the envelope's base64 — which is the
check that catches an encoding mistake in the envelope itself. A signature that
does not verify is a hard failure. Publishing one would strand every client on a
manifest it must reject, and self-verification is the only thing that
distinguishes "signed" from "has a `.sig` file next to it".

### Client-side verification order (must not be reordered)

1. Fetch the `.sig` (≤ 4 KiB), parse, reject an unknown `alg`.
2. Look up `key_id` in the compiled-in trusted key set. Unknown → **fail**.
3. Fetch the manifest bytes (≤ 1 MiB) and verify the signature over the **raw
   bytes**, before parsing any field as trusted.
4. Only then parse the JSON and use its fields.

## Endpoints

```text
https://gitlab.smetonis.net/api/v4/projects/6/packages/generic/lightning-update/latest/update-manifest-v1.json
https://gitlab.smetonis.net/api/v4/projects/6/packages/generic/lightning-update/latest/update-manifest-v1.json.sig

https://gitlab.smetonis.net/api/v4/projects/6/packages/generic/lightning-update/<version>/update-manifest-v1.json
https://gitlab.smetonis.net/api/v4/projects/6/packages/generic/lightning-update/<version>/update-manifest-v1.json.sig
```

Public, unauthenticated, HTTPS, no token in any URL, no query parameter derived
from the user. The `<version>` copies are immutable; `latest/` is the one
deliberately mutable path this pipeline writes.

Artifact URLs inside the manifest always point at the canonical public host
(`CI_API_V4_URL`), never at `PUBLISH_API_BASE` — the pipeline's own requests may
be routed through the internal endpoint, but a durable URL handed to a user must
not be. The generator rejects any artifact URL that is not `https://` or is not
under `…/packages/generic/lightning/<version>/`.

## GitHub bandwidth mirror

**One sentence:** GitLab decides *what* Lightning may install; GitHub is only a
faster place to get the *bytes* GitLab already decided on.

### The two-source model

| | Canonical (GitLab) | Mirror (GitHub) |
| --- | --- | --- |
| Update manifest + signature | **Yes — only here** | Never |
| `release_notes_url` | **Yes — only here** | Never |
| Which version exists / is installable | **Decided here** | Not an input |
| Artifact bytes | Yes (`url`, and the fallback) | Yes (`mirror_url`, tried first) |
| Tag and Release authority | **Yes** | Read-only copy |

`scripts/mirror-release-to-github.sh` uploads the **exact local `dist/` files
recorded in `dist/manifest.json`** — the same bytes `publish-packages` uploaded
and `verify-published-packages` re-downloaded and hash-checked. It rebuilds
nothing and fetches the bytes from nowhere else. It then re-downloads every
uploaded asset **anonymously** (no token — that is what a client will do) and
compares the SHA-256 against `dist/manifest.json`. Any mismatch, missing asset,
or size difference fails the job.

### Why GitLab stays authoritative

Because a mirror that could decide anything would not be a mirror. Concretely:

- The manifest and its signature are fetched **only** from the GitLab endpoints
  above. Nothing is ever read from `api.github.com`, from a GitHub release
  listing, from `/releases/latest`, or from a GitHub tag.
- `mirror_url` lives **inside the signed bytes**. The mirror location is chosen
  by the release authority at signing time, not discovered at runtime.
- The URL form is the immutable, version-specific
  `…/releases/download/<tag>/<filename>` — never `/releases/latest/download/…`,
  which is a GitHub-derived pointer.

**A compromise of the GitHub mirror alone cannot ship a trusted update.** An
attacker who fully controls the mirror can replace a file, and the result is a
download that fails its SHA-256 check and falls back to GitLab. They cannot
produce a manifest a client will accept: it is signed with an Ed25519 key that
exists only as a protected CI variable on project 7 and as a public half
compiled into the binary, and GitHub never holds either. They cannot announce a
version, because the version decision reads only the signed manifest. The
SHA-256 every download is checked against is fixed *before* the first byte is
fetched. The worst outcome available to them is denial of service on the fast
path.

### The URL is derived, not discovered

`scripts/generate-update-manifest.sh` emits, per artifact:

```text
https://github.com/<GITHUB_MIRROR_REPO>/releases/download/<RELEASE_TAG>/<filename>
```

The three hosts (`api.github.com`, `uploads.github.com`, `github.com`) are
constants in `scripts/update-lib.sh`, deliberately **not** overridable by an
environment variable: such a variable could steer the one field whose entire
value is that the release authority chose it. `GITHUB_MIRROR_REPO` is validated
as `<owner>/<repo>` (no scheme, no path segment, no userinfo, no space), and any
filename GitHub would rewrite is refused rather than turned into a URL that will
not resolve. Emission is skipped entirely when mirroring is off, and
determinism is preserved — the versioned manifest copy is immutable, so a
retried job that produced different bytes would be refused as tampering.

### Enabled, disabled, and half-configured

One switch: **`GITHUB_MIRROR_REPO`**.

- **Unset** — no `mirror_url` is emitted, and the mirror job is a clean no-op. A
  pipeline without these variables behaves exactly as it did before.
- **Set, with `GITHUB_MIRROR_TOKEN`** — mirroring runs.
- **Set, token missing** — a **hard failure in the mirror job**, never a skip.
  That job runs before the `latest` promotion precisely so a mirror that cannot
  be completed stops the release from advertising one. For the same reason, the
  mirror job refuses to no-op if the signed manifest it was handed already
  carries `mirror_url` values.

### The tag must exist, and must be the same commit

GitLab push-mirrors **refs** to GitHub asynchronously, so the tag
`finalize-release` just created may not have arrived when the mirror job starts.
Creating a release at a tag GitHub does not have would make GitHub create that
tag itself from the default branch — publishing a "release" of a *different
commit*. So the job:

1. Polls `GET /repos/<repo>/git/ref/tags/<tag>` until the tag appears, bounded by
   `GITHUB_MIRROR_TAG_WAIT_SECONDS` (default **300 s**, polled every
   `GITHUB_MIRROR_TAG_POLL_SECONDS`, default 15 s). On timeout it fails with a
   message naming the push mirror; nothing has been uploaded, no GitHub release
   exists, and the GitLab release is untouched. Re-run the job once the tag is
   mirrored.
2. Peels an annotated tag through `GET /repos/<repo>/git/tags/<sha>` and requires
   the resulting commit to equal the released `SOURCE_SHA`. A different commit is
   a hard failure — the release is **not** created anyway.
3. Creates the release with `tag_name` only, never `target_commitish`. This is
   the `gh release create --verify-tag` guarantee, made explicit: the job cannot
   invent a tag.

> **This replaces the manual step.** Before this job existed, the GitHub Release
> was created by hand after the pipeline finished, with
> `gh release create v<version> --verify-tag --title "Lightning <version>"
> --notes-file docs/releases/v<version>.md <assets…>`. That step is **no longer
> needed** — the pipeline creates it, uploads every asset, and verifies each one
> anonymously. The notes differ deliberately: the automated release body states
> that the page is a read-only mirror and points at the canonical GitLab
> release, rather than repeating the release notes, which live on GitLab and in
> the signed manifest.

### Idempotence, and never overwriting

Re-running the job must converge, and it must never silently replace a published
byte:

- An asset already present with the expected size is **left alone** (and is
  still verified by the anonymous read-back, so byte-identical is proved, not
  assumed).
- An asset present with a **different** size, or whose anonymous read-back
  produces a different SHA-256, is a **hard failure**. The job never overwrites
  and never deletes a GitHub asset.
- An asset stuck in a non-`uploaded` state is a hard failure with an explicit
  instruction to remove it by hand.

### Credential handling

`GITHUB_MIRROR_TOKEN` is written into a `mktemp` curl **config file** with mode
`0600` and an `EXIT` trap that unlinks it — the same shape
`scripts/sign-update-manifest.sh` uses for the signing key and
`scripts/build-windows.sh` uses for the signing PFX. curl reads the
`Authorization` header from that file, so the token is never echoed, never in an
argument vector (argv is world-readable through `/proc` on a shared runner),
never in the working tree, and never in an artifact. `dist/github-mirror.json`
records only public URLs and already-published checksums. Command tracing
(`set -x`) stays off, as everywhere else in this pipeline.

The verification downloads carry **no** credential at all, deliberately: an
asset that is only readable with the publishing token is not a mirror asset.

## Pipeline placement and ordering

Three jobs, in three different stages, and the splits are the point:

| Stage | Job | Runs |
| --- | --- | --- |
| `sign` | `sign-update-manifest` | generate + sign, **before** `finalize-release` |
| `mirror` | `mirror-release-to-github` | copy + verify bytes, **after** `finalize-release` |
| `update` | `publish-update-manifest` | upload both slots, **after** the mirror verifies |

Full stage order: `test → resolve → build → validate → publish → verify → sign → release → mirror → update`.

**Why signing goes before the release.** Generating and signing touch nothing
outside the job. Doing them first turns a missing, malformed, or wrong-type
signing key into a failure that costs nothing. Discovering the same problem
afterwards would mean an immutable tag and GitLab Release already exist for a
version whose update manifest cannot be produced — and tags are never moved or
recreated.

**Why publication goes after the release.** The `latest` slot is what every
installed Lightning polls, and `release_notes_url` points at the GitLab release
page. Promoting `latest` before `finalize-release` would advertise an update
whose release page 404s, and — worse — would leave that advertisement standing
if the release job then failed. The reverse failure is inert and retryable: a
finished release whose manifest has not been promoted yet simply does not notify
anyone until the job is re-run.

**Why the mirror goes between them.** It must run after `finalize-release`
because a GitHub release may only be created at a tag the GitLab release already
produced. It must run before `publish-update-manifest` because the `latest` slot
is what clients poll: promoting it earlier would hand out a `mirror_url` that has
not been proved to serve the right bytes. If the mirror job fails, `latest` is
simply not promoted — the GitLab release stands and remains fully installable,
since `url` is the canonical source and the fallback either way.

**Ordering inside `publish-update-manifest`** is also load-bearing:

1. Upload the immutable per-release copy (manifest, then signature).
2. Read both back and compare bytes **and** SHA-256.
3. Only then write the `latest` slot — **signature first, manifest second**.
4. Read the `latest` slot back and compare again.

Writing the signature first means the narrow window a racing client can observe
is "new signature, old manifest", which fails verification. It never sees a new
manifest with no matching signature treated as valid.

### The one immutability exception

`scripts/publish-packages.sh` refuses to overwrite anything: an identical
existing file is accepted, a different one is a hard conflict. That rule is
untouched for all nine release packages **and** for the versioned update
manifest copies. The `latest` slot is the single deliberate exception, scoped to
those two file names, because a discovery pointer that cannot be moved is
useless after the first release.

GitLab's generic registry keeps superseded revisions of an overwritten file and
serves the most recent one. Old revisions are inert history, not something a
client can be steered toward, so they are left alone — deleting by file name
would also match the revision just uploaded.

> **Project setting:** the `latest` slot requires generic-package duplicates to
> be permitted for `lightning-update` on project 6 (Settings → Packages and
> registries → Generic packages, "Allow duplicates", or a
> `generic_duplicate_exception_regex` matching `lightning-update`). If they are
> forbidden, the upload fails with HTTP 403/400 and the script says so
> explicitly. It does not silently skip the promotion.

### If the mirror job fails

`publish-update-manifest` needs `mirror-release-to-github`, so a GitHub outage,
a rotated token, or a push mirror that never delivers the tag will leave the
`latest` manifest un-promoted. Nothing published becomes invalid: the GitLab
release is complete, its package links are attached, and every artifact is
installable by hand. Only the in-app rollout waits.

Two ways out, in order of preference:

1. **Fix and re-run the mirror job.** It is idempotent — an asset already
   present with identical bytes is accepted, and nothing is overwritten — so a
   retry after the cause is fixed is safe and is the normal answer.
2. **Promote without a mirror.** Re-run the pipeline with `GITHUB_MIRROR_REPO`
   unset. The mirror job becomes a no-op and the manifest is regenerated
   **and re-signed** with no `mirror_url`, so clients download from GitLab
   exactly as they did before mirroring existed. This is a different manifest,
   not the same one published differently: the versioned copy is immutable, so
   this only applies to a version whose manifest was never promoted.

Do not hand-edit a signed manifest to remove `mirror_url`; the signature would
no longer verify, and every client would refuse it.

## Generating the signing key

Run this **outside CI, on a trusted machine**. Never in a pipeline.

```sh
cd /home/roksme/git/lightning-deploy
./scripts/generate-update-signing-key.sh lightning-release-2026a ~/keys
```

It generates an Ed25519 keypair, writes the private key to
`~/keys/lightning-release-2026a.private.pem` with mode `0600`, proves the key
signs and verifies, and prints the **public** key (base64, raw 32 bytes) plus
the key id and the loading instructions.

It **refuses to print the private key**, and that is not squeamishness: this
kind of output gets pasted into chat windows and scrolled back through on shared
screens, and a printed signing key is a compromised signing key. It also refuses
to overwrite an existing key file.

## CI variables

Create these on **project 7 (lightning-deploy)**, Settings → CI/CD → Variables:

| Variable | Value | Flags |
| --- | --- | --- |
| `UPDATE_SIGNING_KEY_B64` | `base64 -w0 < <key>.private.pem` | **Protected**, **Masked**, not a File variable, **environment scope `signing`** |
| `UPDATE_SIGNING_KEY_ID` | e.g. `lightning-release-2026a` | **Protected**, scope `*` (not secret; every job derives the public-key variable name from it) |
| `UPDATE_SIGNING_PUBKEY_2026A` | the raw 32-byte public key, base64 (44 chars ending `=`) — the `public key` line `generate-update-signing-key.sh` prints | **Protected**, **not** masked (it is not secret), scope `*` |

**Environment scope matters.** A project variable with scope `*` is injected
into every job of the pipeline, including the six build jobs that run
project-6 CMake and every `build.rs` in the matrix-sdk dependency graph; one
hostile build script there would read the private key and become the release
authority, which no downstream hash check can contain. Only two jobs declare
`environment: signing` — `resolve-source` (the fail-fast consistency check,
before any build) and `sign-update-manifest` — and neither runs project-6
code. Until the scope is set in project 7's settings the pipeline works
exactly as before and the key is still everywhere.

The key is base64-encoded because GitLab masking requires a single-line value
with no newline; a raw PEM cannot be masked.

For the GitHub bandwidth mirror, create these two as well. Both are **optional**:
without `GITHUB_MIRROR_REPO` nothing mirrors, no `mirror_url` is emitted, and the
pipeline behaves exactly as it did before.

| Variable | Value | Flags |
| --- | --- | --- |
| `GITHUB_MIRROR_REPO` | `Mizerd/lightning` — the mirror repository, `<owner>/<repo>` | **Protected**, not masked (it is not secret) |
| `GITHUB_MIRROR_TOKEN` | A GitHub token that may create a release and upload assets on that repository, and nothing else | **Protected**, **Masked**, not a File variable, **environment scope `mirror`** (only `mirror-release-to-github` declares it) |

Scope the token as narrowly as GitHub allows — a fine-grained personal access
token limited to the mirror repository with *Contents: read and write* is enough
to create a release and upload assets. It never touches the update signing key,
the GitLab registry, or any Lightning source. If it leaks, an attacker can
vandalise the mirror; they still cannot ship an update (see
[GitHub bandwidth mirror](#github-bandwidth-mirror)). Revoke it on GitHub and
re-run the mirror job; the GitLab release is unaffected.

Two optional timing knobs, both with safe defaults:
`GITHUB_MIRROR_TAG_WAIT_SECONDS` (default `300`) and
`GITHUB_MIRROR_TAG_POLL_SECONDS` (default `15`) bound how long the job waits for
GitLab's push mirror to deliver the release tag to GitHub.

### `UPDATE_SIGNING_PUBKEY_2026A` — the trust root inside every package

The private key signs the manifest **once, in CI**. The public key is a
different thing entirely: it is **compiled into every package**, by every build
job, as `-DLIGHTNING_UPDATE_PUBKEY_2026A=…`
(`scripts/configure-build.sh`, `scripts/build-windows.sh`, and the Flatpak
manifest). It is the trust root the shipped binary uses, and it cannot be
changed after the fact — correcting it means cutting a new release.

Consequences worth stating plainly:

- **The variable name is key-id specific, on purpose.** A build may trust
  several key ids at once (that is what makes rotation possible), so one shared
  variable could not say which key is which. `lightning-release-2026a` ⇄
  `UPDATE_SIGNING_PUBKEY_2026A` ⇄ `LIGHTNING_UPDATE_PUBKEY_2026A` in
  `src/update/UpdateTrustStore.cpp`.
- **Introducing `lightning-release-2026b` is a three-part change**, and any one
  part done alone yields a release that cannot verify its own updates:
  1. Lightning (project 6): a new trust-table row **and** a new
     `LIGHTNING_UPDATE_PUBKEY_2026B` cache variable.
  2. lightning-deploy (project 7): a new `UPDATE_SIGNING_PUBKEY_2026B` CI
     variable, a row in `update_pubkey_var_for_key_id`
     (`scripts/update-lib.sh`), and the `-D` flag in `configure-build.sh`,
     `build-windows.sh` and `packaging/flatpak/org.lightning_matrix.Lightning.yaml.in`.
  3. Only then may `UPDATE_SIGNING_KEY_ID` be switched to it.
- **Empty is a refusal for a publishing pipeline, not a warning.** A build with
  no key compiled in fails closed and can never accept an update. That is the
  right behaviour for a source build — but a pipeline that holds the private key
  and ships such packages anyway is shipping a feature that cannot work, so
  `scripts/check-update-signing-keys.sh` stops it.

### The consistency gate

Nothing else in the pipeline connects the three inputs: the signing job uses the
private key, the build jobs embed the public one, and the client trusts a key id
from a table compiled into it. Out of step, the pipeline still **succeeds** — it
publishes a correctly signed manifest that every package it just built must
reject. The failure is invisible until a user clicks "check for updates", and it
is unfixable for that release.

`scripts/check-update-signing-keys.sh` therefore asserts, before anything is
built:

- `UPDATE_SIGNING_KEY_ID` is one of the ids Lightning's compiled trust table
  actually knows (`UPDATE_CLIENT_TRUSTED_KEY_IDS` in `scripts/update-lib.sh`);
- the matching `UPDATE_SIGNING_PUBKEY_<id>` is set and is a base64 raw 32-byte
  Ed25519 public key;
- that value **is** the public half of `UPDATE_SIGNING_KEY_B64`, derived with
  `openssl pkey -pubout`.

It runs twice: in `validate-release-request.sh` (first job — before the builds
that would otherwise bake in the wrong trust root) and again in
`sign-update-manifest.sh` (the job that actually holds the private key). It
prints **neither** key, not even on a mismatch. Both mismatch cases and the
empty/malformed/unknown-id cases are covered by `tests/test-update-manifest.sh`.

Optional, all with safe defaults:

| Variable | Default | Meaning |
| --- | --- | --- |
| `UPDATE_CHANNEL` | `stable` | Only `stable` is accepted today |
| `UPDATE_MIN_UPDATER_VERSION` | `1` | Raise when a manifest needs a newer client updater |
| `UPDATE_RELEASED_AT` | `CI_PIPELINE_CREATED_AT` | Pins the `released` timestamp (retry-stable) |
| `UPDATE_INCLUDE_RELEASE_NOTES` | `true` | `false` publishes an empty `release_notes` |
| `LIGHTNING_RELEASE_BASE_URL` | project 6 web URL | Base for `release_notes_url` |
| `UPDATE_CHANNEL_FLATPAK_AVAILABLE` / `_VERSION` / `_NOTE` | `false` / empty / explanatory | Flip after a Flathub publication exists |
| `UPDATE_CHANNEL_SNAP_AVAILABLE` / `_VERSION` / `_NOTE` | `false` / empty / explanatory | Flip after a Snap Store publication exists |
| `UPDATE_CHANNEL_DEB_REPO_AVAILABLE` / `_VERSION` / `_NOTE` | `false` / empty / explanatory | Flip after an APT repository exists |
| `UPDATE_CHANNEL_RPM_REPO_AVAILABLE` / `_VERSION` / `_NOTE` | `false` / empty / explanatory | Flip after a DNF repository exists |

### How the private key is handled in the job

- Decoded from the variable **through stdin** into a `mktemp` file with mode
  `0600` and a `RETURN`/`EXIT` trap that unlinks it — the same shape
  `scripts/build-windows.sh` uses for the signing PFX.
- Never echoed, never placed in an argument vector (argv is world-readable
  through `/proc` on a shared runner), never written into the working tree,
  never included in an artifact.
- `openssl pkey -text` is used **nowhere** in this pipeline: it prints private
  key material to stdout. Key-type validation instead derives the public
  SubjectPublicKeyInfo and checks its fixed 44-byte Ed25519 shape.
- Command tracing (`set -x`) must stay off in every script involved, exactly as
  `scripts/gitlab-api.sh` already documents for credential headers.
- The **public** key is printed, deliberately. It is not secret, and it lets an
  operator confirm from the job log which key actually signed a release.

## Where the public key lives

In the Lightning application source (project 6), compiled in as a small static
table — `src/update/UpdateTrustStore.{h,cpp}`:

```cpp
struct TrustedUpdateKey { const char* keyId; const char* base64Ed25519PublicKey; bool retired; };
```

Consequences that are the whole security model:

- **The remote side can never add a key.** Adding one requires shipping a new
  Lightning build. A compromised registry can serve any manifest it likes and
  every client will reject it.
- Multiple key ids may be trusted at once, which is what makes rotation
  possible without breaking already-installed clients.
- `retired: true` keys are **rejected**. They stay in the table only as
  documentation of what was withdrawn.

## Key rotation

Rotation is a multi-release process, and it has to be, because an old client
only trusts the keys that were compiled into it. Never skip a step in the hope
of shortening it — a manifest signed by a key nobody has shipped yet is a
manifest nobody can install.

1. **Generate the new key** (§ above), key id `…-2026b`.
2. **Ship trust for it, without using it.** In Lightning, add
   `{ "lightning-release-2026b", LIGHTNING_UPDATE_PUBKEY_2026B, false }` to the
   trust table while leaving `…-2026a` present and un-retired, and add the
   matching `LIGHTNING_UPDATE_PUBKEY_2026B` cache variable beside the 2026a one.
   In lightning-deploy, add the `UPDATE_SIGNING_PUBKEY_2026B` CI variable (the
   new key's public half), its row in `update_pubkey_var_for_key_id`
   (`scripts/update-lib.sh`), its id in `UPDATE_CLIENT_TRUSTED_KEY_IDS`, and the
   `-DLIGHTNING_UPDATE_PUBKEY_2026B=…` flag in `configure-build.sh`,
   `build-windows.sh` and the Flatpak manifest. Release that build normally,
   **still signed by 2026a**.
3. **Wait for adoption.** Until a user updates to that build, their Lightning
   does not know 2026b exists. How long to wait is a judgement call about how
   many users you are willing to strand on manual updates.
4. **Switch signing.** Change `UPDATE_SIGNING_KEY_ID` and
   `UPDATE_SIGNING_KEY_B64` on project 7 to the new key. The next release is
   signed by 2026b; clients from step 2 onward accept it. Older clients now stop
   auto-updating and must update manually — which is the correct, safe failure,
   and the reason step 3 exists.
   If step 2's deploy-side edits were skipped, this step **fails closed**:
   `check-update-signing-keys.sh` rejects the unknown id in the first job rather
   than letting a pipeline sign with a key no shipped client trusts.
5. **Retire the old key, later.** Once no supported release is still being
   verified by it, flip `…-2026a` to `retired: true` in a subsequent Lightning
   build. Keep the row. Keep `UPDATE_SIGNING_PUBKEY_2026A` on project 7 and its
   `-D` flag in the build scripts for as long as the retired row exists — the
   variable feeds the row, and an empty one would make the retired row
   indistinguishable from a build error.
6. **Destroy the old private key** once it is retired everywhere. The old
   **public** key is not secret and is retired by the trust table, not by
   deleting the variable.

The per-release copies signed by the old key stay published and stay valid for
the clients that trust it; nothing already released is invalidated by rotation.

## Emergency: key compromise

If the signing private key is disclosed — leaked variable, exfiltrated runner,
key file recovered from a machine, anything you cannot rule out:

1. **Revoke the CI variables immediately.** Delete `UPDATE_SIGNING_KEY_B64` and
   `UPDATE_SIGNING_KEY_ID` on project 7. Every subsequent
   `sign-update-manifest` then fails closed (`require_var`), and so does
   `validate-release-request` in the first job of any publishing pipeline. No
   release can be cut with the compromised key by accident. Do this before
   anything else; it is the only step that stops the bleeding.
   Leave `UPDATE_SIGNING_PUBKEY_2026A` in place: it is the compromised key's
   **public** half, it is not secret, and deleting it would only make the next
   pipeline fail with a confusing "no public key set" instead of the honest
   "no signing key configured". It becomes irrelevant when the compromised id
   is flipped to `retired: true` in step 4.
2. **Assume the attacker can sign a manifest, and reason about what that buys
   them.** It does **not** let them serve one: the endpoint is project 6's
   registry, and writing there needs a GitLab credential, not the signing key.
   The realistic risk is an attacker who *also* has write access to that
   registry, or who can intercept it. Check the registry for unexpected
   revisions of the `latest` slot and for any `lightning-update` version that
   does not correspond to a real release.
3. **Rotate under compression.** Run the rotation above, but publish step 2's
   trust-only build as a **hotfix release**, and shorten step 3 to the minimum
   you can justify.
4. **Retire the compromised key in the same hotfix if you can.** Setting
   `retired: true` on the compromised id makes clients reject **everything**
   signed by it — including legitimate past releases. That is the correct trade
   under compromise, and it is why `retired` exists as a separate flag rather
   than a deleted row: it is a statement, not a cleanup.
5. **Tell users.** A release note saying the update key was rotated and older
   builds must be updated manually is not optional. Users who cannot
   auto-update need to know why, and users on a compromised-key build need to
   know to verify their download against `SHA256SUMS`.
6. **Rotate the GitLab publication credentials too** if the disclosure could
   plausibly have exposed them (same runner, same variable store, same host).

Do **not** attempt to "un-publish" an already-released manifest by overwriting
a versioned copy. It is immutable by contract and the script will refuse.
Retiring the key id is the mechanism for invalidating signatures.

## Operational notes

- **openssl in CI.** The `alpine:3.22` publish image ships `libcrypto` but not
  the `openssl` CLI, so both update jobs add `openssl` to their `apk add` line.
  The `config-tests` Debian image installs it too, for the test suite. No other
  job needs it.
- **Verifying a published release by hand:**

  ```sh
  base=https://gitlab.smetonis.net/api/v4/projects/6/packages/generic/lightning-update/latest
  curl -fsSL "$base/update-manifest-v1.json"     -o m.json
  curl -fsSL "$base/update-manifest-v1.json.sig" -o m.sig
  jq -r .key_id m.sig                              # which key signed it
  jq -r .sig m.sig | openssl base64 -A -d > sig.bin
  # pub.pem built from the base64 public key in Lightning's trust table
  openssl pkeyutl -verify -pubin -inkey pub.pem -rawin -in m.json -sigfile sig.bin
  ```

  Note that anonymous probes of the GitLab package endpoints 403 under Python's
  default user-agent (reverse-proxy bot filter). Test with `curl`; it is not an
  access failure.
- **`dist/update-publication.json`** is written by the publish job as a record:
  version, tag, source SHA, key id, both checksums, and all four canonical URLs.
  It contains no secrets.

## Tests

`tests/test-update-manifest.sh`, run by `config-tests` on every pipeline, covers
generation from a synthetic publication manifest, the exact artifact key set,
filename/size/SHA-256 passthrough, refusal without `dist/verification.json`,
flatpak/snap absence from `artifacts`, channel honesty, determinism, a
sign→verify round trip with a freshly generated key, tamper and wrong-key
rejection, non-Ed25519 key rejection, the absence of key material from every
captured log, per-release immutability, `latest` overwritability, and the
ordering guarantee that `latest` is written only after the versioned copy.

It also covers the **consistency gate** (`check-update-signing-keys.sh`): a
matching triple is accepted; a public key from an unrelated keypair, an empty
`UPDATE_SIGNING_PUBKEY_2026A`, a PEM pasted in its place, a truncated value, a
key id no shipped Lightning trusts, a malformed key id, and a missing private
key are each rejected; the mismatch diagnostic prints **neither** key; the same
outcomes hold through `validate-release-request.sh`; and a build-only pipeline
is unaffected.

No key material is committed; every key the suite uses is generated at run time.

The same suite covers the **GitHub bandwidth mirror**, against a mock that models
the GitHub tag, release, upload, and anonymous-download endpoints separately from
the GitLab ones: the deterministic `mirror_url` when mirroring is enabled and its
complete absence when it is not; determinism preserved with mirroring on; a
`GITHUB_MIRROR_REPO` that could steer a URL (scheme, extra path segment,
userinfo, space) rejected; a configured repository with no token failing hard
rather than skipping; a job that would no-op while the signed manifest already
promises mirrors refusing; an unmirrored tag failing after the bounded wait and
a tag peeling to a different commit failing outright, with **no** GitHub release
created in either case; every publication-manifest artifact uploaded
byte-identically; the read-back downloads being anonymous while the API calls are
authenticated; the token appearing in no log, no URL, and not in
`dist/github-mirror.json`; an identical re-run uploading nothing; an
already-present asset that differs in bytes **or** in size being a hard failure
that overwrites nothing; a non-`uploaded` asset state refused; a local file that
no longer matches the publication manifest refused; and a signed manifest whose
`mirror_url` disagrees with what the job would create refused.

`tests/test-pipeline-config.py` additionally asserts the stage placement, the
`needs` wiring in both directions (including that the mirror runs after
`finalize-release` and that `latest` is promoted only after the mirror), that all
three jobs are behind `.publish-rules` (so no update manifest can be produced or
promoted, and nothing can be mirrored, in a non-publishing pipeline), that they
install the tools they need, and that no CI job ever runs the operator
key-generation tool.
