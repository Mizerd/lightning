#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
ROOT="$(project_dir)"
load_versions

"$SCRIPT_DIR/configure-build.sh"

STAGE="$ROOT/work/stage"
PKGROOT="$ROOT/work/deb-root"
CONTROL="$PKGROOT/DEBIAN"
mkdir -p "$CONTROL" "$ROOT/dist" "$ROOT/work/debian"
cp -a "$STAGE/." "$PKGROOT/"
install -Dm0644 "$ROOT/packaging/common/copyright" \
    "$PKGROOT/usr/share/doc/lightning/copyright"

cp "$ROOT/packaging/deb/control" "$ROOT/work/debian/control"
# dpkg-shlibdeps needs a debian/ working tree; run it from work/ and keep its
# diagnostics visible so a resolution failure is not silently swallowed.
SHLIBS_OUT="$ROOT/work/shlibdeps.out"
if ! ( cd "$ROOT/work" && dpkg-shlibdeps -O -e "$PKGROOT/usr/bin/matrix-client" ) >"$SHLIBS_OUT" 2>&1; then
    printf 'dpkg-shlibdeps failed:\n' >&2
    cat "$SHLIBS_OUT" >&2
    die "dpkg-shlibdeps could not resolve runtime dependencies"
fi
SHLIBS="$(sed -n 's/^shlibs:Depends=//p' "$SHLIBS_OUT")"
[[ -n "$SHLIBS" ]] || die "dpkg-shlibdeps did not determine runtime dependencies"

{
    cat "$ROOT/packaging/deb/control"
    printf 'Version: %s\n' "$DEB_VERSION"
    printf 'Depends: %s\n' "$SHLIBS"
} >"$CONTROL/control"
install -m0755 "$ROOT/packaging/deb/postinst" "$CONTROL/postinst"
install -m0755 "$ROOT/packaging/deb/postrm" "$CONTROL/postrm"

PACKAGE="$ROOT/dist/lightning_${DEB_VERSION}_amd64.deb"
dpkg-deb --build --root-owner-group "$PKGROOT" "$PACKAGE"
sha256sum "$PACKAGE" >"$PACKAGE.sha256"
printf 'Built %s\n' "$PACKAGE"
