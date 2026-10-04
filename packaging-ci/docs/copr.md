# Fedora COPR (`mizerd/lightning-matrix`)

Fedora COPR builds Lightning from source for each Fedora release, so dnf and
the software centre keep it up to date with the rest of the system. It is the
same rpm as the release pipeline's in everything a user installs: the same
runtime dependencies (`tests/test-copr-spec-parity.py`), the same fail-closed
CMake options, the same portable Qt ABI. Two things differ on purpose:

- **Install type `linux-rpm-repo`.** The in-app updater treats it like Flatpak
  and Snap: it never downloads or installs anything, says dnf manages the
  installation, and shows `sudo dnf upgrade --refresh lightning-matrix`. The GitLab
  `.rpm` stays `linux-rpm` and keeps its automatic install. Nothing at runtime
  tells the two apart, so only the build can say which one it is.
- **No GIF provider keys.** The source RPM is public, so a COPR build has
  none; the GIF picker shows its missing-key state unless the user sets
  `LIGHTNING_GIPHY_API_KEY` / `LIGHTNING_KLIPY_API_KEY`.

## How a build happens

```
tag push vX.Y.Z on GitLab project 6
  -> GitLab webhook (Tag push events) -> COPR package "lightning-matrix"
  -> COPR clones the tag and runs, as root in a Fedora mock chroot with network:
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
2. *Packages → New package → SCM*: name `lightning`, clone URL
   `https://gitlab.smetonis.net/Mizerd/lightning.git`, committish `main`, SRPM
   build method `make srpm`, *Auto-rebuild* ON. The same with copr-cli:
   `copr-cli add-package-scm mizerd/lightning-matrix --name lightning-matrix --clone-url https://gitlab.smetonis.net/Mizerd/lightning.git --commit main --method make_srpm --webhook-rebuild on`
3. First build by hand (*Rebuild*, or
   `copr-cli build-package mizerd/lightning-matrix --name lightning`); watch
   `builder-live.log`, and check the `%check` output for
   `call media engine built in: yes`.
4. *Settings → Integrations*: copy the GitLab webhook URL. In GitLab, project 6
   → *Settings → Webhooks*: that URL with `lightning/` appended (the package
   name; our tags are `vX.Y.Z`, not `lightning-X.Y.Z`), trigger **Tag push
   events** only. Push events would rebuild `main` on every commit.

Or upload a source RPM instead of steps 2-4: the `copr-srpm` job keeps one as
an artifact for a week (`dist/copr/`), and
`packaging-ci/scripts/build-copr-srpm.sh <outdir>` makes one on any machine
with git, cargo, rpmbuild and network; then
`copr-cli build mizerd/lightning-matrix <file>.src.rpm`.

## Each release

Nothing, if the webhook is set: the tag push starts the build. The signed
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

- A build on COPR itself: only the equivalent mock chroot above has built it.
- A COPR build of any tree after v0.9.9, and of `linux-rpm-repo` and
  `-DCMAKE_SKIP_INSTALL_RPATH=ON` at all: the spec's current form has been
  assembled into a source RPM (`copr-srpm`), not compiled.
- `aarch64`, and `fedora-rawhide`.
- The webhook route end to end.
