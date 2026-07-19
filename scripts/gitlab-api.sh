#!/usr/bin/env bash

# Sourced by publication, verification, and release scripts. Do not enable
# command tracing here: request headers carry short-lived credentials.
[[ -n "${BASH_VERSION:-}" ]] || { printf 'error: bash is required\n' >&2; exit 1; }

gitlab_api_init() {
    require_var CI_API_V4_URL
    # Publication and release writes are hard-wired to Lightning project 6; a
    # settable variable must never be able to redirect them elsewhere.
    : "${TARGET_PROJECT_ID:=6}"
    : "${LIGHTNING_PROJECT_ID:=$TARGET_PROJECT_ID}"
    [[ "$TARGET_PROJECT_ID" == 6 ]] || die "publication target must be Lightning project 6"
    [[ "$LIGHTNING_PROJECT_ID" == 6 ]] || die "publication target must be Lightning project 6"
    CURL_BIN="${CURL_BIN:-curl}"
    if [[ -n "${CI_JOB_TOKEN:-}" ]]; then
        AUTH_HEADER_NAME="JOB-TOKEN"
        AUTH_HEADER_VALUE="$CI_JOB_TOKEN"
        AUTH_METHOD="CI_JOB_TOKEN"
    elif [[ -n "${LIGHTNING_PUBLISH_TOKEN:-}" ]]; then
        AUTH_HEADER_NAME="PRIVATE-TOKEN"
        AUTH_HEADER_VALUE="$LIGHTNING_PUBLISH_TOKEN"
        # Consumed by callers after this library is sourced.
        # shellcheck disable=SC2034
        AUTH_METHOD="project access token"
    else
        die "CI_JOB_TOKEN is unavailable and LIGHTNING_PUBLISH_TOKEN is not configured"
    fi
    API_ROOT="${CI_API_V4_URL%/}/projects/${TARGET_PROJECT_ID}"
    PACKAGE_NAME="${PACKAGE_NAME:-lightning}"
}

api_request() {
    "$CURL_BIN" --silent --show-error --location \
        --header "${AUTH_HEADER_NAME}: ${AUTH_HEADER_VALUE}" "$@"
}

# GET a JSON document, requiring HTTP 200.
api_json_get() {
    local path="$1" output="$2" status
    status="$(api_request --output "$output" --write-out '%{http_code}' "${API_ROOT}${path}")" || \
        die "GitLab API request failed for ${path}"
    [[ "$status" == 200 ]] || die "GitLab API returned HTTP ${status} for ${path}"
}

# GET a path and echo only the HTTP status, writing the body to $2.
api_status_get() {
    local path="$1" output="${2:-/dev/null}" status
    status="$(api_request --output "$output" --write-out '%{http_code}' "${API_ROOT}${path}")" || \
        die "GitLab API request failed for ${path}"
    printf '%s' "$status"
}

# Validate the environment shared by every publishing job. Sets PACKAGE_VERSION
# and RELEASE_TAG. Action-specific tag/release checks live in finalize-release.
release_contract_env() {
    load_versions
    require_var PUBLISH_PACKAGES
    [[ "$PUBLISH_PACKAGES" == true ]] || die "publication requires PUBLISH_PACKAGES=true"
    require_var RELEASE_ACTION
    case "$RELEASE_ACTION" in
        create|attach-existing) ;;
        *) die "RELEASE_ACTION must be 'create' or 'attach-existing'" ;;
    esac
    require_var RELEASE_VERSION
    [[ "$RELEASE_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || \
        die "RELEASE_VERSION must be X.Y.Z"
    PACKAGE_VERSION="$RELEASE_VERSION"
    RELEASE_TAG="v${RELEASE_VERSION}"
    if [[ -n "${CI:-}" ]]; then
        [[ -n "${CI_COMMIT_BRANCH:-}" && "$CI_COMMIT_BRANCH" == "${CI_DEFAULT_BRANCH:-}" ]] || \
            die "publication is allowed only from the packaging project's default branch"
    fi
    [[ "$SOURCE_SHA" =~ ^[0-9a-f]{40}$ ]] || die "resolved source SHA is invalid"
    [[ "$LOGICAL_VERSION" == "$PACKAGE_VERSION" ]] || \
        die "resolved package version ${LOGICAL_VERSION} differs from RELEASE_VERSION ${PACKAGE_VERSION}"
    [[ "${IS_EXACT_TAG:-false}" == true ]] || die "publication requires a clean resolved version"
}

# Remove one just-uploaded generic package file (rollback helper). Never used to
# delete an already-released, verified file.
delete_new_package_file() {
    local name="$1" version="${2:-$PACKAGE_VERSION}" packages_json files_json package_id file_id status
    packages_json="$(mktemp)"
    files_json="$(mktemp)"
    status="$(api_request --output "$packages_json" --write-out '%{http_code}' \
        "${API_ROOT}/packages?package_name=${PACKAGE_NAME}&package_version=${version}&package_type=generic")" || return 1
    [[ "$status" == 200 ]] || return 1
    package_id="$(jq -er --arg n "$PACKAGE_NAME" --arg v "$version" \
        'map(select(.name == $n and .version == $v and .package_type == "generic")) | first | .id' "$packages_json")" || return 1
    status="$(api_request --output "$files_json" --write-out '%{http_code}' \
        "${API_ROOT}/packages/${package_id}/package_files")" || return 1
    [[ "$status" == 200 ]] || return 1
    file_id="$(jq -er --arg name "$name" '.[] | select(.file_name == $name) | .id' "$files_json")" || return 0
    status="$(api_request --request DELETE --output /dev/null --write-out '%{http_code}' \
        "${API_ROOT}/packages/${package_id}/package_files/${file_id}")" || return 1
    [[ "$status" == 204 ]]
}
