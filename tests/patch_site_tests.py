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


def check_build(label, folder, source, resolution_name, frame_name):
    eboot = ROOT / 'game' / 'prepared' / folder / 'eboot.bin'
    if not eboot.exists():
        print(f'skipped {label}: {eboot} not found')
        return
    image = prepare_image.map_eboot(read_module(eboot))[0]
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
    spans.sort()
    check(all(a[1] <= b[0] for a, b in zip(spans, spans[1:])), f'{label}: patch sites overlap')
    for at in HEAP_LITERALS:
        check(bytes(image[at:at + 24]) == bytes.fromhex('48c7458800000040' '48c7459000000046' '48c7459800000014'),
              f'{label}: the startup GPU heap sizes moved at {at:#x}')


def main():
    source = (ROOT / 'launcher' / 'game_profile.cpp').read_text(encoding='utf-8')
    check_build('CUSA00062 1.00', 'CUSA00062-1.00', source, 'kEurope100ResolutionSites', 'kEurope100FrameRate')
    check_build('CUSA04430 1.00', 'CUSA04430-1.00', source, 'kAsia100ResolutionSites', 'kAsia100FrameRate')
    for text in failures:
        print(f'FAILED: {text}')
    if not failures:
        print('patch site tests passed')
    return 1 if failures else 0


sys.exit(main())
