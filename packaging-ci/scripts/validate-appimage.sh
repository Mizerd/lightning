#!/usr/bin/env bash
# Clean-system validation of the AppImage on a minimal Debian image without
# Qt, via --appimage-extract-and-run (no FUSE in CI): --version, offscreen
# startup, runtime probes, GIF state and a payload audit.
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
[ "$version_output" = "Lightning $BASE_VERSION" ] \
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

# Ask the shipped AppImage for its call media engine: this proves the engine
# was compiled in, the plugins are bundled, and the AppRun hook points at them.
set +e
( cd /tmp && timeout 60s env QT_QPA_PLATFORM=offscreen "$ROOT/$app" --call-media-status ) \
    > dist/appimage-call-media-status.txt 2>&1
call_media_status=$?
set -e
assert_call_media_engine AppImage dist/appimage-call-media-status.txt \
    "$call_media_status"

# Voice-delay self-test; see assert_queue_selftest in lib.sh.
set +e
( cd /tmp && timeout 180s env QT_QPA_PLATFORM=offscreen "$ROOT/$app" --call-queue-selftest ) \
    > dist/appimage-queue-selftest.txt 2>&1
queue_selftest_status=$?
set -e
assert_queue_selftest AppImage dist/appimage-queue-selftest.txt \
    "$queue_selftest_status"

# Call sounds, warn-only: without an output device QSoundEffect never reaches
# Ready, so this is normally unmeasured here. See assert_call_sounds_status.
set +e
( cd /tmp && timeout 60s env QT_QPA_PLATFORM=offscreen "$ROOT/$app" --call-sounds-status ) \
    > dist/appimage-call-sounds-status.txt 2>&1
call_sounds_status=$?
set -e
assert_call_sounds_status AppImage dist/appimage-call-sounds-status.txt \
    "$call_sounds_status"
# The engine must use the bundle's GStreamer, not the host's.
grep -q "bundled plugin directory: .*/usr/lib/gstreamer-1.0" \
    dist/appimage-call-media-status.txt \
    || die "the AppImage did not report its own bundled GStreamer plugin directory"

# Image decoders are dlopen'd plugins; ask the bundle which ones register.
set +e
( cd /tmp && timeout 60s env QT_QPA_PLATFORM=offscreen "$ROOT/$app" --image-format-status ) \
    > dist/appimage-image-format-status.txt 2>&1
image_format_status=$?
set -e
assert_image_formats AppImage dist/appimage-image-format-status.txt \
    "$image_format_status" jxl

# Payload audit on the extracted squashfs.
audit=$(mktemp -d)
desktop_home=$(mktemp -d)
cleanup() { rm -rf "$audit" "$desktop_home"; }
trap cleanup EXIT
( cd "$audit" && "$ROOT/$app" --appimage-extract >/dev/null )
tree="$audit/squashfs-root"
test -x "$tree/usr/bin/lightning-matrix" || die "binary missing in payload"
# Without the update helper the in-app updater is inert.
test -x "$tree/usr/bin/lightning-updater" || die "update helper missing in payload"
# Self-containment: Qt must be bundled, not expected from the host.
find "$tree/usr/lib" -name 'libQt6Core.so*' | grep -q . || die "Qt not bundled"
find "$tree/usr" -name 'libqoffscreen.so' | grep -q . || die "offscreen platform plugin missing"
find "$tree/usr/qml" -maxdepth 1 -name 'QtQuick' | grep -q . || die "QML modules missing"
# Plugins named individually so a regression names itself. Several are not in
# the engine's required list, and the hook replaces the system plugin path, so
# their absence degrades silently:
#   libgstwebrtc    — webrtcbin
#   libgstsctp      — loaded by webrtcbin itself for the data channel, which
#                     owns the bundled transport; without it nothing is received
#   libgstximagesrc — X11 screen-share fallback
#   libgstopengl    — GPU scaling for screen share (else silently CPU)
#   libgstlevel     — capture level meter
#   libgstjpeg      — camera MJPG chain (else raw)
for gst_plugin in libgstwebrtc libgstsctp libgstnice libgstvpx libgstopus \
                  libgstximagesrc libgstopengl libgstlevel libgstjpeg; do
    test -f "$tree/usr/lib/gstreamer-1.0/$gst_plugin.so" \
        || die "$gst_plugin.so missing from the AppImage payload"
done
# The LGPL text for the bundled gst-plugins-good binaries, checked on the
# payload rather than the script.
good_license="$tree/usr/share/licenses/lightning-gstreamer/gst-plugins-good-1.0/COPYING"
test -s "$good_license" \
    || die "the AppImage bundles gst-plugins-good binaries and carries no licence for them: usr/share/licenses/lightning-gstreamer/gst-plugins-good-1.0/COPYING is missing or empty"
grep -q "GNU LESSER GENERAL PUBLIC LICENSE" "$good_license" \
    || die "the staged gst-plugins-good licence is not the LGPL text"

# Licences for every other bundled project, harvested by build-appimage.sh from
# /usr/share/doc/<pkg>/copyright (about 240 packages). The floor of 100 leaves
# room for Debian package splits while catching a short harvest.
third_party="$tree/usr/share/licenses/third-party"
test -d "$third_party" \
    || die "the AppImage bundles ~240 third-party libraries and carries no licence directory: usr/share/licenses/third-party is missing"
# `-size +0` so empty files do not satisfy the count.
third_party_count=$(find "$third_party" -maxdepth 1 -name '*.copyright' -type f -size +0 | wc -l)
test "$third_party_count" -ge 100 \
    || die "only $third_party_count non-empty third-party licence files are in the payload; the harvest in build-appimage.sh came back short"
# Guard against many copies of one file. Texts repeat legitimately (one per
# source package), so this is a loose floor.
third_party_distinct=$(find "$third_party" -maxdepth 1 -name '*.copyright' -type f -size +0 \
    -exec md5sum {} + | awk '{print $1}' | sort -u | wc -l)
test "$third_party_distinct" -ge 40 \
    || die "the $third_party_count licence files in the payload contain only $third_party_distinct distinct texts; the harvest is emitting the same file under many names"
# Spot-check specific projects: the GPL-2+ ffmpeg codec closure and GStreamer
# core/base. Each entry pairs a library glob with the licence glob owed only if
# that library is present (the ffmpeg plugin is deployed incidentally and may
# disappear). Globs, because Debian package names carry soversions.
while IFS='|' read -r lib_glob owed; do
    [ -n "$lib_glob" ] || continue
    present=$(find "$tree/usr" -name "$lib_glob" -type f 2>/dev/null | head -1)
    [ -n "$present" ] || continue
    hits=$(find "$third_party" -maxdepth 1 -name "$owed.copyright" -type f -size +0 | wc -l)
    test "$hits" -ge 1 \
        || die "the payload carries $(basename "$present") and no licence text for it: no non-empty $third_party/$owed.copyright"
done <<'OWED'
libavcodec.so.*|libavcodec*
libx264.so.*|libx264-*
libgstreamer-1.0.so.*|libgstreamer1.0-*
libgstapp-1.0.so.*|libgstreamer-plugins-base1.0-*
libQt6Core.so.*|libqt6core*
OWED
echo "third-party licences in the payload: $third_party_count"
# NSS's PKCS#11 modules. libsrtp2 uses NSS, which dlopens libsoftokn3 (and it
# libfreebl3) at runtime, so no NEEDED list names them. On a host without NSS
# (e.g. the snap's core24) SRTP cannot initialise and calls carry no media.
for nss_module in libsoftokn3 libfreebl3 libfreeblpriv3 libnssdbm3 libnssckbi; do
    test -f "$tree/usr/lib/$nss_module.so" \
        || die "$nss_module.so missing from the AppImage payload: libsrtp2 is built against NSS, so without NSS's dlopened modules SRTP cannot initialise and every call carries no media on a host that has no NSS of its own"
done
# Image-format plugins by name, so a regression names itself:
#   libqwebp  — not in qtbase; Lightning accepts image/webp
#   kimg_jxl  — the only Qt JPEG XL decoder (KDE kimageformats)
for img_plugin in libqwebp kimg_jxl; do
    test -f "$tree/usr/plugins/imageformats/$img_plugin.so" \
        || die "$img_plugin.so missing from the AppImage payload"
done
# Deployed only on request (EXTRA_QT_PLUGINS in build-appimage.sh). Without
# tls/ there is no https through Qt; without the xdg-shell integration Qt falls
# back to XWayland.
for qt_plugin in tls/libqopensslbackend.so wayland-shell-integration/libxdg-shell.so; do
    test -f "$tree/usr/plugins/$qt_plugin" \
        || die "Qt plugin missing from the AppImage payload: usr/plugins/$qt_plugin"
done
# Wayland client libraries belong to the host: a bundled, older
# libwayland-client broke the host's Mesa EGL and aborted every native Wayland
# session (issue #9).
for host_lib in libwayland-client.so libwayland-cursor.so libwayland-egl.so; do
    if find "$tree/usr/lib" -maxdepth 1 -name "$host_lib*" | grep -q .; then
        die "the payload bundles $host_lib, which belongs to the host (issue #9)"
    fi
done
test -f "$tree/apprun-hooks/gstreamer.sh" \
    || die "the AppRun hook that points GStreamer at the bundled plugins is missing"
# gst-plugin-scanner needs both the executable and the hook export; either
# alone silently falls back to an in-process scan.
test -x "$tree/usr/libexec/gstreamer-1.0/gst-plugin-scanner" \
    || die "gst-plugin-scanner is missing or not executable in the AppImage payload: GStreamer would print 'External plugin loader failed' at every launch and scan in-process"
grep -q 'GST_PLUGIN_SCANNER_1_0=' "$tree/apprun-hooks/gstreamer.sh" \
    || die "the AppRun hook stages gst-plugin-scanner but never exports GST_PLUGIN_SCANNER_1_0, so GStreamer still looks at the build image's compiled-in path"

# ximagesrc must also load. Its X libraries are deliberately left to the host
# by linuxdeploy (they exist on any X11 session), so resolve the staged file as
# the loader would and check only its own DT_NEEDED entries; transitive
# failures already show up in the engine probe.
XIMAGE_SO="$tree/usr/lib/gstreamer-1.0/libgstximagesrc.so"
# Space-delimited, because the `case` below matches *" name "*.
ximage_unresolved=" $(LD_LIBRARY_PATH="$tree/usr/lib" ldd "$XIMAGE_SO" 2>/dev/null \
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
    || die "the bundled ximagesrc cannot load: unresolved$ximage_missing"
readelf -d "$tree/usr/bin/lightning-matrix" | grep -E 'RPATH|RUNPATH' \
    | grep -vE '\$ORIGIN' | grep -q . && die "non-relative RPATH in payload binary"
readelf -d "$tree/usr/bin/lightning-updater" | grep -E 'RPATH|RUNPATH' \
    | grep -vE '\$ORIGIN' | grep -q . && die "non-relative RPATH in the update helper"
# An $ORIGIN RPATH proves linuxdeploy bundled the helper (--executable);
# otherwise it would load the host's Qt and fail at the last step of an update.
readelf -d "$tree/usr/bin/lightning-updater" | grep -E 'RPATH|RUNPATH' \
    | grep -q '\$ORIGIN' \
    || die "the update helper has no \$ORIGIN RPATH; linuxdeploy did not bundle it"
# On Wayland the window icon comes only from app id -> lightning.desktop ->
# Icon=, since Qt implements no icon protocol, so check the entry and icons.
assert_desktop_launcher_payload AppImage "$tree"
assert_appdir_root_icon AppImage "$tree"

# A single-file bundle installs nothing, so the app must publish its entry
# into the user's data directory at startup; ask the shipped bundle.
# APPIMAGE/APPDIR are passed explicitly because extract-and-run (no FUSE) does
# not set them. The XDG dirs are empty, so any visible entry was written now.
mkdir -p "$desktop_home/data" "$desktop_home/empty"
set +e
( cd /tmp && timeout 60s env \
    HOME="$desktop_home" \
    XDG_DATA_HOME="$desktop_home/data" \
    XDG_DATA_DIRS="$desktop_home/empty" \
    APPIMAGE="$ROOT/$app" APPDIR="$tree" \
    QT_QPA_PLATFORM=offscreen "$ROOT/$app" --desktop-status ) \
    > dist/appimage-desktop-status.txt 2>&1
desktop_status=$?
set -e
assert_desktop_status AppImage dist/appimage-desktop-status.txt \
    "$desktop_status" appimage
# The written entry must be launchable and name the icon.
published="$desktop_home/data/applications/lightning.desktop"
test -f "$published" || die "the AppImage reported a published launcher entry that is not there: $published"
grep -qx 'Icon=lightning' "$published" || die "the published launcher entry does not name Icon=lightning"
# -F: the path contains dots and is not a pattern.
grep -qF "Exec=\"$ROOT/$app\"" "$published" || die "the published launcher entry does not exec the running AppImage"
# Required, not optional: the job installs it, so its absence is a failure.
command -v desktop-file-validate >/dev/null 2>&1 \
    || die "desktop-file-validate is missing from the validator image"
desktop-file-validate "$published" \
    || die "the published launcher entry is not a valid desktop entry"

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
