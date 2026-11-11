#!/usr/bin/env python3
"""Static checks for the controller prompts setting and its reversible asset overlay."""
from pathlib import Path
import sys

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
failures = []


def read(path):
    return (ROOT / path).read_text(encoding="utf-8", errors="replace")


def check(condition, message):
    if not condition:
        failures.append(message)


settings_h = read("launcher/settings.h")
settings_cpp = read("launcher/settings.cpp")
prompts_cpp = read("launcher/controller_prompts.cpp")
prompts_h = read("launcher/controller_prompts.h")
prompts_glyphs = read("launcher/controller_prompts_glyphs_data.c")
launcher_cpp = read("launcher/launcher.cpp")
loader_c = read("src/loader.c")
generator = read("tools/prompts/build_controller_prompts.py")
glyph_generator = read("tools/prompts/build_prompt_glyphs.py")
data = read("launcher/controller_prompts_data.c")
cmake = read("launcher/CMakeLists.txt")
root_cmake = read("CMakeLists.txt")

check("controller_prompts" in prompts_h, "the ini key is missing")
check("kDefaultControllerPrompts" in settings_h and "ALN_PROMPTS_DEFAULT_KEY" in settings_h,
      "the default controller prompt style is missing")
check("ALN_PROMPTS_PLAYSTATION_KEY" in prompts_h and "playstation" in prompts_h,
      "PlayStation is not an accepted value")
check("ALN_PROMPTS_XBOX_KEY" in prompts_h and '"xbox"' in prompts_h, "Xbox is not an accepted value")
check("AlnParsePromptStyle" in settings_cpp, "the setting does not validate its values")
check("kControllerPromptsKey" in settings_cpp, "the setting is not managed in settings.cpp")
check("Controller prompts" in launcher_cpp, "the launcher Controls page has no Controller prompts row")
check("Applies after a restart" in launcher_cpp, "the launcher does not say when the choice applies")

check("aln_controller_prompts_install" in loader_c, "the loader never installs the prompt overlay")
check("runtime_file_overlay" in prompts_cpp, "the replacement is not served as a guest file overlay")
check("stream.dat" in prompts_cpp, "the replacement does not target stream.dat")
check("Crc32" in prompts_cpp and "original_crc32" in prompts_cpp, "asset identity is not checked")
check("block_units" in prompts_cpp and "AlnPromptPatchFits" in prompts_cpp, "asset bounds are not checked")
check("std::ios::out" not in prompts_cpp and "ofstream" not in prompts_cpp and 'fopen' not in prompts_cpp,
      "the prompt module writes to a file")
check("rename" not in prompts_cpp and "remove(" not in prompts_cpp and "copy_file" not in prompts_cpp,
      "the prompt module mutates a file")
check("savedata" not in prompts_cpp.lower(), "the prompt module touches save data")
check("_WIN32" not in prompts_cpp and "_WIN32" not in prompts_h, "the prompt module is platform-specific")

check("AlnPromptGlyphAtlas" in prompts_h and "AlnPromptGlyphMask" in prompts_h,
      "the prompt glyph atlas types are missing")
check("AlnPromptGlyphAtlases" in prompts_glyphs and "kPromptGlyphAtlases" in prompts_glyphs,
      "the prompt glyph atlas table is missing")
for atlas in ('"font0"', '"font1"', '"font2"', '"icon"'):
    check(atlas in prompts_glyphs, f"the prompt glyph atlas table is missing {atlas}")
for build in ('"CUSA00062-1.00"', '"CUSA04430-1.00"'):
    check(build in prompts_glyphs, f"the prompt glyph atlas table is missing {build}")
check("BuildGlyphPatches" in prompts_cpp and "UntileAtlas" in prompts_cpp and "TileAtlas" in prompts_cpp,
      "the prompt glyph atlases are not installed")
check("AlnControllerScenePatch" in prompts_h and "AlnControllerScenePatches" in prompts_h,
      "the controller callout scene type is missing")
check("AlnControllerScenePatches" in data and "controller_template" in data,
      "the controller callout scene table is missing")
check(data.count('"controller_template"') == 2, "the controller callout scene table is not per build")
check("0x94u" in data, "the controller callout scene payload offset is missing")
check("SCENE_ANCHORS" in generator and "patch_controller_scene" in generator,
      "the generator does not patch the controller callout scene")
check("CALLOUTS" in generator and "callout_box" in generator and "patch_menu_layout" in generator,
      "the generator does not reduce the controller captions to stubs of their lines")
check("paint_callouts(color)" in generator,
      "the generator does not paint the caption lines into the Xbox controller texture")
check("same_lua_syntax" in generator and "menu layout patch changed the payload length" in generator,
      "the caption layout patch is not validated")
check(data.count('"menu_layout"') == 2, "the controller caption layout table is not per build")
for anchor in ("left_stick", "right_stick", "dpad_left_button", "dpad_right_button",
               "dpad_up_button", "dpad_down_button", "options_button", "triangle_button"):
    check(anchor in generator, f"the controller callout scene is missing the {anchor} anchor")
check("AlnScenePatchFits" in prompts_cpp and "kSceneType" in prompts_cpp,
      "the controller callout scene is not validated before installation")
check("AlnRewritePromptTokens" not in prompts_h and "BuildPromptTextPatch" not in prompts_cpp,
      "the old text-token rewrite is still installed")
check("controller_prompts_glyphs_data.c" in cmake, "the prompt glyph data is not part of the launcher library")

check("ASSETS = [" in generator, "the generator has no asset table")
for name in ("controller_color", "controller_white", "accept_a",
             "help_saboteur", "help_bio", "help_tank"):
    check(name in generator, f"the generator does not create {name}")
for value in ("CUSA00062-1.00", "CUSA04430-1.00"):
    check(value in data, f"the patch data has no row for {value}")
for glyph in ('"A"', '"B"', '"X"', '"Y"', '"LB"', '"RB"', '"LT"', '"RT"', '"View"', '"Menu"'):
    check(glyph in data, f"the Xbox glyph table is missing {glyph}")
for panel in ("Left Stick / L3", "Right Stick / R3"):
    check(panel in data, f"the Xbox glyph table is missing {panel}")
for old in ("SHARE", "OPTIONS", "TOUCHPAD"):
    check(old not in generator, f"the Xbox artwork still refers to {old}")
for label in ("Throw Grenade", "Switch Grenade", "Switch Weapon", "Interaction", "Melee", "Shoot"):
    check(label in generator, f"the labeled controller art lost the action {label}")

check("tutorial_lineart" in generator and "LINEART_CONTROLLER" in generator,
      "the tutorial line art is not part of the generation")
check("LINEART_CONTROLLER" in generator and "xbox-series-lineart" in generator,
      "the generator does not use the Xbox Series line art")
check('"controller_white", 0x1AD80380' in generator and "True" in generator,
      "the controller white texture is not generating mips")
check('"help_saboteur", 0x1ADDA900' in generator and '"help_bio", 0x1ADFED40' in generator,
      "the tutorial help textures are not generating mips")
check("draw.rectangle" not in glyph_generator.split("def shoulder", 1)[1].split("def circle", 1)[0],
      "the shoulder glyph still draws an opaque rectangle")
check("fill=248" not in glyph_generator, "the shoulder glyph still uses a white background")
check("Image.new(\"L\", block.size, 0)" not in glyph_generator.split("def shoulder", 1)[1].split("def face", 1)[0],
      "the shoulder glyph no longer keeps the original button artwork")
check('"PlayStation (Original)"' in launcher_cpp and 'PlayStation (Original)' in prompts_cpp,
      "the launcher label is not PlayStation (Original)")
check("2797328u" in data and "352528u" in data,
      "the generated controller textures do not contain the full mip chains")

check("controller_prompts.cpp" in cmake and "controller_prompts_data.c" in cmake,
      "the prompt module is not part of the shared launcher library")
check("controller_prompts_style.c" in cmake, "the prompt style parser is not compiled")
check("aln-controller-prompts-tests" in root_cmake, "the focused test is not configured")
check('#include "controller_prompts.h"' in data[:200], "the generated data does not include its header")

source = ROOT / "assets" / "source" / "controller"
svg = source / "xbox-series-x-black.svg"
derivative = source / "xbox-series-x-black-alienation.svg"
license_text = (source / "LICENSE-Gamepad-Asset-Pack.txt").read_text(encoding="utf-8")
third_party = read("packaging/licenses/THIRD-PARTY-LICENSES.txt")
check(svg.exists() and "viewBox" in svg.read_text(encoding="utf-8"), "the licensed SVG source is missing")
check("MIT License" in license_text and "Al. Lopez" in license_text, "the upstream MIT notice is missing")
check(derivative.exists(), "the Alienation SVG adaptation is missing")
check((source / "xbox-series-x-black-alienation.png").exists(),
      "the 1024x512 controller derivative is missing")
if derivative.exists():
    adapted = derivative.read_text(encoding="utf-8")
    check("export-filename" not in adapted and "C:\\Users\\AL200" not in adapted,
          "the derivative SVG still carries author machine paths")
check("Gamepad Asset Pack" in third_party and "Al. Lopez" in third_party,
      "the controller attribution is missing from the third-party licence file")
check("Xbox Series X Controller VSCView Black.svg" in third_party,
      "the third-party licence file does not name the Xbox Series X source")
check("SVG_CONTROLLER" in generator and "svg_controller_images" in generator,
      "the controller art is not generated from the SVG derivative")
check("xbox-series-x-black-alienation.png" in generator,
      "the generator does not use the Xbox Series X illustration")
check("controller_glow" not in generator,
      "the generator still replaces the game's own glow texture")
check("xbox-360" not in generator,
      "the generator still refers to the Xbox 360 artwork")

lineart_svg = source / "xbox-series-lineart.svg"
lineart_png = source / "xbox-series-lineart.png"
lineart_license = (source / "LICENSE-Gamepad-Line-Art-Pack.txt").read_text(encoding="utf-8")
check(lineart_svg.exists() and "viewBox" in lineart_svg.read_text(encoding="utf-8"),
      "the Xbox Series line-art SVG is missing")
check(lineart_png.exists(), "the Xbox Series line-art PNG is missing")
if lineart_svg.exists():
    check("59 20 1417 959" in lineart_svg.read_text(encoding="utf-8"),
          "the Xbox Series line-art SVG has the wrong viewBox")
if lineart_png.exists():
    lineart = Image.open(lineart_png)
    check(lineart.size == (1024, 693), "the Xbox Series line-art PNG has the wrong dimensions")
    check(lineart.getchannel("A").getextrema()[0] == 0,
          "the Xbox Series line-art PNG has no transparent pixels")
check("CC0 1.0" in lineart_license, "the Gamepad Line Art Pack licence is missing")
check("Gamepad Line Art Pack" in third_party and "gidedin" in third_party and "CC0" in third_party,
      "the line-art attribution is missing from the third-party licence file")

if failures:
    for failure in failures:
        print("FAIL:", failure)
    sys.exit(1)
print("controller prompt static checks passed")
