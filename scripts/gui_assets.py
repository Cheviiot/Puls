#!/usr/bin/env python3
"""Regenerates the icons and fonts of the graphical interface.

The sources are downloaded by hand and checked against their SHA-256:

  https://registry.npmjs.org/lucide-static/-/lucide-static-1.50.0.tgz
  https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip

  python3 scripts/gui_assets.py icons lucide-static-1.50.0.tgz
  python3 scripts/gui_assets.py fonts Inter-4.1.zip

The fonts need fontTools (pip install fonttools). The results go to
src/puls/gui/qml and are committed.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
QML_DIR = ROOT / "src" / "puls" / "gui" / "qml"

LUCIDE_VERSION = "1.50.0"
LUCIDE_SHA256 = "b23b1b4c30dbf0877eee2087275f48310b45a098e537ba83598cce1ea553dce6"
INTER_VERSION = "4.1"
INTER_SHA256 = "9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e"

# QML property name -> Lucide icon name.
ICONS = {
    "settings": "settings",
    "themeSystem": "sun-moon",
    "themeLight": "sun",
    "themeDark": "moon",
    "close": "x",
    "minimize": "minus",
    "maximize": "square",
    "restore": "copy",
    "ping": "timer",
    "jitter": "audio-waveform",
    "download": "arrow-down",
    "upload": "arrow-up",
    "server": "server",
    "connection": "globe",
    "refresh": "refresh-cw",
    "success": "circle-check",
    "warning": "triangle-alert",
    "error": "circle-x",
    "info": "info",
    "check": "check",
    "plus": "plus",
    "minus": "minus",
    "retry": "rotate-ccw",
    "play": "play",
    "stop": "square",
}

# Output file -> (file in the Inter archive, family, style).
FONTS = {
    "Inter-Regular.ttf": ("extras/ttf/Inter-Regular.ttf", "Inter", "Regular"),
    "Inter-Medium.ttf": ("extras/ttf/Inter-Medium.ttf", "Inter", "Medium"),
    "Inter-SemiBold.ttf": ("extras/ttf/Inter-SemiBold.ttf", "Inter", "SemiBold"),
    "InterDisplay-SemiBold.ttf": (
        "extras/ttf/InterDisplay-SemiBold.ttf",
        "Inter Display",
        "SemiBold",
    ),
}

# Latin, Cyrillic, punctuation, currency and the symbols of the interface.
UNICODES = (
    "U+0020-007E,U+00A0-00FF,U+0152-0153,U+0401-045F,U+0490-0491,U+2002-200B,"
    "U+2010-2027,U+202F,U+2030,U+2039-203A,U+2044,U+20AC,U+20BD,U+2116,U+2122,"
    "U+2190-2195,U+2212,U+2215,U+2219,U+2248,U+2260,U+2264-2265,U+FEFF,U+FFFD"
)

NUMBER = r"[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?"


def read_checked(path: Path, expected: str) -> bytes:
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != expected:
        raise SystemExit(f"{path}: SHA-256 {digest}, ожидался {expected}")
    return data


def number(value: float) -> str:
    text = f"{value:.3f}".rstrip("0").rstrip(".")
    return "0" if text in ("", "-0") else text


def element_path(tag: str, attributes: dict[str, str]) -> str:
    """Converts one SVG element of an icon into path data."""
    if tag == "path":
        # A relative moveto at the start of a path is absolute, and the
        # coordinate pairs that follow it are relative line segments.
        data = attributes["d"]
        match = re.match(rf"\s*m\s*({NUMBER})[\s,]*({NUMBER})(.*)$", data, re.S)
        if not match:
            return data
        x, y, rest = match.groups()
        rest = rest.strip()
        if rest and re.match(r"[-+.\d]", rest):
            rest = "l" + rest
        return f"M{x} {y}{rest}"
    values = {
        key: float(value) for key, value in attributes.items() if key not in ("key", "points")
    }
    if tag == "line":
        return (
            f"M{number(values['x1'])} {number(values['y1'])}"
            f"L{number(values['x2'])} {number(values['y2'])}"
        )
    if tag in ("polyline", "polygon"):
        coordinates = attributes["points"].replace(",", " ").split()
        points = zip(coordinates[0::2], coordinates[1::2])
        path = "M" + "L".join(f"{number(float(x))} {number(float(y))}" for x, y in points)
        return path + ("Z" if tag == "polygon" else "")
    if tag in ("circle", "ellipse"):
        rx = values.get("r", values.get("rx"))
        ry = values.get("r", values.get("ry"))
        cx, cy = values["cx"], values["cy"]
        arc = f"A{number(rx)} {number(ry)} 0 1 0"
        return (
            f"M{number(cx - rx)} {number(cy)}"
            f"{arc} {number(cx + rx)} {number(cy)}"
            f"{arc} {number(cx - rx)} {number(cy)}Z"
        )
    if tag == "rect":
        x, y = values.get("x", 0.0), values.get("y", 0.0)
        width, height = values["width"], values["height"]
        rx = values.get("rx", values.get("ry", 0.0))
        ry = values.get("ry", rx)
        if rx <= 0:
            return f"M{number(x)} {number(y)}H{number(x + width)}V{number(y + height)}H{number(x)}Z"
        arc = f"A{number(rx)} {number(ry)} 0 0 1"
        return (
            f"M{number(x + rx)} {number(y)}H{number(x + width - rx)}"
            f"{arc} {number(x + width)} {number(y + ry)}V{number(y + height - ry)}"
            f"{arc} {number(x + width - rx)} {number(y + height)}H{number(x + rx)}"
            f"{arc} {number(x)} {number(y + height - ry)}V{number(y + ry)}"
            f"{arc} {number(x + rx)} {number(y)}Z"
        )
    raise ValueError(f"неподдерживаемый элемент {tag}")


def icons_qml(nodes: dict[str, list], version: str) -> str:
    lines = [
        "pragma Singleton",
        "import QtQml",
        "",
        f"// Stroke icons of Lucide {version} on a 24 x 24 grid, drawn with round caps",
        "// and joins and a stroke width of 2. Generated by scripts/gui_assets.py.",
        "//",
        "// Copyright (c) 2026 Lucide Icons and Contributors; ISC License. Some",
        "// icons derive from Feather, copyright (c) 2013-present Cole Bemis; MIT",
        "// License. The license texts are in THIRD_PARTY_NOTICES.txt.",
        "QtObject {",
    ]
    for name, icon in ICONS.items():
        path = "".join(element_path(tag, attributes) for tag, attributes in nodes[icon])
        lines.append(f'    readonly property string {name}: "{path}"')
    lines.append("}")
    return "\n".join(lines) + "\n"


def generate_icons(archive: Path) -> None:
    data = read_checked(archive, LUCIDE_SHA256)
    with tarfile.open(fileobj=io.BytesIO(data)) as package:
        nodes = json.load(package.extractfile("package/icon-nodes.json"))
        license_text = package.extractfile("package/LICENSE").read().decode()
    (QML_DIR / "Icons.qml").write_text(icons_qml(nodes, LUCIDE_VERSION))
    (QML_DIR / "LICENSE.lucide.txt").write_text(license_text)


def rename(font, family: str, style: str) -> None:
    """Gives every weight one family so that Qt selects them by weight."""
    names = font["name"]
    for name_id in (21, 22):
        names.removeNames(nameID=name_id)
    full_name = family if style == "Regular" else f"{family} {style}"
    for name_id, value in ((1, family), (2, style), (4, full_name), (16, family), (17, style)):
        names.removeNames(nameID=name_id)
        names.setName(value, name_id, 3, 1, 0x409)
        names.setName(value, name_id, 1, 0, 0)
    os2 = font["OS/2"]
    # Clears the italic, bold and regular bits; only Regular is regular.
    os2.fsSelection = (os2.fsSelection & ~0x61) | (0x40 if style == "Regular" else 0)
    font["head"].macStyle = 0


def generate_fonts(archive: Path) -> None:
    from fontTools import subset
    from fontTools.ttLib import TTFont

    data = read_checked(archive, INTER_SHA256)
    options = subset.Options()
    options.layout_features = ["*"]
    options.name_IDs = ["*"]
    options.name_legacy = True
    options.name_languages = ["*"]
    unicodes = subset.parse_unicodes(UNICODES)
    output = QML_DIR / "fonts"
    output.mkdir(exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as release:
        for name, (member, family, style) in FONTS.items():
            font = TTFont(io.BytesIO(release.read(member)), recalcTimestamp=False)
            subsetter = subset.Subsetter(options)
            subsetter.populate(unicodes=unicodes)
            subsetter.subset(font)
            rename(font, family, style)
            font.save(output / name, reorderTables=True)
        (output / "LICENSE.txt").write_bytes(release.read("LICENSE.txt"))


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("kind", choices=("icons", "fonts"))
    parser.add_argument("archive", type=Path)
    arguments = parser.parse_args(argv)
    if arguments.kind == "icons":
        generate_icons(arguments.archive)
    else:
        generate_fonts(arguments.archive)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
