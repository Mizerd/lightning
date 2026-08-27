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
