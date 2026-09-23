#!/usr/bin/env bash
set -Eeuo pipefail

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

require_var() {
    local name="$1"
    [[ -n "${!name:-}" ]] || die "required environment variable ${name} is not set"
}

project_dir() {
    if [[ -n "${CI_PROJECT_DIR:-}" ]]; then
        printf '%s\n' "$CI_PROJECT_DIR"
    else
        git rev-parse --show-toplevel
    fi
}

load_versions() {
    local root
    root="$(project_dir)"
    [[ -f "$root/dist/version.env" ]] || die "dist/version.env is missing"
    # Generated internally by resolve-version.sh and restricted to simple values.
    # shellcheck disable=SC1091
    source "$root/dist/version.env"
}

json_value() {
    local file="$1" key="$2"
    jq -er --arg key "$key" '.[$key]' "$file"
}

# --- call media engine ------------------------------------------------------
#
# ONE judgement of a `--call-media-status` transcript, shared by every Linux
# format, so the bar cannot drift between them.
#
# It exists because 0.8.0 shipped every Linux package with the engine compiled
# OUT and every check passed: the packages installed, launched, synced and
# refused every call. Nothing that reads a file listing or an ELF header can
# see this -- the plugins are dlopen'd and the engine is a compile-time
# #ifdef -- so the only check that answers the question is running the SHIPPED
# artifact and asking it.
#
# Callers produce the transcript however their format is run (an installed
# binary, an AppImage, the snap launcher, `flatpak run`) and hand the log and
# the exit status here.
#
#   $1  format label for the error message
#   $2  path to the captured combined output
#   $3  the command's exit status
assert_call_media_engine() {
    local label="$1" log="$2" status="$3"
    [[ -s "$log" ]] || die "$label: --call-media-status produced no output at all"
    cat "$log"
    grep -qx 'call media engine built in: yes' "$log" || die \
        "$label: the packaged binary has NO call media engine compiled in. CMake's GStreamer probe found no development files at build time, so calling, screen sharing and the camera are absent and the app refuses every call."
    # The RESULT line is decided by the SFU engine's own element probe -- the
    # same function AppController calls -- so this is the packaged plugin set
    # answering, not a file listing. A deb/rpm proves its Depends; an
    # AppImage/snap proves its bundle; a Flatpak proves its runtime.
    grep -qx 'RESULT: calls can be placed and answered.' "$log" || die \
        "$label: the engine is compiled in but cannot run -- see the missing_element or gstreamer line above. The package does not carry (or cannot find) the GStreamer plugins the call engine needs."
    # Belt and braces: the exit status is the engine's own verdict, and a
    # status that disagrees with the text would mean one of them is lying.
    [[ "$status" == 0 ]] || die \
        "$label: --call-media-status reported success but exited $status"
}

# --- a time bound that exists on macOS too -----------------------------------
#
# `timeout` is GNU coreutils and macOS does not ship it. Adding one to the
# macOS validator failed that job with `timeout: command not found`, which the
# self-test gate then reported correctly as "printed no VERDICT line at all" —
# the gate behaved exactly as designed over a command that never ran, which is
# the one outcome it was written to catch. The bound is still worth having: an
# unbounded hung probe burns the Mac mini to the three-hour job ceiling.
#
# Homebrew's coreutils installs it as `gtimeout`. When neither exists, a plain
# background-and-poll watchdog does the same job with nothing but the shell.
#
#   $1  seconds
#   $@  the command
run_bounded() {
    local secs="$1"; shift
    if command -v timeout >/dev/null 2>&1; then
        timeout "$secs" "$@"
        return $?
    fi
    if command -v gtimeout >/dev/null 2>&1; then
        gtimeout "$secs" "$@"
        return $?
    fi
    "$@" &
    local pid=$! waited=0
    while kill -0 "$pid" 2>/dev/null; do
        if [[ "$waited" -ge "$secs" ]]; then
            kill -TERM "$pid" 2>/dev/null
            sleep 2
            kill -KILL "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
            return 124
        fi
        sleep 1
        waited=$((waited + 1))
    done
    wait "$pid"
    return $?
}

# --- the voice-delay property -----------------------------------------------
#
# THE ONE judgement of a `--call-queue-selftest` transcript, and it really is
# one now: deb, rpm, AppImage, snap, Flatpak, the Windows portable under Wine
# and the macOS bundle all call THIS function. It had three copies for a few
# hours and the promotion note below named one place to change.
#
# What it measures and why it is here: a GStreamer `queue` defaults to holding
# one second and never dropping any of it, so one moment of a consumer falling
# behind is permanent added delay for the rest of a call. That was reported
# live on 2026-09-16 as roughly a second of extra delay in one direction.
# Every check of it before this one was ACOUSTIC -- two machines, a sound card
# and a rig -- which is why it existed on Linux alone and why the Windows guest
# (no sound card at all) could not be measured.
#
# THREE OUTCOMES, NOT TWO, and keeping them apart is the whole point:
#
#   VERDICT: pass          measured, and every shipped queue behaved
#   VERDICT: fail          measured, and one did not
#   VERDICT: unmeasurable  NOTHING was measured -- no media engine, GStreamer
#                          did not initialise, an element could not be made,
#                          or the CONTROL queue did not hold its second, which
#                          means the starvation itself did not happen
#
# `unmeasurable` DIES. "Measured and failed" and "could not measure" must not
# share an exit path: an earlier version matched `^RESULT: ` with a prefix
# grep, so a transcript reading "no element audiotestsrc" three times passed
# the gate as a warning. The sibling helper above has always used `grep -qx`
# on an exact sentence for exactly this reason.
#
# `fail` only WARNS, deliberately. Pipeline 224 died because a plugin was added
# to a REQUIRED list before the image that had to carry it was rebuilt, and the
# lesson recorded from it is that the entry is the half that must come second.
#
# TO PROMOTE IT: once it reports `VERDICT: pass` on deb, rpm, AppImage, snap,
# Flatpak, Windows-under-Wine and macOS in one pipeline, AND a repeat-run
# measurement on the real CI runners shows the wall-clock thresholds inside
# `runQueueSelfTest` are stable there (§16 records four CTest suites that flake
# on timing at -j2), change the `fail` branch below from a warning to `die`.
# Change it HERE; there is nowhere else. Note macOS is `allow_failure` per §14,
# so promoting it cannot make the Mac block a release either way.
#
#   $1  format label for the message
#   $2  path to the captured combined output
#   $3  the command's exit status
#   $4  optional: "soft" -- report and return 1 instead of dying, for a
#       validator that accumulates failures and reports at the end
assert_queue_selftest() {
    local label="$1" log="$2" status="$3" mode="${4:-hard}"
    local complain=die
    [[ "$mode" == "soft" ]] && complain=_queue_selftest_soft_complain
    [[ -s "$log" ]] || $complain "$label: --call-queue-selftest produced no output at all"
    cat "$log"
    # STRIP CR. The Windows portable runs under Wine and its output is CRLF,
    # so the verdict read as `pass\r`, matched no case, and fell through to
    # the catch-all — failing a job whose measurement had PASSED, with a
    # message saying the line was absent when it was right there. Every other
    # transcript this helper reads is LF; this one is not, and nothing else in
    # the file had needed to care.
    local verdict
    verdict="$(grep -m1 '^VERDICT: ' "$log" | tr -d '\r' | awk '{print $2}')"
    case "$verdict" in
    pass)
        echo "$label: voice-delay queue self-test VERDICT: pass (exit $status)"
        return 0
        ;;
    fail)
        echo "WARNING: $label: voice-delay queue self-test VERDICT: fail (exit $status)." >&2
        echo "WARNING: a live queue kept a backlog its consumer had caught up from." >&2
        echo "WARNING: this is NOT yet a hard gate -- see assert_queue_selftest in lib.sh." >&2
        return 0
        ;;
    unmeasurable)
        $complain "$label: --call-queue-selftest measured NOTHING. The transcript above says why. A run that could not measure is not a run that passed."
        return 1
        ;;
    "")
        # NO LINE AT ALL: the command never reached its own report.
        $complain "$label: --call-queue-selftest printed no VERDICT line at all -- it crashed, hung, was not found, or is an older build. The transcript is above."
        return 1
        ;;
    *)
        # A LINE THIS HELPER CANNOT READ is a different fault from no line,
        # and saying "no VERDICT line at all" over a transcript that plainly
        # has one sends the next reader looking for a crash that did not
        # happen. It cost one Windows job exactly that.
        $complain "$label: --call-queue-selftest reported a VERDICT this check does not understand: '$verdict'. The transcript is above."
        return 1
        ;;
    esac
}

_queue_selftest_soft_complain() {
    printf '  FAIL: %s\n' "$1" >&2
}

# --- the call sounds ---------------------------------------------------------
#
# Judges a `--call-sounds-status` transcript for every format. Warn-only for
# now: QSoundEffect needs an output device to reach Ready, and CI containers
# have none (measured: no sound server -> 0 of 15; a PulseAudio null sink ->
# 15 of 15), so a transcript with output `none` is UNMEASURED, not a failure.
# To promote: give the validators a null sink, then make the MEASURED SHORT
# branch below call $complain once every format reports 15 of 15.
# The count is derived from data/sounds/*.wav by test-pipeline-config.py.
CALL_SOUNDS_EXPECTED=15

# $1 format label, $2 captured output, $3 exit status
assert_call_sounds_status() {
    local label="$1" log="$2" status="$3"
    if [[ ! -s "$log" ]]; then
        echo "WARNING: $label: --call-sounds-status produced no output at all (crashed, hung, or an older source without the flag). Not yet a hard gate -- see assert_call_sounds_status in lib.sh." >&2
        return 0
    fi
    cat "$log"
    # Wine writes CRLF.
    local clean result loaded total output
    clean="$(tr -d '\r' <"$log")"
    result="$(grep -m1 -E '^RESULT: [0-9]+ of [0-9]+ call sounds loaded$' <<<"$clean" || true)"
    output="$(grep -m1 '^default audio output: ' <<<"$clean" | sed 's/^default audio output: //' || true)"
    if [[ -z "$result" ]]; then
        echo "WARNING: $label: --call-sounds-status printed no RESULT line -- it crashed, hung, or is an older build without the flag. The transcript is above. Not yet a hard gate." >&2
        return 0
    fi
    loaded="$(awk '{print $2}' <<<"$result")"
    total="$(awk '{print $4}' <<<"$result")"
    if [[ "$total" != "$CALL_SOUNDS_EXPECTED" ]]; then
        echo "WARNING: $label: the build knows $total call sounds and this check expects $CALL_SOUNDS_EXPECTED. A list that changed size must move CALL_SOUNDS_EXPECTED with it." >&2
    fi
    if [[ "$loaded" == "$total" && "$total" == "$CALL_SOUNDS_EXPECTED" ]]; then
        echo "$label: call sounds: $loaded of $total loaded on output '$output' (exit $status)"
        [[ "$status" == 0 ]] || echo "WARNING: $label: every sound loaded but --call-sounds-status exited $status; one of the two is lying." >&2
        return 0
    fi
    if [[ -z "$output" || "$output" == "none" ]]; then
        echo "WARNING: $label: call sounds UNMEASURED: $loaded of $total loaded with NO audio output device in this environment (exit $status)." >&2
        echo "WARNING: QSoundEffect cannot reach Ready without an output device, so this says nothing about the package. See assert_call_sounds_status in lib.sh." >&2
        return 0
    fi
    echo "WARNING: $label: call sounds MEASURED SHORT: only $loaded of $total loaded on output '$output' (exit $status)." >&2
    echo "WARNING: a device was present, so the package's audio backend is the suspect. This is NOT yet a hard gate -- see assert_call_sounds_status in lib.sh." >&2
    return 0
}

# --- image format decoders ---------------------------------------------------
#
# ONE judgement of an `--image-format-status` transcript, shared by every
# format, for exactly the reason the call-engine helper above exists: a Qt
# image format is a dlopen'd plugin, so what a package can DECODE is decided by
# packaging and is invisible to every file listing and every ELF header.
#
# The defect it guards: every Linux package up to and including 0.8.0 shipped
# the three plugins qtbase itself carries -- libqgif, libqico, libqjpeg -- and
# nothing else, while the client's OWN byte sniffers accepted image/webp. It
# accepted, forwarded and re-uploaded a format it could not draw, for a year,
# and no check anywhere looked. Verified on the shipped artifact:
# usr/plugins/imageformats holds three files.
#
# WHY A RUNTIME PROBE AND NOT A FILE LIST. A plugin present is not a plugin
# that loads -- the same distinction that cost this project libgstsctp.dll on
# Windows and the PipeWire SPA modules in the AppImage. The build scripts DO
# assert the files (cheap, and it names the regression early); this asks the
# shipped artifact whether the decoders actually register.
#
#   $1  format label for the error message
#   $2  path to the captured combined output
#   $3  the command's exit status
#   $4  "jxl" when this platform is expected to decode JPEG XL as well.
#       Linux formats pass it; Windows and macOS deliberately do not, because
#       no Qt JPEG XL plugin exists for them at all: Qt has never shipped one
#       (qtimageformats v6.11.1 is dds/icns/jp2/macheif/macjp2/mng/tga/tiff/
#       wbmp/webp) and the only implementation, KDE's kimageformats, is
#       packaged for neither Fedora's mingw64 repo nor Homebrew.
assert_image_formats() {
    local label="$1" log="$2" status="$3" want="${4:-}"
    [[ -s "$log" ]] || die "$label: --image-format-status produced no output at all (an older source has no such flag; the app and the packaging must land together)"
    cat "$log"
    # The REQUIRED formats, named individually. A bare RESULT check would pass
    # on a build whose table had quietly demoted one of them.
    local fmt
    for fmt in png jpeg gif bmp webp; do
        grep -qx "required image/$fmt: decodable" "$log" || die \
            "$label: this package cannot decode image/$fmt, which Lightning's own byte sniffers ACCEPT. It would take in, forward and re-upload a format it cannot draw. See the transcript above for the plugin path it searched."
    done
    grep -q '^RESULT: this build ACCEPTS image formats it cannot decode' "$log" \
        && die "$label: the packaged binary reports an accept/decode mismatch (see above)"
    # Belt and braces, as above: a status disagreeing with the text means one
    # of the two is lying.
    [[ "$status" == 0 ]] || die \
        "$label: --image-format-status reported success but exited $status"
    if [[ "$want" == "jxl" ]]; then
        grep -qx 'optional image/jxl: decodable' "$log" || die \
            "$label: JPEG XL is not decodable. On this platform it is supposed to be -- the AppImage and snap bundle kimg_jxl.so, and the deb/rpm pull kimageformat6-plugins / kf6-kimageformats. This is the reported defect, not a limitation."
    fi
}

# --- desktop launcher entry and icons ----------------------------------------
#
# THE WINDOW ICON IS A PACKAGING PROPERTY ON WAYLAND, AND NOTHING ASSERTED IT.
#
# Qt's Wayland client implements no icon protocol -- `xdg_toplevel_icon` appears
# zero times in libQt6WaylandClient -- so QGuiApplication::setWindowIcon() is
# INERT on a native Wayland session. The compositor's only route to an icon is
# the toplevel's app id ("lightning"), which it resolves to `lightning.desktop`
# in XDG_DATA_HOME/XDG_DATA_DIRS and then reads the Icon= key of. Under X11 and
# XWayland the same setWindowIcon() call sets _NET_WM_ICON and the icon is
# right, which is exactly why this hid: AppImages up to 0.9.0 shipped without
# wayland-shell-integration and ran under XWayland. Staging that plugin moved
# them onto native Wayland and the icon went generic -- reported as "after
# updating, the icon is a placeholder".
#
# So: the entry and the icons it names must be IN the payload (here), and for a
# single-file bundle the app must publish a copy where the session looks
# (assert_desktop_status, below). Neither was checked anywhere.
#
#   $1  format label for the error message
#   $2  the staged/extracted tree whose usr/share is audited
assert_desktop_launcher_payload() {
    local label="$1" tree="$2"
    local entry="$tree/usr/share/applications/lightning.desktop"
    [[ -f "$entry" ]] || die \
        "$label: no launcher entry at usr/share/applications/lightning.desktop. Qt stamps the app id \"lightning\" on every Wayland toplevel and the compositor resolves it to that file -- without it the window and taskbar icon is a generic placeholder and nothing logs a word."
    grep -qx 'Type=Application' "$entry" || die \
        "$label: the launcher entry has no Type=Application"
    # The Icon= key is the ONE line the compositor reads for the icon, and the
    # packaging-ci fallback entry shipped without it for four releases.
    local icon
    icon="$(sed -n 's/^Icon=//p' "$entry" | head -n1)"
    [[ -n "$icon" ]] || die \
        "$label: the launcher entry has no Icon= key, so the app id resolves to an entry that names no icon -- a generic placeholder, and every other check still passes"
    [[ "$icon" == lightning ]] || die \
        "$label: the launcher entry names Icon=$icon; it must be \"lightning\", the basename of the installed hicolor icons"
    grep -qx 'StartupWMClass=lightning-matrix' "$entry" || die \
        "$label: the launcher entry has no StartupWMClass=lightning-matrix, so an X11/XWayland session cannot map the window to it"
    grep -q '^Exec=' "$entry" || die "$label: the launcher entry has no Exec="

    # The icons that entry names. A launcher entry pointing at an icon nobody
    # installed is the same observable as no entry at all.
    local size
    for size in 48 128 192 256; do
        [[ -f "$tree/usr/share/icons/hicolor/${size}x${size}/apps/$icon.png" ]] || die \
            "$label: the launcher entry names Icon=$icon and usr/share/icons/hicolor/${size}x${size}/apps/$icon.png is missing"
    done
    [[ -f "$tree/usr/share/icons/hicolor/scalable/apps/$icon.svg" ]] || die \
        "$label: usr/share/icons/hicolor/scalable/apps/$icon.svg is missing; docks and launchers prefer the scalable slot at every zoom"
    local count
    count="$(find "$tree/usr/share/icons/hicolor" -name "$icon.*" -type f 2>/dev/null | wc -l)"
    printf '%s: launcher entry ok (Icon=%s, %s icon file(s) installed)\n' \
        "$label" "$icon" "$count"
}

# The AppDir's own top-level icon: what a FILE MANAGER shows for the .AppImage
# before it is ever run, and what appimagetool embeds. linuxdeploy creates all
# three from --desktop-file/--icon-file; if either argument ever stops matching
# a real path it fails silently into a bundle with a blank icon.
#
#   $1  format label
#   $2  the extracted AppDir root
assert_appdir_root_icon() {
    local label="$1" tree="$2"
    [[ -f "$tree/lightning.desktop" ]] || die \
        "$label: the AppDir root has no lightning.desktop; appimagetool needs it and integration tools read it"
    [[ -e "$tree/.DirIcon" ]] || die \
        "$label: the AppDir root has no .DirIcon, so a file manager shows a blank icon for the .AppImage itself"
    [[ -e "$tree/lightning.png" ]] || [[ -e "$tree/lightning.svg" ]] || die \
        "$label: the AppDir root has no top-level lightning icon beside .DirIcon"
    printf '%s: AppDir root icon ok\n' "$label"
}

# ONE judgement of a `--desktop-status` transcript.
#
# Asks the SHIPPED artifact the question a file listing cannot: can this
# session resolve the app id to a launcher entry and an icon? For a single-file
# bundle the answer depends on the app publishing a copy into the user's own
# data directory at startup, so this proves the publication runs, writes, and
# lands somewhere the lookup finds -- the whole feature, end to end.
#
#   $1  format label
#   $2  path to the captured combined output
#   $3  the command's exit status
#   $4  "appimage" when the artifact is expected to self-publish an entry
assert_desktop_status() {
    local label="$1" log="$2" status="$3" kind="${4:-}"
    [[ -s "$log" ]] || die "$label: --desktop-status produced no output at all (an older source has no such flag; the app and the packaging must land together)"
    cat "$log"
    grep -qx 'app id (desktop file name): lightning' "$log" || die \
        "$label: the binary does not stamp the app id \"lightning\" on its windows, so no launcher entry can ever match it"
    grep -qx 'launcher entry basename: lightning.desktop' "$log" || die \
        "$label: the app id and the launcher entry basename disagree"
    if [[ "$kind" == appimage ]]; then
        grep -qx 'appimage runtime: yes' "$log" || die \
            "$label: the binary did not see APPIMAGE/APPDIR, so it never even tried to publish a launcher entry"
        grep -q '^payload entry icon name: lightning$' "$log" || die \
            "$label: the payload's own launcher entry does not name Icon=lightning"
        # "written" or "already current" and nothing else. The app has one
        # other legitimate outcome -- it DEFERS to a launcher entry an
        # installed deb/rpm already published, rather than shadowing it -- but
        # the caller runs this against empty scratch XDG directories precisely
        # so that outcome cannot arise. Seeing it here means the validation
        # image carries a Lightning package, which is worth failing over.
        grep -Eqx 'launcher entry: (written|already current)' "$log" || die \
            "$label: the AppImage did not publish a launcher entry (see the 'launcher entry:' line above for the reason). Without one there is no window icon on Wayland at all."
    fi
    grep -q '^visible launcher entry: NONE$' "$log" && die \
        "$label: no launcher entry is visible to the session in XDG_DATA_HOME or XDG_DATA_DIRS -- this is the generic-placeholder icon defect"
    grep -q '^visible icon: NONE$' "$log" && die \
        "$label: a launcher entry is visible but names an icon that is not installed anywhere the session looks"
    grep -q '^RESULT: the app id' "$log" || die \
        "$label: --desktop-status did not report success (see the transcript above)"
    [[ "$status" == 0 ]] || die \
        "$label: --desktop-status reported success but exited $status"
}

# --- Windows signing state ---------------------------------------------------
#
# ONE place decides whether a Windows release is signed, so artifact metadata,
# published asset names, and the release description can never disagree with
# each other or with reality. It is false today: no signing identity is
# configured, no SignPath onboarding has happened, and every published Windows
# artifact is honestly unsigned.
#
# Flip LIGHTNING_WINDOWS_SIGNED to true only in the change that actually makes
# signing happen, together with the credential that performs it. Claiming
# "signed" without a signature is a lie told to users about a security
# property, which is why this is a single explicit switch rather than something
# inferred from an environment that may merely look configured.
windows_signed() {
    [[ "${LIGHTNING_WINDOWS_SIGNED:-false}" == true ]]
}

# "signed" / "unsigned" — for metadata strings and reports.
windows_signing_state() {
    if windows_signed; then printf 'signed\n'; else printf 'unsigned\n'; fi
}

# " (unsigned)" / "" — for user-facing published asset names.
windows_unsigned_suffix() {
    if windows_signed; then printf '\n'; else printf ' (unsigned)\n'; fi
}

# Write a sibling <name>.sha256 that records only the basename, so that a
# downloaded artifact verifies with `sha256sum -c <name>.sha256` regardless of
# where it is checked out.
#
# macOS has no sha256sum; `shasum -a 256` produces byte-identical output, so the
# fallback keeps one checksum format across Linux containers and the native
# macOS runner. Ordering matters: prefer the GNU tool where it exists so Linux
# behaviour is unchanged.
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$@"
    else
        shasum -a 256 "$@"
    fi
}

write_sha256() {
    local path="$1"
    ( cd "$(dirname "$path")" && sha256_of "$(basename "$path")" >"$(basename "$path").sha256" )
}
