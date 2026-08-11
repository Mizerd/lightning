#!/usr/bin/env bash
# Produce the canonical circular Lightning logo from a square raster original.
#
# Usage: scripts/generate-logo-source.sh /path/to/square-logo.png
#
# The maintainer-supplied original stays wherever it lives (it is never
# modified, and it is NOT tracked in this repository — the committed
# lightning-source.png is the processed canonical form, recoverable from git
# history but only regenerable from the original); this script writes the
# processed canonical source the icon pipeline consumes:
# data/icons/lightning-source.png. Processing is a single
# deterministic step — a full-bleed antialiased circular alpha mask, so the
# icon is a clean disk with fully transparent corners (Element-style circular
# treatment) instead of an opaque square. Aspect ratio is preserved: the
# input must already be square, and nothing is stretched. After running this,
# run scripts/generate-icons.sh to regenerate the hicolor sizes.
set -euo pipefail

cd "$(dirname "$0")/.."
src="${1:?usage: generate-logo-source.sh <square-logo.png>}"
out="data/icons/lightning-source.png"
[ -f "$src" ] || { echo "missing $src" >&2; exit 1; }

dims="$(magick identify -format "%w %h" "$src")"
w="${dims% *}"
h="${dims#* }"
if [ "$w" != "$h" ]; then
    echo "source must be square, got ${w}x${h}" >&2
    exit 1
fi

cx=$(awk "BEGIN { printf \"%.1f\", $w / 2 }")
magick "$src" -alpha set \
    \( -size "${w}x${h}" xc:none -fill white \
       -draw "circle ${cx},${cx} ${cx},0.5" \) \
    -compose DstIn -composite -strip "$out"
echo "wrote $out (${w}x${h}, circular alpha)"
