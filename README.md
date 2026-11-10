# Alienation PC

A native PC port of Housemarque's **Alienation** (PS4). The game's original x86-64 code runs directly on the PC through
replacement PS4 system libraries and a Vulkan renderer derived from shadPS4. The port's foundation is the
[Dragon's Crown Pro PC](https://github.com/saulob/dragons-crown-pro-pc) runtime, launcher and renderer.

**Status: early development.** The launcher, package installation and boot image are in place for two builds; the
game itself has not been verified to boot yet. See [docs/FIRST_BOOT.md](docs/FIRST_BOOT.md).

No game files are included. You need your own decrypted Alienation PS4 package.

## Supported builds

| Title ID | APP_VER | Content ID | eboot.bin SHA-256 |
|---|---|---|---|
| `CUSA00062` | `01.00` | `EP9000-CUSA00062_00-ALIENATIONGAMEEU` | `f5f41f870db752add56920c5538441bc3750b1442f69c1de3895ee282887d855` |
| `CUSA04430` | `01.00` | `HP9000-CUSA04430_00-ALIENATIONGAMEKR` | `11928457792fdba30aee3ceb9f5fb300bac720d0f3c1774825a8380a2dc9b72e` |

The `HP` publisher prefix and `KR` suffix of CUSA04430's content ID identify an Asian (Korean) release, whatever name
the package file has. Each build is installed in its own folder (`game\prepared\<TITLE_ID>-1.00`); choosing the package
of an installed build again makes it the active one. The European 1.06 update is not supported yet and is refused.

## Features

Working in the launcher:

- Install from your own `.pkg`, with the build identified by its metadata and executable hash; other builds are refused
- Graphics card and renderer selection, window mode, monitor, VSync, presentation, SMAA, anisotropic filtering,
  FSR 1 and RCAS, frame counter, render resolution (native to 4K) and aspect ratio (16:9, 16:10, 21:9, 32:9, 4:3, 5:4),
  and a 59.94 FPS engine target; the last three are untested in game
- Keyboard mapping, Xbox and PlayStation controllers, up to four local players' controllers
- Trophies page, bug report page, update check against this repository, GitHub Sponsors link

Not available yet:

- Online play: the Online page is present but disabled; the launcher makes no online connection

## Building (Windows)

Requires MSYS2 CLANG64 (`C:\msys64` or `MSYS2_ROOT`). `tools\setup.ps1` reports what is missing.

```powershell
tools\build.ps1
```

The executable is `build\cmake\clang64\bin\alienation.exe`. Start it yourself; the launcher opens and installs the game
from your package. Game logs go to `build\logs\alienation_NNN.log`, settings to `user\alienation.ini`. The build is
incremental; `-Clean` rebuilds from scratch. Every build run writes one new, never overwritten log,
`build\logs\build\alienation_NNN.log`, with the tool output, warnings, errors and the exit code.

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
