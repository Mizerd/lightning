#!/usr/bin/env bash
# Assemble the Lightning snap from the AppImage job's fully bundled AppDir.
#
# See packaging/snap/snap.yaml.in for why snapcraft/snapd are not used on
# this runner fleet: a snap is a squashfs image with meta/snap.yaml, and the
# AppDir already contains the app, Qt, plugins, and QML modules built by
# configure-build.sh (same GIF-key handling as every other format).
#
# Input:  dist/lightning-appdir-<LOGICAL_VERSION>.tar.zst (build-appimage)
# Output: dist/lightning_<LOGICAL_VERSION>_amd64.snap
set -euo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lib.sh
. "$SCRIPT_DIR/lib.sh"
ROOT=$(project_dir)
cd "$ROOT"
load_versions

APPDIR_TAR="dist/lightning-appdir-${LOGICAL_VERSION}.tar.zst"
SNAP_WORK="$ROOT/work/snap"
TREE="$SNAP_WORK/prime"
OUT="dist/lightning_${LOGICAL_VERSION}_amd64.snap"
TEMPLATE="$ROOT/packaging-ci/packaging/snap/snap.yaml.in"

test -f "$APPDIR_TAR" || die "missing $APPDIR_TAR (build-appimage artifact)"
test -f "$TEMPLATE" || die "snap.yaml template missing"

rm -rf "$SNAP_WORK"
mkdir -p "$SNAP_WORK"
tar -C "$SNAP_WORK" -I zstd -xf "$APPDIR_TAR"
test -x "$SNAP_WORK/appdir/usr/bin/lightning-matrix" || die "AppDir payload incomplete"
# The call media plugins ride in from the AppImage job's AppDir. A snap with
# none is a snap that installs, launches and then refuses every call, so the
# absence is fatal here rather than at a user.
gst_bundled=$(find "$SNAP_WORK/appdir/usr/lib/gstreamer-1.0" -maxdepth 1 -name '*.so' 2>/dev/null | wc -l)
[ "$gst_bundled" -ge 20 ] || \
    die "the AppDir carries only $gst_bundled GStreamer plugins; the snap would refuse every call"
test -f "$SNAP_WORK/appdir/usr/lib/gstreamer-1.0/libgstwebrtc.so" || \
    die "the AppDir has no libgstwebrtc.so; the snap would have no call media engine"
# The Qt image-format plugins ride in from the same AppDir, and their absence is
# just as invisible: the snap installs, launches, and then cannot draw a WebP
# that the client's OWN byte sniffers accepted. Named one by one rather than
# counted, because a count is satisfied by the three qtbase carries anyway.
for img_plugin in libqwebp.so kimg_jxl.so; do
    test -f "$SNAP_WORK/appdir/usr/plugins/imageformats/$img_plugin" || \
        die "the AppDir has no $img_plugin; the snap would accept image formats it cannot decode"
done

mkdir -p "$TREE"
# Only usr/ is taken; linuxdeploy's AppImage entry artefacts (AppRun,
# top-level desktop file, .DirIcon) stay behind in the AppDir.
cp -a "$SNAP_WORK/appdir/usr" "$TREE/usr"

# ── THE BASE GRAPHICS STACK, WHICH AN APPIMAGE MUST NOT BUNDLE AND A SNAP
#    CANNOT DO WITHOUT ────────────────────────────────────────────────────
#
# The snap installed and then died on `libEGL.so.1: cannot open shared object
# file` — measured under a real snapd on 2026-09-11, the first time one had
# ever been run. Six libraries were unresolved inside the confinement, and
# every one of them was ABSENT FROM THE PAYLOAD AND FROM core24 WHILE BEING
# PRESENT ON THE HOST. That is exactly why `ldd` resolves them unconfined,
# why every CI check passed, and why nothing caught it for months.
#
# The AppDir is right to omit them. linuxdeploy's excludelist leaves the X,
# GL and Wayland client libraries on the host on purpose, and
# build-appimage.sh records what re-bundling one costs: GitHub issue #9, where
# a bundled libwayland-client older than the host's was handed to the host's
# Mesa EGL and aborted the client on every native Wayland session. An AppImage
# can see /usr/lib. A STRICTLY CONFINED SNAP CANNOT — that is the whole point
# of the confinement — so the same omission that keeps the AppImage portable
# makes the snap unlaunchable.
#
# COMPUTED FROM THE EXCLUDELIST, NOT FROM `ldd` ALONE — and the first version
# of this got it wrong in a way worth recording. It staged whatever the binary
# could not RESOLVE on the build host, which is nothing: the build host HAS
# libEGL, so ldd is perfectly satisfied there. The absence only exists inside
# the confinement. `snap: staged 0 base libraries` was the result, and the
# guard below caught it, which is the only reason this is a build failure and
# not another snap that installs and will not start.
#
# So the rule is the one that actually created the gap: stage every dependency
# the AppDir does NOT carry that falls in the families linuxdeploy's
# excludelist deliberately leaves on the host. That set auto-covers a seventh
# library in the same families without anyone remembering, while never
# touching the loader or the C/C++ runtime, which come from the base snap
# exactly as they come from the host for an AppImage.
stage_unresolved_libs() {
    local probe want src staged=0
    # EVERY PLUGIN, NOT JUST THE PLATFORM ONES. A plugin is dlopened, so its
    # dependencies are invisible to the BINARY's ldd — which is how the shipped
    # snap came to carry a `libqxcb.so` that could not load (libSM), a
    # `libgstalsa.so` that could not load (libasound) and a `libgstopengl.so`
    # that could not load (libGL), all three measured on a real snapd install
    # on 2026-09-12. The last one is why that snap fell back to the software
    # renderer, and on the software renderer Qt Quick draws no call video at
    # all. Probing only `platforms` and `xcbglintegrations` found the first
    # kind and neither of the others.
    local -a probes=("$TREE/usr/bin/lightning-matrix")
    while IFS= read -r probe; do probes+=("$probe"); done < <(
        find "$TREE/usr/plugins" "$TREE/usr/lib/gstreamer-1.0" \
             -name '*.so' 2>/dev/null)
    for probe in "${probes[@]}"; do
        [ -e "$probe" ] || continue
        while IFS= read -r src; do
            [ -n "$src" ] && [ -e "$src" ] || continue
            want="$(basename "$src")"
            # Already bundled by linuxdeploy: leave it, it has been rewritten.
            [ -e "$TREE/usr/lib/$want" ] && continue
            case "$want" in
                # The base-system families linuxdeploy excludes. An AppImage
                # can see the host's; a confined snap cannot see anything.
                libX*.so.*|libxcb*.so.*|libGL*.so.*|libEGL*.so.*|\
                libGLdispatch.so.*|libGLX*.so.*|libOpenGL.so.*|\
                libxkbcommon*.so.*|libwayland-*.so.*|libdrm.so.*|libgbm.so.*|\
                libasound.so.*|\
                libSM.so.*|libICE.so.*) ;;
                *) continue ;;
            esac
            cp -Ln "$src" "$TREE/usr/lib/$want" 2>/dev/null && staged=$((staged+1))
        done < <(ldd "$probe" 2>/dev/null | awk '/=> \// { print $3 }')
    done
    echo "$staged"
}
snap_staged=$(stage_unresolved_libs)
echo "snap: staged $snap_staged base libraries the AppDir deliberately omits"

# AND ASSERT IT, because the failure mode is a snap that installs cleanly and
# then does not start — which no build-time check can see and which the
# runner fleet cannot reach at all (validate-snap.sh says so itself: a real
# `snap install --dangerous` needs a running snapd and there is none). This
# is the same shape as the GStreamer-plugin and image-format guards above,
# and it exists for the same reason: graceful fallback and silent absence are
# the same observable unless something asserts the payload.
# libSM/libICE: X SESSION MANAGEMENT, and the one that actually shipped
# broken. MEASURED 2026-09-12 on a real `snap install --dangerous` in an
# Ubuntu 24.04 guest under snapd 2.76.3 — the only way this can be measured at
# all — the snap installed, `lightning --version` and `--call-media-status`
# both answered correctly, and the GUI could not start:
#
#   cannot load: ... libqxcb.so: libSM.so.6: cannot open shared object file
#   qt.qpa.plugin: Could not load the Qt platform plugin "xcb" ... even though
#   it was found.
#
# Qt's own advice on that path names xcb-cursor0, which IS staged, so the
# message sends you looking at the wrong library.
for base_lib in libEGL.so.1 libGLX.so.0 libGLdispatch.so.0 \
                libX11.so.6 libX11-xcb.so.1 libxcb.so.1 \
                libSM.so.6 libICE.so.6; do
    test -f "$TREE/usr/lib/$base_lib" || \
        die "the snap payload has no $base_lib; it would install and then fail to start"
done
# Nothing the binary needs may remain unresolved against the payload alone.
still_missing=$(LD_LIBRARY_PATH="$TREE/usr/lib" ldd "$TREE/usr/bin/lightning-matrix" 2>/dev/null \
                | awk '/not found/ { print $1 }' | tr '\n' ' ')
[ -z "$still_missing" ] || \
    die "the snap payload cannot resolve: $still_missing"

# AND THE SAME QUESTION OF EVERY PLUGIN, WHICH IS WHAT libSM ESCAPED THROUGH.
#
# A Qt platform plugin is dlopened, so its dependencies are NOT the binary's:
# the loop above is perfectly satisfied while `libqxcb.so` cannot load at all,
# and the app then exits with "no Qt platform plugin could be initialized".
# The named-library list above cannot cover this on its own either — it only
# ever names what someone has already been bitten by.
#
# Nor can any job on the runner fleet: validate-snap.sh runs the binary with
# QT_QPA_PLATFORM=offscreen, which never loads xcb, so a snap whose windowing
# is completely broken passes every check we have and installs cleanly. This
# is the fourth appearance of "a library loads its own plugins" in this
# project, and the first one a build-time check catches by itself.
# A Qt plugin that cannot load is FATAL and a GStreamer one is a feature lost,
# so they are judged differently — but both are reported, because neither is
# visible any other way.
qt_unresolved=""
gst_unresolved=""
gst_fatal=""
while IFS= read -r plugin; do
    missing=$(LD_LIBRARY_PATH="$TREE/usr/lib" ldd "$plugin" 2>/dev/null \
              | awk '/not found/ { print $1 }' | sort -u | tr '\n' ' ')
    [ -n "$missing" ] || continue
    case "$plugin" in
        "$TREE"/usr/plugins/*)
            qt_unresolved="$qt_unresolved
    $(basename "$plugin"): $missing" ;;
        *)
            gst_unresolved="$gst_unresolved
    $(basename "$plugin"): $missing"
            # These two are not optional: libgstopengl is the GPU share chain
            # AND the reason Qt gets a usable GL context here at all, and
            # libgstwebrtc is every call.
            case "$(basename "$plugin")" in
                libgstopengl.so|libgstwebrtc.so)
                    gst_fatal="$gst_fatal $(basename "$plugin")" ;;
            esac ;;
    esac
done < <(find "$TREE/usr/plugins" "$TREE/usr/lib/gstreamer-1.0" \
             -name '*.so' 2>/dev/null | sort)

[ -z "$qt_unresolved" ] || \
    die "snap Qt plugins cannot resolve against the payload:$qt_unresolved"
[ -z "$gst_unresolved" ] || \
    echo "snap: WARNING - GStreamer plugins that will not load:$gst_unresolved"
[ -z "$gst_fatal" ] || \
    die "snap: these GStreamer plugins are load-bearing and cannot resolve:$gst_fatal"

# Launcher: point Qt at the bundled runtime under $SNAP -- and GStreamer too.
# The snap takes only usr/ from the AppDir, so linuxdeploy's AppRun and its
# apprun-hooks/gstreamer.sh stay behind; without the three variables below the
# call plugins are inside the snap and GStreamer never looks at them, because
# it scans the path compiled into the build image. The AppImage learned this
# the same way.
mkdir -p "$TREE/bin"
cat > "$TREE/bin/lightning-launch" <<'EOF'
#!/bin/sh
set -e
: "${SNAP:?lightning-launch must run inside a snap environment}"
export LD_LIBRARY_PATH="$SNAP/usr/lib:${LD_LIBRARY_PATH:-}"
export QT_PLUGIN_PATH="$SNAP/usr/plugins"
export QML2_IMPORT_PATH="$SNAP/usr/qml"
export QML_IMPORT_PATH="$SNAP/usr/qml"
export XDG_DATA_DIRS="$SNAP/usr/share:${XDG_DATA_DIRS:-/usr/share}"
export GST_PLUGIN_SYSTEM_PATH_1_0="$SNAP/usr/lib/gstreamer-1.0"
export GST_PLUGIN_PATH_1_0="$SNAP/usr/lib/gstreamer-1.0"
# $SNAP is read-only and its revision changes on every refresh, so the plugin
# registry cache has to live in the user's own (snap-confined) cache dir.
export GST_REGISTRY_1_0="${XDG_CACHE_HOME:-$HOME/.cache}/lightning/gst-registry.bin"
mkdir -p "$(dirname "$GST_REGISTRY_1_0")" 2>/dev/null || true
exec "$SNAP/usr/bin/lightning-matrix" --backend=rust "$@"
EOF
chmod 0755 "$TREE/bin/lightning-launch"

# Snap metadata + desktop integration.
mkdir -p "$TREE/meta/gui"
sed -e "s|@VERSION@|$LOGICAL_VERSION|" "$TEMPLATE" > "$TREE/meta/snap.yaml"
grep -q '@VERSION@' "$TREE/meta/snap.yaml" && die "unsubstituted snap.yaml"
sed -e 's|^Exec=.*$|Exec=lightning|' \
    -e 's|^Icon=.*$|Icon=${SNAP}/meta/gui/lightning.png|' \
    "$TREE/usr/share/applications/lightning.desktop" \
    > "$TREE/meta/gui/lightning.desktop"
cp "$TREE/usr/share/icons/hicolor/192x192/apps/lightning.png" \
    "$TREE/meta/gui/lightning.png"

python3 - "$TREE/meta/snap.yaml" <<'EOF'
import sys, yaml
with open(sys.argv[1]) as fh:
    meta = yaml.safe_load(fh)
assert meta["name"] == "lightning"
assert meta["confinement"] == "strict"
assert meta["base"] == "core24"
assert "lightning" in meta["apps"]
assert "password-manager-service" in meta["apps"]["lightning"]["plugs"]
# Calling needs the microphone. audio-playback alone is a call nobody can
# hear you on.
assert "audio-record" in meta["apps"]["lightning"]["plugs"]
print("snap.yaml structurally valid")
EOF

mkdir -p dist
rm -f "$OUT"
# Standard snap squashfs settings (xz, root-owned).
mksquashfs "$TREE" "$OUT" -noappend -comp xz -all-root -no-xattrs >/dev/null
test -s "$OUT" || die "snap not produced"
write_sha256 "$OUT"
echo "built $OUT"
