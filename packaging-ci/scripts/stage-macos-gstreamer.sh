#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"

# Stage the GStreamer runtime the call media engine needs into a macOS .app.
#
# Plugins are dlopen'd, so macdeployqt cannot discover them; without this step
# the app refuses every call with "missing_element:webrtcbin".
#
# Layout:
#   Contents/PlugIns/gstreamer-plugins/   the plugins
#   Contents/PlugIns/gstreamer-libs/      every dylib they load
#   Contents/MacOS/gstreamer-1.0          symlink -> ../PlugIns/gstreamer-plugins
#
# The app looks in applicationDirPath()/gstreamer-1.0, but codesign rejects a
# real directory with that name anywhere in the bundle: a dotted directory
# name is taken for a nested bundle ("bundle format unrecognized"). A symlink
# is sealed as a symlink, so the payload lives in dot-free directories and the
# dotted path points at it.
#
# The official framework is relocatable (@rpath everywhere), so only LC_RPATH
# needs changing. Each staged binary gets two rpaths:
#   @executable_path/../PlugIns/gstreamer-libs   when the app loads it;
#   @loader_path/../gstreamer-libs               when the validator probes the
#                                                directory from another tool.
# The framework's own rpaths are stripped first; they name directories that do
# not exist in the bundle.
#
# The main executable links glib/gobject/libintl directly, and macdeployqt
# (which runs first) resolves those to Homebrew's copies in
# Contents/Frameworks, which Qt needs. Two GLib copies in one process are
# fine, but the app's own g_object_* calls on GstElements must reach the same
# GObject type system the plugins registered in. So after staging, every
# executable dependency that is part of the staged set is rewritten to an
# explicit @executable_path/../PlugIns/gstreamer-libs/... path. System
# libraries (/usr/lib, /System) are never touched.

APP_DIR="${1:-}"
GST_PREFIX="${2:-}"
[[ -n "$APP_DIR" && -n "$GST_PREFIX" ]] \
    || die "usage: stage-macos-gstreamer.sh <App.app> <GStreamer.framework/Versions/1.0>"
[[ -d "$APP_DIR/Contents/MacOS" ]] || die "not an app bundle: $APP_DIR"
[[ -d "$GST_PREFIX/lib/gstreamer-1.0" ]] \
    || die "no GStreamer plugins at $GST_PREFIX/lib/gstreamer-1.0 (run install-macos-gstreamer.sh)"

CONTENTS="$APP_DIR/Contents"
PLUGIN_DIR="$CONTENTS/PlugIns/gstreamer-plugins"
SUPPORT_DIR="$CONTENTS/PlugIns/gstreamer-libs"
SUPPORT_REL="PlugIns/gstreamer-libs"

APP_NAME="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$CONTENTS/Info.plist" 2>/dev/null || true)"
[[ -n "$APP_NAME" ]] || die "Info.plist has no CFBundleExecutable"
MAIN_BINARY="$CONTENTS/MacOS/$APP_NAME"
[[ -x "$MAIN_BINARY" ]] || die "main executable not found: $MAIN_BINARY"

# Plugins carrying the elements SfuMediaEngine requires or its pipelines name,
# not the whole directory (~250 plugins pulling in gtk4, ffmpeg and x265).
#
#   app                  appsink/appsrc: decoded receive frames into QVideoSink
#   applemedia           avfvideosrc: camera and `capture-screen=true` share
#   audioconvert         audioconvert
#   audiomixer           audiomixer (mixed receive path)
#   audioresample        audioresample
#   audiotestsrc         audiotestsrc: the silent source a muted publish uses
#   autodetect           autoaudiosrc/autoaudiosink/autovideosrc
#   compositor           compositor (reachable from a pipeline description)
#   coreelements         queue valve capsfilter fakesink identity tee funnel
#   dtls                 dtlssrtpenc/dtlssrtpdec/dtlsenc/dtlsdec
#   jpeg                 jpegenc/jpegdec: the camera's compressed chain. The app
#                        decides whether a camera uses it by building
#                        `videotestsrc ! jpegenc ! <entry> ! fakesink`, so
#                        jpegenc is needed for that decision too. Without it a
#                        camera falls back to the much slower raw entry.
#   level                level: the per-participant loudness meter
#   nice                 nicesrc/nicesink (ICE)
#   opus                 opusenc/opusdec
#   osxaudio             osxaudiosrc/osxaudiosink: what autoaudio* resolves to
#   rtp                  rtpopuspay/depay, rtpvp8pay/depay, rtpstorage
#   rtpmanager           rtpbin and the rest of webrtcbin's internals
#   sctp                 sctpenc/sctpdec: webrtcbin loads these for data
#   srtp                 srtpenc/srtpdec
#   videoconvertscale    videoconvert/videoscale
#   videorate            videorate: the pinned 30/1 stage
#   videotestsrc         videotestsrc
#   volume               volume: the per-participant gain
#   vpx                  vp8enc/vp8dec
#   webrtc               webrtcbin
#   webrtcdsp            webrtcdsp/webrtcechoprobe: the microphone AGC
#
# Lightning's own VP8 payloader is compiled into the binary, not a plugin.
PLUGINS=(
    app applemedia audioconvert audiomixer audioresample audiotestsrc
    autodetect compositor coreelements dtls level nice opus osxaudio
    rtp rtpmanager sctp srtp videoconvertscale videorate videotestsrc
    volume vpx webrtc webrtcdsp
)

# Optional until a macOS job has proven it stages. The loop below dies on a
# missing required plugin and macOS is allow_failure, so requiring it first
# would silently cost a release its macOS asset.
#
# TO PROMOTE IT: once a macOS job reports `staged optional GStreamer plugin:
# jpeg` and `camera compressed (MJPG) chain: available`, move `jpeg` into
# PLUGINS above and add it to the required list in validate-macos-artifacts.sh
# — both, in one change.
OPTIONAL_PLUGINS=(jpeg)

rm -rf -- "$PLUGIN_DIR" "$SUPPORT_DIR" "$CONTENTS/MacOS/gstreamer-1.0"
mkdir -p "$PLUGIN_DIR" "$SUPPORT_DIR"

work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT

# The bundle is arm64-only but the official packages are universal.
copy_thin() {
    local src="$1" dest="$2" lipo_err archs
    lipo_err="$(lipo -thin arm64 "$src" -output "$dest" 2>&1)" && return 0
    # Only "not a fat file" is acceptable here; any other lipo failure (full
    # disk, unwritable path) must be reported as itself.
    archs="$(lipo -archs "$src" 2>/dev/null || echo unknown)"
    [[ "$archs" == "arm64" ]] || die "cannot thin $src (archs: $archs): $lipo_err"
    cp "$src" "$dest" || die "cannot copy $src to $dest"
}

# otool -L indents each dependency with a tab; on a fat file it repeats the
# header per architecture, so the tab (not `tail -n +2`) separates them.
deps_of() { otool -L "$1" 2>/dev/null | grep '^	' | awk '{print $1}'; }
# `|| true`: an executable has no LC_ID_DYLIB, so grep returns 1, which under
# pipefail would abort the caller with no message.
id_of()   { otool -D "$1" 2>/dev/null | grep -v ':$' | head -1 || true; }
rpaths_of() {
    otool -l "$1" 2>/dev/null \
        | awk '/^ *cmd LC_RPATH$/{f=1;next} f&&/^ *path /{print $2; f=0}'
}
is_macho() { file -b "$1" 2>/dev/null | grep -q 'Mach-O'; }
# Every staged Mach-O, found by content rather than by a `*.dylib` name filter
# (basenames come from dependency records). $SCANNER_DEST is listed explicitly
# because it lives in MacOS/, not PlugIns/, and must still get the rpath
# retarget, the self-containment check and the arch check.
staged_machos() {
    find "$PLUGIN_DIR" "$SUPPORT_DIR" "$SCANNER_DEST" -type f 2>/dev/null | while IFS= read -r f; do
        is_macho "$f" && printf '%s\n' "$f"
    done
}

for p in "${PLUGINS[@]}"; do
    src="$GST_PREFIX/lib/gstreamer-1.0/libgst${p}.dylib"
    [[ -f "$src" ]] || die "GStreamer plugin not found in the SDK: $src"
    copy_thin "$src" "$PLUGIN_DIR/libgst${p}.dylib"
done
staged_optional=0
for p in "${OPTIONAL_PLUGINS[@]}"; do
    src="$GST_PREFIX/lib/gstreamer-1.0/libgst${p}.dylib"
    if [[ -f "$src" ]]; then
        copy_thin "$src" "$PLUGIN_DIR/libgst${p}.dylib"
        staged_optional=$((staged_optional + 1))
        printf 'staged optional GStreamer plugin: %s\n' "$p"
    else
        printf 'WARNING: optional GStreamer plugin not in the SDK: %s (%s)\n' \
            "$p" "$src" >&2
    fi
done
printf 'staged %d GStreamer plugins (+%d optional)\n' \
    "${#PLUGINS[@]}" "$staged_optional"

# gst-plugin-scanner builds the plugin registry out of process. Its path is
# compiled into libgstreamer as the builder's libexec directory, so without a
# bundled copy every launch logs "External plugin loader failed" and GStreamer
# falls back to scanning in-process.
#
# It lives in Contents/MacOS beside the main executable (no dot in the path,
# and build-macos.sh's inside-out codesign pass signs it; an unsigned helper is
# SIGKILLed on Apple Silicon). The app exports GST_PLUGIN_SCANNER from its own
# location before gst_init (src/calls/GstBootstrap.cpp).
SCANNER_SRC="$GST_PREFIX/libexec/gstreamer-1.0/gst-plugin-scanner"
SCANNER_DEST="$CONTENTS/MacOS/gst-plugin-scanner"
[[ -f "$SCANNER_SRC" ]] \
    || die "gst-plugin-scanner not found in the SDK: $SCANNER_SRC"
rm -f -- "$SCANNER_DEST"
copy_thin "$SCANNER_SRC" "$SCANNER_DEST"
chmod 0755 "$SCANNER_DEST"
printf 'staged the GStreamer registry helper (gst-plugin-scanner)\n'

# Breadth-first closure over @rpath dependencies, resolved against the SDK's
# lib/. An unresolvable dependency is fatal: the bundle would work only where
# the SDK is installed.
#
# Seeded from the main executable too, so a new GStreamer module linked by
# Lightning's CMake is picked up here rather than failing in dyld.
#
# The queue is a file, not an array: macOS /bin/bash 3.2 errors on expanding an
# empty array under `set -u`.
: >"$work/libs"
: >"$work/queue"
printf '%s\n' "$MAIN_BINARY" >>"$work/queue"
# The registry helper runs as its own process with no Qt, so its core and glib
# dependencies must be bundled too.
printf '%s\n' "$SCANNER_DEST" >>"$work/queue"
for p in "${PLUGINS[@]}"; do
    printf '%s\n' "$PLUGIN_DIR/libgst${p}.dylib" >>"$work/queue"
done
# Optional plugins are seeded from what was actually staged.
for p in "${OPTIONAL_PLUGINS[@]}"; do
    [[ -f "$PLUGIN_DIR/libgst${p}.dylib" ]] \
        && printf '%s\n' "$PLUGIN_DIR/libgst${p}.dylib" >>"$work/queue"
done
while [[ -s "$work/queue" ]]; do
    : >"$work/next"
    while IFS= read -r macho; do
        [[ -n "$macho" ]] || continue
        self="$(id_of "$macho")"
        while IFS= read -r dep; do
            [[ -n "$dep" ]] || continue
            [[ "$dep" == "$self" ]] && continue
            case "$dep" in
                # Qt frameworks are macdeployqt's business.
                @rpath/*.framework/*) continue ;;
                @rpath/*) base="${dep#@rpath/}" ;;
                # @executable_path entries are repaired further down.
                /usr/lib/*|/System/*|@executable_path/*|@loader_path/*) continue ;;
                *)
                    # An absolute host path on the main executable is Qt's and
                    # is handled by build-macos.sh's repair pass and the
                    # validator. On a staged library it means the SDK is not
                    # the relocatable framework we expect.
                    [[ "$macho" == "$MAIN_BINARY" ]] && continue
                    die "$(basename "$macho") has a non-relocatable dependency: $dep"
                    ;;
            esac
            # A dependency needing a directory tree cannot be a file copy.
            case "$base" in
                */*) die "$(basename "$macho") needs $dep, which is not a plain dylib" ;;
            esac
            [[ -f "$GST_PREFIX/lib/$base" ]] \
                || die "$(basename "$macho") needs $dep, absent from $GST_PREFIX/lib"
            grep -qxF "$base" "$work/libs" && continue
            printf '%s\n' "$base" >>"$work/libs"
            # Copy before queueing, so the next round walks the staged copy.
            copy_thin "$GST_PREFIX/lib/$base" "$SUPPORT_DIR/$base"
            printf '%s\n' "$SUPPORT_DIR/$base" >>"$work/next"
        done < <(deps_of "$macho")
    done <"$work/queue"
    mv "$work/next" "$work/queue"
done
support_count="$(wc -l <"$work/libs" | tr -d ' ')"
printf 'staged %s support libraries\n' "$support_count"

# --- install names -----------------------------------------------------------
# Snapshot each rpath list before rewriting: streaming otool over a file that
# install_name_tool is modifying is only safe if it writes in place.
retarget_rpaths() {
    local macho="$1" rp
    rpaths_of "$macho" >"$work/rpaths"
    while IFS= read -r rp; do
        [[ -n "$rp" ]] || continue
        install_name_tool -delete_rpath "$rp" "$macho" 2>/dev/null || true
    done <"$work/rpaths"
    install_name_tool -add_rpath "@executable_path/../$SUPPORT_REL" "$macho"
    install_name_tool -add_rpath "@loader_path/../gstreamer-libs" "$macho"
}
staged_machos >"$work/staged"
staged=0
while IFS= read -r macho; do
    [[ -n "$macho" ]] || continue
    retarget_rpaths "$macho"
    staged=$((staged + 1))
done <"$work/staged"
printf 'retargeted rpaths on %d staged binaries\n' "$staged"

# gst-plugin-scanner sits in MacOS/, where `@loader_path/../gstreamer-libs`
# resolves to nothing. This covers it being exec'd with the app as the main
# executable.
install_name_tool -add_rpath "@loader_path/../$SUPPORT_REL" "$SCANNER_DEST" \
    || die "could not point the registry helper at the bundled libraries"

# gstreamer-1.0.pc adds -Wl,-rpath,${libdir}, leaving an absolute builder path
# on the executable. Remove it; a failed removal is fatal.
removed_host_rpaths=0
rpaths_of "$MAIN_BINARY" >"$work/main-rpaths"
while IFS= read -r rp; do
    [[ -n "$rp" ]] || continue
    case "$rp" in
        @executable_path*|@loader_path*) continue ;;
    esac
    install_name_tool -delete_rpath "$rp" "$MAIN_BINARY" \
        || die "could not remove the builder rpath from the executable: $rp"
    printf 'removed host rpath from the executable: %s\n' "$rp"
    removed_host_rpaths=$((removed_host_rpaths + 1))
done <"$work/main-rpaths"
if ! rpaths_of "$MAIN_BINARY" | grep -qxF "@executable_path/../$SUPPORT_REL"; then
    install_name_tool -add_rpath "@executable_path/../$SUPPORT_REL" "$MAIN_BINARY"
fi

# Bind the executable's GStreamer stack explicitly (see the note at the top):
# macdeployqt has already pointed some of these at Homebrew's copies.
rebound=0
deps_of "$MAIN_BINARY" >"$work/main-deps"
while IFS= read -r dep; do
    [[ -n "$dep" ]] || continue
    case "$dep" in
        @rpath/*|@executable_path/../Frameworks/*) ;;
        *) continue ;;
    esac
    base="${dep##*/}"
    [[ -f "$SUPPORT_DIR/$base" ]] || continue
    target="@executable_path/../$SUPPORT_REL/$base"
    [[ "$dep" == "$target" ]] && continue
    install_name_tool -change "$dep" "$target" "$MAIN_BINARY" \
        || die "could not rebind $dep on the executable"
    printf 'rebound the executable: %s -> %s\n' "$dep" "$target"
    rebound=$((rebound + 1))
done <"$work/main-deps"

# The path the application actually looks in.
ln -s "../PlugIns/gstreamer-plugins" "$CONTENTS/MacOS/gstreamer-1.0"
[[ -d "$CONTENTS/MacOS/gstreamer-1.0" ]] || die "the plugin symlink does not resolve"

# --- prove the staged set is closed and self-contained ------------------------
# Every failure mode of this script produces a bundle that runs on the build
# machine, so check self-containment explicitly.
unresolved=0
{ cat "$work/staged"; printf '%s\n' "$MAIN_BINARY"; } >"$work/verify"
while IFS= read -r macho; do
    [[ -n "$macho" ]] || continue
    self="$(id_of "$macho")"
    while IFS= read -r dep; do
        [[ -n "$dep" ]] || continue
        [[ "$dep" == "$self" ]] && continue
        base="${dep##*/}"
        case "$dep" in
            /usr/lib/*|/System/*) continue ;;
            @rpath/*.framework/*) continue ;;
            @rpath/*)
                if [[ -f "$SUPPORT_DIR/$base" && "$macho" == "$MAIN_BINARY" ]]; then
                    printf '  not rebound: the executable still loads %s by search\n' "$dep" >&2
                    unresolved=$((unresolved + 1))
                elif [[ ! -f "$SUPPORT_DIR/$base" ]]; then
                    printf '  unresolved: %s needs %s\n' "${macho#"$APP_DIR"/}" "$dep" >&2
                    unresolved=$((unresolved + 1))
                fi
                ;;
            @executable_path/../Frameworks/*)
                # A GStreamer library reached through Qt's Frameworks is the
                # wrong copy.
                if [[ -f "$SUPPORT_DIR/$base" ]]; then
                    printf '  wrong copy: %s loads %s from the Qt frameworks\n' \
                        "${macho#"$APP_DIR"/}" "$base" >&2
                    unresolved=$((unresolved + 1))
                fi
                ;;
            @executable_path/*|@loader_path/*) continue ;;
            *)
                printf '  absolute dependency: %s needs %s\n' "${macho#"$APP_DIR"/}" "$dep" >&2
                unresolved=$((unresolved + 1))
                ;;
        esac
    done < <(deps_of "$macho")
done <"$work/verify"
(( unresolved == 0 )) || die "$unresolved staged GStreamer dependencies do not resolve inside the bundle"

printf 'GStreamer staged: %d plugins, %s libraries, %d host rpaths removed, %d deps rebound\n' \
    "${#PLUGINS[@]}" "$support_count" "$removed_host_rpaths" "$rebound"
