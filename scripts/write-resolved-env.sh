#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
ROOT="$(project_dir)"
load_versions
[[ "$SOURCE_SHA" =~ ^[0-9a-f]{40}$ ]] || die "resolved source SHA is invalid"
printf 'EXPECTED_SOURCE_SHA=%s\n' "$SOURCE_SHA" >"$ROOT/dist/resolved.env"
printf 'Pinned all package builds to Lightning source %s\n' "$SOURCE_SHA"
