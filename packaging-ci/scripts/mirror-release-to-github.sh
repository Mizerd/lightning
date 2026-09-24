#!/usr/bin/env bash
set -Eeuo pipefail

# Mirror the already-published release binaries to a GitHub Release
# (MIRROR-SPEC §6).
#
# GitLab is the release authority; GitHub only holds byte-identical copies. A
# compromised mirror cannot ship an update: the manifest is signed with a key
# GitHub never holds, is fetched only from GitLab, and pins every SHA-256.
#
# Runs after finalize-release (the tag must exist) and before
# publish-update-manifest (so `latest` is promoted only once every mirror URL
# serves the right bytes). It uploads the exact dist/ files recorded in
# dist/manifest.json, and never overwrites or deletes a GitHub asset: an
# identical one is reused so retries converge, a differing one is fatal.
#
# GITHUB_MIRROR_TOKEN lives only in a 0600 curl config file removed on exit,
# never in argv (readable via /proc) or output. Keep `set -x` off here and in
# everything sourced.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
# shellcheck source=./scripts/gitlab-api.sh
source "$SCRIPT_DIR/gitlab-api.sh"
# shellcheck source=./scripts/update-lib.sh
source "$SCRIPT_DIR/update-lib.sh"

ROOT="$(project_dir)"
# Only for the publication contract and CURL_BIN; no GitLab request is made.
gitlab_api_init
release_contract_env

manifest="$ROOT/dist/manifest.json"
verification="$ROOT/dist/verification.json"
update_manifest="$ROOT/dist/${UPDATE_MANIFEST_NAME}"

[[ -f "$manifest" ]] || die "publication manifest is missing"
# Only mirror a publication that was verified.
[[ -f "$verification" ]] || die "verification record is missing; refusing to mirror"
[[ "$(jq -er '.source_sha' "$manifest")" == "$SOURCE_SHA" ]] || die "manifest source SHA mismatch"
[[ "$(jq -er '.version' "$manifest")" == "$PACKAGE_VERSION" ]] || die "manifest version mismatch"
[[ "$(jq -er '.source_sha' "$verification")" == "$SOURCE_SHA" ]] || die "verification source SHA mismatch"

# --- Disabled: a clean no-op, but never a silent one -------------------------
if ! update_mirror_enabled; then
    # A signed manifest that already promises mirror URLs must not be
    # promoted without the mirror release it points at.
    if [[ -f "$update_manifest" ]] && \
       jq -e '[.artifacts[] | select(has("mirror_url"))] | length > 0' "$update_manifest" >/dev/null; then
        die "the signed update manifest carries mirror_url values but GITHUB_MIRROR_REPO is not set"
    fi
    printf 'GITHUB_MIRROR_REPO is not set; the GitHub bandwidth mirror is disabled for this pipeline\n'
    printf 'Nothing was uploaded, and the update manifest carries no mirror_url\n'
    exit 0
fi
# A manifest refresh adds no package; everything was mirrored at release time.
# Exit before any request so a dead mirror token cannot block the refresh.
if [[ "${UPDATE_REFRESH_LATEST_ONLY:-false}" == true ]]; then
    printf 'Manifest refresh (UPDATE_REFRESH_LATEST_ONLY=true): the release mirror is untouched and GitHub is not contacted\n'
    exit 0
fi

# Enabled, so the token is required: a mirror that cannot be completed must
# stop the `latest` promotion rather than be skipped.
require_var GITHUB_MIRROR_TOKEN
update_mirror_repo_valid "$GITHUB_MIRROR_REPO" || \
    die "GITHUB_MIRROR_REPO must be <owner>/<repo>"
# Shape-check only (never printed); it makes the curl config below safe to quote.
[[ "$GITHUB_MIRROR_TOKEN" =~ ^[A-Za-z0-9_.-]{20,255}$ ]] || \
    die "GITHUB_MIRROR_TOKEN is not a plausible GitHub token (expected 20-255 chars of [A-Za-z0-9_.-])"

: "${GITHUB_MIRROR_TAG_WAIT_SECONDS:=300}"
: "${GITHUB_MIRROR_TAG_POLL_SECONDS:=15}"
[[ "$GITHUB_MIRROR_TAG_WAIT_SECONDS" =~ ^[0-9]+$ ]] || die "GITHUB_MIRROR_TAG_WAIT_SECONDS must be an integer"
[[ "$GITHUB_MIRROR_TAG_POLL_SECONDS" =~ ^[1-9][0-9]*$ ]] || die "GITHUB_MIRROR_TAG_POLL_SECONDS must be a positive integer"

tmp_dir="$(mktemp -d)"
chmod 700 "$tmp_dir"
auth_conf="$(mktemp)"
chmod 600 "$auth_conf"
cleanup() {
    rm -f "$auth_conf"
    rm -rf "$tmp_dir"
}
trap cleanup EXIT

# Credential via config file, never argv.
{
    printf 'header = "Authorization: Bearer %s"\n' "$GITHUB_MIRROR_TOKEN"
    printf 'header = "Accept: application/vnd.github+json"\n'
    printf 'header = "X-GitHub-Api-Version: 2022-11-28"\n'
} >"$auth_conf"

api_base="${UPDATE_MIRROR_API_HOST}/repos/${GITHUB_MIRROR_REPO}"
upload_base="${UPDATE_MIRROR_UPLOAD_HOST}/repos/${GITHUB_MIRROR_REPO}"

# Token-bearing, so no redirects: these endpoints never redirect, and the
# Authorization header must not reach a host we did not choose.
gh_get() { # url output -> prints status
    local url="$1" out="$2"
    "$CURL_BIN" --silent --show-error --max-redirs 0 \
        --config "$auth_conf" --output "$out" --write-out '%{http_code}' "$url"
}

# Anonymous, exactly as a client fetches it.
gh_get_anonymous() { # url output -> prints status
    local url="$1" out="$2"
    "$CURL_BIN" --silent --show-error --location --max-redirs 5 \
        --output "$out" --write-out '%{http_code}' "$url"
}

sha_of() { sha256sum "$1" | cut -d' ' -f1; }
size_of() { wc -c <"$1" | tr -d ' '; }

# --- The mirrored tag must exist, and must be the SAME commit ----------------
#
# GitLab push-mirrors refs asynchronously, so the tag may not have arrived.
# Creating a release for a tag GitHub lacks would make GitHub tag the default
# branch, so wait for the tag and prove it peels to the released commit (the
# `gh release create --verify-tag` guarantee).
# Prints the commit sha and returns 0; returns 10 for "not there yet", the only
# retryable outcome. Anything else dies (subshell exit 1), so a 401 never
# turns into a five-minute wait.
resolve_mirror_tag_commit() {
    local ref_json="$tmp_dir/tag-ref.json" status obj_type obj_sha tag_json
    status="$(gh_get "${api_base}/git/ref/tags/${RELEASE_TAG}" "$ref_json")" || \
        die "GitHub tag lookup request failed"
    case "$status" in
        200) ;;
        404) return 10 ;;
        401|403) die "GitHub refused the tag lookup (HTTP $status); check GITHUB_MIRROR_TOKEN's scope and that it can read ${GITHUB_MIRROR_REPO}" ;;
        *) die "GitHub tag lookup returned HTTP $status" ;;
    esac
    obj_type="$(jq -er '.object.type' "$ref_json")" || die "GitHub tag ref has no object type"
    obj_sha="$(jq -er '.object.sha' "$ref_json")" || die "GitHub tag ref has no object sha"
    case "$obj_type" in
        commit)
            printf '%s' "$obj_sha" ;;
        tag)
            # Annotated tag (finalize-release creates one): peel it.
            tag_json="$tmp_dir/tag-object.json"
            status="$(gh_get "${api_base}/git/tags/${obj_sha}" "$tag_json")" || \
                die "GitHub annotated-tag lookup request failed"
            [[ "$status" == 200 ]] || die "GitHub annotated-tag lookup returned HTTP $status"
            jq -er '.object.sha' "$tag_json" || die "GitHub annotated tag does not peel to a commit" ;;
        *)
            die "GitHub tag ${RELEASE_TAG} points at an unexpected object type: $obj_type" ;;
    esac
}

waited=0
tag_commit=""
while true; do
    lookup_rc=0
    tag_commit="$(resolve_mirror_tag_commit)" || lookup_rc=$?
    if (( lookup_rc == 0 )); then
        break
    fi
    # Anything other than "not there yet" has already reported itself.
    if (( lookup_rc != 10 )); then
        exit "$lookup_rc"
    fi
    if (( waited >= GITHUB_MIRROR_TAG_WAIT_SECONDS )); then
        die "tag ${RELEASE_TAG} did not appear on ${GITHUB_MIRROR_REPO} within ${GITHUB_MIRROR_TAG_WAIT_SECONDS}s; the GitLab push mirror has not delivered it yet. Nothing was uploaded, no GitHub release was created, and the GitLab release is unaffected — re-run this job once the tag is mirrored."
    fi
    printf 'Tag %s is not on the GitHub mirror yet; waiting %ss (%ss/%ss elapsed)\n' \
        "$RELEASE_TAG" "$GITHUB_MIRROR_TAG_POLL_SECONDS" "$waited" "$GITHUB_MIRROR_TAG_WAIT_SECONDS"
    sleep "$GITHUB_MIRROR_TAG_POLL_SECONDS"
    waited=$(( waited + GITHUB_MIRROR_TAG_POLL_SECONDS ))
done

[[ "$tag_commit" == "$SOURCE_SHA" ]] || \
    die "tag ${RELEASE_TAG} on ${GITHUB_MIRROR_REPO} peels to ${tag_commit}, not the released commit ${SOURCE_SHA}; refusing to mirror a different commit"
printf 'GitHub tag %s peels to the released commit %s\n' "$RELEASE_TAG" "$SOURCE_SHA"

# --- Files to mirror, proved to be the published bytes ------------------------
#
# The publication manifest's sha256 is authoritative; the local re-hash only
# proves the file is still the published one, and a mismatch is fatal.
declare -a mirror_files=() mirror_names=() mirror_shas=() mirror_sizes=()
while IFS= read -r entry; do
    filename="$(jq -er '.filename' <<<"$entry")"
    local_path="$(jq -er '.local_path' <<<"$entry")"
    sha256="$(jq -er '.sha256' <<<"$entry")"
    size="$(jq -er '.size' <<<"$entry")"
    path="$ROOT/$local_path"

    update_mirror_filename_safe "$filename" || \
        die "GitHub would rewrite the asset name '$filename'; refusing to publish a mirror URL that will not resolve"
    [[ -f "$path" ]] || die "mirror input missing: $local_path (this job uploads the published bytes; it does not rebuild them)"
    [[ "$(sha_of "$path")" == "$sha256" ]] || \
        die "$filename does not match the SHA-256 recorded in the publication manifest; refusing to mirror bytes that were not the published ones"
    [[ "$(size_of "$path")" == "$size" ]] || \
        die "$filename does not match the size recorded in the publication manifest"

    mirror_files+=("$path")
    mirror_names+=("$filename")
    mirror_shas+=("$sha256")
    mirror_sizes+=("$size")
done < <(jq -c '.entries[]' "$manifest")

(( ${#mirror_names[@]} >= 1 )) || die "the publication manifest lists no files to mirror"

# --- The signed manifest and this job must agree on every URL ----------------
#
# generate-update-manifest.sh derives mirror_url in an earlier stage from the
# same inputs; cross-check before uploading anything.
if [[ -f "$update_manifest" ]]; then
    while IFS= read -r row; do
        u_name="$(jq -er '.filename' <<<"$row")"
        u_mirror="$(jq -r '.mirror_url // ""' <<<"$row")"
        [[ -n "$u_mirror" ]] || \
            die "the signed update manifest has no mirror_url for $u_name while mirroring is enabled; regenerate it with GITHUB_MIRROR_REPO set"
        expected="$(update_mirror_asset_url "$GITHUB_MIRROR_REPO" "$RELEASE_TAG" "$u_name")"
        [[ "$u_mirror" == "$expected" ]] || \
            die "the signed update manifest's mirror_url for $u_name is $u_mirror, not $expected"
        # No pipe: grep -q exits early and SIGPIPE would fail under pipefail.
        grep -Fxq "$u_name" <<<"$(printf '%s\n' "${mirror_names[@]}")" || \
            die "the signed update manifest advertises a mirror for $u_name, which is not in the publication manifest"
    done < <(jq -c '.artifacts[]' "$update_manifest")
    printf 'Signed update manifest mirror URLs agree with this job for %s artifact(s)\n' \
        "$(jq '.artifacts | length' "$update_manifest")"
fi

# --- The mirror release ------------------------------------------------------
release_json="$tmp_dir/release.json"
status="$(gh_get "${api_base}/releases/tags/${RELEASE_TAG}" "$release_json")" || \
    die "GitHub release lookup request failed"

mirror_notes() { # output
    local canonical="${LIGHTNING_RELEASE_BASE_URL:-https://gitlab.smetonis.net/Mizerd/lightning}/-/releases/${RELEASE_TAG}"
    # Release notes first, from the same file finalize-release uses for the
    # GitLab description, then the mirror notice. A missing notes file only
    # warns: the tag and GitLab release already exist, so failing here would
    # leave a half-finished release.
    : >"$1"
    local notes_src="$ROOT/dist/release-notes.md"
    if [[ -s "$notes_src" ]]; then
        cat "$notes_src" >>"$1"
        printf '\n\n---\n\n' >>"$1"
        printf 'Mirror release notes: using %s\n' "$notes_src" >&2
    else
        printf 'WARNING: %s is missing or empty; the mirror release will carry only the mirror notice\n' \
            "$notes_src" >&2
    fi
    cat >>"$1" <<EOF
**This is a read-only mirror. It is not a release authority.**

The canonical Lightning ${PACKAGE_VERSION} release lives on GitLab:
${canonical}

The files attached here are byte-identical copies of the artifacts published
there. They exist so downloads do not all have to come from one small server.
Nothing about this page decides anything: it does not determine what version
exists, what is installable, or what Lightning will accept.

Verify any download against the \`SHA256SUMS\` asset, which is itself a copy of
the published one.

Lightning's in-app updater fetches its update manifest **only** from GitLab and
verifies an Ed25519 signature made with a key that exists nowhere near GitHub.
Every downloaded file is checked against the SHA-256 recorded in that signed
manifest before it is installed, so replacing a file here cannot cause Lightning
to install it.

Issues, source, and history: ${LIGHTNING_RELEASE_BASE_URL:-https://gitlab.smetonis.net/Mizerd/lightning}
EOF
}

case "$status" in
    200)
        release_id="$(jq -er '.id' "$release_json")" || die "existing GitHub release has no id"
        printf 'Mirror release %s already exists (id %s); reusing it\n' "$RELEASE_TAG" "$release_id"
        ;;
    404)
        notes="$tmp_dir/mirror-notes.md"
        body="$tmp_dir/create-release.json"
        response="$tmp_dir/create-response.json"
        mirror_notes "$notes"
        # tag_name only: a target_commitish would let GitHub create its own tag.
        jq -n \
            --arg tag_name "$RELEASE_TAG" \
            --arg name "Lightning ${PACKAGE_VERSION}" \
            --rawfile body "$notes" \
            '{tag_name:$tag_name, name:$name, body:$body,
              draft:false, prerelease:false}' >"$body"
        status="$("$CURL_BIN" --silent --show-error \
            --config "$auth_conf" --request POST \
            --header 'Content-Type: application/json' \
            --data "@$body" --output "$response" --write-out '%{http_code}' \
            "${api_base}/releases")" || die "GitHub release creation request failed"
        [[ "$status" == 201 ]] || {
            printf 'GitHub release creation returned HTTP %s\n' "$status" >&2
            die "could not create the mirror release for ${RELEASE_TAG}"
        }
        release_id="$(jq -er '.id' "$response")" || die "GitHub release creation returned no id"
        cp "$response" "$release_json"
        printf 'Created mirror release %s (id %s)\n' "$RELEASE_TAG" "$release_id"
        ;;
    401|403)
        die "GitHub refused the release lookup (HTTP $status); check GITHUB_MIRROR_TOKEN's scope" ;;
    *)
        die "GitHub release lookup returned HTTP $status" ;;
esac

[[ "$release_id" =~ ^[0-9]+$ ]] || die "GitHub release id is not numeric"

# --- Upload, idempotently ----------------------------------------------------
#
# An asset already present with the expected size is left alone; a differing
# one is fatal. The mirror never overwrites or deletes a published file.
existing_asset() { # name -> prints the asset object, empty when absent
    jq -c --arg n "$1" '[.assets[]? | select(.name == $n)] | first // empty' "$release_json"
}

uploaded=0
reused=0
for i in "${!mirror_names[@]}"; do
    name="${mirror_names[$i]}"
    path="${mirror_files[$i]}"
    size="${mirror_sizes[$i]}"
    asset="$(existing_asset "$name")"
    if [[ -n "$asset" ]]; then
        state="$(jq -r '.state // ""' <<<"$asset")"
        asset_size="$(jq -r '.size // -1' <<<"$asset")"
        [[ "$state" == uploaded ]] || \
            die "mirror asset $name exists in state '$state'; a partially uploaded asset must be removed by hand before this job can converge"
        [[ "$asset_size" == "$size" ]] || \
            die "mirror asset $name is already present with a different size (${asset_size} vs ${size}); refusing to overwrite a published asset"
        printf 'Already mirrored: %s (%s bytes)\n' "$name" "$size"
        reused=$(( reused + 1 ))
        continue
    fi
    # No --location: a redirect would resend the whole body.
    status="$("$CURL_BIN" --silent --show-error \
        --config "$auth_conf" --request POST \
        --header 'Content-Type: application/octet-stream' \
        --upload-file "$path" \
        --output "$tmp_dir/upload.json" --write-out '%{http_code}' \
        "${upload_base}/releases/${release_id}/assets?name=${name}")" || \
        die "upload request failed for $name"
    [[ "$status" == 201 ]] || {
        printf 'upload of %s returned HTTP %s\n' "$name" "$status" >&2
        die "mirror upload failed for $name"
    }
    printf 'Uploaded %s (%s bytes)\n' "$name" "$size"
    uploaded=$(( uploaded + 1 ))
done

# --- Verify, anonymously -----------------------------------------------------
#
# Re-read the release, then fetch every asset anonymously from the URL the
# signed manifest uses, checking size and SHA-256 against dist/manifest.json.
status="$(gh_get "${api_base}/releases/tags/${RELEASE_TAG}" "$release_json")" || \
    die "GitHub release re-read request failed"
[[ "$status" == 200 ]] || die "GitHub release re-read returned HTTP $status"

assets_record='[]'
for i in "${!mirror_names[@]}"; do
    name="${mirror_names[$i]}"
    sha256="${mirror_shas[$i]}"
    size="${mirror_sizes[$i]}"
    asset="$(existing_asset "$name")"
    [[ -n "$asset" ]] || die "mirror asset $name is missing from the release after upload"
    [[ "$(jq -r '.state // ""' <<<"$asset")" == uploaded ]] || \
        die "mirror asset $name did not reach the uploaded state"
    [[ "$(jq -r '.size // -1' <<<"$asset")" == "$size" ]] || \
        die "mirror asset $name has size $(jq -r '.size' <<<"$asset"), expected $size"

    url="$(update_mirror_asset_url "$GITHUB_MIRROR_REPO" "$RELEASE_TAG" "$name")"
    dl="$tmp_dir/verify-$name"
    status="$(gh_get_anonymous "$url" "$dl")" || die "anonymous read-back request failed for $name"
    [[ "$status" == 200 ]] || die "anonymous read-back of $name returned HTTP $status"
    [[ "$(size_of "$dl")" == "$size" ]] || \
        die "$name downloaded from the mirror is $(size_of "$dl") bytes, expected $size"
    [[ "$(sha_of "$dl")" == "$sha256" ]] || \
        die "$name downloaded from the mirror does not match the published SHA-256"
    rm -f "$dl"
    printf 'Verified anonymously: %s (%s bytes, sha256 %s)\n' "$name" "$size" "$sha256"

    assets_record="$(jq -c \
        --arg filename "$name" --arg sha256 "$sha256" --arg url "$url" \
        --argjson size "$size" \
        '. + [{filename:$filename, size:$size, sha256:$sha256, mirror_url:$url}]' \
        <<<"$assets_record")"
done

# Diagnostic record: public URLs and published checksums only.
jq -n \
    --arg version "$PACKAGE_VERSION" \
    --arg tag "$RELEASE_TAG" \
    --arg source_sha "$SOURCE_SHA" \
    --arg repository "$GITHUB_MIRROR_REPO" \
    --arg release_url "${UPDATE_MIRROR_DOWNLOAD_HOST}/${GITHUB_MIRROR_REPO}/releases/tag/${RELEASE_TAG}" \
    --argjson release_id "$release_id" \
    --argjson uploaded "$uploaded" \
    --argjson reused "$reused" \
    --argjson assets "$assets_record" \
    '{version:$version, tag:$tag, source_sha:$source_sha,
      repository:$repository, release_id:$release_id, release_url:$release_url,
      uploaded:$uploaded, already_present:$reused, assets:$assets}' \
    >"$ROOT/dist/github-mirror.json"

printf 'Mirrored %s artifact(s) to %s %s (%s uploaded, %s already present)\n' \
    "${#mirror_names[@]}" "$GITHUB_MIRROR_REPO" "$RELEASE_TAG" "$uploaded" "$reused"
printf 'GitLab remains the release authority; this mirror carries bytes only\n'
