#!/usr/bin/env python3
"""Regenerate the screenshot-demo's illustrated pictures.

Development only. Three scenes the demo's conversations share, drawn here as
flat illustrations so no photograph and no third-party artwork enters the
tree: a lake at sunrise with rowing boats, a forest trail, and a fallen maple
leaf. Everything is computed from the shapes below and a fixed seed, so a
re-run reproduces the same files.

Output: resources/screenshot-demo/{lake,forest-trail,fallen-leaf}.png,
1200x750. Requires ImageMagick with an SVG delegate (librsvg).

    scripts/generate-demo-scenes.py
"""
from __future__ import annotations

import math
import os
import random
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "resources", "screenshot-demo")
W, H = 1200, 750


def svg_open(defs: str) -> str:
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
            f'viewBox="0 0 {W} {H}"><defs>{defs}</defs>')


def ridge(rng: random.Random, y: float, amp: float, step: int,
          jag: float) -> str:
    """A mountain/hill silhouette from the left edge to the right edge."""
    pts = [(0, H)]
    x = 0
    while x <= W:
        pts.append((x, y - amp * (0.5 + 0.5 * math.sin(x / 170.0 + y))
                    - rng.uniform(0, jag)))
        x += step
    pts.append((W, H))
    return " ".join(f"{px:.1f},{py:.1f}" for px, py in pts)


# ── Lake at sunrise ──────────────────────────────────────────────────────
def boat(x: float, y: float, s: float, angle: float, hull: str,
         rim: str) -> str:
    return (f'<g transform="translate({x} {y}) rotate({angle}) scale({s})">'
            # Shadow on the shingle.
            '<ellipse cx="0" cy="26" rx="118" ry="16" fill="#0d0b16" opacity="0.55"/>'
            # Hull, inside, thwarts and rim.
            f'<path d="M-120 -6 Q-96 34 -20 36 L64 34 Q112 26 128 -10 Z" fill="{hull}"/>'
            f'<path d="M-112 -6 Q-90 22 -20 24 L62 22 Q104 16 120 -8 Z" fill="#3a1a12"/>'
            f'<rect x="-50" y="-4" width="12" height="26" fill="{rim}"/>'
            f'<rect x="20" y="-4" width="12" height="24" fill="{rim}"/>'
            f'<path d="M-122 -8 Q-96 6 -20 8 L64 8 Q112 4 130 -12" stroke="{rim}" '
            'stroke-width="5" fill="none" stroke-linecap="round"/>'
            '</g>')


def lake() -> str:
    rng = random.Random(7)
    horizon = 420
    sx = 770
    defs = (
        '<linearGradient id="sky" x1="0" y1="0" x2="0" y2="1">'
        '<stop offset="0" stop-color="#1c2453"/><stop offset="0.38" stop-color="#5b3f86"/>'
        '<stop offset="0.68" stop-color="#e2765a"/><stop offset="0.9" stop-color="#ffb36b"/>'
        '<stop offset="1" stop-color="#ffd58a"/></linearGradient>'
        '<linearGradient id="water" x1="0" y1="0" x2="0" y2="1">'
        '<stop offset="0" stop-color="#f2a56b"/><stop offset="0.25" stop-color="#a5607a"/>'
        '<stop offset="0.6" stop-color="#3d3968"/><stop offset="1" stop-color="#151a38"/>'
        '</linearGradient>'
        '<radialGradient id="glow" cx="0.5" cy="0.5" r="0.5">'
        '<stop offset="0" stop-color="#fff3c4" stop-opacity="0.95"/>'
        '<stop offset="0.35" stop-color="#ffd27e" stop-opacity="0.55"/>'
        '<stop offset="1" stop-color="#ff9a5a" stop-opacity="0"/></radialGradient>'
        '<filter id="soft"><feGaussianBlur stdDeviation="8"/></filter>'
    )
    t = [svg_open(defs), f'<rect width="{W}" height="{horizon + 2}" fill="url(#sky)"/>']
    t.append(f'<circle cx="{sx}" cy="{horizon - 125}" r="280" fill="url(#glow)"/>')
    t.append(f'<circle cx="{sx}" cy="{horizon - 125}" r="50" fill="#fff1c9"/>')
    # Clouds lit from below.
    for cx, cy, rx, ry, col in ((260, 150, 230, 26, "#f08a74"), (520, 95, 180, 18, "#c46a8a"),
                                (930, 170, 260, 24, "#f7a06e"), (1080, 90, 160, 16, "#b25f88"),
                                (140, 250, 200, 16, "#ffb27a")):
        t.append(f'<ellipse cx="{cx}" cy="{cy}" rx="{rx}" ry="{ry}" fill="{col}" '
                 'opacity="0.55" filter="url(#soft)"/>')
    # Ranges, far to near.
    for y, amp, col, op in ((horizon - 40, 70, "#9a6a98", 1.0),
                            (horizon - 10, 55, "#5a4277", 0.9),
                            (horizon + 4, 30, "#2f2a50", 1.0)):
        t.append(f'<polygon points="{ridge(rng, y, amp, 40, 12)}" fill="{col}" opacity="{op}"/>')
    t.append(f'<rect y="{horizon}" width="{W}" height="{H - horizon}" fill="url(#water)"/>')
    # The sun's path on the water.
    for i in range(26):
        y = horizon + 6 + i * 11
        w = 150 - i * 4 + rng.uniform(-20, 20)
        t.append(f'<rect x="{sx - w / 2:.1f}" y="{y}" width="{w:.1f}" height="4" rx="2" '
                 f'fill="#ffe6a8" opacity="{max(0.08, 0.85 - i * 0.03):.2f}"/>')
    for i in range(40):
        y = horizon + 10 + rng.uniform(0, H - horizon - 10)
        x = rng.uniform(0, W)
        t.append(f'<rect x="{x:.0f}" y="{y:.0f}" width="{rng.uniform(40, 160):.0f}" '
                 'height="2" fill="#ffd6a0" opacity="0.18"/>')
    # A jetty running out from the right.
    for i in range(9):
        x = 840 + i * 38
        top = horizon + 20 + i * 6
        t.append(f'<rect x="{x}" y="{top}" width="7" height="{46 + i * 4}" fill="#1b1628"/>')
    t.append(f'<polygon points="836,{horizon + 30} 1200,{horizon + 66} 1200,{horizon + 78} '
             f'836,{horizon + 38}" fill="#241c33"/>')
    # Shingle shore and the boats drawn up on it.
    t.append('<path d="M0 600 Q260 560 620 610 Q900 650 1200 640 L1200 750 L0 750 Z" '
             'fill="#221d31"/>')
    for _ in range(260):
        x, y = rng.uniform(0, W), rng.uniform(600, H)
        t.append(f'<ellipse cx="{x:.0f}" cy="{y:.0f}" rx="{rng.uniform(2, 6):.1f}" '
                 f'ry="{rng.uniform(1.5, 3.5):.1f}" fill="#4b4060" opacity="0.6"/>')
    hulls = (("#9b4425", "#e08a4e"), ("#7d3a2a", "#d07a46"), ("#a3532b", "#f0a060"),
             ("#87402a", "#d98850"), ("#6e3324", "#c87244"))
    for i, (bx, by, s, a) in enumerate(((170, 640, 0.95, 12), (360, 655, 1.05, -8),
                                        (560, 668, 1.0, 6), (760, 690, 1.1, -4),
                                        (980, 700, 1.15, 8))):
        hull, rim = hulls[i]
        t.append(boat(bx, by, s, a, hull, rim))
    t.append("</svg>")
    return "".join(t)


# ── Forest trail ─────────────────────────────────────────────────────────
def conifer(x: float, base: float, h: float, col: str) -> str:
    w = h * 0.34
    tiers = []
    for k in range(4):
        top = base - h + k * h * 0.2
        bottom = base - h * 0.3 + k * h * 0.22
        half = w * (0.45 + k * 0.2)
        tiers.append(f'<polygon points="{x:.1f},{top:.1f} {x - half:.1f},{bottom:.1f} '
                     f'{x + half:.1f},{bottom:.1f}" fill="{col}"/>')
    trunk = (f'<rect x="{x - h * 0.02:.1f}" y="{base - h * 0.3:.1f}" '
             f'width="{h * 0.04:.1f}" height="{h * 0.32:.1f}" fill="{col}"/>')
    return trunk + "".join(tiers)


def forest() -> str:
    rng = random.Random(11)
    defs = (
        '<linearGradient id="fog" x1="0" y1="0" x2="0" y2="1">'
        '<stop offset="0" stop-color="#e8f1d8"/><stop offset="0.45" stop-color="#b9d3b0"/>'
        '<stop offset="1" stop-color="#5f8a6a"/></linearGradient>'
        '<linearGradient id="trail" x1="0" y1="0" x2="0" y2="1">'
        '<stop offset="0" stop-color="#d9b98a"/><stop offset="1" stop-color="#8a6440"/>'
        '</linearGradient>'
        '<linearGradient id="ray" x1="0" y1="0" x2="1" y2="1">'
        '<stop offset="0" stop-color="#fffbe6" stop-opacity="0.55"/>'
        '<stop offset="1" stop-color="#fffbe6" stop-opacity="0"/></linearGradient>'
    )
    t = [svg_open(defs), f'<rect width="{W}" height="{H}" fill="url(#fog)"/>']
    layers = (("#a7c4a5", 300, 170, 26), ("#7fa486", 380, 230, 22),
              ("#4f7a5c", 470, 320, 18), ("#2c5240", 590, 460, 14))
    for col, base, height, count in layers:
        for i in range(count):
            x = (i + rng.uniform(-0.3, 0.3)) * W / (count - 1)
            # Leave the trail's corridor open.
            if abs(x - 610) < 30 + height * 0.18:
                continue
            t.append(conifer(x, base + rng.uniform(0, 14),
                             height * rng.uniform(0.75, 1.15), col))
    # Light through the canopy.
    for x0, w in ((120, 120), (300, 70), (430, 150)):
        t.append(f'<polygon points="{x0},0 {x0 + w},0 {x0 + w + 520},{H} {x0 + 380},{H}" '
                 'fill="url(#ray)" opacity="0.5"/>')
    # Forest floor, lit towards the clearing.
    t.append('<path d="M0 560 Q600 500 1200 560 L1200 750 L0 750 Z" fill="#355d45"/>')
    # The trail, winding into the distance.
    t.append('<path d="M600 470 C590 500 640 520 620 560 C600 610 520 640 470 750 '
             'L800 750 C760 660 700 620 690 570 C680 520 640 500 622 470 Z" '
             'fill="url(#trail)"/>')
    # Ferns and grass on the near ground.
    for _ in range(170):
        x = rng.uniform(0, W)
        if 470 < x < 800 and rng.random() < 0.85:
            continue
        y = rng.uniform(600, H)
        h = rng.uniform(18, 46)
        lean = rng.uniform(-14, 14)
        col = rng.choice(("#2f5e3e", "#3d7349", "#24493a", "#4c8a55"))
        t.append(f'<path d="M{x:.0f} {y:.0f} q{lean / 2:.0f} {-h / 2:.0f} {lean:.0f} {-h:.0f}" '
                 f'stroke="{col}" stroke-width="3" fill="none" stroke-linecap="round"/>')
    t.append("</svg>")
    return "".join(t)


# ── Fallen leaf ──────────────────────────────────────────────────────────
def maple_points(cx: float, cy: float, r: float, rot: float) -> str:
    """A five-lobed, toothed outline in polar form."""
    pts = []
    n = 720
    for i in range(n):
        th = 2 * math.pi * i / n
        # Measured from straight up: lobes at 0, ±72 and ±144 degrees, and
        # the stem's notch at 180, between the two lower lobes.
        a = th + math.pi / 2
        lobes = abs(math.cos(2.5 * a)) ** 0.6
        teeth = 0.06 * abs(math.sin(18 * a))
        notch = 0.35 if abs((a % (2 * math.pi)) - math.pi) < 0.25 else 0.0
        rr = r * (0.42 + 0.58 * lobes - teeth) * (1 - notch * 0.6)
        x = cx + rr * math.cos(th + rot)
        y = cy + rr * math.sin(th + rot)
        pts.append(f"{x:.1f},{y:.1f}")
    return " ".join(pts)


def leaf() -> str:
    rng = random.Random(23)
    defs = (
        '<linearGradient id="ground" x1="0" y1="0" x2="1" y2="1">'
        '<stop offset="0" stop-color="#2b1a12"/><stop offset="0.5" stop-color="#5a3418"/>'
        '<stop offset="1" stop-color="#2a170e"/></linearGradient>'
        '<radialGradient id="leafFill" cx="0.45" cy="0.45" r="0.6">'
        '<stop offset="0" stop-color="#ffc35a"/><stop offset="0.55" stop-color="#f07a2a"/>'
        '<stop offset="1" stop-color="#b8361d"/></radialGradient>'
        '<filter id="blur"><feGaussianBlur stdDeviation="6"/></filter>'
        '<filter id="shadow"><feGaussianBlur stdDeviation="14"/></filter>'
    )
    t = [svg_open(defs), f'<rect width="{W}" height="{H}" fill="url(#ground)"/>']
    # Out-of-focus leaves and light behind it.
    for _ in range(70):
        x, y, r = rng.uniform(0, W), rng.uniform(0, H), rng.uniform(14, 80)
        col = rng.choice(("#f2a541", "#e0672a", "#ffd27a", "#b84a1f", "#8a5a22"))
        t.append(f'<circle cx="{x:.0f}" cy="{y:.0f}" r="{r:.0f}" fill="{col}" '
                 f'opacity="{rng.uniform(0.12, 0.42):.2f}" filter="url(#blur)"/>')
    cx, cy, r, rot = 600, 375, 250, math.radians(-18)
    t.append(f'<polygon points="{maple_points(cx + 18, cy + 26, r, rot)}" fill="#120a06" '
             'opacity="0.6" filter="url(#shadow)"/>')
    t.append(f'<polygon points="{maple_points(cx, cy, r, rot)}" fill="url(#leafFill)"/>')
    # Veins to the five lobe tips, and the stem.
    for k in range(5):
        a = -math.pi / 2 + k * 2 * math.pi / 5 + rot
        x2, y2 = cx + r * 0.9 * math.cos(a), cy + r * 0.9 * math.sin(a)
        t.append(f'<line x1="{cx}" y1="{cy + 40}" x2="{x2:.1f}" y2="{y2:.1f}" '
                 'stroke="#9a2e14" stroke-width="5" stroke-linecap="round" opacity="0.7"/>')
    a = math.pi / 2 + rot
    t.append(f'<line x1="{cx}" y1="{cy + 40}" x2="{cx + 190 * math.cos(a):.1f}" '
             f'y2="{cy + 190 * math.sin(a):.1f}" stroke="#7a2a12" stroke-width="9" '
             'stroke-linecap="round"/>')
    t.append("</svg>")
    return "".join(t)


def main() -> int:
    magick = shutil.which("magick") or shutil.which("convert")
    if not magick:
        print("error: ImageMagick not found", file=sys.stderr)
        return 1
    os.makedirs(OUT, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        for name, build in (("lake", lake), ("forest-trail", forest),
                            ("fallen-leaf", leaf)):
            path = os.path.join(tmp, name + ".svg")
            with open(path, "w", encoding="utf-8") as f:
                f.write(build())
            out = os.path.join(OUT, name + ".png")
            subprocess.run([magick, path, "-strip", "PNG24:" + out], check=True)
            print(f"wrote {os.path.relpath(out, ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
