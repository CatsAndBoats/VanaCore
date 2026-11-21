// Included in lua_imgui.cpp (its anonymous namespace): ImGui's structures as live Lua objects. A field reads and writes
// the structure itself (lua_imgui_views.inc, generated from imgui.h, lists them); a method is called with ':'. Arrays
// and ImVectors index from 1, as Ashita's do (Colors[ImGuiCol_Text + 1]). A name the structure hasn't reads as nil.
// For an addon passing ImGui 1.81's numbers (legacy_numbers), a field can turn them (ConfigFlags, Colors' index), and
// 1.81's fields 1.92 dropped read as 1.81 had them (io.KeysDown, io.KeyMap, font.FontSize).

enum class Kind : std::uint8_t {
    Float, Double, Int, UInt, Short, UShort, S8, U8, S64, U64, Bool, Vec2, Vec4, String, CharArray, Pointer,
    Array, Vector, Object, Embedded, Method, Getter, Virtual,
};

struct View;
struct Field {
    const char* name;
    std::size_t offset = 0;
    Kind kind = Kind::Float;
    bool writable = false;
    Kind element = Kind::Float;       // Array, Vector, Virtual: its elements'
    int count = 0;                    // Array, Virtual: how many
    std::size_t stride = 0;           // Array, Vector: an element's size
    const View* view = nullptr;       // Object, Embedded, and elements that are
    lua_CFunction function = nullptr; // Method
    void (*get)(lua_State*, const void*) = nullptr; // Getter; Virtual: an element (index 0-based on the stack top)
    void (*set)(lua_State*, void*, int) = nullptr;  // Getter: the value at the stack index
    int (*from_181)(int) = nullptr;   // an Int field (or Array element's index) in 1.81's numbers, as 1.92's
    int (*to_181)(int) = nullptr;     // and back
};
struct View {
    const char* name;
    std::vector<Field> fields;
    std::unordered_map<std::string, std::size_t> by_name; // built on first use
    // The last lookups, by the address of the name asked for (find). LuaJIT interns its strings, so the "AddLine" of an
    // addon's `dl:AddLine(...)` has the one address for as long as that string lives, and a draw list's methods are
    // asked for thousands of times a frame; by_name hashed each name as a std::string every time. A hit is checked
    // against the field's name, as a collected string's address can be another string's later.
    struct Recent {
        const char* key;
        std::size_t index;
    };
    Recent recent[64] = {};
};

struct Object {
    const void* tag; // kObjectType (tagged)
    void* base;
    const View* view;
    void (*destroy)(void*); // an object Lua owns (made with .new): its destructor, run when collected
};
struct ArrayRef {
    void* base;           // Vector: the ImVector
    const Field* field;   // what it is (element, count, stride, view, from_181)
};

Field value(const char* name, std::size_t offset, Kind kind, bool writable = false)
{
    Field f{name};
    f.offset = offset, f.kind = kind, f.writable = writable;
    return f;
}
Field array(const char* name, std::size_t offset, Kind element, int count, const View* view, std::size_t stride)
{
    Field f = value(name, offset, Kind::Array, true);
    f.element = element, f.count = count, f.view = view, f.stride = stride;
    return f;
}
Field vector(const char* name, std::size_t offset, Kind element, const View* view, std::size_t stride)
{
    Field f = value(name, offset, Kind::Vector, true);
    f.element = element, f.view = view, f.stride = stride;
    return f;
}
Field object(const char* name, std::size_t offset, const View* view, Kind kind)
{
    Field f = value(name, offset, kind);
    f.view = view;
    return f;
}
Field method(const char* name, lua_CFunction function)
{
    Field f = value(name, 0, Kind::Method);
    f.function = function;
    return f;
}
Field getter(const char* name, void (*get)(lua_State*, const void*), void (*set)(lua_State*, void*, int) = nullptr)
{
    Field f = value(name, 0, Kind::Getter, set != nullptr);
    f.get = get, f.set = set;
    return f;
}
// An array that isn't in the structure: element i (0-based) pushed by `get` (the index on the stack top).
Field virtual_array(const char* name, int count, void (*get)(lua_State*, const void*))
{
    Field f = value(name, 0, Kind::Virtual);
    f.count = count, f.get = get;
    return f;
}

#include "lua_imgui_views.inc"

const Field* find(const View& view, const char* name)
{
    if (name == nullptr)
        return nullptr;
    auto& v = const_cast<View&>(view);
    View::Recent& recent = v.recent[(reinterpret_cast<std::uintptr_t>(name) >> 4) & 63];
    if (recent.key == name && std::strcmp(v.fields[recent.index].name, name) == 0)
        return &v.fields[recent.index];
    if (v.by_name.size() != v.fields.size()) {
        v.by_name.clear();
        for (std::size_t i = 0; i < v.fields.size(); ++i)
            v.by_name.emplace(v.fields[i].name, i);
    }
    const auto found = v.by_name.find(name);
    if (found == v.by_name.end())
        return nullptr;
    recent = {name, found->second};
    return &v.fields[found->second];
}

void push_object(lua_State* L, void* base, const View* view)
{
    if (base == nullptr) {
        lua_pushnil(L);
        return;
    }
    auto* o = static_cast<Object*>(lua_newuserdata(L, sizeof(Object)));
    *o = {kObjectType, base, view, nullptr};
    set_metatable(L, kObjectType);
}

// A new T Lua owns: ImGuiListClipper.new() and the like.
template <class T, class... Args>
T* push_owned(lua_State* L, const View* view, Args&&... args)
{
    constexpr std::size_t header = (sizeof(Object) + alignof(T) - 1) / alignof(T) * alignof(T);
    auto* o = static_cast<Object*>(lua_newuserdata(L, header + sizeof(T)));
    T* t = new (reinterpret_cast<char*>(o) + header) T(std::forward<Args>(args)...);
    *o = {kObjectType, t, view, [](void* p) { static_cast<T*>(p)->~T(); }};
    set_metatable(L, kObjectType);
    return t;
}

void push_array(lua_State* L, void* base, const Field* field)
{
    auto* a = static_cast<ArrayRef*>(lua_newuserdata(L, sizeof(ArrayRef)));
    *a = {base, field};
    set_metatable(L, kArrayType);
}

// The object at `i` if it's one of `view`'s, else nullptr.
void* object_of(lua_State* L, int i, const View& view)
{
    const Object* o = tagged<Object>(L, i, kObjectType);
    return o != nullptr && o->view == &view ? o->base : nullptr;
}
template <class T>
T* self_of(lua_State* L, const View& view)
{
    auto* p = static_cast<T*>(object_of(L, 1, view));
    if (p == nullptr)
        luaL_error(L, "%s method called without its object (use ':')", view.name);
    return p;
}

void push_value(lua_State* L, void* at, Kind kind, const View* view)
{
    switch (kind) {
    case Kind::Float: lua_pushnumber(L, *static_cast<float*>(at)); break;
    case Kind::Double: lua_pushnumber(L, *static_cast<double*>(at)); break;
    case Kind::Int: lua_pushnumber(L, *static_cast<int*>(at)); break;
    case Kind::UInt: lua_pushnumber(L, *static_cast<unsigned*>(at)); break;
    case Kind::Short: lua_pushnumber(L, *static_cast<short*>(at)); break;
    case Kind::UShort: lua_pushnumber(L, *static_cast<unsigned short*>(at)); break;
    case Kind::S8: lua_pushnumber(L, *static_cast<signed char*>(at)); break;
    case Kind::U8: lua_pushnumber(L, *static_cast<unsigned char*>(at)); break;
    case Kind::S64: lua_pushnumber(L, static_cast<lua_Number>(*static_cast<std::int64_t*>(at))); break;
    case Kind::U64: lua_pushnumber(L, static_cast<lua_Number>(*static_cast<std::uint64_t*>(at))); break;
    case Kind::Bool: lua_pushboolean(L, *static_cast<bool*>(at)); break;
    case Kind::Vec2: push_vec_ref(L, static_cast<float*>(at), 2); break;
    case Kind::Vec4: push_vec_ref(L, static_cast<float*>(at), 4); break;
    case Kind::String: push_string(L, *static_cast<const char**>(at)); break;
    case Kind::CharArray: lua_pushstring(L, static_cast<const char*>(at)); break;
    case Kind::Pointer: lua_pushnumber(L, static_cast<lua_Number>(reinterpret_cast<std::uintptr_t>(*static_cast<void**>(at)))); break;
    case Kind::Object: push_object(L, *static_cast<void**>(at), view); break;
    case Kind::Embedded: push_object(L, at, view); break;
    default: lua_pushnil(L); break;
    }
}

// A number, bool or vector written into a field or element; false if it can't be.
bool store_value(lua_State* L, void* at, Kind kind, int i)
{
    switch (kind) {
    case Kind::Float: *static_cast<float*>(at) = opt_float(L, i, 0); return true;
    case Kind::Double: *static_cast<double*>(at) = opt_double(L, i, 0); return true;
    case Kind::Int: *static_cast<int*>(at) = opt_int(L, i, 0); return true;
    case Kind::UInt: *static_cast<unsigned*>(at) = opt_u32(L, i, 0); return true;
    case Kind::Short: *static_cast<short*>(at) = static_cast<short>(opt_int(L, i, 0)); return true;
    case Kind::UShort: *static_cast<unsigned short*>(at) = static_cast<unsigned short>(opt_int(L, i, 0)); return true;
    case Kind::S8: *static_cast<signed char*>(at) = static_cast<signed char>(opt_int(L, i, 0)); return true;
    case Kind::U8: *static_cast<unsigned char*>(at) = static_cast<unsigned char>(opt_int(L, i, 0)); return true;
    case Kind::S64: *static_cast<std::int64_t*>(at) = whole(opt_double(L, i, 0)); return true;
    case Kind::U64: *static_cast<std::uint64_t*>(at) = static_cast<std::uint64_t>(whole(opt_double(L, i, 0))); return true;
    case Kind::Bool: *static_cast<bool*>(at) = lua_toboolean(L, i) != 0; return true;
    case Kind::Vec2: return read_vec(L, i, static_cast<float*>(at), 2);
    case Kind::Vec4: return read_vec(L, i, static_cast<float*>(at), 4);
    default: return false;
    }
}

int object_index(lua_State* L)
{
    const Object* o = &checked<Object>(L, 1, kObjectType);
    const Field* field = lua_type(L, 2) == LUA_TSTRING ? find(*o->view, lua_tostring(L, 2)) : nullptr;
    if (field == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    auto* at = static_cast<char*>(o->base) + field->offset;
    switch (field->kind) {
    case Kind::Array:
    case Kind::Vector: push_array(L, at, field); break;
    case Kind::Virtual: push_array(L, o->base, field); break;
    case Kind::Method: lua_pushcfunction(L, field->function); break;
    case Kind::Getter: field->get(L, o->base); break;
    case Kind::Int:
        lua_pushnumber(L, field->to_181 != nullptr ? out_enum(L, *reinterpret_cast<int*>(at), field->to_181) : *reinterpret_cast<int*>(at));
        break;
    default: push_value(L, at, field->kind, field->view); break;
    }
    return 1;
}

int object_newindex(lua_State* L)
{
    const Object* o = &checked<Object>(L, 1, kObjectType);
    const Field* field = lua_type(L, 2) == LUA_TSTRING ? find(*o->view, lua_tostring(L, 2)) : nullptr;
    if (field == nullptr || !field->writable)
        return luaL_error(L, "%s.%s can't be set", o->view->name, lua_tostring(L, 2) ? lua_tostring(L, 2) : "?");
    auto* at = static_cast<char*>(o->base) + field->offset;
    if (field->kind == Kind::Getter) {
        field->set(L, o->base, 3);
    } else if (field->kind == Kind::Int && field->from_181 != nullptr) {
        *reinterpret_cast<int*>(at) = opt_enum(L, 3, 0, field->from_181);
    } else if (field->kind == Kind::CharArray) {
        // (a fixed buffer: written as much as fits)
    } else {
        store_value(L, at, field->kind, 3);
    }
    return 0;
}

int object_eq(lua_State* L)
{
    const Object* a = tagged<Object>(L, 1, kObjectType);
    const Object* b = tagged<Object>(L, 2, kObjectType);
    lua_pushboolean(L, a != nullptr && b != nullptr && a->base == b->base);
    return 1;
}

int object_tostring(lua_State* L)
{
    const Object* o = &checked<Object>(L, 1, kObjectType);
    lua_pushfstring(L, "%s: %p", o->view->name, o->base);
    return 1;
}

int object_gc(lua_State* L)
{
    Object* o = &checked<Object>(L, 1, kObjectType);
    if (o->destroy != nullptr) {
        o->destroy(o->base);
        o->destroy = nullptr;
    }
    return 0;
}

struct VectorHeader {
    int Size;
    int Capacity;
    char* Data;
};

// An array's element `i` (1-based), or nullptr out of range. 1.81's index turned into 1.92's where the field says.
void* element_at(lua_State* L, const ArrayRef& a, lua_Integer i)
{
    const Field& f = *a.field;
    int index = static_cast<int>(i) - 1;
    if (f.from_181 != nullptr && legacy_numbers(L))
        index = f.from_181(index);
    if (f.kind == Kind::Vector) {
        const auto* v = static_cast<const VectorHeader*>(a.base);
        return index >= 0 && index < v->Size ? v->Data + static_cast<std::size_t>(index) * f.stride : nullptr;
    }
    return index >= 0 && index < f.count ? static_cast<char*>(a.base) + static_cast<std::size_t>(index) * f.stride : nullptr;
}

int array_index(lua_State* L)
{
    const auto* a = static_cast<const ArrayRef*>(luaL_checkudata(L, 1, kArrayType));
    if (!lua_isnumber(L, 2)) {
        lua_pushnil(L);
        return 1;
    }
    if (a->field->kind == Kind::Virtual) {
        const lua_Integer i = static_cast<lua_Integer>(lua_tonumber(L, 2));
        if (i < 1 || i > a->field->count) {
            lua_pushnil(L);
            return 1;
        }
        lua_pushinteger(L, i - 1);
        a->field->get(L, a->base);
        lua_remove(L, -2);
        return 1;
    }
    void* at = element_at(L, *a, static_cast<lua_Integer>(lua_tonumber(L, 2)));
    if (at == nullptr)
        lua_pushnil(L);
    else
        push_value(L, at, a->field->element, a->field->view);
    return 1;
}

int array_newindex(lua_State* L)
{
    const auto* a = static_cast<const ArrayRef*>(luaL_checkudata(L, 1, kArrayType));
    void* at = a->field->kind != Kind::Virtual && lua_isnumber(L, 2) ? element_at(L, *a, static_cast<lua_Integer>(lua_tonumber(L, 2))) : nullptr;
    if (at == nullptr || !store_value(L, at, a->field->element, 3))
        return luaL_error(L, "no element %s to set", lua_tostring(L, 2) ? lua_tostring(L, 2) : "?");
    return 0;
}

int array_len(lua_State* L)
{
    const auto* a = static_cast<const ArrayRef*>(luaL_checkudata(L, 1, kArrayType));
    const Field& f = *a->field;
    lua_pushinteger(L, f.kind == Kind::Vector ? static_cast<const VectorHeader*>(a->base)->Size : f.count);
    return 1;
}

// The pushers and readers lua_imgui.inc's generated functions use.
int push_io(lua_State* L, ImGuiIO* io)
{
    push_object(L, io, &view_ImGuiIO());
    return 1;
}
int push_style(lua_State* L, ImGuiStyle* style)
{
    push_object(L, style, &view_ImGuiStyle());
    return 1;
}
int push_font(lua_State* L, ImFont* font)
{
    push_object(L, font, &view_ImFont());
    return 1;
}
int push_sort_specs(lua_State* L, ImGuiTableSortSpecs* specs)
{
    push_object(L, specs, &view_ImGuiTableSortSpecs());
    return 1;
}
int push_viewport(lua_State* L, ImGuiViewport* viewport)
{
    push_object(L, viewport, &view_ImGuiViewport());
    return 1;
}
int push_draw_list(lua_State* L, ImDrawList* list)
{
    push_object(L, list, &view_ImDrawList());
    return 1;
}
ImFont* opt_font(lua_State* L, int i)
{
    return static_cast<ImFont*>(object_of(L, i, view_ImFont()));
}
ImGuiStyle* opt_style(lua_State* L, int i)
{
    return static_cast<ImGuiStyle*>(object_of(L, i, view_ImGuiStyle()));
}
ImGuiViewport* opt_viewport(lua_State* L, int i)
{
    return static_cast<ImGuiViewport*>(object_of(L, i, view_ImGuiViewport()));
}
