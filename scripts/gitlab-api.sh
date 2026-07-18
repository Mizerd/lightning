#!/usr/bin/env bash

# Sourced by publication scripts. Do not enable command tracing here: request
# headers contain short-lived credentials.
[[ -n "${BASH_VERSION:-}" ]] || { printf 'error: bash is required\n' >&2; exit 1; }

gitlab_api_init() {
    require_var CI_API_V4_URL
    : "${LIGHTNING_PROJECT_ID:=6}"
    [[ "$LIGHTNING_PROJECT_ID" == 6 ]] || die "publication target must be Lightning project 6"
    CURL_BIN="${CURL_BIN:-curl}"
    if [[ -n "${CI_JOB_TOKEN:-}" ]]; then
        AUTH_HEADER_NAME="JOB-TOKEN"
        AUTH_HEADER_VALUE="$CI_JOB_TOKEN"
        AUTH_METHOD="CI_JOB_TOKEN"
    elif [[ -n "${LIGHTNING_PUBLISH_TOKEN:-}" ]]; then
        AUTH_HEADER_NAME="PRIVATE-TOKEN"
        AUTH_HEADER_VALUE="$LIGHTNING_PUBLISH_TOKEN"
        # Used by the calling publication script after this library is sourced.
        # shellcheck disable=SC2034
        AUTH_METHOD="project access token"
    else
        die "CI_JOB_TOKEN is unavailable and LIGHTNING_PUBLISH_TOKEN is not configured"
    fi
    API_ROOT="${CI_API_V4_URL%/}/projects/${LIGHTNING_PROJECT_ID}"
}

api_request() {
    "$CURL_BIN" --silent --show-error --location \
        --header "${AUTH_HEADER_NAME}: ${AUTH_HEADER_VALUE}" "$@"
}

api_json_get() {
    local path="$1" output="$2" status
    status="$(api_request --output "$output" --write-out '%{http_code}' "${API_ROOT}${path}")" || \
        die "GitLab API request failed for ${path}"
    [[ "$status" == 200 ]] || die "GitLab API returned HTTP ${status} for ${path}"
}

validate_release_contract() {
    load_versions
    require_var SOURCE_REF
    require_var PUBLISH_PACKAGES
    [[ "$PUBLISH_PACKAGES" == true ]] || die "publication requires PUBLISH_PACKAGES=true"
    if [[ -n "${CI:-}" ]]; then
        [[ -n "${CI_COMMIT_BRANCH:-}" && "$CI_COMMIT_BRANCH" == "${CI_DEFAULT_BRANCH:-}" ]] || \
            die "publication is allowed only from the packaging project's default branch"
    fi
    [[ "$SOURCE_REF" =~ ^v([0-9]+\.[0-9]+\.[0-9]+)$ ]] || \
        die "publication requires SOURCE_REF=vX.Y.Z"
    PACKAGE_VERSION="${BASH_REMATCH[1]}"
    [[ "$IS_EXACT_TAG" == true ]] || die "publication requires an exact source tag"
    [[ "$LOGICAL_VERSION" == "$PACKAGE_VERSION" ]] || die "tag and package versions differ"
    [[ "$SOURCE_SHA" =~ ^[0-9a-f]{40}$ ]] || die "resolved source SHA is invalid"

    local root tag_json release_json tag_sha
    root="$(project_dir)"
    tag_json="$root/dist/tag.json"
    release_json="$root/dist/release.json"
    api_json_get "/repository/tags/${SOURCE_REF}" "$tag_json"
    tag_sha="$(jq -er '.commit.id' "$tag_json")"
    [[ "$tag_sha" == "$SOURCE_SHA" ]] || die "project 6 tag target differs from resolved source SHA"
    api_json_get "/releases/${SOURCE_REF}" "$release_json"
}

delete_new_package_file() {
    local name="$1" packages_json files_json package_id file_id status
    packages_json="$(mktemp)"
    files_json="$(mktemp)"
    status="$(api_request --output "$packages_json" --write-out '%{http_code}' \
        "${API_ROOT}/packages?package_name=lightning&package_version=${PACKAGE_VERSION}&package_type=generic")" || return 1
    [[ "$status" == 200 ]] || return 1
    package_id="$(jq -er 'map(select(.name == "lightning" and .package_type == "generic")) | first | .id' "$packages_json")" || return 1
    status="$(api_request --output "$files_json" --write-out '%{http_code}' \
        "${API_ROOT}/packages/${package_id}/package_files")" || return 1
    [[ "$status" == 200 ]] || return 1
    file_id="$(jq -er --arg name "$name" '.[] | select(.file_name == $name) | .id' "$files_json")" || return 0
    status="$(api_request --request DELETE --output /dev/null --write-out '%{http_code}' \
        "${API_ROOT}/packages/${package_id}/package_files/${file_id}")" || return 1
    [[ "$status" == 204 ]]
}
