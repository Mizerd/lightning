#!/usr/bin/env bash
set -Eeuo pipefail

# Publish the SIGNED update-manifest pair to the GitHub mirror's fixed
# `update-latest` release slot, after GitLab's `latest` slot has been written.
#
# WHY THIS EXISTS. Every installed Lightning compiles in a fallback address
# for its update metadata — `…/releases/download/update-latest/…` on the
# mirror (src/update/UpdateEndpoints.cpp) — and reads it only when the
# canonical GitLab host does not answer. Until 2026-09-03 no pipeline job
# ever WROTE that slot, so the fallback was inert: a GitLab outage, a power
# cut, a dead server, and no installed client could learn of an update or
# find a download, even though every package byte was already on GitHub.
# With this job the whole update path — manifest, signature, artifacts — is
# reachable from GitHub alone, verified by the same Ed25519 key GitLab holds.
#
# WHAT IT IS NOT. GitHub is still not a release authority: the manifest is
# signed on GitLab, the client verifies that signature, and this job only
# copies bytes that GitLab has ALREADY promoted (dist/update-publication.json
# is the proof, and the manifest's SHA-256 must match it). The release at
# `update-latest` is a MOVING POINTER by design — its two assets are replaced
# on every promotion — and it is marked so GitHub never treats it as the
# repository's "latest release" (make_latest=false), which the website reads.
#
# Runs with allow_failure in CI: a GitHub hiccup must never fail a release
# that GitLab has completed. It is idempotent, so a retry converges.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
# shellcheck source=./scripts/update-lib.sh
source "$SCRIPT_DIR/update-lib.sh"

ROOT="${CI_PROJECT_DIR:-$(cd -- "$SCRIPT_DIR/.." && pwd)}"
CURL_BIN="${CURL_BIN:-curl}"
# The slot name is a constant from update-lib.sh (see there for why); the
# client's address is compiled in, so a different value here would write a
# slot nothing reads while every check below still passed.
[[ "$UPDATE_LATEST_TAG" == update-latest ]] || die "UPDATE_LATEST_TAG must be update-latest"

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
# PROOF OF PROMOTION. This job mirrors what GitLab's latest slot holds; the
# publication record is written only after that promotion verified, and it
# names the exact bytes.
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
        # First promotion ever: the slot is created at the RELEASED commit,
        # which the tag mirror has already delivered (mirror-release-to-github
        # proved it), so target_commitish names an existing commit and GitHub
        # creates the pointer tag there. The tag never needs to move: clients
        # read the two assets by a fixed URL and no release metadata.
        # prerelease:true AND make_latest:"false": GitHub's /releases/latest
        # never returns a prerelease, so even if the newest versioned release
        # were ever deleted and GitHub fell back to date order, this slot --
        # created AFTER that release in every pipeline -- could not become the
        # "latest release" the website reads.
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

# --- Replace the pair: signature FIRST, manifest second ------------------------
#
# The same order GitLab's promotion uses: a client that races the swap sees a
# new signature with the old manifest and fails verification cleanly, rather
# than a new manifest with no matching signature. Replacement is delete-then-
# upload because GitHub cannot overwrite an asset in place; the gap is
# seconds and a client that hits it simply retries on its next check.
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
    status="$(gh_get_anonymous "$url" "$dl")" || die "anonymous read-back request failed for $name"
    [[ "$status" == 200 ]] || die "anonymous read-back of $name from the update slot returned HTTP $status"
    [[ "$(sha_of "$dl")" == "$(sha_of "$local_file")" ]] || \
        die "$name read back from the update slot does not match the promoted bytes"
    printf 'Verified anonymously: %s (%s bytes)\n' "$name" "$(size_of "$local_file")"
done

jq -n --arg version "$manifest_version" --arg repo "$GITHUB_MIRROR_REPO" \
    --arg tag "$UPDATE_LATEST_TAG" \
    --arg manifest_url "${UPDATE_MIRROR_DOWNLOAD_HOST}/${GITHUB_MIRROR_REPO}/releases/download/${UPDATE_LATEST_TAG}/${UPDATE_MANIFEST_NAME}" \
    --arg manifest_sha256 "$(sha_of "$manifest")" \
    '{version:$version, repository:$repo, slot:$tag, manifest_url:$manifest_url, manifest_sha256:$manifest_sha256}' \
    >"$ROOT/dist/github-update-manifest-mirror.json"
printf 'The GitHub update slot now advertises %s; installed clients can check for updates from GitHub alone\n' "$manifest_version"
