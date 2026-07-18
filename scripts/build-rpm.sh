#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
ROOT="$(project_dir)"
load_versions

"$SCRIPT_DIR/configure-build.sh"

# The Qt6 CMake finalization adds an $ORIGIN-relative RPATH, but every runtime
# dependency is a system library in a standard search path, so the RPATH is
# superfluous. rpmlint treats it as an error (binary-or-shlib-defines-rpath),
# so remove it from the staged binary before packaging.
patchelf --remove-rpath "$ROOT/work/stage/usr/bin/matrix-client"

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
