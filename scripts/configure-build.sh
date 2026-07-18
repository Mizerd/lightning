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
[[ "$BUILD_JOBS" =~ ^[12]$ ]] || die "BUILD_JOBS must be 1 or 2 on the package runners"
[[ -f "$SOURCE_DIR/CMakeLists.txt" && -f "$SOURCE_DIR/rust/Cargo.lock" ]] || die "Lightning source is incomplete"
[[ -f "$SOURCE_DIR/LICENSE" && -f "$SOURCE_DIR/README.md" ]] || \
    die "Lightning source must include its licence and README"

export CMAKE_BUILD_PARALLEL_LEVEL="$BUILD_JOBS"
export CARGO_BUILD_JOBS="$BUILD_JOBS"
export CARGO_HOME="$ROOT/work/cargo-home"
export CFLAGS="${CFLAGS:-} -ffile-prefix-map=$ROOT=/usr/src/lightning -fdebug-prefix-map=$ROOT=/usr/src/lightning"
export CXXFLAGS="${CXXFLAGS:-} -ffile-prefix-map=$ROOT=/usr/src/lightning -fdebug-prefix-map=$ROOT=/usr/src/lightning"
export RUSTFLAGS="${RUSTFLAGS:-} --remap-path-prefix=$ROOT=/usr/src/lightning"
mkdir -p "$CARGO_HOME" "$STAGE_DIR"

printf 'Toolchain: '; cmake --version | head -1
printf 'Rust: '; rustc --version
printf 'Cargo: '; cargo --version
printf 'Fetching locked Rust dependencies before the source-enforced offline build\n'
cargo fetch --locked --manifest-path "$SOURCE_DIR/rust/Cargo.toml"

cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_TESTING=OFF \
    -DENABLE_RUST_SDK_BACKEND=ON
cmake --build "$BUILD_DIR" --parallel "$BUILD_JOBS"
DESTDIR="$STAGE_DIR" cmake --install "$BUILD_DIR"

# Native packages use system libraries in standard paths. Remove the
# build-generated RPATH from the staged executable for both formats.
patchelf --remove-rpath "$STAGE_DIR/usr/bin/matrix-client"

install -Dm0644 "$ROOT/packaging/common/lightning.desktop" \
    "$STAGE_DIR/usr/share/applications/lightning.desktop"
install -Dm0644 "$ROOT/packaging/common/lightning.metainfo.xml" \
    "$STAGE_DIR/usr/share/metainfo/lightning.metainfo.xml"
install -Dm0644 "$ROOT/packaging/common/copyright" \
    "$STAGE_DIR/usr/share/licenses/lightning/copyright"
install -Dm0644 "$SOURCE_DIR/LICENSE" \
    "$STAGE_DIR/usr/share/doc/lightning/LICENSE"
install -Dm0644 "$SOURCE_DIR/README.md" \
    "$STAGE_DIR/usr/share/doc/lightning/README.md"

test -x "$STAGE_DIR/usr/bin/matrix-client"
"$STAGE_DIR/usr/bin/matrix-client" --version
