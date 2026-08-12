#!/usr/bin/env bash
# Regenerate the hicolor application icons from the tracked source artwork.
#
# Usage: scripts/generate-icons.sh
#
# The source of truth is data/icons/lightning-source.png — since 2026-08-12
# this is the maintainer's RAW transparent "thick" mark (1254×1254 RGBA),
# deliberately NOT circular-masked: the speech-bubble tail crosses the
# inscribed circle, and the full-bleed mask of generate-logo-source.sh was
# what amputated it on every icon surface. That script remains for opaque
# square originals that want the disk treatment. Standard hicolor sizes plus 256/512 for high-DPI
# displays (taskbars, window switchers, the Windows .ico) are produced with
# deterministic Lanczos downscaling — every size is a real downscale of the
# high-resolution source, so nothing is upscaled or pixelated. The generated
# files are committed so builds and packages never need ImageMagick.
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
