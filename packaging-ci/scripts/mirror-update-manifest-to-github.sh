#!/usr/bin/env bash
set -Eeuo pipefail

# Publish the SIGNED update-manifest pair to the GitHub mirror's fixed
# `update-latest` release slot, after GitLab's `latest` slot has been written.
#
# Clients compile in this slot (src/update/UpdateEndpoints.cpp) as the
# fallback when GitLab does not answer, so the whole update path stays
# reachable from GitHub alone. GitHub is still not an authority: the manifest
# is signed on GitLab and this job only copies bytes GitLab has already
# promoted (dist/update-publication.json). `update-latest` is a moving pointer
# and is never marked GitHub's "latest release", which the website reads.
#
# allow_failure in CI, and idempotent so a retry converges.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
# shellcheck source=./scripts/update-lib.sh
source "$SCRIPT_DIR/update-lib.sh"

ROOT="${CI_PROJECT_DIR:-$(cd -- "$SCRIPT_DIR/.." && pwd)}"
CURL_BIN="${CURL_BIN:-curl}"
# The client compiles this slot name in; any other value is written nowhere
# a client reads.
[[ "$UPDATE_LATEST_TAG" == update-latest ]] || die "UPDATE_LATEST_TAG must be update-latest"
# How long the anonymous read-back may wait for GitHub's CDN to propagate.
: "${UPDATE_SLOT_READBACK_WAIT_SECONDS:=300}"
: "${UPDATE_SLOT_READBACK_POLL_SECONDS:=10}"
[[ "$UPDATE_SLOT_READBACK_WAIT_SECONDS" =~ ^[0-9]+$ ]] || die "UPDATE_SLOT_READBACK_WAIT_SECONDS must be an integer"
# Must be positive, or the wait loop never reaches its deadline.
[[ "$UPDATE_SLOT_READBACK_POLL_SECONDS" =~ ^[1-9][0-9]*$ ]] || die "UPDATE_SLOT_READBACK_POLL_SECONDS must be a positive integer"

if ! update_mirror_enabled; then
    printf 'GitHub mirroring is disabled (GITHUB_MIRROR_REPO unset); the update manifest stays on GitLab only\n'
    exit 0
fi
update_mirror_repo_valid "$GITHUB_MIRROR_REPO" || \
    die "GITHUB_MIRROR_REPO must be <owner>/<repo>, got '$GITHUB_MIRROR_REPO'"
require_var GITHUB_MIRROR_TOKEN
load_versions "$ROOT"
require_var SOURCE_SHA

manifest="$ROOT/dist/${UPDATE_MANIFEST_NAME}"
sig="$ROOT/dist/${UPDATE_SIG_NAME}"
publication="$ROOT/dist/update-publication.json"
[[ -f "$manifest" && -f "$sig" ]] || die "signed update manifest pair missing from dist/"
# The publication record exists only after GitLab's promotion verified, and
# names the exact bytes to mirror.
[[ -f "$publication" ]] || \
    die "dist/update-publication.json is missing: the GitLab latest slot has not been promoted, so there is nothing to mirror"
sha_of() { sha256sum "$1" | cut -d' ' -f1; }
size_of() { wc -c <"$1" | tr -d ' '; }
[[ "$(jq -r '.manifest_sha256 // ""' "$publication")" == "$(sha_of "$manifest")" ]] || \
    die "the local manifest is not the one GitLab promoted (sha256 differs from dist/update-publication.json)"
[[ "$(jq -r '.signature_sha256 // ""' "$publication")" == "$(sha_of "$sig")" ]] || \
    die "the local signature is not the one GitLab promoted"
manifest_version="$(jq -r '.version // ""' "$manifest")"
[[ -n "$manifest_version" ]] || die "the manifest carries no version"

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
api_base="${UPDATE_MIRROR_API_HOST}/repos/${GITHUB_MIRROR_REPO}"
upload_base="${UPDATE_MIRROR_UPLOAD_HOST}/repos/${GITHUB_MIRROR_REPO}"

gh_get() { # url output -> prints status; token-bearing, so never a redirect
    "$CURL_BIN" --silent --show-error --max-redirs 0 \
        --config "$auth_conf" --output "$2" --write-out '%{http_code}' "$1"
}
gh_get_anonymous() { # exactly as a client fetches it: no token
    "$CURL_BIN" --silent --show-error --location --max-redirs 5 \
        --output "$2" --write-out '%{http_code}' "$1"
}

# --- The moving-pointer release ----------------------------------------------
release_json="$tmp_dir/release.json"
status="$(gh_get "${api_base}/releases/tags/${UPDATE_LATEST_TAG}" "$release_json")" || \
    die "GitHub release lookup request failed"
case "$status" in
    200)
        release_id="$(jq -er '.id' "$release_json")" || die "the update-latest release has no id"
        printf 'Update slot release %s exists (id %s)\n' "$UPDATE_LATEST_TAG" "$release_id" ;;
    404)
        # First promotion, or the slot was orphaned into a draft (lookups do
        # not find drafts). `update-latest` exists only on GitHub, so the
        # GitLab push mirror must keep keep_divergent_refs=true or it deletes
        # the tag and the release reverts to an unreadable draft. If duplicate
        # drafts appear, check that setting first.
        #
        # Created at the released commit (already mirrored); the tag never
        # moves because clients read the assets by fixed URL. prerelease and
        # make_latest:"false" keep it from ever being GitHub's latest release.
        body="$tmp_dir/create.json"
        jq -n --arg tag "$UPDATE_LATEST_TAG" --arg sha "$SOURCE_SHA" \
            '{tag_name:$tag, target_commitish:$sha,
              name:"Update manifest (moving pointer, not a release)",
              body:"This slot carries the CURRENT signed update manifest so installed clients can check for updates when the canonical GitLab host is unreachable. Its two assets are replaced on every release; nothing else here is meaningful. Downloads are listed on the versioned releases. The manifest is signed on GitLab with a key that exists nowhere near GitHub, and every client verifies that signature.",
              draft:false, prerelease:true, make_latest:"false"}' >"$body"
        response="$tmp_dir/create-response.json"
        status="$("$CURL_BIN" --silent --show-error --config "$auth_conf" \
            --request POST --header 'Content-Type: application/json' \
            --data "@$body" --output "$response" --write-out '%{http_code}' \
            "${api_base}/releases")" || die "GitHub release creation request failed"
        [[ "$status" == 201 ]] || die "creating the ${UPDATE_LATEST_TAG} release returned HTTP $status"
        release_id="$(jq -er '.id' "$response")" || die "release creation returned no id"
        cp "$response" "$release_json"
        printf 'Created the update slot release %s (id %s) at %s\n' "$UPDATE_LATEST_TAG" "$release_id" "$SOURCE_SHA" ;;
    401|403) die "GitHub refused the release lookup (HTTP $status); check GITHUB_MIRROR_TOKEN" ;;
    *) die "GitHub release lookup returned HTTP $status" ;;
esac

existing_asset() { jq -c --arg n "$1" '[.assets[]? | select(.name == $n)] | first // empty' "$release_json"; }

# --- Replace the pair: signature first, manifest second ------------------------
#
# Same order as GitLab's promotion: a client racing the swap fails
# verification cleanly. GitHub cannot overwrite in place, so this is
# delete-then-upload; a client hitting the gap retries on its next check.
publish_slot_asset() { # local_file name
    local file="$1" name="$2" asset asset_id url dl
    asset="$(existing_asset "$name")"
    if [[ -n "$asset" ]]; then
        url="${UPDATE_MIRROR_DOWNLOAD_HOST}/${GITHUB_MIRROR_REPO}/releases/download/${UPDATE_LATEST_TAG}/${name}"
        dl="$tmp_dir/current-$name"
        if [[ "$(jq -r '.state // ""' <<<"$asset")" == uploaded ]] \
            && [[ "$(gh_get_anonymous "$url" "$dl")" == 200 ]] \
            && [[ "$(sha_of "$dl")" == "$(sha_of "$file")" ]]; then
            printf 'Update slot already carries these bytes: %s\n' "$name"
            return 0
        fi
        asset_id="$(jq -er '.id' <<<"$asset")" || die "existing asset $name has no id"
        status="$("$CURL_BIN" --silent --show-error --max-redirs 0 --config "$auth_conf" \
            --request DELETE --output /dev/null --write-out '%{http_code}' \
            "${api_base}/releases/assets/${asset_id}")" || die "asset delete request failed for $name"
        [[ "$status" == 204 ]] || die "deleting the previous $name from the update slot returned HTTP $status"
        printf 'Removed the previous %s from the update slot\n' "$name"
    fi
    status="$("$CURL_BIN" --silent --show-error --config "$auth_conf" \
        --request POST --header 'Content-Type: application/octet-stream' \
        --upload-file "$file" --output "$tmp_dir/upload.json" --write-out '%{http_code}' \
        "${upload_base}/releases/${release_id}/assets?name=${name}")" || die "upload request failed for $name"
    [[ "$status" == 201 ]] || die "uploading $name to the update slot returned HTTP $status"
    printf 'Uploaded %s to the update slot\n' "$name"
}
publish_slot_asset "$sig" "$UPDATE_SIG_NAME"
publish_slot_asset "$manifest" "$UPDATE_MANIFEST_NAME"

# --- Verify anonymously, at the exact URLs the client compiles in -------------
for name in "$UPDATE_SIG_NAME" "$UPDATE_MANIFEST_NAME"; do
    local_file="$ROOT/dist/$name"
    url="${UPDATE_MIRROR_DOWNLOAD_HOST}/${GITHUB_MIRROR_REPO}/releases/download/${UPDATE_LATEST_TAG}/${name}"
    dl="$tmp_dir/verify-$name"
    # Retry until the bytes match, not just until a 200: GitHub's asset
    # storage is eventually consistent and can briefly answer 404 after
    # upload, or 200 with the previous release's object from a stale CDN edge.
    status=""
    verified=0
    waited=0
    while :; do
        status="$(gh_get_anonymous "$url" "$dl")" \
            || die "anonymous read-back request failed for $name"
        # Compare the digest on every attempt, not once after the loop.
        if [[ "$status" == 200 ]] && [[ "$(sha_of "$dl")" == "$(sha_of "$local_file")" ]]; then
            verified=1
            break
        fi
        if (( waited >= UPDATE_SLOT_READBACK_WAIT_SECONDS )); then
            break
        fi
        printf 'Waiting for %s at the update slot: HTTP %s, %s (%ss of %ss)\n' \
            "$name" "$status" \
            "$([[ "$status" == 200 ]] && printf 'bytes do not match yet' || printf 'not readable yet')" \
            "$waited" "$UPDATE_SLOT_READBACK_WAIT_SECONDS"
        sleep "$UPDATE_SLOT_READBACK_POLL_SECONDS"
        waited=$(( waited + UPDATE_SLOT_READBACK_POLL_SECONDS ))
    done
    if [[ "$verified" != 1 ]]; then
        [[ "$status" == 200 ]] || die "anonymous read-back of $name from the update slot returned HTTP $status after ${UPDATE_SLOT_READBACK_WAIT_SECONDS}s"
        die "$name read back from the update slot does not match the promoted bytes after ${UPDATE_SLOT_READBACK_WAIT_SECONDS}s: the stored asset is wrong, or a stale CDN object never expired"
    fi
    printf 'Verified anonymously: %s (%s bytes)\n' "$name" "$(size_of "$local_file")"
done

jq -n --arg version "$manifest_version" --arg repo "$GITHUB_MIRROR_REPO" \
    --arg tag "$UPDATE_LATEST_TAG" \
    --arg manifest_url "${UPDATE_MIRROR_DOWNLOAD_HOST}/${GITHUB_MIRROR_REPO}/releases/download/${UPDATE_LATEST_TAG}/${UPDATE_MANIFEST_NAME}" \
    --arg manifest_sha256 "$(sha_of "$manifest")" \
    '{version:$version, repository:$repo, slot:$tag, manifest_url:$manifest_url, manifest_sha256:$manifest_sha256}' \
    >"$ROOT/dist/github-update-manifest-mirror.json"
printf 'The GitHub update slot now advertises %s; installed clients can check for updates from GitHub alone\n' "$manifest_version"
