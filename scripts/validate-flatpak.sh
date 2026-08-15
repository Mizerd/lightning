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

APP_ID=net.smetonis.Lightning
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
        "QT_QPA_PLATFORM=offscreen exec /app/bin/matrix-client $*"
}

version_output=$(run_app --version)
[ "$version_output" = "matrix-client $BASE_VERSION" ] \
    || die "--version mismatch: $version_output"

set +e
timeout 20s flatpak run --user --command=sh "$APP_ID" -c \
    "cd /tmp && QT_QPA_PLATFORM=offscreen exec /app/bin/matrix-client --backend=rust" \
    > dist/flatpak-launch.log 2>&1
status=$?
set -e
[ "$status" = 0 ] || [ "$status" = 124 ] \
    || { cat dist/flatpak-launch.log; die "offscreen launch failed ($status)"; }
grep -Eiq "module .* is not installed|could not find|failed to load|error while loading shared libraries" \
    dist/flatpak-launch.log && { cat dist/flatpak-launch.log; die "launch reported missing components"; }

# GIF provider state with every key variable unset (embedded keys only).
gif_env_clear="env -u GIPHY_API_KEY -u KLIPY_API_KEY \
 -u LIGHTNING_GIPHY_API_KEY -u LIGHTNING_KLIPY_API_KEY \
 -u LIGHTNING_BUILD_GIPHY_API_KEY -u LIGHTNING_BUILD_KLIPY_API_KEY"
flatpak run --user --command=sh "$APP_ID" -c \
    "$gif_env_clear QT_QPA_PLATFORM=offscreen /app/bin/matrix-client --gif-status" \
    | tee dist/flatpak-gif-status.txt
if [ "${PUBLISH_PACKAGES:-false}" = "true" ]; then
    grep -q "GIPHY configured: yes" dist/flatpak-gif-status.txt || die "GIPHY not embedded"
    grep -q "KLIPY configured: yes" dist/flatpak-gif-status.txt || die "KLIPY not embedded"
    flatpak run --user --command=sh "$APP_ID" -c \
        "$gif_env_clear QT_QPA_PLATFORM=offscreen /app/bin/matrix-client --gif-selftest" \
        | tee dist/flatpak-gif-selftest.txt
    grep -q "GIPHY request: ok" dist/flatpak-gif-selftest.txt || die "GIPHY selftest failed"
    grep -q "KLIPY request: ok" dist/flatpak-gif-selftest.txt || die "KLIPY selftest failed"
    grep -Eq "api_key=|://" dist/flatpak-gif-status.txt dist/flatpak-gif-selftest.txt \
        && die "GIF output leaked a URL or key"
fi

# Leak audit inside the mounted app tree.
appdir="$FLATPAK_USER_DIR/app/$APP_ID/current/active/files"
test -d "$appdir" || die "installed app files missing"
# The update helper comes from the source's own install(TARGETS ...) rule. A
# Flatpak is updated by Flatpak, so Lightning never RUNS the helper here — but
# its presence proves the install rule placed BOTH executables, and its absence
# would mean that rule had failed in every format built the same way.
test -x "$appdir/bin/lightning-updater" || die "update helper missing from the app tree"
grep -RIl -e /nix/store -e /home/roksme -e /builds/ \
    -e 'LIGHTNING_GIPHY_API_KEY=' -e 'LIGHTNING_KLIPY_API_KEY=' \
    -e 'PRIVATE-TOKEN:' -e 'recovery_key=' "$appdir" \
    && die "forbidden path or credential marker in app tree"
# The exported build manifest (files/manifest.json) legitimately references
# the flatpak-internal /run/build/<module> sandbox paths in its own build
# options — every flatpak bundle carries them, and the credential/private
# markers above still apply to it. Any OTHER file referencing /run/build
# would mean RPATH/debug-path leakage and stays fatal.
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
