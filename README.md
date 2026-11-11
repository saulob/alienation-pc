# Alienation PC

A native PC port of Housemarque's **Alienation** (PS4). The game's original x86-64 code runs directly on the PC through
replacement PS4 system libraries and a Vulkan renderer derived from shadPS4. The port's foundation is the
[Dragon's Crown Pro PC](https://github.com/saulob/dragons-crown-pro-pc) runtime, launcher and renderer.

**Status: early development.** The launcher, package installation and boot image are in place for every supported
build; in-game behaviour is tested by hand. See [docs/FIRST_BOOT.md](docs/FIRST_BOOT.md).

No game files are included. You need your own decrypted Alienation PS4 package.

## Supported builds

| Build | Title ID | Version | Packages | Content ID | eboot.bin SHA-256 |
|---|---|---|---|---|---|
| CUSA00062 1.00 · EUR | `CUSA00062` | 1.00 | base | `EP9000-CUSA00062_00-ALIENATIONGAMEEU` | `f5f41f870db752add56920c5538441bc3750b1442f69c1de3895ee282887d855` |
| CUSA00062 1.06 · EUR | `CUSA00062` | 1.06 | base + update, or the DUPLEX single-package repack | `EP9000-CUSA00062_00-ALIENATIONGAMEEU` | `c65c18865691d7288055cae70af983aa86898d59936976ef12f5eb033eda7243` |
| CUSA04430 1.00 · ASIA/KR | `CUSA04430` | 1.00 | base | `HP9000-CUSA04430_00-ALIENATIONGAMEKR` | `11928457792fdba30aee3ceb9f5fb300bac720d0f3c1774825a8380a2dc9b72e` |

The `HP` publisher prefix and `KR` suffix of CUSA04430's content ID identify an Asian (Korean) release, whatever name
the package file has. Each build is installed in its own folder (`game\prepared\<TITLE_ID>-<version>`); choosing the
package of an installed build again makes it the active one. The USA release (CUSA04181) is not supported: no game data
of it was available to verify.

Every build has the same port features: render resolution, aspect ratio and ultrawide, GPU heap scaling, the 59.94 FPS
engine target, Skip Intro, Exit Game and the Xbox prompts, each on its own verified code. The Xbox prompts stay Xbox in
every text language, Korean, Japanese and Chinese included. Menus confirm with Cross (A) and cancel with Circle (B) by
default; **Asian Menu Buttons** (Controls page) switches CUSA00062 1.06 and CUSA04430 to Circle confirms and Cross
cancels, the PS4 Asian convention, without changing gameplay buttons or prompt style. The DUPLEX package is the official
1.06 game in one package: same executable and game data, installed with **Install game** and run as CUSA00062 1.06.
[docs/GAME_BUILDS.md](docs/GAME_BUILDS.md) has the parity table, how each entry is verified and what is still untested
in game.

### Installing an update

Install the base game first (**Install game...**), then press **Install update...** on the Play page and select the update
package. The launcher checks the package's metadata (title, update category, version), refuses a base package, another
title's update or an older update before writing anything, lays the update over the installed base in a staging folder,
checks it and builds its boot image, and only then switches to it. A failed or cancelled update leaves the installed game
as it was. Saves and settings are never touched. **Remove update** returns to the base game. The DUPLEX package already
holds the update: install it with **Install game**. Install update then refuses the official 1.06 update before changing
anything ("This game already contains version 1.06"), and Remove update is not offered: there is no update layer.

## Features

Working in the launcher:

- Install from your own `.pkg`, with the build identified by its metadata and executable hash; other builds are refused
- Install update and Remove update, staged and checked before the installed game changes
- Graphics card and renderer selection, window mode, monitor, VSync, presentation, SMAA, anisotropic filtering,
  FSR 1 and RCAS, frame counter, render resolution (native to 4K) and aspect ratio (16:9, 16:10, 21:9, 32:9, 4:3, 5:4),
  and a 59.94 FPS engine target, in every supported build
- Keyboard mapping, Xbox and PlayStation controllers, up to four local players' controllers
- Trophies page, bug report page, update check against this repository, GitHub Sponsors link

In game:

- Exit Game as the last row of the Options menu (after Credits) and of the Select Campaign menu, in the game's language

Not available yet:

- Online play: the Online page is present but disabled; the launcher makes no online connection

## Building (Windows)

Requires MSYS2 CLANG64 (`C:\msys64` or `MSYS2_ROOT`). `tools\setup.ps1` reports what is missing.

```powershell
tools\build.ps1
```

The executable is `build\cmake\clang64\bin\alienation.exe`. Start it yourself; the launcher opens and installs the game
from your package. Game logs go to `build\logs\alienation_NNN.log`, settings to `user\alienation.ini`. The build is
incremental; `-Clean` rebuilds from scratch. Every build run writes one new, never overwritten log with the tool
output, warnings, errors and the exit code: `build\logs\build\build_NNN.log`, or with `-Run` the session log
`build\logs\alienation_NNN.log`, whose `[build]` section the game's `[run]` lines then continue.

`tools\test.ps1` builds and runs the focused headless tests and the static script tests (`tests\*.ps1`).

## Layout

| Path | Content |
|---|---|
| `src/` | Loader and PS4 runtime (libkernel, libc contracts, threads, memory, files, audio, pad, save data, services) |
| `gpu/` | Video core (shadPS4-derived GNM to Vulkan), in-game overlay and options menu, AvPlayer on FFmpeg |
| `launcher/` | Launcher, package extraction and installation, boot image builder, settings |
| `scripts/` | Development-only Python tools (package inspection, extraction, boot image, import names) |
| `tools/` | Build, test, package and inspection scripts |

## License

GPL-2.0-or-later for the port's own code, GPL-2.0-only for the libSceJson2 code ported from AnyPS5; third-party components keep their licenses (`packaging/licenses`).
Alienation is a trademark of its rights holders; this project is not affiliated with or endorsed by Housemarque or
Sony Interactive Entertainment.
