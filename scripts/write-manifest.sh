#!/usr/bin/env bash
set -Eeuo pipefail

# Build the single authoritative publication manifest. Only files listed here
# may be uploaded to project 6. Entries are declared explicitly (never by an
# unrestricted dist/ wildcard) so logs, checksums, and metadata cannot leak into
# the package registry. A future package format is added by appending one more
# explicit entry below; the registry version layout does not change.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
# shellcheck source=./scripts/gitlab-api.sh
source "$SCRIPT_DIR/gitlab-api.sh"

ROOT="$(project_dir)"
gitlab_api_init
release_contract_env

# Durable registry URLs are always canonical/public (release links point
# here); the upload/verify requests map them onto API_ROOT separately.
registry_base="${CANONICAL_API_ROOT}/packages/generic/${PACKAGE_NAME}/${PACKAGE_VERSION}"

# Declared package files for this release. Append future formats here only.
declare -a formats=(deb rpm flatpak appimage snap)
declare -A file_of arch_of name_of
file_of[deb]="$ROOT/dist/lightning_${PACKAGE_VERSION}_amd64.deb"
arch_of[deb]="amd64"
name_of[deb]="Lightning ${PACKAGE_VERSION} — Debian amd64"
file_of[rpm]="$ROOT/dist/lightning-${PACKAGE_VERSION}-1.x86_64.rpm"
arch_of[rpm]="x86_64"
name_of[rpm]="Lightning ${PACKAGE_VERSION} — RPM x86_64"
file_of[flatpak]="$ROOT/dist/lightning_${PACKAGE_VERSION}_amd64.flatpak"
arch_of[flatpak]="amd64"
name_of[flatpak]="Lightning ${PACKAGE_VERSION} — Flatpak amd64"
file_of[appimage]="$ROOT/dist/Lightning-${PACKAGE_VERSION}-x86_64.AppImage"
arch_of[appimage]="x86_64"
name_of[appimage]="Lightning ${PACKAGE_VERSION} — AppImage x86_64"
file_of[snap]="$ROOT/dist/lightning_${PACKAGE_VERSION}_amd64.snap"
arch_of[snap]="amd64"
name_of[snap]="Lightning ${PACKAGE_VERSION} — Snap amd64"

entries="[]"
for fmt in "${formats[@]}"; do
    path="${file_of[$fmt]}"
    [[ -f "$path" ]] || die "manifest input missing for ${fmt}: $(basename "$path")"
    filename="$(basename "$path")"
    sha256="$(sha256sum "$path" | cut -d' ' -f1)"
    size="$(wc -c <"$path" | tr -d ' ')"
    entry="$(jq -n \
        --arg local_path "dist/${filename}" \
        --arg filename "$filename" \
        --arg format "$fmt" \
        --arg architecture "${arch_of[$fmt]}" \
        --arg version "$PACKAGE_VERSION" \
        --arg source_sha "$SOURCE_SHA" \
        --arg sha256 "$sha256" \
        --argjson size "$size" \
        --arg asset_name "${name_of[$fmt]}" \
        --arg asset_path "/packages/${PACKAGE_VERSION}/${filename}" \
        --arg registry_url "${registry_base}/${filename}" \
        '{local_path:$local_path, filename:$filename, format:$format,
          architecture:$architecture, version:$version, source_sha:$source_sha,
          sha256:$sha256, size:$size, asset_name:$asset_name,
          asset_path:$asset_path, registry_url:$registry_url}')"
    entries="$(jq -c --argjson e "$entry" '. + [$e]' <<<"$entries")"
done

# Every manifest filename must be unique; a duplicate would collide in the
# registry version and in release links.
dupes="$(jq -r '[.[].filename] | (length) as $n | unique | length as $u | $n - $u' <<<"$entries")"
[[ "$dupes" == 0 ]] || die "manifest contains duplicate filenames"

jq -n \
    --arg package_name "$PACKAGE_NAME" \
    --arg version "$PACKAGE_VERSION" \
    --arg source_sha "$SOURCE_SHA" \
    --arg release_tag "$RELEASE_TAG" \
    --arg release_action "$RELEASE_ACTION" \
    --argjson target_project_id "${TARGET_PROJECT_ID}" \
    --argjson entries "$entries" \
    '{package_name:$package_name, version:$version, source_sha:$source_sha,
      release_tag:$release_tag, release_action:$release_action,
      target_project_id:$target_project_id, entries:$entries}' \
    >"$ROOT/dist/manifest.json"

printf 'Publication manifest lists %s file(s) for %s %s\n' \
    "$(jq '.entries | length' "$ROOT/dist/manifest.json")" "$PACKAGE_NAME" "$PACKAGE_VERSION"
