#!/usr/bin/env bash
# Assemble the Lightning snap from the AppImage job's fully bundled AppDir.
#
# See packaging/snap/snap.yaml.in for why snapcraft/snapd are not used on
# this runner fleet: a snap is a squashfs image with meta/snap.yaml, and the
# AppDir already contains the app, Qt, plugins, and QML modules built by
# configure-build.sh (same GIF-key handling as every other format).
#
# Input:  dist/lightning-appdir-<LOGICAL_VERSION>.tar.zst (build-appimage)
# Output: dist/lightning_<LOGICAL_VERSION>_amd64.snap
set -euo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lib.sh
. "$SCRIPT_DIR/lib.sh"
ROOT=$(project_dir)
cd "$ROOT"
load_versions

APPDIR_TAR="dist/lightning-appdir-${LOGICAL_VERSION}.tar.zst"
SNAP_WORK="$ROOT/work/snap"
TREE="$SNAP_WORK/prime"
OUT="dist/lightning_${LOGICAL_VERSION}_amd64.snap"
TEMPLATE="$ROOT/packaging/snap/snap.yaml.in"

test -f "$APPDIR_TAR" || die "missing $APPDIR_TAR (build-appimage artifact)"
test -f "$TEMPLATE" || die "snap.yaml template missing"

rm -rf "$SNAP_WORK"
mkdir -p "$SNAP_WORK"
tar -C "$SNAP_WORK" -I zstd -xf "$APPDIR_TAR"
test -x "$SNAP_WORK/appdir/usr/bin/matrix-client" || die "AppDir payload incomplete"
# The call media plugins ride in from the AppImage job's AppDir. A snap with
# none is a snap that installs, launches and then refuses every call, so the
# absence is fatal here rather than at a user.
gst_bundled=$(find "$SNAP_WORK/appdir/usr/lib/gstreamer-1.0" -maxdepth 1 -name '*.so' 2>/dev/null | wc -l)
[ "$gst_bundled" -ge 20 ] || \
    die "the AppDir carries only $gst_bundled GStreamer plugins; the snap would refuse every call"
test -f "$SNAP_WORK/appdir/usr/lib/gstreamer-1.0/libgstwebrtc.so" || \
    die "the AppDir has no libgstwebrtc.so; the snap would have no call media engine"

mkdir -p "$TREE"
# Only usr/ is taken; linuxdeploy's AppImage entry artefacts (AppRun,
# top-level desktop file, .DirIcon) stay behind in the AppDir.
cp -a "$SNAP_WORK/appdir/usr" "$TREE/usr"

# Launcher: point Qt at the bundled runtime under $SNAP -- and GStreamer too.
# The snap takes only usr/ from the AppDir, so linuxdeploy's AppRun and its
# apprun-hooks/gstreamer.sh stay behind; without the three variables below the
# call plugins are inside the snap and GStreamer never looks at them, because
# it scans the path compiled into the build image. The AppImage learned this
# the same way.
mkdir -p "$TREE/bin"
cat > "$TREE/bin/lightning-launch" <<'EOF'
#!/bin/sh
set -e
: "${SNAP:?lightning-launch must run inside a snap environment}"
export LD_LIBRARY_PATH="$SNAP/usr/lib:${LD_LIBRARY_PATH:-}"
export QT_PLUGIN_PATH="$SNAP/usr/plugins"
export QML2_IMPORT_PATH="$SNAP/usr/qml"
export QML_IMPORT_PATH="$SNAP/usr/qml"
export XDG_DATA_DIRS="$SNAP/usr/share:${XDG_DATA_DIRS:-/usr/share}"
export GST_PLUGIN_SYSTEM_PATH_1_0="$SNAP/usr/lib/gstreamer-1.0"
export GST_PLUGIN_PATH_1_0="$SNAP/usr/lib/gstreamer-1.0"
# $SNAP is read-only and its revision changes on every refresh, so the plugin
# registry cache has to live in the user's own (snap-confined) cache dir.
export GST_REGISTRY_1_0="${XDG_CACHE_HOME:-$HOME/.cache}/lightning/gst-registry.bin"
mkdir -p "$(dirname "$GST_REGISTRY_1_0")" 2>/dev/null || true
exec "$SNAP/usr/bin/matrix-client" --backend=rust "$@"
EOF
chmod 0755 "$TREE/bin/lightning-launch"

# Snap metadata + desktop integration.
mkdir -p "$TREE/meta/gui"
sed -e "s|@VERSION@|$LOGICAL_VERSION|" "$TEMPLATE" > "$TREE/meta/snap.yaml"
grep -q '@VERSION@' "$TREE/meta/snap.yaml" && die "unsubstituted snap.yaml"
sed -e 's|^Exec=.*$|Exec=lightning|' \
    -e 's|^Icon=.*$|Icon=${SNAP}/meta/gui/lightning.png|' \
    "$TREE/usr/share/applications/lightning.desktop" \
    > "$TREE/meta/gui/lightning.desktop"
cp "$TREE/usr/share/icons/hicolor/192x192/apps/lightning.png" \
    "$TREE/meta/gui/lightning.png"

python3 - "$TREE/meta/snap.yaml" <<'EOF'
import sys, yaml
with open(sys.argv[1]) as fh:
    meta = yaml.safe_load(fh)
assert meta["name"] == "lightning"
assert meta["confinement"] == "strict"
assert meta["base"] == "core24"
assert "lightning" in meta["apps"]
assert "password-manager-service" in meta["apps"]["lightning"]["plugs"]
# Calling needs the microphone. audio-playback alone is a call nobody can
# hear you on.
assert "audio-record" in meta["apps"]["lightning"]["plugs"]
print("snap.yaml structurally valid")
EOF

mkdir -p dist
rm -f "$OUT"
# Standard snap squashfs settings (xz, root-owned).
mksquashfs "$TREE" "$OUT" -noappend -comp xz -all-root -no-xattrs >/dev/null
test -s "$OUT" || die "snap not produced"
write_sha256 "$OUT"
echo "built $OUT"
