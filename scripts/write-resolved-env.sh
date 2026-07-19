#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
ROOT="$(project_dir)"
load_versions
[[ "$SOURCE_SHA" =~ ^[0-9a-f]{40}$ ]] || die "resolved source SHA is invalid"
printf 'EXPECTED_SOURCE_SHA=%s\n' "$SOURCE_SHA" >"$ROOT/dist/resolved.env"

# For a create-mode release, capture a tracked release-notes file from the
# source checkout now, while it is available. RELEASE_NOTES_B64 (decoded later
# in finalize-release) takes precedence and does not need this capture.
if [[ "${PUBLISH_PACKAGES:-false}" == true && "${RELEASE_ACTION:-}" == create && -z "${RELEASE_NOTES_B64:-}" ]]; then
    notes="$ROOT/work/lightning/docs/releases/v${RELEASE_VERSION:-}.md"
    if [[ -n "${RELEASE_VERSION:-}" && -f "$notes" ]]; then
        cp "$notes" "$ROOT/dist/release-notes.md"
        printf 'Captured tracked release notes docs/releases/v%s.md\n' "$RELEASE_VERSION"
    fi
fi

printf 'Pinned all package builds to Lightning source %s\n' "$SOURCE_SHA"
