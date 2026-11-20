"""Generates src/core/lua_imgui_views.inc from third_party/imgui/imgui.h (ImGui 1.92): for each of ImGui's structures Ashita
4.30's addons reach (io, style, fonts, viewports, draw lists, ...: Ashita's annotations, libs/annotations/SDK/
IGuiManagerTypes.lua), its fields as lua_imgui.cpp's views describe them: a field reads and writes the structure
itself. Methods, and what addons written for Ashita 4.16 (ImGui 1.81) expect, are added by hand in lua_imgui.cpp.

Usage:
    python tools/gen_imgui_views.py
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGUI_H = os.path.join(ROOT, "third_party", "imgui", "imgui.h")
OUT = os.path.join(ROOT, "src", "core", "lua_imgui_views.inc")

STRUCTS = [
    "ImGuiIO", "ImGuiStyle", "ImGuiPlatformIO", "ImGuiPlatformMonitor", "ImGuiPlatformImeData", "ImGuiViewport",
    "ImGuiWindowClass", "ImGuiKeyData", "ImGuiInputTextCallbackData", "ImGuiSizeCallbackData", "ImGuiPayload",
    "ImGuiTableSortSpecs", "ImGuiTableColumnSortSpecs", "ImGuiListClipper", "ImGuiMultiSelectIO", "ImGuiSelectionRequest",
    "ImGuiSelectionBasicStorage", "ImGuiSelectionExternalStorage", "ImGuiStorage", "ImGuiStoragePair", "ImGuiTextBuffer",
    "ImGuiTextFilter", "ImColor", "ImDrawCmd", "ImDrawCmdHeader", "ImDrawVert", "ImDrawChannel", "ImDrawListSplitter",
    "ImDrawList", "ImDrawData", "ImTextureRect", "ImTextureRef", "ImTextureData", "ImFontConfig", "ImFontGlyph",
    "ImFontGlyphRangesBuilder", "ImFontAtlasRect", "ImFontAtlas", "ImFontBaked", "ImFont",
]

DEFINED = {"IMGUI_HAS_DOCK", "IMGUI_HAS_VIEWPORT", "IMGUI_HAS_TEXTURES", "IMGUI_HAS_TABLE", "_MSC_VER", "_WIN32"}

SCALARS = {
    "float": "Float", "double": "Double", "int": "Int", "bool": "Bool", "short": "Short", "unsigned int": "UInt",
    "unsigned short": "UShort", "unsigned char": "U8", "char": "S8", "ImS8": "S8", "ImU8": "U8", "ImS16": "Short",
    "ImU16": "UShort", "ImS32": "Int", "ImU32": "UInt", "ImS64": "S64", "ImU64": "U64", "ImGuiID": "UInt",
    "ImWchar": "UShort", "ImWchar16": "UShort", "ImWchar32": "UInt", "ImDrawIdx": "UShort", "ImTextureID": "U64",
    "ImGuiSelectionUserData": "S64", "ImGuiSortDirection": "U8", "size_t": "UInt", "ImFontAtlasRectId": "Int",
    "ImGuiDir": "Int", "ImGuiKey": "Int", "ImGuiMouseSource": "Int", "ImTextureFormat": "Int", "ImTextureStatus": "Int",
    "ImGuiSelectionRequestType": "Int", "ImGuiDataType": "Int", "ImGuiCol": "Int",
}


def die(message):
    sys.exit("gen_imgui_views: " + message)


def preprocess(text):
    """The header with comments gone and #if blocks resolved for VanaCore's configuration (obsolete functions kept)."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    out, stack = [], []  # stack of (taking, taken_any)
    for line in text.split("\n"):
        s = line.strip()
        m = re.match(r"#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)", s)
        if m:
            kind, rest = m.group(1), m.group(2).strip()
            outer = all(t for t, _ in stack)
            if kind in ("ifdef", "ifndef", "if"):
                if kind == "ifdef":
                    cond = rest in DEFINED
                elif kind == "ifndef":
                    cond = rest not in DEFINED
                else:
                    cond = _cond(rest)
                stack.append((outer and cond, cond))
            elif kind == "elif":
                taking, taken = stack.pop()
                cond = not taken and _cond(rest)
                stack.append((all(t for t, _ in stack) and cond, taken or cond))
            elif kind == "else":
                taking, taken = stack.pop()
                stack.append((all(t for t, _ in stack) and not taken, True))
            else:
                stack.pop()
            continue
        if s.startswith("#"):
            continue
        if all(t for t, _ in stack):
            out.append(line)
    return "\n".join(out)


def _cond(expr):
    expr = re.sub(r"defined\s*\(\s*(\w+)\s*\)", lambda m: "1" if m.group(1) in DEFINED else "0", expr)
    expr = re.sub(r"defined\s+(\w+)", lambda m: "1" if m.group(1) in DEFINED else "0", expr)
    expr = expr.replace("&&", " and ").replace("||", " or ").replace("!", " not ")
    expr = re.sub(r"\b[A-Za-z_]\w*\b", lambda m: m.group(0) if m.group(0) in ("and", "or", "not") else "0", expr)
    try:
        return bool(eval(expr, {"__builtins__": {}}))
    except Exception:
        return False


def struct_body(text, name):
    m = re.search(r"\nstruct %s\s*\n?\s*\{" % re.escape(name), text)
    if not m:
        die("no struct " + name)
    i, depth = m.end(), 1
    start = i
    while depth:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    return text[start:i - 1]


def statements(body):
    """The body's statements at depth 0 (nested struct and function bodies dropped)."""
    out, cur, depth = [], "", 0
    for c in body:
        if c == "{":
            depth += 1
            if depth == 1:
                cur += "{}"
            continue
        if c == "}":
            depth -= 1
            if depth == 0 and cur.strip().endswith("{}") and re.search(r"\)\s*(const)?\s*\{\}$|^\s*\w[\w\s:<>\*&,=\"'.\(\)\[\]-]*\)\s*(const)?\s*(:.*)?\{\}$", cur.strip()):
                cur = ""  # an inline function: no semicolon follows
            continue
        if depth:
            continue
        if c == ";":
            out.append(cur.strip())
            cur = ""
            continue
        cur += c
    return [s for s in out if s]


def fields_of(text, name):
    fields, skipped = [], []
    for st in statements(struct_body(text, name)):
        st = " ".join(st.split())
        if st.startswith(("typedef", "static", "enum", "friend", "IMGUI_API", "inline", "constexpr", "template", "using")):
            continue
        if "{}" in st:  # a nested struct, or a function
            continue
        if re.search(r"\boperator\b", st):
            continue
        if "(" in st:
            if re.search(r"\(\s*\*\s*\w+\s*\)\s*\(", st):
                skipped.append(st)  # a function pointer
            continue
        st = re.sub(r"\s*=\s*[^,]+", "", st)  # a default member initializer
        m = re.match(r"^(.*?)\s*\b(\w+)\s*(\[[^\]]*\])?\s*(:\s*\d+)?\s*((?:,\s*\w+\s*(?:\[[^\]]*\])?\s*)*)$", st)
        if not m:
            skipped.append(st)
            continue
        base = m.group(1).strip()
        names = [(m.group(2), m.group(3), m.group(4))]
        for extra in re.findall(r",\s*(\w+)\s*(\[[^\]]*\])?", m.group(5) or ""):
            names.append((extra[0], extra[1] or None, None))
        for n, arr, bits in names:
            fields.append((base, n, arr, bits))
    return fields, skipped


def kind_of(t, known_structs):
    """(kind, element kind or None, view or None) for a field's type, or None."""
    t = " ".join(t.replace("const ", "").split())
    t = t.replace(" *", "*")
    m = re.match(r"^ImVector<\s*(.+?)\s*>$", t)
    if m:
        inner = m.group(1).replace(" *", "*")
        if inner.endswith("*"):
            target = inner[:-1].strip()
            if target in known_structs:
                return ("Vector", "Object", target)
            if target == "char":
                return ("Vector", "String", None)
            return ("Vector", "Pointer", None)
        if inner in SCALARS:
            return ("Vector", SCALARS[inner], None)
        if inner in ("ImVec2", "ImVec4"):
            return ("Vector", inner[2:], None)
        if inner in known_structs:
            return ("Vector", "Embedded", inner)
        return None
    if t.endswith("*"):
        target = t[:-1].strip()
        if target == "char":
            return ("String", None, None)
        if target in known_structs:
            return ("Object", None, target)
        return ("Pointer", None, None)
    if t in SCALARS:
        return (SCALARS[t], None, None)
    if t in ("ImVec2", "ImVec4"):
        return (t[2:], None, None)
    if t in known_structs:
        return ("Embedded", None, t)
    if re.match(r"^Im(Gui|Draw|Font)\w*Flags$", t) or t in ("ImGuiKeyChord", "ImGuiViewportFlags"):
        return ("Int", None, None)
    return None


def main():
    text = preprocess(open(IMGUI_H, encoding="utf-8").read().replace("\r\n", "\n"))
    known = set(STRUCTS)
    out = [
        "// Generated by tools/gen_imgui_views.py from third_party/imgui/imgui.h: do not edit. Included in lua_imgui.cpp:",
        "// ImGui's structures' fields, as views (see there).",
        "",
    ]
    for s in STRUCTS:
        out.append("View& view_%s()" % s)
        out.append("{")
        out.append('    static View view{"%s"};' % s)
        out.append("    return view;")
        out.append("}")
    out.append("")
    out.append("void fill_views()")
    out.append("{")
    unknown = []
    count = 0
    for s in STRUCTS:
        fields, skipped = fields_of(text, s)
        lines = []
        for t, n, arr, bits in fields:
            k = kind_of(t, known)
            if bits:
                if k is None or k[0] not in ("UInt", "Int"):
                    unknown.append("%s.%s (%s)" % (s, n, t))
                    continue
                lines.append('        getter("%s", [](lua_State* L, const void* b) { lua_pushnumber(L, static_cast<const T*>(b)->%s); },'
                             ' [](lua_State* L, void* b, int i) { static_cast<T*>(b)->%s = static_cast<unsigned>(opt_u32(L, i, 0)); }),'
                             % (n, n, n))
                count += 1
                continue
            if k is None:
                unknown.append("%s.%s (%s)" % (s, n, t))
                continue
            kind, element, view = k
            off = "offsetof(T, %s)" % n
            if arr:
                if kind in ("Vector", "Object", "Pointer"):
                    unknown.append("%s.%s (%s%s)" % (s, n, t, arr))
                    continue
                if kind == "S8" and t.strip().endswith("char"):
                    lines.append('        value("%s", %s, Kind::CharArray, false),' % (n, off))
                    count += 1
                    continue
                v = "&view_%s()" % view if view else "nullptr"
                lines.append('        array("%s", %s, Kind::%s, static_cast<int>(std::extent_v<decltype(T::%s)>), %s, sizeof(T::%s[0])),'
                             % (n, off, kind, n, v, n))
            elif kind == "Vector":
                v = "&view_%s()" % view if view else "nullptr"
                lines.append('        vector("%s", %s, Kind::%s, %s, sizeof(decltype(T::%s)::value_type)),' % (n, off, element, v, n))
            elif kind in ("Object", "Embedded"):
                lines.append('        object("%s", %s, &view_%s(), Kind::%s),' % (n, off, view, kind))
            else:
                writable = "false" if kind in ("String", "Pointer") else "true"
                lines.append('        value("%s", %s, Kind::%s, %s),' % (n, off, kind, writable))
            count += 1
        out.append("    {")
        out.append("        using T = %s;" % s)
        out.append("        view_%s().fields = {" % s)
        out += ["    " + l for l in lines]
        out.append("        };")
        out.append("    }")
    out.append("}")
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    print("gen_imgui_views: %d structures, %d fields" % (len(STRUCTS), count))
    if unknown:
        print("gen_imgui_views: left out: " + ", ".join(unknown))


if __name__ == "__main__":
    main()
