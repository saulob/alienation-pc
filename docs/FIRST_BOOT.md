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
| Supported | yes (primary) | yes | installed and recognized, guest patches not ported ([GAME_BUILDS.md](GAME_BUILDS.md)) |

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

CUSA00062 1.06's `eboot.bin` is `c65c18865691d7288055cae70af983aa86898d59936976ef12f5eb033eda7243`; its two modules are the
same as above. Its layout and imports are compared with 1.00 in [GAME_BUILDS.md](GAME_BUILDS.md).

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
both builds; the localization database is in the game data) matches each build.

## Frame rate

The engine starts at 59.94 FPS (`SetFps(59.94f)` at EUR `0x1402f4f`). The display turns the rate into a flip rate:
`flip_rate = clamp(round(1 / (fps * vblank_period)) - 1, 0, 2)` (`0x1fbc20`), so 59.94 flips every vblank and 29.97
every second one. The game's scripts (`stream.dat`) call `engine_set_fps` five times with a float constant (two with
29.97, three with 59.94), so the 30 FPS the port showed comes from those script calls, not from the port's vblank.

The port makes the binding return at once (EUR `0x2507b0`, EUR 1.06 `0x253f10`, Asia `0x2513c0`: `mov rax,[rdi+0x28]` becomes
`xor eax,eax; ret`; `launcher/frame_rate.c`), so the startup 59.94 stays and the delta time stays `1 / fps`. Nothing
else is touched. Static evidence only: whether Havok stepping, animation, scripted timers and cutscenes stay correct at
59.94 in the campaign levels, which were tuned at 29.97, needs a gameplay test.

The community shadPS4 patch (halves the delta at EUR `0x14035FC` / `0x1403632` and needs a 120 Hz vblank) is not used.

### The game's timing

The engine's frame update (EUR `0xf20e0`) measures the wall-clock time since the last frame with `sceKernelReadTsc`
and hands it to the stepper (`0xf1b90`) with the step `1 / fps` (`1 / 59.94`), a clamp of 8 steps and the max frame
skip (4). The stepper always runs one fixed step (input, game logic scaled by `an__delta_t_multiplier`, world), then
more while the wall-clock lag is a whole step or more: at most 5 a frame, none after a step that took longer than real
time, and the lag it cannot catch up is dropped. Then one variable update gets the measured delta and the render update
gets a fixed `1 / 30` (`sasquatch__fixed_render_frame_rate`, on by default). The frame ends by submitting the flip and
waiting for it on the flip event queue (`0x1fb700`); the flip completes on the guest vblank. The online lockstep
objects (`determinism_sync_*` script natives: per-frame data and a frame counter check), the NP callbacks (Matching2,
sessions) and the local players' pads are all driven by the game's own frames and steps.

So the game makes one frame per flip at 60 Hz and runs at least one step per frame: frames flipped faster than its step
would make the whole game faster, and a slower host is caught up from the wall clock, as on the console. The same code
is in every build (`tests/patch_site_tests.py` checks it on each executable; `docs/GAME_BUILDS.md` has the offsets).

### Frame rate setting

The Frame rate setting (`frame_rate`: 30, 60, 90, 120, 144, 165, 240, 0 for Unlimited; 60 by default) is presentation
only and changes nothing in the game: the guest vblank stays 60 Hz and every flip is taken and completed on time. 30
shows every second flip (a game already flipping every second vblank is shown whole). Above 60 the present thread shows
the last frame again on the slots of the target rate that fall before the next vblank (`gpu/shim/aln_frame_pacing.h`):
the game still makes 60 new frames a second, so there is no interpolation and motion is as smooth as at 60. VSync bounds
the repeats to the display's refresh (a display at 60 Hz has nothing to repeat), Unlimited repeats up to 480 a second,
and it applies at start and
when changed in the Options menu. The log line is
`VideoOut: vblank 60 Hz, frame rate 144, repeat cadence 144 FPS, VSync on, display 144.00 Hz`.

Frame rate in title counts every presentation; while frames are repeated the game's new frames follow in brackets
(`FPS: 144.00 (game 60.00)`). The frame counter (F2) shows the presentation rate and interval.

Known, not caused by the setting: the guest vblank is 60.000 Hz and the step `1 / 59.94`, so the stepper's clock runs
0.1% ahead of the wall clock (about 3.6 s an hour). While that lead lasts, a stretch the host renders below 60 is not
caught up and plays slower, until the lead is used up.

## Render resolution and aspect ratio

The engine's display configuration defaults to 1920 x 1080 (`mov dword [rdi+0x34], 1080` / `mov dword [rdi+0x30], 1920`
in the constructor at EUR `0x1fa8a0`). The display builds its frontbuffers and the
`RENDER_RESOLUTION_X/Y` every render script sizes its targets from out of that pair. The launcher writes the chosen size
into both immediates (EUR image offsets `0x1fa8da` / `0x1fa8e1`, EUR 1.06 `0x1fd98a` / `0x1fd991`, Asia `0x1fb4ea` /
`0x1fb4f1`; every build's sites are in [GAME_BUILDS.md](GAME_BUILDS.md)) after checking the
original instruction. The AvPlayer surfaces (EUR `0xcbf8d`, `0xcc011`) stay 1080p on purpose.

The constructor's heap sizes are not used: the game's startup function (EUR `0x380` and `0x440`, 1.06 and Asia the same
offsets) overwrites them with 1 GiB, `0x46000000` and `0x14000000` (`mov qword [rbp-0x78], imm32` at `0x3bb` and
`0x47b`), the three direct allocations in the log. The first is the heap whose allocator (vtable `0x1f5dfb0`, allocate
at `0x12a22b0`) serves the render targets and textures (the manager's `+0x10` getter has 25 callers). A full heap
makes it return a null block, which the texture constructor at EUR `0xfa870` stores and then clears with `memset`
(call at `0xfaaa6`): that is the crash at 3360 x 1440. The port grows that one literal with the target's pixel count
(2 MiB steps, below 2 GiB, the limit of the 32-bit immediate; native and smaller targets keep 1 GiB) in both
startup functions and gives the direct pool the same amount on top of 5056 MiB. The other two heaps are left alone.

Every run now logs `Patch:` (the display size read back from the image), `VideoOut: ... display buffers registered`,
the AvPlayer lifecycle and, when guest flips stop for 5 s, a thread snapshot (`ALN_BOOT_DIAGNOSTIC=0` turns that
off). A normal exit logs `Process: exit()` or `Window: closed`; `tools/build.ps1 -Run` appends the exit code to the
log. A run that ends with none of these was ended from outside the process or by a fast-fail (Windows reports
in the Application event log).

Not verified: that the camera and HUD follow the new shape (a 16:9 default of 1.7777 sits at EUR `0x17ccfd0` and
`2/1920`, `2/1080` at `0x17c0d78`), that every post-process target scales with the script globals, and whether the
graphics memory pools hold 1440p and above. These need a gameplay test.

## Crash reports (Windows)

An unrecovered exception writes `CRASH:` into the numbered log: the exception and access, `rip` with its module or guest
offset, the fault address and its memory state, the startup phase, render size, last video and last checkpoint, then
the detailed report (registers, frames, modules, recent guest memory operations) and `alienation_NNN.dmp` next to the
log (thread stacks and the module list, no game memory). The process ends with the exception code, which
`tools/build.ps1 -Run` prints and appends to the log. `alienation_NNN.dmp` always belongs to `alienation_NNN.log`; its
comment stream names the process, exception and log. A fault inside a library (FFmpeg, a driver, the C runtime) is logged
first as `Diagnostics: first-chance ... UNCONFIRMED candidate` with registers, the code addresses on the stack (the
caller of the library function) and the recent guest memory operations, and left to its owner when a handler can be
reached from that stack. Guest code has no unwind data, so below a guest frame no handler and no unhandled-exception
filter is ever reached and Windows ends the process silently: such a fault is reported at once. Overlay and capture
DLLs can replace the filter: it is checked every second and after the GPU starts, and a replacement is logged and
undone. `build.ps1 -Run` prints the last first-chance fault when a run ends without a crash report, as unconfirmed.
