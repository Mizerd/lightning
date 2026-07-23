#!/usr/bin/env bash
# Development-only screenshot/demo launcher for Lightning.
#
# Builds a DEDICATED development build tree with the screenshot-demo compile
# option enabled and launches the REAL Lightning UI on the in-memory mock
# backend with deterministic fake accounts/rooms — for promotional screenshots.
#
#   * No network, no Matrix credentials, no real homeserver, no crypto store.
#   * Storage is isolated: XDG_{DATA,CONFIG,CACHE}_HOME point at a dedicated
#     demo directory, so the demo NEVER reads or writes your real Lightning
#     configuration. (The app additionally uses an isolated applicationName.)
#   * This mode CANNOT exist in a release binary: the compile option is
#     development-only and is rejected in a Rust-only release build.
#
# Usage:
#   scripts/run-screenshot-demo.sh                 # build + launch demo
#   scripts/run-screenshot-demo.sh --reset         # remove the demo profile
#   scripts/run-screenshot-demo.sh -- <extra app args...>
#
# Environment:
#   LIGHTNING_DEMO_DIR   Override the isolated demo directory
#                        (default: $HOME/.local/share/lightning-screenshot-demo).
#   LIGHTNING_DEMO_BUILD Override the demo build tree (default: build-demo).
set -euo pipefail
cd "$(dirname "$0")/.."

DEMO_DIR="${LIGHTNING_DEMO_DIR:-$HOME/.local/share/lightning-screenshot-demo}"
BUILD_DIR="${LIGHTNING_DEMO_BUILD:-build-demo}"
MARKER=".lightning-screenshot-demo"

# --- Strong path validation before any deletion --------------------------
# Refuse to delete anything that is not our own, clearly-named, absolute demo
# directory living under $HOME. Never a broad/ambiguous recursive delete.
validate_demo_dir() {
    case "$DEMO_DIR" in
        "$HOME"/*) : ;;
        *) echo "refusing: demo dir '$DEMO_DIR' is not under \$HOME" >&2; exit 1 ;;
    esac
    case "$DEMO_DIR" in
        *"lightning-screenshot-demo"*) : ;;
        *) echo "refusing: demo dir '$DEMO_DIR' is not a lightning-screenshot-demo path" >&2; exit 1 ;;
    esac
    if [ "$DEMO_DIR" = "/" ] || [ "$DEMO_DIR" = "$HOME" ]; then
        echo "refusing: demo dir '$DEMO_DIR' is a protected path" >&2; exit 1
    fi
}

if [ "${1:-}" = "--reset" ]; then
    validate_demo_dir
    if [ -d "$DEMO_DIR" ]; then
        if [ ! -e "$DEMO_DIR/$MARKER" ]; then
            echo "refusing: '$DEMO_DIR' has no $MARKER marker (not created by this script)" >&2
            exit 1
        fi
        rm -rf -- "$DEMO_DIR"
        echo "removed screenshot-demo profile: $DEMO_DIR"
    else
        echo "nothing to reset: $DEMO_DIR does not exist"
    fi
    exit 0
fi

# Pass-through extra app args after a literal `--`.
EXTRA_ARGS=()
if [ "${1:-}" = "--" ]; then
    shift
    EXTRA_ARGS=("$@")
fi

# --- Isolated storage ----------------------------------------------------
validate_demo_dir
mkdir -p "$DEMO_DIR/data" "$DEMO_DIR/config" "$DEMO_DIR/cache"
: > "$DEMO_DIR/$MARKER"
export XDG_DATA_HOME="$DEMO_DIR/data"
export XDG_CONFIG_HOME="$DEMO_DIR/config"
export XDG_CACHE_HOME="$DEMO_DIR/cache"

# --- Build the demo tree (development option ON; mock backend, no Rust) ---
# The screenshot demo uses the in-memory mock backend, so the demo tree does
# not need the Rust SDK — a plain mock/http build with the demo option is much
# faster to configure and build.
nix develop -c cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DLIGHTNING_ENABLE_SCREENSHOT_DEMO=ON >/dev/null
nix develop -c cmake --build "$BUILD_DIR" --target matrix-client

echo "launching Lightning screenshot demo (isolated storage: $DEMO_DIR)"
exec nix develop -c "./$BUILD_DIR/matrix-client" --screenshot-demo "${EXTRA_ARGS[@]}"
