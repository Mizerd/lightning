#!/usr/bin/env bash
set -Eeuo pipefail

# Fail a publishing pipeline loudly and early when its inputs are wrong, instead
# of silently building packages that later rule evaluation would refuse to
# publish. Runs first in resolve-source. Non-publishing (build-only) pipelines
# accept any ref and skip every publication requirement here.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"

# Publication must never target another project through a settable variable.
: "${TARGET_PROJECT_ID:=6}"
[[ "$TARGET_PROJECT_ID" == 6 ]] || die "TARGET_PROJECT_ID must be 6"
: "${LIGHTNING_PROJECT_ID:=6}"
[[ "$LIGHTNING_PROJECT_ID" == 6 ]] || die "LIGHTNING_PROJECT_ID must be 6"

: "${PUBLISH_PACKAGES:=false}"
case "$PUBLISH_PACKAGES" in
    true|false) ;;
    *) die "PUBLISH_PACKAGES must be true or false" ;;
esac

# The ref's SHAPE is checked on EVERY pipeline, publishing or not. Git accepts
# shell metacharacters in a ref name (`v1$(id)` and `a;b` both pass
# check-ref-format), and the value is written into dist/version.env, which
# every later script `source`s. resolve-version.sh also %q-quotes it; this is
# the belt to that brace.
if [[ -n "${SOURCE_REF:-}" ]]; then
    [[ "$SOURCE_REF" =~ ^[A-Za-z0-9][A-Za-z0-9._/-]{0,254}$ ]] || \
        die "SOURCE_REF must be 1-255 characters of [A-Za-z0-9._/-] and start alphanumeric"
fi

if [[ "$PUBLISH_PACKAGES" != true ]]; then
    printf 'Build-only pipeline: publication and release stages are disabled\n'
    exit 0
fi

# From here on this is a publishing pipeline; every requirement is mandatory.
require_var RELEASE_ACTION
case "$RELEASE_ACTION" in
    create|attach-existing) ;;
    *) die "RELEASE_ACTION must be 'create' or 'attach-existing'" ;;
esac

require_var RELEASE_VERSION
[[ "$RELEASE_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || \
    die "RELEASE_VERSION must be X.Y.Z with no leading v"

require_var SOURCE_REF
case "$RELEASE_ACTION" in
    create)
        # A brand-new release must pin an exact, immutable commit; never a
        # branch and never a tag that could be recreated.
        [[ "$SOURCE_REF" =~ ^[0-9a-f]{40}$ ]] || \
            die "RELEASE_ACTION=create requires SOURCE_REF to be a full 40-character commit SHA"
        ;;
    attach-existing)
        # A backfill runs against the already-published tag or its exact commit.
        [[ "$SOURCE_REF" == "v${RELEASE_VERSION}" || "$SOURCE_REF" =~ ^[0-9a-f]{40}$ ]] || \
            die "RELEASE_ACTION=attach-existing requires SOURCE_REF=v${RELEASE_VERSION} or a full commit SHA"
        ;;
esac

if [[ -n "${CI:-}" ]]; then
    [[ -n "${CI_COMMIT_BRANCH:-}" && "$CI_COMMIT_BRANCH" == "${CI_DEFAULT_BRANCH:-}" ]] || \
        die "publication is allowed only from the packaging project's default branch"
fi

# Official packages embed application GIF provider keys so installed clients work
# without user configuration. Presence-only check (never the value, length, or
# any fragment) so a release without keys fails here, before the expensive build.
[[ -n "${GIPHY_API_KEY:-}" ]] || die "official release requires the GIPHY_API_KEY CI variable"
[[ -n "${KLIPY_API_KEY:-}" ]] || die "official release requires the KLIPY_API_KEY CI variable"
printf 'GIPHY_API_KEY is configured\n'
printf 'KLIPY_API_KEY is configured\n'

# Update-signing consistency. Deliberately here, in the first job: the public
# key is compiled into every package by the BUILD jobs, so a mismatch found
# after the build is baked into artifacts that cannot be corrected. In CI
# resolve-source declares the `signing` environment precisely so the private
# key IS present here and the full check runs before anything is built (see
# .gitlab-ci.yml). The public-only branch exists for a run without the key
# -- a build-only pipeline on a misconfigured project, a local invocation --
# and never applies to a publishing pipeline in CI, where require_var in the
# full check refuses. Prints no key material.
if [[ -n "${UPDATE_SIGNING_KEY_B64:-}" ]]; then
    "$SCRIPT_DIR/check-update-signing-keys.sh"
else
    "$SCRIPT_DIR/check-update-signing-keys.sh" --public-only
fi

printf 'Publishing pipeline accepted: action=%s version=%s ref=%s\n' \
    "$RELEASE_ACTION" "$RELEASE_VERSION" "$SOURCE_REF"
