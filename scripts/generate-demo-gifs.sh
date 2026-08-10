#!/usr/bin/env bash
# Regenerate the animated GIF fixtures used by screenshot-demo mode.
#
# The GIF picker needs a grid of MOVING thumbnails to photograph honestly; demo
# mode has no network and no provider key, so those frames have to be bundled.
# Every output here is derived from a still that is already in
# resources/screenshot-demo/ — same license, same provenance, no new third-party
# asset enters the tree.
#
# Development-only assets: CMake adds resources/screenshot-demo/ to the binary
# ONLY under -DLIGHTNING_ENABLE_SCREENSHOT_DEMO=ON, so none of this ships in a
# release build.
#
#   scripts/generate-demo-gifs.sh
#
# Deterministic: same inputs and same ImageMagick produce the same bytes, so
# rerunning it on an unchanged tree leaves git clean.
set -euo pipefail

cd "$(dirname "$0")/.."
DIR="resources/screenshot-demo"
[ -d "$DIR" ] || { echo "missing $DIR" >&2; exit 1; }

command -v magick >/dev/null || { echo "ImageMagick 'magick' not found" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Common output shape: small, short, and optimized. These are thumbnails in a
# 3-column grid, never full-size media.
FRAMES=12
DELAY=8          # centiseconds -> ~12 fps
EDGE=220         # longest edge

# pan <src> <out> — slow horizontal drift across a crop of the source.
pan() {
  local src="$DIR/$1" out="$DIR/$2" i x
  local w; w=$(magick identify -format "%w" "$src")
  local step=$(( (w / 4) / FRAMES )); [ "$step" -lt 1 ] && step=1
  for ((i=0; i<FRAMES; i++)); do
    x=$(( i * step ))
    magick "$src" -crop "$(( w * 3 / 4 ))x+${x}+0" +repage \
      -resize "${EDGE}x${EDGE}" "$WORK/f$(printf '%02d' $i).png"
  done
  magick -delay "$DELAY" -loop 0 "$WORK"/f*.png -layers Optimize "$out"
  rm -f "$WORK"/f*.png
}

# zoom <src> <out> — gentle push-in and back out, so the loop is seamless.
zoom() {
  local src="$DIR/$1" out="$DIR/$2" i pct
  for ((i=0; i<FRAMES; i++)); do
    # Triangle wave 100 -> 112 -> 100.
    if (( i < FRAMES / 2 )); then pct=$(( 100 + i * 2 ));
    else pct=$(( 100 + (FRAMES - i) * 2 )); fi
    magick "$src" -resize "${EDGE}x${EDGE}" -gravity center \
      -resize "${pct}%" -extent "${EDGE}x${EDGE}" \
      "$WORK/f$(printf '%02d' $i).png"
  done
  magick -delay "$DELAY" -loop 0 "$WORK"/f*.png -layers Optimize "$out"
  rm -f "$WORK"/f*.png
}

# hue <src> <out> — rotate the palette through a full turn.
hue() {
  local src="$DIR/$1" out="$DIR/$2" i deg
  for ((i=0; i<FRAMES; i++)); do
    deg=$(( 100 + (i * 200 / FRAMES) ))
    magick "$src" -resize "${EDGE}x${EDGE}" \
      -modulate "100,120,${deg}" "$WORK/f$(printf '%02d' $i).png"
  done
  magick -delay "$DELAY" -loop 0 "$WORK"/f*.png -layers Optimize "$out"
  rm -f "$WORK"/f*.png
}

pan  coast.png         gif-coast.gif
pan  timelapse.png     gif-city.gif
zoom artwork.png       gif-artwork.gif
zoom portrait.png      gif-portrait.gif
zoom shot-timeline.png gif-interface.gif
hue  square.png        gif-square.gif
hue  palette.png       gif-palette.gif

echo "Regenerated:"
ls -la "$DIR"/gif-*.gif
