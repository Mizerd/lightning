#!/usr/bin/env bash
set -Eeuo pipefail

# Build the authoritative publication manifest: only files listed here may be
# uploaded to project 6. Entries are explicit, never a dist/ wildcard, so logs
# and metadata cannot leak into the registry.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
# shellcheck source=./scripts/gitlab-api.sh
source "$SCRIPT_DIR/gitlab-api.sh"

ROOT="$(project_dir)"
gitlab_api_init
release_contract_env

# Recorded URLs are canonical; uploads map them onto API_ROOT separately.
registry_base="${CANONICAL_API_ROOT}/packages/generic/${PACKAGE_NAME}/${PACKAGE_VERSION}"

# Windows and macOS names embed the 7-char source SHA, as build-windows.sh does.
short_sha="${SOURCE_SHA:0:7}"

# Asset names state the signing status from the shared switch in lib.sh.
WINDOWS_SUFFIX="$(windows_unsigned_suffix)"

declare -a formats=(deb deb-ubuntu rpm flatpak appimage snap windows-portable windows-msi windows-setup)
declare -A file_of arch_of name_of
file_of[deb]="$ROOT/dist/lightning_${PACKAGE_VERSION}_amd64.deb"
arch_of[deb]="amd64"
name_of[deb]="Lightning ${PACKAGE_VERSION} — Debian amd64"
# The Ubuntu deb is published but must not enter the signed update manifest:
# it reports the same `linux-deb` install type as the Debian deb, so a client
# could not choose between them.
file_of[deb-ubuntu]="$ROOT/dist/lightning_${PACKAGE_VERSION}_ubuntu2604_amd64.deb"
arch_of[deb-ubuntu]="amd64"
name_of[deb-ubuntu]="Lightning ${PACKAGE_VERSION} — Ubuntu 26.04 amd64"
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
file_of[windows-portable]="$ROOT/dist/windows/Lightning-${PACKAGE_VERSION}-${short_sha}-windows-x86_64-portable.zip"
arch_of[windows-portable]="x86_64"
name_of[windows-portable]="Lightning ${PACKAGE_VERSION} — Windows x86_64 portable${WINDOWS_SUFFIX}"
file_of[windows-msi]="$ROOT/dist/windows/Lightning-${PACKAGE_VERSION}-${short_sha}-windows-x86_64.msi"
arch_of[windows-msi]="x86_64"
name_of[windows-msi]="Lightning ${PACKAGE_VERSION} — Windows x86_64 MSI installer${WINDOWS_SUFFIX}"
file_of[windows-setup]="$ROOT/dist/windows/Lightning-${PACKAGE_VERSION}-${short_sha}-windows-x86_64-setup.exe"
arch_of[windows-setup]="x86_64"
name_of[windows-setup]="Lightning ${PACKAGE_VERSION} — Windows x86_64 setup EXE${WINDOWS_SUFFIX}"

# macOS is optional: it builds on a single Mac mini, and that host being
# offline must not block a release (the job is allow_failure too). It is a
# download-only asset (arm64, macOS 26+, ad-hoc signed, un-notarized) and is
# never in the signed update manifest, since the client cannot self-install it.
macos_zip="$ROOT/dist/macos/Lightning-${PACKAGE_VERSION}-${short_sha}-macos-arm64.zip"
if [[ -f "$macos_zip" ]]; then
    formats+=(macos-arm64)
    file_of[macos-arm64]="$macos_zip"
    arch_of[macos-arm64]="arm64"
    name_of[macos-arm64]="Lightning ${PACKAGE_VERSION} — macOS arm64 (unsigned, macOS 26+)"
    printf 'macOS bundle present; it will be published as a download-only asset\n' >&2
else
    printf 'NO macOS bundle at %s — publishing this release WITHOUT a macOS artifact\n' \
        "${macos_zip#"$ROOT"/}" >&2
fi

entries="[]"
sums_file="$ROOT/dist/SHA256SUMS"
: >"$sums_file"
for fmt in "${formats[@]}"; do
    path="${file_of[$fmt]}"
    [[ -f "$path" ]] || die "manifest input missing for ${fmt}: $(basename "$path")"
    filename="$(basename "$path")"
    sha256="$(sha256sum "$path" | cut -d' ' -f1)"
    size="$(wc -c <"$path" | tr -d ' ')"
    # Two-space form, verifiable with sha256sum -c.
    printf '%s  %s\n' "$sha256" "$filename" >>"$sums_file"
    entry="$(jq -n \
        --arg local_path "${path#"$ROOT"/}" \
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

# SHA256SUMS is itself a published asset; derived from the above, it is
# deterministic across re-runs.
sums_name="$(basename "$sums_file")"
sums_sha="$(sha256sum "$sums_file" | cut -d' ' -f1)"
sums_size="$(wc -c <"$sums_file" | tr -d ' ')"
sums_entry="$(jq -n \
    --arg local_path "${sums_file#"$ROOT"/}" \
    --arg filename "$sums_name" \
    --arg format "checksums" \
    --arg architecture "any" \
    --arg version "$PACKAGE_VERSION" \
    --arg source_sha "$SOURCE_SHA" \
    --arg sha256 "$sums_sha" \
    --argjson size "$sums_size" \
    --arg asset_name "Lightning ${PACKAGE_VERSION} — SHA-256 checksums" \
    --arg asset_path "/packages/${PACKAGE_VERSION}/${sums_name}" \
    --arg registry_url "${registry_base}/${sums_name}" \
    '{local_path:$local_path, filename:$filename, format:$format,
      architecture:$architecture, version:$version, source_sha:$source_sha,
      sha256:$sha256, size:$size, asset_name:$asset_name,
      asset_path:$asset_path, registry_url:$registry_url}')"
entries="$(jq -c --argjson e "$sums_entry" '. + [$e]' <<<"$entries")"

# Filenames must be unique in the registry version and release links.
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
