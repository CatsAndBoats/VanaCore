// IFontManager and IPrimitiveManager: plugins' on-screen text and rectangles, drawn by VanaCore's
// overlay every frame (fonts with VanaCore's text renderer, primitives as filled or textured rectangles
// with their borders).

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../../d3d8.hpp"
#include "../../gametext.hpp"
#include "../../layout.hpp"
#include "../../log.hpp"
#include "../../text.hpp"
#include "../../ui.hpp"
#include "../plugin_move.hpp"
#include "backend.hpp"

namespace vanacore::ashita {
namespace {

using sdk::D3DCOLOR;
using sdk::fontkeyboardevent_f;
using sdk::fontmouseevent_f;
using sdk::Ashita::FontBorderFlags;
using sdk::Ashita::FontCreateFlags;
using sdk::Ashita::FontDrawFlags;
using sdk::Ashita::FrameAnchor;
using sdk::Ashita::PrimitiveDrawFlags;

struct PrimitiveState {
    Links* links = nullptr;
    void* object = nullptr; // this version's IPrimitiveObject
    std::string alias;
    std::string owner;
    bool visible = false;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    float texture_x = 0.0f;
    float texture_y = 0.0f;
    D3DCOLOR color = 0xFFFFFFFF;
    bool border_visible = false;
    D3DCOLOR border_color = 0xFF000000;
    FontBorderFlags border_flags = FontBorderFlags::All;
    RECT border_sizes{};
    bool can_focus = false;
    bool locked = false;
    bool locked_z = false;
    PrimitiveDrawFlags draw_flags = PrimitiveDrawFlags::None;
    // Its texture (IDirect3DTexture8, a reference of its own) and its size; where it came from, to make it again on a
    // new device (a file, or the image's bytes; neither for a plugin's own texture).
    void* texture = nullptr;
    UINT texture_width = 0;
    UINT texture_height = 0;
    std::wstring texture_path;
    std::vector<uint8_t> texture_data;
    D3DCOLOR colour_key = 0;
    fontkeyboardevent_f keyboard = nullptr;
    fontmouseevent_f mouse = nullptr;
};

struct FontState {
    Links* links = nullptr;
    void* object = nullptr; // this version's IFontObject
    std::string alias;
    std::string owner;
    std::string text;
    std::string family = "Arial";
    std::string file;
    uint32_t height = 10; // points
    FontCreateFlags create_flags = FontCreateFlags::None;
    FontDrawFlags draw_flags = FontDrawFlags::None;
    bool visible = true;
    bool can_focus = false;
    bool locked = false;
    bool locked_z = false;
    bool dirty = true;
    bool bold = false;
    bool italic = false;
    bool right = false;
    bool strike = false;
    bool underline = false;
    bool auto_resize = true;
    D3DCOLOR color = 0xFFFFFFFF;
    D3DCOLOR outline = 0xFF000000;
    float padding = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    float window_width = 0.0f;
    float window_height = 0.0f;
    FrameAnchor anchor = FrameAnchor::TopLeft;
    FrameAnchor anchor_parent = FrameAnchor::TopLeft;
    FontState* parent = nullptr;
    PrimitiveState background;
    text::Sprite sprite;
    bool baked = false;       // the background is painted into the text's pixels (refresh), as the last one did it
    D3DCOLOR baked_color = 0; // in this colour
    fontkeyboardevent_f keyboard = nullptr;
    fontmouseevent_f mouse = nullptr;
};

std::recursive_mutex g_lock;
std::vector<std::unique_ptr<FontState>> g_fonts;           // in the order they were made
std::vector<std::unique_ptr<PrimitiveState>> g_primitives; // likewise
bool g_fonts_visible = true;
bool g_primitives_visible = true;

// What Addons > Layout set for the objects an owner has on the screen, as the last frame had it: everything of the owner's
// drawn scaled from (ox, oy), the top-left corner of all of it, then moved by (dx, dy). Nothing set is the identity.
struct Group {
    float ox = 0.0f, oy = 0.0f;
    float dx = 0.0f, dy = 0.0f;
    float scale = 1.0f;
};
std::map<std::string, Group> g_groups; // by owner

Group group_of(const std::string& owner)
{
    const auto found = g_groups.find(owner);
    return found != g_groups.end() ? found->second : Group{};
}

// Where a point the owner's objects put themselves is drawn.
struct Spot {
    float x, y;
};
Spot moved(const Group& g, float x, float y)
{
    return {g.ox + (x - g.ox) * g.scale + g.dx, g.oy + (y - g.oy) * g.scale + g.dy};
}

// "addon equipmon" (as calling() has an Ashita addon) as the Layout tab knows it: "ashita/equipmon"; a plugin: "plugin/Minimap"
std::string layout_owner(const std::string& owner)
{
    if (owner.empty())
        return {};
    return owner.rfind("addon ", 0) == 0 ? "ashita/" + owner.substr(6) : "plugin/" + owner;
}

FontState& font(void* context)
{
    return *static_cast<FontState*>(context);
}

PrimitiveState& primitive(void* context)
{
    return *static_cast<PrimitiveState*>(context);
}

std::wstring wide(const std::string& text)
{
    // FFXI's text, and plugins', is Shift-JIS, with the game's own row for ñ, é, ¿ and the like
    return gametext::to_wide(text);
}

bool has(FontDrawFlags flags, FontDrawFlags flag)
{
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
}

// Whether a font's background is a box of its own under the text, as Ashita draws them, rather than
// painted round the text at the text's size: when it keeps a size of its own (auto_resize off), has a
// texture, or a border.
bool own_background(const FontState& f)
{
    return f.background.visible && (!f.auto_resize || f.background.texture != nullptr || f.background.border_visible);
}

// Whether a font draws anything: its text, or with none, a background that keeps a size of its own (auto_resize
// off), which Ashita draws all the same. Points' compact bar is such a box, its icon and text other fonts on it.
bool shows(const FontState& f)
{
    if (!f.text.empty())
        return true;
    const PrimitiveState& b = f.background;
    return b.visible && !f.auto_resize && b.width > 0.0f && b.height > 0.0f;
}

// The text as it will be drawn (rendered now if anything changed), so its size is known.
void refresh(FontState& f)
{
    text::Style style;
    style.family = wide(f.family);
    style.size = std::max(6, static_cast<int>(f.height * 4 / 3));
    style.weight = f.bold ? FW_BOLD : FW_NORMAL;
    style.italic = f.italic;
    text::argb_colour(f.color, style.color);
    style.stroke = has(f.draw_flags, FontDrawFlags::Outlined) ? 1 : 0;
    text::argb_colour(f.outline, style.stroke_color);
    f.baked = f.background.visible && !own_background(f);
    f.baked_color = f.background.color;
    if (f.baked)
        text::argb_colour(f.background.color, style.background);
    else
        style.background[3] = 0.0f;
    style.padding = static_cast<int>(f.padding);
    style.align_right = f.right;
    style.color_codes = true; // Ashita's |cAARRGGBB| ... |r
    f.sprite.set(wide(f.text), style);
    f.dirty = false;
}

// Renders the text again when it, or how its background is painted into it, changed: the background's
// own settings (visible, colour, texture, border) don't mark the font.
void refresh_if_stale(FontState& f)
{
    const bool bake = f.background.visible && !own_background(f);
    if (f.dirty || f.baked != bake || (bake && f.baked_color != f.background.color))
        refresh(f);
}

// A font's box: its background's size when that keeps one of its own (auto_resize off), else its text's.
SIZE extent(FontState& f)
{
    refresh_if_stale(f);
    const PrimitiveState& b = f.background;
    if (b.visible && !f.auto_resize && b.width > 0.0f && b.height > 0.0f)
        return {static_cast<LONG>(b.width * b.scale_x), static_cast<LONG>(b.height * b.scale_y)};
    return {f.sprite.width(), f.sprite.height()};
}

// Where a font's top-left corner goes: its position, counted from the corner of the screen its anchor
// names, or, under a parent, from the corner of the parent that anchor_parent names (Deeps hangs each bar
// from the bottom-left of the one above it), to the corner of its own box the anchor names. Its background
// is put there too, at the text's size when it follows the text: addons read where it is and hit-test it.
POINT place(FontState& f, int depth = 0)
{
    unsigned screen_width = 0, screen_height = 0;
    ui::game_screen(screen_width, screen_height);
    const auto anchor = static_cast<uint32_t>(f.anchor);
    const bool from_right = (anchor & static_cast<uint32_t>(FrameAnchor::Right)) != 0;
    const bool from_bottom = (anchor & static_cast<uint32_t>(FrameAnchor::Bottom)) != 0;
    float left = 0.0f, top = 0.0f, right = static_cast<float>(screen_width), bottom = static_cast<float>(screen_height);
    bool at_right = from_right, at_bottom = from_bottom; // the corner the position counts from
    if (f.parent != nullptr && depth < 8) {
        const POINT origin = place(*f.parent, depth + 1);
        const SIZE parent = extent(*f.parent);
        left = static_cast<float>(origin.x);
        top = static_cast<float>(origin.y);
        right = left + static_cast<float>(parent.cx);
        bottom = top + static_cast<float>(parent.cy);
        const auto corner = static_cast<uint32_t>(f.anchor_parent);
        at_right = (corner & static_cast<uint32_t>(FrameAnchor::Right)) != 0;
        at_bottom = (corner & static_cast<uint32_t>(FrameAnchor::Bottom)) != 0;
    }
    const SIZE box = extent(f);
    const float width = static_cast<float>(box.cx);
    const float height = static_cast<float>(box.cy);
    const float from_x = at_right ? right : left;
    const float from_y = at_bottom ? bottom : top;
    float x = from_right ? from_x - f.x - width : from_x + f.x;
    float y = from_bottom ? from_y - f.y - height : from_y + f.y;
    // A negative position on the screen counts from its right or bottom edge (tparty sits on it).
    if (f.parent == nullptr && !from_right && f.x < 0.0f)
        x += right;
    if (f.parent == nullptr && !from_bottom && f.y < 0.0f)
        y += bottom;
    if (f.right && !from_right)
        x -= width; // right-justified: x is the right edge
    const POINT at = {static_cast<LONG>(x), static_cast<LONG>(y)};
    f.background.x = static_cast<float>(at.x);
    f.background.y = static_cast<float>(at.y);
    if (f.auto_resize) {
        f.background.width = static_cast<float>(f.sprite.width());
        f.background.height = static_cast<float>(f.sprite.height());
    }
    return at;
}

void release_texture(PrimitiveState& p)
{
    if (p.texture != nullptr)
        d3d8::call<ULONG>(p.texture, d3d8::slot::Release);
    p.texture = nullptr;
    p.texture_width = p.texture_height = 0;
}

// Makes the primitive's texture from where it came from (on a new device too). False if it can't be read.
bool load_primitive_texture(PrimitiveState& p)
{
    if (p.texture_path.empty() && p.texture_data.empty())
        return false;
    UINT width = 0, height = 0;
    void* texture = load_texture(device(), p.texture_path, p.texture_data.data(), static_cast<UINT>(p.texture_data.size()),
                                 p.colour_key, width, height);
    release_texture(p);
    if (texture == nullptr)
        return false;
    p.texture = texture;
    p.texture_width = width;
    p.texture_height = height;
    // a primitive without a size yet takes its texture's
    if (p.width <= 0.0f && p.height <= 0.0f) {
        p.width = static_cast<float>(width);
        p.height = static_cast<float>(height);
    }
    return true;
}

bool set_texture_file(PrimitiveState& p, std::wstring path, D3DCOLOR colour_key)
{
    p.texture_path = std::move(path);
    p.texture_data.clear();
    p.colour_key = colour_key;
    if (load_primitive_texture(p))
        return true;
    log::write(L"Ashita plugins: %hs's primitive %hs: couldn't load the texture %ls", p.owner.c_str(), p.alias.c_str(),
               p.texture_path.c_str());
    p.texture_path.clear();
    return false;
}

bool set_texture_data(PrimitiveState& p, const void* data, size_t size, D3DCOLOR colour_key)
{
    if (data == nullptr || size == 0)
        return false;
    p.texture_path.clear();
    p.texture_data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    p.colour_key = colour_key;
    if (load_primitive_texture(p))
        return true;
    p.texture_data.clear();
    return false;
}

// A path a plugin names: as it is when absolute, else from VanaCore's folder.
std::wstring plugin_path(const char* name)
{
    std::wstring path = wide(name != nullptr ? name : "");
    if (path.size() >= 2 && (path[1] == L':' || (path[0] == L'\\' && path[1] == L'\\')))
        return path;
    return wide(install_path()) + path;
}

// A texture of the resource manager's by name: an image file, as plugin_path finds it, else in VanaCore's resources
// folder.
std::wstring cached_texture_path(const char* name)
{
    std::wstring path = plugin_path(name);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        path = wide(install_path()) + L"resources\\" + wide(name != nullptr ? name : "");
    return path;
}

// At (x, y) as it's to be drawn, `scale` times as big (Addons > Layout)
void draw_primitive(void* device, const PrimitiveState& p, float x, float y, float scale = 1.0f)
{
    const float width = p.width * p.scale_x * scale;
    const float height = p.height * p.scale_y * scale;
    if (p.texture != nullptr && p.texture_width != 0 && p.texture_height != 0) {
        // the part of the texture at its offset that is the primitive's own size (in the texture's pixels: an
        // atlas of icons shows one, as points' job icons and HXUI's party titles have it), the whole texture when
        // that's smaller than the primitive
        const float tw = static_cast<float>(p.texture_width);
        const float th = static_cast<float>(p.texture_height);
        const float u = p.texture_x / tw;
        const float v = p.texture_y / th;
        const float u2 = std::min(p.texture_x + std::max(p.width, 1.0f), tw) / tw;
        const float v2 = std::min(p.texture_y + std::max(p.height, 1.0f), th) / th;
        text::draw_texture(device, p.texture, x, y, width, height, u, v, u2, v2, p.color);
    } else {
        text::fill(device, static_cast<int>(x), static_cast<int>(y), static_cast<int>(width), static_cast<int>(height), p.color);
    }
    if (!p.border_visible)
        return;
    // the border outside it, its sides as the flags have them
    const auto flags = static_cast<uint32_t>(p.border_flags);
    const auto side = [&](FontBorderFlags flag) { return (flags & static_cast<uint32_t>(flag)) != 0; };
    const auto sized = [&](LONG size) { return static_cast<int>(std::lround(static_cast<float>(size) * scale)); };
    const int left = side(FontBorderFlags::Left) ? sized(p.border_sizes.left) : 0;
    const int top = side(FontBorderFlags::Top) ? sized(p.border_sizes.top) : 0;
    const int right = side(FontBorderFlags::Right) ? sized(p.border_sizes.right) : 0;
    const int bottom = side(FontBorderFlags::Bottom) ? sized(p.border_sizes.bottom) : 0;
    const int ix = static_cast<int>(x), iy = static_cast<int>(y), iw = static_cast<int>(width), ih = static_cast<int>(height);
    text::fill(device, ix - left, iy - top, iw + left + right, top, p.border_color);
    text::fill(device, ix - left, iy + ih, iw + left + right, bottom, p.border_color);
    text::fill(device, ix - left, iy, left, ih, p.border_color);
    text::fill(device, ix + iw, iy, right, ih, p.border_color);
}

// A font or primitive drawn by hand (Render: ManualRender) from a plugin's or addon's present, amid
// what it set on the device: VanaCore's drawing state for it alone, then theirs back.
template <class Draw>
void by_hand(Draw draw)
{
    void* d = device();
    if (d == nullptr)
        return;
    unsigned width = 0, height = 0;
    ui::game_screen(width, height);
    DWORD state = 0;
    if (!text::prepare(d, width, height, state))
        return;
    plugin_move::pause(); // (moved as its owner's objects are, by its own place: not by the plugin's drawing's move as well)
    draw(d);
    plugin_move::resume();
    text::restore(d, state);
}

void draw_font(void* device, FontState& f)
{
    refresh_if_stale(f);
    if (!shows(f) || (!f.text.empty() && !f.sprite.upload(device)))
        return;
    const POINT at = place(f);
    const Group g = group_of(f.owner);
    const Spot to = moved(g, static_cast<float>(at.x), static_cast<float>(at.y));
    if (own_background(f))
        draw_primitive(device, f.background, to.x, to.y, g.scale);
    if (!f.text.empty())
        f.sprite.draw(device, static_cast<int>(std::lround(to.x)), static_cast<int>(std::lround(to.y)), 1.0f, g.scale);
}

// A primitive where its owner's group has it
void draw_primitive_moved(void* device, const PrimitiveState& p)
{
    const Group g = group_of(p.owner);
    const Spot to = moved(g, p.x, p.y);
    draw_primitive(device, p, to.x, to.y, g.scale);
}

PrimitiveState* find_primitive(const char* alias)
{
    for (auto& p : g_primitives)
        if (alias != nullptr && same(p->alias, alias))
            return p.get();
    return nullptr;
}

FontState* find_font(const char* alias)
{
    for (auto& f : g_fonts)
        if (alias != nullptr && same(f->alias, alias))
            return f.get();
    return nullptr;
}

void free_primitive_object(PrimitiveState& p)
{
    release_texture(p);
    if (p.object != nullptr)
        p.links->free_primitive(p.object);
    p.object = nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

#define PROPERTY(Type, Name, State, field)                                                                      \
    static Type Get##Name(void* c) { const std::lock_guard lock(g_lock); return State(c).field; }                  \
    static void Set##Name(void* c, Type value) { const std::lock_guard lock(g_lock); State(c).field = value; }
#define FONT_PROPERTY(Type, Name, field)                                                                        \
    static Type Get##Name(void* c) { const std::lock_guard lock(g_lock); return font(c).field; }                   \
    static void Set##Name(void* c, Type value)                                                                  \
    {                                                                                                           \
        const std::lock_guard lock(g_lock);                                                                     \
        font(c).field = value;                                                                                  \
        font(c).dirty = true;                                                                                   \
    }

struct PrimitiveImpl {
    static void Render(void* c)
    {
        const std::lock_guard lock(g_lock);
        const PrimitiveState& p = primitive(c);
        by_hand([&](void* d) { draw_primitive_moved(d, p); });
    }
    // Textures: an image file (a path from VanaCore's folder unless absolute), an image in memory (`colour_key` made
    // transparent), a module's resource (its type "PNG", "BMP" or "RCDATA"), one of the resource manager's (as
    // GetTexture finds it), or a texture of the plugin's own at that size.
    static bool SetTextureFromFile(void* c, const char* path)
    {
        const std::lock_guard lock(g_lock);
        return set_texture_file(primitive(c), plugin_path(path), 0);
    }
    static bool SetTextureFromMemory(void* c, const void* data, uint32_t size, D3DCOLOR colour_key)
    {
        const std::lock_guard lock(g_lock);
        return set_texture_data(primitive(c), data, size, colour_key);
    }
    static bool SetTextureFromResource(void* c, const char* module, const char* name)
    {
        const std::lock_guard lock(g_lock);
        const HMODULE handle = module != nullptr && module[0] != '\0' ? GetModuleHandleA(module) : nullptr;
        if (name == nullptr || (module != nullptr && module[0] != '\0' && handle == nullptr))
            return false;
        for (const char* type : {"PNG", "BMP", "JPG", "RCDATA"}) {
            HRSRC found = FindResourceA(handle, name, type);
            if (found == nullptr && std::strcmp(type, "BMP") == 0)
                found = FindResourceA(handle, name, MAKEINTRESOURCEA(2)); // RT_BITMAP: a DIB
            if (found == nullptr)
                continue;
            const HGLOBAL loaded = LoadResource(handle, found);
            const void* data = loaded != nullptr ? LockResource(loaded) : nullptr;
            return set_texture_data(primitive(c), data, SizeofResource(handle, found), 0);
        }
        return false;
    }
    static bool SetTextureFromResourceCache(void* c, const char* name)
    {
        const std::lock_guard lock(g_lock);
        return set_texture_file(primitive(c), cached_texture_path(name), 0);
    }
    static bool SetTextureFromTexture(void* c, sdk::IDirect3DTexture8* texture, uint32_t width, uint32_t height)
    {
        const std::lock_guard lock(g_lock);
        PrimitiveState& p = primitive(c);
        p.texture_path.clear();
        p.texture_data.clear();
        release_texture(p);
        if (texture == nullptr)
            return false;
        d3d8::call<ULONG>(texture, 1); // AddRef: a reference of its own
        p.texture = texture;
        p.texture_width = width;
        p.texture_height = height;
        return true;
    }
    static bool HitTest(void* c, int32_t x, int32_t y)
    {
        const std::lock_guard lock(g_lock);
        const PrimitiveState& p = primitive(c);
        return p.visible && x >= p.x && y >= p.y && x < p.x + p.width * p.scale_x && y < p.y + p.height * p.scale_y;
    }
    static const char* GetAlias(void* c) { return primitive(c).alias.c_str(); }
    static void SetAlias(void* c, const char* alias)
    {
        const std::lock_guard lock(g_lock);
        primitive(c).alias = alias != nullptr ? alias : "";
    }
    static RECT GetBorderSizes(void* c) { return primitive(c).border_sizes; }
    static void SetBorderSizes(void* c, RECT sizes) { primitive(c).border_sizes = sizes; }
    PROPERTY(float, TextureOffsetX, primitive, texture_x)
    PROPERTY(float, TextureOffsetY, primitive, texture_y)
    PROPERTY(bool, BorderVisible, primitive, border_visible)
    PROPERTY(D3DCOLOR, BorderColor, primitive, border_color)
    PROPERTY(FontBorderFlags, BorderFlags, primitive, border_flags)
    PROPERTY(bool, Visible, primitive, visible)
    PROPERTY(float, PositionX, primitive, x)
    PROPERTY(float, PositionY, primitive, y)
    PROPERTY(bool, CanFocus, primitive, can_focus)
    PROPERTY(bool, Locked, primitive, locked)
    PROPERTY(bool, LockedZ, primitive, locked_z)
    PROPERTY(float, ScaleX, primitive, scale_x)
    PROPERTY(float, ScaleY, primitive, scale_y)
    PROPERTY(float, Width, primitive, width)
    PROPERTY(float, Height, primitive, height)
    PROPERTY(PrimitiveDrawFlags, DrawFlags, primitive, draw_flags)
    PROPERTY(D3DCOLOR, Color, primitive, color)
    PROPERTY(fontkeyboardevent_f, KeyboardCallback, primitive, keyboard)
    PROPERTY(fontmouseevent_f, MouseCallback, primitive, mouse)
};

struct FontImpl {
    static void Render(void* c)
    {
        const std::lock_guard lock(g_lock);
        by_hand([&](void* d) { draw_font(d, font(c)); });
    }
    static void GetTextSize(void* c, SIZE* size)
    {
        if (size == nullptr)
            return;
        const std::lock_guard lock(g_lock);
        FontState& f = font(c);
        if (f.dirty)
            refresh(f);
        size->cx = f.sprite.width();
        size->cy = f.sprite.height();
    }
    static bool HitTest(void* c, int32_t x, int32_t y)
    {
        const std::lock_guard lock(g_lock);
        FontState& f = font(c);
        const POINT at = place(f);
        const SIZE box = extent(f);
        return f.visible && x >= at.x && y >= at.y && x < at.x + box.cx && y < at.y + box.cy;
    }
    static void* GetParent(void* c)
    {
        FontState* parent = font(c).parent;
        return parent != nullptr ? parent->object : nullptr;
    }
    static void* GetBackground(void* c)
    {
        const std::lock_guard lock(g_lock);
        FontState& f = font(c);
        if (f.background.object == nullptr)
            f.background.object = f.links->make_primitive(&f.background);
        return f.background.object;
    }
    static float GetRealPositionX(void* c)
    {
        const std::lock_guard lock(g_lock);
        return static_cast<float>(place(font(c)).x);
    }
    static float GetRealPositionY(void* c)
    {
        const std::lock_guard lock(g_lock);
        return static_cast<float>(place(font(c)).y);
    }
    static const char* GetAlias(void* c) { return font(c).alias.c_str(); }
    static void SetAlias(void* c, const char* alias)
    {
        const std::lock_guard lock(g_lock);
        font(c).alias = alias != nullptr ? alias : "";
    }
    static const char* GetFontFile(void* c) { return font(c).file.c_str(); }
    static void SetFontFile(void* c, const char* file)
    {
        const std::lock_guard lock(g_lock);
        font(c).file = file != nullptr ? file : "";
    }
    static const char* GetFontFamily(void* c) { return font(c).family.c_str(); }
    static void SetFontFamily(void* c, const char* family)
    {
        const std::lock_guard lock(g_lock);
        font(c).family = family != nullptr && family[0] != '\0' ? family : "Arial";
        font(c).dirty = true;
    }
    static const char* GetText(void* c) { return font(c).text.c_str(); }
    static void SetText(void* c, const char* text)
    {
        const std::lock_guard lock(g_lock);
        FontState& f = font(c);
        const std::string next = text != nullptr ? text : "";
        if (next != f.text) {
            f.text = next;
            f.dirty = true;
        }
    }
    static void SetParent(void* c, void* parent_object)
    {
        const std::lock_guard lock(g_lock);
        FontState& f = font(c);
        f.parent = nullptr;
        for (auto& other : g_fonts)
            if (other->object == parent_object && other.get() != &f)
                f.parent = other.get();
    }
    static bool GetRightJustified(void* c) { return font(c).right; }
    static void SetRightJustified(void* c, bool right)
    {
        const std::lock_guard lock(g_lock);
        font(c).right = right;
        font(c).dirty = true;
    }
    static FontDrawFlags GetDrawFlags(void* c) { return font(c).draw_flags; }
    static void SetDrawFlags(void* c, FontDrawFlags flags)
    {
        const std::lock_guard lock(g_lock);
        FontState& f = font(c);
        f.draw_flags = flags;
        f.right = has(flags, FontDrawFlags::RightJustified) || f.right;
        f.dirty = true;
    }
    static FontCreateFlags GetCreateFlags(void* c) { return font(c).create_flags; }
    static void SetCreateFlags(void* c, FontCreateFlags flags)
    {
        const std::lock_guard lock(g_lock);
        FontState& f = font(c);
        f.create_flags = flags;
        const auto bits = static_cast<uint32_t>(flags);
        f.bold = (bits & static_cast<uint32_t>(FontCreateFlags::Bold)) != 0;
        f.italic = (bits & static_cast<uint32_t>(FontCreateFlags::Italic)) != 0;
        f.dirty = true;
    }
    static bool GetIsDirty(void* c) { return font(c).dirty; }
    static void SetIsDirty(void* c, bool dirty) { font(c).dirty = dirty || font(c).dirty; }
    FONT_PROPERTY(bool, Visible, visible)
    FONT_PROPERTY(bool, CanFocus, can_focus)
    FONT_PROPERTY(bool, Locked, locked)
    FONT_PROPERTY(bool, LockedZ, locked_z)
    FONT_PROPERTY(float, WindowWidth, window_width)
    FONT_PROPERTY(float, WindowHeight, window_height)
    FONT_PROPERTY(uint32_t, FontHeight, height)
    FONT_PROPERTY(bool, Bold, bold)
    FONT_PROPERTY(bool, Italic, italic)
    FONT_PROPERTY(bool, StrikeThrough, strike)
    FONT_PROPERTY(bool, Underlined, underline)
    FONT_PROPERTY(D3DCOLOR, Color, color)
    FONT_PROPERTY(D3DCOLOR, ColorOutline, outline)
    FONT_PROPERTY(float, Padding, padding)
    FONT_PROPERTY(float, PositionX, x)
    FONT_PROPERTY(float, PositionY, y)
    FONT_PROPERTY(bool, AutoResize, auto_resize)
    FONT_PROPERTY(FrameAnchor, Anchor, anchor)
    FONT_PROPERTY(FrameAnchor, AnchorParent, anchor_parent)
    FONT_PROPERTY(fontkeyboardevent_f, KeyboardCallback, keyboard)
    FONT_PROPERTY(fontmouseevent_f, MouseCallback, mouse)
};

struct FontManagerImpl {
    static void* Create(void* c, const char* alias)
    {
        if (alias == nullptr || alias[0] == '\0')
            return nullptr;
        const std::lock_guard lock(g_lock);
        if (FontState* existing = find_font(alias))
            return existing->object;
        auto state = std::make_unique<FontState>();
        state->links = &links(c);
        state->alias = alias;
        state->owner = std::string(calling());
        state->background.links = state->links;
        state->background.alias = std::string(alias) + "_background";
        state->background.owner = state->owner;
        state->object = state->links->make_font(state.get());
        g_fonts.push_back(std::move(state));
        return g_fonts.back()->object;
    }
    static void* Get(void*, const char* alias)
    {
        const std::lock_guard lock(g_lock);
        FontState* f = find_font(alias);
        return f != nullptr ? f->object : nullptr;
    }
    static void Delete(void*, const char* alias)
    {
        const std::lock_guard lock(g_lock);
        for (auto it = g_fonts.begin(); it != g_fonts.end(); ++it) {
            if (alias != nullptr && same((*it)->alias, alias)) {
                FontState& f = **it;
                for (auto& other : g_fonts)
                    if (other->parent == &f)
                        other->parent = nullptr;
                f.sprite.release();
                free_primitive_object(f.background);
                f.links->free_font(f.object);
                g_fonts.erase(it);
                return;
            }
        }
    }
    static void* GetFocusedObject(void*) { return nullptr; }
    static bool SetFocusedObject(void*, const char*) { return false; }
    static bool GetVisible(void*) { return g_fonts_visible; }
    static void SetVisible(void*, bool visible) { g_fonts_visible = visible; }
};

struct PrimitiveManagerImpl {
    static void* Create(void* c, const char* alias)
    {
        if (alias == nullptr || alias[0] == '\0')
            return nullptr;
        const std::lock_guard lock(g_lock);
        if (PrimitiveState* existing = find_primitive(alias))
            return existing->object;
        auto state = std::make_unique<PrimitiveState>();
        state->links = &links(c);
        state->alias = alias;
        state->owner = std::string(calling());
        state->object = state->links->make_primitive(state.get());
        g_primitives.push_back(std::move(state));
        return g_primitives.back()->object;
    }
    static void* Get(void*, const char* alias)
    {
        const std::lock_guard lock(g_lock);
        PrimitiveState* p = find_primitive(alias);
        return p != nullptr ? p->object : nullptr;
    }
    static void Delete(void*, const char* alias)
    {
        const std::lock_guard lock(g_lock);
        for (auto it = g_primitives.begin(); it != g_primitives.end(); ++it) {
            if (alias != nullptr && same((*it)->alias, alias)) {
                free_primitive_object(**it);
                g_primitives.erase(it);
                return;
            }
        }
    }
    static void* GetFocusedObject(void*) { return nullptr; }
    static bool SetFocusedObject(void*, const char*) { return false; }
    static bool GetVisible(void*) { return g_primitives_visible; }
    static void SetVisible(void*, bool visible) { g_primitives_visible = visible; }
};

template <template <class> class Object, class Impl>
void* make_object(void* state)
{
    return make<Object, Impl>(state);
}

} // namespace

void release_objects(std::string_view owner)
{
    if (owner.empty())
        return;
    const std::lock_guard lock(g_lock);
    for (size_t i = 0; i < g_fonts.size();) {
        if (g_fonts[i]->owner == owner)
            FontManagerImpl::Delete(nullptr, g_fonts[i]->alias.c_str());
        else
            ++i;
    }
    for (size_t i = 0; i < g_primitives.size();) {
        if (g_primitives[i]->owner == owner)
            PrimitiveManagerImpl::Delete(nullptr, g_primitives[i]->alias.c_str());
        else
            ++i;
    }
}

namespace {

// The font being dragged: where the mouse and the font were when it started.
struct Drag {
    FontState* font = nullptr;
    int mouse_x = 0;
    int mouse_y = 0;
    float font_x = 0.0f;
    float font_y = 0.0f;
} g_drag;

bool exists(const FontState* font)
{
    for (const auto& f : g_fonts)
        if (f.get() == font)
            return true;
    return false;
}

// Each owner's objects are one window to Addons > Layout: where all of them are (their top-left corner and size, unmoved),
// told to it, and what it has set for them kept for drawing. A plugin's own drawing on the device (its minimap) is part of
// them (plugin_move: where it drew last frame), and moved with them. Every frame, so the tab always has them.
void update_groups()
{
    struct Box {
        float x0, y0, x1, y1;
    };
    std::map<std::string, Box> boxes;
    const auto add = [&](const std::string& owner, float x, float y, float width, float height) {
        if (owner.empty() || width <= 0.0f || height <= 0.0f)
            return;
        const auto [it, fresh] = boxes.try_emplace(owner, Box{x, y, x + width, y + height});
        if (!fresh) {
            it->second.x0 = std::min(it->second.x0, x);
            it->second.y0 = std::min(it->second.y0, y);
            it->second.x1 = std::max(it->second.x1, x + width);
            it->second.y1 = std::max(it->second.y1, y + height);
        }
    };
    if (g_primitives_visible)
        for (const auto& p : g_primitives)
            if (p->visible)
                add(p->owner, p->x, p->y, p->width * p->scale_x, p->height * p->scale_y);
    if (g_fonts_visible) {
        for (const auto& f : g_fonts) {
            if (!f->visible || !shows(*f))
                continue;
            const POINT at = place(*f);
            const SIZE box = extent(*f);
            add(f->owner, static_cast<float>(at.x), static_cast<float>(at.y), static_cast<float>(box.cx),
                static_cast<float>(box.cy));
        }
    }
    for (const auto& [owner, box] : plugin_move::boxes())
        add(owner, box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0);
    std::vector<layout::ObjectGroup> groups;
    std::vector<const std::string*> owners;
    for (const auto& [owner, box] : boxes) {
        const std::string key = layout_owner(owner);
        groups.push_back({"Text and shapes##objects/" + key, key, box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0});
        owners.push_back(&owner);
    }
    const std::vector<layout::ObjectMove> moves = layout::objects(groups);
    g_groups.clear();
    std::map<std::string, plugin_move::Move> plugin_moves;
    for (size_t i = 0; i < groups.size() && i < moves.size(); ++i) {
        g_groups[*owners[i]] = {groups[i].x, groups[i].y, moves[i].dx, moves[i].dy, moves[i].scale};
        plugin_moves[*owners[i]] = {groups[i].x, groups[i].y, moves[i].dx, moves[i].dy, moves[i].scale};
    }
    plugin_move::set_moves(std::move(plugin_moves));
}

} // namespace

bool drag_objects(int type, int x, int y)
{
    const std::lock_guard lock(g_lock);
    if (g_drag.font != nullptr && !exists(g_drag.font))
        g_drag.font = nullptr; // deleted meanwhile
    if (type == 0 && g_drag.font != nullptr) {
        // Its x counts from the right edge when anchored right; y from the bottom likewise.
        FontState& f = *g_drag.font;
        const auto anchor = static_cast<uint32_t>(f.anchor);
        const float scale = group_of(f.owner).scale; // (what's drawn at a scale moves by that much of what the font moves)
        const float dx = static_cast<float>(x - g_drag.mouse_x) / scale;
        const float dy = static_cast<float>(y - g_drag.mouse_y) / scale;
        f.x = g_drag.font_x + ((anchor & static_cast<uint32_t>(FrameAnchor::Right)) != 0 ? -dx : dx);
        f.y = g_drag.font_y + ((anchor & static_cast<uint32_t>(FrameAnchor::Bottom)) != 0 ? -dy : dy);
        return true;
    }
    if (type == 1 && (GetKeyState(VK_SHIFT) & 0x8000) != 0 && g_fonts_visible) {
        // The topmost (drawn last) under the mouse.
        for (auto it = g_fonts.rbegin(); it != g_fonts.rend(); ++it) {
            FontState& f = **it;
            if (!f.visible || !f.can_focus || f.locked || f.text.empty() || has(f.draw_flags, FontDrawFlags::ManualRender))
                continue;
            const Group g = group_of(f.owner);
            const POINT base = place(f);
            const SIZE box = extent(f);
            const Spot at = moved(g, static_cast<float>(base.x), static_cast<float>(base.y));
            if (x >= at.x && y >= at.y && x < at.x + box.cx * g.scale && y < at.y + box.cy * g.scale) {
                g_drag = {&f, x, y, f.x, f.y};
                return true;
            }
        }
        return false;
    }
    if (type == 2 && g_drag.font != nullptr) {
        g_drag.font = nullptr;
        return true;
    }
    return false;
}

void draw_objects(void* device)
{
    const std::lock_guard lock(g_lock);
    // A new device: textures made on the old one are gone.
    static void* drawn_on = nullptr;
    if (device != drawn_on) {
        for (auto& f : g_fonts)
            f->sprite.release();
        // primitives' textures made again on it (a plugin's own texture went with the old one)
        const auto again = [](PrimitiveState& p) {
            if (p.texture != nullptr && !load_primitive_texture(p))
                release_texture(p);
        };
        for (auto& p : g_primitives)
            again(*p);
        for (auto& f : g_fonts)
            again(f->background);
        drawn_on = device;
    }
    update_groups();
    if (g_primitives_visible)
        for (auto& p : g_primitives)
            if (p->visible && (static_cast<uint32_t>(p->draw_flags) & static_cast<uint32_t>(PrimitiveDrawFlags::ManualRender)) == 0)
                draw_primitive_moved(device, *p);
    if (g_fonts_visible)
        for (auto& f : g_fonts)
            if (f->visible && !has(f->draw_flags, FontDrawFlags::ManualRender))
                draw_font(device, *f);
}

void build_fonts(Links& a, Links& b)
{
    namespace v416 = sdk::v416;
    namespace v430 = sdk::v430;
    a.fonts = make<v416::FontManagerObject, FontManagerImpl>(&a);
    b.fonts = make<v430::FontManagerObject, FontManagerImpl>(&b);
    a.primitives = make<v416::PrimitiveManagerObject, PrimitiveManagerImpl>(&a);
    b.primitives = make<v430::PrimitiveManagerObject, PrimitiveManagerImpl>(&b);
    a.make_font = &make_object<v416::FontObjectObject, FontImpl>;
    b.make_font = &make_object<v430::FontObjectObject, FontImpl>;
    a.free_font = &destroy<v416::FontObjectObject, FontImpl>;
    b.free_font = &destroy<v430::FontObjectObject, FontImpl>;
    a.make_primitive = &make_object<v416::PrimitiveObjectObject, PrimitiveImpl>;
    b.make_primitive = &make_object<v430::PrimitiveObjectObject, PrimitiveImpl>;
    a.free_primitive = &destroy<v416::PrimitiveObjectObject, PrimitiveImpl>;
    b.free_primitive = &destroy<v430::PrimitiveObjectObject, PrimitiveImpl>;
}

} // namespace vanacore::ashita
