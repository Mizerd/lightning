#!/usr/bin/env bash
# Assemble the Lightning snap from the AppImage job's fully bundled AppDir.
#
# See packaging/snap/snap.yaml.in for why snapcraft is not used. The AppDir
# already carries the app, Qt, plugins and QML modules.
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
# Without the AppDir's call plugins the snap would refuse every call.
gst_bundled=$(find "$SNAP_WORK/appdir/usr/lib/gstreamer-1.0" -maxdepth 1 -name '*.so' 2>/dev/null | wc -l)
[ "$gst_bundled" -ge 20 ] || \
    die "the AppDir carries only $gst_bundled GStreamer plugins; the snap would refuse every call"
test -f "$SNAP_WORK/appdir/usr/lib/gstreamer-1.0/libgstwebrtc.so" || \
    die "the AppDir has no libgstwebrtc.so; the snap would have no call media engine"
# Named individually: a count would be satisfied by qtbase's own plugins.
for img_plugin in libqwebp.so kimg_jxl.so; do
    test -f "$SNAP_WORK/appdir/usr/plugins/imageformats/$img_plugin" || \
        die "the AppDir has no $img_plugin; the snap would accept image formats it cannot decode"
done

mkdir -p "$TREE"
# Only usr/; AppRun, the top-level desktop file and .DirIcon stay behind.
cp -a "$SNAP_WORK/appdir/usr" "$TREE/usr"

# Base graphics/session libraries. linuxdeploy's excludelist rightly leaves
# the X, GL, Wayland and audio client libraries to the host (bundling
# libwayland-client broke Mesa EGL, issue #9), but a strictly confined snap
# cannot see the host and core24 does not carry them. Stage every dependency
# in those excluded families that the AppDir lacks. `ldd` alone cannot find
# the gap: the build host has these libraries, so nothing looks unresolved.
stage_unresolved_libs() {
    local probe want src staged=0
    # Probe every plugin too: their dlopen'd dependencies (libSM for xcb,
    # libasound for alsa, libGL for gstopengl) are invisible to the binary.
    local -a probes=("$TREE/usr/bin/lightning-matrix")
    while IFS= read -r probe; do probes+=("$probe"); done < <(
        find "$TREE/usr/plugins" "$TREE/usr/lib/gstreamer-1.0" \
             -name '*.so' 2>/dev/null)
    for probe in "${probes[@]}"; do
        [ -e "$probe" ] || continue
        while IFS= read -r src; do
            [ -n "$src" ] && [ -e "$src" ] || continue
            want="$(basename "$src")"
            # Already bundled by linuxdeploy.
            [ -e "$TREE/usr/lib/$want" ] && continue
            case "$want" in
                # The base-system families linuxdeploy excludes.
                libX*.so.*|libxcb*.so.*|libGL*.so.*|libEGL*.so.*|\
                libGLdispatch.so.*|libGLX*.so.*|libOpenGL.so.*|\
                libxkbcommon*.so.*|libwayland-*.so.*|libdrm.so.*|libgbm.so.*|\
                libasound.so.*|\
                libSM.so.*|libICE.so.*|libuuid.so.*|\
                libfribidi.so.*|libthai.so.*) ;;
                *) continue ;;
            esac
            cp -Ln "$src" "$TREE/usr/lib/$want" 2>/dev/null && staged=$((staged+1))
            # LD_LIBRARY_PATH lets ldd walk through payload libraries: a
            # GStreamer plugin's RUNPATH is $ORIGIN only, so ldd would
            # otherwise stop at libgstgl and never reach libGL/libgbm. Taking
            # the host's copy is intended here, unlike in the guard below.
        done < <(LD_LIBRARY_PATH="$TREE/usr/lib" ldd "$probe" 2>/dev/null \
                 | awk '/=> \// { print $3 }')
    done
    echo "$staged"
}
snap_staged=$(stage_unresolved_libs)
echo "snap: staged $snap_staged base libraries the AppDir deliberately omits"

# Assert the result: a snap missing these installs cleanly and then cannot
# start, which no job on this fleet can observe. libSM/libICE are needed by
# libqxcb (Qt's error for that misleadingly names xcb-cursor0).
for base_lib in libEGL.so.1 libGLX.so.0 libGLdispatch.so.0 \
                libX11.so.6 libX11-xcb.so.1 libxcb.so.1 \
                libSM.so.6 libICE.so.6; do
    test -f "$TREE/usr/lib/$base_lib" || \
        die "the snap payload has no $base_lib; it would install and then fail to start"
done
# Every plugin's dependencies must resolve inside the payload (validate-snap
# runs offscreen and never loads xcb, so nothing later would notice). Resolved
# with readelf against the payload, not ldd, which falls back to the build
# host's ld.so.cache and reports "found" for anything installed there.
#
# Only the base snap's C/C++ runtime may come from outside the payload
# (libresolv ships with glibc).
BASE_SNAP_LIBS="ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libdl.so.2 \
libpthread.so.0 librt.so.1 libresolv.so.2 libstdc++.so.6 libgcc_s.so.1"

# Walked transitively: libqxcb reaches libSM and libgstopengl reaches libGL
# only through intermediate libraries.
plugin_needs_missing() {   # object -> names no payload library can satisfy
    # Separate statements: `local a="$1" q="$a"` expands before assigning.
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
            # linuxdeploy flattens into usr/lib; anything elsewhere fails loud.
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

# Critical Qt plugins are fatal; other plugins are reported.
qt_unresolved=""
gst_unresolved=""
gst_fatal=""
while IFS= read -r plugin; do
    missing="$(plugin_needs_missing "$plugin")"
    [ -n "$missing" ] || continue
    case "$plugin" in
        # Fatal: the binary, platform plugins, and
        #   * tls/ — without libqopensslbackend every https request, including
        #     the update check, fails;
        #   * wayland-shell-integration/ — without libxdg-shell Qt falls back to
        #     XWayland, where screen shares capture a black root window.
        "$TREE"/usr/bin/*|"$TREE"/usr/plugins/platforms/*|\
        "$TREE"/usr/plugins/tls/*|\
        "$TREE"/usr/plugins/wayland-shell-integration/*)
            qt_unresolved="$qt_unresolved
    $(basename "$plugin"):$missing" ;;
        # Other Qt plugins are optional features: report only.
        "$TREE"/usr/plugins/*)
            gst_unresolved="$gst_unresolved
    $(basename "$plugin"):$missing" ;;
        *)
            gst_unresolved="$gst_unresolved
    $(basename "$plugin"):$missing"
            # libgstopengl (GPU share chain, GL context) and libgstwebrtc
            # (every call) are required.
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

# core24 has no xkb keymaps or fontconfig configuration, and the app crashes
# after placing its window without them. Fonts themselves are not staged: the
# `desktop` interface bind-mounts the host's, and fontconfig's <dir> entries
# are absolute anyway.
stage_confined_data() {
    local staged=0 dangling rules rule
    if [ -d /usr/share/X11/xkb ]; then
        mkdir -p "$TREE/usr/share/X11/xkb"
        cp -a /usr/share/X11/xkb/. "$TREE/usr/share/X11/xkb/" && staged=$((staged + 1))
    fi
    if [ -d /etc/fonts ]; then
        mkdir -p "$TREE/etc/fonts"
        # -L: most conf.d entries link into /usr/share/fontconfig/conf.avail,
        # which is not in core24.
        cp -aL /etc/fonts/. "$TREE/etc/fonts/" && staged=$((staged + 1))
    fi
    # `cp -a src/. dst/` succeeds on an empty source, so assert the files the
    # app actually opens.
    [ "$staged" -eq 2 ] || \
        die "snap: expected xkb and fontconfig to stage; got $staged (does the build image install xkb-data and fontconfig-config?)"
    [ -f "$TREE/usr/share/X11/xkb/rules/evdev.xml" ] || \
        die "snap: xkb staged without rules/evdev.xml — xkbcommon cannot build a keymap"
    [ -f "$TREE/etc/fonts/fonts.conf" ] || \
        die "snap: fontconfig staged without fonts.conf"
    dangling=$(find "$TREE/etc/fonts" -xtype l 2>/dev/null | wc -l)
    [ "$dangling" -eq 0 ] || \
        die "snap: $dangling dangling symlink(s) under etc/fonts — the rules are present but point outside the snap"
    # The dangling check passes vacuously when the rules are absent (an image
    # without /usr/share/fontconfig), so count them as well.
    rules=$(find "$TREE/etc/fonts/conf.d" -maxdepth 1 -name '*.conf' 2>/dev/null | wc -l)
    [ "$rules" -ge 30 ] || \
        die "snap: only $rules fontconfig rules staged (expected 30+) — is /usr/share/fontconfig present in the build image?"
    for rule in 45-generic.conf 60-latin.conf 70-no-bitmaps-except-emoji.conf; do
        [ -f "$TREE/etc/fonts/conf.d/$rule" ] || \
            die "snap: fontconfig rule $rule did not stage — generic-family or emoji fallback will be wrong"
    done
    echo "snap: staged xkb + fontconfig for strict confinement"
}
stage_confined_data
# Mount point for the gpu-2404 content interface (avoids snapd's mimic).
mkdir -p "$TREE/gpu-2404"

# Launcher: point Qt and GStreamer at the bundled runtime under $SNAP. The
# AppImage's AppRun hooks are not carried over, so this is the only place.
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
# THE SCANNER, which this launcher did not point at for as long as the snap
# has existed. The binary IS in the payload -- it rides along in the AppDir
# the AppImage job stages -- but libgstreamer looks for it at the path
# compiled into the BUILD IMAGE, which does not exist inside the snap. The
# result is "External plugin loader failed" at every launch and an in-process
# scan, losing the crash isolation a separate process buys. Found 2026-09-12
# by installing the snap under a REAL snapd for the first time; the
# structural validation the snap.yaml comment describes cannot see it,
# because the file it would look for is present and only the pointer is
# missing. Both spellings, for the reason build-appimage.sh gives: GStreamer
# reads the versioned one first and falls back to the plain one, so a host
# value left in the unversioned variable would otherwise win the fallback.
# These name ONE EXECUTABLE, never a colon-joined list.
export GST_PLUGIN_SCANNER_1_0="$SNAP/usr/libexec/gstreamer-1.0/gst-plugin-scanner"
export GST_PLUGIN_SCANNER="$SNAP/usr/libexec/gstreamer-1.0/gst-plugin-scanner"
# $SNAP is read-only and its revision changes on every refresh, so the plugin
# registry cache has to live in the user's own (snap-confined) cache dir.
export GST_REGISTRY_1_0="${XDG_CACHE_HOME:-$HOME/.cache}/lightning/gst-registry.bin"
mkdir -p "$(dirname "$GST_REGISTRY_1_0")" 2>/dev/null || true
# SOCKETS THE SESSION PUTS IN THE RUNTIME DIR, BRIDGED INTO snapd'S.
#
# snapd remaps XDG_RUNTIME_DIR to $XDG_RUNTIME_DIR/snap.<name>. Everything a
# desktop session leaves in the REAL runtime dir -- the compositor socket,
# PipeWire, PulseAudio -- therefore sits one level up and is invisible to a
# client that resolves a relative name. snapcraft's desktop-launch bridges
# them; this launcher is hand-written and bridged none, and each absence is a
# different broken feature:
#
#   * wayland-0   -> Qt finds no platform plugin and the app ABORTS. The snap
#                    could not start on any Wayland session.
#   * pipewire-0  -> `micsrc` fails "Connection refused", the publish branch
#                    errors, and the pipeline cascades into
#                    "srtpenc0: Could not initialize SRTP encoder".
#   * pulse/native-> Qt Multimedia enumerates no audio devices at all, so the
#                    Sound & video picker reads "No microphone was found".
#
# All three measured under a real snapd on Ubuntu 24.04 (2026-09-13) with the
# mutation both ways -- the audio pair by counting `pa_context_connect()
# failed`: 1 without the bridge, 0 with it.
#
# Each is best-effort: a session that does not run PipeWire has nothing to
# bridge, and refusing to launch over that would be worse than launching
# without audio.
bridge_runtime_entry() {
    # $1 = path relative to the REAL runtime dir, e.g. "pipewire-0" or
    #      "pulse/native". Creates $XDG_RUNTIME_DIR/$1 -> ../<depth>/$1 .
    [ -n "${XDG_RUNTIME_DIR:-}" ] || return 0
    case "$1" in
        # ONE level of nesting only. The depth of `..` is computed from the
        # shape below, so a caller passing "a/b/c" would get a link that
        # resolves to the wrong place -- and because the next run finds it
        # existing, it would stay broken forever while the feature silently
        # did nothing. Refuse instead of guessing. Raised in review.
        */*/*) return 0 ;;
        */*) _bre_dir="${1%/*}"; _bre_up="../../" ;;
        *)   _bre_dir=""; _bre_up="../" ;;
    esac
    [ -e "$XDG_RUNTIME_DIR/$1" ] && return 0
    if [ -n "$_bre_dir" ]; then
        mkdir -p "$XDG_RUNTIME_DIR/$_bre_dir" 2>/dev/null || return 0
    else
        mkdir -p "$XDG_RUNTIME_DIR" 2>/dev/null || return 0
    fi
    ln -sf "$_bre_up$1" "$XDG_RUNTIME_DIR/$1" 2>/dev/null || true
}
# An ABSOLUTE or path-bearing WAYLAND_DISPLAY is left alone: libwayland takes
# the first as a path, and the second is not a display name at all.
case "${WAYLAND_DISPLAY:-}" in
    "") : ;;
    */*) : ;;
    *) bridge_runtime_entry "$WAYLAND_DISPLAY" ;;
esac
bridge_runtime_entry "pipewire-0"
bridge_runtime_entry "pulse/native"
# DATA FILES STRICT CONFINEMENT LEAVES THE APP WITHOUT, and the absence of
# these SEGFAULTED the snap immediately after it placed its window (exit 139).
# Under strict confinement /usr is the base snap's, and core24 carries no
# fontconfig configuration and no xkb keymaps at all, so the host's copies are
# unreachable by construction. The control that attributed the crash to these
# rather than to the renderer: the identical payload run UNCONFINED with
# QT_QUICK_BACKEND=software ran fine for 35 s.
#
# FONTS THEMSELVES ARE NOT STAGED, and that is deliberate. fontconfig's
# <dir> entries are ABSOLUTE, so a font tree under $SNAP is on no search path
# and would be inert; snapd's `desktop` interface bind-mounts the HOST's
# /usr/share/fonts and /var/cache/fontconfig into the sandbox, which is where
# the glyphs actually come from. What snapd does NOT provide is /etc/fonts --
# hence staging the configuration and only the configuration. Raised in
# review, where the first version staged fonts that nothing could find.
export FONTCONFIG_PATH="$SNAP/etc/fonts"
export FONTCONFIG_FILE="$SNAP/etc/fonts/fonts.conf"
export XKB_CONFIG_ROOT="$SNAP/usr/share/X11/xkb"
# GRAPHICS, THROUGH THE gpu-2404 CONTENT SNAP (see snap.yaml.in).
#
# The provider's wrapper sets LD_LIBRARY_PATH, __EGL_VENDOR_LIBRARY_DIRS, the
# dri driver path and the rest, then execs what it is handed. Without it the
# app gets no EGL -- the payload has only glvnd's DISPATCH stubs, because the
# vendor driver is dlopened and `ldd` never sees it -- and Qt falls back to a
# software renderer that cannot draw video at all.
#
# NOT a hard requirement: an unconnected snap still starts, on the software
# renderer, and the app says so in the UI. Refusing to launch would be worse.
GPU_WRAPPER="$SNAP/gpu-2404/bin/gpu-2404-provider-wrapper"
if [ -x "$GPU_WRAPPER" ]; then
    exec "$GPU_WRAPPER" "$SNAP/usr/bin/lightning-matrix" --backend=rust "$@"
fi
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
# Graphics. THE EARLIER AND CHEAPER GATE: validate-snap.sh asserts the same
# thing, but it runs in a later job, so a template edit that dropped the plug
# would build a snap and only fail forty minutes afterwards. Raised in review.
assert "gpu-2404" in meta["apps"]["lightning"]["plugs"]
gpu = meta["plugs"]["gpu-2404"]
assert gpu["interface"] == "content", gpu
assert gpu["target"] == "$SNAP/gpu-2404", gpu
assert gpu["default-provider"] == "mesa-2404", gpu
print("snap.yaml structurally valid")
EOF

mkdir -p dist
rm -f "$OUT"
# Standard snap squashfs settings (xz, root-owned).
mksquashfs "$TREE" "$OUT" -noappend -comp xz -all-root -no-xattrs >/dev/null
test -s "$OUT" || die "snap not produced"
write_sha256 "$OUT"
echo "built $OUT"
