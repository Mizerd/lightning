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

deb="$ROOT/dist/lightning_${PACKAGE_VERSION}_amd64.deb"
rpm="$ROOT/dist/lightning-${PACKAGE_VERSION}-1.x86_64.rpm"
[[ -f "$deb" && -f "$rpm" ]] || die "both final release packages are required"
mapfile -t packages < <(find "$ROOT/dist" -maxdepth 1 -type f \( -name '*.deb' -o -name '*.rpm' \) -print)
(( ${#packages[@]} == 2 )) || die "publication input must contain exactly one DEB and one RPM"

registry_base="${API_ROOT}/packages/generic/lightning/${PACKAGE_VERSION}"
tmp_dir="$(mktemp -d)"
cleanup() { rm -rf "$tmp_dir"; }
trap cleanup EXIT

preflight_file() {
    local file="$1" name status remote
    name="$(basename "$file")"
    remote="$tmp_dir/$name"
    status="$(api_request --output "$remote" --write-out '%{http_code}' "$registry_base/$name")" || \
        die "package preflight request failed for $name"
    case "$status" in
        200)
            if ! cmp -s "$file" "$remote"; then
                die "immutable package conflict for $name"
            fi
            printf 'Package already published and identical: %s\n' "$name"
            printf 'identical\n' >"$tmp_dir/$name.state"
            ;;
        404)
            printf 'Package is not yet published: %s\n' "$name"
            printf 'missing\n' >"$tmp_dir/$name.state"
            ;;
        *) die "package preflight returned HTTP $status for $name" ;;
    esac
}

upload_file() {
    local file="$1" name status
    name="$(basename "$file")"
    [[ "$(<"$tmp_dir/$name.state")" == missing ]] || return 0
    if ! status="$(api_request --request PUT --upload-file "$file" --output "$tmp_dir/upload.json" \
        --write-out '%{http_code}' "$registry_base/$name")"; then
        return 1
    fi
    [[ "$status" == 201 ]] || return 1
    printf 'Uploaded %s\n' "$name"
    printf 'uploaded\n' >"$tmp_dir/$name.state"
}

# Check both immutable destinations and the matching release before changing
# registry state. Uploads happen only after the complete preflight succeeds.
preflight_file "$deb"
preflight_file "$rpm"
if ! upload_file "$deb"; then
    deb_name="$(basename "$deb")"
    if [[ "$(<"$tmp_dir/$deb_name.state")" == missing ]]; then
        delete_new_package_file "$deb_name" || true
    fi
    die "DEB upload failed; any newly uploaded package file was rolled back"
fi
if ! upload_file "$rpm"; then
    rpm_name="$(basename "$rpm")"
    deb_name="$(basename "$deb")"
    # A failed request may have reached GitLab before the connection failed.
    # Remove only files that were absent during this pipeline's preflight.
    if [[ "$(<"$tmp_dir/$rpm_name.state")" == missing ]]; then
        delete_new_package_file "$rpm_name" || true
    fi
    if [[ "$(<"$tmp_dir/$deb_name.state")" == uploaded ]]; then
        delete_new_package_file "$deb_name" || \
            die "RPM upload failed and automatic DEB rollback also failed"
    fi
    die "RPM upload failed; any newly uploaded package files were rolled back"
fi

jq -n \
    --arg tag "$SOURCE_REF" \
    --arg version "$PACKAGE_VERSION" \
    --arg source_sha "$SOURCE_SHA" \
    --arg auth_method "$AUTH_METHOD" \
    --arg deb_name "$(basename "$deb")" \
    --arg deb_url "$registry_base/$(basename "$deb")" \
    --arg deb_sha "$(sha256sum "$deb" | cut -d' ' -f1)" \
    --arg rpm_name "$(basename "$rpm")" \
    --arg rpm_url "$registry_base/$(basename "$rpm")" \
    --arg rpm_sha "$(sha256sum "$rpm" | cut -d' ' -f1)" \
    '{tag:$tag,version:$version,source_sha:$source_sha,authentication:$auth_method,
      files:[{format:"deb",name:$deb_name,url:$deb_url,sha256:$deb_sha},
             {format:"rpm",name:$rpm_name,url:$rpm_url,sha256:$rpm_sha}]}' \
    >"$ROOT/dist/publication.json"

printf 'Published exactly two package files to Lightning project 6\n'
