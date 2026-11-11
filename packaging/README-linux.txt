Alienation - PC port (Linux)
Version @VERSION@

Alienation for PlayStation 4, running natively on Linux x86_64: the game's own
code runs on the PC with a replacement for the PS4 system libraries and a
Vulkan renderer. This is an early, unfinished port; Linux is not validated yet.

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
"Install update..." on the Play page and select the update package. 1.06 is
recognized but not ported yet: it runs without the 1.00 enhancements (render
resolution, aspect ratio, frame rate target, Skip Intro, Exit Game, Xbox
prompts) and is untested. "Remove update" returns to 1.00; saves are kept.

Any other build is refused, and the log names what was found.


REQUIREMENTS
------------
- Linux x86_64 with a Vulkan 1.3 driver
- A processor with AVX and LZCNT: Intel Core 4th generation (Haswell), AMD FX
  or newer
- Your own Alienation package, plus about 3 GB of free disk space


INSTALL
-------
Start the AppImage, click "Install game..." on the Play page and select your
.pkg file. When the Play page says "Ready to play", press PLAY.

Online play is not available yet. The launcher makes no online connection.


LICENSE
-------
The original project code of this port is licensed under the GNU General
Public License v2.0 or later (GPL-2.0-or-later). Third-party components keep
their own licenses. Game files are not included and are not covered by this
project's license. This port is not affiliated with or endorsed by
Housemarque, Sony Interactive Entertainment or any other rights holder of
Alienation.
