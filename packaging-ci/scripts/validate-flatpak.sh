#!/usr/bin/env bash
# Clean-system validation of the Flatpak bundle: install into a disposable
# per-job user installation, run the installed app offscreen with the mock
# backend, check the GIF provider state, audit for leaks, and uninstall.
# Runs on package-runner-flatpak (bwrap needs the relaxed-seccomp runner).
set -euo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lib.sh
. "$SCRIPT_DIR/lib.sh"
ROOT=$(project_dir)
cd "$ROOT"
load_versions

APP_ID=org.lightning_matrix.Lightning
FLATHUB_REPO=https://dl.flathub.org/repo/flathub.flatpakrepo

count=$(find dist -maxdepth 1 -name '*.flatpak' | wc -l)
[ "$count" = 1 ] || die "expected exactly one .flatpak in dist, found $count"
bundle="dist/lightning_${LOGICAL_VERSION}_amd64.flatpak"
test -f "$bundle" || die "expected bundle name $bundle"
test ! -e work/lightning || die "validation must not see a source checkout"
command -v nix >/dev/null 2>&1 && die "validation image must not provide nix"
( cd dist && sha256sum -c "$(basename "$bundle").sha256" )

# Disposable per-job installation; the shared cache only provides the
# runtime download so validation stays fast.
export FLATPAK_USER_DIR=$(mktemp -d)
cleanup() { rm -rf "$FLATPAK_USER_DIR"; }
trap cleanup EXIT

flatpak remote-add --user --if-not-exists flathub "$FLATHUB_REPO"
flatpak install --user --noninteractive --bundle "$bundle"
flatpak info --user "$APP_ID" | tee dist/flatpak-info.txt
grep -q "GPL-3.0-or-later" dist/flatpak-info.txt \
    || echo "note: licence not shown by flatpak info"

run_app() {
    flatpak run --user --command=sh "$APP_ID" -c \
        "QT_QPA_PLATFORM=offscreen exec /app/bin/lightning-matrix $*"
}

version_output=$(run_app --version)
[ "$version_output" = "Lightning $BASE_VERSION" ] \
    || die "--version mismatch: $version_output"

set +e
timeout 20s flatpak run --user --command=sh "$APP_ID" -c \
    "cd /tmp && QT_QPA_PLATFORM=offscreen exec /app/bin/lightning-matrix --backend=rust" \
    > dist/flatpak-launch.log 2>&1
status=$?
set -e
[ "$status" = 0 ] || [ "$status" = 124 ] \
    || { cat dist/flatpak-launch.log; die "offscreen launch failed ($status)"; }
grep -Eiq "module .* is not installed|could not find|failed to load|error while loading shared libraries" \
    dist/flatpak-launch.log && { cat dist/flatpak-launch.log; die "launch reported missing components"; }

# Ask the installed Flatpak for its call media engine. The plugins come from
# the runtime, so this also catches a runtime bump that drops one.
set +e
timeout 60s flatpak run --user --command=sh "$APP_ID" -c \
    "cd /tmp && QT_QPA_PLATFORM=offscreen exec /app/bin/lightning-matrix --call-media-status" \
    > dist/flatpak-call-media-status.txt 2>&1
call_media_status=$?
set -e
assert_call_media_engine Flatpak dist/flatpak-call-media-status.txt "$call_media_status"

# Voice-delay self-test; see assert_queue_selftest in lib.sh.
set +e
timeout 180s flatpak run --user --command=sh "$APP_ID" -c \
    "cd /tmp && QT_QPA_PLATFORM=offscreen exec /app/bin/lightning-matrix --call-queue-selftest" \
    > dist/flatpak-queue-selftest.txt 2>&1
queue_selftest_status=$?
set -e
assert_queue_selftest Flatpak dist/flatpak-queue-selftest.txt "$queue_selftest_status"

# Call sounds, warn-only: without an output device QSoundEffect never reaches
# Ready, so this is normally unmeasured here. See assert_call_sounds_status.
set +e
timeout 60s flatpak run --user --command=sh "$APP_ID" -c \
    "cd /tmp && QT_QPA_PLATFORM=offscreen exec /app/bin/lightning-matrix --call-sounds-status" \
    > dist/flatpak-call-sounds-status.txt 2>&1
call_sounds_status=$?
set -e
assert_call_sounds_status Flatpak dist/flatpak-call-sounds-status.txt "$call_sounds_status"

# Image decoders come from org.kde.Platform; catch a runtime bump dropping one.
set +e
timeout 60s flatpak run --user --command=sh "$APP_ID" -c \
    "cd /tmp && QT_QPA_PLATFORM=offscreen exec /app/bin/lightning-matrix --image-format-status" \
    > dist/flatpak-image-format-status.txt 2>&1
image_format_status=$?
set -e
assert_image_formats Flatpak dist/flatpak-image-format-status.txt "$image_format_status" jxl

# GIF provider state with every key variable unset (embedded keys only).
gif_env_clear="env -u GIPHY_API_KEY -u KLIPY_API_KEY \
 -u LIGHTNING_GIPHY_API_KEY -u LIGHTNING_KLIPY_API_KEY \
 -u LIGHTNING_BUILD_GIPHY_API_KEY -u LIGHTNING_BUILD_KLIPY_API_KEY"
flatpak run --user --command=sh "$APP_ID" -c \
    "$gif_env_clear QT_QPA_PLATFORM=offscreen /app/bin/lightning-matrix --gif-status" \
    | tee dist/flatpak-gif-status.txt
if [ "${PUBLISH_PACKAGES:-false}" = "true" ]; then
    grep -q "GIPHY configured: yes" dist/flatpak-gif-status.txt || die "GIPHY not embedded"
    grep -q "KLIPY configured: yes" dist/flatpak-gif-status.txt || die "KLIPY not embedded"
    flatpak run --user --command=sh "$APP_ID" -c \
        "$gif_env_clear QT_QPA_PLATFORM=offscreen /app/bin/lightning-matrix --gif-selftest" \
        | tee dist/flatpak-gif-selftest.txt
    grep -q "GIPHY request: ok" dist/flatpak-gif-selftest.txt || die "GIPHY selftest failed"
    grep -q "KLIPY request: ok" dist/flatpak-gif-selftest.txt || die "KLIPY selftest failed"
    grep -Eq "api_key=|://" dist/flatpak-gif-status.txt dist/flatpak-gif-selftest.txt \
        && die "GIF output leaked a URL or key"
fi

# Leak audit inside the mounted app tree.
appdir="$FLATPAK_USER_DIR/app/$APP_ID/current/active/files"
test -d "$appdir" || die "installed app files missing"
# Flatpak never runs the update helper, but its presence proves the install
# rule placed both executables.
test -x "$appdir/bin/lightning-updater" || die "update helper missing from the app tree"
grep -RIl -e /nix/store -e /home/roksme -e /builds/ \
    -e 'LIGHTNING_GIPHY_API_KEY=' -e 'LIGHTNING_KLIPY_API_KEY=' \
    -e 'PRIVATE-TOKEN:' -e 'recovery_key=' "$appdir" \
    && die "forbidden path or credential marker in app tree"
# files/manifest.json legitimately references /run/build/<module>; any other
# file doing so means RPATH/debug-path leakage.
grep -RIl -e /run/build/ "$appdir" | grep -v '/manifest\.json$' | grep -q . \
    && die "forbidden /run/build reference outside the exported manifest"
find "$appdir" -name 'LightningGifBuildKeys.h' | grep -q . \
    && die "generated key header leaked into the app"
find "$appdir" -perm -0002 \( -type f -o -type d \) | grep -q . \
    && die "world-writable content"
test -f "$appdir/share/applications/$APP_ID.desktop" || die "app-id desktop file missing"
test -f "$appdir/share/metainfo/$APP_ID.metainfo.xml" || die "metainfo missing"
test -f "$appdir/share/icons/hicolor/128x128/apps/$APP_ID.png" || die "icon missing"

flatpak uninstall --user --noninteractive "$APP_ID"
flatpak info --user "$APP_ID" >/dev/null 2>&1 && die "uninstall left the app behind"
echo "flatpak validation passed"
