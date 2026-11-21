#include "grass_assets.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../gfx/vanacore_gfx.h"
#include "dat_textures.hpp"
#include "dats.hpp"
#include "image.hpp"
#include "json.hpp"
#include "log.hpp"
#include "pivot.hpp"

namespace vanacore::grass_assets {
namespace {

constexpr unsigned kMaxAssets = 64; // grass.c ASSETMAX
constexpr unsigned kDrawn = 2;      // grass.c DRAWN: indices 0 and 1 are the drawn blades, type 1 and type 2
constexpr unsigned kMaxSprites = 32;
constexpr std::uint64_t kSearchBytes = 3ull << 30; // a capture reads at most this much of the game's DATs looking for its sheets

struct Asset {
    Info info;
    std::wstring folder;             // where its .json (and picture) are
    std::string picture;             // a PNG beside the .json: a capture no game file matched (the player's own)
    std::string dat, texture;        // or a recipe: the DAT under the FFXI folder ("ROM2/22/4.DAT") and its texture's name
    unsigned size_w = 0, size_h = 0; // the size the sprites' rectangles are in (another size of the texture scales them)
    float height = 1.0f;
    std::vector<float> rects;        // x0 y0 x1 y1 each
};

std::mutex g_lock; // list() is asked from the Lua state, apply() runs with the settings
std::vector<Asset> g_assets; // [0] and [1] the drawn blades
bool g_loaded = false;
bool g_waiting = false; // recipes wait for the game's folder (FFXiMain.dll loaded) and for Mass Grass on: read again then
bool g_on = false;      // [graphics] grass, as apply() last saw it
bool g_sent_any = false; // pictures have been handed to the grass (to clear the ones a reload drops)
unsigned g_sent = 0;
std::wstring g_ini, g_dir;
int g_zone = 0;

// A capture's sheet, and where in the game's files it was found.
struct Sheet {
    unsigned long long hash = 0;
    unsigned w = 0, h = 0;
    std::vector<std::uint32_t> argb;
    std::vector<float> rects;
    std::string dat, texture;
};
struct Search {
    std::vector<Sheet> sheets;
    std::atomic<bool> done{false};
    unsigned files = 0;
};

// a capture under way, and what to say about the last
bool g_capturing = false;
std::string g_capture_name;
int g_capture_zone = 0;
ULONGLONG g_capture_started = 0;
std::shared_ptr<Search> g_search; // its sheets looked for in the game's DATs (a thread)
std::string g_status;

std::wstring widen(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n > 0 ? n : 0, L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& s)
{
    if (s.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n : 0, '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool read_text(const std::wstring& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool write_text(const std::wstring& path, const std::string& text)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    f << text;
    return static_cast<bool>(f);
}

// The zone's model DAT: file id 100 + the zone below 0x100, 0x147B3 + (zone - 0x100) from there (as zonelines.cpp).
std::uint32_t zone_model(int zone)
{
    return zone < 0x100 ? 100u + zone : 0x147B3u + (zone - 0x100u);
}

// A recipe's DAT as a path under the FFXI folder ("ROM2\22\4.DAT"); empty unless it's ROM<n>\<folder>\<file>.DAT.
std::wstring dat_path(const std::string& dat)
{
    const auto digits = [](const wchar_t* from, const wchar_t* to) { return from < to && std::all_of(from, to, [](wchar_t c) { return c >= L'0' && c <= L'9'; }); };
    std::wstring path = widen(dat);
    std::replace(path.begin(), path.end(), L'/', L'\\');
    int parts = 0;
    for (std::size_t at = 0; at <= path.size(); ++parts) {
        const std::size_t end = std::min(path.find(L'\\', at), path.size());
        const std::wstring part = path.substr(at, end - at);
        const wchar_t* b = part.c_str();
        const wchar_t* e = b + part.size();
        const bool ok = parts == 0   ? part.size() >= 3 && _wcsnicmp(b, L"ROM", 3) == 0 && (part.size() == 3 || digits(b + 3, e))
                        : parts == 1 ? digits(b, e)
                        : parts == 2 ? part.size() > 4 && _wcsicmp(e - 4, L".DAT") == 0 && digits(b, e - 4)
                                     : false;
        if (!ok)
            return {};
        at = end + 1;
    }
    return parts == 3 ? path : std::wstring();
}

// "rom2\22\4.dat" as a recipe writes it: "ROM2/22/4.DAT"
std::string dat_name(const std::wstring& path)
{
    std::string out = narrow(path);
    for (char& c : out)
        c = c == '\\' ? '/' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// the .json files of a folder, sorted by name
std::vector<std::wstring> json_files(const std::wstring& folder)
{
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW found;
    HANDLE h = FindFirstFileW((folder + L"*.json").c_str(), &found);
    if (h == INVALID_HANDLE_VALUE)
        return names;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            names.push_back(found.cFileName);
    } while (FindNextFileW(h, &found));
    FindClose(h);
    std::sort(names.begin(), names.end());
    return names;
}

void read_folder(const std::wstring& folder, bool shipped, std::vector<Asset>& out)
{
    for (const std::wstring& file : json_files(folder)) {
        if (out.size() >= kMaxAssets)
            break;
        std::string text, error;
        json::Value v;
        if (!read_text(folder + file, text) || !json::parse(text, v, error) || !v.is_object()) {
            log::write(L"Graphics: grass: %ls%ls isn't a catalogue entry (%hs)", folder.c_str(), file.c_str(), error.c_str());
            continue;
        }
        Asset a;
        a.info.id = narrow(file.substr(0, file.size() - 5));
        a.info.name = v.get_string("name", a.info.id);
        a.info.kind = v.get_string("kind", "plant");
        a.info.shipped = shipped;
        a.folder = folder;
        a.dat = v.get_string("dat");
        if (!a.dat.empty())
            a.texture = v.get_string("texture");
        else
            a.picture = v.get_string("picture");
        a.info.picture = !a.picture.empty();
        if (const json::Value* size = v.find("size"); size != nullptr && size->is_array() && size->array.size() == 2)
            a.size_w = static_cast<unsigned>(size->array[0].number), a.size_h = static_cast<unsigned>(size->array[1].number);
        a.height = static_cast<float>(v.get_number("height", 1.0));
        if (const json::Value* sprites = v.find("sprites"); sprites != nullptr && sprites->is_array())
            for (const json::Value& r : sprites->array)
                if (r.is_array() && r.array.size() == 4 && a.rects.size() / 4 < kMaxSprites)
                    for (const json::Value& n : r.array)
                        a.rects.push_back(static_cast<float>(n.number));
        if ((a.picture.empty() && (a.dat.empty() || a.texture.empty())) || a.rects.empty()) {
            log::write(L"Graphics: grass: %ls%ls has no picture or game texture, or no sprites; left out", folder.c_str(), file.c_str());
            continue;
        }
        if (!a.dat.empty() && dat_path(a.dat).empty()) {
            log::write(L"Graphics: grass: %ls%ls's dat %hs isn't ROM<n>/<folder>/<file>.DAT; left out", folder.c_str(), file.c_str(),
                       a.dat.c_str());
            continue;
        }
        out.push_back(std::move(a));
    }
}

// A recipe's sheet from the player's own game files (through pivot's overlays, as the game reads them). False with `why`.
bool recipe_pixels(const Asset& a, const std::wstring& ffxi, std::map<std::wstring, std::vector<std::uint8_t>>& read,
                   image::Pixels& px, std::string& why)
{
    const std::wstring rel = dat_path(a.dat);
    auto [it, fresh] = read.try_emplace(rel);
    if (fresh && !dats::read_file(pivot::resolve(ffxi + rel), it->second))
        it->second.clear();
    if (it->second.empty()) {
        why = "isn't in the game's files";
        return false;
    }
    dat_textures::Header h;
    if (!dat_textures::find(it->second, a.texture, h)) {
        why = "has no texture \"" + a.texture + "\"";
        return false;
    }
    if (!dat_textures::decode(it->second, h, px.argb)) {
        why = "has texture \"" + a.texture + "\" in a form VanaCore can't read";
        return false;
    }
    px.width = static_cast<int>(h.width), px.height = static_cast<int>(h.height);
    return true;
}

// The drawn blades (grass.c makes them), at indices 0 and 1: type 1, the first broad blades, and type 2, the finer ones. Type 2
// keeps the id the drawn blades always had ("blades", on until set); type 1 is off until switched on.
std::vector<Info> drawn_blades()
{
    return { { "blades1", "Blade grass Type 1 (drawn)", "grass", true },
             { "blades", "Blade grass Type 2 (drawn)", "grass", true } };
}

// The catalogue read again, its pictures handed to the grass (indices kDrawn on; the ones no longer there cleared).
// The zones' dirt levels (dirt()): zone id to level, the level of a zone the file doesn't list, and whether it's been read;
// the zones whose blended ground grows grass too (blended())
std::map<int, float> g_dirt;
float g_dirt_rest = 50.0f;
bool g_dirt_read = false;
std::set<int> g_blended;
std::map<int, std::string> g_ground; // ground(): the zones' own grass ground textures
std::set<int> g_bare;                 // grows(): the zones with no grass at all

// addons\vanacore\vana\data\grass_dirt.json, VanaCore's own: { "default": 50, "zones": { "238": { "name": "Windurst Waters",
// "dirt": 60 }, "121": { "name": "The Sanctuary of Zi'Tah", "blended": true, "ground": [ "65b719e367503797" ] }, "245": {
// "name": "Lower Jeuno", "grass": false }, ... } } (a zone's entry may be the level alone; "ground" the textures that are grass
// there whatever their colours say; "grass": false none grows there). Levels 1 to 100; without the file every zone is 50, grows
// grass and none grows on blended ground
void read_dirt()
{
    g_dirt.clear(), g_dirt_rest = 50.0f, g_dirt_read = true, g_blended.clear(), g_ground.clear(), g_bare.clear();
    const std::wstring path = g_dir + L"\\addons\\vanacore\\vana\\data\\grass_dirt.json";
    std::string text, error;
    json::Value v;
    if (!read_text(path, text)) {
        log::write(L"Graphics: grass: no %ls: every zone's dirt level is 50", path.c_str());
        return;
    }
    if (!json::parse(text, v, error) || !v.is_object()) {
        log::write(L"Graphics: grass: %ls isn't a JSON object (%hs): every zone's dirt level is 50", path.c_str(), error.c_str());
        return;
    }
    const auto level = [](double x) { return static_cast<float>(std::clamp(x, 1.0, 100.0)); };
    g_dirt_rest = level(v.get_number("default", 50.0));
    if (const json::Value* zones = v.find("zones"); zones != nullptr && zones->is_object())
        for (const auto& [key, z] : zones->object) {
            const int id = std::atoi(key.c_str());
            const json::Value* dirt = z.is_object() ? z.find("dirt") : &z;
            if (id > 0 && dirt != nullptr && dirt->is_number())
                g_dirt[id] = level(dirt->number);
            const json::Value* layered = z.is_object() ? z.find("blended") : nullptr;
            if (id > 0 && layered != nullptr && layered->is_bool() && layered->boolean)
                g_blended.insert(id);
            if (const json::Value* grows = z.is_object() ? z.find("grass") : nullptr; id > 0 && grows != nullptr && grows->is_bool() && !grows->boolean)
                g_bare.insert(id);
            if (const json::Value* ground = z.is_object() ? z.find("ground") : nullptr; id > 0 && ground != nullptr && ground->is_array())
                for (const json::Value& t : ground->array)
                    if (t.is_string())
                        g_ground[id] += (g_ground[id].empty() ? "" : " ") + t.string;
        }
    log::write(L"Graphics: grass: dirt levels for %u zones, %g for the rest; %u zones grow on blended ground, %u none at all (data\\grass_dirt.json)",
               static_cast<unsigned>(g_dirt.size()), g_dirt_rest, static_cast<unsigned>(g_blended.size()), static_cast<unsigned>(g_bare.size()));
}

void load()
{
    std::vector<Asset> assets;
    for (const Info& info : drawn_blades()) {
        Asset blades;
        blades.info = info;
        assets.push_back(blades);
    }
    read_folder(g_dir + L"\\addons\\vanacore\\vana\\grass\\", true, assets);
    read_folder(g_dir + L"\\config\\grass\\", false, assets);
    const std::wstring ffxi = dats::ffxi_folder();
    std::map<std::wstring, std::vector<std::uint8_t>> read; // the DATs recipes have read, once each
    unsigned sent = kDrawn, recipes = 0, pictures = 0;
    g_waiting = false;
    for (std::size_t i = kDrawn; i < assets.size(); ++i) {
        const Asset& a = assets[i];
        image::Pixels px;
        std::vector<float> rects = a.rects;
        if (!a.dat.empty()) {
            std::string why;
            if (ffxi.empty() || !g_on) { // the game isn't loaded yet or no grass grows: read again when it is and does
                g_waiting = true;
                vanacore_gfx_grass_asset(static_cast<unsigned>(i), nullptr, 0, 0, nullptr, 0, 1.0f);
                continue;
            }
            if (!recipe_pixels(a, ffxi, read, px, why)) {
                log::write(L"Graphics: grass: %ls: %hs %hs; left out", widen(a.info.id).c_str(), a.dat.c_str(), why.c_str());
                vanacore_gfx_grass_asset(static_cast<unsigned>(i), nullptr, 0, 0, nullptr, 0, 1.0f);
                continue;
            }
            // the rectangles were found on a texture of `size` (the HD overlay's): this one may be smaller
            if (a.size_w > 0 && a.size_h > 0 && (static_cast<unsigned>(px.width) != a.size_w || static_cast<unsigned>(px.height) != a.size_h))
                for (std::size_t k = 0; k < rects.size(); ++k)
                    rects[k] *= k % 2 == 0 ? static_cast<float>(px.width) / a.size_w : static_cast<float>(px.height) / a.size_h;
            ++recipes;
        } else {
            if (!image::decode_file(a.folder + widen(a.picture), px) || px.width <= 0 || px.height <= 0) {
                log::write(L"Graphics: grass: %ls's picture %hs can't be read; left out", widen(a.info.id).c_str(), a.picture.c_str());
                vanacore_gfx_grass_asset(static_cast<unsigned>(i), nullptr, 0, 0, nullptr, 0, 1.0f);
                continue;
            }
            ++pictures;
        }
        vanacore_gfx_grass_asset(static_cast<unsigned>(i), px.argb.data(), static_cast<unsigned>(px.width), static_cast<unsigned>(px.height),
                                 rects.data(), static_cast<unsigned>(rects.size() / 4), a.height);
        // flowers keep their own colours with grass.match
        vanacore_gfx_grass_flower(static_cast<unsigned>(i), a.info.kind == "flower" ? 1 : 0);
        sent = static_cast<unsigned>(i) + 1;
    }
    for (unsigned i = static_cast<unsigned>(assets.size()); g_sent_any && i < g_sent; ++i)
        vanacore_gfx_grass_asset(i, nullptr, 0, 0, nullptr, 0, 1.0f);
    g_sent = std::max(sent, static_cast<unsigned>(assets.size()));
    g_sent_any = true;
    std::lock_guard<std::mutex> lock(g_lock);
    g_assets = std::move(assets);
    g_loaded = true;
    if (g_waiting)
        log::write(L"Graphics: grass: %u assets in the catalogue; the ones from the game's files wait for the game and the grass",
                   static_cast<unsigned>(g_assets.size()));
    else
        log::write(L"Graphics: grass: %u assets in the catalogue (the drawn blades' two types, %u from the game's files, %u pictures)",
                   static_cast<unsigned>(g_assets.size()), recipes, pictures);
}

void send_weights()
{
    std::lock_guard<std::mutex> lock(g_lock);
    for (std::size_t i = 0; i < g_assets.size(); ++i) {
        const std::wstring key = L"grass.asset." + widen(g_assets[i].info.id);
        const bool blades = g_assets[i].info.id == "blades"; // the drawn blades of type 2: on, 100 until set; the rest 0
        const bool on = GetPrivateProfileIntW(L"graphics", key.c_str(), blades ? 1 : 0, g_ini.c_str()) != 0;
        wchar_t text[32] = {};
        GetPrivateProfileStringW(L"graphics", (key + L".amount").c_str(), blades ? L"100" : L"0", text, 32, g_ini.c_str());
        float amount = static_cast<float>(_wtof(text));
        amount = amount < 0.0f ? 0.0f : amount > 1000.0f ? 1000.0f : amount;
        vanacore_gfx_grass_weight(static_cast<unsigned>(i), on ? amount : 0.0f);
    }
}

std::string hex16(unsigned long long v)
{
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", v);
    return b;
}

// The game's upload of a texture and the same texture decoded from its DAT: alike up to the decoders' rounding (and a
// palette's colours put in 16 bits). Most of the pixels are tested first, the rest only when those agree.
bool same_picture(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b)
{
    if (a.size() != b.size() || a.empty())
        return false;
    std::uint64_t diff = 0, opaque = 0, alpha_off = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const std::uint32_t p = a[i], q = b[i];
        const bool pa = (p >> 24) >= 128, qa = (q >> 24) >= 128;
        if (pa != qa)
            ++alpha_off;
        else if (pa) {
            diff += std::abs(static_cast<int>((p >> 16) & 255) - static_cast<int>((q >> 16) & 255)) +
                    std::abs(static_cast<int>((p >> 8) & 255) - static_cast<int>((q >> 8) & 255)) +
                    std::abs(static_cast<int>(p & 255) - static_cast<int>(q & 255));
            ++opaque;
        }
        if ((i & 4095) == 4095 && (alpha_off * 50 > i || (opaque > 0 && diff > opaque * 24)))
            return false;
    }
    return opaque > 0 && alpha_off * 100 <= a.size() && diff <= opaque * 24;
}

// The thread that finds a capture's sheets in the game's files: the zone's model DAT, then the DATs the game opened
// lately (its sub-areas', the models around), the newest first, until every sheet is found.
void search(std::shared_ptr<Search> s, int zone)
{
    const std::wstring ffxi = dats::ffxi_folder();
    std::vector<std::wstring> files;
    std::wstring zone_dat = dats::rom_path(zone_model(zone));
    for (wchar_t& c : zone_dat)
        c = static_cast<wchar_t>(std::towlower(c));
    if (!zone_dat.empty())
        files.push_back(zone_dat);
    for (std::wstring& f : pivot::opened_dats())
        if (f != zone_dat)
            files.push_back(std::move(f));
    std::uint64_t bytes = 0;
    std::vector<std::uint8_t> data;
    std::vector<std::uint32_t> px;
    for (const std::wstring& rel : files) {
        if (ffxi.empty() || bytes > kSearchBytes ||
            std::all_of(s->sheets.begin(), s->sheets.end(), [](const Sheet& sh) { return !sh.dat.empty(); }))
            break;
        if (!dats::read_file(pivot::resolve(ffxi + rel), data))
            continue;
        bytes += data.size();
        ++s->files;
        for (const dat_textures::Header& h : dat_textures::list(data)) {
            bool decoded = false;
            for (Sheet& sh : s->sheets) {
                if (!sh.dat.empty() || h.width != sh.w || h.height != sh.h)
                    continue;
                if (!decoded && !(decoded = dat_textures::decode(data, h, px)))
                    break;
                if (same_picture(px, sh.argb)) {
                    sh.dat = dat_name(rel);
                    sh.texture = dat_textures::trimmed(h.name);
                }
            }
        }
    }
    s->done = true;
}

// A finished capture into the library: each sheet a recipe (the DAT and texture it's in), or, when no game file has
// it, a PNG of its own. A .json already there is kept: it may have been edited.
void write_capture(const Search& s)
{
    const std::wstring folder = g_dir + L"\\config\\grass\\";
    CreateDirectoryW((g_dir + L"\\config").c_str(), nullptr);
    CreateDirectoryW(folder.c_str(), nullptr);
    SYSTEMTIME now;
    GetLocalTime(&now);
    char date[16];
    std::snprintf(date, sizeof date, "%04u-%02u-%02u", now.wYear, now.wMonth, now.wDay);
    int added = 0, known = 0, failed = 0, pictures = 0;
    const int count = static_cast<int>(s.sheets.size());
    for (int i = 0; i < count; ++i) {
        const Sheet& sh = s.sheets[i];
        const std::string stem = "z" + std::to_string(g_capture_zone) + "-" + hex16(sh.hash);
        const std::wstring json_path = folder + widen(stem) + L".json";
        if (GetFileAttributesW(json_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            ++known;
            continue;
        }
        json::Value v = json::Value::make_object();
        std::string name = g_capture_name.empty() ? "Zone " + std::to_string(g_capture_zone) + " plants" : g_capture_name;
        if (count > 1)
            name += " " + std::to_string(i + 1);
        v["name"] = name;
        v["kind"] = "plant";
        if (!sh.dat.empty()) {
            v["dat"] = sh.dat;
            v["texture"] = sh.texture;
        } else {
            if (!vanacore_gfx_write_png((folder + widen(stem) + L".png").c_str(), sh.argb.data(), sh.w, sh.h)) {
                ++failed;
                continue;
            }
            v["picture"] = stem + ".png";
            ++pictures;
        }
        json::Value size = json::Value::make_array();
        size.push(static_cast<double>(sh.w));
        size.push(static_cast<double>(sh.h));
        v["size"] = std::move(size);
        v["height"] = 1.0;
        json::Value sprites = json::Value::make_array();
        for (std::size_t k = 0; k + 3 < sh.rects.size(); k += 4) {
            json::Value r = json::Value::make_array();
            for (int j = 0; j < 4; ++j)
                r.push(static_cast<double>(sh.rects[k + j]));
            sprites.push(std::move(r));
        }
        v["sprites"] = std::move(sprites);
        v["zone"] = g_capture_zone;
        v["hash"] = hex16(sh.hash);
        v["captured"] = date;
        if (write_text(json_path, json::dump(v)))
            ++added;
        else
            ++failed;
    }
    char msg[320];
    if (count == 0)
        std::snprintf(msg, sizeof msg, "No plant sprites near you to capture (stand by grass or shrubs; Mass Grass has to be on).");
    else
        std::snprintf(msg, sizeof msg, "Captured %d plant sheet%s into config\\grass%s%s%s.", added, added == 1 ? "" : "s",
                      pictures == 0 ? "" : pictures == added ? " (not found in your game files: kept as your own pictures)"
                                                             : " (some not found in your game files: kept as your own pictures)",
                      known ? ", others were already in your library" : "", failed ? ", some couldn't be written" : "");
    g_status = msg;
    log::write(L"Graphics: grass: %hs (%u game files looked through)", msg, s.files);
    for (const Sheet& sh : s.sheets)
        if (!sh.dat.empty())
            log::write(L"Graphics: grass: sheet %hs is %hs's \"%hs\"", hex16(sh.hash).c_str(), sh.dat.c_str(), sh.texture.c_str());
    if (added > 0)
        reload();
}

} // namespace

void apply(const std::wstring& ini, const std::wstring& dir)
{
    g_ini = ini, g_dir = dir;
    g_on = GetPrivateProfileIntW(L"graphics", L"grass", 0, ini.c_str()) != 0;
    if (!g_loaded || (g_waiting && g_on && !dats::ffxi_folder().empty()))
        load();
    send_weights();
}

void reload()
{
    g_dirt_read = false;
    if (g_dir.empty()) {
        g_loaded = false;
        return;
    }
    load();
    send_weights();
}

float dirt(int zone)
{
    if (g_dir.empty())
        return 50.0f;
    if (!g_dirt_read)
        read_dirt();
    const auto found = g_dirt.find(zone);
    return found != g_dirt.end() ? found->second : g_dirt_rest;
}

bool blended(int zone)
{
    if (g_dir.empty())
        return false;
    if (!g_dirt_read)
        read_dirt();
    return g_blended.count(zone) != 0;
}

std::string ground(int zone)
{
    if (g_dir.empty())
        return {};
    if (!g_dirt_read)
        read_dirt();
    const auto found = g_ground.find(zone);
    return found != g_ground.end() ? found->second : std::string();
}

bool grows(int zone)
{
    if (g_dir.empty())
        return true;
    if (!g_dirt_read)
        read_dirt();
    return g_bare.count(zone) == 0;
}

std::vector<Info> list()
{
    std::lock_guard<std::mutex> lock(g_lock);
    std::vector<Info> out;
    for (const Asset& a : g_assets)
        out.push_back(a.info);
    if (out.empty())
        out = drawn_blades();
    return out;
}

bool capture(const std::string& name, std::string& message)
{
    if (g_capturing || g_search) {
        message = "A capture is already running.";
        return false;
    }
    if (g_ini.empty() || GetPrivateProfileIntW(L"graphics", L"grass", 0, g_ini.c_str()) == 0) {
        message = "Turn Mass Grass on first (Graphics tab): the capture looks at what it sees.";
        return false;
    }
    g_capturing = true;
    g_capture_name = name;
    g_capture_zone = g_zone;
    g_capture_started = GetTickCount64();
    vanacore_gfx_grass_capture();
    message = "Capturing the plants around you, then finding them in your game files...";
    return true;
}

void on_frame()
{
    if (g_waiting && g_on && !g_dir.empty() && !dats::ffxi_folder().empty())
        reload();
    if (g_search) {
        if (!g_search->done)
            return;
        const std::shared_ptr<Search> s = std::move(g_search);
        write_capture(*s);
        return;
    }
    if (!g_capturing)
        return;
    const int count = vanacore_gfx_grass_capture_count();
    if (count < 0) {
        if (GetTickCount64() - g_capture_started > 10000) { // the scene never ran (DirectX 8 or 9, or no 3D)
            g_capturing = false;
            g_status = "The capture didn't finish: it needs the game drawn with DirectX 11 or 12.";
            vanacore_gfx_grass_capture_clear(); // stop the gfx side hashing and noting every 3D draw
        }
        return;
    }
    g_capturing = false;
    auto s = std::make_shared<Search>();
    for (int i = 0; i < count; ++i) {
        Sheet sh;
        unsigned n = 0;
        const std::uint32_t* argb = nullptr;
        float rects[kMaxSprites * 4];
        if (!vanacore_gfx_grass_capture_result(static_cast<unsigned>(i), &sh.hash, &sh.w, &sh.h, &argb, rects, &n) || argb == nullptr)
            continue;
        sh.argb.assign(argb, argb + static_cast<std::size_t>(sh.w) * sh.h);
        sh.rects.assign(rects, rects + std::min(n, kMaxSprites) * 4);
        s->sheets.push_back(std::move(sh));
    }
    vanacore_gfx_grass_capture_clear();
    if (s->sheets.empty()) {
        write_capture(*s);
        return;
    }
    g_search = s;
    std::thread(search, s, g_capture_zone).detach();
}

std::string take_status()
{
    std::string s;
    s.swap(g_status);
    return s;
}

void set_zone(int zone)
{
    g_zone = zone;
}

} // namespace vanacore::grass_assets
