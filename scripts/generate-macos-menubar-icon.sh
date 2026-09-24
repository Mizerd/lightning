#!/usr/bin/env bash
# Regenerate the macOS menu-bar (status item) template icon from the tracked
# source artwork.
#
# Usage: scripts/generate-macos-menubar-icon.sh
#
# The macOS menu bar takes a template image: AppKit uses only the alpha
# channel and tints it for the current appearance, so the colour app icon
# cannot be used. Its alpha is also one blob (the bolt sits inside the bubble),
# so this script separates the two marks, outlines the bubble, cuts a gap
# around the bolt and puts the bolt back solid.
#
# Sizes: Qt (qcocoasystemtrayicon.mm) picks the largest pixmap whose height
# fits `NSStatusBar.thickness - 4` points; thickness is 22 or 24, so 18/20/22
# and their 2x are exact, 16 is the floor and 32 covers 1.5x. Output is
# committed so builds never need ImageMagick or librsvg.
set -euo pipefail

cd "$(dirname "$0")/.."
src="data/icons/lightning.svg"
[ -f "$src" ] || { echo "missing $src" >&2; exit 1; }

if command -v magick >/dev/null 2>&1; then
    im=magick
elif command -v convert >/dev/null 2>&1; then
    im=convert
else
    echo "ImageMagick (magick/convert) is required" >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT

# Split on the gradient fill, not path index, so a redrawn source still works
# and one without those gradient ids fails loudly.
python3 - "$src" "$work" <<'PY'
import re
import sys

source, work = sys.argv[1], sys.argv[2]
svg = open(source, encoding="utf-8").read()
paths = re.findall(r"<path[^>]*?/>", svg)
bubble = [p for p in paths if "purpleGradient" in p]
bolt = [p for p in paths if "goldGradient" in p]
if not bubble or not bolt:
    raise SystemExit(
        "the source no longer has a purpleGradient bubble and a goldGradient "
        "bolt; this script cannot separate the two marks")

header = ('<svg xmlns="http://www.w3.org/2000/svg" width="1254" height="1254" '
          'viewBox="0 0 1254 1254" shape-rendering="geometricPrecision">'
          '<g fill-rule="evenodd">')
black = lambda p: re.sub(r'fill="[^"]*"', 'fill="#000000"', p)
for name, group in (("bubble", bubble), ("bolt", bolt)):
    with open(f"{work}/{name}.svg", "w", encoding="utf-8") as out:
        out.write(header + "".join(black(p) for p in group) + "</g></svg>")
PY

# Render at 1024 and work on alpha only.
"$im" -background none "$work/bubble.svg" -resize 1024x1024 \
    -alpha extract "$work/bubble.png"
"$im" -background none "$work/bolt.svg" -resize 1024x1024 \
    -alpha extract "$work/bolt.png"

# Bubble outline: filled shape minus a 40px erosion; thinner vanishes at 1x.
"$im" "$work/bubble.png" -morphology Erode Disk:40 -negate "$work/inner.png"
"$im" "$work/bubble.png" "$work/inner.png" -compose Multiply -composite \
    "$work/ring.png"

# Gap around the bolt keeps the marks from fusing at 18px; the bolt overruns
# the bubble, so it cannot simply be knocked out of a filled shape.
"$im" "$work/bolt.png" -morphology Dilate Disk:26 -negate "$work/boltgap.png"
"$im" "$work/ring.png" "$work/boltgap.png" -compose Multiply -composite \
    "$work/ringgap.png"
"$im" "$work/ringgap.png" "$work/bolt.png" -compose Lighten -composite \
    "$work/mask.png"

out_dir="data/icons/menubar"
mkdir -p "$out_dir"
for size in 16 18 20 22 32 36 40 44; do
    out="$out_dir/lightning-template-${size}.png"
    # AppKit ignores a template's colour; black keeps the file viewable and
    # the bytes deterministic.
    "$im" "$work/mask.png" -trim +repage \
        -filter Lanczos -resize "x${size}" \
        -background black -gravity center -extent "${size}x${size}" \
        -alpha off -colorspace Gray "$work/alpha-${size}.png"
    # PNG32: a grayscale PNG has no alpha and renders as a black square.
    "$im" -size "${size}x${size}" "xc:black" "$work/alpha-${size}.png" \
        -alpha off -compose CopyOpacity -composite \
        -strip "PNG32:$out"
    echo "wrote $out"
done
