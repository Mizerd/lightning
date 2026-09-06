#!/usr/bin/env bash
set -Eeuo pipefail

# Prove, BEFORE anything is published, that the GitHub mirror can be written.
#
# The mirror job runs after the GitLab tag and release exist, so a dead token
# discovered there fails a release at its most expensive moment and leaves
# every installed Lightning without its GitHub fallback. Fine-grained GitHub
# tokens EXPIRE (one year at most), and nothing else in this pipeline would
# notice until then. So this runs first, in a job that holds nothing but the
# token (environment `mirror`), and refuses to let a publishing pipeline start
# when the token is unusable or about to become so.
#
# With GITHUB_MIRROR_REPO unset this is a clean no-op: mirroring is off and
# there is nothing to check.
#
# Never prints the token. The only header value it reads is the expiration.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
# shellcheck source=./scripts/update-lib.sh
source "$SCRIPT_DIR/update-lib.sh"

ROOT="${CI_PROJECT_DIR:-$(cd -- "$SCRIPT_DIR/.." && pwd)}"
CURL_BIN="${CURL_BIN:-curl}"
mkdir -p "$ROOT/dist"

# Fail when fewer than this many days of token life remain: a run takes well
# under a day, but a token that dies next week dies during the next release.
: "${GITHUB_MIRROR_TOKEN_MIN_DAYS:=2}"
# Warn loudly below this, so the rotation lands before the failure does.
: "${GITHUB_MIRROR_TOKEN_WARN_DAYS:=30}"

if ! update_mirror_enabled; then
    printf 'GitHub mirroring is disabled (GITHUB_MIRROR_REPO unset); nothing to preflight\n'
    jq -n '{enabled:false}' >"$ROOT/dist/github-mirror-preflight.json"
    exit 0
fi
# A lull refresh of the manifest re-promotes GitLab's `latest` slot and needs
# nothing from GitHub; the slot job that follows is best-effort. A dead
# mirror token must not stop the one operation that keeps every
# GitLab-reachable client current, so the check is skipped rather than
# failed -- and it will run, hard, on the next real release.
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
# A fine-grained token reports what it may do; the mirror needs to write
# release assets, which GitHub exposes as `push`. "GitHub said no" and
# "GitHub did not say" are different answers: only the first is a refusal,
# because the shape of this object is GitHub's to change and a missing field
# must not turn into a release that cannot run.
perm="$(jq -r 'if has("permissions") then (.permissions.push // false | tostring) else "unknown" end' "$repo_json")"
case "$perm" in
    true) ;;
    unknown) printf 'WARNING: GitHub returned no permissions object for %s; write access could not be confirmed here and the mirror job will be the first to find out.\n' "$GITHUB_MIRROR_REPO" ;;
    *) die "GITHUB_MIRROR_TOKEN can read ${GITHUB_MIRROR_REPO} but cannot write to it (Contents: read and write is required)" ;;
esac

# Fine-grained tokens carry their expiry on every response; classic tokens
# without one carry nothing, which is reported as unknown rather than fine.
expires_raw="$(tr -d '\r' <"$headers" | awk -F': ' 'tolower($1)=="github-authentication-token-expiration"{print $2; exit}')"
days_left=""
if [[ -n "$expires_raw" ]]; then
    # e.g. "2027-01-01 00:00:00 UTC" — GNU date (coreutils, installed by the
    # job; BusyBox date cannot parse it) reads it as given. A format this
    # cannot parse is not evidence of expiry: the token has already answered
    # 200, so it is a warning, and the day count is left unknown.
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

# THE SLOT ITSELF. mirror-update-manifest-to-github is allow_failure so a
# GitHub outage cannot fail a completed release -- which means a slot that
# rotted (an upload that failed on every retry, an asset removed by hand)
# would otherwise announce itself to nobody until a client fell back to it.
# Read both assets anonymously, exactly as a client would, and WARN. Never a
# failure: a first-ever release has no slot yet, and a broken slot is fixed
# by the promotion this very pipeline is about to make.
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
