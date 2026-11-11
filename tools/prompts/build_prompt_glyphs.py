#!/usr/bin/env python3
"""Build the Xbox prompt glyph masks for Alienation's font atlases."""
from __future__ import annotations

import struct
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[2]
OUT_DATA = ROOT / "launcher" / "controller_prompts_glyphs_data.c"
FONT = ROOT / "gpu" / "third_party" / "fonts" / "DejaVuSans.ttf"

# (kind, x0, y0, x1, y1, label)
CUSA00062_TEXT0 = [
    ("face", 1308, 111, 1398, 202, "A"), ("face", 1405, 111, 1494, 202, "X"),
    ("face", 1501, 116, 1590, 206, "Y"), ("face", 1803, 111, 1893, 202, "B"),
    ("shoulder", 825, 4, 957, 96, "RT"), ("shoulder", 964, 4, 1097, 96, "LT"),
    ("shoulder", 1246, 4, 1381, 86, "RB"), ("shoulder", 1104, 4, 1239, 86, "LB"),
    ("circle", 1104, 93, 1199, 190, "RS"), ("circle", 1206, 93, 1301, 190, "LS"),
    ("circle", 964, 103, 1059, 199, "RS"), ("circle", 1924, 4, 2019, 100, "LS"),
    ("text", 640, 4, 818, 125, "MENU"), ("text", 1505, 4, 1602, 109, "VIEW"),
]
CUSA00062_TEXT1 = [
    ("face", 1793, 102, 1882, 193, "A"), ("face", 565, 93, 655, 184, "X"),
    ("face", 1889, 102, 1978, 193, "Y"), ("face", 1696, 102, 1786, 193, "B"),
    ("shoulder", 189, 4, 321, 96, "RT"), ("shoulder", 328, 4, 461, 96, "LT"),
    ("shoulder", 468, 4, 603, 86, "RB"), ("shoulder", 610, 4, 745, 86, "LB"),
    ("circle", 1594, 4, 1689, 100, "RS"), ("circle", 1288, 4, 1383, 100, "LS"),
    ("circle", 1390, 4, 1485, 100, "RS"), ("circle", 1492, 4, 1587, 100, "LS"),
    ("text", 4, 4, 182, 125, "MENU"), ("text", 869, 4, 966, 109, "VIEW"),
]
CUSA00062_TEXT2 = [
    ("face", 565, 93, 655, 184, "A"), ("face", 1696, 102, 1786, 193, "X"),
    ("face", 1889, 102, 1978, 193, "Y"), ("face", 1793, 102, 1882, 193, "B"),
    ("shoulder", 189, 4, 321, 96, "RT"), ("shoulder", 328, 4, 461, 96, "LT"),
    ("shoulder", 468, 4, 603, 86, "RB"), ("shoulder", 610, 4, 745, 86, "LB"),
    ("circle", 1390, 4, 1485, 100, "RS"), ("circle", 1492, 4, 1587, 100, "LS"),
    ("circle", 1594, 4, 1689, 100, "RS"), ("circle", 1288, 4, 1383, 100, "LS"),
    ("text", 4, 4, 182, 125, "MENU"), ("text", 869, 4, 966, 109, "VIEW"),
]
ICON = [
    ("shoulder", 1617, 4, 1748, 84, "LB"), ("shoulder", 1755, 4, 1887, 84, "RB"),
    ("shoulder", 653, 4, 772, 121, "RT"), ("shoulder", 779, 4, 898, 121, "LT"),
    ("circle", 1383, 4, 1493, 110, "LS"), ("circle", 1500, 4, 1610, 110, "RS"),
    ("circle", 1894, 4, 2004, 98, "LS"), ("circle", 1755, 91, 1865, 185, "RS"),
    ("text", 479, 4, 646, 88, "VIEW"), ("view", 152, 30, 339, 121, ""),
]
CUSA04430_TEXT = [
    ("face", 449, 84, 538, 173, "B"), ("face", 540, 84, 629, 173, "X"),
    ("face", 1617, 93, 1706, 182, "A"), ("face", 1708, 93, 1797, 182, "Y"),
    ("shoulder", 180, 1, 313, 92, "LT"), ("shoulder", 315, 1, 447, 92, "RT"),
    ("shoulder", 449, 1, 583, 82, "RB"), ("shoulder", 585, 1, 720, 82, "LB"),
    ("circle", 1230, 1, 1325, 96, "RS"), ("circle", 1327, 1, 1422, 96, "RS"),
    ("circle", 1424, 1, 1519, 96, "LS"), ("circle", 1521, 1, 1615, 96, "LS"),
    ("text", 1, 1, 178, 120, "MENU"), ("text", 833, 1, 930, 104, "VIEW"),
]
BUILDS = {
    "CUSA00062-1.00": [
        ("font0", 0x2E0A40, CUSA00062_TEXT0),
        ("font1", 0x217BC0, CUSA00062_TEXT1),
        ("font2", 0x3E9D00, CUSA00062_TEXT2),
        ("icon", 0x31851600, ICON),
    ],
    "CUSA04430-1.00": [
        ("font0", 0x5AB7C0, CUSA04430_TEXT),
        ("font1", 0x2C4B80, CUSA04430_TEXT),
        ("font2", 0xFDF880, CUSA04430_TEXT),
        ("icon", 0x32B69B00, ICON),
    ],
}


def micro_index(x, y):
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3)


def untille(buf: bytes, width: int, height: int) -> bytes:
    out = bytearray(width * height)
    tiles_per_row = width // 8
    for y in range(height):
        for x in range(width):
            src = ((y // 8) * tiles_per_row + (x // 8)) * 64 + micro_index(x & 7, y & 7)
            out[y * width + x] = buf[src]
    return bytes(out)


def tile(buf: bytes, width: int, height: int) -> bytes:
    out = bytearray(width * height)
    tiles_per_row = width // 8
    for y in range(height):
        for x in range(width):
            dst = ((y // 8) * tiles_per_row + (x // 8)) * 64 + micro_index(x & 7, y & 7)
            out[dst] = buf[y * width + x]
    return bytes(out)


def morph(mask: np.ndarray, size: int, mode: str) -> np.ndarray:
    image = Image.fromarray((mask * 255).astype(np.uint8))
    kernel = ImageFilter.MinFilter(size) if mode == "erode" else ImageFilter.MaxFilter(size)
    return np.array(image.filter(kernel)) > 127


def fill_holes(mask: np.ndarray) -> np.ndarray:
    """Close the holes of a binary mask."""
    height, width = mask.shape
    outside = np.zeros((height, width), dtype=bool)
    stack = []
    for x in range(width):
        for y in (0, height - 1):
            if not mask[y, x] and not outside[y, x]:
                outside[y, x] = True
                stack.append((y, x))
    for y in range(height):
        for x in (0, width - 1):
            if not mask[y, x] and not outside[y, x]:
                outside[y, x] = True
                stack.append((y, x))
    while stack:
        cy, cx = stack.pop()
        for ny, nx in ((cy - 1, cx), (cy + 1, cx), (cy, cx - 1), (cy, cx + 1)):
            if 0 <= ny < height and 0 <= nx < width and not mask[ny, nx] and not outside[ny, nx]:
                outside[ny, nx] = True
                stack.append((ny, nx))
    return ~outside


def fit_label(size: tuple[int, int], label: str, box: tuple[int, int, int, int],
              level: int = 255) -> Image.Image:
    """Render the label centred in box (x0, y0, x1, y1) of the size canvas."""
    image = Image.new("L", size, 0)
    draw = ImageDraw.Draw(image)
    x0, y0, x1, y1 = box
    target_height = max(6, y1 - y0 + 1)
    target_width = max(6, x1 - x0 + 1)
    font_size = max(6, int(target_height * 1.4))
    font = ImageFont.truetype(str(FONT), font_size)
    metrics = draw.textbbox((0, 0), label, font=font)
    while font_size > 6 and (metrics[3] - metrics[1] > target_height or
                             metrics[2] - metrics[0] > target_width):
        font_size -= 1
        font = ImageFont.truetype(str(FONT), font_size)
        metrics = draw.textbbox((0, 0), label, font=font)
    draw.text(((x0 + x1 - (metrics[2] - metrics[0])) / 2 - metrics[0],
               (y0 + y1 - (metrics[3] - metrics[1])) / 2 - metrics[1]),
              label, font=font, fill=level)
    return image


def shoulder(block: Image.Image, label: str) -> Image.Image:
    """Keep the original button shape and swap its caption for the Xbox name."""
    source = np.array(block).astype(np.uint8)
    width, height = block.size
    ink = source > 96
    core = morph(ink, 5, "erode")
    solid = fill_holes(core)
    hole = solid & ~core

    if hole.sum() > 0.02 * width * height:
        # Outlined button: keep the original frame, replace the caption inside.
        interior = morph(morph(hole, 21, "dilate"), 21, "erode") & solid
        caption = core & interior
        cys, cxs = np.nonzero(caption)
        if len(cxs):
            box = (int(cxs.min()), int(cys.min()), int(cxs.max()), int(cys.max()))
        else:
            ys, xs = np.nonzero(interior)
            box = (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max()))
        frame = solid & ~morph(solid, 25, "erode")
        keep = np.where(frame | ~solid, source, 0).astype(np.uint8)
        text = fit_label((width, height), label, box)
        keep = np.maximum(keep, np.array(text))
    else:
        # Solid button: the caption is a darker grey stamped on the shape.
        caption = morph(core, 15, "erode") & (source < 240)
        cys, cxs = np.nonzero(caption)
        if not len(cxs):
            raise ValueError("the shoulder glyph has no caption")
        x0, x1 = int(cxs.min()), int(cxs.max())
        y0, y1 = int(cys.min()), int(cys.max())
        level = int(np.median(source[caption]))
        keep = source.copy()
        widened = morph(caption, 5, "dilate")
        keep[widened] = 255
        text = fit_label((width, height), label, (x0, y0, x1, y1), level)
        alpha = np.array(text, dtype=np.float32) / max(1, level)
        keep = (keep.astype(np.float32) * (1.0 - alpha) + level * alpha).astype(np.uint8)
    return Image.fromarray(keep, mode="L").filter(ImageFilter.GaussianBlur(0.4))


def face(block: Image.Image, letter: str) -> Image.Image:
    block = block.copy()
    width, height = block.size
    draw = ImageDraw.Draw(block)
    cx, cy = width / 2, height / 2
    radius = min(width, height) * 0.34
    draw.ellipse([cx - radius, cy - radius, cx + radius, cy + radius], fill=0)
    font = ImageFont.truetype(str(FONT), int(height * 0.55))
    box = draw.textbbox((0, 0), letter, font=font)
    draw.text((cx - (box[2] - box[0]) / 2 - box[0], cy - (box[3] - box[1]) / 2 - box[1]),
              letter, font=font, fill=255, stroke_width=1, stroke_fill=255)
    return block.filter(ImageFilter.GaussianBlur(0.5))


def circle(block: Image.Image, label: str) -> Image.Image:
    block = block.copy()
    width, height = block.size
    draw = ImageDraw.Draw(block)
    cx, cy = width / 2, height / 2
    radius = min(width, height) * 0.32
    draw.ellipse([cx - radius, cy - radius, cx + radius, cy + radius], fill=0)
    font = ImageFont.truetype(str(FONT), int(height * 0.42))
    box = draw.textbbox((0, 0), label, font=font)
    draw.text((cx - (box[2] - box[0]) / 2 - box[0], cy - (box[3] - box[1]) / 2 - box[1]),
              label, font=font, fill=255, stroke_width=1, stroke_fill=255)
    return block.filter(ImageFilter.GaussianBlur(0.4))


def text(block: Image.Image, label: str) -> Image.Image:
    block = Image.new("L", block.size, 0)
    width, height = block.size
    draw = ImageDraw.Draw(block)
    size = int(height * 0.5)
    while size > 8:
        font = ImageFont.truetype(str(FONT), size)
        if draw.textlength(label, font=font) <= width * 0.86:
            break
        size -= 1
    box = draw.textbbox((0, 0), label, font=font)
    draw.text(((width - (box[2] - box[0])) / 2 - box[0], (height - (box[3] - box[1])) / 2 - box[1]),
              label, font=font, fill=255)
    return block


def view(block: Image.Image) -> Image.Image:
    block = Image.new("L", block.size, 0)
    width, height = block.size
    draw = ImageDraw.Draw(block)
    margin = min(width, height) * 0.12
    stroke = max(2, int(height * 0.06))
    draw.rounded_rectangle([margin, margin, width * 0.62, height - margin], radius=height * 0.12,
                           outline=255, width=stroke)
    draw.rounded_rectangle([width * 0.38, height * 0.38, width - margin, height * 0.78], radius=height * 0.12,
                           outline=255, width=stroke)
    return block


def c_array(name: str, data: bytes) -> str:
    body = []
    for offset in range(0, len(data), 12):
        body.append("    " + ", ".join(f"0x{value:02x}" for value in data[offset:offset + 12]) + ",")
    return f"static const unsigned char {name}[] = {{\n" + "\n".join(body) + "\n};\n"


def read_block(stream: Path, offset: int):
    with stream.open("rb") as file:
        import mmap
        mm = mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ)
        id0, zsize, dsize, units, id1, kind = struct.unpack_from("<6I", mm, offset)
        if kind != 0x54455820 or id0 != id1:
            raise ValueError(f"{offset:#x}: not a TEX block")
        payload = zlib.decompress(mm[offset + 40:offset + 40 + zsize])
        compressed = bytes(mm[offset + 40:offset + 40 + zsize])
        mm.close()
    return zsize, dsize, units, compressed, payload


def main():
    parts = [
        "/* Generated by tools/prompts/build_prompt_glyphs.py. Do not edit. */",
        '#include "controller_prompts.h"',
        "",
    ]
    rows = []
    for build, atlases in BUILDS.items():
        stream = ROOT / "game" / "prepared" / build / "stream.dat"
        for atlas_name, offset, glyphs in atlases:
            zsize, dsize, units, compressed, payload = read_block(stream, offset)
            image = Image.frombytes("L", (2048, 2048), untille(payload[16 + 0x100:], 2048, 2048))
            masks = []
            for index, (kind, x0, y0, x1, y1, label) in enumerate(glyphs):
                block = image.crop((x0, y0, x1, y1))
                if kind == "face":
                    block = face(block, label)
                elif kind == "shoulder":
                    block = shoulder(block, label)
                elif kind == "circle":
                    block = circle(block, label)
                elif kind == "text":
                    block = text(block, label)
                elif kind == "view":
                    block = view(block)
                else:
                    raise ValueError(kind)
                raw = block.tobytes()
                packed = zlib.compress(raw, 9)
                name = f"k_{build.replace('-', '_').replace('.', '_')}_{atlas_name}_{index}"
                parts.append(c_array(name, packed))
                masks.append((x0, y0, block.width, block.height, len(packed), len(raw), name))
                image.paste(block, (x0, y0))
            new_pixel = tile(image.tobytes(), 2048, 2048)
            packed = zlib.compress(payload[:16 + 0x100] + new_pixel, 9)
            capacity = units * 64 - 40
            if len(packed) > capacity:
                raise SystemExit(f"{build} {atlas_name}: {len(packed)} > {capacity}")
            print(f"{build} {atlas_name}: {len(packed)} / {capacity}")
            rows.append((build, atlas_name, offset, zsize, dsize, units,
                         zlib.crc32(compressed) & 0xFFFFFFFF, masks))

    for build, atlas_name, offset, zsize, dsize, units, crc, masks in rows:
        name = f"k_{build.replace('-', '_').replace('.', '_')}_{atlas_name}_masks"
        parts.append(f"static const AlnPromptGlyphMask {name}[] = {{")
        for x, y, width, height, packed, raw, data in masks:
            parts.append(f"    {{{x}u, {y}u, {width}u, {height}u, {packed}u, {raw}u, {data}}},")
        parts.append("};")
        parts.append("")
    parts.append("const AlnPromptGlyphAtlas kPromptGlyphAtlases[] = {")
    for build, atlas_name, offset, zsize, dsize, units, crc, masks in rows:
        parts.append(f'    {{"{build}", "{atlas_name}", 0x{offset:x}, {zsize}u, {dsize}u, 0x{crc:08x}u, '
                     f'{units}u, k_{build.replace("-", "_").replace(".", "_")}_{atlas_name}_masks, '
                     f'{len(masks)}u}},')
    parts.append("};")
    parts.append("")
    parts.append("const AlnPromptGlyphAtlas *AlnPromptGlyphAtlases(size_t *count) {")
    parts.append("    *count = sizeof(kPromptGlyphAtlases) / sizeof(kPromptGlyphAtlases[0]);")
    parts.append("    return kPromptGlyphAtlases;")
    parts.append("}")
    OUT_DATA.write_text("\n".join(parts), encoding="utf-8", newline="\n")
    print("wrote", OUT_DATA)


if __name__ == "__main__":
    main()
