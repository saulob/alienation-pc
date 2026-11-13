# Release and self-update

The project version is the `project(alienation_pc VERSION x.y.z)` line in `CMakeLists.txt`. The executable's version
resource, the launcher subtitle and Port version row, the updater's current version, the packagers and every artifact name
read it. Nothing is published, tagged or uploaded by these tools.

## Windows package

    pwsh -NoProfile -ExecutionPolicy Bypass -File tools\package-release.ps1               # Windows ZIP and Linux AppImage
    pwsh -NoProfile -ExecutionPolicy Bypass -File tools\package.ps1                       # Windows only
    pwsh -NoProfile -ExecutionPolicy Bypass -File tools\package.ps1 -SkipBuild            # package the existing build
    pwsh -NoProfile -ExecutionPolicy Bypass -File tools\package.ps1 -VerifyOnly dist\<name>.zip

`tools\package.ps1` builds with `tools\build.ps1` (the one `RelWithDebInfo` configuration: it is what is tested, and the
debug information is split off), stages the allowlisted files into `dist\AlienationPC-v<version>-windows-x64\`, runs
`tools\check-release-folder.ps1` on it, writes `AlienationPC-v<version>-windows-x64.zip` and `.zip.sha256` next to it, then
extracts the ZIP into a temporary folder and checks it again.

The ZIP holds one folder, `AlienationPC-v<version>-windows-x64`, with `alienation.exe`, `trg-updater.exe`, the DLLs they
load (SDL3, the LGPL FFmpeg, the pinned ANGLE files and the MSYS2 libraries listed in
`licenses\MSYS2-PACKAGES.txt`), `README.txt`, `LICENSE.txt`, `licenses\` and `assets\launcher\header-art.png` and
`sponsor-heart.png`. Nothing else: no game files, PS4 packages, `eboot.bin`, `user`, `user_data`, saves, profiles, logs,
caches, dumps, symbols, RenderDoc or Vulkan layers, updater workspaces or `trg-updater.log`, developer paths or
`DEBUG_SAULO` text. The checker also refuses a DLL that nothing imports, a stale ANGLE file, an `alienation.exe` whose
version differs from the folder name, and a `trg-updater.exe` that imports anything but Windows.

`trg-updater.exe` is built by the same MSYS2 CLANG64 CMake build as the game, linked `-static`: its only imports are
`KERNEL32`, `SHELL32` and the Windows UCRT (`api-ms-win-crt-*`), so it runs from `%TEMP%\trg-update-*` without any DLL of
the installation. Its debug information goes to `dist\AlienationPC-v<version>-windows-x64-symbols\trg-updater.debug`,
outside the ZIP.

`tools\test-release-check.ps1` (part of `tools\test.ps1`) runs the checker on placeholder folders and ZIPs that each add
one forbidden item or damage one thing.

## Linux AppImage

    tools/package-linux.sh            # on Ubuntu 20.04 or WSL
    tools/package-linux.sh --verify-only dist/AlienationPC-v<version>-x86_64.AppImage

`tools\package-release.ps1` runs it in the `AlnFocalBuild` WSL distribution (glibc 2.31 baseline, unchanged) and writes
`AlienationPC-v<version>-x86_64.AppImage` and `.AppImage.sha256`. See [LINUX.md](LINUX.md).

## Publishing a release

The updater accepts only the latest stable GitHub release of `saulob/alienation-pc`, tagged `v<version>` (`v` prefix
optional), with these assets next to each other:

| Asset | Content |
| --- | --- |
| `AlienationPC-v<version>-windows-x64.zip` | the package above |
| `AlienationPC-v<version>-windows-x64.zip.sha256` | `<sha256>  AlienationPC-v<version>-windows-x64.zip` |
| `AlienationPC-v<version>-x86_64.AppImage` | the Linux AppImage |
| `AlienationPC-v<version>-x86_64.AppImage.sha256` | `<sha256>  AlienationPC-v<version>-x86_64.AppImage` |

The SHA-256 is trusted from the GitHub asset digest first and from the `.sha256` asset otherwise. A release without one of
them is never installed. Drafts and prereleases are ignored. Release notes: `docs\releases\v<version>.md`.

## How the launcher updates itself

1. One asynchronous check per launcher session; a failure is silent and never blocks Play. A repository with no
   published release (GitHub answers 404 for `latest`) is not an error.
2. A newer stable release shows the banner in the header: version, release notes link and UPDATE NOW. Nothing is installed
   without that click.
3. UPDATE NOW downloads the ZIP, checks its size and SHA-256, extracts it into a temporary staging folder (never over the
   installation) with the path, link and size checks, resolves the staging root (flat, or exactly one wrapper folder) and
   builds the manifest of release-owned files.
4. RESTART TO UPDATE copies the installed `trg-updater.exe` to `%TEMP%\trg-update-*`, writes the job there and quits the
   launcher. The helper waits for the launcher, backs up the release-owned files, replaces them, verifies the installed
   files, restarts `alienation.exe` with `--launcher` (the original arguments are kept; `--aln-skip-launcher` and
   `--launcher` are dropped first) and removes the backup, staging folder and job. On any failure it restores the backup
   and restarts the previous program. The helper writes `trg-updater.log` in its workspace.
5. A release older than the updater does not need to contain `trg-updater.exe`: the running build supplies the helper.

Protected, never part of the manifest and never touched: `game`, `user`, `user_data`, `user_data.migrating`, `logs`,
`cache`, `content`, `save`, `saves`, `savedata`, `settings`, `profiles`, `achievements`, `trophy`, `trophies`,
`screenshots`, `online-instances`, `sce_sys`, `sce_module`, `prepared`, `extracted`, `image`, `dlc`, the title folders
`CUSA00062` and `CUSA04430`, `alienation.ini`, `keys.json`, the last run logs and the original game's files
(`eboot.bin`, `param.sfo`, `boot.bin`, `content.bin`, `image.json`, …), and the file types `.pkg .sfo .self .sprx .prx
.elf .cpk .psarc .plt .trp .sav .dat .log .dmp .pdb .ilk .obj .bin .ini .json .key .meta .spv .part .bak .tmp .zip .7z
.rar .tar .gz .appimage`. The release carries no configuration file: the player's settings are
`user\alienation.ini`, user-owned and never replaced.

On Linux the AppImage is the whole installation. The launcher replaces the AppImage the run was started from (`APPIMAGE`,
which must be an `AlienationPC-v<version>-x86_64.AppImage` outside its own mount, with the running program inside
`APPDIR`) with a verified sibling file, keeps its permissions, and starts it again; if it cannot be started, the previous
AppImage is put back. `user` and `game` stay beside the AppImage, never inside the mount.

## Testing the banner without a public release

Always from a disposable extraction of the package, never from the development folder. The master switch makes the other
variables effective; without it they do nothing.

    $env:ALN_UPDATE_TEST = '1'
    $env:ALN_UPDATE_TEST_CURRENT_VERSION = '0.0.9'             # reported as the running version
    $env:ALN_UPDATE_TEST_RELEASE_JSON = 'C:\path\release.json'  # GitHub release JSON used instead of the network
    $env:ALN_UPDATE_TEST_ASSET_SHA256 = '<sha256 of the ZIP>'   # trusted digest for the injected release

The variables are removed from the environment of the restarted program; everything else is kept.

With no public release, the banner, the release notes link, UPDATE NOW, the verification states and the retry/error state
can be checked, but the download itself ends in a 404 from GitHub: that is expected, and a real replacement can only be
tried once the release exists. The replacement itself (backup, replace, verify, restart, rollback, development executable
replaced by a public release of the same version) is covered by `aln-update-tests` and `aln-update-helper-tests`.

### Local test, step by step

    $root = 'S:\VisualStudio\repos\alienation-pc'
    Set-Location $root
    $version = (Select-String -Path CMakeLists.txt -Pattern '^project\(alienation_pc VERSION ([0-9.]+)').Matches[0].Groups[1].Value
    $name = "AlienationPC-v$version-windows-x64"

    # 1. The official package, then its checks again from the ZIP.
    .\tools\package.ps1
    .\tools\package.ps1 -VerifyOnly "dist\$name"
    .\tools\package.ps1 -VerifyOnly "dist\$name.zip"

    # 2. A disposable extraction.
    $test = Join-Path $env:TEMP 'alienation-updater-test'
    Remove-Item $test -Recurse -Force -ErrorAction SilentlyContinue
    Expand-Archive "dist\$name.zip" $test
    $exe = Join-Path $test "$name\alienation.exe"

    # 3. The release JSON for the generated ZIP.
    $zip = Get-Item "dist\$name.zip"
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    $url = "https://github.com/saulob/alienation-pc/releases/download/v$version/$($zip.Name)"
    $json = Join-Path $test 'release.json'
    @{ tag_name = "v$version"; draft = $false; prerelease = $false
       html_url = "https://github.com/saulob/alienation-pc/releases/tag/v$version"; body = 'Updater test'
       assets = @(@{ name = $zip.Name; size = $zip.Length; browser_download_url = $url; digest = "sha256:$hash" }) } |
        ConvertTo-Json -Depth 5 | Set-Content $json -Encoding utf8

    # 4. Test mode, then start the extracted program by hand (--launcher shows the launcher).
    Get-ChildItem Env:ALN_UPDATE_TEST* | Remove-Item
    $env:ALN_UPDATE_TEST = '1'
    $env:ALN_UPDATE_TEST_CURRENT_VERSION = '0.0.9'
    $env:ALN_UPDATE_TEST_RELEASE_JSON = $json
    $env:ALN_UPDATE_TEST_ASSET_SHA256 = $hash
    & $exe --launcher

    # 5. Afterwards.
    Get-ChildItem Env:ALN_UPDATE_TEST* | Remove-Item
    Get-ChildItem (Join-Path $env:TEMP 'trg-update-*\trg-updater.log') -ErrorAction SilentlyContinue

Check the launcher subtitle and About page (`v<version>`), the banner (Update available, Version, You are using 0.0.9,
Release notes, UPDATE NOW), and the download attempt, at 100%, 150% and 225% display scaling. Without a public release
the download ends in the 404 error state with RETRY: expected.
