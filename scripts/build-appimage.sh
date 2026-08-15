#!/usr/bin/env bash
# Build the Lightning AppImage from the shared Release staged tree.
#
# Reuses configure-build.sh (same build, GIF-key handling, and RPATH
# removal as the deb/rpm), then bundles Qt libraries, platform plugins, and
# the dynamic QML modules with pinned linuxdeploy releases. AppImage is
# unsandboxed by design; the bundle must be self-contained apart from the
# documented base-system excludelist (glibc, GL, X11).
#
# Outputs:
#   dist/Lightning-<LOGICAL_VERSION>-x86_64.AppImage
#   dist/lightning-appdir-<LOGICAL_VERSION>.tar.zst   (consumed by build-snap)
set -euo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lib.sh
. "$SCRIPT_DIR/lib.sh"
ROOT=$(project_dir)
cd "$ROOT"

LIGHTNING_INSTALL_TYPE=linux-appimage "$SCRIPT_DIR/configure-build.sh"
load_versions

STAGE="$ROOT/work/stage"
APPDIR="$ROOT/work/appdir"
TOOLS="$ROOT/work/appimage-tools"
OUT="dist/Lightning-${LOGICAL_VERSION}-x86_64.AppImage"

# Pinned bundling tools (dated upstream releases, checksum-verified).
LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20240109-1/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_SHA256=c86d6540f1df31061f02f539a2d3445f8d7f85cc3994eee1e74cd1ac97b76df0
PLUGIN_QT_URL="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20240109-1/linuxdeploy-plugin-qt-x86_64.AppImage"
PLUGIN_QT_SHA256=f53349093d333a6558c560844c1a0f64a3b6bd077bf02740af3ad3dbb8827433

fetch_tool() {
    local url=$1 sha=$2 out=$3
    if [ ! -f "$out" ] || ! echo "$sha  $out" | sha256sum -c --quiet 2>/dev/null; then
        curl -fsSL -o "$out" "$url"
    fi
    echo "$sha  $out" | sha256sum -c --quiet || die "checksum mismatch for $out"
    chmod +x "$out"
}
mkdir -p "$TOOLS" dist
fetch_tool "$LINUXDEPLOY_URL" "$LINUXDEPLOY_SHA256" "$TOOLS/linuxdeploy"
fetch_tool "$PLUGIN_QT_URL" "$PLUGIN_QT_SHA256" "$TOOLS/linuxdeploy-plugin-qt"

rm -rf "$APPDIR"
mkdir -p "$APPDIR"
cp -a "$STAGE/usr" "$APPDIR/usr"
strip --strip-unneeded "$APPDIR/usr/bin/matrix-client"
test -x "$APPDIR/usr/bin/lightning-updater" || die "update helper missing from the staged tree"
strip --strip-unneeded "$APPDIR/usr/bin/lightning-updater"

# The QML runtime modules ELF scanning cannot discover. Keep aligned with
# the deb QML_DEPENDS list in build-deb.sh (source of both: the production
# QML import scan).
export QML_SOURCES_PATHS="$ROOT/work/lightning/qml"
# Offscreen is required so the validation job (and headless users) can run
# the bundle; wayland keeps the primary platform native.
export EXTRA_PLATFORM_PLUGINS="libqoffscreen.so;libqwayland-generic.so;libqwayland-egl.so"
export VERSION="$LOGICAL_VERSION"
export LDAI_OUTPUT="$OUT"
export APPIMAGE_EXTRACT_AND_RUN=1

# linuxdeploy-plugin-qt finds Qt through qmake6.
command -v qmake6 >/dev/null || die "qmake6 missing in build image"
export QMAKE=$(command -v qmake6)

# linuxdeploy's excludelist keeps libgpg-error on the host but bundles
# libgcrypt, and the two are version-locked (trixie's libgcrypt needs
# gpgrt_* symbols older distros lack — seen live on Ubuntu 24.04 in
# validate-snap). Ship the exact libgpg-error the bundled libgcrypt was
# built against.
# BOTH executables are declared. linuxdeploy only resolves libraries for, and
# rewrites the RPATH of, the executables it is told about — an unlisted binary
# is copied along by the usr/ tree and then fails to start on any host without
# Qt6, which is precisely the host an AppImage exists for. The helper is the
# process that installs the update, so a helper that cannot start turns the
# whole feature into a silent failure at the last step.
"$TOOLS/linuxdeploy" --appdir "$APPDIR" \
    --desktop-file "$APPDIR/usr/share/applications/lightning.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/192x192/apps/lightning.png" \
    --executable "$APPDIR/usr/bin/matrix-client" \
    --executable "$APPDIR/usr/bin/lightning-updater" \
    --library /lib/x86_64-linux-gnu/libgpg-error.so.0 \
    --plugin qt \
    --output appimage

test -s "$OUT" || die "AppImage not produced at $OUT"
write_sha256 "$OUT"

# Hand the fully bundled AppDir to the snap job so it does not recompile.
tar -C "$ROOT/work" -I 'zstd -T0 -6' -cf \
    "dist/lightning-appdir-${LOGICAL_VERSION}.tar.zst" appdir
echo "built $OUT"
