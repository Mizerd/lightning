#!/usr/bin/env bash
set -Eeuo pipefail

# Fail the pipeline when an OPTIONAL asset the run asked for did not make it
# into the published release.
#
# WHY THIS EXISTS. `macos-package-test` is `allow_failure: true` and
# `publish-packages` needs it `optional` — deliberately, and that must not
# change: one sleeping Mac may not block a release (§14, docs/macos-packaging.md).
# The price of that safety valve is that the Mac lane is the ONLY one whose
# absence a green pipeline does not report. It was paid in full at 0.9.5: the
# bundle built, passed every check, failed to upload, and the release published
# green with no macOS download. Nobody found out from the pipeline.
#
# So this job restores the signal without restoring the dependency. It runs in
# the LAST stage, after finalize-release and both mirrors, and nothing needs
# it — the release is already complete and immutable by the time it speaks. It
# cannot block or alter publication. All it can do is refuse to let the
# pipeline claim success while a requested asset is missing.
#
# It asks the PUBLISHED RELEASE rather than the job's status, which is the same
# discipline §14's verification bar uses: a job that says it uploaded is not an
# asset that is there.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
# shellcheck source=./scripts/gitlab-api.sh
source "$SCRIPT_DIR/gitlab-api.sh"

gitlab_api_init
# RELEASE_TAG comes from here, and its absence is why this job failed on its
# FIRST EVER execution -- pipeline 222, the 0.9.6 release, with
# "RELEASE_TAG: unbound variable". The job was committed in 64a1f6d and no
# release ran between then and now, so nothing could have found out: a job
# that exists and looks right is not a job that has run. Every other
# publishing script pairs these two calls; this one had only the first.
release_contract_env

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

missing=0

# --- macOS ------------------------------------------------------------------
#
# Only checked when the run actually asked for it. A release deliberately cut
# without the Mac (BUILD_MACOS_PACKAGES=false) is not incomplete, and must not
# be reported as such.
if [[ "${BUILD_MACOS_PACKAGES:-false}" == true ]]; then
    links_json="$tmp_dir/links.json"
    api_json_get "/releases/${RELEASE_TAG}/assets/links?per_page=100" "$links_json"

    # Match the FILENAME the macOS lane produces, not a display name: link
    # names are editorial and have been reworded before, whereas the suffix is
    # the same token data-lg-match uses on the website and the same one
    # check-assets.py resolves against.
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
