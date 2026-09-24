#!/usr/bin/env bash
set -Eeuo pipefail

# Fail the pipeline when an OPTIONAL asset the run asked for did not make it
# into the published release.
#
# `macos-package-test` is allow_failure and optional for `publish-packages` so
# an offline Mac cannot block a release (docs/macos-packaging.md), which means
# a missing macOS asset would otherwise go unreported. This runs last, after
# the release is complete, so it cannot block publication; it only refuses to
# let the pipeline report success. It checks the published release itself,
# not job status.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
# shellcheck source=./scripts/gitlab-api.sh
source "$SCRIPT_DIR/gitlab-api.sh"

gitlab_api_init
# Sets RELEASE_TAG.
release_contract_env

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

missing=0

# --- macOS ------------------------------------------------------------------
#
# Only when requested: a release cut with BUILD_MACOS_PACKAGES=false is not
# incomplete.
if [[ "${BUILD_MACOS_PACKAGES:-false}" == true ]]; then
    links_json="$tmp_dir/links.json"
    api_json_get "/releases/${RELEASE_TAG}/assets/links?per_page=100" "$links_json"

    # Match the filename suffix, not the editorial link name; the website
    # (data-lg-match) and check-assets.py key on the same suffix.
    if jq -e '[.[] | select(.url | endswith("-macos-arm64.zip"))] | length > 0' \
            "$links_json" >/dev/null; then
        printf 'ok: the macOS bundle is attached to %s\n' "$RELEASE_TAG"
    else
        printf 'MISSING: %s was built with BUILD_MACOS_PACKAGES=true and has NO macOS asset.\n' \
            "$RELEASE_TAG" >&2
        printf '  The release itself is complete and correct -- every other artifact\n' >&2
        printf '  published, and the signed update manifest never carries macOS anyway.\n' >&2
        printf '  What failed is the macOS lane, and this job exists so that failure is\n' >&2
        printf '  not silent. Read the macos-package-test log, then\n' >&2
        printf '  docs/macos-packaging.md ("The artifact upload").\n' >&2
        printf '  DO NOT backfill with attach-existing: a publishing pipeline rebuilds\n' >&2
        printf '  every format, those bytes differ from the published ones, and\n' >&2
        printf '  SHA256SUMS would no longer cover what is published.\n' >&2
        missing=1
    fi
fi

if (( missing )); then
    die "a requested optional asset is missing from ${RELEASE_TAG}"
fi

printf 'Every requested optional asset is present on %s\n' "$RELEASE_TAG"
