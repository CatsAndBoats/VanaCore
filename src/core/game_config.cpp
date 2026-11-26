#include "game_config.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include <string>

#include "graphics.hpp"
#include "ini.hpp"
#include "log.hpp"
#include "scan.hpp"

namespace vanacore::game_config {
namespace {

// FsConfig::getConfig (FFXiMain.dll): mov ecx, [config]; test ecx, ecx; jz; push [esp+4]; call
// findConfigSubject; ret. The global it reads is the config object: +4 its table of options.
constexpr char kGetConfig[] = "8B 0D ?? ?? ?? ?? 85 C9 74 0B 8B 44 24 04 50 E8 ?? ?? ?? ?? C3 83 C8 FF C3";
constexpr int kOptions = 0x2394 / 0x2c; // as the game's save loop walks them
constexpr std::size_t kEntry = 0x2c;    // +4 the option's number, +8 its value, +0xc its type
constexpr int kSavedType = 4;           // written to cnf.dat
constexpr int kManyAtOnce = 8;          // more changed in one look: a config loaded, said as one line

// FsConfig::changeConfig's wrapper: mov ecx, [config]; test ecx, ecx; jz; push value; push option;
// call; ret. cdecl (option, value): the value checked against the option's range and applied, as the
// Config menu does.
constexpr char kSetConfig[] = "8B 0D ?? ?? ?? ?? 85 C9 74 10 8B 44 24 08 8B 54 24 04 50 52 E8 ?? ?? ?? ?? C3 83 C8 FF C3";
using SetConfigFn = int(__cdecl*)(int option, int value);

// The game's shadows (Config: Shadows): 0 Normal, 1 Off, 2 High
constexpr int kShadowsOption = 58;
constexpr int kShadowsOff = 1;

SetConfigFn g_set = nullptr;

// The log window's "Maximum lines displayed" (Config > Windows > Window Settings) down to 1, not 4. Option 15's range is in
// its table entry (+0x18 the least, +0x1C the most, 20, +0x20 its default, 8), which setConfig and a character's config
// loading hold it to; and the window's Left key won't go under 4 by itself (conf5w1's handler, VA 0x10198229:
// mov eax,[esi+2Ch]; cmp eax,4; jle; ... push 0Fh), the 4 at +5.
constexpr int kLogLinesOption = 15;
constexpr std::size_t kLeast = 0x18;
constexpr std::int32_t kLogLinesGame = 4, kLogLinesLeast = 1;
constexpr char kLogLinesLeft[] = "8B 46 2C 83 F8 04 7E ?? 8B 16 48 50 6A 0F 8B CE 89 46 2C";
// And the log window's own setter of its most lines (VA 0x101618E0, both windows) makes anything over 24 or under 4 24:
// mov ax,[esp+4]; cmp ax,18h; jg; cmp ax,4; jl; mov [ecx+4Eh],ax ... mov word [ecx+4Eh],18h. The 4 at +0xD.
constexpr char kLogLinesSet[] = "66 8B 44 24 04 66 3D 18 00 7F 0C 66 3D 04 00 7C 06 66 89 41 4E";

const wchar_t* shadows_name(int value)
{
    return value == 0 ? L"Normal" : value == 1 ? L"Off" : value == 2 ? L"High" : L"?";
}

std::uintptr_t g_config_global = 0; // the address of the pointer to the config object
bool g_searched = false;
std::uintptr_t g_table = 0;         // the table last read
std::vector<std::uint8_t> g_last;   // its bytes then
ULONGLONG g_next_look = 0;

bool read(void* out, std::uintptr_t from, std::size_t size)
{
    SIZE_T got = 0;
    return from != 0 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(from), out, size, &got) &&
           got == size;
}

std::int32_t field(const std::vector<std::uint8_t>& table, int option, std::size_t offset)
{
    std::int32_t value = 0;
    std::memcpy(&value, table.data() + option * kEntry + offset, sizeof value);
    return value;
}

void log_lines_least(std::uintptr_t at, std::uintptr_t offset, const wchar_t* what)
{
    auto* least = reinterpret_cast<std::uint8_t*>(at + offset);
    DWORD old = 0;
    if (at == 0 || *least != kLogLinesGame || !VirtualProtect(least, 1, PAGE_EXECUTE_READWRITE, &old)) {
        log::write(L"Game config: %ls wasn't found: the log's maximum lines stop at 4", what);
        return;
    }
    *least = static_cast<std::uint8_t>(kLogLinesLeast);
    VirtualProtect(least, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), least, 1);
}

// Option 15's least lowered in a (new) table of options
void log_lines_range(std::uintptr_t table)
{
    const std::uintptr_t entry = table + kLogLinesOption * kEntry, at = entry + kLeast;
    std::int32_t number = 0, least = 0;
    if (read(&number, entry + 4, sizeof number) && number == kLogLinesOption && read(&least, at, sizeof least) &&
        least == kLogLinesGame)
        *reinterpret_cast<std::int32_t*>(at) = kLogLinesLeast;
}

void find()
{
    g_searched = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t code = game != nullptr ? scan::find(game, kGetConfig) : 0;
    if (code == 0) {
        log::write(L"Game config: couldn't find the game's options in FFXiMain.dll; changes aren't logged");
        return;
    }
    std::memcpy(&g_config_global, reinterpret_cast<const void*>(code + 2), sizeof g_config_global);
    g_set = reinterpret_cast<SetConfigFn>(scan::find(game, kSetConfig));
    log_lines_least(scan::find(game, kLogLinesLeft), 5, L"the Window Settings' Left key for the log's maximum lines");
    log_lines_least(scan::find(game, kLogLinesSet), 0xD, L"the log window's setter of its maximum lines");
    log::write(L"Game config: the game's options hang from FFXiMain.dll+0x%X; changes are logged",
               static_cast<unsigned>(g_config_global - reinterpret_cast<std::uintptr_t>(game)));
}

// The game's shadows off while VanaCore's are on: a character's config loading, or the Config menu,
// turning them back on turns them off again. VanaCore's off leaves them off; the Config menu puts
// them back.
void shadows(std::int32_t now)
{
    if (g_set == nullptr || now == kShadowsOff || !graphics::sun_shadows() || graphics::keep_game_shadows())
        return;
    if (g_set(kShadowsOption, kShadowsOff) > 0)
        log::write(L"Game config: the game's own shadows off (they were %ls): VanaCore's sun shadows are on "
                   L"([graphics] gameshadows=keep leaves them)",
                   shadows_name(now));
}

} // namespace

void install(const wchar_t* ini)
{
    // gameshadows.own: the setting older VanaCores kept to put back, which nothing reads now
    if (ini != nullptr && GetPrivateProfileIntW(L"graphics", L"gameshadows.own", -1, ini) >= 0)
        ini::set(ini, L"graphics", L"gameshadows.own", nullptr);
}

void on_frame()
{
    const ULONGLONG now = GetTickCount64();
    if (now < g_next_look)
        return;
    g_next_look = now + 250;
    if (!g_searched)
        find();
    std::uintptr_t object = 0, table = 0;
    if (!read(&object, g_config_global, sizeof object) || !read(&table, object + 4, sizeof table) || table == 0)
        return;
    std::vector<std::uint8_t> bytes(kOptions * kEntry);
    if (!read(bytes.data(), table, bytes.size()))
        return;
    if (table != g_table || g_last.size() != bytes.size()) {
        log_lines_range(table);
        // a new table (the game made its options): the first look, said once
        if (g_table == 0) {
            int saved = 0;
            for (int i = 0; i < kOptions; ++i)
                saved += field(bytes, i, 0xc) == kSavedType ? 1 : 0;
            log::write(L"Game config: %d options, %d of them saved to USER\\<character>\\cnf.dat", kOptions, saved);
        }
        g_table = table;
        g_last = std::move(bytes);
        return;
    }
    int changed = 0;
    for (int i = 0; i < kOptions; ++i)
        changed += field(bytes, i, 8) != field(g_last, i, 8) ? 1 : 0;
    if (changed > kManyAtOnce) {
        log::write(L"Game config: %d options changed at once (a character's config loaded)", changed);
    } else if (changed > 0) {
        int saved = 0; // the option's place among those cnf.dat keeps
        for (int i = 0; i < kOptions; ++i) {
            const bool in_file = field(bytes, i, 0xc) == kSavedType;
            const std::int32_t value = field(bytes, i, 8), was = field(g_last, i, 8);
            if (value != was) {
                if (in_file)
                    log::write(L"Game config: option %d = %d (was %d; cnf.dat value %d at +0x%X)", field(bytes, i, 4), value, was,
                               saved, 4 + saved * 4);
                else
                    log::write(L"Game config: option %d = %d (was %d; type %d, not in cnf.dat)", field(bytes, i, 4), value, was,
                               field(bytes, i, 0xc));
            }
            saved += in_file ? 1 : 0;
        }
    }
    const std::int32_t shadows_now = field(bytes, kShadowsOption, 8);
    g_last = std::move(bytes);
    shadows(shadows_now);
}

} // namespace vanacore::game_config
