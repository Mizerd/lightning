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
    "$ROOT/$app" --backend=rust ) > dist/appimage-launch.log 2>&1
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

# The call media engine, asked of the SHIPPED AppImage. An AppImage bundles its
# plugins rather than depending on them, so this proves three things at once
# that no file listing can: the engine was compiled in, the plugins are inside
# the image, and the AppRun hook actually points GStreamer at them. Staging
# without the hook bundles files nothing ever loads, and looks identical from
# the outside.
set +e
( cd /tmp && timeout 60s env QT_QPA_PLATFORM=offscreen "$ROOT/$app" --call-media-status ) \
    > dist/appimage-call-media-status.txt 2>&1
call_media_status=$?
set -e
assert_call_media_engine AppImage dist/appimage-call-media-status.txt \
    "$call_media_status"

# Payload audit on the extracted squashfs.
audit=$(mktemp -d)
cleanup() { rm -rf "$audit"; }
trap cleanup EXIT
( cd "$audit" && "$ROOT/$app" --appimage-extract >/dev/null )
tree="$audit/squashfs-root"
test -x "$tree/usr/bin/matrix-client" || die "binary missing in payload"
# The update helper ships beside the application. Without it the in-app updater
# has nothing to hand a verified AppImage to and the feature is inert.
test -x "$tree/usr/bin/lightning-updater" || die "update helper missing in payload"
# Self-containment: Qt must be bundled, not expected from the host.
find "$tree/usr/lib" -name 'libQt6Core.so*' | grep -q . || die "Qt not bundled"
find "$tree/usr" -name 'libqoffscreen.so' | grep -q . || die "offscreen platform plugin missing"
find "$tree/usr/qml" -maxdepth 1 -name 'QtQuick' | grep -q . || die "QML modules missing"
# The call plugins are bundled, not depended on. The runtime check above proves
# they LOAD; this proves the two that cost whole release rounds elsewhere are
# actually present, so a regression names itself instead of surfacing as a
# generic "missing_element".
#   libgstwebrtc  — webrtcbin, without which there is no call at all
#   libgstsctp    — named NOWHERE in Lightning: webrtcbin loads it itself for
#                   the data channel, and LiveKit's subscriber offer puts one
#                   in media section 0, which under bundle-policy=max-bundle
#                   owns the transport every audio and video section rides on.
#                   Windows shipped for months able to send and unable to
#                   receive because of exactly this file.
#   libgstximagesrc — the X11 screen-share fallback. NOT in the engine's
#                   required-element list, so the runtime check above is green
#                   with it missing while the feature is dead: the hook REPLACES
#                   the system plugin path, so the host's plugins-good is
#                   invisible and the route refuses with "install
#                   gst-plugins-good", which changes nothing.
#   libgstopengl  — glupload and friends, the GPU scale path for a screen
#                   share, which is now the DEFAULT rather than opt-in. Its
#                   absence CANNOT fail anything: the engine probes for the
#                   element, logs `element "glupload" is not available in this
#                   build` and uses the CPU. Measured on the 0.8.2 AppImage,
#                   where the feature was simply absent and every check passed.
#                   Same shape as libgstximagesrc above — a graceful fallback
#                   is exactly what stops a packaging gap from being noticed.
for gst_plugin in libgstwebrtc libgstsctp libgstnice libgstvpx libgstopus \
                  libgstximagesrc libgstopengl; do
    test -f "$tree/usr/lib/gstreamer-1.0/$gst_plugin.so" \
        || die "$gst_plugin.so missing from the AppImage payload"
done
test -f "$tree/apprun-hooks/gstreamer.sh" \
    || die "the AppRun hook that points GStreamer at the bundled plugins is missing"

# ...and staging a plugin that cannot LOAD is staging nothing. ximagesrc links
# libX11/libXext/libXfixes/libXdamage/libXtst, which linuxdeploy's excludelist
# deliberately leaves on the host — correctly, since the fallback only ever runs
# on an X11 session, where they are always present. This resolves the staged
# file exactly as the loader will, and fails on any of ITS OWN DT_NEEDED entries
# that come back unresolved.
#
# Scoped to its own NEEDED list on purpose: an unresolved TRANSITIVE library of
# the bundle's gst/glib stack would break every plugin, which the engine probe
# above already reports — attributing it to ximagesrc here would name the wrong
# cause. This check exists for the one failure NOTHING else can see.
XIMAGE_SO="$tree/usr/lib/gstreamer-1.0/libgstximagesrc.so"
# tr to SPACES: the `case` below matches on space-delimited words, and awk's
# newline-separated output never matches a *" x "* pattern. That mistake made
# this check pass with the X libraries deliberately removed -- caught by
# measuring it both ways, not by reading it.
ximage_unresolved=" $(LD_LIBRARY_PATH="$tree/usr/lib" ldd "$XIMAGE_SO" 2>/dev/null \
    | awk '/not found/{print $1}' | tr '\n' ' ')"
ximage_missing=""
# GUARDED: a `for` over an empty list iterates zero times and reports PASS.
# If readelf fails or its output format shifts, this sweep would silently
# stop checking anything — the exact no-teeth trap this repo records.
ximage_needs="$(readelf -d "$XIMAGE_SO" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p')"
[ -n "$ximage_needs" ] || die "readelf produced no NEEDED list for $XIMAGE_SO; the plugin-dependency sweep would pass without checking anything"
for ximage_need in $ximage_needs; do
    case "$ximage_unresolved" in
        *" $ximage_need "*) ximage_missing="$ximage_missing $ximage_need" ;;
    esac
done
[ -z "$ximage_missing" ] \
    || die "the bundled ximagesrc cannot load: unresolved$ximage_missing"
readelf -d "$tree/usr/bin/matrix-client" | grep -E 'RPATH|RUNPATH' \
    | grep -vE '\$ORIGIN' | grep -q . && die "non-relative RPATH in payload binary"
readelf -d "$tree/usr/bin/lightning-updater" | grep -E 'RPATH|RUNPATH' \
    | grep -vE '\$ORIGIN' | grep -q . && die "non-relative RPATH in the update helper"
# linuxdeploy rewrites the RPATH only of the executables it was told about, so a
# relative $ORIGIN entry here is the evidence that --executable was actually
# passed for the helper. Without it the helper resolves Qt from the host and
# fails to start on exactly the machines an AppImage exists for — at the last
# step of an update, after the download has already been verified.
readelf -d "$tree/usr/bin/lightning-updater" | grep -E 'RPATH|RUNPATH' \
    | grep -q '\$ORIGIN' \
    || die "the update helper has no \$ORIGIN RPATH; linuxdeploy did not bundle it"
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
