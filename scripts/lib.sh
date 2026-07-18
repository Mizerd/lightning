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

# Write a sibling <name>.sha256 that records only the basename, so that a
# downloaded artifact verifies with `sha256sum -c <name>.sha256` regardless of
# where it is checked out.
write_sha256() {
    local path="$1"
    ( cd "$(dirname "$path")" && sha256sum "$(basename "$path")" >"$(basename "$path").sha256" )
}
