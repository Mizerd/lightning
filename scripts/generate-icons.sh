#!/usr/bin/env bash
# Regenerate the hicolor application icons from the tracked source artwork.
#
# Usage: scripts/generate-icons.sh
#
# The source of truth is data/icons/lightning-source.png (1254×1254 RGBA).
# Since 2026-09-23 it is a render of data/icons/lightning.svg — the
# maintainer's "thick" mark (its three paths copied verbatim) scaled onto a
# light circular plate that fills Flathub's icon-grid circle, so the dark end
# of the ring keeps its contrast on a dark background. Regenerate it with
#     rsvg-convert -w 1254 -h 1254 data/icons/lightning.svg -o tmp.png
#     magick tmp.png -strip PNG32:data/icons/lightning-source.png
# and then run this script. It is still NOT circular-masked:
# generate-logo-source.sh's full-bleed mask is what once amputated the
# speech-bubble tail, and it remains only for opaque square originals.
# Standard hicolor sizes plus 256/512 for high-DPI
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
