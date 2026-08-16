# Code signing policy

Free code signing provided by SignPath.io, certificate by SignPath Foundation.

[SignPath.io](https://signpath.io/) · [SignPath Foundation](https://signpath.org/)

> **Current status (2026-08-12): not yet active.** Lightning has *applied for*
> nothing yet and has *not* been approved by SignPath Foundation, and no
> Lightning release is signed today. Every published Windows artifact is
> currently **unsigned**, and Windows will show an "unknown publisher" /
> SmartScreen warning. This document describes the policy that governs signing
> once (and only if) SignPath Foundation accepts the project. It will be updated
> to state that releases are signed only after a signed release actually ships.

## Project identity

| | |
|---|---|
| **Project** | Lightning — a native Qt 6 / QML Matrix desktop client |
| **Licence** | GPL-3.0-or-later ([`LICENSE`](../LICENSE)) — OSI-approved, no commercial dual-licensing |
| **Canonical source** | <https://gitlab.smetonis.net/Mizerd/lightning> (self-managed GitLab) |
| **Canonical releases** | <https://gitlab.smetonis.net/Mizerd/lightning/-/releases> |
| **Release automation** | <https://gitlab.smetonis.net/Mizerd/lightning-deploy> |
| **Mirror** | <https://github.com/Mizerd/lightning> — automatic, read-only. Serves byte-identical copies of published artifacts for update downloads; never a build source, never a release authority, and never holds a signing key |
| **Maintainer** | Rokas Smetonis |

## Roles

Lightning is a single-maintainer project. All three roles are currently held by
the same person, which is stated plainly here rather than padded with names that
do not exist.

| Role | Holder | Responsibility |
|---|---|---|
| **Authors / committers** | Rokas Smetonis | Trusted to commit to the canonical `main` branch without additional review |
| **Reviewers** | Rokas Smetonis | Reviews every change originating from anyone without commit access, before it is merged |
| **Approvers** | Rokas Smetonis | Approves each individual production signing request |

**Contributions from people without direct commit access** — issues, patches, and
merge requests — are never merged unmodified. They are reviewed by the maintainer
before landing on `main`, and only commits on `main` are ever released. Direct
write access to the canonical repository is limited to the maintainer.

If a second person is ever given commit, review, or approval rights, this table
is updated in the same commit that grants the access, and that person must have
multi-factor authentication enabled on both GitLab and SignPath.

## Multi-factor authentication

Multi-factor authentication is required for every person holding any of the roles
above, on both the canonical GitLab instance and (once onboarded) SignPath. This
is an account setting that cannot be proven from a repository, so it is asserted
here and verified by the maintainer directly in the account settings.

## What is signed

Only Lightning's *own* build outputs are signed. Upstream binaries redistributed
inside Lightning packages (Qt, the MinGW runtime, FFmpeg, Qt plugins) are shipped
as their projects built them and are **never** re-signed as Lightning.

The authoritative, machine-checked list is
[`docs/windows-signing-inventory.md`](windows-signing-inventory.md).

## How signing is triggered

1. A release is prepared on the canonical GitLab `main` branch.
2. An automated GitLab pipeline builds the Windows artifacts from one exact,
   immutable source commit — never from a developer workstation, never from an
   arbitrary branch, and never from a locally supplied binary.
3. The unsigned Lightning-owned payload is stored as a GitLab pipeline artifact.
4. A signing request is submitted to SignPath from that pipeline.
5. **The maintainer approves each signing request manually.** There is no
   automatic approval, and no bypass exists for production signing. A GitLab
   `when: manual` job is *not* treated as a substitute for SignPath's own
   approval step.
6. The signed payload is repackaged, the installers are signed, the artifacts are
   verified, and only then published.

The full provenance design is documented in
[`docs/signpath-build-provenance.md`](signpath-build-provenance.md).

## Privacy

Lightning's privacy behaviour is documented in full, derived from the source
code, in [`docs/privacy.md`](privacy.md).

Summary for this policy: Lightning is a Matrix client, so it communicates with
the homeserver the user chooses when they sign in. Apart from that, it will not
transfer any information to other networked systems unless specifically
requested by the user — GIF providers are contacted only while the user is using
the GIF picker, and automatic link-preview fetching is **off by default** and
must be enabled explicitly. Lightning contains no analytics, no telemetry, no
crash reporting, and no update check.

## Reporting a problem

Security-relevant reports, including anything about a signed artifact that does
not match this policy, go to the maintainer: Rokas Smetonis,
<antrasrokas@gmail.com>.
