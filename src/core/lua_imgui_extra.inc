// Included in lua_imgui.cpp, after its hand-written IGuiManager methods: the rest of Ashita 4.30's (images, scalars, drag
// and drop, multi-selection, docking, settings in memory), and the 4.16 forms of those 1.92 changed.

// BeginDisabled([disabled]) / EndDisabled(): ImGui's own. An EndDisabled with no BeginDisabled is left out (ImGui would
// assert), so an addon's unbalanced pair can't break every window.
int g_disabled_depth = 0;
int g_disabled_frame = -1;

int BeginDisabled(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    if (g_disabled_frame != ImGui::GetFrameCount()) {
        g_disabled_frame = ImGui::GetFrameCount();
        g_disabled_depth = 0;
    }
    ImGui::BeginDisabled(opt_bool(L, 1, true));
    ++g_disabled_depth;
    return 0;
}

int EndDisabled(lua_State*)
{
    if (!gui::in_frame() || g_disabled_frame != ImGui::GetFrameCount() || g_disabled_depth <= 0)
        return 0;
    --g_disabled_depth;
    ImGui::EndDisabled();
    return 0;
}

// IsWindowHovered([flags]): only the flags ImGui 1.92 takes for a window. RectOnly (1.81's, which 1.81 took for a window
// too, and 1.92's) has AllowWhenOverlapped in it, which 1.92 refuses here ("Invalid flags for IsWindowHovered()!"),
// answering false, so fancychat's chat windows never read as hovered.
int IsWindowHovered(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    const ImGuiHoveredFlags flags =
        opt_enum(L, 1, 0, &legacy::in_ImGuiHoveredFlags) & ImGuiHoveredFlags_AllowedMaskForIsWindowHovered;
    lua_pushboolean(L, ImGui::IsWindowHovered(flags));
    return 1;
}

// Image(tex, size, uv0, uv1) (4.30), or Image(tex, size, uv0, uv1, tint, border) (4.16)
int Image(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    const ImTextureRef tex = opt_texture(L, 1);
    const ImVec2 size = opt_vec2(L, 2, ImVec2(0, 0)), uv0 = opt_vec2(L, 3, ImVec2(0, 0)), uv1 = opt_vec2(L, 4, ImVec2(1, 1));
    if (absent(L, 5) && absent(L, 6))
        ImGui::Image(tex, size, uv0, uv1);
    else
        ImGui::Image(tex, size, uv0, uv1, opt_vec4(L, 5, ImVec4(1, 1, 1, 1)), opt_vec4(L, 6, ImVec4(0, 0, 0, 0)));
    return 0;
}
int ImageWithBg(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    ImGui::ImageWithBg(opt_texture(L, 1), opt_vec2(L, 2, ImVec2(0, 0)), opt_vec2(L, 3, ImVec2(0, 0)), opt_vec2(L, 4, ImVec2(1, 1)),
                       opt_vec4(L, 5, ImVec4(0, 0, 0, 0)), opt_vec4(L, 6, ImVec4(1, 1, 1, 1)));
    return 0;
}
// ImageButton(str_id, tex, size, uv0, uv1, bg, tint) (4.30), or ImageButton(tex, size, uv0, uv1, frame_padding, bg, tint)
// (4.16: the texture is its ID, frame_padding its padding, -1 the style's)
int ImageButton(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    bool pressed = false;
    if (lua_type(L, 1) == LUA_TSTRING) {
        pressed = ImGui::ImageButton(opt_string(L, 1, ""), opt_texture(L, 2), opt_vec2(L, 3, ImVec2(0, 0)), opt_vec2(L, 4, ImVec2(0, 0)),
                                     opt_vec2(L, 5, ImVec2(1, 1)), opt_vec4(L, 6, ImVec4(0, 0, 0, 0)), opt_vec4(L, 7, ImVec4(1, 1, 1, 1)));
    } else {
        const int padding = opt_int(L, 5, -1);
        ImGui::PushID(opt_pointer(L, 1));
        if (padding >= 0)
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(static_cast<float>(padding), static_cast<float>(padding)));
        pressed = ImGui::ImageButton("#image", opt_texture(L, 1), opt_vec2(L, 2, ImVec2(0, 0)), opt_vec2(L, 3, ImVec2(0, 0)),
                                     opt_vec2(L, 4, ImVec2(1, 1)), opt_vec4(L, 6, ImVec4(0, 0, 0, 0)), opt_vec4(L, 7, ImVec4(1, 1, 1, 1)));
        if (padding >= 0)
            ImGui::PopStyleVar();
        ImGui::PopID();
    }
    lua_pushboolean(L, pressed);
    return 1;
}

// GetKeyIndex(ImGuiKey_X) (4.16): the Win32 key code Ashita mapped 1.81's key to; 1.92's keys are their own index
int GetKeyIndex(lua_State* L)
{
    const int key = opt_int(L, 1, 0);
    lua_pushnumber(L, key >= 0 && key < legacy::kLegacyKeyCount ? legacy::legacy_key_vk(key) : key);
    return 1;
}

int GetIO(lua_State* L) { return ImGui::GetCurrentContext() != nullptr ? push_io(L, &ImGui::GetIO()) : 0; }
int GetStyle(lua_State* L) { return ImGui::GetCurrentContext() != nullptr ? push_style(L, &ImGui::GetStyle()) : 0; }
int GetPlatformIO(lua_State* L)
{
    if (ImGui::GetCurrentContext() == nullptr)
        return 0;
    push_object(L, &ImGui::GetPlatformIO(), &view_ImGuiPlatformIO());
    return 1;
}
int GetBackgroundDrawList(lua_State* L) { return gui::in_frame() ? push_draw_list(L, ImGui::GetBackgroundDrawList(opt_viewport(L, 1))) : 0; }
int GetForegroundDrawList(lua_State* L) { return gui::in_frame() ? push_draw_list(L, ImGui::GetForegroundDrawList(opt_viewport(L, 1))) : 0; }
int GetFontBaked(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    push_object(L, ImGui::GetFontBaked(), &view_ImFontBaked());
    return 1;
}
int FindViewportByPlatformHandle(lua_State* L)
{
    return ImGui::GetCurrentContext() != nullptr ? push_viewport(L, ImGui::FindViewportByPlatformHandle(opt_pointer(L, 1))) : 0;
}

// OpenPopup(str_id | id, popup_flags)
int OpenPopup(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    const ImGuiPopupFlags flags = opt_enum(L, 2, 0, &legacy::in_ImGuiPopupFlags);
    if (is_number(L, 1))
        ImGui::OpenPopup(opt_u32(L, 1, 0), flags);
    else
        ImGui::OpenPopup(opt_string(L, 1, ""), flags);
    return 0;
}

// PushStyleVarX/Y(idx, value): one of an ImVec2 variable's components
template <void (*Push)(ImGuiStyleVar, float)>
int push_style_var_axis(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    const ImGuiStyleVar idx = opt_enum(L, 1, 0, &legacy::in_ImGuiStyleVar);
    if (idx >= 0 && idx < ImGuiStyleVar_COUNT)
        Push(idx, opt_float(L, 2, 0));
    return 0;
}
void push_style_var_x(ImGuiStyleVar idx, float v) { ImGui::PushStyleVarX(idx, v); }
void push_style_var_y(ImGuiStyleVar idx, float v) { ImGui::PushStyleVarY(idx, v); }

// ---- Scalars: DragScalar and the like, p_data a table of `components` values of `data_type` (or one number) ------------

struct Scalars {
    ImGuiDataType type;
    int count;
    alignas(8) unsigned char bytes[4 * 8] = {}; // packed as ImGui reads them: each the data type's size
    bool present = false;

    Scalars(lua_State* L, int i, ImGuiDataType data_type, int n) : type(data_type), count(n < 1 ? 1 : n > 4 ? 4 : n)
    {
        if (lua_istable(L, i)) {
            present = true;
            for (int k = 0; k < count; ++k) {
                lua_rawgeti(L, i, k + 1);
                set(k, lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0);
                lua_pop(L, 1);
            }
        } else if (lua_isnumber(L, i)) {
            present = true;
            set(0, lua_tonumber(L, i));
        }
    }
    std::size_t size() const { return ImGui::DataTypeGetInfo(type)->Size; }
    void* at(int k) { return bytes + static_cast<std::size_t>(k) * size(); }
    template <class T>
    void put(int k, T v) { std::memcpy(at(k), &v, sizeof v); }
    template <class T>
    T take(int k) { T v; std::memcpy(&v, at(k), sizeof v); return v; }
    void set(int k, double v)
    {
        switch (type) {
        case ImGuiDataType_S8: put(k, static_cast<ImS8>(whole(v))); break;
        case ImGuiDataType_U8: put(k, static_cast<ImU8>(whole(v))); break;
        case ImGuiDataType_S16: put(k, static_cast<ImS16>(whole(v))); break;
        case ImGuiDataType_U16: put(k, static_cast<ImU16>(whole(v))); break;
        case ImGuiDataType_S32: put(k, static_cast<ImS32>(whole(v))); break;
        case ImGuiDataType_U32: put(k, static_cast<ImU32>(whole(v))); break;
        case ImGuiDataType_S64: put(k, static_cast<ImS64>(whole(v))); break;
        case ImGuiDataType_U64: put(k, static_cast<ImU64>(whole(v))); break;
        case ImGuiDataType_Double: put(k, v); break;
        case ImGuiDataType_Bool: put(k, v != 0); break;
        default: put(k, static_cast<float>(v)); break;
        }
    }
    double get(int k)
    {
        switch (type) {
        case ImGuiDataType_S8: return take<ImS8>(k);
        case ImGuiDataType_U8: return take<ImU8>(k);
        case ImGuiDataType_S16: return take<ImS16>(k);
        case ImGuiDataType_U16: return take<ImU16>(k);
        case ImGuiDataType_S32: return take<ImS32>(k);
        case ImGuiDataType_U32: return take<ImU32>(k);
        case ImGuiDataType_S64: return static_cast<double>(take<ImS64>(k));
        case ImGuiDataType_U64: return static_cast<double>(take<ImU64>(k));
        case ImGuiDataType_Double: return take<double>(k);
        case ImGuiDataType_Bool: return take<bool>(k) ? 1 : 0;
        default: return take<float>(k);
        }
    }
    void* data() { return present ? static_cast<void*>(bytes) : nullptr; }
    void store(lua_State* L, int i)
    {
        if (!lua_istable(L, i))
            return;
        for (int k = 0; k < count; ++k) {
            lua_pushnumber(L, get(k));
            lua_rawseti(L, i, k + 1);
        }
    }
};

ImGuiDataType opt_data_type(lua_State* L, int i)
{
    const int t = opt_enum(L, i, ImGuiDataType_Float, &legacy::in_ImGuiDataType);
    return t >= 0 && t < ImGuiDataType_COUNT ? t : ImGuiDataType_Float;
}

// DragScalar(label, type, p_data, v_speed, p_min, p_max, format, flags), DragScalarN(label, type, p_data, n, ...)
int drag_scalar(lua_State* L, bool n)
{
    if (!gui::in_frame())
        return 0;
    const ImGuiDataType type = opt_data_type(L, 2);
    const int at = n ? 5 : 4;
    Scalars data(L, 3, type, n ? opt_int(L, 4, 1) : 1), lo(L, at + 1, type, 1), hi(L, at + 2, type, 1);
    const bool changed = ImGui::DragScalarN(opt_string(L, 1, ""), type, data.bytes, data.count, opt_float(L, at, 1.0f), lo.data(), hi.data(),
                                            opt_string(L, at + 3, nullptr), opt_enum(L, at + 4, 0, &legacy::in_ImGuiSliderFlags));
    data.store(L, 3);
    lua_pushboolean(L, changed);
    return 1;
}
int DragScalar(lua_State* L) { return drag_scalar(L, false); }
int DragScalarN(lua_State* L) { return drag_scalar(L, true); }

// SliderScalar(label, type, p_data, p_min, p_max, format, flags), SliderScalarN(label, type, p_data, n, ...)
int slider_scalar(lua_State* L, bool n)
{
    if (!gui::in_frame())
        return 0;
    const ImGuiDataType type = opt_data_type(L, 2);
    const int at = n ? 5 : 4;
    Scalars data(L, 3, type, n ? opt_int(L, 4, 1) : 1), lo(L, at, type, 1), hi(L, at + 1, type, 1);
    if (!lo.present || !hi.present)
        return 0;
    const bool changed = ImGui::SliderScalarN(opt_string(L, 1, ""), type, data.bytes, data.count, lo.data(), hi.data(),
                                              opt_string(L, at + 2, nullptr), opt_enum(L, at + 3, 0, &legacy::in_ImGuiSliderFlags));
    data.store(L, 3);
    lua_pushboolean(L, changed);
    return 1;
}
int SliderScalar(lua_State* L) { return slider_scalar(L, false); }
int SliderScalarN(lua_State* L) { return slider_scalar(L, true); }

// VSliderScalar(label, size, type, p_data, p_min, p_max, format, flags)
int VSliderScalar(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    const ImGuiDataType type = opt_data_type(L, 3);
    Scalars data(L, 4, type, 1), lo(L, 5, type, 1), hi(L, 6, type, 1);
    if (!lo.present || !hi.present)
        return 0;
    const bool changed = ImGui::VSliderScalar(opt_string(L, 1, ""), opt_vec2(L, 2, ImVec2(0, 0)), type, data.bytes, lo.data(), hi.data(),
                                              opt_string(L, 7, nullptr), opt_enum(L, 8, 0, &legacy::in_ImGuiSliderFlags));
    data.store(L, 4);
    lua_pushboolean(L, changed);
    return 1;
}

// InputScalar(label, type, p_data, p_step, p_step_fast, format, flags), InputScalarN(label, type, p_data, n, ...)
int input_scalar(lua_State* L, bool n)
{
    if (!gui::in_frame())
        return 0;
    const ImGuiDataType type = opt_data_type(L, 2);
    const int at = n ? 5 : 4;
    Scalars data(L, 3, type, n ? opt_int(L, 4, 1) : 1), step(L, at, type, 1), fast(L, at + 1, type, 1);
    const bool changed = ImGui::InputScalarN(opt_string(L, 1, ""), type, data.bytes, data.count, step.data(), fast.data(),
                                             opt_string(L, at + 2, nullptr), opt_enum(L, at + 3, 0, &legacy::in_ImGuiInputTextFlags));
    data.store(L, 3);
    lua_pushboolean(L, changed);
    return 1;
}
int InputScalar(lua_State* L) { return input_scalar(L, false); }
int InputScalarN(lua_State* L) { return input_scalar(L, true); }

// ---- Drag and drop: a payload is a string's bytes (or a number's, as a double, or `sz` bytes at a pointer) ---------------

int SetDragDropPayload(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    std::size_t length = 0;
    const void* data = nullptr;
    double number = 0;
    if (lua_type(L, 2) == LUA_TSTRING) {
        data = lua_tolstring(L, 2, &length);
        if (lua_isnumber(L, 3) && opt_u32(L, 3, 0) < length)
            length = opt_u32(L, 3, 0);
    } else if (lua_type(L, 2) == LUA_TNUMBER && !lua_isnumber(L, 3)) {
        number = lua_tonumber(L, 2);
        data = &number;
        length = sizeof number;
    } else if (void* p = opt_pointer(L, 2)) {
        data = p;
        length = opt_u32(L, 3, 0);
    }
    lua_pushboolean(L, ImGui::SetDragDropPayload(opt_string(L, 1, ""), data, length, opt_int(L, 4, 0)));
    return 1;
}
int AcceptDragDropPayload(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    const ImGuiPayload* p = ImGui::AcceptDragDropPayload(opt_string(L, 1, ""), opt_enum(L, 2, 0, &legacy::in_ImGuiDragDropFlags));
    push_object(L, const_cast<ImGuiPayload*>(p), &view_ImGuiPayload());
    return 1;
}
int GetDragDropPayload(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    push_object(L, const_cast<ImGuiPayload*>(ImGui::GetDragDropPayload()), &view_ImGuiPayload());
    return 1;
}

// ---- Multi-selection, docking --------------------------------------------------------------------------------------------

int BeginMultiSelect(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    push_object(L, ImGui::BeginMultiSelect(opt_int(L, 1, 0), opt_int(L, 2, -1), opt_int(L, 3, -1)), &view_ImGuiMultiSelectIO());
    return 1;
}
int EndMultiSelect(lua_State* L)
{
    if (!gui::in_frame() || GImGui->CurrentMultiSelect == nullptr)
        return 0;
    push_object(L, ImGui::EndMultiSelect(), &view_ImGuiMultiSelectIO());
    return 1;
}
const ImGuiWindowClass* opt_window_class(lua_State* L, int i)
{
    return static_cast<const ImGuiWindowClass*>(object_of(L, i, view_ImGuiWindowClass()));
}
int DockSpace(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    lua_pushnumber(L, ImGui::DockSpace(opt_u32(L, 1, 0), opt_vec2(L, 2, ImVec2(0, 0)), opt_int(L, 3, 0), opt_window_class(L, 4)));
    return 1;
}
int DockSpaceOverViewport(lua_State* L)
{
    if (!gui::in_frame())
        return 0;
    lua_pushnumber(L, ImGui::DockSpaceOverViewport(opt_u32(L, 1, 0), opt_viewport(L, 2), opt_int(L, 3, 0), opt_window_class(L, 4)));
    return 1;
}
int SetNextWindowClass(lua_State* L)
{
    const ImGuiWindowClass* c = opt_window_class(L, 1);
    if (c != nullptr && gui::in_frame())
        ImGui::SetNextWindowClass(c);
    return 0;
}

// ---- Settings in memory, ImGui's allocator --------------------------------------------------------------------------------

int SaveIniSettingsToMemory(lua_State* L)
{
    if (ImGui::GetCurrentContext() == nullptr)
        return 0;
    std::size_t size = 0;
    const char* text = ImGui::SaveIniSettingsToMemory(&size);
    lua_pushlstring(L, text != nullptr ? text : "", text != nullptr ? size : 0);
    if (lua_istable(L, 1)) {
        lua_pushnumber(L, static_cast<lua_Number>(size));
        lua_rawseti(L, 1, 1);
    }
    return 1;
}
int LoadIniSettingsFromMemory(lua_State* L)
{
    if (ImGui::GetCurrentContext() == nullptr || lua_type(L, 1) != LUA_TSTRING)
        return 0;
    std::size_t length = 0;
    const char* text = lua_tolstring(L, 1, &length);
    ImGui::LoadIniSettingsFromMemory(text, lua_isnumber(L, 2) && opt_u32(L, 2, 0) < length ? opt_u32(L, 2, 0) : length);
    return 0;
}
// (malloc and free, as Ashita's ImGui: a pointer as a number)
int MemAlloc(lua_State* L)
{
    const std::uint32_t size = opt_u32(L, 1, 0);
    lua_pushnumber(L, static_cast<lua_Number>(reinterpret_cast<std::uintptr_t>(size > 0 ? std::malloc(size) : nullptr)));
    return 1;
}
int MemFree(lua_State* L)
{
    std::free(opt_pointer(L, 1));
    return 0;
}

const luaL_Reg kExtra[] = {
    {"BeginDisabled", &BeginDisabled},
    {"EndDisabled", &EndDisabled},
    {"IsWindowHovered", &IsWindowHovered},
    {"Image", &Image},
    {"ImageWithBg", &ImageWithBg},
    {"ImageButton", &ImageButton},
    {"GetKeyIndex", &GetKeyIndex},
    {"GetIO", &GetIO},
    {"GetStyle", &GetStyle},
    {"GetPlatformIO", &GetPlatformIO},
    {"GetBackgroundDrawList", &GetBackgroundDrawList},
    {"GetForegroundDrawList", &GetForegroundDrawList},
    {"GetFontBaked", &GetFontBaked},
    {"FindViewportByPlatformHandle", &FindViewportByPlatformHandle},
    {"OpenPopup", &OpenPopup},
    {"PushStyleVarX", &push_style_var_axis<&push_style_var_x>},
    {"PushStyleVarY", &push_style_var_axis<&push_style_var_y>},
    {"DragScalar", &DragScalar},
    {"DragScalarN", &DragScalarN},
    {"SliderScalar", &SliderScalar},
    {"SliderScalarN", &SliderScalarN},
    {"VSliderScalar", &VSliderScalar},
    {"InputScalar", &InputScalar},
    {"InputScalarN", &InputScalarN},
    {"SetDragDropPayload", &SetDragDropPayload},
    {"AcceptDragDropPayload", &AcceptDragDropPayload},
    {"GetDragDropPayload", &GetDragDropPayload},
    {"BeginMultiSelect", &BeginMultiSelect},
    {"EndMultiSelect", &EndMultiSelect},
    {"DockSpace", &DockSpace},
    {"DockSpaceOverViewport", &DockSpaceOverViewport},
    {"SetNextWindowClass", &SetNextWindowClass},
    {"SaveIniSettingsToMemory", &SaveIniSettingsToMemory},
    {"LoadIniSettingsFromMemory", &LoadIniSettingsFromMemory},
    {"MemAlloc", &MemAlloc},
    {"MemFree", &MemFree},
    {nullptr, nullptr},
};
