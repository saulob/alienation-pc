"""Checks the engine code that turns the VideoOut display size into its screen aspect, in every prepared executable.

The engine's Display_Control::GetScreen calls sceVideoOutGetResolutionStatus and divides pane_width by pane_height; the
port reports the render size there (AlnGpuConfig.render_width/height). A build whose prepared folder is missing is
skipped. Usage: python tests/display_aspect_tests.py
"""
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import prepare_image  # noqa: E402
from ps4_module import read_module  # noqa: E402

GET_RESOLUTION_STATUS = '6kPnj51T62Y'
CODE_END = 0x1780000
# mov rax,[rbp-0x40] (pane_width, pane_height); mov rcx,rax; shr rcx,32 ... vcvtsi2ss xmm0,xmm0,rax; vcvtsi2ss xmm1,xmm0,rcx; vdivss
PANE_LOAD = bytes.fromhex('488b45c0')
DIVIDE = bytes.fromhex('c4e1fa2ac0c4e1fa2ac9')
failures = []


def check(condition, text):
    if not condition:
        failures.append(text)


def check_build(label, folder):
    eboot = ROOT / 'game' / 'prepared' / folder / 'eboot.bin'
    if not eboot.exists():
        print(f'skipped {label}: {eboot} not found')
        return
    image, _, relocs, imported, _, _ = prepare_image.map_eboot(read_module(eboot))
    image = bytes(image)
    wanted = [i for i, key in enumerate(imported) if key[0] == GET_RESOLUTION_STATUS]
    check(len(wanted) == 1, f'{label}: sceVideoOutGetResolutionStatus is not imported once')
    slots = {t for t, kind, index, _ in relocs if kind != 0 and index in wanted}
    stubs = [m.start() for m in re.finditer(b'\xff\x25', image[:CODE_END])
             if m.start() + 6 + struct.unpack_from('<i', image, m.start() + 2)[0] in slots]
    check(len(stubs) == 1, f'{label}: expected one stub for the import, found {len(stubs)}')
    calls = [m.start() for m in re.finditer(b'\xe8', image[:CODE_END])
             if stubs and m.start() + 5 + struct.unpack_from('<i', image, m.start() + 1)[0] == stubs[0]]
    check(len(calls) == 2, f'{label}: expected the init and GetScreen call sites, found {len(calls)}')
    screen = [c for c in calls if image[c + 5:c + 9] == PANE_LOAD and DIVIDE in image[c + 9:c + 64]]
    check(len(screen) == 1, f'{label}: expected one pane width / pane height division after the call')


check_build('CUSA00062 1.00 EUR', 'CUSA00062-1.00')
check_build('CUSA00062 1.06 EUR', 'CUSA00062-1.06')
check_build('CUSA04430 1.00 ASIA/KR', 'CUSA04430-1.00')

if failures:
    print('\n'.join(failures))
    sys.exit(1)
print('display aspect tests passed')
