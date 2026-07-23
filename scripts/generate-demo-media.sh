#!/usr/bin/env bash
# Regenerate the development-only screenshot-demo media fixtures.
#
# These are small, deterministic, ABSTRACT raster images (gradients + simple
# geometry) generated entirely by this script — no photographs, no real people,
# no third-party/commercial artwork, no network. They exist only so the demo's
# image/video/GIF/avatar rows render as real pictures through the production
# media delegates (see docs/screenshot-demo.md → Media fixtures). They are bundled
# ONLY in a LIGHTNING_ENABLE_SCREENSHOT_DEMO build and excluded from every
# release artifact.
#
# Output: resources/screenshot-demo/*.png, *.gif, *.txt (committed).
# Requires ImageMagick (`convert`). Re-running reproduces the same images.
#
# Fixtures are generated at display resolution (the timeline shows images at
# <=360px, so larger is wasted bytes) while keeping each declared aspect ratio,
# and quantized to a compact palette so the whole set stays tiny (<~150 KB).
#
#   scripts/generate-demo-media.sh
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="resources/screenshot-demo"
mkdir -p "$OUT"

CONVERT="${IMAGEMAGICK_CONVERT:-convert}"
if ! command -v "$CONVERT" >/dev/null 2>&1; then
    echo "error: ImageMagick 'convert' not found (set IMAGEMAGICK_CONVERT)" >&2
    exit 1
fi
MOGRIFY="${IMAGEMAGICK_MOGRIFY:-mogrify}"

mk() { "$CONVERT" -strip "$@"; }

# ── Abstract avatars (square; the provider circle-masks them) ────────────
# One stable hue per fictional person + a soft radial highlight and an initial.
avatar() { # name hexA hexB initial
    local name="$1" a="$2" b="$3" ch="$4"
    mk -size 224x224 "radial-gradient:${a}-${b}" \
        -gravity center -pointsize 120 -font DejaVu-Sans-Bold \
        -fill "#ffffffcc" -annotate 0 "$ch" \
        "$OUT/avatar-${name}.png"
}
avatar alex   "#5b8def" "#2b4a8f" "A"
avatar taylor "#e0794a" "#a8431f" "T"
avatar nova   "#7b5bef" "#3d2b8f" "N"
avatar maya   "#ef5b9c" "#8f2b5a" "M"
avatar jordan "#4ac0a0" "#1f7a5f" "J"
avatar sam    "#efc14a" "#a8801f" "S"
avatar aisha  "#5bcfef" "#1f6f8f" "A"
avatar noah   "#8fb04a" "#4f6f1f" "N"
avatar priya  "#ef7b5b" "#a8431f" "P"
avatar leo    "#5b6bef" "#2b358f" "L"

# ── Message images (aspect ratio matches the mock's declared media sizes) ─
# Landscape (Photography "coast") — 8:5.
mk -size 720x450 "gradient:#0f2a4a-#e08a4a" \
    -fill "#ffffff22" -draw "circle 540,110 540,55" \
    "$OUT/coast.png"
# Portrait — 2:3.
mk -size 480x720 "gradient:#3a1f6f-#e05b9c" \
    -fill "#ffffff1a" -draw "rectangle 70,420 410,456" \
    "$OUT/portrait.png"
# Square artwork — 1:1.
mk -size 600x600 "gradient:#1f6f8f-#7be0c0" \
    -fill "#ffffff22" -draw "circle 300,300 300,130" \
    "$OUT/square.png"
# Photo-style abstract illustration (concentric) — 1:1.
mk -size 600x600 "radial-gradient:#e0c04a-#8f2b5a" \
    -fill none -stroke "#ffffff33" -strokewidth 8 \
    -draw "circle 300,300 300,150" -draw "circle 300,300 300,90" \
    "$OUT/artwork.png"
# "Screenshot-of-the-app" hero shot (dark UI-ish bars) — 8:5.
mk -size 720x450 "gradient:#12151c-#1c2230" \
    -fill "#5b8def55" -draw "rectangle 34,34 686,68" \
    -fill "#ffffff14" -draw "rectangle 34,100 350,136" \
    -draw "rectangle 34,170 506,206" -draw "rectangle 34,240 426,276" \
    "$OUT/shot-timeline.png"
# Colour palette board (theme swatches) — 1:1.
mk -size 600x600 xc:"#12151c" \
    -fill "#1a2740" -draw "rectangle 300,0 600,300" \
    -fill "#3a2b6f" -draw "rectangle 0,300 300,600" \
    -fill "#f5f5f7" -draw "rectangle 300,300 600,600" \
    "$OUT/palette.png"
# Video poster (16:9) with a play triangle.
mk -size 640x360 "gradient:#1a3a5a-#e0a04a" \
    -fill "#00000033" -draw "circle 320,180 320,140" \
    -fill "#ffffffcc" -draw "polygon 305,160 305,200 345,180" \
    "$OUT/timelapse.png"

# ── Short animated GIF (3 frames, loops) ─────────────────────────────────
mk -delay 30 -loop 0 \
    -size 360x360 "gradient:#5b8def-#7be0c0" \
    -size 360x360 "gradient:#7b5bef-#ef5b9c" \
    -size 360x360 "gradient:#4ac0a0-#efc14a" \
    "$OUT/loop.gif"

# ── Small document attachment (text) ─────────────────────────────────────
cat > "$OUT/release-notes.txt" <<'DOC'
Lightning 0.6.4 (demo release notes)

- Deferred timeline anchor corrections until the gesture settles.
- Dropped per-delta geometry scans during touchpad scrolling.
- Refreshed the room list and media rows.

(This is a fictional screenshot-demo fixture — not a real release.)
DOC

# ── Shrink: quantize smooth gradients + max PNG compression (stable bytes) ─
"$MOGRIFY" -strip -colors 64 \
    -define png:compression-level=9 -define png:compression-strategy=0 \
    "$OUT"/*.png

echo "generated demo media in $OUT:"
ls -1 "$OUT"
