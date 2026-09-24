#!/usr/bin/env bash
# Regenerate the development-only screenshot-demo media fixtures.
#
# Small, deterministic, abstract images (gradients and geometry) with no
# third-party content, so the demo's media rows render real pictures through
# the production delegates. Bundled only in LIGHTNING_ENABLE_SCREENSHOT_DEMO
# builds.
#
# Output: resources/screenshot-demo/*.png, *.gif, *.txt (committed).
# Requires ImageMagick. Images are sized for display (<=360px) and quantized
# to keep the set small.
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

# ── Message images ───────────────────────────────────────────────────────
# Abstract art: three-corner gradients (`bary`) plus geometric accents, at the
# aspect ratios the mock declares.
bary() { # W H tlColor trColor blColor out
    "$CONVERT" -strip -size "$1x$2" xc: -sparse-color barycentric \
        "0,0 $3  $1,0 $4  0,$2 $5" "$6"
}

# Landscape "Golden hour by the coast" — 8:5 sunset, with a calm sea band.
bary 720 450 "#123a63" "#f2a24e" "#e86a3a" "$OUT/coast.png"
mk "$OUT/coast.png" -fill "#0c1c33" -draw "rectangle 0,372 720,450" \
    -fill "#0c1c3366" -draw "rectangle 0,356 720,372" "$OUT/coast.png"
# Portrait "natural light" — 2:3 warm duotone.
bary 480 720 "#3a2160" "#ef9bc0" "#8a4dd6" "$OUT/portrait.png"
# Square "album cover crop" — 1:1 vivid, with a thin ring.
bary 600 600 "#0f5c66" "#8fe0a6" "#146a8f" "$OUT/square.png"
mk "$OUT/square.png" -fill none -stroke "#ffffff40" -strokewidth 9 \
    -draw "circle 300,300 300,158" "$OUT/square.png"
# Abstract illustration "wallpaper artwork" — 1:1 concentric aurora.
mk -size 600x600 "radial-gradient:#c05be0-#141a44" \
    -fill none -stroke "#ffffff33" -strokewidth 6 \
    -draw "circle 300,300 300,150" -draw "circle 300,300 300,96" \
    "$OUT/artwork.png"
# "shot-timeline-dark.png": dark-theme hero shot, deep indigo to violet.
bary 720 450 "#3b2f8c" "#2f6be0" "#7a3bd0" "$OUT/shot-timeline.png"
mk "$OUT/shot-timeline.png" \
    -fill "#0f122688" -draw "rectangle 0,0 720,450" \
    "$OUT/shot-timeline.png"
# "palette.png" — the theme swatches, a clean 2x2 board.
mk -size 600x600 xc:"#0d1020" \
    -fill "#12213f" -draw "roundrectangle 30,30 288,288 16,16" \
    -fill "#154b52" -draw "roundrectangle 312,30 570,288 16,16" \
    -fill "#3a2b6f" -draw "roundrectangle 30,312 288,570 16,16" \
    -fill "#eef1f7" -draw "roundrectangle 312,312 570,570 16,16" \
    "$OUT/palette.png"
# Video poster (16:9). No baked play button; the video card draws its own.
bary 640 360 "#1a3f66" "#eab066" "#123452" "$OUT/timelapse.png"
mk "$OUT/timelapse.png" -fill "#0c1f36" -draw "rectangle 0,306 640,360" \
    "$OUT/timelapse.png"

# ── Short animated GIF (3 frames, loops) ─────────────────────────────────
mk -delay 30 -loop 0 \
    -size 360x360 "radial-gradient:#6b9bf0-#274a8f" \
    -size 360x360 "radial-gradient:#a06bf0-#5a2a8f" \
    -size 360x360 "radial-gradient:#4ac0a0-#1f7a6f" \
    "$OUT/loop.gif"

# ── Small document attachment (text) ─────────────────────────────────────
cat > "$OUT/release-notes.txt" <<'DOC'
Lightning 0.6.4 (demo release notes)

- Deferred timeline anchor corrections until the gesture settles.
- Dropped per-delta geometry scans during touchpad scrolling.
- Refreshed the room list and media rows.

(This is a fictional screenshot-demo fixture — not a real release.)
DOC

# Shrink: palette-reduce without dithering (grain bloats PNGs), then maximum
# compression for stable bytes. Avatars use a smaller palette.
"$MOGRIFY" -strip +dither -colors 160 \
    -define png:compression-level=9 -define png:compression-strategy=0 \
    "$OUT"/*.png
"$MOGRIFY" -strip +dither -colors 48 \
    -define png:compression-level=9 "$OUT"/avatar-*.png

echo "generated demo media in $OUT:"
ls -1 "$OUT"
