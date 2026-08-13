#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"

# macOS counterpart of prepare-pinned-source.sh.
#
# It differs in one deliberate way: it does NOT re-run resolve-version.sh.
# That script derives the snapshot version from the commit's UTC date via GNU
# `date -u -d`, which BSD date does not implement. Porting it would mean a
# second, independent version resolution on a different platform — and if the
# two ever disagreed (a commit near a UTC day boundary is enough), the macOS
# artifact would carry a different version string than the Linux artifacts built
# from the identical commit. Instead this consumes the version.env that
# resolve-source already produced on Linux, which stays the single authority.
#
# It also cleans work/lightning first. The container jobs get a fresh filesystem
# every time; the macOS shell executor reuses its build directory, so a source
# tree left behind by an earlier job would otherwise make the clone fail (or,
# worse, be silently reused).

require_var EXPECTED_SOURCE_SHA
[[ "$EXPECTED_SOURCE_SHA" =~ ^[0-9a-f]{40}$ ]] || die "EXPECTED_SOURCE_SHA is invalid"

ROOT="$(project_dir)"
SOURCE_DIR="$ROOT/work/lightning"

[[ -f "$ROOT/dist/version.env" ]] || \
    die "dist/version.env is missing — this job must consume the resolve-source artifacts"

# Confirm the inherited resolution matches what this pipeline pinned before any
# source is fetched.
load_versions
[[ "$SOURCE_SHA" == "$EXPECTED_SOURCE_SHA" ]] || \
    die "resolve-source pinned ${SOURCE_SHA} but this job expected ${EXPECTED_SOURCE_SHA}"

case "$SOURCE_DIR" in
    "$ROOT/work/lightning") ;;
    *) die "refusing to clean an unexpected source path" ;;
esac
rm -rf -- "$SOURCE_DIR"

"$SCRIPT_DIR/fetch-lightning-source.sh"

# fetch-lightning-source.sh rewrites dist/source-info.json; re-read it and prove
# the freshly cloned tree is the pinned commit, not merely whatever the ref
# points at now.
FETCHED_SHA="$(json_value "$ROOT/dist/source-info.json" resolved_sha)"
[[ "$FETCHED_SHA" == "$EXPECTED_SOURCE_SHA" ]] || \
    die "fetched source ${FETCHED_SHA} does not match the pinned ${EXPECTED_SOURCE_SHA}"

# version.env must survive untouched — nothing here may rewrite the authoritative
# version resolved on Linux.
load_versions
[[ "$SOURCE_SHA" == "$EXPECTED_SOURCE_SHA" ]] || die "version.env changed unexpectedly"

printf 'Prepared pinned Lightning source %s for macOS (version %s)\n' \
    "$EXPECTED_SOURCE_SHA" "$LOGICAL_VERSION"
