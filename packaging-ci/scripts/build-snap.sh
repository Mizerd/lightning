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
                libSM.so.*|libICE.so.*|libuuid.so.*|\
                libfribidi.so.*|libthai.so.*) ;;
                *) continue ;;
            esac
            cp -Ln "$src" "$TREE/usr/lib/$want" 2>/dev/null && staged=$((staged+1))
            # THROUGH THE PAYLOAD, and this is not the same caution as the
            # guard below. Qt's plugins carry RUNPATH=$ORIGIN/../../lib so
            # `ldd` walks into the AppDir and reaches libSM; a GSTREAMER
            # plugin carries $ORIGIN alone, so without this `ldd` stops at
            # `libgstgl-1.0.so.0 => not found` and NEVER REACHES libGL or
            # libgbm — which is why naming those families staged nothing and
            # libgstopengl stayed unloadable. Measured in the job's own image
            # against pipeline 198's payload: 10 staged without it, 12 with.
            #
            # Taking the HOST's copy is the whole point HERE (the loop already
            # skips anything the payload carries, so nothing is copied over
            # itself). The guard further down must NOT do this — there, host
            # visibility is what makes the check dishonest.
        done < <(LD_LIBRARY_PATH="$TREE/usr/lib" ldd "$probe" 2>/dev/null \
                 | awk '/=> \// { print $3 }')
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
# ── DOES EVERY PLUGIN'S DEPENDENCY EXIST IN THE PAYLOAD? ─────────────────
#
# ASKED WITH `readelf`, NOT `ldd`, AND THAT IS THE WHOLE POINT. `ldd` resolves
# against the BUILD HOST: `LD_LIBRARY_PATH` only PREPENDS, so it still falls
# back to /etc/ld.so.cache and answers "found" for anything the build image
# happens to have installed. The libSM defect was caught only because
# ubuntu:24.04 plus this job's apt list happens not to pull libSM in — the day
# some unrelated package does, the check goes quiet and a snap that cannot open
# a window ships again. A set difference against the payload cannot drift that
# way.
#
# What may legitimately come from OUTSIDE the payload is the base snap's C and
# C++ runtime, and nothing else: linuxdeploy puts everything else in usr/lib.
# What may legitimately come from the base snap: the C and C++ runtime, and
# nothing else — linuxdeploy puts everything else in usr/lib. `libresolv.so.2`
# is on this list because it ships WITH glibc on a modern Ubuntu base; the
# binary itself needs it, and the snap demonstrably runs.
BASE_SNAP_LIBS="ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libdl.so.2 \
libpthread.so.0 librt.so.1 libresolv.so.2 libstdc++.so.6 libgcc_s.so.1"

# TRANSITIVELY, and that is not a refinement — it is the difference between
# catching the defect and not. `readelf -d` lists only DIRECT dependencies, and
# neither library that broke the shipped snap is direct: `libqxcb.so` reaches
# libSM through Qt's own XCB support library, and `libgstopengl.so` reaches
# libGL through libgstgl. A one-level check reports neither. So walk the graph
# the loader would walk, resolving every name against the PAYLOAD.
plugin_needs_missing() {   # object -> names no payload library can satisfy
    # SEPARATE STATEMENTS. `local a="$1" q="$a"` expands every word BEFORE it
    # assigns any of them, so `q` would be empty and the walk would never
    # start — silently, reporting nothing missing on a payload that is.
    local root="$1"
    local seen="" current need resolved missing=""
    local queue="$root"
    while [ -n "$queue" ]; do
        current="${queue%% *}"
        case "$queue" in *" "*) queue="${queue#* }" ;; *) queue="" ;; esac
        case " $seen " in *" $current "*) continue ;; esac
        seen="$seen $current"
        while IFS= read -r need; do
            [ -n "$need" ] || continue
            case " $BASE_SNAP_LIBS " in *" $need "*) continue ;; esac
            # ONE DIRECTORY, deliberately: linuxdeploy flattens everything
            # into usr/lib, so a payload library in a subdirectory would read
            # as missing and fail LOUD rather than pass quietly. That is the
            # safe direction for a guard whose whole job is catching absence.
            resolved="$TREE/usr/lib/$need"
            if [ -e "$resolved" ]; then
                case " $seen " in *" $resolved "*) ;; *) queue="$queue $resolved" ;; esac
            else
                case " $missing " in
                    *" $need "*) ;;
                    *) missing="$missing $need" ;;
                esac
            fi
        done < <(readelf -d "$current" 2>/dev/null \
                 | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
    done
    echo "$missing"
}

# A Qt plugin that cannot load is FATAL — the app exits with "no Qt platform
# plugin could be initialized" — and a GStreamer one is a feature lost. Both
# are reported, because neither is visible any other way: validate-snap.sh runs
# with QT_QPA_PLATFORM=offscreen, which never loads xcb at all.
qt_unresolved=""
gst_unresolved=""
gst_fatal=""
while IFS= read -r plugin; do
    missing="$(plugin_needs_missing "$plugin")"
    [ -n "$missing" ] || continue
    case "$plugin" in
        # FATAL: the binary (which used to have its own `ldd` check with the
        # same build-host flaw — one sweep, one method), the PLATFORM plugins,
        # and two directories that are not "features" in this application's
        # terms. `validate-appimage.sh` already treats the PRESENCE of these
        # two as fatal; their LOADABILITY deserves the same:
        #
        #   * tls/ — without libqopensslbackend every QNetworkAccessManager
        #     https request fails, which includes the update check and the
        #     download. §16 records the 0.9.0 AppImage consequence exactly: it
        #     "will never offer the next release by itself". An updater going
        #     permanently dark is not a degraded feature.
        #   * wayland-shell-integration/ — without libxdg-shell Qt refuses its
        #     Wayland plugin and runs under XWayland, where a screen share
        #     captures a BLACK ROOT WINDOW. `platforms/*` covers
        #     libqwayland-generic.so and does not cover this directory.
        "$TREE"/usr/bin/*|"$TREE"/usr/plugins/platforms/*|\
        "$TREE"/usr/plugins/tls/*|\
        "$TREE"/usr/plugins/wayland-shell-integration/*)
            qt_unresolved="$qt_unresolved
    $(basename "$plugin"):$missing" ;;
        # Every other Qt plugin is a FEATURE, not the app: an image format, a
        # media backend, a TLS backend. Report it and keep going — several
        # have been quietly unloadable for as long as this package has
        # existed, and turning that into a release blocker on the day the
        # check was written would be a different kind of mistake.
        "$TREE"/usr/plugins/*)
            gst_unresolved="$gst_unresolved
    $(basename "$plugin"):$missing" ;;
        *)
            gst_unresolved="$gst_unresolved
    $(basename "$plugin"):$missing"
            # Not optional: libgstopengl is the GPU share chain AND the reason
            # Qt gets a usable GL context here, and libgstwebrtc is every call.
            case "$(basename "$plugin")" in
                libgstopengl.so|libgstwebrtc.so)
                    gst_fatal="$gst_fatal $(basename "$plugin")" ;;
            esac ;;
    esac
done < <({ echo "$TREE/usr/bin/lightning-matrix";
           find "$TREE/usr/plugins" "$TREE/usr/lib/gstreamer-1.0" \
                -name '*.so' 2>/dev/null | sort; })

[ -z "$qt_unresolved" ] || \
    die "snap Qt plugins cannot resolve against the payload:$qt_unresolved"
[ -z "$gst_unresolved" ] || \
    echo "snap: WARNING - plugins that will not load:$gst_unresolved"
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
