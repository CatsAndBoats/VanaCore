#include "pluginapi.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <vanacore/plugin_api.h>
#include <vanacore/version.hpp>

#include "chat.hpp"
#include "compat/ashita/host.hpp"
#include "entityhp.hpp"
#include "game.hpp"
#include "gametext.hpp"
#include "gameui.hpp"
#include "gamewin.hpp"
#include "gui.hpp"
#include "log.hpp"
#include "maxhp_ask.hpp"
#include "ui.hpp"
#include "uispace.hpp"

// Built 2026-11-06 for FancyChat's move to a plugin (Arielfy: "everything you made for FC specifically, like rendering the
// input bar off screen and the game windows over FC, doesn't affect the plugin version"). Not tried in game.

namespace vanacore::pluginapi {
namespace {

// A window read for a plugin (gameui_window): the strings and arrays its VanaCoreWindow points at, kept until the plugin
// asks for the same window again, or goes
struct WindowCopy {
    std::string asked;       // the name it was asked by
    std::uint64_t shown = 0; // what the last read found, as one number
    std::uint32_t changes = 0;
    std::vector<std::string> texts;
    std::vector<VanaCoreWindowRow> rows;
    std::vector<VanaCoreWindowLine> lines;
    std::vector<VanaCoreWindowPicture> pictures;
    VanaCoreWindow out{};
};

// A plugin that has asked for something since it loaded
struct Asker {
    std::string display; // as its file spells it ("FancyChat")
    bool on_top = false; // game_on_top: over its ImGui windows
    std::uint32_t on_top_flags = 0;
    std::vector<RECT> areas; // game_on_top_areas
    std::uint32_t area_flags = 0;
    bool caret = false; // chat_input_caret
    float caret_x = 0, caret_top = 0, caret_bottom = 0;
    std::vector<std::string> hidden; // what it has hidden, for the log (a plugin may ask every frame: said once)
    std::vector<std::unique_ptr<WindowCopy>> windows; // gameui_window's, by the name asked
};
std::vector<Asker> g_askers;

std::string gameui_key(const std::string& display)
{
    return gameui::key("plugin", display);
}

// The plugin calling now (on the game's thread, from one of its callbacks), its gameui entry made at its first call;
// nullptr for anything else (an addon, another thread)
Asker* caller()
{
    const std::string_view calling = ashita::calling();
    if (calling.empty() || calling.rfind("addon ", 0) == 0)
        return nullptr;
    for (Asker& asker : g_askers)
        if (asker.display == calling)
            return &asker;
    Asker asker;
    asker.display = std::string(calling);
    gameui::attach(gameui_key(asker.display), asker.display);
    log::write(L"Plugin API: %hs uses VanaCore's plugin API", asker.display.c_str());
    g_askers.push_back(std::move(asker));
    return &g_askers.back();
}

int code_of(gameui::Result result)
{
    switch (result) {
    case gameui::Result::Done:
        return VANACORE_OK;
    case gameui::Result::Held:
        return VANACORE_HELD;
    case gameui::Result::Unknown:
        return VANACORE_UNKNOWN;
    case gameui::Result::Unloaded:
        return VANACORE_NOT_A_PLUGIN;
    default:
        return VANACORE_BAD_ARGUMENT;
    }
}

// `text` into `out` (`size` bytes, NUL included, cut to fit); its whole length
std::uint32_t copy_out(const std::string& text, char* out, std::uint32_t size)
{
    if (out != nullptr && size > 0) {
        const std::size_t n = std::min<std::size_t>(text.size(), size - 1);
        std::memcpy(out, text.data(), n);
        out[n] = '\0';
    }
    return static_cast<std::uint32_t>(text.size());
}

std::wstring wide(const char* utf8)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring out(n > 1 ? static_cast<std::size_t>(n) : 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), n);
    out.resize(out.size() - 1);
    return out;
}

// A hide or show done, in the log when it changes what the plugin has hidden
void note_hide(Asker& asker, const char* element, bool hidden)
{
    const auto at = std::find(asker.hidden.begin(), asker.hidden.end(), element);
    if (hidden == (at != asker.hidden.end()))
        return;
    if (hidden)
        asker.hidden.emplace_back(element);
    else
        asker.hidden.erase(at);
    log::write(L"Plugin API: %hs %hs %hs", asker.display.c_str(), hidden ? "hides" : "shows", element);
}

// ---- gameui_window's copy ----

// The game's pixels (the back buffer's, as plugins draw) and whether the interface's units can be put in them
struct Pixels {
    unsigned width = 0, height = 0;
    bool known = false;
};

Pixels pixels()
{
    Pixels p;
    ui::game_screen(p.width, p.height);
    p.known = p.width > 0 && p.height > 0 && uispace::find(GetModuleHandleW(L"FFXiMain.dll"));
    return p;
}

// `rect` (the interface's units) as the game's pixels, x, y, w, h; the pixels a unit (1 while that isn't known)
float place(const Pixels& p, const gamewin::Rect& rect, float& x, float& y, float& w, float& h)
{
    float x0 = static_cast<float>(rect.left), y0 = static_cast<float>(rect.top);
    float x1 = static_cast<float>(rect.right), y1 = static_cast<float>(rect.bottom);
    float scale = 1.0f, px = 0, py = 0, qx = 0, qy = 0;
    if (p.known && uispace::to_pixels(x0, y0, p.width, p.height, px, py, scale) &&
        uispace::to_pixels(x1, y1, p.width, p.height, qx, qy, scale))
        x0 = px, y0 = py, x1 = qx, y1 = qy;
    else
        scale = 1.0f;
    x = x0, y = y0, w = x1 - x0, h = y1 - y0;
    return scale;
}

// The colour the game draws `colour` (0xAARRGGBB) as: 0x80 is full, drawn twice as bright
std::uint32_t drawn_argb(std::uint32_t colour)
{
    std::uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8)
        out |= std::min(0xFFu, ((colour >> shift) & 0xFFu) * 2u) << shift;
    return out;
}

// The game's text (Shift-JIS with its own row for ñ, é, ¿: gametext.hpp; ASCII as it is) as UTF-8
std::string utf8_of(const std::string& sjis)
{
    if (std::all_of(sjis.begin(), sjis.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; }))
        return sjis;
    return gametext::to_utf8(sjis);
}

std::uint64_t mix(std::uint64_t hash, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i)
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

std::uint64_t mix(std::uint64_t hash, const gamewin::Rect& rect, const std::string& text, std::uint32_t number)
{
    return mix(mix(mix(hash, &rect, sizeof rect), text.data(), text.size()), &number, sizeof number);
}

// What a read of the window found, as one number: its place on the screen, cursor, rows, text and pictures
std::uint64_t shown_of(const gamewin::Window& w, const Pixels& p)
{
    const int head[] = {static_cast<int>(p.width), static_cast<int>(p.height), w.cursor, w.count, w.focused, w.closing,
                        w.hidden, w.drawn};
    std::uint64_t h = mix(mix(1469598103934665603ull, head, sizeof head), w.rect, w.name, 0);
    for (const gamewin::Row& r : w.rows) {
        const int flags = r.id << 8 | (r.cursor ? 1 : 0) | (r.disabled ? 2 : 0) | (r.drawn ? 4 : 0);
        h = mix(h, r.rect, r.text, static_cast<std::uint32_t>(r.state) ^ static_cast<std::uint32_t>(flags) << 4);
    }
    for (const gamewin::Line& l : w.lines)
        h = mix(h, l.rect, l.text, l.colour);
    for (const gamewin::Picture& pic : w.pictures)
        h = mix(h, pic.rect, pic.name, pic.colour);
    return h;
}

// The window's strings into the copy, all of them before anything points at one (the vector doesn't move after)
void keep_texts(WindowCopy& c, const gamewin::Window& w)
{
    c.texts.clear();
    c.texts.reserve(1 + w.rows.size() + w.lines.size() + w.pictures.size());
    c.texts.push_back(w.name);
    for (const gamewin::Row& r : w.rows)
        c.texts.push_back(utf8_of(r.text));
    for (const gamewin::Line& l : w.lines)
        c.texts.push_back(utf8_of(l.text));
    for (const gamewin::Picture& pic : w.pictures)
        c.texts.push_back(pic.name);
}

void fill_rows(WindowCopy& c, const gamewin::Window& w, const Pixels& p)
{
    c.rows.clear();
    std::size_t text = 1;
    for (const gamewin::Row& r : w.rows) {
        VanaCoreWindowRow row{};
        row.id = r.id;
        place(p, r.rect, row.x, row.y, row.w, row.h);
        row.text = c.texts[text++].c_str();
        row.state = r.state;
        row.cursor = r.cursor ? 1 : 0;
        row.disabled = r.disabled ? 1 : 0;
        row.drawn = r.drawn ? 1 : 0;
        c.rows.push_back(row);
    }
}

void fill_drawn(WindowCopy& c, const gamewin::Window& w, const Pixels& p)
{
    c.lines.clear();
    c.pictures.clear();
    std::size_t text = 1 + w.rows.size();
    for (const gamewin::Line& l : w.lines) {
        VanaCoreWindowLine line{};
        line.text = c.texts[text++].c_str();
        place(p, l.rect, line.x, line.y, line.w, line.h);
        line.argb = drawn_argb(l.colour);
        c.lines.push_back(line);
    }
    for (const gamewin::Picture& pic : w.pictures) {
        VanaCoreWindowPicture picture{};
        picture.name = c.texts[text++].c_str();
        place(p, pic.rect, picture.x, picture.y, picture.w, picture.h);
        picture.argb = drawn_argb(pic.colour);
        c.pictures.push_back(picture);
    }
}

// The copy made again from a read of the window
void fill(WindowCopy& c, const gamewin::Window& w)
{
    const Pixels p = pixels();
    const std::uint64_t shown = shown_of(w, p);
    if (shown != c.shown || c.changes == 0)
        c.shown = shown, ++c.changes;
    keep_texts(c, w);
    fill_rows(c, w, p);
    fill_drawn(c, w, p);
    VanaCoreWindow& out = c.out;
    out = VanaCoreWindow{};
    out.size = sizeof(VanaCoreWindow);
    out.name = c.texts[0].c_str();
    out.scale = place(p, w.rect, out.x, out.y, out.w, out.h);
    out.cursor = w.cursor, out.count = w.count;
    out.focused = w.focused ? 1 : 0, out.closing = w.closing ? 1 : 0;
    out.hidden = w.hidden ? 1 : 0, out.drawn = w.drawn ? 1 : 0;
    out.changes = c.changes;
    out.rows = c.rows.data(), out.row_count = static_cast<std::uint32_t>(c.rows.size());
    out.lines = c.lines.data(), out.line_count = static_cast<std::uint32_t>(c.lines.size());
    out.pictures = c.pictures.data(), out.picture_count = static_cast<std::uint32_t>(c.pictures.size());
}

WindowCopy& copy_for(Asker& asker, const char* name)
{
    for (const std::unique_ptr<WindowCopy>& c : asker.windows)
        if (c->asked == name)
            return *c;
    asker.windows.push_back(std::make_unique<WindowCopy>());
    asker.windows.back()->asked = name;
    return *asker.windows.back();
}

// ---- the table's calls ----

int __cdecl gameui_hide(const char* element, int hidden)
{
    Asker* asker = caller();
    if (asker == nullptr)
        return VANACORE_NOT_A_PLUGIN;
    if (element == nullptr)
        return VANACORE_BAD_ARGUMENT;
    const int code = code_of(gameui::hide(gameui_key(asker->display), element, hidden != 0));
    if (code == VANACORE_OK)
        note_hide(*asker, element, hidden != 0);
    return code;
}

int __cdecl gameui_move(const char* element, double x, double y, char* holder, std::uint32_t holder_size)
{
    const Asker* asker = caller();
    if (asker == nullptr)
        return VANACORE_NOT_A_PLUGIN;
    if (element == nullptr)
        return VANACORE_BAD_ARGUMENT;
    std::string by;
    const int code = code_of(gameui::move(gameui_key(asker->display), element, x, y, by));
    copy_out(by, holder, holder_size);
    return code;
}

int __cdecl gameui_unmove(const char* element)
{
    const Asker* asker = caller();
    if (asker == nullptr)
        return VANACORE_NOT_A_PLUGIN;
    return element == nullptr ? VANACORE_BAD_ARGUMENT : code_of(gameui::unmove(gameui_key(asker->display), element));
}

int __cdecl gameui_image(const char* element, const VanaCoreImage* image, char* holder, std::uint32_t holder_size)
{
    const Asker* asker = caller();
    if (asker == nullptr)
        return VANACORE_NOT_A_PLUGIN;
    if (element == nullptr || image == nullptr || image->file == nullptr || *image->file == '\0')
        return VANACORE_BAD_ARGUMENT;
    gameui::Picture picture;
    picture.file = wide(image->file);
    picture.width = std::clamp(image->width, 0.0f, 4096.0f);
    picture.height = std::clamp(image->height, 0.0f, 4096.0f);
    picture.x = std::clamp(image->x, -4096.0f, 4096.0f);
    picture.y = std::clamp(image->y, -4096.0f, 4096.0f);
    picture.argb = image->argb;
    std::string by;
    const int code = code_of(gameui::image(gameui_key(asker->display), element, picture, by));
    copy_out(by, holder, holder_size);
    return code;
}

int __cdecl gameui_unimage(const char* element)
{
    const Asker* asker = caller();
    if (asker == nullptr)
        return VANACORE_NOT_A_PLUGIN;
    return element == nullptr ? VANACORE_BAD_ARGUMENT : code_of(gameui::unimage(gameui_key(asker->display), element));
}

std::uint32_t __cdecl gameui_hidden_by(const char* element, char* out, std::uint32_t size)
{
    if (element == nullptr || caller() == nullptr)
        return copy_out({}, out, size);
    return copy_out(gameui::hidden_by(element), out, size);
}

int __cdecl game_on_top(int on, std::uint32_t flags)
{
    Asker* asker = caller();
    if (asker == nullptr)
        return VANACORE_NOT_A_PLUGIN;
    if (asker->on_top != (on != 0))
        log::write(on != 0 ? L"Plugin API: %hs asks for the game's windows over its own ([client] menus.ontop)"
                           : L"Plugin API: %hs no longer asks for the game's windows over its own",
                   asker->display.c_str());
    asker->on_top = on != 0;
    asker->on_top_flags = flags;
    return VANACORE_OK;
}

int __cdecl game_on_top_areas(const VanaCoreArea* areas, std::uint32_t count, std::uint32_t flags)
{
    Asker* asker = caller();
    if (asker == nullptr)
        return VANACORE_NOT_A_PLUGIN;
    if (count > VANACORE_ON_TOP_AREAS_MOST || (count > 0 && areas == nullptr))
        return VANACORE_BAD_ARGUMENT;
    asker->areas.clear();
    for (std::uint32_t i = 0; i < count; ++i)
        if (areas[i].right > areas[i].left && areas[i].bottom > areas[i].top)
            asker->areas.push_back({areas[i].left, areas[i].top, areas[i].right, areas[i].bottom});
    asker->area_flags = flags;
    return VANACORE_OK;
}

int __cdecl game_on_top_active()
{
    return chat::raising() ? 1 : 0;
}

int __cdecl game_window_at(std::int32_t x, std::int32_t y)
{
    return gui::game_window_at(static_cast<float>(x), static_cast<float>(y)) ? 1 : 0;
}

int __cdecl chat_input_caret(int shown, float x, float top, float bottom)
{
    Asker* asker = caller();
    if (asker == nullptr)
        return VANACORE_NOT_A_PLUGIN;
    if (shown != 0 && !(bottom >= top))
        return VANACORE_BAD_ARGUMENT;
    asker->caret = shown != 0;
    asker->caret_x = x, asker->caret_top = top, asker->caret_bottom = bottom;
    return VANACORE_OK;
}

const VanaCoreWindow* __cdecl gameui_window(const char* name)
{
    Asker* asker = caller();
    if (asker == nullptr || name == nullptr || *name == '\0')
        return nullptr;
    gamewin::Window w;
    if (!gamewin::read(name, w))
        return nullptr;
    WindowCopy& copy = copy_for(*asker, name);
    fill(copy, w);
    return &copy.out;
}

std::uint32_t __cdecl gameui_windows(char* out, std::uint32_t size)
{
    std::string list;
    if (caller() != nullptr)
        for (const std::string& name : gamewin::names())
            list += (list.empty() ? "" : ",") + name;
    return copy_out(list, out, size);
}

int __cdecl gameui_press(const char* name, std::int32_t row, std::int32_t event)
{
    if (caller() == nullptr)
        return VANACORE_NOT_A_PLUGIN;
    if (name == nullptr || row < 0 || event < 0)
        return VANACORE_BAD_ARGUMENT;
    switch (gamewin::press(name, row, event)) {
    case gamewin::Press::Done:
        return VANACORE_OK;
    case gamewin::Press::NotUp:
        return VANACORE_NOT_UP;
    case gamewin::Press::NoRow:
        return VANACORE_BAD_ARGUMENT;
    case gamewin::Press::Busy:
        return VANACORE_BUSY;
    default:
        return VANACORE_UNAVAILABLE;
    }
}

VanaCoreMax max_of(const entityhp::Max& max)
{
    return VanaCoreMax{static_cast<std::uint32_t>(max.source), max.value, max.low, max.high};
}

VanaCoreVitals vitals_of(const entityhp::Vitals& vitals)
{
    VanaCoreVitals out{};
    out.size = sizeof(out);
    out.id = vitals.id;
    out.party_slot = vitals.party_slot;
    out.hp = vitals.hp, out.mp = vitals.mp, out.tp = vitals.tp;
    out.hpp = vitals.hpp, out.mpp = vitals.mpp;
    out.hp_max = max_of(vitals.hp_max);
    out.mp_max = max_of(vitals.mp_max);
    return out;
}

// What the plugin asked for in the struct it passed (`out`, whose `size` is its header's): `whole` as far as both know it,
// with the size set to what was filled
template <typename T>
int give(T* out, T whole)
{
    if (out == nullptr || out->size < sizeof(std::uint32_t))
        return VANACORE_BAD_ARGUMENT;
    whole.size = std::min<std::uint32_t>(out->size, sizeof(T));
    std::memcpy(out, &whole, whole.size);
    return VANACORE_OK;
}

// The entity's record, flat, with its health from the party where it has it
VanaCoreEntity entity_of(const game::Entity& e)
{
    VanaCoreEntity out{};
    out.index = e.index, out.id = e.id;
    const std::string name = gametext::to_utf8(e.name);
    std::memcpy(out.name, name.data(), std::min(name.size(), sizeof(out.name) - 1)); // (out.name is zeroed)
    out.x = e.x, out.y = e.y, out.z = e.z, out.heading = e.heading, out.distance = e.distance;
    out.speed = e.speed, out.anim_speed = e.anim_speed, out.model_size = e.model_size, out.hitbox_size = e.hitbox_size;
    out.hpp = e.hpp, out.type = e.type, out.race = e.race;
    out.status = e.status, out.status_server = e.status_server, out.status_event = e.status_event;
    out.spawn_flags = e.spawn_flags, out.claim = e.claim, out.target_index = e.target_index;
    out.pet_index = e.pet_index, out.fellow_index = e.fellow_index, out.trust_owner_index = e.trust_owner_index;
    out.interaction_target_index = e.interaction_target_index;
    std::copy(std::begin(e.render), std::end(e.render), out.render);
    out.name_color = e.name_color, out.linkshell_color = e.linkshell_color, out.costume_id = e.costume_id;
    out.mount_id = e.mount_id, out.campaign_name_flag = e.campaign_name_flag, out.ballista_flags = e.ballista_flags;
    std::copy(std::begin(e.models), std::end(e.models), out.models);
    std::copy(std::begin(e.animations), std::end(e.animations), out.animations);
    out.animation_time = e.animation_time, out.animation_step = e.animation_step, out.animation_play = e.animation_play;
    out.emote_target_index = e.emote_target_index, out.emote_id = e.emote_id, out.last_action_id = e.last_action_id;
    out.zone_id = e.zone_id, out.door_id = e.door_id;
    out.valid_target = (e.render_flags & 0x200) != 0 ? 1 : 0;
    entityhp::Vitals vitals;
    out.has_vitals = entityhp::vitals(e.id, vitals) ? 1 : 0;
    out.vitals = vitals_of(out.has_vitals != 0 ? vitals : entityhp::Vitals{});
    if (out.has_vitals == 0)
        out.vitals.hp_max = max_of(maxhp::known(e.id)); // (unless the server was asked: maxhp_ask.hpp)
    return out;
}

int __cdecl entity(std::uint32_t index, VanaCoreEntity* out)
{
    if (caller() == nullptr)
        return VANACORE_NOT_A_PLUGIN;
    if (out == nullptr || out->size < sizeof(std::uint32_t))
        return VANACORE_BAD_ARGUMENT;
    game::Entity found;
    return game::entity(index, found) ? give(out, entity_of(found)) : VANACORE_UNKNOWN;
}

int __cdecl entity_by_id(std::uint32_t id, VanaCoreEntity* out)
{
    if (caller() == nullptr)
        return VANACORE_NOT_A_PLUGIN;
    if (out == nullptr || out->size < sizeof(std::uint32_t))
        return VANACORE_BAD_ARGUMENT;
    game::Entity found;
    const unsigned index = game::entity_index_by_id(id);
    return index != 0 && game::entity(index, found) && found.id == id ? give(out, entity_of(found)) : VANACORE_UNKNOWN;
}

int __cdecl entity_vitals(std::uint32_t id, VanaCoreVitals* out)
{
    if (caller() == nullptr)
        return VANACORE_NOT_A_PLUGIN;
    if (out == nullptr || out->size < sizeof(std::uint32_t))
        return VANACORE_BAD_ARGUMENT;
    entityhp::Vitals vitals;
    return entityhp::vitals(id, vitals) ? give(out, vitals_of(vitals)) : VANACORE_UNKNOWN;
}

int __cdecl entity_request_max_hp(const std::uint32_t* ids, std::uint32_t count, VanaCoreMaxHpCallback callback, void* user)
{
    const Asker* asker = caller();
    if (asker == nullptr)
        return VANACORE_NOT_A_PLUGIN;
    if (ids == nullptr || count == 0 || count > maxhp::kMaxIds || callback == nullptr)
        return VANACORE_BAD_ARGUMENT;
    const char* refused = maxhp::ask(
        ids, count,
        [callback, user](unsigned, const maxhp::Answer* answers, unsigned total) {
            VanaCoreMaxHpAnswer out[maxhp::kMaxIds] = {};
            for (unsigned i = 0; i < total; ++i)
                out[i] = {answers[i].id, answers[i].hp_max, static_cast<std::uint32_t>(answers[i].status), answers[i].hp};
            callback(user, out, total);
        },
        asker->display);
    return refused == nullptr ? VANACORE_OK : VANACORE_UNAVAILABLE;
}

const VanaCorePluginApi& table()
{
    static const std::string version = [] {
        std::string out;
        for (const wchar_t* c = kVersion; *c != L'\0'; ++c)
            out += static_cast<char>(*c);
        return out;
    }();
    static const VanaCorePluginApi api = {
        sizeof(VanaCorePluginApi), VANACORE_PLUGIN_API_VERSION, version.c_str(),
        &gameui_hide, &gameui_move, &gameui_unmove, &gameui_image, &gameui_unimage, &gameui_hidden_by,
        &game_on_top, &game_on_top_areas, &game_on_top_active, &game_window_at,
        &chat_input_caret,
        &gameui_window, &gameui_windows, &gameui_press,
        &entity, &entity_by_id, &entity_vitals, &entity_request_max_hp,
    };
    return api;
}

} // namespace

void release(const std::string& display)
{
    gameui::release(gameui_key(display)); // (logs when it had changed something)
    maxhp::release(display);              // (its max HP requests still waiting)
    const auto gone = std::remove_if(g_askers.begin(), g_askers.end(), [&](const Asker& a) { return a.display == display; });
    if (gone != g_askers.end())
        log::write(L"Plugin API: %hs went: its asks are dropped", display.c_str());
    g_askers.erase(gone, g_askers.end());
}

void over(std::vector<RECT>& chat_kept, std::vector<RECT>& others)
{
    for (const Asker& asker : g_askers) {
        if (asker.on_top)
            gui::shown_windows_of({"plugin/" + asker.display},
                                  (asker.on_top_flags & VANACORE_ON_TOP_NOT_CHAT_LOG) != 0 ? chat_kept : others);
        std::vector<RECT>& areas = (asker.area_flags & VANACORE_ON_TOP_NOT_CHAT_LOG) != 0 ? chat_kept : others;
        areas.insert(areas.end(), asker.areas.begin(), asker.areas.end());
    }
}

bool caret(float& x, float& top, float& bottom)
{
    for (const Asker& asker : g_askers) {
        if (!asker.caret)
            continue;
        x = asker.caret_x, top = asker.caret_top, bottom = asker.caret_bottom;
        return true;
    }
    return false;
}

} // namespace vanacore::pluginapi

// VanaCore.Core.def: the plugin API's table for a plugin built for `version` (vanacore_plugin_api), or null for a newer
// one than VanaCore has. Any thread (the calls in it are the game thread's).
extern "C" const VanaCorePluginApi* __cdecl VanaCoreGetPluginApi(std::uint32_t version)
{
    if (version == 0 || version > VANACORE_PLUGIN_API_VERSION)
        return nullptr;
    return &vanacore::pluginapi::table();
}
