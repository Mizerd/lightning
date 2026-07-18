#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(git rev-parse --show-toplevel)"
TEST_ROOT="$(mktemp -d)"
cleanup() { rm -rf "$TEST_ROOT"; }
trap cleanup EXIT
mkdir -p "$TEST_ROOT/dist"
sha=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
cat >"$TEST_ROOT/dist/version.env" <<EOF
BASE_VERSION=1.2.3
LOGICAL_VERSION=1.2.3
DEB_VERSION=1.2.3
RPM_VERSION=1.2.3
RPM_RELEASE=1
SOURCE_SHA=$sha
SOURCE_REF=v1.2.3
IS_EXACT_TAG=true
EOF
printf 'deb package\n' >"$TEST_ROOT/dist/lightning_1.2.3_amd64.deb"
printf 'rpm package\n' >"$TEST_ROOT/dist/lightning-1.2.3-1.x86_64.rpm"

export CI_PROJECT_DIR="$TEST_ROOT"
export CI_API_V4_URL=https://gitlab.example/api/v4
export CI_JOB_TOKEN=mock-secret-value
export LIGHTNING_PROJECT_ID=6
export SOURCE_REF=v1.2.3
export PUBLISH_PACKAGES=true
export CURL_BIN="$ROOT/tests/mock-curl.sh"
export MOCK_SOURCE_SHA="$sha"
export MOCK_DEB="$TEST_ROOT/dist/lightning_1.2.3_amd64.deb"
export MOCK_RPM="$TEST_ROOT/dist/lightning-1.2.3-1.x86_64.rpm"
export MOCK_CURL_LOG="$TEST_ROOT/curl.log"

run_publish() {
    : >"$MOCK_CURL_LOG"
    "$ROOT/scripts/publish-packages.sh" >"$TEST_ROOT/output.log" 2>&1
    if grep -q 'mock-secret-value' "$TEST_ROOT/output.log"; then
        printf 'credential leaked into output\n' >&2
        exit 1
    fi
}

MOCK_SCENARIO=success run_publish
[[ "$(jq '.files | length' "$TEST_ROOT/dist/publication.json")" == 2 ]]
[[ "$(grep -c '^PUT .*packages/generic/lightning/' "$MOCK_CURL_LOG")" == 2 ]]

MOCK_SCENARIO=identical run_publish
if grep -q '^PUT ' "$MOCK_CURL_LOG"; then
    printf 'identical package was uploaded again\n' >&2
    exit 1
fi

for scenario in conflict missing-release authentication insufficient network; do
    : >"$MOCK_CURL_LOG"
    if MOCK_SCENARIO="$scenario" "$ROOT/scripts/publish-packages.sh" >"$TEST_ROOT/output.log" 2>&1; then
        printf 'scenario unexpectedly succeeded: %s\n' "$scenario" >&2
        exit 1
    fi
    if grep -q 'mock-secret-value' "$TEST_ROOT/output.log"; then
        printf 'credential leaked into failure output\n' >&2
        exit 1
    fi
done

: >"$MOCK_CURL_LOG"
if MOCK_SCENARIO=partial-upload "$ROOT/scripts/publish-packages.sh" >"$TEST_ROOT/output.log" 2>&1; then
    printf 'partial upload scenario unexpectedly succeeded\n' >&2
    exit 1
fi
[[ "$(grep -c '^DELETE .*package_files/' "$MOCK_CURL_LOG")" == 2 ]]

MOCK_SCENARIO=success run_publish
deb_url="$(jq -r '.files[] | select(.format == "deb") | .url' "$TEST_ROOT/dist/publication.json")"
rpm_url="$(jq -r '.files[] | select(.format == "rpm") | .url' "$TEST_ROOT/dist/publication.json")"
export MOCK_EXISTING_LINKS="[{\"name\":\"Lightning 1.2.3 Debian amd64\",\"url\":\"$deb_url\",\"link_type\":\"package\"},{\"name\":\"Lightning 1.2.3 RPM x86_64\",\"url\":\"$rpm_url\",\"link_type\":\"package\"}]"
export MOCK_CONFLICTING_LINKS='[{"name":"Lightning 1.2.3 Debian amd64","url":"https://wrong.example/file","link_type":"package"}]'

: >"$MOCK_CURL_LOG"
MOCK_SCENARIO=success "$ROOT/scripts/attach-release-assets.sh" >"$TEST_ROOT/output.log" 2>&1
[[ "$(grep -c '^POST .*assets/links' "$MOCK_CURL_LOG")" == 2 ]]

: >"$MOCK_CURL_LOG"
MOCK_SCENARIO=duplicate-link "$ROOT/scripts/attach-release-assets.sh" >"$TEST_ROOT/output.log" 2>&1
if grep -q '^POST ' "$MOCK_CURL_LOG"; then
    printf 'duplicate release link was created\n' >&2
    exit 1
fi

: >"$MOCK_CURL_LOG"
if MOCK_SCENARIO=conflicting-link "$ROOT/scripts/attach-release-assets.sh" >"$TEST_ROOT/output.log" 2>&1; then
    printf 'conflicting release link unexpectedly succeeded\n' >&2
    exit 1
fi
if grep -q 'mock-secret-value' "$TEST_ROOT/output.log"; then
    printf 'credential leaked into link failure output\n' >&2
    exit 1
fi
printf 'Publication and release-link API tests passed\n'
