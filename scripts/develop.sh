#!/usr/bin/env bash
# Used by the Nix shell's lightning-dev command. Run from the checkout root.
set -euo pipefail

usage() {
    echo "Usage: lightning-dev {configure|build|run} [extra arguments]"
    echo "  configure: fetch locked Cargo sources and configure a Debug Rust build"
    echo "  build:     incrementally build lightning-matrix"
    echo "  run:       launch the existing binary with the Rust backend"
    echo "Set LIGHTNING_BUILD_DIR to choose a build tree (default: build-nixos)."
}

action=${1:-help}
case "$action" in
    help|-h|--help) usage; exit 0 ;;
    configure|build|run) shift ;;
    *) usage >&2; exit 2 ;;
esac

if [[ ! -f CMakeLists.txt || ! -f rust/Cargo.toml ]]; then
    echo "Run lightning-dev from the Lightning checkout root." >&2
    exit 1
fi

build_dir=${LIGHTNING_BUILD_DIR:-build-nixos}
case "$build_dir" in
    /*) ;;
    *) build_dir="./$build_dir" ;;
esac

if [[ "$action" == run ]]; then
    if [[ ! -x "$build_dir/lightning-matrix" ]]; then
        echo "No executable at $build_dir/lightning-matrix; configure and build first." >&2
        exit 1
    fi
    exec "$build_dir/lightning-matrix" --backend=rust "$@"
fi

# Cache definitions may customize the configuration without redirecting
# CMake into a different tree from the one protected by our lock.
if [[ "$action" == configure ]]; then
    for argument in "$@"; do
        if [[ "$argument" != -D?*=* ]]; then
            echo "Configure accepts only -DNAME=VALUE arguments; use LIGHTNING_BUILD_DIR for the tree." >&2
            exit 2
        fi
    done
elif [[ $# -ne 0 ]]; then
    echo "Build takes no extra arguments; set CARGO_BUILD_JOBS to choose 1-8 jobs." >&2
    exit 2
fi

# Bound both nested Cargo work and CMake, even outside the Nix shell.
jobs=${CARGO_BUILD_JOBS:-8}
if [[ ! "$jobs" =~ ^[1-8]$ ]]; then
    echo "CARGO_BUILD_JOBS must be between 1 and 8." >&2
    exit 2
fi
export CARGO_BUILD_JOBS="$jobs"
export CMAKE_BUILD_PARALLEL_LEVEL="$jobs"
export CARGO_PROFILE_DEV_DEBUG="${CARGO_PROFILE_DEV_DEBUG:-1}"

if [[ "$action" == build && ! -f "$build_dir/CMakeCache.txt" ]]; then
    echo "No CMake cache in $build_dir; run lightning-dev configure first." >&2
    exit 1
fi

mkdir -p -- "$build_dir"
exec 9>"$build_dir/.developer-build.lock"
if ! flock -n 9; then
    echo "Another configure/build holds $build_dir/.developer-build.lock." >&2
    exit 1
fi

case "$action" in
    configure)
        cargo fetch --locked --manifest-path rust/Cargo.toml
        args=(-S . -B "$build_dir" -G Ninja
              -DENABLE_RUST_SDK_BACKEND=ON -DCMAKE_BUILD_TYPE=Debug
              -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
        if [[ "$(uname -s)" == Linux ]]; then
            args+=(-DCMAKE_LINKER_TYPE=MOLD)
        fi
        cmake "${args[@]}" "$@"
        ;;
    build)
        cmake --build "$build_dir" --parallel "$jobs" --target lightning-matrix
        ;;
esac
