#!/usr/bin/env python3
"""Check the generated Xbox controller assets: artwork, mip chain and callout anchors.

    py -3.13 tools/prompts/verify_prompts.py
"""
from __future__ import annotations

import math
import re
import sys
import zlib
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / "launcher" / "controller_prompts_data.c"
OUT = ROOT / "cache" / "prompt_check"

ELEM = {10: 4, 35: 8, 37: 16, 41: 16}
DXGI = {35: 71, 37: 77, 41: 98}


def micro_index(x: int, y: int) -> int:
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3)


def untile(buf: bytes, width: int, height: int, elem: int) -> bytes:
    out = bytearray(width * height * elem)
    tiles = (width + 7) // 8
    for y in range(height):
        for x in range(width):
            src = ((y // 8) * tiles + (x // 8)) * 64 * elem + micro_index(x & 7, y & 7) * elem
            chunk = buf[src:src + elem]
            off = (y * width + x) * elem
            out[off:off + elem] = chunk + b"\0" * (elem - len(chunk))
    return bytes(out)


def untile_blocks(buf: bytes, width: int, height: int, elem: int) -> bytes:
    out = bytearray(width * height * elem)
    tiles = (width + 7) // 8
    for y in range(height):
        for x in range(width):
            src = ((y // 8) * tiles + (x // 8)) * 64 * elem + micro_index(x & 7, y & 7) * elem
            chunk = buf[src:src + elem]
            off = (y * width + x) * elem
            out[off:off + elem] = chunk + b"\0" * (elem - len(chunk))
    return bytes(out)


def dds(data: bytes, width: int, height: int, dxgi: int) -> Image.Image:
    import io
    import struct
    header = bytearray(b'DDS ' + struct.pack('<7I', 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000,
                                              height, width, len(data), 1, 1) + b'\0' * 44)
    header += struct.pack('<II4s', 32, 0x4, b'DX10') + b'\0' * 20
    header += struct.pack('<5I', 0, 0, 0, 0, 0)
    header += struct.pack('<5I', dxgi, 3, 0, 1, 0)
    return Image.open(io.BytesIO(bytes(header) + data)).convert("RGBA")


def levels(payload: bytes):
    gnf = payload[16:16 + 0x100]
    word = np.frombuffer(gnf, dtype="<u4", count=8, offset=0x10)
    fmt = int((word[1] >> 20) & 0x3F)
    width = int(word[2] & 0x3FFF) + 1
    height = int((word[2] >> 14) & 0x3FFF) + 1
    total = int(np.frombuffer(gnf, dtype="<u4", count=1, offset=0x2C)[0])
    data = payload[16 + 0x100:16 + 0x100 + total]
    images = []
    offset = 0
    level = 0
    while True:
        padded = max(32, (width + 31) // 32 * 32)
        if fmt == 10:
            size = padded * height * 4
            if offset + size > len(data):
                break
            image = Image.frombytes("RGBA", (padded, height),
                                    untile(data[offset:offset + size], padded, height, 4)).crop((0, 0, width, height))
        else:
            rows = max(4, (height + 3) // 4 * 4)
            size = (padded // 4) * (rows // 4) * ELEM[fmt]
            if offset + size > len(data):
                break
            image = dds(untile_blocks(data[offset:offset + size], padded // 4, rows // 4, ELEM[fmt]),
                        padded, rows, DXGI[fmt]).crop((0, 0, width, min(height, rows)))
        offset += size
        images.append((f"mip{level}", width, height, image))
        level += 1
        if width <= 16 and height <= 8:
            break
        width = max(1, width // 2)
        height = max(1, height // 2)
    return fmt, images


def scene_anchors(payload: bytes) -> dict[str, tuple[float, float]]:
    """Read every named anchor position out of a controller template scene."""
    anchors = {}
    magic = b"\x6e\x53\x6d\xba"
    position = payload.find(magic)
    while position >= 0:
        name_offset = position + 0x4C
        end = payload.find(b"\x00", name_offset)
        if end > name_offset and position >= 0x20:
            name = payload[name_offset:end].decode("ascii", "replace")
            x, y = np.frombuffer(payload, dtype="<f4", count=2, offset=position - 0x20)
            if name.isidentifier():
                anchors[name] = (float(x), float(y))
        position = payload.find(magic, position + 1)
    return anchors


# The page space to texture pixel mapping, measured on the original artwork.
PAGE_TO_PIXEL_X = (0.001424085, -0.722275)
PAGE_TO_PIXEL_Y = (-0.001458715, 0.373644)


FACE_COLOURS = {
    "Y": lambda a: (a[:, :, 0] > 180) & (a[:, :, 1] > 150) & (a[:, :, 2] < 120),
    "B": lambda a: (a[:, :, 0] > 150) & (a[:, :, 1] < 90) & (a[:, :, 2] < 90),
    "A": lambda a: (a[:, :, 1] > 130) & (a[:, :, 0] < 150) & (a[:, :, 2] < 120),
    "X": lambda a: (a[:, :, 2] > 140) & (a[:, :, 0] < 120) & (a[:, :, 1] < 200),
}


def verify_glyphs() -> list[str]:
    """The shoulder glyphs must keep the button outline instead of plain text."""
    sys.path.insert(0, str(Path(__file__).parent))
    import build_prompt_glyphs as generator

    path = ROOT / "launcher" / "controller_prompts_glyphs_data.c"
    text = path.read_text(encoding="utf-8")
    arrays = {}
    for match in re.finditer(r"static const unsigned char (k_[A-Za-z0-9_]+)\[\] = \{(.*?)\};", text, re.S):
        arrays[match.group(1)] = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{2})", match.group(2)))
    masks = {}
    for match in re.finditer(r"static const AlnPromptGlyphMask (k_[A-Za-z0-9_]+_masks)\[\] = \{(.*?)\};", text, re.S):
        rows = []
        for row in re.finditer(r"\{(\d+)u, (\d+)u, (\d+)u, (\d+)u, (\d+)u, (\d+)u, (k_[A-Za-z0-9_]+)\}", match.group(2)):
            rows.append((int(row.group(1)), int(row.group(2)), int(row.group(3)), int(row.group(4)),
                         int(row.group(5)), int(row.group(6)), row.group(7)))
        masks[match.group(1)] = rows

    failures = []
    tables = {
        "font0": generator.CUSA00062_TEXT0,
        "font1": generator.CUSA00062_TEXT1,
        "font2": generator.CUSA00062_TEXT2,
        "icon": generator.ICON,
    }
    for name, table in tables.items():
        rows = masks.get(f"k_CUSA00062_1_00_{name}_masks")
        if rows is None:
            failures.append(f"the {name} atlas has no glyph table")
            continue
        for index, (kind, _, _, _, _, label) in enumerate(table):
            if kind != "shoulder":
                continue
            _, _, width, height, _, _, data = rows[index]
            raw = zlib.decompress(arrays[data])
            pixels = np.frombuffer(raw, dtype=np.uint8).reshape(height, width)
            ys, xs = np.nonzero(pixels > 32)
            if not len(xs):
                failures.append(f"the {name} {label} glyph is empty")
                continue
            span_x = (xs.max() - xs.min() + 1) / width
            span_y = (ys.max() - ys.min() + 1) / height
            if span_x < 0.75 or span_y < 0.6:
                failures.append(f"the {name} {label} glyph has no button outline "
                                f"(span {span_x:.2f}x{span_y:.2f})")
            else:
                print(f"    {name} {label}: outline span {span_x:.2f}x{span_y:.2f} "
                      f"coverage {100 * (pixels > 32).mean():.1f}%")
    return failures


def verify_anchors(anchors: dict[str, tuple[float, float]], controller: np.ndarray) -> list[str]:
    """Check that every callout anchor sits on the control its caption belongs to."""
    failures = []
    required = ("left_stick", "right_stick", "cross_button", "circle_button", "triangle_button",
                "square_button", "l1_button", "l2_button", "r1_button", "r2_button",
                "directional_button", "dpad_up_button", "dpad_down_button", "dpad_left_button",
                "dpad_right_button", "touch_button", "options_button")
    for name in required:
        if name not in anchors:
            failures.append(f"the scene has no {name} anchor")
    if failures:
        return failures

    # Page space has +x right and +y up, so "below" means a smaller y.
    if not anchors["l1_button"][1] < anchors["l2_button"][1]:
        failures.append("l1_button is not on the bumper below the l2 trigger")
    if not anchors["r1_button"][1] < anchors["r2_button"][1]:
        failures.append("r1_button is not on the bumper below the r2 trigger")
    if not (anchors["l1_button"][0] < 0 and anchors["r1_button"][0] > 0):
        failures.append("l1/r1 are not on the left/right shoulders")
    if not anchors["left_stick"][1] > anchors["directional_button"][1]:
        failures.append("left_stick is not above the directional_button D-pad")
    if not (anchors["left_stick"][0] < 0 and anchors["right_stick"][0] > 0):
        failures.append("the thumb sticks are not left/right")
    if not anchors["touch_button"][0] < anchors["options_button"][0]:
        failures.append("touch_button is not left of the options button")

    # The face diamond: triangle/Y on top, cross/A below, square/X left, circle/B right.
    if not (anchors["triangle_button"][1] > anchors["square_button"][1] >
            anchors["cross_button"][1]):
        failures.append("triangle/square/cross are not stacked like the Xbox Y/X/A buttons")
    if not (anchors["square_button"][0] < anchors["triangle_button"][0] <
            anchors["circle_button"][0]):
        failures.append("square/triangle/circle are not ordered like the Xbox X/Y/B buttons")

    # Every anchor must land inside the controller silhouette.
    height, width = controller.shape[:2]
    for name in required:
        x, y = anchors[name]
        pixel_x = int(round((x - PAGE_TO_PIXEL_X[1]) / PAGE_TO_PIXEL_X[0]))
        pixel_y = int(round((y - PAGE_TO_PIXEL_Y[1]) / PAGE_TO_PIXEL_Y[0]))
        if not (0 <= pixel_x < width and 0 <= pixel_y < height):
            failures.append(f"the {name} anchor is outside the controller texture")
            continue
        if controller[pixel_y, pixel_x, 3] <= 200:
            failures.append(f"the {name} anchor is not on the controller silhouette")
    return failures



CONTROLS = {
    "lt": (268.7, 37.3, 45),
    "rt": (747.4, 33.8, 44),
    "lb": (272.0, 58.0, 30),
    "rb": (730.0, 58.0, 30),
    "left_stick": (293.1, 238.1, 47),
    "right_stick": (621.6, 357.9, 48),
    "dpad": (401.2, 349.5, 63),
    "y": (730.0, 168.0, 27),
    "x": (672.0, 226.0, 27),
    "b": (787.0, 220.0, 27),
    "a": (729.0, 274.0, 27),
    "view": (449.7, 225.8, 20),
    "menu": (572.7, 225.8, 20),
    "share": (510.6, 270.0, 24),
    "guide": (511.2, 144.8, 35),
}

# Tutorial highlight anchor -> control centre measured on the controller_white line art.
LINEART_TARGETS = {
    "triangle_button": ("Y button", 703, 113),
    "circle_button": ("B button", 753, 163),
    "cross_button": ("A button", 703, 212),
    "square_button": ("X button", 653, 163),
    "left_stick": ("left stick", 312, 155),
    "right_stick": ("right stick", 609, 273),
    "directional_button": ("D-pad", 409, 272),
    "touch_button": ("View button", 465, 159),
    "options_button": ("Menu button", 559, 159),
}

# caption -> the Xbox control its line must end on
CAPTIONS = {
    "touch": "view",
    "ls": "left_stick",
    "rs": "right_stick",
    "l2": "lt",
    "l1": "lb",
    "r2": "rt",
    "r1": "rb",
    "options": "menu",
    "triangle": "y",
    "square": "x",
    "circle": "b",
    "cross": "a",
    "dpad_up": "dpad",
    "dpad_down": "dpad",
    "dpad_left": "dpad",
    "dpad_right": "dpad",
}

SHOULDER_CONTROLS = {"lt", "rt", "lb", "rb"}
# The D-pad arms each caption line must end on, in texture pixels (left, top, right, bottom).
DPAD_ARMS = {
    "dpad_up": (381, 294, 420, 334),
    "dpad_down": (381, 368, 420, 405),
    "dpad_left": (338, 334, 381, 370),
    "dpad_right": (420, 334, 464, 370),
}


def point_to_segment(point, start, end) -> float:
    px, py = point
    ax, ay = start
    bx, by = end
    dx, dy = bx - ax, by - ay
    length = dx * dx + dy * dy
    if length <= 1e-9:
        return math.hypot(px - ax, py - ay)
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / length))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def verify_layout(payload: bytes, original: bytes) -> list[str]:
    """The script patch must keep the payload length, header and Lua tokens intact."""
    failures = []
    if len(payload) != len(original):
        failures.append(f"layout payload is {len(payload)} bytes, original {len(original)}")
    if payload[:0x18] != original[:0x18]:
        failures.append("layout payload header differs from the original")
    declared = int.from_bytes(payload[0x10:0x14], "little")
    if declared != len(payload):
        failures.append(f"layout header declares {declared} bytes, payload has {len(payload)}")
    offset = int.from_bytes(payload[0x08:0x0C], "little")
    if offset != 0x18:
        failures.append(f"layout source offset is {offset:#x}, expected 0x18")
    sys.path.insert(0, str(Path(__file__).parent))
    import build_controller_prompts as generator
    if not generator.same_lua_syntax(original[0x18:], payload[0x18:]):
        failures.append("layout payload changed the Lua token stream")
    return failures


def layout_boxes(text: str, page: str) -> dict[str, tuple[float, float, float, float]]:
    pattern = (r'box_page ?= ?"' + page + r'", ?box_name ?= ?"([^"]+)", ?\s*left ?= ?(-?[\d.]+), ?\s*'
               r'right ?= ?(-?[\d.]+), ?\s*top ?= ?(-?[\d.]+), ?\s*bottom ?= ?(-?[\d.]+)')
    return {m.group(1): tuple(float(m.group(i)) for i in (2, 3, 4, 5)) for m in re.finditer(pattern, text)}


def segments_gap(a0, a1, b0, b1) -> float:
    """Smallest distance between two axis-aligned segments."""
    if min(a0[0], a1[0]) <= max(b0[0], b1[0]) and min(b0[0], b1[0]) <= max(a0[0], a1[0]) and \
       min(a0[1], a1[1]) <= max(b0[1], b1[1]) and min(b0[1], b1[1]) <= max(a0[1], a1[1]):
        return 0.0
    return min(point_to_segment(a0, b0, b1), point_to_segment(a1, b0, b1),
               point_to_segment(b0, a0, a1), point_to_segment(b1, a0, a1))


def verify_captions(payload: bytes, original: bytes, controller: np.ndarray) -> list[str]:
    """Every caption stub must start its painted line, and the line must end on its Xbox
    control without crossing another control or caption line."""
    sys.path.insert(0, str(Path(__file__).parent))
    import build_controller_prompts as generator

    failures = []
    text = payload[0x18:].decode("latin-1")
    source = original[0x18:].decode("latin-1")
    for page in generator.LABEL_PAGES:
        boxes, originals = layout_boxes(text, page), layout_boxes(source, page)
        for name in list(generator.CALLOUTS) + list(generator.CALLOUT_ALIASES):
            if name not in boxes or name not in originals:
                failures.append(f"{page} has no {name} caption")
                continue
            expected = generator.callout_box(name, originals[name])
            if any(abs(a - b) > 1e-4 for a, b in zip(boxes[name], expected)):
                failures.append(f"{page} {name} caption is not the stub of its line")

    routes = {name: [generator.menu_to_texture(x, y) for x, y in route]
              for name, (_, route) in generator.CALLOUTS.items()}
    shoulders = {"lt": (256, 0, 310, 46), "rt": (706, 0, 764, 46),
                 "lb": (212, 38, 394, 100), "rb": (630, 38, 813, 100)}
    for name, target in CAPTIONS.items():
        route = routes[name]
        end_x, end_y = route[-1]
        box = DPAD_ARMS.get(name) or shoulders.get(target)
        if box:
            reached = box[0] <= end_x <= box[2] and box[1] <= end_y <= box[3]
        else:
            cx, cy, radius = CONTROLS[target]
            # The radii are the inner rings; lines may stop on the outer ring.
            reached = math.hypot(end_x - cx, end_y - cy) <= radius + 10
        if not reached:
            failures.append(f"the {name} line ends at {end_x:.0f},{end_y:.0f}, not on {target}")
        for control, (cx, cy, radius) in CONTROLS.items():
            if control == target or (target in SHOULDER_CONTROLS and control in SHOULDER_CONTROLS):
                continue
            for a, b in zip(route, route[1:]):
                if point_to_segment((cx, cy), a, b) - radius < 2:
                    failures.append(f"the {name} line crosses {control}")
                    break
        for x, y in [((a[0] + b[0]) / 2, (a[1] + b[1]) / 2) for a, b in zip(route, route[1:])]:
            r, g, b_, alpha = controller[int(round(y)), int(round(x))]
            if not (r < 60 and g > 170 and b_ > 170 and alpha > 200):
                failures.append(f"the {name} line is not painted at {x:.0f},{y:.0f}")
    names = list(routes)
    for i, first in enumerate(names):
        for second in names[i + 1:]:
            for a0, a1 in zip(routes[first], routes[first][1:]):
                for b0, b1 in zip(routes[second], routes[second][1:]):
                    if segments_gap(a0, a1, b0, b1) < 6:
                        failures.append(f"the {first} and {second} lines touch")
    return failures


def read_stream_block(stream: Path, offset: int, payload_offset: int, zsize: int, dsize: int) -> bytes:
    with stream.open("rb") as file:
        import mmap
        mm = mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ)
        payload = zlib.decompress(mm[offset + payload_offset:offset + payload_offset + zsize])
        mm.close()
    if len(payload) != dsize:
        raise ValueError(f"{offset:#x}: short payload")
    return payload


def main() -> int:
    text = DATA.read_text(encoding="utf-8")
    arrays = {}
    for match in re.finditer(r"static const unsigned char (k_[A-Za-z0-9_]+)\[\] = \{(.*?)\};", text, re.S):
        arrays[match.group(1)] = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{2})", match.group(2)))
    rows = re.findall(r'\{"([^"]+)", "([^"]+)", (0x[0-9a-f]+), (\d+)u, (\d+)u, (0x[0-9a-f]+)u, (\d+)u, (\d+)u, (\d+)u, (k_[A-Za-z0-9_]+)\}',
                      text)
    scenes = re.findall(r'\{"([^"]+)", "([^"]+)", (0x[0-9a-f]+), (\d+)u, (\d+)u, (0x[0-9a-f]+)u, (\d+)u, (\d+)u, (0x[0-9a-f]+)u, (k_scene_[A-Za-z0-9_]+)\}',
                        text)
    OUT.mkdir(parents=True, exist_ok=True)
    failures = []
    seen = set()
    controller = None
    lineart = None
    for build, name, offset, oz, od, crc, units, nz, nd, data in rows:
        if (build, name) in seen:
            continue
        seen.add((build, name))
        payload = zlib.decompress(arrays[data])
        fmt, images = levels(payload)
        print(f"{build} {name}: fmt={fmt} levels={len(images)}")
        for tag, width, height, image in images:
            array = np.array(image).astype(int)
            alpha = array[:, :, 3]
            print(f"    {tag} {width}x{height} alpha mean {alpha.mean():.1f} "
                  f"coverage {100 * (alpha > 8).mean():.1f}%")
            image.save(OUT / f"{build}_{name}_{tag}.png")
        if name == "controller_color":
            array = np.array(images[0][3]).astype(int)
            controller = array
            for letter, mask in FACE_COLOURS.items():
                if not mask(array).any():
                    failures.append(f"controller_color has no {letter} button")
        if name == "controller_white":
            lineart = np.array(images[0][3]).astype(int)
            for tag, _, _, image in images:
                array = np.array(image).astype(int)
                alpha = array[:, :, 3]
                coverage = float((alpha > 200).mean())
                if not 0.25 <= coverage <= 0.70:
                    failures.append(f"controller_white {tag} silhouette coverage {coverage:.2f}")
                if float(array[:, :, :3].mean()) > 60:
                    failures.append(f"controller_white {tag} body is not dark")
    for build, name, offset, oz, od, crc, units, nz, nd, data in scenes:
        payload = zlib.decompress(arrays[data])
        if name == "menu_layout":
            original = read_stream_block(ROOT / "game" / "prepared" / build / "stream.dat",
                                         int(offset, 16), int(nd, 16), int(oz), int(od))
            print(f"{build} {name}: {len(payload)} bytes (original {len(original)})")
            for failure in verify_layout(payload, original):
                failures.append(f"{build} {failure}")
            if controller is None:
                failures.append(f"{build} has no controller texture for its captions")
            else:
                caption_failures = verify_captions(payload, original, controller)
                failures.extend(f"{build} {failure}" for failure in caption_failures)
                if not caption_failures:
                    print("    caption stubs start their painted lines, which reach their controls")
            continue
        anchors = scene_anchors(payload)
        print(f"{build} {name}: {len(anchors)} callout anchors")
        if not anchors:
            failures.append(f"{name} has no callout anchors")
            continue
        if lineart is None:
            continue
        shoulder_failures = verify_anchors(anchors, lineart)
        if shoulder_failures:
            for failure in shoulder_failures:
                failures.append(f"{build} {failure}")
        else:
            print("    shoulder, stick and D-pad anchors sit on the tutorial line art")
        for anchor, (control, cx, cy) in LINEART_TARGETS.items():
            if anchor not in anchors:
                failures.append(f"the scene has no {anchor} anchor")
                continue
            x, y = anchors[anchor]
            pixel_x = (x - PAGE_TO_PIXEL_X[1]) / PAGE_TO_PIXEL_X[0]
            pixel_y = (y - PAGE_TO_PIXEL_Y[1]) / PAGE_TO_PIXEL_Y[0]
            if math.hypot(pixel_x - cx, pixel_y - cy) > 12:
                failures.append(f"the {anchor} anchor is not on the line-art {control}")
            else:
                print(f"    {anchor} -> {control} at {pixel_x:.0f},{pixel_y:.0f}")
    for failure in failures:
        print("FAIL:", failure)
    print("checking the prompt glyphs")
    failures.extend(verify_glyphs())
    for failure in failures:
        print("FAIL:", failure)
    if failures:
        return 1
    print("prompt assets verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
