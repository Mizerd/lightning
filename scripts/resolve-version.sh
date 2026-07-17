#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"

ROOT="$(project_dir)"
SOURCE_DIR="$ROOT/work/lightning"
INFO="$ROOT/dist/source-info.json"
[[ -f "$INFO" && -f "$SOURCE_DIR/CMakeLists.txt" ]] || die "source fetch metadata is missing"

BASE_VERSION="$(sed -nE '/^[[:space:]]*VERSION[[:space:]]+[0-9]+\.[0-9]+\.[0-9]+/{s/.*VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+).*/\1/p;q}' "$SOURCE_DIR/CMakeLists.txt")"
[[ "$BASE_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "unable to resolve authoritative CMake project version"

SOURCE_SHA="$(json_value "$INFO" resolved_sha)"
SOURCE_REF="$(json_value "$INFO" requested_ref)"
SOURCE_TIME="$(json_value "$INFO" commit_time)"
EXACT_TAG="$(json_value "$INFO" exact_tag)"
SHORT_SHA="${SOURCE_SHA:0:7}"
GIT_DATE="$(date -u -d "$SOURCE_TIME" +%Y%m%d)"

IS_EXACT_TAG=false
if [[ -n "$EXACT_TAG" && "$SOURCE_REF" == "$EXACT_TAG" && "$EXACT_TAG" =~ ^v?${BASE_VERSION}$ ]]; then
    IS_EXACT_TAG=true
    LOGICAL_VERSION="$BASE_VERSION"
    DEB_VERSION="$BASE_VERSION"
    RPM_VERSION="$BASE_VERSION"
    RPM_RELEASE="1"
    NIX_VERSION="$BASE_VERSION"
else
    LOGICAL_VERSION="${BASE_VERSION}+git${GIT_DATE}.${SHORT_SHA}"
    DEB_VERSION="$LOGICAL_VERSION"
    RPM_VERSION="$BASE_VERSION"
    RPM_RELEASE="0.git${GIT_DATE}.${SHORT_SHA}"
    NIX_VERSION="$LOGICAL_VERSION"
fi

cat >"$ROOT/dist/version.env" <<EOF
BASE_VERSION=$BASE_VERSION
LOGICAL_VERSION=$LOGICAL_VERSION
DEB_VERSION=$DEB_VERSION
RPM_VERSION=$RPM_VERSION
RPM_RELEASE=$RPM_RELEASE
NIX_VERSION=$NIX_VERSION
SOURCE_SHA=$SOURCE_SHA
SOURCE_REF=$SOURCE_REF
IS_EXACT_TAG=$IS_EXACT_TAG
EOF

jq -n \
    --arg base_version "$BASE_VERSION" \
    --arg logical_version "$LOGICAL_VERSION" \
    --arg debian_version "$DEB_VERSION" \
    --arg rpm_version "$RPM_VERSION" \
    --arg rpm_release "$RPM_RELEASE" \
    --arg nix_version "$NIX_VERSION" \
    --arg source_sha "$SOURCE_SHA" \
    --arg source_ref "$SOURCE_REF" \
    --argjson exact_tag "$IS_EXACT_TAG" \
    '{base_version:$base_version, logical_version:$logical_version,
      debian_version:$debian_version, rpm_version:$rpm_version,
      rpm_release:$rpm_release, nix_version:$nix_version,
      source_sha:$source_sha, source_ref:$source_ref, exact_tag:$exact_tag}' \
    >"$ROOT/dist/version.json"

printf 'Resolved package version: %s (source %s)\n' "$LOGICAL_VERSION" "$SOURCE_SHA"
