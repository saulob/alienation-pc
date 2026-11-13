# Launcher header art

`header-art-source.png` is the official art (2172x724 RGB, approved by the author; no characters, logo or text). Only
its 2172x300 band from row 110 is used. `header-art.png` is derived from it: 3840x432 (8.89:1) RGBA, the band scaled
uniformly to 432 rows and anchored right, with transparent padding on the left. Do not edit `header-art.png`
separately, and there is no SVG for this art.

A horizontal alpha mask lets the launcher's own header gradient show through on the left: alpha is 0 up to 22% of the
width, rises along a smoothstep curve and reaches full at 55%, so the art has no visible edge and the title side stays
dark. Regenerate with (ffmpeg on PATH):

```powershell
tools\make-header-art.ps1
```

Only `header-art.png` ships; the source stays in the repository. The launcher draws it over the header gradient at the
header's full height, flush with the header's top, right and bottom; a narrow window cuts it from the left
(`launcher/header_art.h`). Without the file, or when it cannot be decoded, it keeps the gradient alone.

# Sponsor heart

`sponsor-heart.svg` is the source of truth for the heart in the Sponsor on GitHub buttons (CC0, see
`packaging/licenses/THIRD-PARTY-LICENSES.txt`). `sponsor-heart.png` is the runtime asset, a white 64x64 rendering of it
with a transparent background. Do not edit it by hand; regenerate it after every SVG change, from the repository root
with rsvg-convert (MSYS2 `mingw-w64-clang-x86_64-librsvg`):

```sh
rsvg-convert -w 64 -h 64 -f png -o assets/launcher/sponsor-heart.png assets/launcher/sponsor-heart.svg
```

The launcher draws it before the button label, as a square slightly taller than the label's text.

# Page icons

`icons/` holds the sidebar's nine SVGs, the source of truth, with one file per page and no rendered copies. They are
compiled into the launcher (`launcher/page_icons.cpp`), so they ship in no package as loose files. `icons/SOURCES.md`
records where each one came from and its license.
