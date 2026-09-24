#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"

ROOT="$(project_dir)"
SOURCE_DIR="$ROOT/work/lightning"
BUILD_DIR="$ROOT/work/build"
STAGE_DIR="$ROOT/work/stage"
BUILD_TYPE="${BUILD_TYPE:-Release}"
BUILD_JOBS="${BUILD_JOBS:-2}"

[[ "$BUILD_TYPE" == "Release" ]] || die "only BUILD_TYPE=Release is supported for distributable packages"
[[ "$BUILD_JOBS" =~ ^[1-8]$ ]] || die "BUILD_JOBS must be 1-8 on the package runners"
[[ -f "$SOURCE_DIR/CMakeLists.txt" && -f "$SOURCE_DIR/rust/Cargo.lock" ]] || die "Lightning source is incomplete"
[[ -f "$SOURCE_DIR/LICENSE" && -f "$SOURCE_DIR/README.md" ]] || \
    die "Lightning source must include its licence and README"

# The updater picks its install strategy from this, so there is deliberately
# no default: a wrong guess would offer users the wrong kind of update.
: "${LIGHTNING_INSTALL_TYPE:?must be set by the per-format build script}"
case "$LIGHTNING_INSTALL_TYPE" in
    linux-appimage|linux-deb|linux-rpm|linux-flatpak|linux-snap) ;;
    *) die "unsupported LIGHTNING_INSTALL_TYPE for a Linux package: $LIGHTNING_INSTALL_TYPE" ;;
esac

# Public update-manifest signing key. Empty fails closed: the build can check
# for updates but never accept one. The private key never reaches this job.
if [[ -n "${UPDATE_SIGNING_PUBKEY_2026A:-}" ]]; then
    printf 'Update signing public key: embedded (key id %s)\n' \
        "${UPDATE_SIGNING_KEY_ID:-lightning-release-2026a}"
else
    printf 'Update signing public key: NOT set — this build cannot accept updates\n'
fi

# Official builds embed the GIF provider keys. They reach the source generator
# only through the build-only LIGHTNING_BUILD_* environment variables — never a
# command line, log, dotenv or artifact — and the generated header is scrubbed
# on exit.
REQUIRE_GIF_KEYS=OFF
GENERATED_GIF_HEADER="$BUILD_DIR/generated/LightningGifBuildKeys.h"
scrub_gif_header() { rm -f "$GENERATED_GIF_HEADER" 2>/dev/null || true; }
trap scrub_gif_header EXIT
if [[ "${PUBLISH_PACKAGES:-false}" == true ]]; then
    [[ -n "${GIPHY_API_KEY:-}" ]] || die "official build requires GIPHY_API_KEY"
    [[ -n "${KLIPY_API_KEY:-}" ]] || die "official build requires KLIPY_API_KEY"
    export LIGHTNING_BUILD_GIPHY_API_KEY="$GIPHY_API_KEY"
    export LIGHTNING_BUILD_KLIPY_API_KEY="$KLIPY_API_KEY"
    REQUIRE_GIF_KEYS=ON
    printf 'Embedding official GIF provider keys into the release build\n'
fi

export CMAKE_BUILD_PARALLEL_LEVEL="$BUILD_JOBS"
export CARGO_BUILD_JOBS="$BUILD_JOBS"

# Per-runner cache at /cache (never shared between runners); it holds only
# data derived from public sources.
CACHE_ROOT=""
if [[ -d /cache && -w /cache ]]; then
    CACHE_ROOT=/cache
fi

if [[ -n "$CACHE_ROOT" ]]; then
    # Keeps crate sources and stable mtimes for cargo's fingerprints.
    export CARGO_HOME="$CACHE_ROOT/cargo-home"
else
    export CARGO_HOME="$ROOT/work/cargo-home"
fi
export CFLAGS="${CFLAGS:-} -ffile-prefix-map=$ROOT=/usr/src/lightning -fdebug-prefix-map=$ROOT=/usr/src/lightning"
export CXXFLAGS="${CXXFLAGS:-} -ffile-prefix-map=$ROOT=/usr/src/lightning -fdebug-prefix-map=$ROOT=/usr/src/lightning"
export RUSTFLAGS="${RUSTFLAGS:-} --remap-path-prefix=$ROOT=/usr/src/lightning"
mkdir -p "$CARGO_HOME" "$STAGE_DIR"

# The source pins CARGO_TARGET_DIR to <build>/rust; symlink it onto the cache.
# The Rust tree never sees the GIF keys, so this is safe for official builds.
if [[ -n "$CACHE_ROOT" ]]; then
    mkdir -p "$CACHE_ROOT/cargo-target" "$BUILD_DIR"
    if [[ ! -e "$BUILD_DIR/rust" || -L "$BUILD_DIR/rust" ]]; then
        ln -sfn "$CACHE_ROOT/cargo-target" "$BUILD_DIR/rust"
    fi
fi

# No ccache for publishing builds: objects compiled from the GIF-key header
# must not outlive the job.
CCACHE_ARGS=()
if [[ "${PUBLISH_PACKAGES:-false}" == true ]]; then
    printf 'C++ compile cache disabled for the official (key-embedding) build\n'
elif [[ -n "$CACHE_ROOT" ]] && command -v ccache >/dev/null 2>&1; then
    export CCACHE_DIR="$CACHE_ROOT/ccache"
    export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-3G}"
    CCACHE_ARGS=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
    printf 'ccache enabled at %s\n' "$CCACHE_DIR"
fi

printf 'Toolchain: '; cmake --version | head -1
printf 'Rust: '; rustc --version
printf 'Cargo: '; cargo --version
printf 'Fetching locked Rust dependencies before the source-enforced offline build\n'
cargo fetch --locked --manifest-path "$SOURCE_DIR/rust/Cargo.toml"

# Without the GStreamer dev files CMake silently configures the call engine
# out. LIGHTNING_REQUIRE_WEBRTC=ON turns that into a configure-time error; the
# staged-binary check at the end of this file proves what was actually built.
cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_TESTING=OFF \
    -DENABLE_RUST_SDK_BACKEND=ON \
    -DLIGHTNING_RUST_ONLY=ON \
    -DLIGHTNING_REQUIRE_GIF_KEYS="$REQUIRE_GIF_KEYS" \
    -DLIGHTNING_ENABLE_WEBRTC=ON \
    -DLIGHTNING_REQUIRE_WEBRTC=ON \
    -DLIGHTNING_REQUIRE_QT_SVG=ON \
    -DLIGHTNING_ARTIFACT_KIND=release \
    -DLIGHTNING_INSTALL_TYPE="$LIGHTNING_INSTALL_TYPE" \
    -DLIGHTNING_UPDATE_PUBKEY_2026A="${UPDATE_SIGNING_PUBKEY_2026A:-}" \
    -DLIGHTNING_SOURCE_SHA="${SOURCE_SHA:-}" \
    "${CCACHE_ARGS[@]}"
cmake --build "$BUILD_DIR" --parallel "$BUILD_JOBS"
DESTDIR="$STAGE_DIR" cmake --install "$BUILD_DIR"

# The keys are embedded now; drop the plaintext header and env values (the
# EXIT trap is a backstop).
scrub_gif_header
unset LIGHTNING_BUILD_GIPHY_API_KEY LIGHTNING_BUILD_KLIPY_API_KEY 2>/dev/null || true

# Every Linux format is assembled from this stage, so fail here if the update
# helper is missing rather than in a per-format audit.
[[ -x "$STAGE_DIR/usr/bin/lightning-updater" ]] || \
    die "the update helper was not installed: $STAGE_DIR/usr/bin/lightning-updater"

# Native packages use system libraries in standard paths. Remove the
# build-generated RPATH from both staged executables for every format.
patchelf --remove-rpath "$STAGE_DIR/usr/bin/lightning-matrix"
patchelf --remove-rpath "$STAGE_DIR/usr/bin/lightning-updater"

# Fallback for pinned sources older than 0.7, which did not install their own
# desktop entry.
if [ ! -f "$STAGE_DIR/usr/share/applications/lightning.desktop" ]; then
    install -Dm0644 "$ROOT/packaging-ci/packaging/common/lightning.desktop" \
        "$STAGE_DIR/usr/share/applications/lightning.desktop"
fi
install -Dm0644 "$ROOT/packaging-ci/packaging/common/lightning.metainfo.xml" \
    "$STAGE_DIR/usr/share/metainfo/lightning.metainfo.xml"
install -Dm0644 "$ROOT/packaging-ci/packaging/common/copyright" \
    "$STAGE_DIR/usr/share/licenses/lightning/copyright"
install -Dm0644 "$SOURCE_DIR/LICENSE" \
    "$STAGE_DIR/usr/share/doc/lightning/LICENSE"
install -Dm0644 "$SOURCE_DIR/README.md" \
    "$STAGE_DIR/usr/share/doc/lightning/README.md"

test -x "$STAGE_DIR/usr/bin/lightning-matrix"
"$STAGE_DIR/usr/bin/lightning-matrix" --version

# Fail closed on the Rust-only release invariant: the staged binary must ship
# only the Rust backend (no HTTP/mock compiled in, no runtime fallback).
staged_build_info="$("$STAGE_DIR/usr/bin/lightning-matrix" --build-info)"
printf '%s\n' "$staged_build_info"
mkdir -p "$ROOT/dist"
printf '%s\n' "$staged_build_info" >"$ROOT/dist/build-info-linux.txt"
printf '%s\n' "$staged_build_info" | grep -qx 'matrix_backend: rust' \
    || die "staged binary is not Rust-only (matrix_backend != rust)"
printf '%s\n' "$staged_build_info" | grep -qx 'http_backend_compiled: false' \
    || die "staged binary compiled the HTTP backend"
printf '%s\n' "$staged_build_info" | grep -qx 'mock_backend_compiled: false' \
    || die "staged binary compiled the mock backend"

# Fail closed on the call media engine by asking the staged binary. Only the
# compile-time "built in" line is asserted: runtime plugin availability is
# checked by each format's validator against the installed artifact. The exit
# status is non-zero whenever the engine is unusable, so it is not the test.
call_media_status="$(timeout 60s "$STAGE_DIR/usr/bin/lightning-matrix" --call-media-status 2>&1 || true)"
printf '%s\n' "$call_media_status"
printf '%s\n' "$call_media_status" >"$ROOT/dist/call-media-status-build.txt"
printf '%s\n' "$call_media_status" | grep -qx 'call media engine built in: yes' || \
    die "the staged binary has NO call media engine: CMake's GStreamer probe found\
 no development files, so LIGHTNING_ENABLE_WEBRTC=ON was silently ignored and\
 calls, screen sharing and the camera are compiled out. Install the GStreamer\
 development packages in this build job (Debian/Ubuntu: libgstreamer1.0-dev\
 libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev; Fedora:\
 gstreamer1-devel gstreamer1-plugins-base-devel gstreamer1-plugins-bad-free-devel).\
 If SOURCE_REF is pinned to a commit older than 2026-08-26 it has no\
 --call-media-status at all and no engine to assert; that is a source too old\
 to publish a calling client from, not a check to remove."
