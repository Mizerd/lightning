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
  "min_updater_version": 1,
  "release_notes_url": "https://gitlab.smetonis.net/Mizerd/lightning/-/releases/v0.8.0",
  "release_notes": "markdown, may be empty",
  "artifacts": {
    "linux-appimage":   { "filename": "…", "size": 0, "sha256": "…", "url": "https://…" },
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

## Pipeline placement and ordering

Two jobs, in two different stages, and the split is the point:

| Stage | Job | Runs |
| --- | --- | --- |
| `sign` | `sign-update-manifest` | generate + sign, **before** `finalize-release` |
| `update` | `publish-update-manifest` | upload both slots, **after** `finalize-release` |

Full stage order: `test → resolve → build → validate → publish → verify → sign → release → update`.

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
| `UPDATE_SIGNING_KEY_B64` | `base64 -w0 < <key>.private.pem` | **Protected**, **Masked**, not a File variable |
| `UPDATE_SIGNING_KEY_ID` | e.g. `lightning-release-2026a` | **Protected** |
| `UPDATE_SIGNING_PUBKEY_2026A` | the raw 32-byte public key, base64 (44 chars ending `=`) — the `public key` line `generate-update-signing-key.sh` prints | **Protected**, **not** masked (it is not secret) |

The key is base64-encoded because GitLab masking requires a single-line value
with no newline; a raw PEM cannot be masked.

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
     `build-windows.sh` and `packaging/flatpak/net.smetonis.Lightning.yaml.in`.
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

`tests/test-pipeline-config.py` additionally asserts the stage placement, the
`needs` wiring in both directions, that both jobs are behind `.publish-rules`
(so no update manifest can be produced or promoted in a non-publishing
pipeline), that they install `openssl`, and that no CI job ever runs the
operator key-generation tool.
