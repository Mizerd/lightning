#!/usr/bin/env bash
set -Eeuo pipefail

# Build dist/update-manifest-v1.json — the signed document Lightning polls to
# discover a newer release (UPDATE-SPEC §4).
#
# Runs after verify-published-packages. Every value is copied from
# dist/manifest.json, the verified record; nothing is re-hashed locally.
# Output is deterministic (sorted keys, pinned timestamp) because the
# per-release copy is immutable and a retry must be byte-identical.

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

manifest="$ROOT/dist/manifest.json"
verification="$ROOT/dist/verification.json"
[[ -f "$manifest" ]] || die "publication manifest is missing"
# Only build from a verified publication.
[[ -f "$verification" ]] || die "verification record is missing; refusing to build an update manifest"
[[ "$(jq -er '.source_sha' "$manifest")" == "$SOURCE_SHA" ]] || die "manifest source SHA mismatch"
[[ "$(jq -er '.version' "$manifest")" == "$PACKAGE_VERSION" ]] || die "manifest version mismatch"
[[ "$(jq -er '.source_sha' "$verification")" == "$SOURCE_SHA" ]] || die "verification source SHA mismatch"
[[ "$(jq -er '.version' "$verification")" == "$PACKAGE_VERSION" ]] || die "verification version mismatch"

# Every advertised artifact must be in the record's verified_files.
verified_names="$(jq -c '.verified_files' "$verification")"

# --- Publication format -> UPDATE-SPEC §5 install-type key -------------------
#
# Only directly updatable formats. Flatpak and snap installs are updated by
# their own ecosystems and are described in `channels` instead (UPDATE-SPEC §4,
# §10); SHA256SUMS is not a package.
declare -a update_formats=(
    windows-msi windows-setup windows-portable
    appimage deb rpm
)
declare -A install_key_of=(
    [windows-msi]="windows-msi"
    [windows-setup]="windows-setup"
    [windows-portable]="windows-portable"
    [appimage]="linux-appimage"
    [deb]="linux-deb"
    [rpm]="linux-rpm"
)

# --- Optional GitHub bandwidth mirror (MIRROR-SPEC §3) ------------------------
#
# With mirroring enabled, each artifact gets an optional `mirror_url` derived
# from GITHUB_MIRROR_REPO, the tag and the filename; otherwise the field is
# absent. It is signed, so the release authority chooses the mirror location.
# `url` stays required and `schema` stays 1; older clients ignore the field.
if update_mirror_enabled; then
    update_mirror_repo_valid "$GITHUB_MIRROR_REPO" || \
        die "GITHUB_MIRROR_REPO must be <owner>/<repo>"
    printf 'GitHub bandwidth mirror enabled: artifacts will carry mirror_url on %s\n' \
        "$GITHUB_MIRROR_REPO" >&2
else
    printf 'No GITHUB_MIRROR_REPO configured; artifacts will carry no mirror_url\n' >&2
fi

artifacts='{}'
for fmt in "${update_formats[@]}"; do
    entry="$(jq -c --arg f "$fmt" '[.entries[] | select(.format == $f)] | first // empty' "$manifest")"
    if [[ -z "$entry" ]]; then
        # An absent install type means "no direct download".
        printf 'No published %s artifact; omitting %s from the update manifest\n' \
            "$fmt" "${install_key_of[$fmt]}" >&2
        continue
    fi

    filename="$(jq -er '.filename' <<<"$entry")"
    size="$(jq -er '.size' <<<"$entry")"
    sha256="$(jq -er '.sha256' <<<"$entry")"
    url="$(jq -er '.registry_url' <<<"$entry")"

    [[ "$sha256" =~ ^[0-9a-f]{64}$ ]] || die "published $fmt entry has a malformed SHA-256"
    [[ "$size" =~ ^[1-9][0-9]*$ ]] || die "published $fmt entry has a non-positive size"
    # HTTPS on the canonical registry only; the client enforces an allowlist.
    [[ "$url" == https://* ]] || die "published $fmt URL is not https: $url"
    [[ "$url" == "${CANONICAL_API_ROOT}/packages/generic/${PACKAGE_NAME}/${PACKAGE_VERSION}/"* ]] || \
        die "published $fmt URL is not on the canonical release registry path"
    jq -e --arg n "$filename" --argjson v "$verified_names" \
        -n '$v | index($n) != null' >/dev/null || \
        die "published $fmt file $filename is not in the verification record"

    mirror_url=""
    if update_mirror_enabled; then
        # A name GitHub would rewrite would yield a URL that does not resolve.
        update_mirror_filename_safe "$filename" || \
            die "GitHub would rewrite the asset name '$filename'; refusing to emit a mirror_url that will not resolve"
        mirror_url="$(update_mirror_asset_url "$GITHUB_MIRROR_REPO" "$RELEASE_TAG" "$filename")"
        [[ "$mirror_url" == https://* ]] || die "derived mirror URL is not https: $mirror_url"
    fi

    # No mirror means an absent field, not null or "".
    artifacts="$(jq -c \
        --arg key "${install_key_of[$fmt]}" \
        --arg filename "$filename" \
        --arg sha256 "$sha256" \
        --arg url "$url" \
        --arg mirror_url "$mirror_url" \
        --argjson size "$size" \
        '. + {($key): ({filename:$filename, size:$size, sha256:$sha256, url:$url}
                       + (if $mirror_url == "" then {} else {mirror_url:$mirror_url} end))}' \
        <<<"$artifacts")"
done

(( "$(jq 'length' <<<"$artifacts")" >= 1 )) || die "no directly updatable artifact was published"

# --- Ecosystem channels ------------------------------------------------------
#
# Driven by variables so enabling a channel needs no code change. All four are
# false: no Flathub, Snap Store, APT or DNF/YUM publication exists, and the
# release bundles are manual downloads.
: "${UPDATE_CHANNEL_FLATPAK_AVAILABLE:=false}"
: "${UPDATE_CHANNEL_FLATPAK_VERSION:=}"
: "${UPDATE_CHANNEL_FLATPAK_NOTE:=No Flathub publication exists. The .flatpak bundle on the release page is a manual download, not an update source.}"
: "${UPDATE_CHANNEL_SNAP_AVAILABLE:=false}"
: "${UPDATE_CHANNEL_SNAP_VERSION:=}"
: "${UPDATE_CHANNEL_SNAP_NOTE:=No Snap Store publication exists. The .snap on the release page is a manual download, not an update source.}"
: "${UPDATE_CHANNEL_DEB_REPO_AVAILABLE:=false}"
: "${UPDATE_CHANNEL_DEB_REPO_VERSION:=}"
: "${UPDATE_CHANNEL_DEB_REPO_NOTE:=No APT repository exists. Install the published .deb directly.}"
: "${UPDATE_CHANNEL_RPM_REPO_AVAILABLE:=false}"
: "${UPDATE_CHANNEL_RPM_REPO_VERSION:=}"
: "${UPDATE_CHANNEL_RPM_REPO_NOTE:=No DNF/YUM repository exists. Install the published .rpm directly.}"

channel_entry() { # available version note
    local available="$1" version="$2" note="$3"
    case "$available" in
        true|false) ;;
        *) die "channel availability must be true or false, got '$available'" ;;
    esac
    # available=true with no version would let a client compare against null.
    if [[ "$available" == true ]]; then
        [[ -n "$version" ]] || die "an available channel must state its version"
    fi
    jq -cn --argjson available "$available" \
        --arg version "$version" --arg note "$note" \
        '{available:$available,
          version: (if $version == "" then null else $version end),
          note: (if $note == "" then null else $note end)}'
}

channels="$(jq -cn \
    --argjson flatpak "$(channel_entry "$UPDATE_CHANNEL_FLATPAK_AVAILABLE" "$UPDATE_CHANNEL_FLATPAK_VERSION" "$UPDATE_CHANNEL_FLATPAK_NOTE")" \
    --argjson snap "$(channel_entry "$UPDATE_CHANNEL_SNAP_AVAILABLE" "$UPDATE_CHANNEL_SNAP_VERSION" "$UPDATE_CHANNEL_SNAP_NOTE")" \
    --argjson debrepo "$(channel_entry "$UPDATE_CHANNEL_DEB_REPO_AVAILABLE" "$UPDATE_CHANNEL_DEB_REPO_VERSION" "$UPDATE_CHANNEL_DEB_REPO_NOTE")" \
    --argjson rpmrepo "$(channel_entry "$UPDATE_CHANNEL_RPM_REPO_AVAILABLE" "$UPDATE_CHANNEL_RPM_REPO_VERSION" "$UPDATE_CHANNEL_RPM_REPO_NOTE")" \
    '{"linux-flatpak":$flatpak, "linux-snap":$snap,
      "linux-deb-repo":$debrepo, "linux-rpm-repo":$rpmrepo}')"

# --- Release metadata --------------------------------------------------------
: "${UPDATE_CHANNEL:=stable}"
[[ "$UPDATE_CHANNEL" == stable ]] || die "only the stable channel is published today"
: "${UPDATE_MIN_UPDATER_VERSION:=1}"
[[ "$UPDATE_MIN_UPDATER_VERSION" =~ ^[0-9]+$ ]] || die "UPDATE_MIN_UPDATER_VERSION must be an integer"
: "${LIGHTNING_RELEASE_BASE_URL:=https://gitlab.smetonis.net/Mizerd/lightning}"
[[ "$LIGHTNING_RELEASE_BASE_URL" == https://* ]] || die "release base URL must be https"
release_notes_url="${LIGHTNING_RELEASE_BASE_URL}/-/releases/${RELEASE_TAG}"

# An explicit override, else the pipeline creation time: both are stable
# across retries, which the immutable per-release copy requires.
released="${UPDATE_RELEASED_AT:-${CI_PIPELINE_CREATED_AT:-}}"
if [[ -z "$released" ]]; then
    released="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'warning: no UPDATE_RELEASED_AT or CI_PIPELINE_CREATED_AT; using wall clock (not retry-stable)\n' >&2
elif [[ ! "$released" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$ ]]; then
    released="$(date -u -d "$released" +%Y-%m-%dT%H:%M:%SZ)" || die "could not normalise the release timestamp"
fi

# A signature does not prove freshness; `expires` limits how long a captured
# manifest can be replayed. Clients from 0.8.4 require it. It is measured from
# `released` to stay retry-stable. To extend it without a release, refresh
# `latest` with UPDATE_REFRESH_LATEST_ONLY=true, the original
# UPDATE_RELEASED_AT and an explicit UPDATE_EXPIRES_AT (docs/update-manifest.md).
: "${UPDATE_MANIFEST_VALIDITY_DAYS:=120}"
[[ "$UPDATE_MANIFEST_VALIDITY_DAYS" =~ ^[1-9][0-9]{0,2}$ ]] && (( 10#$UPDATE_MANIFEST_VALIDITY_DAYS <= 366 )) || \
    die "UPDATE_MANIFEST_VALIDITY_DAYS must be a decimal integer between 1 and 366"
if [[ -n "${UPDATE_EXPIRES_AT:-}" ]]; then
    [[ "$UPDATE_EXPIRES_AT" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$ ]] || \
        die "UPDATE_EXPIRES_AT must be an ISO-8601 UTC instant (YYYY-MM-DDTHH:MM:SSZ)"
    expires="$UPDATE_EXPIRES_AT"
    [[ "$(date -u -d "$expires" +%s)" -gt "$(date -u -d "$released" +%s)" ]] || \
        die "UPDATE_EXPIRES_AT must be after the release timestamp"
else
    expires="$(date -u -d "${released} + ${UPDATE_MANIFEST_VALIDITY_DAYS} days" +%Y-%m-%dT%H:%M:%SZ)" || \
        die "could not compute the manifest expiry (GNU date is required)"
fi

notes_file="$(mktemp)"
tmp_cleanup() { rm -f "$notes_file"; }
trap tmp_cleanup EXIT
: >"$notes_file"
if [[ "${UPDATE_INCLUDE_RELEASE_NOTES:-true}" == true ]]; then
    # Same resolution order as finalize-release.sh; its policy footer is for
    # the release page and is deliberately omitted.
    if [[ -n "${RELEASE_NOTES_B64:-}" ]]; then
        printf '%s' "$RELEASE_NOTES_B64" | base64 -d >"$notes_file" 2>/dev/null || \
            die "RELEASE_NOTES_B64 is not valid base64"
    elif [[ -f "$ROOT/dist/release-notes.md" ]]; then
        cp "$ROOT/dist/release-notes.md" "$notes_file"
    fi
fi

out="$ROOT/dist/${UPDATE_MANIFEST_NAME}"
# -S sorts keys so the bytes are reproducible.
jq -S -n \
    --arg version "$PACKAGE_VERSION" \
    --arg channel "$UPDATE_CHANNEL" \
    --arg tag "$RELEASE_TAG" \
    --arg released "$released" \
    --arg expires "$expires" \
    --arg release_notes_url "$release_notes_url" \
    --rawfile release_notes "$notes_file" \
    --argjson schema 1 \
    --argjson min_updater_version "$UPDATE_MIN_UPDATER_VERSION" \
    --argjson artifacts "$artifacts" \
    --argjson channels "$channels" \
    '{schema:$schema, version:$version, channel:$channel, tag:$tag,
      released:$released, expires:$expires, min_updater_version:$min_updater_version,
      release_notes_url:$release_notes_url, release_notes:$release_notes,
      artifacts:$artifacts, channels:$channels}' >"$out"

# The client bound is 1 MiB (UPDATE-SPEC §2); stay well below it. If notes are
# too long, shorten them or set UPDATE_INCLUDE_RELEASE_NOTES=false.
manifest_size="$(wc -c <"$out" | tr -d ' ')"
(( manifest_size <= 262144 )) || \
    die "update manifest is ${manifest_size} bytes; the client bound is 1 MiB and the pipeline limit is 256 KiB"

printf 'Update manifest describes %s directly updatable artifact(s) for %s %s (%s bytes)\n' \
    "$(jq '.artifacts | length' "$out")" "$PACKAGE_NAME" "$PACKAGE_VERSION" "$manifest_size"
printf 'Install types: %s\n' "$(jq -r '.artifacts | keys | join(", ")' "$out")"
printf 'Ecosystem channels advertised as available: %s\n' \
    "$(jq -r '[.channels | to_entries[] | select(.value.available) | .key] | if length == 0 then "none" else join(", ") end' "$out")"
