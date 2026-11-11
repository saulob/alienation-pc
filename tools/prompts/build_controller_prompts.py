#!/usr/bin/env python3
"""Build Alienation's Xbox controller prompt assets from the game's own controller textures.

The controller illustration uses the licensed Gamepad Asset Pack SVG derivative in
assets/source/controller; its source and licence are recorded there.

    python tools/prompts/build_controller_prompts.py --game game/prepared/CUSA00062-1.00
"""
from __future__ import annotations

import argparse
import mmap
import re
import struct
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

import bc7

ROOT = Path(__file__).resolve().parents[2]
FONT = ROOT / "gpu" / "third_party" / "fonts" / "DejaVuSans.ttf"
OUT_ASSETS = ROOT / "assets" / "controller_prompts"
OUT_DATA = ROOT / "launcher" / "controller_prompts_data.c"
SVG_CONTROLLER = ROOT / "assets" / "source" / "controller" / "xbox-series-x-black-alienation.png"
LINEART_CONTROLLER = ROOT / "assets" / "source" / "controller" / "xbox-series-lineart.png"

SCALE = 2
W, H = 1024, 512
WHITE = (235, 240, 246, 255)
OUTLINE = (210, 220, 230, 255)
CYAN = (70, 212, 230, 255)
XBOX = {
    "a": (91, 190, 74, 255),
    "b": (217, 74, 74, 255),
    "x": (61, 139, 217, 255),
    "y": (224, 185, 63, 255),
}


def font(size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(FONT), size * SCALE)


def s(value: float) -> int:
    return int(round(value * SCALE))


def rgba(color, alpha=None):
    return color if alpha is None else (color[0], color[1], color[2], alpha)


def rounded(draw: ImageDraw.ImageDraw, box, radius, fill=None, outline=None, width=0):
    draw.rounded_rectangle([s(v) for v in box], radius=s(radius), fill=fill, outline=outline, width=s(width))


def ellipse(draw, box, fill=None, outline=None, width=0):
    draw.ellipse([s(v) for v in box], fill=fill, outline=outline, width=s(width))


def line(draw, points, fill, width):
    draw.line([(s(x), s(y)) for x, y in points], fill=fill, width=s(width), joint="curve")


def text(draw, xy, value, size, fill, anchor="la", stroke=0, stroke_fill=None):
    draw.text((s(xy[0]), s(xy[1])), value, font=font(size), fill=fill, anchor=anchor,
              stroke_width=s(stroke), stroke_fill=stroke_fill)


def outline_button(draw, cx, cy, label, width, height):
    """A small outlined button badge with its caption inside."""
    left, top = cx - width / 2, cy - height / 2
    right, bottom = cx + width / 2, cy + height / 2
    rounded(draw, (left, top, right, bottom), height * 0.3, outline=WHITE, width=2)
    text(draw, (cx, cy + 1), label, 15, WHITE, anchor="mm")


def body_mask() -> Image.Image:
    mask = Image.new("L", (W * SCALE, H * SCALE), 0)
    d = ImageDraw.Draw(mask)
    # Triggers, bumpers, centre and grips form one silhouette so the outline never shows internal seams.
    rounded(d, (238, 44, 382, 102), 24, fill=255)
    rounded(d, (642, 44, 786, 102), 24, fill=255)
    rounded(d, (218, 80, 402, 150), 32, fill=255)
    rounded(d, (622, 80, 806, 150), 32, fill=255)
    rounded(d, (252, 118, 772, 350), 78, fill=255)
    rounded(d, (132, 196, 344, 500), 86, fill=255)
    rounded(d, (680, 196, 892, 500), 86, fill=255)
    return mask


def silhouette() -> tuple[Image.Image, Image.Image]:
    mask = body_mask()
    outline = ImageChops.subtract(mask, mask.filter(ImageFilter.MinFilter(13)))
    return mask, outline


def draw_internals(d: ImageDraw.ImageDraw, color: bool):
    light = OUTLINE if color else WHITE
    accent = rgba(CYAN if color else WHITE, 120)
    fill = (24, 27, 32, 255) if color else None

    # Central seam and HUD panel.
    line(d, [(512, 120), (512, 348)], rgba(light, 55), 2)
    rounded(d, (438, 300, 586, 342), 12, outline=rgba(light, 120), width=2)
    line(d, [(452, 319), (572, 319)], rgba(light, 100), 2)
    line(d, [(452, 327), (548, 327)], rgba(light, 70), 2)

    # Bumpers and triggers.
    outline_button(d, 310, 116, "LB", 46, 24)
    outline_button(d, 714, 116, "RB", 46, 24)
    outline_button(d, 310, 72, "LT", 46, 24)
    outline_button(d, 714, 72, "RT", 46, 24)
    line(d, [(222, 151), (398, 151)], rgba(light, 65), 1)
    line(d, [(626, 151), (802, 151)], rgba(light, 65), 1)

    # Sticks.
    for cx, cy, label in ((330, 214, "L3"), (700, 372, "R3")):
        ellipse(d, (cx - 56, cy - 56, cx + 56, cy + 56), fill=(30, 34, 40, 255) if color else None,
                outline=rgba(light, 150), width=2)
        ellipse(d, (cx - 44, cy - 44, cx + 44, cy + 44), fill=(20, 23, 28, 255) if color else None,
                outline=light, width=3)
        ellipse(d, (cx - 31, cy - 31, cx + 31, cy + 31), fill=(38, 43, 51, 255) if color else None,
                outline=accent, width=2)
        text(d, (cx, cy + 70), label, 12, rgba(light, 170), anchor="mm")

    # D-pad.
    dp = (330, 372)
    rounded(d, (dp[0] - 20, dp[1] - 52, dp[0] + 20, dp[1] + 52), 7, fill=fill, outline=light, width=2)
    rounded(d, (dp[0] - 52, dp[1] - 20, dp[0] + 52, dp[1] + 20), 7, fill=fill, outline=light, width=2)
    for dx, dy in ((0, -42), (0, 41), (-42, 0), (41, 0)):
        line(d, [(dp[0] + dx - 4, dp[1] + dy), (dp[0] + dx + 4, dp[1] + dy)], rgba(light, 120), 2)

    # Xbox face buttons: Y top, B right, A bottom, X left.
    for name, cx, cy in (("Y", 694, 160), ("B", 750, 216), ("A", 694, 272), ("X", 638, 216)):
        col = XBOX[name.lower()] if color else WHITE
        ellipse(d, (cx - 26, cy - 26, cx + 26, cy + 26), fill=fill, outline=rgba(col, 225), width=2)
        ellipse(d, (cx - 20, cy - 20, cx + 20, cy + 20), outline=rgba(col, 140), width=1)
        text(d, (cx, cy + 1), name, 23, col, anchor="mm", stroke=1, stroke_fill=(0, 0, 0, 180))

    # Guide, View and Menu.
    ellipse(d, (484, 178, 540, 234), fill=(17, 19, 23, 255) if color else None,
            outline=rgba(CYAN if color else WHITE, 220), width=2)
    line(d, [(499, 193), (525, 219)], rgba(CYAN if color else WHITE, 220), 3)
    line(d, [(525, 193), (499, 219)], rgba(CYAN if color else WHITE, 220), 3)
    ellipse(d, (503, 197, 521, 215), outline=rgba(CYAN if color else WHITE, 210), width=2)

    for cx, name in ((470, "VIEW"), (554, "MENU")):
        rounded(d, (cx - 12, 270, cx + 12, 292), 4, outline=rgba(light, 190), width=2)
        if name == "VIEW":
            rounded(d, (cx - 10, 260, cx + 4, 278), 3, outline=rgba(light, 150), width=1)
            rounded(d, (cx - 4, 266, cx + 10, 284), 3, outline=rgba(light, 150), width=1)
        else:
            for yy in (265, 273, 281):
                line(d, [(cx - 7, yy), (cx + 7, yy)], rgba(light, 180), 2)
        text(d, (cx, 304), name, 9, rgba(light, 160), anchor="mm")

    # Grip accents.
    line(d, [(198, 360), (228, 448), (276, 480)], rgba(light, 75), 2)
    line(d, [(826, 360), (796, 448), (748, 480)], rgba(light, 75), 2)


def draw_controller(color: bool) -> Image.Image:
    mask, outline = silhouette()
    img = Image.new("RGBA", (W * SCALE, H * SCALE), (0, 0, 0, 0))
    if color:
        grad = Image.new("RGBA", img.size)
        gd = ImageDraw.Draw(grad)
        top, bottom = (48, 53, 61, 255), (18, 20, 24, 255)
        for y in range(img.height):
            t = y / max(1, img.height - 1)
            gd.line([(0, y), (img.width, y)], fill=(
                int(top[0] * (1 - t) + bottom[0] * t),
                int(top[1] * (1 - t) + bottom[1] * t),
                int(top[2] * (1 - t) + bottom[2] * t), 255))
        img.alpha_composite(Image.composite(grad, Image.new("RGBA", img.size), mask))
        img.alpha_composite(Image.composite(Image.new("RGBA", img.size, OUTLINE),
                                            Image.new("RGBA", img.size), outline))
    else:
        img.alpha_composite(Image.composite(Image.new("RGBA", img.size, WHITE),
                                            Image.new("RGBA", img.size), outline))
    d = ImageDraw.Draw(img)
    draw_internals(d, color)
    return img.resize((W, H), Image.LANCZOS)


def fill_from_border(blocked: np.ndarray) -> np.ndarray:
    """Mark the free pixels connected to the border of a boolean blocked mask."""
    height, width = blocked.shape
    filled = np.zeros_like(blocked)
    stack = []
    for x in range(width):
        for y in (0, height - 1):
            if not blocked[y, x] and not filled[y, x]:
                filled[y, x] = True
                stack.append((y, x))
    for y in range(height):
        for x in (0, width - 1):
            if not blocked[y, x] and not filled[y, x]:
                filled[y, x] = True
                stack.append((y, x))
    while stack:
        y, x = stack.pop()
        left = x
        while left > 0 and not blocked[y, left - 1]:
            left -= 1
        right = x
        while right < width - 1 and not blocked[y, right + 1]:
            right += 1
        filled[y, left:right + 1] = True
        for row in (y - 1, y + 1):
            if row < 0 or row >= height:
                continue
            span = ~blocked[row, left:right + 1] & ~filled[row, left:right + 1]
            if not span.any():
                continue
            columns = np.nonzero(span)[0] + left
            for column in columns:
                filled[row, column] = True
                stack.append((row, int(column)))
    return filled


def tutorial_lineart() -> Image.Image:
    """The Xbox Series line art in the shape the game expects: a greyscale
    drawing inside an opaque silhouette mask, like the original texture."""
    art = Image.open(LINEART_CONTROLLER).convert("RGBA")
    scale = min(W / art.width, H / art.height)
    resized = art.resize((max(1, round(art.width * scale)), max(1, round(art.height * scale))), Image.LANCZOS)
    image = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    image.alpha_composite(resized, ((W - resized.width) // 2, (H - resized.height) // 2))
    lines = np.array(image)[:, :, 3]
    ink = lines > 32
    enclosed = ~ink & ~fill_from_border(ink)
    silhouette = ink | enclosed
    drawing = np.repeat(lines[:, :, None], 3, axis=2)
    alpha = np.where(silhouette, 255, 0).astype(np.uint8)
    return Image.fromarray(np.dstack((drawing, alpha)).astype(np.uint8), "RGBA")


def svg_controller_images() -> tuple[Image.Image, Image.Image]:
    """The Xbox Series menu illustration with its caption lines, and the tutorial line art."""
    color = Image.open(SVG_CONTROLLER).convert("RGBA")
    if color.size != (W, H):
        raise SystemExit(f"{SVG_CONTROLLER} must be {W}x{H}")
    return paint_callouts(color), tutorial_lineart()


def label(d: ImageDraw.ImageDraw, xy, lines, size, anchor, leader_to=None, color=(240, 180, 50, 255)):
    x, y = xy
    line_h = size + 2
    widths = [d.textlength(t, font=font(size)) / SCALE for t in lines]
    pad = 5
    total_h = line_h * len(lines) + pad * 2
    if anchor == "ra":
        box = (x - max(widths) - pad * 2, y - pad, x, y + total_h)
        tx = x - pad
        start = (box[2], (box[1] + box[3]) / 2)
    else:
        box = (x, y - pad, x + max(widths) + pad * 2, y + total_h)
        tx = x + pad
        start = (box[0], (box[1] + box[3]) / 2)
    rounded(d, box, 3, fill=(0, 0, 0, 245))
    for i, t in enumerate(lines):
        text(d, (tx, y + i * line_h), t, size, color, anchor=anchor, stroke=1, stroke_fill=(0, 0, 0, 230))
    if leader_to:
        line(d, [start, leader_to], (240, 180, 50, 210), 2)
        ellipse(d, (leader_to[0] - 3, leader_to[1] - 3, leader_to[0] + 3, leader_to[1] + 3),
                fill=(240, 180, 50, 255))


def draw_labeled(labels: dict) -> Image.Image:
    base = draw_controller(color=False)
    img = Image.new("RGBA", (W * SCALE, H * SCALE), (0, 0, 0, 255))
    img.alpha_composite(base.resize((W * SCALE, H * SCALE), Image.LANCZOS))
    d = ImageDraw.Draw(img)
    label(d, (246, 20), labels["lt"], 16, "ra", (300, 76))
    label(d, (246, 78), labels["lb"], 16, "ra", (310, 124))
    label(d, (246, 494), ["Move"], 16, "ra", (330, 280))
    label(d, (778, 20), labels["rt"], 16, "la", (724, 76))
    label(d, (778, 78), labels["rb"], 16, "la", (714, 124))
    label(d, (782, 158), ["Switch Grenade"], 15, "la", (716, 162))
    label(d, (782, 220), labels["b"], 15, "la", (738, 218))
    label(d, (782, 282), ["Interaction"], 15, "la", (706, 274))
    label(d, (782, 344), ["Switch Weapon"], 15, "la", (656, 238))
    label(d, (866, 494), ["Aim"], 16, "la", (712, 372))
    return img.resize((W, H), Image.LANCZOS)


def draw_accept() -> Image.Image:
    S = 4
    img = Image.new("RGBA", (128 * S, 128 * S), (0, 0, 0, 0))
    shadow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow)
    sd.ellipse([10 * S, 12 * S, 118 * S, 120 * S], fill=(0, 0, 0, 120))
    img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(5 * S)))
    d = ImageDraw.Draw(img)
    d.ellipse([10 * S, 8 * S, 118 * S, 116 * S], fill=(58, 61, 67, 255),
              outline=(120, 126, 136, 255), width=2 * S)
    d.ellipse([18 * S, 16 * S, 110 * S, 108 * S], outline=(80, 85, 94, 255), width=2 * S)
    glow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.text((64 * S, 62 * S), "A", font=ImageFont.truetype(str(FONT), 54 * S), fill=XBOX["a"], anchor="mm")
    img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(3 * S)))
    d = ImageDraw.Draw(img)
    d.text((64 * S, 62 * S), "A", font=ImageFont.truetype(str(FONT), 54 * S), fill=XBOX["a"], anchor="mm",
           stroke_width=2 * S, stroke_fill=(15, 40, 18, 255))
    return img.resize((128, 128), Image.LANCZOS)


LABELS = {
    "saboteur": {
        "lt": ["Rush (HOLD: Vault)"],
        "lb": ["Throw Grenade", "(TAP AGAIN: Detonate)"],
        "rt": ["Melee"],
        "rb": ["Shoot", "(HOLD: Power Shot)"],
        "b": ["Heal"],
    },
    "bio": {
        "lt": ["Warp (HOLD: Vault)"],
        "lb": ["Throw Grenade", "(TAP AGAIN: Detonate)"],
        "rt": ["Melee"],
        "rb": ["Shoot", "(HOLD: Power Shot)"],
        "b": ["??????"],
    },
    "tank": {
        "lt": ["Power Rush (HOLD)"],
        "lb": ["Throw Grenade", "(TAP AGAIN: Detonate)"],
        "rt": ["Melee", "(HOLD: Power Melee)"],
        "rb": ["Shoot", "(HOLD: Power Shot)"],
        "b": ["Shield"],
    },
}


ASSETS = [
    # name, CUSA00062 offset, CUSA04430 offset, image, format, block bytes, mips
    ("controller_color", 0x0D9119C0, 0x0E56AD80, "controller_color", 41, 16, False),
    ("controller_white", 0x1AD80380, 0x1BAD5480, "controller_white", 10, 4, True),
    ("help_saboteur", 0x1ADDA900, 0x1BB2FAC0, "help_saboteur", 35, 8, True),
    ("help_bio", 0x1ADFED40, 0x1BB53900, "help_bio", 35, 8, True),
    ("help_tank", 0x1AE0C380, 0x1BB60F40, "help_tank", 35, 8, True),
    ("accept_a", 0x0D90B000, 0x0E5643C0, "accept_a", 37, 16, False),
    ("accept_a_alt", 0x31913740, 0x32C2BC40, "accept_a", 37, 16, False),
]
BUILDS = [("CUSA00062-1.00", 1), ("CUSA04430-1.00", 2)]

SCENE_PAYLOAD_OFFSET = 0x94
SCENE_MAGIC = 0x53434E45  # ENCS
SCRIPT_MAGIC = 0x53435249  # IRCS
SCENE_RECORD_MAGIC = b"\x6e\x53\x6d\xba"
SCENE_NAME_TO_MAGIC = 0x4C
SCENE_POSITION_BEFORE_MAGIC = 0x20
SCRIPT_PAYLOAD_OFFSET = 0x30
LAYOUT_BLOCKS = [
    ("menu_layout", 0x475A5580, 0x488F3C80),
]

# Each caption line is drawn from its layout box in a shape fixed per caption:
# straight, vertical for touch/ls/rs, and the dpad_right (right, then down) and
# square (up, then left) elbows. Those shapes cannot reach the Xbox Series
# controls without crossing other buttons, so every box shrinks to a stub that
# only places its caption and the full lines are painted into controller_color.
# Points are menu pixels at 1920x1080; a route starts at its caption.
CALLOUT_COLOR = (0, 212, 212, 255)
CALLOUT_WIDTH = 4.5
CALLOUT_STUB = 6
CALLOUTS = {
    "l2": ("straight", [(450, 285), (712, 285)]),
    "l1": ("straight", [(450, 349), (690, 349)]),
    "r2": ("straight", [(1468, 285), (1195, 285)]),
    "r1": ("straight", [(1468, 349), (1235, 349)]),
    "options": ("straight", [(1468, 395), (1021, 395), (1021, 484)]),
    "triangle": ("straight", [(1468, 442), (1207, 442)]),
    "circle": ("straight", [(1468, 494), (1263, 494)]),
    "cross": ("straight", [(1468, 553), (1203, 553)]),
    "square": ("upleft", [(1468, 597), (1139, 597), (1139, 520)]),
    "dpad_up": ("straight", [(440, 588), (832, 588)]),
    "dpad_left": ("straight", [(440, 632), (790, 632)]),
    "dpad_down": ("straight", [(440, 676), (832, 676)]),
    "dpad_right": ("downright", [(440, 736), (889, 736), (889, 645)]),
    "touch": ("down", [(896, 300), (896, 484)]),
    "ls": ("up", [(922, 787), (922, 541), (786, 541)]),
    "rs": ("up", [(1104, 840), (1104, 676)]),
}
# Alternative captions that share a control with one above.
CALLOUT_ALIASES = {"dpad": "dpad_left", "face": "circle"}
LABEL_PAGES = ("Template_Controller", "Menu_Options_Controller", "Menu_Briefing_Loading")
SCRIPT_SOURCE_OFFSET = 0x18

# The controller template scene is drawn over controller_white by the in-game
# tutorial hints, which highlight the button they teach at these points. The
# Controller Settings caption lines do not use them; see CALLOUTS.
#
# Each anchor is placed on the Xbox Series X control that emits the matching
# PlayStation button, because runtime_pad.c maps the gamepad buttons one to one
# (LB=L1, LT=L2, RB=R1, RT=R2, X=square, Y=triangle, B=circle, A=cross,
# View=touchpad, Menu=options). Points are controller_white line-art pixels.
SCENES = [
    ("controller_template", 0x1AD7F6C0, 0x1BAD47C0),
]
SCENE_ANCHORS = {
    "left_stick": (312, 155),
    "right_stick": (609, 273),
    "cross_button": (703, 212),
    "circle_button": (753, 163),
    "triangle_button": (703, 113),
    "square_button": (653, 163),
    "r1_button": (700, 34),
    "r2_button": (690, 14),
    "l1_button": (320, 34),
    "l2_button": (330, 14),
    "l3_button": (312, 155),
    "r3_button": (609, 273),
    "touch_button": (465, 159),
    "ps_button": (511, 87),
    "share_button": (511, 198),
    "options_button": (559, 159),
    "directional_button": (409, 272),
    "dpad_down_button": (409, 303),
    "dpad_up_button": (409, 240),
    "dpad_right_button": (443, 272),
    "dpad_left_button": (375, 272),
}


def texture_to_scene(pixel_x: float, pixel_y: float) -> tuple[float, float]:
    """Scene position of a controller texture pixel, measured on the original artwork."""
    return (0.001424085 * pixel_x - 0.722275, -0.001458715 * pixel_y + 0.373644)


def minify_lua(text: str) -> str:
    """Collapse Lua whitespace outside string literals and long brackets."""
    out = []
    index = 0
    length = len(text)
    while index < length:
        char = text[index]
        if char in "\"'":
            quote = char
            out.append(char)
            index += 1
            while index < length:
                out.append(text[index])
                if text[index] == "\\":
                    if index + 1 < length:
                        out.append(text[index + 1])
                        index += 2
                        continue
                if text[index] == quote:
                    index += 1
                    break
                index += 1
            continue
        if text.startswith("--[[", index):
            end = text.find("]]", index + 4)
            end = length if end < 0 else end + 2
            out.append(text[index:end])
            index = end
            continue
        if text.startswith("--", index):
            end = text.find("\n", index)
            end = length if end < 0 else end
            out.append(text[index:end])
            index = end
            continue
        if text.startswith("[[", index):
            end = text.find("]]", index + 2)
            end = length if end < 0 else end + 2
            out.append(text[index:end])
            index = end
            continue
        if char in " \t\r\n":
            while index < length and text[index] in " \t\r\n":
                index += 1
            out.append(" ")
            continue
        out.append(char)
        index += 1
    return re.sub(r" ?([{}\[\]=,;()]) ?", r"\1", "".join(out))


LUA_TOKEN = re.compile(rb"""
    (?P<space>\s+)
  | (?P<comment>--\[\[.*?\]\]|--[^\n]*)
  | (?P<long>\[\[.*?\]\])
  | (?P<string>"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')
  | (?P<number>-?\d+\.?\d*)
  | (?P<name>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<symbol>[^\s])
""", re.S | re.X)


def lua_tokens(source: bytes) -> list[tuple[str, bytes]]:
    tokens = []
    position = 0
    while position < len(source):
        match = LUA_TOKEN.match(source, position)
        if not match:
            tokens.append(("bad", source[position:position + 1]))
            position += 1
            continue
        position = match.end()
        if match.lastgroup in ("space", "comment"):
            continue
        tokens.append((match.lastgroup, match.group()))
    return tokens


def same_lua_syntax(original: bytes, patched: bytes) -> bool:
    """True when the two sources differ only in numeric literals."""
    left = lua_tokens(original)
    right = lua_tokens(patched)
    if len(left) != len(right):
        return False
    for (lkind, lvalue), (rkind, rvalue) in zip(left, right):
        if lkind == "number" and rkind == "number":
            continue
        if lkind != rkind or lvalue != rvalue:
            return False
    return True


# Menu pixel mappings measured on 1920x1080 captures of Controller Settings.
def menu_to_box(x: float, y: float) -> tuple[float, float]:
    return ((x - 961.0) / 1052.0, (532.0 - y) / 1062.0)


def menu_to_texture(x: float, y: float) -> tuple[float, float]:
    return ((x - 440.0) / 1.015, (y - 268.0) / 1.035)


TEXTURE_BOTTOM = 268.0 + H * 1.035


def callout_box(name: str, original: tuple[float, float, float, float]) -> tuple[float, float, float, float]:
    """The stub box (left, right, top, bottom) that places a caption at the start of its route."""
    shape, route = CALLOUTS[CALLOUT_ALIASES.get(name, name)]
    (x0, y0), (x1, y1) = route[0], route[1]
    left, right, top, bottom = original
    if shape == "straight":
        height = top - bottom
        x_end = x0 + CALLOUT_STUB if x1 > x0 else x0 - CALLOUT_STUB
        (bx0, by), (bx1, _) = menu_to_box(x0, y0), menu_to_box(x_end, y0)
        return (min(bx0, bx1), max(bx0, bx1), by + height / 2, by - height / 2)
    if shape == "downright":
        (bl, bt), (br, bb) = menu_to_box(x0, y0), menu_to_box(x0 + CALLOUT_STUB, y0 + 2)
        return (bl, br, bt, bb)
    if shape == "upleft":
        (bl, bt), (br, bb) = menu_to_box(x0 - CALLOUT_STUB, y0 - 2), menu_to_box(x0, y0)
        return (bl, br, bt, bb)
    half = (right - left) / 2
    if shape == "down":
        (bx, bt), (_, bb) = menu_to_box(x0, y0), menu_to_box(x0, y0 + CALLOUT_STUB)
        return (bx - half, bx + half, bt, bb)
    # "up": the stub reaches into the texture so the painted line joins it.
    stub_top = min(y0 - CALLOUT_STUB, TEXTURE_BOTTOM - 8)
    (bx, bt), (_, bb) = menu_to_box(x0, stub_top), menu_to_box(x0, y0)
    return (bx - half, bx + half, bt, bb)


def paint_callouts(image: Image.Image) -> Image.Image:
    """Paint every caption route into the controller texture with square corners."""
    factor = 4
    overlay = Image.new("RGBA", (W * factor, H * factor), (0, 0, 0, 0))
    d = ImageDraw.Draw(overlay)
    half_x = CALLOUT_WIDTH / 2 / 1.015 * factor
    half_y = CALLOUT_WIDTH / 2 / 1.035 * factor
    for _, route in CALLOUTS.values():
        points = [menu_to_texture(x, y) for x, y in route]
        for (ax, ay), (bx, by) in zip(points, points[1:]):
            d.rectangle([min(ax, bx) * factor - half_x, min(ay, by) * factor - half_y,
                         max(ax, bx) * factor + half_x, max(ay, by) * factor + half_y], fill=CALLOUT_COLOR)
    painted = image.copy()
    painted.alpha_composite(overlay.resize((W, H), Image.LANCZOS))
    return painted


def patch_menu_layout(payload: bytes) -> bytes:
    """Shrink the callout boxes to caption stubs, keeping the payload byte length unchanged."""
    header = payload[:SCRIPT_SOURCE_OFFSET]
    text = payload[SCRIPT_SOURCE_OFFSET:].decode("latin-1")
    pages = "|".join(LABEL_PAGES)
    pattern = re.compile(
        r'box_page = "(' + pages + r')", box_name = "([^"]+)",\s*'
        r'left = (-?[\d.]+),\s*right = (-?[\d.]+),\s*'
        r'top = (-?[\d.]+),\s*bottom = (-?[\d.]+),', re.S)

    def replace(match):
        name = match.group(2)
        if name not in CALLOUTS and name not in CALLOUT_ALIASES:
            return match.group(0)
        values = callout_box(name, tuple(float(match.group(i)) for i in range(3, 7)))
        body = ", ".join(f"{key} = {value:.5f}" for key, value in
                         zip(("left", "right", "top", "bottom"), values))
        return f'box_page = "{match.group(1)}", box_name = "{name}", {body},'

    edited = minify_lua(pattern.sub(replace, text))
    if len(edited) > len(text):
        raise SystemExit(f"menu layout grew to {len(edited)} bytes (limit {len(text)})")
    edited += " " * (len(text) - len(edited))
    if not same_lua_syntax(text.encode("latin-1"), edited.encode("latin-1")):
        raise SystemExit("menu layout minification changed the Lua token stream")
    patched = header + edited.encode("latin-1")
    if len(patched) != len(payload):
        raise SystemExit("menu layout patch changed the payload length")
    return patched


def patch_controller_scene(payload: bytes) -> bytes:
    data = bytearray(payload)
    for name, pixel in SCENE_ANCHORS.items():
        x, y = texture_to_scene(*pixel)
        name_offset = data.find(name.encode("ascii") + b"\x00")
        if name_offset < 0:
            raise SystemExit(f"controller scene anchor not found: {name}")
        magic = name_offset - SCENE_NAME_TO_MAGIC
        if data[magic:magic + 4] != SCENE_RECORD_MAGIC:
            raise SystemExit(f"controller scene record magic mismatch: {name}")
        position = magic - SCENE_POSITION_BEFORE_MAGIC
        old = struct.unpack_from("<4f", data, position)
        struct.pack_into("<4f", data, position, x, y, old[2], old[3])
    return bytes(data)


def read_script_block(mm, offset, magic):
    id0, zsize, dsize, units, id1, kind = struct.unpack_from("<6I", mm, offset)
    if kind != magic:
        raise ValueError(f"{offset:#x}: unexpected block type")
    name = "scene" if magic == SCENE_MAGIC else "script"
    payload_offset = SCENE_PAYLOAD_OFFSET if magic == SCENE_MAGIC else SCRIPT_PAYLOAD_OFFSET
    if units == 0 or offset + units * 64 > len(mm):
        raise ValueError(f"{offset:#x}: bad {name} block size")
    compressed = bytes(mm[offset + payload_offset:offset + payload_offset + zsize])
    if len(compressed) != zsize:
        raise ValueError(f"{offset:#x}: short {name} payload")
    payload = zlib.decompress(compressed)
    if len(payload) != dsize:
        raise ValueError(f"{offset:#x}: short {name} data")
    return {
        "offset": offset,
        "zsize": zsize,
        "dsize": dsize,
        "units": units,
        "id0": id0,
        "id1": id1,
        "payload_offset": payload_offset,
        "payload": payload,
        "crc": zlib.crc32(compressed) & 0xFFFFFFFF,
    }


def read_scene_block(mm, offset):
    return read_script_block(mm, offset, SCENE_MAGIC)


def encode_bc1(img: Image.Image) -> bytes:
    rgba = np.array(img.convert("RGBA"))
    h, w = rgba.shape[:2]
    blocks = rgba.reshape(h // 4, 4, w // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
    rgb = blocks[:, :, :3].astype(np.int32)
    mn, mx = rgb.min(1), rgb.max(1)
    c0 = ((mx[:, 0] >> 3) << 11) | ((mx[:, 1] >> 2) << 5) | (mx[:, 2] >> 3)
    c1 = ((mn[:, 0] >> 3) << 11) | ((mn[:, 1] >> 2) << 5) | (mn[:, 2] >> 3)
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    equal = c0 == c1
    c1 = np.where(equal & (c0 > 0), c0 - 1, c1)
    c0 = np.where(equal & (c0 == 0), 1, c0)

    def expand(c):
        r = ((c >> 11) & 31) * 255 // 31
        g = ((c >> 5) & 63) * 255 // 63
        b = (c & 31) * 255 // 31
        return np.stack([r, g, b], axis=1)

    p0, p1 = expand(c0), expand(c1)
    palette = np.stack([p0, p1, (2 * p0 + p1) // 3, (p0 + 2 * p1) // 3], axis=1)
    diff = rgb[:, :, None, :] - palette[:, None, :, :]
    idx = (diff * diff).sum(-1).argmin(-1)
    bits = np.zeros(len(idx), dtype=np.uint32)
    for i in range(16):
        bits |= idx[:, i].astype(np.uint32) << (2 * i)
    out = np.zeros((len(idx), 8), dtype=np.uint8)
    out[:, 0] = c0 & 0xFF
    out[:, 1] = c0 >> 8
    out[:, 2] = c1 & 0xFF
    out[:, 3] = c1 >> 8
    out[:, 4] = bits & 0xFF
    out[:, 5] = (bits >> 8) & 0xFF
    out[:, 6] = (bits >> 16) & 0xFF
    out[:, 7] = (bits >> 24) & 0xFF
    return out.tobytes()


def encode_bc3(img: Image.Image) -> bytes:
    rgba = np.array(img.convert("RGBA"))
    h, w = rgba.shape[:2]
    blocks = rgba.reshape(h // 4, 4, w // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
    rgb = blocks[:, :, :3].astype(np.int32)
    mn, mx = rgb.min(1), rgb.max(1)
    c0 = ((mx[:, 0] >> 3) << 11) | ((mx[:, 1] >> 2) << 5) | (mx[:, 2] >> 3)
    c1 = ((mn[:, 0] >> 3) << 11) | ((mn[:, 1] >> 2) << 5) | (mn[:, 2] >> 3)
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    equal = c0 == c1
    c1 = np.where(equal & (c0 > 0), c0 - 1, c1)
    c0 = np.where(equal & (c0 == 0), 1, c0)

    def expand(c):
        r = ((c >> 11) & 31) * 255 // 31
        g = ((c >> 5) & 63) * 255 // 63
        b = (c & 31) * 255 // 31
        return np.stack([r, g, b], axis=1)

    p0, p1 = expand(c0), expand(c1)
    palette = np.stack([p0, p1, (2 * p0 + p1) // 3, (p0 + 2 * p1) // 3], axis=1)
    diff = rgb[:, :, None, :] - palette[:, None, :, :]
    cidx = (diff * diff).sum(-1).argmin(-1)
    cbits = np.zeros(len(cidx), dtype=np.uint32)
    for i in range(16):
        cbits |= cidx[:, i].astype(np.uint32) << (2 * i)

    alpha = blocks[:, :, 3].astype(np.int32)
    a0, a1 = alpha.max(1), alpha.min(1)
    eq = a0 == a1
    a1 = np.where(eq & (a0 > 0), a0 - 1, a1)
    a0 = np.where(eq & (a0 == 0), 1, a0)
    apal = np.stack([a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)], axis=1)
    adiff = alpha[:, :, None] - apal[:, None, :]
    aidx = np.abs(adiff).argmin(-1)
    abits = np.zeros(len(aidx), dtype=np.uint64)
    for i in range(16):
        abits |= aidx[:, i].astype(np.uint64) << (3 * i)
    out = np.zeros((len(blocks), 16), dtype=np.uint8)
    out[:, 0] = a0
    out[:, 1] = a1
    for i in range(6):
        out[:, 2 + i] = (abits >> (8 * i)) & 0xFF
    out[:, 8] = c0 & 0xFF
    out[:, 9] = c0 >> 8
    out[:, 10] = c1 & 0xFF
    out[:, 11] = c1 >> 8
    out[:, 12] = cbits & 0xFF
    out[:, 13] = (cbits >> 8) & 0xFF
    out[:, 14] = (cbits >> 16) & 0xFF
    out[:, 15] = (cbits >> 24) & 0xFF
    return out.tobytes()


def micro_index(x, y):
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3)


def tile(linear: bytes, width: int, height: int, elem_bytes: int, block: bool) -> bytes:
    bw = width // 4 if block else width
    bh = height // 4 if block else height
    pitch = bw
    tiles_per_row = max(1, (pitch + 7) // 8)
    out = bytearray(((bh + 7) // 8) * tiles_per_row * 64 * elem_bytes)
    for by in range(bh):
        for bx in range(bw):
            tile_index = (by // 8) * tiles_per_row + (bx // 8)
            src = (by * bw + bx) * elem_bytes
            dst = tile_index * 64 * elem_bytes + micro_index(bx & 7, by & 7) * elem_bytes
            out[dst:dst + elem_bytes] = linear[src:src + elem_bytes]
    return bytes(out)


def read_block(mm, offset):
    id0, zsize, dsize, units, id1, kind = struct.unpack_from("<6I", mm, offset)
    if kind != 0x54455820:  # "TEX " stored reversed in the file
        raise ValueError(f"{offset:#x}: not a TEX block")
    if units == 0 or offset + units * 64 > len(mm):
        raise ValueError(f"{offset:#x}: bad block size")
    pos = mm.find(b"\x78\xda", offset + 24, offset + units * 64)
    if pos < 0:
        raise ValueError(f"{offset:#x}: no deflate stream")
    data = zlib.decompress(mm[pos:offset + units * 64])
    if len(data) < dsize:
        raise ValueError(f"{offset:#x}: short payload")
    data = data[:dsize]
    return {
        "offset": offset,
        "zsize": zsize,
        "dsize": dsize,
        "units": units,
        "id0": id0,
        "id1": id1,
        "payload": data,
        "crc": zlib.crc32(mm[offset + 40:offset + 40 + zsize]) & 0xFFFFFFFF,
    }


def encode_mip(image: Image.Image, fmt: int, elem: int) -> bytes:
    # The game stores every mip level in tile columns that are 32 pixels wide.
    width = max(32, (image.width + 31) // 32 * 32)
    if fmt == 10:
        height = image.height
        padded = Image.new("RGBA", (width, height), (0, 0, 0, 0))
        padded.alpha_composite(image.convert("RGBA"), (0, 0))
        return tile(padded.convert("RGBA").tobytes(), width, height, elem, False)
    height = max(4, (image.height + 3) // 4 * 4)
    padded = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    padded.alpha_composite(image.convert("RGBA"), (0, 0))
    if fmt == 41:
        linear = bc7.encode(np.array(padded))
    elif fmt == 35:
        linear = encode_bc1(padded)
    elif fmt == 37:
        linear = encode_bc3(padded)
    else:
        raise ValueError(f"unsupported format {fmt}")
    return tile(linear, width, height, elem, True)


def build_payload(original: bytes, image: Image.Image, fmt: int, elem: int,
                  mips: bool = False) -> tuple[bytes, int, int]:
    prefix = original[:16]
    gnf = bytearray(original[16:16 + 0x100])
    if gnf[:4] != b"GNF ":
        raise ValueError("the source texture is not GNF")
    contents = struct.unpack_from("<I", gnf, 4)[0]
    if contents != 0xF8:
        raise ValueError(f"unexpected GNF contents size {contents:#x}")
    if mips:
        total = struct.unpack_from("<I", gnf, 0x2C)[0]
        pixel = bytearray()
        level = image
        while True:
            pixel += encode_mip(level, fmt, elem)
            if level.width <= 16 and level.height <= 8:
                break
            level = level.resize((max(1, level.width // 2), max(1, level.height // 2)), Image.LANCZOS)
        if len(pixel) > total:
            raise ValueError(f"mip chain exceeds the source texture: {len(pixel)} > {total}")
        pixel += b"\0" * (total - len(pixel))
        struct.pack_into("<I", gnf, 0x0C, 0x100 + len(pixel))
        payload = prefix + bytes(gnf) + pixel
        packed = zlib.compress(payload, 9)
        return packed, len(payload), total
    if fmt == 41:
        linear = bc7.encode(np.array(image.convert("RGBA")))
    elif fmt == 35:
        linear = encode_bc1(image)
    elif fmt == 37:
        linear = encode_bc3(image)
    elif fmt == 10:
        linear = image.convert("RGBA").tobytes()
    else:
        raise ValueError(f"unsupported format {fmt}")
    pixel = tile(linear, image.width, image.height, elem, fmt != 10)
    word = list(struct.unpack_from("<8I", gnf, 0x10))
    word[1] = (word[1] & ~(0x3F << 20)) | ((fmt & 0x3F) << 20)
    word[3] &= ~((0xF << 12) | (0xF << 16))
    word[7] = len(pixel)
    struct.pack_into("<8I", gnf, 0x10, *word)
    struct.pack_into("<I", gnf, 0x0C, 0x100 + len(pixel))
    payload = prefix + bytes(gnf) + pixel
    packed = zlib.compress(payload, 9)
    return packed, len(payload), word[7]


def c_array(name: str, data: bytes) -> str:
    body = []
    for i in range(0, len(data), 12):
        body.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 12]) + ",")
    return f"static const unsigned char {name}[] = {{\n" + "\n".join(body) + "\n};\n"


def write_data(entries, scenes, glyphs) -> None:
    parts = [
        "/* Generated by tools/prompts/build_controller_prompts.py. Do not edit. */",
        '#include "controller_prompts.h"',
        "",
    ]
    for name, data, _ in entries:
        parts.append(c_array("k_" + name, data))
    parts.append("const AlnControllerPromptPatch kControllerPromptPatches[] = {")
    for name, data, rows in entries:
        for row in rows:
            parts.append(
                f'    {{"{row["build"]}", "{row["name"]}", 0x{row["offset"]:x}, {row["oz"]}u, {row["od"]}u, '
                f'0x{row["crc"]:08x}u, {row["units"]}u, {row["nz"]}u, {row["nd"]}u, k_{name}}},'
            )
    parts.append("};")
    parts.append("")
    parts.append("const AlnControllerPromptPatch *AlnControllerPromptPatches(size_t *count) {")
    parts.append("    *count = sizeof(kControllerPromptPatches) / sizeof(kControllerPromptPatches[0]);")
    parts.append("    return kControllerPromptPatches;")
    parts.append("}")
    parts.append("")
    for index in range(len(scenes)):
        parts.append(c_array(f"k_scene_{index}", scenes[index][1]))
    parts.append("const AlnControllerScenePatch kControllerScenePatches[] = {")
    for index, (name, data, rows) in enumerate(scenes):
        for row in rows:
            parts.append(
                f'    {{"{row["build"]}", "{row["name"]}", 0x{row["offset"]:x}, {row["oz"]}u, {row["od"]}u, '
                f'0x{row["crc"]:08x}u, {row["units"]}u, {row["nz"]}u, 0x{row["payload_offset"]:x}u, '
                f'k_scene_{index}}},'
            )
    parts.append("};")
    parts.append("")
    parts.append("const AlnControllerScenePatch *AlnControllerScenePatches(size_t *count) {")
    parts.append("    *count = sizeof(kControllerScenePatches) / sizeof(kControllerScenePatches[0]);")
    parts.append("    return kControllerScenePatches;")
    parts.append("}")
    parts.append("")
    parts.append("static const char *const kGlyphNames[] = {")
    for glyph in glyphs:
        parts.append(f'    "{glyph}",')
    parts.append("};")
    parts.append("")
    parts.append("const char *const *AlnXboxGlyphNames(size_t *count) {")
    parts.append("    *count = sizeof(kGlyphNames) / sizeof(kGlyphNames[0]);")
    parts.append("    return kGlyphNames;")
    parts.append("}")
    parts.append("")
    OUT_DATA.write_text("\n".join(parts), encoding="utf-8", newline="\n")
    print("wrote", OUT_DATA)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", action="store_true", help="write PNG previews only")
    parser.add_argument("--game", default=str(ROOT / "game" / "prepared" / "CUSA00062-1.00"))
    parser.add_argument("--game-kr", default=str(ROOT / "game" / "prepared" / "CUSA04430-1.00"))
    args = parser.parse_args()
    OUT_ASSETS.mkdir(parents=True, exist_ok=True)
    controller_color, controller_white = svg_controller_images()
    images = {
        "controller_color": controller_color,
        "controller_white": controller_white,
        "accept_a": draw_accept(),
        "help_saboteur": draw_labeled(LABELS["saboteur"]),
        "help_bio": draw_labeled(LABELS["bio"]),
        "help_tank": draw_labeled(LABELS["tank"]),
    }
    for name in ("controller_color", "controller_white", "accept_a"):
        images[name].save(OUT_ASSETS / (name + ".png"), format="PNG")
        print("wrote", OUT_ASSETS / (name + ".png"))
    if args.preview:
        return

    sources = {1: Path(args.game) / "stream.dat", 2: Path(args.game_kr) / "stream.dat"}
    opened = {}
    for build, path in sources.items():
        if path.exists():
            f = path.open("rb")
            opened[build] = (f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ))
        else:
            print("missing", path, "- its build rows will not be generated")
    entries = []
    for name, eu, kr, image_name, fmt, elem, mips in ASSETS:
        image = images[image_name]
        source = None
        for build, offset in ((1, eu), (2, kr)):
            if build not in opened:
                continue
            mm = opened[build][1]
            block = read_block(mm, offset)
            if source is None:
                source = block
            else:
                source.setdefault("other", []).append(block)
        if source is None:
            raise SystemExit(f"{name}: no source build found")
        packed, dsize, _ = build_payload(source["payload"], image, fmt, elem, mips)
        rows = []
        for build, offset in ((1, eu), (2, kr)):
            if build not in opened:
                continue
            mm = opened[build][1]
            block = read_block(mm, offset)
            if 40 + len(packed) > block["units"] * 64:
                raise SystemExit(
                    f"{name}: {len(packed)} compressed bytes do not fit the {block['units'] * 64 - 40}-byte block")
            rows.append({
                "build": BUILDS[build - 1][0], "name": name, "offset": offset,
                "oz": block["zsize"], "od": block["dsize"], "crc": block["crc"],
                "units": block["units"], "nz": len(packed), "nd": dsize,
            })
        print(f"{name}: {len(packed)} compressed, {dsize} uncompressed, "
              f"fits {rows[0]['units'] * 64 - 40} bytes")
        entries.append((name, packed, rows))

    scenes = []
    for name, eu, kr in SCENES:
        source = None
        for build, offset in ((1, eu), (2, kr)):
            if build not in opened:
                continue
            block = read_scene_block(opened[build][1], offset)
            if source is None:
                source = block
            elif block["payload"] != source["payload"]:
                raise SystemExit(f"{name}: the builds use different controller scenes")
        if source is None:
            raise SystemExit(f"{name}: no source build found")
        patched = patch_controller_scene(source["payload"])
        packed = zlib.compress(patched, 9)
        rows = []
        for build, offset in ((1, eu), (2, kr)):
            if build not in opened:
                continue
            block = read_scene_block(opened[build][1], offset)
            if SCENE_PAYLOAD_OFFSET + len(packed) > block["units"] * 64:
                raise SystemExit(
                    f"{name}: {len(packed)} compressed bytes do not fit the "
                    f"{block['units'] * 64 - SCENE_PAYLOAD_OFFSET}-byte scene block")
            rows.append({
                "build": BUILDS[build - 1][0], "name": name, "offset": offset,
                "oz": block["zsize"], "od": block["dsize"], "crc": block["crc"],
                "units": block["units"], "nz": len(packed),
                "payload_offset": SCENE_PAYLOAD_OFFSET,
            })
        print(f"{name}: {len(packed)} compressed, {len(patched)} uncompressed, "
              f"fits {rows[0]['units'] * 64 - SCENE_PAYLOAD_OFFSET} bytes")
        scenes.append((name, packed, rows))

    for name, eu, kr in LAYOUT_BLOCKS:
        for build, offset in ((1, eu), (2, kr)):
            if build not in opened:
                continue
            block = read_script_block(opened[build][1], offset, SCRIPT_MAGIC)
            patched = patch_menu_layout(block["payload"])
            if len(patched) != block["dsize"]:
                raise SystemExit(f"{name}: layout patch changed the payload size")
            packed = zlib.compress(patched, 9)
            if SCRIPT_PAYLOAD_OFFSET + len(packed) > block["units"] * 64:
                raise SystemExit(
                    f"{name}: {len(packed)} compressed bytes do not fit the "
                    f"{block['units'] * 64 - SCRIPT_PAYLOAD_OFFSET}-byte layout block")
            row = {
                "build": BUILDS[build - 1][0], "name": name, "offset": offset,
                "oz": block["zsize"], "od": block["dsize"], "crc": block["crc"],
                "units": block["units"], "nz": len(packed),
                "payload_offset": SCRIPT_PAYLOAD_OFFSET,
            }
            scenes.append((name, packed, [row]))
            print(f"{name}: {len(packed)} compressed, {len(patched)} uncompressed, "
                  f"fits {block['units'] * 64 - SCRIPT_PAYLOAD_OFFSET} bytes")

    for f, mm in opened.values():
        mm.close()
        f.close()
    if not entries:
        raise SystemExit("no patch rows generated")
    glyphs = ["A", "B", "X", "Y", "LB", "RB", "LT", "RT", "View", "Menu", "Left Stick / L3", "Right Stick / R3"]
    write_data(entries, scenes, glyphs)


if __name__ == "__main__":
    main()
