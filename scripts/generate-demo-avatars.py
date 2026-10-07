#!/usr/bin/env python3
"""Regenerate the screenshot-demo avatars as flat illustrated portraits.

Development only. Each fictional demo person gets a deterministic, drawn
(never photographed) head-and-shoulders portrait, so the demo's avatars read
as people rather than as initials on a gradient. Nothing is fetched and no
third-party artwork is used: every shape is defined below.

Output: resources/screenshot-demo/avatar-<name>.png, 224x224 (the size the
fixtures have always had; the image provider circle-masks them).
Requires ImageMagick with an SVG delegate (librsvg).

    scripts/generate-demo-avatars.py
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "resources", "screenshot-demo")
SIZE = 224          # output edge
CANVAS = 512        # drawing coordinates
# The visible window onto them: head and shoulders fill the frame, so a face
# still reads at the 28-32 px a room list draws an avatar.
VIEW = "86 64 340 340"

# Skin tones, light to deep, each (base, shade).
SKIN = {
    "s1": ("#f6d3b8", "#e6b493"),
    "s2": ("#eebf98", "#d9a073"),
    "s3": ("#d49a6a", "#b97d4f"),
    "s4": ("#a86b45", "#8c5535"),
    "s5": ("#7a4a2c", "#633a21"),
}


def darker(hex_colour: str, f: float = 0.82) -> str:
    h = hex_colour.lstrip("#")
    r, g, b = (int(h[i:i + 2], 16) for i in (0, 2, 4))
    return "#%02x%02x%02x" % (int(r * f), int(g * f), int(b * f))


def hair_back(style: str, c: str) -> str:
    if style == "long":
        return (f'<path d="M150 230 C150 120 362 120 362 230 L372 420 '
                f'C330 445 182 445 140 420 Z" fill="{c}"/>')
    if style == "bob":
        return (f'<path d="M156 236 C150 120 362 120 356 236 L360 330 '
                f'C320 352 192 352 152 330 Z" fill="{c}"/>')
    if style == "afro":
        return (f'<circle cx="256" cy="196" r="128" fill="{c}"/>'
                f'<circle cx="160" cy="250" r="56" fill="{c}"/>'
                f'<circle cx="352" cy="250" r="56" fill="{c}"/>')
    if style == "bun":
        return f'<circle cx="256" cy="104" r="44" fill="{c}"/>'
    if style == "hijab":
        return (f'<path d="M136 256 C120 56 392 56 376 256 L392 420 '
                f'C340 470 172 470 120 420 Z" fill="{c}"/>')
    return ""


def hair_front(style: str, c: str) -> str:
    if style in ("short", "bun", "long"):
        part = ("M168 214 C160 120 230 104 262 108 C318 110 356 142 346 220 "
                "C332 176 300 158 262 156 C224 158 196 170 168 214 Z")
        if style == "long":
            part = ("M166 230 C156 118 236 102 262 106 C322 110 358 150 348 236 "
                    "C330 190 302 160 252 150 C226 176 196 196 166 230 Z")
        return f'<path d="{part}" fill="{c}"/>'
    if style == "wavy":
        return (f'<path d="M164 226 C150 130 220 96 268 104 C330 112 366 150 '
                f'350 228 C344 196 330 180 316 172 C304 186 286 176 274 166 '
                f'C258 182 236 172 226 164 C210 184 190 186 164 226 Z" '
                f'fill="{c}"/>')
    if style == "bob":
        return (f'<path d="M164 214 C160 120 352 120 348 214 L346 196 '
                f'C300 186 220 186 166 200 Z" fill="{c}"/>'
                f'<path d="M168 206 C190 150 330 150 346 206 C310 176 230 172 '
                f'168 206 Z" fill="{c}"/>')
    if style == "buzz":
        return (f'<path d="M172 206 C170 128 342 128 340 206 C320 168 192 168 '
                f'172 206 Z" fill="{c}"/>')
    if style == "curly":
        bumps = "".join(
            f'<circle cx="{x}" cy="{y}" r="26" fill="{c}"/>'
            for x, y in ((186, 172), (214, 140), (252, 128), (292, 136),
                         (324, 162), (338, 196), (174, 204)))
        return bumps
    if style == "afro":
        return (f'<path d="M168 214 C176 150 336 150 344 214 C320 188 196 188 '
                f'168 214 Z" fill="{c}"/>')
    if style == "hijab":
        return (f'<path d="M160 236 C150 70 362 70 352 236 C338 160 174 160 '
                f'160 236 Z" fill="{c}"/>'
                # The drape over the neck and chest.
                f'<path d="M172 290 C196 352 316 352 340 290 L372 420 '
                f'C330 446 182 446 140 420 Z" fill="{c}"/>')
    return ""


def portrait(p: dict) -> str:
    skin, shade = SKIN[p["skin"]]
    hair = p.get("hair", "#2b1d16")
    style = p["style"]
    shirt = p["shirt"]
    bg1, bg2 = p["bg"]
    ink = "#2a1d1a"
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{CANVAS}" '
        f'height="{CANVAS}" viewBox="{VIEW}">',
        '<defs><linearGradient id="bg" x1="0" y1="0" x2="0.4" y2="1">'
        f'<stop offset="0" stop-color="{bg1}"/>'
        f'<stop offset="1" stop-color="{bg2}"/></linearGradient></defs>',
        f'<rect width="{CANVAS}" height="{CANVAS}" fill="url(#bg)"/>',
        hair_back(style, p.get("scarf", hair) if style == "hijab" else hair),
        # Shoulders and collar.
        f'<path d="M84 512 C84 404 168 362 256 362 C344 362 428 404 428 512 Z" '
        f'fill="{shirt}"/>',
        f'<path d="M206 366 C222 404 290 404 306 366 C290 372 222 372 206 366 Z" '
        f'fill="{darker(shirt)}"/>',
        # Neck.
        f'<path d="M222 300 L222 372 C238 392 274 392 290 372 L290 300 Z" '
        f'fill="{shade}"/>',
    ]
    if style != "hijab":
        parts += [f'<ellipse cx="170" cy="240" rx="16" ry="24" fill="{shade}"/>',
                  f'<ellipse cx="342" cy="240" rx="16" ry="24" fill="{shade}"/>']
    parts += [
        # Head.
        f'<ellipse cx="256" cy="232" rx="86" ry="100" fill="{skin}"/>',
        hair_front(style, p.get("scarf", hair) if style == "hijab" else hair),
        # Brows, eyes, nose, smile, cheeks.
        f'<path d="M210 206 Q226 196 242 204" stroke="{darker(hair, 0.9)}" '
        'stroke-width="7" fill="none" stroke-linecap="round"/>',
        f'<path d="M270 204 Q286 196 302 206" stroke="{darker(hair, 0.9)}" '
        'stroke-width="7" fill="none" stroke-linecap="round"/>',
        f'<ellipse cx="226" cy="234" rx="8" ry="10" fill="{ink}"/>',
        f'<ellipse cx="286" cy="234" rx="8" ry="10" fill="{ink}"/>',
        f'<path d="M256 244 Q248 272 258 276" stroke="{shade}" stroke-width="6" '
        'fill="none" stroke-linecap="round"/>',
        f'<path d="M228 292 Q256 316 284 292" stroke="{ink}" stroke-width="7" '
        'fill="none" stroke-linecap="round"/>',
        '<ellipse cx="206" cy="270" rx="16" ry="9" fill="#e8737333"/>',
        '<ellipse cx="306" cy="270" rx="16" ry="9" fill="#e8737333"/>',
    ]
    if p.get("beard"):
        parts.append(
            f'<path d="M172 250 C176 330 214 348 256 350 C298 348 336 330 340 250 '
            f'C330 300 300 306 284 300 C270 292 242 292 228 300 C212 306 182 300 '
            f'172 250 Z" fill="{hair}"/>'
            f'<path d="M232 300 Q256 318 280 300" stroke="{ink}" stroke-width="6" '
            'fill="none" stroke-linecap="round"/>')
    if p.get("glasses"):
        g = p.get("frame", "#1d1d24")
        parts.append(
            f'<g fill="#ffffff22" stroke="{g}" stroke-width="7">'
            '<rect x="196" y="212" width="58" height="44" rx="14"/>'
            '<rect x="258" y="212" width="58" height="44" rx="14"/></g>'
            f'<path d="M254 230 L258 230" stroke="{g}" stroke-width="7"/>')
    if p.get("earring"):
        parts.append(f'<circle cx="172" cy="270" r="7" fill="{p["earring"]}"/>')
    parts.append("</svg>")
    return "\n".join(x for x in parts if x)


PEOPLE = {
    "alex":   dict(skin="s2", hair="#5a3a22", style="short", shirt="#2f6f8f",
                   bg=("#bcd7f5", "#7fa8e0")),
    "maya":   dict(skin="s1", hair="#1b1716", style="bob", shirt="#e0675a",
                   bg=("#ffd9c2", "#f5a98a"), earring="#f2c14e"),
    "jordan": dict(skin="s5", hair="#151110", style="buzz", shirt="#e3a72f",
                   bg=("#c6efd9", "#86d0ab"), beard=True),
    "sam":    dict(skin="s3", hair="#2a1a12", style="wavy", shirt="#5b4bb7",
                   bg=("#ddd3fb", "#b2a2f0"), glasses=True),
    "aisha":  dict(skin="s4", hair="#1a1210", style="hijab", scarf="#7a3b69",
                   shirt="#7a3b69", bg=("#fbefc4", "#f2d27d")),
    "noah":   dict(skin="s1", hair="#c99a4b", style="short", shirt="#3e7c4f",
                   bg=("#d8efc4", "#a9d58b"), beard=True),
    "priya":  dict(skin="s4", hair="#18110e", style="long", shirt="#2d8a8a",
                   bg=("#fbd0de", "#ef9bb8"), earring="#f2c14e"),
    "leo":    dict(skin="s1", hair="#b5532a", style="curly", shirt="#34495e",
                   bg=("#c9e9f7", "#8fcbe8"), glasses=True, frame="#3a2a20"),
    "taylor": dict(skin="s3", hair="#7a3420", style="bun", shirt="#384a9c",
                   bg=("#f6e2c8", "#e8bf8e")),
    "nova":   dict(skin="s5", hair="#20140f", style="afro", shirt="#c04a7a",
                   bg=("#e3d2fb", "#b394ee"), earring="#f2c14e"),
}


def main() -> int:
    magick = shutil.which("magick") or shutil.which("convert")
    if not magick:
        print("error: ImageMagick not found", file=sys.stderr)
        return 1
    os.makedirs(OUT, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        for name, spec in PEOPLE.items():
            svg = os.path.join(tmp, f"{name}.svg")
            with open(svg, "w", encoding="utf-8") as f:
                f.write(portrait(spec))
            out = os.path.join(OUT, f"avatar-{name}.png")
            subprocess.run([magick, "-background", "none", "-density", "96",
                            svg, "-resize", f"{SIZE}x{SIZE}", "-strip", out],
                           check=True)
            print(f"wrote {os.path.relpath(out, ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
