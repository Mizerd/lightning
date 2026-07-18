#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
# shellcheck source=./scripts/gitlab-api.sh
source "$SCRIPT_DIR/gitlab-api.sh"
ROOT="$(project_dir)"
gitlab_api_init
validate_release_contract

publication="$ROOT/dist/publication.json"
[[ -f "$publication" ]] || die "publication metadata is missing"
[[ "$(jq -er '.source_sha' "$publication")" == "$SOURCE_SHA" ]] || die "publication source SHA mismatch"
[[ "$(jq -er '.files | length' "$publication")" == 2 ]] || die "publication metadata must contain two files"

links_json="$ROOT/dist/release-links.json"
api_json_get "/releases/${SOURCE_REF}/assets/links?per_page=100" "$links_json"

ensure_link() {
    local format="$1" name="$2" url existing_same name_url url_name response status
    url="$(jq -er --arg format "$format" '.files[] | select(.format == $format) | .url' "$publication")"
    existing_same="$(jq -r --arg name "$name" --arg url "$url" \
        '[.[] | select(.name == $name and .url == $url and .link_type == "package")] | length' "$links_json")"
    if [[ "$existing_same" == 1 ]]; then
        printf 'Release link already exists: %s\n' "$name"
        return 0
    fi
    name_url="$(jq -r --arg name "$name" '[.[] | select(.name == $name) | .url] | first // ""' "$links_json")"
    url_name="$(jq -r --arg url "$url" '[.[] | select(.url == $url) | .name] | first // ""' "$links_json")"
    [[ -z "$name_url" ]] || die "release link name exists with a different URL: $name"
    [[ -z "$url_name" ]] || die "package URL is already linked under a different name: $url_name"
    response="$ROOT/dist/link-${format}.json"
    status="$(api_request --request POST --output "$response" --write-out '%{http_code}' \
        --data-urlencode "name=$name" --data-urlencode "url=$url" \
        --data-urlencode 'link_type=package' \
        "${API_ROOT}/releases/${SOURCE_REF}/assets/links")" || die "release link request failed"
    [[ "$status" == 201 ]] || die "release link creation returned HTTP $status"
    printf 'Created release link: %s\n' "$name"
}

ensure_link deb "Lightning ${PACKAGE_VERSION} Debian amd64"
ensure_link rpm "Lightning ${PACKAGE_VERSION} RPM x86_64"
printf 'Release %s has the two package registry asset links\n' "$SOURCE_REF"
