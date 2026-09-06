#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"

ROOT="$(project_dir)"
SOURCE_DIR="$ROOT/work/lightning"
INFO="$ROOT/dist/source-info.json"
[[ -f "$INFO" && -f "$SOURCE_DIR/CMakeLists.txt" ]] || die "source fetch metadata is missing"

# The authoritative application version is the one the source declares in CMake.
BASE_VERSION="$(sed -nE '/^[[:space:]]*VERSION[[:space:]]+[0-9]+\.[0-9]+\.[0-9]+/{s/.*VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+).*/\1/p;q}' "$SOURCE_DIR/CMakeLists.txt")"
[[ "$BASE_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "unable to resolve authoritative CMake project version"

SOURCE_SHA="$(json_value "$INFO" resolved_sha)"
SOURCE_REF="$(json_value "$INFO" requested_ref)"
SOURCE_TIME="$(json_value "$INFO" commit_time)"
EXACT_TAG="$(json_value "$INFO" exact_tag)"
SHORT_SHA="${SOURCE_SHA:0:7}"
GIT_DATE="$(date -u -d "$SOURCE_TIME" +%Y%m%d)"

# Publication mode is chosen explicitly. When it is enabled the pipeline must
# produce clean, stable release package metadata driven by RELEASE_VERSION, and
# the source must actually declare that version. Otherwise this is a snapshot
# build whose metadata carries Git provenance and is never published.
PUBLISHING=false
if [[ "${PUBLISH_PACKAGES:-false}" == "true" ]]; then
    PUBLISHING=true
fi

if [[ "$PUBLISHING" == true ]]; then
    require_var RELEASE_VERSION
    [[ "$RELEASE_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || \
        die "publication requires RELEASE_VERSION=X.Y.Z (no leading v)"
    [[ "$BASE_VERSION" == "$RELEASE_VERSION" ]] || \
        die "source CMake version ${BASE_VERSION} does not equal RELEASE_VERSION ${RELEASE_VERSION}"
    LOGICAL_VERSION="$RELEASE_VERSION"
    DEB_VERSION="$RELEASE_VERSION"
    RPM_VERSION="$RELEASE_VERSION"
    RPM_RELEASE="1"
    IS_EXACT_TAG=true
elif [[ -n "$EXACT_TAG" && "$SOURCE_REF" == "$EXACT_TAG" && "$EXACT_TAG" =~ ^v?${BASE_VERSION}$ ]]; then
    IS_EXACT_TAG=true
    LOGICAL_VERSION="$BASE_VERSION"
    DEB_VERSION="$BASE_VERSION"
    RPM_VERSION="$BASE_VERSION"
    RPM_RELEASE="1"
else
    IS_EXACT_TAG=false
    LOGICAL_VERSION="${BASE_VERSION}+git${GIT_DATE}.${SHORT_SHA}"
    DEB_VERSION="$LOGICAL_VERSION"
    RPM_VERSION="$BASE_VERSION"
    RPM_RELEASE="0.git${GIT_DATE}.${SHORT_SHA}"
fi

cat >"$ROOT/dist/version.env" <<EOF
BASE_VERSION=$BASE_VERSION
LOGICAL_VERSION=$LOGICAL_VERSION
DEB_VERSION=$DEB_VERSION
RPM_VERSION=$RPM_VERSION
RPM_RELEASE=$RPM_RELEASE
SOURCE_SHA=$SOURCE_SHA
SOURCE_REF=$(printf '%q' "$SOURCE_REF")
IS_EXACT_TAG=$IS_EXACT_TAG
PUBLISHING=$PUBLISHING
EOF

jq -n \
    --arg base_version "$BASE_VERSION" \
    --arg logical_version "$LOGICAL_VERSION" \
    --arg debian_version "$DEB_VERSION" \
    --arg rpm_version "$RPM_VERSION" \
    --arg rpm_release "$RPM_RELEASE" \
    --arg source_sha "$SOURCE_SHA" \
    --arg source_ref "$SOURCE_REF" \
    --argjson exact_tag "$IS_EXACT_TAG" \
    --argjson publishing "$PUBLISHING" \
    '{base_version:$base_version, logical_version:$logical_version,
      debian_version:$debian_version, rpm_version:$rpm_version,
      rpm_release:$rpm_release,
      source_sha:$source_sha, source_ref:$source_ref, exact_tag:$exact_tag,
      publishing:$publishing}' \
    >"$ROOT/dist/version.json"

printf 'Resolved package version: %s (source %s)\n' "$LOGICAL_VERSION" "$SOURCE_SHA"
