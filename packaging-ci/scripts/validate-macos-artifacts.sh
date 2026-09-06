#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"

# Structural validation of the assembled Lightning.app. This proves the bundle
# is well-formed, self-contained, arm64, and runnable on this machine — it does
# NOT prove it is distributable. Gatekeeper acceptance is checked and reported
# honestly as a known failure; see docs/macos-packaging.md.

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
# Not cosmetic: the client records voice messages, and macOS kills any process
# that opens the microphone without a usage string. A bundle missing this is
# broken at runtime, so it is a hard failure here.
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
# Every dependency must resolve inside the bundle or in /System|/usr/lib. A
# leftover /opt/homebrew dependency means macdeployqt missed a library and the
# app would only launch on this build machine.
#
# The install NAME (LC_ID_DYLIB) is deliberately excluded. macdeployqt copies
# Homebrew's frameworks without rewriting their own IDs, so every bundled
# framework still calls itself /opt/homebrew/opt/qt*/lib/... That string is what
# a future linker would record, not something dyld resolves at run time — the
# loader follows the *dependent's* load command, which is @rpath or
# @executable_path. Treating the ID as a leak flagged all 46 frameworks on a
# bundle that runs correctly.
#
# `otool -L` prints: line 1 the file path, line 2 the install ID (for
# libraries/frameworks only), then the dependencies. `otool -D` yields the ID on
# its own, so it can be subtracted precisely instead of guessing a line offset.
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

# Same check across the whole bundle. Every Mach-O is identified by content
# rather than by permission bits or extension, because framework payloads carry
# neither a .dylib suffix nor a consistent mode.
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
# The modules Lightning links; if macdeployqt missed one the app dies at launch.
for fw in QtCore QtGui QtQml QtQuick QtQuickControls2 QtNetwork QtSql QtWidgets QtMultimedia; do
    check "framework bundled: $fw" test -d "$CONTENTS/Frameworks/${fw}.framework"
done
# QML plugins live under Resources/qml; without them the UI fails to instantiate
# even though the app links fine.
check "QML modules bundled" test -d "$CONTENTS/Resources/qml"
for qml_mod in QtQuick QtQml; do
    check "QML module present: $qml_mod" test -d "$CONTENTS/Resources/qml/$qml_mod"
done
# Cocoa platform plugin — without it Qt cannot create a window at all.
check "cocoa platform plugin bundled" \
    test -f "$CONTENTS/PlugIns/platforms/libqcocoa.dylib"

qt_fw_count="$(find "$CONTENTS/Frameworks" -maxdepth 1 -name '*.framework' 2>/dev/null | wc -l | tr -d ' ')"
printf '  bundled frameworks: %s\n' "$qt_fw_count"

# --- call media engine (GStreamer) -------------------------------------------
# Everything here exists because the failure mode is INVISIBLE from outside: a
# bundle with no plugins installs, launches, renders, syncs, and then refuses
# every call with "Joining isn't available". Checking that some files were
# copied is not enough either, so this section ends by building a real GStreamer
# registry out of the bundled plugins and asking for every element by name.
GST_PLUGIN_LINK="$CONTENTS/MacOS/gstreamer-1.0"
GST_PLUGIN_DIR="$CONTENTS/PlugIns/gstreamer-plugins"
GST_LIB_DIR="$CONTENTS/PlugIns/gstreamer-libs"

# The app looks for applicationDirPath()/gstreamer-1.0 and nothing else
# (src/calls/SfuMediaEngine.cpp). It has to be a SYMLINK: codesign refuses to
# seal a directory whose name carries an extension, and "1.0" is one — see
# scripts/stage-macos-gstreamer.sh for the three experiments that established
# it. Assert the symlink, that it resolves, AND that it resolves to the staged
# payload: each check alone passes on a bundle that cannot place a call.
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

# The executable must link GStreamer — the media engine is the only thing in
# Lightning that does, and its CMake probe fails silently — and it must link the
# BUNDLED one, EXPLICITLY.
#
# `grep libgstreamer` alone would not prove the second half. macdeployqt runs
# before the staging step and resolves the executable's @rpath GStreamer-stack
# dependencies out of Homebrew, rewriting them into Contents/Frameworks:
# measured on the runner, libglib/libgobject/libintl all landed there, pointing
# the app's own g_* calls at Qt's GLib while the plugins use GStreamer's. The
# staging script rebinds them; this is the check that proves it happened.
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

# The builder's SDK path arrives here by default: gstreamer-1.0.pc's Libs line
# ends in -Wl,-rpath,${libdir}, so the link records an absolute rpath into the
# runner's home. Scoped to the executable and the staged payload ON PURPOSE —
# Homebrew's libjpeg/libdbus/libjasper carry /opt/homebrew/Cellar rpaths of
# their own, and this check is about what this packaging step added.
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
done < <(find "$CONTENTS/MacOS/$APP_NAME" "$GST_PLUGIN_DIR" "$GST_LIB_DIR" -type f 2>/dev/null)
if (( host_rpaths > 0 )); then
    printf '  FAIL: %d builder rpaths survive in the executable or the staged runtime\n' "$host_rpaths" >&2
    failures=$((failures + 1))
else
    printf '  ok: no builder rpaths in the executable or the staged GStreamer runtime\n'
fi

# Closure. Every @rpath dependency of every staged binary has to be present in
# gstreamer-libs, and every staged binary has to be arm64. Neither is implied by
# the element probe below, and a support library that is merely absent produces
# a bundle that works perfectly on the build machine and nowhere else.
#
# Mach-O files are identified by CONTENT, not by a '*.dylib' name filter: the
# staged basenames come from dependency records, and anything that did not end
# in .dylib would be skipped by exactly the check meant to catch it.
gst_unresolved=0
gst_wrong_arch=0
while IFS= read -r macho; do
    file -b "$macho" 2>/dev/null | grep -q 'Mach-O' || continue
    # `|| true`: a file with no LC_ID_DYLIB makes `grep -v` return 1, which
    # under `set -o pipefail` fails the assignment and `set -e` aborts.
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
done < <(find "$GST_PLUGIN_DIR" "$GST_LIB_DIR" -type f 2>/dev/null)
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

# THE check. Everything above is still file inspection: it cannot tell you
# whether GStreamer can build a registry out of these plugins and hand back the
# elements. So build one — through the SYMLINK the application itself uses, not
# the real directory, because that string is what SfuMediaEngine constructs and
# a symlink pointing somewhere else would otherwise pass every check here.
#
# THE PROBE TOOL IS REBUILT, NOT BORROWED, and that detail is the difference
# between a real check and a decorative one. The bundle ships no gst-inspect of
# its own, so it comes from the SDK — but the SDK's copy carries
# @executable_path/../lib in its own LC_RPATH, and dyld resolves @rpath against
# the MAIN EXECUTABLE's rpaths as well as the loading library's. Probing with it
# as-is lets the SDK's lib/ quietly satisfy anything the bundle is missing:
# measured, by deleting libvpx.9.dylib from a staged bundle and watching vp8enc
# resolve anyway. A copy with every rpath stripped and one absolute rpath into
# the bundle's own gstreamer-libs can see nothing else.
#
# The GST_*_1_0 variables are cleared alongside the unversioned ones because
# GStreamer reads the VERSIONED name first: a leftover GST_PLUGIN_PATH_1_0 in
# the runner's environment would point the scan at the SDK's ~250 plugins and
# report every element resolving from a bundle containing none.
#
# Its absence is a hard failure, not a skip: a validation that cannot run is not
# a validation that passed.
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
    # install_name_tool invalidates the signature, and Apple Silicon kills an
    # unsigned-but-signature-bearing binary outright (SIGKILL, no message),
    # which would read here as "every element is missing".
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
        # REQUIRED is SfuMediaEngine::runtimeAvailable()'s own probe list plus
        # the macOS capture sources and the receive-path elements its pipelines
        # name — AND sctpenc/sctpdec, which that rule cannot reach.
        #
        # Nothing in Lightning names them: webrtcbin loads them itself for the
        # DATA CHANNEL, and LiveKit's subscriber offer puts a data channel in
        # media section 0, which under bundle-policy=max-bundle owns the
        # transport every audio and video section rides on. Windows shipped
        # without the plugin and could SEND while receiving nothing at all, and
        # a required-element list derived from what the application spells out
        # is precisely what failed to notice. This bundle stages sctp today;
        # without this line, trimming the PLUGINS list would break macOS the
        # same way with every check still green.
        #
        # ADVISORY is the set the engine explicitly tolerates the absence
        # of — SfuMediaEngine.cpp says so of `compositor` in as many words, and
        # registers without webrtcdsp (losing only the microphone AGC). Failing
        # the whole packaging job for an element the engine never requires would
        # be a spurious red on the next GStreamer release that splits a plugin.
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
    fi
    rm -rf -- "$probe_dir"
fi

gst_plugin_count="$(find "$GST_PLUGIN_DIR" -maxdepth 1 -type f 2>/dev/null | wc -l | tr -d ' ')"
gst_lib_count="$(find "$GST_LIB_DIR" -maxdepth 1 -type f 2>/dev/null | wc -l | tr -d ' ')"
printf '  GStreamer: %s plugins, %s support libraries\n' "$gst_plugin_count" "$gst_lib_count"

# --- signature ---------------------------------------------------------------
# Ad-hoc signature must be structurally valid or the bundle will not run on
# Apple Silicon at all.
check "code signature verifies" codesign --verify --deep --strict "$APP_DIR"
codesign -dv --verbose=4 "$APP_DIR" >"$REPORT_DIR/codesign-info.txt" 2>&1 || true

# Gatekeeper is EXPECTED to reject this bundle: it is ad-hoc signed, not
# Developer ID signed, and not notarized. Recorded as evidence rather than
# asserted as a pass, so nobody mistakes a green pipeline for a distributable
# artifact.
if spctl --assess --type execute --verbose=4 "$APP_DIR" >"$REPORT_DIR/spctl.txt" 2>&1; then
    printf '  note: Gatekeeper ACCEPTED the bundle (unexpected for an ad-hoc signature)\n'
    GATEKEEPER=accepted
else
    printf '  expected: Gatekeeper rejects the bundle (unsigned/un-notarized test build)\n'
    GATEKEEPER=rejected
fi

# --- launch smoke ------------------------------------------------------------
# Headless CLI entry points only. The runner has a GUI session, but asserting on
# a real window from CI would be flaky and is not what this proves; this
# confirms the bundled binary and its frameworks actually load and execute.
if "$CONTENTS/MacOS/$APP_NAME" --version >"$REPORT_DIR/version.txt" 2>&1; then
    printf '  ok: bundled binary runs (--version): %s\n' "$(tr -d '\n' <"$REPORT_DIR/version.txt")"
else
    printf '  FAIL: bundled binary could not execute --version\n' >&2
    failures=$((failures + 1))
fi
# THE CHECK THAT WOULD HAVE CAUGHT THE LAST THREE ROUNDS.
#
# Everything above proves the bundle's SHAPE — the plugins are there, every
# dependency resolves inside the bundle, every element resolves when the probe
# is handed the plugin path explicitly. None of it proves the APPLICATION finds
# them, and that is exactly what was broken on Windows: the plugin path was
# applied after gst_init had already run, so a bundle with correct plugins
# still refused every call.
#
# --call-media-status probes through the same functions AppController calls, so
# a pass here means the shipped bundle would offer a call button.
if "$CONTENTS/MacOS/$APP_NAME" --call-media-status         >"$REPORT_DIR/call-media-status.txt" 2>&1; then
    check "the bundled app can place calls"         grep -qF 'RESULT: calls can be placed and answered'             "$REPORT_DIR/call-media-status.txt"
    # It must be the bundle's OWN plugins. Falling back to a system GStreamer
    # would pass on this runner (which has one installed) and fail on every
    # user's Mac, which is the worst possible shape for a check.
    check "the bundled app used its own plugin directory"         grep -Eq '^bundled plugin directory: .*gstreamer-1\.0'             "$REPORT_DIR/call-media-status.txt"
else
    printf '  FAIL: the bundled app cannot place calls
' >&2
    sed 's/^/        /' "$REPORT_DIR/call-media-status.txt" >&2 || true
    failures=$((failures + 1))
fi
# THE IMAGE DECODERS, asked of the bundle the same way. macdeployqt copies
# every plugin in its default categories, so this bundle gets Homebrew
# qtimageformats' set (webp, tiff, icns, jp2, mng, tga, wbmp) plus qmacheif,
# which is Qt's ImageIO-backed HEIF plugin and exists on macOS alone. It does
# NOT get JPEG XL: Qt has never shipped a JXL plugin and Homebrew packages no
# build of KDE's kimageformats, which is the only implementation. So `jxl` is
# deliberately NOT required here — the check asserts the required set and
# leaves JPEG XL reported as a platform limit, which is the honest answer.
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
# Same intent as the Windows validator — an artifact must not carry runner
# tokens, provider key VALUES, or the builder's private paths.
#
# Scanning for generic strings like "PRIVATE KEY" does not work here: Qt's TLS
# code contains PEM header literals ("-----BEGIN PRIVATE KEY-----") as ordinary
# format strings, so that pattern matches a perfectly clean bundle. Scanning for
# the variable NAMES is equally meaningless — a name is not a secret. What
# matters is whether a real secret value ended up in the payload, so the actual
# values are scanned when they were supplied.
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
# The GStreamer SDK lives in the runner's home, so it is a class of builder path
# the /opt/homebrew checks above never covered. The rpath sweep in the GStreamer
# section catches load commands; this catches an embedded string anywhere.
scan_for "the builder's GStreamer SDK path" "$GST_SDK_PREFIX"

# GIF provider keys are a conditional check, not an unconditional one. When the
# project's GIPHY_API_KEY/KLIPY_API_KEY variables are available the build
# *deliberately* compiles them in, exactly like the Windows test path, so the
# picker works without local configuration — finding them is then correct, not a
# leak. The property worth enforcing is the opposite one: a build that reported
# itself keyless must not contain a key. build-info.json records which happened.
#
# Written as full `if` blocks: under `set -e` a trailing `[[ ... ]] && cmd` whose
# test is false makes the whole list return 1 and aborts the script.
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
    --argjson gst_optional_absent "${gst_degraded:-0}" \
    '{bundle_identifier:$bundle_id, version:$version, architectures:$archs,
      bundled_frameworks:$frameworks, bundle_bytes:$bundle_bytes,
      signature:"ad-hoc", notarized:false, gatekeeper:$gatekeeper,
      structural_failures:$failures,
      gstreamer:{plugins:$gst_plugins, support_libraries:$gst_libs,
                 call_elements:$gst_elements,
                 optional_elements_absent:$gst_optional_absent},
      call_media_live_tested:false,
      native_macos_acceptance_tested:false}' \
    >"$REPORT_DIR/macos-validation.json"

if (( failures > 0 )); then
    die "macOS bundle validation failed ($failures checks)"
fi
printf 'macOS bundle validation passed (%s, %s bytes)\n' "$ARCHS" "$BUNDLE_BYTES"
