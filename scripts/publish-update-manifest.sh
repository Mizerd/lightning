#!/usr/bin/env bash
set -Eeuo pipefail

# Upload the signed update manifest to project 6's Generic Package Registry
# under TWO paths (UPDATE-SPEC §6):
#
#   packages/generic/lightning-update/<version>/   immutable per-release copy
#   packages/generic/lightning-update/latest/      the stable slot clients poll
#
# Ordering inside this script is a security property, not a convenience: the
# versioned copy is uploaded and read back byte-for-byte FIRST, and only then is
# the `latest` pointer moved. A `latest` slot that advertised a release whose
# own copy had not finished publishing would be pointing clients at bytes that
# may not exist.
#
# The `latest` slot is the ONE deliberately mutable path this pipeline writes.
# publish-packages.sh treats a pre-existing file as immutable and requires byte
# identity; that rule is untouched for every release package and for the
# versioned copy here. The exception below is scoped to the two `latest` files
# and nothing else.

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

# The manifest must describe THIS release. A stale artifact from a previous
# pipeline would otherwise be republished under the new version's slot.
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
            # The most likely cause for the mutable slot specifically.
            printf 'hint: the "latest" slot requires generic-package duplicates to be permitted for %s on project 6\n' \
                "$UPDATE_PACKAGE_NAME" >&2
        fi
        die "upload of $label failed"
    fi
    printf 'Uploaded %s\n' "$label"
}

# --- Phase 1: the immutable per-release copy ---------------------------------
#
# Same contract as publish-packages.sh: an identical existing file is accepted
# (so a job retry converges), a different one is a hard conflict.
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

publish_immutable "$manifest" "${version_base}/${UPDATE_MANIFEST_NAME}" "${PACKAGE_VERSION}/${UPDATE_MANIFEST_NAME}"
publish_immutable "$sig" "${version_base}/${UPDATE_SIG_NAME}" "${PACKAGE_VERSION}/${UPDATE_SIG_NAME}"

verify_published "$manifest" "${version_base}/${UPDATE_MANIFEST_NAME}" "${PACKAGE_VERSION}/${UPDATE_MANIFEST_NAME}"
verify_published "$sig" "${version_base}/${UPDATE_SIG_NAME}" "${PACKAGE_VERSION}/${UPDATE_SIG_NAME}"

# --- Phase 2: the mutable `latest` pointer -----------------------------------
#
# DELIBERATE IMMUTABILITY EXCEPTION, scoped to these two files only.
#
# `latest` is the discovery slot Lightning polls; it exists precisely so that a
# newer release can replace what it points at. Refusing to overwrite it — the
# rule that protects every release package — would make it useless after the
# first release. Everything else keeps the immutable contract, including the
# versioned copies published above.
#
# Two consequences worth stating plainly:
#   * GitLab's generic registry keeps superseded revisions of an overwritten
#     file and serves the most recent one. Old revisions are inert history, not
#     something a client can be steered to, so they are left alone; deleting by
#     file name here would also match the revision just uploaded.
#   * The signature travels with the manifest. A client that races the two
#     fetches mid-swap sees a manifest and a signature from different releases
#     and simply fails verification — which is the correct, safe outcome. It
#     never sees an unsigned or mis-signed manifest as valid.
#
# The signature is written FIRST and the manifest second, so the narrow race
# window is "new signature, old manifest" rather than "new manifest, no matching
# signature yet".
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
