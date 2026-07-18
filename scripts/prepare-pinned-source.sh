#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
require_var EXPECTED_SOURCE_SHA
[[ "$EXPECTED_SOURCE_SHA" =~ ^[0-9a-f]{40}$ ]] || die "EXPECTED_SOURCE_SHA is invalid"
ROOT="$(project_dir)"
rm -f "$ROOT/dist/source-info.json" "$ROOT/dist/version.env" "$ROOT/dist/version.json"
"$SCRIPT_DIR/fetch-lightning-source.sh"
"$SCRIPT_DIR/resolve-version.sh"
load_versions
[[ "$SOURCE_SHA" == "$EXPECTED_SOURCE_SHA" ]] || die "package source SHA differs from resolver"
