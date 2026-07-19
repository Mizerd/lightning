#!/usr/bin/env bash
# Run a from-source Lightning build with the local developer environment.
#
# Loads the untracked, gitignored ./lightning-gif.env (if present), which
# exports LIGHTNING_GIPHY_API_KEY / LIGHTNING_KLIPY_API_KEY — the runtime
# override with the highest precedence in gif::resolveProviderKey — so the
# GIF picker works in source runs that have no compiled-in keys. The file's
# contents are never printed or logged.
#
# Usage: scripts/run-dev.sh [extra matrix-client args]
#   default backend: rust; default build tree: build-rust
set -euo pipefail
cd "$(dirname "$0")/.."

if [ -f lightning-gif.env ]; then
    set -a
    # shellcheck disable=SC1091
    . ./lightning-gif.env
    set +a
fi

BINARY=${LIGHTNING_DEV_BINARY:-./build-rust/matrix-client}
[ -x "$BINARY" ] || {
    echo "missing $BINARY — configure/build first (see CLAUDE.md)" >&2
    exit 1
}
exec nix develop -c "$BINARY" --backend="${LIGHTNING_DEV_BACKEND:-rust}" "$@"
