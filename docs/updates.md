# Application updates

Lightning can tell you when a newer release exists and, for the package types
that allow it, install that release for you. This document describes exactly
what it contacts, what it sends, how an update is verified, and what happens
for each package format.

Historically Lightning did not check for updates at all. That is no longer
true, so the privacy section below is part of the contract, not a footnote.

## What Lightning contacts, and what it sends

Two hosts, with sharply different roles.

**`gitlab.smetonis.net` — the release authority.** It is the first source of
update *metadata*: which version exists, what the artifact is called, how big it
is, and what its SHA-256 must be. Every decision Lightning makes about updating
comes from a signed document — signed on this host, with a key that exists
nowhere else — and from nothing else. Where that document is *fetched* from
can be GitLab or, when GitLab does not answer or answers with something that
does not verify, the fixed fallback slot on the mirror below; the signature
check is identical either way.

**`github.com` — a bandwidth mirror, and the fallback copy of the metadata.**
Lightning downloads the update *file* from the project's read-only GitHub
mirror first, to keep the load off the project's own server, and reads the
signed manifest pair from one fixed GitHub release slot when GitLab is
unreachable (see *When GitLab is unreachable* below). GitHub redirects both
to `objects.githubusercontent.com` or `release-assets.githubusercontent.com`,
so those hosts are contacted too. If the mirror cannot supply the file,
Lightning falls back to GitLab and the update proceeds normally.

GitHub is never asked *what* to install. Lightning makes no GitHub API call, and
never reads GitHub's releases list, `/releases/latest`, tags, or any other
GitHub information — the mirror's URL is itself part of the signed manifest, so
the release authority chooses it rather than Lightning discovering it. Anything
the mirror returns is treated as a bag of bytes and is immediately checked
against the SHA-256 the signed manifest already fixed. See *Why a mirror cannot
publish an update* below.

An update check is an ordinary anonymous HTTPS GET of two small public files
from the project's package registry — from GitLab first, and from the fixed
GitHub slot only when GitLab does not answer:

```
.../api/v4/projects/6/packages/generic/lightning-update/latest/update-manifest-v1.json
.../api/v4/projects/6/packages/generic/lightning-update/latest/update-manifest-v1.json.sig
```

The request carries the URL and Lightning's update user agent, which is exactly
`Lightning/<version>` — the application version and nothing else. The same is
true of the artifact download, whichever host serves it, and of a fallback
metadata check: GitHub sees an anonymous request for a public release file,
plus the connecting IP address, as
any file download does. Specifically, neither a check nor a download **ever**
includes:

- your Matrix user ID, display name or avatar
- your homeserver
- your device ID or any session identifier
- an access or refresh token
- any room, message or contact information
- any analytics identifier, and there is no updater-specific tracking ID —
  none is generated, stored or sent

Update preferences are stored outside every account: the key `update/` in
application settings holds only the automatic-check preference, the time of the
last successful check, and the version you last dismissed. No Matrix data is
stored with them, and switching accounts does not touch update state or restart
a download.

### Manual and automatic checks

**Manual** — Settings → Updates → *Check for updates*. Always available, always
explicitly initiated by you.

**Automatic** — on by default, and switchable off in Settings → Updates.
Lightning checks at most
once every 24 hours, never during the first 30 seconds after launch, and never
in response to switching room or account. Checks are asynchronous: startup is
never delayed waiting for the network.

## How an update is verified

The trust chain, in order. Every step must pass; there is no way to continue
past a failure, and the interface deliberately offers none.

```
public key compiled into Lightning
  -> Ed25519 signature over the exact manifest bytes   (GitLab only)
  -> expected filename, size and SHA-256 of the artifact
  -> those exact bytes fetched from the GitHub mirror, or GitLab if that fails
  -> SHA-256 verified against the signed value
  -> package-specific installation
```

1. **The signature is checked over the raw manifest bytes before a single field
   is read.** The `.sig` file is a small JSON envelope naming an algorithm and a
   key ID; the key ID must already be in Lightning's compiled-in trust table.
   A manifest cannot introduce a key, and an unknown key ID is a hard failure.
2. **The signature algorithm is Ed25519, verified through OpenSSL 3's EVP API.**
   Lightning implements no cryptography of its own.
3. **The artifact is streamed to a private temporary file** and its SHA-256 is
   computed as it arrives, then compared against the value in the signed
   manifest. The manifest's declared size is enforced as a hard ceiling during
   the transfer, so a truncated or oversized download fails rather than
   completing.
4. **The download host is chosen from the signed manifest, never discovered.**
   The mirror is tried first and the canonical host is the fallback. Both are
   checked against a compiled-in allowlist of hosts, exact-matched. A manifest
   naming an unknown host for the *canonical* address is rejected outright; an
   unknown *mirror* host is simply ignored, and that artifact downloads from
   the canonical source as if no mirror had been offered — so moving the mirror
   in future cannot strand clients built before the move. Metadata is accepted
   only from the canonical host, so the mirror cannot serve a manifest, a
   signature or the release notes.
5. **A mismatch from the mirror is retried once from the canonical host, and a
   mismatch there stops everything.** Falling back keeps updates working when a
   mirror is broken or stale, and costs nothing: both attempts are checked
   against the same signed hash, so neither can install different bytes than the
   other would have.
6. **Failure is terminal.** Invalid signature, unknown key, wrong hash, wrong
   size, unsupported schema, or a URL that is not HTTPS on an allowed host all
   stop the update, and nothing in the interface offers a way past them.

### Why a mirror cannot publish an update

The mirror holds bytes. It holds no key, and it is not consulted about
versions. Concretely:

- **If GitHub serves a modified binary**, its SHA-256 will not match the value
  in the signed manifest and the download is discarded. Lightning then retries
  from GitLab; if that also fails, the update fails. A tampered file is never
  installed, and there is no way for a user to override that.
- **If GitHub serves modified metadata**, nothing happens, because Lightning
  never reads metadata from GitHub. The manifest and its signature come only
  from the canonical host, and the mirror's host is not accepted for either.
- **If GitHub hosts a newer or extra release**, it is ignored entirely.
  Lightning's idea of "what version exists" comes from the signed manifest, so
  an unsigned GitHub release — however new it looks — is invisible to it.
- **If someone takes over the GitHub account**, they can break downloads and
  they can be noticed doing it, but they cannot make Lightning install anything:
  publishing a trusted update requires the Ed25519 signing key, which lives only
  in the release pipeline's protected variables and never touches GitHub.

The property to hold on to is that the signed manifest fixes the exact filename,
size and hash *before any download starts*. Choosing a different host to fetch
from cannot change what is considered acceptable to install.

### The manifest cannot tell Lightning what to run

This is the property that matters most. The manifest describes *which file*,
*how big*, and *what hash* — never *what to do*. There is no command, argument,
script or URL-to-execute field, and Lightning has no code path that would
execute one. Every installation strategy below is compiled into Lightning and
the updater helper. Remote metadata selects among fixed, trusted behaviours; it
can never define one.

### Freshness: `expires` is information, never a lock

A signature proves who produced the manifest; it cannot prove the manifest is
the *current* one. Every manifest therefore carries a signed `expires`
instant (`released` plus 120 days from the pipeline), and Lightning shows,
once that instant has passed, that the update information was expected to be
refreshed by then and the project may be offline. **Nothing else changes:**
the update the manifest names is still offered, its downloads are still
verified against the signed hashes, a manifest with no `expires` at all is
accepted as one that never expires, and one whose `expires` cannot be read
is treated as already stale (the line shows; nothing else changes), so a
generator defect cannot silently switch the signal off. This is deliberate. The maintainer's
requirement is that installed clients keep working — and keep updating from
the GitHub mirror — if his servers lose power or disappear for good, with no
action from him or from GitLab; a manifest that turned into a failure on a
date would do the opposite. What the expiry gives up is freeze detection: a
captured pair can be replayed to keep a client on an old release. What a
replay cannot do is install anything the signing key did not sign, or
downgrade anyone. The pipeline can still refresh `latest` without a release
(lightning-deploy `docs/update-manifest.md`), which is what keeps the status
line quiet during a long lull.

### Version comparison

Semantic, never lexicographic, so `0.10.0` is correctly newer than `0.9.0`.
Prerelease versions sort below their release. **This build never installs a
prerelease, whatever the manifest calls its channel.** The earlier rule ("the
stable channel never offers a prerelease") read the manifest's own `channel`
field, so a document that simply called itself something else walked past it;
the decision now belongs to the build, not the document. If the published
version equals yours, or is older than yours, Lightning reports that you are
up to date and does not downgrade. A version string that does not parse is an error, never an
assumption that an update exists.

### When GitLab is unreachable: the GitHub fallback

The client compiles in a second address for its update metadata, the fixed
GitHub release slot `…/releases/download/update-latest/…`, and reads it only
after the canonical host failed to answer. The release pipeline writes that
slot after every GitLab promotion (`mirror-update-manifest-to-github`), and
every package is already mirrored per release, so with GitLab unreachable —
or answering with a document that does not verify, which is what a lapsed
domain or a hijacking proxy looks like — the whole path — manifest,
signature, artifacts — is served by GitHub and verified by the same
compiled-in Ed25519 key. One retry, once per check, and the mirror copy is
held to the identical verification. GitHub decides nothing: the client reads no
release metadata, calls no API, and follows the fixed path only. The
document fetcher accepts a redirect from `github.com` to GitHub's asset
object hosts for that fallback, and for nothing else; the canonical fetch
still refuses to leave the canonical host. Before 2026-09-03 the fallback
was doubly inert: no pipeline job wrote the slot, and the fetcher refused the
mirror host for metadata even when asked for it.

## Installation type detection

Lightning must know how it was installed before it can offer to update itself,
and it does not guess from directory names. The package build stamps an
explicit identifier into the binary (`LIGHTNING_INSTALL_TYPE`), and runtime
evidence overrides it only for the three ecosystems that can host any build:
`FLATPAK_ID` or `/.flatpak-info` means Flatpak, `SNAP`/`SNAP_NAME` means Snap,
and a valid `APPIMAGE` path means AppImage.

Identifiers: `windows-msi`, `windows-setup`, `windows-portable`,
`linux-appimage`, `linux-deb`, `linux-rpm`, `linux-flatpak`, `linux-snap`,
`macos-dmg`, `development`, `unknown`. The current type is shown in
Settings → Updates.

**A development build never installs an update.** A build from source reports
`development`, which permits a manual check only if explicitly enabled and
refuses installation outright, so a source tree can never turn itself into a
packaged installation.

## Behaviour by package format

| Format | What Lightning does |
|---|---|
| Windows MSI | Downloads and verifies the MSI, then the helper runs Windows Installer against it. The MSI is per-user with a stable UpgradeCode, so this is an ordinary major upgrade and needs no elevation. |
| Windows Setup EXE | Downloads and verifies the NSIS installer, then the helper runs it in the installer's documented silent mode. Per-user, so no elevation prompt. |
| Windows portable ZIP | Downloads and verifies the ZIP, extracts it to a staging directory with strict path checks, validates that the result really is a Lightning layout, then swaps directories and rolls back on any failure. |
| Linux AppImage | Downloads and verifies the new AppImage, preserves the executable bit, and atomically replaces the running AppImage, restoring the previous file if the replacement fails. |
| Linux DEB | Downloads and verifies the `.deb`, then hands it to the system package manager through PolicyKit. dpkg/APT stays the owner of every installed file. |
| Linux RPM | Downloads and verifies the `.rpm`, then hands it to `dnf5`/`dnf`/`rpm-ostree` (whichever exists) through PolicyKit. RPM stays the owner of every installed file. |
| Linux Flatpak | Nothing is downloaded. Flatpak owns this installation; Lightning says so and offers the correct command. |
| Linux Snap | Nothing is downloaded. Snap owns this installation and refreshes it itself; Lightning says so and offers the correct command. |

Lightning never copies files into `/usr`, never unpacks a `.deb` or `.rpm` over
the filesystem, never uses `rpm --force` or a `--force-*` dpkg option to make an
install succeed, never runs `sudo`, and never asks for your password. Where
elevation is genuinely required it uses PolicyKit, which prompts through your
desktop's normal mechanism. Nothing is ever passed through a shell: the helper
builds an argument vector, so a path containing spaces, quotes, parentheses or
non-ASCII characters is handled correctly and cannot be reinterpreted as a
command.

### Package channels versus a GitLab release

A GitLab release existing does not mean every downstream channel has published
it. Flatpak and Snap builds are produced today but are **not** published to
Flathub or the Snap Store, so the signed manifest marks those channels
unavailable and Lightning will not tell a Flatpak or Snap user that an update is
ready to install. The same applies to the APT and DNF repositories, which do not
exist yet.

## The updater helper

Some steps cannot happen while Lightning is running, so a small separate binary,
`lightning-updater`, is installed alongside it. It receives a fixed, validated
set of arguments — the install type, the path to an artifact Lightning has
already verified, the process ID to wait for, and where to write a status file —
waits for Lightning to exit, performs exactly one operation, and records the
result. It restarts Lightning only when you chose *Install and restart*; if you
chose to install without restarting, it applies the update and leaves the
application closed.

It accepts no commands and no scripts. It links only Qt Core (and zlib for
archive extraction): no network stack, and none of Lightning's Matrix, storage
or account code. It cannot download anything, cannot read a token, and cannot
open a Matrix store, because none of that code is in it.

### The verified bytes are re-checked at every hand-over

The download verifies the manifest's SHA-256 while the bytes stream in. After
that, the application and the helper are connected by nothing but a
filesystem path, and that path can sit armed for hours on the
"install when I quit" path before `pkexec dpkg -i <path>` reads it as root.
So the digest is taken again at each hand-over: the application re-hashes the
staged file right before it launches the helper (and again at quit, for the
deferred path, recording a refusal in the status file since the UI is gone by
then), and the helper receives the manifest's digest on its argv as
`--sha256 <64 hex>` and re-hashes the file itself immediately before acting.
A mismatch installs nothing, deletes the staged file, and is reported on the
next launch as `artifact-digest-mismatch` (a file that has simply vanished
reports `artifact-missing`). The helper also refuses a symbolic link for every
path option, and the application resolves the target and relaunch paths
before handing them over, so a link cannot redirect a chmod, a replace, or a
package-manager read. One consequence: if `~/Applications/Lightning.AppImage`
is a symlink to a versioned file, the *pointed-to* file is now replaced
rather than the link — better, but if it sits somewhere unwritable the
update fails where it used to succeed by clobbering the link.

## Recovery, rollback and concurrency

For the two formats Lightning replaces itself (portable ZIP and AppImage) the
replacement is transactional: the previous version is moved aside first and
restored if anything fails, so an interrupted update never leaves half of one
version and half of another. If the install directory is not writable, that is
reported clearly rather than partially attempted.

For MSI, Setup EXE, DEB and RPM, the installer or package manager owns the
transaction and Lightning does not attempt a second one on top of it.

Your data is never touched by an update. Matrix sessions, crypto stores,
settings and saved media live outside the installation directory and are neither
moved nor deleted by any update path.

If more than one Lightning instance is running, an update lock ensures only one
of them can download or install; the others report that an update is already in
progress.

On the next start after an update, Lightning reads the helper's small status
file so it can tell you whether the update succeeded, failed, or was rolled
back. That report is cleared once you have seen it and is never shown again.

## Release notes

Release notes come from the signed manifest or the release page and are treated
as untrusted remote text. They are not rendered as HTML and cannot run scripts
or invoke application commands.

## Validating the first real upgrade (0.7.1 -> 0.7.2)

Nothing in this document has been exercised by a real upgrade yet. 0.7.1 is
the first release that can be updated *from*, so the upgrade **into** 0.7.2
is the first end-to-end test of the whole chain, and it should be treated as
a test rather than assumed to work.

Do this on Windows first, because all three Windows formats install
differently and none of them has ever been driven by the updater on a real
machine.

### Before publishing 0.7.2

Have a 0.7.1 install that you did **not** build locally — download the
published artifact, so the thing being upgraded is the same bytes a user
has. Sign in, join a room, send a message, and leave the account signed in.
That matters: the update must not disturb the session, the store, or
settings, and you cannot tell whether it did unless there was something
there to disturb.

Note the install location and, for the portable ZIP, take a copy of the
whole directory. Rolling back a portable install is a directory swap; having
the original makes a failed run recoverable in seconds.

### Per format

Repeat for **MSI**, **Setup EXE** and **portable ZIP** separately. They take
three different code paths and passing one says nothing about the others.

1. Settings -> Updates -> *Check for updates*. It should report 0.7.2.
   If it reports nothing, the manifest is the first thing to look at, not
   the client: fetch it yourself and check its `version`.
2. Install. For the MSI and Setup EXE this needs no elevation prompt —
   **if Windows asks for administrator rights, that is a defect**, not a
   normal step, because both are per-user installs.
3. After the restart, confirm all of:
   - `--version` reports 0.7.2;
   - you are **still signed in** — a re-login prompt means the session or
     the store was disturbed and is a serious defect;
   - the room you left open still has its history, and encrypted rooms
     still decrypt (this is the check that proves the crypto store
     survived);
   - your settings — theme, text size, notification modes — are unchanged.
4. For the portable ZIP specifically, confirm the directory really was
   swapped and not merged: no stale files from 0.7.1 should remain.

### What to capture if something fails

The updater writes a status file; its path is in the update logs. Capture
that, the application log, and the exact point the run stopped. A failure
after verification but during installation is a very different defect from a
verification failure, and the status file is what distinguishes them.

Do not work around a failure by installing 0.7.2 by hand and calling the
update tested — that is the one outcome that would leave this document
saying something untrue.

### Linux

The AppImage, DEB and RPM paths are equally untested. The AppImage is the
cheapest to try (it replaces a single file and restores it on failure). The
DEB and RPM paths hand the package to the system package manager through
PolicyKit, so expect a normal desktop authentication prompt — and confirm
afterwards that `dpkg -S` / `rpm -qf` still report the package as owning its
files, which is the property that whole design exists to preserve.

## Signing status, stated honestly

The signed update manifest is Lightning's integrity and authenticity guarantee
today, on every platform.

Windows packages are **not** Authenticode-signed. The infrastructure for it is
prepared but not active, and no signing identity exists yet. When Windows code
signing becomes available, Lightning will verify it as an *additional* layer;
it will not replace manifest verification. Linux packages and repository
metadata are likewise not GPG-signed today, because no APT or DNF repository is
published. Where a native package manager does verify signatures, Lightning
never disables or bypasses those checks — the signed manifest complements them.

Release signing keys, rotation and the compromise procedure are documented in
the deployment repository, in `docs/update-manifest.md`.
