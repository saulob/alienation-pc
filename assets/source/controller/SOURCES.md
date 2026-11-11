# Xbox Series X controller illustration

`xbox-series-x-black.svg` is unmodified from the Gamepad Asset Pack:

- Source: https://github.com/AL2009man/Gamepad-Asset-Pack
- File: `Controller Asset Pack/Xbox Wireless Controller Images/Default Theme/Theme SVG/Xbox Series X Color/Xbox Series X Controller VSCView Black.svg`
- Author: Al. Lopez
- License: MIT, Copyright (c) 2024 Al. Lopez. See `LICENSE-Gamepad-Asset-Pack.txt`.

`xbox-series-x-black-alienation.svg` is the Alienation adaptation used for the controller
illustration. It gives the body a dark fill, the outlines a light fill and drops the author's
local export paths. `xbox-series-x-black-alienation.png` is the 1024x512 runtime derivative
that the menu screens draw.

The upstream README notes that parts of the pack are based on official controller graphics. The
SVG is used here under the repository's MIT terms and is not presented as original Alienation art.

Rasterizer: `tools/prompts/svg_raster.c` (nanosvg, zlib licence).

Reproduce the runtime PNG with:

    cc -O2 -Ilauncher/third_party/nanosvg tools/prompts/svg_raster.c -o tools/prompts/svg_raster -lm
    py -3.13 tools/prompts/build_controller_svg.py
    py -3.13 tools/prompts/render_controller_svg.py --rasterizer tools/prompts/svg_raster

# Xbox Series controller line art

`xbox-series-lineart.svg` and `xbox-series-lineart.png` are from the Gamepad Line Art Pack:

- Source: https://gidedin.itch.io/gamepad-line-art-pack
- File: `svg/regular/xbox-series.svg` and `png/regular/light/xbox-series.png`
- Author: gidedin
- License: CC0 1.0 Universal. See `LICENSE-Gamepad-Line-Art-Pack.txt`.
- The pack covers the drawings only and grants no trademark rights. The controller art is not
  presented as endorsed by Microsoft.

The light PNG is the 1024 px transparent derivative used for the in-game tutorial controller. The
runtime texture generator preserves transparency and resamples it to the game's controller texture
mip levels.

# Caption lines

Controller Settings and the loading Controls page draw each caption line from its box in the
`menu_layout` script, with a shape fixed per caption (straight, vertical, or the `dpad_right`
and `square` elbows). Those shapes cannot reach the Xbox Series X controls without crossing other
buttons, so `tools/prompts/build_controller_prompts.py` shrinks each box to a short stub that only
places its caption and paints the complete lines into `controller_color` (`CALLOUTS`). The
mappings from 1920x1080 menu pixels were measured on in-game captures:

    box_x = (x - 961) / 1052        box_y = (532 - y) / 1062
    texture_x = (x - 440) / 1.015   texture_y = (y - 268) / 1.035

The `controller_template` scene anchors (`SCENE_ANCHORS`) are where the in-game tutorial hints
highlight a button over `controller_white`, so they are measured on that line art; the caption
lines do not end on them.
