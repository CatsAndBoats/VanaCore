"""Generates, from Ashita's IGuiManager (the Ashita SDK's imgui.h, interfaces 4.16 and 4.30) and VanaCore's Dear ImGui
(third_party/imgui: 1.92.3 WIP docking at a28cb615, the version interface 4.30 wraps), how VanaCore gives ImGui to
Ashita's plugins and addons:

  src/core/compat/ashita/gui_v430.inc: for interface 4.30 plugins, each method a static function that calls ImGui's
      own. IGuiManager is ImGui's API as virtual methods, one to one, so each function takes the method's parameters as
      abi.hpp declares them (the SDK's ImGui types, in sdk::v430) and passes them through `in()` (compat/ashita/gui.cpp),
      which turns the SDK's types into ImGui's (the same layout); what comes back is turned back the same way. The
      generated object (abi.hpp) calls these as Impl::<Method>(context, args...); its variadic methods (Text(fmt, ...))
      call the V forms (TextV).

  src/core/compat/ashita/gui_v416.inc: the same for interface 4.16 plugins, built against ImGui 1.81: their numbers
      (flags, colours, style variables, keys) are turned into 1.92's (imgui_legacy.inc) going in and back coming out,
      and the structures whose layout changed (ImGuiIO, ImGuiStyle, ImFont) are 1.81-shaped copies (gui.cpp). Methods
      1.92 doesn't have as they were are written by hand in compat/ashita/gui.cpp: V416_HAND_WRITTEN.

  src/core/imgui_legacy.inc: those numbers: for each of 1.81's enum and flag types, 1.81's value as 1.92's
      (in_<Type>) and back (out_<Type>), matched by name (tools/imgui_enums.py).

  src/core/lua_imgui.inc: for addons, each 4.30 method a Lua function (lua_imgui.cpp) as Ashita's Addons plugin binds
      it: called with a dot (imgui.Begin(...)), a pointer is a table whose [1] (or [1]..[N] for an array) is read and
      written back, ImVec2 and ImVec4 are tables ({x, y}) going in and several numbers coming out, missing or nil
      arguments take the C++ defaults, extra ones are ignored (on interface 4.16, a nil p_open in BeginPopupModal and
      BeginTabItem takes the argument after it too, as Ashita 4.16's did: LUA_416_OPTIONAL_TABLE). An addon that
      loaded Ashita 4.16's libs\\imgui.lua (IMGUI_VERSION_NUM 18000) passes 1.81's numbers: turned into 1.92's as for
      4.16 plugins. The 4.16 methods 4.30 dropped are bound too, for those addons. Functions that draw do nothing
      outside a frame. Overloaded methods, and those whose parameters don't map one to one (printf-style text, text
      buffers, item lists, callbacks), are written by hand in lua_imgui.cpp or not given to Lua: LUA_HAND_WRITTEN,
      LUA_NOT_BOUND.

Usage (each SDK is the Ashita v4 SDK's plugins/sdk folder):
    python tools/gen_ashita_gui.py <sdk 4.16 folder> <sdk 4.30 folder>
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import imgui_enums  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGUI = os.path.join(ROOT, "third_party", "imgui")
OUT_V416 = os.path.join(ROOT, "src", "core", "compat", "ashita", "gui_v416.inc")
OUT_V430 = os.path.join(ROOT, "src", "core", "compat", "ashita", "gui_v430.inc")
OUT_LEGACY = os.path.join(ROOT, "src", "core", "imgui_legacy.inc")
OUT_LUA = os.path.join(ROOT, "src", "core", "lua_imgui.inc")

COUNTS = {"4.16": 371, "4.30": 418}

# Plugins, both interfaces: Ashita's own (visibility, fonts through the shared atlas), or needing care (a context or
# the allocators pulled from under VanaCore, the frame VanaCore runs): compat/ashita/gui.cpp.
HAND_WRITTEN = {
    "GetVisible", "SetVisible", "DestroyContext", "SetAllocatorFunctions", "MemAlloc", "MemFree",
    "AddFontFromFileTTF", "AddFontFromMemoryCompressedTTF", "AddFontFromMemoryCompressedBase85TTF",
    "NewFrame", "EndFrame", "Render",
    # through layout.hpp: windows moved and scaled from the Layout tab
    "Begin", "End", "SetWindowPos",
}
# Interface 4.16 plugins: what 1.92 changed (another signature, another name, a structure of another layout).
V416_HAND_WRITTEN = {
    "CreateContext", "GetIO", "GetStyle", "ShowStyleEditor", "StyleColorsDark", "StyleColorsLight",
    "StyleColorsClassic", "PushFont", "GetFont", "ImageButton", "InputText", "InputTextMultiline", "InputTextWithHint",
    "ListBoxHeader", "ListBoxFooter", "CalcListClipping", "GetKeyIndex", "IsKeyDown", "IsKeyPressed", "IsKeyReleased",
    "GetKeyPressedAmount", "CaptureKeyboardFromApp", "CaptureMouseFromApp", "PushAllowKeyboardFocus",
    "PopAllowKeyboardFocus", "GetWindowContentRegionWidth",
}

# Addons: written by hand in lua_imgui.cpp (besides the overloaded methods, which always are).
LUA_HAND_WRITTEN = {
    "GetVisible", "SetVisible", "AddFontFromFileTTF", "AddFontFromMemoryCompressedTTF",
    "AddFontFromMemoryCompressedBase85TTF",
    "Text", "TextColored", "TextDisabled", "TextWrapped", "LabelText", "BulletText", "SetTooltip", "SetItemTooltip",
    "LogText", "DebugLog", "TextUnformatted", "CalcTextSize", "InputText", "InputTextMultiline",
    "InputTextWithHint", "ListBox", "ColorPicker4", "ColorConvertRGBtoHSV", "ColorConvertHSVtoRGB",
    "CalcListClipping", "SetNextWindowSizeConstraints", "PushFont", "Image", "ImageWithBg", "ImageButton",
    "GetKeyIndex", "GetIO", "GetStyle", "GetPlatformIO", "PushStyleColor", "PushStyleVar", "PushStyleVarX",
    "PushStyleVarY", "GetColorU32", "SaveIniSettingsToMemory", "LoadIniSettingsFromMemory", "MemAlloc", "MemFree",
    "SetDragDropPayload", "AcceptDragDropPayload", "GetDragDropPayload", "BeginMultiSelect", "EndMultiSelect",
    "DockSpace", "DockSpaceOverViewport", "SetNextWindowClass", "GetBackgroundDrawList", "GetForegroundDrawList",
    "GetFontBaked", "BeginChild", "ListBoxHeader", "DragScalar", "DragScalarN", "SliderScalar", "SliderScalarN",
    "VSliderScalar", "InputScalar", "InputScalarN", "BeginDisabled", "EndDisabled", "FindViewportByPlatformHandle",
    "Begin", "End",  # through layout.hpp (SetWindowPos is overloaded: hand-written anyway)
    "IsWindowHovered",  # only the flags 1.92 takes for a window (lua_imgui_extra.inc)
}
# Addons: not theirs to call (the frame and the context are VanaCore's), or not bound by Ashita either (the V forms).
LUA_NOT_BOUND = {
    "CreateContext", "DestroyContext", "GetCurrentContext", "SetCurrentContext", "NewFrame", "EndFrame", "Render",
    "GetDrawData", "SetAllocatorFunctions", "GetAllocatorFunctions", "DebugCheckVersionAndDataLayout",
    "SetStateStorage", "GetStateStorage", "GetDrawListSharedData", "UpdatePlatformWindows",
    "RenderPlatformWindowsDefault", "DestroyPlatformWindows", "DebugStartItemPicker",
}
# Addons: 4.16's methods ImGui 1.92 dropped, as what they did ({0}, {1}: the arguments).
LUA_LEGACY_CALLS = {
    "GetWindowContentRegionWidth": "(ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x)",
    "PushAllowKeyboardFocus": "ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop, !{0})",
    "PopAllowKeyboardFocus": "ImGui::PopItemFlag()",
    "ListBoxFooter": "ImGui::EndListBox()",
    "CaptureKeyboardFromApp": "ImGui::SetNextFrameWantCaptureKeyboard({0})",
    "CaptureMouseFromApp": "ImGui::SetNextFrameWantCaptureMouse({0})",
    # The key the addon gives may be a Win32 code key_in has no ImGui key for, or a raw number: a key ImGui's
    # IsKeyDown reads out of bounds for (imgui_legacy.hpp key_ok). These read as "not pressed", as 1.81 did.
    "IsKeyDown": "legacy::is_key_down({0})",
    "IsKeyPressed": "legacy::is_key_pressed({0}, {1})",
    "IsKeyReleased": "legacy::is_key_released({0})",
    "GetKeyPressedAmount": "legacy::key_pressed_amount({0}, {1}, {2})",
}

# Addons on interface 4.16: a nil p_open takes the argument after it too, as Ashita 4.16's binding read it
# (lua_imgui.cpp ashita416_optional_table), by the p_open's argument index. Begin, written by hand, does the same.
LUA_416_OPTIONAL_TABLE = {"BeginPopupModal": 2, "BeginTabItem": 2}

# interface 4.30 forwarders: the same guard, over key_in's result (the SDK hands ImGui's own keys).
PLUGIN_KEY_CALLS = {
    "IsKeyDown": "legacy::is_key_down",
    "IsKeyPressed": "legacy::is_key_pressed",
    "IsKeyReleased": "legacy::is_key_released",
    "GetKeyPressedAmount": "legacy::key_pressed_amount",
}

# Addons: callable outside a frame (when an addon loads, or from a command): they read or set state and draw nothing.
LUA_OUTSIDE_FRAME = {
    "GetIO", "GetStyle", "GetVersion", "StyleColorsDark", "StyleColorsLight", "StyleColorsClassic", "GetFont",
    "GetFontSize", "GetFontTexUvWhitePixel", "GetStyleColorVec4", "GetTextLineHeight", "GetTextLineHeightWithSpacing",
    "GetFrameHeight", "GetFrameHeightWithSpacing", "GetTime", "GetFrameCount", "GetStyleColorName",
    "ColorConvertU32ToFloat4", "ColorConvertFloat4ToU32", "GetKeyIndex", "IsKeyDown", "IsKeyPressed", "IsKeyReleased",
    "GetKeyPressedAmount", "GetKeyName", "IsMouseDown", "IsAnyMouseDown", "GetMousePos", "IsMousePosValid",
    "GetClipboardText", "SetClipboardText", "LoadIniSettingsFromDisk", "SaveIniSettingsToDisk", "GetMouseCursor",
    "SetColorEditOptions", "CaptureKeyboardFromApp", "CaptureMouseFromApp", "IsMouseClicked", "IsMouseReleased",
    "IsMouseDoubleClicked", "IsMouseDragging", "GetMouseDragDelta", "ResetMouseDragDelta", "IsMouseReleasedWithDelay",
    "GetMouseClickedCount", "SetNextFrameWantCaptureKeyboard", "SetNextFrameWantCaptureMouse", "GetMainViewport",
}

# Plugins: types returned by reference or pointer that are ImGui's structures are reinterpreted as the SDK's; ImVec2
# and ImVec4 returned by value are copied bit for bit.
BY_VALUE = {"ImVec2", "ImVec4"}
# ImGui 1.92's enums that are enum types (the SDK's are int): an int argument is cast to them.
ENUM_CLASSES = {"ImGuiDir", "ImGuiKey", "ImGuiMouseSource", "ImGuiSortDirection"}
UNSIGNED = {"unsigned int", "ImU32", "ImGuiID", "size_t"}


def die(message):
    sys.exit("gen_ashita_gui: " + message)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read().replace("\r\n", "\n")


def write(path, lines):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def split_top(text, sep=","):
    """Splits on `sep` outside parentheses, brackets and quotes."""
    parts, depth, quote, cur = [], 0, None, ""
    for c in text:
        if quote:
            cur += c
            if c == quote:
                quote = None
            continue
        if c in "\"'":
            quote = c
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == sep and depth == 0:
            parts.append(cur)
            cur = ""
            continue
        cur += c
    if cur.strip():
        parts.append(cur)
    return [p.strip() for p in parts]


def methods_of(sdk_folder, version):
    text = read(os.path.join(sdk_folder, "imgui.h"))
    m = re.search(r"\ninterface IGuiManager\s*\{(.*?)\n\};", text, re.S)
    if not m:
        die("no IGuiManager in %s's imgui.h" % version)
    body = re.sub(r"//[^\n]*", "", m.group(1))
    found = re.findall(r"virtual\s+(?:IMGUI_API\s+)?(.+?)\s+(\w+)\s*\((.*?)\)\s*(const)?\s*=\s*0\s*;", body, re.S)
    if len(found) != COUNTS[version]:
        die("expected %d IGuiManager methods in %s, found %d" % (COUNTS[version], version, len(found)))
    out = []
    for ret, name, params, _ in found:
        ret = " ".join(ret.replace(" *", "*").split())
        params = " ".join(params.split())
        plist = [] if params in ("", "void") else split_top(params)
        out.append((ret, name, plist))
    return text, out


def param_decl(param, index):
    """A parameter declared as `aN`, its default dropped; arrays as pointers."""
    param = split_top(param, "=")[0].strip()
    name = "a%d" % index
    m = re.match(r"^(.*)\(\s*\*\s*(\w+)\s*\)\s*(\(.*\))$", param)  # function pointer
    if m:
        return "%s(*%s)%s" % (m.group(1).strip(), name, m.group(3))
    m = re.match(r"^(.*?)(\w+)\s*(\[[^\]]*\])?$", param)
    if not m:
        die("can't read parameter: " + param)
    type_ = " ".join(m.group(1).replace(" *", "*").split())
    if m.group(3):
        type_ += "*"
    if not type_:
        die("no type in parameter: " + param)
    return "%s %s" % (type_, name)


def parse_param(param):
    """(type, array size or None, default or None); None for a function pointer."""
    parts = split_top(param, "=")
    decl, default = parts[0].strip(), (parts[1].strip() if len(parts) > 1 else None)
    if re.match(r"^.*\(\s*\*\s*\w+\s*\)\s*\(.*\)$", decl):
        return None
    m = re.match(r"^(.*?)(\w+)\s*(?:\[([^\]]*)\])?$", decl)
    if not m:
        die("can't read parameter: " + param)
    return " ".join(m.group(1).replace(" *", "*").split()), m.group(3), default


def param_type(param):
    parsed = parse_param(param)
    return None if parsed is None else parsed[0]


# ---- 1.81's numbers as 1.92's -------------------------------------------------------------------------------------

def generate_legacy(old_text, new_text):
    old_enums = imgui_enums.parse(old_text)
    new_enums = imgui_enums.parse(new_text)
    types = re.findall(r"typedef int (Im\w+);", old_text)
    out = [
        "// Generated by tools/gen_ashita_gui.py from the Ashita 4.16 SDK's imgui.h (ImGui 1.81) and third_party/imgui",
        "// (ImGui 1.92): do not edit. Included in imgui_legacy.hpp: 1.81's numbers as 1.92's (in_), and back (out_).",
        "",
    ]
    converted = set()
    for t in types:
        if t in ("ImGuiKey", "ImGuiNavInput"):
            continue  # keys: imgui_legacy.hpp (1.81's are Win32's key codes); nav inputs: gone
        table, dropped = imgui_enums.mapping(old_enums, new_enums, t)
        if table is None:
            continue
        converted.add(t)
        identity = all(k == v for k, v in table.items())
        if dropped:
            out.append("// %s: 1.92 has no %s" % (t, ", ".join(dropped)))
        if imgui_enums.is_flags(t):
            if identity:
                out.append("inline int in_%s(int v) { return v; }" % t)
                out.append("inline int out_%s(int v) { return v; }" % t)
            else:
                pairs = sorted(table.items())
                out.append("inline int in_%s(int v)" % t)
                out.append("{")
                out.append("    int r = 0;")
                for bit, new in pairs:
                    out.append("    if (v & 0x%X) r |= 0x%X;" % (bit, new & 0xFFFFFFFF))
                out.append("    return r;")
                out.append("}")
                out.append("inline int out_%s(int v)" % t)
                out.append("{")
                out.append("    int r = 0;")
                for bit, new in pairs:
                    if new != 0:
                        out.append("    if ((v & 0x%X) == 0x%X) r |= 0x%X;" % (new & 0xFFFFFFFF, new & 0xFFFFFFFF, bit))
                out.append("    return r;")
                out.append("}")
        else:
            if identity:
                out.append("inline int in_%s(int v) { return v; }" % t)
                out.append("inline int out_%s(int v) { return v; }" % t)
            else:
                pairs = sorted(table.items())
                out.append("inline int in_%s(int v)" % t)
                out.append("{")
                out.append("    switch (v) {")
                for old, new in pairs:
                    out.append("    case %d: return %d;" % (old, new))
                out.append("    default: return v;")
                out.append("    }")
                out.append("}")
                out.append("inline int out_%s(int v)" % t)
                out.append("{")
                out.append("    switch (v) {")
                seen = set()
                for old, new in pairs:
                    if new in seen:
                        continue
                    seen.add(new)
                    out.append("    case %d: return %d;" % (new, old))
                out.append("    default: return -1;")
                out.append("    }")
                out.append("}")
        out.append("")
    write(OUT_LEGACY, out)
    print("gen_ashita_gui: %d of 1.81's types converted" % len(converted))
    return converted


# ---- Plugins ------------------------------------------------------------------------------------------------------

def generate_plugins(methods, path, version, legacy):
    v = "v" + version.replace(".", "")
    out = [
        "// Generated by tools/gen_ashita_gui.py from the Ashita SDK's imgui.h (interface %s): do not edit." % version,
        "// Included in compat/ashita/gui.cpp's Gui%s, where the SDK's ImGui types (sdk::%s) are the unqualified" % (v.upper()[0] + v[1:], v),
        "// names and ImGui's own are ::ImVec2 and the like.",
        "",
    ]
    count = 0
    hand = HAND_WRITTEN | (V416_HAND_WRITTEN if legacy else set())
    for ret, name, plist in methods:
        if name in hand:
            continue
        if plist and plist[-1] == "...":
            continue  # the object calls the V form
        decls = ["void*"] + [param_decl(p, i) for i, p in enumerate(plist)]
        args = []
        for i, p in enumerate(plist):
            t = param_type(p)
            if t == "ImGuiKey":  # 1.92's keys, or Win32's key codes (what 1.81 took)
                args.append("legacy::key_in(a%d)" % i)
            elif legacy and t in legacy:
                arg = "legacy::in_%s(a%d)" % (t, i)
                args.append("static_cast<::%s>(%s)" % (t, arg) if t in ENUM_CLASSES else arg)
            elif t in ENUM_CLASSES:
                args.append("static_cast<::%s>(a%d)" % (t, i))
            else:
                args.append("in(a%d)" % i)
        callee = PLUGIN_KEY_CALLS.get(name, "ImGui::%s" % name)
        call = "%s(%s)" % (callee, ", ".join(args))
        base = re.sub(r"\bconst\b|[&*]", "", ret).strip()
        if ret == "void":
            stmt = "%s;" % call
        elif legacy and ret in legacy:
            stmt = "return legacy::out_%s(%s);" % (ret, call)
        elif ("&" in ret or "*" in ret) and base.startswith("Im"):
            stmt = "return reinterpret_cast<%s>(%s);" % (ret, call)
        elif base in BY_VALUE:
            stmt = "return std::bit_cast<%s>(%s);" % (ret, call)
        else:
            stmt = "return %s;" % call
        out.append("static %s %s(%s) { %s }" % (ret, name, ", ".join(decls), stmt))
        count += 1
    write(path, out)
    print("gen_ashita_gui: interface %s: %d forwarders (%d methods, %d hand-written or variadic)"
          % (version, count, len(methods), len(methods) - count))


# ---- Addons -------------------------------------------------------------------------------------------------------

def is_int_type(t):
    return t in ("int", "short") or t == "ImGuiID" or (re.match(r"^Im(Gui|Draw|Font)\w*$", t) is not None and
                                                       t not in ("ImGuiIO", "ImGuiStyle", "ImGuiViewport"))


def lua_reader(type_, array, default, var, index, legacy):
    """(declaration line, argument expression, store line or None), or None when unsupported."""
    if array is not None:
        if type_ in ("float", "int") and array.isdigit():
            return ("Array<%s, %s> %s(L, %d);" % (type_, array, var, index), "%s.data()" % var, "%s.store(L);" % var)
        return None
    if default == "NULL":
        default = "nullptr"
    if type_ == "const char*":
        d = default if default is not None else '""'
        return ("const char* %s = opt_string(L, %d, %s);" % (var, index, d), var, None)
    if type_ == "bool":
        return ("const bool %s = opt_bool(L, %d, %s);" % (var, index, default or "false"), var, None)
    if type_ in ("float", "double"):
        fn = "opt_float" if type_ == "float" else "opt_double"
        return ("const %s %s = %s(L, %d, %s);" % (type_, var, fn, index, default or "0"), var, None)
    if type_ in UNSIGNED:
        return ("const %s %s = static_cast<%s>(opt_u32(L, %d, %s));" % (type_, var, type_, index, default or "0"), var, None)
    if type_ == "ImGuiKey":
        return ("const ImGuiKey %s = opt_key(L, %d, %s);" % (var, index, default or "ImGuiKey_None"), var, None)
    if type_ == "ImGuiKeyChord":
        return ("const ImGuiKeyChord %s = opt_chord(L, %d, %s);" % (var, index, default or "0"), var, None)
    if type_ == "ImGuiSelectionUserData":
        return ("const ImGuiSelectionUserData %s = static_cast<ImGuiSelectionUserData>(opt_double(L, %d, 0));" % (var, index), var, None)
    if is_int_type(type_):
        if type_ in legacy:
            return ("const %s %s = static_cast<%s>(opt_enum(L, %d, %s, &legacy::in_%s));" % (type_, var, type_, index, default or "0", type_), var, None)
        return ("const %s %s = static_cast<%s>(opt_int(L, %d, %s));" % (type_, var, type_, index, default or "0"), var, None)
    if type_ in ("const ImVec2&", "ImVec2"):
        return ("const ImVec2 %s = opt_vec2(L, %d, %s);" % (var, index, default or "ImVec2(0, 0)"), var, None)
    if type_ == "const ImVec4&":
        return ("const ImVec4 %s = opt_vec4(L, %d, %s);" % (var, index, default or "ImVec4(0, 0, 0, 0)"), var, None)
    if type_ in ("ImTextureID", "ImTextureRef"):
        return ("const ImTextureRef %s = opt_texture(L, %d);" % (var, index), var, None)
    if type_ == "ImFont*":
        return ("ImFont* %s = opt_font(L, %d);" % (var, index), var, None)
    if type_ == "ImGuiStyle*":
        return ("ImGuiStyle* %s = opt_style(L, %d);" % (var, index), var, None)
    if type_ in ("ImGuiViewport*", "const ImGuiViewport*"):
        return ("ImGuiViewport* %s = opt_viewport(L, %d);" % (var, index), var, None)
    if type_ == "bool*":
        return ("BoolRef %s(L, %d);" % (var, index), "%s.get()" % var, "%s.store(L);" % var)
    if type_ in ("int*", "float*", "double*", "unsigned int*"):
        return ("Array<%s, 1> %s(L, %d);" % (type_[:-1], var, index), "%s.get()" % var, "%s.store(L);" % var)
    if type_ == "const ImVec2*":
        return ("OptVec2 %s(L, %d);" % (var, index), "%s.get()" % var, None)
    return None


def lua_return(ret, legacy):
    """(statements after `r` is the result), or None when unsupported."""
    if ret == "void":
        return "return 0;"
    if ret == "bool":
        return "lua_pushboolean(L, r); return 1;"
    if ret in legacy:
        return "lua_pushnumber(L, static_cast<lua_Number>(out_enum(L, r, &legacy::out_%s))); return 1;" % ret
    if ret in ("float", "double", "int") or ret in UNSIGNED or is_int_type(ret):
        return "lua_pushnumber(L, static_cast<lua_Number>(r)); return 1;"
    if ret == "ImVec2":
        return "return push_vec2(L, r);"
    if ret in ("ImVec4", "const ImVec4&"):
        return "return push_vec4(L, r);"
    if ret == "const char*":
        return "return push_string(L, r);"
    pushers = {"ImDrawList*": "push_draw_list", "ImFont*": "push_font", "ImGuiTableSortSpecs*": "push_sort_specs",
               "ImGuiViewport*": "push_viewport"}
    if ret in pushers:
        return "return %s(L, r);" % pushers[ret]
    return None


def generate_lua(methods_430, methods_416, legacy):
    names_430 = {name for _, name, _ in methods_430}
    by_name = {}
    for ret, name, plist in methods_430:
        by_name.setdefault(name, []).append((ret, plist, False))
    # 4.16's that 4.30 dropped, for addons written for it (ImGui 1.92 still has most as obsolete functions)
    for ret, name, plist in methods_416:
        if name not in names_430:
            by_name.setdefault(name, []).append((ret, plist, True))
    out = [
        "// Generated by tools/gen_ashita_gui.py from the Ashita SDK's imgui.h (interface 4.30, and 4.16's methods 4.30",
        "// dropped): do not edit. Included in lua_imgui.cpp: Ashita's Lua binding of IGuiManager, method by method.",
        "",
    ]
    names, skipped = [], []
    for name, overloads in by_name.items():
        if name in LUA_HAND_WRITTEN or name in LUA_NOT_BOUND or name.endswith("V"):
            continue
        if len(overloads) > 1:
            skipped.append(name + " (overloaded)")
            continue
        ret, plist, from_416 = overloads[0]
        if plist and plist[-1] == "...":
            skipped.append(name + " (printf-style)")
            continue
        body, args, stores, ok = [], [], [], True
        for i, p in enumerate(plist):
            parsed = parse_param(p)
            reader = lua_reader(*parsed, "a%d" % i, i + 1, legacy) if parsed else None
            if reader is None:
                ok = False
                break
            body.append(reader[0])
            args.append(reader[1])
            if reader[2]:
                stores.append(reader[2])
        tail = lua_return(ret, legacy)
        if not ok or tail is None:
            skipped.append(name + " (parameter or result)")
            continue
        call = LUA_LEGACY_CALLS[name].format(*args) if name in LUA_LEGACY_CALLS else "ImGui::%s(%s)" % (name, ", ".join(args))
        uses_state = bool(plist) or ret != "void"
        lines = ["// %s %s(%s)%s" % (ret, name, ", ".join(plist), " (4.16)" if from_416 else ""),
                 "int %s(lua_State*%s)" % (name, " L" if uses_state else ""), "{"]
        if name not in LUA_OUTSIDE_FRAME:
            lines.append("    if (!gui::in_frame())")
            lines.append("        return 0;")
        if name in LUA_416_OPTIONAL_TABLE:
            lines.append("    ashita416_optional_table(L, %d);" % LUA_416_OPTIONAL_TABLE[name])
        lines += ["    " + b for b in body]
        if ret == "void":
            lines.append("    %s;" % call)
            lines += ["    " + s for s in stores]
            lines.append("    return 0;")
        else:
            result_type = "const auto" if not ret.endswith("&") else "auto&"
            lines.append("    %s r = %s;" % (result_type, call))
            lines += ["    " + s for s in stores]
            lines.append("    " + tail)
        lines += ["}", ""]
        out += lines
        names.append(name)
    out.append("const luaL_Reg kGenerated[] = {")
    for n in names:
        out.append('    {"%s", &%s},' % (n, n))
    out.append("    {nullptr, nullptr},")
    out.append("};")
    write(OUT_LUA, out)
    print("gen_ashita_gui: %d Lua functions generated" % len(names))
    unbound = sorted(set(skipped))
    if unbound:
        print("gen_ashita_gui: for lua_imgui.cpp to write by hand or leave out: " + ", ".join(unbound))


def main():
    if len(sys.argv) != 3:
        die("usage: gen_ashita_gui.py <sdk 4.16 folder> <sdk 4.30 folder>")
    old_text, methods_416 = methods_of(sys.argv[1], "4.16")
    _, methods_430 = methods_of(sys.argv[2], "4.30")
    new_text = read(os.path.join(IMGUI, "imgui.h")) + read(os.path.join(IMGUI, "imgui_internal.h"))
    legacy = generate_legacy(old_text, new_text)
    generate_plugins(methods_430, OUT_V430, "4.30", None)
    generate_plugins(methods_416, OUT_V416, "4.16", legacy)
    generate_lua(methods_430, methods_416, legacy)


if __name__ == "__main__":
    main()
