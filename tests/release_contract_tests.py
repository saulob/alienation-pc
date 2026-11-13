"""The release contract, without building or network: one version source, the updater's release address and asset
names against the packaging scripts, the desktop entry and AppStream metadata, the forbidden-file and licence lists of
both packagers, the FFmpeg pin and the release notes.

Usage: python tests/release_contract_tests.py
"""
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
failures = []
checks = 0


def check(condition, text):
    global checks
    checks += 1
    if not condition:
        failures.append(text)


def read(relative):
    return (ROOT / relative).read_text(encoding='utf-8')


VERSION_PATTERN = r'project\(alienation_pc VERSION ([0-9]+\.[0-9]+\.[0-9]+)'
REPOSITORY = 'saulob/alienation-pc'
APP_ID = 'io.github.saulob.alienation_pc'


def version():
    found = re.findall(r'(?m)^' + VERSION_PATTERN, read('CMakeLists.txt'))
    check(len(found) == 1, 'CMakeLists.txt must declare the project VERSION exactly once')
    return found[0] if found else '0.0.0'


def check_version_source(current):
    for script in ('tools/package.ps1', 'tools/package-release.ps1'):
        check(r"project\(alienation_pc VERSION ([0-9]+\.[0-9]+\.[0-9]+)" in read(script),
              f'{script} does not read the version from CMakeLists.txt')
    check(r"s/^project\(alienation_pc VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p" in read('tools/package-linux.sh'),
          'tools/package-linux.sh does not read the version from CMakeLists.txt')
    cmake = read('CMakeLists.txt')
    check('set(ALN_PORT_VERSION "${PROJECT_VERSION}")' in cmake, 'ALN_PORT_VERSION is not the project VERSION')
    check('#define ALN_VERSION_STRING \\"${PROJECT_VERSION}\\"' in cmake, 'the Windows version resource has its own version')
    check('ALN_PORT_VERSION="${PROJECT_VERSION}"' in read('launcher/CMakeLists.txt'), 'the launcher has its own version')
    check('"v" ALN_PORT_VERSION' in read('launcher/launcher.cpp'), 'the launcher does not show ALN_PORT_VERSION')
    check('config.current_version = ALN_PORT_VERSION;' in read('launcher/update_config.cpp'),
          'the updater does not compare against ALN_PORT_VERSION')
    for template in ('packaging/README.txt', 'packaging/README-linux.txt', f'packaging/linux/{APP_ID}.appdata.xml'):
        text = read(template)
        check('@VERSION@' in text, f'{template} does not take the version from the packager')
        check(f'v{current}' not in text and f'Version {current}' not in text, f'{template} spells the version')
    # Sources must not spell the release version (only CMakeLists.txt holds it) nor keep temporary diagnostics.
    literal = re.compile(r'["\']v?' + re.escape(current) + r'["\']')
    for folder in ('src', 'launcher', 'gpu/shim', 'gpu/shadps4'):
        for path in (ROOT / folder).rglob('*'):
            if path.suffix in ('.c', '.cpp', '.h', '.hpp', '.inc') and 'third_party' not in path.parts:
                text = path.read_text(encoding='utf-8', errors='replace')
                check(not literal.search(text), f'{path.relative_to(ROOT)} spells the version {current}')
                check('DEBUG_SAULO' not in text, f'{path.relative_to(ROOT)} keeps a DEBUG_SAULO diagnostic')


def check_updater(current):
    config = read('launcher/update_config.cpp')
    check(f'"https://api.github.com/repos/{REPOSITORY}/releases/latest"' in config, 'the update address is not this repository')
    check(f'"https://github.com/{REPOSITORY}/releases/download/"' in config, 'downloads are not limited to this repository')
    check(f'"github.com/{REPOSITORY}"' in read('launcher/game_profile.h'), 'the launcher names another repository')
    windows = re.search(r'"AlienationPC-v" \+ version\.CoreString\(\) \+ "-windows-x64\.zip"', config)
    linux = re.search(r'"AlienationPC-v" \+ version\.CoreString\(\) \+ "-x86_64\.AppImage"', config)
    check(windows and linux, 'the updater looks for other asset names')
    check('"-windows-x64.zip.sha256"' in config and '"-x86_64.AppImage.sha256"' in config, 'no checksum asset names')
    check('$name = "AlienationPC-v$version-windows-x64"' in read('tools/package.ps1'), 'the Windows package has another name')
    check('name=AlienationPC-v$version-x86_64' in read('tools/package-linux.sh'), 'the AppImage has another name')
    release = read('tools/package-release.ps1')
    check('"AlienationPC-v$version-windows-x64"' in release and '"AlienationPC-v$version-x86_64"' in release,
          'tools/package-release.ps1 expects other artifact names')
    controller = read('launcher/updater/update_controller.cpp')
    check('report.status == 404' in controller and 'outcome->no_release = true' in controller,
          'a missing first release is reported as an error')
    check(re.search(r"'aln-update-network-tests'", read('tools/test.ps1')) is not None and
          'aln-update-network-tests' in read('tools/build.sh'), 'the updater network tests are not run on both platforms')


def desktop_entries(text):
    sections, current = {}, None
    for line in text.splitlines():
        if line.startswith('[') and line.endswith(']'):
            current = sections.setdefault(line[1:-1], {})
        elif '=' in line and current is not None:
            key, value = line.split('=', 1)
            current[key] = value
    return sections


def check_desktop_and_metainfo(current):
    desktop_path = f'packaging/linux/{APP_ID}.desktop'
    entries = desktop_entries(read(desktop_path))
    main = entries.get('Desktop Entry', {})
    expected = {'Type': 'Application', 'Name': 'Alienation PC', 'Exec': 'AppRun', 'Icon': 'alienation-pc',
                'Terminal': 'false', 'StartupWMClass': 'alienation', 'Actions': 'Launcher;'}
    for key, value in expected.items():
        check(main.get(key) == value, f'{desktop_path}: {key} is {main.get(key)!r}, not {value!r}')
    check('Game;' in main.get('Categories', ''), f'{desktop_path}: not in the Game category')
    check(main.get('Comment', '').startswith('PC port of Alienation'), f'{desktop_path}: Comment')
    action = entries.get('Desktop Action Launcher', {})
    check(action.get('Exec') == 'AppRun --launcher' and action.get('Name') == 'Show launcher',
          f'{desktop_path}: no Show launcher action with --launcher')
    check('"--launcher"' in read('src/loader.c'), 'the loader does not accept --launcher')

    metainfo_path = f'packaging/linux/{APP_ID}.appdata.xml'
    root = ET.fromstring(read(metainfo_path))
    check(root.tag == 'component' and root.get('type') == 'desktop-application', f'{metainfo_path}: component type')
    check(root.findtext('id') == APP_ID, f'{metainfo_path}: id is not {APP_ID}')
    check(root.findtext('launchable') == f'{APP_ID}.desktop', f'{metainfo_path}: launchable is not the desktop file')
    check(root.findtext('name') == main.get('Name'), f'{metainfo_path}: name differs from the desktop entry')
    check(root.findtext('summary') == main.get('Comment'), f'{metainfo_path}: summary differs from the desktop entry')
    check(root.findtext('metadata_license') == 'CC0-1.0', f'{metainfo_path}: metadata licence')
    check('GPL-2.0-only' in (root.findtext('project_license') or ''), f'{metainfo_path}: project licence')
    for url in root.findall('url'):
        check((url.text or '').startswith(f'https://github.com/{REPOSITORY}') or
              url.text == 'https://github.com/sponsors/saulob', f'{metainfo_path}: unexpected url {url.text}')
    check({url.get('type') for url in root.findall('url')} >= {'homepage', 'bugtracker'}, f'{metainfo_path}: urls')
    release = root.find('releases/release')
    check(release is not None and release.get('version') == '@VERSION@' and release.get('date') == '@DATE@',
          f'{metainfo_path}: the release entry must be filled by tools/package-linux.sh')
    check(root.find('content_rating') is not None, f'{metainfo_path}: no content rating')
    package = read('tools/package-linux.sh')
    check(f'app_id={APP_ID}' in package and 'desktop-file-validate "$appdir/$desktop_file" ||' in package,
          'tools/package-linux.sh does not stage and validate this desktop entry')
    check('"$metainfo"' in package and 'appstreamcli validate' in package, 'the metainfo is not staged or validated')
    check('patchelf --remove-rpath "$appdir/usr/bin/alienation"' in package and
          "patchelf --force-rpath --set-rpath '$ORIGIN/../lib'" in package and "'(RPATH)" in package,
          'the AppImage binary may keep a DT_RUNPATH, which LD_LIBRARY_PATH overrides')
    for path in (desktop_path, metainfo_path):
        text = read(path)
        check(not re.search(r'[A-Za-z]:\\|/home/|/root/|VisualStudio', text), f'{path} names a local path')


def check_packagers():
    windows = read('tools/package.ps1')
    linux = read('tools/package-linux.sh')
    names_windows = set(re.findall(r"'([\w.]+)'", re.search(r'\$ForbiddenNames = @\((.*?)\)', windows, re.S).group(1)))
    names_linux = set(n.replace('\\', '') for n in
                      re.search(r"forbidden_names='\^\((.*?)\)\$'", linux).group(1).split('|'))
    check(names_windows == names_linux, f'the packagers forbid different names: {sorted(names_windows ^ names_linux)}')
    check('alienation.ini' in names_windows, 'the settings file is not forbidden in a package')
    for word in ('DEBUG_SAULO', 'VisualStudio'):
        check(word in windows and word in linux, f'a packager does not search for {word}')
    built_windows = set(re.findall(r"^\s+'([\w-]+)'\s+= '", re.search(r'\$BuiltInLicenses = \[ordered\]@\{(.*?)\n\}',
                                                                     windows, re.S).group(1), re.M))
    built_linux = set(re.findall(r'\[([\w-]+)\]=', re.search(r'declare -A built_in=\((.*?)\)\n', linux, re.S).group(1)))
    check(built_windows == built_linux, f'the packagers ship different licence texts: {sorted(built_windows ^ built_linux)}')
    third = read('packaging/licenses/THIRD-PARTY-LICENSES.txt')
    for component in ('shadPS4', 'AnyPS5', 'sirit', 'SPIRV-Headers', 'SMAA', 'FidelityFX', 'Dear ImGui', 'miniz',
                      'nanosvg', 'xbyak', 'TRG Launcher', 'SDL3', 'Zydis', 'xxHash', 'OpenSSL', 'FFmpeg', 'Noto Sans',
                      'DejaVu', 'ANGLE', 'libstdc++'):
        check(component in third, f'THIRD-PARTY-LICENSES.txt does not list {component}')
    check('RelWithDebInfo' in read('tools/build.ps1') and '--strip-debug' in windows and '--strip-debug' in linux,
          'the packages do not strip the debug information of the tested build')
    for script in ('tools/package.ps1', 'tools/package-release.ps1', 'tools/package-linux.sh', 'tools/package-release-linux.sh'):
        text = read(script)
        check(not re.search(r'build\.sh[^\n]*\s--run\b', text) and not re.search(r'build\.ps1[^\n]*\s-Run\b', text) and
              "@{ Run = $true }" not in text, f'{script} starts the game')


def check_ffmpeg():
    pin = {}
    for line in read('gpu/ffmpeg.txt').splitlines():
        if line.strip() and not line.startswith('#'):
            key, _, value = line.partition(' ')
            pin[key] = value
    check(re.fullmatch(r'[0-9a-f]{64}', pin.get('sha256', '')) is not None, 'gpu/ffmpeg.txt: sha256')
    check(pin.get('url') == f'https://ffmpeg.org/releases/ffmpeg-{pin.get("version")}.tar.xz', 'gpu/ffmpeg.txt: url')
    options = ' '.join(pin.get(k, '') for k in ('configure', 'windows', 'linux'))
    for forbidden in ('--enable-gpl', '--enable-nonfree', '--enable-version3'):
        check(forbidden not in options, f'gpu/ffmpeg.txt: {forbidden} would change FFmpeg\'s licence')
    for needed in ('--disable-everything', '--disable-autodetect', '--disable-network', '--enable-shared',
                   '--enable-decoder=h264,hevc,aac', '--enable-demuxer=mov', '--enable-protocol=file'):
        check(needed in pin.get('configure', ''), f'gpu/ffmpeg.txt: {needed} missing')
    check('--enable-w32threads' in pin.get('windows', '') and '--enable-pthreads' in pin.get('linux', ''),
          'gpu/ffmpeg.txt: threads must be enabled on both platforms (autodetect is off)')
    codecs = read('gpu/shadps4/core/libraries/avplayer/avplayer_source.cpp')
    check('AV_CODEC_ID_H264' in codecs and 'AV_CODEC_ID_HEVC' in codecs and 'AV_CODEC_ID_AAC' in codecs,
          'AvPlayer accepts codecs the FFmpeg build does not decode')
    check('tools/build-ffmpeg.sh' in read('packaging/linux/ci-deps.sh'), 'the Linux release environment has no FFmpeg')
    check('Sync-FFmpeg' in read('tools/build.ps1') and 'ALN_FFMPEG_PREFIX' in read('gpu/CMakeLists.txt'),
          'the Windows build may link MSYS2\'s GPL FFmpeg')
    check('Test-LgplFFmpegDll' in read('tools/package.ps1') and "'LGPL version 2.1 or later'" in read('tools/package-linux.sh'),
          'a packager does not refuse a GPL FFmpeg')


def check_release_notes(current):
    notes = ROOT / 'docs' / 'releases' / f'v{current}.md'
    check(notes.is_file(), f'no release notes in docs/releases/v{current}.md')
    if notes.is_file():
        text = notes.read_text(encoding='utf-8')
        for name in (f'AlienationPC-v{current}-windows-x64.zip', f'AlienationPC-v{current}-x86_64.AppImage'):
            check(name in text, f'the release notes do not name {name}')
        check('virustotal' not in text.lower(), 'the release notes link VirusTotal')
        check('Known issues' in text and 'No game files' in text, 'the release notes lack known issues or the no-game-files note')


def main():
    current = version()
    check_version_source(current)
    check_updater(current)
    check_desktop_and_metainfo(current)
    check_packagers()
    check_ffmpeg()
    check_release_notes(current)
    for text in failures:
        print(f'FAILED: {text}')
    print(f'release contract: version {current}, {checks} checks, {len(failures)} failures')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
