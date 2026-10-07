#!/usr/bin/env bash
set -Eeuo pipefail

# Fail the pipeline when the published release lacks the macOS asset.
#
# Since 2026-10-07 a release cannot publish without a green macos-package-test
# (no allow_failure, no `optional` need, and write-manifest.sh refuses a
# missing bundle), so this is the after-the-fact proof that the bytes really
# reached the release. It runs last, after the release is complete, and checks
# the published release itself, not job status. It is not allow_failure: a
# miss turns the pipeline red.

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
# Every publishing pipeline builds macOS now (.macos-test-rules), whatever
# BUILD_MACOS_PACKAGES says, so the asset is always required.
links_json="$tmp_dir/links.json"
api_json_get "/releases/${RELEASE_TAG}/assets/links?per_page=100" "$links_json"

# Match the filename suffix, not the editorial link name; the website
# (data-lg-match) and check-assets.py key on the same suffix.
if jq -e '[.[] | select(.url | endswith("-macos-arm64.zip"))] | length > 0' \
        "$links_json" >/dev/null; then
    printf 'ok: the macOS bundle is attached to %s\n' "$RELEASE_TAG"
else
    printf 'MISSING: %s has NO macOS asset, although publication requires a green\n' \
        "$RELEASE_TAG" >&2
    printf '  macos-package-test and write-manifest.sh refuses a missing bundle.\n' >&2
    printf '  Something between the build and the release link lost it. Read the\n' >&2
    printf '  publish-packages and finalize-release logs, then docs/macos-packaging.md\n' >&2
    printf '  ("The artifact upload").\n' >&2
    printf '  DO NOT backfill with attach-existing: a publishing pipeline rebuilds\n' >&2
    printf '  every format, those bytes differ from the published ones, and\n' >&2
    printf '  SHA256SUMS would no longer cover what is published.\n' >&2
    missing=1
fi

if (( missing )); then
    die "a required asset is missing from ${RELEASE_TAG}"
fi

printf 'Every required asset is present on %s\n' "$RELEASE_TAG"
