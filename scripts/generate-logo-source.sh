#!/usr/bin/env bash
# Produce a circular-masked logo from a square raster original.
#
# Not for the committed lightning-source.png: the mask would cut the
# speech-bubble tail. Use only for opaque square originals.
#
# Usage: scripts/generate-logo-source.sh /path/to/square-logo.png
#
# Writes data/icons/lightning-source.png with a full-bleed antialiased
# circular alpha mask; the input must already be square. Then run
# scripts/generate-icons.sh.
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
