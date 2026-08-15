#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
ROOT="$(project_dir)"
load_versions

LIGHTNING_INSTALL_TYPE=linux-deb "$SCRIPT_DIR/configure-build.sh"

STAGE="$ROOT/work/stage"
PKGROOT="$ROOT/work/deb-root"
CONTROL="$PKGROOT/DEBIAN"
mkdir -p "$CONTROL" "$ROOT/dist" "$ROOT/work/debian"
cp -a "$STAGE/." "$PKGROOT/"

# Strip the release binaries (lintian error: unstripped-binary-or-object).
# The update helper is a second shipped ELF and lintian holds it to the same
# rule, so leaving it unstripped would fail validate-deb's --fail-on error.
strip "$PKGROOT/usr/bin/matrix-client"
strip "$PKGROOT/usr/bin/lightning-updater"

install -Dm0644 "$ROOT/packaging/common/copyright" \
    "$PKGROOT/usr/share/doc/lightning/copyright"

# Debian requires a changelog (lintian error: no-changelog). This is a native
# package (no Debian revision), so it must be shipped as changelog.gz compressed
# at maximum level. Reproducible content beyond the wall-clock build date.
MAINTAINER="$(sed -n 's/^Maintainer:[[:space:]]*//p' "$ROOT/packaging/deb/control")"
{
    printf 'lightning (%s) unstable; urgency=medium\n\n' "$DEB_VERSION"
    printf '  * Automated package build from Lightning source %s.\n\n' "$SOURCE_SHA"
    printf ' -- %s  %s\n' "$MAINTAINER" "$(date -R)"
} >"$ROOT/work/changelog"
install -d "$PKGROOT/usr/share/doc/lightning"
gzip -9nc "$ROOT/work/changelog" >"$PKGROOT/usr/share/doc/lightning/changelog.gz"

# dpkg-shlibdeps expects a *source-package* debian/control (first stanza with a
# Source field, then a binary Package stanza), which differs from the binary
# DEBIAN/control shipped in the .deb. Synthesize a minimal one for it here.
cat >"$ROOT/work/debian/control" <<'EOF'
Source: lightning

Package: lightning
Architecture: amd64
EOF
# Run dpkg-shlibdeps from work/ and keep its diagnostics visible so a
# resolution failure is not silently swallowed.
# Both shipped ELF executables are analysed. The helper links only Qt6Core (and
# zlib), a subset of what the application already needs — but deriving that from
# the binary is what makes it true rather than assumed, and it is what would
# catch a helper that grows a dependency the package does not declare.
SHLIBS_OUT="$ROOT/work/shlibdeps.out"
if ! ( cd "$ROOT/work" && dpkg-shlibdeps -O \
        -e "$PKGROOT/usr/bin/matrix-client" \
        -e "$PKGROOT/usr/bin/lightning-updater" ) >"$SHLIBS_OUT" 2>&1; then
    printf 'dpkg-shlibdeps failed:\n' >&2
    cat "$SHLIBS_OUT" >&2
    die "dpkg-shlibdeps could not resolve runtime dependencies"
fi
SHLIBS="$(sed -n 's/^shlibs:Depends=//p' "$SHLIBS_OUT")"
[[ -n "$SHLIBS" ]] || die "dpkg-shlibdeps did not determine runtime dependencies"
# QML imports are loaded dynamically and therefore cannot be discovered from
# ELF NEEDED entries by dpkg-shlibdeps. Keep this list aligned with the
# production QML import scan; the clean install/startup test enforces it.
QML_DEPENDS="qml6-module-qtquick, qml6-module-qtquick-controls, qml6-module-qtquick-dialogs, qml6-module-qtquick-effects, qml6-module-qtquick-layouts, qml6-module-qtquick-window, qml6-module-qtmultimedia"

{
    cat "$ROOT/packaging/deb/control"
    printf 'Version: %s\n' "$DEB_VERSION"
    printf 'Depends: %s, %s\n' "$SHLIBS" "$QML_DEPENDS"
} >"$CONTROL/control"
install -m0755 "$ROOT/packaging/deb/postinst" "$CONTROL/postinst"
install -m0755 "$ROOT/packaging/deb/postrm" "$CONTROL/postrm"

PACKAGE="$ROOT/dist/lightning_${DEB_VERSION}_amd64.deb"
dpkg-deb --build --root-owner-group "$PKGROOT" "$PACKAGE"
write_sha256 "$PACKAGE"
printf 'Built %s\n' "$PACKAGE"
