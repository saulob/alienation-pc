Alienation - PC port
Version @VERSION@

Alienation for PlayStation 4, running natively on Windows: the game's own code
runs on the PC with a replacement for the PS4 system libraries and a Vulkan
renderer. This is an early, unfinished port.

Source code and issue tracker:
https://github.com/saulob/alienation-pc


NO GAME FILES ARE INCLUDED
--------------------------
You need your own legally obtained copy of the game: a decrypted Alienation
PS4 package (.pkg). This port currently accepts these builds:

  Title ID CUSA00062, APP_VER 01.00 (content ID EP9000-CUSA00062_00-ALIENATIONGAMEEU, Europe)
  Title ID CUSA04430, APP_VER 01.00 (content ID HP9000-CUSA04430_00-ALIENATIONGAMEKR, Asia)

Updates are not supported yet: the European 1.06 update package is refused.

Any other build is refused, and the log names what was found. A package still
encrypted for a PS4 console cannot be unpacked on a PC.


REQUIREMENTS
------------
- Windows 10 version 1903 or later, or Windows 11, 64-bit
- A processor with AVX and LZCNT: Intel Core 4th generation (Haswell), AMD FX
  or newer
- A graphics card and driver with Vulkan 1.3
- Your own Alienation package, plus about 3 GB of free disk space next to the
  program for the installed game


INSTALL
-------
1. Extract the ZIP anywhere you can write to (not "Program Files").
2. Run alienation.exe. The launcher opens.
3. On the Play page, click "Install game..." and select your .pkg file.
4. Wait while the launcher checks the package and unpacks it into the "game"
   folder next to the program. The .pkg file is only read.
5. When the Play page says "Ready to play", press PLAY.


PLAYING
-------
- F1 (or L3 + R3 on a controller) opens the Options menu.
- F2 shows the frame counter. F11 switches between Windowed and Borderless
  Fullscreen.
- Xbox and PlayStation controllers and the keyboard are supported; the
  launcher's Controls page lists the keys.
- Online play is not available yet. The launcher makes no online connection.


WHERE THINGS ARE
----------------
- Game files:     the "game" folder next to alienation.exe
- Logs:           the "logs" folder next to alienation.exe (alienation_NNN.log)
- Settings (alienation.ini), saves and shader cache: the "user" folder


LICENSE
-------
The original project code of this port is licensed under the GNU General
Public License v2.0 or later (GPL-2.0-or-later). The full text is in
LICENSE.txt. Third-party components keep their own licenses; see the
"licenses" folder.

Game files are not included and are not covered by this project's license.
This port is not affiliated with or endorsed by Housemarque, Sony Interactive
Entertainment or any other rights holder of Alienation.
