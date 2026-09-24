#!/usr/bin/env bash
# Regenerate the hicolor application icons from the tracked source artwork.
#
# Usage: scripts/generate-icons.sh
#
# Source: data/icons/lightning-source.png, the raw transparent mark. It is
# not circular-masked because the speech-bubble tail crosses the inscribed
# circle. Every size is a Lanczos downscale of that source; the outputs are
# committed so builds never need ImageMagick.
set -euo pipefail

cd "$(dirname "$0")/.."
src="data/icons/lightning-source.png"
[ -f "$src" ] || { echo "missing $src" >&2; exit 1; }

for size in 16 32 48 64 128 192 256 512; do
    out="data/icons/hicolor/${size}x${size}/apps/lightning.png"
    mkdir -p "$(dirname "$out")"
    magick "$src" -filter Lanczos -resize "${size}x${size}!" -strip "$out" \
        2>/dev/null || convert "$src" -filter Lanczos -resize "${size}x${size}!" -strip "$out"
    echo "wrote $out"
done
