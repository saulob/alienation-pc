Alienation - PC port (Linux)
Version @VERSION@

Alienation for PlayStation 4, running natively on Linux x86_64: the game's own
code runs on the PC with a replacement for the PS4 system libraries and a
Vulkan renderer. The Linux and Steam Deck build is experimental: it is built
and checked headlessly, but not yet tested in real gameplay or on Steam Deck
hardware.

Source code and issue tracker:
https://github.com/saulob/alienation-pc


NO GAME FILES ARE INCLUDED
--------------------------
You need your own legally obtained copy of the game: a decrypted Alienation
PS4 package (.pkg). This port currently accepts these builds:

  Title ID CUSA00062, APP_VER 01.00 (content ID EP9000-CUSA00062_00-ALIENATIONGAMEEU, Europe)
  Title ID CUSA00062, APP_VER 01.06 (the European update, installed over 01.00)
  Title ID CUSA04430, APP_VER 01.00 (content ID HP9000-CUSA04430_00-ALIENATIONGAMEKR, Asia)

To add the European 1.06 update, install the base game first, then click
"Install update..." on the Play page and select the update package. "Remove
update" returns to 1.00; saves are kept. A complete CUSA00062 1.06 package is
installed with "Install game..." and runs as 1.06. Every build has the same
port features; local co-op exists only in 1.06.

Any other build is refused, and the log names what was found.


REQUIREMENTS
------------
- Linux x86_64 with a Vulkan 1.3 driver
- A processor with AVX and LZCNT: Intel Core 4th generation (Haswell), AMD FX
  or newer
- Your own Alienation package, plus about 3 GB of free disk space


INSTALL
-------
1. Put the AppImage in a folder of its own: the game, saves, settings and
   logs are created beside it (or in ~/.local/share/AlienationPC when that
   folder is read-only), never inside the AppImage.
2. Make it executable (chmod +x, or the file manager's Properties) and run
   it. FUSE is not required.
3. Click "Install game..." on the Play page and select your .pkg file. When
   the Play page says "Ready to play", press PLAY.

Start it with --launcher (or the desktop entry's "Show launcher" action) to
open the launcher when it is set to start straight into the game.

STEAM DECK
----------
Set it up in Desktop Mode as above, then add the AppImage to Steam with
"Add a Non-Steam Game". In Gaming Mode, put --launcher in its Launch Options
to open the launcher; L3 + R3 opens the Options menu in game.

ONLINE
------
Turn Online play On in the launcher's Online page, run the Connection test
once, add friends with their Friend Code and use the game's own squad menus.
Windows and Linux players of the same game build play together. With Online
play Off the game makes no online connection.


LICENSE
-------
The port's own code is licensed under the GNU General Public License v2.0
or later, its libSceJson2 code under GPL-2.0 only, so the program is
distributed under GPL-2.0. Third-party components keep their own licenses
(usr/share/licenses/AlienationPC inside the AppImage). Game files are not
included and are not covered by this
project's license. This port is not affiliated with or endorsed by
Housemarque, Sony Interactive Entertainment or any other rights holder of
Alienation.
