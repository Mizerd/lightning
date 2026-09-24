#!/usr/bin/env bash
# Structural validation of the snap (the runners cannot run snapd): unsquash,
# verify meta/snap.yaml and desktop integration, run the payload offscreen
# through the launcher with a simulated $SNAP, check GIF state, audit leaks.
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
             "audio-playback", "audio-record",
             # Graphics. Without it the app sees only glvnd's dispatch stubs,
             # gets no EGL, and falls back to a software renderer that cannot
             # draw video at all -- no call video, no screen share.
             "gpu-2404"):
    assert plug in app["plugs"], plug
# THE CONTENT PLUG ITSELF, IN THE PARSED TREE. A `grep gpu-2404 snap.yaml`
# looks equivalent and is not: build-snap.sh copies the template's COMMENTS
# into meta/snap.yaml, and those mention gpu-2404 several times, so the grep
# passes with the entire plug stanza deleted. Measured in review.
gpu = meta["plugs"]["gpu-2404"]
assert gpu["interface"] == "content", gpu
assert gpu["target"] == "$SNAP/gpu-2404", gpu
assert gpu["default-provider"] == "mesa-2404", gpu
print("snap.yaml valid")
EOF
test -x "$audit/prime/usr/bin/lightning-matrix" || die "snap application binary missing"
# snapd refreshes the snap, but a missing helper here means the AppImage it was
# repacked from lacks it too.
test -x "$audit/prime/usr/bin/lightning-updater" || die "update helper missing from the snap payload"
test -f "$audit/prime/meta/gui/lightning.desktop" || die "snap desktop file missing"
test -f "$audit/prime/meta/gui/lightning.png" || die "snap icon missing"
test -x "$audit/prime/bin/lightning-launch" || die "launcher missing"
# Licences are asserted on the payload itself rather than inferred from the
# AppImage it was repacked from.
snap_good_license="$audit/prime/usr/share/licenses/lightning-gstreamer/gst-plugins-good-1.0/COPYING"
test -s "$snap_good_license" \
    || die "the snap bundles gst-plugins-good binaries and carries no licence for them: usr/share/licenses/lightning-gstreamer/gst-plugins-good-1.0/COPYING is missing or empty"
grep -q "GNU LESSER GENERAL PUBLIC LICENSE" "$snap_good_license" \
    || die "the staged gst-plugins-good licence in the snap is not the LGPL text"
# Third-party licences harvested per Debian package by build-appimage.sh.
snap_third_party="$audit/prime/usr/share/licenses/third-party"
test -d "$snap_third_party" \
    || die "the snap carries ~240 third-party libraries and no licence directory: usr/share/licenses/third-party is missing"
# `-size +0` so empty files do not satisfy the count.
snap_tp_count=$(find "$snap_third_party" -maxdepth 1 -name '*.copyright' -type f -size +0 | wc -l)
test "$snap_tp_count" -ge 100 \
    || die "only $snap_tp_count third-party licence files are in the snap payload; the AppImage harvest came back short"
echo "third-party licences in the snap payload: $snap_tp_count"

# Run through the snap launcher with $SNAP simulated (snapd would provide
# it at runtime); everything but base-system libs must come from the snap.
version_output=$(cd /tmp && env SNAP="$audit/prime" QT_QPA_PLATFORM=offscreen \
    "$audit/prime/bin/lightning-launch" --version)
# The launcher forces --backend=rust; --version prints before backend init.
[ "$version_output" = "Lightning $BASE_VERSION" ] \
    || die "--version mismatch: $version_output"

set +e
( cd /tmp && timeout 20s env SNAP="$audit/prime" QT_QPA_PLATFORM=offscreen \
    "$audit/prime/usr/bin/lightning-matrix" --backend=rust \
    ) > dist/snap-launch.log 2>&1
status=$?
set -e
[ "$status" = 0 ] || [ "$status" = 124 ] \
    || { cat dist/snap-launch.log; die "offscreen launch failed ($status)"; }

# Run through the launcher: the snap takes only usr/ from the AppDir, so the
# launcher alone points GStreamer at the bundled plugins.
set +e
( cd /tmp && timeout 60s env SNAP="$audit/prime" QT_QPA_PLATFORM=offscreen \
    "$audit/prime/bin/lightning-launch" --call-media-status ) \
    > dist/snap-call-media-status.txt 2>&1
call_media_status=$?
set -e
assert_call_media_engine snap dist/snap-call-media-status.txt "$call_media_status"

# Voice-delay self-test; see assert_queue_selftest in lib.sh.
set +e
( cd /tmp && timeout 180s env SNAP="$audit/prime" QT_QPA_PLATFORM=offscreen \
    "$audit/prime/bin/lightning-launch" --call-queue-selftest ) \
    > dist/snap-queue-selftest.txt 2>&1
queue_selftest_status=$?
set -e
assert_queue_selftest snap dist/snap-queue-selftest.txt "$queue_selftest_status"

# Call sounds, warn-only: without an output device QSoundEffect never reaches
# Ready, so this is normally unmeasured here. See assert_call_sounds_status.
set +e
( cd /tmp && timeout 60s env SNAP="$audit/prime" QT_QPA_PLATFORM=offscreen \
    "$audit/prime/bin/lightning-launch" --call-sounds-status ) \
    > dist/snap-call-sounds-status.txt 2>&1
call_sounds_status=$?
set -e
assert_call_sounds_status snap dist/snap-call-sounds-status.txt "$call_sounds_status"

# Image decoders, through the launcher, which sets QT_PLUGIN_PATH.
set +e
( cd /tmp && timeout 60s env SNAP="$audit/prime" QT_QPA_PLATFORM=offscreen \
    "$audit/prime/bin/lightning-launch" --image-format-status ) \
    > dist/snap-image-format-status.txt 2>&1
image_format_status=$?
set -e
assert_image_formats snap dist/snap-image-format-status.txt "$image_format_status" jxl
grep -q 'GST_PLUGIN_SYSTEM_PATH_1_0' "$audit/prime/bin/lightning-launch" \
    || die "the snap launcher does not point GStreamer at the bundled plugins"
# The launcher must also point at the bundled gst-plugin-scanner; otherwise
# libgstreamer looks for the build image's path and silently scans in-process.
test -x "$audit/prime/usr/libexec/gstreamer-1.0/gst-plugin-scanner" \
    || die "gst-plugin-scanner is not in the snap payload"
# libsrtp2 uses NSS, which dlopens libsoftokn3/libfreebl3 at runtime (invisible
# to ldd). core24 has no NSS, so without these SRTP cannot initialise and calls
# carry no media.
for nss_module in libsoftokn3 libfreebl3 libfreeblpriv3 libnssdbm3 libnssckbi; do
    test -f "$audit/prime/usr/lib/$nss_module.so" \
        || die "$nss_module.so is not in the snap payload and core24 has no NSS: SRTP cannot initialise, so every call carries no media in either direction"
done
grep -q 'GST_PLUGIN_SCANNER_1_0' "$audit/prime/bin/lightning-launch" \
    || die "the snap launcher does not point GStreamer at the bundled gst-plugin-scanner: every launch prints 'External plugin loader failed' and scans in-process"
# `if` rather than `grep && die`, which is easy to misread under `set -e`.
if grep -qi 'External plugin loader failed' dist/snap-call-media-status.txt; then
    die "the snap still prints 'External plugin loader failed' when run through its own launcher — the scanner pointer is wrong, not merely absent"
fi
# What core24 does not provide: the socket bridges into snapd's remapped
# XDG_RUNTIME_DIR, xkb keymaps and fontconfig. These are text/file checks only;
# proving the snap starts needs a real snapd.
# Match the `ln` itself, not the path, which also appears in its guard.
grep -q 'ln -sf "\$_bre_up\$1"' "$audit/prime/bin/lightning-launch" \
    || die "the snap launcher no longer symlinks session sockets into snapd's per-snap XDG_RUNTIME_DIR: Qt cannot reach the compositor and the snap ABORTS on every Wayland session"
for entry in pipewire-0 pulse/native; do
    grep -q "bridge_runtime_entry \"$entry\"" "$audit/prime/bin/lightning-launch" \
        || die "the snap launcher no longer bridges $entry: the microphone reports 'Connection refused' and the audio pipeline errors out"
done
grep -q 'bridge_runtime_entry "\$WAYLAND_DISPLAY"' "$audit/prime/bin/lightning-launch" \
    || die "the snap launcher no longer bridges the compositor socket"
# Staged data is useless unless the launcher points at it.
for var in FONTCONFIG_FILE FONTCONFIG_PATH XKB_CONFIG_ROOT; do
    grep -q "export $var=" "$audit/prime/bin/lightning-launch" \
        || die "the snap launcher no longer exports $var, so the data staged beside it is never found"
done
test -f "$audit/prime/usr/share/X11/xkb/rules/evdev.xml" \
    || die "xkb keymaps are not in the snap payload and core24 has none: the snap segfaults just after placing its window"
test -f "$audit/prime/etc/fonts/fonts.conf" \
    || die "fontconfig configuration is not in the snap payload and core24 has none"
# A rule symlinked outside the snap passes file checks but is dead at runtime.
test -z "$(find "$audit/prime/etc/fonts" -xtype l 2>/dev/null | head -1)" \
    || die "dangling symlinks under etc/fonts — the fontconfig rules are staged but point outside the snap (use cp -aL)"
# The dangling check passes vacuously with no rules, so count them and name
# the three whose absence is visible.
snap_rules=$(find "$audit/prime/etc/fonts/conf.d" -maxdepth 1 -name '*.conf' 2>/dev/null | wc -l)
[ "$snap_rules" -ge 30 ] \
    || die "only $snap_rules fontconfig rules in the payload (expected 30+): generic-family and emoji fallback rules are missing"
for rule in 45-generic.conf 60-latin.conf 70-no-bitmaps-except-emoji.conf; do
    test -f "$audit/prime/etc/fonts/conf.d/$rule" \
        || die "fontconfig rule $rule missing from the payload"
done
test -d "$audit/prime/gpu-2404" \
    || die "the gpu-2404 content mount point is missing from the payload"
# Match the exec, not the GPU_WRAPPER= assignment.
grep -q 'exec "\$GPU_WRAPPER"' "$audit/prime/bin/lightning-launch" \
    || die "the snap declares the gpu-2404 plug but the launcher never execs through the provider wrapper, so the driver paths are never set"
# libgstximagesrc (X11 screen-share fallback) is not in the engine's required
# list, and the launcher replaces the system plugin path, so check it here.
for gst_plugin in libgstwebrtc libgstsctp libgstnice libgstvpx libgstopus \
                  libgstximagesrc; do
    test -f "$audit/prime/usr/lib/gstreamer-1.0/$gst_plugin.so" \
        || die "$gst_plugin.so missing from the snap payload"
done
# ximagesrc must also load. Its X libraries are deliberately left to the host
# by linuxdeploy (they exist on any X11 session), so resolve the staged file as
# the loader would and check only its own DT_NEEDED entries; transitive
# failures already show up in the engine probe.
XIMAGE_SO="$audit/prime/usr/lib/gstreamer-1.0/libgstximagesrc.so"
# Space-delimited, because the `case` below matches *" name "*.
ximage_unresolved=" $(LD_LIBRARY_PATH="$audit/prime/usr/lib" ldd "$XIMAGE_SO" 2>/dev/null \
    | awk '/not found/{print $1}' | tr '\n' ' ')"
ximage_missing=""
# Guard against an empty NEEDED list, which would make the loop vacuous.
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
