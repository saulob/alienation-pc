# First boot: what is known statically

Read from the user's own packages without running game code. It says what the loader can handle, not that the game runs.

## Packages

| | CUSA00062 base | CUSA04430 | CUSA00062 update |
|---|---|---|---|
| Content ID | `EP9000-CUSA00062_00-ALIENATIONGAMEEU` | `HP9000-CUSA04430_00-ALIENATIONGAMEKR` | same as the base |
| CATEGORY | `gd` | `gd` | `gp` |
| APP_VER / VERSION | 01.00 / 01.00 | 01.00 / 01.00 | 01.06 / 01.00 |
| SYSTEM_VER | `0x03008000` | `0x03008000` | `0x03008000` |
| ATTRIBUTE / PARENTAL_LEVEL | 2 / 7 | 0 / 9 | |
| Pubtool date | 2018-06-02 | 2018-03-11 | 2018-06-02 |
| Image key | 3: fake PKG | 3: fake PKG | 3: fake PKG |
| Header vs file size | match | match | match |
| Supported | yes (primary) | yes | no: refused until analysed |

CUSA04430's content ID (`HP` publisher prefix, `KR` suffix) is an Asian Korean release, not a USA one. Neither base
param.sfo has `USER_DEFINED_PARAM_*`, so both content profiles are `ALNCONT1`, SKU 3 and four zeros.

Contents: `eboot.bin`, `sce_module/{libc,libSceFios2}.prx`, `sce_sys/`, `sce_discmap.plt`, `audio.pck` (Wwise AKPK),
`stream.dat`, `video/{splash,intro,loading,title}.mp4`, `Havok/`, `Navmeshes/`. CUSA04430 also has `video/splash_SIE.mp4`
(955 files against 954); `audio.pck` and `stream.dat` differ in size between the builds.

## Executable fingerprints

| File | CUSA00062 1.00 | CUSA04430 1.00 |
|---|---|---|
| `eboot.bin` | `f5f41f870db752add56920c5538441bc3750b1442f69c1de3895ee282887d855` | `11928457792fdba30aee3ceb9f5fb300bac720d0f3c1774825a8380a2dc9b72e` |
| `sce_module/libc.prx` | `e436eb5bd6bc6dd706edea917da1e52460ce760971b8f067149f479fd3410bfe` | same |
| `sce_module/libSceFios2.prx` | `cf4f3e57d05df4cd22a54de08ff790d5aca979f02bd5ed5c70c10ac7f5f69410` | same |

## Executables

Both are plaintext SELFs, ELF type `0xfe10` at image base 0, procparam SDK `0x3008041`, 11 program headers, 44 NEEDED
libraries, the same library and module ids and the same 654 imports. The layouts differ, so no address carries over:

| | CUSA00062 1.00 | CUSA04430 1.00 |
|---|---|---|
| Entry | `0xd75c0` | `0xd8180` |
| Code (memsz) | 0 (`0x1ea0210`) | 0 (`0x1e97da8`) |
| SCE_RELRO | `0x1ea4000` | `0x1e98000` |
| Data (filesz / memsz) | `0x2180000` (`0x4fcb40` / `0xb6c678`) | `0x2174000` (`0x4fcb80` / `0xb6ca18`) |
| Static TLS | `0x2180060`, 32 bytes, none initialized | `0x2174048`, 32 bytes, none initialized |
| Relocations | 372,658 (369,879 relative) | 372,664 (369,885 relative) |
| `mov rax, fs:[0]` sites | 8,338 | 8,338 |

The boot image builder rewrites the TCB loads of both, and its static TLS sits below the TCB as the runtime lays it out.
Both boot images are 49,628,528 bytes with both modules linked; `libc`'s `_init_env` is a bare RET. The launcher
(`launcher/boot_image.cpp`) and `scripts/prepare_image.py` produce byte-identical `boot.bin`, `content.bin` and
`image.json` for each build.

## Engine evidence

Housemarque's own engine (`C:\Work\Projects\AlienNation\Game\Code\...`), Lua 5.1.4 game scripts, Havok 2016-03-31,
Wwise with both `AK::LibAudioOut` and `AK::SceAudio3dSink`, Telemetry. `audio.pck` (CUSA04430) holds Wwise Vorbis (2007
files) and Wwise ADPCM (1536) only: no ATRAC9 media. The frame rate is set by script (`engine_set_fps`: 59.94 or 29.97,
with `engine_set_max_frame_skip`), and a `determinism_sync` layer keeps networked games in lockstep. Local co-op appears
as `local_player_count` and four-controller handling.

## Imports

746 imports in each boot image (eboot and both modules); the two builds' import lists are identical:

| | Count |
|---|---:|
| Bound to the game's own `libc.prx` / `libSceFios2.prx` | 204 |
| Bound by the runtime | 449 |
| Bound by the video core (Gnm, VideoOut, event queues, AvPlayer) | 93 |
| Unbound: stop with `STOP: first unsupported PS4 import` when called | 0 |

The 47 online and social imports that used to stop the game are bound: `libSceJson2` is complete (all 54 imports: the
container classes copy deeply, values serialize, `referObject` creates the object), the IME keyboard registers a handler
(the game's handler only reads key-down `0x101` and key-up `0x102` events into a key table; none is delivered yet),
and everything that needs PlayStation Network answers as signed out: NpScore (6), NpWebApi service push events (4),
NpMatching2 (3), the invitation dialog (initializes, cannot open), HTTPS callbacks (2), `sceSslGetSerialNumber`
(invalid certificate id), `sceNetGetSockInfo` (unreachable). Nothing authenticates, connects or sends data.

Added for this game: `libSceJson2` (33 imports, ported through Resogun PC from AnyPS5), `libSceAudio3d` as a stereo
renderer over AudioOut, `libSceAjm` contexts and instances without a decoder (a decode request stops explicitly),
`libSceAvPlayer` from shadPS4 on FFmpeg, RFC 3339 time formatting, kernel local-time conversions, thread ids and
scheduling parameters, user colours and a few dialog calls.

## Not known until the first boot

Whether startup reaches the game's entry, the first call the runtime cannot answer, the renderer's behaviour with this
game's shaders, the sound path Wwise picks (Audio3d or AudioOut), frame pacing, and whether the language list (shared by
both builds; the localization database is in the game data) matches each build. Render resolution and aspect ratio stay
native until sites are verified in each executable.

## 60 FPS patch (CUSA00062 1.00 only, not applied)

The community shadPS4 patch "60 FPS v0.2 - application timestep x0.5 - VBlank 120 Hz" (r3ntonboy, `TitleID` CUSA00062,
`AppVer` 01.00, `AppElf` eboot.bin, its note: "Gameplay validation pending") writes two 8-byte lines. shadPS4 places
eboot patch addresses relative to 0x400000, which puts them at the EUR image offsets below:

| Patch address | Image offset | Original | Patched |
|---|---|---|---|
| `0x018035FC` | `0x14035FC` | `C5FA5905F4B83801` `vmulss xmm0,xmm0,[0x278EEF8]` | `C5FA5905B0A53000` `vmulss xmm0,xmm0,[0x170DBB4]` |
| `0x01803632` | `0x1403632` | `C5FA5905BEB83801` `vmulss xmm0,xmm0,[0x278EEF8]` | `C5FA59057AA53000` `vmulss xmm0,xmm0,[0x170DBB4]` |

Each line keeps the instruction and changes only its RIP-relative operand. `0x278EEF8` is the engine tunable
`an__delta_t_multiplier` (zero-initialized data, set to 1.0 and registered by a static initializer at `0xcc840`); these
two instructions are its only readers. `0x170DBB4` is a read-only `0.5f`. The readers are two adjacent virtual
functions: the application update at `0x1403610`, which scales the frame delta once and passes it to two subsystems,
and `0x14035F0`, which forwards the scaled delta to one child. So the patch fixes the delta at half of each frame's
time, and the game keeps normal speed only if it also runs twice as many frames, which the patch gets from a 120 Hz
vblank.

The port does not apply it:

- The runtime's vblank is a fixed 60 Hz (`gpu/shim/core/emulator_settings.h`, checked by `tests/frame_pacing_tests.cpp`).
  The patch alone would run the game at half speed.
- Nothing shows yet that every timed system (Havok stepping, animation, the frame-skip catch-up, scripted timers and
  `determinism_sync`) follows the scaled delta, rather than its own clock or a fixed step.
- `an__delta_t_multiplier` is a tunable, so a script that sets it is overridden.
- At the same addresses, CUSA04430 holds unrelated bytes, so the patch must never be applied to that build.

What remains is a 120 Hz vblank mode limited to CUSA00062 1.00, and a gameplay test of movement, physics, weapons,
timers and cutscenes at 60 FPS against the same scenes at 30 FPS.
