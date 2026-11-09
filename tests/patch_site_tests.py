"""Checks the game patch sites in launcher/game_profile.cpp against the prepared executables.

A build whose prepared folder is missing is skipped. Usage: python tests/patch_site_tests.py
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import prepare_image  # noqa: E402
from ps4_module import read_module  # noqa: E402

failures = []
HEAP_LITERALS = (0x3bb, 0x47b)  # the startup functions that set the three GPU heap sizes, in both builds
WAS_PRESSED = bytes.fromhex('554889e541565348 89fb4189f6488b03ff5028 84c074 0f4889df4489f6 e8'.replace(' ', ''))
WAS_PRESSED_TAIL = bytes.fromhex('3401eb0231c05b415e5dc3')
UPDATE = bytes.fromhex('554889e54156534889fb8b7b1085ff78') + b'\0' + bytes.fromhex('c5fc104318c5fc104b38c5fc105358c5fc105b70')
DISPATCH = bytes.fromhex('89f083fe0f7603 31c0c3488d0d'.replace(' ', ''))
CROSS_NOW = bytes.fromhex('8a47192440c0e806c3')
CROSS_BEFORE = bytes.fromhex('8a87910000002440c0e806c3')
CROSS = 13


def check(condition, text):
    if not condition:
        failures.append(text)


def sites(source, name):
    body = re.search(rf'{name}\[\] = \{{(.*?)\n\}};', source, re.S).group(1)
    found = []
    for offset, length, opcode, value, height, budget in re.findall(
            r'\{(0x[0-9a-f]+), (\d+), \{([^}]*)\}, (\w+), (\d), (\d)\}', body):
        found.append((int(offset, 16), int(length), bytes(int(b, 16) for b in opcode.split(',')), int(value, 0), int(height),
                      int(budget)))
    return found


def frame_rate(source, name):
    match = re.search(rf'{name} = \{{(0x[0-9a-f]+), \{{([^}}]*)\}}, \{{([^}}]*)\}}\}};', source)
    to_bytes = lambda text: bytes(int(b, 16) for b in text.split(','))
    return int(match.group(1), 16), to_bytes(match.group(2)), to_bytes(match.group(3))


def skip_intro_site(source, build):
    match = re.search(r'\{"' + build + r'", (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+)\}', source)
    return tuple(int(group, 16) for group in match.groups()) if match else None


def count_pattern(image, prefix, tail, gap):
    return [m.start() for m in re.finditer(re.escape(prefix) + b'.' * gap + re.escape(tail), bytes(image), re.S)]


def dword(image, at):
    return int.from_bytes(image[at:at + 4], 'little', signed=True)


def cross_entry(image, function):
    if bytes(image[function:function + len(DISPATCH)]) != DISPATCH:
        return -1
    table = function + 17 + dword(image, function + 13)
    return table + dword(image, table + CROSS * 4)


def check_skip_intro(label, image, relocs, skip_source, build, spans):
    site = skip_intro_site(skip_source, build)
    check(site is not None, f'{label}: no Skip Intro site')
    if not site:
        return
    is_down, was_pressed, update, slot = site
    check(bytes(image[was_pressed:was_pressed + len(WAS_PRESSED)]) == WAS_PRESSED, f'{label}: wasPressed differs')
    check(bytes(image[was_pressed + 34:was_pressed + 34 + len(WAS_PRESSED_TAIL)]) == WAS_PRESSED_TAIL,
          f'{label}: wasPressed tail differs')
    helper = was_pressed + 34 + dword(image, was_pressed + 30)
    entry = cross_entry(image, helper)
    check(bytes(image[entry:entry + len(CROSS_BEFORE)]) == CROSS_BEFORE, f'{label}: previous-state Cross test differs')
    entry = cross_entry(image, is_down)
    check(bytes(image[entry:entry + len(CROSS_NOW)]) == CROSS_NOW, f'{label}: isDown Cross test differs')
    slots = {target: value for target, kind, value, addend in relocs if kind == 0}
    check(slots.get(slot) == is_down and slots.get(slot + 0x10) == was_pressed, f'{label}: isDown is not the vtable slot')
    mask = bytes(image[update:update + len(UPDATE)])
    check(mask[:16] == UPDATE[:16] and mask[17:] == UPDATE[17:], f'{label}: update differs')
    whole = bytes(image)
    check(len(count_pattern(image, WAS_PRESSED, WAS_PRESSED_TAIL, 4)) == 1, f'{label}: wasPressed is not unique')
    check(len(re.findall(re.escape(UPDATE[:16]) + b'.' + re.escape(UPDATE[17:]), whole, re.S)) == 1,
          f'{label}: update is not unique')
    spans.append((is_down, is_down + 5))
    spans.append((update, update + 10))


def check_build(label, folder, source, skip_source, build, resolution_name, frame_name):
    eboot = ROOT / 'game' / 'prepared' / folder / 'eboot.bin'
    if not eboot.exists():
        print(f'skipped {label}: {eboot} not found')
        return
    image, _, relocs = prepare_image.map_eboot(read_module(eboot))[:3]
    spans = []
    resolution = sites(source, resolution_name)
    check(len(resolution) == 4, f'{label}: expected two heap sites and the width and height sites')
    for offset, length, opcode, value, height, budget in resolution:
        check(bytes(image[offset:offset + length]) == opcode, f'{label}: opcode differs at {offset:#x}')
        check(int.from_bytes(image[offset + length:offset + length + 4], 'little') == value,
              f'{label}: original immediate differs at {offset:#x}')
        check(value == (0x40000000 if budget else 1080 if height else 1920), f'{label}: {offset:#x} is not the native value')
        spans.append((offset, offset + length + 4))
    check(sum(1 for site in resolution if site[5]) == 2, f'{label}: expected both startup functions')
    check({site[0] for site in resolution if site[5]} == set(HEAP_LITERALS), f'{label}: heap sites are not the startup literals')
    offset, original, replacement = frame_rate(source, frame_name)
    check(bytes(image[offset:offset + 4]) == original, f'{label}: engine_set_fps entry differs at {offset:#x}')
    check(replacement == bytes.fromhex('31c0c390'), f'{label}: replacement is not xor eax,eax; ret')
    spans.append((offset, offset + 4))
    check_skip_intro(label, image, relocs, skip_source, build, spans)
    spans.sort()
    check(all(a[1] <= b[0] for a, b in zip(spans, spans[1:])), f'{label}: patch sites overlap')
    for at in HEAP_LITERALS:
        check(bytes(image[at:at + 24]) == bytes.fromhex('48c7458800000040' '48c7459000000046' '48c7459800000014'),
              f'{label}: the startup GPU heap sizes moved at {at:#x}')


def main():
    source = (ROOT / 'launcher' / 'game_profile.cpp').read_text(encoding='utf-8')
    skip_source = (ROOT / 'src' / 'skip_intro.c').read_text(encoding='utf-8')
    check_build('CUSA00062 1.00', 'CUSA00062-1.00', source, skip_source, 'CUSA00062-1.00', 'kEurope100ResolutionSites',
                'kEurope100FrameRate')
    check_build('CUSA04430 1.00', 'CUSA04430-1.00', source, skip_source, 'CUSA04430-1.00', 'kAsia100ResolutionSites',
                'kAsia100FrameRate')
    for text in failures:
        print(f'FAILED: {text}')
    if not failures:
        print('patch site tests passed')
    return 1 if failures else 0


sys.exit(main())
