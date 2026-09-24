#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"

# Structural validation of the assembled Lightning.app: well-formed,
# self-contained, arm64 and runnable here. It does not prove distributability;
# Gatekeeper rejection is expected and reported. See docs/macos-packaging.md.

[[ $# -eq 1 ]] || die "usage: validate-macos-artifacts.sh <dist/macos>"
MACOS_DIST="$1"
APP_NAME="Lightning"
APP_DIR="$MACOS_DIST/${APP_NAME}.app"
CONTENTS="$APP_DIR/Contents"
REPORT_DIR="$MACOS_DIST/reports"
mkdir -p "$REPORT_DIR"

[[ -d "$APP_DIR" ]] || die "bundle not found: $APP_DIR"

failures=0
check() {
    local desc="$1"; shift
    if "$@" >/dev/null 2>&1; then
        printf '  ok: %s\n' "$desc"
    else
        printf '  FAIL: %s\n' "$desc" >&2
        failures=$((failures + 1))
    fi
}

printf 'Validating %s\n' "$APP_DIR"

# --- bundle structure --------------------------------------------------------
check "Info.plist present"            test -f "$CONTENTS/Info.plist"
check "executable present"            test -x "$CONTENTS/MacOS/$APP_NAME"
check "icon present"                  test -f "$CONTENTS/Resources/${APP_NAME}.icns"
check "PkgInfo present"               test -f "$CONTENTS/PkgInfo"
check "build-info.json present"       test -f "$CONTENTS/Resources/build-info.json"
check "Frameworks directory present"  test -d "$CONTENTS/Frameworks"

# --- Info.plist keys ---------------------------------------------------------
plist_get() { /usr/libexec/PlistBuddy -c "Print :$1" "$CONTENTS/Info.plist" 2>/dev/null; }
for key in CFBundleIdentifier CFBundleExecutable CFBundleName \
           CFBundleShortVersionString CFBundleVersion LSMinimumSystemVersion; do
    check "Info.plist has $key" test -n "$(plist_get "$key")"
done
# macOS kills a process that opens the microphone without a usage string.
check "Info.plist declares NSMicrophoneUsageDescription" \
    test -n "$(plist_get NSMicrophoneUsageDescription)"

BUNDLE_VERSION="$(plist_get CFBundleShortVersionString || true)"
BUNDLE_ID="$(plist_get CFBundleIdentifier || true)"
printf '  bundle: %s %s\n' "$BUNDLE_ID" "$BUNDLE_VERSION"

# --- architecture ------------------------------------------------------------
ARCHS="$(lipo -archs "$CONTENTS/MacOS/$APP_NAME" 2>/dev/null || echo unknown)"
printf '  architectures: %s\n' "$ARCHS"
check "main executable is arm64" test "$ARCHS" = "arm64"
check "main executable is Mach-O" sh -c \
    "file -b '$CONTENTS/MacOS/$APP_NAME' | grep -q 'Mach-O.*arm64'"

# --- self-containment --------------------------------------------------------
# Every dependency must resolve inside the bundle or in /System and /usr/lib.
#
# The install name (LC_ID_DYLIB) is excluded: macdeployqt leaves Homebrew's
# framework IDs unchanged, but dyld follows the dependent's load command, not
# the ID. `otool -D` yields the ID so it can be subtracted exactly.
deps_of() {
    local macho="$1" id
    id="$(otool -D "$macho" 2>/dev/null | tail -n +2 | head -1)"
    otool -L "$macho" 2>/dev/null | tail -n +2 | sed -E 's/^[[:space:]]*//; s/ \(compatibility.*$//' \
        | { if [[ -n "$id" ]]; then grep -vxF "$id"; else cat; fi; }
}

otool -L "$CONTENTS/MacOS/$APP_NAME" >"$REPORT_DIR/otool-main.txt" 2>&1 || true
external="$(deps_of "$CONTENTS/MacOS/$APP_NAME" | grep -E '^(/opt/homebrew|/usr/local)' || true)"
if [[ -n "$external" ]]; then
    printf '  FAIL: executable links libraries outside the bundle:\n%s\n' "$external" >&2
    failures=$((failures + 1))
else
    printf '  ok: executable has no /opt/homebrew or /usr/local dependencies\n'
fi

# Mach-O files are identified by content: framework payloads have neither a
# .dylib suffix nor a consistent mode.
: >"$REPORT_DIR/otool-all.txt"
leaked=0
checked=0
while IFS= read -r candidate; do
    file -b "$candidate" 2>/dev/null | grep -q 'Mach-O' || continue
    checked=$((checked + 1))
    { printf '== %s\n' "${candidate#$APP_DIR/}"; otool -L "$candidate" 2>&1; } >>"$REPORT_DIR/otool-all.txt"
    if deps_of "$candidate" | grep -qE '^(/opt/homebrew|/usr/local)'; then
        printf '  leaked host dependency in: %s\n' "${candidate#$APP_DIR/}" >&2
        leaked=$((leaked + 1))
    fi
done < <(find "$APP_DIR" -type f 2>/dev/null)
if (( leaked > 0 )); then
    printf '  FAIL: %d of %d bundled binaries depend on host-only paths\n' "$leaked" "$checked" >&2
    failures=$((failures + 1))
else
    printf '  ok: none of %d bundled binaries depend on host-only paths\n' "$checked"
fi

# --- Qt runtime --------------------------------------------------------------
# The modules Lightning links; a missing one kills the app at launch.
for fw in QtCore QtGui QtQml QtQuick QtQuickControls2 QtNetwork QtSql QtWidgets QtMultimedia; do
    check "framework bundled: $fw" test -d "$CONTENTS/Frameworks/${fw}.framework"
done
# Without the QML plugins the UI fails to instantiate even though it links.
check "QML modules bundled" test -d "$CONTENTS/Resources/qml"
for qml_mod in QtQuick QtQml; do
    check "QML module present: $qml_mod" test -d "$CONTENTS/Resources/qml/$qml_mod"
done
# Without the cocoa platform plugin Qt cannot create a window.
check "cocoa platform plugin bundled" \
    test -f "$CONTENTS/PlugIns/platforms/libqcocoa.dylib"
# Received media must never be decoded as SVG (build-macos.sh prunes it).
check "no SVG image-format plugin" \
    bash -c '! compgen -G "$1/PlugIns/imageformats/*svg*" >/dev/null' _ "$CONTENTS"

qt_fw_count="$(find "$CONTENTS/Frameworks" -maxdepth 1 -name '*.framework' 2>/dev/null | wc -l | tr -d ' ')"
printf '  bundled frameworks: %s\n' "$qt_fw_count"

# --- call media engine (GStreamer) -------------------------------------------
# A bundle with no plugins launches and syncs but refuses every call, so this
# section ends by building a real registry from the bundled plugins and asking
# for every element by name.
GST_PLUGIN_LINK="$CONTENTS/MacOS/gstreamer-1.0"
GST_PLUGIN_DIR="$CONTENTS/PlugIns/gstreamer-plugins"
GST_LIB_DIR="$CONTENTS/PlugIns/gstreamer-libs"
GST_SCANNER="$CONTENTS/MacOS/gst-plugin-scanner"

# The app looks only in applicationDirPath()/gstreamer-1.0, which must be a
# symlink because codesign rejects a dotted directory name (see
# stage-macos-gstreamer.sh). Check that it is a symlink, that it resolves, and
# that it resolves to the staged payload.
check "plugin path is a symlink"      test -L "$GST_PLUGIN_LINK"
check "plugin path resolves"          test -d "$GST_PLUGIN_LINK"
check "plugin payload directory"      test -d "$GST_PLUGIN_DIR"
check "support library directory"     test -d "$GST_LIB_DIR"
if [[ -d "$GST_PLUGIN_LINK" && -d "$GST_PLUGIN_DIR" ]] \
   && [[ "$(cd "$GST_PLUGIN_LINK" && pwd -P)" == "$(cd "$GST_PLUGIN_DIR" && pwd -P)" ]]; then
    printf '  ok: the plugin symlink points at the staged plugins\n'
else
    printf '  FAIL: %s does not resolve to %s\n' "$GST_PLUGIN_LINK" "$GST_PLUGIN_DIR" >&2
    failures=$((failures + 1))
fi

for plugin in app applemedia audioconvert audiomixer audioresample audiotestsrc \
              autodetect compositor coreelements dtls level nice opus osxaudio \
              rtp rtpmanager sctp srtp videoconvertscale videorate videotestsrc \
              volume vpx webrtc webrtcdsp; do
    check "GStreamer plugin bundled: $plugin" \
        test -f "$GST_PLUGIN_DIR/libgst${plugin}.dylib"
done
check "GStreamer core library bundled" test -f "$GST_LIB_DIR/libgstreamer-1.0.0.dylib"

# Without the registry helper GStreamer silently scans in-process and logs
# "External plugin loader failed". An unsigned helper is SIGKILLed on Apple
# Silicon with no message, so presence, mode and signature are all checked.
check "GStreamer registry helper bundled" test -f "$GST_SCANNER"
check "GStreamer registry helper is executable" test -x "$GST_SCANNER"
if [[ -f "$GST_SCANNER" ]]; then
    if codesign --verify --strict "$GST_SCANNER" >/dev/null 2>&1; then
        printf '  ok: the registry helper carries a valid signature\n'
    else
        printf '  FAIL: the registry helper is unsigned or its signature is stale\n' >&2
        printf '        (Apple Silicon SIGKILLs such a binary with no message, which\n' >&2
        printf '         is indistinguishable from the helper being absent)\n' >&2
        failures=$((failures + 1))
    fi
fi

# The executable must link the bundled GStreamer explicitly. macdeployqt
# resolves its glib/gobject/libintl dependencies to Homebrew's copies in
# Contents/Frameworks; the staging script rebinds them and this proves it.
main_deps="$(otool -L "$CONTENTS/MacOS/$APP_NAME" 2>/dev/null | grep '^	' | awk '{print $1}' || true)"
if printf '%s\n' "$main_deps" \
     | grep -qxF "@executable_path/../PlugIns/gstreamer-libs/libgstreamer-1.0.0.dylib"; then
    printf '  ok: the executable links the bundled GStreamer explicitly\n'
else
    printf '  FAIL: the executable does not load libgstreamer-1.0.0.dylib from\n' >&2
    printf '        PlugIns/gstreamer-libs — the media engine is missing or misbound\n' >&2
    failures=$((failures + 1))
fi
misbound=0
while IFS= read -r dep; do
    [[ -n "$dep" ]] || continue
    base="${dep##*/}"
    [[ -f "$GST_LIB_DIR/$base" ]] || continue
    case "$dep" in
        "@executable_path/../PlugIns/gstreamer-libs/$base") continue ;;
    esac
    printf '  the executable loads %s from %s, not from the staged copy\n' \
        "$base" "$dep" >&2
    misbound=$((misbound + 1))
done <<<"$main_deps"
if (( misbound > 0 )); then
    printf '  FAIL: %d GStreamer libraries are loaded from the wrong copy\n' "$misbound" >&2
    failures=$((failures + 1))
else
    printf '  ok: every GStreamer library the executable loads is the staged copy\n'
fi
# Qt never links GStreamer on macOS (Qt Multimedia uses AVFoundation), so a
# libgst* in the Qt framework directory can only be a second instance.
if find "$CONTENTS/Frameworks" -maxdepth 1 -name 'libgst*' 2>/dev/null | grep -q .; then
    printf '  FAIL: a second GStreamer copy is present in Contents/Frameworks\n' >&2
    failures=$((failures + 1))
else
    printf '  ok: no GStreamer libraries in the Qt framework directory\n'
fi

# gstreamer-1.0.pc adds an absolute rpath into the runner's home. Scoped to the
# executable and the staged payload: Homebrew's own libraries carry
# /opt/homebrew/Cellar rpaths that this step did not add.
rpaths_of() {
    otool -l "$1" 2>/dev/null \
        | awk '/^ *cmd LC_RPATH$/{f=1;next} f&&/^ *path /{print $2; f=0}'
}
host_rpaths=0
while IFS= read -r macho; do
    file -b "$macho" 2>/dev/null | grep -q 'Mach-O' || continue
    while IFS= read -r rp; do
        [[ -n "$rp" ]] || continue
        case "$rp" in
            @executable_path*|@loader_path*|/System/*|/usr/lib/*) continue ;;
        esac
        printf '  builder rpath in %s: %s\n' "${macho#"$APP_DIR"/}" "$rp" >&2
        host_rpaths=$((host_rpaths + 1))
    done < <(rpaths_of "$macho")
done < <(find "$CONTENTS/MacOS/$APP_NAME" "$GST_SCANNER" "$GST_PLUGIN_DIR" \
              "$GST_LIB_DIR" -type f 2>/dev/null)
if (( host_rpaths > 0 )); then
    printf '  FAIL: %d builder rpaths survive in the executable or the staged runtime\n' "$host_rpaths" >&2
    failures=$((failures + 1))
else
    printf '  ok: no builder rpaths in the executable or the staged GStreamer runtime\n'
fi

# Closure: every @rpath dependency of every staged binary must be in
# gstreamer-libs, and every staged binary must be arm64. The element probe
# below implies neither. Mach-O files are identified by content, not name.
gst_unresolved=0
gst_wrong_arch=0
while IFS= read -r macho; do
    file -b "$macho" 2>/dev/null | grep -q 'Mach-O' || continue
    # `|| true`: no LC_ID_DYLIB makes grep return 1 under pipefail.
    self="$(otool -D "$macho" 2>/dev/null | grep -v ':$' | head -1 || true)"
    if [[ "$(lipo -archs "$macho" 2>/dev/null)" != "arm64" ]]; then
        printf '  not arm64: %s\n' "${macho#"$APP_DIR"/}" >&2
        gst_wrong_arch=$((gst_wrong_arch + 1))
    fi
    while IFS= read -r dep; do
        [[ -n "$dep" ]] || continue
        [[ "$dep" == "$self" ]] && continue
        case "$dep" in
            /usr/lib/*|/System/*) continue ;;
            @rpath/*)
                [[ -f "$GST_LIB_DIR/${dep#@rpath/}" ]] && continue
                printf '  %s needs %s, absent from gstreamer-libs\n' \
                    "${macho#"$APP_DIR"/}" "$dep" >&2
                ;;
            *) printf '  %s has a non-relocatable dependency: %s\n' \
                    "${macho#"$APP_DIR"/}" "$dep" >&2 ;;
        esac
        gst_unresolved=$((gst_unresolved + 1))
    done < <(otool -L "$macho" 2>/dev/null | grep '^	' | awk '{print $1}')
done < <(find "$GST_SCANNER" "$GST_PLUGIN_DIR" "$GST_LIB_DIR" -type f 2>/dev/null)
if (( gst_unresolved > 0 )); then
    printf '  FAIL: %d staged GStreamer dependencies do not resolve inside the bundle\n' \
        "$gst_unresolved" >&2
    failures=$((failures + 1))
else
    printf '  ok: every staged GStreamer dependency resolves inside the bundle\n'
fi
if (( gst_wrong_arch > 0 )); then
    printf '  FAIL: %d staged GStreamer binaries are not arm64\n' "$gst_wrong_arch" >&2
    failures=$((failures + 1))
else
    printf '  ok: every staged GStreamer binary is arm64\n'
fi

# Build a real registry through the symlink the application itself uses, and
# ask for every element.
#
# The probe tool is a copy of the SDK's gst-inspect with its rpaths replaced by
# one absolute rpath into the bundle's gstreamer-libs. Used as-is, its
# @executable_path/../lib rpath would let the SDK satisfy anything the bundle
# is missing.
#
# The versioned GST_*_1_0 variables are cleared too because GStreamer reads
# them first; a leftover one would point the scan at the SDK.
#
# A missing probe tool is a failure, not a skip.
GST_SDK_PREFIX="${LIGHTNING_MACOS_GSTREAMER_PREFIX:-$HOME/opt/gstreamer/GStreamer.framework/Versions/1.0}"
GST_INSPECT="$GST_SDK_PREFIX/bin/gst-inspect-1.0"
if [[ ! -x "$GST_INSPECT" ]]; then
    printf '  FAIL: gst-inspect-1.0 not found at %s — cannot prove the plugins load\n' \
        "$GST_INSPECT" >&2
    failures=$((failures + 1))
    gst_elements_state=unproven
else
    probe_dir="$(mktemp -d)"
    probe="$probe_dir/gst-inspect-probe"
    lipo -thin arm64 "$GST_INSPECT" -output "$probe" 2>/dev/null || cp "$GST_INSPECT" "$probe"
    chmod 0755 "$probe"
    rpaths_of "$probe" >"$probe_dir/rpaths"
    while IFS= read -r rp; do
        [[ -n "$rp" ]] || continue
        install_name_tool -delete_rpath "$rp" "$probe" 2>/dev/null || true
    done <"$probe_dir/rpaths"
    install_name_tool -add_rpath "$(cd "$GST_LIB_DIR" && pwd -P)" "$probe"
    # install_name_tool invalidates the signature, and Apple Silicon SIGKILLs
    # such a binary, which would read as "every element is missing".
    codesign --force --sign - --timestamp=none "$probe" >/dev/null 2>&1 || true

    gst_registry="$probe_dir/registry.bin"
    gst_probe_env=(
        env
        GST_PLUGIN_SYSTEM_PATH= GST_PLUGIN_SYSTEM_PATH_1_0=
        GST_PLUGIN_PATH="$GST_PLUGIN_LINK" GST_PLUGIN_PATH_1_0="$GST_PLUGIN_LINK"
        GST_REGISTRY="$gst_registry" GST_REGISTRY_1_0="$gst_registry"
    )
    if ! "${gst_probe_env[@]}" "$probe" --version >/dev/null 2>&1; then
        printf '  FAIL: the element probe tool could not run against the bundle\n' >&2
        printf '        (the bundled GStreamer core, glib or gobject did not load)\n' >&2
        failures=$((failures + 1))
        gst_elements_state=unproven
    else
        # REQUIRED is SfuMediaEngine::runtimeAvailable()'s probe list plus the
        # macOS capture sources and the receive-path elements its pipelines
        # name, plus sctpenc/sctpdec: webrtcbin loads those itself for the data
        # channel that LiveKit's subscriber offer bundles every media section
        # onto, so without them nothing is received.
        #
        # ADVISORY is what the engine tolerates missing (compositor, and
        # webrtcdsp at the cost of the microphone AGC).
        gst_missing=0
        for element in \
            webrtcbin nicesrc nicesink dtlssrtpenc dtlssrtpdec opusenc opusdec \
            rtpopuspay rtpopusdepay audioconvert audioresample audiotestsrc \
            fakesink autoaudiosrc autoaudiosink queue valve volume capsfilter \
            vp8enc vp8dec rtpvp8pay rtpvp8depay videoconvert videoscale \
            videotestsrc videorate identity tee funnel level appsink appsrc \
            autovideosrc avfvideosrc osxaudiosrc osxaudiosink \
            sctpenc sctpdec
        do
            if ! "${gst_probe_env[@]}" "$probe" "$element" >/dev/null 2>&1; then
                printf '  missing element in the bundled plugins: %s\n' "$element" >&2
                gst_missing=$((gst_missing + 1))
            fi
        done
        gst_degraded=0
        for element in webrtcdsp webrtcechoprobe compositor; do
            if ! "${gst_probe_env[@]}" "$probe" "$element" >/dev/null 2>&1; then
                printf '  note: optional element absent (%s) — the engine tolerates it\n' "$element"
                gst_degraded=$((gst_degraded + 1))
            fi
        done
        if (( gst_missing > 0 )); then
            printf '  FAIL: %d required call elements do not resolve from the bundled plugins\n' \
                "$gst_missing" >&2
            failures=$((failures + 1))
            gst_elements_state=unresolved
        elif (( gst_degraded > 0 )); then
            printf '  ok: every required call element resolves; %d optional element(s) absent\n' \
                "$gst_degraded"
            gst_elements_state=degraded
        else
            printf '  ok: every call media element resolves from the bundled plugins alone\n'
            gst_elements_state=resolved
        fi

        # The registry must be built by the bundled helper, not by the
        # in-process fallback, which resolves the same elements. A fresh
        # registry is required: with the cached one nothing is scanned and no
        # helper is exec'd.
        scanner_registry="$probe_dir/registry-scanner.bin"
        scanner_log="$probe_dir/scanner.log"
        env \
            GST_PLUGIN_SYSTEM_PATH= GST_PLUGIN_SYSTEM_PATH_1_0= \
            GST_PLUGIN_PATH="$GST_PLUGIN_LINK" GST_PLUGIN_PATH_1_0="$GST_PLUGIN_LINK" \
            GST_REGISTRY="$scanner_registry" GST_REGISTRY_1_0="$scanner_registry" \
            GST_PLUGIN_SCANNER="$GST_SCANNER" GST_PLUGIN_SCANNER_1_0="$GST_SCANNER" \
            "$probe" webrtcbin >/dev/null 2>"$scanner_log" || true
        if [[ ! -s "$scanner_registry" ]]; then
            printf '  FAIL: no registry was built through the bundled gst-plugin-scanner\n' >&2
            failures=$((failures + 1))
            gst_scanner_state=unproven
        elif grep -q 'External plugin loader failed' "$scanner_log"; then
            printf '  FAIL: the bundled gst-plugin-scanner could not be run\n' >&2
            printf '        GStreamer fell back to scanning plugins in-process and\n' >&2
            printf '        printed "External plugin loader failed" — the warning every\n' >&2
            printf '        macOS launch carried before the helper was staged. Usually a\n' >&2
            printf '        missing, unsigned or non-executable %s\n' \
                "${GST_SCANNER#"$APP_DIR"/}" >&2
            failures=$((failures + 1))
            gst_scanner_state=failed
        else
            printf '  ok: the bundled gst-plugin-scanner builds the registry\n'
            gst_scanner_state=ok
        fi
    fi
    rm -rf -- "$probe_dir"
fi

gst_plugin_count="$(find "$GST_PLUGIN_DIR" -maxdepth 1 -type f 2>/dev/null | wc -l | tr -d ' ')"
gst_lib_count="$(find "$GST_LIB_DIR" -maxdepth 1 -type f 2>/dev/null | wc -l | tr -d ' ')"
printf '  GStreamer: %s plugins, %s support libraries\n' "$gst_plugin_count" "$gst_lib_count"

# --- licences ----------------------------------------------------------------
# Asserted on the bundle, not on the staging script. Lightning is
# GPL-3.0-or-later and must ship its own licence text.
check "Lightning's own GPL-3 text is in the bundle" \
    test -s "$APP_DIR/Contents/Resources/licenses/Lightning-GPL-3.0.txt"
check "the staged gst-plugins-good licence is present" \
    test -s "$APP_DIR/Contents/Resources/licenses/lightning-gstreamer/gst-plugins-good-1.0/COPYING"

# --- signature ---------------------------------------------------------------
# An invalid ad-hoc signature will not run on Apple Silicon at all.
check "code signature verifies" codesign --verify --deep --strict "$APP_DIR"
codesign -dv --verbose=4 "$APP_DIR" >"$REPORT_DIR/codesign-info.txt" 2>&1 || true

# Gatekeeper is expected to reject an ad-hoc, un-notarized bundle; recorded as
# evidence, not asserted.
if spctl --assess --type execute --verbose=4 "$APP_DIR" >"$REPORT_DIR/spctl.txt" 2>&1; then
    printf '  note: Gatekeeper ACCEPTED the bundle (unexpected for an ad-hoc signature)\n'
    GATEKEEPER=accepted
else
    printf '  expected: Gatekeeper rejects the bundle (unsigned/un-notarized test build)\n'
    GATEKEEPER=rejected
fi

# --- launch smoke ------------------------------------------------------------
# Headless CLI entry points only: this proves the binary and its frameworks
# load, not that a window works.
if "$CONTENTS/MacOS/$APP_NAME" --version >"$REPORT_DIR/version.txt" 2>&1; then
    printf '  ok: bundled binary runs (--version): %s\n' "$(tr -d '\n' <"$REPORT_DIR/version.txt")"
else
    printf '  FAIL: bundled binary could not execute --version\n' >&2
    failures=$((failures + 1))
fi
# The checks above prove the bundle's shape, not that the application finds
# its plugins. --call-media-status probes through the same functions
# AppController uses.
if "$CONTENTS/MacOS/$APP_NAME" --call-media-status         >"$REPORT_DIR/call-media-status.txt" 2>&1; then
    check "the bundled app can place calls"         grep -qF 'RESULT: calls can be placed and answered'             "$REPORT_DIR/call-media-status.txt"
    # Must be the bundle's own plugins: this runner has a system GStreamer that
    # users do not.
    check "the bundled app used its own plugin directory"         grep -Eq '^bundled plugin directory: .*gstreamer-1\.0'             "$REPORT_DIR/call-media-status.txt"
    # Asked of the running app, which ties together GstBootstrap's path, the
    # staging script's layout and the file actually being there.
    check "the bundled app found its registry helper" \
        grep -Eq '^plugin scanner: .*/gst-plugin-scanner$' \
            "$REPORT_DIR/call-media-status.txt"
else
    printf '  FAIL: the bundled app cannot place calls
' >&2
    sed 's/^/        /' "$REPORT_DIR/call-media-status.txt" >&2 || true
    failures=$((failures + 1))
fi

# Voice-delay self-test; see assert_queue_selftest in lib.sh. A verdict is
# required, a failing verdict only warns for now. Bounded so a hung probe
# cannot hold the Mac until the job timeout.
queue_selftest_status=0
run_bounded 300 "$CONTENTS/MacOS/$APP_NAME" --call-queue-selftest \
    >"$REPORT_DIR/queue-selftest.txt" 2>&1 || queue_selftest_status=$?
# Soft mode: this validator accumulates failures instead of dying on the first.
if ! assert_queue_selftest macOS "$REPORT_DIR/queue-selftest.txt" \
        "$queue_selftest_status" soft; then
    failures=$((failures + 1))
fi
# Call sounds, warn-only like every format (see assert_call_sounds_status in
# lib.sh). CoreAudio normally presents an output device even with nothing
# plugged in, so this lane can usually measure. run_bounded, because macOS has
# no GNU timeout.
call_sounds_status=0
run_bounded 60 "$CONTENTS/MacOS/$APP_NAME" --call-sounds-status \
    >"$REPORT_DIR/call-sounds-status.txt" 2>&1 || call_sounds_status=$?
assert_call_sounds_status macOS "$REPORT_DIR/call-sounds-status.txt" \
    "$call_sounds_status"
# Image decoders. macdeployqt ships Homebrew qtimageformats plus qmacheif. JPEG
# XL is not required: no Qt JXL plugin exists for macOS, so it is reported as a
# platform limit.
if "$CONTENTS/MacOS/$APP_NAME" --image-format-status \
        >"$REPORT_DIR/image-format-status.txt" 2>&1; then
    check "the bundled app can decode every image format Lightning accepts" \
        grep -qx 'required image/webp: decodable' \
            "$REPORT_DIR/image-format-status.txt"
else
    printf '  FAIL: the bundled app accepts image formats it cannot decode\n' >&2
    sed 's/^/        /' "$REPORT_DIR/image-format-status.txt" >&2 || true
    failures=$((failures + 1))
fi
if "$CONTENTS/MacOS/$APP_NAME" --build-info >"$REPORT_DIR/bundle-build-info.txt" 2>&1; then
    check "bundle reports the Rust backend" \
        grep -qx 'matrix_backend: rust' "$REPORT_DIR/bundle-build-info.txt"
    check "bundle excludes the HTTP backend" \
        grep -qx 'http_backend_compiled: false' "$REPORT_DIR/bundle-build-info.txt"
    check "bundle excludes the mock backend" \
        grep -qx 'mock_backend_compiled: false' "$REPORT_DIR/bundle-build-info.txt"
else
    printf '  FAIL: bundled binary could not execute --build-info\n' >&2
    failures=$((failures + 1))
fi

# --- leak scan ---------------------------------------------------------------
# An artifact must not carry runner tokens, provider key values or builder
# paths. Values are scanned, not names: Qt's TLS code contains PEM header
# literals, and a variable name is not a secret.
leak_hits=0
scan_for() {
    local label="$1" pattern="$2"
    if grep -rqa -- "$pattern" "$APP_DIR" 2>/dev/null; then
        printf '  FAIL: bundle contains %s\n' "$label" >&2
        leak_hits=$((leak_hits + 1))
    fi
}
scan_for "a GitLab runner token" 'glrt-'
scan_for "an SSH private key" '-----BEGIN OPENSSH PRIVATE KEY-----'
scan_for "a builder SSH path" "$HOME/.ssh"
# The GStreamer SDK lives in the runner's home; this catches an embedded string
# anywhere, beyond the rpath sweep above.
scan_for "the builder's GStreamer SDK path" "$GST_SDK_PREFIX"

# A build may deliberately embed the GIF keys (build-info.json says so). What
# must hold is that a build reporting itself keyless contains no key.
#
# Full `if` blocks: under `set -e` a false trailing `[[ ]] && cmd` aborts.
GIF_EMBEDDED="$(jq -r '.gif_keys_embedded // false' "$CONTENTS/Resources/build-info.json" 2>/dev/null || echo unknown)"
if [[ "$GIF_EMBEDDED" == "true" ]]; then
    printf '  note: GIF provider keys are intentionally embedded in this build\n'
    printf '        (developer-scoped, expiring artifact — an embedded key is extractable)\n'
else
    if [[ -n "${GIPHY_API_KEY:-}" ]]; then
        scan_for "the Giphy provider key value in a keyless build" "$GIPHY_API_KEY"
    fi
    if [[ -n "${KLIPY_API_KEY:-}" ]]; then
        scan_for "the Klipy provider key value in a keyless build" "$KLIPY_API_KEY"
    fi
fi
if (( leak_hits == 0 )); then
    printf '  ok: no runner tokens, private keys, or builder paths in the bundle\n'
else
    failures=$((failures + leak_hits))
fi

# --- report ------------------------------------------------------------------
BUNDLE_BYTES="$(du -sk "$APP_DIR" | awk '{print $1 * 1024}')"
jq -n \
    --arg bundle_id "$BUNDLE_ID" \
    --arg version "$BUNDLE_VERSION" \
    --arg archs "$ARCHS" \
    --arg gatekeeper "$GATEKEEPER" \
    --argjson frameworks "${qt_fw_count:-0}" \
    --argjson bundle_bytes "$BUNDLE_BYTES" \
    --argjson failures "$failures" \
    --argjson gst_plugins "${gst_plugin_count:-0}" \
    --argjson gst_libs "${gst_lib_count:-0}" \
    --arg gst_elements "${gst_elements_state:-unproven}" \
    --arg gst_scanner "${gst_scanner_state:-unproven}" \
    --argjson gst_optional_absent "${gst_degraded:-0}" \
    '{bundle_identifier:$bundle_id, version:$version, architectures:$archs,
      bundled_frameworks:$frameworks, bundle_bytes:$bundle_bytes,
      signature:"ad-hoc", notarized:false, gatekeeper:$gatekeeper,
      structural_failures:$failures,
      gstreamer:{plugins:$gst_plugins, support_libraries:$gst_libs,
                 call_elements:$gst_elements,
                 registry_scanner:$gst_scanner,
                 optional_elements_absent:$gst_optional_absent},
      call_media_live_tested:false,
      native_macos_acceptance_tested:false}' \
    >"$REPORT_DIR/macos-validation.json"

if (( failures > 0 )); then
    die "macOS bundle validation failed ($failures checks)"
fi
printf 'macOS bundle validation passed (%s, %s bytes)\n' "$ARCHS" "$BUNDLE_BYTES"
