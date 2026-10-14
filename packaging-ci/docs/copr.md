# Fedora COPR (`mizerd/lightning-matrix`)

Fedora COPR builds Lightning from source for each Fedora release, so dnf and
the software centre keep it up to date with the rest of the system. It is the
same rpm as the release pipeline's in everything a user installs: the same
runtime dependencies (`tests/test-copr-spec-parity.py`), the same fail-closed
CMake options, the same portable Qt ABI. Two things differ on purpose:

- **Install type `linux-rpm-repo`.** The in-app updater treats it like Flatpak
  and Snap: it never downloads or installs anything, says dnf manages the
  installation, and shows `sudo dnf upgrade --refresh lightning-matrix` (on
  openSUSE, where `/usr/bin/zypper` exists and no dnf does: zypper, and
  `sudo zypper refresh && sudo zypper update lightning-matrix`). The release
  `.rpm` stays `linux-rpm` and keeps its automatic install. Nothing at runtime
  tells the two apart, so only the build can say which one it is.
- **No GIF provider keys.** The source RPM is public, so a COPR build has
  none; the GIF picker shows its missing-key state unless the user sets
  `LIGHTNING_GIPHY_API_KEY` / `LIGHTNING_KLIPY_API_KEY`.

## How a build happens

```
tag vX.Y.Z created on GitHub (the project 6 push mirror delivers it)
  -> GitHub webhook (the "Branch or tag creation" event only)
  -> COPR /webhooks/github/<id>/<uuid>/lightning-matrix/ -> package "lightning-matrix"
  -> COPR clones https://github.com/Mizerd/lightning.git at the tag and runs, as root in a Fedora mock chroot with network:
       make -f .copr/Makefile srpm outdir=<dir> spec=<ignored>
         -> packaging-ci/scripts/build-copr-srpm.sh
              git archive HEAD, cargo vendor --locked, stamp the spec
              (packaging-ci/packaging/rpm/lightning-copr.spec.in)
  -> one mock build per enabled chroot, without network, from that SRPM
```

A release tag builds as `X.Y.Z-1.fcNN.coprN`; anything else as a snapshot,
`X.Y.Z^YYYYMMDDgit<sha7>`, which sorts after the release it builds on and
before the next one.

The `copr-srpm` pipeline job runs the same `make` target on the pinned source
of every rpm pipeline and compares the dependencies rpm expands from both
specs, so a COPR build that would break shows up in the pipeline first. It
compiles nothing and gates no publication.

## What the build needs, measured

Measured 2026-09-29 in a COPR-equivalent mock chroot (fedora-44-x86_64, 4
cores, 8 GB, no swap), v0.9.9: **41 minutes**, of which Rust 30 at two crates
at a time; peak 7.6 GB, all of it the single fat-LTO `rustc`. COPR's standard
builders have 4 vCPU and 7.6 GiB with 143 GiB of swap, so that link swaps
there; expect one to two hours. Two Fedora-only failures, both fixed in the
spec and both invisible to the release pipeline:

- `aws-lc-sys` links a feature probe with Fedora's hardened `LDFLAGS` but
  without its `CFLAGS` (`recompile with -fPIE`): the spec unsets `LDFLAGS`
  after `%cmake`, which has already cached them for the C++ link.
- Four `rustc` beside the C++ compile passed 8 GB and were OOM-killed: the
  spec builds `matrix-client-rust-build` first with `CARGO_BUILD_JOBS=2`.

`qt_standard_project_setup()` asks for `$ORIGIN` install RPATHs, which the
release pipeline strips with `patchelf`; the COPR spec passes
`-DCMAKE_SKIP_INSTALL_RPATH=ON` and its `%check` refuses an RPATH.

## One-time setup (maintainer only)

Everything here is outward-facing and done by the maintainer, by hand.

1. At <https://copr.fedorainfracloud.org/coprs/mizerd/lightning-matrix/edit/>:
   chroots `fedora-44-x86_64` and `fedora-45-x86_64` (add `fedora-rawhide-x86_64`
   if wanted; `aarch64` only after an x86_64 build has passed, it has never been
   built); keep *Follow Fedora branching*; *Enable internet access during
   builds* OFF (the crates are vendored into the SRPM; the SRPM step has
   network regardless). The default 5-hour build timeout is enough.
2. *Packages → New package → SCM*: name `lightning-matrix`, clone URL
   `https://github.com/Mizerd/lightning.git`, committish `main`, SRPM
   build method `make srpm`, *Auto-rebuild* ON. The same with copr-cli:
   `copr-cli add-package-scm mizerd/lightning-matrix --name lightning-matrix --clone-url https://github.com/Mizerd/lightning.git --commit main --method make_srpm --webhook-rebuild on`
3. First build by hand (*Rebuild*, or
   `copr-cli build-package mizerd/lightning-matrix --name lightning-matrix`); watch
   `builder-live.log`, and check the `%check` output for
   `call media engine built in: yes`.
4. *Settings → Integrations*: copy the **GitHub** webhook URL
   (`https://copr.fedorainfracloud.org/webhooks/github/<id>/<uuid>/`). In GitHub,
   `Mizerd/lightning` → *Settings → Webhooks → Add webhook*: that URL with
   `lightning-matrix/` appended (the package name), content type
   `application/json` (COPR reads `flask.request.json`), and under *Let me select
   individual events* tick **Branch or tag creation** (the `create` event) and
   nothing else. Remove the old GitLab webhook (project 6 → *Settings →
   Webhooks*) and, for an existing package, repoint it:
   `copr-cli edit-package-scm mizerd/lightning-matrix --name lightning-matrix --clone-url https://github.com/Mizerd/lightning.git --commit main --method make_srpm --webhook-rebuild on`.

   **Why `create`, and not `push` or `release`** (read from COPR's frontend,
   `webhooks_ns/webhooks_general.py` and `logic/packages_logic.py`, 2026-10-13):
   the GitHub endpoint takes `ref_type` and `ref` straight from the payload, and
   only the `create` event carries them (`ref_type: "tag"`, `ref: "v0.10.2"`).
   With `ref_type == "tag"` and the package name in the URL the package always
   matches, and the build's committish is the tag. A `push` event has no
   `ref_type`, so COPR treats it as a commit push: a push to `main` that touches
   any file rebuilds `main` (the package's committish is `main`), and a tag push
   (`ref: refs/tags/...`, no commits) matches nothing, so it would never build
   the tag. A `release` event has neither field and builds nothing. `create` is
   delivered when GitHub receives the tag, which is exactly "after the tag exists
   on GitHub"; the mirror's sync of `main` creates no `create` event, so `main`
   is not rebuilt per commit. Branch creations also arrive (`ref_type: "branch"`)
   and match nothing. Two caveats: GitHub sends no `create` events when more than
   three tags are pushed in one go (a first full sync of old tags), which is fine
   because only new release tags matter; and a delivery can be redelivered from
   *Settings → Webhooks → Recent deliveries* if COPR was down.

   Not chosen: a pipeline job calling COPR's `custom` webhook after
   `mirror-release-to-github`. The custom webhook sends no tag (it rebuilds the
   package with its configured committish, `main`), so building a tag that way
   needs `copr-cli`/API credentials in CI. The `create` webhook needs no pipeline
   job and no secret outside GitHub's webhook settings and COPR.

Or upload a source RPM instead of steps 2-4: the `copr-srpm` job keeps one as
an artifact for a week (`dist/copr/`), and
`packaging-ci/scripts/build-copr-srpm.sh <outdir>` makes one on any machine
with git, cargo, rpmbuild and network; then
`copr-cli build mizerd/lightning-matrix <file>.src.rpm`.

## openSUSE chroots (Tumbleweed and Leap 16.0)

COPR builds for openSUSE too, from the same source RPM: the spec's
`%{?suse_version}` branch names openSUSE's packages (`cmake(Qt6…)`,
`pkgconfig(gstreamer-…)`, `libopenssl-devel` — not `pkgconfig(openssl)`, which
LibreSSL also provides and zypper picked on Leap) and calls CMake directly,
since openSUSE's `%cmake` comments out every `set(CMAKE_BUILD_TYPE …)` in the
tree and picks Makefiles. The runtime `Requires` are the same lines as
Fedora's; they were written by capability for exactly this.

Why bother when the release `.rpm` installs on Tumbleweed: **Leap 16.0 cannot
install it** (Qt 6.9.1 against the release `.rpm`'s Qt 6.11 floor) and is
otherwise left with the Flatpak; and a COPR build links openSUSE's own
libraries and is updated by zypper with the rest of the system.

Measured 2026-10-06 in rig containers (`opensuse/leap:16.0`: Qt 6.9.1,
GStreamer 1.26.7, GCC 15, Rust 1.98; Tumbleweed: Qt 6.11.2, GStreamer
1.28.7), Rust from the distribution: the source RPM from `build-copr-srpm.sh`
compiles through `rpmbuild --rebuild` on both, about 40 minutes each at -j8.
The Leap package passes `validate-rpm.sh` on a clean Leap 16.0; the Tumbleweed
package (29 MB, stripped) installs over the 0.10.0 `lightning` rpm on a
Tumbleweed KDE desktop and calls (`docs/open-items.md`).
The pipeline repeats the Leap half on every rpm pipeline: `build-rpm-opensuse`
compiles `copr-srpm`'s source RPM on Leap 16.0 and `validate-rpm-leap`
installs it on a clean Leap 16.0. Both are canaries; neither gates a release.

**What the maintainer clicks (once):** at
<https://copr.fedorainfracloud.org/coprs/mizerd/lightning-matrix/edit/> tick
the chroots `opensuse-tumbleweed-x86_64` and `opensuse-leap-16.0-x86_64`, save,
then *Rebuild* the latest build (or wait for the next release tag) so the two new
chroots get packages. Nothing else changes: same package, same webhook, same
SRPM method. `aarch64` only after x86_64 is green.

Users then add the repository (form of COPR's generated `.repo` files; check
the links on the project page once the chroots exist):

```sh
# Tumbleweed
sudo zypper addrepo https://copr.fedorainfracloud.org/coprs/mizerd/lightning-matrix/repo/opensuse-tumbleweed/mizerd-lightning-matrix-opensuse-tumbleweed.repo
# Leap 16.0
sudo zypper addrepo https://copr.fedorainfracloud.org/coprs/mizerd/lightning-matrix/repo/opensuse-leap-16.0/mizerd-lightning-matrix-opensuse-leap-16.0.repo

sudo zypper --gpg-auto-import-keys refresh
sudo zypper install lightning-matrix
```

These lines join `docs/install.md` only after a green COPR build in both
chroots, as the Fedora ones do.

## Each release

Nothing, if the webhook is set: the tag arriving on GitHub starts the build. The signed
update manifest's `linux-rpm-repo` channel is `false` by default, which is
what a COPR install reads; see `docs/update-manifest.md` for the two ways to
set it `true`.

A Fedora Qt **minor** update no longer needs a rebuild: the package imports no
Qt private-ABI symbol, and the Qt version tag is a floor, which a newer Qt
still satisfies. A new Fedora release needs its chroot enabled (*Follow
Fedora branching* does that).

## GitLab rpm and COPR on one machine

Both are named `lightning`. rpm compares `0.9.9-1.fc44.copr<N>` as newer than
the GitLab `0.9.9-1`, so enabling the COPR replaces a downloaded rpm at the
next `dnf upgrade`; a later GitLab `0.9.10-1` would outrank COPR's `0.9.9`
again. Users should pick one.

## Not tested

- Any build in COPR's openSUSE chroots: only the Leap 16.0 rig container
  above has compiled the spec's openSUSE branch, and Tumbleweed only as an
  install of that Leap build. The `.repo` URLs above are COPR's naming
  pattern, not yet fetched. The zypper wording of the in-app updater on a
  COPR install is unit-tested, not seen on screen.
- A build on COPR itself: only the equivalent mock chroot above has built it.
  (Seen 2026-10-06 through COPR's API, not reviewed: build 11073358,
  `0.10.0^20261004git21b6234`, succeeded for fedora-44 and fedora-45; the
  project has *internet access during builds* ON, which step 1 says to keep
  OFF.)
- A COPR build of any tree after v0.9.9, and of `linux-rpm-repo` and
  `-DCMAKE_SKIP_INSTALL_RPATH=ON` at all: the spec's current form has been
  assembled into a source RPM (`copr-srpm`), not compiled.
- `aarch64`, and `fedora-rawhide`.
- The webhook route end to end (the GitHub `create` route is read from COPR's
  source, not exercised).
