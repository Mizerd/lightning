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

archive_name="lightning-${NIX_VERSION}-x86_64-linux-nix-cache.tar.zst"
archive="$ROOT/work/$archive_name"
zstd -t "$archive"
tar --use-compress-program=zstd -tf "$archive" >"$ROOT/dist/nix-archive-contents.txt"
# The archive stays under work/ (it is published to the package registry, not
# shipped as an artifact); its basename-relative checksum lives in dist/.
(cd "$ROOT/work" && sha256sum -c "$ROOT/dist/${archive_name}.sha256")

# Confirm the registry parts reassemble byte-for-byte into the published archive.
cat "$ROOT/work/${archive_name}.part."* >"$ROOT/work/reassembled.tar.zst"
reassembled_sha="$(sha256sum "$ROOT/work/reassembled.tar.zst" | cut -d' ' -f1)"
expected_sha="$(cut -d' ' -f1 <"$ROOT/dist/${archive_name}.sha256")"
[[ "$reassembled_sha" == "$expected_sha" ]] || die "reassembled parts do not match the archive checksum"
rm -f "$ROOT/work/reassembled.tar.zst"

TEST_DIR="$(mktemp -d "$ROOT/work/nix-cache-test.XXXXXX")"
trap 'rm -rf "$TEST_DIR"' EXIT
tar --use-compress-program=zstd -xf "$archive" -C "$TEST_DIR"
nix copy --from "file://$TEST_DIR/nix-cache" "$(readlink -f "$ROOT/result")" --no-check-sigs
nix path-info "$(readlink -f "$ROOT/result")"
