#!/usr/bin/env bash
set -Eeuo pipefail

# THE FLATHUB PRE-SUBMISSION GATE, RUN THE WAY FLATHUB RUNS IT.
#
# This is not a CI job. It is the local gate the maintainer asked for — "on the
# machine that supports flatpak, run the full lint and confirm it is good for
# submission" — and it exists as a tracked file for one reason: every time it
# has been hand-rolled, the flags came out wrong and the wrong conclusion got
# written down.
#
# WHAT WENT WRONG ON 2026-09-17, because this script is the fix for it.
#
# A rig driver invoked `flatpak-builder` directly with
# `--mirror-screenshots-url=https://dl.flathub.org/media/` and no
# `--compose-url-policy`. The default policy is `partial`, which makes
# appstreamcli compose write the catalogue as
# `<components media_baseurl="https://dl.flathub.org/media/">` plus RELATIVE
# image paths — and `flatpak-builder-lint` tests each `<image>` with a bare
# `startswith("https://dl.flathub.org/media")` and never resolves
# `media_baseurl` (verified in its own source: `checks/screenshots.py` and
# `appstream.is_remote_icon_mirrored`). So the repo lint reported
# `appstream-external-screenshot-url` and `appstream-remote-icon-not-mirrored`
# on a manifest that is fine.
#
# `--compose-url-policy=full` was then tried and recorded as REFUTED. It was
# not: that run's log says `Cache hit for cleanup, skipping`, and the cleanup
# stage is where flatpak-builder runs appstreamcli compose. The flag never
# reached the tool. Re-run with a cold cache it changes the output completely —
# `<components version="1.0">` with absolute `https://dl.flathub.org/media/...`
# URLs — and both errors go away. Measured 2026-09-19 on Lightning's own
# builddir: 20/20 image URLs and 2/2 remote icons absolute, `lint builddir` and
# `lint repo` both exit 0 with no output.
#
# GENERALISE: a flag tested over a cache hit was never tested. The stage that
# would have consumed it did not run.
#
# `flathub-build`, shipped inside the org.flatpak.Builder flatpak, passes the
# exact set Flathub's own buildbot uses — including BOTH flags — so this script
# calls that and never assembles its own argument list:
#
#   --verbose --force-clean --sandbox --keep-build-dirs
#   --override-source-date-epoch 1321009871 --user --install-deps-from=flathub
#   --ccache --mirror-screenshots-url=https://dl.flathub.org/media
#   --compose-url-policy=full --repo=repo builddir <manifest>
#
# THREE TRAPS THIS SCRIPT HANDLES, ALL PAID FOR ALREADY.
#
#  1. NO SESSION BUS, NO BUILD. flatpak-builder inside the org.flatpak.Builder
#     sandbox resolves its sdk by running `flatpak info` ON THE HOST through
#     the spawn portal, which needs a bus carrying org.freedesktop.Flatpak.
#     Without one it dies at init with "Unable to find sdk org.kde.Sdk version
#     6.11" while `flatpak info org.kde.Sdk//6.11` in the same shell prints the
#     ref. `dbus-run-session` is the whole fix; D-Bus activates the portal from
#     /usr/libexec/flatpak-portal by itself. Three builds were lost to this.
#  2. THE SANDBOX CANNOT WRITE A HOST-OWNED DIRECTORY. It maps host uid 1000 to
#     nobody, so a work directory owned by the invoking user is read-only
#     inside it and the lint dies with "Can't create state directory: ...
#     Permission denied" before it reads anything. That looks like a lint
#     failure and is not one.
#  3. `cmd | tail` MAKES `$?` THE STATUS OF `tail`. It reported "builder rc=0"
#     over a build that never started. Every status here comes from PIPESTATUS
#     or from the command itself.
#
# WHAT THIS DOES NOT ANSWER: whether the app WORKS. A lint reads metadata. The
# sandbox + portal call path — camera, screen share, audio both ways — has to
# be exercised by hand from the installed build; see docs/live-validation.md.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

WORKDIR="${FLATHUB_LINT_WORKDIR:-}"
[ -n "$WORKDIR" ] || die "set FLATHUB_LINT_WORKDIR to a scratch directory the flatpak sandbox can write (see trap 2 above); it will be emptied"

command -v flatpak >/dev/null 2>&1 || die "flatpak is not installed; this gate needs a host that supports it"
command -v dbus-run-session >/dev/null 2>&1 || die "dbus-run-session is not installed, and without a session bus the build dies claiming it cannot find the sdk (see trap 1 above)"

flatpak info org.flatpak.Builder >/dev/null 2>&1 \
    || die "org.flatpak.Builder is not installed: flatpak install -y flathub org.flatpak.Builder"

# The submission set is three files at the repository ROOT, deliberately, so
# the Flathub repository is a straight copy of them.
for f in org.lightning_matrix.Lightning.yaml cargo-sources.json flathub.json; do
    [ -f "$REPO_ROOT/$f" ] || die "missing submission file: $REPO_ROOT/$f"
done

rm -rf -- "$WORKDIR"
mkdir -p -- "$WORKDIR"
cp -- "$REPO_ROOT/org.lightning_matrix.Lightning.yaml" \
      "$REPO_ROOT/cargo-sources.json" \
      "$REPO_ROOT/flathub.json" "$WORKDIR/"
# 0777, not 0755: the sandbox runs as a different uid (trap 2).
chmod -R 0777 -- "$WORKDIR"

printf '== pin under test ==\n'
grep -E '^ *(url|tag|commit):' "$WORKDIR/org.lightning_matrix.Lightning.yaml" | head -6

printf '== toolchain ==\n'
dbus-run-session -- flatpak run --filesystem="$WORKDIR" \
    --command=flatpak-builder org.flatpak.Builder --version

printf '== manifest lint ==\n'
set +e
dbus-run-session -- flatpak run --filesystem="$WORKDIR" \
    --command=flatpak-builder-lint org.flatpak.Builder \
    manifest "$WORKDIR/org.lightning_matrix.Lightning.yaml"
manifest_rc=$?
set -e
printf 'manifest lint rc=%s\n' "$manifest_rc"

printf '== build (flathub-build, Flathub own flags) ==\n'
set +e
( cd "$WORKDIR" && dbus-run-session -- flatpak run --filesystem="$WORKDIR" \
    --command=flathub-build org.flatpak.Builder \
    org.lightning_matrix.Lightning.yaml ) >"$WORKDIR/build.log" 2>&1
build_rc=$?
set -e
printf 'build rc=%s (log: %s)\n' "$build_rc" "$WORKDIR/build.log"
tail -5 "$WORKDIR/build.log" || true

# A CACHE HIT ON `cleanup` MEANS THE CATALOGUE WAS NOT REGENERATED, and that is
# exactly how the 2026-09-17 refutation went wrong. Say so rather than let the
# next reader assume the flags applied.
if grep -q 'Cache hit for cleanup' "$WORKDIR/build.log"; then
    printf 'WARNING: the cleanup stage was served from cache, so appstreamcli compose did NOT re-run.\n'
    printf '         Any conclusion about the compose URL policy from this run is void.\n'
fi

[ "$build_rc" -eq 0 ] || die "the build failed; the repo lint below would be meaningless"

printf '== what the catalogue actually says ==\n'
catalogue="$(find "$WORKDIR/builddir/files/share/app-info/xmls" -name '*.xml.gz' | head -1)"
[ -n "$catalogue" ] || die "no appstream catalogue was produced"
gunzip -c "$catalogue" | sed -n 2p | cut -c1-140
images_total="$(gunzip -c "$catalogue" | grep -cE '<image[^>]*>' || true)"
images_ok="$(gunzip -c "$catalogue" | grep -cE '<image[^>]*>https://dl\.flathub\.org/media/' || true)"
icons_total="$(gunzip -c "$catalogue" | grep -cE '<icon type="remote"[^>]*>' || true)"
icons_ok="$(gunzip -c "$catalogue" | grep -cE '<icon type="remote"[^>]*>https://dl\.flathub\.org/media/' || true)"
printf 'image urls %s/%s absolute; remote icons %s/%s absolute\n' \
    "$images_ok" "$images_total" "$icons_ok" "$icons_total"
[ "$images_total" -gt 0 ] && [ "$images_ok" = "$images_total" ] \
    || die "the catalogue still carries relative image URLs: the compose URL policy did not apply, and the two appstream-*-url errors below are the harness rather than the manifest"

printf '== builddir lint ==\n'
set +e
dbus-run-session -- flatpak run --filesystem="$WORKDIR" \
    --command=flatpak-builder-lint org.flatpak.Builder builddir "$WORKDIR/builddir"
builddir_rc=$?
set -e
printf 'builddir lint rc=%s\n' "$builddir_rc"

printf '== repo lint ==\n'
set +e
dbus-run-session -- flatpak run --filesystem="$WORKDIR" \
    --command=flatpak-builder-lint org.flatpak.Builder repo "$WORKDIR/repo"
repo_rc=$?
set -e
printf 'repo lint rc=%s\n' "$repo_rc"

printf '== verdict ==\n'
printf 'manifest=%s builddir=%s repo=%s\n' "$manifest_rc" "$builddir_rc" "$repo_rc"
# A SILENT PASS IS NOT EVIDENCE. Mutate the manifest and confirm the linter
# fires before believing a clean run — `app-id` renamed gives
# appid-filename-mismatch, `--filesystem=host` added gives
# finish-args-host-filesystem-access. Both have been used here twice.
printf 'a clean run is only evidence once it has been mutation-checked; see the note at the end of this script\n'

[ "$manifest_rc" -eq 0 ] && [ "$builddir_rc" -eq 0 ] && [ "$repo_rc" -eq 0 ] \
    || die "one or more lints failed"
printf 'ALL THREE LINTS CLEAN\n'
