#include "lua_imgui.hpp"

#include <windows.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "addon_trace.hpp"
#include "gui.hpp"
#include "imgui_legacy.hpp"
#include "layout.hpp"
#include "log.hpp"
#include "lua_api.hpp"

namespace vanacore::lua_imgui {
namespace {

namespace legacy = vanacore::imgui_legacy;

constexpr int kCdata = 10; // LuaJIT's lua_type for FFI data

constexpr char kVecType[] = "vanacore.imgui.vec";
constexpr char kObjectType[] = "vanacore.imgui.object";
constexpr char kArrayType[] = "vanacore.imgui.array";

// ---- Arguments, as Ashita's binding reads them ---------------------------------------------------

bool absent(lua_State* L, int i)
{
    return lua_type(L, i) <= LUA_TNIL;
}

// The binding's userdata (VecRef, Object below) each begin with a tag: the address of their type's name. The userdata
// at `i` is one when it's as big as T and its tag is the type's, which costs four calls into Lua and no table lookup;
// the metatable test it replaces (luaL_checkudata, lua_getmetatable + luaL_getmetatable) interned the type's name as a
// Lua string and looked it up in the registry every time, twice per `dl:AddLine(...)` (the method's __index, then the
// method), a few thousand times a frame under a draw-list heavy addon (YunaUI: 0.3 ms a frame in the sampler).
template <class T>
T* tagged(lua_State* L, int i, const char* type)
{
    if (lua_type(L, i) != LUA_TUSERDATA || lua_objlen(L, i) < sizeof(T))
        return nullptr;
    auto* t = static_cast<T*>(lua_touserdata(L, i));
    return t->tag == static_cast<const void*>(type) ? t : nullptr;
}
// The same, or the error luaL_checkudata would raise.
template <class T>
T& checked(lua_State* L, int i, const char* type)
{
    T* t = tagged<T>(L, i, type);
    if (t == nullptr)
        luaL_typerror(L, i, type);
    return *t;
}
// The metatable `type` names set on the value at the stack top. make_metatable keeps each in the registry under the
// name's address as well as the name, so this is a rawget by a light userdata: no string interned.
void set_metatable(lua_State* L, const char* type)
{
    lua_pushlightuserdata(L, const_cast<char*>(type));
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_setmetatable(L, -2);
}

const char* opt_string(lua_State* L, int i, const char* fallback)
{
    const int type = lua_type(L, i);
    return type == LUA_TSTRING || type == LUA_TNUMBER ? lua_tostring(L, i) : fallback;
}

bool opt_bool(lua_State* L, int i, bool fallback)
{
    return absent(L, i) ? fallback : lua_toboolean(L, i) != 0;
}

float opt_float(lua_State* L, int i, float fallback)
{
    return lua_isnumber(L, i) ? static_cast<float>(lua_tonumber(L, i)) : fallback;
}

double opt_double(lua_State* L, int i, double fallback)
{
    return lua_isnumber(L, i) ? lua_tonumber(L, i) : fallback;
}

// A number as C casts it to an integer (toward zero); 0 out of range.
std::int64_t whole(double value)
{
    return value > -9.2e18 && value < 9.2e18 ? static_cast<std::int64_t>(value) : 0;
}

int opt_int(lua_State* L, int i, int fallback)
{
    return lua_isnumber(L, i) ? static_cast<int>(whole(lua_tonumber(L, i))) : fallback;
}

// Colours and IDs: 0xFF000000 and -16777216 (what bit.bor makes of it) are the same ImU32.
std::uint32_t opt_u32(lua_State* L, int i, std::uint32_t fallback)
{
    return lua_isnumber(L, i) ? static_cast<std::uint32_t>(whole(lua_tonumber(L, i))) : fallback;
}

// Whose numbers an addon passes: ImGui 1.81's if it loaded Ashita 4.16's libs\imgui.lua (IMGUI_VERSION_NUM 18000), else
// 1.92's (Ashita 4.30's, VanaCore's). Known once the library has set IMGUI_VERSION_NUM.
bool legacy_numbers(lua_State* L)
{
    lua_api::Context& context = lua_api::context(L);
    if (context.imgui_numbers == 0) {
        lua_getglobal(L, "IMGUI_VERSION_NUM");
        if (lua_isnumber(L, -1))
            context.imgui_numbers = lua_tonumber(L, -1) < 19000 ? 181 : 192;
        lua_pop(L, 1);
    }
    return context.imgui_numbers == 181;
}

// An enum or flags argument: 1.81's value turned into 1.92's for an addon passing 1.81's numbers. A missing one is
// `fallback`, which is 1.92's already.
int opt_enum(lua_State* L, int i, int fallback, int (*from_181)(int))
{
    if (!lua_isnumber(L, i))
        return fallback;
    const int value = static_cast<int>(whole(lua_tonumber(L, i)));
    return legacy_numbers(L) ? from_181(value) : value;
}
// An enum or flags result, as the addon numbers it.
int out_enum(lua_State* L, int value, int (*to_181)(int))
{
    return legacy_numbers(L) ? to_181(value) : value;
}

// A key: ImGui 1.92's (ImGuiKey_NamedKey_BEGIN and up), or a Win32 key code, which is what 1.81's IsKeyDown and the like
// took (Ashita's io.KeyMap is Win32's).
ImGuiKey opt_key(lua_State* L, int i, ImGuiKey fallback)
{
    return lua_isnumber(L, i) ? legacy::key_in(static_cast<int>(whole(lua_tonumber(L, i)))) : fallback;
}
// A key chord: a key (as opt_key) with ImGuiMod_ bits.
ImGuiKeyChord opt_chord(lua_State* L, int i, ImGuiKeyChord fallback)
{
    if (!lua_isnumber(L, i))
        return fallback;
    const int chord = static_cast<int>(whole(lua_tonumber(L, i)));
    return (chord & ImGuiMod_Mask_) | legacy::key_in(chord & ~ImGuiMod_Mask_);
}

struct VecRef {
    const void* tag; // kVecType (tagged)
    float* p;
    int n; // 2 or 4
};

// Component `n` (1-based) of table `t`, or its .x/.y/.z/.w; 0 when it has neither.
float component(lua_State* L, int t, int n)
{
    static const char* const kNames[] = {"x", "y", "z", "w"};
    lua_rawgeti(L, t, n);
    if (lua_type(L, -1) == LUA_TNUMBER) { // {x, y}: four calls, the usual way in
        const float value = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        return value;
    }
    if (!lua_isnumber(L, -1)) {
        lua_pop(L, 1);
        lua_getfield(L, t, kNames[n - 1]);
    }
    const float value = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : 0.0f;
    lua_pop(L, 1);
    return value;
}

// An ImVec2 or ImVec4 as `n` floats: a table ({x, y}), or one of the structures' vectors.
bool read_vec(lua_State* L, int i, float* out, int n)
{
    if (lua_istable(L, i)) {
        for (int k = 0; k < n; ++k)
            out[k] = component(L, i, k + 1);
        return true;
    }
    if (const auto* ref = tagged<VecRef>(L, i, kVecType)) {
        for (int k = 0; k < n; ++k)
            out[k] = k < ref->n ? ref->p[k] : 0.0f;
        return true;
    }
    return false;
}

ImVec2 opt_vec2(lua_State* L, int i, ImVec2 fallback)
{
    float v[2];
    return read_vec(L, i, v, 2) ? ImVec2(v[0], v[1]) : fallback;
}

ImVec4 opt_vec4(lua_State* L, int i, ImVec4 fallback)
{
    float v[4];
    return read_vec(L, i, v, 4) ? ImVec4(v[0], v[1], v[2], v[3]) : fallback;
}

// A texture: its IDirect3DTexture8 pointer as a number (tonumber(ffi.cast('uint32_t', texture))), a
// light userdata, or an FFI pointer.
void* opt_pointer(lua_State* L, int i)
{
    switch (lua_type(L, i)) {
    case LUA_TNUMBER:
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(static_cast<std::uint32_t>(whole(lua_tonumber(L, i)))));
    case LUA_TLIGHTUSERDATA:
        return lua_touserdata(L, i);
    case kCdata:
        return *static_cast<void* const*>(lua_topointer(L, i));
    default:
        return nullptr;
    }
}
ImTextureRef opt_texture(lua_State* L, int i)
{
    return ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(opt_pointer(L, i))));
}

// A pointer: a table's [1] ([1]..[N] for an array), written back after the call. Anything else is
// a pointer to zeros that goes nowhere (ImGui would crash on none).
template <class T, int N>
class Array {
public:
    Array(lua_State* L, int i)
    {
        if (!lua_istable(L, i))
            return;
        index_ = i;
        for (int k = 0; k < N; ++k) {
            lua_rawgeti(L, i, k + 1);
            if constexpr (std::is_same_v<T, bool>)
                values_[k] = lua_toboolean(L, -1) != 0;
            else if constexpr (std::is_floating_point_v<T>)
                values_[k] = static_cast<T>(lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0);
            else
                values_[k] = static_cast<T>(lua_isnumber(L, -1) ? whole(lua_tonumber(L, -1)) : 0);
            lua_pop(L, 1);
            before_[k] = values_[k];
        }
    }
    T* get() { return values_; }
    T* data() { return values_; }
    void store(lua_State* L)
    {
        if (index_ == 0)
            return;
        for (int k = 0; k < N; ++k) {
            if (values_[k] == before_[k])
                continue;
            if constexpr (std::is_same_v<T, bool>)
                lua_pushboolean(L, values_[k]);
            else
                lua_pushnumber(L, static_cast<lua_Number>(values_[k]));
            lua_rawseti(L, index_, k + 1);
        }
    }

private:
    int index_ = 0;
    T values_[N] = {};
    T before_[N] = {};
};

// An optional bool* (p_open, p_visible, p_selected): a table, or none (nil, true, anything else).
class BoolRef {
public:
    BoolRef(lua_State* L, int i) : array_(L, i), present_(lua_istable(L, i)) {}
    bool* get() { return present_ ? array_.get() : nullptr; }
    void store(lua_State* L) { array_.store(L); }

private:
    Array<bool, 1> array_;
    bool present_;
};

// A p_open as Ashita 4.16's Addons plugin (CatsEyeXI's Addons.dll) reads it in Begin, BeginPopupModal and BeginTabItem:
// a sol::optional<sol::table>, which in its sol2 takes up no argument for a nil, so the next one is read from the nil
// and the rest one place early (BeginTabItem(label, nil, flags) gets no flags; Ashita 4.3.0.2 fixed it). For an Ashita
// addon on interface 4.16 the nil at `i` is doubled to read the same: ventures sets ImGuiTabItemFlags_SetSelected every
// frame on the tab of the pool it last saved, and saves the pool of the tab shown, so with the flag taken the two tabs
// took turns each frame and its table showed both pools at once.
void ashita416_optional_table(lua_State* L, int i)
{
    const lua_api::Context& context = lua_api::context(L);
    if (context.flavour != lua_api::Flavour::Ashita || context.ashita_interface > 4.2 || lua_gettop(L) <= i ||
        !lua_isnil(L, i))
        return;
    lua_pushnil(L);
    lua_insert(L, i);
}

// An optional const ImVec2*.
class OptVec2 {
public:
    OptVec2(lua_State* L, int i) : present_(read_vec(L, i, &value_.x, 2)) {}
    const ImVec2* get() const { return present_ ? &value_ : nullptr; }

private:
    ImVec2 value_;
    bool present_;
};

// ---- Results ---------------------------------------------------------------------------------------

int push_vec2(lua_State* L, const ImVec2& v)
{
    lua_pushnumber(L, v.x);
    lua_pushnumber(L, v.y);
    return 2;
}

int push_vec4(lua_State* L, const ImVec4& v)
{
    lua_pushnumber(L, v.x);
    lua_pushnumber(L, v.y);
    lua_pushnumber(L, v.z);
    lua_pushnumber(L, v.w);
    return 4;
}

int push_string(lua_State* L, const char* text)
{
    if (text != nullptr)
        lua_pushstring(L, text);
    else
        lua_pushnil(L);
    return 1;
}

void push_vec_ref(lua_State* L, float* p, int n)
{
    auto* ref = static_cast<VecRef*>(lua_newuserdata(L, sizeof(VecRef)));
    *ref = {kVecType, p, n};
    set_metatable(L, kVecType);
}

#include "lua_imgui_objects.inc"

// .x .y .z .w (or [1]..[4]) of one of the structures' vectors
int vec_slot(lua_State* L, const VecRef& v)
{
    if (lua_type(L, 2) == LUA_TSTRING) {
        const char* key = lua_tostring(L, 2);
        const int k = key[0] != '\0' && key[1] == '\0' ? (key[0] == 'x' ? 0 : key[0] == 'y' ? 1 : key[0] == 'z' ? 2 : key[0] == 'w' ? 3 : -1) : -1;
        return k < v.n ? k : -1;
    }
    const int k = lua_isnumber(L, 2) ? static_cast<int>(lua_tonumber(L, 2)) - 1 : -1;
    return k >= 0 && k < v.n ? k : -1;
}

int vec_index(lua_State* L)
{
    const VecRef* v = &checked<VecRef>(L, 1, kVecType);
    const int k = vec_slot(L, *v);
    if (k < 0)
        lua_pushnil(L);
    else
        lua_pushnumber(L, v->p[k]);
    return 1;
}

int vec_newindex(lua_State* L)
{
    const VecRef* v = &checked<VecRef>(L, 1, kVecType);
    const int k = vec_slot(L, *v);
    if (k < 0)
        return luaL_error(L, "a vector has no %s", lua_tostring(L, 2) ? lua_tostring(L, 2) : "?");
    v->p[k] = opt_float(L, 3, 0);
    return 0;
}

int vec_tostring(lua_State* L)
{
    const VecRef* v = &checked<VecRef>(L, 1, kVecType);
    if (v->n == 2)
        lua_pushfstring(L, "ImVec2(%f, %f)", v->p[0], v->p[1]);
    else
        lua_pushfstring(L, "ImVec4(%f, %f, %f, %f)", v->p[0], v->p[1], v->p[2], v->p[3]);
    return 1;
}

#include "lua_imgui_methods.inc"

// ---- IGuiManager, method by method ---------------------------------------------------------------

#include "lua_imgui.inc"

// printf-style text as Ashita's addons give it: the format alone, so %% is % (any other % stays as it
// is, where C would read an argument that isn't there). Addons 2.2 on (interface 4.30) take text as it is.
std::string unformat(lua_State* L, const char* text)
{
    const lua_api::Context& context = lua_api::context(L);
    if (context.flavour == lua_api::Flavour::Ashita && context.ashita_interface > 4.2)
        return text;
    std::string out;
    for (const char* p = text; *p != '\0'; ++p) {
        out += *p;
        if (p[0] == '%' && p[1] == '%')
            ++p;
    }
    return out;
}

int Text(lua_State* L)
{
    if (gui::in_frame())
        ImGui::TextUnformatted(unformat(L, opt_string(L, 1, "")).c_str());
    return 0;
}
int TextColored(lua_State* L)
{
    if (gui::in_frame())
        ImGui::TextColored(opt_vec4(L, 1, ImVec4(1, 1, 1, 1)), "%s", unformat(L, opt_string(L, 2, "")).c_str());
    return 0;
}
int TextDisabled(lua_State* L)
{
    if (gui::in_frame())
        ImGui::TextDisabled("%s", unformat(L, opt_string(L, 1, "")).c_str());
    return 0;
}
int TextWrapped(lua_State* L)
{
    if (gui::in_frame())
        ImGui::TextWrapped("%s", unformat(L, opt_string(L, 1, "")).c_str());
    return 0;
}
int LabelText(lua_State* L)
{
    if (gui::in_frame())
        ImGui::LabelText(opt_string(L, 1, ""), "%s", unformat(L, opt_string(L, 2, "")).c_str());
    return 0;
}
int BulletText(lua_State* L)
{
    if (gui::in_frame())
        ImGui::BulletText("%s", unformat(L, opt_string(L, 1, "")).c_str());
    return 0;
}
int SetTooltip(lua_State* L)
{
    if (gui::in_frame())
        ImGui::SetTooltip("%s", unformat(L, opt_string(L, 1, "")).c_str());
    return 0;
}
int LogText(lua_State* L)
{
    if (gui::in_frame())
        ImGui::LogText("%s", unformat(L, opt_string(L, 1, "")).c_str());
    return 0;
}
// the text alone (a second argument, the text's end in C, is ignored)
int TextUnformatted(lua_State* L)
{
    if (gui::in_frame())
        ImGui::TextUnformatted(opt_string(L, 1, ""));
    return 0;
}

int SetItemTooltip(lua_State* L)
{
    if (gui::in_frame())
        ImGui::SetItemTooltip("%s", unformat(L, opt_string(L, 1, "")).c_str());
    return 0;
}
int DebugLog(lua_State* L)
{
    if (ImGui::GetCurrentContext() != nullptr)
        ImGui::DebugLog("%s", opt_string(L, 1, ""));
    return 0;
}

// CalcTextSize(text, hide_text_after_double_hash, wrap_width) (4.30), or (text, text_end, hide, wrap) (4.16)
int CalcTextSize(lua_State* L)
{
    if (ImGui::GetCurrentContext() == nullptr || ImGui::GetFont() == nullptr) // before the first frame
        return push_vec2(L, ImVec2(0, 0));
    const int hide = legacy_numbers(L) || lua_type(L, 2) == LUA_TSTRING ? 3 : 2;
    return push_vec2(L, ImGui::CalcTextSize(opt_string(L, 1, ""), nullptr, opt_bool(L, hide, false), opt_float(L, hide + 1, -1.0f)));
}

// InputText's callback, a Lua function: called with the callback's data (an ImGuiInputTextCallbackData object: its
// fields read and written live, InsertChars and the rest as methods); its number back is the callback's result.
struct InputCallback {
    lua_State* L;
    int function; // its stack index
};
std::atomic<bool> g_logged_callback{false};

int input_callback(ImGuiInputTextCallbackData* data)
{
    auto* callback = static_cast<InputCallback*>(data->UserData);
    lua_State* L = callback->L;
    lua_pushvalue(L, callback->function);
    push_object(L, data, &view_ImGuiInputTextCallbackData());
    int result = 0;
    if (lua_pcall(L, 1, 1, 0) != 0) {
        if (!g_logged_callback.exchange(true))
            log::write(L"ImGui: an addon's text input callback failed: %hs", lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
    } else if (lua_isnumber(L, -1)) {
        result = static_cast<int>(lua_tonumber(L, -1));
    }
    lua_pop(L, 1);
    return result;
}

// InputText(label, buffer, size, flags, callback), InputTextWithHint(label, hint, ...),
// InputTextMultiline(label, buffer, size, box size, flags, callback). The buffer is a table: [1] is
// the text; `size` is C's (with the NUL), so 2 allows one character.
int input_text(lua_State* L, int label_i, int hint_i, int buffer_i, bool multiline)
{
    if (!gui::in_frame())
        return 0;
    const char* label = opt_string(L, label_i, "");
    const char* hint = hint_i != 0 ? opt_string(L, hint_i, "") : nullptr;
    std::uint32_t size = opt_u32(L, buffer_i + 1, 256);
    size = size < 1 ? 1 : size > (1u << 20) ? (1u << 20) : size;
    int next = buffer_i + 2;
    const ImVec2 box = multiline ? opt_vec2(L, next++, ImVec2(0, 0)) : ImVec2(0, 0);
    const ImGuiInputTextFlags flags = opt_enum(L, next++, 0, &legacy::in_ImGuiInputTextFlags) & ~ImGuiInputTextFlags_CallbackResize;
    InputCallback callback{L, next};
    const ImGuiInputTextCallback call = lua_isfunction(L, next) ? &input_callback : nullptr;

    std::string before;
    const bool table = lua_istable(L, buffer_i);
    if (table) {
        lua_rawgeti(L, buffer_i, 1);
        if (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER) {
            std::size_t n = 0;
            const char* s = lua_tolstring(L, -1, &n);
            before.assign(s, n);
        }
        lua_pop(L, 1);
    }
    std::string buffer(size, '\0');
    std::memcpy(buffer.data(), before.data(), before.size() < size ? before.size() : size - 1);
    bool changed = false;
    if (multiline)
        changed = ImGui::InputTextMultiline(label, buffer.data(), size, box, flags, call, &callback);
    else if (hint != nullptr)
        changed = ImGui::InputTextWithHint(label, hint, buffer.data(), size, flags, call, &callback);
    else
        changed = ImGui::InputText(label, buffer.data(), size, flags, call, &callback);
    if (table && std::strcmp(buffer.c_str(), before.c_str()) != 0) {
        lua_pushstring(L, buffer.c_str());
        lua_rawseti(L, buffer_i, 1);
    }
    lua_pushboolean(L, changed);
    return 1;
}
int InputText(lua_State* L) { return input_text(L, 1, 0, 2, false); }
int InputTextWithHint(lua_State* L) { return input_text(L, 1, 2, 3, false); }
int InputTextMultiline(lua_State* L) { return input_text(L, 1, 0, 2, true); }

// A table of strings as C strings (kept here: numbers become strings of their own).
std::vector<std::string> strings(lua_State* L, int i)
{
    std::vector<std::string> out;
    const int n = static_cast<int>(lua_objlen(L, i));
    for (int k = 1; k <= n; ++k) {
        lua_rawgeti(L, i, k);
        std::size_t length = 0;
        const char* s = lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER ? lua_tolstring(L, -1, &length) : "";
        out.emplace_back(s, length);
        lua_pop(L, 1);
    }
    return out;
}

std::vector<const char*> pointers(const std::vector<std::string>& items)
{
    std::vector<const char*> out;
    for (const std::string& item : items)
        out.push_back(item.c_str());
    return out;
}

// Combo(label, current, items, count, popup_height): items a table of strings, or one string of
// them each ended by \0 (then the 4th argument is popup_height). The index is 0-based, as in C.
int Combo(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    const char* label = opt_string(L, 1, "");
    Array<int, 1> current(L, 2);
    bool changed = false;
    if (lua_istable(L, 3)) {
        const std::vector<std::string> items = strings(L, 3);
        const std::vector<const char*> list = pointers(items);
        int count = opt_int(L, 4, static_cast<int>(list.size()));
        count = count < 0 ? 0 : count > static_cast<int>(list.size()) ? static_cast<int>(list.size()) : count;
        changed = ImGui::Combo(label, current.get(), list.data(), count, opt_int(L, 5, -1));
    } else if (lua_type(L, 3) == LUA_TSTRING) {
        std::size_t length = 0;
        const char* s = lua_tolstring(L, 3, &length);
        std::string items(s, length);
        items.append(2, '\0'); // ends with an empty item whatever it had
        changed = ImGui::Combo(label, current.get(), items.c_str(), opt_int(L, 4, -1));
    }
    current.store(L);
    lua_pushboolean(L, changed);
    return 1;
}

int ListBox(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    const char* label = opt_string(L, 1, "");
    Array<int, 1> current(L, 2);
    bool changed = false;
    if (lua_istable(L, 3)) {
        const std::vector<std::string> items = strings(L, 3);
        const std::vector<const char*> list = pointers(items);
        int count = opt_int(L, 4, static_cast<int>(list.size()));
        count = count < 0 ? 0 : count > static_cast<int>(list.size()) ? static_cast<int>(list.size()) : count;
        changed = ImGui::ListBox(label, current.get(), list.data(), count, opt_int(L, 5, -1));
    }
    current.store(L);
    lua_pushboolean(L, changed);
    return 1;
}

// PlotLines/PlotHistogram(label, values, count, offset, overlay, scale_min, scale_max, size): values
// a table of numbers.
template <void (*Plot)(const char*, const float*, int, int, const char*, float, float, ImVec2, int)>
int plot(lua_State* L)
{
    if (!gui::in_frame() || !lua_istable(L, 2))
        return 0;
    std::vector<float> values;
    const int n = static_cast<int>(lua_objlen(L, 2));
    for (int k = 1; k <= n; ++k) {
        lua_rawgeti(L, 2, k);
        values.push_back(lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : 0.0f);
        lua_pop(L, 1);
    }
    int count = opt_int(L, 3, n);
    count = count < 0 ? 0 : count > n ? n : count;
    Plot(opt_string(L, 1, ""), values.data(), count, opt_int(L, 4, 0), opt_string(L, 5, nullptr), opt_float(L, 6, FLT_MAX),
         opt_float(L, 7, FLT_MAX), opt_vec2(L, 8, ImVec2(0, 0)), static_cast<int>(sizeof(float)));
    return 0;
}
void plot_lines(const char* label, const float* values, int count, int offset, const char* overlay, float min, float max,
                ImVec2 size, int stride)
{
    ImGui::PlotLines(label, values, count, offset, overlay, min, max, size, stride);
}
void plot_histogram(const char* label, const float* values, int count, int offset, const char* overlay, float min, float max,
                    ImVec2 size, int stride)
{
    ImGui::PlotHistogram(label, values, count, offset, overlay, min, max, size, stride);
}

int ColorPicker4(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    Array<float, 4> colour(L, 2);
    float reference[4];
    const bool has_reference = read_vec(L, 4, reference, 4);
    const bool changed = ImGui::ColorPicker4(opt_string(L, 1, ""), colour.data(), opt_enum(L, 3, 0, &legacy::in_ImGuiColorEditFlags),
                                             has_reference ? reference : nullptr);
    colour.store(L);
    lua_pushboolean(L, changed);
    return 1;
}

int ColorConvertRGBtoHSV(lua_State* L)
{
    float h = 0, s = 0, v = 0;
    ImGui::ColorConvertRGBtoHSV(opt_float(L, 1, 0), opt_float(L, 2, 0), opt_float(L, 3, 0), h, s, v);
    lua_pushnumber(L, h);
    lua_pushnumber(L, s);
    lua_pushnumber(L, v);
    return 3;
}

int ColorConvertHSVtoRGB(lua_State* L)
{
    float r = 0, g = 0, b = 0;
    ImGui::ColorConvertHSVtoRGB(opt_float(L, 1, 0), opt_float(L, 2, 0), opt_float(L, 3, 0), r, g, b);
    lua_pushnumber(L, r);
    lua_pushnumber(L, g);
    lua_pushnumber(L, b);
    return 3;
}

// CalcListClipping(count, height) (4.16; 1.92 dropped it): which of `count` rows `height` apart, from the cursor down,
// the window shows
int CalcListClipping(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    const int count = opt_int(L, 1, 0);
    const float height = opt_float(L, 2, 0);
    int first = 0, last = count;
    ImGuiWindow* window = GImGui->CurrentWindow;
    if (window != nullptr && height > 0.0f && !GImGui->LogEnabled) {
        if (window->SkipItems) {
            first = last = 0;
        } else {
            first = static_cast<int>((window->ClipRect.Min.y - window->DC.CursorPos.y) / height);
            last = static_cast<int>((window->ClipRect.Max.y - window->DC.CursorPos.y) / height) + 1;
            first = first < 0 ? 0 : first > count ? count : first;
            last = last < first ? first : last > count ? count : last;
        }
    }
    lua_pushnumber(L, first);
    lua_pushnumber(L, last);
    return 2;
}

// (a Lua callback isn't called: no addon gives one)
int SetNextWindowSizeConstraints(lua_State* L)
{
    ImGui::SetNextWindowSizeConstraints(opt_vec2(L, 1, ImVec2(0, 0)), opt_vec2(L, 2, ImVec2(FLT_MAX, FLT_MAX)));
    return 0;
}

// PushFont(font, size): nil keeps the current font, a size of 0 the current size (1.92's). With no size (Ashita 4.16's
// PushFont(font)), the size it was added at, as before 1.92.
int PushFont(lua_State* L)
{
    if (gui::in_frame()) {
        ImFont* font = opt_font(L, 1);
        if (font != nullptr && !font->IsLoaded())
            font = nullptr;
        const float size = lua_isnumber(L, 2) ? opt_float(L, 2, 0.0f) : font != nullptr ? font->LegacySize : 0.0f;
        ImGui::PushFont(font, size);
    }
    return 0;
}

// AddFontFromFileTTF(path, size): a font for PushFont, in the atlas every window shares (made again
// before the next frame, as fonts added mid-frame are in Ashita). nil if the file can't be read.
int AddFontFromFileTTF(lua_State* L)
{
    const char* path = opt_string(L, 1, "");
    // Lua's paths are ANSI; ImGui opens UTF-8 ones
    wchar_t wide[MAX_PATH * 2] = {};
    char file[MAX_PATH * 4] = {};
    if (MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH * 2) == 0 ||
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, file, sizeof file, nullptr, nullptr) == 0 ||
        GetFileAttributesW(wide) == INVALID_FILE_ATTRIBUTES) {
        lua_pushnil(L);
        return 1;
    }
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    const bool locked = atlas->Locked;
    atlas->Locked = false;
    ImFont* font = atlas->AddFontFromFileTTF(file, opt_float(L, 2, 13.0f), opt_font_config(L, 3));
    atlas->Locked = locked;
    return push_font(L, font);
}

// AshitaCore:GetGuiManager():GetVisible() / :SetVisible(bool): called with ':' (the manager first)
int GetVisible(lua_State* L)
{
    lua_pushboolean(L, gui::visible());
    return 1;
}
int SetVisible(lua_State* L)
{
    gui::set_visible(lua_toboolean(L, lua_gettop(L)) != 0);
    return 0;
}

// ---- Overloaded methods: the one the arguments' Lua types pick, as sol2 does ------------------------

bool is_number(lua_State* L, int i)
{
    return lua_type(L, i) == LUA_TNUMBER;
}

int BeginChild(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    // child flags (4.30), or a border (4.16: true is ImGuiChildFlags_Borders)
    const ImVec2 size = opt_vec2(L, 2, ImVec2(0, 0));
    const ImGuiChildFlags child = lua_type(L, 3) == LUA_TBOOLEAN ? (lua_toboolean(L, 3) ? ImGuiChildFlags_Borders : 0)
                                  : legacy_numbers(L)           ? (opt_int(L, 3, 0) != 0 ? ImGuiChildFlags_Borders : 0)
                                                                : opt_int(L, 3, 0);
    const ImGuiWindowFlags flags = opt_enum(L, 4, 0, &legacy::in_ImGuiWindowFlags);
    const bool open = is_number(L, 1) ? ImGui::BeginChild(opt_u32(L, 1, 0), size, child, flags)
                                      : ImGui::BeginChild(opt_string(L, 1, ""), size, child, flags);
    lua_pushboolean(L, open);
    return 1;
}

int CheckboxFlags(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    Array<int, 1> flags(L, 2);
    const bool changed = ImGui::CheckboxFlags(opt_string(L, 1, ""), flags.get(), opt_int(L, 3, 0));
    flags.store(L);
    lua_pushboolean(L, changed);
    return 1;
}

int CollapsingHeader(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    bool open = false;
    if (lua_istable(L, 2)) {
        BoolRef visible(L, 2);
        open = ImGui::CollapsingHeader(opt_string(L, 1, ""), visible.get(), opt_enum(L, 3, 0, &legacy::in_ImGuiTreeNodeFlags));
        visible.store(L);
    } else {
        open = ImGui::CollapsingHeader(opt_string(L, 1, ""), opt_enum(L, 2, 0, &legacy::in_ImGuiTreeNodeFlags));
    }
    lua_pushboolean(L, open);
    return 1;
}

// GetColorU32({r, g, b, a}), GetColorU32(ImGuiCol_X, alpha), or GetColorU32(u32): a lone number
// below ImGuiCol_COUNT is a style colour
int GetColorU32(lua_State* L)
{
    ImU32 colour = 0;
    const double count = legacy_numbers(L) ? 53.0 : static_cast<double>(ImGuiCol_COUNT); // (1.81 had 53)
    if (ImGui::GetCurrentContext() == nullptr)
        colour = 0;
    else if (!is_number(L, 1))
        colour = ImGui::GetColorU32(opt_vec4(L, 1, ImVec4(0, 0, 0, 0)));
    else if (is_number(L, 2) || (lua_tonumber(L, 1) >= 0 && lua_tonumber(L, 1) < count))
        colour = ImGui::GetColorU32(opt_enum(L, 1, 0, &legacy::in_ImGuiCol), opt_float(L, 2, 1.0f));
    else
        colour = ImGui::GetColorU32(opt_u32(L, 1, 0));
    lua_pushnumber(L, colour);
    return 1;
}

int GetID(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    const ImGuiID id = is_number(L, 1)               ? ImGui::GetID(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(opt_u32(L, 1, 0))))
                       : lua_type(L, 2) == LUA_TSTRING ? ImGui::GetID(opt_string(L, 1, ""), opt_string(L, 1, "") + std::strlen(opt_string(L, 1, "")))
                                                       : ImGui::GetID(opt_string(L, 1, ""));
    lua_pushnumber(L, id);
    return 1;
}

int IsRectVisible(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    const bool visible = absent(L, 2) ? ImGui::IsRectVisible(opt_vec2(L, 1, ImVec2(0, 0)))
                                      : ImGui::IsRectVisible(opt_vec2(L, 1, ImVec2(0, 0)), opt_vec2(L, 2, ImVec2(0, 0)));
    lua_pushboolean(L, visible);
    return 1;
}

int ListBoxHeader(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    // (4.16's; BeginListBox now) the height from a count of items, or a size
    ImVec2 size = opt_vec2(L, 2, ImVec2(0, 0));
    if (is_number(L, 2)) {
        const int count = opt_int(L, 2, 0), rows = opt_int(L, 3, -1);
        const int shown = rows < 0 ? (count < 7 ? count : 7) : rows;
        size = ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * (static_cast<float>(shown) + 0.25f) + ImGui::GetStyle().FramePadding.y * 2.0f);
    }
    const bool open = ImGui::BeginListBox(opt_string(L, 1, ""), size);
    lua_pushboolean(L, open);
    return 1;
}

int MenuItem(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    bool chosen = false;
    if (lua_istable(L, 3)) {
        BoolRef selected(L, 3);
        chosen = ImGui::MenuItem(opt_string(L, 1, ""), opt_string(L, 2, nullptr), selected.get(), opt_bool(L, 4, true));
        selected.store(L);
    } else {
        chosen = ImGui::MenuItem(opt_string(L, 1, ""), opt_string(L, 2, nullptr), opt_bool(L, 3, false), opt_bool(L, 4, true));
    }
    lua_pushboolean(L, chosen);
    return 1;
}

int PushID(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    if (is_number(L, 1))
        ImGui::PushID(opt_int(L, 1, 0));
    else if (lua_type(L, 1) == LUA_TLIGHTUSERDATA)
        ImGui::PushID(lua_touserdata(L, 1));
    else
        ImGui::PushID(opt_string(L, 1, ""));
    return 0;
}

int PushStyleColor(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    const ImGuiCol idx = opt_enum(L, 1, 0, &legacy::in_ImGuiCol);
    if (idx < 0 || idx >= ImGuiCol_COUNT)
        return 0;
    if (is_number(L, 2))
        ImGui::PushStyleColor(idx, opt_u32(L, 2, 0));
    else
        ImGui::PushStyleColor(idx, opt_vec4(L, 2, ImVec4(0, 0, 0, 0)));
    return 0;
}

int PushStyleVar(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    const ImGuiStyleVar idx = opt_enum(L, 1, 0, &legacy::in_ImGuiStyleVar);
    if (idx < 0 || idx >= ImGuiStyleVar_COUNT)
        return 0;
    if (is_number(L, 2))
        ImGui::PushStyleVar(idx, opt_float(L, 2, 0));
    else
        ImGui::PushStyleVar(idx, opt_vec2(L, 2, ImVec2(0, 0)));
    return 0;
}

int RadioButton(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    bool pressed = false;
    if (lua_istable(L, 2)) {
        Array<int, 1> value(L, 2);
        pressed = ImGui::RadioButton(opt_string(L, 1, ""), value.get(), opt_int(L, 3, 0));
        value.store(L);
    } else {
        pressed = ImGui::RadioButton(opt_string(L, 1, ""), opt_bool(L, 2, false));
    }
    lua_pushboolean(L, pressed);
    return 1;
}

int Selectable(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    const ImGuiSelectableFlags flags = opt_enum(L, 3, 0, &legacy::in_ImGuiSelectableFlags);
    const ImVec2 size = opt_vec2(L, 4, ImVec2(0, 0));
    bool pressed = false;
    if (lua_istable(L, 2)) {
        BoolRef selected(L, 2);
        pressed = ImGui::Selectable(opt_string(L, 1, ""), selected.get(), flags, size);
        selected.store(L);
    } else {
        pressed = ImGui::Selectable(opt_string(L, 1, ""), opt_bool(L, 2, false), flags, size);
    }
    lua_pushboolean(L, pressed);
    return 1;
}

// SetWindowX(value, cond) for the current window, or SetWindowX(name, value, cond) for a named one
bool named(lua_State* L)
{
    return lua_type(L, 1) == LUA_TSTRING;
}

int SetWindowCollapsed(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    if (named(L))
        ImGui::SetWindowCollapsed(opt_string(L, 1, ""), opt_bool(L, 2, false), opt_int(L, 3, 0));
    else
        ImGui::SetWindowCollapsed(opt_bool(L, 1, false), opt_int(L, 2, 0));
    return 0;
}

int SetWindowFocus(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    if (named(L))
        ImGui::SetWindowFocus(opt_string(L, 1, ""));
    else
        ImGui::SetWindowFocus();
    return 0;
}

// Where an addon places its windows goes through layout.hpp (they can be moved and scaled from the Layout tab)
int SetWindowPos(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    if (named(L)) {
        const char* name = opt_string(L, 1, "");
        ImVec2 pos = opt_vec2(L, 2, ImVec2(0, 0));
        const int cond = opt_int(L, 3, 0);
        layout::window_pos(name, pos.x, pos.y, cond);
        ImGui::SetWindowPos(name, pos, cond);
    } else {
        ImVec2 pos = opt_vec2(L, 1, ImVec2(0, 0));
        const int cond = opt_int(L, 2, 0);
        layout::window_pos(nullptr, pos.x, pos.y, cond);
        ImGui::SetWindowPos(pos, cond);
    }
    return 0;
}

// The addon a Lua state is ("ashita/partyfinder"): who its windows are, for layout.hpp
std::string owner_of(lua_State* L)
{
    const lua_api::Context& context = lua_api::context(L);
    const char* kind = context.flavour == lua_api::Flavour::Ashita     ? "ashita/"
                       : context.flavour == lua_api::Flavour::Windower ? "windower/"
                                                                        : "vanacore/";
    return kind + context.name;
}

// The top-level windows of traced addons (addon_trace.hpp) between their Begin and End
struct TracedWindow {
    std::string addon;
    std::string name;
};

std::vector<TracedWindow>& trace_windows()
{
    static std::vector<TracedWindow> windows;
    return windows;
}

// bool Begin(const char* name, bool* p_open = nullptr, ImGuiWindowFlags flags = 0)
int Begin(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    const char* name = opt_string(L, 1, "");
    ashita416_optional_table(L, 2);
    BoolRef open(L, 2);
    const ImGuiWindowFlags flags = static_cast<ImGuiWindowFlags>(opt_enum(L, 3, 0, &legacy::in_ImGuiWindowFlags));
    const ImGuiWindowFlags set = layout::before_begin(name, owner_of(L)); // (locked, no pointer events)
    const bool traced = addon_trace::any() && addon_trace::on(lua_api::context(L).name);
    const bool open_before = open.get() != nullptr && *open.get();
    const bool shown = ImGui::Begin(name, open.get(), flags | set);
    layout::after_begin();
    if (traced) {
        addon_trace::Window state;
        state.has_open = open.get() != nullptr;
        state.open_before = open_before;
        state.open_after = state.has_open && *open.get();
        state.shown = shown;
        state.collapsed = ImGui::IsWindowCollapsed();
        state.appearing = ImGui::IsWindowAppearing();
        const ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        state.x = pos.x;
        state.y = pos.y;
        state.w = size.x;
        state.h = size.y;
        state.screen_w = ImGui::GetIO().DisplaySize.x;
        state.screen_h = ImGui::GetIO().DisplaySize.y;
        state.flags = flags;
        addon_trace::window_begin(lua_api::context(L).name, name, state);
        std::vector<TracedWindow>& open_windows = trace_windows();
        if (open_windows.size() >= 16)
            open_windows.clear(); // an addon that errored between its Begin and End leaves one behind
        open_windows.push_back({lua_api::context(L).name, name});
    }
    open.store(L);
    lua_pushboolean(L, shown);
    return 1;
}

// void End(void)
int End(lua_State*)
{
    if (!gui::in_frame())
        return 0;
    layout::before_end();
    // A traced addon's window as it ends: how much it drew
    if (!trace_windows().empty()) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        const TracedWindow& top = trace_windows().back();
        if (window != nullptr && top.name == window->Name) {
            addon_trace::window_end(top.addon, top.name.c_str(), window->DrawList->VtxBuffer.Size, window->DrawList->IdxBuffer.Size,
                                    window->ContentSize.x, window->ContentSize.y);
            trace_windows().pop_back();
        }
    }
    ImGui::End();
    return 0;
}

int SetWindowSize(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    if (named(L))
        ImGui::SetWindowSize(opt_string(L, 1, ""), opt_vec2(L, 2, ImVec2(0, 0)), opt_int(L, 3, 0));
    else
        ImGui::SetWindowSize(opt_vec2(L, 1, ImVec2(0, 0)), opt_int(L, 2, 0));
    return 0;
}

const void* pointer_id(lua_State* L, int i)
{
    return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(opt_u32(L, i, 0)));
}

// TreeNode(label), or TreeNode(id, text) with the text printf-style
int TreeNode(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    bool open = false;
    if (absent(L, 2))
        open = ImGui::TreeNode(opt_string(L, 1, ""));
    else if (is_number(L, 1))
        open = ImGui::TreeNode(pointer_id(L, 1), "%s", unformat(L, opt_string(L, 2, "")).c_str());
    else
        open = ImGui::TreeNode(opt_string(L, 1, ""), "%s", unformat(L, opt_string(L, 2, "")).c_str());
    lua_pushboolean(L, open);
    return 1;
}

int TreeNodeEx(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    const ImGuiTreeNodeFlags flags = opt_enum(L, 2, 0, &legacy::in_ImGuiTreeNodeFlags);
    bool open = false;
    if (absent(L, 3))
        open = ImGui::TreeNodeEx(opt_string(L, 1, ""), flags);
    else if (is_number(L, 1))
        open = ImGui::TreeNodeEx(pointer_id(L, 1), flags, "%s", unformat(L, opt_string(L, 3, "")).c_str());
    else
        open = ImGui::TreeNodeEx(opt_string(L, 1, ""), flags, "%s", unformat(L, opt_string(L, 3, "")).c_str());
    lua_pushboolean(L, open);
    return 1;
}

int TreePush(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    if (lua_type(L, 1) == LUA_TSTRING)
        ImGui::TreePush(opt_string(L, 1, ""));
    else
        ImGui::TreePush(is_number(L, 1) ? pointer_id(L, 1) : nullptr);
    return 0;
}

// Value(prefix, bool | int | float, float_format)
int Value(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    const char* prefix = opt_string(L, 1, "");
    if (lua_type(L, 2) == LUA_TBOOLEAN) {
        ImGui::Value(prefix, lua_toboolean(L, 2) != 0);
    } else {
        const double v = opt_double(L, 2, 0);
        if (!absent(L, 3) || v != std::floor(v))
            ImGui::Value(prefix, static_cast<float>(v), opt_string(L, 3, nullptr));
        else
            ImGui::Value(prefix, static_cast<int>(whole(v)));
    }
    return 0;
}

#include "lua_imgui_extra.inc"

const luaL_Reg kHandWritten[] = {
    {"Text", &Text},
    {"TextColored", &TextColored},
    {"TextDisabled", &TextDisabled},
    {"TextWrapped", &TextWrapped},
    {"LabelText", &LabelText},
    {"BulletText", &BulletText},
    {"SetTooltip", &SetTooltip},
    {"LogText", &LogText},
    {"TextUnformatted", &TextUnformatted},
    {"SetItemTooltip", &SetItemTooltip},
    {"DebugLog", &DebugLog},
    {"CalcTextSize", &CalcTextSize},
    {"InputText", &InputText},
    {"InputTextWithHint", &InputTextWithHint},
    {"InputTextMultiline", &InputTextMultiline},
    {"Combo", &Combo},
    {"ListBox", &ListBox},
    {"PlotLines", &plot<&plot_lines>},
    {"PlotHistogram", &plot<&plot_histogram>},
    {"ColorPicker4", &ColorPicker4},
    {"ColorConvertRGBtoHSV", &ColorConvertRGBtoHSV},
    {"ColorConvertHSVtoRGB", &ColorConvertHSVtoRGB},
    {"CalcListClipping", &CalcListClipping},
    {"SetNextWindowSizeConstraints", &SetNextWindowSizeConstraints},
    {"PushFont", &PushFont},
    {"AddFontFromFileTTF", &AddFontFromFileTTF},
    {"GetVisible", &GetVisible},
    {"SetVisible", &SetVisible},
    {"BeginChild", &BeginChild},
    {"CheckboxFlags", &CheckboxFlags},
    {"CollapsingHeader", &CollapsingHeader},
    {"GetColorU32", &GetColorU32},
    {"GetID", &GetID},
    {"IsRectVisible", &IsRectVisible},
    {"ListBoxHeader", &ListBoxHeader},
    {"MenuItem", &MenuItem},
    {"PushID", &PushID},
    {"PushStyleColor", &PushStyleColor},
    {"PushStyleVar", &PushStyleVar},
    {"RadioButton", &RadioButton},
    {"Selectable", &Selectable},
    {"SetWindowCollapsed", &SetWindowCollapsed},
    {"SetWindowFocus", &SetWindowFocus},
    {"SetWindowPos", &SetWindowPos},
    {"SetWindowSize", &SetWindowSize},
    {"Begin", &Begin},
    {"End", &End},
    {"TreeNode", &TreeNode},
    {"TreeNodeEx", &TreeNodeEx},
    {"TreePush", &TreePush},
    {"Value", &Value},
    {nullptr, nullptr},
};

void set_functions(lua_State* L, const luaL_Reg* functions)
{
    for (const luaL_Reg* f = functions; f->name != nullptr; ++f) {
        lua_pushcfunction(L, f->func);
        lua_setfield(L, -2, f->name);
    }
}

void make_metatable(lua_State* L, const char* type, const luaL_Reg* metamethods)
{
    luaL_newmetatable(L, type);
    set_functions(L, metamethods);
    lua_pushlightuserdata(L, const_cast<char*>(type)); // registry[&type] = the metatable too, for set_metatable
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);
}

} // namespace

void push(lua_State* L)
{
    static const luaL_Reg kVec[] = {{"__index", &vec_index}, {"__newindex", &vec_newindex}, {"__tostring", &vec_tostring}, {nullptr, nullptr}};
    static const luaL_Reg kObject[] = {{"__index", &object_index}, {"__newindex", &object_newindex}, {"__eq", &object_eq},
                                       {"__tostring", &object_tostring}, {"__gc", &object_gc}, {nullptr, nullptr}};
    static const luaL_Reg kArray[] = {{"__index", &array_index}, {"__newindex", &array_newindex}, {"__len", &array_len}, {nullptr, nullptr}};
    build_views();
    make_metatable(L, kVecType, kVec);
    make_metatable(L, kObjectType, kObject);
    make_metatable(L, kArrayType, kArray);

    lua_newtable(L);
    set_functions(L, kGenerated);
    set_functions(L, kHandWritten);
    set_functions(L, kExtra);
    push_types(L);
    lua_setfield(L, -2, "types");
}

} // namespace vanacore::lua_imgui
