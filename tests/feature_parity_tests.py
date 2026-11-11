"""Checks the feature parity contract (scripts/feature_parity.py) against the build profiles, every guest patch table,
the tests that carry its evidence and docs/GAME_BUILDS.md, without game files. tests/patch_site_tests.py checks the same
sites on the real executables.

Usage: python tests/feature_parity_tests.py
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'tests'))
import feature_parity as parity  # noqa: E402
import game_profiles  # noqa: E402
import prompt_language_tests  # noqa: E402

failures = []
STATUSES = (parity.SUPPORTED, parity.DIFFERENT, parity.UNAVAILABLE)
LEVELS = (parity.STATIC, parity.HEADLESS, parity.MANUAL)
# Only playing the game shows these; no table or headless run can claim more.
MANUAL_ONLY = ('Boot', 'Save', 'Trophies', 'Audio', 'Controller support', 'SMAA', 'FSR', 'Texture filtering', 'Online',
               'Matching2', 'P2P')


def check(condition, text):
    if not condition:
        failures.append(text)


def read(relative):
    return (ROOT / relative).read_text(encoding='utf-8')


def keys():
    return [f'{title}-{version}' for title, version, _ in game_profiles.builds()]


def columns():
    return [key for key, _ in parity.COLUMNS]


def cpp_builds():
    """build key -> (resolution sites array name, frame rate) from launcher/game_profile.cpp."""
    source = read('launcher/game_profile.cpp')
    table = re.search(r'constexpr GameBuild kGameBuilds\[\] = \{(.*?)\n\};', source, re.S).group(1)
    found = {}
    for entry in re.findall(r'\n    \{(.*?)\n    \},', table, re.S):
        fields = [f.strip().rstrip(',') for f in entry.strip().split('\n')]
        found[fields[3].strip('"')] = (fields[9], fields[10])
    return source, found


def resolution_sites(source, name):
    body = re.search(rf'{name}\[\] = \{{(.*?)\n\}};', source, re.S)
    if not body:
        return []
    return [(int(offset, 16), int(length), int(budget))
            for offset, length, budget in re.findall(r'\{(0x[0-9a-f]+), (\d+), \{[^}]*\}, \w+, \d, (\d)\}', body.group(1))]


def table_keys(relative):
    return set(re.findall(r'\{"(CUSA\d{5}-\d+\.\d\d)"', read(relative)))


def check_declarations():
    """Every build and form declares every feature with a status and a verification level, and nothing else."""
    check(set(parity.PARITY) == set(keys()), f'parity builds {sorted(parity.PARITY)} are not the supported {keys()}')
    for key, features in parity.PARITY.items():
        check(set(features) == set(parity.FEATURES), f'{key}: features {sorted(set(parity.FEATURES) ^ set(features))}')
    for column in columns():
        check(column in parity.PARITY or column in parity.PACKAGE_FORMS, f'docs column {column} is no build or form')
        for feature in parity.FEATURES:
            state, level, note = parity.status(column, feature)
            check(state in STATUSES, f'{column} {feature}: unknown status {state}')
            check(level in LEVELS, f'{column} {feature}: unknown verification {level}')
            check(state == parity.SUPPORTED or len(note.strip()) >= 12, f'{column} {feature}: {state} without a reason')
            check(feature not in MANUAL_ONLY or level == parity.MANUAL,
                  f'{column} {feature}: {level}, but only the game can show it')
            check(level != parity.HEADLESS or re.search(r'tests/\w+\.(c|cpp|py)', note),
                  f'{column} {feature}: headless verification without naming its test')


def check_guest_patches():
    """A supported guest patch names the build in its table; an unsupported one never does."""
    source, builds = cpp_builds()
    tables = {name: table_keys(name) for name in ('src/skip_intro.c', 'src/exit_game.c', 'launcher/exit_game_script.c',
                                                  'launcher/controller_prompts_data.c',
                                                  'launcher/controller_prompts_glyphs_data.c', 'launcher/menu_buttons.c')}
    prompts, glyphs = read('launcher/controller_prompts_data.c'), read('launcher/controller_prompts_glyphs_data.c')
    for key in keys():
        resolution_name, frame_rate = builds[key]
        sites = resolution_sites(source, resolution_name)
        exit_game = key in tables['src/exit_game.c'] and key in tables['launcher/exit_game_script.c']
        atlases = re.search(r'(\d+) glyph atlases', parity.PARITY[key]['Xbox prompts'][2])
        evidence = {
            '60 FPS': frame_rate != 'nullptr',
            'Render Resolution': len([s for s in sites if not s[2]]) == 2,
            'Aspect Ratio': len([s for s in sites if not s[2]]) == 2,
            'True Ultrawide': len([s for s in sites if not s[2]]) == 2,
            'GPU heap scaling': len([s for s in sites if s[2]]) == 2,
            'Skip Intro': key in tables['src/skip_intro.c'],
            'Exit Game: Options': exit_game,
            'Exit Game: Select Campaign': exit_game,
            'Xbox prompts': prompts.count(f'{{"{key}",') == 9 and atlases is not None and
                            glyphs.count(f'{{"{key}",') == int(atlases.group(1)),
            'Asian Menu Buttons': key in tables['launcher/menu_buttons.c'],
        }
        for feature, present in evidence.items():
            state = parity.PARITY[key][feature][0]
            check(present == (state == parity.SUPPORTED), f'{key} {feature}: {state} but the patch tables say {present}')
    for name, found in tables.items():
        check(found <= set(keys()), f'{name} names unsupported builds {sorted(found - set(keys()))}')
    patch_sites = read('tests/patch_site_tests.py')
    for key in keys():
        check(f"'{key}'" in patch_sites, f'tests/patch_site_tests.py does not check {key} on its executable')


def registered(test):
    """tools/test.ps1 runs the Python test, or builds and runs the test target."""
    runner = read('tools/test.ps1')
    if test.endswith('.py'):
        return f'tests\\{test}' in runner
    cmake = read('CMakeLists.txt') + read('launcher/CMakeLists.txt')
    return f"'{test}'" in runner and f'add_executable({test} ' in cmake


def check_semantic_evidence():
    """A headless claim is carried by a test that runs the build, and says what that test proves."""
    menu_test = read('tests/exit_game_menu_tests.c')
    builds = re.search(r'builds\[\] = \{(.*?)\};', menu_test)
    check(registered('aln-exit-game-menu-tests'), 'the Exit Game menu test is not run by tools/test.ps1')
    check(registered('prompt_language_tests.py'), 'the prompt language test is not run by tools/test.ps1')
    for key in keys():
        check(builds is not None and f'"{key}"' in builds.group(1), f'{key}: the Exit Game menus are not executed')
        languages = prompt_language_tests.LANGUAGES.get(key, {})
        note = parity.PARITY[key]['Xbox prompts in every language'][2]
        check(note.startswith(f'{len(languages)} languages'), f'{key}: the language note does not count '
                                                              f'{len(languages)} languages')
        check(('kr, jp, cn' in note) == ('kr' in languages), f'{key}: the note and the test disagree on kr, jp, cn')

    # Asian Menu Buttons: off by default, the build's own answer in the note, and Cross fixed where there is no getter.
    check('{kAsianMenuButtonsKey, "0"}' in read('launcher/settings.cpp'), 'Asian Menu Buttons is not off by default')
    check('kAsianMenuButtonsKey, false, "Off", "On"' in read('launcher/launcher.cpp'),
          'the Asian Menu Buttons toggle does not default to Off')
    menu_test = read('tests/menu_buttons_tests.c')
    check(registered('aln-menu-buttons-tests'), 'the menu buttons test is not run by tools/test.ps1')
    for key in keys():
        state, _, note = parity.PARITY[key]['Asian Menu Buttons']
        native = re.search(r'chosen_layout\("' + re.escape(key) + r'", (\d)\)', menu_test)
        if state == parity.SUPPORTED:
            check(native is not None, f'{key}: Asian Menu Buttons is not run by tests/menu_buttons_tests.c')
            if native:
                check(('native On' in note) == (native.group(1) == '1'), f'{key}: the note has the wrong native layout')
        else:
            check('Cross always confirms' in note and native is None, f'{key}: unavailable without the scripts\' Cross')

    install = read('launcher/game_install.cpp')
    install_test = read('tests/game_install_tests.cpp')
    check('TestInstallLineage(scratch);' in install_test and registered('aln-game-install-tests'),
          'the install lineage is not tested')
    for name, form in parity.PACKAGE_FORMS.items():
        update, remove = form['differs'].get('Install Update'), form['differs'].get('Remove Update')
        check(update is not None and update[0] == parity.DIFFERENT and 'refused before any change' in update[2],
              f'{name}: Install Update is not refused')
        check(remove is not None and remove[0] == parity.UNAVAILABLE, f'{name}: Remove Update is offered')
    check('RefuseUpdateOverRepack(roots, hooks, builds, build)' in install and
          'InstallStatus::kAlreadyContainsUpdate' in install, 'an update over a repack is not refused')
    check('== InstallProvenance::kUpdateLayers' in install, 'Remove update does not require official update layers')


def check_overlaps():
    """No two guest sites of one build touch the same bytes (static: from the tables)."""
    source, builds = cpp_builds()
    skip = read('src/skip_intro.c')
    exit_game = read('src/exit_game.c')
    menu = read('launcher/menu_buttons.c')
    frame_rates = dict(re.findall(r'constexpr AlnFrameRatePatch (\w+) = \{(0x[0-9a-f]+),', source))
    for key in keys():
        resolution_name, frame_rate = builds[key]
        spans = [(offset, offset + length + 4) for offset, length, _ in resolution_sites(source, resolution_name)]
        if frame_rate != 'nullptr':
            at = int(frame_rates[frame_rate.lstrip('&')], 16)
            spans.append((at, at + 4))
        row = re.search(r'\{"' + key + r'", (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+)\}', skip)
        check((row is not None) == (f'"{key}"' in skip), f'{key}: the Skip Intro row does not parse')
        if row:
            is_down, _, update, _ = (int(v, 16) for v in row.groups())
            spans += [(is_down, is_down + 5), (update, update + 10)]
        row = re.search(r'\{"' + key + r'",' + r',\s*'.join([r'\s*(0x[0-9a-f]+)u'] * 10) + r'\}', exit_game)
        check((row is not None) == (f'"{key}"' in exit_game), f'{key}: the Exit Game row does not parse')
        if row:
            exit_app, _, text_copy, _, box_text = (int(v, 16) for v in row.groups()[:5])
            spans += [(exit_app + 7, exit_app + 13), (text_copy, text_copy + 5), (box_text, box_text + 5)]
        row = re.search(r'\{"' + key + r'", (0x[0-9a-f]+)u, (0x[0-9a-f]+)u, 0x[0-9a-f]+u, 0x[0-9a-f]+u\}', menu)
        check((row is not None) == (f'"{key}"' in menu), f'{key}: the menu button row does not parse')
        if row:
            spans += [(int(v, 16) + 3, int(v, 16) + 7) for v in row.groups()]
        check(len(spans) >= 9, f'{key}: only {len(spans)} guest sites found')
        spans.sort()
        check(all(a[1] <= b[0] for a, b in zip(spans, spans[1:])), f'{key}: guest patch sites overlap: {spans}')


def check_regions():
    expected = {'CUSA00062': ('EUR', 'EP'), 'CUSA04430': ('ASIA/KR', 'HP')}
    for title, profile in game_profiles.PROFILES.items():
        check(title in expected, f'{title} has no expected region')
        if title in expected:
            region, prefix = expected[title]
            check(profile['region'] == region, f'{title}: region {profile["region"]}, expected {region}')
            check(profile['content_id'].startswith(prefix), f'{title}: content id {profile["content_id"]} is not {prefix}')
        check(not re.search(r'\bUSA\b', profile['region']), f'{title}: region says USA')
    for relative in ('docs/GAME_BUILDS.md', 'README.md'):
        for line in read(relative).splitlines():
            if 'CUSA04430' in line:
                check(not re.search(r'\bUSA\b', line), f'{relative} calls CUSA04430 USA: {line.strip()}')
            if 'CUSA04181' in line:
                check(re.search(r'not supported|unsupported|no game data|not owned', line, re.I) is not None,
                      f'{relative} names CUSA04181 without saying it is unsupported: {line.strip()}')
    check('CUSA04181' not in game_profiles.PROFILES, 'CUSA04181 is supported without game data')
    for relative in ('launcher/game_profile.cpp', 'src/skip_intro.c', 'src/exit_game.c', 'launcher/exit_game_script.c',
                     'launcher/menu_buttons.c', 'launcher/controller_prompts_data.c'):
        check('CUSA04181' not in read(relative), f'{relative} names CUSA04181')


def check_package_forms():
    """A package form runs as its build only through that build's executable fingerprint, and keeps its online key."""
    install = read('launcher/game_install.cpp')
    files = read('launcher/game_files.cpp')
    for name, form in parity.PACKAGE_FORMS.items():
        title, version = form['build'].split('-')
        build = game_profiles.PROFILES[title]['versions'][version]
        aliases = [a for a in build.get('package_aliases', ()) if a['name'] == form['alias']]
        check(len(aliases) == 1, f'{name}: {form["build"]} has no {form["alias"]} alias')
        check(form['eboot_sha256'] == build['eboot_sha256'], f'{name}: executable fingerprint is not {form["build"]}\'s')
        check(name not in keys() and f'"{name}' not in read('launcher/game_profile.cpp'),
              f'{name} became a build key of its own')
        for feature in form['differs']:
            check(feature in parity.FEATURES, f'{name}: {feature} is no feature')
    check('sha == identity.build->eboot_sha256 || (sha.empty() && !identity.alias)' in install,
          'a repack package is not refused without a readable, matching eboot.bin')
    check('identity.alias ? "eboot.bin of the repack is not the supported build\'s"' in files,
          'an installed repack is not checked against the build\'s eboot.bin')


def check_identity():
    """Runtime identity and installation provenance are separate: a repack runs as its build without its lineage."""
    check(set(parity.IDENTITY) == set(columns()), 'the identity table does not cover every column')
    for column in columns():
        runtime, provenance, online = parity.IDENTITY[column]
        build = parity.PACKAGE_FORMS[column]['build'] if column in parity.PACKAGE_FORMS else column
        check(runtime == build, f'{column}: runs as {runtime}, not {build}')
        check(online == runtime and f'lobby key {online}' in parity.status(column, 'Online')[2],
              f'{column}: the online key is not the runtime identity')
        title, version = build.split('-')
        layers = game_profiles.PROFILES[title]['versions'][version]['layers']
        update = any(layer['update'] for layer in layers)
        layered = 'update layers' in provenance and 'no update layer' not in provenance
        check(layered == (update and column not in parity.PACKAGE_FORMS),
              f'{column}: provenance "{provenance}" does not match how it is installed')
        removable = parity.status(column, 'Remove Update')[0] == parity.SUPPORTED
        check(removable == layered, f'{column}: Remove Update does not follow the provenance')


def check_online_keys():
    online = {key: parity.PARITY[key]['Online'][2] for key in keys()}
    for key, note in online.items():
        check(key in note, f'{key}: the online note does not name its lobby key')
    check(len(set(online.values())) == len(online), 'two builds share an online note')
    for name, form in parity.PACKAGE_FORMS.items():
        check(form['build'] in keys(), f'{name}: online key {form["build"]} is not a build key')


def check_docs():
    text = read('docs/GAME_BUILDS.md')
    check(parity.markdown_table() in text and parity.identity_table() in text,
          'docs/GAME_BUILDS.md does not hold the current parity tables (python scripts/feature_parity.py)')


def main():
    check_declarations()
    check_guest_patches()
    check_semantic_evidence()
    check_overlaps()
    check_regions()
    check_package_forms()
    check_identity()
    check_online_keys()
    check_docs()
    for text in failures:
        print(f'FAILED: {text}')
    if not failures:
        print(f'feature parity tests passed ({len(parity.PARITY)} builds, {len(parity.PACKAGE_FORMS)} package form, '
              f'{len(parity.FEATURES)} features)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
