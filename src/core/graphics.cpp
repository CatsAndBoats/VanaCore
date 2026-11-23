#include "graphics.hpp"

#include <windows.h>
#include <devpropdef.h>
#include <dxgi.h>
#include <setupapi.h>
#include <shellapi.h>

#include <mutex>
#include <thread>
#include <windowsx.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <string>
#include <unordered_map>
#include <vector>

#include <vanacore/boot_info.hpp>

#include "../gfx/vanacore_gfx.h"
#include "bumpmap.hpp"
#include "camera.hpp"
#include "chat.hpp"
#include "compat/windower/objects.hpp"
#include "d3d9.hpp"
#include "damagetext.hpp"
#include "casters.hpp"
#include "zonebatch.hpp"
#include "dats.hpp"
#include "distance.hpp"
#include "framerate.hpp"
#include "game.hpp"
#include "gmicons.hpp"
#include "grass_assets.hpp"
#include "grass_floors.hpp"
#include "imports.hpp"
#include "zone_casters.hpp"
#include "ini.hpp"
#include "log.hpp"
#include "lod.hpp"
#include "nameplates.hpp"
#include "others.hpp"
#include "packets.hpp"
#include "registry.hpp"
#include "scan.hpp"
#include "settings.hpp"
#include "shutdown.hpp"
#include "trusts.hpp"
#include "ui.hpp"
#include "vanaclock.hpp"
#include "water.hpp"
#include "widgets.hpp"
#include "window.hpp"
#include "zoneweather.hpp"
#include "zonefloor.hpp"
#include "zonelines.hpp"
#include "sparkles.hpp"

#pragma comment(lib, "setupapi.lib") // the graphics card's driver (find_driver)

namespace vanacore::graphics {
namespace {

// FFXiMain.dll's camera object keeps the 3D projection's shape at +0x2F0, as h / (w * 0.75) (1 for
// 4:3); the game sets it from the configured resolution, so any other window shape stretches the
// scene. The code that sets it names the global the object hangs from (Ashita's aspect addon finds
// it the same way): mov eax,[global]; test eax,eax; je; fld [esp+4]; fmul [0.25]; fmul [3.0]
constexpr char kAspectSetter[] = "A1 ?? ?? ?? ?? 85 C0 74 ?? D9 44 24 04 D8 0D ?? ?? ?? ?? D8 0D";
constexpr std::uintptr_t kAspectField = 0x2F0;

// [graphics.effects]: upstream's scene-effect settings, by the same keys (xi-on-mac fx.txt: the effects,
// then the lighting and the sun's shadows), then the cel look's and depth of field's (VanaCore's own)
constexpr const char* kEffectKeys[] = {"ao",      "radius",      "grade",      "sat",     "contrast", "brightness", "shadows", "highlights", "warmth", "sharpen",
                                       "filter",  "aniso",       "fog",        "fog_falloff", "fog_height", "fog_floor", "fog_max",
                                       "fog_sun", "fog_g",
                                       "bloom",   "threshold",   "rays",       "rays_decay",  "rays_length", "debug",
                                       "light",   "sun",   "sun_distance", "sun_near", "sun_mid",
                                       "sun_soft", "sun_soft_chars", "sun_soft_zone", "sun_curved", "sun_face", "sun_min",    "sun_direct", "sun_dusk", "sun_moon", "sun_moon_strength", "sun_moon_casters", "sun_free", "sun_free_az", "sun_free_el", "sun_tilt", "sun_game_shading", "sun_centre", "sun_gate", "sun_seek", "sun_zone_mesh", "sun_cache", "sun_late", "sun_sharp", "sun_cull", "sun_casters", "moghouse", "temporal",
                                       "dlss_compare", "dlss_preset", "dlss5_preset", "dlss5_style", "dlss5_intensity", "dlss5_tone", "dlss5_structure", "dlss5_skin", "dlss5_mask",
                                       "dlss5_fx_out", "dlss5_fx_light", "dlss5_fx_gain", "dlss5_fx_actor", "dlss5_fx_cap", "dlss5_fx_norm", "dlss5_elight_post", "dlss_motion_all", "fg_fx_motion", "dlss_motion_plants", "dlss_motion_chars", "dlss_motion_batch", "dlss_reactive", "dlss_reactive_moving",
                                       "dlss5_passes", "dlss5_taper", "dlss5_mix", "dlss5_colour", "dlss5_brighten", "dlss5_rebuild", "dlss5_scale",
                                       "outline", "outline_width", "outline_depth", "outline_crease", "outline_fade", "outline_nograss", "outline_width_chars",
                                       "outline_grass", "outline_width_grass", "outline_depth_grass", "outline_fade_grass",
                                       "bands",  "band_soft",   "smooth",
                                       "dof",     "dof_focus",   "dof_player", "dof_range",  "dof_far",  "dof_near", "dof_sky",
                                       "refl",    "refl_wet",    "refl_rough", "refl_dist", "refl_quality", "refl_sun", "refl_floors", "refl_bump", "refl_lift", "gi", "gi_radius", "gi_quality",
                                       "midtones", "whites", "rolloff", "light_tint", "hue",
                                       "clarity", "elight", "elight_radius", "elight_warp", "elight_lantern", "elight_dim", "elight_actor",
                                       "ao_quality", "ao_rings", "ao_ring_spacing", "ao_chars", "ao_soft_chars", "sun_rings", "sun_ring_spacing", "sun_detail", "sun_far_hold", "sun_near_hold", "sun_roll", "sun_plants", "sun_plants_still", "sun_fade", "normalmap", "normalmap_strength", "normalmap_view", "normalmap_refl", "normalmap_fx",
                                       "moon_size", "moon_brightness", "night", "night_moon", "night_bright", "night_bright_ambient", "day_bright", "indoor_bright", "ambient_force", "day_night_warmth", "player_light", "player_light_radius", "player_light_warm", "player_light_day", "textbatch", "batch"};

std::wstring g_ini;
std::wstring g_dir;          // VanaCore.exe's folder: relative paths start here
bool g_d3d12 = false;        // renderer=d3d12
bool g_d3d11 = false;        // renderer=d3d11: VanaCore's Direct3D 8 on Direct3D 11
bool g_d3d11_for_12 = false; // renderer=d3d12, drawn by Direct3D 11 (g_d3d11, not g_d3d12): 12 can't draw here
bool g_d3d11_kept = false;   // ... and vanacore.ini says renderer=d3d11 from now on (d3d12.unsupported=1 with it)
bool g_d3d12_unsupported = false; // [graphics] d3d12.unsupported, as the device came up: Direct3D 12 can't draw here
bool g_d3d9 = false;         // renderer=d3d9
bool g_d3d9_active = false;  // ... and the game has our Direct3D 8 on Direct3D 9
bool g_active = false;       // ... and the game has our Direct3D 8
bool g_gpu_data = false;     // [logging] gpu: a lost graphics card's log says where it stopped (Direct3D 12)
int g_dlss = -1;             // [graphics] dlss (or fsr): an NVSDK_NGX_PerfQuality_Value, or -1 off
float g_dlss_scale = 0.0f;   // ... the share of the drawn size the scene is drawn at for it, each way
int g_upscaler = VANA_GFX_UPSCALER_DLSS; // ... which upscaler: DLSS ([graphics] dlss) or AMD FSR ([graphics] fsr)
int g_framegen = 0;          // [graphics] dlss.framegen (or fsr.framegen): frames made between two, 0 off
int g_framegen_kind = VANA_GFX_FG_DLSS; // ... whose: NVIDIA's or AMD's
// ... kept off for the rest of the session: the graphics card was lost twice while it ran (recover_if_lost)
bool g_framegen_held = false;
// ... held off till a character is first in the world: started at the title screen, it could stay stuck with the game
// waiting on its present thread (about 20 FPS) where started in the world it runs at the frame rate
bool g_framegen_in_world = false;
// [graphics] dlss5: DLSS 5 (neural rendering) before DLSS. Held off at the title screen too (g_framegen_in_world): made
// there, it ran its first frames as the title scene loaded, and an RTX 5080 lost the card 5-8 s later, 3 starts of 5
// (2026-10-17, "DXGI_ERROR_DEVICE_HUNG")
bool g_dlss5 = false;
bool g_dlss_gone = false;    // ... it (DLSS or FSR) isn't available here, or failed: the scene back at its own size
std::string g_dlss_gone_why; // ... "Not available: ..." or "Stopped: ...", for /vana
// The DLSS (or FSR) helper lost (crashed, ended, stopped answering): the overlay's toast says so for kLostToastMs
constexpr ULONGLONG kLostToastMs = 5000;
ULONGLONG g_lost_at = 0;     // when (0: not this session)
std::wstring g_lost_title;   // "DLSS crashed", "FSR stopped"
bool g_background = false;   // [graphics] background sets the scene's size (else the launcher's)
unsigned g_render_w = 0, g_render_h = 0; // window.cpp's: in full screen, resolution.game's size (0: the game's own)
bool g_sun_shadows = false;  // ... and its sun shadows are on, cast by characters day and night (effects=1, [graphics.effects] sun > 0, sun_casters and sun_moon_casters != 2)
bool g_effects = false;      // ... effects=1: its scene effects run
int g_render_visual = 0;     // ... [graphics.effects] debug as the renderer has it: one part of them alone (render_visual), 0 none
bool g_keep_game_shadows = false; // [graphics] gameshadows=keep
int g_fill_mode = 3;              // /fillmode: a D3DFILLMODE, 3 solid (the game's own) till set
float g_aspect = -1.0f;      // aspect: < 0 off, 0 the window's shape, > 0 that shape
float g_ui_aspect = 0.0f;    // [ui] aspect: the interface's shape, 0 off
bool g_ui_game = false;      // [ui] aspect=game: the shape of the game's resolution, as it is
FILETIME g_ini_time{};
ULONGLONG g_next_check = 0;

// Only touched on the game's thread.
std::uintptr_t g_aspect_global = 0;
bool g_aspect_searched = false;
bool g_mouse_held = false;
distance::Settings g_distance;                     // [graphics] drawdistance, drawdistance.mobs, fog
bumpmap::Mode g_bump = bumpmap::Mode::Game;        // [graphics] bump: game (the default), 1 or 0
bool g_nameplates_fix = true; // [graphics] nameplates: fix or spaced (the default)
bool g_nameplates_spaced = true; // ... spaced: the letters by their own widths (else the game's, a cell each)
float g_nameplate_gap = 3.0f;  // ... spaced: nameplates.gap, -8 to 16
zonelines::Settings g_zonelines; // zonelines, zonelines.color, zonelines.strength
sparkles::Settings g_sparkles;   // sparkles, sparkles.strength, .size, .density, .places
nameplates::Names g_names;      // nameplates.hideown, .hideparty, .hidealliance, .hideplayers, .hidenpcs, .hidemonsters, .hidetrusts, .hidepets, .bazaar, .linkshell, .mastery, .size, .color.*
damagetext::Settings g_damage;  // damage.*: the numbers over characters as they're hit
bool g_grass_auto = false;      // grass.brightness.auto: the grass dims through the Vana'diel night (track_daylight)
float g_grass_night = 0.5f;     // ... grass.brightness.night: its share of grass.brightness at night
bool g_grass_floors = false;    // grass, and grass.walkable or grass.nowater: the zone's floors are read for it (track_focus, grass_floors; the
                                // reflections ask for them themselves, vanacore_gfx_floors_wanted)
unsigned g_grass_zone = 0;      // the zone the grass's settings were last given for (its dirt level: track_focus)
bool g_grass_freecam = true;    // grass.freecam: while the free camera is on, the grass grows round it, not the player (track_grass_centre)
bool g_snow_floors = false;     // snow: the zone's floors are read for the footprints too (track_focus)
bool g_water_floors = false;    // water with water.detect=2: the zone's floors are read for the water's sheets (d3d8_water.h water_sheet)
bool g_snow_floors_said = false; // ... and whether the log has said it's on
bool g_collision_show = false;  // collision.show (/vana debug): the zone's whole collision drawn over the world (track_focus)

decltype(&GetCursorPos) g_get_cursor_pos = nullptr;

// While apply_live reads the settings: each section of vanacore.ini read whole, once (GetPrivateProfileString opens and
// parses the file for each key, and apply_live reads some 300: 10 ms or more, each frame a /vana slider is dragged)
struct Snapshot {
    bool on = false;
    std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::wstring>> sections; // by lower-case names
};
Snapshot g_snapshot;

struct Snapshotted {
    Snapshotted() { g_snapshot.on = true; }
    ~Snapshotted() { g_snapshot = Snapshot{}; }
    Snapshotted(const Snapshotted&) = delete;
    Snapshotted& operator=(const Snapshotted&) = delete;
};

std::wstring lower(std::wstring text)
{
    for (wchar_t& c : text)
        c = static_cast<wchar_t>(std::towlower(c));
    return text;
}

// The section's keys and values, as GetPrivateProfileString gives them (the first of a key, a value's quotes taken off)
const std::unordered_map<std::wstring, std::wstring>& snapshot_section(const wchar_t* section)
{
    std::wstring name = lower(section);
    if (const auto found = g_snapshot.sections.find(name); found != g_snapshot.sections.end())
        return found->second;
    auto& keys = g_snapshot.sections[std::move(name)];
    std::vector<wchar_t> buffer(16384);
    DWORD got = 0;
    for (;;) {
        got = GetPrivateProfileSectionW(section, buffer.data(), static_cast<DWORD>(buffer.size()), g_ini.c_str());
        if (got + 2 < buffer.size() || buffer.size() >= (1u << 22))
            break;
        buffer.resize(buffer.size() * 2);
    }
    buffer.resize(static_cast<std::size_t>(got) + 2);
    buffer[got] = buffer[got + 1] = L'\0';
    const auto trim = [](const wchar_t* from, const wchar_t* to) {
        while (from < to && (*from == L' ' || *from == L'\t'))
            ++from;
        while (to > from && (to[-1] == L' ' || to[-1] == L'\t'))
            --to;
        return std::wstring(from, to);
    };
    for (std::size_t at = 0; at < got && buffer[at] != L'\0';) {
        const wchar_t* const line = &buffer[at];
        const std::size_t length = std::wcslen(line);
        at += length + 1;
        const wchar_t* const equals = std::wcschr(line, L'=');
        if (equals == nullptr)
            continue;
        std::wstring value = trim(equals + 1, line + length);
        if (value.size() >= 2 && (value.front() == L'"' || value.front() == L'\'') && value.back() == value.front())
            value = value.substr(1, value.size() - 2);
        keys.emplace(lower(trim(line, equals)), std::move(value));
    }
    return keys;
}

std::wstring read(const wchar_t* section, const wchar_t* key, const wchar_t* fallback = L"")
{
    if (g_snapshot.on) {
        const auto& keys = snapshot_section(section);
        const auto found = keys.find(lower(key));
        return found != keys.end() ? found->second : fallback;
    }
    wchar_t value[512] = {};
    GetPrivateProfileStringW(section, key, fallback, value, 512, g_ini.c_str());
    return value;
}

// As GetPrivateProfileInt: `fallback` when the key isn't there or is empty, else its number (0x for hex; 0 if none)
int read_int(const wchar_t* section, const wchar_t* key, int fallback)
{
    if (!g_snapshot.on)
        return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, g_ini.c_str()));
    const auto& keys = snapshot_section(section);
    const auto found = keys.find(lower(key));
    if (found == keys.end() || found->second.empty())
        return fallback;
    const wchar_t* text = found->second.c_str();
    const bool negative = *text == L'-';
    if (*text == L'-' || *text == L'+')
        ++text;
    const bool hex = text[0] == L'0' && (text[1] == L'x' || text[1] == L'X');
    const unsigned long value = std::wcstoul(hex ? text + 2 : text, nullptr, hex ? 16 : 10);
    return negative ? -static_cast<int>(value) : static_cast<int>(value);
}

std::string narrow(const std::wstring& text, UINT codepage = CP_UTF8)
{
    const int length = WideCharToMultiByte(codepage, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(length > 0 ? length - 1 : 0, '\0');
    if (length > 1)
        WideCharToMultiByte(codepage, 0, text.c_str(), -1, out.data(), length, nullptr, nullptr);
    return out;
}

void log_line(const char* line)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, line, -1, nullptr, 0);
    std::wstring text(length > 0 ? length - 1 : 0, L'\0');
    if (length > 1)
        MultiByteToWideChar(CP_UTF8, 0, line, -1, text.data(), length);
    log::write(L"Graphics: %ls", text.c_str());
}

// A path from vanacore.ini: relative ones are from VanaCore.exe's folder.
std::wstring resolve(const std::wstring& path)
{
    if (path.size() >= 2 && (path[1] == L':' || (path[0] == L'\\' && path[1] == L'\\')))
        return path;
    return g_dir + L"\\" + path;
}

// w:h, wxh, w/h, or a ratio (1.778); 0 if it's none of those
float parse_shape(const std::wstring& text)
{
    wchar_t* end = nullptr;
    double a = wcstod(text.c_str(), &end), b = 1.0;
    if (*end == L':' || *end == L'x' || *end == L'/')
        b = wcstod(end + 1, &end);
    if (*end != L'\0' || !(a > 0) || !(b > 0) || a / b < 0.5 || a / b > 8)
        return 0.0f;
    return static_cast<float>(a / b);
}

bool flag(const wchar_t* key, bool fallback)
{
    return read_int(L"graphics", key, fallback ? 1 : 0) != 0;
}

// A number from [graphics], from min to max: `fallback` when it isn't there, or isn't one (said in the log).
float number(const wchar_t* key, float fallback, float min, float max)
{
    const std::wstring text = read(L"graphics", key);
    if (text.empty())
        return fallback;
    wchar_t* end = nullptr;
    const float value = std::wcstof(text.c_str(), &end);
    if (end == text.c_str() || *end != L'\0' || !(value >= min && value <= max)) {
        log::write(L"Graphics: %ls=%ls isn't a number from %g to %g; ignored", key, text.c_str(), min, max);
        return fallback;
    }
    return value;
}

// A colour from [graphics]: #RRGGBB (or RRGGBB); unset, empty or game, the game's own (said in the log when
// it isn't one).
std::uint32_t colour(const wchar_t* key)
{
    const std::wstring text = read(L"graphics", key);
    if (text.empty() || _wcsicmp(text.c_str(), L"game") == 0)
        return nameplates::kGameColour;
    const wchar_t* digits = text.c_str() + (text[0] == L'#' ? 1 : 0);
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(digits, &end, 16);
    if (std::wcslen(digits) != 6 || *end != L'\0') {
        log::write(L"Graphics: %ls=%ls isn't a colour (#RRGGBB); the game's", key, text.c_str());
        return nameplates::kGameColour;
    }
    return static_cast<std::uint32_t>(value);
}

// The damage numbers' colours: damage.color, .miss.color, .taken.color, .heal.color (the game's unset; a critical hit's,
// damage.crit.color, is read with damage.crit) and .heal.plus; each kind's gradient (sharp only), <kind>.gradient from its
// colour to <kind>.color2 (unset: Settings' own), top to bottom or, damage.gradient.direction=horizontal, left to right;
// and the outline's, damage.outline.color (unset: the dark outline)
void read_damage_colours(damagetext::Settings& damage)
{
    damage.hit_colour = colour(L"damage.color");
    damage.miss_colour = colour(L"damage.miss.color");
    damage.taken_colour = colour(L"damage.taken.color");
    damage.heal_colour = colour(L"damage.heal.color");
    damage.heal_plus = flag(L"damage.heal.plus", false);
    const damagetext::Settings defaults;
    const auto or_default = [](std::uint32_t read, std::uint32_t fallback) {
        return read != nameplates::kGameColour ? read : fallback;
    };
    damage.hit_gradient = flag(L"damage.gradient", false);
    damage.hit_second = or_default(colour(L"damage.color2"), defaults.hit_second);
    damage.miss_gradient = flag(L"damage.miss.gradient", false);
    damage.miss_second = or_default(colour(L"damage.miss.color2"), defaults.miss_second);
    damage.taken_gradient = flag(L"damage.taken.gradient", false);
    damage.taken_second = or_default(colour(L"damage.taken.color2"), defaults.taken_second);
    damage.heal_gradient = flag(L"damage.heal.gradient", false);
    damage.heal_second = or_default(colour(L"damage.heal.color2"), defaults.heal_second);
    damage.crit_gradient = flag(L"damage.crit.gradient", false);
    damage.crit_second = or_default(colour(L"damage.crit.color2"), defaults.crit_second);
    damage.gradient_across = _wcsicmp(read(L"graphics", L"damage.gradient.direction").c_str(), L"horizontal") == 0;
    damage.outline_colour = or_default(colour(L"damage.outline.color"), defaults.outline_colour);
}

// [graphics.effects] <key>.enabled: 0 turns that effect off, keeping <key>'s value.
bool effect_enabled(const wchar_t* key)
{
    return read_int(L"graphics.effects", (std::wstring(key) + L".enabled").c_str(), 1) != 0;
}

// An effect setting to the renderer, debug's kept as it has it (render_visual: the composite takes it as int(debug))
void fx_set(const char* key, float value)
{
    vanacore_gfx_fx_set(key, value);
    if (std::strcmp(key, "debug") == 0)
        g_render_visual = std::isnan(value) ? 0 : static_cast<int>(value); // not set: the renderer's default, off
}

// aspect=auto|off|<w:h>
float read_aspect()
{
    const std::wstring text = read(L"graphics", L"aspect", L"auto");
    if (_wcsicmp(text.c_str(), L"off") == 0)
        return -1.0f;
    if (_wcsicmp(text.c_str(), L"auto") == 0 || text.empty())
        return 0.0f;
    const float shape = parse_shape(text);
    if (shape == 0.0f)
        log::write(L"Graphics: aspect=%ls isn't auto, off or a shape (16:9); auto", text.c_str());
    return shape > 0.0f ? shape : 0.0f;
}

// [ui] aspect=game|off|<w:h>: -1 for game (the default), 0 off, else the shape
float read_ui_aspect()
{
    const std::wstring text = read(L"ui", L"aspect", L"game");
    if (_wcsicmp(text.c_str(), L"game") == 0 || _wcsicmp(text.c_str(), L"auto") == 0 || text.empty())
        return -1.0f;
    if (_wcsicmp(text.c_str(), L"off") == 0)
        return 0.0f;
    const float shape = parse_shape(text);
    if (shape == 0.0f)
        log::write(L"Graphics: [ui] aspect=%ls isn't game, off or a shape (16:9); game", text.c_str());
    return shape > 0.0f ? shape : -1.0f;
}

// The shape of the game's resolution (its back buffer), 0 before the game has one
float game_shape()
{
    unsigned w = 0, h = 0;
    ui::game_screen(w, h);
    return w != 0 && h != 0 ? static_cast<float>(w) / static_cast<float>(h) : 0.0f;
}

FILETIME ini_time()
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    GetFileAttributesExW(g_ini.c_str(), GetFileExInfoStandard, &data);
    return data.ftLastWriteTime;
}

// antialias=off|fxaa: true for FXAA
bool read_antialias()
{
    const std::wstring text = read(L"graphics", L"antialias", L"off");
    if (_wcsicmp(text.c_str(), L"fxaa") == 0)
        return true;
    if (!text.empty() && _wcsicmp(text.c_str(), L"off") != 0)
        log::write(L"Graphics: antialias=%ls isn't off or fxaa; off", text.c_str());
    return false;
}

// [graphics] resolution=window|game: true draws the frame at the window's size, false (game, the
// default) at the game's resolution
bool read_draw_window(bool report)
{
    const std::wstring text = read(L"graphics", L"resolution", L"game");
    if (_wcsicmp(text.c_str(), L"window") == 0)
        return true;
    if (report && !text.empty() && _wcsicmp(text.c_str(), L"game") != 0)
        log::write(L"Graphics: resolution=%ls isn't window or game (the game's resolution is [ffxi] "
                   L"resolution.game); game",
                   text.c_str());
    return false;
}

// [graphics] background: the 3D scene's size (registry::parse_background). With DirectX 11 or 12 it's VanaCore's to
// set, apart from the launcher: empty, the old "launcher" or junk default to 3072x3072, not the launcher's [ffxi]
// resolution.background. With DirectX 8 or 9 it stays 0 x 0, so the launcher's is used.
void read_background(bool report, long& width, long& height)
{
    const std::wstring text = read(L"graphics", L"background");
    if (!registry::parse_background(text, width, height) && report)
        log::write(L"Graphics: background=%ls isn't a size from 512x512 to 8192x8192; 3072x3072", text.c_str());
    if ((g_d3d12 || g_d3d11) && width == 0)
        width = height = 3072;
}

// The /vana DLSS slider's 0-100 (dlss.quality) as the share of the drawn size the scene is drawn at, each way:
// performance (50%) at 0, balanced (58%) at 35, quality (67%) at 65, native (100%, DLAA) at 100, straight between
float dlss_quality_scale(float quality)
{
    static const float at[4] = {0.0f, 35.0f, 65.0f, 100.0f}, share[4] = {0.5f, 0.58f, 0.667f, 1.0f};
    for (int i = 0; i < 3; ++i)
        if (quality <= at[i + 1])
            return share[i] + (share[i + 1] - share[i]) * ((quality > at[i] ? quality : at[i]) - at[i]) / (at[i + 1] - at[i]);
    return 1.0f;
}

// [graphics] dlss=off|on|dlaa|quality|balanced|performance|ultra: NVIDIA DLSS (renderer=d3d12, an RTX card), as an
// NVSDK_NGX_PerfQuality_Value (-1 off), and the share of the drawn size the scene is drawn at for it (scale). `on`
// draws it at dlss.quality, 0-100 (the /vana panel's slider: dlss_quality_scale; else an older dlss.scale, the
// scene's percent), with the nearest mode's model and DLAA's at 100; the named modes at their own.
int read_dlss(bool report, float& scale)
{
    const std::wstring text = read(L"graphics", L"dlss", L"off");
    struct Mode {
        const wchar_t* name;
        int quality;
        float scale;
    };
    static const Mode modes[] = {{L"off", -1, 0.0f},        {L"dlaa", 5, 1.0f},        {L"quality", 2, 0.667f},
                                 {L"balanced", 1, 0.58f},  {L"performance", 0, 0.5f}, {L"ultra", 3, 0.333f}};
    for (const Mode& m : modes)
        if (_wcsicmp(text.c_str(), m.name) == 0) {
            scale = m.scale;
            return m.quality;
        }
    if (_wcsicmp(text.c_str(), L"on") == 0) {
        if (!read(L"graphics", L"dlss.quality").empty() || read(L"graphics", L"dlss.scale").empty())
            scale = dlss_quality_scale(number(L"dlss.quality", 35.0f, 0.0f, 100.0f));
        else
            scale = number(L"dlss.scale", 58.0f, 33.0f, 100.0f) / 100.0f;
        return scale >= 0.999f ? 5 : scale >= 0.625f ? 2 : scale >= 0.54f ? 1 : scale >= 0.42f ? 0 : 3;
    }
    if (report)
        log::write(L"Graphics: dlss=%ls isn't off, on, dlaa, quality, balanced, performance or ultra; off", text.c_str());
    scale = 0.0f;
    return -1;
}

// The /vana FSR slider's 0-100 (fsr.quality) as the share of the drawn size the scene is drawn at, each way, as DLSS's
// slider with AMD's scales: performance (50%, 2x) at 0, balanced (59%, 1.7x) at 35, quality (67%, 1.5x) at 65, native
// (100%, Native AA) at 100, straight between
float fsr_quality_scale(float quality)
{
    static const float at[4] = {0.0f, 35.0f, 65.0f, 100.0f}, share[4] = {0.5f, 0.588f, 0.667f, 1.0f};
    for (int i = 0; i < 3; ++i)
        if (quality <= at[i + 1])
            return share[i] + (share[i + 1] - share[i]) * ((quality > at[i] ? quality : at[i]) - at[i]) / (at[i + 1] - at[i]);
    return 1.0f;
}

// [graphics] fsr=off|on|native|quality|balanced|performance|ultra: AMD FSR (renderer=d3d12, any card: FSR 4 on a Radeon
// RX 7000 or 9000, FSR 3.1 elsewhere), as read_dlss: the NVSDK_NGX_PerfQuality_Value of the nearest DLSS mode (-1 off;
// the renderer runs either on the same scale) and the share of the drawn size the scene is drawn at (scale), AMD's: `on`
// at fsr.quality, 0-100 (the /vana panel's slider: fsr_quality_scale), the named modes at AMD's own (Native AA 1x,
// Quality 1.5x, Balanced 1.7x, Performance 2x, Ultra Performance 3x).
int read_fsr(bool report, float& scale)
{
    const std::wstring text = read(L"graphics", L"fsr", L"off");
    struct Mode {
        const wchar_t* name;
        int quality;
        float scale;
    };
    static const Mode modes[] = {{L"off", -1, 0.0f},       {L"native", 5, 1.0f},      {L"quality", 2, 0.667f},
                                 {L"balanced", 1, 0.588f}, {L"performance", 0, 0.5f}, {L"ultra", 3, 0.333f}};
    for (const Mode& m : modes)
        if (_wcsicmp(text.c_str(), m.name) == 0) {
            scale = m.scale;
            return m.quality;
        }
    if (_wcsicmp(text.c_str(), L"on") == 0) {
        scale = fsr_quality_scale(number(L"fsr.quality", 35.0f, 0.0f, 100.0f));
        return scale >= 0.999f ? 5 : scale >= 0.625f ? 2 : scale >= 0.54f ? 1 : scale >= 0.42f ? 0 : 3;
    }
    if (report)
        log::write(L"Graphics: fsr=%ls isn't off, on, native, quality, balanced, performance or ultra; off", text.c_str());
    scale = 0.0f;
    return -1;
}

// [graphics] fsr.version=auto|4|3|1: the FSR version asked for, 0 FSR's own choice (FSR 4 where it runs, else FSR 3.1, and
// VanaCore's FSR 1 where AMD's doesn't run at all); 1 FSR 1 alone (no helper: any DirectX 11 or 12 card)
int read_fsr_version(bool report)
{
    const std::wstring text = read(L"graphics", L"fsr.version", L"auto");
    if (text.empty() || _wcsicmp(text.c_str(), L"auto") == 0)
        return 0;
    if (text == L"4" || text == L"3" || text == L"1")
        return text[0] - L'0';
    if (report)
        log::write(L"Graphics: fsr.version=%ls isn't auto, 4, 3 or 1; auto", text.c_str());
    return 0;
}

// [graphics] dlss.framegen=off|2x|3x|4x: DLSS frame generation (with DLSS on; an RTX 40 or later, 3x and 4x an RTX
// 50): the frames made between two the game draws, 0 off.
// `fsr` is AMD's with DLSS instead (2x; any DirectX 12 card: an RTX 20 or 30 has no frame generation of NVIDIA's): kind.
int read_framegen(bool report, int& kind)
{
    const std::wstring text = read(L"graphics", L"dlss.framegen", L"off");
    kind = VANA_GFX_FG_DLSS;
    static const wchar_t* const names[] = {L"off", L"2x", L"3x", L"4x"};
    for (int i = 0; i < 4; ++i)
        if (_wcsicmp(text.c_str(), names[i]) == 0)
            return i;
    if (_wcsicmp(text.c_str(), L"fsr") == 0) {
        kind = VANA_GFX_FG_FSR;
        return 1;
    }
    if (report)
        log::write(L"Graphics: dlss.framegen=%ls isn't off, 2x, 3x, 4x or fsr; off", text.c_str());
    return 0;
}

// [graphics] fsr.framegen=off|2x (the default): AMD's frame generation with FSR (FSR FG 4 on a Radeon RX 9000 under
// Windows 11, else FSR FG 3.1): a frame made between two, 0 off.
int read_fsr_framegen(bool report)
{
    const std::wstring text = read(L"graphics", L"fsr.framegen", L"2x");
    if (_wcsicmp(text.c_str(), L"off") == 0)
        return 0;
    if (_wcsicmp(text.c_str(), L"2x") == 0 || _wcsicmp(text.c_str(), L"on") == 0)
        return 1;
    if (report)
        log::write(L"Graphics: fsr.framegen=%ls isn't off or 2x; off", text.c_str());
    return 0;
}

// The ferries' zones (Ship bound for Selbina / Mhaura and their pirate versions, the Open sea routes to Al Zahbi / Mhaura): the
// character stands still and the whole zone slides and turns past them for the ride's twelve minutes. The sun shadows' cache
// keeps the zone's pieces where they were last seen, so it took the coast going by as new pieces every frame and the game fell
// to 1-3 FPS (a stats recording, 2026-10-29: 98% of the frame in sun_cache_update). The zone's shadows (sun_casters 0 and 2)
// are left out there; characters keep theirs.
bool g_ship_zone = false;

bool ship_zone(unsigned zone)
{
    return zone == 46 || zone == 47 || zone == 220 || zone == 221 || zone == 227 || zone == 228;
}

// Startup, the device made, vanacore.ini changed on disk, or reload() (the /vana window: no log).
enum class When { Startup, Device, Reload, Quiet };

// [graphics] glow: the glow round trusts and avatars - off, the game's at a size (256 as it has it, 512, 1024, ...), or custom
// (VanaCore's own); glow.strength, glow.size and glow.quality shape VanaCore's own (apply_live's)
void apply_glow(When when)
{
    const std::wstring glow = read(L"graphics", L"glow", L"custom");
    int glow_mode = -1;
    if (_wcsicmp(glow.c_str(), L"off") == 0)
        glow_mode = 0;
    else if (_wcsicmp(glow.c_str(), L"custom") == 0)
        glow_mode = -1;
    else {
        const long n = std::wcstol(glow.c_str(), nullptr, 10);
        if (n >= 64 && n <= 4096)
            glow_mode = static_cast<int>(n);
        else
            log::write(L"Graphics: glow=%ls isn't off, a size from 64 to 4096 or custom; custom", glow.c_str());
    }
    static int said = -2;
    if (glow_mode != said && when != When::Quiet)
        log::write(glow_mode == 0 ? L"Graphics: the trusts' and avatars' glow is off (glow=off)"
                   : glow_mode < 0 ? L"Graphics: the trusts' and avatars' glow is VanaCore's own (glow=custom)"
                              : L"Graphics: the trusts' and avatars' glow is the game's, drawn at %dx%d (glow)",
                   glow_mode, glow_mode);
    said = glow_mode;
    vanacore_gfx_set_glow(glow_mode);
    // [graphics] glow.strength and glow.size: VanaCore's own glow (custom) stronger or fainter, wider or tighter
    const float strength = number(L"glow.strength", 1.0f, 0.0f, 4.0f);
    const float size = number(L"glow.size", 1.0f, 0.1f, 4.0f);
    // glow.quality 0 to 3: how smooth a big glow is (finer blur steps, more rounds of it)
    const float quality = number(L"glow.quality", 1.0f, 0.0f, 3.0f);
    static float said_strength = 1.0f, said_size = 1.0f, said_quality = 1.0f;
    if ((strength != said_strength || size != said_size || quality != said_quality) && when != When::Quiet)
        log::write(L"Graphics: the glow's strength %.2f, size %.2f, quality %.0f (glow.strength, glow.size, glow.quality)", strength,
                   size, quality);
    said_strength = strength, said_size = size, said_quality = quality;
    vanacore_gfx_fx_set("glow_strength", strength);
    vanacore_gfx_fx_set("glow_size", size);
    vanacore_gfx_fx_set("glow_quality", quality);
}

// [graphics] grass=0|1 (Mass Grass, experimental): tufts of grass on the zone's grass-textured ground round the player.
// grass.textures: hex hashes of the
// ground textures that are grass (empty, any mostly green one; grass.log=1 lists the ones it finds in the log and
// logs\grass); grass.radius, grass.density (tufts to a square yalm), grass.tufts (the most tufts at once, in thousands), grass.height,
// grass.slope (the steepest ground, in degrees, it grows on), grass.width, grass.variation (how much the heights differ), grass.brightness, grass.flowers (1: a tenth of the tufts with a
// flower), grass.flower_brightness, grass.flowers_only,
// grass.brightness.auto (dims through the Vana'diel night to grass.brightness.night of it: track_daylight)
void apply_grass(When when)
{
    const bool on = flag(L"grass", false);
    const std::string textures = narrow(read(L"graphics", L"grass.textures"));
    static bool said = false;
    if (on != said && when != When::Quiet)
        log::write(on ? L"Graphics: grass on (grass)" : L"Graphics: grass off (grass)");
    said = on;
    // a zone the shipped data\grass_dirt.json says "grass": false of grows none, whatever grass says (Jeuno's cities); it follows
    // the zone (track_focus)
    const bool here = grass_assets::grows(static_cast<int>(g_grass_zone));
    static unsigned bare_said = 0;
    if (on && !here && bare_said != g_grass_zone)
        log::write(L"Graphics: grass: none in zone %u (data\\grass_dirt.json)", g_grass_zone);
    bare_said = on && !here ? g_grass_zone : 0;
    VanaGrass grass{};
    grass.enabled = on && here ? 1 : 0;
    grass.textures = textures.c_str();
    grass.radius = number(L"grass.radius", 150.0f, 4.0f, 500.0f);
    grass.density = number(L"grass.density", 100.0f, 0.1f, 100.0f);
    grass.height = number(L"grass.height", 0.4818f, 0.05f, 10.0f);
    grass.width = number(L"grass.width", 1.0f, 0.1f, 10.0f);
    grass.variation = number(L"grass.variation", 0.5f, 0.0f, 1.0f);
    grass.brightness = number(L"grass.brightness", 1.0f, 0.05f, 10.0f);
    {
        const bool dims = flag(L"grass.brightness.auto", true);
        const float night = number(L"grass.brightness.night", 0.5f, 0.05f, 1.0f);
        if ((dims != g_grass_auto || (dims && night != g_grass_night)) && when != When::Quiet)
            log::write(dims ? L"Graphics: the grass dims at night, to %.2f of its brightness (grass.brightness.auto, grass.brightness.night)"
                            : L"Graphics: the grass keeps its brightness day and night (grass.brightness.auto)",
                       night);
        g_grass_auto = dims, g_grass_night = night;
    }
    // grass.flowers (0 to 1, default 0): 1 a tenth of the tufts with a flower (2026-11-22: it was the share itself, and 0.05,
    // the panel's least step, was still too many)
    grass.flowers = number(L"grass.flowers", 0.0f, 0.0f, 1.0f) * 0.1f;
    grass.flower_brightness = number(L"grass.flower_brightness", 1.0f, 0.05f, 10.0f);
    grass.flowers_only = flag(L"grass.flowers_only", false) ? 1 : 0;
    // grass.tufts: a ceiling on the tufts at once, in thousands (1000: a million); unset or 0, none: as many as grass.radius and
    // grass.density need (grass.c tuft_limit). grass.limit, in tufts, from before it, while it's unset
    {
        const float tufts = number(L"grass.tufts", number(L"grass.limit", 0.0f, 0.0f, 1000000.0f) / 1000.0f, 0.0f, 1000.0f);
        grass.limit = tufts > 0.0f ? static_cast<unsigned>(tufts * 1000.0f + 0.5f) : 0u;
    }
    grass.slope = number(L"grass.slope", 40.0f, 5.0f, 85.0f);
    // the rings: grass.ring.spacing (percent of grass.radius, 25 unless set) puts rings 1-3 at it, twice it and three times it;
    // grass.ring1-3 (percent) put one where it says instead. grass.ring1-3.amount the share of tufts each keeps (percent; inside
    // ring 1, all of them). grass.c keeps them in order, each keeping no more than the one inside it. grass.rings.show=1 draws
    // them round the player (//vana's Debug)
    {
        static const float amount[3] = {80.0f, 50.0f, 30.0f};
        const float spacing = number(L"grass.ring.spacing", 25.0f, 1.0f, 100.0f);
        grass.rings_set = 1;
        for (int i = 0; i < 3; ++i) {
            const std::wstring key = L"grass.ring" + std::to_wstring(i + 1);
            const float start = spacing * static_cast<float>(i + 1);
            grass.ring[i] = number(key.c_str(), start < 100.0f ? start : 100.0f, 0.0f, 100.0f) / 100.0f;
            grass.ring_amount[i] = number((key + L".amount").c_str(), amount[i], 0.0f, 100.0f) / 100.0f;
        }
        grass.show_rings = flag(L"grass.rings.show", false) ? 1 : 0;
    }
    // grass.cover.show=1 (//vana's Debug): the tufts a floor over them covers (a building's, laid over the ground; left out
    // otherwise) drawn red, those with no floor at or under them to tell yellow
    grass.show_cover = flag(L"grass.cover.show", false) ? 1 : 0;
    grass.log = flag(L"grass.log", false) ? 1 : 0;
    // grass.edges=1 (the default): near the brink of a drop steeper than grass.slope (a ledge, a rock's top) the tufts grow
    // smaller, none hanging out over it
    grass.edges = flag(L"grass.edges", true) ? 1 : 0;
    // grass.edge.fade (yalms, 0 to 10, default 2; 0 off): tufts this near where the grass ends (a drop, a slope steeper than
    // grass.slope, rock or stone after soil, the ground's texture turning from grass) grow shorter, to grass.edge.height percent
    // of their height at it (5 to 100, default 25); climbing from half grass.slope to grass.slope they shorten the same
    grass.edge_fade = number(L"grass.edge.fade", 2.0f, 0.0f, 10.0f);
    grass.edge_low = number(L"grass.edge.height", 25.0f, 5.0f, 100.0f) / 100.0f;
    // grass.cull=1 (the default): a square of tufts wholly outside the camera's view isn't drawn (it's still made and kept, so
    // turning the camera shows it at once); 0 draws every square in reach
    grass.cull = flag(L"grass.cull", true) ? 1 : 0;
    // grass.match (0 to 1, default 0.75): how far the tufts are made the ground's colour under them, lit as the ground is (0 their own)
    grass.match = number(L"grass.match", 0.75f, 0.0f, 1.0f);
    // grass.dry=1 (default 0): the yellow-olive of dry grass (East Sarutabaruta's) counts as grass ground, not only green
    grass.dry = flag(L"grass.dry", false) ? 1 : 0;
    // grass.walkable=1 (the default): tufts only on ground the zone's collision says the player could stand on (path, grass,
    // stone...), not on carts, crates, trees or roofs (grass_floors reads the zone's floors from its DAT)
    grass.walkable = flag(L"grass.walkable", true) ? 1 : 0;
    // grass.nowater=1 (the default): no grass on ground the zone's collision calls shallow water (the game's footsteps splash
    // there: Dragon's Aery's meadow under its waterfalls, Pashhow's marsh), walkable or not; 0 grows it there as on grass
    grass.nowater = flag(L"grass.nowater", true) ? 1 : 0;
    // grass.moss=1 (default 0): low cushions of moss on that shallow water in place of the tufts (with grass.nowater)
    grass.moss = flag(L"grass.moss", false) ? 1 : 0;
    g_grass_floors = grass.enabled && (grass.walkable || grass.nowater);
    // grass.overgrown=1 (an easter egg, default 0): dry grass ground grows on floors the game's footsteps call stone or metal
    // throughout too, as the Nyzul portal in Alzadaal did (its gold is dry grass's colour); 0, only with soft ground under it
    grass.overgrown = flag(L"grass.overgrown", false) ? 1 : 0;
    // grass.shade=1 (the default): in grass ground, the texels in shadow (too dark to read as any colour) and the yellow-olive of
    // dry grass as saturated as dry grass is grow too, not only the green ones (Bhaflau Thickets' forest floors)
    grass.shade = flag(L"grass.shade", true) ? 1 : 0;
    // grass.dry.saturation (0.05 to 1, default 0.25) and grass.dry.hue (1 to 70 degrees, default 30: East Sarutabaruta's pale straw is 28-39): where dry grass's
    // yellow-olive starts, by its least mean saturation over a texture and its reddest hue
    grass.dry_sat = number(L"grass.dry.saturation", 0.25f, 0.05f, 1.0f);
    grass.dry_hue = number(L"grass.dry.hue", 30.0f, 1.0f, 70.0f);
    // grass.pathpad (yalms, 0 to 4; /vana's Path padding): no tuft grows with a path's ground this near it, so paths read wider.
    // The default for the zones data\grass_dirt.json gives no "pathpad" of their own (unset, its top-level one, 0.25); a zone's own
    // always wins (the slider locked there). It follows the zone (track_focus)
    grass.pathpad = grass_assets::pathpad(static_cast<int>(g_grass_zone), read(L"graphics", L"grass.pathpad").empty()
                                                                               ? grass_assets::pathpad_default()
                                                                               : number(L"grass.pathpad", 0.25f, 0.0f, 4.0f));
    // grass.build (ms a frame, default 1.5; -1 no limit): the time a frame spends making squares of tufts, the rest of a
    // square waiting for the next frame, so walking into a field or a ring's upgrade never stalls a frame
    grass.build_ms = number(L"grass.build", 1.5f, -1.0f, 50.0f);
    // grass.wind.speed (0 to 4, default 1): how fast the grass sways, times its own pace (0 stills it);
    // grass.wind.turbulence (0 to 4, default 1): how much the sway differs from place to place (0 a field moving as one)
    grass.wind_speed = number(L"grass.wind.speed", 1.0f, 0.0f, 4.0f);
    grass.wind_turbulence = number(L"grass.wind.turbulence", 1.0f, 0.0f, 4.0f);
    // grass.freecam=1 (the default): while the free camera (/freecam) is on, the grass grows round the ground under it, not round
    // the player, so a flight over a field has grass all the way; 0 keeps it round the player
    g_grass_freecam = flag(L"grass.freecam", true);
    // the zone's dirt level: how colourful its ground has to be to grow grass (higher leaves more of it bare as dirt), VanaCore's
    // own for each zone (data\grass_dirt.json), not a setting; it follows the zone (track_focus)
    grass.dirt = grass_assets::dirt(static_cast<int>(g_grass_zone));
    // grass.blended (unset: the zone's, from the same file; 1 every zone, 0 none): grass-textured ground the game draws blended
    // grows grass too, a layer over other ground (The Sanctuary of Zi'Tah's moss over its soil), as much as its vertices' alpha
    // shows of it. It follows the zone too
    {
        const std::wstring layered = read(L"graphics", L"grass.blended");
        grass.blended = layered.empty() ? (grass_assets::blended(static_cast<int>(g_grass_zone)) ? 1 : 0) : (flag(L"grass.blended", false) ? 1 : 0);
    }
    // the zone's own grass ground (the same file's "ground"): textures that are grass there whatever their colours say (The
    // Sanctuary of Zi'Tah's dark leaf-litter floor, taken for a plant's cut-outs by its darkness)
    const std::string ground = grass_assets::ground(static_cast<int>(g_grass_zone));
    grass.ground = ground.c_str();
    vanacore_gfx_set_grass(&grass);
    // the catalogue (addons\vanacore\vana\grass and the player's config\grass): its pictures, read once, and grass.asset.<id>
    // (on or off) and grass.asset.<id>.amount (its share of the mix) each time
    grass_assets::apply(g_ini, g_dir);
}

// The settings that take effect while the game runs: the effects, the shapes, vsync, antialias.
#include "graphics_water.h"
#include "graphics_rain.h"

void apply_live(When when)
{
    const Snapshotted snapshot; // vanacore.ini read a section at a time while this runs
    const bool first = when != When::Reload;
    const float aspect = read_aspect(), wanted_ui = read_ui_aspect();
    g_ui_game = wanted_ui < 0;
    const float ui_aspect = g_ui_game ? game_shape() : wanted_ui;
    if (when == When::Startup || (when == When::Reload && aspect != g_aspect))
        log::write(L"Graphics: 3D aspect ratio %ls", aspect < 0 ? L"as the game sets it (aspect=off)"
                                                     : aspect == 0 ? L"follows the window (aspect=auto)"
                                                                   : L"fixed (aspect=<w:h>)");
    g_aspect = aspect;
    g_keep_game_shadows = _wcsicmp(read(L"graphics", L"gameshadows", L"auto").c_str(), L"keep") == 0;
    // widgets.log=1 (/vana > Client > Developer > World widgets): a frame that draws the widgets in the world unlike the
    // frame before goes to the log (widgets.cpp's watch)
    widgets::set_watch(flag(L"widgets.log", false));
    // drawlog=<path>: the next frame's draws are written there while <path>.go exists (d3d8.c cap_present). Live: set the
    // key and reload (vana.config.reload(), [client] ini.upstream); the renderer copies the path, so no pointer is ever
    // written into it from outside (2026-10-10's crash in Present).
    const std::wstring drawlog = read(L"graphics", L"drawlog");
    const std::string drawlog_path = drawlog.empty() || !log::enabled() ? std::string() : narrow(resolve(drawlog), CP_ACP);
    vanacore_gfx_set_drawlog(drawlog_path.c_str());
    // lod=high|range|game, drawdistance, fog: applied at the next frame (lod::apply, distance::apply)
    const std::wstring lod = read(L"graphics", L"lod", L"game");
    const bool lod_range = _wcsicmp(lod.c_str(), L"range") == 0;
    const bool lod_known = lod_range || _wcsicmp(lod.c_str(), L"game") == 0 || _wcsicmp(lod.c_str(), L"high") == 0;
    if (!lod_known)
        log::write(L"Graphics: lod=%ls isn't high, range or game; game", lod.c_str());
    lod::set(_wcsicmp(lod.c_str(), L"high") == 0, lod_range ? number(L"lod.range", 2.0f, 0.25f, 10.0f) : 0.0f);
    // bump=game|1|0: the game's Bump Mapping option, changed while it runs (bumpmap.cpp); game leaves it as [ffxi]
    // bump_mapping set it at the start
    const std::wstring bump = read(L"graphics", L"bump", L"game");
    if (bump.empty() || _wcsicmp(bump.c_str(), L"game") == 0)
        g_bump = bumpmap::Mode::Game;
    else if (bump == L"1" || _wcsicmp(bump.c_str(), L"on") == 0 || _wcsicmp(bump.c_str(), L"true") == 0)
        g_bump = bumpmap::Mode::On;
    else if (bump == L"0" || _wcsicmp(bump.c_str(), L"off") == 0 || _wcsicmp(bump.c_str(), L"false") == 0)
        g_bump = bumpmap::Mode::Off;
    else {
        log::write(L"Graphics: bump=%ls isn't game, 1 or 0; game", bump.c_str());
        g_bump = bumpmap::Mode::Game;
    }
    // drawdistance.enabled=0 puts both back to 1x; the values are 5 when not set
    const bool distance_on = flag(L"drawdistance.enabled", true);
    g_distance.scenery = distance_on ? number(L"drawdistance", 5.0f, 0.0f, 100.0f) : 1.0f;
    g_distance.mobs = distance_on ? number(L"drawdistance.mobs", 5.0f, 0.0f, 100.0f) : 1.0f;
    // drawdistance.ship: the zone's on a ferry (g_ship_zone; the panel's Boat rides), 1 (the game's) when not set: the whole
    // zone moves past the boat with no view lists, so the game draws all of it in view out to the distance. Read again as
    // the player boards or leaves one (apply_live from the zone's watch)
    if (g_ship_zone && distance_on)
        g_distance.scenery = number(L"drawdistance.ship", 1.0f, 0.0f, 100.0f);
    // drawdistance.target=game|drawn: characters drawn past the game's own distance can't be targeted (distance.cpp
    // targetable), or can be as far as they're drawn
    const std::wstring target = read(L"graphics", L"drawdistance.target", L"game");
    g_distance.target_drawn = _wcsicmp(target.c_str(), L"drawn") == 0;
    if (!g_distance.target_drawn && _wcsicmp(target.c_str(), L"game") != 0)
        log::write(L"Graphics: drawdistance.target=%ls isn't game or drawn; game", target.c_str());
    // drawdistance.full=1: past the game's view lists too (every piece in view, the far ground filled in); off by default,
    // it costs a lot of frames (distance.cpp skip_view_lists)
    g_distance.full = distance_on && flag(L"drawdistance.full", false);
    // drawdistance.fill=1: the lists, and the big pieces they leave out (distance.cpp fill_past_lists); off by default
    g_distance.fill = distance_on && flag(L"drawdistance.fill", false);
    // drawdistance.large_off=1: Al'Taieu, drawn whole (no view lists; distance.cpp kLargeZones), keeps the game's own scenery
    // distance, the characters and the fog ours (distance.cpp zone_multiplier, fog); on by default
    g_distance.large_off = flag(L"drawdistance.large_off", true);
    // occl=0|1|2: the zone's pieces far off and hidden behind what's drawn left undrawn (1), or only counted in the log (2);
    // occl.near (world units), occl.frames (tests in a row), occl.margin (thousandths of the scene's width): occlusion.cpp.
    // On by default; Direct3D 12, a zone drawn from its quadtree past the game's own draw distance
    g_distance.occl.mode = static_cast<int>(number(L"occl", 1.0f, 0.0f, 2.0f));
    g_distance.occl.nearest = number(L"occl.near", 80.0f, 0.0f, 5000.0f);
    g_distance.occl.frames = static_cast<int>(number(L"occl.frames", 2.0f, 1.0f, 60.0f));
    g_distance.occl.margin = number(L"occl.margin", 10.0f, 0.0f, 200.0f);
    // fog=distance|game|custom; distance when unset
    const std::wstring fog = read(L"graphics", L"fog", L"distance");
    g_distance.fog = _wcsicmp(fog.c_str(), L"game") == 0       ? distance::Fog::Game
                     : _wcsicmp(fog.c_str(), L"custom") == 0 ? distance::Fog::Custom
                                                             : distance::Fog::Distance;
    if (g_distance.fog == distance::Fog::Distance && _wcsicmp(fog.c_str(), L"distance") != 0)
        log::write(L"Graphics: fog=%ls isn't distance, game or custom; distance", fog.c_str());
    g_distance.fog_start = number(L"fog.start", 100.0f, 0.0f, 5000.0f);
    g_distance.fog_end = number(L"fog.end", 400.0f, 0.0f, 5000.0f);
    if (g_distance.fog_end < g_distance.fog_start + 1.0f) // as the game keeps it: whole at least a unit farther
        g_distance.fog_end = g_distance.fog_start + 1.0f;
    // nameplates=stretched (game, off) | fix (fixed) | spaced; nameplates.gap: spaced's gap between
    // letters, in the font's pixels
    const std::wstring nameplates = read(L"graphics", L"nameplates", L"spaced");
    const wchar_t* const mode = nameplates.c_str();
    const bool stretched = _wcsicmp(mode, L"stretched") == 0 || _wcsicmp(mode, L"game") == 0 || _wcsicmp(mode, L"off") == 0;
    const bool fixed = _wcsicmp(mode, L"fix") == 0 || _wcsicmp(mode, L"fixed") == 0;
    if (!stretched && !fixed && _wcsicmp(mode, L"spaced") != 0)
        log::write(L"Graphics: nameplates=%ls isn't stretched, fix or spaced; spaced", mode);
    g_nameplates_fix = !stretched;
    const std::wstring gap_text = read(L"graphics", L"nameplates.gap", L"3");
    wchar_t* gap_end = nullptr;
    float gap = std::wcstof(gap_text.c_str(), &gap_end);
    if (gap_end == gap_text.c_str() || *gap_end != L'\0' || !(gap >= -8 && gap <= 16)) {
        log::write(L"Graphics: nameplates.gap=%ls isn't a number from -8 to 16; 3", gap_text.c_str());
        gap = 3.0f;
    }
    g_nameplates_spaced = !stretched && !fixed;
    g_nameplate_gap = gap;
    g_names.hide_own = flag(L"nameplates.hideown", false);
    g_names.hide_players = flag(L"nameplates.hideplayers", false);
    g_names.hide_npcs = flag(L"nameplates.hidenpcs", false);
    g_names.hide_monsters = flag(L"nameplates.hidemonsters", false);
    g_names.hide_trusts = flag(L"nameplates.hidetrusts", false);
    g_names.hide_pets = flag(L"nameplates.hidepets", false);
    g_names.hide_party = flag(L"nameplates.hideparty", false);
    g_names.hide_alliance = flag(L"nameplates.hidealliance", false);
    g_names.bazaar = flag(L"nameplates.bazaar", true);
    g_names.linkshell = flag(L"nameplates.linkshell", true);
    g_names.mastery = flag(L"nameplates.mastery", true);
    g_names.gm = flag(L"nameplates.gm", true); // staff's GM icons as CatsEyeXI's pictures (assets\gm), 0 the game's
    gmicons::enable(g_names.gm);               // ... and the Target window's mark of a GM
    g_names.gm_size = number(L"nameplates.gm.size", 1.25f, 0.5f, 3.0f);
    g_names.gm_x = number(L"nameplates.gm.x", 0.0f, -100.0f, 100.0f);
    g_names.gm_y = number(L"nameplates.gm.y", -2.0f, -100.0f, 100.0f);
    // nameplates.stars.style=game | one | small: the job master's three stars as the game draws them, only the big middle one, or three small ones
    const std::wstring star_style = read(L"graphics", L"nameplates.stars.style", L"game");
    g_names.star_style = nameplates::StarStyle::game;
    if (_wcsicmp(star_style.c_str(), L"one") == 0)
        g_names.star_style = nameplates::StarStyle::one;
    else if (_wcsicmp(star_style.c_str(), L"small") == 0)
        g_names.star_style = nameplates::StarStyle::tiny;
    else if (_wcsicmp(star_style.c_str(), L"game") != 0)
        log::write(L"Graphics: nameplates.stars.style=%ls isn't game, one or small; game", star_style.c_str());
    g_names.size = number(L"nameplates.size", 1.0f, 0.25f, 4.0f);
    g_names.smooth = flag(L"nameplates.smooth", true);
    g_names.distance = number(L"nameplates.distance", 100.0f, 0.0f, 100.0f) / 100.0f;
    g_names.order = flag(L"nameplates.order", true);
    g_names.blink = flag(L"nameplates.blink", true); // 0: the target's name doesn't pulse
    g_names.pearl_x = number(L"nameplates.pearl.x", 38.0f, -100.0f, 100.0f);
    g_names.pearl_y = number(L"nameplates.pearl.y", -1.0f, -100.0f, 100.0f);
    g_names.pearl_size = number(L"nameplates.pearl.size", 0.45f, 0.1f, 2.0f);
    // the bazaar bag from where and how big the pearl's settings make it
    g_names.bazaar_x = number(L"nameplates.bazaar.x", 4.0f, -100.0f, 100.0f);
    g_names.bazaar_y = number(L"nameplates.bazaar.y", -7.0f, -100.0f, 100.0f);
    g_names.bazaar_size = number(L"nameplates.bazaar.size", 1.0f, 0.25f, 4.0f);
    // nameplates.pearl.shape=16:9 | 4:3 | game | <w:h>: the pearl's and the bag's width over their height on the screen, the same in any window (game: as the game draws them, stretching with the window)
    const std::wstring pearl_shape = read(L"graphics", L"nameplates.pearl.shape", L"16:9");
    g_names.pearl_shape = 16.0f / 9.0f;
    if (_wcsicmp(pearl_shape.c_str(), L"game") == 0)
        g_names.pearl_shape = 0.0f;
    else if (const float shape = parse_shape(pearl_shape); shape > 0.0f)
        g_names.pearl_shape = shape;
    else
        log::write(L"Graphics: nameplates.pearl.shape=%ls isn't game or a shape (16:9); 16:9", pearl_shape.c_str());
    g_names.centre = flag(L"nameplates.center", true);
    g_names.characters_only = flag(L"nameplates.characteronly", true);
    g_names.plate_x = number(L"nameplates.plate.x", 0.0f, -100.0f, 100.0f);
    g_names.plate_y = number(L"nameplates.plate.y", 0.0f, -100.0f, 100.0f);
    g_names.name_x = number(L"nameplates.name.x", 0.0f, -100.0f, 100.0f);
    g_names.name_y = number(L"nameplates.name.y", 0.0f, -100.0f, 100.0f);
    g_names.stars_x = number(L"nameplates.stars.x", 1.0f, -100.0f, 100.0f);
    g_names.stars_y = number(L"nameplates.stars.y", 20.0f, -100.0f, 100.0f);
    g_names.stars_spacing = number(L"nameplates.stars.spacing", 40.0f, 0.0f, 200.0f) / 100.0f; // percent of the game's
    g_names.stars_size = number(L"nameplates.stars.size", 0.65f, 0.25f, 3.0f);
    g_names.players = colour(L"nameplates.color.players");
    g_names.npcs = colour(L"nameplates.color.npcs");
    g_names.monsters = colour(L"nameplates.color.monsters");
    g_names.claimed = colour(L"nameplates.color.claimed");
    g_names.mine = colour(L"nameplates.color.mine");
    g_names.trusts = colour(L"nameplates.color.trusts");
    g_names.pets = colour(L"nameplates.color.pets");
    // nameplates.font=game | sharp: the letters as VanaCore's own text (nameplates.family, .bold, .italic, .uppercase, .fontsize,
    // .textspacing), at the screen's pixels; with Direct3D 12 the names are drawn over the scene at the size it's shown (names_native)
    // (not [client] truefont.disable's: that's the interface's areas alone; the names and damage numbers keep their own
    // switches, 2026-11-09)
    const std::wstring font = read(L"graphics", L"nameplates.font", L"game");
    g_names.sharp = _wcsicmp(font.c_str(), L"sharp") == 0;
    if (_wcsicmp(font.c_str(), L"sharp") != 0 && _wcsicmp(font.c_str(), L"game") != 0)
        log::write(L"Graphics: nameplates.font=%ls isn't game or sharp; game", font.c_str());
    // its font, as every True Font area's: its own, else the default font ([client] truefont.family)
    const std::wstring family = read(L"graphics", L"nameplates.family"), standard = read(L"client", L"truefont.family");
    g_names.family = !family.empty() ? family : !standard.empty() ? standard : L"Segoe UI";
    g_names.bold = flag(L"nameplates.bold", true);
    g_names.italic = flag(L"nameplates.italic", false);
    g_names.upper = flag(L"nameplates.uppercase", false);
    // nameplates.fontsize: the interface's pixels, as every True Font area's; unset, nameplates.textsize's (the old Text size,
    // times the game's letters) times 18 px, about the same
    g_names.font_size = number(L"nameplates.fontsize", std::round(18.0f * number(L"nameplates.textsize", 1.0f, 0.5f, 2.0f)), 4.0f, 64.0f);
    g_names.text_spacing = number(L"nameplates.textspacing", 0.0f, -8.0f, 16.0f);
    g_names.trace = number(L"nameplates.trace", 0.0f, 0.0f, 60.0f);
    // nameplates.framegen=0: frame generation's frames have the names where the game's next frame has them, as before
    g_names.framegen = flag(L"nameplates.framegen", true);
    // damage.font=game | sharp: the numbers and "Miss!" over characters as they're hit, as VanaCore's own text
    // (damage.family, .bold, .italic, .textsize, .outline, kept between damage.min and damage.max pixels);
    // damage.crit: critical hits in damage.crit.color, damage.crit.size times bigger; damage.preview: samples on the screen
    const std::wstring damage_font = read(L"graphics", L"damage.font", L"game");
    g_damage.sharp = _wcsicmp(damage_font.c_str(), L"sharp") == 0;
    if (_wcsicmp(damage_font.c_str(), L"sharp") != 0 && _wcsicmp(damage_font.c_str(), L"game") != 0)
        log::write(L"Graphics: damage.font=%ls isn't game or sharp; game", damage_font.c_str());
    const std::wstring damage_family = read(L"graphics", L"damage.family", L"Segoe UI");
    g_damage.family = damage_family.empty() ? L"Segoe UI" : damage_family;
    g_damage.bold = flag(L"damage.bold", true);
    g_damage.italic = flag(L"damage.italic", true);
    g_damage.text_size = number(L"damage.textsize", 1.0f, 0.5f, 3.0f);
    g_damage.outline = number(L"damage.outline", 1.0f, 0.0f, 4.0f);
    g_damage.min = number(L"damage.min", 0.0f, 0.0f, 200.0f);
    g_damage.max = number(L"damage.max", 0.0f, 0.0f, 400.0f);
    g_damage.commas = flag(L"damage.commas", false);
    g_damage.miss_caps = flag(L"damage.misscaps", false);
    g_damage.crit = flag(L"damage.crit", true);
    const std::uint32_t crit_colour = colour(L"damage.crit.color");
    g_damage.crit_colour = crit_colour != nameplates::kGameColour ? crit_colour : 0xFFD040;
    g_damage.crit_size = number(L"damage.crit.size", 1.3f, 1.0f, 3.0f);
    g_damage.crit_exclaim = flag(L"damage.crit.exclaim", false);
    g_damage.preview = flag(L"damage.preview", false);
    g_damage.on_top = flag(L"damage.ontop", true); // drawn without the depth test: characters in front don't hide them
    // damage.duration: seconds a number stays up (the game's 2.5), rising as far as the game's over it
    g_damage.duration = number(L"damage.duration", damagetext::kGameDuration, damagetext::kMinDuration, damagetext::kMaxDuration);
    // damage.spawn=game | stacked | random with damage.offset.x/.y and damage.spread (digit heights); damage.color,
    // damage.miss.color, damage.taken.color, damage.heal.color; damage.multi, .multi.icon=swords | count, .multi.size (an icon beside
    // the numbers of a round of hits on one target, sharp); damage.label, .label.size (the skill's name above the
    // number, sharp)
    const std::wstring damage_spawn = read(L"graphics", L"damage.spawn", L"game");
    g_damage.spawn = _wcsicmp(damage_spawn.c_str(), L"stacked") == 0  ? damagetext::Spawn::Stacked
                     : _wcsicmp(damage_spawn.c_str(), L"random") == 0 ? damagetext::Spawn::Random
                                                                      : damagetext::Spawn::Game;
    if (g_damage.spawn == damagetext::Spawn::Game && _wcsicmp(damage_spawn.c_str(), L"game") != 0)
        log::write(L"Graphics: damage.spawn=%ls isn't game, stacked or random; game", damage_spawn.c_str());
    g_damage.offset_x = number(L"damage.offset.x", 0.0f, -10.0f, 10.0f);
    g_damage.offset_y = number(L"damage.offset.y", 0.0f, -10.0f, 10.0f);
    g_damage.spread = number(L"damage.spread", 1.0f, 0.0f, 10.0f);
    read_damage_colours(g_damage);
    g_damage.multi =flag(L"damage.multi", false);
    const std::wstring multi_icon = read(L"graphics", L"damage.multi.icon", L"swords");
    g_damage.multi_icon = _wcsicmp(multi_icon.c_str(), L"count") == 0 ? damagetext::MultiIcon::Count : damagetext::MultiIcon::Swords;
    if (g_damage.multi_icon == damagetext::MultiIcon::Swords && _wcsicmp(multi_icon.c_str(), L"swords") != 0)
        log::write(L"Graphics: damage.multi.icon=%ls isn't swords or count; swords", multi_icon.c_str());
    g_damage.multi_size = number(L"damage.multi.size", 0.6f, 0.2f, 2.0f);
    g_damage.label = flag(L"damage.label", false);
    g_damage.label_size = number(L"damage.label.size", 0.5f, 0.2f, 1.5f);
    vanacore_gfx_fx_set("names_native", g_d3d12 && (g_names.sharp || g_damage.sharp) ? 1.0f : 0.0f);
    vanacore_gfx_fx_set("names_fg", g_names.framegen ? 1.0f : 0.0f);
    g_zonelines.on = flag(L"zonelines", true);
    const std::uint32_t line_colour = colour(L"zonelines.color");
    g_zonelines.colour = line_colour != nameplates::kGameColour ? line_colour : 0x60C0FF;
    g_zonelines.strength = number(L"zonelines.strength", 1.0f, 0.0f, 1.0f);
    // zonelines.style=fill|line|dots|stars|floor
    const std::wstring line_style = read(L"graphics", L"zonelines.style", L"dots");
    g_zonelines.style = _wcsicmp(line_style.c_str(), L"line") == 0    ? 1u
                        : _wcsicmp(line_style.c_str(), L"dots") == 0  ? 2u
                        : _wcsicmp(line_style.c_str(), L"stars") == 0 ? 3u
                        : _wcsicmp(line_style.c_str(), L"floor") == 0 ? 4u
                                                                      : 0u;
    if (g_zonelines.style == 0 && _wcsicmp(line_style.c_str(), L"fill") != 0)
        log::write(L"Graphics: zonelines.style=%ls isn't fill, line, dots, stars or floor; fill", line_style.c_str());
    g_zonelines.glow = flag(L"zonelines.glow", true);
    g_zonelines.height = number(L"zonelines.height", 1.5f, -10.0f, 10.0f); // above the ground under each line
    g_zonelines.size = number(L"zonelines.size", 1.0f, 0.25f, 4.0f);
    g_zonelines.fade = number(L"zonelines.fade", 4.0f, 0.0f, 10.0f);
    // sparkles: points of light rising out of the places sparkles.places names (zone:x,y,z,radius,height[,#RRGGBB], a ;
    // between), the Nyzul Isle portal without it
    g_sparkles.on = flag(L"sparkles", true);
    g_sparkles.strength = number(L"sparkles.strength", 1.0f, 0.0f, 2.0f);
    g_sparkles.size = number(L"sparkles.size", 1.0f, 0.25f, 4.0f);
    g_sparkles.density = number(L"sparkles.density", 1.0f, 0.1f, 4.0f);
    g_sparkles.speed = number(L"sparkles.speed", 1.0f, 0.1f, 4.0f);
    g_sparkles.match = flag(L"sparkles.match", true); // each the colour of the scene where it's born
    // gmaura=1: an aura rising round every character showing a GM's icon, gmaura.color, gmaura.strength (off by default, and
    // not in the /vana window, for now)
    g_sparkles.aura = flag(L"gmaura", false);
    const std::uint32_t aura_colour = colour(L"gmaura.color");
    g_sparkles.aura_colour = aura_colour != nameplates::kGameColour ? aura_colour : 0xFF2A10;
    g_sparkles.aura_strength = number(L"gmaura.strength", 1.0f, 0.0f, 2.0f);

    {
        static std::wstring parsed;
        static bool any = false;
        const std::wstring places = read(L"graphics", L"sparkles.places", sparkles::default_places());
        if (!any || places != parsed)
            parsed = places, any = true, g_sparkles.places = sparkles::parse(places);
    }
    // trusts.spawn=cast|always|never: the effect trusts arrive with
    const std::wstring spawn = read(L"graphics", L"trusts.spawn", L"cast");
    const bool spawn_always = _wcsicmp(spawn.c_str(), L"always") == 0, spawn_never = _wcsicmp(spawn.c_str(), L"never") == 0;
    if (!spawn_always && !spawn_never && _wcsicmp(spawn.c_str(), L"cast") != 0)
        log::write(L"Graphics: trusts.spawn=%ls isn't cast, always or never; cast", spawn.c_str());
    trusts::set(spawn_always ? trusts::Spawn::Always : spawn_never ? trusts::Spawn::Never : trusts::Spawn::Cast);
    // hide.players, hide.pets, hide.trusts: other players, their pets and their trusts left out (others.cpp)
    {
        const unsigned hide = (flag(L"hide.players", false) ? others::kPlayers : 0u) | (flag(L"hide.pets", false) ? others::kPets : 0u) |
                              (flag(L"hide.trusts", false) ? others::kTrusts : 0u);
        static unsigned said = 0;
        if (hide != said && when != When::Quiet)
            log::write(L"Graphics: other players %ls, their pets %ls, their trusts %ls ([graphics] hide.players, hide.pets, hide.trusts)",
                       hide & others::kPlayers ? L"left out" : L"shown", hide & others::kPets ? L"left out" : L"shown",
                       hide & others::kTrusts ? L"left out" : L"shown");
        said = hide;
        others::set(hide);
    }
    if (!g_active)
    {
        if (g_d3d9_active)
            d3d9::set_vsync(flag(L"vsync", false)); // at the next Reset, or start
        g_sun_shadows = false;
        g_ui_aspect = ui_aspect;
        return;
    }
    if (when != When::Quiet && (first || ui_aspect != g_ui_aspect))
        log::write(g_ui_game ? L"Graphics: the interface keeps the game's shape, centered in a window of another shape "
                               L"([ui] aspect=game)"
                   : ui_aspect > 0 ? L"Graphics: the interface keeps the shape %.3f:1, centered ([ui] aspect)"
                                   : L"Graphics: the interface fills the window ([ui] aspect=off)",
                   ui_aspect);
    g_ui_aspect = ui_aspect;
    vanacore_gfx_set_ui_aspect(ui_aspect);
    vanacore_gfx_set_vsync(framerate::vsync(flag(L"vsync", false)) ? 1 : 0); // vsync.cutscene's in an event
    g_effects = flag(L"effects", true);
    vanacore_gfx_fx_set("fx", g_effects ? 1.0f : 0.0f);
    const bool fxaa = read_antialias();
    vanacore_gfx_fx_set("aa", fxaa ? 1.0f : 0.0f);
    const bool at_window = read_draw_window(when == When::Reload);
    long background_w = 0, background_h = 0;
    read_background(when == When::Reload, background_w, background_h);
    g_background = background_w != 0;
    float scene = 0.0f;
    // DLSS (Direct3D 12 only) or FSR (Direct3D 12 or 11; off, whatever vanacore.ini says, with any other renderer): the
    // scene drawn at its quality's share of the drawn size, in place of [graphics] background; not after it turned out not
    // to be there. Both on: DLSS on a card it runs on, FSR elsewhere (DLSS isn't known to run before the device is made:
    // FSR till then)
    float dlss_scale = 0.0f, fsr_scale = 0.0f;
    const int dlss_asked = g_d3d12 ? read_dlss(when == When::Reload, dlss_scale) : -1;
    const int fsr_asked = g_d3d12 || g_d3d11 ? read_fsr(when == When::Reload, fsr_scale) : -1;
    const bool fsr = fsr_asked >= 0 && (dlss_asked < 0 || !g_active || !vanacore_gfx_dlss_supported(nullptr, 0));
    const int upscaler = fsr ? VANA_GFX_UPSCALER_FSR : VANA_GFX_UPSCALER_DLSS;
    const int dlss = fsr ? fsr_asked : dlss_asked;
    if (fsr)
        dlss_scale = fsr_scale;
    const wchar_t* const name = fsr ? L"FSR" : L"DLSS";
    if ((dlss != g_dlss || dlss_scale != g_dlss_scale || upscaler != g_upscaler) && when != When::Quiet) {
        if (dlss < 0)
            log::write(L"Graphics: DLSS and FSR off ([graphics] dlss, fsr)");
        else
            log::write(L"Graphics: %ls on: the 3D scene drawn at %.0f%% of the size it's shown at ([graphics] %ls)%ls", name,
                       dlss_scale * 100.0f, fsr ? L"fsr" : L"dlss",
                       fsr && dlss_asked >= 0 ? L"; DLSS is on too, but doesn't run on this card" : L"");
    }
    if (dlss != g_dlss || upscaler != g_upscaler)
        g_dlss_gone = false;
    g_dlss = dlss, g_dlss_scale = dlss_scale, g_upscaler = upscaler;
    if (g_dlss >= 0 && !g_dlss_gone)
        scene = g_dlss_scale;
    vanacore_gfx_set_resolution(at_window ? 1 : 0, scene);
    vanacore_gfx_set_background(static_cast<uint32_t>(background_w), static_cast<uint32_t>(background_h));
    vanacore_gfx_set_render_size(g_render_w, g_render_h);
    // the upscaler first (FSR's sharpening and version with it), then whether it's on; the helper's log file: none (an
    // empty path) with logging off
    vanacore_gfx_set_upscaler(g_upscaler, number(L"fsr.sharpness", 0.0f, 0.0f, 1.0f), read_fsr_version(when == When::Reload));
    vanacore_gfx_set_dlss(g_dlss_gone ? -1 : g_dlss, (g_dir + L"\\dlss\\VanaCore.Dlss.exe").c_str(),
                          log::enabled() ? (g_dir + L"\\logs\\dlss.log").c_str() : L"");
    const bool upscaling = g_dlss >= 0 && !g_dlss_gone;
    const bool dlss_runs = upscaling && g_upscaler == VANA_GFX_UPSCALER_DLSS;
    // frame generation, with the upscaler (its depth and motion): with DLSS, NVIDIA's or AMD's ([graphics] dlss.framegen);
    // with FSR, AMD's ([graphics] fsr.framegen)
    int fg_kind = VANA_GFX_FG_DLSS;
    int framegen = 0;
    if (dlss_runs)
        framegen = read_framegen(when == When::Reload, fg_kind);
    else if (upscaling && !g_d3d11) // (frame generation is Direct3D 12's)
        framegen = read_fsr_framegen(when == When::Reload), fg_kind = VANA_GFX_FG_FSR;
    if (g_framegen_held)
        framegen = 0;
    if ((framegen != g_framegen || (framegen && fg_kind != g_framegen_kind)) && when != When::Quiet) {
        const bool amd = fg_kind == VANA_GFX_FG_FSR;
        if (framegen)
            log::write(L"Graphics: %ls frame generation %dx ([graphics] %ls)", amd ? L"AMD's (FSR)" : L"DLSS", framegen + 1,
                       g_upscaler == VANA_GFX_UPSCALER_FSR ? L"fsr.framegen" : L"dlss.framegen");
        else
            log::write(L"Graphics: frame generation off ([graphics] dlss.framegen, fsr.framegen)");
    }
    g_framegen = framegen, g_framegen_kind = fg_kind;
    vanacore_gfx_set_framegen_kind(g_framegen_kind);
    vanacore_gfx_set_framegen(g_framegen_in_world ? g_framegen : 0);
    // DLSS 5 (neural rendering), with DLSS: where the helper has nvngx_dlssnr.dll and NVIDIA's NGX gives it
    const bool dlss5 = dlss_runs && flag(L"dlss5", false);
    if (dlss5 != g_dlss5 && when != When::Quiet)
        log::write(dlss5 ? L"Graphics: DLSS 5 on: the 3D scene through it before DLSS ([graphics] dlss5)"
                         : L"Graphics: DLSS 5 off ([graphics] dlss5)");
    g_dlss5 = dlss5;
    vanacore_gfx_fx_set("dlss5", g_dlss5 && g_framegen_in_world ? 1.0f : 0.0f);
    apply_glow(when);
    // [graphics] characters.brightness: the light on players, NPCs and monsters times this (1 the game's; 0 unlit)
    {
        const float light = number(L"characters.brightness", 1.0f, 0.0f, 10.0f);
        static float said = 1.0f;
        if (light != said && when != When::Quiet)
            log::write(L"Graphics: characters lit %.2f times as brightly (characters.brightness)", light);
        said = light;
        vanacore_gfx_set_character_light(light);
    }
    // [graphics] untextured=0|1: the world's opaque surfaces drawn plain white, only the light and the vertex colours showing
    {
        const bool plain = flag(L"untextured", false);
        static bool said = false;
        if (plain != said && when != When::Quiet)
            log::write(L"Graphics: the world drawn %ls (untextured)", plain ? L"without its textures" : L"with its textures");
        said = plain;
        vanacore_gfx_set_untextured(plain ? 1 : 0);
    }
    // [graphics] grass*: Mass Grass (apply_grass)
    apply_grass(when);
    // [graphics] snow=0|1 (off by default): footprints pressed into ground the zone's collision calls snow, where players, NPCs and
    // monsters walk (src/gfx/snow.c); shading over the snow the game draws, nothing dug. snow.depth (yalms, how steep the walls look),
    // snow.strength (how dark), snow.fade (seconds a footprint takes to fill in with snow; 0 never)
    {
        VanaSnow snow{};
        snow.enabled = flag(L"snow", false) ? 1 : 0;
        snow.depth = number(L"snow.depth", 0.12f, 0.02f, 0.5f);
        snow.strength = number(L"snow.strength", 1.0f, 0.0f, 2.0f);
        snow.fade = number(L"snow.fade", 120.0f, 0.0f, 86400.0f);
        // snow.sink (yalms, 0 off): characters standing in snow are lowered into it, the snow's surface hiding their feet;
        // snow.soft (0 to 1, default 0.6): how soft the prints' edges are. A print keeps the look it was pressed in with
        snow.sink = number(L"snow.sink", 0.0f, 0.0f, 1.0f);
        snow.soft = number(L"snow.soft", 0.6f, 0.0f, 1.0f);
        g_snow_floors = snow.enabled != 0;
        if (snow.enabled != (g_snow_floors_said ? 1 : 0) && when != When::Quiet)
            log::write(snow.enabled ? L"Graphics: snow trails on (snow)" : L"Graphics: snow trails off (snow)");
        g_snow_floors_said = snow.enabled != 0;
        vanacore_gfx_set_snow(&snow);
    }
    // [graphics] sky=0|1 (off by default): VanaCore's sky in place of the game's dome (src/gfx/sky.c; Direct3D 12): for now the
    // dome's own colours drawn again (True Sky's first phase, research/true-sky.md). sky.log=1: a line as it takes the dome or not
    {
        VanaSky sky{};
        sky.enabled = flag(L"sky", false) ? 1 : 0;
        sky.log = flag(L"sky.log", false) ? 1 : 0;
        static int said = 0;
        if (sky.enabled != said && when != When::Quiet)
            log::write(sky.enabled ? L"Graphics: VanaCore's sky on (sky)" : L"Graphics: VanaCore's sky off (sky)");
        said = sky.enabled;
        vanacore_gfx_set_sky(&sky);
    }
    // [graphics] collision.show=1 (/vana debug's Collision, for this session only: ini::session_only): the zone's whole
    // collision, walls and floors, drawn over the world see-through, walls red and floors by their kind of ground (floors.c)
    {
        const bool show = flag(L"collision.show", false);
        if (show != g_collision_show && when != When::Quiet)
            log::write(show ? L"Graphics: the zone's collision shown (collision.show)" : L"Graphics: the zone's collision hidden (collision.show)");
        g_collision_show = show;
        vanacore_gfx_show_collision(show ? 1 : 0); // (the collision is read for the rain's shelter too: drawn only while shown)
    }
    read_water(when); // [graphics] water*: VanaCore's water (graphics_water.h)
    read_rain(when);  // [graphics] rain*: the characters wet in the rain (graphics_rain.h)
    if (when == When::Device) {
        log::write(L"Graphics: the frame drawn at %ls (resolution)", at_window ? L"the window's size" : L"the game's");
        if (background_w != 0)
            log::write(L"Graphics: the 3D scene drawn at %ldx%ld ([graphics] background)", background_w, background_h);
        else
            log::write(L"Graphics: the 3D scene drawn at the launcher's size, %ls ([ffxi] resolution.background)",
                       read(L"ffxi", L"resolution.background", L"2048x2048").c_str());
    }
    std::wstring changed;
    for (const char* key : kEffectKeys) {
        const std::wstring wide(key, key + std::strlen(key));
        // <key>.enabled=0 turns it off and keeps its value (the /vana window's effect toggles); Bright night's General lighting
        // goes with Bright night's switch
        if (!effect_enabled(wide == L"night_bright_ambient" ? L"night_bright" : wide.c_str())) {
            fx_set(key, 0.0f);
            changed += L" " + wide + L"=off";
            continue;
        }
        const std::wstring text = read(L"graphics.effects", wide.c_str());
        if (text.empty()) {
            fx_set(key, NAN); // not in vanacore.ini (or taken out): the renderer's default
            continue;
        }
        wchar_t* end = nullptr;
        const float value = wcstof(text.c_str(), &end);
        if (end == text.c_str() || *end != L'\0') {
            log::write(L"Graphics: [graphics.effects] %ls=%ls isn't a number; ignored", wide.c_str(), text.c_str());
            continue;
        }
        fx_set(key, value);
        changed += L" " + wide + L"=" + text;
    }
    // on a ferry, characters alone cast (g_ship_zone), by day and by night; the settings' own values are back with the next zone
    if (g_ship_zone) {
        fx_set("sun_casters", 1.0f);
        fx_set("sun_moon_casters", 1.0f);
    }
    // elight_lantern_tex=<hash>,<hash>...: the textures a lantern's or torch's flame is drawn with (the drawlog's "hash" of a
    // draw), which the light from the effects takes at elight_lantern's share (0.5 of elight by default)
    {
        // Norg's lanterns: the flame, its halo and its embers
        constexpr const wchar_t* kLanternTex = L"91f16c90d340945e,0aa3fb24d66bfa53,60c9c264c0de9cb1";
        const std::wstring tex = read(L"graphics.effects", L"elight_lantern_tex", kLanternTex);
        vanacore_gfx_set_lantern_textures(narrow(tex, CP_ACP).c_str());
    }
    // elight_dim_tex=<hash>,...: the same for a zone's effects that give far more light than they look to, taken at
    // elight_dim's share (0.2 by default)
    {
        // Al'Taieu's portal pools: the ring round the pool (Home Point #2's blew a character standing in it out to white)
        constexpr const wchar_t* kDimTex = L"996d1973ab02ec66";
        const std::wstring tex = read(L"graphics.effects", L"elight_dim_tex", kDimTex);
        vanacore_gfx_set_dim_textures(narrow(tex, CP_ACP).c_str());
    }
    // dlss5_skip_tex=<hash>,...: effects DLSS 5 isn't to take as lights (dlss5_fx_light)
    {
        // Al'Taieu's portal pools: the ring and its two glow sprites (DLSS 5 turned the pool into a field of coloured blocks)
        constexpr const wchar_t* kSkipTex = L"996d1973ab02ec66,f7a8bca5097ad216,8d99561f9547bd07";
        const std::wstring tex = read(L"graphics.effects", L"dlss5_skip_tex", kSkipTex);
        vanacore_gfx_set_dlss5_skip_textures(narrow(tex, CP_ACP).c_str());
    }
    // fog_color=game|sky|custom and fog_custom=#RRGGBB: the height fog's colour (the zone's own, as the game sets it; matched to
    // the sky, Direct3D 12's; or this one). The renderer takes them as a mode and three numbers.
    {
        const std::wstring fog_choice = read(L"graphics.effects", L"fog_color");
        const std::wstring custom = read(L"graphics.effects", L"fog_custom");
        unsigned long rgb = 0xB8C8DC;
        if (!custom.empty()) {
            const wchar_t* digits = custom[0] == L'#' ? custom.c_str() + 1 : custom.c_str();
            wchar_t* end = nullptr;
            const unsigned long value = wcstoul(digits, &end, 16);
            if (end != digits && *end == L'\0' && wcslen(digits) == 6)
                rgb = value;
            else
                log::write(L"Graphics: [graphics.effects] fog_custom=%ls isn't #RRGGBB; ignored", custom.c_str());
        }
        vanacore_gfx_fx_set("fog_mode", _wcsicmp(fog_choice.c_str(), L"sky") == 0 ? 1.0f : _wcsicmp(fog_choice.c_str(), L"custom") == 0 ? 2.0f : 0.0f);
        vanacore_gfx_fx_set("fog_cr", static_cast<float>((rgb >> 16) & 255) / 255.0f);
        vanacore_gfx_fx_set("fog_cg", static_cast<float>((rgb >> 8) & 255) / 255.0f);
        vanacore_gfx_fx_set("fog_cb", static_cast<float>(rgb & 255) / 255.0f);
    }
    const std::wstring sun = read(L"graphics.effects", L"sun");
    // sun_casters=2: the zone alone casts VanaCore's shadows; characters keep the game's own. sun_moon_casters the same through
    // the night (18:00 to 06:00 outdoors, with sun_moon on: gfx_dispatch.c gfx_sun_hours)
    const auto chars_cast = [](const std::wstring& casters) {
        return casters.empty() || std::lround(wcstof(casters.c_str(), nullptr)) != 2;
    };
    const std::wstring moon = read(L"graphics.effects", L"sun_moon");
    const bool at_night = moon.empty() || wcstof(moon.c_str(), nullptr) != 0.0f; // on by default
    const bool day_chars = chars_cast(read(L"graphics.effects", L"sun_casters")) || g_ship_zone;
    const bool night_chars = !at_night || chars_cast(read(L"graphics.effects", L"sun_moon_casters")) || g_ship_zone;
    const bool sun_on = flag(L"effects", true) && effect_enabled(L"sun") &&
                        (sun.empty() || wcstof(sun.c_str(), nullptr) > 0.0f); // 0.6 by default
    // the game's own option held Off (game_config.cpp) while characters cast VanaCore's at every hour; with no shadows at night
    // (sun_moon=0) it's held Off then too, as before there were night settings
    g_sun_shadows = sun_on && day_chars && night_chars;
    // ... and the game's own character shadow left out draw by draw while they do (d3d8.c game_shadow_hidden): it never shows
    // for the moment before game_config.cpp holds the game's option at Off again (gameshadows=keep: as the game draws it). Where
    // the environment alone casts by day or by night, the option is left as the player has it and the game's shadow is left
    // out only at the hours characters cast VanaCore's (gfx_sun_chars_now)
    vanacore_gfx_set_hide_game_shadows(sun_on && (day_chars || night_chars) && !g_keep_game_shadows ? 1 : 0);
    if (when == When::Reload)
        log::write(L"Graphics: settings reloaded from vanacore.ini: effects=%d, antialias=%ls, resolution=%ls, "
                   L"background=%ls%ls",
                   flag(L"effects", true) ? 1 : 0, fxaa ? L"fxaa" : L"off", at_window ? L"window" : L"game",
                   background_w != 0 ? (std::to_wstring(background_w) + L"x" + std::to_wstring(background_h)).c_str()
                                     : L"launcher",
                   changed.c_str());
}

// The window's shape (width over height), or 0 while it has no size.
float window_shape()
{
    RECT client{};
    const HWND window = window::handle();
    if (window == nullptr || !GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0)
        return 0.0f;
    return static_cast<float>(client.right) / static_cast<float>(client.bottom);
}

// The 3D scene projected for the shape it's shown at (aspect=auto) or the one asked for.
void fix_aspect()
{
    if (g_aspect < 0)
        return;
    if (!g_aspect_searched) {
        g_aspect_searched = true;
        const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
        const std::uintptr_t setter = game != nullptr ? scan::find(game, kAspectSetter) : 0;
        if (setter != 0)
            g_aspect_global = *reinterpret_cast<const std::uintptr_t*>(setter + 1);
        log::write(setter != 0 ? L"Graphics: aspect ratio: the camera hangs from FFXiMain.dll+0x%X"
                               : L"Graphics: aspect ratio: couldn't find the game's camera; the scene keeps its shape",
                   static_cast<unsigned>(g_aspect_global - reinterpret_cast<std::uintptr_t>(game)));
    }
    if (g_aspect_global == 0)
        return;
    auto* camera = *reinterpret_cast<std::uint8_t* const*>(g_aspect_global);
    if (camera == nullptr)
        return;
    const float ratio = g_aspect > 0 ? g_aspect : window_shape();
    if (!(ratio > 0))
        return;
    const float value = 4.0f / 3.0f / ratio;
    auto* field = reinterpret_cast<float*>(camera + kAspectField);
    if (*field != value)
        *field = value;
}

bool buttons_down()
{
    return (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000;
}

// The cursor as the game should see it, for a game that asks where it is.
BOOL WINAPI get_cursor_pos(LPPOINT point)
{
    const BOOL ok = g_get_cursor_pos(point);
    const HWND window = window::handle();
    if (!ok || point == nullptr || window == nullptr || !g_active || !(g_ui_aspect > 0))
        return ok;
    POINT client = *point;
    RECT area{};
    if (!ScreenToClient(window, &client) || !GetClientRect(window, &area) || client.x < 0 || client.y < 0 ||
        client.x >= area.right || client.y >= area.bottom)
        return ok;
    static bool logged = false;
    if (!logged) {
        logged = true;
        log::write(L"Graphics: the game reads the cursor with GetCursorPos; mapped for [ui] aspect");
    }
    int x = client.x, y = client.y;
    vanacore_gfx_unsqueeze(window, &x, &y, buttons_down() ? 1 : 0);
    point->x += x - client.x;
    point->y += y - client.y;
    return ok;
}

// The game's and the plugins' modules, not VanaCore's own (its overlay wants the real cursor).
void on_module(HMODULE module)
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&on_module), &self);
    if (module != self && !imports::is_windows_module(module))
        imports::hook(module, "USER32.dll", "GetCursorPos", &get_cursor_pos, g_get_cursor_pos);
}

// Environment settings the ported code reads with getenv (the upstream names).
void set_env(const char* name, const std::string& value)
{
    _putenv_s(name, value.c_str());
}

} // namespace

void install(const wchar_t* ini, const wchar_t* install_dir)
{
    g_ini = ini != nullptr ? ini : L"";
    g_dir = install_dir != nullptr ? install_dir : L"";
    vanacore_gfx_set_log(&log_line);
    if (g_ini.empty())
        return;
    // the interface's settings moved from [graphics] (ui.aspect, ui.native) to [ui]
    for (const wchar_t* key : {L"ui.aspect", L"ui.native"})
        if (!read(L"graphics", key).empty())
            log::write(L"Graphics: [graphics] %ls is ignored: it's %ls in [ui] now", key, key + 3);
    const std::wstring renderer = read(L"graphics", L"renderer", L"d3d8");
    g_d3d12 = _wcsicmp(renderer.c_str(), L"d3d12") == 0;
    g_d3d11 = _wcsicmp(renderer.c_str(), L"d3d11") == 0;
    g_d3d9 = _wcsicmp(renderer.c_str(), L"d3d9") == 0;
    if (!g_d3d12 && !g_d3d11 && !g_d3d9 && _wcsicmp(renderer.c_str(), L"d3d8") != 0)
        log::write(L"Graphics: renderer=%ls isn't d3d8, d3d9, d3d11 or d3d12; the game's own Direct3D 8", renderer.c_str());
    vanacore_gfx_set_d3d11(g_d3d11 ? 1 : 0); // VanaCore's Direct3D 8 on Direct3D 11 in place of 12
    if (!g_d3d12) {
        float scale = 0.0f;
        if (read_dlss(false, scale) >= 0)
            log::write(L"Graphics: [graphics] dlss is on but DLSS needs renderer=d3d12; DLSS stays off (and its colour grade)");
        if (!g_d3d11 && read_fsr(false, scale) >= 0)
            log::write(L"Graphics: [graphics] fsr is on but FSR needs renderer=d3d12 or d3d11; FSR stays off");
    }
    g_ini_time = ini_time();
    apply_live(When::Startup);
    if (!g_d3d12 && !g_d3d11) {
        if (g_ui_aspect > 0 || !read(L"graphics", L"textures").empty() || read_antialias() ||
            !read(L"graphics", L"background").empty())
            log::write(L"Graphics: [ui] aspect, textures, antialias and background need renderer=d3d12 or d3d11; ignored "
                       L"(the 3D scene is drawn at [ffxi] resolution.background, the launcher's)");
        if (g_d3d9)
            log::write(L"Graphics: renderer=d3d9: the game's Direct3D 8 through Direct3D 9 (vsync=%d)", flag(L"vsync", false) ? 1 : 0);
        return;
    }

    // Upstream's switches, which the ported code reads from the environment.
    set_env("FFXI_FX", flag(L"effects", true) ? "1" : "0");
    set_env("FFXI_FPS", flag(L"overlay", false) ? "1" : "0"); // its own frame-rate text; VanaCore has fps.display
    set_env("FFXI_PROFILE", flag(L"profile", false) ? "1" : "0");
    set_env("FFXI_D3D12_DEBUG", flag(L"debug", false) ? "1" : "0");
    set_env("FFXI_D3D11_DEBUG", flag(L"debug", false) ? "1" : "0");
    // [graphics] debug.gpu: the debug layer's GPU-based validation too (Direct3D 12; many times slower)
    set_env("FFXI_D3D12_GBV", flag(L"debug.gpu", false) ? "1" : "0");
    // [logging] gpu: GPU crash data (Windows' DRED) - where the graphics card was if it stops (Direct3D 12)
    g_gpu_data = GetPrivateProfileIntW(L"logging", L"gpu", 0, g_ini.c_str()) != 0;
    set_env("FFXI_D3D12_DRED", g_gpu_data ? "1" : "0");
    // [graphics] slabs=0: Direct3D 12's static buffers each their own resource again, as before the slab allocator
    set_env("FFXI_D3D12_SLABS", flag(L"slabs", true) ? "1" : "0");
    // [graphics] effects.aftersky=0: the effects run before the sky's clouds, sun and moon again (d3d8.c effects_after_sky)
    set_env("FFXI_SKY_IN_SCENE", flag(L"effects.aftersky", true) ? "1" : "0");
    if (_wcsicmp(read(L"graphics", L"probe", L"fast").c_str(), L"gpu") == 0)
        set_env("FFXI_PROBE", "gpu");
    // Off by default: the game's own textures, drawn larger, are magnified with its point filter
    // (blocky at 1.5x); the game's stretch of the finished menu is smooth, as it is in retail.
    if (GetPrivateProfileIntW(L"ui", L"native", 0, g_ini.c_str()) == 0)
        set_env("FFXI_UI_NATIVE", "0");
    const std::wstring cache = g_dir + L"\\cache";
    CreateDirectoryW(cache.c_str(), nullptr);
    set_env("FFXI_CACHE_DIR", narrow(cache, CP_ACP));
    const std::wstring drawlog = read(L"graphics", L"drawlog");
    if (!drawlog.empty() && log::enabled())
        set_env("FFXI_DRAWLOG", narrow(resolve(drawlog), CP_ACP));

    // tex_debug=<folder>: every texture the game uploads, as it goes up (see d3d8.c texture_replacement)
    const std::wstring tex_debug = read(L"graphics", L"tex_debug");
    if (!tex_debug.empty()) {
        const std::wstring folder = resolve(tex_debug);
        for (size_t i = folder.find(L'\\', 3); i != std::wstring::npos; i = folder.find(L'\\', i + 1))
            CreateDirectoryW(folder.substr(0, i).c_str(), nullptr);
        CreateDirectoryW(folder.c_str(), nullptr);
        set_env("FFXI_TEXLOG", "1");
        set_env("FFXI_TEXDUMP", narrow(folder, CP_ACP));
    }

    // normalmap=0|1|2|3: see d3d8.c normal_map_mode
    const std::wstring normalmap = read(L"graphics", L"normalmap");
    if (!normalmap.empty())
        set_env("FFXI_NORMALMAP", narrow(normalmap, CP_ACP));

    // textures=<folder>[;<folder>...]
    const std::wstring textures = read(L"graphics", L"textures");
    for (size_t start = 0; start < textures.size();) {
        size_t end = textures.find(L';', start);
        if (end == std::wstring::npos)
            end = textures.size();
        const std::wstring folder = textures.substr(start, end - start);
        if (!folder.empty())
            vanacore_gfx_texture_pack(narrow(resolve(folder), CP_ACP).c_str());
        start = end + 1;
    }

    if (!imports::for_each_module(&on_module))
        log::write(L"Graphics: couldn't watch modules load: a game that reads the cursor with GetCursorPos isn't "
                   L"mapped for [ui] aspect");
    log::write(L"Graphics: renderer=%ls: the game gets VanaCore's Direct3D 8 on Direct3D %ls (effects=%d, "
               L"vsync=%d, antialias=%ls)",
               g_d3d11 ? L"d3d11" : L"d3d12", g_d3d11 ? L"11" : L"12", flag(L"effects", true) ? 1 : 0,
               flag(L"vsync", false) ? 1 : 0, read(L"graphics", L"antialias", L"off").c_str());
}

void* direct3d_create()
{
    if (g_d3d9) {
        void* d3d = d3d9::direct3d_create(flag(L"vsync", false));
        if (d3d == nullptr) {
            log::write(L"Graphics: Direct3D 9 didn't come up; the game uses its own Direct3D 8");
            return nullptr;
        }
        g_d3d9_active = true;
        return d3d;
    }
    if (!g_d3d12 && !g_d3d11)
        return nullptr;
    if (const HMODULE game = GetModuleHandleW(L"FFXiMain.dll")) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(game) + dos->e_lfanew);
        const auto base = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(game));
        vanacore_gfx_set_image(base, base + nt->OptionalHeader.SizeOfImage);
    }
    void* d3d = vanacore_gfx_direct3d_create();
    if (d3d == nullptr) {
        log::write(L"Graphics: Direct3D %ls didn't come up; the game uses its own Direct3D 8", g_d3d11 ? L"11" : L"12");
        return nullptr;
    }
    g_d3d12_unsupported = flag(L"d3d12.unsupported", false);
    if (g_d3d12 && vanacore_gfx_d3d11() != 0) {
        // Direct3D 12 came up but its shaders don't compile (Wine's and Proton's d3dcompiler_47), or there's no device it
        // can use (below resource binding tier 2: Wine's vkd3d on a Mac under CrossOver; the line before says which):
        // Direct3D 11 draws the game, as renderer=d3d11, and what's Direct3D 12's alone (DLSS, DLSS's colour grade, frame
        // generation) is off
        g_d3d12 = false, g_d3d11 = true, g_d3d11_for_12 = true;
        log::write(L"Graphics: Direct3D 12 can't draw here (its shaders don't compile, or its device can't take VanaCore's "
                   L"renderer: the line before says which); Direct3D 11 draws the game in its place. Under Wine or Proton, "
                   L"Microsoft's d3dcompiler_47 in the prefix (winetricks d3dcompiler_47) lets Direct3D 12 draw: then set "
                   L"renderer=d3d12 again");
        // and from now on: renderer=d3d11, so the next start doesn't try Direct3D 12 first (the launcher's Renderer shows
        // vanacore.ini's, and leaves it be at Start Game), and d3d12.unsupported=1, for /vana > Client > About to say
        // why. The overlay says so for this session (switched_to_d3d11).
        g_d3d12_unsupported = true;
        g_d3d11_kept = ini::set(g_ini, L"graphics", L"renderer", L"d3d11") &&
                       ini::set(g_ini, L"graphics", L"d3d12.unsupported", L"1");
        log::write(g_d3d11_kept ? L"Graphics: vanacore.ini set to renderer=d3d11 (and d3d12.unsupported=1)"
                                : L"Graphics: couldn't write renderer=d3d11 to vanacore.ini; the next start tries Direct3D 12 again");
    } else if (g_d3d12 && g_d3d12_unsupported) {
        // Direct3D 12 draws here after all (Microsoft's d3dcompiler_47 put in Wine's place, a newer Wine): the mark goes
        g_d3d12_unsupported = false;
        ini::set(g_ini, L"graphics", L"d3d12.unsupported", nullptr);
        log::write(L"Graphics: Direct3D 12 draws here; d3d12.unsupported taken out of vanacore.ini");
    }
    g_active = true;
    apply_live(When::Device);
    return d3d;
}

bool active()
{
    return g_active;
}

namespace {

// The graphics card lost on Direct3D 12: VanaCore's device made again under the game (vanacore_gfx_device_recover), the
// settings sent to it again and the player told in the chat. At most three times a session, and not within five minutes of
// the last: Windows restarts the PC at five timeouts in a minute, and a hang that is VanaCore's own work would only come
// again. Frame generation stays off for the session from the second time, if it was running: both device losses seen on an
// RTX 5070 Ti Laptop (2026-10-21, 2026-10-24) were found at frame generation's Present with none of the game's own work
// unfinished. True when the game draws again.
bool recover_if_lost()
{
    static int recoveries = 0;
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (recoveries >= 3 || (recoveries != 0 && now - last < 5 * 60 * 1000)) {
        log::write(recoveries >= 3 ? L"Graphics: the graphics card was lost a fourth time this session: not made again"
                                   : L"Graphics: the graphics card was lost again within five minutes: not made again");
        return false;
    }
    log::write(L"Graphics: the graphics card was lost (the GPU lines before say how): VanaCore makes its device again under "
               L"the game (time %d of 3 this session)",
               recoveries + 1);
    const int on = vanacore_gfx_device_recover();
    if (on == 0) {
        log::write(L"Graphics: the device couldn't be made again (the gfx lines before say why)");
        return false;
    }
    ++recoveries, last = GetTickCount64();
    std::string told = "VanaCore made its device again and the game carried on";
    if (on == 2) {
        // Direct3D 11 draws the game from here, as with renderer=d3d11, and from the next start: Direct3D 12's device
        // couldn't be made again on this PC
        g_d3d12 = false, g_d3d11 = true, g_d3d11_for_12 = true;
        log::write(ini::set(g_ini, L"graphics", L"renderer", L"d3d11")
                       ? L"Graphics: Direct3D 11 draws the game from here; vanacore.ini set to renderer=d3d11: the next start is Direct3D 11's"
                       : L"Graphics: Direct3D 11 draws the game from here; couldn't write renderer=d3d11 to vanacore.ini");
        told += " on DirectX 11: DirectX 12 couldn't come back, so DLSS and frame generation are off until the next start";
    }
    told += ".";
    if (recoveries >= 2 && g_framegen != 0 && !g_framegen_held) {
        g_framegen_held = true;
        log::write(L"Graphics: frame generation stays off for the rest of this session: the card was lost twice while it ran "
                   L"([graphics] dlss.framegen, fsr.framegen)");
        told += " Frame generation stays off for this session, since the card stopped twice while it ran.";
    }
    grass_floors::forget(); // the zone's floors went with the device: handed over again
    zone_casters::forget(); // ... and its own meshes for the sun's shadows
    apply_live(When::Device); // the effects, the upscaler and frame generation, vsync: to the new device
    log::write(L"Graphics: the device made again in %llu ms: the game draws on", last - now);
    chat::write(chat::kDefaultMode,
                chat::header() + chat::colour(chat::kError, "Your graphics card stopped and Windows reset it (a driver timeout).") +
                    " " + told + (log::enabled() ? " What happened is in logs\\vanacore.log." : ""),
                "VanaCore");
    return true;
}

} // namespace

void close_if_lost()
{
    static bool closing = false; // the message box below runs game frames (FFXiMain's 0x401), so Present comes back here
    if (closing || !g_active || !vanacore_gfx_device_lost())
        return;
    // Direct3D 12's device made again under the game, and on it goes; Direct3D 11's closes as before (no gfx_shutdown yet)
    if (g_d3d12 && recover_if_lost())
        return;
    closing = true;
    log::write(L"Graphics: the graphics card was lost (the GPU lines before say how): nothing the game draws can show "
               L"again, so it closes");
    // Lost on Direct3D 12: the next start is Direct3D 11's (renderer=d3d11), so a card or driver that times out on 12
    // doesn't go on doing it. Into the settings file now, not 3 seconds on (ini::save): the game ends below. The file
    // read back, so the message says only what the next start will do; the launcher's Renderer takes it at Start Game.
    bool to_d3d11 = false;
    if (g_d3d12 && ini::set(g_ini, L"graphics", L"renderer", L"d3d11")) {
        ini::save(true);
        wchar_t renderer[16] = {};
        GetPrivateProfileStringW(L"graphics", L"renderer", L"", renderer, 16,
                                 (settings::file().empty() ? g_ini : settings::file()).c_str());
        to_d3d11 = _wcsicmp(renderer, L"d3d11") == 0;
        log::write(to_d3d11 ? L"Graphics: vanacore.ini set to renderer=d3d11: the next start is Direct3D 11's"
                            : L"Graphics: couldn't write renderer=d3d11 to vanacore.ini; the next start is Direct3D 12's again");
    }
    shutdown::run_bounded(L"the graphics card was lost", 5000); // what addons and plugins save as they go, saved
    if (const HWND window = window::handle())
        ShowWindow(window, SW_HIDE); // its last picture, frozen over the screen
    // A timeout stops the card for every game on the PC, and each closes here: the first says so, the rest close quietly
    // (the handle is left to the process's end, which follows the message)
    CreateMutexW(nullptr, FALSE, L"Local\\VanaCore.GraphicsCardLost");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        log::write(L"Graphics: another game on this PC is showing the graphics card's message; this one closes without it");
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(kExitGraphicsCardLost));
    }
    std::wstring text = L"Your graphics card stopped and Windows reset it (most often a graphics driver timeout). The game "
                        L"can't draw again after that, so it closes.\n\nYour addons' and plugins' settings were saved. What "
                        L"happened is in VanaCore's logs\\vanacore.log.";
    if (to_d3d11)
        text += L"\n\nVanaCore has switched you to DirectX 11 for the next start. To go back to DirectX 12, choose it in "
                L"the launcher's Accounts > Renderer or in /vana > Client.";
    if (!g_gpu_data && !g_d3d11)
        text += std::wstring(to_d3d11 ? L" If it happens again on DirectX 12" : L"\n\nIf it happens again") +
                L": tick Log GPU crash data in the launcher's VanaCore settings, play until it does, and send the support "
                L"files. The log then says what the card was doing when it stopped.";
    MessageBoxW(nullptr, text.c_str(), L"VanaCore", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(kExitGraphicsCardLost));
}

bool d3d9_active()
{
    return g_d3d9_active;
}

bool sun_shadows()
{
    return g_active && g_sun_shadows;
}

bool switched_to_d3d11()
{
    return g_d3d11_for_12;
}

int render_visual()
{
    return g_active && g_effects ? g_render_visual : 0;
}

bool keep_game_shadows()
{
    return g_keep_game_shadows;
}

int fill_mode()
{
    return g_fill_mode;
}

bool set_fill_mode(int mode)
{
    if (mode < 1 || mode > 3 || !g_active)
        return false;
    g_fill_mode = mode;
    vanacore_gfx_set_fill_mode(mode == 3 ? 0 : mode); // solid leaves the game's own fill mode, which is solid
    return true;
}

bool settling()
{
    if (!g_active || g_d3d11)
        return false;
    char message[8];
    return vanacore_gfx_dlss_state(message, sizeof message) == 1 || vanacore_gfx_framegen_state(message, sizeof message) == 1;
}

int shown_per_frame()
{
    if (!g_active || g_d3d11)
        return 1;
    char message[8];
    return vanacore_gfx_framegen_state(message, sizeof message) == 2 ? 1 + g_framegen : 1;
}

namespace {

// The graphics card's driver, as Device Manager has it: SetupAPI's display device for the card, found by the PCI ids of
// DXGI's adapter of that name (else by the name itself). devpkey.h's and devguid.h's keys, here without initguid.h.
constexpr GUID kDisplayClass{0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
constexpr DEVPROPKEY kDriverDate{{0xa8b865dd, 0x2e3d, 0x4094, {0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6}}, 2};
constexpr DEVPROPKEY kDriverVersion{{0xa8b865dd, 0x2e3d, 0x4094, {0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6}}, 3};
constexpr DEVPROPKEY kInstallDate{{0x83da6326, 0x97a6, 0x4088, {0x94, 0x53, 0xa1, 0x92, 0x3f, 0x57, 0x3b, 0x29}}, 100};

struct Driver {
    std::string shown;     // its maker's number: NVIDIA's 616.92, AMD's Adrenalin 24.9.1, else Windows'
    std::string version;   // Windows' (32.0.16.1692)
    std::string released;  // the driver's own date, YYYY-MM-DD
    std::string installed; // when this PC got it (the last install), YYYY-MM-DD, local time
    ULONGLONG installed_at = 0; // ... as a FILETIME, for its age
};

// Found on a thread of its own the first time a card is asked about (SetupAPI can take a while), then kept
std::mutex g_driver_lock;
std::string g_driver_card; // the card it's for, or being found for
Driver g_driver;

std::string ymd(const FILETIME& time, bool local)
{
    SYSTEMTIME utc{}, at{};
    if (!FileTimeToSystemTime(&time, &utc) || (local && !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &at)))
        return {};
    if (!local)
        at = utc; // a driver's date is midnight UTC: a day early in the local time west of it
    char text[16];
    std::snprintf(text, sizeof text, "%04u-%02u-%02u", at.wYear, at.wMonth, at.wDay);
    return text;
}

Driver find_driver(const std::string& card)
{
    UINT vendor = 0, device = 0, subsys = 0;
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; vendor == 0 && factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && narrow(desc.Description) == card)
                vendor = desc.VendorId, device = desc.DeviceId, subsys = desc.SubSysId;
            adapter->Release();
        }
        factory->Release();
    }
    const HDEVINFO set = SetupDiGetClassDevsW(&kDisplayClass, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return {};
    wchar_t want[48], want_subsys[24];
    std::swprintf(want, 48, L"PCI\\VEN_%04X&DEV_%04X", vendor, device);
    std::swprintf(want_subsys, 24, L"&SUBSYS_%08X", subsys);
    const std::wstring name = [&] {
        std::wstring out(card.size(), L'\0');
        out.resize(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, card.data(), static_cast<int>(card.size()), out.data(),
                                                           static_cast<int>(out.size()))));
        return out;
    }();
    // the best match: the card's ids and its board's (two of the same card), its ids, else its name
    SP_DEVINFO_DATA chosen{sizeof chosen};
    int best = 0;
    bool nvidia = vendor == 0x10DE;
    SP_DEVINFO_DATA dev{sizeof dev};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        wchar_t ids[1024] = {}, text[256] = {};
        int score = 0;
        if (vendor != 0 &&
            SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(ids), sizeof ids - 4, nullptr))
            for (const wchar_t* id = ids; *id != L'\0'; id += std::wcslen(id) + 1)
                if (_wcsnicmp(id, want, std::wcslen(want)) == 0 && score < 3)
                    score = std::wcsstr(id, want_subsys) != nullptr ? 3 : 2;
        if (score == 0 && ((SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_FRIENDLYNAME, nullptr, reinterpret_cast<BYTE*>(text),
                                                               sizeof text - 2, nullptr) &&
                            _wcsicmp(text, name.c_str()) == 0) ||
                           (SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_DEVICEDESC, nullptr, reinterpret_cast<BYTE*>(text),
                                                              sizeof text - 2, nullptr) &&
                            _wcsicmp(text, name.c_str()) == 0))) {
            score = 1;
            nvidia = nvidia || (SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(ids),
                                                                  sizeof ids - 4, nullptr) &&
                                std::wcsstr(ids, L"VEN_10DE") != nullptr);
        }
        if (score > best)
            best = score, chosen = dev;
    }
    Driver out;
    if (best > 0) {
        DEVPROPTYPE type = 0;
        wchar_t version[64] = {};
        if (SetupDiGetDevicePropertyW(set, &chosen, &kDriverVersion, &type, reinterpret_cast<BYTE*>(version), sizeof version - 2,
                                      nullptr, 0) &&
            type == DEVPROP_TYPE_STRING)
            out.version = narrow(version);
        FILETIME time{};
        if (SetupDiGetDevicePropertyW(set, &chosen, &kDriverDate, &type, reinterpret_cast<BYTE*>(&time), sizeof time, nullptr, 0) &&
            type == DEVPROP_TYPE_FILETIME)
            out.released = ymd(time, false);
        if (SetupDiGetDevicePropertyW(set, &chosen, &kInstallDate, &type, reinterpret_cast<BYTE*>(&time), sizeof time, nullptr, 0) &&
            type == DEVPROP_TYPE_FILETIME) {
            out.installed = ymd(time, true);
            out.installed_at = (static_cast<ULONGLONG>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        }
        // AMD's own number (Adrenalin's), in the driver's key
        if (const HKEY key = SetupDiOpenDevRegKey(set, &chosen, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
            key != INVALID_HANDLE_VALUE) {
            wchar_t radeon[64] = {};
            DWORD size = sizeof radeon;
            if (RegGetValueW(key, nullptr, L"RadeonSoftwareVersion", RRF_RT_REG_SZ, nullptr, radeon, &size) == ERROR_SUCCESS &&
                radeon[0] != L'\0')
                out.shown = "Adrenalin " + narrow(radeon);
            RegCloseKey(key);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    // NVIDIA's own number: the last five digits of Windows' (32.0.16.1692: 616.92)
    if (out.shown.empty() && nvidia) {
        std::string digits;
        for (const char c : out.version)
            if (c >= '0' && c <= '9')
                digits += c;
        if (digits.size() >= 5)
            out.shown = digits.substr(digits.size() - 5, 3) + "." + digits.substr(digits.size() - 2);
    }
    if (out.shown.empty())
        out.shown = out.version.empty() ? "Unknown" : out.version;
    if (out.version.empty())
        log::write(L"Graphics: no driver found for %ls", name.c_str());
    else
        log::write(L"Graphics: driver %hs (%hs), dated %hs, installed %hs", out.shown.c_str(), out.version.c_str(),
                   out.released.c_str(), out.installed.c_str());
    return out;
}

// The card's driver; empty until its thread has found it
Driver driver(const std::string& card)
{
    std::lock_guard<std::mutex> lock(g_driver_lock);
    if (card.empty())
        return {};
    if (card != g_driver_card) {
        g_driver_card = card;
        g_driver = {};
        std::thread([card] {
            Driver found = find_driver(card);
            std::lock_guard<std::mutex> lock(g_driver_lock);
            if (g_driver_card == card)
                g_driver = std::move(found);
        }).detach();
    }
    return g_driver;
}

} // namespace

Info info()
{
    Info out;
    // what vanacore.ini says: d3d11 once the switch from d3d12 is written there, so /vana's restart alert stays away
    out.renderer = g_d3d11_for_12 && !g_d3d11_kept ? "d3d12" : g_d3d12 ? "d3d12" : g_d3d11 ? "d3d11" : g_d3d9 ? "d3d9" : "d3d8";
    out.d3d12_unsupported = g_active && g_d3d11 && g_d3d12_unsupported;
    out.running = g_active ? (g_d3d11 ? "d3d11" : "d3d12") : g_d3d9_active ? "d3d9" : "d3d8";
    out.active = g_active;
    out.effects = g_active;
    out.adapter = g_d3d9_active ? d3d9::adapter() : vanacore_gfx_adapter();
    const Driver card = driver(out.adapter);
    out.driver = card.shown;
    out.driver_version = card.version;
    out.driver_date = card.released;
    out.driver_installed = card.installed;
    if (card.installed_at != 0) {
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        const ULONGLONG at = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        out.driver_days = at > card.installed_at ? static_cast<int>((at - card.installed_at) / (86400ULL * 10000000ULL)) : 0;
    }
    if (g_active && !g_d3d11) {
        char message[256] = "";
        const int state = vanacore_gfx_dlss_state(message, sizeof message);
        // the upscaler's state (DLSS's or FSR's, whichever runs), the other's Off
        const std::string upscaling = state == 0   ? (g_dlss_gone ? g_dlss_gone_why : std::string("Off"))
                                      : state == 1 ? "Starting"
                                      : state == 2 ? std::string(message)
                                      : state == 3 ? std::string("Not available: ") + message
                                                   : std::string("Failed: ") + message;
        const bool fsr = g_upscaler == VANA_GFX_UPSCALER_FSR;
        out.dlss = fsr ? "Off" : upscaling;
        out.fsr = fsr ? upscaling : "Off";
        char fg[256] = "";
        const int fg_state = vanacore_gfx_framegen_state(fg, sizeof fg);
        if (fg_state == 1 && fg[0] >= 'a' && fg[0] <= 'z')
            fg[0] = static_cast<char>(fg[0] - 'a' + 'A');
        out.framegen = fg_state == 0   ? std::string("Off")
                       : fg_state <= 2 ? std::string(fg)
                       : fg_state == 3 ? std::string("Not available: ") + fg
                                       : std::string("Failed: ") + fg;
    } else if (g_active) {
        // Direct3D 11: FSR (the helper, on this device's shared textures), not DLSS or frame generation
        char message[256] = "";
        const int state = vanacore_gfx_dlss_state(message, sizeof message);
        out.fsr = state == 0   ? (g_dlss_gone ? g_dlss_gone_why : std::string("Off"))
                  : state == 1 ? "Starting"
                  : state == 2 ? std::string(message)
                  : state == 3 ? std::string("Not available: ") + message
                               : std::string("Failed: ") + message;
        out.dlss = "Needs DirectX 12";
        out.framegen = out.dlss;
    } else {
        out.dlss = "Needs DirectX 12";
        out.fsr = "Needs DirectX 11 or 12";
        out.framegen = out.dlss;
    }
    char why[200] = "";
    out.dlss_card = g_active && !g_d3d11 && vanacore_gfx_dlss_supported(why, sizeof why) != 0;
    out.dlss_card_why = out.dlss_card ? "" : g_active && !g_d3d11 ? why : "needs DirectX 12";
    // with DLSS off on a card it doesn't run on: why, in /vana's Resolution line under the locked switch
    if (!out.dlss_card && g_active && !g_d3d11 && out.dlss == "Off")
        out.dlss = "Not available: " + out.dlss_card_why;
    out.fsr_card = g_active && vanacore_gfx_fsr_supported(why, sizeof why) != 0;
    out.fsr_card_why = out.fsr_card ? "" : g_active ? why : "needs DirectX 11 or 12";
    if (!out.fsr_card && g_active && out.fsr == "Off")
        out.fsr = "Not available: " + out.fsr_card_why;
    out.upscaler = g_dlss < 0 ? "" : g_upscaler == VANA_GFX_UPSCALER_FSR ? "fsr" : "dlss";
    out.dlss5 = g_active && !g_d3d11 && vanacore_gfx_dlss5_supported(why, sizeof why) != 0;
    out.dlss5_why = out.dlss5 ? "" : g_active && !g_d3d11 ? why : "needs DirectX 12";
    if (g_active) {
        uint32_t w = 0, h = 0;
        vanacore_gfx_scene_size(&w, &h);
        out.scene_width = w, out.scene_height = h;
    }
    out.dlss_sized = g_active && g_dlss >= 0 && !g_dlss_gone;
    if (g_active && g_dlss >= 0 && g_dlss_gone)
        out.stopped = g_upscaler == VANA_GFX_UPSCALER_FSR ? "fsr" : "dlss";
    return out;
}

std::string scene_words()
{
    if (!g_active)
        return {};
    uint32_t w = 0, h = 0;
    vanacore_gfx_scene_size(&w, &h);
    if (w == 0 || h == 0)
        return {};
    char text[128];
    std::snprintf(text, sizeof text, "3D scene %ux%u (%s)", w, h,
                  g_dlss >= 0 && !g_dlss_gone ? (g_upscaler == VANA_GFX_UPSCALER_FSR ? "FSR's size" : "DLSS's size")
                  : g_background              ? "Background resolution"
                                              : "the launcher's background resolution");
    return text;
}

Waiting waiting()
{
    Waiting out;
    if (!g_active)
        return out;
    // the helper just lost: said for kLostToastMs, the bar running down, before anything waited on
    if (g_lost_at != 0) {
        const ULONGLONG since = GetTickCount64() - g_lost_at;
        if (since < kLostToastMs) {
            out.title = g_lost_title;
            out.detail = L"Switch it on again in /vana > Graphics to try again";
            out.alert = true;
            out.done = static_cast<unsigned>(kLostToastMs - since), out.total = static_cast<unsigned>(kLostToastMs);
            return out;
        }
        g_lost_at = 0;
    }
    const unsigned bits = vanacore_gfx_waiting();
    // with how far it has come (12 of 43; DLSS's start's stage of 4) and what it's at (the shader compiling, the helper
    // making DLSS for the textures) once the back end says
    const auto counted = [&out](unsigned bit, const wchar_t* title) {
        out.title = title;
        unsigned done = 0, total = 0;
        if (vanacore_gfx_wait_progress(bit, &done, &total) && total != 0)
            out.done = done, out.total = total;
        char words[96] = "";
        if (vanacore_gfx_wait_step(bit, words, sizeof words) && words[0])
            out.detail.assign(words, words + std::strlen(words)); // ASCII, the back end's own words
    };
    if (bits & VANA_GFX_WAIT_DLSS) {
        counted(VANA_GFX_WAIT_DLSS, bits & VANA_GFX_WAIT_DLSS5             ? L"Starting DLSS 5"
                                    : g_upscaler == VANA_GFX_UPSCALER_FSR ? L"Starting FSR"
                                                                          : L"Starting DLSS");
        if (out.detail.empty()) // FSR 1, in this process: no stages
            out.detail = L"The scene is drawn small meanwhile";
    } else if (bits & VANA_GFX_WAIT_FRAMEGEN)
        counted(VANA_GFX_WAIT_FRAMEGEN, L"Starting frame generation");
    else if (bits & VANA_GFX_WAIT_EFFECTS)
        counted(VANA_GFX_WAIT_EFFECTS, L"Compiling shaders");
    else if (bits & VANA_GFX_WAIT_SHADOWS)
        counted(VANA_GFX_WAIT_SHADOWS, L"Preparing shadows");
    return out;
}

void reload()
{
    g_ini_time = ini_time(); // this change is applied now: not again when on_frame sees the file changed
    apply_live(When::Quiet);
}

void retry()
{
    if (!g_dlss_gone)
        return;
    log::write(L"Graphics: %ls tried again (/vana)", g_upscaler == VANA_GFX_UPSCALER_FSR ? L"FSR" : L"DLSS");
    g_dlss_gone = false;
    g_dlss_gone_why.clear();
    apply_live(When::Quiet);
}

bool dlss_crash_test(int kind, std::string& why)
{
    char text[160] = "";
    if (!g_active || g_d3d11) {
        why = "needs DirectX 12";
        return false;
    }
    if (!vanacore_gfx_dlss_crash_test(kind, text, sizeof text)) {
        why = text;
        return false;
    }
    log::write(L"Graphics: DLSS crash test %d asked for (/vana debug): the helper fails at its next frame", kind);
    return true;
}

bool recompile_effects()
{
    if (!g_active || !vanacore_gfx_fx_rebuild())
        return false;
    log::write(L"Graphics: the scene effects' shaders are compiled again (Recompile shaders)");
    return true;
}

void own_drawing(bool on)
{
    if (g_active)
        vanacore_gfx_set_squeeze(on ? 0 : 1);
}

void own_pixels(bool on)
{
    if (g_active)
        vanacore_gfx_set_own_pixels(on ? 1 : 0);
}

bool plugin_wheel_over(int x, int y)
{
    return g_active && vanacore_gfx_plugin_wheel_over(x, y) != 0;
}

void set_mover(int slot, float ax, float ay, float scale)
{
    if (g_active)
        vanacore_gfx_set_mover(slot, ax, ay, scale);
}

bool mover_box(int slot, float box[4])
{
    return g_active && vanacore_gfx_mover_box(slot, box) != 0;
}

bool ui_size(unsigned& width, unsigned& height)
{
    if (!g_active)
        return false;
    uint32_t w = 0, h = 0;
    vanacore_gfx_screen_size(&w, &h);
    if (w == 0 || h == 0)
        return false;
    width = w, height = h;
    return true;
}

float ui_aspect()
{
    return g_active && g_ui_aspect > 0 ? g_ui_aspect : 0.0f;
}

bool ui_squeezed(unsigned ui_width, unsigned ui_height)
{
    return g_active && g_ui_aspect > 0 && vanacore_gfx_ui_squeezed(ui_width, ui_height) != 0;
}

bool ui_layer(unsigned& width, unsigned& height, std::vector<unsigned char>& pixels)
{
    uint32_t w = 0, h = 0;
    if (!g_active || !vanacore_gfx_ui_layer(&w, &h, nullptr, 0) || w == 0 || h == 0)
        return false;
    pixels.resize(static_cast<size_t>(w) * h * 4);
    if (!vanacore_gfx_ui_layer(&w, &h, pixels.data(), w * 4))
        return false;
    width = w, height = h;
    return true;
}

bool raise_take(int x, int y, int width, int height)
{
    if (!g_active)
        return false;
    vanacore_gfx_raise_take(x, y, width, height);
    return true;
}

void raise_put()
{
    if (g_active)
        vanacore_gfx_raise_put();
}

bool dlss_indicator()
{
    DWORD value = 0, size = sizeof value;
    return RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"ShowDlssIndicator",
                        RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
           value != 0;
}

void set_dlss_indicator(bool on)
{
    std::thread([on] {
        const wchar_t* args = on ? L"add \"HKLM\\SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore\" /v ShowDlssIndicator "
                                   L"/t REG_DWORD /d 1024 /f"
                                 : L"delete \"HKLM\\SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore\" /v ShowDlssIndicator /f";
        SHELLEXECUTEINFOW run{};
        run.cbSize = sizeof run;
        run.fMask = SEE_MASK_NOCLOSEPROCESS;
        run.lpVerb = L"runas";
        run.lpFile = L"reg.exe";
        run.lpParameters = args;
        run.nShow = SW_HIDE;
        if (!ShellExecuteExW(&run)) {
            log::write(L"Graphics: NVIDIA's DLSS indicator not %ls: %ls", on ? L"turned on" : L"turned off",
                       GetLastError() == ERROR_CANCELLED ? L"Windows' administrator prompt was declined"
                                                         : L"reg.exe didn't start");
            return;
        }
        if (run.hProcess) {
            WaitForSingleObject(run.hProcess, 15000);
            CloseHandle(run.hProcess);
        }
        log::write(dlss_indicator() ? L"Graphics: NVIDIA's DLSS indicator on, in every game that runs DLSS (from its next start)"
                                    : L"Graphics: NVIDIA's DLSS indicator off");
    }).detach();
}

void render_size(unsigned width, unsigned height)
{
    g_render_w = width, g_render_h = height;
    if (g_active)
        vanacore_gfx_set_render_size(width, height);
}

// [graphics] grass.brightness.auto: the grass's light at a minute of the Vana'diel day (0 to 1439): the whole of it at noon, `night`
// of it at midnight and halfway at 6:00 and 18:00, a little more or less every minute (a cosine over the day: never a sudden change)
float daylight(double minute, float night)
{
    const double day = 0.5 - 0.5 * std::cos(minute * (2.0 * 3.14159265358979 / 1440.0));
    return night + (1.0f - night) * static_cast<float>(day);
}

// The grass's light each frame (vanacore_gfx_set_grass_light): daylight's at the Vana'diel time while grass.brightness.auto is
// on, else the whole of it. The clock is the server's, as LandSandBoat keeps it (CVanaTime: 25 Vana'diel minutes to an Earth
// minute from Unix 1009810800), from the last buffer's header; the PC's before one comes or if the two are more than a day apart,
// as windower.lua's world().
void track_daylight()
{
    // The weather, said once when it changes (0x057's id, in Ashita's order: 0 clear, 1 sunny, 2 cloudy, 3 fog, then each element
    // single and double): a flicker that only some weather gives is found by its time in the log. Only once a packet has given it.
    {
        static std::uint32_t was = 0xFFFFFFFFu;
        static const wchar_t* const names[] = {L"clear",     L"sunny",      L"cloudy",       L"fog",        L"fire",
                                               L"fire x2",   L"water",      L"water x2",     L"earth",      L"earth x2",
                                               L"wind",      L"wind x2",    L"ice",          L"ice x2",     L"lightning",
                                               L"lightning x2", L"light",   L"light x2",     L"dark",       L"dark x2"};
        std::uint32_t server = 0, weather = 0;
        windower::state_world(server, weather);
        if (server != 0 && weather != was) {
            was = weather;
            log::write(L"Graphics: the weather is %ls (id %u)", weather < sizeof names / sizeof names[0] ? names[weather] : L"unknown",
                       static_cast<unsigned>(weather));
        }
    }
    std::uint32_t server = 0, weather = 0;
    windower::state_world(server, weather);
    vanacore_gfx_set_weather(weather, server != 0 ? 1 : 0); // the characters wet in the rain (d3d8_rain.h)
    double minute = vanaclock::server_minute(server); // to the millisecond, not in whole seconds' steps
    vanaclock::held_minute(minute); // the game's own hour while its timer is off: a cutscene's, the free camera's Time
    // the sun's shadows round sunrise and sunset (sun_dusk: 05:30 to 06:30, the game's light being the moon's going out until
    // 06:00; 17:30 to 18:30, the game's light turning from the sun to the moon at 18:00)
    vanacore_gfx_set_vana_minute(static_cast<float>(minute));
    vanacore_gfx_set_grass_light(g_grass_auto ? daylight(minute, g_grass_night) : 1.0f);
}

// Where the grass grows round in place of the player (vanacore_gfx_set_grass_centre): with grass.freecam, the free camera's eye
// while it's on (the view's world, as the focus), the grass growing round the ground under it; else round the player
void track_grass_centre()
{
    float pose[5];
    if (g_grass_freecam && camera::fly_pose(pose))
        vanacore_gfx_set_grass_centre(pose[0], pose[1], pose[2], 1);
    else
        vanacore_gfx_set_grass_centre(0.0f, 0.0f, 0.0f, 0);
}

// Whether the zone is lit by a fixed light alone, as its model DAT says (zonefloor::fixed_light), read once a zone and handed
// over each frame (vanacore_gfx_set_fixed_light). d3d8_sun.c otherwise judges by watching whether the zone's light moves with
// the clock, and Palborough Mines, whose every environment is a fixed light's, had the sun's shadows (a player, 2026-11-04).
// The model DAT as zonelines.cpp finds it: file id 100 + the zone below 0x100, 0x147B3 + (zone - 0x100) from there.
void track_fixed_light(unsigned zone)
{
    static unsigned looked = ~0u;
    static int fixed = 0;
    if (zone != looked) {
        looked = zone;
        std::vector<std::uint8_t> dat;
        const std::uint32_t file = zone < 0x100 ? 100u + zone : 0x147B3u + (zone - 0x100u);
        const int light = zone != 0 && dats::read(file, dat) ? zonefloor::fixed_light(dat) : -1;
        if ((light == 1) != (fixed != 0))
            log::write(light == 1 ? L"Graphics: zone %u is lit by a fixed light alone (its DAT, file %u): the sun's shadows scaled by moghouse there"
                                  : L"Graphics: zone %u isn't lit by a fixed light alone (its DAT, file %u): the sun's shadows as its light has them",
                       zone, file);
        fixed = light == 1 ? 1 : 0;
    }
    vanacore_gfx_set_fixed_light(fixed);
}

// Where the player stands, for the shadows' sharp map (vanacore_gfx_set_focus): their entity's position each
// frame, the party's first slot naming it (looked up twice a second). The entity's position is the world as
// the game draws it: x, then height (game::Entity's z), then y.
void track_focus()
{
    static unsigned index = 0;
    static std::uint32_t id = 0;
    static unsigned zone = 0;
    static ULONGLONG next = 0;
    const ULONGLONG now = GetTickCount64();
    if (now >= next) {
        next = now + 500;
        game::PartyMember members[18];
        game::Alliance alliance;
        const bool in = game::party(members, alliance) && members[0].active && members[0].target_index != 0;
        index = in ? members[0].target_index : 0;
        id = in ? members[0].id : 0;
        // a moment without the player in the party list (zoning, a lookup that fails once) keeps the zone: it goes to 0
        // (which makes the grass start over) only after five seconds of it
        static ULONGLONG lost_since = 0;
        if (in) {
            zone = members[0].zone;
            lost_since = 0;
        } else {
            if (lost_since == 0)
                lost_since = now;
            if (now - lost_since > 5000)
                zone = 0;
        }
    }
    if (ship_zone(zone) != g_ship_zone) {
        g_ship_zone = !g_ship_zone;
        log::write(g_ship_zone ? L"Graphics: zone %u is a ferry, the whole zone moves: the zone's shadows are left out there (sun_casters)"
                               : L"Graphics: zone %u isn't a ferry: sun_casters as the settings have it",
                   zone);
        apply_live(When::Quiet); // sun_casters and the game's own character shadows, as this zone has them
    }
    track_fixed_light(zone);
    track_rain_weathers(zone); // rain in thunder weather only where the zone has rain (graphics_rain.h)
    if (zone != g_grass_zone) { // the grass's settings as this zone has them (its dirt level and blended ground, grass_assets)
        g_grass_zone = zone;
        apply_grass(When::Quiet);
    }
    vanacore_gfx_set_grass_zone(static_cast<int>(zone));
    grass_assets::set_zone(static_cast<int>(zone));
    grass_floors::update(static_cast<int>(zone), g_grass_floors || g_snow_floors || g_water_floors || g_rain_shelter || (vanacore_gfx_floors_wanted() & 1) != 0); // (the rain's drops land on the floors)
    grass_floors::update_collision(static_cast<int>(zone), g_collision_show || g_rain_shelter); // (the rain's shelter reads it too)
    zone_casters::update(static_cast<int>(zone), g_effects || g_rain_shelter); // the zone's own meshes for the sun's shadows (sun_zone_mesh), and the rain's roofs
    game::Entity self;
    const bool known = index != 0 && game::entity(index, self) && self.id == id;
    vanacore_gfx_set_focus(known ? self.x : 0.0f, known ? self.z : 0.0f, known ? self.y : 0.0f, known ? 1 : 0);
    // ... and for the sun's shadows from what the camera doesn't see (casters.cpp)
    casters::update(known ? self.x : 0.0f, known ? self.z : 0.0f, known ? self.y : 0.0f, known);
    zonebatch::update(g_ship_zone); // the zone's draws of one mesh sent together ([graphics.effects] batch), a ferry's left alone
    // the characters and fixtures (home points, lamps) within 60 yalms: an effect on a character (a spell, an ability)
    // lights its surroundings at [graphics.effects] elight_actor's share; one on a fixture as any other
    static float places[256 * 4];
    vanacore_gfx_set_characters(places, index != 0 ? game::entity_places(places, 256, 60.0f) : 0);
}

void on_frame()
{
    fix_aspect();
    grass_assets::on_frame();
    if (g_active)
    {
        track_focus();
        track_grass_centre();
        track_daylight();
        zonelines::apply(g_zonelines);
        sparkles::apply(g_sparkles);
        vanacore_gfx_set_moghouse(packets::in_mog_house() ? 1 : 0); // the sun's shadows scaled by moghouse there
        vanacore_gfx_set_warping(packets::warping() ? 1 : 0);        // the glow effects lighting toned down (elight_warp)
        water::on_frame(); // the game's water models, known as it loads them (water.detect, debug=11)
    }
    lod::apply();
    distance::apply(g_distance);
    bumpmap::apply(g_bump);
    nameplates::apply(g_nameplates_fix, g_nameplates_spaced, g_nameplate_gap, window_shape(), g_names);
    damagetext::apply(g_damage);
    // the window's shape for [ui] aspect: the size it was made at, what Windows took of it for the title bar and borders
    // left out (window::made_size), so a windowed game whose title bar didn't fit the screen keeps its interface filling
    // the window, its edges on the window's
    if (g_active) {
        unsigned made_w = 0, made_h = 0;
        if (!window::made_size(made_w, made_h))
            made_w = made_h = 0;
        vanacore_gfx_set_window_size(made_w, made_h);
    }
    // [ui] aspect=game: the game's resolution's shape, once it has one and whenever it changes
    if (g_ui_game && g_active) {
        if (const float shape = game_shape(); shape > 0 && shape != g_ui_aspect) {
            g_ui_aspect = shape;
            vanacore_gfx_set_ui_aspect(shape);
            log::write(L"Graphics: the interface keeps the game's shape, %.3f:1 ([ui] aspect=game)", shape);
        }
    }
    const ULONGLONG now = GetTickCount64();
    if (now < g_next_check)
        return;
    g_next_check = now + 500;
    // frame generation and DLSS 5 from the first time a character is in the world (g_framegen_in_world)
    if (!g_framegen_in_world && !game::local_player_name().empty()) {
        g_framegen_in_world = true;
        if (g_framegen) {
            log::write(L"Graphics: frame generation starts now a character is in the world (held at the title screen)");
            vanacore_gfx_set_framegen(g_framegen);
        }
        if (g_dlss5) {
            log::write(L"Graphics: DLSS 5 starts now a character is in the world (held at the title screen)");
            vanacore_gfx_fx_set("dlss5", 1.0f);
        }
    }
    // DLSS or FSR that isn't there after all (DLSS: not an RTX card, an old driver; FSR: AMD's DLLs or an upscaler for the
    // card missing; no helper): the scene back at its size
    if (g_active && g_dlss >= 0 && !g_dlss_gone) {
        char message[256] = "";
        const int state = vanacore_gfx_dlss_state(message, sizeof message);
        if (state >= 3) {
            const bool fsr = g_upscaler == VANA_GFX_UPSCALER_FSR;
            g_dlss_gone = true;
            g_dlss_gone_why = (state == 3 ? std::string("Not available: ")
                                          : std::string(fsr ? "Stopped (switch FSR on again to try again): "
                                                            : "Stopped (switch DLSS on again to try again): ")) + message;
            if (state == 3)
                log::write(L"Graphics: %ls isn't available (%hs); the 3D scene drawn as without it ([graphics] %ls)",
                           fsr ? L"FSR" : L"DLSS", message, fsr ? L"fsr" : L"dlss");
            else
                log::write(L"Graphics: %ls stopped (%hs); the 3D scene drawn as without it till it's turned on again ([graphics] %ls)",
                           fsr ? L"FSR" : L"DLSS", message, fsr ? L"fsr" : L"dlss");
            apply_live(When::Quiet);
        }
    }
    // The DLSS helper crashed, ended or stopped answering: what it ran is off (above, and frame generation in the renderer),
    // and the player is told once in the chat, with why (the crash's code)
    if (g_active && !g_d3d11) {
        static int lost_told = 0;
        char why[320] = "";
        const int lost = vanacore_gfx_helper_lost(why, sizeof why);
        if (lost != lost_told) {
            lost_told = lost;
            log::write(L"Graphics: %hs; the player told in the chat", why);
            g_lost_at = GetTickCount64();
            g_lost_title = std::wstring(g_upscaler == VANA_GFX_UPSCALER_FSR ? L"FSR" : L"DLSS") +
                           (std::strstr(why, "crashed") != nullptr ? L" crashed" : L" stopped");
            chat::write(chat::kDefaultMode,
                        chat::header() + chat::colour(chat::kError, std::string(why) + ".") +
                            " Check its settings in /vana > Graphics, then switch it on again or restart the game "
                            "to try again." +
                            (log::enabled() ? " What happened is in logs\\vanacore.log." : ""),
                        "VanaCore");
        }
    }
    const FILETIME time = ini_time();
    if (CompareFileTime(&time, &g_ini_time) != 0) {
        g_ini_time = time;
        apply_live(When::Reload);
    }
}

LPARAM mouse(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    constexpr WPARAM kButtons = MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2;
    switch (message) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
        break;
    default:
        return lparam;
    }
    // a press keeps the mode it started in until the buttons are up (a window dragged over the world)
    const bool held = g_mouse_held;
    g_mouse_held = (wparam & kButtons) != 0;
    if (!g_active || !(g_ui_aspect > 0))
        return lparam;
    int x = GET_X_LPARAM(lparam), y = GET_Y_LPARAM(lparam);
    vanacore_gfx_unsqueeze(window, &x, &y, held ? 1 : 0);
    return MAKELPARAM(static_cast<WORD>(static_cast<short>(x)), static_cast<WORD>(static_cast<short>(y)));
}

} // namespace vanacore::graphics
