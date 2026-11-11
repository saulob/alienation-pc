"""The feature parity contract: every port feature's status in every supported build and package form, and how strongly
that status is verified.

Status
  SUPPORTED                the feature works on this build through its own code or data.
  INTENTIONALLY DIFFERENT  the build behaves otherwise on purpose; the note says how.
  UNAVAILABLE              not offered; the note is the technical reason.
Verification
  STATICALLY VERIFIED             the sites and data are checked byte for byte against the real executables and
                                  stream.dat (tests/patch_site_tests.py); nothing is executed.
  HEADLESS SEMANTICALLY VERIFIED  the behaviour is executed or derived without the game, on the real game data where it
                                  exists (the note names the test).
  MANUAL TEST REQUIRED            only playing the game can show it.
No status is final until the manual test matrix passes in game.

tests/feature_parity_tests.py checks this table against the build profiles, the patch tables, the tests and
docs/GAME_BUILDS.md. Usage: python scripts/feature_parity.py   prints the docs tables
"""
SUPPORTED = 'SUPPORTED'
DIFFERENT = 'INTENTIONALLY DIFFERENT'
UNAVAILABLE = 'UNAVAILABLE'
STATIC = 'STATICALLY VERIFIED'
HEADLESS = 'HEADLESS SEMANTICALLY VERIFIED'
MANUAL = 'MANUAL TEST REQUIRED'

# The table each guest patch feature needs the build's key (or its sites) in.
GUEST_PATCHES = {
    '60 FPS': 'frame rate',
    'Render Resolution': 'resolution',
    'Aspect Ratio': 'resolution',
    'True Ultrawide': 'resolution',
    'GPU heap scaling': 'heap',
    'Skip Intro': 'src/skip_intro.c',
    'Exit Game: Options': 'src/exit_game.c launcher/exit_game_script.c',
    'Exit Game: Select Campaign': 'src/exit_game.c launcher/exit_game_script.c',
    'Xbox prompts': 'launcher/controller_prompts_data.c launcher/controller_prompts_glyphs_data.c',
    'Asian Menu Buttons': 'launcher/menu_buttons.c',
}

FEATURES = (
    'Boot', 'Save', 'Trophies', 'Audio', 'Controller support', 'PlayStation Original prompts', 'Xbox prompts',
    'Xbox prompts in every language', 'Asian Menu Buttons', 'Skip Intro', 'Exit Game: Options',
    'Exit Game: Select Campaign', '60 FPS', 'Render Resolution', 'Aspect Ratio', 'True Ultrawide', 'GPU heap scaling',
    'SMAA', 'FSR', 'Texture filtering', 'Online', 'Matching2', 'P2P', 'Install Game', 'Install Update', 'Remove Update',
    'Build diagnostics', 'Bug reports',
)

# Features whose note is shown even when SUPPORTED: the notes that differ between builds.
NOTED = ('Save', 'Xbox prompts', 'Xbox prompts in every language', 'Asian Menu Buttons', 'Install Game',
         'Install Update', 'Remove Update')

_RUNTIME = 'runtime and renderer, no guest patch'
_EXIT_MENUS = 'tests/exit_game_menu_tests.c runs the real menu bytecode'
_INSTALL = 'tests/game_install_tests.cpp'


def _common(save, prompts, languages, menu_buttons, install_update, remove_update, online):
    return {
        'Boot': (SUPPORTED, MANUAL, 'boot image of this build'),
        'Save': (SUPPORTED, MANUAL, save),
        'Trophies': (SUPPORTED, MANUAL, 'NPWR09388_00, one trophy set for every build'),
        'Audio': (SUPPORTED, MANUAL, _RUNTIME),
        'Controller support': (SUPPORTED, MANUAL, _RUNTIME),
        'PlayStation Original prompts': (SUPPORTED, STATIC, 'the game\'s own assets, untouched'),
        'Xbox prompts': (SUPPORTED, STATIC, prompts),
        'Xbox prompts in every language': (SUPPORTED, HEADLESS, languages),
        'Asian Menu Buttons': menu_buttons,
        'Skip Intro': (SUPPORTED, STATIC, 'Controller_Orbis hooks'),
        'Exit Game: Options': (SUPPORTED, HEADLESS, 'last of 6 rows; ' + _EXIT_MENUS),
        'Exit Game: Select Campaign': (SUPPORTED, HEADLESS, 'after LOAD; ' + _EXIT_MENUS),
        '60 FPS': (SUPPORTED, STATIC, 'engine_set_fps binding'),
        'Render Resolution': (SUPPORTED, STATIC, 'display size sites'),
        'Aspect Ratio': (SUPPORTED, STATIC, 'render size + VideoOut status'),
        'True Ultrawide': (SUPPORTED, STATIC, '21:9 and 32:9 render targets'),
        'GPU heap scaling': (SUPPORTED, STATIC, 'both startup heap literals'),
        'SMAA': (SUPPORTED, MANUAL, _RUNTIME),
        'FSR': (SUPPORTED, MANUAL, _RUNTIME),
        'Texture filtering': (SUPPORTED, MANUAL, _RUNTIME),
        'Online': (SUPPORTED, MANUAL, online),
        'Matching2': (SUPPORTED, MANUAL, online),
        'P2P': (SUPPORTED, MANUAL, online),
        'Install Game': (SUPPORTED, HEADLESS, 'from its base package; ' + _INSTALL),
        'Install Update': install_update,
        'Remove Update': remove_update,
        'Build diagnostics': (SUPPORTED, STATIC, 'identity, patch and STOP lines in the log'),
        'Bug reports': (SUPPORTED, STATIC, 'build key in the report'),
    }


_LATIN = '15 languages, Latin and Cyrillic fonts'
_ASIAN = '18 languages: 15 on the Latin and Cyrillic fonts, kr, jp, cn on their own fonts'
_MENU_ON = 'Off: Cross confirms, Circle cancels; On: the reverse, through the engine getters'

PARITY = {
    'CUSA00062-1.00': _common(
        'user\\savedata\\CUSA00062', '7 textures, 2 scenes, 4 glyph atlases',
        _LATIN + ' share the 3 redrawn atlases; tests/prompt_language_tests.py',
        (UNAVAILABLE, STATIC, 'Cross always confirms: the menu scripts compare with Cross and ask no engine getter'),
        (SUPPORTED, HEADLESS, 'base of the 1.06 update; ' + _INSTALL),
        (UNAVAILABLE, HEADLESS, 'nothing to remove: the base; removing 1.06 returns here; ' + _INSTALL),
        'lobby key CUSA00062-1.00'),
    'CUSA00062-1.06': _common(
        'user\\savedata\\CUSA00062, shared with 1.00', '7 textures, 2 scenes, 8 glyph atlases',
        _ASIAN + '; every font redrawn; tests/prompt_language_tests.py',
        (SUPPORTED, HEADLESS, _MENU_ON + '; native Off; tests/menu_buttons_tests.c'),
        (SUPPORTED, HEADLESS, 'laid over CUSA00062 1.00; ' + _INSTALL),
        (SUPPORTED, HEADLESS, 'official layers only; returns to 1.00; ' + _INSTALL),
        'lobby key CUSA00062-1.06; never meets 1.00'),
    'CUSA04430-1.00': _common(
        'user\\savedata\\CUSA04430', '7 textures, 2 scenes, 8 glyph atlases',
        _ASIAN + '; every font redrawn; tests/prompt_language_tests.py',
        (SUPPORTED, HEADLESS, _MENU_ON + '; native On; tests/menu_buttons_tests.c'),
        (UNAVAILABLE, STATIC, 'no CUSA04430 update is known; the profile has no update build'),
        (UNAVAILABLE, STATIC, 'no CUSA04430 update is known'),
        'lobby key CUSA04430-1.00'),
}

# Package forms that are not builds of their own: each runs as its build, with the build's executable as the proof.
PACKAGE_FORMS = {
    'DUPLEX': {
        'build': 'CUSA00062-1.06',
        'alias': 'single-package repack',
        'eboot_sha256': 'c65c18865691d7288055cae70af983aa86898d59936976ef12f5eb033eda7243',
        'differs': {
            'Install Game': (SUPPORTED, HEADLESS, 'as CUSA00062 1.06, executable checked; ' + _INSTALL),
            'Install Update': (DIFFERENT, HEADLESS, 'refused before any change: it already contains 1.06; ' + _INSTALL),
            'Remove Update': (UNAVAILABLE, HEADLESS,
                              'no update layer: a complete package cannot become 1.00; ' + _INSTALL),
        },
    },
}

# What a build runs as, and how its files got there: DUPLEX runs as 1.06 but was never an update laid over 1.00.
IDENTITY = {
    'CUSA00062-1.00': ('CUSA00062-1.00', 'base package (gd, APP_VER 01.00)', 'CUSA00062-1.00'),
    'CUSA00062-1.06': ('CUSA00062-1.06', 'base 1.00 + official update layers (gp, APP_VER 01.06), removable',
                       'CUSA00062-1.06'),
    'DUPLEX': ('CUSA00062-1.06', 'single-package repack (gd, APP_VER 01.00, VERSION 01.06), no update layer',
               'CUSA00062-1.06'),
    'CUSA04430-1.00': ('CUSA04430-1.00', 'base package (gd, APP_VER 01.00)', 'CUSA04430-1.00'),
}

COLUMNS = (('CUSA00062-1.00', 'CUSA00062 1.00 · EUR'), ('CUSA00062-1.06', 'CUSA00062 1.06 · EUR'),
           ('DUPLEX', 'DUPLEX (CUSA00062 1.06)'), ('CUSA04430-1.00', 'CUSA04430 1.00 · ASIA/KR'))


def status(column, feature):
    """(status, verification, note) of a feature in a build key or a package form."""
    if column in PACKAGE_FORMS:
        form = PACKAGE_FORMS[column]
        return form['differs'].get(feature, PARITY[form['build']][feature])
    return PARITY[column][feature]


def _row(title, cells):
    return f'| {title} | ' + ' | '.join(cells) + ' |'


def identity_table():
    lines = [_row('', [name for _, name in COLUMNS]), '|' + '---|' * (len(COLUMNS) + 1)]
    for index, title in enumerate(('Runtime build identity', 'Installation provenance', 'Online key')):
        lines.append(_row(title, [IDENTITY[key][index] for key, _ in COLUMNS]))
    return '\n'.join(lines)


def markdown_table():
    lines = [_row('Feature', [name for _, name in COLUMNS]), '|' + '---|' * (len(COLUMNS) + 1)]
    for feature in FEATURES:
        cells = []
        for key, _ in COLUMNS:
            state, level, note = status(key, feature)
            shown = state != SUPPORTED or feature in NOTED
            cells.append(f'{state} · {level}' + (f': {note}' if shown else ''))
        lines.append(_row(feature, cells))
    return '\n'.join(lines)


if __name__ == '__main__':
    print(identity_table())
    print()
    print(markdown_table())
