#!/usr/bin/env python3
"""Rasterize the adapted Xbox Series X SVG to the 1024x512 controller illustration."""
from __future__ import annotations

import argparse
import subprocess
import tempfile
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]

# The game draws the controller into a fixed area of the 1024x512 texture. The
# artwork is scaled into the same window so the composition matches the original.
CANVAS = (1024, 512)
PLACEMENT = (42, 5, 981, 506)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", default=str(ROOT / "assets" / "source" / "controller" /
                                                   "xbox-series-x-black-alienation.svg"))
    parser.add_argument("--target", default=str(ROOT / "assets" / "source" / "controller" /
                                                   "xbox-series-x-black-alienation.png"))
    parser.add_argument("--rasterizer", default=str(ROOT / "tools" / "prompts" / "svg_raster"))
    parser.add_argument("--width", type=int, default=2048)
    args = parser.parse_args()

    rasterizer = Path(args.rasterizer)
    if not rasterizer.exists() and Path(str(rasterizer) + ".exe").exists():
        rasterizer = Path(str(rasterizer) + ".exe")
    with tempfile.TemporaryDirectory() as directory:
        raw = Path(directory) / "controller.rgba"
        result = subprocess.run([str(rasterizer), args.source, str(raw), str(args.width)],
                                check=True, capture_output=True, text=True)
        width, height = (int(value) for value in result.stderr.split())
        image = Image.frombytes("RGBA", (width, height), raw.read_bytes())
    alpha = image.getchannel("A")
    bbox = alpha.point(lambda value: 255 if value > 8 else 0).getbbox()
    if bbox is None:
        raise SystemExit("the SVG has no visible pixels")
    art = image.crop(bbox)
    left, top, right, bottom = PLACEMENT
    scale = min((right - left + 1) / art.width, (bottom - top + 1) / art.height)
    resized = art.resize((max(1, round(art.width * scale)), max(1, round(art.height * scale))),
                          Image.LANCZOS)
    canvas = Image.new("RGBA", CANVAS, (0, 0, 0, 0))
    canvas.alpha_composite(resized, (left + (right - left + 1 - resized.width) // 2,
                                     top + (bottom - top + 1 - resized.height) // 2))
    canvas.save(args.target)
    print("wrote", args.target)


if __name__ == "__main__":
    main()
