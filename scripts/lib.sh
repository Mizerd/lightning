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
