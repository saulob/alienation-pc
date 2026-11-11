#!/usr/bin/env python3
"""Adapt the licensed Xbox Series X SVG to Alienation's dark, light-outline controller style."""
from __future__ import annotations

import argparse
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SVG_NS = "http://www.w3.org/2000/svg"
INKSCAPE_NS = "http://www.inkscape.org/namespaces/inkscape"
SODIPODI_NS = "http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"

DARK = "#20242a"
LIGHT = "#c8d0d8"
BRIGHT = "#e8edf2"

COLOURS = {
    "fill:#808080": f"fill:{LIGHT}",
    "fill:#a6a6a6": f"fill:{BRIGHT}",
    "fill:#7f7f7f": f"fill:{LIGHT}",
    "fill:#525252": f"fill:{LIGHT}",
    "stroke:#525252": "stroke:#aab4be",
}


def label(element):
    return element.get(f"{{{INKSCAPE_NS}}}label", "")


def set_style(element, updates):
    style = element.get("style", "")
    values = {}
    for item in style.split(";"):
        if ":" in item:
            key, value = item.split(":", 1)
            values[key.strip()] = value.strip()
    values.update(updates)
    element.set("style", ";".join(f"{key}:{value}" for key, value in values.items()))


def adapt(source: Path, target: Path):
    ET.register_namespace("", SVG_NS)
    ET.register_namespace("inkscape", INKSCAPE_NS)
    ET.register_namespace("sodipodi", SODIPODI_NS)
    tree = ET.parse(source)
    root = tree.getroot()

    for element in root.iter():
        for attribute in (f"{{{INKSCAPE_NS}}}export-filename", f"{{{SODIPODI_NS}}}docname"):
            element.attrib.pop(attribute, None)
        style = element.get("style")
        if not style:
            continue
        for old, new in COLOURS.items():
            style = style.replace(old, new)
        element.set("style", style)

    for element in root.iter():
        name = label(element)
        if name == "Color":
            set_style(element, {"fill": DARK})
        elif name == "Outline":
            set_style(element, {"fill": LIGHT})

    tree.write(target, encoding="utf-8", xml_declaration=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", default=str(ROOT / "assets" / "source" / "controller" /
                                                   "xbox-series-x-black.svg"))
    parser.add_argument("--target", default=str(ROOT / "assets" / "source" / "controller" /
                                                   "xbox-series-x-black-alienation.svg"))
    args = parser.parse_args()
    adapt(Path(args.source), Path(args.target))
    print("wrote", args.target)


if __name__ == "__main__":
    main()
