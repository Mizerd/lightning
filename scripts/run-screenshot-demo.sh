#!/usr/bin/env bash
# Development-only screenshot/demo launcher for Lightning.
#
# Builds a DEDICATED development build tree with the screenshot-demo compile
# option enabled and launches the REAL Lightning UI on the in-memory mock
# backend with three deterministic fictional accounts — for promotional
# screenshots.
#
#   * No network, no Matrix credentials, no real homeserver, no crypto store,
#     no libsecret / production SecretStore.
#   * Storage is isolated: XDG_{DATA,CONFIG,CACHE}_HOME point at a dedicated
#     demo directory, so the demo NEVER reads or writes your real Lightning
#     configuration. (The app additionally uses an isolated applicationName.)
#   * This mode CANNOT exist in a release binary: the compile option is
#     development-only and is rejected in a Rust-only release build.
#
# Usage:
#   scripts/run-screenshot-demo.sh                      # build + launch demo
#   scripts/run-screenshot-demo.sh --reset              # remove the demo profile
#   scripts/run-screenshot-demo.sh --list               # print every scenario
#   scripts/run-screenshot-demo.sh --scenario main-chat
#   scripts/run-screenshot-demo.sh --scenario channels-home
#   scripts/run-screenshot-demo.sh --theme ocean
#   scripts/run-screenshot-demo.sh --appearance dark
#   scripts/run-screenshot-demo.sh --size 1440x900
#   scripts/run-screenshot-demo.sh --account work
#   scripts/run-screenshot-demo.sh --hide-controls
#   scripts/run-screenshot-demo.sh \
#       --scenario main-chat --theme ocean --size 1440x900 --hide-controls
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

# The valid screenshot scenarios (kept in sync with ScreenshotDemoController).
VALID_SCENARIOS="channels-home channels-space channels-people classic-home \
 call-grid call-screen-share \
home-overview main-chat direct-message development \
media-gallery thread-view poll settings-themes account-switching security \
invite work-overview community-overview responsive-chat \
menu-message menu-room find-in-room quick-switcher quick-switcher-command emoji-picker \
gif-picker member-profile mention-popup trust-card new-conversation \
settings-search invite-people create-poll"

usage() {
    sed -n '3,40p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

# --- Strong path validation before any deletion --------------------------
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

do_reset() {
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
}

# --- Parse launcher arguments --------------------------------------------
RESET=0
LIST=0
SCENARIO=""
THEME=""
APPEARANCE=""
SIZE=""
ACCOUNT=""
HIDE_CONTROLS=0
EXTRA_ARGS=()

need_value() {
    if [ -z "${2:-}" ]; then
        echo "error: $1 requires a value" >&2; exit 2
    fi
}

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage 0 ;;
        --reset) RESET=1; shift ;;
        --list) LIST=1; shift ;;
        --scenario) need_value "$1" "${2:-}"; SCENARIO="$2"; shift 2 ;;
        --theme) need_value "$1" "${2:-}"; THEME="$2"; shift 2 ;;
        --appearance) need_value "$1" "${2:-}"; APPEARANCE="$2"; shift 2 ;;
        --size) need_value "$1" "${2:-}"; SIZE="$2"; shift 2 ;;
        --account) need_value "$1" "${2:-}"; ACCOUNT="$2"; shift 2 ;;
        --hide-controls) HIDE_CONTROLS=1; shift ;;
        --) shift; EXTRA_ARGS=("$@"); break ;;
        *) echo "error: unknown argument '$1' (see --help)" >&2; exit 2 ;;
    esac
done

if [ "$RESET" = 1 ]; then
    do_reset
    exit 0
fi

if [ "$LIST" = 1 ]; then
    echo "Scenarios (--scenario <id>):"
    for sc in $VALID_SCENARIOS; do echo "  $sc"; done
    echo
    echo "Themes (--theme <name|id>): storm is the brand theme; indigo-night is"
    echo "the flagship dark and moss-light the flagship light. --appearance"
    echo "light|dark picks those two by name."
    exit 0
fi

# --- Validate values -----------------------------------------------------
if [ -n "$SCENARIO" ]; then
    ok=0
    for s in $VALID_SCENARIOS; do [ "$s" = "$SCENARIO" ] && ok=1 && break; done
    if [ "$ok" = 0 ]; then
        echo "error: unknown scenario '$SCENARIO'" >&2
        echo "valid: $VALID_SCENARIOS" >&2
        exit 2
    fi
fi
if [ -n "$SIZE" ]; then
    if [ "$SIZE" != "narrow" ] && [ "$SIZE" != "wide" ] \
       && ! [[ "$SIZE" =~ ^[0-9]+x[0-9]+$ ]]; then
        echo "error: invalid --size '$SIZE' (use WxH, e.g. 1440x900, or narrow/wide)" >&2
        exit 2
    fi
fi
case "$ACCOUNT" in
    ""|personal|work|community|alex|taylor|nova) : ;;
    *) echo "error: invalid --account '$ACCOUNT' (personal|work|community)" >&2; exit 2 ;;
esac

# --- Isolated storage ----------------------------------------------------
validate_demo_dir
mkdir -p "$DEMO_DIR/data" "$DEMO_DIR/config" "$DEMO_DIR/cache"
: > "$DEMO_DIR/$MARKER"
export XDG_DATA_HOME="$DEMO_DIR/data"
export XDG_CONFIG_HOME="$DEMO_DIR/config"
export XDG_CACHE_HOME="$DEMO_DIR/cache"

# --- Build the demo tree (development option ON; mock backend, no Rust) ---
nix develop -c cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DLIGHTNING_ENABLE_SCREENSHOT_DEMO=ON >/dev/null
nix develop -c cmake --build "$BUILD_DIR" --target matrix-client

# --- Assemble the development-only demo CLI args -------------------------
APP_ARGS=(--screenshot-demo)
[ -n "$SCENARIO" ]   && APP_ARGS+=("--demo-scenario=$SCENARIO")
[ -n "$ACCOUNT" ]    && APP_ARGS+=("--demo-account=$ACCOUNT")
[ -n "$THEME" ]      && APP_ARGS+=("--demo-theme=$THEME")
[ -n "$APPEARANCE" ] && APP_ARGS+=("--demo-appearance=$APPEARANCE")
[ -n "$SIZE" ]       && APP_ARGS+=("--demo-size=$SIZE")
[ "$HIDE_CONTROLS" = 1 ] && APP_ARGS+=(--demo-hide-controls)

# --- Concise summary before launch ---------------------------------------
echo "── Lightning screenshot demo ───────────────────────────────"
echo "  storage:     $DEMO_DIR (isolated; no network, no libsecret)"
echo "  scenario:    ${SCENARIO:-home-overview (default)}"
echo "  account:     ${ACCOUNT:-personal (default)}"
echo "  theme:       ${THEME:-scenario default}"
echo "  appearance:  ${APPEARANCE:-scenario default}"
echo "  size:        ${SIZE:-scenario default}"
echo "  controls:    $([ "$HIDE_CONTROLS" = 1 ] && echo hidden || echo visible) (Ctrl+Shift+D toggles)"
echo "────────────────────────────────────────────────────────────"

exec nix develop -c "./$BUILD_DIR/matrix-client" "${APP_ARGS[@]}" "${EXTRA_ARGS[@]}"
