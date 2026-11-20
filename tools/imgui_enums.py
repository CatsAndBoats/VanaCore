"""Dear ImGui's enums, read from a header, and how 1.81's (Ashita 4.16's) map onto 1.92's (Ashita 4.30's, VanaCore's):
a module of gen_ashita_gui.py.

1.81's values reach VanaCore from interface 4.16 plugins and from addons that load Ashita 4.16's libs\\imgui.lua
(IMGUI_VERSION_NUM 18000). 1.92 renumbered many of them (window flags, colours, keys, table flags, ...), so each is
turned into 1.92's by its name: a flag bit by bit, a value as a whole. A name 1.92 lost is mapped by RENAMED, or dropped.
"""

import re

# 1.81 names 1.92 knows by another (its obsolete aliases cover most: these are the rest).
RENAMED = {
    "ImGuiKey_KeyPadEnter": "ImGuiKey_KeypadEnter",
    "ImGuiWindowFlags_NavFlattened": "ImGuiWindowFlags_NavFlattened",
    "ImGuiCol_NavHighlight": "ImGuiCol_NavCursor",
    "ImGuiTreeNodeFlags_AllowItemOverlap": "ImGuiTreeNodeFlags_AllowOverlap",
    "ImGuiSelectableFlags_AllowItemOverlap": "ImGuiSelectableFlags_AllowOverlap",
    "ImGuiSelectableFlags_SpanAllColumns": "ImGuiSelectableFlags_SpanAllColumns",
    "ImGuiButtonFlags_MouseButtonDefault_": "ImGuiButtonFlags_MouseButtonLeft",
    "ImGuiTableFlags_ColumnsWidthStretch": "ImGuiTableFlags_SizingStretchSame",
    "ImGuiTableFlags_ColumnsWidthFixed": "ImGuiTableFlags_SizingFixedFit",
    "ImGuiTableColumnFlags_WidthAuto": "ImGuiTableColumnFlags_WidthFixed",
    "ImGuiTableColumnFlags_NoSortAscending": "ImGuiTableColumnFlags_NoSortAscending",
    "ImGuiColorEditFlags__OptionsDefault": "ImGuiColorEditFlags_DefaultOptions_",
    "ImGuiColorEditFlags__DisplayMask": "ImGuiColorEditFlags_DisplayMask_",
    "ImGuiColorEditFlags__DataTypeMask": "ImGuiColorEditFlags_DataTypeMask_",
    "ImGuiColorEditFlags__PickerMask": "ImGuiColorEditFlags_PickerMask_",
    "ImGuiColorEditFlags__InputMask": "ImGuiColorEditFlags_InputMask_",
    "ImGuiHoveredFlags_AllowWhenBlockedByPopup": "ImGuiHoveredFlags_AllowWhenBlockedByPopup",
    "ImGuiSliderFlags_ClampOnInput": "ImGuiSliderFlags_AlwaysClamp",
    "ImGuiDragDropFlags_SourceAutoExpirePayload": "ImGuiDragDropFlags_PayloadAutoExpire",
    "ImGuiColorEditFlags_RGB": "ImGuiColorEditFlags_DisplayRGB",
    "ImGuiColorEditFlags_HSV": "ImGuiColorEditFlags_DisplayHSV",
    "ImGuiColorEditFlags_HEX": "ImGuiColorEditFlags_DisplayHex",
    "ImGuiInputTextFlags_AlwaysInsertMode": "ImGuiInputTextFlags_AlwaysOverwrite",
    "ImGuiKeyModFlags_Ctrl": "ImGuiMod_Ctrl",
    "ImGuiKeyModFlags_Shift": "ImGuiMod_Shift",
    "ImGuiKeyModFlags_Alt": "ImGuiMod_Alt",
    "ImGuiKeyModFlags_Super": "ImGuiMod_Super",
}

# 1.81's corner flags (ImDrawCornerFlags) are 1.92's ImDrawFlags_RoundCorners*, four bits up.
CORNERS = {
    "ImDrawCornerFlags_TopLeft": "ImDrawFlags_RoundCornersTopLeft",
    "ImDrawCornerFlags_TopRight": "ImDrawFlags_RoundCornersTopRight",
    "ImDrawCornerFlags_BotLeft": "ImDrawFlags_RoundCornersBottomLeft",
    "ImDrawCornerFlags_BotRight": "ImDrawFlags_RoundCornersBottomRight",
}

# Types whose values are flags (a bit each), as against values (one each).
def is_flags(type_name):
    return type_name.endswith("Flags") or type_name == "ImGuiCond"


def _strip(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return "\n".join(line for line in text.split("\n") if not line.strip().startswith("#"))


def parse(text):
    """{enum name: [(member, value), ...]} for each enum defined (not just declared) in `text`."""
    text = _strip(text.replace("\r\n", "\n"))
    enums = {}
    known = {}
    for m in re.finditer(r"\benum\s+(\w+)\s*(?::\s*\w+\s*)?\{(.*?)\}\s*;", text, re.S):
        name, body = m.group(1), m.group(2)
        members, last = [], -1
        for part in body.split(","):
            part = part.strip()
            if not part:
                continue
            if "=" in part:
                member, expr = (s.strip() for s in part.split("=", 1))
                value = _eval(expr, known)
            else:
                member, value = part, last + 1
            if value is None:
                continue
            known[member] = value
            members.append((member, value))
            last = value
        enums[name] = members
    return enums


def _eval(expr, known):
    expr = re.sub(r"\(\s*int\s*\)", "", expr)
    expr = re.sub(r"(\d+)[uU]?[lL]*\b", r"\1", expr)
    names = set(re.findall(r"[A-Za-z_]\w*", expr))
    env = {}
    for n in names:
        if n not in known:
            return None
        env[n] = known[n]
    try:
        return int(eval(expr, {"__builtins__": {}}, env))
    except Exception:
        return None


def members_of(enums, type_name):
    """The members of a type's enum (ImGuiWindowFlags: enum ImGuiWindowFlags_, ImGuiKey: ImGuiKey_ or ImGuiKey)."""
    for name in (type_name + "_", type_name):
        if name in enums:
            return enums[name]
    return None


def _all_members(enums):
    out = {}
    for members in enums.values():
        for member, value in members:
            out.setdefault(member, value)
    return out


def mapping(old_enums, new_enums, type_name, new_type=None):
    """(table, dropped): for a flags type, {old bit: new value}; for a value type, {old value: new value}. `dropped`
    lists the old names 1.92 has nothing for."""
    old = members_of(old_enums, type_name)
    if old is None:
        return None, []
    new_all = _all_members(new_enums)
    table, dropped = {}, []
    flags = is_flags(type_name)
    for member, value in old:
        target = CORNERS.get(member) or RENAMED.get(member) or member
        if type_name == "ImDrawCornerFlags" and member not in CORNERS:
            continue
        new_value = new_all.get(target)
        if flags:
            if value <= 0 or value & (value - 1):  # not a single bit: composites and masks follow from the bits
                continue
            if new_value is None:
                dropped.append(member)
                continue
            table[value] = new_value
        else:
            if member.endswith("_COUNT") or member.endswith("COUNT"):
                continue
            if new_value is None:
                dropped.append(member)
                continue
            table.setdefault(value, new_value)
    return table, dropped
