#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
ROOT="$(project_dir)"
load_versions
nix flake check "path:$ROOT"
nix path-info "$ROOT/result"
nix path-info --closure-size "$ROOT/result"
nix store verify --no-trust "$ROOT/result"
"$ROOT/result/bin/matrix-client" --version

archive="$ROOT/dist/lightning-${NIX_VERSION}-x86_64-linux-nix-cache.tar.zst"
zstd -t "$archive"
tar --use-compress-program=zstd -tf "$archive" >"$ROOT/dist/nix-archive-contents.txt"
(cd "$ROOT/dist" && sha256sum -c "$(basename "$archive").sha256")

TEST_DIR="$(mktemp -d "$ROOT/work/nix-cache-test.XXXXXX")"
trap 'rm -rf "$TEST_DIR"' EXIT
tar --use-compress-program=zstd -xf "$archive" -C "$TEST_DIR"
nix copy --from "file://$TEST_DIR/nix-cache" "$(readlink -f "$ROOT/result")" --no-check-sigs
nix path-info "$(readlink -f "$ROOT/result")"
