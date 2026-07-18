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
mkdir -p "$ROOT/work/cargo-home" "$ROOT/dist"

# Vendor the locked Rust crates using Lightning's own dev shell so the flake
# build can run fully offline. All dependencies come from crates.io (no git
# sources), so a single crates-io replacement is sufficient.
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

nix path-info --json "$ROOT/result" >"$ROOT/dist/nix-path-info.json"
nix path-info --closure-size "$ROOT/result" >"$ROOT/dist/nix-closure-size.txt"

# Export a portable file-based binary cache and archive it. Both live under work/
# (not dist/) because the closure is far larger than GitLab's per-artifact limit;
# only small metadata is shipped as a job artifact.
CACHE_DIR="$ROOT/work/nix-cache"
mkdir -p "$CACHE_DIR"
nix copy --to "file://$CACHE_DIR" "$ROOT/result"

ARCHIVE_NAME="lightning-${NIX_VERSION}-x86_64-linux-nix-cache.tar.zst"
ARCHIVE="$ROOT/work/$ARCHIVE_NAME"
tar --sort=name --mtime='UTC 1970-01-01' -C "$ROOT/work" -cf - nix-cache |
    zstd -T2 -10 -o "$ARCHIVE"
( cd "$ROOT/work" && sha256sum "$ARCHIVE_NAME" >"$ROOT/dist/$ARCHIVE_NAME.sha256" )
ARCHIVE_SHA="$(cut -d' ' -f1 <"$ROOT/dist/$ARCHIVE_NAME.sha256")"
ARCHIVE_SIZE="$(stat -c %s "$ARCHIVE")"

# The archive is larger than the 100 MB request-body limit imposed by the
# Cloudflare edge in front of GitLab, so split it into <100 MB parts and publish
# each to the project generic package registry with CI_JOB_TOKEN. Only a manifest
# and checksums are kept as the job artifact.
PKG_NAME="lightning-nix-cache"
REGISTRY_BASE="${CI_API_V4_URL:-}/projects/${CI_PROJECT_ID:-}/packages/generic/${PKG_NAME}/${NIX_VERSION}"
( cd "$ROOT/work" && split -b 90M -d -a 2 "$ARCHIVE_NAME" "${ARCHIVE_NAME}.part." )
mapfile -t PARTS < <(cd "$ROOT/work" && ls "${ARCHIVE_NAME}.part."* | sort)

PUBLISHED=false
if [[ -n "${CI_JOB_TOKEN:-}" && -n "${CI_API_V4_URL:-}" && -n "${CI_PROJECT_ID:-}" ]]; then
    for part in "${PARTS[@]}"; do
        curl --fail --show-error --silent \
            --header "JOB-TOKEN: ${CI_JOB_TOKEN}" \
            --upload-file "$ROOT/work/$part" "${REGISTRY_BASE}/${part}"
    done
    PUBLISHED=true
    printf 'Published %d parts of %s (%s bytes) to the generic package registry\n' \
        "${#PARTS[@]}" "$ARCHIVE_NAME" "$ARCHIVE_SIZE"
else
    printf 'CI job-token context unavailable; skipping registry upload\n' >&2
fi

PARTS_JSON="$(cd "$ROOT/work" && for part in "${PARTS[@]}"; do
    jq -n --arg name "$part" \
          --arg sha256 "$(sha256sum "$part" | cut -d' ' -f1)" \
          --argjson size_bytes "$(stat -c %s "$part")" \
          --arg url "${REGISTRY_BASE}/${part}" \
          '{name:$name, sha256:$sha256, size_bytes:$size_bytes, url:$url}'
done | jq -s .)"

jq -n \
    --arg filename "$ARCHIVE_NAME" \
    --arg sha256 "$ARCHIVE_SHA" \
    --argjson size_bytes "$ARCHIVE_SIZE" \
    --arg package_name "$PKG_NAME" \
    --arg package_version "$NIX_VERSION" \
    --argjson published "$PUBLISHED" \
    --argjson parts "$PARTS_JSON" \
    '{filename:$filename, sha256:$sha256, size_bytes:$size_bytes,
      registry_package:$package_name, registry_version:$package_version,
      published_to_registry:$published, parts:$parts,
      reassemble:("cat " + ($parts | map(.name) | join(" ")) + " > " + $filename),
      note:"Archive exceeds the 100 MB Cloudflare request-body limit; it is split and stored in the project generic package registry, not as a job artifact."}' \
    >"$ROOT/dist/nix-package-manifest.json"

STORE_PATH="$(readlink -f "$ROOT/result")"
PART_LIST="$(printf '%s ' "${PARTS[@]}")"
cat >"$ROOT/dist/nix-install-instructions.txt" <<EOF
Lightning Nix binary-cache archive: ${ARCHIVE_NAME}
Store path: ${STORE_PATH}
SHA-256:    ${ARCHIVE_SHA}
Size:       ${ARCHIVE_SIZE} bytes

The archive is split into <100 MB parts and stored in the project generic
package registry (Cloudflare caps request bodies at 100 MB). Download every
part with a GitLab token (api or read_package_registry scope):

$(for part in "${PARTS[@]}"; do
    printf '  curl --header "PRIVATE-TOKEN: <token>" --output %s "%s/%s"\n' "$part" "$REGISTRY_BASE" "$part"
done)

Reassemble, verify, and import into a local Nix store:

  cat ${PART_LIST}> ${ARCHIVE_NAME}
  sha256sum -c ${ARCHIVE_NAME}.sha256
  tar --use-compress-program=zstd -xf ${ARCHIVE_NAME}
  nix copy --no-check-sigs --from file://\$PWD/nix-cache ${STORE_PATH}
  ${STORE_PATH}/bin/matrix-client --version
EOF
