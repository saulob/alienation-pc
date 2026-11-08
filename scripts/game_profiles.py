"""Per-title facts for the games this port knows, keyed by TITLE_ID in sce_sys/param.sfo.

A version's layers are its packages, the base
first and then the updates applied over it; the last layer's param.sfo is the finished game's. launcher/game_profile.cpp
mirrors these values, and tests/game_profile_parity.py checks that the two agree.
"""
from pathlib import Path

from ps4_module import parse_sfo

ENTRY = 'eboot.bin'
# Linked after the eboot in this order: libc.prx needs libSceFios2.prx.
MODULES = ('libc.prx', 'libSceFios2.prx')
# What the loader needs; the game data is only unpacked with --scope full.
CORE_PATTERNS = ('eboot.bin', 'sce_module/*', 'sce_sys/*', '*.plt')

PROFILES = {
    'CUSA00062': {
        'name': 'Alienation',
        'region': 'Europe',
        'content_id': 'EP9000-CUSA00062_00-ALIENATIONGAMEEU',
        'versions': {
            '1.00': {
                'layers': ({'app_ver': '01.00', 'sfo_version': '01.00', 'update': False},),
                'eboot_sha256': 'f5f41f870db752add56920c5538441bc3750b1442f69c1de3895ee282887d855',
                'folder': 'CUSA00062-1.00',
                'data_files': ('audio.pck', 'stream.dat', 'video/intro.mp4', 'video/loading.mp4', 'video/splash.mp4',
                               'video/title.mp4'),
            },
        },
        'entry': ENTRY,
        'extra_executables': (),
        'modules': MODULES,
        'core_patterns': CORE_PATTERNS,
    },
    'CUSA04430': {
        'name': 'Alienation',
        'region': 'Asia',
        'content_id': 'HP9000-CUSA04430_00-ALIENATIONGAMEKR',
        'versions': {
            '1.00': {
                'layers': ({'app_ver': '01.00', 'sfo_version': '01.00', 'update': False},),
                'eboot_sha256': '11928457792fdba30aee3ceb9f5fb300bac720d0f3c1774825a8380a2dc9b72e',
                'folder': 'CUSA04430-1.00',
                'data_files': ('audio.pck', 'stream.dat', 'video/intro.mp4', 'video/loading.mp4', 'video/splash.mp4',
                               'video/splash_SIE.mp4', 'video/title.mp4'),
            },
        },
        'entry': ENTRY,
        'extra_executables': (),
        'modules': MODULES,
        'core_patterns': CORE_PATTERNS,
    },
}


def builds():
    """(TITLE_ID, profile version, build) of every supported build, in launcher order."""
    return [(title, version, build) for title, found in PROFILES.items() for version, build in found['versions'].items()]


def title_id(game):
    """TITLE_ID of a game folder, or None."""
    try:
        return parse_sfo((Path(game) / 'sce_sys' / 'param.sfo').read_bytes()).get('TITLE_ID')
    except (OSError, ValueError, KeyError):
        return None


def profile(game_or_title):
    """The profile for a TITLE_ID or a game folder, or None for an unknown title."""
    key = str(game_or_title)
    if key not in PROFILES:
        key = title_id(game_or_title) or ''
    return PROFILES.get(key)


def finished_layer(build):
    """The layer whose param.sfo the installed game has."""
    return build['layers'][-1]


def layer_identity(title, layer):
    """(TITLE_ID, APP_VER, VERSION, is an update) of the package a layer is."""
    return title, layer['app_ver'], layer['sfo_version'], layer['update']


def package_identity(info):
    """The identity of a package, as layer_identity() names it, or None when it is not a package of a supported title."""
    sfo = info['param_sfo']
    found = PROFILES.get(sfo.get('TITLE_ID', ''))
    if not found or sfo.get('CONTENT_ID') != found['content_id']:
        return None
    return sfo['TITLE_ID'], sfo.get('APP_VER'), sfo.get('VERSION'), sfo.get('CATEGORY') == 'gp'


def finishing_version(title, identity):
    """The version of `title` whose finished game is the package with this identity, or None."""
    for version, build in PROFILES[title]['versions'].items():
        if layer_identity(title, finished_layer(build)) == identity:
            return version
    return None


def version_of(game):
    """The profile version whose finished param.sfo matches the game folder, or None."""
    try:
        sfo = parse_sfo((Path(game) / 'sce_sys' / 'param.sfo').read_bytes())
    except (OSError, ValueError):
        return None
    found = PROFILES.get(sfo.get('TITLE_ID', ''))
    if not found:
        return None
    return next((v for v, build in found['versions'].items()
                 if (finished_layer(build)['app_ver'], finished_layer(build)['sfo_version'])
                 == (sfo.get('APP_VER'), sfo.get('VERSION'))), None)


def package_layer(info):
    """(TITLE_ID, profile version, layer index) of the package of a supported build's finished game, or None.

    `info` is what scripts/pkg_info.py reads. The index is the package's place in the build's layers.
    """
    identity = package_identity(info)
    if identity is None:
        return None
    version = finishing_version(identity[0], identity)
    if version is None:
        return None
    return identity[0], version, len(PROFILES[identity[0]]['versions'][version]['layers']) - 1
