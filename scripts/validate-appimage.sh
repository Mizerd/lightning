#!/usr/bin/env bash
# Clean-system validation of the AppImage on a minimal Debian image that
# provides only the documented base-system libraries (no Qt): run
# --appimage-extract-and-run (no FUSE in CI), check --version, offscreen
# mock-backend startup, GIF provider state, and audit the payload. The
# "uninstall" equivalent for a single-file format is file removal, which is
# exercised implicitly; AppImage is unsandboxed and documented as such.
set -euo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lib.sh
. "$SCRIPT_DIR/lib.sh"
ROOT=$(project_dir)
cd "$ROOT"
load_versions

count=$(find dist -maxdepth 1 -name '*.AppImage' | wc -l)
[ "$count" = 1 ] || die "expected exactly one AppImage in dist, found $count"
app="dist/Lightning-${LOGICAL_VERSION}-x86_64.AppImage"
test -f "$app" || die "expected AppImage name $app"
test ! -e work/lightning || die "validation must not see a source checkout"
command -v nix >/dev/null 2>&1 && die "validation image must not provide nix"
( cd dist && sha256sum -c "$(basename "$app").sha256" )

chmod +x "$app"
file "$app" | grep -q 'ELF 64-bit' || die "not an ELF AppImage"
export APPIMAGE_EXTRACT_AND_RUN=1

version_output=$(cd /tmp && "$ROOT/$app" --version)
[ "$version_output" = "matrix-client $BASE_VERSION" ] \
    || die "--version mismatch: $version_output"

set +e
( cd /tmp && timeout 20s env QT_QPA_PLATFORM=offscreen \
    "$ROOT/$app" --backend=mock ) > dist/appimage-launch.log 2>&1
status=$?
set -e
[ "$status" = 0 ] || [ "$status" = 124 ] \
    || { cat dist/appimage-launch.log; die "offscreen launch failed ($status)"; }
grep -Eiq "module .* is not installed|could not find|failed to load|error while loading shared libraries" \
    dist/appimage-launch.log && { cat dist/appimage-launch.log; die "launch reported missing components"; }

gif_env_clear() {
    unset GIPHY_API_KEY KLIPY_API_KEY \
        LIGHTNING_GIPHY_API_KEY LIGHTNING_KLIPY_API_KEY \
        LIGHTNING_BUILD_GIPHY_API_KEY LIGHTNING_BUILD_KLIPY_API_KEY \
        2>/dev/null || true
}
( gif_env_clear; cd /tmp && QT_QPA_PLATFORM=offscreen "$ROOT/$app" --gif-status ) \
    | tee dist/appimage-gif-status.txt
if [ "${PUBLISH_PACKAGES:-false}" = "true" ]; then
    grep -q "GIPHY configured: yes" dist/appimage-gif-status.txt || die "GIPHY not embedded"
    grep -q "KLIPY configured: yes" dist/appimage-gif-status.txt || die "KLIPY not embedded"
    ( gif_env_clear; cd /tmp && QT_QPA_PLATFORM=offscreen "$ROOT/$app" --gif-selftest ) \
        | tee dist/appimage-gif-selftest.txt
    grep -q "GIPHY request: ok" dist/appimage-gif-selftest.txt || die "GIPHY selftest failed"
    grep -q "KLIPY request: ok" dist/appimage-gif-selftest.txt || die "KLIPY selftest failed"
    grep -Eq "api_key=|://" dist/appimage-gif-status.txt dist/appimage-gif-selftest.txt \
        && die "GIF output leaked a URL or key"
fi

# Payload audit on the extracted squashfs.
audit=$(mktemp -d)
cleanup() { rm -rf "$audit"; }
trap cleanup EXIT
( cd "$audit" && "$ROOT/$app" --appimage-extract >/dev/null )
tree="$audit/squashfs-root"
test -x "$tree/usr/bin/matrix-client" || die "binary missing in payload"
# Self-containment: Qt must be bundled, not expected from the host.
find "$tree/usr/lib" -name 'libQt6Core.so*' | grep -q . || die "Qt not bundled"
find "$tree/usr" -name 'libqoffscreen.so' | grep -q . || die "offscreen platform plugin missing"
find "$tree/usr/qml" -maxdepth 1 -name 'QtQuick' | grep -q . || die "QML modules missing"
readelf -d "$tree/usr/bin/matrix-client" | grep -E 'RPATH|RUNPATH' \
    | grep -vE '\$ORIGIN' | grep -q . && die "non-relative RPATH in payload binary"
grep -RIl -e /nix/store -e /home/roksme -e /builds/ \
    -e 'LIGHTNING_GIPHY_API_KEY=' -e 'LIGHTNING_KLIPY_API_KEY=' \
    -e 'PRIVATE-TOKEN:' -e 'recovery_key=' "$tree/usr/bin" \
    && die "forbidden path or credential marker in payload"
find "$tree" -name 'LightningGifBuildKeys.h' | grep -q . \
    && die "generated key header leaked into the payload"
find "$tree" -perm -0002 \( -type f -o -type d \) | grep -q . \
    && die "world-writable content"
find "$tree" -perm -4000 -o -perm -2000 | grep -q . \
    && die "unexpected setuid/setgid content"
echo "appimage validation passed"
