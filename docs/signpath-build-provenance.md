# Build provenance for SignPath signing

How a Windows artifact gets from a commit in this repository to something that
can be signed with a verifiable claim about where it came from. Written for the
SignPath Foundation review, and for whoever wires the signing pipeline up later.

Nothing here is active yet. No signing request has been made, no SignPath
account exists, and no identifiers, tokens, or connector URLs are invented
anywhere in this repository.

## What SignPath's GitLab connector actually verifies

From SignPath's current documentation
([docs.signpath.io/trusted-build-systems/gitlab](https://docs.signpath.io/trusted-build-systems/gitlab)),
the GitLab connector checks three things:

1. the artifact was built by a **GitLab pipeline**, not by something else
   holding an API token;
2. the **origin metadata is supplied by GitLab, not by the build script**, and
   therefore cannot be forged;
3. the artifact **exists as a GitLab pipeline artifact** before it is submitted
   for signing.

Origin metadata means the repository URL, ref, and commit *of the pipeline that
ran*. Two consequences follow, and they drive everything below.

## The current architecture, and the gap

Today, Windows packages are built by the pipeline in
[**lightning-deploy**](https://gitlab.smetonis.net/Mizerd/lightning-deploy)
(project 7). That pipeline is already strict about provenance, and none of that
is being thrown away:

- the source repository URL is **hard-pinned in the script**
  (`scripts/fetch-lightning-source.sh` refuses any value but the canonical
  Lightning URL) — it is not a free variable;
- a publishing run requires `SOURCE_REF` to be a **full 40-character commit
  SHA**; a branch or a recreatable tag is refused
  (`scripts/validate-release-request.sh`);
- the resolved SHA is recorded, propagated to every downstream job, and
  re-verified in each one, and the checkout must be clean;
- the CMake project version in the fetched source must equal the declared
  `RELEASE_VERSION`, or the run fails;
- publication is possible only from the packaging project's protected default
  branch, and the tag and Release are created only after packages are built,
  validated, published, and verified;
- the released commit must be reachable from this repository's `main`.

**The gap is not weak pinning — it is who attests it.** If the signing request
comes from a pipeline running in `lightning-deploy`, the origin metadata GitLab
supplies to SignPath names *lightning-deploy* and *its* commit. The application
revision that was actually compiled would be a claim made by the build script —
exactly the class of statement the connector is designed not to trust. SignPath
could attest "this was built by pipeline N of Mizerd/lightning-deploy", but not
"this is a build of Lightning commit `<sha>`".

## Target architecture

**The SignPath-bound Windows pipeline must originate in the canonical Lightning
repository**, so that the commit GitLab attests *is* the application revision
being released.

```text
Mizerd/lightning  ── the released commit
        │              (GitLab attests repo + commit for this pipeline)
        │
        ├─ .gitlab-ci.yml  includes packaging YAML from Mizerd/lightning-deploy
        │                  at a PINNED 40-character SHA, and checks out the
        │                  packaging scripts at that same pinned SHA
        ▼
   Windows build job  ── builds THIS commit (CI_COMMIT_SHA), no SOURCE_REF input
        │
        ▼
   GitLab pipeline artifact:  unsigned Lightning.exe + SHA-256 + build-info
        │
        ▼
   SignPath signing request   ── manual approval by the maintainer
        │
        ▼
   signed Lightning.exe
        │
        ├─► portable ZIP        ├─► MSI (then signed)   ├─► setup EXE (then signed)
        ▼
   validation → publish to the Package Registry → attach to the Release
```

Packaging logic stays in `lightning-deploy`. It is *consumed*, not duplicated:

- GitLab's `include: project:` accepts a **full 40-character commit SHA** as
  `ref`, which its own documentation calls the most stable and secure option.
  Pinning it there means the packaging YAML cannot change under a release.
- The packaging *scripts* (not just the YAML) are checked out in-job from
  `lightning-deploy` at the **same pinned SHA**, recorded in the build metadata.
  There is no "clone whatever is on `main`" step anywhere in the path.

The pin lives in this repository's tracked `.gitlab-ci.yml`, so it is part of the
commit GitLab attests. That closes the loop: one attested commit determines the
application source *and* the packaging logic that built it.

## Artifact boundary

The signing request needs a deterministic, minimal payload. Lightning's is one
file — see [`docs/windows-signing-inventory.md`](windows-signing-inventory.md) —
so no wrapper archive is needed:

| | |
|---|---|
| Artifact job | the Windows build job in this repository's pipeline |
| Artifact path | the unsigned `Lightning.exe`, in a dedicated directory containing nothing else |
| Alongside it | a SHA-256 checksum and the build metadata (version, source commit, packaging commit, toolchain versions) |
| Not in it | installers, the staged Qt runtime, logs, reports — those are produced *after* signing |

The packaging pipeline already stages the application, signs the staged
executable **before** the ZIP/MSI/NSIS steps consume it, and then signs the MSI
and the setup EXE. That ordering is already correct for SignPath; what changes is
where the signature comes from.

## Self-managed GitLab

This project's GitLab is **self-managed**
(`gitlab.smetonis.net`), not GitLab.com. SignPath's documentation states that it
hosts a GitLab connector linked to GitLab.com and that **integrating a
self-managed GitLab instance requires contacting SignPath support**.

Therefore:

- the public GitLab.com connector is *not* assumed to work here, and is not
  hard-coded anywhere;
- no self-managed connector URL is invented; it does not exist until SignPath
  provisions one;
- whether SignPath can reach and verify a self-hosted instance at all — and what
  network access or configuration that requires — is an open question for
  SignPath support, not something this repository can decide.

This is the single largest external unknown in the whole plan, and it is listed
as such in the readiness matrix rather than papered over.

## What is deliberately not done yet

- No SignPath CI job, component reference, or connector URL is committed.
- No organization ID, project slug, signing-policy slug, artifact-configuration
  slug, or API token appears anywhere — real or placeholder-with-a-value.
- No `.gitlab-ci.yml` has been added to this repository. Adding one changes what
  runs on every push; it is a deliberate, separate step for the maintainer,
  taken once SignPath onboarding tells us what the signing job must contain.

Once SignPath provides the connector and the identifiers, the secrets belong in
**protected, masked GitLab CI/CD variables** on the canonical project — never in
a repository file. The variable and input names are whatever SignPath's current
GitLab documentation specifies at that time (its component takes
`organization_id`, `project_slug`, `signing_policy_slug`,
`gitlab_artifact_job_name`, and `gitlab_artifact_path` as inputs, plus an API
token supplied as a CI variable); they should be re-read then rather than copied
from here.

## Manual signing approval

Every production signing request requires deliberate human approval. A GitLab
`when: manual` job is **not** a substitute for SignPath's own signing-request
approval, and both will be kept: the release job is manual, and the SignPath
request is approved by the maintainer in SignPath. No automatic approval and no
bypass will be implemented.
