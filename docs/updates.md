# Application updates

Lightning can tell you when a newer release exists and, for the package types
that allow it, install that release for you. This document describes exactly
what it contacts, what it sends, how an update is verified, and what happens
for each package format.

Historically Lightning did not check for updates at all. That is no longer
true, so the privacy section below is part of the contract, not a footnote.

## What Lightning contacts, and what it sends

One host: the canonical Lightning GitLab, `gitlab.smetonis.net`, over HTTPS.
Nothing else. Not the GitHub mirror, which is a read-only mirror of git refs
and is never authoritative for updates.

An update check is an ordinary anonymous HTTPS GET of two small public files
from the project's package registry:

```
.../api/v4/projects/6/packages/generic/lightning-update/latest/update-manifest-v1.json
.../api/v4/projects/6/packages/generic/lightning-update/latest/update-manifest-v1.json.sig
```

The request carries the URL and Lightning's update user agent, which is exactly
`Lightning/<version>` — the application version and nothing else. That is all.
Specifically, an update check **never** includes:

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

**Automatic** — off by default. When you turn it on, Lightning checks at most
once every 24 hours, never during the first 30 seconds after launch, and never
in response to switching room or account. Checks are asynchronous: startup is
never delayed waiting for the network.

## How an update is verified

The trust chain, in order. Every step must pass; there is no way to continue
past a failure, and the interface deliberately offers none.

```
public key compiled into Lightning
  -> Ed25519 signature over the exact manifest bytes
  -> SHA-256 of the exact artifact, taken from the signed manifest
  -> bytes verified after download
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
4. **A mismatch deletes the download and stops.** Invalid signature, unknown key,
   wrong hash, wrong size, unsupported schema, or a URL that is not HTTPS on the
   expected host are all terminal.

### The manifest cannot tell Lightning what to run

This is the property that matters most. The manifest describes *which file*,
*how big*, and *what hash* — never *what to do*. There is no command, argument,
script or URL-to-execute field, and Lightning has no code path that would
execute one. Every installation strategy below is compiled into Lightning and
the updater helper. Remote metadata selects among fixed, trusted behaviours; it
can never define one.

### Version comparison

Semantic, never lexicographic, so `0.10.0` is correctly newer than `0.9.0`.
Prerelease versions sort below their release. On the stable channel a
prerelease is ignored entirely. If the published version equals yours, or is
older than yours, Lightning reports that you are up to date and does not
downgrade. A version string that does not parse is an error, never an
assumption that an update exists.

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
