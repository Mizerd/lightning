#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"

# Stage the GStreamer runtime Lightning's call media engine needs into a macOS
# .app bundle.
#
# WHY THIS EXISTS AT ALL. A GStreamer plugin is dlopen'd, never linked, so
# nothing in the executable's import table names one and macdeployqt — which
# walks Mach-O load commands — cannot discover a single plugin. Without this
# step the app links libgstreamer, launches, and then refuses every call with
# "missing_element:webrtcbin" because SfuMediaEngine::runtimeAvailable() probes
# its element factories before it registers.
#
# LAYOUT, AND WHY IT IS NOT THE OBVIOUS ONE.
#
#   Contents/PlugIns/gstreamer-plugins/   the plugins themselves
#   Contents/PlugIns/gstreamer-libs/      every dylib they load
#   Contents/MacOS/gstreamer-1.0          symlink -> ../PlugIns/gstreamer-plugins
#
# The app looks for the plugin directory at applicationDirPath()/gstreamer-1.0
# (src/calls/SfuMediaEngine.cpp), which on macOS is Contents/MacOS. A directory
# of that NAME cannot be signed anywhere in the bundle — measured, not guessed:
#
#   $ codesign --force --sign - Lightning.app
#   Lightning.app: bundle format unrecognized, invalid, or unsuitable
#   In subcomponent: Lightning.app/Contents/PlugIns/gstreamer-1.0
#
# It is the DOT, not the location. codesign's default resource rules treat a
# directory under MacOS/, PlugIns/, Frameworks/ ... as nested code, and a
# directory whose name carries an extension is taken for a bundle — extension
# "0", which is not a bundle format it knows. Three experiments on the same
# tree separated the two candidate causes:
#
#   Contents/MacOS/gstreamer-1.0    (real dir)  -> codesign FAILS
#   Contents/PlugIns/gstreamer-1.0  (real dir)  -> codesign FAILS, symlink or not
#   Contents/PlugIns/gstreamer-plugins          -> signs
#   Contents/MacOS/gstreamer-1.0 -> ../PlugIns/gstreamer-plugins (symlink)
#                                               -> signs
#
# Which is why the payload sits in dot-free PlugIns subdirectories — the shape
# Qt's own plugins already ship in here (PlugIns/platforms, PlugIns/quick) — and
# the dotted path the application asks for is a SYMLINK onto it. A symlink is
# sealed as a symlink, so codesign never tries to read a bundle out of it.
#
# INSTALL NAMES. The official GStreamer macOS framework is built relocatable:
# every library's install id and every inter-library dependency is already
# @rpath/libfoo.dylib. For the staged plugins and libraries that means no
# dependency has to be rewritten at all — only LC_RPATH. Two are added to each,
# because the plugins are reached through two different paths:
#
#   @executable_path/../PlugIns/gstreamer-libs      at run time, when the app
#       loads them via the Contents/MacOS/gstreamer-1.0 symlink and @loader_path
#       may or may not have been resolved through it;
#   @loader_path/../gstreamer-libs                  when validate-macos-artifacts
#       probes the real directory with a tool that lives somewhere else, where
#       @executable_path means nothing.
#
# The pre-baked rpaths from the framework (@loader_path/../lib and friends) are
# stripped first: they name directories that do not exist inside the bundle, and
# leaving them would let a stray sibling directory satisfy a load by accident.
#
# THE MAIN EXECUTABLE IS DIFFERENT, AND THIS IS THE SUBTLE PART.
#
# It links GStreamer directly, so gstreamer-1.0.pc's `Requires: glib-2.0
# gobject-2.0` puts @rpath/libglib-2.0.0.dylib, @rpath/libgobject-2.0.0.dylib
# and @rpath/libintl.8.dylib on it — and macdeployqt, which runs BEFORE this
# script, RESOLVES THOSE FROM HOMEBREW and rewrites them into
# Contents/Frameworks, because Qt links glib too and Homebrew's copy is already
# on its search list. Measured on the runner with a Qt+GStreamer test binary:
#
#   after macdeployqt:  @executable_path/../Frameworks/libglib-2.0.0.dylib
#                       (the file there is Homebrew's, compat 8801 — Qt needs it)
#   the plugins load:   PlugIns/gstreamer-libs/libglib-2.0.0.dylib  (compat 8201)
#
# Two GLib copies in one process is unavoidable and fine — Qt requires 8801 and
# GStreamer was built against 8201 — but the APPLICATION's own
# g_signal_connect/g_object_set calls operate on GstElements, so they have to
# reach the same GObject type system the plugins registered in. Bound to
# Homebrew's glib instead, they act on a type system that knows nothing about
# those objects.
#
# Deleting the builder rpath BEFORE macdeployqt does NOT prevent this — also
# measured: macdeployqt never needed our rpath to find Homebrew's glib, and the
# same three libraries were deployed either way. The only reliable repair is the
# one below: after staging, every dependency of the main executable whose
# basename is part of the staged GStreamer set is rewritten to an EXPLICIT
# @executable_path/../PlugIns/gstreamer-libs/... path, leaving dyld no search
# order to get wrong. System dependencies (/usr/lib, /System) are never touched,
# so a libz or libffi the platform provides stays the platform's.

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

# The plugins that carry the elements SfuMediaEngine requires, plus the ones
# the pipelines it builds reach for by name. Not the whole plugin directory:
# that is ~250 plugins pulling gtk4, ffmpeg and x265 into the closure for
# capabilities a Matrix client never uses.
#
#   app                  appsink/appsrc — decoded receive frames into QVideoSink
#   applemedia           avfvideosrc — camera AND `capture-screen=true` share
#   audioconvert         audioconvert
#   audiomixer           audiomixer (mixed receive path)
#   audioresample        audioresample
#   audiotestsrc         audiotestsrc — the silent source a muted publish uses
#   autodetect           autoaudiosrc/autoaudiosink/autovideosrc
#   compositor           compositor (refuted as the share's rate stage, still
#                        reachable from a pipeline description)
#   coreelements         queue valve capsfilter fakesink identity tee funnel
#   dtls                 dtlssrtpenc/dtlssrtpdec/dtlsenc/dtlsdec
#   level                level — the per-participant loudness meter
#   nice                 nicesrc/nicesink (ICE)
#   opus                 opusenc/opusdec
#   osxaudio             osxaudiosrc/osxaudiosink — what autoaudio* resolves to
#   rtp                  rtpopuspay/depay, rtpvp8pay/depay, rtpstorage
#   rtpmanager           rtpbin and the rest of webrtcbin's internals
#   sctp                 sctpenc/sctpdec — webrtcbin loads these for data
#   srtp                 srtpenc/srtpdec
#   videoconvertscale    videoconvert/videoscale
#   videorate            videorate — the pinned 30/1 stage
#   videotestsrc         videotestsrc
#   volume               volume — the per-participant gain
#   vpx                  vp8enc/vp8dec
#   webrtc               webrtcbin
#   webrtcdsp            webrtcdsp/webrtcechoprobe — the microphone AGC
#
# Lightning's own VP8 payloader is compiled into the binary and registered at
# gst_init time; it is deliberately not a plugin file.
PLUGINS=(
    app applemedia audioconvert audiomixer audioresample audiotestsrc
    autodetect compositor coreelements dtls level nice opus osxaudio
    rtp rtpmanager sctp srtp videoconvertscale videorate videotestsrc
    volume vpx webrtc webrtcdsp
)

rm -rf -- "$PLUGIN_DIR" "$SUPPORT_DIR" "$CONTENTS/MacOS/gstreamer-1.0"
mkdir -p "$PLUGIN_DIR" "$SUPPORT_DIR"

work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT

# The bundle is arm64-only (CMAKE_OSX_ARCHITECTURES=arm64) but the official
# GStreamer packages are universal, so half of every byte copied would be
# x86_64 that can never run here.
copy_thin() {
    local src="$1" dest="$2" lipo_err archs
    lipo_err="$(lipo -thin arm64 "$src" -output "$dest" 2>&1)" && return 0
    # Not a fat file, or something else went wrong. Only the first is
    # acceptable, and lipo's own message says which — reporting every failure as
    # an architecture problem would hide a full disk or an unwritable path.
    archs="$(lipo -archs "$src" 2>/dev/null || echo unknown)"
    [[ "$archs" == "arm64" ]] || die "cannot thin $src (archs: $archs): $lipo_err"
    cp "$src" "$dest" || die "cannot copy $src to $dest"
}

# otool -L prints the file path as a header, then (for a library) its own
# install id, then the dependencies — each dependency indented with a TAB. On a
# fat file it repeats the header per architecture, which is why the tab, and
# not `tail -n +2`, is what separates dependencies from headers.
deps_of() { otool -L "$1" 2>/dev/null | grep '^	' | awk '{print $1}'; }
# `|| true` is load-bearing: an executable has no LC_ID_DYLIB, so `grep -v`
# matches nothing and returns 1 — which under `set -o pipefail` makes the
# CALLER's assignment fail and `set -e` abort the script with no message.
id_of()   { otool -D "$1" 2>/dev/null | grep -v ':$' | head -1 || true; }
rpaths_of() {
    otool -l "$1" 2>/dev/null \
        | awk '/^ *cmd LC_RPATH$/{f=1;next} f&&/^ *path /{print $2; f=0}'
}
is_macho() { file -b "$1" 2>/dev/null | grep -q 'Mach-O'; }
# Every staged Mach-O, identified by CONTENT. A `-name '*.dylib'` filter would
# be narrower than what the closure below actually copies — the basename comes
# from the dependency record, which this script does not get to choose — and the
# files such a filter missed would be exactly the ones no later loop repairs or
# checks.
staged_machos() {
    find "$PLUGIN_DIR" "$SUPPORT_DIR" -type f 2>/dev/null | while IFS= read -r f; do
        is_macho "$f" && printf '%s\n' "$f"
    done
}

for p in "${PLUGINS[@]}"; do
    src="$GST_PREFIX/lib/gstreamer-1.0/libgst${p}.dylib"
    [[ -f "$src" ]] || die "GStreamer plugin not found in the SDK: $src"
    copy_thin "$src" "$PLUGIN_DIR/libgst${p}.dylib"
done
printf 'staged %d GStreamer plugins\n' "${#PLUGINS[@]}"

# Breadth-first closure over @rpath dependencies, resolved against the SDK's
# lib/. A dependency that does not resolve there is a hard failure: shipping a
# plugin whose library is absent produces a bundle that loads on this machine
# (where the SDK exists) and dies on a user's.
#
# Seeded from the MAIN EXECUTABLE as well as the plugins. Its @rpath
# dependencies are exactly the ones the linker took from gstreamer-1.0.pc, so a
# seventh pkg_check_modules module added to Lightning's CMake lands here rather
# than in a dyld error on a user's machine.
#
# The queue is a file rather than an array: /bin/bash on macOS is 3.2, where
# expanding an EMPTY array under `set -u` is an error, and the terminating
# round of a breadth-first walk is exactly an empty array.
: >"$work/libs"
: >"$work/queue"
printf '%s\n' "$MAIN_BINARY" >>"$work/queue"
for p in "${PLUGINS[@]}"; do
    printf '%s\n' "$PLUGIN_DIR/libgst${p}.dylib" >>"$work/queue"
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
                # Qt reaches the executable as @rpath/QtCore.framework/... —
                # a framework, not a plain dylib, and macdeployqt's business.
                @rpath/*.framework/*) continue ;;
                @rpath/*) base="${dep#@rpath/}" ;;
                # After macdeployqt the executable also carries @executable_path
                # entries; those are repaired further down, not walked here.
                /usr/lib/*|/System/*|@executable_path/*|@loader_path/*) continue ;;
                *)
                    # An absolute host path on the MAIN EXECUTABLE is Qt's, and
                    # it belongs to build-macos.sh's load-command repair pass and
                    # to the validator's self-containment scan — both of which
                    # already fail the build on it. Refusing it here as well
                    # would stop the GStreamer staging on a defect it does not
                    # own and cannot fix. On a STAGED library it is ours, and it
                    # means the SDK is not the relocatable framework we think.
                    [[ "$macho" == "$MAIN_BINARY" ]] && continue
                    die "$(basename "$macho") has a non-relocatable dependency: $dep"
                    ;;
            esac
            # A dependency that is not a plain file name would need a directory
            # tree, not a file copy. Nothing in this SDK ships one; refuse
            # rather than silently produce a path that cannot work.
            case "$base" in
                */*) die "$(basename "$macho") needs $dep, which is not a plain dylib" ;;
            esac
            [[ -f "$GST_PREFIX/lib/$base" ]] \
                || die "$(basename "$macho") needs $dep, absent from $GST_PREFIX/lib"
            grep -qxF "$base" "$work/libs" && continue
            printf '%s\n' "$base" >>"$work/libs"
            # Copy before queueing, so the next round reads the STAGED copy and
            # the closure is proven against what actually ships.
            copy_thin "$GST_PREFIX/lib/$base" "$SUPPORT_DIR/$base"
            printf '%s\n' "$SUPPORT_DIR/$base" >>"$work/next"
        done < <(deps_of "$macho")
    done <"$work/queue"
    mv "$work/next" "$work/queue"
done
support_count="$(wc -l <"$work/libs" | tr -d ' ')"
printf 'staged %s support libraries\n' "$support_count"

# --- install names -----------------------------------------------------------
# Every rpath list is SNAPSHOT to a file before the first install_name_tool
# call. `while read … done < <(otool -l "$f")` would keep otool streaming the
# very file being rewritten, and whether that is safe depends on whether the
# toolchain's install_name_tool edits in place or writes-and-renames.
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

# The builder's SDK path arrives on the executable by default:
# gstreamer-1.0.pc's `Libs:` line ends in -Wl,-rpath,${libdir}, so the link
# records an absolute rpath into the runner's home. It is dead on a user's
# machine and it is a builder path in a shipped artifact. A failed removal is
# fatal rather than logged: reporting a removal that did not happen is the worst
# possible line to read during an incident.
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

# Bind the executable's GStreamer stack EXPLICITLY — see the long note at the
# top of this file. @rpath would resolve, but it leaves dyld a search order, and
# macdeployqt has already pointed some of these at Homebrew's copies in
# Contents/Frameworks.
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
# Not decoration: every failure mode of this script produces a bundle that runs
# perfectly on the build machine.
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
                # Qt's own payload there is macdeployqt's business; a GStreamer
                # library reached through it is the wrong copy.
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
