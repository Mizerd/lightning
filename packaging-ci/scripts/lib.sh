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
# Judges a `--call-media-status` transcript; shared by every Linux format so the
# bar cannot drift. The engine is a compile-time #ifdef and its plugins are
# dlopen'd, so only running the shipped artifact can answer whether it works.
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
    # RESULT comes from the engine's own element probe, so it reflects the
    # packaged plugin set (deb/rpm Depends, bundle, or Flatpak runtime).
    grep -qx 'RESULT: calls can be placed and answered.' "$log" || die \
        "$label: the engine is compiled in but cannot run -- see the missing_element or gstreamer line above. The package does not carry (or cannot find) the GStreamer plugins the call engine needs."
    [[ "$status" == 0 ]] || die \
        "$label: --call-media-status reported success but exited $status"
}

# --- a time bound that exists on macOS too -----------------------------------
#
# macOS has no GNU `timeout`; Homebrew's coreutils names it `gtimeout`. Without
# either, fall back to a background-and-poll watchdog. A hung probe must not
# run to the job's three-hour ceiling.
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
# Judges a `--call-queue-selftest` transcript for every format (Linux, Windows
# under Wine, macOS). A default GStreamer `queue` holds one second and never
# leaks it, so a consumer that falls behind once adds permanent call delay.
#
#   VERDICT: pass          measured, and every shipped queue behaved
#   VERDICT: fail          measured, and one did not
#   VERDICT: unmeasurable  nothing was measured (no engine, GStreamer init or
#                          element failure, or the control queue did not fill)
#
# `unmeasurable` dies: "could not measure" must never pass as a warning.
# `fail` only warns until the check has proven stable everywhere.
#
# TO PROMOTE IT: once it reports `VERDICT: pass` on every format in one
# pipeline and the wall-clock thresholds in `runQueueSelfTest` are stable on
# the CI runners, change the `fail` branch below to `die`. This is the only
# place to change.
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
    # Wine output is CRLF; strip CR so `pass\r` still matches.
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
        # The command never reached its own report.
        $complain "$label: --call-queue-selftest printed no VERDICT line at all -- it crashed, hung, was not found, or is an older build. The transcript is above."
        return 1
        ;;
    *)
        # An unreadable verdict is a different fault from a missing one.
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
# Judges a `--call-sounds-status` transcript for every format. Warn-only:
# QSoundEffect needs an output device to reach Ready and CI containers have
# none, so output `none` means unmeasured, not failed. To promote, give the
# validators a null sink and make the MEASURED SHORT branch fail.
# test-pipeline-config.py checks the count against data/sounds/*.wav.
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
# Judges an `--image-format-status` transcript for every format. Qt image
# formats are dlopen'd plugins, so only the shipped artifact can say what it
# decodes; a plugin file being present does not mean it loads. The build
# scripts assert the files; this asserts the decoders register, so the app
# never accepts a format (e.g. webp) it cannot draw.
#
#   $1  format label for the error message
#   $2  path to the captured combined output
#   $3  the command's exit status
#   $4  "jxl" when this platform must also decode JPEG XL. Linux only: Qt
#       ships no JPEG XL plugin and KDE's kimageformats is packaged for neither
#       Fedora's mingw64 repo nor Homebrew.
assert_image_formats() {
    local label="$1" log="$2" status="$3" want="${4:-}"
    [[ -s "$log" ]] || die "$label: --image-format-status produced no output at all (an older source has no such flag; the app and the packaging must land together)"
    cat "$log"
    # Named individually: a bare RESULT check would miss a demoted format.
    local fmt
    for fmt in png jpeg gif bmp webp; do
        grep -qx "required image/$fmt: decodable" "$log" || die \
            "$label: this package cannot decode image/$fmt, which Lightning's own byte sniffers ACCEPT. It would take in, forward and re-upload a format it cannot draw. See the transcript above for the plugin path it searched."
    done
    grep -q '^RESULT: this build ACCEPTS image formats it cannot decode' "$log" \
        && die "$label: the packaged binary reports an accept/decode mismatch (see above)"
    [[ "$status" == 0 ]] || die \
        "$label: --image-format-status reported success but exited $status"
    if [[ "$want" == "jxl" ]]; then
        grep -qx 'optional image/jxl: decodable' "$log" || die \
            "$label: JPEG XL is not decodable. On this platform it is supposed to be -- the AppImage and snap bundle kimg_jxl.so, and the deb/rpm pull kimageformat6-plugins / kf6-kimageformats. This is the reported defect, not a limitation."
    fi
}

# --- desktop launcher entry and icons ----------------------------------------
#
# On native Wayland, Qt implements no icon protocol, so setWindowIcon() is
# inert: the compositor maps the app id "lightning" to `lightning.desktop` in
# XDG_DATA_DIRS and uses its Icon= key. The entry and its icons must be in the
# payload (here), and a single-file bundle must publish a copy where the
# session looks (assert_desktop_status, below).
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
    # Icon= is the line the compositor reads for the window icon.
    local icon
    icon="$(sed -n 's/^Icon=//p' "$entry" | head -n1)"
    [[ -n "$icon" ]] || die \
        "$label: the launcher entry has no Icon= key, so the app id resolves to an entry that names no icon -- a generic placeholder, and every other check still passes"
    [[ "$icon" == lightning ]] || die \
        "$label: the launcher entry names Icon=$icon; it must be \"lightning\", the basename of the installed hicolor icons"
    grep -qx 'StartupWMClass=lightning-matrix' "$entry" || die \
        "$label: the launcher entry has no StartupWMClass=lightning-matrix, so an X11/XWayland session cannot map the window to it"
    grep -q '^Exec=' "$entry" || die "$label: the launcher entry has no Exec="

    # An entry naming an uninstalled icon is as bad as no entry.
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

# The AppDir's top-level icon: what a file manager shows for the .AppImage and
# what appimagetool embeds. linuxdeploy silently omits it if --desktop-file or
# --icon-file stops matching a real path.
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

# Judges a `--desktop-status` transcript: can this session resolve the app id
# to a launcher entry and an icon? For a single-file bundle this also proves the
# app publishes its entry into the user's data directory at startup.
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
        # The app may also defer to an entry an installed deb/rpm published,
        # but callers use empty scratch XDG directories, so that outcome here
        # means the validation image carries a Lightning package.
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
# The single switch for whether Windows artifacts are signed, so metadata,
# asset names and release notes cannot disagree. No signing is configured, so
# it is false. Set LIGHTNING_WINDOWS_SIGNED=true only in the change that
# actually adds signing; it is explicit rather than inferred from the
# environment so a release never claims a signature it does not have.
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

# macOS has no sha256sum; `shasum -a 256` produces identical output.
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$@"
    else
        shasum -a 256 "$@"
    fi
}

# Write a sibling <name>.sha256 recording only the basename, so the artifact
# verifies with `sha256sum -c <name>.sha256` wherever it is downloaded.
write_sha256() {
    local path="$1"
    ( cd "$(dirname "$path")" && sha256_of "$(basename "$path")" >"$(basename "$path").sha256" )
}
