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

PLATFORM_FEATURES adds the platforms: Windows x64 (ZIP), Linux x86_64 (AppImage) and the Steam Deck, which runs the same
AppImage. EQUIVALENT is the same result through the platform's own mechanism. The Steam Deck column is what the Linux
build is expected to do there; no Steam Deck hardware test is claimed.

tests/feature_parity_tests.py checks these tables against the build profiles, the patch tables, the tests, the sources
and docs/GAME_BUILDS.md and docs/LINUX.md.
Usage: python scripts/feature_parity.py               prints the build tables
       python scripts/feature_parity.py --platforms   prints the platform table
       python scripts/feature_parity.py --summary     one line per platform feature (tools/build.sh --test)
"""
import sys
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
    'Online status': 'src/online_status.c',
}

FEATURES = (
    'Boot', 'Save', 'Trophies', 'Audio', 'Controller support', 'PlayStation Original prompts', 'Xbox prompts',
    'Xbox prompts in every language', 'Asian Menu Buttons', 'Skip Intro', 'Exit Game: Options',
    'Exit Game: Select Campaign', '60 FPS', 'Frame rate', 'Render Resolution', 'Aspect Ratio', 'True Ultrawide', 'GPU heap scaling',
    'SMAA', 'FSR', 'Texture filtering', 'Online', 'Friends', 'Online status', 'Matching2', 'P2P',
    'Sessions and invitations', 'Local co-op', 'Voice-Over Language text fix',
    'Install Game', 'Install Update', 'Remove Update',
    'Build diagnostics', 'Bug reports',
)

# Features whose note is shown even when SUPPORTED: the notes that differ between builds.
NOTED = ('Save', 'Xbox prompts', 'Xbox prompts in every language', 'Asian Menu Buttons', 'Local co-op',
         'Voice-Over Language text fix', 'Install Game', 'Install Update', 'Remove Update')

_RUNTIME = 'runtime and renderer, no guest patch'
_EXIT_MENUS = 'tests/exit_game_menu_tests.c runs the real menu bytecode'
_INSTALL = 'tests/game_install_tests.cpp'


_NO_LOCAL_COOP = ('the menus have no local co-op join, and the executable lacks engine natives the 1.06 co-op scripts call '
                  '(the 1.06 update added both); tests/patch_site_tests.py')


_TEXT_FIX_NONE = (UNAVAILABLE, STATIC, 'nothing to fix: this build\'s Voice-Over Language names are right')


def _common(save, prompts, languages, menu_buttons, install_update, remove_update, online, local_coop,
            text_fix=_TEXT_FIX_NONE):
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
        'Frame rate': (SUPPORTED, HEADLESS, 'presentation only, no guest patch; the flip-paced fixed-step timing is '
                       'checked per build by tests/patch_site_tests.py and modelled by tests/frame_pacing_tests.cpp'),
        'Render Resolution': (SUPPORTED, STATIC, 'display size sites'),
        'Aspect Ratio': (SUPPORTED, STATIC, 'render size + VideoOut status'),
        'True Ultrawide': (SUPPORTED, STATIC, '21:9 and 32:9 render targets'),
        'GPU heap scaling': (SUPPORTED, STATIC, 'both startup heap literals'),
        'SMAA': (SUPPORTED, MANUAL, _RUNTIME),
        'FSR': (SUPPORTED, MANUAL, _RUNTIME),
        'Texture filtering': (SUPPORTED, MANUAL, _RUNTIME),
        'Online': (SUPPORTED, MANUAL, online),
        'Friends': (SUPPORTED, HEADLESS, 'friend codes, presence and invitations on the launcher\'s Online page; '
                    'tests/online_view_tests.cpp and the lobby tests'),
        'Online status': (SUPPORTED, HEADLESS, 'Online Connecting/Connected/Unavailable/Off in the native box, 18 languages; tests/online_status_tests.c'),
        'Matching2': (SUPPORTED, MANUAL, online),
        'P2P': (SUPPORTED, MANUAL, online),
        'Sessions and invitations': (SUPPORTED, MANUAL, 'NP sessions for every squad, invitations, Join'),
        'Local co-op': local_coop,
        'Voice-Over Language text fix': text_fix,
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
        'lobby key CUSA00062-1.00', (UNAVAILABLE, STATIC, _NO_LOCAL_COOP)),
    'CUSA00062-1.06': _common(
        'user\\savedata\\CUSA00062, shared with 1.00', '7 textures, 2 scenes, 8 glyph atlases',
        _ASIAN + '; every font redrawn; tests/prompt_language_tests.py',
        (SUPPORTED, HEADLESS, _MENU_ON + '; native Off; tests/menu_buttons_tests.c'),
        (SUPPORTED, HEADLESS, 'laid over CUSA00062 1.00; ' + _INSTALL),
        (SUPPORTED, HEADLESS, 'official layers only; returns to 1.00; ' + _INSTALL),
        'lobby key CUSA00062-1.06; never meets 1.00',
        (SUPPORTED, MANUAL, '2 players confirmed in game, a save per user; 3-4 headless only; offline only; '
                    'tests/local_pad_tests.c')),
    'CUSA04430-1.00': _common(
        'user\\savedata\\CUSA04430', '7 textures, 2 scenes, 8 glyph atlases',
        _ASIAN + '; every font redrawn; tests/prompt_language_tests.py',
        (SUPPORTED, HEADLESS, _MENU_ON + '; native On; tests/menu_buttons_tests.c'),
        (UNAVAILABLE, STATIC, 'no CUSA04430 update is known; the profile has no update build'),
        (UNAVAILABLE, STATIC, 'no CUSA04430 update is known'),
        'lobby key CUSA04430-1.00', (UNAVAILABLE, STATIC, _NO_LOCAL_COOP),
        (SUPPORTED, HEADLESS, 'Portuguese (Brasil, Portugal) "Inglês" without the cut letter; tests/text_fix_tests.c')),
}

# Package forms that are not builds of their own: each runs as its build, with the build's executable as the proof.
PACKAGE_FORMS = {
    'COMPLETE': {
        'build': 'CUSA00062-1.06',
        'alias': 'complete package',
        'eboot_sha256': 'c65c18865691d7288055cae70af983aa86898d59936976ef12f5eb033eda7243',
        'differs': {
            'Install Game': (SUPPORTED, HEADLESS, 'as CUSA00062 1.06, executable checked; ' + _INSTALL),
            'Install Update': (DIFFERENT, HEADLESS, 'refused before any change: it already contains 1.06; ' + _INSTALL),
            'Remove Update': (UNAVAILABLE, HEADLESS,
                              'no update layer: a complete package cannot become 1.00; ' + _INSTALL),
        },
    },
}

# What a build runs as, and how its files got there: the complete package runs as 1.06 but was never an update laid over 1.00.
IDENTITY = {
    'CUSA00062-1.00': ('CUSA00062-1.00', 'base package (gd, APP_VER 01.00)', 'CUSA00062-1.00'),
    'CUSA00062-1.06': ('CUSA00062-1.06', 'base 1.00 + official update layers (gp, APP_VER 01.06), removable',
                       'CUSA00062-1.06'),
    'COMPLETE': ('CUSA00062-1.06', 'complete package (gd, APP_VER 01.00, VERSION 01.06), no update layer',
                 'CUSA00062-1.06'),
    'CUSA04430-1.00': ('CUSA04430-1.00', 'base package (gd, APP_VER 01.00)', 'CUSA04430-1.00'),
}

COLUMNS = (('CUSA00062-1.00', 'CUSA00062 1.00 · EUR'), ('CUSA00062-1.06', 'CUSA00062 1.06 · EUR'),
           ('COMPLETE', 'CUSA00062 1.06 · EUR (complete package)'), ('CUSA04430-1.00', 'CUSA04430 1.00 · ASIA/KR'))


WINDOWS, LINUX, DECK = 'Windows', 'Linux', 'Steam Deck'
PLATFORMS = (WINDOWS, LINUX, DECK)
EQUIVALENT = 'EQUIVALENT'
ALL_BUILDS = 'all'
_S, _E, _U = SUPPORTED, EQUIVALENT, UNAVAILABLE
_REPEATS = 'above 60 the latest game frame is shown again (the game still runs at 60)'
_GAMESCOPE = 'Gamescope paces presentation itself: rates above the display\'s refresh may not show'

# feature: (Windows, Linux, Steam Deck, builds, note). builds is ALL_BUILDS or the build feature whose SUPPORTED
# columns it follows. A note is required wherever a platform is not SUPPORTED.
PLATFORM_FEATURES = {
    'Launcher pages (Play, Display, Graphics, Gameplay, Controls, Online, Trophies, Bug Report, About)':
        (_S, _S, _S, ALL_BUILDS, 'the same launcher code'),
    'Show the launcher at start':
        (_S, _E, _E, ALL_BUILDS, 'Shift or --launcher; Linux: --launcher or the desktop entry\'s Show launcher action '
         '(Shift is read only on X11); Steam Deck Gaming Mode: --launcher in the Launch Options'),
    'Install game': (_S, _E, _E, 'Install Game', 'Windows file dialog; Linux: zenity or kdialog, else the launcher\'s '
                     'own chooser (mouse, keyboard, controller)'),
    'Install update': (_S, _S, _S, 'Install Update', 'official CUSA00062 1.06 layers over 1.00'),
    'Remove update': (_S, _S, _S, 'Remove Update', 'official layers only'),
    'Complete package form': (_S, _S, _S, ALL_BUILDS, 'runs as CUSA00062 1.06; an update over it is refused'),
    'Online Play': (_S, _S, _S, 'Online', 'one lobby protocol and online version on every platform'),
    'Friends and friend codes': (_S, _S, _S, 'Friends', 'the same lobby accounts'),
    'Invitations and Accept and play': (_S, _S, _S, 'Sessions and invitations', 'the same launcher online client'),
    'Matching2 rooms of 2 to 4 players': (_S, _S, _S, 'Matching2', 'full mesh of peers'),
    'P2P with relay fallback': (_S, _S, _S, 'P2P', 'direct UDP, STUN, UPnP, the lobby relay when direct fails'),
    'Cross-platform play': (_S, _S, _S, 'Online', 'Windows and Linux players of the same build meet in one room'),
    'Online status box': (_S, _S, _S, 'Online status', 'Online Off, Connecting, Connected, Unavailable'),
    'Local co-op': (_S, _S, _S, 'Local co-op', 'one to four controllers; only where the game has it'),
    'Frame rate (30, 60, 90, 120, 144, 165, 240, Unlimited)': (_S, _S, _S, 'Frame rate', _REPEATS + '; ' + _GAMESCOPE),
    'Frame counter, frame rate and render resolution in the title': (_S, _S, _S, ALL_BUILDS, 'the same overlay code'),
    'Vulkan renderer': (_S, _S, _S, ALL_BUILDS, 'the only released renderer: v0.1.0 requires Vulkan, with no renderer setting'),
    'Render resolution': (_S, _S, _S, 'Render Resolution', 'the same guest patch'),
    'Aspect ratio': (_S, _S, _S, 'Aspect Ratio', 'the same guest patch'),
    'True Ultrawide': (_S, _S, _S, 'True Ultrawide', 'mission map markers may be misplaced (known issue)'),
    'SMAA': (_S, _S, _S, 'SMAA', 'Vulkan post-processing'),
    'FSR 1 and RCAS': (_S, _S, _S, 'FSR', 'Vulkan post-processing'),
    'Texture filtering': (_S, _S, _S, 'Texture filtering', 'the same Vulkan sampler setting'),
    'VSync': (_S, _S, _E, ALL_BUILDS, _GAMESCOPE),
    'Display mode, monitor and graphics card': (_S, _S, _E, ALL_BUILDS, 'Gamescope owns the Steam Deck\'s output and '
                                                'shows the game full screen'),
    'Skip Intro': (_S, _S, _S, 'Skip Intro', 'the same guest patch'),
    'Exit Game': (_S, _S, _S, 'Exit Game: Options', 'the same menu patch'),
    'Xbox and PlayStation prompts': (_S, _S, _S, 'Xbox prompts', 'the same patched assets'),
    'Asian Menu Buttons': (_S, _S, _S, 'Asian Menu Buttons', 'the same engine getters'),
    'Voice-Over Language text fix': (_S, _S, _S, 'Voice-Over Language text fix', 'the same stream.dat patch'),
    'Controller and keyboard input': (_S, _S, _S, 'Controller support', 'SDL3; Steam Input on the Steam Deck'),
    'Save data': (_S, _S, _S, 'Save', 'per user and title, in the data folder'),
    'Trophies and Trophy Key': (_S, _S, _S, 'Trophies', 'importing a key file needs a file chooser on Linux (zenity or '
                                'kdialog); Enter Trophy Key works everywhere'),
    'Bug Report and the logs folder': (_S, _E, _E, 'Bug reports', 'Explorer; Linux: xdg-open'),
    'Built-in updater': (_S, _S, _S, ALL_BUILDS, 'Windows: the ZIP through trg-updater.exe; Linux: the AppImage file is '
                         'replaced beside itself and restarted'),
    'Data folder': (_S, _E, _E, ALL_BUILDS, 'beside alienation.exe; Linux: beside the AppImage file, or '
                    '$XDG_DATA_HOME/AlienationPC when that folder is read-only; never inside the AppImage'),
    'Allow overlay programs': (_S, _U, _U, ALL_BUILDS, 'Linux overlays are Vulkan layers or LD_PRELOAD libraries that '
                               'attach to the game process as it is'),
}


def platform_builds(feature):
    """The build columns where a platform feature exists."""
    builds = PLATFORM_FEATURES[feature][3]
    if builds == ALL_BUILDS:
        return [key for key, _ in COLUMNS]
    return [key for key, _ in COLUMNS if status(key, builds)[0] == SUPPORTED]


def platform_table():
    names = dict(COLUMNS)
    lines = [_row('Feature', list(PLATFORMS) + ['Builds', 'Notes']), '|' + '---|' * (len(PLATFORMS) + 3)]
    for feature, (windows, linux, deck, _, note) in PLATFORM_FEATURES.items():
        builds = platform_builds(feature)
        listed = [key for key in builds if key != 'COMPLETE' or 'CUSA00062-1.06' not in builds]
        shown = 'all' if len(builds) == len(COLUMNS) else ', '.join(names[key] for key in listed)
        lines.append(_row(feature, [windows, linux, deck, shown, note]))
    return '\n'.join(lines)


def platform_summary():
    width = max(map(len, PLATFORM_FEATURES))
    return '\n'.join(f'{feature:<{width}}  ' + '  '.join(f'{p}: {state}' for p, state in zip(PLATFORMS, states[:3]))
                     for feature, states in PLATFORM_FEATURES.items())


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
    if '--platforms' in sys.argv:
        print(platform_table())
    elif '--summary' in sys.argv:
        print(platform_summary())
    else:
        print(identity_table())
        print()
        print(markdown_table())
