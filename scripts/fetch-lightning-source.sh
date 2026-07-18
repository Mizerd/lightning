#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"

require_var LIGHTNING_REPOSITORY
require_var SOURCE_REF

ROOT="$(project_dir)"
SOURCE_DIR="$ROOT/work/lightning"
DIST_DIR="$ROOT/dist"
EXPECTED_REPOSITORY="https://gitlab.smetonis.net/Mizerd/lightning.git"

[[ "$LIGHTNING_REPOSITORY" == "$EXPECTED_REPOSITORY" ]] || \
    die "LIGHTNING_REPOSITORY must be ${EXPECTED_REPOSITORY}"
[[ "$SOURCE_REF" != -* && "$SOURCE_REF" != *$'\n'* && "$SOURCE_REF" != *$'\r'* ]] || \
    die "SOURCE_REF contains unsafe characters"

mkdir -p "$ROOT/work" "$DIST_DIR"
[[ ! -e "$SOURCE_DIR" ]] || die "source directory already exists: ${SOURCE_DIR}"

ASKPASS_DIR="$(mktemp -d "$ROOT/work/git-askpass.XXXXXX")"
cleanup() {
    rm -f "$ASKPASS_DIR/askpass.sh"
    rmdir "$ASKPASS_DIR" 2>/dev/null || true
}
trap cleanup EXIT

cat >"$ASKPASS_DIR/askpass.sh" <<'ASKPASS'
#!/bin/sh
case "$1" in
    *sername*) printf '%s\n' "${LIGHTNING_GIT_USERNAME:?}" ;;
    *assword*) printf '%s\n' "${LIGHTNING_GIT_PASSWORD:?}" ;;
    *) exit 1 ;;
esac
ASKPASS
chmod 0700 "$ASKPASS_DIR/askpass.sh"

if [[ -n "${CI_JOB_TOKEN:-}" ]]; then
    export LIGHTNING_GIT_USERNAME="gitlab-ci-token"
    export LIGHTNING_GIT_PASSWORD="$CI_JOB_TOKEN"
elif [[ -n "${LIGHTNING_DEPLOY_TOKEN:-}" && -n "${LIGHTNING_DEPLOY_USER:-}" ]]; then
    export LIGHTNING_GIT_USERNAME="$LIGHTNING_DEPLOY_USER"
    export LIGHTNING_GIT_PASSWORD="$LIGHTNING_DEPLOY_TOKEN"
else
    die "CI_JOB_TOKEN is unavailable and no read-only deploy-token fallback is configured"
fi

export GIT_ASKPASS="$ASKPASS_DIR/askpass.sh"
export GIT_TERMINAL_PROMPT=0

printf 'Fetching Lightning ref %s from the configured private GitLab project\n' "$SOURCE_REF"
git clone --no-checkout --origin origin "$LIGHTNING_REPOSITORY" "$SOURCE_DIR"
git -C "$SOURCE_DIR" fetch --force --tags origin

if git -C "$SOURCE_DIR" show-ref --verify --quiet "refs/tags/$SOURCE_REF"; then
    RESOLVED_SHA="$(git -C "$SOURCE_DIR" rev-parse "refs/tags/$SOURCE_REF^{commit}")"
elif git -C "$SOURCE_DIR" show-ref --verify --quiet "refs/remotes/origin/$SOURCE_REF"; then
    RESOLVED_SHA="$(git -C "$SOURCE_DIR" rev-parse "refs/remotes/origin/$SOURCE_REF^{commit}")"
elif RESOLVED_SHA="$(git -C "$SOURCE_DIR" rev-parse --verify "${SOURCE_REF}^{commit}" 2>/dev/null)"; then
    :
else
    die "Lightning ref not found: ${SOURCE_REF}"
fi

if [[ -n "${EXPECTED_SOURCE_SHA:-}" && "$RESOLVED_SHA" != "$EXPECTED_SOURCE_SHA" ]]; then
    die "source ref moved: expected ${EXPECTED_SOURCE_SHA}, resolved ${RESOLVED_SHA}"
fi

git -C "$SOURCE_DIR" checkout --detach "$RESOLVED_SHA"
git -C "$SOURCE_DIR" submodule update --init --recursive
git -C "$SOURCE_DIR" remote set-url origin "$EXPECTED_REPOSITORY"

unset LIGHTNING_GIT_PASSWORD CI_JOB_TOKEN LIGHTNING_DEPLOY_TOKEN
cleanup
trap - EXIT

STATUS="$(git -C "$SOURCE_DIR" status --porcelain)"
[[ -z "$STATUS" ]] || die "checked-out Lightning source is dirty"

COMMIT_TIME="$(git -C "$SOURCE_DIR" show -s --format=%cI HEAD)"
CLOSEST_TAG="$(git -C "$SOURCE_DIR" describe --tags --abbrev=0 2>/dev/null || true)"
EXACT_TAG="$(git -C "$SOURCE_DIR" describe --tags --exact-match 2>/dev/null || true)"

jq -n \
    --arg requested_ref "$SOURCE_REF" \
    --arg resolved_sha "$RESOLVED_SHA" \
    --arg commit_time "$COMMIT_TIME" \
    --arg closest_tag "$CLOSEST_TAG" \
    --arg exact_tag "$EXACT_TAG" \
    --arg repository "$EXPECTED_REPOSITORY" \
    '{repository:$repository, requested_ref:$requested_ref, resolved_sha:$resolved_sha,
      commit_time:$commit_time, closest_tag:$closest_tag, exact_tag:$exact_tag,
      dirty:false}' >"$DIST_DIR/source-info.json"

printf 'Resolved Lightning ref %s to %s (%s)\n' "$SOURCE_REF" "$RESOLVED_SHA" "$COMMIT_TIME"
