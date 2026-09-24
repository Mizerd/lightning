#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"

# macOS counterpart of prepare-pinned-source.sh.
#
# It does not re-run resolve-version.sh (which needs GNU `date -d`): a second
# resolution on another platform could disagree near a UTC day boundary. The
# version.env produced on Linux stays the single authority.
#
# It also cleans work/lightning first, because the shell executor reuses its
# build directory.

require_var EXPECTED_SOURCE_SHA
[[ "$EXPECTED_SOURCE_SHA" =~ ^[0-9a-f]{40}$ ]] || die "EXPECTED_SOURCE_SHA is invalid"

ROOT="$(project_dir)"
SOURCE_DIR="$ROOT/work/lightning"

[[ -f "$ROOT/dist/version.env" ]] || \
    die "dist/version.env is missing — this job must consume the resolve-source artifacts"

# Check the inherited resolution before fetching any source.
load_versions
[[ "$SOURCE_SHA" == "$EXPECTED_SOURCE_SHA" ]] || \
    die "resolve-source pinned ${SOURCE_SHA} but this job expected ${EXPECTED_SOURCE_SHA}"

case "$SOURCE_DIR" in
    "$ROOT/work/lightning") ;;
    *) die "refusing to clean an unexpected source path" ;;
esac
rm -rf -- "$SOURCE_DIR"

"$SCRIPT_DIR/fetch-lightning-source.sh"

# Prove the fresh clone is the pinned commit, not whatever the ref points at.
FETCHED_SHA="$(json_value "$ROOT/dist/source-info.json" resolved_sha)"
[[ "$FETCHED_SHA" == "$EXPECTED_SOURCE_SHA" ]] || \
    die "fetched source ${FETCHED_SHA} does not match the pinned ${EXPECTED_SOURCE_SHA}"

# version.env must be unchanged.
load_versions
[[ "$SOURCE_SHA" == "$EXPECTED_SOURCE_SHA" ]] || die "version.env changed unexpectedly"

printf 'Prepared pinned Lightning source %s for macOS (version %s)\n' \
    "$EXPECTED_SOURCE_SHA" "$LOGICAL_VERSION"
