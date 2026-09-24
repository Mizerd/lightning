#!/usr/bin/env bash
set -Eeuo pipefail

# Prove, before anything is published, that the GitHub mirror can be written.
# The mirror job runs after the tag exists, so a dead or expiring token must be
# caught here. Runs in a job holding only the token (environment `mirror`);
# a no-op when GITHUB_MIRROR_REPO is unset. Never prints the token.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
# shellcheck source=./scripts/update-lib.sh
source "$SCRIPT_DIR/update-lib.sh"

ROOT="${CI_PROJECT_DIR:-$(cd -- "$SCRIPT_DIR/.." && pwd)}"
CURL_BIN="${CURL_BIN:-curl}"
mkdir -p "$ROOT/dist"

# Fail below this many days of token life.
: "${GITHUB_MIRROR_TOKEN_MIN_DAYS:=2}"
# Warn below this, so rotation lands before the failure.
: "${GITHUB_MIRROR_TOKEN_WARN_DAYS:=30}"

if ! update_mirror_enabled; then
    printf 'GitHub mirroring is disabled (GITHUB_MIRROR_REPO unset); nothing to preflight\n'
    jq -n '{enabled:false}' >"$ROOT/dist/github-mirror-preflight.json"
    exit 0
fi
# A manifest refresh needs nothing from GitHub, so a dead token must not block
# it; the check runs again on the next real release.
if [[ "${UPDATE_REFRESH_LATEST_ONLY:-false}" == true ]]; then
    printf 'Manifest refresh (UPDATE_REFRESH_LATEST_ONLY=true): the mirror token is not required, preflight skipped\n'
    jq -n '{enabled:true, skipped:"refresh"}' >"$ROOT/dist/github-mirror-preflight.json"
    exit 0
fi

update_mirror_repo_valid "$GITHUB_MIRROR_REPO" || \
    die "GITHUB_MIRROR_REPO must be <owner>/<repo>, got '$GITHUB_MIRROR_REPO'"
require_var GITHUB_MIRROR_TOKEN

tmp_dir="$(mktemp -d)"
chmod 700 "$tmp_dir"
auth_conf="$(mktemp)"
chmod 600 "$auth_conf"
cleanup() { rm -f "$auth_conf"; rm -rf "$tmp_dir"; }
trap cleanup EXIT
{
    printf 'header = "Authorization: Bearer %s"\n' "$GITHUB_MIRROR_TOKEN"
    printf 'header = "Accept: application/vnd.github+json"\n'
    printf 'header = "X-GitHub-Api-Version: 2022-11-28"\n'
} >"$auth_conf"

repo_json="$tmp_dir/repo.json"
headers="$tmp_dir/headers.txt"
# Token-bearing: no redirects, ever (see mirror-release-to-github.sh).
status="$("$CURL_BIN" --silent --show-error --max-redirs 0 \
    --config "$auth_conf" --dump-header "$headers" \
    --output "$repo_json" --write-out '%{http_code}' \
    "${UPDATE_MIRROR_API_HOST}/repos/${GITHUB_MIRROR_REPO}")" || \
    die "GitHub could not be reached to preflight the mirror token"
case "$status" in
    200) ;;
    401) die "GITHUB_MIRROR_TOKEN is not accepted by GitHub (HTTP 401): it has expired or been revoked. Rotate it in project 7 before publishing; the mirror is what keeps installed clients updating if GitLab is down." ;;
    403) die "GITHUB_MIRROR_TOKEN is refused for ${GITHUB_MIRROR_REPO} (HTTP 403): it lacks access to the mirror repository or is rate-limited" ;;
    404) die "GitHub answered 404 for ${GITHUB_MIRROR_REPO}: the repository does not exist or the token cannot see it" ;;
    *) die "GitHub answered HTTP ${status} for ${GITHUB_MIRROR_REPO}" ;;
esac
# Writing release assets needs `push`. A missing permissions object only
# warns: GitHub may change the response shape.
perm="$(jq -r 'if has("permissions") then (.permissions.push // false | tostring) else "unknown" end' "$repo_json")"
case "$perm" in
    true) ;;
    unknown) printf 'WARNING: GitHub returned no permissions object for %s; write access could not be confirmed here and the mirror job will be the first to find out.\n' "$GITHUB_MIRROR_REPO" ;;
    *) die "GITHUB_MIRROR_TOKEN can read ${GITHUB_MIRROR_REPO} but cannot write to it (Contents: read and write is required)" ;;
esac

# Fine-grained tokens report their expiry in a header; classic ones may not.
expires_raw="$(tr -d '\r' <"$headers" | awk -F': ' 'tolower($1)=="github-authentication-token-expiration"{print $2; exit}')"
days_left=""
if [[ -n "$expires_raw" ]]; then
    # e.g. "2027-01-01 00:00:00 UTC"; needs GNU date, not BusyBox. An
    # unparseable value only warns, since the token already answered 200.
    if expires_epoch="$(date -u -d "$expires_raw" +%s 2>/dev/null)"; then
        now_epoch="$(date -u +%s)"
        days_left=$(( (expires_epoch - now_epoch) / 86400 ))
    else
        printf 'WARNING: GitHub reported a token expiry this job could not parse (%s); the token works today, but put a rotation date on the calendar yourself.\n' "$expires_raw"
    fi
fi
if [[ -n "$days_left" ]]; then
    if (( days_left < GITHUB_MIRROR_TOKEN_MIN_DAYS )); then
        die "GITHUB_MIRROR_TOKEN expires in ${days_left} day(s) (${expires_raw}); rotate it in project 7 before publishing so the mirror job cannot die after the tag exists"
    fi
    if (( days_left < GITHUB_MIRROR_TOKEN_WARN_DAYS )); then
        printf 'WARNING: GITHUB_MIRROR_TOKEN expires in %s day(s) (%s). Rotate it now; a release after that date will fail here, before publishing anything.\n' \
            "$days_left" "$expires_raw"
    fi
    printf 'GitHub mirror token for %s is valid, can write, and expires in %s day(s)\n' \
        "$GITHUB_MIRROR_REPO" "$days_left"
elif [[ -z "$expires_raw" ]]; then
    printf 'GitHub mirror token for %s is valid and can write; GitHub reported no expiry (classic token?) -- put a rotation date on the calendar yourself\n' \
        "$GITHUB_MIRROR_REPO"
fi

# The slot job is allow_failure, so check the fallback slot anonymously here
# and warn if it is broken. Never fatal: a first release has no slot yet, and
# this pipeline's promotion repairs it.
for name in "$UPDATE_SIG_NAME" "$UPDATE_MANIFEST_NAME"; do
    url="${UPDATE_MIRROR_DOWNLOAD_HOST}/${GITHUB_MIRROR_REPO}/releases/download/${UPDATE_LATEST_TAG}/${name}"
    slot_status="$("$CURL_BIN" --silent --show-error --location --max-redirs 5 --max-time 30 \
        --output /dev/null --write-out '%{http_code}' "$url" 2>/dev/null || printf '000')"
    if [[ "$slot_status" == 200 ]]; then
        printf 'GitHub update slot serves %s\n' "$name"
    else
        printf 'WARNING: the GitHub update slot does not serve %s (HTTP %s). Installed clients have no working fallback until this pipeline promotes a manifest; if this is not the first release, the previous slot publication failed silently.\n' \
            "$name" "$slot_status"
    fi
done

jq -n --arg repo "$GITHUB_MIRROR_REPO" --arg expires "$expires_raw" \
    --argjson days "${days_left:-null}" \
    '{enabled:true, repository:$repo, token_expires:(if $expires == "" then null else $expires end), days_left:$days}' \
    >"$ROOT/dist/github-mirror-preflight.json"
