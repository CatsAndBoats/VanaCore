// IGuiManager: Dear ImGui for Ashita plugins. VanaCore builds in ImGui 1.92.3 WIP docking at a28cb615 (gui.hpp), the
// version interface 4.30 wraps, so a 4.30 method is ImGui's own (gui_v430.inc). Interface 4.16 wraps 1.81 WIP
// (6487860a): its methods are ImGui 1.92's with 1.81's numbers turned into 1.92's going in and back coming out
// (imgui_legacy.hpp), and the structures 1.92 laid out otherwise (ImGuiIO, ImGuiStyle, ImFont and its atlas) are
// 1.81-shaped copies (imgui_v181.hpp) kept in step with VanaCore's (gui_v416.inc and the methods below). Both generated
// by tools/gen_ashita_gui.py.

#include <imgui.h>
#include <imgui_internal.h>

#include <atomic>
#include <bit>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "../../gui.hpp"
#include "../../imgui_legacy.hpp"
#include "../../layout.hpp"
#include "../../log.hpp"
#include "backend.hpp"
#include "imgui_v181.hpp"

namespace vanacore::ashita {

namespace legacy = vanacore::imgui_legacy;

namespace {

std::atomic<bool> g_logged_destroy{false};
std::atomic<bool> g_logged_allocators{false};
std::atomic<bool> g_logged_frame{false};
std::atomic<bool> g_logged_context{false};

std::string caller_name()
{
    return calling().empty() ? std::string("a plugin") : std::string(calling());
}

// A font added to the atlas every ImGui window shares, between frames or not, as Ashita does (1.92 makes its glyphs
// as they're drawn: gui.cpp).
template <class Add>
::ImFont* add_font(Add add)
{
    ::ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    const bool locked = atlas->Locked;
    atlas->Locked = false;
    ::ImFont* font = add(*atlas);
    atlas->Locked = locked;
    return font;
}

// What both interfaces do alike.
struct Common {
    static bool GetVisible(void*) { return gui::visible(); }
    static void SetVisible(void*, bool visible) { gui::set_visible(visible); }

    // A plugin's windows, as an addon's: moved and scaled from the Layout tab (layout.hpp), locked or with no pointer events
    static bool begin(const char* name, bool* open, ImGuiWindowFlags flags)
    {
        const std::string_view plugin = calling();
        flags |= layout::before_begin(name, plugin.empty() ? std::string("plugin") : "plugin/" + std::string(plugin));
        const bool shown = ImGui::Begin(name, open, flags);
        layout::after_begin();
        return shown;
    }
    static void End(void*)
    {
        layout::before_end();
        ImGui::End();
    }
    static void set_window_pos(const char* name, ::ImVec2 at, ImGuiCond cond)
    {
        layout::window_pos(name, at.x, at.y, cond);
        if (name != nullptr)
            ImGui::SetWindowPos(name, at, cond);
        else
            ImGui::SetWindowPos(at, cond);
    }

    // VanaCore runs the frame and owns the context everything draws with: a plugin can't start, end or destroy them
    static void frame(const char* what)
    {
        if (!g_logged_frame.exchange(true))
            log::write(L"Ashita plugins: %hs called ImGui's %hs, which is VanaCore's to call; it did nothing", caller_name().c_str(), what);
    }
    static void NewFrame(void*) { frame("NewFrame"); }
    static void EndFrame(void*) { frame("EndFrame"); }
    static void Render(void*) { frame("Render"); }
    static void destroy_context(::ImGuiContext* native)
    {
        if (native == nullptr || native == gui::context()) {
            if (!g_logged_destroy.exchange(true))
                log::write(L"Ashita plugins: %hs asked to destroy the ImGui context everything draws with; it stays", caller_name().c_str());
            return;
        }
        ImGui::DestroyContext(native);
    }
    // ImGui's allocators are the whole process's: another's would free what VanaCore's allocated
    static void set_allocators()
    {
        if (!g_logged_allocators.exchange(true))
            log::write(L"Ashita plugins: %hs set ImGui's memory allocators; VanaCore keeps its own", caller_name().c_str());
    }
    // Ashita's ImGui allocates with malloc and free (its SDK's note), so a plugin's ImVector can free what it gave
    static void* MemAlloc(void*, size_t size) { return std::malloc(size); }
    static void MemFree(void*, void* p) { std::free(p); }
};

} // namespace

// ---- Interface 4.30: ImGui 1.92's own ------------------------------------------------------------------------------

namespace sdk::v430 {
namespace {

// The SDK's ImGui types as ImGui's own: the same layout (the SDK's imgui.h is ImGui's, a28cb615), so they're
// reinterpreted. Anything that isn't one of them passes as it is.
template <class T>
    requires(!std::is_class_v<T>)
T& in(T& value)
{
    return value;
}
const ::ImVec2& in(const ImVec2& v) { return reinterpret_cast<const ::ImVec2&>(v); }
const ::ImVec4& in(const ImVec4& v) { return reinterpret_cast<const ::ImVec4&>(v); }
const ::ImVec2* in(const ImVec2* v) { return reinterpret_cast<const ::ImVec2*>(v); }
::ImTextureRef in(const ImTextureRef& t) { return std::bit_cast<::ImTextureRef>(t); }
::ImGuiContext* in(ImGuiContext* p) { return reinterpret_cast<::ImGuiContext*>(p); }
::ImFontAtlas* in(ImFontAtlas* p) { return reinterpret_cast<::ImFontAtlas*>(p); }
::ImFont* in(ImFont* p) { return reinterpret_cast<::ImFont*>(p); }
::ImGuiStyle* in(ImGuiStyle* p) { return reinterpret_cast<::ImGuiStyle*>(p); }
::ImGuiStorage* in(ImGuiStorage* p) { return reinterpret_cast<::ImGuiStorage*>(p); }
::ImGuiViewport* in(ImGuiViewport* p) { return reinterpret_cast<::ImGuiViewport*>(p); }
const ::ImGuiViewport* in(const ImGuiViewport* p) { return reinterpret_cast<const ::ImGuiViewport*>(p); }
const ::ImGuiWindowClass* in(const ImGuiWindowClass* p) { return reinterpret_cast<const ::ImGuiWindowClass*>(p); }
::ImGuiSizeCallback in(ImGuiSizeCallback f) { return reinterpret_cast<::ImGuiSizeCallback>(f); }
::ImGuiInputTextCallback in(ImGuiInputTextCallback f) { return reinterpret_cast<::ImGuiInputTextCallback>(f); }
::ImGuiMemAllocFunc* in(ImGuiMemAllocFunc* f) { return reinterpret_cast<::ImGuiMemAllocFunc*>(f); }
::ImGuiMemFreeFunc* in(ImGuiMemFreeFunc* f) { return reinterpret_cast<::ImGuiMemFreeFunc*>(f); }

const ::ImFontConfig* config_in(const ImFontConfig* c) { return reinterpret_cast<const ::ImFontConfig*>(c); }

struct GuiV430 : Common {
#include "gui_v430.inc"

    static bool Begin(void*, const char* name, bool* open, ImGuiWindowFlags flags) { return begin(name, open, flags); }
    static void SetWindowPos(void*, const ImVec2& pos, ImGuiCond cond) { set_window_pos(nullptr, in(pos), cond); }
    static void SetWindowPos(void*, const char* name, const ImVec2& pos, ImGuiCond cond) { set_window_pos(name, in(pos), cond); }
    static void DestroyContext(void*, ImGuiContext* context) { destroy_context(in(context)); }
    static void SetAllocatorFunctions(void*, ImGuiMemAllocFunc, ImGuiMemFreeFunc, void*) { set_allocators(); }

    static ImFont* AddFontFromFileTTF(void*, const char* file, float size, const ImFontConfig* config)
    {
        return reinterpret_cast<ImFont*>(add_font([&](::ImFontAtlas& atlas) { return atlas.AddFontFromFileTTF(file, size, config_in(config)); }));
    }
    static ImFont* AddFontFromMemoryCompressedTTF(void*, const void* data, int data_size, float size, const ImFontConfig* config)
    {
        return reinterpret_cast<ImFont*>(add_font([&](::ImFontAtlas& atlas) {
            return atlas.AddFontFromMemoryCompressedTTF(data, data_size, size, config_in(config));
        }));
    }
    static ImFont* AddFontFromMemoryCompressedBase85TTF(void*, const char* data, float size, const ImFontConfig* config)
    {
        return reinterpret_cast<ImFont*>(add_font([&](::ImFontAtlas& atlas) {
            return atlas.AddFontFromMemoryCompressedBase85TTF(data, size, config_in(config));
        }));
    }
};

} // namespace
} // namespace sdk::v430

// ---- Interface 4.16: ImGui 1.92 with 1.81's numbers and structures -------------------------------------------------

namespace sdk::v416 {
namespace {

template <class T>
    requires(!std::is_class_v<T>)
T& in(T& value)
{
    return value;
}
const ::ImVec2& in(const ImVec2& v) { return reinterpret_cast<const ::ImVec2&>(v); }
const ::ImVec4& in(const ImVec4& v) { return reinterpret_cast<const ::ImVec4&>(v); }
const ::ImVec2* in(const ImVec2* v) { return reinterpret_cast<const ::ImVec2*>(v); }
::ImGuiContext* in(ImGuiContext* p) { return reinterpret_cast<::ImGuiContext*>(p); }
::ImGuiStorage* in(ImGuiStorage* p) { return reinterpret_cast<::ImGuiStorage*>(p); }
// (the size callback's data is laid out alike in 1.81 and 1.92)
::ImGuiSizeCallback in(ImGuiSizeCallback f) { return reinterpret_cast<::ImGuiSizeCallback>(f); }

// A texture: an IDirect3DTexture8, 1.81's ImTextureID
::ImTextureRef texture(ImTextureID t)
{
    return ::ImTextureRef(static_cast<::ImTextureID>(reinterpret_cast<std::uintptr_t>(t)));
}

// ---- 1.81-shaped copies of VanaCore's structures -------------------------------------------------------------------
// What a plugin is given (GetIO, GetStyle, GetFont, io.Fonts) is one of these, filled from VanaCore's own as it's
// handed out and before each frame's plugins draw (sync()). What a plugin writes into one is seen at the next of those
// (by comparing with what it was given) and carried into VanaCore's.

v181::IO g_io{}, g_io_given{};
v181::Style g_style{}, g_style_given{};
bool g_io_valid = false, g_style_valid = false;
v181::FontAtlas g_atlas{};
std::vector<v181::Font*> g_atlas_fonts; // g_atlas.Fonts's data
std::unordered_map<::ImFont*, std::unique_ptr<v181::Font>> g_font_copy;
std::unordered_map<const v181::Font*, ::ImFont*> g_font_real;

v181::Vec2 out(const ::ImVec2& v) { return {v.x, v.y}; }
v181::Vec4 out(const ::ImVec4& v) { return {v.x, v.y, v.z, v.w}; }
::ImVec2 back(const v181::Vec2& v) { return ::ImVec2(v.x, v.y); }
::ImVec4 back(const v181::Vec4& v) { return ::ImVec4(v.x, v.y, v.z, v.w); }
bool same(const v181::Vec2& a, const v181::Vec2& b) { return a.x == b.x && a.y == b.y; }
bool same(const v181::Vec4& a, const v181::Vec4& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
template <class T>
bool same(const T& a, const T& b) { return a == b; }

v181::Font* font_out(::ImFont* font);

// A font's copy, as 1.81 described one: its size the size it was added at, its metrics at that size.
void fill_font(v181::Font& copy, ::ImFont& font)
{
    copy.FontSize = font.LegacySize;
    copy.Scale = 1.0f;
    copy.ContainerAtlas = &g_atlas;
    copy.ConfigData = nullptr;
    copy.ConfigDataCount = static_cast<short>(font.Sources.Size);
    copy.FallbackChar = static_cast<unsigned short>(font.FallbackChar);
    copy.EllipsisChar = static_cast<unsigned short>(font.EllipsisChar);
    copy.DirtyLookupTables = false;
    if (ImGui::GetCurrentContext() != nullptr && GImGui->WithinFrameScope) {
        if (::ImFontBaked* baked = font.GetFontBaked(font.LegacySize)) {
            copy.Ascent = baked->Ascent;
            copy.Descent = baked->Descent;
            copy.FallbackAdvanceX = baked->FallbackAdvanceX;
        }
    }
}

v181::Font* font_out(::ImFont* font)
{
    if (font == nullptr)
        return nullptr;
    auto& slot = g_font_copy[font];
    if (!slot) {
        slot = std::make_unique<v181::Font>();
        std::memset(slot.get(), 0, sizeof(v181::Font));
        g_font_real[slot.get()] = font;
    }
    fill_font(*slot, *font);
    return slot.get();
}

// A font a plugin passes back: one of the copies, or (from a plugin that kept one some other way) VanaCore's own.
::ImFont* font_in(const void* font)
{
    if (font == nullptr)
        return nullptr;
    const auto found = g_font_real.find(static_cast<const v181::Font*>(font));
    if (found != g_font_real.end())
        return found->second;
    for (::ImFont* real : ImGui::GetIO().Fonts->Fonts)
        if (real == font)
            return real;
    return nullptr;
}

void fill_atlas()
{
    ::ImFontAtlas& atlas = *ImGui::GetIO().Fonts;
    g_atlas.Locked = atlas.Locked;
    g_atlas.Flags = atlas.Flags;
    const ::ImTextureID id = atlas.TexRef._TexData != nullptr ? atlas.TexRef._TexData->TexID : atlas.TexRef._TexID;
    g_atlas.TexID = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
    g_atlas.TexGlyphPadding = atlas.TexGlyphPadding;
    if (atlas.TexData != nullptr) {
        g_atlas.TexWidth = atlas.TexData->Width;
        g_atlas.TexHeight = atlas.TexData->Height;
    }
    g_atlas.TexUvScale = out(atlas.TexUvScale);
    g_atlas.TexUvWhitePixel = out(atlas.TexUvWhitePixel);
    g_atlas_fonts.clear();
    for (::ImFont* font : atlas.Fonts)
        g_atlas_fonts.push_back(font_out(font));
    g_atlas.Fonts.Size = g_atlas.Fonts.Capacity = static_cast<int>(g_atlas_fonts.size());
    g_atlas.Fonts.Data = g_atlas_fonts.data();
}

// ImGuiIO's fields a plugin may set, and whether it did (since it was last given the copy).
#define IO_FIELDS(X)                                                                                                   \
    X(IniSavingRate) X(MouseDoubleClickTime) X(MouseDoubleClickMaxDist) X(MouseDragThreshold) X(KeyRepeatDelay)        \
    X(KeyRepeatRate) X(FontAllowUserScaling) X(MouseDrawCursor) X(ConfigInputTextCursorBlink)                         \
    X(ConfigDragClickToInputText) X(ConfigWindowsResizeFromEdges) X(ConfigWindowsMoveFromTitleBarOnly)                \
    X(ConfigMemoryCompactTimer) X(ConfigMacOSXBehaviors)

void io_back()
{
    if (!g_io_valid)
        return;
    ::ImGuiIO& io = ImGui::GetIO();
#define BACK(name)                                                                                                     \
    if (!same(g_io.name, g_io_given.name))                                                                             \
        io.name = g_io.name;
    IO_FIELDS(BACK)
#undef BACK
    if (g_io.ConfigFlags != g_io_given.ConfigFlags)
        io.ConfigFlags = legacy::in_ImGuiConfigFlags(g_io.ConfigFlags);
    if (g_io.FontGlobalScale != g_io_given.FontGlobalScale)
        ImGui::GetStyle().FontScaleMain = g_io.FontGlobalScale;
    if (g_io.FontDefault != g_io_given.FontDefault)
        io.FontDefault = font_in(g_io.FontDefault);
    if (!same(g_io.DisplayFramebufferScale, g_io_given.DisplayFramebufferScale))
        io.DisplayFramebufferScale = back(g_io.DisplayFramebufferScale);
}

void io_out()
{
    ::ImGuiIO& io = ImGui::GetIO();
    v181::IO& c = g_io;
#define OUT(name) c.name = io.name;
    IO_FIELDS(OUT)
#undef OUT
    c.ConfigFlags = legacy::out_ImGuiConfigFlags(io.ConfigFlags);
    c.BackendFlags = legacy::out_ImGuiBackendFlags(io.BackendFlags);
    c.DisplaySize = out(io.DisplaySize);
    c.DeltaTime = io.DeltaTime;
    c.IniFilename = io.IniFilename;
    c.LogFilename = io.LogFilename;
    for (int i = 0; i < v181::kKeyCount; ++i)
        c.KeyMap[i] = legacy::legacy_key_vk(i);
    fill_atlas();
    c.Fonts = &g_atlas;
    c.FontGlobalScale = ImGui::GetStyle().FontScaleMain;
    c.FontDefault = font_out(io.FontDefault);
    c.DisplayFramebufferScale = out(io.DisplayFramebufferScale);
    c.BackendPlatformName = io.BackendPlatformName;
    c.BackendRendererName = io.BackendRendererName;
    c.MousePos = out(io.MousePos);
    for (int i = 0; i < 5; ++i) {
        c.MouseDown[i] = io.MouseDown[i];
        c.MouseClickedPos[i] = out(io.MouseClickedPos[i]);
        c.MouseClickedTime[i] = io.MouseClickedTime[i];
        c.MouseClicked[i] = io.MouseClicked[i];
        c.MouseDoubleClicked[i] = io.MouseDoubleClicked[i];
        c.MouseReleased[i] = io.MouseReleased[i];
        c.MouseDownOwned[i] = io.MouseDownOwned[i];
        c.MouseDownDuration[i] = io.MouseDownDuration[i];
        c.MouseDownDurationPrev[i] = io.MouseDownDurationPrev[i];
        c.MouseDragMaxDistanceAbs[i] = out(io.MouseDragMaxDistanceAbs[i]);
        c.MouseDragMaxDistanceSqr[i] = io.MouseDragMaxDistanceSqr[i];
    }
    c.MouseWheel = io.MouseWheel;
    c.MouseWheelH = io.MouseWheelH;
    c.KeyCtrl = io.KeyCtrl;
    c.KeyShift = io.KeyShift;
    c.KeyAlt = io.KeyAlt;
    c.KeySuper = io.KeySuper;
    c.KeyMods = legacy::out_ImGuiKeyModFlags(io.KeyMods);
    // 1.81's keys were Win32's key codes (Ashita's io.KeyMap)
    if (ImGui::GetCurrentContext() != nullptr) {
        for (int vk = 0; vk < 256; ++vk) {
            const ::ImGuiKey key = legacy::vk_key(vk);
            const ImGuiKeyData* data = key != ImGuiKey_None ? ImGui::GetKeyData(key) : nullptr;
            c.KeysDown[vk] = data != nullptr && data->Down;
            c.KeysDownDuration[vk] = data != nullptr ? data->DownDuration : -1.0f;
            c.KeysDownDurationPrev[vk] = data != nullptr ? data->DownDurationPrev : -1.0f;
        }
    }
    c.WantCaptureMouse = io.WantCaptureMouse;
    c.WantCaptureKeyboard = io.WantCaptureKeyboard;
    c.WantTextInput = io.WantTextInput;
    c.WantSetMousePos = io.WantSetMousePos;
    c.WantSaveIniSettings = io.WantSaveIniSettings;
    c.NavActive = io.NavActive;
    c.NavVisible = io.NavVisible;
    c.Framerate = io.Framerate;
    c.MetricsRenderVertices = io.MetricsRenderVertices;
    c.MetricsRenderIndices = io.MetricsRenderIndices;
    c.MetricsRenderWindows = io.MetricsRenderWindows;
    c.MetricsActiveWindows = io.MetricsActiveWindows;
    c.MouseDelta = out(io.MouseDelta);
    c.MousePosPrev = out(io.MousePosPrev);
    c.PenPressure = io.PenPressure;
    g_io_given = g_io;
    g_io_valid = true;
}

// ImGuiStyle: every field a plugin may set; colours by 1.81's numbers.
#define STYLE_FIELDS(X)                                                                                                \
    X(Alpha) X(WindowPadding) X(WindowRounding) X(WindowBorderSize) X(WindowMinSize) X(WindowTitleAlign)               \
    X(WindowMenuButtonPosition) X(ChildRounding) X(ChildBorderSize) X(PopupRounding) X(PopupBorderSize)                \
    X(FramePadding) X(FrameRounding) X(FrameBorderSize) X(ItemSpacing) X(ItemInnerSpacing) X(CellPadding)              \
    X(TouchExtraPadding) X(IndentSpacing) X(ColumnsMinSpacing) X(ScrollbarSize) X(ScrollbarRounding) X(GrabMinSize)    \
    X(GrabRounding) X(LogSliderDeadzone) X(TabRounding) X(TabBorderSize) X(ColorButtonPosition) X(ButtonTextAlign)     \
    X(SelectableTextAlign) X(DisplayWindowPadding) X(DisplaySafeAreaPadding) X(MouseCursorScale) X(AntiAliasedLines)  \
    X(AntiAliasedLinesUseTex) X(AntiAliasedFill) X(CurveTessellationTol)

template <class T>
T value_of(const T& v) { return v; }
v181::Vec2 value_of(const ::ImVec2& v) { return out(v); }
float value_back(float v) { return v; }
int value_back(int v) { return v; }
bool value_back(bool v) { return v; }
::ImVec2 value_back(const v181::Vec2& v) { return back(v); }

// A 1.81 style's contents from a 1.92 one, and the other way (a field at a time where 1.81's `given` differs from
// `from`: what a plugin changed).
void style_out(const ::ImGuiStyle& s, v181::Style& c)
{
#define OUT(name) c.name = static_cast<decltype(c.name)>(value_of(s.name));
    STYLE_FIELDS(OUT)
#undef OUT
    c.TabMinWidthForCloseButton = s.TabCloseButtonMinWidthUnselected;
    c.CircleSegmentMaxError = s.CircleTessellationMaxError;
    for (int i = 0; i < v181::kColCount; ++i) {
        const int to = legacy::in_ImGuiCol(i);
        c.Colors[i] = to >= 0 && to < ImGuiCol_COUNT ? out(s.Colors[to]) : v181::Vec4{};
    }
}

void style_back(const v181::Style& c, const v181::Style* given, ::ImGuiStyle& s)
{
#define BACK(name)                                                                                                     \
    if (given == nullptr || !same(c.name, given->name))                                                                \
        s.name = static_cast<decltype(s.name)>(value_back(c.name));
    STYLE_FIELDS(BACK)
#undef BACK
    if (given == nullptr || c.TabMinWidthForCloseButton != given->TabMinWidthForCloseButton)
        s.TabCloseButtonMinWidthUnselected = c.TabMinWidthForCloseButton;
    if (given == nullptr || c.CircleSegmentMaxError != given->CircleSegmentMaxError)
        s.CircleTessellationMaxError = c.CircleSegmentMaxError;
    for (int i = 0; i < v181::kColCount; ++i) {
        const int to = legacy::in_ImGuiCol(i);
        if (to >= 0 && to < ImGuiCol_COUNT && (given == nullptr || !same(c.Colors[i], given->Colors[i])))
            s.Colors[to] = back(c.Colors[i]);
    }
}

void style_refresh()
{
    style_out(ImGui::GetStyle(), g_style);
    g_style_given = g_style;
    g_style_valid = true;
}

// What a plugin wrote into its copies, into VanaCore's; then the copies as VanaCore's are now.
void sync()
{
    if (ImGui::GetCurrentContext() == nullptr)
        return;
    io_back();
    if (g_style_valid)
        style_back(g_style, &g_style_given, ImGui::GetStyle());
    io_out();
    style_refresh();
}

// A 1.81 style a plugin keeps of its own (StyleColorsDark(&mine)), or the copy it was given.
template <void (*Colours)(::ImGuiStyle*)>
void style_colours(ImGuiStyle* dst)
{
    auto* mine = reinterpret_cast<v181::Style*>(dst);
    if (mine == nullptr || mine == &g_style) {
        if (mine == &g_style)
            sync();
        Colours(nullptr);
        style_refresh();
        return;
    }
    ::ImGuiStyle made = ImGui::GetStyle();
    Colours(&made);
    v181::Style colours{};
    style_out(made, colours);
    std::memcpy(mine->Colors, colours.Colors, sizeof mine->Colors);
}
void dark(::ImGuiStyle* s) { ImGui::StyleColorsDark(s); }
void light(::ImGuiStyle* s) { ImGui::StyleColorsLight(s); }
void classic(::ImGuiStyle* s) { ImGui::StyleColorsClassic(s); }

// ---- InputText's callback, 1.81's ------------------------------------------------------------------------------------

using Callback181 = int (*)(v181::InputTextCallbackData*);
struct InputCallback {
    Callback181 callback;
    void* user_data;
};

int input_callback(::ImGuiInputTextCallbackData* data)
{
    const auto* call = static_cast<const InputCallback*>(data->UserData);
    v181::InputTextCallbackData d{};
    d.EventFlag = legacy::out_ImGuiInputTextFlags(data->EventFlag);
    d.Flags = legacy::out_ImGuiInputTextFlags(data->Flags);
    d.UserData = call->user_data;
    d.EventChar = static_cast<unsigned short>(data->EventChar);
    d.EventKey = legacy::legacy_key_of(data->EventKey);
    d.Buf = data->Buf;
    d.BufTextLen = data->BufTextLen;
    d.BufSize = data->BufSize;
    d.BufDirty = false;
    d.CursorPos = data->CursorPos;
    d.SelectionStart = data->SelectionStart;
    d.SelectionEnd = data->SelectionEnd;
    const int result = call->callback(&d);
    data->EventChar = static_cast<ImWchar>(d.EventChar);
    data->Buf = d.Buf;
    data->BufSize = d.BufSize;
    if (d.BufDirty) {
        data->BufTextLen = d.BufTextLen;
        data->BufDirty = true;
    }
    data->CursorPos = d.CursorPos;
    data->SelectionStart = d.SelectionStart;
    data->SelectionEnd = d.SelectionEnd;
    return result;
}

struct GuiV416 : Common {
#include "gui_v416.inc"

    static bool Begin(void*, const char* name, bool* open, ImGuiWindowFlags flags)
    {
        return begin(name, open, legacy::in_ImGuiWindowFlags(flags));
    }
    static void SetWindowPos(void*, const ImVec2& pos, ImGuiCond cond) { set_window_pos(nullptr, in(pos), legacy::in_ImGuiCond(cond)); }
    static void SetWindowPos(void*, const char* name, const ImVec2& pos, ImGuiCond cond)
    {
        set_window_pos(name, in(pos), legacy::in_ImGuiCond(cond));
    }
    static void DestroyContext(void*, ImGuiContext* context) { destroy_context(in(context)); }
    static void SetAllocatorFunctions(void*, void* (*)(size_t, void*), void (*)(void*, void*), void*) { set_allocators(); }
    // One context for everything: a plugin asking for another gets VanaCore's
    static ImGuiContext* CreateContext(void*, ImFontAtlas*)
    {
        if (!g_logged_context.exchange(true))
            log::write(L"Ashita plugins: %hs asked for an ImGui context of its own; it was given the one everything draws with",
                       caller_name().c_str());
        return reinterpret_cast<ImGuiContext*>(gui::context());
    }

    static ImGuiIO& GetIO(void*)
    {
        sync();
        return reinterpret_cast<ImGuiIO&>(g_io);
    }
    static ImGuiStyle& GetStyle(void*)
    {
        sync();
        return reinterpret_cast<ImGuiStyle&>(g_style);
    }
    static void ShowStyleEditor(void*, ImGuiStyle* ref)
    {
        sync();
        ImGui::ShowStyleEditor(nullptr);
        (void)ref;
    }
    static void StyleColorsDark(void*, ImGuiStyle* dst) { style_colours<&dark>(dst); }
    static void StyleColorsLight(void*, ImGuiStyle* dst) { style_colours<&light>(dst); }
    static void StyleColorsClassic(void*, ImGuiStyle* dst) { style_colours<&classic>(dst); }

    // 1.81's PushFont(font) drew at the size the font was added at
    static void PushFont(void*, ImFont* font)
    {
        ::ImFont* real = font_in(font);
        ImGui::PushFont(real, real != nullptr ? real->LegacySize : 0.0f);
    }
    static ImFont* GetFont(void*) { return reinterpret_cast<ImFont*>(font_out(ImGui::GetFont())); }

    static ImFont* add(::ImFont* font) { return reinterpret_cast<ImFont*>(font_out(font)); }
    static ::ImFontConfig config(const ImFontConfig* given, const ImWchar* ranges)
    {
        ::ImFontConfig c;
        if (const auto* g = reinterpret_cast<const v181::FontConfig*>(given)) {
            c.FontData = g->FontData;
            c.FontDataSize = g->FontDataSize;
            c.FontDataOwnedByAtlas = g->FontDataOwnedByAtlas;
            c.FontNo = static_cast<ImU32>(g->FontNo);
            c.SizePixels = g->SizePixels;
            c.OversampleH = static_cast<ImS8>(g->OversampleH);
            c.OversampleV = static_cast<ImS8>(g->OversampleV);
            c.PixelSnapH = g->PixelSnapH;
            c.GlyphExtraAdvanceX = g->GlyphExtraSpacing.x;
            c.GlyphOffset = back(g->GlyphOffset);
            c.GlyphRanges = g->GlyphRanges;
            c.GlyphMinAdvanceX = g->GlyphMinAdvanceX;
            c.GlyphMaxAdvanceX = g->GlyphMaxAdvanceX;
            c.MergeMode = g->MergeMode;
            c.FontLoaderFlags = g->FontBuilderFlags;
            c.RasterizerMultiply = g->RasterizerMultiply;
            c.EllipsisChar = g->EllipsisChar;
            std::memcpy(c.Name, g->Name, sizeof c.Name);
        }
        if (ranges != nullptr)
            c.GlyphRanges = ranges;
        return c;
    }
    static ImFont* AddFontFromFileTTF(void*, const char* file, float size, const ImFontConfig* given, const ImWchar* ranges)
    {
        const ::ImFontConfig c = config(given, ranges);
        return add(add_font([&](::ImFontAtlas& atlas) { return atlas.AddFontFromFileTTF(file, size, &c); }));
    }
    static ImFont* AddFontFromMemoryCompressedTTF(void*, const void* data, int data_size, float size, const ImFontConfig* given,
                                                  const ImWchar* ranges)
    {
        const ::ImFontConfig c = config(given, ranges);
        return add(add_font([&](::ImFontAtlas& atlas) { return atlas.AddFontFromMemoryCompressedTTF(data, data_size, size, &c); }));
    }
    static ImFont* AddFontFromMemoryCompressedBase85TTF(void*, const char* data, float size, const ImFontConfig* given,
                                                        const ImWchar* ranges)
    {
        const ::ImFontConfig c = config(given, ranges);
        return add(add_font([&](::ImFontAtlas& atlas) { return atlas.AddFontFromMemoryCompressedBase85TTF(data, size, &c); }));
    }

    // 1.81's ImageButton: the texture is its ID, frame_padding its padding (-1: the style's)
    static bool ImageButton(void*, ImTextureID tex, const ImVec2& size, const ImVec2& uv0, const ImVec2& uv1, int frame_padding,
                            const ImVec4& bg, const ImVec4& tint)
    {
        ImGui::PushID(tex);
        if (frame_padding >= 0)
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ::ImVec2(static_cast<float>(frame_padding), static_cast<float>(frame_padding)));
        const bool pressed = ImGui::ImageButton("#image", texture(tex), in(size), in(uv0), in(uv1), in(bg), in(tint));
        if (frame_padding >= 0)
            ImGui::PopStyleVar();
        ImGui::PopID();
        return pressed;
    }

    static bool InputText(void*, const char* label, char* buf, size_t size, ImGuiInputTextFlags flags, ImGuiInputTextCallback callback,
                          void* user_data)
    {
        InputCallback call{reinterpret_cast<Callback181>(callback), user_data};
        return ImGui::InputText(label, buf, size, legacy::in_ImGuiInputTextFlags(flags), callback != nullptr ? &input_callback : nullptr,
                                callback != nullptr ? &call : user_data);
    }
    static bool InputTextMultiline(void*, const char* label, char* buf, size_t size, const ImVec2& box, ImGuiInputTextFlags flags,
                                   ImGuiInputTextCallback callback, void* user_data)
    {
        InputCallback call{reinterpret_cast<Callback181>(callback), user_data};
        return ImGui::InputTextMultiline(label, buf, size, in(box), legacy::in_ImGuiInputTextFlags(flags),
                                         callback != nullptr ? &input_callback : nullptr, callback != nullptr ? &call : user_data);
    }
    static bool InputTextWithHint(void*, const char* label, const char* hint, char* buf, size_t size, ImGuiInputTextFlags flags,
                                  ImGuiInputTextCallback callback, void* user_data)
    {
        InputCallback call{reinterpret_cast<Callback181>(callback), user_data};
        return ImGui::InputTextWithHint(label, hint, buf, size, legacy::in_ImGuiInputTextFlags(flags),
                                        callback != nullptr ? &input_callback : nullptr, callback != nullptr ? &call : user_data);
    }

    static bool ListBoxHeader(void*, const char* label, const ImVec2& size) { return ImGui::BeginListBox(label, in(size)); }
    static bool ListBoxHeader(void*, const char* label, int count, int height_in_items)
    {
        const int rows = height_in_items < 0 ? (count < 7 ? count : 7) : height_in_items;
        const float height = ImGui::GetTextLineHeightWithSpacing() * (static_cast<float>(rows) + 0.25f) + ImGui::GetStyle().FramePadding.y * 2.0f;
        return ImGui::BeginListBox(label, ::ImVec2(0.0f, height));
    }
    static void ListBoxFooter(void*) { ImGui::EndListBox(); }

    // 1.81's: which of `count` rows `height` apart, from the cursor down, the window shows
    static void CalcListClipping(void*, int count, float height, int* start, int* end)
    {
        ImGuiWindow* window = GImGui != nullptr ? GImGui->CurrentWindow : nullptr;
        int first = 0, last = count;
        if (window != nullptr && height > 0.0f && !GImGui->LogEnabled) {
            if (window->SkipItems) {
                first = last = 0;
            } else {
                const float y = window->DC.CursorPos.y;
                first = static_cast<int>((window->ClipRect.Min.y - y) / height);
                last = static_cast<int>((window->ClipRect.Max.y - y) / height) + 1;
                first = first < 0 ? 0 : first > count ? count : first;
                last = last < first ? first : last > count ? count : last;
            }
        }
        if (start != nullptr)
            *start = first;
        if (end != nullptr)
            *end = last;
    }

    // 1.81's keys: Win32's key codes (Ashita's io.KeyMap), GetKeyIndex giving one for an ImGuiKey_
    static int GetKeyIndex(void*, ImGuiKey key) { return legacy::legacy_key_vk(key); }
    static bool IsKeyDown(void*, int vk)
    {
        const ::ImGuiKey key = legacy::key_in(vk);
        return key != ImGuiKey_None && ImGui::IsKeyDown(key);
    }
    static bool IsKeyPressed(void*, int vk, bool repeat)
    {
        const ::ImGuiKey key = legacy::key_in(vk);
        return key != ImGuiKey_None && ImGui::IsKeyPressed(key, repeat);
    }
    static bool IsKeyReleased(void*, int vk)
    {
        const ::ImGuiKey key = legacy::key_in(vk);
        return key != ImGuiKey_None && ImGui::IsKeyReleased(key);
    }
    static int GetKeyPressedAmount(void*, int vk, float delay, float rate)
    {
        const ::ImGuiKey key = legacy::key_in(vk);
        return key != ImGuiKey_None ? ImGui::GetKeyPressedAmount(key, delay, rate) : 0;
    }
    static void CaptureKeyboardFromApp(void*, bool capture) { ImGui::SetNextFrameWantCaptureKeyboard(capture); }
    static void CaptureMouseFromApp(void*, bool capture) { ImGui::SetNextFrameWantCaptureMouse(capture); }
    static void PushAllowKeyboardFocus(void*, bool allow) { ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop, !allow); }
    static void PopAllowKeyboardFocus(void*) { ImGui::PopItemFlag(); }
    static float GetWindowContentRegionWidth(void*) { return ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x; }
};

} // namespace
} // namespace sdk::v416

void gui_frame()
{
    sdk::v416::sync();
}

void build_gui(Links& a, Links& b)
{
    a.gui = make<sdk::v416::GuiManagerObject, sdk::v416::GuiV416>(&a);
    b.gui = make<sdk::v430::GuiManagerObject, sdk::v430::GuiV430>(&b);
}

} // namespace vanacore::ashita
