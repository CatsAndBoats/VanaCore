#include "equipstats.hpp"

#include <windows.h>

#include <lua.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../gfx/vanacore_gfx.h"
#include "chatfont.hpp"
#include "compat/ashita/host.hpp"
#include "equipdesc.hpp"
#include "compat/windower/objects.hpp"
#include "game.hpp"
#include "gametext.hpp"
#include "hudwin.hpp"
#include "imports.hpp"
#include "json.hpp"
#include "listtext.hpp"
#include "log.hpp"
#include "outline.hpp"
#include "packets.hpp"
#include "scan.hpp"
#include "uispace.hpp"

namespace vanacore::equipstats {
namespace {

// CTkEquip::OnDrawPrimitive in the PS2 client's names (TkEquip.cpp; FUN_10132aa0), __thiscall on the Equipment window's
// handler (the global at 0x10576038), no arguments: each equipped item's icon in its square, then the ten numbers its
// OnDrawCalc laid out (the elements, Attack, Defense: FFwNumLabel, 20h each from +88h). Its control is at +8. The window
// is its layout's 182 x 190 ("menu    equip   ", 130,48 - 312,238 in the 512 x 448), the numbers' last row 175 down and
// 14 tall: nothing below them. itemslot.cpp redirects two calls inside it (the wardrobe badge); the start is this:
// sub esp,10h; push ebp; push esi; mov esi,ecx; push edi; xor eax,eax; mov ebp,1; mov edi,[esi+8]; mov [esp+14h],ax;
// mov [esp+16h],ax; mov [esp+18h],ax; mov [esp+1Ah],ax; cmp [edi+58h],bp
constexpr char kEquipDraw[] = "83 EC 10 55 56 8B F1 57 33 C0 BD 01 00 00 00 8B 7E 08 66 89 44 24 14 66 89 44 24 16 "
                              "66 89 44 24 18 66 89 44 24 1A 66 39 6F 58";
constexpr std::size_t kPrologue = 5; // sub esp,10h; push ebp; push esi
constexpr std::ptrdiff_t kCtrlAt = 8;
// YkDrawString (FUN_101c7ee0, __cdecl: x, y, text, colour; where the text ended): font 0 at full size, through the
// scaled one: mov eax,[esp+10h]; mov ecx,[esp+0Ch]; mov edx,[esp+8]; push 0; push 1.0; push 1.0; push eax; ...; call;
// add esp,1Ch; ret
constexpr char kDrawString[] = "8B 44 24 10 8B 4C 24 0C 8B 54 24 08 6A 00 68 00 00 80 3F 68 00 00 80 3F 50 8B 44 24 14 "
                               "51 52 50 E8 ?? ?? ?? ?? 83 C4 1C C3";
// The fonts (FUN_101c7390, __cdecl: index; 0 past the 18): mov eax,[esp+4]; test eax,eax; jl; cmp eax,12h; jge;
// mov eax,[eax*4+fonts]; ret
constexpr char kFonts[] = "8B 44 24 04 85 C0 7C 0D 83 F8 12 7D 08 8B 04 85 ?? ?? ?? ?? C3 33 C0 C3";
// YmMenuShape::Draw (FUN_1011f760), which calls the quad drawing (FUN_1011e580, __thiscall on the quad, ret 18h: x, y,
// scale x, scale y, colour, blending) here; the quad drawing's own start is another module's hook (itemslot.cpp). Its
// first 7 bytes (push ecx; push ebx; push ebp; push esi; mov esi,[ecx+14h]) left out: buttons.cpp hooks it there.
constexpr char kShapeDraw[] = "?? ?? ?? ?? ?? ?? ?? 8D 59 04 57 8B CB E8 ?? ?? ?? ?? 89 44 24 10 8B 44 24 28 33 FF 85 C0 7E 13";
constexpr std::ptrdiff_t kQuadCall = 0x6D;
// The game's resize of a window's control (FUN_1011a1a0, statuswindow.cpp's: __thiscall, ret 1Ch: x, y, width, height,
// then 1, 0, 0 to keep the size in its frame) and the menu manager's windows' list (its draw: mov ecx,list; call first)
constexpr char kResize[] = "83 EC 08 56 8B F1 8D 4E 14 E8 ?? ?? ?? ?? 85 C0 89 44 24 08 C7 44 24 04 00 00 00 00 0F 84";
constexpr char kWindows[] = "32 DB B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 89 44 24 0C 74 3A 8D 44 24 0C B9 ?? ?? ?? ?? 50 E8";
constexpr std::ptrdiff_t kWindowsAt = 3;
// The game's window frame (FUN_10233040, __thiscall on a frame, ret 1Ch: x, y, scale across, scale down (about its middle),
// colour, 0 (the window's flags: 8, 44h), 0 (non-zero: the background alone)): every window's background and border, nine
// pieces of the menu resource, the corners 24 units. A frame is {width, height, kind}; the window draw (0x101173D0) calls this
// for a layout's frame piece (its +4: the Equipment window's {182, its height, 1}). Seen live 2026-11-06.
constexpr char kFrameDraw[] = "83 EC 08 53 55 56 8B F1 DB 44 24 18 8B 06 33 DB D1 E8 89 44 24 0C 89 5C 24 10 DF 6C 24 0C D9 44 24 "
                              "20 D8 25 ?? ?? ?? ?? 57 DE C9 DE E9 E8";
struct Frame {
    int width, height, kind; // kind: the background's piece (non-zero: the 9th, the Equipment window's)
};

// A control: its layout at +4 (the resource's own rect at +10h: left, top, right, bottom shorts, never changed by a
// resize), its window at +0Ch (whose own rect at +14h its text is drawn from), its rect at +3Ah (left, top, right,
// bottom), what every resize sets, its pictures' list at +14h. A list node: the next at +0, its item at +10h.
constexpr std::ptrdiff_t kLayoutAt = 4, kLayoutRect = 0x10, kWindowAt = 0xC, kWindowRect = 0x14, kRectAt = 0x3A, kPictures = 0x14;
constexpr std::ptrdiff_t kNext = 0, kNodeItem = 0x10;
constexpr std::ptrdiff_t kStateAt = 0x10; // a window's state: kClosedState while the game still draws it closing
constexpr std::uint32_t kClosedState = 0xE;
constexpr int kUnderGap = 2; // the game's item details stand 2 below the window over them (240 under 48 + 190)
// A menu picture (its type at +0, 0 for one with a shape, the shape at +8) and its quads (statuswindow.cpp's): the
// shape's list at +4 and count at +14h; a quad's texture's name at +32h, its corners from +50h, 1Ch apart (colour +10h,
// its alpha +13h, u +14h, v +18h), their places worked out from the shorts at +C0h (x) and +C8h (y); corners top left,
// top right, bottom left, bottom right. Font 0's dash: an 8-texel quad whose white row is 3.5, 6.5 texels in.
constexpr std::ptrdiff_t kShape = 8, kParts = 4, kPartCount = 0x14, kQuadName = 0x32, kCorners = 0x50, kCorner = 0x1C,
                         kColour = 0x10, kU = 0x14, kV = 0x18, kLocalX = 0xC0, kLocalY = 0xC8;
constexpr std::size_t kQuadSize = 0xD0;
constexpr float kDashWhite[2] = {3.5f / 8.0f, 6.5f / 8.0f};

// The Equipment window's own row of Attack and Defense, taken out while equip.stats is on (the rows here have them): its
// numbers are two of the handler's ten (FFwNumLabel, 20h each from +88h: the elements' eight, then Attack, then Defense;
// drawn by OnDrawPrimitive's last loop at their screen y +6, digits +1Ch, none drawing nothing; their row in the window
// +0Ah), its words letters of the "font    font" sheet in the window's picture, in that row. The elements' balls ("menu
// colorbal" quads) and numbers are drawn the row's height higher, the window as much shorter.
constexpr std::ptrdiff_t kNumbers = 0x88, kNumber = 0x20, kNumberY = 6, kNumberRow = 0xA, kDigits = 0x1C;
constexpr int kElementNumbers = 8, kAttackNumber = 8, kDefenseNumber = 9;
constexpr char kLetterSheet[16] = {'f', 'o', 'n', 't', ' ', ' ', ' ', ' ', 'f', 'o', 'n', 't', ' ', ' ', ' ', ' '};
constexpr char kBall[16] = {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 'c', 'o', 'l', 'o', 'r', 'b', 'a', 'l'};

// The layout, in the interface's units: one stat a row, its label kInset from the window's left, its number to the
// right kInset from its right, rows kRow apart (font 0's letters are kGlyph tall, drawn at full size: a narrower bitmap
// letter is a blurred one), a line before each block, at most kMaxGearRows of what the gear adds
constexpr int kInset = 8, kRow = 14, kGlyph = 11, kLineGap = 3, kBlockGap = 7, kBottom = 7;
constexpr int kMaxGearRows = 32;
constexpr int kAway = -2000; // text measured off the screen
constexpr std::uint32_t kWhite = 0x80808080; // the game's colours: 80h is full (drawn doubled)
constexpr std::uint32_t kLineColour = 0x70646C7C; // the Status window's separator's (#C8D8F8, 70h)

// /checkparam: the client's 0x0DD (the player's id at +4, index at +8, kind 2 at +0Ch) and the server's answer, 0x029
// lines (actor +4, target +8, two numbers +0Ch and +10h, the message +18h): name, item level, main hand, off hand
// (0, 0 without a weapon there), ranged (0, 0 without one), evasion and defence last (packets/c2s/0x0dd_equip_inspect.cpp)
constexpr std::uint16_t kInspect = 0x0DD, kBattleMessage = 0x029;
constexpr std::uint8_t kCheckParam = 2;
constexpr std::uint16_t kName = 733, kItemLevel = 731, kPrimary = 712, kAuxiliary = 713, kRange = 714, kDefense = 715;
// what makes the answer worth asking for again: the gear (0x050), the stats (0x061), the effects (0x063)
constexpr std::uint16_t kWatched[] = {0x050, 0x061, 0x063};
constexpr ULONGLONG kAskEvery = 1000, kAnswerWithin = 5000, kClosedAfter = 500;

// The stats shown (equip_mods.json's "stats", gen_gamedata.py's EQUIP_STATS first, in order; found by the server's
// modifier name): the label, the server's unit (100: a 10000 base, so 375 is 3.75%), whether it's a percentage, whether
// several items' combine by the highest alone (MOVE_SPEED_GEAR_BONUS) or are added, and a short label (none: empty) for
// where the label and its number don't fit side by side; and the modifier's name (for /vana statsdump)
struct Stat {
    std::string label;
    int scale = 1;
    bool percent = false, highest = false;
    std::string brief, name;
    bool shown = true; // (false: one the window doesn't show, kept for vana.gamedata.item_stats: add_unshown)
};
// The elements' families ("families": Fire ... Dark staff bonus), one row where all eight are the same: its label and
// short one, its members (stats) and the first of them in the list, where the row goes
struct Family {
    std::string label, brief;
    std::vector<int> members;
    int first = 0;
};
const Stat kPlain{};
// The server's totals' stats (/checkparam's; the gear's own sums while it hasn't answered), by their place in the list
enum Total { kAcc, kAtt, kRacc, kRatt, kEva, kTotals };
constexpr const char* kTotalMods[kTotals] = {"ACC", "ATT", "RACC", "RATT", "EVA"};

using EquipDraw = void(__thiscall*)(void* self);
using DrawString = int(__cdecl*)(int x, int y, const char* text, std::uint32_t colour);
using Fonts = char**(__cdecl*)(int index);
using DrawQuad = void(__thiscall*)(void* quad, int x, int y, float sx, float sy, std::uint32_t colour, int blending);
using Resize = void(__thiscall*)(void* ctrl, int x, int y, int width, int height, int keep, int, int);
using FrameDraw = void(__thiscall*)(const Frame* frame, int x, int y, float sx, float sy, std::uint32_t colour, int flags, int bare);

// [client] equip.stats*
bool g_on = false; // (off unless set, 2026-11-06)
bool g_ask = true; // .checkparam
// .move: the stats in a box of their own, its left edge at .x and its top at .y (percents of the interface), the Equipment
// window left at the game's height (its Attack and Defense row still taken out)
bool g_move = false;
constexpr double kDefaultX = 26.0, kDefaultY = 11.0; // (beside the item list, as the Equipment window opens on 1080 lines)
double g_x = kDefaultX, g_y = kDefaultY;
std::wstring g_dir;

bool g_tried = false;
EquipDraw g_draw = nullptr; // the game's, through the trampoline
DrawString g_string = nullptr;
Fonts g_fonts = nullptr;
DrawQuad g_quad = nullptr;    // null: no lines
Resize g_resize = nullptr;    // null: nothing drawn (no room)
FrameDraw g_frame = nullptr;  // null: no box of their own (equip.stats.move does nothing)
char* const* g_windows = nullptr;

// Where the stats were last drawn, for the HUD Manager's box while the Equipment window is shut: their own box's size, and
// their part of the Equipment window (attached)
int g_box_w = 182, g_box_h = 300;
outline::Box g_attached;
bool g_attached_seen = false;

// The item data (equip_mods.json), read on a thread of its own the first time the window is drawn: the stats, each
// item's and each augment's modifiers that are shown (index into g_stats), each item's latent ones (with the server's
// condition, LATENT, and its parameter), and the charged items (whose extdata isn't augments). Written before g_data says
// kRead, only read after.
struct Mod {
    std::uint16_t stat;
    std::int16_t value;
};
struct AugmentMod {
    std::uint16_t stat;
    std::int16_t value;
    std::uint8_t multiplier; // (the server's is a uint8)
};
struct Latent {
    std::uint16_t stat;
    std::int16_t value;
    std::uint16_t condition;
    std::uint16_t param;
};
std::vector<Stat> g_stats;
std::vector<Family> g_families;
std::vector<int> g_family_of; // each stat's family, -1 for none
int g_total_at[kTotals] = {-1, -1, -1, -1, -1};
std::unordered_map<std::uint16_t, std::vector<Mod>> g_items;
std::unordered_map<std::uint16_t, std::vector<AugmentMod>> g_augments;
std::unordered_map<std::uint16_t, std::vector<Latent>> g_latents;
std::unordered_set<std::uint16_t> g_charged;
// What a description's labels mean (its "words": a stat and the scale its number takes, -1 for one written the other way
// round, 100 for a 10000 base) and each item's description's fingerprint when the data was made ("described"), for an
// item CatsEyeXI has changed since (equipdesc.hpp)
struct Word {
    std::uint16_t stat;
    std::int16_t scale;
};
std::unordered_map<std::string, Word> g_words;
std::unordered_map<std::uint16_t, std::uint32_t> g_described;
enum Data { kUnread, kReading, kRead, kMissing };
std::atomic<int> g_data{kUnread};
std::string g_generated, g_source; // the data's "generated" and "source" (vana.gamedata.item_stats)

// The server's totals and the asking for them (g_lock: the packets come on the game's thread, but inject is any)
struct Totals {
    int acc = 0, att = 0, acc_sub = 0, att_sub = 0, racc = 0, ratt = 0, def = 0, eva = 0;
    bool known = false;
};
std::mutex g_lock;
Totals g_totals, g_coming;
std::uint32_t g_self = 0; // whose lines are taken
int g_expected = 0;       // answers asked for that haven't begun
bool g_in_answer = false; // the lines of one are coming
ULONGLONG g_asked_at = 0;
std::atomic<bool> g_awaiting{false};
// The window's state for asking: when it was last drawn, the packets' times then, whether to ask
ULONGLONG g_drawn_at = 0;
double g_seen[std::size(kWatched)] = {};
bool g_stale = true;

// The window made taller: its control and how much; each window under it moved down, where to and by how much
struct Grown {
    char* ctrl = nullptr;
    int extra = 0;
};
Grown g_grown;
struct Lowered {
    char* ctrl;
    short y;
    int by;
};
std::vector<Lowered> g_lowered;

// The elements' balls raised and the Attack and Defense words' letters moved out of the window, down (each quad, where
// VanaCore put it and by how much: up for more than 0)
struct Raised {
    char* quad;
    short y;
    int by;
};
std::vector<Raised> g_raised;
// How far down the letters go: out of the window and off the screen. (See-through, True Font still drew their outline: the
// words stood black over the elements.)
constexpr int kAwayDown = 0x1000;

std::unordered_map<std::string, int> g_widths; // text measured, by its text

alignas(16) char g_line[kQuadSize];

template <typename T> T& field(char* base, std::ptrdiff_t offset) { return *reinterpret_cast<T*>(base + offset); }
template <typename T> T value_at(const std::uint8_t* at)
{
    T out{};
    std::memcpy(&out, at, sizeof out);
    return out;
}
std::uintptr_t call_target(std::uintptr_t call) { return call + 5 + value_at<std::int32_t>(reinterpret_cast<const std::uint8_t*>(call + 1)); }

// ── the item data ────────────────────────────────────────────────────────────────────────────────────────────────────

using StatIndex = std::unordered_map<std::string, int>;

int stat_of(const StatIndex& index, const json::Value& name)
{
    if (!name.is_string())
        return -1;
    const auto found = index.find(name.string);
    return found != index.end() ? found->second : -1;
}

bool read_file(const std::wstring& path, std::string& out)
{
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr)
        return false;
    char buffer[65536];
    for (std::size_t got; (got = std::fread(buffer, 1, sizeof buffer, file)) > 0;)
        out.append(buffer, got);
    std::fclose(file);
    return true;
}

void read_stats(const json::Value& stats, StatIndex& index)
{
    for (const json::Value& row : stats.array) {
        if (!row.is_array() || row.array.size() < 5 || !row.array[0].is_string() || !row.array[1].is_string() ||
            index.count(row.array[0].string) != 0 || g_stats.size() >= 0xFFFF)
            continue;
        index.emplace(row.array[0].string, static_cast<int>(g_stats.size()));
        g_stats.push_back({row.array[1].string, std::max(1, static_cast<int>(row.array[2].number)), row.array[3].number != 0,
                           row.array[4].number != 0, row.array.size() > 5 && row.array[5].is_string() ? row.array[5].string : "",
                           row.array[0].string});
    }
    for (int t = 0; t < kTotals; ++t)
        if (const auto found = index.find(kTotalMods[t]); found != index.end())
            g_total_at[t] = found->second;
}

void read_families(const json::Value& v, const StatIndex& index)
{
    g_family_of.assign(g_stats.size(), -1);
    const json::Value* families = v.find("families");
    if (families == nullptr || !families->is_array())
        return;
    for (const json::Value& row : families->array) {
        if (!row.is_array() || row.array.size() < 3 || !row.array[0].is_string() || !row.array[2].is_array())
            continue;
        Family family{row.array[0].string, row.array[1].is_string() ? row.array[1].string : "", {}, 0};
        for (const json::Value& name : row.array[2].array)
            if (const int stat = stat_of(index, name); stat >= 0 && g_family_of[stat] < 0)
                family.members.push_back(stat);
        if (family.members.size() < 2)
            continue;
        family.first = *std::min_element(family.members.begin(), family.members.end());
        for (const int stat : family.members)
            g_family_of[stat] = static_cast<int>(g_families.size());
        g_families.push_back(std::move(family));
    }
}

void read_items(const json::Value& items, const StatIndex& index)
{
    for (const auto& [key, mods] : items.object) {
        std::vector<Mod> kept;
        for (const auto& [name, value] : mods.object)
            if (const auto found = index.find(name); found != index.end() && value.is_number())
                kept.push_back({static_cast<std::uint16_t>(found->second), static_cast<std::int16_t>(value.number)});
        if (!kept.empty())
            g_items[static_cast<std::uint16_t>(std::atoi(key.c_str()))] = std::move(kept);
    }
}

void read_latents(const json::Value& latents, const StatIndex& index)
{
    for (const auto& [key, rows] : latents.object) {
        std::vector<Latent> kept;
        for (const json::Value& row : rows.array)
            if (const int stat = row.is_array() && row.array.size() >= 4 ? stat_of(index, row.array[0]) : -1; stat >= 0)
                kept.push_back({static_cast<std::uint16_t>(stat), static_cast<std::int16_t>(row.array[1].number),
                                static_cast<std::uint16_t>(row.array[2].number), static_cast<std::uint16_t>(row.array[3].number)});
        if (!kept.empty())
            g_latents[static_cast<std::uint16_t>(std::atoi(key.c_str()))] = std::move(kept);
    }
}

void read_augments(const json::Value& augments, const StatIndex& index)
{
    for (const auto& [key, rows] : augments.object) {
        std::vector<AugmentMod> kept;
        for (const json::Value& row : rows.array)
            if (const int stat = row.is_array() && row.array.size() >= 3 ? stat_of(index, row.array[0]) : -1; stat >= 0)
                kept.push_back({static_cast<std::uint16_t>(stat), static_cast<std::int16_t>(row.array[1].number),
                                static_cast<std::uint8_t>(row.array[2].number)});
        if (!kept.empty())
            g_augments[static_cast<std::uint16_t>(std::atoi(key.c_str()))] = std::move(kept);
    }
}

void read_descriptions(const json::Value& v, const StatIndex& index)
{
    if (const json::Value* words = v.find("words"); words != nullptr && words->is_object())
        for (const auto& [label, row] : words->object)
            if (const int stat = row.is_array() && row.array.size() >= 2 ? stat_of(index, row.array[0]) : -1; stat >= 0)
                g_words[label] = {static_cast<std::uint16_t>(stat), static_cast<std::int16_t>(row.array[1].number)};
    if (const json::Value* described = v.find("described"); described != nullptr && described->is_object())
        for (const auto& [key, hash] : described->object)
            if (hash.is_number())
                g_described[static_cast<std::uint16_t>(std::atoi(key.c_str()))] = static_cast<std::uint32_t>(hash.number);
}

std::size_t read_sets(const json::Value& v, const StatIndex& index); // how many (equipstats_sets.h)
void add_unshown(const json::Value& v, StatIndex& index);             // (equipstats_lua.h)

void read_data()
{
    const std::wstring path = g_dir + L"\\addons\\vanacore\\libs\\gamedata\\equip_mods.json";
    std::string text, error;
    json::Value v;
    const json::Value* stats = nullptr;
    if (!read_file(path, text) || !json::parse(text, v, error) || !v.is_object() || (stats = v.find("stats")) == nullptr ||
        !stats->is_array()) {
        log::write(L"Equipment stats: %ls wasn't read (%hs): only the server's totals are shown", path.c_str(),
                   !error.empty() ? error.c_str() : text.empty() ? "missing" : "no stats in it: an older file");
        g_data.store(kMissing);
        return;
    }
    StatIndex index;
    read_stats(*stats, index);
    add_unshown(v, index);
    if (const json::Value* items = v.find("items"); items != nullptr && items->is_object())
        read_items(*items, index);
    if (const json::Value* latents = v.find("latents"); latents != nullptr && latents->is_object())
        read_latents(*latents, index);
    if (const json::Value* augments = v.find("augments"); augments != nullptr && augments->is_object())
        read_augments(*augments, index);
    if (const json::Value* charged = v.find("charged"); charged != nullptr && charged->is_array())
        for (const json::Value& id : charged->array)
            g_charged.insert(static_cast<std::uint16_t>(id.number));
    read_descriptions(v, index);
    const std::size_t sets = read_sets(v, index);
    read_families(v, index);
    log::write(L"Equipment stats: %u stats (%u families), %u items', %u items' latent and %u augments' modifiers, %u words, %u "
               L"descriptions and %u sets read (%hs; %hs)",
               static_cast<unsigned>(g_stats.size()), static_cast<unsigned>(g_families.size()), static_cast<unsigned>(g_items.size()), static_cast<unsigned>(g_latents.size()),
               static_cast<unsigned>(g_augments.size()), static_cast<unsigned>(g_words.size()), static_cast<unsigned>(g_described.size()),
               static_cast<unsigned>(sets), v.get_string("generated").c_str(), v.get_string("source").c_str());
    g_generated = v.get_string("generated"), g_source = v.get_string("source");
    g_data.store(kRead);
}

// ── latent effects ───────────────────────────────────────────────────────────────────────────────────────────────────

// What a latent effect's condition is tested against, read once a draw: the player's (party slot 0, the game's copy of
// its status, its entity's status: 1 engaged), its party's, and Vana'diel's clock (the server's time, as windower's world
// keeps it from the packets) and weather (0x057's, the server's Weather order)
struct World {
    bool known = false;
    int hp = 0, hp_max = 0, mp = 0, mp_max = 0, tp = 0;
    int main_job = 0, sub_job = 0, level = 0, nation = -1, zone = 0;
    int party = 0, in_zone = 0; // the party's members (trusts too; 0 alone), and those in the player's zone
    bool engaged = false;
    std::int16_t buffs[32] = {};
    int hour = 0, weekday = 0, moon = 0, moon_way = 0, weather = 0;
};

// The server's LATENT ids (latent_effect.h) tested here; the rest (a region's, a pet's, a target's, a specific food's,
// Dynamis, Assault...) the client can't know: those aren't counted
enum LatentId {
    kHpUnder = 0, kHpOver = 1, kHpUnderTpUnder100 = 2, kHpOverTpUnder100 = 3, kMpUnderPercent = 4, kMpUnder = 5, kTpUnder = 6,
    kTpOver = 7, kSubJob = 8, kWeaponDrawn = 10, kWeaponSheathed = 11, kStatusActive = 13, kNoFood = 14, kPartyMembers = 15,
    kPartyInZone = 16, kZone = 23, kTimeOfDay = 26, kHourOfDay = 27, kFirstDay = 28, kLastDay = 36, kMoonPhase = 37,
    kJobMultiple = 38, kEquippedInSlot = 40, kWeatherCondition = 42, kDrawnHpUnder = 43, kNationCitizen = 44,
    kLevelBelow = 50, kLevelAbove = 51, kWeatherElement = 52, kMpOver = 55, kDrawnMpOver = 56, kMainJob = 62
};
constexpr int kFood = 251; // EFFECT_FOOD
// FIRESDAY (28) to LIGHTSDAY (36) as Vana'diel's weekday (0 Firesday ... 7 Darksday); 33 is none
constexpr int kWeekdayOf[kLastDay - kFirstDay + 1] = {0, 1, 2, 3, 7, -1, 4, 5, 6};
// Each weather's element (zoneutils GetWeatherElement): 1 fire, 2 ice, 3 wind, 4 earth, 5 thunder, 6 water, 7 light, 8 dark
constexpr int kWeatherElements[20] = {0, 0, 0, 0, 1, 1, 6, 6, 4, 4, 3, 3, 2, 2, 5, 5, 7, 7, 8, 8};

void vana_clock(World& w)
{
    std::uint32_t server = 0, weather = 0;
    windower::state_world(server, weather);
    long long now = static_cast<long long>(std::time(nullptr));
    if (server != 0 && std::llabs(static_cast<long long>(server) - now) < 86400)
        now = server;
    const long long minutes = (now - 1009810800LL) * 25 / 60; // Vana'diel's, since its epoch (graphics.cpp's clock)
    const long long days = minutes / 1440;
    w.hour = static_cast<int>(minutes % 1440 / 60);
    w.weekday = static_cast<int>(days % 8);
    const int cycle = static_cast<int>((days + 886LL * 360 + 26) % 84); // vanadiel_time::moon
    w.moon = cycle >= 42 ? static_cast<int>(100.0 * (cycle - 42) / 42 + 0.5) : static_cast<int>(100.0 * (1.0 - cycle / 42.0) + 0.5);
    w.moon_way = cycle == 42 || cycle == 0 ? 0 : cycle < 42 ? 1 : 2; // 1 waning, 2 waxing
    w.weather = static_cast<int>(weather);
}

World read_world()
{
    World w;
    game::PartyMember members[18];
    game::Alliance alliance;
    game::Player player;
    if (!game::party(members, alliance) || !members[0].active || !game::player(player))
        return w;
    const game::PartyMember& me = members[0];
    w.known = true;
    w.hp = static_cast<int>(me.hp), w.mp = static_cast<int>(me.mp), w.tp = static_cast<int>(me.tp), w.zone = me.zone;
    w.hp_max = static_cast<int>(player.hp_max), w.mp_max = static_cast<int>(player.mp_max);
    w.main_job = player.main_job, w.sub_job = player.sub_job, w.level = player.main_job_level, w.nation = player.nation;
    std::copy(std::begin(player.buffs), std::end(player.buffs), w.buffs);
    w.party = alliance.party_counts[0] > 1 ? alliance.party_counts[0] : 0;
    for (int i = 0; i < 6 && w.party > 0; ++i)
        w.in_zone += members[i].active && members[i].zone == me.zone ? 1 : 0;
    game::Entity self{};
    w.engaged = game::entity(me.target_index, self) && self.status == 1;
    vana_clock(w);
    return w;
}

bool has_buff(const World& w, int id)
{
    return std::find(std::begin(w.buffs), std::end(w.buffs), static_cast<std::int16_t>(id)) != std::end(w.buffs);
}

// Each group of conditions: 1 true, 0 false, -1 not one of its own (latent_effect_container.cpp ProcessLatentEffect's tests)
int health_test(int id, int p, const World& w)
{
    const float hpp = w.hp_max > 0 ? 100.0f * static_cast<float>(w.hp) / static_cast<float>(w.hp_max) : 0.0f;
    switch (id) {
    case kHpUnder: return hpp <= p;
    case kHpOver: return hpp >= p;
    case kHpUnderTpUnder100: return hpp <= p && w.tp < 1000;
    case kHpOverTpUnder100: return hpp >= p && w.tp < 1000;
    case kMpUnderPercent: return w.mp_max > 0 && 100.0f * static_cast<float>(w.mp) / static_cast<float>(w.mp_max) <= p;
    case kMpUnder: return w.mp <= p;
    case kMpOver: return w.mp >= p;
    case kTpUnder: return w.tp < p;
    case kTpOver: return w.tp > p;
    case kDrawnHpUnder: return w.hp < p && w.engaged;
    case kDrawnMpOver: return w.mp > p && w.engaged;
    default: return -1;
    }
}

int self_test(int id, int p, const World& w)
{
    switch (id) {
    case kSubJob: return w.sub_job == p;
    case kMainJob: return w.main_job == p;
    case kWeaponDrawn: return w.engaged;
    case kWeaponSheathed: return !w.engaged;
    case kStatusActive: return has_buff(w, p);
    case kNoFood: return !has_buff(w, kFood);
    case kJobMultiple: return p == 0 ? w.level % 2 == 1 : w.level % p == 0;
    case kNationCitizen: return w.nation == p;
    case kLevelBelow: return w.level < p;
    case kLevelAbove: return w.level >= p;
    default: return -1;
    }
}

int place_test(int id, int p, int slot, const World& w)
{
    switch (id) {
    case kPartyMembers: return p <= w.party;
    case kPartyInZone: return p <= w.in_zone;
    case kZone: return w.zone == p;
    case kEquippedInSlot: return slot == p;
    case kWeatherCondition: return w.weather == p;
    case kWeatherElement: return w.weather >= 0 && w.weather < 20 && kWeatherElements[w.weather] == p;
    default: return -1;
    }
}

int day_test(int p, int h) // TIME_OF_DAY: 0 daytime, 1 nighttime, 2 dusk to dawn
{
    return p == 0 ? h >= 6 && h < 18 : p == 1 ? h >= 18 || h < 6 : p == 2 ? h >= 17 || h < 7 : 0;
}

int hour_test(int p, int h) // HOUR_OF_DAY: 1 new day, 2 dawn, 3 day, 4 dusk, 5 evening, 6 dead of night
{
    switch (p) {
    case 1: return h == 4;
    case 2: return h == 6;
    case 3: return h >= 7 && h < 17;
    case 4: return h >= 16 && h < 18;
    case 5: return h >= 18 && h < 20;
    case 6: return h >= 20 || h < 4;
    default: return 0;
    }
}

// MOON_PHASE: 0 new moon ... 4 full moon ... 7 waning crescent, as the moon's percent from low to high going `way` (1
// waning, 2 waxing, 0 either); new and full moon have a second range (the others' never matches)
struct MoonRange {
    int low, high, way;
};
constexpr MoonRange kMoonRanges[8][2] = {
    {{0, 5, 0}, {0, 10, 1}},   {{7, 38, 2}, {-1, -1, 0}},  {{40, 55, 2}, {-1, -1, 0}}, {{57, 88, 2}, {-1, -1, 0}},
    {{95, 100, 0}, {90, 100, 2}}, {{62, 93, 1}, {-1, -1, 0}}, {{45, 60, 1}, {-1, -1, 0}}, {{12, 43, 1}, {-1, -1, 0}}};

int moon_test(int p, int moon, int way)
{
    if (p < 0 || p > 7)
        return 0;
    for (const MoonRange& range : kMoonRanges[p])
        if (moon >= range.low && moon <= range.high && (range.way == 0 || range.way == way))
            return 1;
    return 0;
}

int time_test(int id, int p, const World& w)
{
    if (id == kTimeOfDay)
        return day_test(p, w.hour);
    if (id == kHourOfDay)
        return hour_test(p, w.hour);
    if (id == kMoonPhase)
        return moon_test(p, w.moon, w.moon_way);
    if (id >= kFirstDay && id <= kLastDay)
        return kWeekdayOf[id - kFirstDay] >= 0 && w.weekday == kWeekdayOf[id - kFirstDay];
    return -1;
}

// Whether the item in `slot`'s latent effect `l` is on now (one whose condition can't be known: not counted)
bool active(const Latent& l, int slot, const World& w)
{
    if (!w.known)
        return false;
    const int id = l.condition, p = l.param;
    for (const int result : {health_test(id, p, w), self_test(id, p, w), place_test(id, p, slot, w), time_test(id, p, w)})
        if (result >= 0)
            return result == 1;
    return false;
}

// ── what the gear adds ───────────────────────────────────────────────────────────────────────────────────────────────

void add_mod(std::vector<int>& sums, int stat, int value)
{
    sums[stat] = g_stats[stat].highest ? std::max(sums[stat], value) : sums[stat] + value;
}

std::int16_t to_16(int value) { return static_cast<std::int16_t>(std::clamp(value, -32768, 32767)); }

#include "equipstats_sets.h"

// `mods` with what description `text` says over them: each stat it gives a number for (its label one of "words"), the
// description's, except the same number with the other sign (the server's way round kept); a stat it doesn't give stays
// the server's (one a description words without a number: Enhances "Double Attack" effect)
void apply_description(std::vector<Mod>& mods, const std::string& text)
{
    std::vector<Mod> said;
    for (const equipdesc::Said& s : equipdesc::parse(text)) {
        const auto word = g_words.find(s.label);
        if (word == g_words.end())
            continue;
        const int value = s.value * word->second.scale;
        const auto at = std::find_if(said.begin(), said.end(), [&](const Mod& m) { return m.stat == word->second.stat; });
        if (at != said.end())
            at->value = to_16(at->value + value);
        else
            said.push_back({word->second.stat, to_16(value)});
    }
    for (const Mod& m : said) {
        const auto at = std::find_if(mods.begin(), mods.end(), [&](const Mod& o) { return o.stat == m.stat; });
        if (at == mods.end())
            mods.push_back(m);
        else if (std::abs(at->value) != std::abs(m.value))
            at->value = m.value;
    }
}

// An item's modifiers (game thread): the server's while the item's description is the one the data was made with, else
// (CatsEyeXI has changed the item since, or it's new) the server's with what its description says over them. Worked out
// once an item (its description read through VanaCore's item resources: Pivot's DATs); the server's alone while the
// item DATs can't be read.
const std::vector<Mod>& item_mods(std::uint16_t id)
{
    static std::unordered_map<std::uint16_t, std::vector<Mod>> resolved;
    static const std::vector<Mod> none;
    if (const auto found = resolved.find(id); found != resolved.end())
        return found->second;
    const auto sql = g_items.find(id);
    const char* raw = ashita::resource_item_description(id);
    if (raw == nullptr)
        return sql != g_items.end() ? sql->second : none;
    std::vector<Mod> mods = sql != g_items.end() ? sql->second : std::vector<Mod>{};
    const std::string text = equipdesc::ascii(raw);
    const auto described = g_described.find(id);
    if (described == g_described.end() || described->second != equipdesc::fingerprint(text))
        apply_description(mods, text);
    return resolved[id] = std::move(mods);
}

// Each augment's modifiers as the server works them out ((value > 0 ? value + n : value - n) * multiplier, in 16 bits;
// items/item_equipment.cpp SetAugmentMod) from the 11-bit id and 5-bit n in the extdata's words at +2, +4, +6, +8 (an
// item that isn't charged)
void augment_sums(std::vector<int>& sums, std::uint16_t id, const std::uint8_t* extra)
{
    if (g_charged.count(id) != 0)
        return;
    for (int i = 0; i < 4; ++i) {
        const auto word = value_at<std::uint16_t>(extra + 2 + i * 2);
        const auto found = word != 0 ? g_augments.find(static_cast<std::uint16_t>(word & 0x7FF)) : g_augments.end();
        if (found == g_augments.end())
            continue;
        const int n = word >> 11;
        for (const AugmentMod& mod : found->second) {
            const int base = mod.value > 0 ? mod.value + n : mod.value - n;
            add_mod(sums, mod.stat, static_cast<std::int16_t>(base * std::max<int>(mod.multiplier, 1)));
        }
    }
}

// An item's modifiers (item_mods), its latent ones that are on, and its augments'
void item_sums(std::vector<int>& sums, const ashita::ItemSlot& item, int slot, const World& world)
{
    for (const Mod& mod : item_mods(item.id))
        add_mod(sums, mod.stat, mod.value);
    if (const auto found = g_latents.find(item.id); found != g_latents.end())
        for (const Latent& latent : found->second)
            if (active(latent, slot, world))
                add_mod(sums, latent.stat, latent.value);
    augment_sums(sums, item.id, item.extra);
}

// What the equipped gear adds to each stat (g_stats' order), its sets' bonuses with it
void gear(std::vector<int>& sums)
{
    sums.assign(g_stats.size(), 0);
    if (g_data.load() != kRead)
        return;
    const World world = read_world();
    std::uint16_t worn[16] = {};
    for (std::uint32_t slot = 0; slot < 16; ++slot) {
        std::uint32_t container = 0, index = 0;
        ashita::ItemSlot item{};
        if (ashita::equipped(slot, container, index) && index != 0 && ashita::container_item(container, index, item) && item.id != 0) {
            item_sums(sums, item, static_cast<int>(slot), world);
            worn[slot] = item.id;
        }
    }
    set_sums(sums, worn, 16);
}

// ── /checkparam ──────────────────────────────────────────────────────────────────────────────────────────────────────

void ask()
{
    game::PartyMember members[18];
    game::Alliance alliance;
    if (!packets::ready() || !game::party(members, alliance) || !members[0].active || members[0].id == 0)
        return;
    std::uint8_t packet[16] = {};
    const std::uint32_t id = members[0].id, index = members[0].target_index;
    std::memcpy(packet + 4, &id, 4);
    std::memcpy(packet + 8, &index, 4);
    packet[0x0C] = kCheckParam;
    {
        const std::lock_guard lock(g_lock);
        if (g_self != id)
            g_totals = Totals{}; // (another character's)
        g_self = id;
        ++g_expected;
        g_asked_at = GetTickCount64();
        g_awaiting.store(true);
    }
    packets::inject(false, kInspect, sizeof packet, packet);
}

// An answer that hasn't come (the server busy, a zone change): given up, so the player's own /checkparam shows again
void give_up_late()
{
    const std::lock_guard lock(g_lock);
    if (g_awaiting.load() && GetTickCount64() - g_asked_at > kAnswerWithin) {
        g_expected = 0;
        g_in_answer = false;
        g_awaiting.store(false);
    }
}

// Asked when the window opens and when the gear, the stats or the effects change while it's open, a second apart at most
void ask_when_due(const char* ctrl)
{
    static const char* drawn = nullptr;
    const ULONGLONG now = GetTickCount64();
    if (ctrl != drawn || now - g_drawn_at > kClosedAfter)
        g_stale = true; // (opened)
    drawn = ctrl, g_drawn_at = now;
    for (std::size_t i = 0; i < std::size(kWatched); ++i) {
        double at = 0;
        packets::last(true, kWatched[i], &at);
        if (at != g_seen[i])
            g_seen[i] = at, g_stale = true;
    }
    if (!g_ask || !g_stale || g_awaiting.load() || now - g_asked_at < kAskEvery)
        return;
    g_stale = false;
    ask();
}

// ── the window ───────────────────────────────────────────────────────────────────────────────────────────────────────

struct Rect {
    short left, top, right, bottom;
};
Rect rect_of(char* ctrl) { return field<Rect>(ctrl, kRectAt); }

// The window's height as the game makes it: its layout's (a window reused while it closes can be mid-shrink)
int game_height(char* ctrl)
{
    char* layout = field<char*>(ctrl, kLayoutAt);
    const Rect own = layout != nullptr ? field<Rect>(layout, kLayoutRect) : Rect{};
    const int height = own.bottom - own.top;
    return height > 0 ? height : rect_of(ctrl).bottom - rect_of(ctrl).top - g_grown.extra;
}

// A window `by` lower (or higher), its own rect with it
void shift(char* ctrl, int by)
{
    const Rect r = rect_of(ctrl);
    g_resize(ctrl, r.left, r.top + by, r.right - r.left, r.bottom - r.top, 1, 0, 0);
    char* window = field<char*>(ctrl, kWindowAt);
    if (window != nullptr && field<Rect>(window, kWindowRect).top == r.top) {
        field<Rect>(window, kWindowRect).top = static_cast<short>(r.top + by);
        field<Rect>(window, kWindowRect).bottom = static_cast<short>(field<Rect>(window, kWindowRect).bottom + by);
    }
}

template <typename Fn> void each_window(Fn fn)
{
    int nodes = 0;
    for (char* node = g_windows != nullptr ? *g_windows : nullptr; node != nullptr && nodes < 256;
         node = field<char*>(node, kNext), ++nodes)
        if (char* ctrl = field<char*>(node, kNodeItem); ctrl != nullptr)
            fn(ctrl);
}

Lowered* lowered_entry(char* ctrl)
{
    for (Lowered& entry : g_lowered)
        if (entry.ctrl == ctrl && rect_of(ctrl).top == entry.y)
            return &entry;
    return nullptr;
}

// The windows under the game's bottom of the Equipment window, at its left: `extra` lower (back where they were for 0).
// One VanaCore already moved is moved by the difference; the game putting one back is seen and it's moved again. One the
// HUD Manager's X and Y place (an item's description with itemdetails.move, dragged there) stays at its box: it went 228
// below it, and hudwin put it back each draw (reported 2026-11-06)
void lower_under(char* equip, int bottom, int extra)
{
    const short left = rect_of(equip).left;
    std::vector<Lowered> kept;
    each_window([&](char* ctrl) {
        if (ctrl == equip || hudwin::moved(ctrl))
            return;
        const Rect r = rect_of(ctrl);
        const Lowered* entry = lowered_entry(ctrl);
        const bool under = entry == nullptr && std::abs(r.left - left) <= 4 && r.top >= bottom - 4 && r.top < bottom + 8 &&
                           r.bottom > r.top;
        const int by = entry != nullptr ? extra - entry->by : under ? extra : 0;
        if (by != 0)
            shift(ctrl, by);
        if ((entry != nullptr || under) && extra != 0)
            kept.push_back({ctrl, rect_of(ctrl).top, extra});
    });
    g_lowered = std::move(kept);
}

// The window `extra` taller than the game's (shorter for less than 0), the windows under it lower by as much
void fit(char* ctrl, int extra)
{
    if (g_grown.ctrl != ctrl && extra == 0)
        return;
    const int height = game_height(ctrl);
    const Rect r = rect_of(ctrl);
    if (r.bottom - r.top != height + extra)
        g_resize(ctrl, r.left, r.top, r.right - r.left, height + extra, 1, 0, 0);
    if (g_grown.ctrl != ctrl)
        g_lowered.clear(); // (another window's: gone with it)
    g_grown = {extra != 0 ? ctrl : nullptr, extra};
    lower_under(ctrl, r.top + height, extra);
}

// ── the game's Attack and Defense row ────────────────────────────────────────────────────────────────────────────────

char* number(char* self, int i) { return self + kNumbers + i * kNumber; }

// How far the elements' rows go up into the Attack and Defense row (from its numbers' row to the elements' first), 0 if
// the numbers aren't laid out as expected
int rise_of(char* self)
{
    const short row = field<short>(number(self, kAttackNumber), kNumberRow);
    if (field<short>(number(self, kDefenseNumber), kNumberRow) != row)
        return 0;
    int first = 0x7FFF;
    for (int i = 0; i < kElementNumbers; ++i)
        first = std::min<int>(first, field<short>(number(self, i), kNumberRow));
    const int rise = first - row;
    return rise > 0 && rise <= 32 ? rise : 0;
}

// Around the game's drawing of the window: the elements' numbers `rise` higher, Attack's and Defense's with no digits
class Numbers {
public:
    Numbers(char* self, int rise) : self_(self), rise_(rise)
    {
        if (rise_ == 0)
            return;
        for (int i = 0; i < kElementNumbers; ++i)
            field<short>(number(self_, i), kNumberY) = static_cast<short>(field<short>(number(self_, i), kNumberY) - rise_);
        for (int i = 0; i < 2; ++i)
            digits_[i] = std::exchange(field<std::uint8_t>(number(self_, kAttackNumber + i), kDigits), std::uint8_t{0});
    }
    ~Numbers()
    {
        if (rise_ == 0)
            return;
        for (int i = 0; i < kElementNumbers; ++i)
            field<short>(number(self_, i), kNumberY) = static_cast<short>(field<short>(number(self_, i), kNumberY) + rise_);
        for (int i = 0; i < 2; ++i)
            field<std::uint8_t>(number(self_, kAttackNumber + i), kDigits) = digits_[i];
    }
    Numbers(const Numbers&) = delete;
    Numbers& operator=(const Numbers&) = delete;

private:
    char* self_;
    int rise_;
    std::uint8_t digits_[2] = {};
};

// Each quad of the window's pictures
template <typename Fn> void each_quad(char* ctrl, Fn fn)
{
    int pictures = 0;
    for (char* node = field<char*>(ctrl, kPictures); node != nullptr && pictures < 32; node = field<char*>(node, kNext), ++pictures) {
        char* picture = field<char*>(node, kNodeItem);
        char* shape = picture != nullptr && field<short>(picture, 0) == 0 ? field<char*>(picture, kShape) : nullptr;
        if (shape == nullptr)
            continue;
        const int count = field<int>(shape, kPartCount);
        int parts = 0;
        for (char* part = field<char*>(shape, kParts); part != nullptr && parts < count && parts < 128;
             part = field<char*>(part, kNext), ++parts)
            if (char* quad = field<char*>(part, kNodeItem); quad != nullptr)
                fn(quad);
    }
}

short& corner_y(char* quad, int corner) { return field<short>(quad, kLocalY + corner * 2); }

// How far up VanaCore has moved a quad (0: where the game put it, or the game has put it back since)
int moved_by(char* quad)
{
    const auto old = std::find_if(g_raised.begin(), g_raised.end(), [quad](const Raised& r) { return r.quad == quad; });
    return old != g_raised.end() && corner_y(quad, 0) == old->y ? old->by : 0;
}

// A quad `rise` higher than the game put it (down for less than 0; 0: back), from where VanaCore last put it
void raise(char* quad, int rise, std::vector<Raised>& kept)
{
    const int by = moved_by(quad);
    for (int i = 0; by != rise && i < 4; ++i)
        corner_y(quad, i) = static_cast<short>(corner_y(quad, i) + by - rise);
    if (rise != 0)
        kept.push_back({quad, corner_y(quad, 0), rise});
}

// The Attack and Defense row taken out of the window's picture (its words' letters, in `row`, moved out of the window, the
// balls `rise` higher), or put back for a `rise` of 0
void reshape(char* ctrl, int rise, int row)
{
    if (rise == 0 && g_raised.empty())
        return;
    std::vector<Raised> raised;
    each_quad(ctrl, [&](char* quad) {
        if (std::memcmp(quad + kQuadName, kBall, sizeof kBall) == 0)
            raise(quad, rise, raised);
        else if (std::memcmp(quad + kQuadName, kLetterSheet, sizeof kLetterSheet) == 0 &&
                 (moved_by(quad) != 0 || (rise != 0 && std::abs(corner_y(quad, 0) - row) <= 6)))
            raise(quad, rise != 0 ? -kAwayDown : 0, raised);
    });
    g_raised = std::move(raised);
}

// ── the stats ────────────────────────────────────────────────────────────────────────────────────────────────────────

int text(int x, int y, const char* line) { return static_cast<short>(g_string(x, y, line, kWhite)); }

// The game's width for `line`
int game_width(const char* line)
{
    if (const auto found = g_widths.find(line); found != g_widths.end())
        return found->second;
    if (g_widths.size() > 512)
        g_widths.clear();
    return g_widths[line] = text(kAway, kAway, line) - kAway;
}

// The True Font area the stats are drawn in: the Equipment window's stats' (equip.stats.font, d3d8_chatfont.h cf_area), the
// Check window's box's too while it's on, else the menus' (equipstats_check.h)
int g_font = VANA_FONT_EQUIP_STATS;

// `line` as one whole text in its True Font, from x or (`right`) ending at x + the game's width: VanaCore's own text box
// (listtext.cpp keep_stat), the game's letters never printed, so the cursor's hand drawn over them can't give one back to the
// game's font (reported 2026-11-14). Else the game's letters laid whole by the graphics layer (vanacore_gfx_text_whole)
void whole(int x, int y, const char* line, int right)
{
    if (listtext::keep_stat(g_font, right != 0 ? x + game_width(line) : x, y, line, right != 0))
        return;
    vanacore_gfx_text_whole(1, right, line);
    text(x, y, line);
    vanacore_gfx_text_whole(0, 0, nullptr);
}

// `line`'s width as it's drawn: the game's, or True Font's where that's wider (each word is laid from where the game's starts,
// by the font's own advances: at 17 to 19 px "Physical damage taken" and "MP recovered while healing" ran into their
// numbers, reported 2026-11-06)
int width(const char* line)
{
    const int game = game_width(line);
    const float own = chatfont::text_width(g_font, line);
    return std::max(game, static_cast<int>(std::ceil(own)));
}

// A flat line `length` long: font 0's dash, every corner on the white middle of its stroke, as two quads that fade out
// at the line's two ends
void line(int x, int y, int length)
{
    char** font = g_quad != nullptr ? g_fonts(0) : nullptr;
    char* dash = font != nullptr && *font != nullptr ? reinterpret_cast<char**>(*font)['-' - 0x20] : nullptr;
    char* node = dash != nullptr ? field<char*>(dash, kParts) : nullptr;
    const char* quad = node != nullptr ? field<char*>(node, kNodeItem) : nullptr;
    if (quad == nullptr)
        return;
    std::memcpy(g_line, quad, kQuadSize);
    char* const first = g_line + kCorners;
    const float u = field<float>(first, kU) + (field<float>(first + kCorner, kU) - field<float>(first, kU)) * kDashWhite[0];
    const float v = field<float>(first, kV) + (field<float>(first + 2 * kCorner, kV) - field<float>(first, kV)) * kDashWhite[1];
    const short half = static_cast<short>(length / 2);
    for (int side = 0; side < 2; ++side) {
        for (int corner = 0; corner < 4; ++corner) {
            const bool right = (corner & 1) != 0;
            field<short>(g_line, kLocalX + corner * 2) = right ? half : 0;
            field<short>(g_line, kLocalY + corner * 2) = static_cast<short>(corner >> 1);
            field<float>(first + corner * kCorner, kU) = u;
            field<float>(first + corner * kCorner, kV) = v;
            const bool faded = side == 0 ? !right : right;
            field<std::uint32_t>(first + corner * kCorner, kColour) = faded ? kLineColour & 0x00FFFFFF : kLineColour;
        }
        g_quad(g_line, x + side * half, y, 1.0f, 1.0f, kWhite, 0);
    }
}

// One stat to show: its label, its short one (none: empty) and its number as written
struct Cell {
    const char* label;
    const char* brief;
    char value[16];
};

void write_value(Cell& cell, int value, const Stat& stat, bool sign)
{
    const char* percent = stat.percent ? "%" : ""; // (one word with the number: d3d8_chatfont.h's cf_number_part)
    const char* plus = value < 0 ? "-" : sign ? "+" : "";
    const int whole = std::abs(value) / stat.scale, part = std::abs(value) % stat.scale;
    if (part == 0)
        std::snprintf(cell.value, sizeof cell.value, "%s%d%s", plus, whole, percent);
    else if (part % 10 == 0)
        std::snprintf(cell.value, sizeof cell.value, "%s%d.%d%s", plus, whole, part / 10, percent);
    else
        std::snprintf(cell.value, sizeof cell.value, "%s%d.%02d%s", plus, whole, part, percent);
}

void add(std::vector<Cell>& cells, const char* label, int value, const Stat& stat, bool sign, const char* brief = "")
{
    Cell cell{label, brief, {}};
    write_value(cell, value, stat, sign);
    cells.push_back(cell);
}

// A total's stat as what the gear adds to it (none when it adds nothing)
void add_gear(std::vector<Cell>& cells, const std::vector<int>& sums, Total total)
{
    const int at = g_total_at[total];
    if (at >= 0 && at < static_cast<int>(sums.size()) && sums[at] != 0)
        add(cells, g_stats[at].label.c_str(), sums[at], g_stats[at], true, g_stats[at].brief.c_str());
}

// The totals: the server's (/checkparam), else Attack and Defense as the game has them and what the gear adds to the
// rest
void totals_of(std::vector<Cell>& totals, const std::vector<int>& sums)
{
    Totals server;
    {
        const std::lock_guard lock(g_lock);
        server = g_totals;
    }
    if (g_ask && server.known) {
        add(totals, "Accuracy", server.acc, kPlain, false);
        add(totals, "Attack", server.att, kPlain, false);
        if (server.acc_sub != 0 || server.att_sub != 0) {
            add(totals, "Accuracy (Sub)", server.acc_sub, kPlain, false);
            add(totals, "Attack (Sub)", server.att_sub, kPlain, false);
        }
        if (server.racc != 0 || server.ratt != 0) {
            add(totals, "Ranged Accuracy", server.racc, kPlain, false);
            add(totals, "Ranged Attack", server.ratt, kPlain, false);
        }
        add(totals, "Defense", server.def, kPlain, false);
        add(totals, "Evasion", server.eva, kPlain, false);
        return;
    }
    game::Player player;
    const bool known = game::player(player);
    add_gear(totals, sums, kAcc);
    if (known)
        add(totals, "Attack", player.attack, kPlain, false);
    add_gear(totals, sums, kRacc);
    add_gear(totals, sums, kRatt);
    if (known)
        add(totals, "Defense", player.defense, kPlain, false);
    add_gear(totals, sums, kEva);
}

// What every member of a family adds when it's the same for all (shown as one row), else 0
int folded(const Family& family, const std::vector<int>& sums)
{
    const int value = sums[family.first];
    for (const int stat : family.members)
        if (sums[stat] != value)
            return 0;
    return value;
}

// The totals, then every other stat the gear changes, a family whose members all add the same as one row where its
// first would be
void cells(std::vector<Cell>& totals, std::vector<Cell>& added)
{
    std::vector<int> sums;
    gear(sums);
    totals_of(totals, sums);
    for (std::size_t i = 0; i < sums.size(); ++i) {
        if (sums[i] == 0 || !g_stats[i].shown || std::find(std::begin(g_total_at), std::end(g_total_at), static_cast<int>(i)) != std::end(g_total_at))
            continue;
        const Family* family = i < g_family_of.size() && g_family_of[i] >= 0 ? &g_families[g_family_of[i]] : nullptr;
        if (family == nullptr || folded(*family, sums) == 0)
            add(added, g_stats[i].label.c_str(), sums[i], g_stats[i], true, g_stats[i].brief.c_str());
        else if (static_cast<int>(i) == family->first)
            add(added, family->label.c_str(), sums[i], g_stats[i], true, family->brief.c_str());
    }
}

int rows_of(std::size_t count, int most) { return std::min(most, static_cast<int>(count)); }

constexpr int kColumnGap = 12; // the least room between a label and its number

// A label that fits in `room`: its own, else its short one, else the shorter of them cut short and ended with "." (the
// Equipment window is the game's 182 wide; "HP recovered while healing" ran into its number)
std::unordered_map<std::string, std::string> g_cut; // by the label and the room

const char* fitted(const Cell& cell, int room)
{
    if (width(cell.label) <= room)
        return cell.label;
    if (cell.brief[0] != '\0' && width(cell.brief) <= room)
        return cell.brief;
    if (g_cut.size() > 256)
        g_cut.clear();
    // (by its width too: the font's size changes where it's cut)
    const auto [it, made] =
        g_cut.try_emplace(std::string(cell.label) + '\n' + std::to_string(room) + '\n' + std::to_string(width(cell.label)));
    if (made) {
        std::string text = cell.brief[0] != '\0' ? cell.brief : cell.label;
        while (text.size() > 1 && width((text + ".").c_str()) > room)
            text.pop_back();
        while (text.size() > 1 && (text.back() == ' ' || text.back() == '.'))
            text.pop_back();
        it->second = text + ".";
    }
    return it->second.c_str();
}

// A block's line (none for the first in a box of their own: `lined`) and its stats from `y` (the line's place), one a row,
// where the next starts
int draw_block(const std::vector<Cell>& list, int left, int right, int y, int most, bool lined)
{
    if (lined)
        line(left + kInset, y + kLineGap, right - left - kInset * 2);
    const int rows = rows_of(list.size(), most);
    for (int i = 0; i < rows; ++i) {
        const int row = y + kBlockGap + i * kRow;
        const int number = width(list[i].value);
        whole(left + kInset, row, fitted(list[i], right - left - kInset * 2 - kColumnGap - number), 0);
        // (the game's number ending at the column's edge: True Font's ends where the game's does)
        whole(right - kInset - game_width(list[i].value), row, list[i].value, 1);
    }
    return y + kBlockGap + rows * kRow;
}

constexpr int kAllTotals = 64;

// How tall the stats are: each block's line and rows, then the room under the last (0 with none)
int stats_height(const std::vector<Cell>& totals, const std::vector<Cell>& added)
{
    int ours = 0;
    if (!totals.empty())
        ours += kBlockGap + rows_of(totals.size(), kAllTotals) * kRow;
    if (!added.empty())
        ours += kBlockGap + rows_of(added.size(), kMaxGearRows) * kRow;
    return ours != 0 ? ours + kGlyph - kRow + kBottom : 0;
}

// How wide a box of their own is: the Equipment window's width, or wider for the longest label and its number side by side
// (under the window, "Spell interruption rate" ran into its number)
int stats_width(const std::vector<Cell>& totals, const std::vector<Cell>& added, int least)
{
    int widest = least;
    const auto fit_rows = [&](const std::vector<Cell>& list, int most) {
        for (int i = 0; i < rows_of(list.size(), most); ++i)
            widest = std::max(widest, kInset + width(list[i].label) + kColumnGap + width(list[i].value) + kInset);
    };
    fit_rows(totals, kAllTotals);
    fit_rows(added, kMaxGearRows);
    return widest;
}

// A box's top-left corner for X `x` (its left edge, equip.stats.x) and Y `y` (its top, .y), percents of the interface,
// kept inside it; false while the interface's area isn't known
bool box_place(double x, double y, int width, int height, int& left, int& top)
{
    int area_left = 0, area_top = 0, area_right = 0, area_bottom = 0;
    if (!uispace::area(area_left, area_top, area_right, area_bottom) || area_right <= area_left || area_bottom <= area_top)
        return false;
    left = area_left + static_cast<int>(std::lround((area_right - area_left) * x / 100.0));
    top = area_top + static_cast<int>(std::lround((area_bottom - area_top) * y / 100.0));
    left = std::clamp(left, area_left, std::max(area_left, area_right - width));
    top = std::clamp(top, area_top, std::max(area_top, area_bottom - height));
    return true;
}

// The stats in a box of their own (equip.stats.move): the game's window frame, then the blocks. Drawn inside the
// Equipment window's draw, as a window of its own for [client] window.corners (its text kept in its own corners).
void draw_box(const std::vector<Cell>& totals, const std::vector<Cell>& added, int least_width, int height)
{
    if (height == 0)
        return;
    const int box_w = stats_width(totals, added, least_width);
    g_box_w = box_w, g_box_h = height;
    int left = 0, top = 0;
    if (!box_place(g_x, g_y, box_w, height, left, top))
        return;
    vanacore_gfx_window_drawing(1);
    const Frame frame{box_w, height, 1};
    g_frame(&frame, left, top, 1.0f, 1.0f, kWhite, 0, 0);
    int y = top;
    if (!totals.empty())
        y = draw_block(totals, left, left + box_w, y, kAllTotals, false);
    if (!added.empty())
        draw_block(added, left, left + box_w, y, kMaxGearRows, !totals.empty());
    vanacore_gfx_window_drawing(0);
}

// The stats under the game's part of the window, which is `rise` shorter (its Attack and Defense row taken out), or in a box
// of their own (equip.stats.move) with the window left at that height
void draw_stats(char* ctrl, int rise)
{
    std::vector<Cell> totals, added;
    cells(totals, added);
    const Rect r = rect_of(ctrl);
    const int ours = stats_height(totals, added);
    if (g_move && g_frame != nullptr) {
        fit(ctrl, -rise);
        draw_box(totals, added, r.right - r.left, ours);
        return;
    }
    fit(ctrl, ours - rise);
    if (ours == 0)
        return;
    int y = r.top + game_height(ctrl) - rise;
    g_attached = {r.left, y, r.right, y + ours};
    g_attached_seen = true;
    if (!totals.empty())
        y = draw_block(totals, r.left, r.right, y, kAllTotals, true);
    if (!added.empty())
        draw_block(added, r.left, r.right, y, kMaxGearRows, true);
}

// The stats' letters in True Font's Equipment Stats area while they're drawn in the Equipment window's (chatfont.cpp Area: its
// slot names are the font sheet's text too, the slots' area's, and took the stats' size when one area's font served both,
// reported 2026-11-06); nothing while the window's area isn't known (no True Font for it)
class StatsArea {
public:
    StatsArea() : was_(vanacore_gfx_text_area(VANA_FONT_EQUIP_STATS))
    {
        if (was_ != VANA_FONT_EQUIP) {
            vanacore_gfx_text_area(was_);
            was_ = -1;
        }
    }
    ~StatsArea()
    {
        if (was_ >= 0)
            vanacore_gfx_text_area(was_);
    }
    StatsArea(const StatsArea&) = delete;
    StatsArea& operator=(const StatsArea&) = delete;

private:
    int was_;
};

void __fastcall draw_hook(char* self, void*)
{
    char* ctrl = self != nullptr ? field<char*>(self, kCtrlAt) : nullptr;
    const int rise = g_on && ctrl != nullptr ? rise_of(self) : 0;
    {
        const Numbers numbers(self, rise);
        g_draw(self);
    }
    if (ctrl == nullptr)
        return;
    reshape(ctrl, rise, rise != 0 ? field<short>(number(self, kAttackNumber), kNumberRow) : 0);
    if (!g_on) {
        fit(ctrl, 0);
        return;
    }
    int expected = kUnread;
    if (g_data.compare_exchange_strong(expected, kReading))
        std::thread(read_data).detach();
    ask_when_due(ctrl);
    const StatsArea area;
    draw_stats(ctrl, rise);
}

// A [client] percent (0 to 100), `fallback` when it isn't set or isn't a number
double percent(const wchar_t* key, double fallback, const wchar_t* ini)
{
    wchar_t text[32] = {};
    GetPrivateProfileStringW(L"client", key, L"", text, 32, ini);
    wchar_t* end = nullptr;
    const double value = std::wcstod(text, &end);
    return text[0] != 0 && end != text ? std::clamp(value, 0.0, 100.0) : fallback;
}

#include "equipstats_check.h" // (the Check window's: someone you /check; and the HUD Manager's boxes, outline_box)
#include "equipstats_dump.h"  // (/vana statsdump)
#include "equipstats_lua.h"   // (vana.gamedata.item_stats)

// VanaCore's folder: VanaCore.Core.dll's (where the item data is, under addons\vanacore)
std::wstring own_folder()
{
    HMODULE self = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&own_folder), &self) ||
        GetModuleFileNameW(self, path, MAX_PATH) == 0)
        return L".";
    std::wstring folder = path;
    const std::size_t slash = folder.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : folder.substr(0, slash);
}

void install_hook()
{
    g_tried = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return;
    const std::uintptr_t at = scan::find(game, kEquipDraw);
    const std::uintptr_t string = scan::find(game, kDrawString);
    const std::uintptr_t fonts = scan::find(game, kFonts);
    const std::uintptr_t resize = scan::find(game, kResize);
    const std::uintptr_t windows = scan::find(game, kWindows);
    if (at == 0 || string == 0 || fonts == 0 || resize == 0 || windows == 0) {
        log::write(L"Equipment stats: the game's Equipment window, text, window list or resize wasn't found in FFXiMain.dll: "
                   L"[client] equip.stats does nothing");
        return;
    }
    g_string = reinterpret_cast<DrawString>(string);
    g_fonts = reinterpret_cast<Fonts>(fonts);
    g_resize = reinterpret_cast<Resize>(resize);
    g_windows = value_at<char* const*>(reinterpret_cast<const std::uint8_t*>(windows + kWindowsAt));
    if (const std::uintptr_t shape = scan::find(game, kShapeDraw); shape != 0 && *reinterpret_cast<const std::uint8_t*>(shape + kQuadCall) == 0xE8)
        g_quad = reinterpret_cast<DrawQuad>(call_target(shape + kQuadCall));
    else
        log::write(L"Equipment stats: the game's quad drawing wasn't found in FFXiMain.dll: no lines between the blocks");
    g_frame = reinterpret_cast<FrameDraw>(scan::find(game, kFrameDraw));
    if (g_frame == nullptr || !uispace::find(game)) {
        g_frame = nullptr;
        log::write(L"Equipment stats: the game's window frame or interface area wasn't found in FFXiMain.dll: "
                   L"[client] equip.stats.move does nothing (the stats stay under the Equipment window)");
    }
    if (!imports::detour(reinterpret_cast<EquipDraw>(at), kPrologue, reinterpret_cast<EquipDraw>(&draw_hook), g_draw)) {
        log::write(L"Equipment stats: the game's drawing of the Equipment window couldn't be hooked: [client] equip.stats does nothing");
        return;
    }
    log::write(L"Equipment stats: the Equipment window's drawing hooked at +0x%X", static_cast<unsigned>(at - reinterpret_cast<std::uintptr_t>(game)));
    install_check_hook(game);
}

} // namespace

void inspected(const std::uint8_t* packet, std::size_t length)
{
    inspect_packet(packet, length);
}

int dump() { return dump_all(); }

int lua_item_stats(lua_State* L) { return item_stats_lua(L); }

void reload(const wchar_t* ini)
{
    g_on = GetPrivateProfileIntW(L"client", L"equip.stats", 0, ini) != 0;
    listtext::set_stats(g_on); // (the windows noted as they're drawn, the stats' text kept in the one it's drawn in)
    g_ask = GetPrivateProfileIntW(L"client", L"equip.stats.checkparam", 1, ini) != 0;
    g_move = GetPrivateProfileIntW(L"client", L"equip.stats.move", 0, ini) != 0;
    g_x = percent(L"equip.stats.x", kDefaultX, ini);
    g_y = percent(L"equip.stats.y", kDefaultY, ini);
    check_reload(ini);
}

// (under the Equipment window as last drawn there, else where .x and .y put their box; dragged, in a box of their own at once)
bool outline(double* moved_x, double* moved_y)
{
    return g_on && outline_box("equip.stats", Placing{g_move, g_x, g_y, g_box_w, g_box_h, g_attached_seen ? &g_attached : nullptr},
                               moved_x, moved_y);
}

bool check_outline(double* moved_x, double* moved_y)
{
    return g_on && g_check_on &&
           outline_box("equip.stats.check",
                       Placing{g_check_move, g_check_x, g_check_y, g_check_w, g_check_h, g_check_seen ? &g_check_attached : nullptr},
                       moved_x, moved_y);
}

void on_frame(const wchar_t* ini)
{
    static bool read = false;
    if (!read && ini != nullptr && *ini != L'\0') {
        read = true;
        reload(ini);
        g_dir = own_folder();
    }
    give_up_late();
    if (!g_tried && g_on && read)
        install_hook();
}

int below(int left, int right, int y)
{
    char* equip = g_grown.ctrl;
    bool open = false;
    if (equip != nullptr && g_grown.extra > 0)
        each_window([&](char* ctrl) { open = open || (ctrl == equip && field<std::uint32_t>(ctrl, kStateAt) != kClosedState); });
    if (!open)
        return y;
    const Rect r = rect_of(equip);
    return left < r.right && right > r.left && y >= r.top && y < r.bottom + kUnderGap ? r.bottom + kUnderGap : y;
}

int lowered(const void* window)
{
    for (const Lowered& entry : g_lowered)
        if (entry.ctrl == window && rect_of(entry.ctrl).top == entry.y)
            return entry.by;
    return 0;
}

bool swallow(const std::uint8_t* packet, std::size_t length)
{
    if (!g_awaiting.load() || length < 0x1C || (value_at<std::uint16_t>(packet) & 0x1FF) != kBattleMessage)
        return false;
    const auto actor = value_at<std::uint32_t>(packet + 4), target = value_at<std::uint32_t>(packet + 8);
    const auto first = value_at<std::int32_t>(packet + 0x0C), second = value_at<std::int32_t>(packet + 0x10);
    const auto message = value_at<std::uint16_t>(packet + 0x18);
    const std::lock_guard lock(g_lock);
    if (actor != g_self || target != g_self)
        return false;
    switch (message) {
    case kName:
        if (g_expected == 0)
            return false;
        --g_expected, g_in_answer = true, g_coming = Totals{};
        return true;
    case kItemLevel:
        return g_in_answer;
    case kPrimary:
        if (g_in_answer)
            g_coming.acc = first, g_coming.att = second;
        return g_in_answer;
    case kAuxiliary:
        if (g_in_answer)
            g_coming.acc_sub = first, g_coming.att_sub = second;
        return g_in_answer;
    case kRange:
        if (g_in_answer)
            g_coming.racc = first, g_coming.ratt = second;
        return g_in_answer;
    case kDefense:
        if (!g_in_answer)
            return false;
        g_coming.eva = first, g_coming.def = second, g_coming.known = true;
        g_totals = g_coming, g_in_answer = false;
        g_awaiting.store(g_expected > 0);
        return true;
    default:
        return false;
    }
}

bool wants_packets() { return g_awaiting.load(); }

} // namespace vanacore::equipstats
