#!/usr/bin/env bash
set -Eeuo pipefail

# Local Flathub pre-submission gate (not a CI job): manifest, builddir and repo
# lint, run the way Flathub runs them.
#
# Build through `flathub-build` from org.flatpak.Builder so the flags match
# Flathub's buildbot exactly, in particular --compose-url-policy=full: with the
# default `partial` policy the catalogue gets a media_baseurl plus relative
# image paths, which flatpak-builder-lint reports as
# appstream-external-screenshot-url / appstream-remote-icon-not-mirrored.
#
# Traps handled here:
#  1. No session bus: flatpak-builder inside the sandbox resolves its sdk via
#     `flatpak info` on the host through the spawn portal, and without a bus it
#     fails with "Unable to find sdk". Run everything under dbus-run-session.
#  2. The sandbox maps the host uid to nobody, so a user-owned work directory
#     is read-only inside it ("Can't create state directory").
#  3. `cmd | tail` reports tail's status; statuses here come from the command.
#
# A lint reads metadata only; it says nothing about whether calls, camera or
# screen share work in the installed build.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

WORKDIR="${FLATHUB_LINT_WORKDIR:-}"
[ -n "$WORKDIR" ] || die "set FLATHUB_LINT_WORKDIR to a scratch directory the flatpak sandbox can write (see trap 2 above); it will be emptied"

command -v flatpak >/dev/null 2>&1 || die "flatpak is not installed; this gate needs a host that supports it"
command -v dbus-run-session >/dev/null 2>&1 || die "dbus-run-session is not installed, and without a session bus the build dies claiming it cannot find the sdk (see trap 1 above)"

flatpak info org.flatpak.Builder >/dev/null 2>&1 \
    || die "org.flatpak.Builder is not installed: flatpak install -y flathub org.flatpak.Builder"

# The submission set lives at the repository root so the Flathub repository
# is a straight copy of it.
for f in org.lightning_matrix.Lightning.yaml cargo-sources.json; do
    [ -f "$REPO_ROOT/$f" ] || die "missing submission file: $REPO_ROOT/$f"
done

rm -rf -- "$WORKDIR"
mkdir -p -- "$WORKDIR"
cp -- "$REPO_ROOT/org.lightning_matrix.Lightning.yaml" \
      "$REPO_ROOT/cargo-sources.json" "$WORKDIR/"
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

# A cache hit on `cleanup` means compose did not re-run, so the compose flags
# were never applied in this run.
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
# Mutation-check a clean run: renaming `app-id` should give
# appid-filename-mismatch, adding `--filesystem=host` should give
# finish-args-host-filesystem-access.
printf 'a clean run is only evidence once it has been mutation-checked; see the note at the end of this script\n'

[ "$manifest_rc" -eq 0 ] && [ "$builddir_rc" -eq 0 ] && [ "$repo_rc" -eq 0 ] \
    || die "one or more lints failed"
printf 'ALL THREE LINTS CLEAN\n'
