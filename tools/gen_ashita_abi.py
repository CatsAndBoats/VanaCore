"""Generates src/core/compat/ashita/abi.hpp: the Ashita v4 plugin interfaces, as C++ declarations
MSVC lays out exactly as Ashita's own SDK does, for each interface version VanaCore hosts.

Ashita plugins are C++ objects talking to C++ objects: every call goes through a vtable, so
VanaCore's objects must have the SDK's virtual functions in the SDK's order, with the same
parameter and return types (sizes, and how MSVC passes and returns them). This script reads the
SDK headers and writes, for each version:

  - the interfaces (IAshitaCore, IChatManager, ..., IGuiManager), pure virtual as in the SDK;
  - for each, a class template `<Name>Object<Impl>` implementing every method: a method calls
    `Impl::<Method>(context, args...)` when Impl has a matching static function, and otherwise
    reports the call to sdk::unimplemented() once and returns zero/false/nullptr.

The backend (src/core/compat/ashita/*.cpp) supplies the Impl classes; whatever it doesn't
implement yet stays callable and harmless.

Usage (the SDK headers are the Ashita v4 SDK's plugins/sdk folder, one per version):
    python tools/gen_ashita_abi.py <sdk 4.16 folder> <sdk 4.30 folder> src/core/compat/ashita/abi.hpp

The SDK is LGPL-3.0 (see THIRD_PARTY_NOTICES.md); the generated header carries its notice.
"""

import os
import re
import sys

VERSIONS = [("v416", "4.16"), ("v430", "4.30")]

FFXI_HEADERS = ["enums.h", "autofollow.h", "castbar.h", "config.h", "entity.h", "inventory.h", "party.h",
                "player.h", "target.h"]

# Interfaces in the order they are written (dependencies first). Anything else with virtual
# functions in the headers is an error, so a new SDK can't slip an interface past this list.
INTERFACES = [
    "IAutoFollow", "ICastBar", "IEntity", "IInventory", "IParty", "IPlayer", "IRecast", "ITarget",
    "IController", "IKeyboard", "IMouse", "IXInput", "IInputManager",
    "IChatManager", "IConfigurationManager", "IMemoryManager", "IOffsetManager", "IPacketManager",
    "IPluginManager", "IPolPluginManager", "IPointerManager", "IResourceManager",
    "IPrimitiveObject", "IPrimitiveManager", "IFontObject", "IFontManager", "ILogManager",
    "IProperties", "IGuiManager", "IAshitaCore", "IPluginBase", "IPolPluginBase",
]
# The plugin's own objects: VanaCore calls these, it never implements them.
CALLED_ONLY = {"IPluginBase", "IPolPluginBase"}
DATA_STRUCTS = ["IAbility", "ISpell", "IMonAbility", "IItem", "IStatusIcon"]

# ImGui's types as the wrapper's signatures use them, per SDK version. Only sizes and how MSVC
# passes them matter: enums and flags are int, callbacks are function pointers, and structs passed
# or returned by value keep their exact layout.
IMGUI_COMMON = """
    using ImGuiID = unsigned int;
    using ImS8 = signed char;
    using ImU8 = unsigned char;
    using ImS16 = signed short;
    using ImU16 = unsigned short;
    using ImS32 = signed int;
    using ImU32 = unsigned int;
    using ImS64 = signed long long;
    using ImU64 = unsigned long long;
    using ImWchar16 = unsigned short;
    using ImWchar32 = unsigned int;
    using ImWchar = ImWchar16;
    using ImDrawIdx = unsigned short;
    struct ImVec2 {
        float x = 0.0f, y = 0.0f;
        constexpr ImVec2() = default;
        constexpr ImVec2(float _x, float _y) : x(_x), y(_y) {}
    };
    struct ImVec4 {
        float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
        constexpr ImVec4() = default;
        constexpr ImVec4(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}
    };
    struct ImGuiContext;
    struct ImGuiIO;
    struct ImGuiStyle;
    struct ImGuiPlatformIO;
    struct ImDrawData;
    struct ImDrawList;
    struct ImDrawListSharedData;
    struct ImFont;
    struct ImFontAtlas;
    struct ImFontConfig;
    struct ImFontBaked;
    struct ImGuiInputTextCallbackData;
    struct ImGuiListClipper;
    struct ImGuiPayload;
    struct ImGuiSizeCallbackData;
    struct ImGuiStorage;
    struct ImGuiTableSortSpecs;
    struct ImGuiViewport;
    struct ImGuiWindowClass;
    struct ImGuiMultiSelectIO;
    struct ImGuiSelectionBasicStorage;
    struct ImTextureData;
    using ImGuiInputTextCallback = int (*)(ImGuiInputTextCallbackData* data);
    using ImGuiSizeCallback = void (*)(ImGuiSizeCallbackData* data);
    using ImGuiMemAllocFunc = void* (*)(size_t sz, void* user_data);
    using ImGuiMemFreeFunc = void (*)(void* ptr, void* user_data);
"""
# Every ImGui enum and flags type: int-sized.
IMGUI_INT_TYPES = [
    "ImGuiCol", "ImGuiCond", "ImGuiDataType", "ImGuiDir", "ImGuiKey", "ImGuiMouseButton", "ImGuiMouseCursor",
    "ImGuiSortDirection", "ImGuiStyleVar", "ImGuiTableBgTarget", "ImDrawFlags", "ImDrawListFlags",
    "ImFontAtlasFlags", "ImGuiBackendFlags", "ImGuiButtonFlags", "ImGuiChildFlags", "ImGuiColorEditFlags",
    "ImGuiConfigFlags", "ImGuiComboFlags", "ImGuiDockNodeFlags", "ImGuiDragDropFlags", "ImGuiFocusedFlags",
    "ImGuiHoveredFlags", "ImGuiInputFlags", "ImGuiInputTextFlags", "ImGuiItemFlags", "ImGuiKeyChord",
    "ImGuiPopupFlags", "ImGuiMultiSelectFlags", "ImGuiSelectableFlags", "ImGuiSliderFlags", "ImGuiTabBarFlags",
    "ImGuiTabItemFlags", "ImGuiTableFlags", "ImGuiTableColumnFlags", "ImGuiTableRowFlags", "ImGuiTreeNodeFlags",
    "ImGuiViewportFlags", "ImGuiWindowFlags", "ImFontFlags", "ImGuiListClipperFlags", "ImTextureFormat",
]
IMGUI_VERSION_TYPES = {
    # ImGui 1.8x (docking), as the 4.16 SDK has it: textures are void*.
    "v416": """
    using ImTextureID = void*;
""",
    # ImGui 1.92.3 (docking), as the 4.30 SDK has it: 64-bit texture ids, and ImTextureRef (16 bytes)
    # passed by value.
    "v430": """
    using ImTextureID = ImU64;
    using ImGuiSelectionUserData = ImS64;
    struct ImTextureRef {
        ImTextureData* _TexData = nullptr;
        ImTextureID _TexID = 0;
        ImTextureRef() = default;
    };
    static_assert(sizeof(ImTextureRef) == 16);
""",
}


def die(message):
    sys.exit("gen_ashita_abi: " + message)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read().replace("\r\n", "\n")


def strip_comments(text):
    """Removes // and /* */ comments, leaving string and character literals alone."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def match_brace(text, at):
    """Index just past the brace block starting at text[at] == '{'."""
    depth = 0
    for i in range(at, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i + 1
    die("unbalanced braces")


def split_top(text, sep):
    parts, depth, start = [], 0, 0
    for i, c in enumerate(text):
        if c in "([<{":
            depth += 1
        elif c in ")]>}":
            depth -= 1
        elif c == sep and depth == 0:
            parts.append(text[start:i])
            start = i + 1
    parts.append(text[start:])
    return parts


def blocks(text):
    """{name: body} for every `struct|interface|class Name ... { ... }` at any depth."""
    found = {}
    for m in re.finditer(r"\b(?:struct|interface|class)\s+(I\w+)\s*(?::[^{;]*)?\{", text):
        start = m.end() - 1
        found.setdefault(m.group(1), (text[start + 1:match_brace(text, start) - 1], m.start()))
    return found


class Method:
    def __init__(self, ret, name, params, const, variadic):
        self.ret, self.name, self.params, self.const, self.variadic = ret, name, params, const, variadic


QUALIFIERS = {"const", "CONST", "unsigned", "signed", "struct", "volatile"}


def parse_param(text, index):
    """(type text with the name replaced by a<index>, name) for one parameter."""
    text = split_top(text, "=")[0].strip()  # default value
    arg = "a%d" % index
    fp = re.search(r"\(\s*\*\s*(\w*)\s*\)", text)
    if fp:  # function pointer: bool (*getter)(void*, int, const char**)
        return text[:fp.start()] + "(*" + arg + ")" + text[fp.end():], arg
    if text.endswith(("*", "&")):  # unnamed: IAshitaCore*
        return text + " " + arg, arg
    m = re.match(r"^(.*?)(\w+)\s*((?:\[[^\]]*\]\s*)*)$", text, re.S)
    if not m:
        die("can't read parameter: " + text)
    head, last, arrays = m.group(1).strip(), m.group(2), m.group(3).strip()
    tokens = re.findall(r"\w+|[*&]", head)
    named = bool(tokens) and (tokens[-1] in "*&" or not all(t in QUALIFIERS for t in tokens))
    if arrays or named:
        return head + " " + arg + arrays, arg
    return text + " " + arg, arg


def parse_method(statement):
    s = statement
    s = re.sub(r"\bvirtual\b|\bIMGUI_API\b", " ", s)
    s = re.sub(r"\bIM_FMT(?:ARGS|LIST)\s*\(\s*\d+\s*\)", " ", s)
    s = " ".join(s.split())
    s = re.sub(r"\s*=\s*0\s*$", "", s)
    const = False
    if s.endswith(" const"):
        const, s = True, s[:-6]
    if not s.endswith(")"):
        die("can't read method: " + statement)
    depth = 0
    for i in range(len(s) - 1, -1, -1):
        if s[i] == ")":
            depth += 1
        elif s[i] == "(":
            depth -= 1
            if depth == 0:
                open_at = i
                break
    head, inner = s[:open_at].strip(), s[open_at + 1:-1].strip()
    m = re.match(r"^(.*?)(\w+)$", head)
    if not m:
        die("can't read method name: " + statement)
    ret, name = m.group(1).strip(), m.group(2)
    params, variadic = [], False
    if inner and inner != "void":
        for i, p in enumerate(split_top(inner, ",")):
            if p.strip() == "...":
                variadic = True
            else:
                params.append(parse_param(p, i))
    return Method(ret, name, params, const, variadic)


def interface_methods(body):
    methods = []
    for statement in split_top(body, ";"):
        statement = statement.strip()
        if statement.startswith("virtual"):
            methods.append(parse_method(statement))
    return methods


def signature_text(iface, m):
    """IPluginManager::Get(const char*): names a method, overloads apart."""
    types = [re.sub(r"\s*\ba\d+\b", "", p).strip() for p, _ in m.params] + (["..."] if m.variadic else [])
    return "%s::%s(%s)" % (iface, m.name, ", ".join(types))


def default_return(ret):
    if ret == "void":
        return ""
    if ret.endswith("&"):
        return "return *reinterpret_cast<std::remove_reference_t<%s>*>(sdk::zeros());" % ret
    return "return {};"


def object_class(iface, methods):
    """The implementation template for one interface."""
    lines = ["template <class Impl>", "struct %sObject final : %s {" % (iface[1:], iface),
             "    void* context = nullptr;"]
    for m in methods:
        params = [p for p, _ in m.params]
        args = ", ".join(["context"] + [a for _, a in m.params])
        signature = "%s %s(%s)%s override" % (m.ret, m.name, ", ".join(params + (["..."] if m.variadic else [])),
                                              " const" if m.const else "")
        fallback = "sdk::unimplemented(\"%s\"); %s" % (signature_text(iface, m).replace('"', '\\"'),
                                                         default_return(m.ret))
        lines.append("    %s {" % signature)
        if m.variadic:
            last = m.params[-1][1]
            call = "Impl::%sV(%s, args)" % (m.name, args)
            lines.append("        va_list args;")
            lines.append("        va_start(args, %s);" % last)
            lines.append("        if constexpr (requires { %s; }) {" % call)
            if m.ret == "void":
                lines.append("            %s;" % call)
                lines.append("            va_end(args);")
            else:
                lines.append("            %s result = static_cast<%s>(%s);" % (m.ret, m.ret, call))
                lines.append("            va_end(args);")
                lines.append("            return result;")
            lines.append("        } else {")
            lines.append("            va_end(args);")
            lines.append("            %s" % fallback)
            lines.append("        }")
        else:
            call = "Impl::%s(%s)" % (m.name, args)
            lines.append("        if constexpr (requires { %s; })" % call)
            lines.append("            %s;" % (call if m.ret == "void" else "return static_cast<%s>(%s)" % (m.ret, call)))
            lines.append("        else {")
            lines.append("            %s" % fallback)
            lines.append("        }")
        lines.append("    }")
    lines.append("};")
    return "\n".join(lines)


def declaration(iface, methods, base=None):
    lines = ["struct %s%s {" % (iface, " : " + base if base else "")]
    for m in methods:
        params = [p for p, _ in m.params] + (["..."] if m.variadic else [])
        lines.append("    virtual %s %s(%s)%s = 0;" % (m.ret, m.name, ", ".join(params), " const" if m.const else ""))
    lines.append("};")
    return "\n".join(lines)


def ffxi_structs(sdk):
    parts = []
    for name in FFXI_HEADERS:
        text = read(os.path.join(sdk, "ffxi", name))
        start = re.search(r"^namespace Ashita", text, re.M)
        if not start:
            die("no namespace in ffxi/" + name)
        text = text[start.start():]
        text = re.sub(r"^#.*$", "", text, flags=re.M)
        parts.append("// ffxi/%s\n%s" % (name, text.strip()))
    return "\n\n".join(parts)


def ashita_h_parts(text):
    """The enums namespace, the callback typedefs and the resource structs, verbatim."""
    enums = re.search(r"^namespace Ashita\n\{.*?^\} // namespace Ashita", text, re.S | re.M)
    if not enums:
        die("no Ashita enums namespace")
    typedefs = [t for t in re.findall(r"^typedef [^\n]*_f\)\([^\n]*;$", text, re.M) if "export_" not in t]
    structs = []
    for name in DATA_STRUCTS:
        m = re.search(r"^struct %s\n\{.*?^\};" % name, text, re.S | re.M)
        if not m:
            die("no struct " + name)
        structs.append(m.group(0))
    return enums.group(0), typedefs, structs


def version_block(tag, label, sdk):
    ashita = strip_comments(read(os.path.join(sdk, "Ashita.h")))
    imgui = strip_comments(read(os.path.join(sdk, "imgui.h")))
    version = re.search(r"ASHITA_INTERFACE_VERSION\s*=\s*([\d.]+)", ashita)
    if not version or version.group(1) != label:
        die("%s: expected interface version %s, found %s" % (sdk, label, version.group(1) if version else "none"))
    found = blocks(ashita)
    gui = blocks(imgui).get("IGuiManager")
    if not gui:
        die("no IGuiManager in imgui.h")
    found["IGuiManager"] = gui
    unknown = [n for n, (body, _) in found.items()
               if "virtual" in body and n not in INTERFACES and n not in ("IPlugin", "IPolPlugin")]
    if unknown:
        die("interfaces this script doesn't know: " + ", ".join(sorted(unknown)))
    decls, objects, counts = [], [], []
    for iface in INTERFACES:
        if iface not in found:
            die("%s: no %s" % (sdk, iface))
        methods = interface_methods(found[iface][0])
        counts.append("%s %d" % (iface, len(methods)))
        decls.append(declaration(iface, methods))
        if iface not in CALLED_ONLY:
            objects.append(object_class(iface, methods))
    # The plugin's main classes add a virtual destructor after the base's methods.
    decls.append("struct IPlugin : IPluginBase {\n    virtual ~IPlugin() = default;\n};")
    decls.append("struct IPolPlugin : IPolPluginBase {\n    virtual ~IPolPlugin() = default;\n};")
    ints = "\n".join("    using %s = int;" % t for t in IMGUI_INT_TYPES)
    forward = "\n".join("struct %s;" % i for i in INTERFACES)
    body = "\n\n".join([
        "// Interface version %s: %s." % (label, ", ".join(counts)),
        "inline constexpr double kInterfaceVersion = %s;" % label,
        "// ImGui's types, as this version's wrapper uses them.\n" + IMGUI_COMMON.strip("\n") + "\n" + ints + "\n" +
        IMGUI_VERSION_TYPES[tag].strip("\n"),
        forward,
        "\n\n".join(decls),
        "// Implementations: Impl supplies the methods it has (see the file's header).",
        "\n\n".join(objects),
        "using export_CreatePlugin_f = IPlugin*(__stdcall*)(const char* args);",
        "using export_DestroyPlugin_f = void(__stdcall*)(void* instance);",
        "using export_GetInterfaceVersion_f = double(__stdcall*)();",
    ])
    return "namespace %s {\n\n%s\n\n} // namespace %s" % (tag, body, tag)


HEADER = """// Generated by tools/gen_ashita_abi.py from the Ashita v4 SDK headers. Do not edit.
//
// The declarations here are adapted from the Ashita SDK (plugins/sdk), Copyright (c) Ashita
// Development Team, licensed under the GNU Lesser General Public License v3.0 (see
// THIRD_PARTY_NOTICES.md). They let VanaCore host Ashita plugins: the interfaces must match the
// SDK's exactly, method for method.
#pragma once

#include <windows.h>
#include <unknwn.h>
#include <Xinput.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>

#pragma warning(push)
#pragma warning(disable : 4100 4201 4324 26812)

namespace vanacore::ashita::sdk {

using std::int8_t, std::int16_t, std::int32_t, std::int64_t, std::uint8_t, std::uint16_t, std::uint32_t,
    std::uint64_t, std::uintptr_t;

// Reports a call to a method VanaCore doesn't implement (logged once per method).
void unimplemented(const char* method);
// 64 KB of zeros: what a method returning a reference returns when it isn't implemented.
void* zeros();

// Direct3D 8 and DirectInput types: plugins get the game's own objects, VanaCore only passes them on.
struct IDirect3D8;
struct IDirect3DDevice8;
struct IDirect3DTexture8;
using D3DCOLOR = DWORD;
using D3DFORMAT = int;
using D3DPRIMITIVETYPE = int;
using D3DRENDERSTATETYPE = int;
struct D3DXIMAGE_INFO {
    UINT Width;
    UINT Height;
    UINT Depth;
    UINT MipLevels;
    D3DFORMAT Format;
    int ResourceType;
    int ImageFileFormat;
};
using LPDIRECTINPUTDEVICE8A = void*;
using LPDIDEVICEOBJECTDATA = void*;

"""


def abi_test(sdk, output):
    """A test plugin, compiled against the SDK itself, that checks VanaCore's objects slot by slot.

    abi_test_call(objects) calls every method of every interface, in order, on objects[i] (the
    interfaces in INTERFACES order, IPluginBase's slots excepted), with zeroed arguments; VanaCore's
    unimplemented() fallbacks then name each method as it's reached. abi_test_plugin(record)
    returns an IPlugin whose every method passes its name to `record`, for the other direction.
    Build it with /RTCs: a call that leaves the stack unbalanced stops it.
    """
    ashita = strip_comments(read(os.path.join(sdk, "Ashita.h")))
    imgui = strip_comments(read(os.path.join(sdk, "imgui.h")))
    found = blocks(ashita)
    found["IGuiManager"] = blocks(imgui)["IGuiManager"]
    out = ['// Generated by tools/gen_ashita_abi.py --test. Built against the SDK headers.',
           '#include "Ashita.h"', "#include <type_traits>", "#pragma warning(disable : 4100 4189 4700)", ""]
    expected = []
    out.append('extern "C" __declspec(dllexport) void abi_test_call(void** objects)')
    out.append("{")
    for index, iface in enumerate(INTERFACES):
        if iface in CALLED_ONLY:
            continue
        for m in interface_methods(found[iface][0]):
            expected.append(signature_text(iface, m))
            locals_, args = [], []
            for p, a in m.params:
                decl = p.replace("[]", "[1]")
                if re.search(r"&\s*%s$" % a, decl):
                    base = decl[:decl.rfind("&")].strip()
                    locals_.append("std::remove_cv_t<%s> %s_storage{}; %s& %s = %s_storage;" % (base, a, base, a, a))
                else:
                    locals_.append("%s{};" % decl)
                args.append(a)
            out.append("    { auto* o = static_cast<%s*>(objects[%d]); %s o->%s(%s); }" % (
                iface, index, " ".join(locals_), m.name, ", ".join(args)))
    out.append("}")
    out.append("")
    out.append("using record_f = void (*)(const char*);")
    out.append("struct Recorder final : IPlugin {")
    out.append("    record_f record;")
    base_methods = interface_methods(found["IPluginBase"][0])
    for m in base_methods:
        params = ", ".join(p for p, _ in m.params)
        ret = "" if m.ret == "void" else " return {};"
        out.append('    %s %s(%s)%s override { record("%s");%s }' % (
            m.ret, m.name, params, " const" if m.const else "", signature_text("IPluginBase", m), ret))
    out.append('    ~Recorder() override { record("~IPlugin()"); }')
    out.append("};")
    out.append('extern "C" __declspec(dllexport) IPlugin* abi_test_plugin(record_f record)')
    out.append("{")
    out.append("    auto* plugin = new Recorder;")
    out.append("    plugin->record = record;")
    out.append("    return plugin;")
    out.append("}")
    with open(output, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    with open(output + ".expected.txt", "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(expected) + "\n")
    with open(output + ".plugin.txt", "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(signature_text("IPluginBase", m) for m in base_methods) + "\n~IPlugin()\n")
    print("gen_ashita_abi: wrote", output, "(%d calls)" % len(expected))


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "--test":
        abi_test(sys.argv[2], sys.argv[3])
        return
    if len(sys.argv) != 4:
        die("usage: gen_ashita_abi.py <sdk 4.16> <sdk 4.30> <output>\n"
            "       gen_ashita_abi.py --test <sdk> <test plugin .cpp>")
    sdk416, sdk430, output = sys.argv[1:]
    # Enums, game structures and resource structures: the newest SDK's (same layouts, better names).
    ashita430 = read(os.path.join(sdk430, "Ashita.h"))
    enums, typedefs, structs = ashita_h_parts(ashita430)
    text = HEADER
    text += "// Ashita.h: enumerations.\n" + enums + "\n\n"
    text += "// Game structures (ffxi/*.h).\n" + ffxi_structs(sdk430) + "\n\n"
    text += "// Ashita.h: resources.\n" + "\n\n".join(structs) + "\n\n"
    text += "// Ashita.h: callbacks.\n" + "\n".join(typedefs) + "\n\n"
    for (tag, label), sdk in zip(VERSIONS, (sdk416, sdk430)):
        text += version_block(tag, label, sdk) + "\n\n"
    text += "} // namespace vanacore::ashita::sdk\n\n#pragma warning(pop)\n"
    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
    with open(output, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("gen_ashita_abi: wrote", output)


if __name__ == "__main__":
    main()
