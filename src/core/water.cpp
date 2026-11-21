#include "water.hpp"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include "../gfx/vanacore_gfx.h"
#include "game.hpp"
#include "imports.hpp"
#include "log.hpp"
#include "packets.hpp"
#include "scan.hpp"

namespace vanacore::water {
namespace {

// A map model (a DAT chunk of type 0x2E) has loaded: its type's handler (FUN_10175990, 2025-12-26: slot 8 of the vtable at
// 0x10334550), __thiscall (resource), nothing on the stack. The resource: +0x20 its name (a FourCC), +0x24 its type (the low 7
// bits) and size, +0x28 a handle to its parent (*handle: the directory it's in; the root is its own), +0x30 its data. It makes
// the model, its vertex buffers among it, in one call: the model maker (FUN_10177db0, thiscall on the models' global: parent
// handle, data, name, size, an offset) at +0x7D, its result stored over the data and left in eax. XI on Anything's
// 2026-09-03 hooks "water_model_load" (0x10176ed0) and "water_model_loaded" (0x10176f52) are this function's start and the
// instruction after that call; hooking the whole function brackets the same.
// sub esp,10h; push ebx; mov ebx,[ecx+28h]; push ebp; mov ebp,[models]; mov dword ptr [esp+8..14h],0; mov eax,[ebp+3Ch];
// push esi; mov esi,[ecx+20h]; push edi
constexpr char kModelLoad[] = "83 EC 10 53 8B 59 28 55 8B 2D ?? ?? ?? ?? C7 44 24 08 00 00 00 00 C7 44 24 0C 00 00 00 00 "
                              "C7 44 24 10 00 00 00 00 C7 44 24 14 00 00 00 00 8B 45 3C 56 8B 71 20 57";
constexpr std::size_t kPrologue = 7;        // sub esp,10h; push ebx; mov ebx,[ecx+28h]
constexpr std::ptrdiff_t kMakeCall = 0x7D;  // call <the model maker>
constexpr std::ptrdiff_t kName = 0x20, kParent = 0x28;
constexpr int kLevels = 4; // the directories looked at above a model
// The directories the zone DATs keep their water models under (XI on Anything's): mizu and miz1 (water), umi1 (the sea), sea,
// taki (waterfalls), kawa (rivers), wat...
constexpr const char* kWaterNames[] = {"miz", "umi", "sea", "taki", "kawa", "wat"};
constexpr ULONGLONG kQuiet = 2000; // ms without a model loading before the count is logged
constexpr int kNames = 8;          // the water directories named in the log line

using ModelLoad = void*(__fastcall*)(char* resource, void* edx); // returns the model (left in eax)

bool g_tried = false;
bool g_wanted = false; // set_wanted: water.detect, the debug view or water.log
ModelLoad g_model_load = nullptr; // the game's, through the trampoline
std::atomic<bool> g_log{false};
// Any thread (the game may load on another): the map models loaded so far, the water ones, and when the last one loaded
std::atomic<unsigned> g_models{0};
std::atomic<unsigned> g_water{0};
std::atomic<ULONGLONG> g_last_load{0};
std::mutex g_names_lock;
std::uint32_t g_names[kNames] = {}; // the water directories seen since the last count logged

// The game's thread: what the last count logged had
ULONGLONG g_next = 0;
ULONGLONG g_counted_load = 0;
unsigned g_seen_models = 0, g_seen_water = 0, g_seen_buffers = 0;
int g_logged_zone = -1;

bool water_name(std::uint32_t name)
{
    char text[5] = {};
    std::memcpy(text, &name, 4);
    for (const char* prefix : kWaterNames)
        if (std::strncmp(text, prefix, std::strlen(prefix)) == 0)
            return true;
    return false;
}

std::wstring name_text(std::uint32_t name)
{
    std::wstring out;
    for (int i = 0; i < 4; ++i) {
        const auto c = static_cast<unsigned char>(name >> (i * 8));
        if (c == 0)
            break;
        out += c >= 0x20 && c < 0x7F ? static_cast<wchar_t>(c) : L'?';
    }
    while (!out.empty() && out.back() == L' ')
        out.pop_back();
    return out.empty() ? L"-" : out;
}

// The model's name, and the names of the directories above it, nearest first (0 past the root). False if they can't be read.
bool read_names(const char* resource, std::uint32_t* name, std::uint32_t* up)
{
    __try {
        *name = *reinterpret_cast<const std::uint32_t*>(resource + kName);
        const char* at = resource;
        for (int n = 0; n < kLevels; ++n) {
            const char* const* handle = *reinterpret_cast<const char* const* const*>(at + kParent);
            const char* parent = handle != nullptr ? *handle : nullptr;
            if (parent == nullptr || parent == at)
                break; // the root
            up[n] = *reinterpret_cast<const std::uint32_t*>(parent + kName);
            at = parent;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void remember(std::uint32_t name)
{
    const std::lock_guard lock(g_names_lock);
    for (std::uint32_t& slot : g_names) {
        if (slot == name)
            return;
        if (slot == 0) {
            slot = name;
            return;
        }
    }
}

// The game's map model loading: a model with a water directory above it loads with VanaCore's Direct3D 8 told, so the
// vertex buffers it makes are flagged as water (on this thread only)
void* __fastcall model_load_hook(char* resource, void* edx)
{
    std::uint32_t name = 0, up[kLevels] = {};
    std::uint32_t found = 0;
    if (resource != nullptr && read_names(resource, &name, up))
        for (const std::uint32_t dir : up)
            if (found == 0 && dir != 0 && water_name(dir))
                found = dir;
    ++g_models;
    g_last_load = GetTickCount64();
    if (found != 0) {
        ++g_water;
        remember(found);
    }
    if (g_log)
        log::write(L"Water: model %ls in %ls/%ls/%ls/%ls%ls", name_text(name).c_str(), name_text(up[3]).c_str(),
                   name_text(up[2]).c_str(), name_text(up[1]).c_str(), name_text(up[0]).c_str(), found != 0 ? L": water" : L"");
    if (found == 0)
        return g_model_load(resource, edx);
    // set for this load and put back after it, not cleared: a load inside another (none known) keeps the outer one's
    const int was = vanacore_gfx_water_loading(1);
    void* const model = g_model_load(resource, edx);
    vanacore_gfx_water_loading(was);
    return model;
}

void install()
{
    g_tried = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t at = game != nullptr ? scan::find(game, kModelLoad) : 0;
    if (at == 0 || *reinterpret_cast<const std::uint8_t*>(at + kMakeCall) != 0xE8) {
        log::write(L"Water: the game's map model loading wasn't found in FFXiMain.dll: its water models aren't known "
                   L"([graphics] water.detect=1 finds no water; the shape test is as before)");
        return;
    }
    if (!imports::detour(reinterpret_cast<ModelLoad>(at), kPrologue, &model_load_hook, g_model_load)) {
        log::write(L"Water: the game's map model loading couldn't be hooked: its water models aren't known");
        return;
    }
    log::write(L"Water: the game's water models are known as they load (map model loading hooked at FFXiMain.dll+0x%X)",
               static_cast<unsigned>(at - reinterpret_cast<std::uintptr_t>(game)));
}

int zone_now()
{
    game::PartyMember members[18];
    game::Alliance alliance;
    const std::uint16_t listed = game::party(members, alliance) && members[0].active ? members[0].zone : 0;
    return packets::player_zone(listed);
}

// Once the loading has gone quiet: how many of the models loaded since the last count were water, and the vertex buffers
// flagged for them. Once a zone, and again in the same zone only for more water.
void log_count()
{
    const ULONGLONG now = GetTickCount64();
    if (now < g_next)
        return;
    g_next = now + 500;
    const ULONGLONG last = g_last_load;
    if (last == 0 || last == g_counted_load || now - last < kQuiet)
        return;
    g_counted_load = last;
    const unsigned models = g_models, water = g_water, buffers = vanacore_gfx_water_buffers();
    const unsigned new_models = models - g_seen_models, new_water = water - g_seen_water, new_buffers = buffers - g_seen_buffers;
    g_seen_models = models, g_seen_water = water, g_seen_buffers = buffers;
    std::wstring names;
    {
        const std::lock_guard lock(g_names_lock);
        for (std::uint32_t& name : g_names) {
            if (name != 0)
                names += (names.empty() ? L"" : L", ") + name_text(name);
            name = 0;
        }
    }
    const int zone = zone_now();
    if (zone == g_logged_zone && new_water == 0)
        return;
    const bool again = zone == g_logged_zone;
    g_logged_zone = zone;
    if (new_water == 0)
        log::write(L"Water: zone %d: none of the %u map models loaded were water", zone, new_models);
    else
        log::write(L"Water: zone %d: %u of the %u map models loaded%ls were water (under %ls), %u vertex buffers flagged", zone,
                   new_water, new_models, again ? L" since" : L"", names.c_str(), new_buffers);
}

} // namespace

void on_frame()
{
    if (!g_tried && g_wanted)
        install();
    if (g_model_load != nullptr && g_wanted)
        log_count();
}

void set_log(bool on)
{
    g_log = on;
}

void set_wanted(bool on)
{
    g_wanted = on;
}

} // namespace vanacore::water
