#!/usr/bin/env bash
set -Eeuo pipefail

# Final pipeline action. Runs only after packages are published and verified.
#   attach-existing: add package links to the already-published release, without
#                    touching the tag, notes, source archives, or other links.
#   create:          create the tag and release from the exact resolved SHA with
#                    the package links attached, only if none exist yet.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
# shellcheck source=./scripts/gitlab-api.sh
source "$SCRIPT_DIR/gitlab-api.sh"

ROOT="$(project_dir)"
gitlab_api_init
release_contract_env

manifest="$ROOT/dist/manifest.json"
verification="$ROOT/dist/verification.json"
[[ -f "$manifest" ]] || die "publication manifest is missing"
[[ -f "$verification" ]] || die "verification record is missing; refusing to release"
[[ "$(jq -er '.source_sha' "$manifest")" == "$SOURCE_SHA" ]] || die "manifest source SHA mismatch"

tmp_dir="$(mktemp -d)"
cleanup() { rm -rf "$tmp_dir"; }
trap cleanup EXIT

# Build the release-links JSON array from the manifest (name, registry url,
# link_type=package, and a stable direct asset path).
links_array="$(jq -c '[.entries[] | {name:.asset_name, url:.registry_url,
    link_type:"package", direct_asset_path:.asset_path}]' "$manifest")"

# --- attach-existing: idempotently add links to the existing release ---
attach_existing() {
    local tag_json="$tmp_dir/tag.json" release_json="$tmp_dir/release.json" tag_sha
    api_json_get "/repository/tags/${RELEASE_TAG}" "$tag_json"
    tag_sha="$(jq -er '.commit.id' "$tag_json")"
    [[ "$tag_sha" == "$SOURCE_SHA" ]] || die "tag ${RELEASE_TAG} does not point at resolved source SHA"
    api_json_get "/releases/${RELEASE_TAG}" "$release_json"
    [[ "$(jq -er '.tag_name' "$release_json")" == "$RELEASE_TAG" ]] || die "release tag mismatch"

    local links_json="$tmp_dir/links.json"
    api_json_get "/releases/${RELEASE_TAG}/assets/links?per_page=100" "$links_json"

    while IFS= read -r link; do
        local name url same name_url url_name response status
        name="$(jq -r '.name' <<<"$link")"
        url="$(jq -r '.url' <<<"$link")"
        same="$(jq --arg n "$name" --arg u "$url" \
            '[.[] | select(.name == $n and .url == $u and .link_type == "package")] | length' "$links_json")"
        if [[ "$same" == 1 ]]; then
            printf 'Release link already present: %s\n' "$name"
            continue
        fi
        name_url="$(jq -r --arg n "$name" '[.[] | select(.name == $n) | .url] | first // ""' "$links_json")"
        url_name="$(jq -r --arg u "$url" '[.[] | select(.url == $u) | .name] | first // ""' "$links_json")"
        [[ -z "$name_url" ]] || die "release link name '$name' already exists with a different URL"
        [[ -z "$url_name" ]] || die "package URL already linked under a different name: $url_name"
        response="$tmp_dir/link-resp.json"
        status="$(api_request --request POST --output "$response" --write-out '%{http_code}' \
            --data-urlencode "name=$name" --data-urlencode "url=$url" \
            --data-urlencode 'link_type=package' \
            --data-urlencode "direct_asset_path=$(jq -r '.direct_asset_path' <<<"$link")" \
            "${API_ROOT}/releases/${RELEASE_TAG}/assets/links")" || die "release link request failed for $name"
        [[ "$status" == 201 ]] || die "release link creation for '$name' returned HTTP $status"
        printf 'Created release link: %s\n' "$name"
    done < <(jq -c '.[]' <<<"$links_array")
}

# --- create: make the tag + release atomically, links included ---
resolve_notes() {
    local out="$1"
    if [[ -n "${RELEASE_NOTES_B64:-}" ]]; then
        printf '%s' "$RELEASE_NOTES_B64" | base64 -d >"$out" 2>/dev/null || \
            die "RELEASE_NOTES_B64 is not valid base64"
    elif [[ -f "$ROOT/dist/release-notes.md" ]]; then
        cp "$ROOT/dist/release-notes.md" "$out"
    else
        die "release notes are required for RELEASE_ACTION=create (set RELEASE_NOTES_B64 or add docs/releases/${RELEASE_TAG}.md)"
    fi
    [[ -s "$out" ]] || die "resolved release notes are empty"
}

create_release() {
    # Refuse if the tag or release already exists.
    local status
    status="$(api_status_get "/repository/tags/${RELEASE_TAG}" "$tmp_dir/tag.json")"
    [[ "$status" == 404 ]] || die "tag ${RELEASE_TAG} already exists (HTTP $status); create mode requires a new tag"
    status="$(api_status_get "/releases/${RELEASE_TAG}" "$tmp_dir/release.json")"
    [[ "$status" == 404 ]] || die "release ${RELEASE_TAG} already exists (HTTP $status); create mode requires a new release"

    # Verify the commit is reachable from project 6 main before tagging it.
    local branch_json="$tmp_dir/main.json" main_sha
    api_json_get "/repository/commits/${SOURCE_SHA}?stats=false" "$tmp_dir/commit.json"
    status="$(api_status_get "/repository/commits/${SOURCE_SHA}/refs?type=branch&per_page=100" "$branch_json")"
    [[ "$status" == 200 ]] || die "could not confirm ${SOURCE_SHA} reachability (HTTP $status)"
    jq -e '[.[] | select(.name == "main")] | length > 0' "$branch_json" >/dev/null || \
        die "resolved source SHA is not reachable from project 6 main"

    local notes="$tmp_dir/notes.md" body="$tmp_dir/body.json" response="$tmp_dir/create-resp.json"
    resolve_notes "$notes"
    jq -n \
        --arg tag_name "$RELEASE_TAG" \
        --arg ref "$SOURCE_SHA" \
        --arg tag_message "Lightning ${RELEASE_VERSION}" \
        --arg name "Lightning ${RELEASE_VERSION}" \
        --rawfile description "$notes" \
        --argjson links "$links_array" \
        '{tag_name:$tag_name, ref:$ref, tag_message:$tag_message, name:$name,
          description:$description, assets:{links:$links}}' >"$body"

    status="$(api_request --request POST \
        --header 'Content-Type: application/json' \
        --data "@$body" --output "$response" --write-out '%{http_code}' \
        "${API_ROOT}/releases")" || die "release creation request failed"
    [[ "$status" == 201 ]] || {
        printf 'release creation returned HTTP %s\n' "$status" >&2
        die "release creation failed for ${RELEASE_TAG}"
    }
    printf 'Created release %s from %s\n' "$RELEASE_TAG" "$SOURCE_SHA"
}

case "$RELEASE_ACTION" in
    attach-existing) attach_existing ;;
    create) create_release ;;
    *) die "unknown RELEASE_ACTION: $RELEASE_ACTION" ;;
esac

# --- Post-conditions: verify the release, tag peel, and every link ---
final_release="$tmp_dir/final-release.json"
api_json_get "/releases/${RELEASE_TAG}" "$final_release"
[[ "$(jq -er '.tag_name' "$final_release")" == "$RELEASE_TAG" ]] || die "release verification failed"

final_tag="$tmp_dir/final-tag.json"
api_json_get "/repository/tags/${RELEASE_TAG}" "$final_tag"
[[ "$(jq -er '.commit.id' "$final_tag")" == "$SOURCE_SHA" ]] || die "tag ${RELEASE_TAG} does not peel to ${SOURCE_SHA}"

# Source archives must remain present.
jq -e '.assets.sources | length > 0' "$final_release" >/dev/null || \
    die "release ${RELEASE_TAG} has no source archives"

# Every manifest package must be a package link on the release.
final_links="$tmp_dir/final-links.json"
api_json_get "/releases/${RELEASE_TAG}/assets/links?per_page=100" "$final_links"
while IFS= read -r row; do
    name="$(jq -r '.asset_name' <<<"$row")"
    url="$(jq -r '.registry_url' <<<"$row")"
    jq -e --arg n "$name" --arg u "$url" \
        '[.[] | select(.name == $n and .url == $u and .link_type == "package")] | length == 1' \
        "$final_links" >/dev/null || die "expected package link missing after release: $name"
done < <(jq -c '.entries[]' "$manifest")

printf 'Release %s finalized with %s package link(s); source archives intact\n' \
    "$RELEASE_TAG" "$(jq '.entries | length' "$manifest")"
