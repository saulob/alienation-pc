"""Checks that scripts/game_profiles.py and launcher/game_profile.cpp describe the same builds, in the same order.

Usage: python tests/game_profile_tests.py
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import game_profiles  # noqa: E402

failures = []
# The guest patch tables keyed by build ("CUSA00062-1.00"): a build is patched only when it is named here.
FEATURE_TABLES = ('src/skip_intro.c', 'src/exit_game.c', 'launcher/exit_game_script.c',
                  'launcher/controller_prompts_data.c', 'launcher/controller_prompts_glyphs_data.c')


def check(condition, text):
    if not condition:
        failures.append(text)


def cpp_arrays(source):
    """name -> list of string literals, for every constexpr const char* array."""
    found = {}
    for name, body in re.findall(r'constexpr const char\* (\w+)\[\] = \{(.*?)\};', source, re.S):
        found[name] = re.findall(r'"([^"]*)"', body)
    return found


def cpp_layers(source):
    found = {}
    for name, body in re.findall(r'constexpr PackageLayer (\w+)\[\] = \{(.*?)\};\n', source, re.S):
        found[name] = [(a, b, u == 'true') for a, b, u in re.findall(r'\{"([^"]+)", "([^"]+)", (true|false)\}', body)]
    return found


def cpp_builds(source):
    table = re.search(r'constexpr GameBuild kGameBuilds\[\] = \{(.*?)\n\};', source, re.S).group(1)
    builds = []
    for entry in re.findall(r'\n    \{(.*?)\n    \},', table, re.S):
        fields = [f.strip() for f in entry.strip().split('\n')]
        fields = [f[:-1] if f.endswith(',') else f for f in fields]
        builds.append(fields)
    return builds


def main():
    source = (ROOT / 'launcher' / 'game_profile.cpp').read_text(encoding='utf-8')
    arrays, layers = cpp_arrays(source), cpp_layers(source)
    cpp = cpp_builds(source)
    python = game_profiles.builds()
    check(len(cpp) == len(python), f'{len(cpp)} builds in game_profile.cpp, {len(python)} in game_profiles.py')

    for fields, (title, version, build) in zip(cpp, python):
        name = f'{title} {version}'
        strings = [f.strip('"') for f in fields[:5]]
        profile = game_profiles.PROFILES[title]
        check(strings == [title, profile['content_id'], version, build['folder'], build['eboot_sha256']],
              f'{name}: identity differs: {strings}')
        check(arrays.get(fields[5]) == list(build['data_files']), f'{name}: data files differ ({fields[5]})')
        check(fields[6].strip('"') == name, f'{name}: label is {fields[6]}')
        check(fields[7].strip('"') == profile['region'], f'{name}: region is {fields[7]}')
        wanted = [(layer['app_ver'], layer['sfo_version'], layer['update']) for layer in build['layers']]
        check(layers.get(fields[8]) == wanted, f'{name}: layers differ ({fields[8]})')
        check(build['folder'] == f'{title}-{version}', f'{name}: folder {build["folder"]}')
        check(re.fullmatch(r'[0-9a-f]{64}', build['eboot_sha256']) is not None, f'{name}: bad sha256')
        check(profile['content_id'][7:16] == title, f'{name}: content id {profile["content_id"]} names another title')

    folders = [b['folder'] for _, _, b in python]
    hashes = [b['eboot_sha256'] for _, _, b in python]
    check(len(set(folders)) == len(folders), 'two builds share a folder')
    check(len(set(hashes)) == len(hashes), 'two builds share an eboot.bin')
    shared = [n for n in re.search(r'kSharedGameFiles\[\] = \{(.*?)\};', (ROOT / 'launcher' / 'game_profile.h')
                                   .read_text(encoding='utf-8'), re.S).group(1).split('"')[1::2]]
    for title, version, build in python:
        modules = game_profiles.PROFILES[title]['modules']
        check(all(f'sce_module/{m}' in shared for m in modules), f'{title} {version}: a module is not a shared file')
        check(not set(build['data_files']) & set(shared), f'{title} {version}: a data file is also a shared file')

    check_updates(python)
    check_feature_isolation(cpp, python)
    for text in failures:
        print(f'FAILED: {text}')
    if not failures:
        print(f'game profile tests passed ({len(python)} builds)')
    return 1 if failures else 0


def check_updates(python):
    """Every update build is laid over exactly one base build of its title, listed before it."""
    order = [(title, version) for title, version, _ in python]
    for title, version, build in python:
        layers = build['layers']
        check(not layers[0]['update'] and all(layer['update'] for layer in layers[1:]),
              f'{title} {version}: only the layers after the base may be updates')
        check(layers[-1]['app_ver'] == version.zfill(5), f'{title} {version}: the finished APP_VER does not name the version')
        if len(layers) == 1:
            continue
        bases = [v for v, b in game_profiles.PROFILES[title]['versions'].items()
                 if len(b['layers']) == 1 and b['layers'][0] == layers[0]]
        check(len(bases) == 1, f'{title} {version}: {len(bases)} base builds')
        if bases:
            check(order.index((title, bases[0])) < order.index((title, version)), f'{title} {version}: listed before its base')
    check(('CUSA00062', '1.06') in order, 'CUSA00062 1.06 is not a supported build')


def check_feature_isolation(cpp, python):
    """A build without verified patch sites is named by no guest patch table, so no other build's patch applies to it."""
    keys = {name: set(re.findall(r'\{"(CUSA\d{5}-\d+\.\d\d)"', (ROOT / name).read_text(encoding='utf-8')))
            for name in FEATURE_TABLES}
    supported = {f'{title}-{version}' for title, version, _ in python}
    for name, found in keys.items():
        check(found, f'{name}: no build keys found')
        check(found <= supported, f'{name}: {sorted(found - supported)} are not supported builds')
    unported = []
    for fields, (title, version, _) in zip(cpp, python):
        key = f'{title}-{version}'
        if fields[9] == '{}' and fields[10] == 'nullptr':
            unported.append(key)
            for name, found in keys.items():
                check(key not in found, f'{key} has no verified sites but {name} names it')
        else:
            check(fields[9] != '{}' and fields[10] != 'nullptr', f'{key}: resolution and frame rate sites must come together')
    check('CUSA00062-1.06' in unported, 'CUSA00062 1.06 must stay without guest patches until its sites are verified')


if __name__ == '__main__':
    sys.exit(main())
