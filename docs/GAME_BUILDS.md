# Game builds and updates

The port recognizes a build from its packages' metadata (`param.sfo` TITLE_ID, CONTENT_ID, CATEGORY, APP_VER, VERSION)
and from the SHA-256 of the finished game's `eboot.bin`, never from a file name. The build table is
`launcher/game_profile.cpp`, mirrored by `scripts/game_profiles.py`; `tests/game_profile_tests.py` checks that they agree.

## Supported builds

| Build key | Region | Packages | eboot.bin SHA-256 | Status |
|---|---|---|---|---|
| `CUSA00062-1.00` | Europe | base (`gd` 01.00) | `f5f41f870db752add56920c5538441bc3750b1442f69c1de3895ee282887d855` | primary build |
| `CUSA00062-1.06` | Europe | base + update (`gp` 01.06 / VERSION 01.00) | `c65c18865691d7288055cae70af983aa86898d59936976ef12f5eb033eda7243` | install and detection only; no guest patches yet |
| `CUSA04430-1.00` | Asia (Korea) | base (`gd` 01.00) | `11928457792fdba30aee3ceb9f5fb300bac720d0f3c1774825a8380a2dc9b72e` | supported |

Each build has its own folders, `game\prepared\<build key>` and the boot image folder `<build key>`. The active build is
the complete one prepared last. 1.00 and 1.06 can be installed side by side.

## Feature availability

Every guest patch is tied to one build's verified code. A build that is not listed for a feature never gets it: the
loader logs the feature as unavailable and the game runs unpatched. `tests/game_profile_tests.py` fails when a build
without verified sites is named by any patch table.

| Feature | CUSA00062 1.00 | CUSA00062 1.06 | CUSA04430 1.00 |
|---|---|---|---|
| Install, detection, boot image | yes | yes | yes |
| Render resolution, aspect ratio (incl. ultrawide), GPU heap scaling | yes | no: forced to native 16:9 | yes |
| 59.94 FPS engine target (`engine_set_fps`) | yes | no: the game's own frame rate | yes |
| Skip Intro | yes | no | yes |
| Exit Game (Options, Select Campaign) | yes | no | yes |
| Xbox controller prompts | yes | no: PlayStation prompts | yes |
| SMAA, FSR, filtering, presentation (renderer side, no guest patch) | yes | yes | yes |
| Online (lobby, Matching2, P2P; unverified in game) | own build | own build: never meets 1.00 | own build |
| Tested in game | as before this change | not yet: boot and gameplay untested | as before this change |

## Install update

1. Install the base game with **Install game...** (Play page).
2. Once it is ready, the Play page shows **Install update...**. Select the update `.pkg` (any file name).
3. The launcher reads the package's `param.sfo` and refuses, before writing anything:
   - a base package (`This is the base game package`), or an update chosen in Install game (`This is an update package`);
   - an update of another title than the active game (`This update is for CUSA00062, but the installed game is CUSA04430`);
   - an update older than the active update of the same title (`A newer update is installed`);
   - another game, an unknown version (logged with its TITLE_ID, APP_VER, VERSION, CATEGORY and executable hash), a
     package encrypted for a console, an incomplete download, a package inside the port's own game folders.
4. The update's base build must be installed and valid (`Base game required` otherwise).
5. The update is unpacked to `<build>.install-update`, the base's files are hard-linked into `<build>.install-temp` (the
   base is never written: replaced files are unlinked, not written through), the update's files are moved over them, the
   result is checked (identity, required files, executable hash), and its boot image is built and checked in
   `<boot image>.install-temp`. Only then are both folders swapped in by rename; a failed swap puts the previous ones back.
6. Any failure or cancellation before the swap removes the staging folders and leaves the installed game as it was. Saves,
   settings, logs, trophies, online identity, friends and bug reports live under `user\` and `logs` and are never touched.
7. The launcher re-reads the active build and shows it, for example `Ready · CUSA00062 1.06`, `APP_VER 1.06 · VERSION 1.00`,
   and logs `package recognized: ... is CUSA00062 1.06 (update, ..., laid over CUSA00062 1.00)`.

**Remove update** (Play page, when an update of the active title is installed) removes the update's two folders only,
after renaming them aside, and makes the base active again. A removal that was interrupted is finished at the next start.
Install game with the base package of an installed build also makes that build active again without copying.

The space check counts the update's full size (about 1.6 GB for 1.06); the base's files are links and cost nothing on
the same drive.

## CUSA00062 1.06

Read from the user's update package without running game code (`scripts/pkg_info.py`, the launcher's package reader).

| | 1.00 base | 1.06 update |
|---|---|---|
| CATEGORY / APP_VER / VERSION | `gd` / 01.00 / 01.00 | `gp` / 01.06 / 01.00 |
| Content ID | `EP9000-CUSA00062_00-ALIENATIONGAMEEU` | same |
| SYSTEM_VER, PUBTOOLVER, c_date | `0x03008000`, `0x2890000`, 2018-06-02 | same |
| Package | 2,113,142,784 bytes, 25 entries | 1,582,694,400 bytes, 29 entries, no delta info |
| Files | 954 | 27: `eboot.bin`, `stream.dat`, `audio.pck`, `sce_module/*`, `sce_discmap*.plt`, `sce_sys/about/right.sprx`, 19 `Navmeshes/*` |

The update replaces the whole `stream.dat` (1,256,531,200 bytes against 1,226,730,688) and `audio.pck` (275,882,690 against
250,270,913) and adds `sce_discmap_patch.plt`. Its `libc.prx`, `libSceFios2.prx` and `right.sprx` are byte-identical to
1.00's. Neither version sets `INSTALL_DIR_SAVEDATA`.

### Executable

| | 1.00 | 1.06 |
|---|---|---|
| Size | 49,417,227 | 49,418,907 |
| Entry | `0xd75c0` | `0xda320` |
| Code (memsz) | 0 (`0x1ea0210`) | 0 (`0x1e9f300`) |
| SCE_RELRO | `0x1ea4000` | `0x1ea0000` |
| Data (filesz / memsz) | `0x2180000` (`0x4fcb40` / `0xb6c678`) | `0x217c000` (`0x4fcb80` / `0xb6d9b8`) |
| Image end | `0x2cec678` | `0x2ce99b8` |
| Static TLS | `0x2180060`, 32 bytes | `0x217c048`, 32 bytes |
| Procparam SDK | `0x3008041` | `0x3008041` |
| Relocations | 372,658 | 372,824 |
| Imports (eboot) | 654 | 660 |
| Boot image | 49,628,528 bytes, 8,338 TLS loads rewritten | 49,628,528 bytes, 8,338 TLS loads rewritten |

The same 44 NEEDED libraries. 7,824 of the 7,840 4 KiB code pages differ at the same offset from byte `0x2d` on: the
code moved, so no 1.00 address applies.

Imports only in 1.06: `sceNpTusCreateRequest`, `sceNpTusDeleteRequest`, `sceNpTusDeleteNpTitleCtx`, `sceNpTusPollAsync`,
`sceNpTssCreateNpTitleCtx`, `sceNpTssGetDataAsync` (libSceNpTus) and `sceAppContentGetAddcontInfo`. Only in 1.00:
`sceAudioOutSetMixLevelPadSpk`. The runtime binds all of them (`src/import_names.inc`): TSS and TUS answer as signed
out, and `sceAppContentGetAddcontInfo` reports no entitlement, as the offline content provider does for every add-on.

### Where the 1.00 patch fingerprints occur in 1.06

Candidates for porting, found by unique byte pattern only. None is enabled: each needs the full verification the 1.00
site has (`tests/patch_site_tests.py`, `tests/skip_intro_tests.c`, `tests/exit_game_tests.c`) and an in-game test.

| 1.00 site | 1.00 | 1.06 candidate |
|---|---|---|
| GPU heap literals (startup functions) | `0x3bb`, `0x47b` | `0x3bb`, `0x47b` |
| Display height / width (`mov dword [rdi+0x34], 1080` / `[rdi+0x30], 1920`) | `0x1fa8da` / `0x1fa8e1` | `0x1fd98a` / `0x1fd991` |
| Skip Intro `wasPressed` / pad update | `0x129fb40` / `0x129fd20` | `0x12a4cc0` / `0x12a4ea0` |
| Exit Game exit-app sequence | `0x239350` | `0x23c880` |
| `engine_set_fps` name string | `0x17e2cbf` | `0x17e12bf` |

The `engine_set_fps` binding itself, the Skip Intro vtable slot, the Exit Game data sites and the Stated bytecode block
in the new `stream.dat` (the 1.00 block at `0x475f6c00` does not apply) still have to be found.

## Online

The build key is the online identity of the game version: the client sends `CUSA00062-1.06` and the lobby only lists,
joins and relays rooms of exactly the same key (`server/lobby/src/lobby.js`). 1.00 and 1.06 players never meet until a
compatibility of their simulation and network data is proven. No lobby change or deployment is needed for this.

### 1.06 multiplayer reconnaissance

Image offsets in 1.06 (`eboot.bin` mapped at 0). The menu text itself is in the zlib-compressed DATS blocks of
`stream.dat`, not in the executable; "UNX Global Matrix" is the Stated string `menu__network__error_general`.

| Feature | 1.06 evidence | In 1.00 |
|---|---|---|
| Local co-op | `menu__missions__squad_join_local` `0x17aedc9`, `game__hud__local_coop_notify` `0x17a7c61`, `menu__briefing__tip_local_coop` `0x17aa075`, `menu__missions__local_coop_tip` `0x17ae35b`, `menu__player__local_coop_tip` `0x17affc3`, `menu__player__coop_switch_player` `0x17aff4d` | no |
| Create / join squad | `menu__missions__squad_creating` `0x17aedaa` (new), `menu__squad_mission__join` `0x17b09d7`, `menu__missions__squad_change` `0x17aed8d`, `menu__missions__editsquad_*` `0x17adf38`, `menu__squad_mission__*` `0x17b099a`-`0x17b0a79` | mostly yes |
| Friends only / invite only | `menu__missions__online_option_friends_only` `0x17aebaf`, `menu__missions__online_option_invite_only` `0x17aebda`, `- friends_only (bool)` `0x17a0fed`, `menu__options__invite_friends` `0x17af923`, `menu__network__invite_*` `0x17af35f`-`0x17af431` | yes |
| Invite sessions (script API) | `Creates a new multiplayer invite session` `0x179d910` and the session/accepted/received helpers to `0x179dc5b`; `Online_Sessions_Orbis.cpp` `0x17f9d42` | yes |
| Leagues | `menu__league__*` `0x17ad63f`-`0x17ada1a` (tiers 0-7, promotion, deadline), new `menu__league__guide2` `0x17ad74f`, `menu__league__reward_xp_multiplier` `0x17ad85d`, `- league_tier(int)` `0x17a22dc`; `Rankings_Cache.cpp` `0x17d83d2`, `Online rankings handle` `0x17cfe3a` | partly |
| Weekly missions | `an__weekly__override_enabled` `0x179eec1` (lea at `0xb24ad`), `an__weekly__mission_location_override` `0x179eede` (`0xb2509`), `an__weekly__mission_id_override` `0x179ef04` (`0xb257a`), `menu__player__weekly_missions_completed` `0x17b00ba`, `menu__weekly__missioninfo` `0x17b1c7b` | partly |
| Title small storage (server data) | script bindings `tss_is_ready` `0x17a1f7f` (lea `0xb9903`), `tss_get_data` `0x17a1fda` (`0xb9a63`), help text `0x17a1f8c` (`0xb9930`); imports `sceNpTssCreateNpTitleCtx`, `sceNpTssGetDataAsync` | no |
| Friends list | `/v1/users/%s/friendList?...` `0x17c38f4`, `online_user_get_friends` `0x17ed3dc` | yes |
| Rooms | `Network_Room_Orbis.cpp` `0x17f9884`, `menu__network__*` | yes |

Weekly missions and leagues read server data through TSS and rankings; the runtime answers those as signed out, so
these modes should report themselves unavailable rather than crash. Not verified in game.

## Saves

1.00 and 1.06 use the same save folder (`user\savedata\CUSA00062`, from TITLE_ID; neither sets `INSTALL_DIR_SAVEDATA`),
as on a PS4 where an update keeps the title's saves. The executables have the same save-data strings (`savedata` folder
name, the same dialogs). Whether 1.06 writes a save layout 1.00 cannot read is not known: nothing migrates or rewrites
saves, and going back to 1.00 after playing 1.06 should be done with a copy of `user\savedata\CUSA00062`.
