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
# AND THAT IT IS THE BUNDLE'S OWN GSTREAMER, not the host's. The AppImage is
# the one Linux package that ships GStreamer, its plugins live in
# usr/lib/gstreamer-1.0 (not beside the binary), and the AppRun hook points at
# them. Until 07f1491 the flag reported "<none - using system GStreamer>" here
# and nothing noticed, because only the RESULT line was asserted. The macOS
# validator has always asserted its own equivalent.
grep -q "bundled plugin directory: .*/usr/lib/gstreamer-1.0" \
    dist/appimage-call-media-status.txt \
    || die "the AppImage did not report its own bundled GStreamer plugin directory"

# THE IMAGE DECODERS, asked of the same shipped bundle on the same Qt-less
# image, for the same reason: Qt image formats are dlopen'd plugins, so what a
# package can draw is decided by packaging and no file listing can tell a
# plugin that is present from one that registers. Every AppImage up to and
# including 0.8.0 fails this — it bundles gif/ico/jpeg and nothing else, while
# the client's own byte sniffers accept image/webp.
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
# The two image-format plugins, in the payload, by name. The runtime probe
# above already proves they REGISTER; this makes a regression name itself
# instead of arriving as a generic "cannot decode image/webp".
#   libqwebp  — qtbase does not carry it, and Lightning ACCEPTS image/webp
#   kimg_jxl  — the only Qt JPEG XL decoder that exists anywhere; it comes
#               from KDE's kimageformats, never from Qt
for img_plugin in libqwebp kimg_jxl; do
    test -f "$tree/usr/plugins/imageformats/$img_plugin.so" \
        || die "$img_plugin.so missing from the AppImage payload"
done
# Qt plugin directories linuxdeploy-plugin-qt only deploys when asked
# (EXTRA_QT_PLUGINS in build-appimage.sh). The 0.9.0 AppImage shipped with an
# EMPTY usr/plugins/tls (no https through Qt: updater, GIF self-test) and no
# wayland-shell-integration (Qt refused its wayland plugin and ran under
# XWayland). Graceful fallback and silent absence are the same observable
# unless something asserts the payload — so this does.
for qt_plugin in tls/libqopensslbackend.so wayland-shell-integration/libxdg-shell.so; do
    test -f "$tree/usr/plugins/$qt_plugin" \
        || die "Qt plugin missing from the AppImage payload: usr/plugins/$qt_plugin"
done
# ...and the mirror image of that rule: a library that must NOT be there is
# just as invisible as one that must. GitHub issue #9 — the 0.9.1 AppImage
# bundled its own libwayland-client.so.0, older than the host's and missing
# `wl_display_dispatch_queue_timeout`, so the HOST's Mesa EGL was handed our
# copy, EGL initialisation failed and the client aborted before a window
# existed on every native Wayland session. The client library of a display
# protocol, and the driver stack around it, belong to the host. Assert they
# are gone, or the next linuxdeploy run quietly bundles them again.
for host_lib in libwayland-client.so libwayland-cursor.so libwayland-egl.so; do
    if find "$tree/usr/lib" -maxdepth 1 -name "$host_lib*" | grep -q .; then
        die "the payload bundles $host_lib, which belongs to the host (issue #9)"
    fi
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
readelf -d "$tree/usr/bin/lightning-matrix" | grep -E 'RPATH|RUNPATH' \
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
# THE LAUNCHER ENTRY AND THE ICONS IT NAMES — the window icon, which on
# Wayland is decided entirely by packaging and by nothing in the picture.
#
# Reported against 0.9.x: after updating, the AppImage's window and taskbar
# icon is a generic placeholder. Qt's Wayland client implements no icon
# protocol at all (`xdg_toplevel_icon` appears zero times in
# libQt6WaylandClient), so setWindowIcon() is inert there and the compositor's
# only route is the app id -> lightning.desktop -> Icon= lookup. AppImages up
# to 0.9.0 shipped without wayland-shell-integration and ran under XWayland,
# where setWindowIcon DOES work — staging that plugin moved them onto native
# Wayland and the icon went generic. Nothing in the payload changed; the
# protocol under it did, and no check anywhere looked at either half.
assert_desktop_launcher_payload AppImage "$tree"
assert_appdir_root_icon AppImage "$tree"

# ...and the half a payload audit cannot see: a single-file bundle installs
# nothing, so the entry above is invisible to the session unless the app
# publishes a copy into the user's own data directory at startup. Ask the
# SHIPPED bundle whether it does.
#
# APPIMAGE/APPDIR are handed in rather than trusted from the runtime. A normal
# (FUSE) run exports both — that is what every AppImage integration, this
# repository's own UrlLauncher included, is built on — but CI runs with
# APPIMAGE_EXTRACT_AND_RUN=1 because containers have no FUSE, and this check is
# about the APPLICATION's behaviour given that environment, not about which
# variables one launch mode happens to set. The runtime overwrites both when it
# does set them, so a FUSE run tests the same path.
#
# XDG_DATA_HOME and XDG_DATA_DIRS point at empty scratch directories, so the
# only way "visible launcher entry" can be non-NONE is that this run wrote it.
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
# The entry it wrote has to be launchable and has to carry the icon name, or
# "written" is a file nobody can act on.
published="$desktop_home/data/applications/lightning.desktop"
test -f "$published" || die "the AppImage reported a published launcher entry that is not there: $published"
grep -qx 'Icon=lightning' "$published" || die "the published launcher entry does not name Icon=lightning"
# -F: the AppImage's own name carries dots, and $ROOT is whatever the runner
# checked out into -- neither is a pattern.
grep -qF "Exec=\"$ROOT/$app\"" "$published" || die "the published launcher entry does not exec the running AppImage"
# UNCONDITIONAL. This ran only `if` the tool happened to be present, and the
# job's package list did not install it -- so the one check that would catch a
# malformed Exec= never ran at all. Graceful fallback and silent absence are
# the same observable; §16 records four packaging defects of exactly that
# shape. The tool is now installed by the job, so its absence is a failure.
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
