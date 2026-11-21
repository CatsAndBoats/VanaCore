#include "tpbar.hpp"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "log.hpp"
#include "partylist.hpp"
#include "scan.hpp"

namespace vanacore::tpbar {
namespace {

// A party member's row (the party list, and "Solo" when you're alone: YkPartyListBase's member draw in the PS2 client;
// VA 0x10200C50 in FFXiMain.dll, __thiscall on the list, ret 0Ch: x, y, the member) draws the HP bar, then the MP bar,
// then the name and the numbers. The MP bar is YkDrawMPBar2 (VA 0x101C9910, __cdecl: x, y, the member's MP percent, a
// pointer to the byte the bar's animation keeps, which is member+0x79), whose fill is a quad of the grey "menu    gauge"
// picture coloured by the quad's own corner colours (the MP bar's are olive, the HP bar's red). VanaCore points that
// one call at itself: for a member with no spells it calls the game's MP bar with the TP percent and the quad's four
// corners blue, and puts the corners back after, so the TP bar is the game's own bar, animation and all.
//
// [client] party.tp gives every member a TP bar of its own beside the MP bar, through the same call: after the game's MP
// bar, the bar under it (VA 0x101C9660, __cdecl: x, y, the fill's shape, the frame's shape, the percent, the animation
// byte, then the fill's and the frame's scale across and down, 1.0 for the game's own) draws the MP bar's fill and frame
// again, blue, 46 units wide where the MP frame is 54, from the row's x+1: the space under the name, left of the MP bar
// (from x+0x31), where the game puts its TP number (x+0x30, y+0xA) when Config shows it; with party.tp on it's shown
// whatever Config says, at the bar's end as the MP number is at the MP bar's. Seen in game 2026-11-05 (a live Lua prototype
// at 1500 TP). The member's own animation bytes are the HP's and the MP's; this bar's are VanaCore's.
//
// mov dl,[esi+31h]; lea ecx,[esi+79h]; push ecx; lea eax,[ebx+11h]; push edx; lea ecx,[edi+31h]; push eax; push ecx;
// call <the MP bar>; add esp,20h; cmp word ptr [esi+50h],0
constexpr char kRow[] = "8A 56 31 8D 4E 79 51 8D 43 11 52 8D 4F 31 50 51 E8 ?? ?? ?? ?? 83 C4 20 66 83 7E 50 00";
constexpr std::ptrdiff_t kCall = 16;
constexpr int kMpX = 0x31; // the MP bar's x in the row (lea ecx,[edi+31h])

// Further on in the row, the MP number: mov eax,[esi+28h]; lea ecx,[esp+14h]; push eax; push <"%d">; push ecx; call
// sprintf; push 1.0; push 1.0; push 0; lea edx,[esp+2Ch]; lea eax,[ebx+0Ah]; push 80808080h; push edx; push eax;
// lea eax,[edi+62h]; push eax; call <the string draw, __cdecl: x, y, text, colour, 0, scale x, scale y>. (The colour any:
// [client] party.mp.color writes it: partylist.cpp)
constexpr char kNumber[] = "8B 46 28 8D 4C 24 ?? 50 68 ?? ?? ?? ?? 51 E8 ?? ?? ?? ?? 68 00 00 80 3F 68 00 00 80 3F 6A 00 "
                           "8D 54 24 ?? 8D 43 0A 68 ?? ?? ?? ?? 52 50 8D 47 62 50 E8 ?? ?? ?? ??";
constexpr std::ptrdiff_t kNumberCall = 49;
constexpr std::size_t kNumberWithin = 0x100; // after the MP bar's call
constexpr std::uint32_t kReadyColour = 0x802060D0; // the game's own TP number at 1000 and over

// After it, the TP number, drawn when Config shows it (option 0AEh) for a member who isn't a trust: push 0; push 0AEh;
// call <the option>; add esp,30h; test al,al; je <past the number>; push esi; call <a trust?>; add esp,4; test al,al;
// jne <past it>; mov ecx,[esi+2Ch]; ... call <the TP number, __thiscall on the list: x+30h, y+0Ah, the TP; grey, blue from
// 1000>. party.tp makes that je's displacement 0, so the number is drawn at the TP bar's end whatever Config says.
constexpr char kTpNumber[] = "6A 00 68 AE 00 00 00 E8 ?? ?? ?? ?? 83 C4 30 84 C0 74 ?? 56 E8 ?? ?? ?? ?? 83 C4 04 84 C0 75 ?? "
                             "8B 4E 2C";
constexpr std::ptrdiff_t kOptionJump = 17;
constexpr std::uint8_t kJe = 0x74;

// The MP bar: push 1; call <the menu resource getter, __cdecl>; mov eax,[eax]; the shape is that table's +0x188
constexpr std::uint8_t kBarStart[3] = {0x6A, 0x01, 0xE8};
constexpr std::ptrdiff_t kShapeTable = 0x188; // the fill
constexpr std::ptrdiff_t kFrameTable = 0x9C;  // its frame
// The MP bar whole, to its call of the bar's body: push 1; call <resources>; mov eax,[eax]; mov ecx,[esp+14h];
// mov edx,[esp+10h]; push ecx; mov ecx,[eax+9Ch]; push edx; mov edx,[eax+188h]; mov eax,[esp+14h]; push ecx;
// mov ecx,[esp+14h]; push edx; push eax; push ecx; call <the body: x, y, fill, frame, percent, animation byte>
constexpr char kMpBar[] = "6A 01 E8 ?? ?? ?? ?? 8B 00 8B 4C 24 14 8B 54 24 10 51 8B 88 9C 00 00 00 52 8B 90 88 01 00 00 "
                          "8B 44 24 14 51 8B 4C 24 14 52 50 51 E8";
constexpr std::ptrdiff_t kBodyCall = 0x2B;
// The body, to its call of the bar with its scales: the args read, push 1.0 four times (the scales), the args pushed,
// call <the bar>
constexpr char kBody[] = "8B 44 24 18 8B 4C 24 14 8B 54 24 10 68 00 00 80 3F 68 00 00 80 3F 68 00 00 80 3F 68 00 00 80 3F "
                         "50 8B 44 24 20 51 8B 4C 24 20 52 8B 54 24 20 50 51 52 E8";
constexpr std::ptrdiff_t kDrawCall = 0x32;
// A shape: a list of its quads from +4 (the first node's value, +0x10, is a quad); its box's left and right (shorts)
constexpr std::ptrdiff_t kShapeList = 0x4;
constexpr std::ptrdiff_t kNodeValue = 0x10;
constexpr std::ptrdiff_t kShapeLeft = 0x20;
constexpr std::ptrdiff_t kShapeRight = 0x22;
// A quad: its texture's name at +0x32, its four corners from +0x50, 0x1C apart, each with its colour (B, G, R, A) at +0x10
constexpr std::ptrdiff_t kQuadName = 0x32;
constexpr std::ptrdiff_t kCorners = 0x50;
constexpr std::ptrdiff_t kCorner = 0x1C;
constexpr std::ptrdiff_t kColour = 0x10;
constexpr char kGauge[16] = {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 'g', 'a', 'u', 'g', 'e', ' ', ' ', ' '};

// party.tp's bar in the row: its frame's left, and its width (the MP frame's 54 scaled to it), 2 short of the MP bar
constexpr int kOwnX = 1;
constexpr int kOwnWidth = 46;

// A party member (0x7C each)
constexpr std::ptrdiff_t kBarState = 0x79;
constexpr std::ptrdiff_t kMp = 0x28;
constexpr std::ptrdiff_t kTp = 0x2C;
constexpr std::ptrdiff_t kMainJob = 0x6D;
constexpr std::ptrdiff_t kSubJob = 0x6F;
constexpr std::ptrdiff_t kSubLevel = 0x70;

// The jobs that cast nothing of their own: WAR MNK THF BST RNG SAM DRG COR PUP DNC
constexpr std::uint32_t kNoSpells = 1u << 1 | 1u << 2 | 1u << 6 | 1u << 9 | 1u << 11 | 1u << 12 | 1u << 14 | 1u << 17 |
                                    1u << 18 | 1u << 19;
constexpr int kFullTp = 3000;
constexpr int kReadyTp = 1000;

// The corners' colours (R, G, B; 0x80 is a texel as it is, doubled as it's drawn): the top two, the bottom two
struct Shade {
    std::uint8_t top[3];
    std::uint8_t bottom[3];
};
constexpr Shade kBuilding = {{0x30, 0x68, 0xE0}, {0x20, 0x50, 0xC0}};
constexpr Shade kReady = {{0x50, 0xA0, 0xFF}, {0x30, 0x80, 0xF0}};

using MpBar = void(__cdecl*)(int x, int y, int percent, std::uint8_t* state);
using Bar = void(__cdecl*)(int x, int y, void* fill, void* frame, int percent, std::uint8_t* state, float fill_x,
                           float fill_y, float frame_x, float frame_y);
using DrawNumber = void(__cdecl*)(int x, int y, const char* text, std::uint32_t colour, int flag, std::uint32_t scale_x,
                                  std::uint32_t scale_y);
using MenuResource = void**(__cdecl*)(int index);

bool g_on = false;       // [client] party.tpbar
bool g_every = false;    // [client] party.tp
bool g_applied = false;  // the call is VanaCore's
bool g_searched = false; // FFXiMain.dll was looked through (g_call != 0: found)
std::uintptr_t g_call = 0;
MpBar g_bar = nullptr;           // the game's
Bar g_draw = nullptr;            // the game's bar under it, with its scales (null: party.tp does nothing)
MenuResource g_resource = nullptr; // the game's
std::int32_t g_original = 0;     // the call's own offset, to put back
std::uintptr_t g_number_call = 0;
DrawNumber g_number = nullptr;       // the game's string draw
std::int32_t g_number_original = 0;  // its call's own offset
std::uintptr_t g_option_jump = 0;    // the je past the TP number (0: not found, party.tp's bar has no number)
std::uint8_t g_option_original = 0;  // its own displacement
bool g_shown = false;                // the TP number is drawn whatever Config says

// The member whose MP holds its TP until the number is drawn (the row formats the MP field, one call after the bar)
struct Swap {
    std::uint8_t* member = nullptr;
    std::int32_t mp = 0;
    std::int32_t tp = 0;
} g_swap;

// party.tp's animation bytes, a member's row (its place in the party's list) each
struct Slot {
    const std::uint8_t* member = nullptr;
    std::uint8_t state = 0;
};
constexpr unsigned kSlots = 8;
Slot g_slots[kSlots];
unsigned g_next_slot = 0;

// The member's own MP back, if its TP is standing in
void unswap()
{
    if (g_swap.member != nullptr)
        std::memcpy(g_swap.member + kMp, &g_swap.mp, 4);
    g_swap.member = nullptr;
}

bool readable(const void* at, std::size_t size)
{
    return !IsBadReadPtr(at, size);
}

// The menu resources' shape table (the game's, found again each time), or null
const std::uint8_t* menu_table()
{
    void** const object = g_resource(1);
    if (object == nullptr || !readable(object, sizeof(void*)))
        return nullptr;
    const auto table = reinterpret_cast<const std::uint8_t*>(*object);
    if (table == nullptr || !readable(table + kFrameTable, kShapeTable - kFrameTable + 4))
        return nullptr;
    return table;
}

// The MP fill's quad, or null if it isn't what it should be
std::uint8_t* gauge_quad(const std::uint8_t* table)
{
    const auto shape = *reinterpret_cast<const std::uint8_t* const*>(table + kShapeTable);
    if (shape == nullptr || !readable(shape + kShapeList, 4))
        return nullptr;
    const auto node = *reinterpret_cast<const std::uint8_t* const*>(shape + kShapeList);
    if (node == nullptr || !readable(node + kNodeValue, 4))
        return nullptr;
    auto* const quad = *reinterpret_cast<std::uint8_t* const*>(node + kNodeValue);
    if (quad == nullptr || !readable(quad, kCorners + 4 * kCorner) || std::memcmp(quad + kQuadName, kGauge, sizeof(kGauge)) != 0)
        return nullptr;
    return quad;
}

// The MP fill's four corners blue for one draw of the bar, and put back after it
class Tint {
public:
    Tint(std::uint8_t* quad, int tp)
        : quad_(quad)
    {
        const Shade& shade = tp >= kReadyTp ? kReady : kBuilding;
        for (int i = 0; i < 4; ++i) {
            std::uint8_t* const colour = quad_ + kCorners + i * kCorner + kColour; // B, G, R, A
            std::memcpy(saved_[i], colour, 4);
            const std::uint8_t* const rgb = i < 2 ? shade.top : shade.bottom;
            colour[0] = rgb[2];
            colour[1] = rgb[1];
            colour[2] = rgb[0];
        }
    }
    ~Tint()
    {
        for (int i = 0; i < 4; ++i)
            std::memcpy(quad_ + kCorners + i * kCorner + kColour, saved_[i], 4);
    }
    Tint(const Tint&) = delete;
    Tint& operator=(const Tint&) = delete;

private:
    std::uint8_t* quad_;
    std::uint8_t saved_[4][4] = {};
};

// The member's TP, 0 to 3000
int tp_of(const std::uint8_t* member)
{
    std::int32_t tp = 0;
    std::memcpy(&tp, member + kTp, 4);
    return std::clamp(tp, 0, kFullTp);
}

// The member's TP bar, in percent, if this member's MP bar is to be one (-1: it stays the MP bar)
int tp_percent(const std::uint8_t* member, int& tp)
{
    const unsigned main_job = member[kMainJob];
    const unsigned sub_job = member[kSubJob];
    if (main_job >= 32 || (kNoSpells >> main_job & 1) == 0)
        return -1;
    if (sub_job != 0 && member[kSubLevel] != 0 && (sub_job >= 32 || (kNoSpells >> sub_job & 1) == 0))
        return -1;
    std::int32_t mp = 0;
    std::memcpy(&mp, member + kMp, 4);
    if (mp != 0)
        return -1;
    std::memcpy(&tp, member + kTp, 4);
    if (tp < 0)
        tp = 0;
    return (tp > kFullTp ? kFullTp : tp) * 100 / kFullTp;
}

// party.tp's animation byte for the member's row
std::uint8_t* own_state(const std::uint8_t* member)
{
    for (Slot& slot : g_slots) {
        if (slot.member == member)
            return &slot.state;
    }
    Slot& slot = g_slots[g_next_slot++ % kSlots];
    slot.member = member;
    slot.state = 0;
    return &slot.state;
}

// party.tp: the member's own TP bar, left of its MP bar (`x`, `y`: the row's, at the MP bar's height)
void own_bar(int x, int y, const std::uint8_t* member)
{
    const std::uint8_t* const table = g_draw != nullptr ? menu_table() : nullptr;
    std::uint8_t* const quad = table != nullptr ? gauge_quad(table) : nullptr;
    if (quad == nullptr)
        return;
    void* const fill = *reinterpret_cast<void* const*>(table + kShapeTable);
    auto* const frame = *reinterpret_cast<std::uint8_t* const*>(table + kFrameTable);
    if (frame == nullptr || !readable(frame + kShapeLeft, 4))
        return;
    std::int16_t left = 0;
    std::int16_t right = 0;
    std::memcpy(&left, frame + kShapeLeft, 2);
    std::memcpy(&right, frame + kShapeRight, 2);
    if (right - left < kOwnWidth / 2)
        return;
    const float scale = static_cast<float>(kOwnWidth) / static_cast<float>(right - left);
    const int tp = tp_of(member);
    const Tint tint(quad, tp);
    g_draw(x + kOwnX, y, fill, frame, tp * 100 / kFullTp, own_state(member), scale, 1.0f, scale, 1.0f);
}

// What the game's MP bar call is made to: the game's bar, with the TP and blue corners for a member with no spells
// (party.tpbar), and party.tp's own TP bar after it
void __cdecl mp_bar(int x, int y, int percent, std::uint8_t* state)
{
    unswap(); // a row whose number wasn't drawn
    std::uint8_t* const member = state != nullptr ? state - kBarState : nullptr;
    int tp = 0;
    const int instead = g_on && !g_every && member != nullptr ? tp_percent(member, tp) : -1;
    const std::uint8_t* const table = instead >= 0 ? menu_table() : nullptr;
    std::uint8_t* const quad = table != nullptr ? gauge_quad(table) : nullptr;
    if (quad == nullptr) {
        g_bar(x, y, percent & 0xFF, state);
        if (g_every && member != nullptr)
            own_bar(x - kMpX, y, member);
        return;
    }
    {
        const Tint tint(quad, tp);
        g_bar(x, y, instead, state);
    }
    if (g_number_call == 0)
        return;
    // the number the row prints next for the MP is the TP
    g_swap.member = member;
    g_swap.tp = tp;
    std::memcpy(&g_swap.mp, member + kMp, 4);
    std::memcpy(member + kMp, &tp, 4);
}

// What the game's MP number draw is made to: the TP in the member's MP field is put back, and drawn in blue from 1000
// (party.tp.color's colour when it's set: partylist.cpp)
void __cdecl draw_number(int x, int y, const char* text, std::uint32_t colour, int flag, std::uint32_t scale_x,
                         std::uint32_t scale_y)
{
    if (g_swap.member != nullptr) {
        colour = partylist::tp_colour(g_swap.tp, g_swap.tp >= kReadyTp ? kReadyColour : colour);
        unswap();
    }
    g_number(x, y, text, colour, flag, scale_x, scale_y);
}

// Where the call `at + call` (the end of `pattern`, which must match at `at`) goes, if it's within the game
std::uintptr_t follow(std::uintptr_t at, const char* pattern, std::ptrdiff_t call, std::uintptr_t base, std::uintptr_t end)
{
    if (scan::find(at, static_cast<std::size_t>(call) + 5, pattern) != at)
        return 0;
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(at + call + 1), 4);
    const std::uintptr_t to = at + call + 5 + rel;
    return to > base && to < end ? to : 0;
}

// The je that leaves the TP number out when Config doesn't show it
void find_option(std::uintptr_t row)
{
    const std::uintptr_t option = scan::find(row, kNumberWithin, kTpNumber);
    const auto* const jump = reinterpret_cast<const std::uint8_t*>(option + kOptionJump);
    if (option == 0 || jump[0] != kJe) {
        log::write(L"TP bar: the party list's TP number wasn't found: [client] party.tp's bar has no number");
        return;
    }
    g_option_jump = option + kOptionJump;
    g_option_original = jump[1];
}

// The bar under the MP bar, with its scales (party.tp's narrower bar), and the MP number's draw after the bar's call
void find_more(std::uintptr_t row, std::uintptr_t bar, std::uintptr_t base, std::uintptr_t end)
{
    find_option(row);
    const std::uintptr_t body = follow(bar, kMpBar, kBodyCall, base, end);
    const std::uintptr_t draw = body != 0 ? follow(body, kBody, kDrawCall, base, end) : 0;
    if (draw != 0)
        g_draw = reinterpret_cast<Bar>(draw);
    else
        log::write(L"TP bar: the game's bar under the MP bar wasn't found: [client] party.tp does nothing");
    // without the number the bar still shows, with the MP's number beside it
    const std::uintptr_t number = scan::find(row, kNumberWithin, kNumber);
    if (number != 0) {
        const std::uintptr_t call = number + kNumberCall;
        std::int32_t rel = 0;
        std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), 4);
        const std::uintptr_t to = call + 5 + rel;
        if (to > base && to < end) {
            g_number_call = call;
            std::memcpy(&g_number_original, reinterpret_cast<const void*>(call + 1), 4);
            g_number = reinterpret_cast<DrawNumber>(to);
        }
    }
    if (g_number_call == 0)
        log::write(L"TP bar: the party list's MP number wasn't found: the TP bar keeps the MP's number");
}

// The call and the bar, the first time they're wanted once FFXiMain.dll is there
bool found()
{
    if (g_searched)
        return g_call != 0;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return false;
    g_searched = true;
    const auto base = reinterpret_cast<std::uintptr_t>(game);
    const std::uintptr_t at = scan::find(game, kRow);
    if (at == 0) {
        log::write(L"TP bar: the party list's MP bar call wasn't found in FFXiMain.dll: [client] party.tpbar and party.tp do nothing");
        return false;
    }
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(at + kCall + 1), 4);
    const std::uintptr_t bar = at + kCall + 5 + rel;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew);
    const std::uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
    bool sane = bar > base && bar + 7 < end && std::memcmp(reinterpret_cast<const void*>(bar), kBarStart, sizeof(kBarStart)) == 0;
    std::uintptr_t resource = 0;
    if (sane) {
        std::memcpy(&rel, reinterpret_cast<const void*>(bar + 3), 4);
        resource = bar + 7 + rel;
        sane = resource > base && resource < end;
    }
    if (!sane) {
        log::write(L"TP bar: the party list's MP bar at +0x%X isn't as expected: [client] party.tpbar and party.tp do nothing",
                   static_cast<unsigned>(at - base));
        return false;
    }
    g_call = at + kCall;
    std::memcpy(&g_original, reinterpret_cast<const void*>(g_call + 1), 4);
    g_bar = reinterpret_cast<MpBar>(bar);
    g_resource = reinterpret_cast<MenuResource>(resource);
    log::write(L"TP bar: the party list's MP bar call found at +0x%X (MP bar +0x%X)", static_cast<unsigned>(g_call - base),
               static_cast<unsigned>(bar - base));
    find_more(at, bar, base, end);
    return true;
}

// A call's offset (4 bytes) or a jump's (1), written in one store (the game's thread runs this code)
template <typename T>
void write_code(std::uintptr_t at, T value)
{
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), sizeof(T), PAGE_EXECUTE_READWRITE, &protect)) {
        log::write(L"TP bar: the game's code at %p couldn't be written (error %lu)", reinterpret_cast<void*>(at), GetLastError());
        return;
    }
    *reinterpret_cast<volatile T*>(at) = value;
    VirtualProtect(reinterpret_cast<void*>(at), sizeof(T), protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(at), sizeof(T));
}

void write_offset(std::uintptr_t call, std::int32_t offset)
{
    write_code(call + 1, offset);
}

// party.tp: the game's TP number drawn at the bar's end whatever Config says (the je past it goes to the next instruction)
void show_number(bool on)
{
    if (on == g_shown)
        return;
    g_shown = on;
    if (g_option_jump != 0)
        write_code(g_option_jump + 1, on ? std::uint8_t{0} : g_option_original);
}

void apply()
{
    const bool wanted = g_on || g_every;
    if ((wanted == g_applied && g_every == g_shown) || !found())
        return;
    show_number(g_every);
    if (wanted == g_applied)
        return;
    if (wanted) {
        write_offset(g_call, static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(&mp_bar) - (g_call + 5)));
        if (g_number_call != 0)
            write_offset(g_number_call,
                         static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(&draw_number) - (g_number_call + 5)));
        log::write(L"TP bar: the party list's MP bar call is VanaCore's ([client] party.tpbar=%d, party.tp=%d)", g_on ? 1 : 0,
                   g_every ? 1 : 0);
    } else {
        write_offset(g_call, g_original);
        if (g_number_call != 0)
            write_offset(g_number_call, g_number_original);
        unswap();
        log::write(L"TP bar: the party list's MP bar as the game draws it ([client] party.tpbar=0, party.tp=0)");
    }
    g_applied = wanted;
}

} // namespace

void reload(const wchar_t* ini)
{
    const bool was_on = g_on;
    const bool was_every = g_every;
    g_on = GetPrivateProfileIntW(L"client", L"party.tpbar", 0, ini) != 0;
    g_every = GetPrivateProfileIntW(L"client", L"party.tp", 0, ini) != 0;
    if (g_applied && (g_on != was_on || g_every != was_every))
        log::write(L"TP bar: [client] party.tpbar=%d, party.tp=%d", g_on ? 1 : 0, g_every ? 1 : 0);
    apply();
}

void install(const wchar_t* ini)
{
    reload(ini);
    partylist::install(ini); // [client] party.names.upper, party.hp.color: the same rows' names and numbers
}

void on_frame()
{
    if ((g_on || g_every) != g_applied || g_every != g_shown)
        apply();
}

} // namespace vanacore::tpbar
