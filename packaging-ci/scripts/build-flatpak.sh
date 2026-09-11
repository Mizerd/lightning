#!/usr/bin/env bash
# Build the Lightning Flatpak single-file bundle from the pinned source.
#
# Runs on package-runner-flatpak: unprivileged, but its job containers get
# relaxed seccomp/apparmor so bwrap user namespaces work (the documented
# exception for this one runner). The app is compiled INSIDE the sandbox
# against org.kde.Platform — Flatpak binaries must link the runtime's
# libraries, so the Debian-staged build is deliberately not reused here.
#
# Output: dist/lightning_<LOGICAL_VERSION>_amd64.flatpak
set -euo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lib.sh
. "$SCRIPT_DIR/lib.sh"
ROOT=$(project_dir)
cd "$ROOT"
load_versions

SOURCE_DIR="$ROOT/work/lightning"
FLATPAK_WORK="$ROOT/work/flatpak"
MANIFEST_TEMPLATE="$ROOT/packaging-ci/packaging/flatpak/org.lightning_matrix.Lightning.yaml.in"
MANIFEST="$FLATPAK_WORK/org.lightning_matrix.Lightning.yaml"
REPO_DIR="$FLATPAK_WORK/repo"
BUILD_DIR="$FLATPAK_WORK/build"
STATE_DIR="$FLATPAK_WORK/state"
BUNDLE="dist/lightning_${LOGICAL_VERSION}_amd64.flatpak"
APP_ID=org.lightning_matrix.Lightning
# org.kde.Sdk//6.9 is marked END-OF-LIFE on Flathub ("We strongly recommend
# moving to the latest stable version"). It still resolves today, so this is
# not yet a broken build, but an EOL runtime stops being rebuilt and will
# eventually go. 6.10 and 6.11 are both current and both sit on freedesktop
# 25.08; 6.11 carries Qt 6.11.1, libsecret-1 0.21.7, all six GStreamer WebRTC
# pkg-config modules at 1.26.11, and the libqwebp.so / kimg_jxl.so image
# decoders validate-flatpak.sh asserts. Verified 2026-09-11 by building the
# v0.9.4 source against it.
RUNTIME_VERSION="6.11"
FLATHUB_REPO=https://dl.flathub.org/repo/flathub.flatpakrepo

test -f "$SOURCE_DIR/CMakeLists.txt" || die "pinned source missing"
test -f "$MANIFEST_TEMPLATE" || die "manifest template missing"

# Persist runtimes across jobs in the runner cache; nothing secret lives
# there (runtimes and ostree objects only).
export FLATPAK_USER_DIR=/cache/flatpak-user
mkdir -p "$FLATPAK_USER_DIR"

REQUIRE_GIF_KEYS=OFF
PERSIST_STATE=false
BUILDER_CACHE_ARGS=()
if [ "${PUBLISH_PACKAGES:-false}" = "true" ]; then
    require_var GIPHY_API_KEY
    require_var KLIPY_API_KEY
    export LIGHTNING_BUILD_GIPHY_API_KEY="$GIPHY_API_KEY"
    export LIGHTNING_BUILD_KLIPY_API_KEY="$KLIPY_API_KEY"
    REQUIRE_GIF_KEYS=ON
elif [ -d /cache ] && [ -w /cache ]; then
    # Build-only pipelines carry no keys, so the builder state (downloads,
    # build cache, ccache) is safe to persist for warm-build speed. Official
    # key-embedding builds keep the ephemeral in-tree state that the EXIT
    # trap wipes.
    STATE_DIR=/cache/flatpak-state
    PERSIST_STATE=true
    BUILDER_CACHE_ARGS=(--ccache)
fi
cleanup_keys() {
    unset LIGHTNING_BUILD_GIPHY_API_KEY LIGHTNING_BUILD_KLIPY_API_KEY \
        2>/dev/null || true
    # The generated key header lives only inside sandbox build dirs; remove
    # them wholesale as a backstop. The persistent state dir is only ever
    # used for keyless builds.
    rm -rf "$BUILD_DIR"
    [ "$PERSIST_STATE" = "true" ] || rm -rf "$STATE_DIR"
}
trap cleanup_keys EXIT

rm -rf "$FLATPAK_WORK"
mkdir -p "$FLATPAK_WORK" dist

# Substitute the placeholders. Source paths are RELATIVE to the manifest
# location (work/flatpak/): flatpak-builder exports the manifest into the
# bundle as /app/manifest.json, and an absolute path would leak the
# ephemeral CI workspace path into the payload (the clean-bundle audit
# rejects /builds/). They stay relative for that reason -- the packaging-ci/
# segment is there because the packaging tree now sits inside the application
# repository rather than being its own project, and a path that walked up to
# the old location resolved to a directory that does not exist.
sed -e "s|@REQUIRE_GIF_KEYS@|$REQUIRE_GIF_KEYS|" \
    -e "s|@UPDATE_SIGNING_PUBKEY_2026A@|${UPDATE_SIGNING_PUBKEY_2026A:-}|" \
    -e "s|@SOURCE_DIR@|../lightning|" \
    -e "s|@METAINFO_PATH@|../../packaging-ci/packaging/common/lightning.metainfo.xml|" \
    "$MANIFEST_TEMPLATE" > "$MANIFEST"
grep -Eq '@(REQUIRE_GIF_KEYS|SOURCE_DIR|METAINFO_PATH|UPDATE_SIGNING_PUBKEY_2026A)@' "$MANIFEST" \
    && die "unsubstituted placeholder in manifest"

flatpak remote-add --user --if-not-exists flathub "$FLATHUB_REPO"
flatpak install --user --noninteractive --or-update flathub \
    "org.kde.Platform//$RUNTIME_VERSION" \
    "org.kde.Sdk//$RUNTIME_VERSION" \
    org.freedesktop.Sdk.Extension.rust-stable//24.08

flatpak-builder --user --force-clean --disable-rofiles-fuse \
    --state-dir="$STATE_DIR" \
    --jobs="${BUILD_JOBS:-4}" \
    "${BUILDER_CACHE_ARGS[@]}" \
    --repo="$REPO_DIR" "$BUILD_DIR" "$MANIFEST"

flatpak build-bundle "$REPO_DIR" "$BUNDLE" "$APP_ID" \
    --runtime-repo="$FLATHUB_REPO"

test -s "$BUNDLE" || die "bundle missing"
write_sha256 "$BUNDLE"
echo "built $BUNDLE"
