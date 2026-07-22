#!/usr/bin/env bash
# Clean-system validation of the snap. A real `snap install --dangerous`
# needs a running snapd, which the Docker-on-Linux fleet cannot provide, so
# this is the closest faithful equivalent (documented in the runner and
# packaging docs): unsquash the image, verify meta/snap.yaml and desktop
# integration, run the payload offscreen through the snap launcher with a
# simulated $SNAP, check GIF provider state, and audit for leaks.
set -euo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lib.sh
. "$SCRIPT_DIR/lib.sh"
ROOT=$(project_dir)
cd "$ROOT"
load_versions

count=$(find dist -maxdepth 1 -name '*.snap' | wc -l)
[ "$count" = 1 ] || die "expected exactly one .snap in dist, found $count"
snap_file="dist/lightning_${LOGICAL_VERSION}_amd64.snap"
test -f "$snap_file" || die "expected snap name $snap_file"
test ! -e work/lightning || die "validation must not see a source checkout"
command -v nix >/dev/null 2>&1 && die "validation image must not provide nix"
( cd dist && sha256sum -c "$(basename "$snap_file").sha256" )
file "$snap_file" | grep -qi squashfs || die "snap is not a squashfs image"

audit=$(mktemp -d)
cleanup() { rm -rf "$audit"; }
trap cleanup EXIT
unsquashfs -q -d "$audit/prime" "$snap_file"

python3 - "$audit/prime/meta/snap.yaml" "$LOGICAL_VERSION" <<'EOF'
import sys, yaml
with open(sys.argv[1]) as fh:
    meta = yaml.safe_load(fh)
assert meta["name"] == "lightning", meta
assert str(meta["version"]) == sys.argv[2], meta
assert meta["confinement"] == "strict"
assert meta["grade"] == "stable"
assert meta["license"] == "GPL-3.0-or-later"
app = meta["apps"]["lightning"]
assert app["command"] == "bin/lightning-launch"
for plug in ("network", "wayland", "x11", "desktop",
             "password-manager-service"):
    assert plug in app["plugs"], plug
print("snap.yaml valid")
EOF
test -f "$audit/prime/meta/gui/lightning.desktop" || die "snap desktop file missing"
test -f "$audit/prime/meta/gui/lightning.png" || die "snap icon missing"
test -x "$audit/prime/bin/lightning-launch" || die "launcher missing"

# Run through the snap launcher with $SNAP simulated (snapd would provide
# it at runtime); everything but base-system libs must come from the snap.
version_output=$(cd /tmp && env SNAP="$audit/prime" QT_QPA_PLATFORM=offscreen \
    "$audit/prime/bin/lightning-launch" --version)
# The launcher forces --backend=rust; --version prints before backend init.
[ "$version_output" = "matrix-client $BASE_VERSION" ] \
    || die "--version mismatch: $version_output"

set +e
( cd /tmp && timeout 20s env SNAP="$audit/prime" QT_QPA_PLATFORM=offscreen \
    "$audit/prime/usr/bin/matrix-client" --backend=rust \
    ) > dist/snap-launch.log 2>&1
status=$?
set -e
[ "$status" = 0 ] || [ "$status" = 124 ] \
    || { cat dist/snap-launch.log; die "offscreen launch failed ($status)"; }

gif_env() {
    env -u GIPHY_API_KEY -u KLIPY_API_KEY \
        -u LIGHTNING_GIPHY_API_KEY -u LIGHTNING_KLIPY_API_KEY \
        -u LIGHTNING_BUILD_GIPHY_API_KEY -u LIGHTNING_BUILD_KLIPY_API_KEY \
        SNAP="$audit/prime" QT_QPA_PLATFORM=offscreen "$@"
}
( cd /tmp && gif_env "$audit/prime/bin/lightning-launch" --gif-status ) \
    | tee dist/snap-gif-status.txt
if [ "${PUBLISH_PACKAGES:-false}" = "true" ]; then
    grep -q "GIPHY configured: yes" dist/snap-gif-status.txt || die "GIPHY not embedded"
    grep -q "KLIPY configured: yes" dist/snap-gif-status.txt || die "KLIPY not embedded"
    ( cd /tmp && gif_env "$audit/prime/bin/lightning-launch" --gif-selftest ) \
        | tee dist/snap-gif-selftest.txt
    grep -q "GIPHY request: ok" dist/snap-gif-selftest.txt || die "GIPHY selftest failed"
    grep -q "KLIPY request: ok" dist/snap-gif-selftest.txt || die "KLIPY selftest failed"
    grep -Eq "api_key=|://" dist/snap-gif-status.txt dist/snap-gif-selftest.txt \
        && die "GIF output leaked a URL or key"
fi

# Payload audit (same policy as every other format).
grep -RIl -e /nix/store -e /home/roksme -e /builds/ \
    -e 'LIGHTNING_GIPHY_API_KEY=' -e 'LIGHTNING_KLIPY_API_KEY=' \
    -e 'PRIVATE-TOKEN:' -e 'recovery_key=' "$audit/prime/usr/bin" \
    && die "forbidden path or credential marker in payload"
find "$audit/prime" -name 'LightningGifBuildKeys.h' | grep -q . \
    && die "generated key header leaked into the payload"
find "$audit/prime" -perm -0002 \( -type f -o -type d \) | grep -q . \
    && die "world-writable content"
find "$audit/prime" -perm -4000 -o -perm -2000 | grep -q . \
    && die "unexpected setuid/setgid content"
echo "snap validation passed (structural + payload; live snapd install is"
echo "not possible on this runner fleet — documented limitation)"
