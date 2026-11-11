#!/usr/bin/env python3
"""Build the Xbox prompt glyph masks for Alienation's font atlases."""
from __future__ import annotations

import math
import mmap
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
# The PlayStation button glyphs are U+E000..U+E016 in the CUSA04430 and 1.06 fonts; these become Xbox ones (the D-pad
# and the touch pad stay).
BUTTON_GLYPHS = (
    (0xE000, "face", "B"), (0xE001, "face", "X"), (0xE003, "face", "A"), (0xE002, "face", "Y"),
    (0xE005, "shoulder", "LT"), (0xE007, "shoulder", "RT"), (0xE006, "shoulder", "RB"), (0xE004, "shoulder", "LB"),
    (0xE014, "circle", "RS"), (0xE008, "circle", "RS"), (0xE009, "circle", "LS"), (0xE013, "circle", "LS"),
    (0xE012, "text", "MENU"), (0xE00A, "text", "VIEW"),
)


class FontCells:
    """An atlas's button cells, read from the glyph records of the FONT block that samples it."""

    def __init__(self, font_offset: int):
        self.font_offset = font_offset


BUILDS = {
    "CUSA00062-1.00": [
        ("font0", 0x2E0A40, CUSA00062_TEXT0),
        ("font1", 0x217BC0, CUSA00062_TEXT1),
        ("font2", 0x3E9D00, CUSA00062_TEXT2),
        ("icon", 0x31851600, ICON),
    ],
    # Every font of the build, by the FONT block whose glyphs sample the atlas: the three Latin weights, then the
    # Korean, Japanese and two Traditional Chinese fonts the kr, jp and cn text languages draw with.
    "CUSA04430-1.00": [
        ("font0", 0x5AB7C0, FontCells(0x2045C0)),
        ("font1", 0x2C4B80, FontCells(0x285C40)),
        ("font2", 0xFDF880, FontCells(0x2BBDC0)),
        ("icon", 0x32B69B00, ICON),
        ("font_kr", 0x38A040, FontCells(0x26AF40)),
        ("font_jp", 0x66F340, FontCells(0x23D640)),
        ("font_cn", 0xA21E80, FontCells(0x209F00)),
        ("font_cn2", 0x325CE580, FontCells(0x28B480)),
    ],
    "CUSA00062-1.06": [
        ("font0", 0x5AE540, FontCells(0x205F80)),
        ("font1", 0x2C6B80, FontCells(0x287C40)),
        ("font2", 0xFFDD00, FontCells(0x2BDDC0)),
        ("icon", 0x3358B400, ICON),
        ("font_kr", 0x38C040, FontCells(0x26CEC0)),
        ("font_jp", 0x6720C0, FontCells(0x23F300)),
        ("font_cn", 0xA2D880, FontCells(0x20B8C0)),
        ("font_cn2", 0x32FEFE80, FontCells(0x28D480)),
    ],
}
# 1.06 ships CUSA04430 1.00's Latin and second Chinese atlases and 1.00's icon atlas byte for byte: their masks are
# reused, not redrawn.
SHARED_ATLASES = {
    ("CUSA00062-1.06", "font0"): ("CUSA04430-1.00", "font0"),
    ("CUSA00062-1.06", "font1"): ("CUSA04430-1.00", "font1"),
    ("CUSA00062-1.06", "font2"): ("CUSA04430-1.00", "font2"),
    ("CUSA00062-1.06", "icon"): ("CUSA00062-1.00", "icon"),
    ("CUSA00062-1.06", "font_cn2"): ("CUSA04430-1.00", "font_cn2"),
}


def masks_name(build: str, atlas_name: str) -> str:
    build, atlas_name = SHARED_ATLASES.get((build, atlas_name), (build, atlas_name))
    return f"k_{build.replace('-', '_').replace('.', '_')}_{atlas_name}_masks"


def micro_index(x, y):
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3)


def micro_order() -> np.ndarray:
    return np.array([micro_index(i & 7, i >> 3) for i in range(64)])


def untille(buf: bytes, width: int, height: int) -> bytes:
    tiles = np.frombuffer(buf, dtype=np.uint8, count=width * height).reshape(-1, 64)[:, micro_order()]
    return tiles.reshape(height // 8, width // 8, 8, 8).transpose(0, 2, 1, 3).tobytes()


def tile(buf: bytes, width: int, height: int) -> bytes:
    rows = np.frombuffer(buf, dtype=np.uint8, count=width * height).reshape(height // 8, 8, width // 8, 8)
    tiles = rows.transpose(0, 2, 1, 3).reshape(-1, 64)
    out = np.empty_like(tiles)
    out[:, micro_order()] = tiles
    return out.tobytes()


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
        inner = morph(solid, 25, "erode")
        caption = core & interior
        cys, cxs = np.nonzero(caption)
        ixs = np.nonzero(interior)[1]
        if len(cxs) and cxs.max() - cxs.min() < 0.4 * (ixs.max() - ixs.min()):
            # A letter fused to the frame is outside the hole's interior; take the ink that gets blanked instead.
            cys, cxs = np.nonzero(core & inner)
        if len(cxs):
            box = (int(cxs.min()), int(cys.min()), int(cxs.max()), int(cys.max()))
        else:
            ys, xs = np.nonzero(interior)
            box = (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max()))
        frame = solid & ~inner
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


def read_font(stream: Path, offset: int) -> bytes:
    """A FONT block's glyph tables: zlib after a table of 32-bit fixups."""
    with stream.open("rb") as file:
        mm = mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ)
        id0, zsize, dsize, units, id1, kind = struct.unpack_from("<6I", mm, offset)
        if kind != 0x464F4E54 or id0 != id1:
            raise ValueError(f"{offset:#x}: not a FONT block")
        start = offset + 0x30 + 4 * struct.unpack_from("<I", mm, offset + 0x1C)[0]
        data = zlib.decompressobj().decompress(mm[start:offset + units * 64])[:dsize]
        mm.close()
    if len(data) != dsize:
        raise ValueError(f"{offset:#x}: short FONT block")
    return data


def font_cells(font: bytes, width: int, height: int) -> list:
    """The button cells from the glyph records (page table at +0x28, 56-byte records starting u0, v0, u1, v1), one pixel
    inside each record's rectangle, as the hand-measured cells of the first layouts."""
    begin, end = struct.unpack_from("<QQ", font, 0x28 + 0xE0 * 16)
    cells = []
    for codepoint, kind, label in BUTTON_GLYPHS:
        at = begin + (codepoint - 0xE000) * 56
        if not begin or at + 56 > end:
            raise ValueError(f"the font has no U+{codepoint:04X}")
        u0, v0, u1, v1 = struct.unpack_from("<4f", font, at)
        cells.append((kind, math.ceil(u0 * width) + 1, math.ceil(v0 * height) + 1, math.ceil(u1 * width) - 1,
                      math.ceil(v1 * height) - 1, label))
    # A cell must not reach into any other glyph the font samples.
    buttons = {codepoint for codepoint, _, _ in BUTTON_GLYPHS}
    for page in range(256):
        begin, end = struct.unpack_from("<QQ", font, 0x28 + page * 16)
        for at in range(begin, end if begin else begin, 56):
            codepoint = page * 256 + (at - begin) // 56
            u0, v0, u1, v1 = struct.unpack_from("<4f", font, at)
            if codepoint in buttons or u1 <= u0 or v1 <= v0:
                continue
            for _, x0, y0, x1, y1, label in cells:
                if x0 < u1 * width and u0 * width < x1 and y0 < v1 * height and v0 * height < y1:
                    raise ValueError(f"the {label} cell overlaps U+{codepoint:04X}")
    return cells


def read_block(stream: Path, offset: int):
    with stream.open("rb") as file:
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
    layouts = {}
    for build, atlases in BUILDS.items():
        stream = ROOT / "game" / "prepared" / build / "stream.dat"
        for atlas_name, offset, glyphs in atlases:
            zsize, dsize, units, compressed, payload = read_block(stream, offset)
            dimensions = struct.unpack_from("<I", payload, 16 + 0x18)[0]
            width, height = (dimensions & 0x3FFF) + 1, ((dimensions >> 14) & 0x3FFF) + 1
            if isinstance(glyphs, FontCells):
                glyphs = font_cells(read_font(stream, glyphs.font_offset), width, height)
            layouts[(build, atlas_name)] = glyphs
            shared = SHARED_ATLASES.get((build, atlas_name))
            if shared:
                owner = next(row for row in rows if (row[0], row[1]) == shared)
                if owner[6] != zlib.crc32(compressed) & 0xFFFFFFFF or layouts[shared] != glyphs:
                    raise SystemExit(f"{build} {atlas_name}: not the {shared[0]} {shared[1]} atlas")
                rows.append((build, atlas_name, offset, zsize, dsize, units, owner[6], owner[7]))
                continue
            image = Image.frombytes("L", (width, height), untille(payload[16 + 0x100:], width, height))
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
            new_pixel = tile(image.tobytes(), width, height)
            packed = zlib.compress(payload[:16 + 0x100] + new_pixel, 9)
            capacity = units * 64 - 40
            if len(packed) > capacity:
                raise SystemExit(f"{build} {atlas_name}: {len(packed)} > {capacity}")
            print(f"{build} {atlas_name}: {len(packed)} / {capacity}")
            rows.append((build, atlas_name, offset, zsize, dsize, units,
                         zlib.crc32(compressed) & 0xFFFFFFFF, masks))

    for build, atlas_name, offset, zsize, dsize, units, crc, masks in rows:
        if (build, atlas_name) in SHARED_ATLASES:
            continue
        name = masks_name(build, atlas_name)
        parts.append(f"static const AlnPromptGlyphMask {name}[] = {{")
        for x, y, width, height, packed, raw, data in masks:
            parts.append(f"    {{{x}u, {y}u, {width}u, {height}u, {packed}u, {raw}u, {data}}},")
        parts.append("};")
        parts.append("")
    parts.append("const AlnPromptGlyphAtlas kPromptGlyphAtlases[] = {")
    for build, atlas_name, offset, zsize, dsize, units, crc, masks in rows:
        parts.append(f'    {{"{build}", "{atlas_name}", 0x{offset:x}, {zsize}u, {dsize}u, 0x{crc:08x}u, '
                     f'{units}u, {masks_name(build, atlas_name)}, {len(masks)}u}},')
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
