#!/usr/bin/env python3
"""Regenerate the screenshot-demo's shared-screen frame.

Development only. The staged demo call (SfuCallController::startDemoCall)
shows one participant sharing their screen; this is the still picture that
share displays: a festival poster on a design canvas, matching the Design
Lounge conversation. Built from shapes defined here, the demo's own lake
illustration (resources/screenshot-demo/lake.png, from
scripts/generate-demo-scenes.py) and the Noto Sans font, so nothing is fetched
and nothing is third-party artwork.

Output: resources/screenshot-demo/share-poster.jpg, 1920x1080.
Requires ImageMagick with an SVG delegate (librsvg) and Noto Sans installed.

    scripts/generate-demo-share.py
"""
from __future__ import annotations

import base64
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "resources", "screenshot-demo")
PHOTO = os.path.join(OUT_DIR, "lake.png")
FONT = "Noto Sans"

W, H = 1920, 1080
NAVY = "#14213d"
ORANGE = "#f28c38"
CREAM = "#fdf3e1"
SWATCHES = [("#14213d", "Headline"), ("#f28c38", "Dates"), ("#f6c177", "Glow"),
            ("#5e7a8c", "Water"), ("#fdf3e1", "Paper")]


def qr(x: int, y: int, size: int) -> str:
    """A deterministic, QR-like block (decorative; it encodes nothing)."""
    n = 21
    cell = size / n
    out = [f'<rect x="{x}" y="{y}" width="{size}" height="{size}" fill="{CREAM}"/>']
    seed = 0x5EED
    for r in range(n):
        for c in range(n):
            seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
            finder = any(r0 <= r < r0 + 7 and c0 <= c < c0 + 7
                         for r0, c0 in ((0, 0), (0, n - 7), (n - 7, 0)))
            if finder:
                rr, cc = r % 7 if r < 7 else (r - (n - 7)), c % 7 if c < 7 else (c - (n - 7))
                on = rr in (0, 6) or cc in (0, 6) or (2 <= rr <= 4 and 2 <= cc <= 4)
            else:
                on = (seed >> 7) & 1
            if on:
                out.append(f'<rect x="{x + c * cell:.2f}" y="{y + r * cell:.2f}" '
                           f'width="{cell + 0.3:.2f}" height="{cell + 0.3:.2f}" '
                           f'fill="{NAVY}"/>')
    return "".join(out)


def svg(photo_href: str) -> str:
    # Poster: A-series ratio, centred on the canvas.
    ph = 900
    pw = int(ph / 1.414)
    px = (W - pw) // 2 - 120
    py = 120
    photo_h = int(ph * 0.58)
    t = []
    t.append(f'<svg xmlns="http://www.w3.org/2000/svg" '
             f'xmlns:xlink="http://www.w3.org/1999/xlink" width="{W}" height="{H}">')
    t.append('<defs>'
             f'<clipPath id="photo"><rect x="{px}" y="{py}" width="{pw}" height="{photo_h}"/></clipPath>'
             f'<linearGradient id="fade" x1="0" y1="0" x2="0" y2="1">'
             f'<stop offset="0.55" stop-color="{NAVY}" stop-opacity="0"/>'
             f'<stop offset="1" stop-color="{NAVY}" stop-opacity="1"/></linearGradient>'
             '<filter id="shadow" x="-10%" y="-10%" width="120%" height="120%">'
             '<feDropShadow dx="0" dy="18" stdDeviation="22" flood-opacity="0.45"/></filter>'
             '</defs>')
    # Canvas, top bar and tool strip of a generic design app.
    t.append(f'<rect width="{W}" height="{H}" fill="#2b2d33"/>')
    dots = "".join(f'<circle cx="{x}" cy="{y}" r="1.4" fill="#3a3d45"/>'
                   for x in range(80, W - 320, 32) for y in range(80, H, 32))
    t.append(dots)
    t.append(f'<rect width="{W}" height="56" fill="#1f2126"/>')
    t.append(f'<text x="28" y="36" font-family="{FONT}" font-size="20" '
             f'fill="#e8e8ec" font-weight="600">Lakeside Lights</text>')
    t.append(f'<text x="196" y="36" font-family="{FONT}" font-size="20" '
             f'fill="#8d909a">/  Poster A2  ·  v4</text>')
    t.append(f'<rect x="{W - 140}" y="14" width="112" height="30" rx="8" fill="{ORANGE}"/>'
             f'<text x="{W - 84}" y="35" text-anchor="middle" font-family="{FONT}" '
             f'font-size="17" font-weight="700" fill="{NAVY}">Export</text>')
    t.append(f'<text x="{W - 170}" y="36" text-anchor="end" font-family="{FONT}" '
             f'font-size="18" fill="#8d909a">100%</text>')
    t.append(f'<rect x="0" y="56" width="56" height="{H - 56}" fill="#1f2126"/>')
    for i in range(6):
        y = 84 + i * 56
        fill = "#3d4049" if i else ORANGE
        t.append(f'<rect x="12" y="{y}" width="32" height="32" rx="7" fill="{fill}"/>')
    # Inspector with the palette.
    ix = W - 300
    t.append(f'<rect x="{ix}" y="56" width="300" height="{H - 56}" fill="#1f2126"/>')
    t.append(f'<text x="{ix + 28}" y="104" font-family="{FONT}" font-size="19" '
             f'font-weight="700" fill="#e8e8ec">Palette</text>')
    t.append(f'<text x="{ix + 28}" y="132" font-family="{FONT}" font-size="15" '
             f'fill="#8d909a">From lake-at-sunrise.png</text>')
    for i, (hexc, name) in enumerate(SWATCHES):
        y = 160 + i * 68
        t.append(f'<rect x="{ix + 28}" y="{y}" width="52" height="52" rx="10" '
                 f'fill="{hexc}" stroke="#3d4049" stroke-width="1.5"/>')
        t.append(f'<text x="{ix + 96}" y="{y + 23}" font-family="{FONT}" '
                 f'font-size="17" font-weight="600" fill="#e8e8ec">{name}</text>')
        t.append(f'<text x="{ix + 96}" y="{y + 45}" font-family="{FONT}" '
                 f'font-size="15" fill="#8d909a">{hexc.upper()}</text>')
    t.append(f'<text x="{ix + 28}" y="560" font-family="{FONT}" font-size="19" '
             f'font-weight="700" fill="#e8e8ec">Type</text>')
    for i, (label, value) in enumerate((("Headline", "Noto Sans Black · 96"),
                                        ("Dates", "Noto Sans Bold · 34"),
                                        ("Body", "Noto Sans · 22"))):
        y = 600 + i * 52
        t.append(f'<text x="{ix + 28}" y="{y}" font-family="{FONT}" font-size="16" '
                 f'fill="#8d909a">{label}</text>')
        t.append(f'<text x="{ix + 28}" y="{y + 22}" font-family="{FONT}" '
                 f'font-size="17" fill="#e8e8ec">{value}</text>')
    # The poster.
    t.append(f'<g filter="url(#shadow)"><rect x="{px}" y="{py}" width="{pw}" '
             f'height="{ph}" fill="{NAVY}"/></g>')
    t.append(f'<image clip-path="url(#photo)" x="{px - 180}" y="{py}" '
             f'width="{int(photo_h * 1.6)}" height="{photo_h}" '
             f'preserveAspectRatio="xMidYMid slice" xlink:href="{photo_href}"/>')
    t.append(f'<rect x="{px}" y="{py}" width="{pw}" height="{photo_h}" fill="url(#fade)"/>')
    cx = px + 44
    t.append(f'<text x="{cx}" y="{py + photo_h - 6}" font-family="{FONT}" '
             f'font-size="86" font-weight="900" fill="{CREAM}" '
             f'letter-spacing="-1">LAKESIDE</text>')
    t.append(f'<text x="{cx}" y="{py + photo_h + 78}" font-family="{FONT}" '
             f'font-size="86" font-weight="900" fill="{CREAM}" '
             f'letter-spacing="-1">LIGHTS</text>')
    t.append(f'<text x="{cx}" y="{py + photo_h + 132}" font-family="{FONT}" '
             f'font-size="26" fill="{CREAM}" opacity="0.85">A weekend of music and film</text>')
    t.append(f'<text x="{cx}" y="{py + photo_h + 206}" font-family="{FONT}" '
             f'font-size="40" font-weight="800" fill="{ORANGE}">12 – 14 SEPTEMBER</text>')
    t.append(f'<line x1="{cx}" y1="{py + ph - 92}" x2="{px + pw - 44}" '
             f'y2="{py + ph - 92}" stroke="{CREAM}" stroke-opacity="0.25" stroke-width="2"/>')
    t.append(f'<text x="{cx}" y="{py + ph - 52}" font-family="{FONT}" font-size="21" '
             f'font-weight="600" fill="{CREAM}">Harbour Park</text>')
    t.append(f'<text x="{cx}" y="{py + ph - 24}" font-family="{FONT}" font-size="18" '
             f'fill="{CREAM}" opacity="0.75">Tickets at the gate or online</text>')
    t.append(qr(px + pw - 44 - 72, py + ph - 82, 72))
    # Selection handles on the headline, as if it is being edited.
    sx, sy, sw, sh = cx - 8, py + photo_h - 82, 472, 178
    t.append(f'<rect x="{sx}" y="{sy}" width="{sw}" height="{sh}" fill="none" '
             f'stroke="#4c8dff" stroke-width="2"/>')
    for hx, hy in ((sx, sy), (sx + sw, sy), (sx, sy + sh), (sx + sw, sy + sh)):
        t.append(f'<rect x="{hx - 6}" y="{hy - 6}" width="12" height="12" '
                 f'fill="#ffffff" stroke="#4c8dff" stroke-width="2"/>')
    t.append('</svg>')
    return "\n".join(t)


def main() -> int:
    magick = shutil.which("magick") or shutil.which("convert")
    if not magick:
        print("error: ImageMagick not found", file=sys.stderr)
        return 1
    if not os.path.isfile(PHOTO):
        print(f"error: {PHOTO} is missing", file=sys.stderr)
        return 1
    out = os.path.join(OUT_DIR, "share-poster.jpg")
    with open(PHOTO, "rb") as f:
        # Inline: ImageMagick's SVG reader will not follow a file reference.
        href = "data:image/png;base64," + base64.b64encode(f.read()).decode()
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "share.svg")
        with open(path, "w", encoding="utf-8") as f:
            f.write(svg(href))
        subprocess.run([magick, path, "-strip", "-quality", "86",
                        "-sampling-factor", "4:2:0", out], check=True)
    print(f"wrote {os.path.relpath(out, ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
