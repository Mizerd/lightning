#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
ROOT="$(project_dir)"
load_versions

# One .rpm for Fedora and openSUSE: no Qt private-ABI import (validate-rpm.sh
# asserts it on the package).
LIGHTNING_INSTALL_TYPE=linux-rpm LIGHTNING_PORTABLE_QT_ABI=ON \
    "$SCRIPT_DIR/configure-build.sh"

TOPDIR="$ROOT/work/rpmbuild"
mkdir -p "$TOPDIR"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
rpmbuild -bb "$ROOT/packaging-ci/packaging/rpm/lightning.spec" \
    --define "_topdir $TOPDIR" \
    --define "pkg_version $RPM_VERSION" \
    --define "pkg_release $RPM_RELEASE" \
    --define "pkg_source_sha $SOURCE_SHA" \
    --define "stage_root $ROOT/work/stage"

mapfile -t packages < <(find "$TOPDIR/RPMS" -type f -name '*.rpm' -print)
(( ${#packages[@]} == 1 )) || die "expected exactly one binary RPM"
# The package is lightning-matrix; the published file keeps its established
# name so the signed manifest, the website and the docs need no change.
PACKAGE="$ROOT/dist/lightning-${RPM_VERSION}-${RPM_RELEASE}.x86_64.rpm"
cp "${packages[0]}" "$PACKAGE"
write_sha256 "$PACKAGE"
printf 'Built %s\n' "$PACKAGE"
