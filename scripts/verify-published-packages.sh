#!/usr/bin/env bash
set -Eeuo pipefail

# Independently confirm that project 6's Package Registry holds exactly the
# manifest's files for this version and nothing else, and that each file
# downloads with the expected checksum. Runs as its own stage between publish
# and release; the release job depends on it.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
# shellcheck source=./scripts/gitlab-api.sh
source "$SCRIPT_DIR/gitlab-api.sh"

ROOT="$(project_dir)"
gitlab_api_init
release_contract_env

manifest="$ROOT/dist/manifest.json"
[[ -f "$manifest" ]] || die "publication manifest is missing"
expected_count="$(jq '.entries | length' "$manifest")"

tmp_dir="$(mktemp -d)"
cleanup() { rm -rf "$tmp_dir"; }
trap cleanup EXIT

# The package must exist as a generic package named lightning at this version.
packages_json="$tmp_dir/packages.json"
api_json_get "/packages?package_name=${PACKAGE_NAME}&package_version=${PACKAGE_VERSION}&package_type=generic&per_page=100" "$packages_json"
package_id="$(jq -er --arg n "$PACKAGE_NAME" --arg v "$PACKAGE_VERSION" \
    'map(select(.name == $n and .version == $v and .package_type == "generic")) | first | .id' "$packages_json")" \
    || die "generic package ${PACKAGE_NAME} ${PACKAGE_VERSION} not found in project 6"
package_status="$(jq -r --arg n "$PACKAGE_NAME" --arg v "$PACKAGE_VERSION" \
    'map(select(.name == $n and .version == $v and .package_type == "generic")) | first | .status // "default"' "$packages_json")"
[[ "$package_status" == default || "$package_status" == hidden ]] || \
    die "package ${PACKAGE_NAME} ${PACKAGE_VERSION} is not in a usable state: $package_status"

# Enumerate stored files. Every manifest file must be present with a matching
# size, and no unexpected file may exist for this version.
files_json="$tmp_dir/files.json"
api_json_get "/packages/${package_id}/package_files?per_page=100" "$files_json"

mapfile -t stored_names < <(jq -r '.[].file_name' "$files_json" | sort -u)
mapfile -t manifest_names < <(jq -r '.entries[].filename' "$manifest" | sort)

for name in "${stored_names[@]}"; do
    if ! jq -e --arg n "$name" '.entries[] | select(.filename == $n)' "$manifest" >/dev/null; then
        die "unexpected file in registry version ${PACKAGE_VERSION}: $name"
    fi
done
(( ${#stored_names[@]} == expected_count )) || \
    die "registry holds ${#stored_names[@]} file(s); manifest expects ${expected_count}"

while IFS= read -r row; do
    filename="$(jq -r '.filename' <<<"$row")"
    expected_sha="$(jq -r '.sha256' <<<"$row")"
    expected_size="$(jq -r '.size' <<<"$row")"
    url="$(jq -r '.registry_url' <<<"$row")"

    # Stored file metadata: size must match; verify SHA-256 when GitLab exposes it.
    stored_size="$(jq -r --arg n "$filename" '.[] | select(.file_name == $n) | .size // empty' "$files_json" | head -1)"
    [[ -n "$stored_size" ]] || die "registry did not report a size for $filename"
    [[ "$stored_size" == "$expected_size" ]] || \
        die "registry size mismatch for $filename (stored $stored_size, expected $expected_size)"
    stored_sha="$(jq -r --arg n "$filename" '.[] | select(.file_name == $n) | .file_sha256 // empty' "$files_json" | head -1)"
    if [[ -n "$stored_sha" ]]; then
        [[ "$stored_sha" == "$expected_sha" ]] || \
            die "registry SHA-256 mismatch for $filename"
    fi

    # Download the file and confirm the bytes and checksum end to end.
    dl="$tmp_dir/dl-$filename"
    status="$(api_request --output "$dl" --write-out '%{http_code}' "$url")" || \
        die "download request failed for $filename"
    [[ "$status" == 200 ]] || die "download of $filename returned HTTP $status"
    actual_sha="$(sha256sum "$dl" | cut -d' ' -f1)"
    [[ "$actual_sha" == "$expected_sha" ]] || die "downloaded $filename has the wrong checksum"
    printf 'Verified downloadable package: %s (%s bytes)\n' "$filename" "$expected_size"
done < <(jq -c '.entries[]' "$manifest")

# Record verification outcome for the release job and diagnostics.
jq \
    --arg package_id "$package_id" \
    '{package_name, version, source_sha, release_tag, release_action,
      target_project_id, package_id:$package_id,
      verified_files:[.entries[].filename]}' \
    "$manifest" >"$ROOT/dist/verification.json"

printf 'Registry verification passed: %s file(s) for %s %s\n' \
    "$expected_count" "$PACKAGE_NAME" "$PACKAGE_VERSION"
