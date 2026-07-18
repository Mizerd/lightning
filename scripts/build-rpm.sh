#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
ROOT="$(project_dir)"
load_versions

"$SCRIPT_DIR/configure-build.sh"

TOPDIR="$ROOT/work/rpmbuild"
mkdir -p "$TOPDIR"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
rpmbuild -bb "$ROOT/packaging/rpm/lightning.spec" \
    --define "_topdir $TOPDIR" \
    --define "pkg_version $RPM_VERSION" \
    --define "pkg_release $RPM_RELEASE" \
    --define "stage_root $ROOT/work/stage"

mapfile -t packages < <(find "$TOPDIR/RPMS" -type f -name '*.rpm' -print)
(( ${#packages[@]} == 1 )) || die "expected exactly one binary RPM"
cp "${packages[0]}" "$ROOT/dist/"
PACKAGE="$ROOT/dist/$(basename "${packages[0]}")"
write_sha256 "$PACKAGE"
printf 'Built %s\n' "$PACKAGE"
