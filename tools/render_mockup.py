#!/usr/bin/env python3
"""Render docs/mockup.png: one screen state as the board draws it, inside a round bezel.

Run the desktop tests first (they write every drawing operation to /tmp/muni_ops.json),
then render with Pillow and the Roboto files ESPHome caches on its first build:

    c++ -std=c++17 -Itest/stubs test/test_muni.cpp -o /tmp/test_muni && /tmp/test_muni
    .venv/bin/pip install pillow
    .venv/bin/python tools/render_mockup.py [scene] [ops.json] [font dir]
"""
import json, os, sys
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
scene = sys.argv[1] if len(sys.argv) > 1 else "normal (12 and 25 min)"
ops_path = sys.argv[2] if len(sys.argv) > 2 else "/tmp/muni_ops.json"
font_dir = sys.argv[3] if len(sys.argv) > 3 else os.path.join(ROOT, ".esphome", "font")

SIZE, BEZEL, SCALE = 720, 40, 2  # panel px, bezel width, supersampling for smooth edges
ops = [o for o in json.load(open(ops_path)) if o["scene"] == scene]
if not ops:
    sys.exit(f"no operations for scene {scene!r} in {ops_path}")

fonts = {}
def font(weight, size):
    key = (weight, size)
    if key not in fonts:
        path = os.path.join(font_dir, f"Roboto@{weight}@False@v1.ttf")
        fonts[key] = ImageFont.truetype(path, size * SCALE)
    return fonts[key]

panel = Image.new("RGB", (SIZE * SCALE, SIZE * SCALE), "black")
d = ImageDraw.Draw(panel)
for o in ops:
    c = tuple(o["c"])
    box = [o["x"] * SCALE, o["y"] * SCALE, (o["x"] + o["w"]) * SCALE, (o["y"] + o["h"]) * SCALE]
    if o["kind"] == "circle":
        d.ellipse(box, fill=c)
    elif o["kind"] == "line":
        d.rectangle(box, fill=c)
    elif o["kind"] == "text":
        d.text((o["px"] * SCALE, o["py"] * SCALE), o["text"], fill=c,
               font=font(o["weight"], o["size"]), anchor="ls")

full = SIZE + 2 * BEZEL
out = Image.new("RGBA", (full * SCALE, full * SCALE), (0, 0, 0, 0))
ImageDraw.Draw(out).ellipse([0, 0, full * SCALE - 1, full * SCALE - 1], fill=(38, 38, 40, 255))
mask = Image.new("L", panel.size, 0)
ImageDraw.Draw(mask).ellipse([0, 0, SIZE * SCALE - 1, SIZE * SCALE - 1], fill=255)
out.paste(panel, (BEZEL * SCALE, BEZEL * SCALE), mask)
out = out.resize((full, full), Image.LANCZOS)

dest = os.path.join(ROOT, "docs", "mockup.png")
os.makedirs(os.path.dirname(dest), exist_ok=True)
out.save(dest, optimize=True)
print(f"wrote {dest}: {scene}")
