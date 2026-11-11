"""Xbox prompts do not depend on the text language. For every text language a prepared build offers, every font
that language draws with and that holds the PlayStation button glyphs must sample an atlas the Xbox prompts redraw, at
exactly those glyphs' cells, and the redraw must not depend on anything but the build and the prompt style.

A build whose prepared folder is missing is skipped. Usage: python tests/prompt_language_tests.py
"""
from pathlib import Path
import math
import mmap
import re
import struct
import sys
import zlib

ROOT = Path(__file__).resolve().parent.parent
FONT, TEXTURE = 0x464F4E54, 0x54455820

# The game's text language names, from its language table, and the script of the fonts each one draws with.
LATIN = ('en', 'bport', 'da', 'de', 'es', 'fi', 'fr', 'it', 'las', 'nl', 'nor', 'pol', 'port', 'ru', 'sw')
LANGUAGES = {
    'CUSA00062-1.00': {name: 'latin' for name in LATIN},
    'CUSA00062-1.06': {**{name: 'latin' for name in LATIN}, 'cn': 'chinese', 'jp': 'japanese', 'kr': 'korean'},
    'CUSA04430-1.00': {**{name: 'latin' for name in LATIN}, 'cn': 'chinese', 'jp': 'japanese', 'kr': 'korean'},
}
# A character only that script's fonts have: Hangul, hiragana, and the Traditional Chinese 戲 (Japanese uses 戯).
PROBES = (('korean', 0xD55C), ('japanese', 0x3072), ('chinese', 0x6232))

# Each PlayStation button glyph and the Xbox one shown in its place, whatever the language.
XBOX = {
    0xE000: 'B', 0xE001: 'X', 0xE002: 'Y', 0xE003: 'A',  # Circle, Square, Triangle, Cross
    0xE004: 'LB', 0xE005: 'LT', 0xE006: 'RB', 0xE007: 'RT',  # L1, L2, R1, R2
    0xE008: 'RS', 0xE009: 'LS', 0xE013: 'LS', 0xE014: 'RS',  # R, L, L3, R3
    0xE00A: 'VIEW', 0xE012: 'MENU',  # SHARE, OPTIONS
}

failures = []


def check(condition, text):
    if not condition:
        failures.append(text)
    return condition


def blocks(mm):
    offset = 0
    while offset + 24 <= len(mm):
        id0, zsize, dsize, units, id1, kind = struct.unpack_from('<6I', mm, offset)
        if id0 != offset // 64 or id1 != id0 or not units:
            offset += 64
            continue
        yield offset, kind, units
        offset += units * 64


def font_data(mm, offset, units):
    dsize = struct.unpack_from('<I', mm, offset + 8)[0]
    start = offset + 0x30 + 4 * struct.unpack_from('<I', mm, offset + 0x1C)[0]
    return zlib.decompressobj().decompress(mm[start:offset + units * 64])[:dsize]


def record(font, codepoint):
    begin, end = struct.unpack_from('<QQ', font, 0x28 + (codepoint >> 8) * 16)
    at = begin + (codepoint & 0xFF) * 56
    if not begin or at + 56 > end:
        return None
    u0, v0, u1, v1 = struct.unpack_from('<4f', font, at)
    return (u0, v0, u1, v1) if u1 > u0 and v1 > v0 else None


def script(font):
    return next((name for name, codepoint in PROBES if record(font, codepoint)), 'latin')


def atlas_size(mm, offset):
    zsize = struct.unpack_from('<I', mm, offset + 4)[0]
    head = zlib.decompressobj().decompress(mm[offset + 40:offset + 40 + zsize], 16 + 0x20)
    dimensions = struct.unpack_from('<I', head, 16 + 0x18)[0]
    return (dimensions & 0x3FFF) + 1, ((dimensions >> 14) & 0x3FFF) + 1


def language_names(eboot):
    start = eboot.find(b'\0en\0bport\0')
    names = []
    for name in eboot[start + 1:start + 200].split(b'\0'):
        if name and name.decode('ascii', 'replace') not in LATIN + ('cn', 'jp', 'kr'):
            break
        if name:
            names.append(name.decode())
    return names if start >= 0 else []


def generator_atlases(build):
    """(atlas name, atlas offset, font offset) of the build's font-derived atlases in the glyph generator."""
    source = (ROOT / 'tools' / 'prompts' / 'build_prompt_glyphs.py').read_text(encoding='utf-8')
    section = re.search(r'"' + re.escape(build) + r'": \[(.*?)\]', source, re.S)
    entry = r'\("(\w+)", (0x[0-9A-Fa-f]+), FontCells\((0x[0-9A-Fa-f]+)\)\)'
    entries = re.findall(entry, section.group(1) if section else '')
    return [(name, int(atlas, 16), int(font, 16)) for name, atlas, font in entries]


def generator_order():
    source = (ROOT / 'tools' / 'prompts' / 'build_prompt_glyphs.py').read_text(encoding='utf-8')
    table = source.split('BUTTON_GLYPHS = (', 1)[1].split('\n)', 1)[0]
    glyph = r'\((0xE0[0-9A-Fa-f]{2}), "\w+", "(\w+)"\)'
    return [(int(codepoint, 16), label) for codepoint, label in re.findall(glyph, table)]


def glyph_masks(build, atlas_name, atlas_offset):
    data = (ROOT / 'launcher' / 'controller_prompts_glyphs_data.c').read_text(encoding='utf-8')
    row = re.search(r'\{"' + re.escape(build) + r'", "' + atlas_name + r'", (0x[0-9a-f]+), \d+u, \d+u, 0x[0-9a-f]+u, '
                    r'\d+u, (k_\w+_masks), (\d+)u\}', data)
    if not row or int(row.group(1), 16) != atlas_offset:
        return None
    table = re.search(r'static const AlnPromptGlyphMask ' + row.group(2) + r'\[\] = \{(.*?)\};', data, re.S)
    masks = [tuple(int(value) for value in mask) for mask in
             re.findall(r'\{(\d+)u, (\d+)u, (\d+)u, (\d+)u, \d+u, \d+u, k_\w+\}', table.group(1) if table else '')]
    return masks if len(masks) == int(row.group(3)) else None


def check_build(build):
    folder = ROOT / 'game' / 'prepared' / build
    if not (folder / 'stream.dat').exists() or not (folder / 'eboot.bin').exists():
        print(f'skipped {build}: {folder} not found')
        return
    languages = LANGUAGES[build]
    check(sorted(language_names((folder / 'eboot.bin').read_bytes())) == sorted(languages),
          f'{build}: the language table is not {sorted(languages)}')
    order = generator_order()
    with (folder / 'stream.dat').open('rb') as file:
        mm = mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ)
        fonts = {offset: font_data(mm, offset, units) for offset, kind, units in blocks(mm) if kind == FONT}
        redrawn = {font: (name, atlas) for name, atlas, font in generator_atlases(build)}
        by_script = {}
        for offset, font in fonts.items():
            buttons = [codepoint for codepoint in XBOX if record(font, codepoint)]
            by_script.setdefault(script(font), []).append(offset)
            if not buttons:
                continue
            # A font that can draw a PlayStation button must have every one of them redrawn, where it samples them.
            if not check(offset in redrawn, f'{build}: the {script(font)} font at {offset:#x} draws PlayStation '
                                            f'buttons but no Xbox atlas replaces them'):
                continue
            name, atlas = redrawn[offset]
            masks = glyph_masks(build, name, atlas)
            if not check(masks is not None and len(masks) == len(order), f'{build}: no glyph masks for {name}'):
                continue
            width, height = atlas_size(mm, atlas)
            for (codepoint, label), (x, y, mask_width, mask_height) in zip(order, masks):
                u0, v0, u1, v1 = record(font, codepoint)
                cell = (math.ceil(u0 * width) + 1, math.ceil(v0 * height) + 1, math.ceil(u1 * width) - 1,
                        math.ceil(v1 * height) - 1)
                check(label == XBOX[codepoint], f'U+{codepoint:04X} becomes {label}, not {XBOX[codepoint]}')
                check((x, y, x + mask_width, y + mask_height) == cell,
                      f'{build} {name}: the {label} mask {(x, y, x + mask_width, y + mask_height)} is not the cell '
                      f'{cell} U+{codepoint:04X} samples')
            check({codepoint for codepoint, _ in order} == set(buttons),
                  f'{build} {name}: the redrawn glyphs are not the font\'s button glyphs')
        mm.close()
    if not redrawn:
        # CUSA00062 1.00's fonts have no button glyphs: every language shares the Latin atlases' hand-measured cells.
        check(set(by_script) == {'latin'} and set(languages.values()) == {'latin'},
              f'{build}: a language draws with a font the hand-measured cells do not cover')
        data = (ROOT / 'launcher' / 'controller_prompts_glyphs_data.c').read_text(encoding='utf-8')
        hand = re.findall(r'\{"' + re.escape(build) + r'", "(font\d)", ', data)
        check(hand == ['font0', 'font1', 'font2'], f'{build}: the Latin atlases are not all redrawn')
    for language, wanted in sorted(languages.items()):
        drawn = by_script.get(wanted, [])
        check(drawn, f'{build}: no {wanted} font for {language}')
        names = [redrawn[offset][0] for offset in drawn if offset in redrawn] or ['font0', 'font1', 'font2']
        print(f'{build} {language:5}: PlayStation = original glyphs; Xbox = redrawn in {", ".join(names)}')


def check_install_ignores_language():
    """The redraw is chosen by build and prompt style alone, and the PlayStation style changes nothing."""
    source = (ROOT / 'launcher' / 'controller_prompts.cpp').read_text(encoding='utf-8')
    install = source.split('extern "C" int aln_controller_prompts_install(', 1)[1]
    check('language' not in source.lower(), 'the Xbox prompts depend on the language')
    check(install.split(')', 1)[0].count(',') == 4, 'the prompt installer takes more than the build and the style')
    check(install.find('style != ALN_PROMPTS_XBOX') < install.find('BuildGlyphPatches('),
          'the PlayStation style reaches the glyph redraw')
    check('std::strcmp(atlas.build, build_key) != 0' in source, 'the glyph atlases are not chosen by build')


def main():
    check_install_ignores_language()
    for build in LANGUAGES:
        check_build(build)
    for text in failures:
        print(f'FAILED: {text}')
    if not failures:
        print('prompt language tests passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
