"""symbols.ttf: the icons VanaCoreApp draws, from Google's Material Symbols Outlined (Apache 2.0).

The variable font is 10 MB; this pins it to the one look the app uses (FILL 0, wght 400, GRAD 0, opsz 24) and keeps
only the icons in ICONS, a few KB in VanaCoreApp.dll. To add an icon: put its name in ICONS (and in shared/ui.h's
icon namespace, its codepoint), then run this again.

  pip install fonttools
  python make_symbols.py <MaterialSymbolsOutlined[FILL,GRAD,opsz,wght].ttf> <its .codepoints> symbols.ttf

Both files: github.com/google/material-design-icons, variablefont/.
"""
import sys

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

ICONS = [
    "play_arrow", "undo", "download", "check_circle", "error", "info", "sync", "close", "settings", "folder_open",
    "keyboard_arrow_up", "keyboard_arrow_down", "sports_esports", "open_in_new", "cancel", "edit", "add", "delete",
    "radio_button_unchecked",
]


def main(font_path, codepoints_path, out_path):
    codepoints = {}
    with open(codepoints_path, encoding="utf-8") as f:
        for line in f:
            name, hex_code = line.split()
            codepoints[name] = int(hex_code, 16)
    missing = [name for name in ICONS if name not in codepoints]
    if missing:
        sys.exit("not in Material Symbols: " + ", ".join(missing))
    font = instancer.instantiateVariableFont(TTFont(font_path), {"FILL": 0, "wght": 400, "GRAD": 0, "opsz": 24})
    options = subset.Options()
    options.layout_features = []  # by codepoint only: no ligatures
    options.name_IDs = ["*"]
    options.notdef_outline = True
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=[codepoints[name] for name in ICONS])
    subsetter.subset(font)
    font.save(out_path)
    for name in ICONS:
        print(f"{name:14} U+{codepoints[name]:04X}")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
