#!/usr/bin/env bash
# Clean-system validation of the snap. A real `snap install --dangerous`
# needs a running snapd, which the Docker-on-Linux fleet cannot provide, so
# this is the closest faithful equivalent (documented in the runner and
# packaging docs): unsquash the image, verify meta/snap.yaml and desktop
# integration, run the payload offscreen through the snap launcher with a
# simulated $SNAP, check GIF provider state, and audit for leaks.
set -euo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lib.sh
. "$SCRIPT_DIR/lib.sh"
ROOT=$(project_dir)
cd "$ROOT"
load_versions

count=$(find dist -maxdepth 1 -name '*.snap' | wc -l)
[ "$count" = 1 ] || die "expected exactly one .snap in dist, found $count"
snap_file="dist/lightning_${LOGICAL_VERSION}_amd64.snap"
test -f "$snap_file" || die "expected snap name $snap_file"
test ! -e work/lightning || die "validation must not see a source checkout"
command -v nix >/dev/null 2>&1 && die "validation image must not provide nix"
( cd dist && sha256sum -c "$(basename "$snap_file").sha256" )
file "$snap_file" | grep -qi squashfs || die "snap is not a squashfs image"

audit=$(mktemp -d)
cleanup() { rm -rf "$audit"; }
trap cleanup EXIT
unsquashfs -q -d "$audit/prime" "$snap_file"

python3 - "$audit/prime/meta/snap.yaml" "$LOGICAL_VERSION" <<'EOF'
import sys, yaml
with open(sys.argv[1]) as fh:
    meta = yaml.safe_load(fh)
assert meta["name"] == "lightning", meta
assert str(meta["version"]) == sys.argv[2], meta
assert meta["confinement"] == "strict"
assert meta["grade"] == "stable"
assert meta["license"] == "GPL-3.0-or-later"
app = meta["apps"]["lightning"]
assert app["command"] == "bin/lightning-launch"
for plug in ("network", "wayland", "x11", "desktop",
             "password-manager-service",
             # Calling. audio-playback covers only what the user hears; the
             # microphone is a separate interface and without it a call is
             # one-directional silence.
             "audio-playback", "audio-record"):
    assert plug in app["plugs"], plug
print("snap.yaml valid")
EOF
test -x "$audit/prime/usr/bin/matrix-client" || die "snap application binary missing"
# A snap is refreshed by snapd, so Lightning never RUNS the helper here — but
# the snap is repacked from the AppImage job's AppDir, so a helper missing from
# this payload means it was also missing from the AppImage, where it IS used.
test -x "$audit/prime/usr/bin/lightning-updater" || die "update helper missing from the snap payload"
test -f "$audit/prime/meta/gui/lightning.desktop" || die "snap desktop file missing"
test -f "$audit/prime/meta/gui/lightning.png" || die "snap icon missing"
test -x "$audit/prime/bin/lightning-launch" || die "launcher missing"

# Run through the snap launcher with $SNAP simulated (snapd would provide
# it at runtime); everything but base-system libs must come from the snap.
version_output=$(cd /tmp && env SNAP="$audit/prime" QT_QPA_PLATFORM=offscreen \
    "$audit/prime/bin/lightning-launch" --version)
# The launcher forces --backend=rust; --version prints before backend init.
[ "$version_output" = "matrix-client $BASE_VERSION" ] \
    || die "--version mismatch: $version_output"

set +e
( cd /tmp && timeout 20s env SNAP="$audit/prime" QT_QPA_PLATFORM=offscreen \
    "$audit/prime/usr/bin/matrix-client" --backend=rust \
    ) > dist/snap-launch.log 2>&1
status=$?
set -e
[ "$status" = 0 ] || [ "$status" = 124 ] \
    || { cat dist/snap-launch.log; die "offscreen launch failed ($status)"; }

# The call media engine, run THROUGH THE LAUNCHER, because the launcher is the
# half the snap has to get right: it takes only usr/ from the AppImage AppDir,
# so linuxdeploy's AppRun and its apprun-hooks/gstreamer.sh stay behind and the
# launcher is the only thing that can point GStreamer at the bundled plugins.
# Running the binary directly would test the payload and silently skip that.
set +e
( cd /tmp && timeout 60s env SNAP="$audit/prime" QT_QPA_PLATFORM=offscreen \
    "$audit/prime/bin/lightning-launch" --call-media-status ) \
    > dist/snap-call-media-status.txt 2>&1
call_media_status=$?
set -e
assert_call_media_engine snap dist/snap-call-media-status.txt "$call_media_status"
grep -q 'GST_PLUGIN_SYSTEM_PATH_1_0' "$audit/prime/bin/lightning-launch" \
    || die "the snap launcher does not point GStreamer at the bundled plugins"
# libgstximagesrc is the X11 screen-share fallback: not in the engine's
# required-element list, so the check above is green without it while the
# feature is dead — the launcher REPLACES the system plugin path, so the host's
# plugins-good is invisible and the route refuses with advice that changes
# nothing.
for gst_plugin in libgstwebrtc libgstsctp libgstnice libgstvpx libgstopus \
                  libgstximagesrc; do
    test -f "$audit/prime/usr/lib/gstreamer-1.0/$gst_plugin.so" \
        || die "$gst_plugin.so missing from the snap payload"
done
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
XIMAGE_SO="$audit/prime/usr/lib/gstreamer-1.0/libgstximagesrc.so"
# tr to SPACES: the `case` below matches on space-delimited words, and awk's
# newline-separated output never matches a *" x "* pattern. That mistake made
# this check pass with the X libraries deliberately removed -- caught by
# measuring it both ways, not by reading it.
ximage_unresolved=" $(LD_LIBRARY_PATH="$audit/prime/usr/lib" ldd "$XIMAGE_SO" 2>/dev/null \
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
    || die "the snap's ximagesrc cannot load: unresolved$ximage_missing"

gif_env() {
    env -u GIPHY_API_KEY -u KLIPY_API_KEY \
        -u LIGHTNING_GIPHY_API_KEY -u LIGHTNING_KLIPY_API_KEY \
        -u LIGHTNING_BUILD_GIPHY_API_KEY -u LIGHTNING_BUILD_KLIPY_API_KEY \
        SNAP="$audit/prime" QT_QPA_PLATFORM=offscreen "$@"
}
( cd /tmp && gif_env "$audit/prime/bin/lightning-launch" --gif-status ) \
    | tee dist/snap-gif-status.txt
if [ "${PUBLISH_PACKAGES:-false}" = "true" ]; then
    grep -q "GIPHY configured: yes" dist/snap-gif-status.txt || die "GIPHY not embedded"
    grep -q "KLIPY configured: yes" dist/snap-gif-status.txt || die "KLIPY not embedded"
    ( cd /tmp && gif_env "$audit/prime/bin/lightning-launch" --gif-selftest ) \
        | tee dist/snap-gif-selftest.txt
    grep -q "GIPHY request: ok" dist/snap-gif-selftest.txt || die "GIPHY selftest failed"
    grep -q "KLIPY request: ok" dist/snap-gif-selftest.txt || die "KLIPY selftest failed"
    grep -Eq "api_key=|://" dist/snap-gif-status.txt dist/snap-gif-selftest.txt \
        && die "GIF output leaked a URL or key"
fi

# Payload audit (same policy as every other format).
grep -RIl -e /nix/store -e /home/roksme -e /builds/ \
    -e 'LIGHTNING_GIPHY_API_KEY=' -e 'LIGHTNING_KLIPY_API_KEY=' \
    -e 'PRIVATE-TOKEN:' -e 'recovery_key=' "$audit/prime/usr/bin" \
    && die "forbidden path or credential marker in payload"
find "$audit/prime" -name 'LightningGifBuildKeys.h' | grep -q . \
    && die "generated key header leaked into the payload"
find "$audit/prime" -perm -0002 \( -type f -o -type d \) | grep -q . \
    && die "world-writable content"
find "$audit/prime" -perm -4000 -o -perm -2000 | grep -q . \
    && die "unexpected setuid/setgid content"
echo "snap validation passed (structural + payload; live snapd install is"
echo "not possible on this runner fleet — documented limitation)"
