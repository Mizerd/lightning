#!/usr/bin/env bash
set -Eeuo pipefail

# Upload exactly the manifest's package files to project 6's Generic Package
# Registry. Immutable: an identical existing file is accepted; a different file
# at the same path fails; a released file is never overwritten or deleted.
# Partial failures roll back only files this pipeline proved absent, so a retry
# converges safely.

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
[[ "$(jq -er '.source_sha' "$manifest")" == "$SOURCE_SHA" ]] || die "manifest source SHA mismatch"
[[ "$(jq -er '.version' "$manifest")" == "$PACKAGE_VERSION" ]] || die "manifest version mismatch"
count="$(jq '.entries | length' "$manifest")"
(( count >= 1 )) || die "manifest lists no files to publish"

tmp_dir="$(mktemp -d)"
uploaded_list="$tmp_dir/uploaded.list"
: >"$uploaded_list"
cleanup() { rm -rf "$tmp_dir"; }
trap cleanup EXIT

rollback() {
    # Remove only files this pipeline actually uploaded this run.
    local name
    while IFS= read -r name; do
        [[ -n "$name" ]] || continue
        delete_new_package_file "$name" "$PACKAGE_VERSION" || \
            printf 'warning: rollback could not remove %s\n' "$name" >&2
    done <"$uploaded_list"
}

# --- Phase 1: preflight all destinations (no writes) ---
declare -a entry_files entry_urls entry_names entry_states
while IFS= read -r row; do
    local_path="$(jq -r '.local_path' <<<"$row")"
    filename="$(jq -r '.filename' <<<"$row")"
    url="$(jq -r '.registry_url' <<<"$row")"
    expected_sha="$(jq -r '.sha256' <<<"$row")"
    file="$ROOT/$local_path"
    [[ -f "$file" ]] || die "manifest file is missing on disk: $filename"
    actual_sha="$(sha256sum "$file" | cut -d' ' -f1)"
    [[ "$actual_sha" == "$expected_sha" ]] || \
        die "manifest checksum mismatch for $filename (validated bytes differ)"
    remote="$tmp_dir/remote-$filename"
    status="$(api_request --output "$remote" --write-out '%{http_code}' "$url")" || \
        die "package preflight request failed for $filename"
    case "$status" in
        200)
            cmp -s "$file" "$remote" || die "immutable package conflict for $filename"
            printf 'Already published and identical: %s\n' "$filename"
            state=identical ;;
        404)
            printf 'Not yet published: %s\n' "$filename"
            state=missing ;;
        *) die "package preflight returned HTTP $status for $filename" ;;
    esac
    entry_files+=("$file")
    entry_urls+=("$url")
    entry_names+=("$filename")
    entry_states+=("$state")
done < <(jq -c '.entries[]' "$manifest")

# --- Phase 2: upload every missing file ---
for i in "${!entry_files[@]}"; do
    [[ "${entry_states[$i]}" == missing ]] || continue
    file="${entry_files[$i]}"; url="${entry_urls[$i]}"; name="${entry_names[$i]}"
    if ! status="$(api_request --request PUT --upload-file "$file" \
        --output "$tmp_dir/upload-$name.json" --write-out '%{http_code}' "$url")"; then
        rollback
        die "upload request failed for $name; newly uploaded files were rolled back"
    fi
    if [[ "$status" != 201 ]]; then
        # A 201-less response may still have created the file; record it so
        # rollback can remove it, then roll back everything from this run.
        printf '%s\n' "$name" >>"$uploaded_list"
        rollback
        die "upload of $name returned HTTP $status; newly uploaded files were rolled back"
    fi
    printf '%s\n' "$name" >>"$uploaded_list"
    printf 'Uploaded %s\n' "$name"
done

# --- Phase 3: publication record for downstream jobs ---
jq \
    --arg auth_method "$AUTH_METHOD" \
    '{package_name, version, source_sha, release_tag, release_action,
      target_project_id, authentication:$auth_method, entries}' \
    "$manifest" >"$ROOT/dist/publication.json"

printf 'Published %s package file(s) to Lightning project 6\n' "$count"
