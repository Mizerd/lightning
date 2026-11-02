#!/usr/bin/env bash
set -Eeuo pipefail

# Copy the published release bytes and the signed update-manifest pair to the
# public gitlab.com mirror, the clients' last-resort byte source
# (src/update/UpdateEndpoints.cpp).
#
#   <base>/<version>/<file>                  every file in dist/manifest.json
#   <base>/update-latest/update-manifest-v1.json{,.sig}   the manifest slot
#
# gitlab.com holds bytes and decides nothing: the manifest is signed in this
# pipeline and clients verify the signature and every SHA-256 whichever host
# served them. The host, project and package name are constants so no variable
# can steer where clients are told to look. A no-op without GITLAB_COM_TOKEN.
# allow_failure in CI; idempotent; release files are never replaced, and the
# slot never moves backwards without UPDATE_ALLOW_LATEST_ROLLBACK=true.
#
# GITLAB_COM_TOKEN lives only in a 0600 curl config file removed on exit,
# never in argv or output. Keep `set -x` off here and in everything sourced.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
# shellcheck source=./scripts/gitlab-api.sh
source "$SCRIPT_DIR/gitlab-api.sh"
# shellcheck source=./scripts/update-lib.sh
source "$SCRIPT_DIR/update-lib.sh"

GITLAB_COM_PACKAGE_BASE="https://gitlab.com/api/v4/projects/88402223/packages/generic/lightning"
GITLAB_COM_SLOT_VERSION="update-latest"
: "${GITLAB_COM_READBACK_WAIT_SECONDS:=120}"
: "${GITLAB_COM_READBACK_POLL_SECONDS:=10}"
[[ "$GITLAB_COM_READBACK_WAIT_SECONDS" =~ ^[0-9]+$ ]] || die "GITLAB_COM_READBACK_WAIT_SECONDS must be an integer"
[[ "$GITLAB_COM_READBACK_POLL_SECONDS" =~ ^[1-9][0-9]*$ ]] || die "GITLAB_COM_READBACK_POLL_SECONDS must be a positive integer"

if [[ -z "${GITLAB_COM_TOKEN:-}" ]]; then
    printf 'GITLAB_COM_TOKEN is not set; the gitlab.com mirror is skipped\n'
    exit 0
fi
[[ "$GITLAB_COM_TOKEN" =~ ^[A-Za-z0-9_.-]{20,255}$ ]] || \
    die "GITLAB_COM_TOKEN is not a plausible GitLab token (expected 20-255 chars of [A-Za-z0-9_.-])"

ROOT="$(project_dir)"
# Only for the publication contract and CURL_BIN; no request goes to the
# project's own GitLab.
gitlab_api_init
release_contract_env
CURL_BIN="${CURL_BIN:-curl}"

manifest="$ROOT/dist/manifest.json"
update_manifest="$ROOT/dist/${UPDATE_MANIFEST_NAME}"
update_sig="$ROOT/dist/${UPDATE_SIG_NAME}"
publication="$ROOT/dist/update-publication.json"

[[ -f "$manifest" ]] || die "publication manifest is missing"
[[ -f "$update_manifest" && -f "$update_sig" ]] || die "signed update manifest pair missing from dist/"
[[ -f "$publication" ]] || \
    die "dist/update-publication.json is missing: the latest slot has not been promoted, so there is nothing to mirror"
[[ "$(jq -er '.version' "$manifest")" == "$PACKAGE_VERSION" ]] || die "manifest version mismatch"
[[ "$PACKAGE_VERSION" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || die "version '$PACKAGE_VERSION' is not a safe gitlab.com package version"

sha_of() { sha256sum "$1" | cut -d' ' -f1; }
size_of() { wc -c <"$1" | tr -d ' '; }

[[ "$(jq -r '.manifest_sha256 // ""' "$publication")" == "$(sha_of "$update_manifest")" ]] || \
    die "the local manifest is not the one that was promoted (sha256 differs from dist/update-publication.json)"
[[ "$(jq -r '.signature_sha256 // ""' "$publication")" == "$(sha_of "$update_sig")" ]] || \
    die "the local signature is not the one that was promoted"
[[ "$(jq -r '.version // ""' "$update_manifest")" == "$PACKAGE_VERSION" ]] || \
    die "the signed update manifest does not describe version $PACKAGE_VERSION"

tmp_dir="$(mktemp -d)"
chmod 700 "$tmp_dir"
auth_conf="$(mktemp)"
chmod 600 "$auth_conf"
cleanup() { rm -f "$auth_conf"; rm -rf "$tmp_dir"; }
trap cleanup EXIT
printf 'header = "PRIVATE-TOKEN: %s"\n' "$GITLAB_COM_TOKEN" >"$auth_conf"

# Anonymous, as a client fetches it, within the client's redirect limit.
fetch_anonymous() { # url output -> prints status
    "$CURL_BIN" --silent --show-error --location --max-redirs 4 --max-time 120 \
        --output "$2" --write-out '%{http_code}' "$1"
}

upload() { # version file name
    local status
    status="$("$CURL_BIN" --silent --show-error --max-redirs 0 --max-time 600 \
        --config "$auth_conf" --request PUT --upload-file "$2" \
        --output "$tmp_dir/upload.json" --write-out '%{http_code}' \
        "${GITLAB_COM_PACKAGE_BASE}/$1/$3?select=package_file")" || die "upload request failed for $3"
    [[ "$status" == 200 || "$status" == 201 ]] || die "uploading $3 to gitlab.com returned HTTP $status"
}

# Waits for the exact bytes, not just a 200: package storage is eventually
# consistent and a duplicate file can briefly serve the previous one.
verify_by_redownload() { # version file name
    local url="${GITLAB_COM_PACKAGE_BASE}/$1/$3" dl="$tmp_dir/verify-$3" status waited=0
    while :; do
        status="$(fetch_anonymous "$url" "$dl")" || status="curl-failed"
        if [[ "$status" == 200 ]] && [[ "$(sha_of "$dl")" == "$(sha_of "$2")" ]]; then
            printf 'Verified by re-download: %s/%s (%s bytes)\n' "$1" "$3" "$(size_of "$2")"
            return 0
        fi
        if (( waited >= GITLAB_COM_READBACK_WAIT_SECONDS )); then
            die "re-download of $1/$3 from gitlab.com does not match the published bytes (last status: $status)"
        fi
        sleep "$GITLAB_COM_READBACK_POLL_SECONDS"
        waited=$(( waited + GITLAB_COM_READBACK_POLL_SECONDS ))
    done
}

# --- Release files: byte-identical to the published ones, never replaced ------
if [[ "${UPDATE_REFRESH_LATEST_ONLY:-false}" == true ]]; then
    printf 'Manifest refresh (UPDATE_REFRESH_LATEST_ONLY=true): release files are left as they are\n'
else
    declare -a uploaded_names=()
    while IFS= read -r entry; do
        filename="$(jq -er '.filename' <<<"$entry")"
        local_path="$(jq -er '.local_path' <<<"$entry")"
        sha256="$(jq -er '.sha256' <<<"$entry")"
        size="$(jq -er '.size' <<<"$entry")"
        path="$ROOT/$local_path"

        update_mirror_filename_safe "$filename" || die "'$filename' is not a safe gitlab.com file name"
        [[ -f "$path" ]] || die "mirror input missing: $local_path"
        [[ "$(sha_of "$path")" == "$sha256" ]] || \
            die "$filename does not match the SHA-256 recorded in the publication manifest"
        [[ "$(size_of "$path")" == "$size" ]] || \
            die "$filename does not match the size recorded in the publication manifest"

        existing="$tmp_dir/existing-$filename"
        status="$(fetch_anonymous "${GITLAB_COM_PACKAGE_BASE}/${PACKAGE_VERSION}/${filename}" "$existing")" || status="curl-failed"
        if [[ "$status" == 200 ]]; then
            [[ "$(sha_of "$existing")" == "$sha256" ]] || \
                die "gitlab.com already holds a different $filename for $PACKAGE_VERSION; refusing to replace a published file"
            printf 'gitlab.com already carries %s\n' "$filename"
        else
            upload "$PACKAGE_VERSION" "$path" "$filename"
            printf 'Uploaded %s\n' "$filename"
        fi
        verify_by_redownload "$PACKAGE_VERSION" "$path" "$filename"
        uploaded_names+=("$filename")
    done < <(jq -c '.entries[]' "$manifest")
    (( ${#uploaded_names[@]} >= 1 )) || die "the publication manifest lists no files to mirror"

    # A manifest artifact gitlab.com lacks would only 404 for clients, but it
    # means the mirror is incomplete.
    while IFS= read -r wanted; do
        printf '%s\n' "${uploaded_names[@]}" | grep -Fxq -- "$wanted" || \
            die "the signed update manifest names $wanted, which is not among the mirrored files"
    done < <(jq -r '.artifacts[].filename' "$update_manifest")
fi

# --- The manifest slot: signature first, manifest second, never backwards -----
current="$tmp_dir/current-slot-manifest"
status="$(fetch_anonymous "${GITLAB_COM_PACKAGE_BASE}/${GITLAB_COM_SLOT_VERSION}/${UPDATE_MANIFEST_NAME}" "$current")" || status="curl-failed"
if [[ "$status" == 200 ]]; then
    current_version="$(jq -r '.version // ""' "$current" 2>/dev/null || true)"
    if [[ -n "$current_version" && "$current_version" != "$PACKAGE_VERSION" ]]; then
        newest="$(printf '%s\n%s\n' "$current_version" "$PACKAGE_VERSION" | sort -V | tail -n 1)"
        if [[ "$newest" != "$PACKAGE_VERSION" ]]; then
            if [[ "${UPDATE_ALLOW_LATEST_ROLLBACK:-false}" == true ]]; then
                printf 'WARNING: rolling the gitlab.com slot back from %s to %s (UPDATE_ALLOW_LATEST_ROLLBACK=true)\n' \
                    "$current_version" "$PACKAGE_VERSION"
            else
                die "refusing to move the gitlab.com slot backwards from ${current_version} to ${PACKAGE_VERSION}"
            fi
        fi
    fi
fi

for pair in "$update_sig:$UPDATE_SIG_NAME" "$update_manifest:$UPDATE_MANIFEST_NAME"; do
    file="${pair%%:*}"
    name="${pair##*:}"
    status="$(fetch_anonymous "${GITLAB_COM_PACKAGE_BASE}/${GITLAB_COM_SLOT_VERSION}/${name}" "$tmp_dir/slot-$name")" || status="curl-failed"
    if [[ "$status" == 200 && "$(sha_of "$tmp_dir/slot-$name")" == "$(sha_of "$file")" ]]; then
        printf 'The gitlab.com slot already carries %s\n' "$name"
    else
        upload "$GITLAB_COM_SLOT_VERSION" "$file" "$name"
        printf 'Uploaded %s to the gitlab.com slot\n' "$name"
    fi
    verify_by_redownload "$GITLAB_COM_SLOT_VERSION" "$file" "$name"
done

printf 'The gitlab.com mirror now serves %s\n' "$PACKAGE_VERSION"
