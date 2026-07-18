#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
ROOT="$(project_dir)"
load_versions
BUILD_JOBS="${BUILD_JOBS:-2}"
[[ "$BUILD_JOBS" =~ ^[12]$ ]] || die "BUILD_JOBS must be 1 or 2"

printf 'Nix: '; nix --version
mkdir -p "$ROOT/work/cargo-home" "$ROOT/dist/nix-cache"
nix develop "path:$ROOT/work/lightning" --command \
    cargo vendor --locked --manifest-path "$ROOT/work/lightning/rust/Cargo.toml" \
    "$ROOT/work/cargo-vendor" >"$ROOT/work/vendor-config.raw"

cat >"$ROOT/work/cargo-config.toml" <<'EOF'
[source.crates-io]
replace-with = "vendored-sources"

[source.vendored-sources]
directory = "@VENDOR_DIR@"

[net]
offline = true
EOF

nix flake check "path:$ROOT" --no-build
nix build "path:$ROOT" --out-link "$ROOT/result" \
    --cores "$BUILD_JOBS" --max-jobs "$BUILD_JOBS"

nix copy --to "file://$ROOT/dist/nix-cache" "$ROOT/result"
nix path-info --json "$ROOT/result" >"$ROOT/dist/nix-path-info.json"
nix path-info --closure-size "$ROOT/result" >"$ROOT/dist/nix-closure-size.txt"

ARCHIVE="$ROOT/dist/lightning-${NIX_VERSION}-x86_64-linux-nix-cache.tar.zst"
tar --sort=name --mtime='UTC 1970-01-01' -C "$ROOT/dist" -cf - nix-cache |
    zstd -T2 -10 -o "$ARCHIVE"
write_sha256 "$ARCHIVE"

cat >"$ROOT/dist/nix-install-instructions.txt" <<EOF
Extract the archive, then copy the closure into the local Nix store:

  tar --use-compress-program=zstd -xf $(basename "$ARCHIVE")
  nix copy --from file://\$PWD/nix-cache $(readlink -f "$ROOT/result")

The recorded output path is also available in nix-path-info.json.
EOF
