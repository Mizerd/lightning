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

# Which package this build will become. The binary reports it to the updater,
# which uses it to pick a compiled-in install strategy -- so a wrong value here
# means a user is offered the wrong kind of update. Callers must set it; there
# is deliberately no default, because guessing would be worse than failing.
: "${LIGHTNING_INSTALL_TYPE:?must be set by the per-format build script}"
case "$LIGHTNING_INSTALL_TYPE" in
    linux-appimage|linux-deb|linux-rpm|linux-flatpak|linux-snap) ;;
    *) die "unsupported LIGHTNING_INSTALL_TYPE for a Linux package: $LIGHTNING_INSTALL_TYPE" ;;
esac

# Public half of the update-manifest signing key, embedded so the client can
# verify a manifest. Empty is allowed and fails CLOSED: such a build can check
# for updates but can never accept one. Never the private key -- that stays a
# protected CI variable and is only ever used by the signing job.
if [[ -n "${UPDATE_SIGNING_PUBKEY_2026A:-}" ]]; then
    printf 'Update signing public key: embedded (key id %s)\n' \
        "${UPDATE_SIGNING_KEY_ID:-lightning-release-2026a}"
else
    printf 'Update signing public key: NOT set — this build cannot accept updates\n'
fi

# Official release packages embed application GIF provider keys so installed
# clients work without user configuration. Values come from the protected CI
# variables GIPHY_API_KEY / KLIPY_API_KEY and are mapped to the build-only
# LIGHTNING_BUILD_* names the source generator reads from the environment — never
# passed on a command line, echoed, or written to a dotenv/artifact. This runs
# in a child process, so the exports do not leak back to the caller, and the
# generated header is scrubbed on exit. Keyless build-only pipelines skip this.
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

# Persistent per-runner build caches. Every package runner bind-mounts its own
# host cache directory at /cache into job containers; runners never share one.
# Everything stored there derives from public sources only.
CACHE_ROOT=""
if [[ -d /cache && -w /cache ]]; then
    CACHE_ROOT=/cache
fi

if [[ -n "$CACHE_ROOT" ]]; then
    # Registry index, crate sources, and git checkouts survive across jobs, so
    # a warm build skips the network fetch and keeps stable source mtimes for
    # cargo's fingerprints.
    export CARGO_HOME="$CACHE_ROOT/cargo-home"
else
    export CARGO_HOME="$ROOT/work/cargo-home"
fi
export CFLAGS="${CFLAGS:-} -ffile-prefix-map=$ROOT=/usr/src/lightning -fdebug-prefix-map=$ROOT=/usr/src/lightning"
export CXXFLAGS="${CXXFLAGS:-} -ffile-prefix-map=$ROOT=/usr/src/lightning -fdebug-prefix-map=$ROOT=/usr/src/lightning"
export RUSTFLAGS="${RUSTFLAGS:-} --remap-path-prefix=$ROOT=/usr/src/lightning"
mkdir -p "$CARGO_HOME" "$STAGE_DIR"

# The source pins CARGO_TARGET_DIR to <build>/rust, so persist that exact
# location via a symlink onto the runner cache. Cargo's own fingerprints
# decide what is reusable; the Rust tree never sees the GIF keys, so this is
# safe for official builds as well.
if [[ -n "$CACHE_ROOT" ]]; then
    mkdir -p "$CACHE_ROOT/cargo-target" "$BUILD_DIR"
    if [[ ! -e "$BUILD_DIR/rust" || -L "$BUILD_DIR/rust" ]]; then
        ln -sfn "$CACHE_ROOT/cargo-target" "$BUILD_DIR/rust"
    fi
fi

# C++ compile cache. Deliberately disabled for publishing builds: object files
# compiled from the generated GIF-key header must never persist outside the
# job. Build-only pipelines carry no keys, so caching them is safe.
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

# Voice/video calling is requested EXPLICITLY even though the source option
# already defaults to ON, because the default is not what decides it: the
# source only sets HAVE_LIGHTNING_WEBRTC when a pkg-config probe finds the
# GStreamer WebRTC development files, and with none installed it configures the
# engine OUT and says so in one STATUS line among hundreds. Every Linux 0.8.0
# package shipped that way -- `--call-media-status` in the published deb
# answered "call media engine built in: no" and `ldd` named no GStreamer at
# all, so calling, screen sharing and the camera were compiled out and the app
# refused every call. Naming the option here does not change what CMake DOES;
# it changes what this script is entitled to assert afterwards, which is the
# staged-binary check at the end of this file.
#
# LIGHTNING_REQUIRE_WEBRTC=ON is the half that fails FAST. The source added it
# for exactly this: with it, a failed probe becomes a CMake FATAL_ERROR naming
# the missing pkg-config modules, so the job dies during CONFIGURE rather than
# after the ~30 minutes of Rust it takes to reach the staged-binary check at
# the end of this file. Both guards stay — this one catches a missing dev
# package early, and that one is the only thing that proves what was STAGED.
cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_TESTING=OFF \
    -DENABLE_RUST_SDK_BACKEND=ON \
    -DLIGHTNING_RUST_ONLY=ON \
    -DLIGHTNING_REQUIRE_GIF_KEYS="$REQUIRE_GIF_KEYS" \
    -DLIGHTNING_ENABLE_WEBRTC=ON \
    -DLIGHTNING_REQUIRE_WEBRTC=ON \
    -DLIGHTNING_ARTIFACT_KIND=release \
    -DLIGHTNING_INSTALL_TYPE="$LIGHTNING_INSTALL_TYPE" \
    -DLIGHTNING_UPDATE_PUBKEY_2026A="${UPDATE_SIGNING_PUBKEY_2026A:-}" \
    -DLIGHTNING_SOURCE_SHA="${SOURCE_SHA:-}" \
    "${CCACHE_ARGS[@]}"
cmake --build "$BUILD_DIR" --parallel "$BUILD_JOBS"
DESTDIR="$STAGE_DIR" cmake --install "$BUILD_DIR"

# The keys were consumed at configure/compile time and are embedded in the
# binary. Remove the generated plaintext header and drop the build-only env
# values now; the EXIT trap is a backstop.
scrub_gif_header
unset LIGHTNING_BUILD_GIPHY_API_KEY LIGHTNING_BUILD_KLIPY_API_KEY 2>/dev/null || true

# Every Linux format is assembled from this staged tree, so a helper that is not
# installed here is a helper that ships in none of them — and Lightning's in-app
# updater has nothing to hand a verified artifact to. Fail here, where the cause
# is obvious, rather than in one format's payload audit (or, for the RPM, in an
# "installed but unpackaged file" abort).
[[ -x "$STAGE_DIR/usr/bin/lightning-updater" ]] || \
    die "the update helper was not installed: $STAGE_DIR/usr/bin/lightning-updater"

# Native packages use system libraries in standard paths. Remove the
# build-generated RPATH from both staged executables for every format.
patchelf --remove-rpath "$STAGE_DIR/usr/bin/lightning-matrix"
patchelf --remove-rpath "$STAGE_DIR/usr/bin/lightning-updater"

# Since Lightning 0.7 the source installs its own desktop entry and hicolor
# icons via cmake --install; the copy here is only a fallback so older
# pinned source SHAs (pre-icon) can still be rebuilt.
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

# Fail closed on the call media engine. Ask the BINARY, not the build log: the
# build log is what everybody read in 0.8.0 and it never said the engine was
# missing. `--call-media-status` is the source's own probe, and its first line
# is decided purely by whether HAVE_LIGHTNING_WEBRTC was defined at compile
# time -- which is exactly the question a build job can answer.
#
# Only that line is asserted here. The lines under it describe the RUNTIME
# (whether gst_init found plugins, and whether every element the engine needs
# is registered), and a build image is the wrong place to judge those: a deb
# gets its plugins from its Depends and an AppImage from its own bundle. The
# per-format validators run the same command against the INSTALLED artifact
# and require the engine to be genuinely usable there.
#
# The command exits non-zero whenever the engine cannot be used, so its status
# is deliberately not the test.
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
