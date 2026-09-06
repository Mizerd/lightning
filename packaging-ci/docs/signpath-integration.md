# SignPath integration (prepared, not active)

Everything needed to turn on SignPath Foundation code signing for the Windows
artifacts, and everything that deliberately has *not* been turned on.

**Nothing in this repository submits a signing request today.** No SignPath
account exists, no connector is configured, and no organization ID, project
slug, policy slug, artifact-configuration slug, connector URL, or API token
appears anywhere in these files — real or fabricated. The Windows artifacts are
honestly unsigned, and `LIGHTNING_WINDOWS_SIGNED` (see `scripts/lib.sh`) is
`false`, which is what makes every artifact name, metadata string, and release
description say so.

The project-side policy lives in the application repository:
[`docs/code-signing-policy.md`](https://gitlab.smetonis.net/Mizerd/lightning/-/blob/main/docs/code-signing-policy.md),
with the provenance design in `docs/signpath-build-provenance.md` and the
per-file inventory in `docs/windows-signing-inventory.md`.

## What is already in place

| Requirement | Where |
|---|---|
| Exactly two Lightning-owned PE files — the application and the update helper — declared explicitly | `packaging/windows/signing-inventory.json` |
| Each Lightning-owned PE carries its OWN version resource, so `OriginalFilename` names the file it is actually in | `write_version_rc` + `packaging/windows/version-resources.cmake` |
| Product metadata generated from one canonical version and verified | `scripts/build-windows.sh`, `scripts/verify-windows-metadata.py` |
| Upstream binaries classified, never signed, never relabelled | same, plus `scripts/validate-windows-artifacts.sh` |
| A deterministic unsigned payload with a checksum per owned file | `dist/windows/signing-payload/` |
| Both payloads are signed **before** the ZIP/MSI/setup consume them | `sign_windows_file` call order in `scripts/build-windows.sh` |
| One switch for signed/unsigned wording everywhere | `windows_signed` in `scripts/lib.sh` |
| A `Code signing policy` link on every future release page | `append_policy_footer` in `scripts/finalize-release.sh` |
| Tests for all of the above | `tests/test-windows-metadata.py`, `tests/test-release-notes-policy.py` |

## The blocking unknown: self-managed GitLab

SignPath's documentation states that it hosts a GitLab connector linked to
**GitLab.com**, and that integrating a **self-managed** GitLab instance requires
contacting SignPath support. This project's GitLab is self-managed
(`gitlab.smetonis.net`).

So the first integration step is a conversation, not a commit:

1. Ask SignPath support whether a connector can be provisioned for this
   self-managed instance, and what it requires (network reachability to the
   instance, an access token, an allowlist).
2. Only then do the identifiers and the connector URL exist. Until they do,
   there is nothing truthful to write into CI.

## Where the signing pipeline must run

SignPath's GitLab connector verifies that the artifact was built by a GitLab
pipeline, that the origin metadata came from **GitLab rather than the build
script**, and that the artifact exists as a GitLab pipeline artifact before
submission.

That last point about origin metadata is decisive for this repository's
architecture. A pipeline running *here* (project 7) is attested by GitLab as
"lightning-deploy at commit X". The application revision that was actually
compiled — the thing a user cares about — would be a claim made by
`fetch-lightning-source.sh`, not by GitLab. It is a well-pinned claim (the
repository URL is hard-coded, publication demands a 40-character commit SHA, the
SHA is re-verified in every job) but it is still the build script talking.

**Therefore the SignPath-bound Windows pipeline should originate in the
application repository**, where `CI_COMMIT_SHA` *is* the released revision, with
this repository consumed at a pinned SHA:

```yaml
# In Mizerd/lightning — sketch only; not committed anywhere yet.
include:
  - project: 'Mizerd/lightning-deploy'
    ref: '<full 40-character commit SHA of this repository>'
    file: '/packaging/windows/signing-pipeline.yml'
```

GitLab's own documentation says `include:project` accepts a full SHA and calls it
the most stable option. The packaging *scripts* must be checked out from the same
pinned SHA inside the job; `include` brings YAML only. The pin then lives in the
application repository's tracked CI file, which is part of the commit GitLab
attests — so one attested commit fixes both the source and the packaging logic,
and nothing anywhere says "use whatever is on main".

## The signing job, once the identifiers exist

SignPath publishes a GitLab CI component for submitting a signing request. Its
current inputs are `organization_id`, `project_slug`, `signing_policy_slug`,
`gitlab_artifact_job_name`, and `gitlab_artifact_path`, with an API token
supplied as a CI/CD variable. **Re-read SignPath's current documentation at the
time of integration** rather than trusting this paragraph; the component is
versioned and these names can change.

Shape of the eventual job:

```text
build-windows-payload   → artifact: dist/windows/signing-payload/
                          (Lightning.exe + lightning-updater.exe, each with
                           its .sha256; the helper is the process that replaces
                           the application on disk, so it is signed too)
        ↓
submit-signing-request  → SignPath component, inputs above
        ↓  (maintainer approves the request in SignPath — no auto-approval)
package-windows         → signed payloads → portable ZIP / MSI / setup EXE
                          then Authenticode-sign the MSI and the setup EXE
        ↓
validate → publish → release
```

Secrets belong in **protected, masked GitLab CI/CD variables** on the project
that runs the pipeline, never in a repository file. That includes the SignPath
API token and any GitLab access token the connector needs.

Two approvals are kept, and they are not the same thing:

- a GitLab `when: manual` gate on the release job, and
- **SignPath's own signing-request approval**, which the maintainer performs in
  SignPath for every production signing request.

No bypass for either is implemented, and none should be.

## Turning it on, later

1. Complete SignPath onboarding and obtain the connector plus identifiers.
2. Add the signing pipeline to the application repository, pinned as above.
3. Store the secrets as protected/masked CI variables.
4. Set `LIGHTNING_WINDOWS_SIGNED=true` **in the same change that actually
   signs**, so artifact names, PE metadata, MSI metadata, and release
   descriptions flip together and none of them can claim a signature that is
   not there.
5. Update the application repository's `docs/code-signing-policy.md` and README
   to say releases are signed — only after a signed release has shipped and been
   verified. The `signpath-compliance` test in the application repository
   deliberately fails if the README stops disclosing unsigned artifacts, so that
   step is a conscious edit rather than a drift.
