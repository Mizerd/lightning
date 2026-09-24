#!/usr/bin/env bash
set -Eeuo pipefail

# Upload the signed update manifest to project 6's Generic Package Registry
# under TWO paths (UPDATE-SPEC §6):
#
#   packages/generic/lightning-update/<version>/   immutable per-release copy
#   packages/generic/lightning-update/latest/      the stable slot clients poll
#
# The versioned copy is uploaded and verified before `latest` moves, so the
# pointer never advertises bytes that may not exist. `latest` is the only
# mutable path; everything else, including the versioned copy, is immutable.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
# shellcheck source=./scripts/gitlab-api.sh
source "$SCRIPT_DIR/gitlab-api.sh"
# shellcheck source=./scripts/update-lib.sh
source "$SCRIPT_DIR/update-lib.sh"

ROOT="$(project_dir)"
gitlab_api_init
release_contract_env

manifest="$ROOT/dist/${UPDATE_MANIFEST_NAME}"
sig="$ROOT/dist/${UPDATE_SIG_NAME}"
[[ -f "$ROOT/dist/verification.json" ]] || \
    die "verification record is missing; refusing to publish an update manifest"
[[ -f "$manifest" ]] || die "update manifest is missing; run generate-update-manifest.sh first"
[[ -f "$sig" ]] || die "update manifest signature is missing; run sign-update-manifest.sh first"

# Reject a stale manifest from a previous pipeline.
[[ "$(jq -er '.version' "$manifest")" == "$PACKAGE_VERSION" ]] || die "update manifest version mismatch"
[[ "$(jq -er '.tag' "$manifest")" == "$RELEASE_TAG" ]] || die "update manifest tag mismatch"
[[ "$(jq -er '.schema' "$manifest")" == 1 ]] || die "update manifest is not schema 1"
[[ "$(jq -er '.alg' "$sig")" == "$UPDATE_SIG_ALG" ]] || die "signature envelope declares an unexpected algorithm"
jq -e '.key_id | type == "string" and length > 0' "$sig" >/dev/null || \
    die "signature envelope has no key id"
jq -e '.sig | type == "string" and length > 0' "$sig" >/dev/null || \
    die "signature envelope has no signature"

tmp_dir="$(mktemp -d)"
cleanup() { rm -rf "$tmp_dir"; }
trap cleanup EXIT

version_base="${CANONICAL_API_ROOT}/packages/generic/${UPDATE_PACKAGE_NAME}/${PACKAGE_VERSION}"
latest_base="${CANONICAL_API_ROOT}/packages/generic/${UPDATE_PACKAGE_NAME}/${UPDATE_LATEST_SLOT}"

sha_of() { sha256sum "$1" | cut -d' ' -f1; }

# GET a published file and prove it is byte-identical to the local one.
verify_published() { # local_file url label
    local file="$1" url="$2" label="$3" dl status
    dl="$tmp_dir/verify-$(basename "$file").$RANDOM"
    status="$(api_request --output "$dl" --write-out '%{http_code}' "$(api_request_url "$url")")" || \
        die "read-back request failed for $label"
    [[ "$status" == 200 ]] || die "read-back of $label returned HTTP $status"
    cmp -s "$file" "$dl" || die "$label does not match the published bytes"
    [[ "$(sha_of "$file")" == "$(sha_of "$dl")" ]] || die "$label checksum mismatch after publication"
    printf 'Verified %s (%s bytes, sha256 %s)\n' "$label" "$(wc -c <"$dl" | tr -d ' ')" "$(sha_of "$dl")"
}

upload_file() { # local_file url label
    local file="$1" url="$2" label="$3" status
    status="$(api_request --request PUT --upload-file "$file" \
        --output "$tmp_dir/upload.json" --write-out '%{http_code}' "$(api_request_url "$url")")" || \
        die "upload request failed for $label"
    if [[ "$status" != 201 ]]; then
        printf 'upload of %s returned HTTP %s\n' "$label" "$status" >&2
        if [[ "$status" == 403 || "$status" == 400 ]]; then
            # Most likely cause for the mutable slot.
            printf 'hint: the "latest" slot requires generic-package duplicates to be permitted for %s on project 6\n' \
                "$UPDATE_PACKAGE_NAME" >&2
        fi
        die "upload of $label failed"
    fi
    printf 'Uploaded %s\n' "$label"
}

# --- Refresh mode --------------------------------------------------------------
#
# UPDATE_REFRESH_LATEST_ONLY=true re-promotes `latest` for the version already
# there, to renew the signed `expires` without a release; the immutable copy is
# untouched. Generate with the original UPDATE_RELEASED_AT and an explicit
# UPDATE_EXPIRES_AT, sign, then run this in refresh mode.
: "${UPDATE_REFRESH_LATEST_ONLY:=false}"
case "$UPDATE_REFRESH_LATEST_ONLY" in
    true|false) ;;
    *) die "UPDATE_REFRESH_LATEST_ONLY must be true or false" ;;
esac

# --- Phase 1: the immutable per-release copy ---------------------------------
#
# As in publish-packages.sh: identical existing bytes are accepted so retries
# converge; different bytes are a conflict.
publish_immutable() { # local_file url label
    local file="$1" url="$2" label="$3" remote status
    remote="$tmp_dir/remote-$(basename "$file")"
    status="$(api_request --output "$remote" --write-out '%{http_code}' "$(api_request_url "$url")")" || \
        die "preflight request failed for $label"
    case "$status" in
        200)
            cmp -s "$file" "$remote" || die "immutable conflict for $label (already published with different bytes)"
            printf 'Already published and identical: %s\n' "$label" ;;
        404)
            upload_file "$file" "$url" "$label" ;;
        *) die "preflight returned HTTP $status for $label" ;;
    esac
}

if [[ "$UPDATE_REFRESH_LATEST_ONLY" == true ]]; then
    printf 'Refresh mode: the immutable %s copy is left untouched; only the latest slot is re-promoted\n' "$PACKAGE_VERSION"
else
    publish_immutable "$manifest" "${version_base}/${UPDATE_MANIFEST_NAME}" "${PACKAGE_VERSION}/${UPDATE_MANIFEST_NAME}"
    publish_immutable "$sig" "${version_base}/${UPDATE_SIG_NAME}" "${PACKAGE_VERSION}/${UPDATE_SIG_NAME}"

    verify_published "$manifest" "${version_base}/${UPDATE_MANIFEST_NAME}" "${PACKAGE_VERSION}/${UPDATE_MANIFEST_NAME}"
    verify_published "$sig" "${version_base}/${UPDATE_SIG_NAME}" "${PACKAGE_VERSION}/${UPDATE_SIG_NAME}"
fi

# --- Phase 2: the mutable `latest` pointer -----------------------------------
#
# The deliberate immutability exception, scoped to these two files.
#
# GitLab keeps superseded revisions and serves the newest; they are left alone
# because deleting by name would also match the new upload. The signature is
# written first, so a client racing the swap sees a mismatched pair and fails
# verification safely.
publish_latest() { # local_file url label
    local file="$1" url="$2" label="$3" remote status
    remote="$tmp_dir/latest-$(basename "$file")"
    status="$(api_request --output "$remote" --write-out '%{http_code}' "$(api_request_url "$url")")" || \
        die "preflight request failed for $label"
    case "$status" in
        200)
            if cmp -s "$file" "$remote"; then
                printf 'Latest slot already carries these bytes: %s\n' "$label"
                return 0
            fi
            printf 'Replacing the latest slot: %s\n' "$label"
            upload_file "$file" "$url" "$label" ;;
        404)
            upload_file "$file" "$url" "$label" ;;
        *) die "preflight returned HTTP $status for $label" ;;
    esac
}

# `latest` must not move backwards by accident: `attach-existing` on an old
# release would otherwise re-point it, and clients refusing the downgrade would
# silently stop seeing updates. UPDATE_ALLOW_LATEST_ROLLBACK is for yanking a
# bad release. The current `.version` is unverified, so it is only ever used to
# refuse a promotion, never to cause one.
current_latest="$tmp_dir/current-latest.json"
status="$(api_request --output "$current_latest" --write-out '%{http_code}' \
    "$(api_request_url "${latest_base}/${UPDATE_MANIFEST_NAME}")")" || \
    die "could not read the current latest manifest"
if [[ "$UPDATE_REFRESH_LATEST_ONLY" == true ]]; then
    [[ "$status" == 200 ]] || die "refresh mode needs an existing latest manifest (HTTP $status)"
    current_version="$(jq -r '.version // empty' "$current_latest" 2>/dev/null || true)"
    [[ "$current_version" == "$PACKAGE_VERSION" ]] || \
        die "refresh mode re-promotes the version already in the latest slot (${current_version:-none}); this manifest is for ${PACKAGE_VERSION}"
fi
if [[ "$status" == 200 ]]; then
    current_version="$(jq -r '.version // empty' "$current_latest" 2>/dev/null || true)"
    if [[ -n "$current_version" && "$current_version" != "$PACKAGE_VERSION" ]]; then
        newest="$(printf '%s\n%s\n' "$current_version" "$PACKAGE_VERSION" | sort -V | tail -n 1)"
        if [[ "$newest" != "$PACKAGE_VERSION" ]]; then
            if [[ "${UPDATE_ALLOW_LATEST_ROLLBACK:-false}" == true ]]; then
                printf 'WARNING: rolling the latest slot back from %s to %s (UPDATE_ALLOW_LATEST_ROLLBACK=true)\n' \
                    "$current_version" "$PACKAGE_VERSION"
            else
                die "refusing to move the latest slot backwards from ${current_version} to ${PACKAGE_VERSION}; set UPDATE_ALLOW_LATEST_ROLLBACK=true only to yank a bad release deliberately"
            fi
        fi
    fi
fi

publish_latest "$sig" "${latest_base}/${UPDATE_SIG_NAME}" "${UPDATE_LATEST_SLOT}/${UPDATE_SIG_NAME}"
publish_latest "$manifest" "${latest_base}/${UPDATE_MANIFEST_NAME}" "${UPDATE_LATEST_SLOT}/${UPDATE_MANIFEST_NAME}"

verify_published "$sig" "${latest_base}/${UPDATE_SIG_NAME}" "${UPDATE_LATEST_SLOT}/${UPDATE_SIG_NAME}"
verify_published "$manifest" "${latest_base}/${UPDATE_MANIFEST_NAME}" "${UPDATE_LATEST_SLOT}/${UPDATE_MANIFEST_NAME}"

# --- Record for diagnostics and downstream jobs ------------------------------
jq -n \
    --arg version "$PACKAGE_VERSION" \
    --arg tag "$RELEASE_TAG" \
    --arg source_sha "$SOURCE_SHA" \
    --arg key_id "$(jq -er '.key_id' "$sig")" \
    --arg manifest_sha256 "$(sha_of "$manifest")" \
    --arg signature_sha256 "$(sha_of "$sig")" \
    --arg version_manifest_url "${version_base}/${UPDATE_MANIFEST_NAME}" \
    --arg version_signature_url "${version_base}/${UPDATE_SIG_NAME}" \
    --arg latest_manifest_url "${latest_base}/${UPDATE_MANIFEST_NAME}" \
    --arg latest_signature_url "${latest_base}/${UPDATE_SIG_NAME}" \
    '{version:$version, tag:$tag, source_sha:$source_sha, key_id:$key_id,
      manifest_sha256:$manifest_sha256, signature_sha256:$signature_sha256,
      urls:{version_manifest:$version_manifest_url,
            version_signature:$version_signature_url,
            latest_manifest:$latest_manifest_url,
            latest_signature:$latest_signature_url}}' \
    >"$ROOT/dist/update-publication.json"

printf 'Update manifest published for %s and promoted to the %s slot\n' \
    "$RELEASE_TAG" "$UPDATE_LATEST_SLOT"
