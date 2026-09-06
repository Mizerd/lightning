#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
[[ $# -eq 1 ]] || die "usage: write-build-info.sh <deb|rpm>"
FORMAT="$1"
[[ "$FORMAT" =~ ^(deb|rpm)$ ]] || die "unsupported package format: $FORMAT"
ROOT="$(project_dir)"
load_versions

case "$FORMAT" in
    deb) PACKAGE_VERSION="$DEB_VERSION"; PACKAGE_ARCHITECTURE="amd64" ;;
    rpm) PACKAGE_VERSION="${RPM_VERSION}-${RPM_RELEASE}"; PACKAGE_ARCHITECTURE="x86_64" ;;
esac

mapfile -t artifacts < <(find "$ROOT/dist" -maxdepth 1 -type f \
    \( -name '*.deb' -o -name '*.rpm' \) -printf '%f\n' | sort)
(( ${#artifacts[@]} > 0 )) || die "no distributable artifact found"
FILES_JSON="$(printf '%s\n' "${artifacts[@]}" | jq -R . | jq -s .)"
CHECKSUMS_JSON="$(cd "$ROOT/dist" && for file in "${artifacts[@]}"; do
    sha256sum "$file" | jq -R 'split("  ") | {filename:.[1], sha256:.[0]}'
done | jq -s .)"

SOURCE_TIME="$(json_value "$ROOT/dist/source-info.json" commit_time)"
EXACT_TAG="$(json_value "$ROOT/dist/source-info.json" exact_tag)"
QT_VERSION="$(pkg-config --modversion Qt6Core 2>/dev/null || true)"
RUST_VERSION="$(rustc --version 2>/dev/null || true)"
CMAKE_VERSION="$(cmake --version 2>/dev/null | head -1 || true)"
BUILD_TIMESTAMP="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

jq -n \
    --arg project_name "lightning-deploy" \
    --arg pipeline_project_url "${CI_PROJECT_URL:-local}" \
    --arg pipeline_id "${CI_PIPELINE_ID:-local}" \
    --arg job_id "${CI_JOB_ID:-local}" \
    --arg job_url "${CI_JOB_URL:-local}" \
    --arg package_format "$FORMAT" \
    --arg repository "${LIGHTNING_REPOSITORY:-https://gitlab.smetonis.net/Mizerd/lightning.git}" \
    --arg requested_ref "$SOURCE_REF" \
    --arg source_sha "$SOURCE_SHA" \
    --arg source_commit_time "$SOURCE_TIME" \
    --arg source_tag "$EXACT_TAG" \
    --arg logical_version "$LOGICAL_VERSION" \
    --arg package_version "$PACKAGE_VERSION" \
    --arg architecture "$PACKAGE_ARCHITECTURE" \
    --arg build_type "${BUILD_TYPE:-Release}" \
    --arg runner_description "${CI_RUNNER_DESCRIPTION:-local}" \
    --arg runner_id "${CI_RUNNER_ID:-local}" \
    --arg runner_tags "${CI_RUNNER_TAGS:-local}" \
    --arg base_image "${BASE_IMAGE:-local}" \
    --arg qt_version "$QT_VERSION" \
    --arg cmake_version "$CMAKE_VERSION" \
    --arg rust_version "$RUST_VERSION" \
    --arg build_timestamp "$BUILD_TIMESTAMP" \
    --argjson files "$FILES_JSON" \
    --argjson checksums "$CHECKSUMS_JSON" \
    '{project_name:$project_name, pipeline_project_url:$pipeline_project_url,
      pipeline_id:$pipeline_id, job_id:$job_id, job_url:$job_url,
      package_format:$package_format, lightning_repository:$repository,
      requested_lightning_ref:$requested_ref, resolved_source_sha:$source_sha,
      source_commit_time:$source_commit_time, source_tag:(if $source_tag == "" then null else $source_tag end),
      logical_version:$logical_version, package_version:$package_version,
      architecture:$architecture, build_type:$build_type,
      rust_sdk_backend:true, e2ee_enabled:true,
      runner:{description:$runner_description,id:$runner_id,tags:$runner_tags},
      base_image:$base_image,
      toolchain:{qt:$qt_version,cmake:$cmake_version,rust:$rust_version},
      build_timestamp_utc:$build_timestamp, artifacts:$files, checksums:$checksums}' \
    >"$ROOT/dist/build-info.json"
