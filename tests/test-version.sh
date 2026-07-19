#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(git rev-parse --show-toplevel)"
TEST_ROOT="$(mktemp -d)"
cleanup() { rm -rf "$TEST_ROOT"; }
trap cleanup EXIT
mkdir -p "$TEST_ROOT/work/lightning" "$TEST_ROOT/dist"
printf 'project(matrix-client\n    VERSION 1.2.3\n    LANGUAGES CXX\n)\n' >"$TEST_ROOT/work/lightning/CMakeLists.txt"

write_info() {
    local ref="$1" tag="$2"
    jq -n --arg ref "$ref" --arg tag "$tag" \
        '{requested_ref:$ref,resolved_sha:"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          commit_time:"2026-07-18T10:00:00+00:00",exact_tag:$tag}' \
        >"$TEST_ROOT/dist/source-info.json"
}

write_info v1.2.3 v1.2.3
CI_PROJECT_DIR="$TEST_ROOT" "$ROOT/scripts/resolve-version.sh"
grep -qx 'LOGICAL_VERSION=1.2.3' "$TEST_ROOT/dist/version.env"
grep -qx 'IS_EXACT_TAG=true' "$TEST_ROOT/dist/version.env"

write_info main ''
CI_PROJECT_DIR="$TEST_ROOT" "$ROOT/scripts/resolve-version.sh"
grep -qx 'LOGICAL_VERSION=1.2.3+git20260718.aaaaaaa' "$TEST_ROOT/dist/version.env"
grep -qx 'RPM_RELEASE=0.git20260718.aaaaaaa' "$TEST_ROOT/dist/version.env"
grep -qx 'IS_EXACT_TAG=false' "$TEST_ROOT/dist/version.env"

write_info v1.2.4 v1.2.4
CI_PROJECT_DIR="$TEST_ROOT" "$ROOT/scripts/resolve-version.sh"
grep -qx 'IS_EXACT_TAG=false' "$TEST_ROOT/dist/version.env"

# Publishing mode forces the clean RELEASE_VERSION even from a bare SHA ref.
write_info 1111111111111111111111111111111111111111 ''
CI_PROJECT_DIR="$TEST_ROOT" PUBLISH_PACKAGES=true RELEASE_VERSION=1.2.3 \
    "$ROOT/scripts/resolve-version.sh"
grep -qx 'LOGICAL_VERSION=1.2.3' "$TEST_ROOT/dist/version.env"
grep -qx 'DEB_VERSION=1.2.3' "$TEST_ROOT/dist/version.env"
grep -qx 'RPM_RELEASE=1' "$TEST_ROOT/dist/version.env"
grep -qx 'IS_EXACT_TAG=true' "$TEST_ROOT/dist/version.env"
grep -qx 'PUBLISHING=true' "$TEST_ROOT/dist/version.env"

# Publishing with a RELEASE_VERSION the source does not declare must fail.
write_info v9.9.9 v9.9.9
if CI_PROJECT_DIR="$TEST_ROOT" PUBLISH_PACKAGES=true RELEASE_VERSION=9.9.9 \
    "$ROOT/scripts/resolve-version.sh" >/dev/null 2>&1; then
    printf 'source/RELEASE_VERSION mismatch unexpectedly succeeded\n' >&2
    exit 1
fi

# Publishing without RELEASE_VERSION must fail.
write_info 1111111111111111111111111111111111111111 ''
if CI_PROJECT_DIR="$TEST_ROOT" PUBLISH_PACKAGES=true \
    "$ROOT/scripts/resolve-version.sh" >/dev/null 2>&1; then
    printf 'publishing without RELEASE_VERSION unexpectedly succeeded\n' >&2
    exit 1
fi
printf 'Version contract tests passed\n'
