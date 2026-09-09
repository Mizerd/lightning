#!/usr/bin/env bash
# Regenerate the macOS menu-bar (status item) template icon from the tracked
# source artwork.
#
# Usage: scripts/generate-macos-menubar-icon.sh
#
# WHY A SEPARATE ASSET EXISTS AT ALL.
#
# The macOS menu bar takes a TEMPLATE image: AppKit reads only its ALPHA
# channel and paints the shape itself, so the icon comes out black on a light
# menu bar, white on a dark one, and inverted while the menu is pulled down.
# The application icon cannot serve: it is a purple speech bubble with a gold
# bolt, and a full-colour mark in the menu bar is wrong on macOS. Recolouring
# it white is wrong in the opposite appearance, which is precisely the problem
# a template image exists to solve, so it is refused here.
#
# Nor can the template be derived from the colour icon at runtime. A template
# is the alpha channel, and the colour icon's alpha is the UNION of the bubble
# and the bolt — one undifferentiated blob, because the bolt sits inside the
# bubble. The two marks have to be separated before they are flattened, which
# is what this script does: it renders the two source groups apart, turns the
# bubble into an outline, cuts a gap where the bolt crosses it, and puts the
# solid bolt back. That reads as a bubble with a bolt in it at 18px, which the
# silhouette does not.
#
# Sizes: the menu bar asks for `NSStatusBar.thickness - 4` points, and Qt
# (qcocoasystemtrayicon.mm) picks the largest available pixmap whose HEIGHT is
# at most that times the device pixel ratio, then centres it. Thickness is 22
# on most Macs and 24 on some, so 18/20/22 (1x) and 36/40/44 (2x) are exact
# hits; 16 is the floor so there is always something small enough, and 32 is
# there for a 1.5x scale. Every one is a real downscale of a 1024px render.
#
# The output is committed so builds and packages never need ImageMagick or
# librsvg, exactly as scripts/generate-icons.sh does.
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

# Split the source into its two marks. The SVG carries exactly three paths:
# two filled with the purple gradient (the bubble) and one with the gold
# gradient (the bolt). Splitting on the FILL rather than on a path index means
# a redrawn source with a different number of bubble paths still works, and a
# source that stops using those two gradient ids fails loudly here instead of
# producing a silently wrong mark.
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

# Render both at 1024 and work in the alpha channel alone; everything below is
# a shape operation and colour never enters it.
"$im" -background none "$work/bubble.svg" -resize 1024x1024 \
    -alpha extract "$work/bubble.png"
"$im" -background none "$work/bolt.svg" -resize 1024x1024 \
    -alpha extract "$work/bolt.png"

# Outline the bubble: the filled shape minus a copy eroded by 40px. 40 gives a
# stroke that survives the 1x downscale (roughly one pixel at 18px, two at
# 36px); thinner disappears into the antialiasing at 1x.
"$im" "$work/bubble.png" -morphology Erode Disk:40 -negate "$work/inner.png"
"$im" "$work/bubble.png" "$work/inner.png" -compose Multiply -composite \
    "$work/ring.png"

# Cut a gap where the bolt crosses the outline, then put the bolt back solid.
# Without the gap the two marks fuse into one blob at 18px; the bolt overruns
# the top of the bubble, so it is not enclosed and cannot simply be knocked
# out of a filled shape either.
"$im" "$work/bolt.png" -morphology Dilate Disk:26 -negate "$work/boltgap.png"
"$im" "$work/ring.png" "$work/boltgap.png" -compose Multiply -composite \
    "$work/ringgap.png"
"$im" "$work/ringgap.png" "$work/bolt.png" -compose Lighten -composite \
    "$work/mask.png"

out_dir="data/icons/menubar"
mkdir -p "$out_dir"
for size in 16 18 20 22 32 36 40 44; do
    out="$out_dir/lightning-template-${size}.png"
    # Black pixels carrying the mask as alpha. AppKit ignores the colour of a
    # template image entirely, but a defined one keeps the file readable in an
    # ordinary viewer and keeps the bytes deterministic.
    "$im" "$work/mask.png" -trim +repage \
        -filter Lanczos -resize "x${size}" \
        -background black -gravity center -extent "${size}x${size}" \
        -alpha off -colorspace Gray "$work/alpha-${size}.png"
    # PNG32 explicitly: an 8-bit grayscale PNG has no alpha channel at all,
    # and a template image with no alpha is a solid black rectangle in the
    # menu bar. `identify` calls the two the same thing until you read the
    # colourspace, which is how that would have shipped.
    "$im" -size "${size}x${size}" "xc:black" "$work/alpha-${size}.png" \
        -alpha off -compose CopyOpacity -composite \
        -strip "PNG32:$out"
    echo "wrote $out"
done
