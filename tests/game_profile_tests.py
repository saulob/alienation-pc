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

    for text in failures:
        print(f'FAILED: {text}')
    if not failures:
        print(f'game profile tests passed ({len(python)} builds)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
