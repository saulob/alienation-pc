"""Checks every guest patch site of every supported build against its prepared executable and stream.dat: the
resolution, frame rate, Skip Intro, Exit Game and menu button sites, and the stream.dat blocks the Exit Game bytecode
and the Xbox prompts rewrite. No two sites of one build may overlap.

A build whose prepared folder is missing is skipped. Usage: python tests/patch_site_tests.py
"""
from pathlib import Path
import re
import struct
import sys
import zlib

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import feature_parity  # noqa: E402
import prepare_image  # noqa: E402
from ps4_module import read_module  # noqa: E402

failures = []
HEAP_LITERALS = (0x3bb, 0x47b)  # the startup functions that set the three GPU heap sizes, in every build
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


EXIT_APP = re.compile(rb'\x48\x8d\x05....\x48\x8b\x38\x48\x8b\x07\xff\x60\x28', re.S)
TEXT_COPY = bytes.fromhex('4c8d65c8488d73204d8dbeb00000008b43104c89e7488d540320e8')
BOX_NEW_TEXT = bytes.fromhex('488d9dd8feffff4889dfe8')
BOX_OFFSET_STORE = bytes.fromhex('4189475c41894f6041c6474801')
LANGUAGE_NAME = re.compile(rb'\x48\x63\x05....\x48\x8d\x0d....\x48\xc1\xe0\x04\x48\x8d\x7c\x08\x08\xe9', re.S)
DISPLAY_DEFAULTS = bytes.fromhex('48c70700000040' '48c7470800000008' '48c7471000000020')
REGISTER_NATIVE = re.compile(rb'\x48\x8d\x35(....)\x48\x8d\x0d(....)\x48\x89\xda\xe8', re.S)


def rip(image, operand):
    return operand + 4 + dword(image, operand)


def table_row(source, build, count):
    match = re.search(r'\{"' + re.escape(build) + r'",\s*' + r',\s*'.join([r'(0x[0-9a-f]+)u?'] * count) + r'\}', source)
    return tuple(int(group, 16) for group in match.groups()) if match else None


def registered_natives(image):
    """function -> registered name, for every `lea rsi, fn; lea rcx, name; mov rdx, rbx; call` in the code."""
    found = {}
    for match in REGISTER_NATIVE.finditer(bytes(image[:0x1ea0000])):
        at = match.start()
        name = rip(image, at + 10)
        end = bytes(image).find(b'\0', name, name + 128)
        if end > 0:
            found[rip(image, at + 3)] = bytes(image[name:end]).decode('latin-1')
    return found


def check_exit_game(label, image, build, spans):
    site = table_row((ROOT / 'src' / 'exit_game.c').read_text(encoding='utf-8'), build, 10)
    check(site is not None, f'{label}: no Exit Game hook site')
    if not site:
        return
    exit_app, application, text_copy, text_target, box_text, box_copy, box_store, language, index, table = site
    whole = bytes(image)
    check(EXIT_APP.fullmatch(whole[exit_app:exit_app + 16]) is not None and rip(image, exit_app + 3) == application,
          f'{label}: exit_game_application differs')
    check(registered_natives(image).get(exit_app) == 'exit_game_application', f'{label}: {exit_app:#x} is not the native')
    check(whole[text_copy - 26:text_copy + 1] == TEXT_COPY and rip(image, text_copy + 1) == text_target,
          f'{label}: the hud text copy differs')
    check(whole[box_text - 10:box_text + 1] == BOX_NEW_TEXT and rip(image, box_text + 1) == box_copy,
          f'{label}: the layout box text parameter differs')
    check(whole[box_store:box_store + 13] == BOX_OFFSET_STORE and box_store < box_text < box_store + 0x1000,
          f'{label}: the layout box offset store differs')
    check(LANGUAGE_NAME.fullmatch(whole[language:language + 24]) is not None and rip(image, language + 3) == index and
          rip(image, language + 10) == table and index > table, f'{label}: the language getter differs')
    for pattern, at in ((TEXT_COPY, text_copy - 26), (BOX_OFFSET_STORE, box_store)):
        check(whole.count(pattern) == 1, f'{label}: {at:#x} is not unique')
    spans.extend([(exit_app + 7, exit_app + 13), (text_copy, text_copy + 5), (box_text, box_text + 5)])


def check_menu_buttons(label, image, build, spans):
    site = table_row((ROOT / 'launcher' / 'menu_buttons.c').read_text(encoding='utf-8'), build, 4)
    natives = registered_natives(image)
    names = set(natives.values())
    if site is None:
        check('engine_get_confirm_button' not in names and b'engine_get_confirm_button\0' not in bytes(image),
              f'{label}: the executable asks the engine for the Confirm button but has no menu button site')
        return
    confirm, cancel, cross, circle = site
    whole = bytes(image)
    check(whole[cross:cross + 6] == b'cross\0' and whole[circle:circle + 7] == b'circle\0', f'{label}: button names moved')
    for at, name in ((confirm, 'engine_get_confirm_button'), (cancel, 'engine_get_cancel_button')):
        check(whole[at:at + 3] == b'\x48\x8d\x35' and rip(image, at + 3) in (cross, circle), f'{label}: {name} differs')
        check(natives.get(at - 0x1b) == name, f'{label}: {at - 0x1b:#x} is not {name}')
        spans.append((at + 3, at + 7))
    check({rip(image, confirm + 3), rip(image, cancel + 3)} == {cross, circle}, f'{label}: Confirm and Cancel are the same')


STATUS_HOOKS = (('auth_code', 8), ('lookup_language', 5), ('lookup_default', 5))
# The network status box: its state names, the localization keys the box shows and the natives its script patch uses.
STATUS_SCRIPT = (b'CONNECTED', b'LOGIN', b'GENERIC_ERROR', b'WAITING_PSN', b'NETWORK_OFFLINE', b'USER_OFFLINE', b'vec4',
                 b'server_request_auth_code', b'layout_box_set_text_visible', b'layout_box_set_text_color',
                 b'layout_box_set_visible', b'is_string', b'hide_type_from_compiler', b'Network_Status',
                 b'menu__network__no_matching',
                 b'menu__network__login_wait_prompt', b'menu__network__connected', b'menu__network__error_general',
                 b'menu__network__platform_offline', b'menu__network__user_offline')


def source_pattern(source, name):
    body = re.search(r'static const short ' + name + r'\[\] = \{(.*?)\};', source, re.S).group(1)
    return [None if token.strip() == 'ANY' else int(token, 16) for token in body.split(',') if token.strip()]


def check_online_status(label, image, build, spans):
    """The status hooks: server_request_auth_code's prologue and the two lookups of the helper str_local calls."""
    source = (ROOT / 'src' / 'online_status.c').read_text(encoding='utf-8')
    row = re.search(r'\{"' + re.escape(build) + r'", (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+)\}', source)
    check(row is not None, f'{label}: no online status site')
    if not row:
        return
    site = dict(zip(('auth_code', 'lookup_language', 'lookup_default'), (int(v, 16) for v in row.groups())))
    natives = registered_natives(image)
    whole = bytes(image)
    check(natives.get(site['auth_code']) == 'server_request_auth_code', f'{label}: {site["auth_code"]:#x} is not '
          'server_request_auth_code')
    for at, name in ((site['auth_code'], 'kAuthCode'), (site['lookup_language'] - 11, 'kLookup')):
        pattern = source_pattern(source, name)
        check(all(p is None or whole[at + i] == p for i, p in enumerate(pattern)), f'{label}: {name} differs at {at:#x}')
    check(site['lookup_default'] == site['lookup_language'] + 15, f'{label}: the lookups are not one call apart')
    str_local = next((at for at, name in natives.items() if name == 'str_local'), None)
    localize = str_local and whole.find(bytes.fromhex('baffffffff4c89f7e8'), str_local, str_local + 0x60)
    helper = rip(image, localize + 9) if localize and localize > 0 else None
    check(helper is not None and helper < site['lookup_language'] < helper + 0x400,
          f'{label}: the lookups are not in the helper str_local calls')
    for key, length in STATUS_HOOKS:
        spans.append((site[key], site[key] + length))


def check_status_script(label, code):
    """The status machine still speaks what the script patch and the box texts rely on."""
    for text in STATUS_SCRIPT:
        check(b'\0' + text + b'\0' in code, f'{label}: the status script has no {text.decode()}')


def check_display_defaults(label, image, resolution):
    """The width and height sites sit in the display configuration constructor and are the image's only such stores."""
    height = next(site[0] for site in resolution if not site[5] and site[4])
    ctor = height - 0x3a
    check(bytes(image[ctor:ctor + len(DISPLAY_DEFAULTS)]) == DISPLAY_DEFAULTS, f'{label}: {ctor:#x} is not the display ctor')
    for store in (bytes.fromhex('c7473438040000'), bytes.fromhex('c7473080070000')):
        check(bytes(image).count(store) == 1, f'{label}: the display size store {store.hex()} is not unique')


def check_build(label, folder, source, skip_source, build, resolution_name, frame_name):
    eboot = ROOT / 'game' / 'prepared' / folder / 'eboot.bin'
    if not eboot.exists():
        print(f'skipped {label}: {eboot} not found')
        return
    module = read_module(eboot)
    check(module['sha256'] == re.search(r'"' + re.escape(build) + r'",\s*"([0-9a-f]{64})"', source).group(1),
          f'{label}: the prepared eboot.bin is not the build\'s')
    image, _, relocs = prepare_image.map_eboot(module)[:3]
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
    check(registered_natives(image).get(offset) == 'engine_set_fps', f'{label}: {offset:#x} is not engine_set_fps')
    check_display_defaults(label, image, resolution)
    check_skip_intro(label, image, relocs, skip_source, build, spans)
    check_exit_game(label, image, build, spans)
    check_menu_buttons(label, image, build, spans)
    check_online_status(label, image, build, spans)
    spans.sort()
    check(all(a[1] <= b[0] for a, b in zip(spans, spans[1:])), f'{label}: patch sites overlap')
    for at in HEAP_LITERALS:
        check(bytes(image[at:at + 24]) == bytes.fromhex('48c7458800000040' '48c7459000000046' '48c7459800000014'),
              f'{label}: the startup GPU heap sizes moved at {at:#x}')
    check_stream(label, folder, build)


def block_header(stream, offset):
    stream.seek(offset)
    return struct.unpack('<6I', stream.read(24))


def payload_crc(stream, offset, payload_offset, zsize):
    stream.seek(offset + payload_offset)
    return zlib.crc32(stream.read(zsize)) & 0xFFFFFFFF


def check_local_coop(label, build, stream, offset, zsize, dsize):
    """Local co-op is what the build's own menus offer: the join message exists only where the parity table says so."""
    stream.seek(offset + 0x28)
    payload = stream.read(zsize)
    code = payload if zsize == dsize else zlib.decompress(payload)
    check_status_script(label, code)
    offered = b'menu__missions__squad_join_local' in code and b'local_user_join' in code
    check(offered == (feature_parity.PARITY[build]['Local co-op'][0] == feature_parity.SUPPORTED),
          f'{label}: the menus {"offer" if offered else "do not offer"} local co-op, unlike the parity table')


def check_stream(label, folder, build):
    """Every stream.dat block a build's patches rewrite is the build's own original one."""
    path = ROOT / 'game' / 'prepared' / folder / 'stream.dat'
    if not path.exists():
        print(f'skipped {label} stream.dat: {path} not found')
        return
    script = (ROOT / 'launcher' / 'exit_game_script.c').read_text(encoding='utf-8')
    row = re.search(r'\{"' + re.escape(build) + r'", (0x[0-9a-f]+)u, (0x[0-9a-f]+)u, (0x[0-9a-f]+)u, (0x[0-9a-f]+)u, '
                    r'(0x[0-9a-f]+)u', script)
    blocks = []
    with path.open('rb') as stream:
        check(row is not None, f'{label}: no Exit Game bytecode site')
        if row:
            offset, zsize, dsize, units, crc = (int(group, 16) for group in row.groups())
            check(block_header(stream, offset) == (offset // 64, zsize, dsize, units, offset // 64, 0x53544144),
                  f'{label}: the Stated bytecode block differs')
            check(payload_crc(stream, offset, 0x28, zsize) == crc, f'{label}: the Stated bytecode CRC differs')
            check_local_coop(label, build, stream, offset, zsize, dsize)
            blocks.append((offset, units))
        data = (ROOT / 'launcher' / 'controller_prompts_data.c').read_text(encoding='utf-8')
        glyphs = (ROOT / 'launcher' / 'controller_prompts_glyphs_data.c').read_text(encoding='utf-8')
        prompt_rows = re.findall(r'\{"' + re.escape(build) + r'", "(\w+)", (0x[0-9a-f]+), (\d+)u, (\d+)u, (0x[0-9a-f]+)u, '
                                 r'(\d+)u, \d+u, (0x[0-9a-f]+u|\d+u)', data)
        glyph_rows = re.findall(r'\{"' + re.escape(build) + r'", "(\w+)", (0x[0-9a-f]+), (\d+)u, (\d+)u, (0x[0-9a-f]+)u, '
                                r'(\d+)u, k_\w+_masks', glyphs)
        # The three Latin fonts and the icons; CUSA04430 and 1.06 add their Korean, Japanese and two Chinese fonts.
        atlases = 4 if build == 'CUSA00062-1.00' else 8
        check(len(prompt_rows) == 9 and len(glyph_rows) == atlases, f'{label}: {len(prompt_rows)} prompt and '
              f'{len(glyph_rows)} glyph rows (expected 9 and {atlases})')
        for name, offset, zsize, dsize, crc, units, last in prompt_rows + [row + ('',) for row in glyph_rows]:
            offset, zsize, dsize, crc, units = int(offset, 16), int(zsize), int(dsize), int(crc, 16), int(units)
            kind = block_header(stream, offset)[5]
            payload = {0x54455820: 40, 0x53434E45: 0x94, 0x53435249: 0x30}.get(kind, 0)
            check(payload and block_header(stream, offset)[:5] == (offset // 64, zsize, dsize, units, offset // 64),
                  f'{label}: the {name} block differs')
            check(payload and payload_crc(stream, offset, payload, zsize) == crc, f'{label}: the {name} CRC differs')
            blocks.append((offset, units))
        fixes = re.findall(r'\{"' + re.escape(build) + r'", (0x[0-9a-f]+)u, (\d+)u, (\d+)u, (\d+)u, (0x[0-9a-f]+)u, \d+u\}',
                           (ROOT / 'launcher' / 'text_fix.c').read_text(encoding='utf-8'))
        check(len(fixes) == (2 if build == 'CUSA04430-1.00' else 0), f'{label}: {len(fixes)} text fixes')
        for offset, zsize, dsize, units, crc in fixes:
            offset, zsize, dsize, units, crc = int(offset, 16), int(zsize), int(dsize), int(units), int(crc, 16)
            check(block_header(stream, offset) == (offset // 64, zsize, dsize, units, offset // 64, 0x52415744),
                  f'{label}: the text fix block at {offset:#x} differs')
            check(payload_crc(stream, offset, 0x30, zsize) == crc, f'{label}: the text fix CRC at {offset:#x} differs')
            blocks.append((offset, units))
    blocks.sort()
    check(all(a[0] + a[1] * 64 <= b[0] for a, b in zip(blocks, blocks[1:])), f'{label}: stream.dat patch blocks overlap')


def main():
    source = (ROOT / 'launcher' / 'game_profile.cpp').read_text(encoding='utf-8')
    skip_source = (ROOT / 'src' / 'skip_intro.c').read_text(encoding='utf-8')
    check_build('CUSA00062 1.00', 'CUSA00062-1.00', source, skip_source, 'CUSA00062-1.00', 'kEurope100ResolutionSites',
                'kEurope100FrameRate')
    check_build('CUSA00062 1.06', 'CUSA00062-1.06', source, skip_source, 'CUSA00062-1.06', 'kEurope106ResolutionSites',
                'kEurope106FrameRate')
    check_build('CUSA04430 1.00', 'CUSA04430-1.00', source, skip_source, 'CUSA04430-1.00', 'kAsia100ResolutionSites',
                'kAsia100FrameRate')
    for text in failures:
        print(f'FAILED: {text}')
    if not failures:
        print('patch site tests passed')
    return 1 if failures else 0


sys.exit(main())
