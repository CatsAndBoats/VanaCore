#include "profilewin.hpp"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iterator>

#include "../gfx/vanacore_gfx.h"
#include "chatfont.hpp"
#include "log.hpp"
#include "scan.hpp"
#include "text.hpp"

namespace vanacore::profilewin {
namespace {

template <typename T> T& at(char* base, std::ptrdiff_t offset) { return *reinterpret_cast<T*>(base + offset); }

// A window: its layout at +4 (named at +0x46); its pieces' list at +0x14 (a node: +0 the next, +0x10 the piece, +0x14 set
// for one taken out); a frame piece (type 0) has its picture at +8: its quads' list at +4 (a node's quad at +0x10), their
// count at +0x14. A quad: its texture's name at +0x32; its corners' places in the window at +0xC0 (x, 4 shorts) and +0xC8
// (y), TL, TR, BL, BR; their colours (B, G, R, A, each 0x80 full: drawn doubled) at +0x50 + 0x10, 0x1C a corner; its
// blending at +0x2E (A, B, D of the PS2's (A - B) * alpha + D, each 0 the quad, 1 what's under it, 2 none). The game keeps
// a quad's colours as its resource gave them and works out the drawn ones from them (hudwin.cpp, itemslot.cpp)
constexpr std::ptrdiff_t kWindowLayout = 4, kLayoutName = 0x46, kWinPieceList = 0x14, kNodePiece = 0x10, kNodeOut = 0x14;
constexpr std::ptrdiff_t kPiecePicture = 8, kShapeQuads = 4, kShapeCount = 0x14, kQuadName = 0x32, kQuadX = 0xC0, kQuadY = 0xC8;
constexpr std::ptrdiff_t kCorners = 0x50, kCorner = 0x1C, kColour = 0x10, kBlend = 0x2E;
constexpr short kPieceFrame = 0;
constexpr int kListAtMost = 256;
constexpr char kProfileName[16 + 1] = "menu    profile ";
// The Status menu's other windows whose title bar profile.title.* draws too (2026-11-08): Job Levels, Combat, Magic
// and Craft Skills (their names beside "menu    profile" in FFXiMain's table of the Status menu's windows, 0x1036F6C0)
constexpr char kTitled[][16 + 1] = {"menu    joblevel", "menu    btlskill", "menu    mgcskill", "menu    trdskill"};
constexpr char kLetterSheet[16 + 1] = "font    font    "; // a label's letter
// A title bar (read live 2026-11-08: the Profile panel's two, 0..252 wide, at 0 and 124; Job Levels' one, 0..252 at 0, the
// one gauge in its frame: its rows' orange cursor bars are its buttons', not the frame's, so left alone). In the kTitled
// windows only a gauge with its top at the window's top is one
constexpr char kBarName[16 + 1] = "menu    gauge   ";
constexpr short kTitleTop = 2;
constexpr std::uint8_t kBlendOver[3] = {0, 1, 1};       // the quad blended over what's under it

// profile.clean's labels. The panel's picture (CatsEyeXI's ROM\119\51 "menu    profile", 252 x 178, read live
// 2026-11-08) has its labels as letters ("font    font" sprites, one a letter), a row's letters' tops from its row's top to
// 3 below (capitals, small letters, colons): the rows here, each word in it in x order and where it goes. Two words a row
// for Rank / Rank Points and Mastery Rank / Mentor. The title rows ("Profile", "Monstrosity") stay
struct Word {
    short row;
    short to_y;
    bool colon; // ends in a colon, put after its last letter (the game's has a space before it on four of them)
};
constexpr Word kWords[] = {
    {20, 20, true},   // Nation:
    {36, 35, true},   // Race:
    {52, 50, true},   // Home Point :
    {68, 65, true},   // Rank :
    {68, 80, false},  // Rank Points
    {106, 95, true},  // Mastery Rank :
    {106, 110, true}, // Mentor :
    {145, 145, true}, // Gladiator Rank :
    {161, 160, true}, // Prestige:
};
constexpr short kTitleRows[] = {3, 127};
constexpr short kLabelLeft = 8;
constexpr int kRowSlack = 3, kWordGap = 12, kColonWidest = 5;

// The panel's own drawing of its values (CTkProfile::OnDrawPrimitive, FUN_101474b0: __thiscall on its owner, from the
// owner's vtable +10h, 0x10332818), each at the owner's left and top (+14h, +16h) plus a place in its code. Its places
// are the immediates of two adds before each draw (the patterns, unique in the function; read live 2026-11-08); the rank
// is drawn right-aligned at its x (0x101C82B0), which profile.clean makes the left-aligned draw (0x101C7EE0, the home
// point's: their first four arguments are the same, cdecl). The game's places are read back from the code as it's found.
// sub esp,0DCh; push esi; mov esi,ecx; call; test al,al; jz; mov al,[esi+1Ch]; test al,al; jnz
constexpr char kDraw[] = "81 EC DC 00 00 00 56 8B F1 E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 8A 46 1C 84 C0 0F 85";
constexpr std::size_t kDrawSize = 0x3EA; // through its last ret
constexpr short kColumn = 116;          // profile.clean's values' left edge, past the widest label ("Gladiator Rank:")
struct Site {
    const char* pattern;
    std::uint8_t x_at, x_size, y_at, y_size;
    short x, y; // profile.clean's
};
constexpr Site kSites[] = {
    {"83 C1 12 25 FF 00 00 00 68 80 80 80 80 83 C2 37", 15, 1, 2, 1, kColumn, 18},            // the nation's picture
    {"66 83 C1 24 50 66 83 C2 32", 8, 1, 3, 1, kColumn, 35},                                  // the race
    {"66 05 24 00 66 83 C1 32", 7, 1, 2, 2, kColumn, 35},                                     // the race (the other way)
    {"66 83 C1 34 66 83 C2 5E 50 51 52 E8", 7, 1, 3, 1, kColumn, 50},                         // the home point
    {"66 83 C2 44 51 66 05 40 00 52 50 E8", 7, 2, 3, 1, kColumn, 65},                         // the rank
    {"66 83 C1 54 D9 1C 24 52 66 8B 56 14 50 66 83 C2 5C", 16, 1, 3, 1, kColumn - 4, 81},     // its points' gauge (its arrow first)
    {"66 83 C1 6A 50 66 83 C2 6E", 8, 1, 3, 1, kColumn, 95},                                  // the mastery rank
    {"83 C1 68 25 FF 00 00 00 68 80 80 80 80 81 C2 DA 00 00 00", 15, 4, 2, 1, kColumn, 108}, // the mentor's flag
    {"66 83 C2 68 51 66 05 DA 00", 7, 2, 3, 1, kColumn, 110},                                 // no flag: its text
    {"66 05 91 00 52 66 81 C1 8C 00", 8, 2, 2, 2, kColumn, 145},                              // the gladiator rank
    {"66 81 C2 A1 00 51 66 05 5A 00", 8, 2, 3, 2, kColumn, 160},                              // the prestige
};
constexpr std::size_t kHomeSite = 3, kRankSite = 4, kCallAt = 11;

// craft.brackets=0: Craft Skills' levels without their brackets, "60" for "(60)". The craft panel's draw (FFXiMain
// 0x10148290, trdskill's vtable slot 4) prints each with sprintf "(%d)" (0x1036F898, that push its one user; read live
// 2026-11-09) and draws it right-aligned (0x101C82B0); the push is pointed at "%d" instead, the number ending where ")" did.
// shr ebx,5; and ebx,3FFh; lea edx,[esp+38h]; push ebx; push "(%d)"; push edx; call sprintf
constexpr char kCraftFormat[] = "C1 EB 05 81 E3 FF 03 00 00 8D 54 24 38 53 68 ?? ?? ?? ?? 52 E8";
constexpr std::size_t kCraftFormatAt = 15;
constexpr char kGameFormat[] = "(%d)";
constexpr char kBareFormat[] = "%d";

// [client] profile.*, craft.brackets
bool g_clean = false;
bool g_brackets = true;
bool g_coloured = false;     // profile.title.color set
std::uint32_t g_colour = 0;  // its B, G, R in the game's scale
int g_alpha = -1;            // the bars' alpha (0x80 full), -1 the game's

// The code, once found
bool g_searched = false;
std::uintptr_t g_draw = 0;
std::uintptr_t g_site[std::size(kSites)] = {};
int g_game_x[std::size(kSites)] = {}, g_game_y[std::size(kSites)] = {};
std::uintptr_t g_left_draw = 0;
std::int32_t g_rank_rel = 0;
bool g_patched = false;
bool g_odd_logged = false;
bool g_craft_searched = false;
std::uintptr_t g_craft_push = 0; // the push's operand, once found
std::uint32_t g_craft_game = 0;  // what it held: the game's "(%d)"
bool g_bare = false;             // it points at kBareFormat

// What a draw changed, put back as it ends
struct Moved {
    char* quad;
    short x[4], y[4];
};
struct Bar {
    char* quad;
    std::uint32_t colour[4];
    std::uint8_t blend[3];
};
Moved g_moved[128];
int g_moved_count = 0;
Bar g_bars[8];
int g_bar_count = 0;
// profile.title.font: the title rows' letters ("Profile", "Monstrosity") of the panel being drawn, for TitleQuad
const char* g_titles[32];
int g_title_count = 0;

int read_imm(std::uintptr_t p, int size)
{
    return size == 1 ? *reinterpret_cast<const std::int8_t*>(p)
           : size == 2 ? *reinterpret_cast<const std::int16_t*>(p)
                       : *reinterpret_cast<const std::int32_t*>(p);
}

void write_imm(std::uintptr_t p, int size, int value)
{
    if (size == 1)
        *reinterpret_cast<std::int8_t*>(p) = static_cast<std::int8_t>(value);
    else if (size == 2)
        *reinterpret_cast<std::int16_t*>(p) = static_cast<std::int16_t>(value);
    else
        *reinterpret_cast<std::int32_t*>(p) = value;
}

std::int32_t call_rel(std::uintptr_t call) { return *reinterpret_cast<const std::int32_t*>(call + 1); }

void find_code()
{
    g_searched = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t draw = game != nullptr ? scan::find(game, kDraw) : 0;
    if (draw == 0) {
        log::write(L"Profile: the panel's drawing wasn't found in FFXiMain.dll: profile.clean lays out its labels only");
        return;
    }
    for (std::size_t i = 0; i < std::size(kSites); ++i) {
        const Site& site = kSites[i];
        const std::uintptr_t found = scan::find(draw, kDrawSize, site.pattern);
        if (found == 0) {
            log::write(L"Profile: a value's place (%zu) wasn't found in the panel's drawing: profile.clean lays out its labels only", i);
            return;
        }
        g_site[i] = found;
        g_game_x[i] = read_imm(found + site.x_at, site.x_size);
        g_game_y[i] = read_imm(found + site.y_at, site.y_size);
    }
    const std::uintptr_t home_call = g_site[kHomeSite] + kCallAt, rank_call = g_site[kRankSite] + kCallAt;
    g_left_draw = home_call + 5 + static_cast<std::uintptr_t>(call_rel(home_call));
    g_rank_rel = call_rel(rank_call);
    g_draw = draw;
}

// The panel's drawing of its values as profile.clean says (its places and the rank's draw), or as the game had it
void patch(bool clean)
{
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(g_draw), kDrawSize, PAGE_EXECUTE_READWRITE, &protect))
        return;
    for (std::size_t i = 0; i < std::size(kSites); ++i) {
        const Site& site = kSites[i];
        write_imm(g_site[i] + site.x_at, site.x_size, clean ? site.x : g_game_x[i]);
        write_imm(g_site[i] + site.y_at, site.y_size, clean ? site.y : g_game_y[i]);
    }
    const std::uintptr_t rank_call = g_site[kRankSite] + kCallAt;
    const auto rel = clean ? static_cast<std::int32_t>(g_left_draw - (rank_call + 5)) : g_rank_rel;
    std::memcpy(reinterpret_cast<void*>(rank_call + 1), &rel, 4);
    VirtualProtect(reinterpret_cast<void*>(g_draw), kDrawSize, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(g_draw), kDrawSize);
    g_patched = clean;
    log::write(clean ? L"Profile: its values laid out in one column ([client] profile.clean=1)"
                     : L"Profile: its values where the game lays them");
}

void sync_code()
{
    if (g_clean == g_patched)
        return;
    if (!g_searched)
        find_code();
    if (g_draw != 0)
        patch(g_clean);
}

// The craft panel's push of "(%d)", checked to be that before it's taken
void find_craft()
{
    g_craft_searched = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t at = game != nullptr ? scan::find(game, kCraftFormat) : 0;
    __try {
        const std::uint32_t format = at != 0 ? *reinterpret_cast<const std::uint32_t*>(at + kCraftFormatAt) : 0;
        if (format != 0 && std::memcmp(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(format)), kGameFormat, sizeof kGameFormat) == 0) {
            g_craft_push = at + kCraftFormatAt;
            g_craft_game = format;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (g_craft_push == 0)
        log::write(L"Profile: Craft Skills' \"(%%d)\" wasn't found in FFXiMain.dll: craft.brackets does nothing");
}

// The craft panel's levels printed as craft.brackets says, "(60)" or "60"
void sync_brackets()
{
    if (g_bare == !g_brackets)
        return;
    if (!g_craft_searched)
        find_craft();
    DWORD protect = 0;
    if (g_craft_push == 0 || !VirtualProtect(reinterpret_cast<void*>(g_craft_push), 4, PAGE_EXECUTE_READWRITE, &protect))
        return;
    const std::uint32_t format = g_brackets ? g_craft_game : static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(kBareFormat));
    std::memcpy(reinterpret_cast<void*>(g_craft_push), &format, 4);
    VirtualProtect(reinterpret_cast<void*>(g_craft_push), 4, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(g_craft_push), 4);
    g_bare = !g_brackets;
    log::write(g_bare ? L"Profile: Craft Skills' levels without brackets ([client] craft.brackets=0)"
                      : L"Profile: Craft Skills' levels in brackets, as the game has them");
}

bool named(const char* quad, const char (&name)[17]) { return std::memcmp(quad + kQuadName, name, 16) == 0; }

// Each quad of the window's pictures, `fn(quad)`
template <typename Fn> void each_quad(char* window, Fn fn)
{
    int pieces = 0;
    for (char* node = at<char*>(window, kWinPieceList); node != nullptr && pieces < kListAtMost; node = at<char*>(node, 0), ++pieces) {
        char* piece = at<char*>(node, kNodePiece);
        char* shape = piece != nullptr && at<std::uint8_t>(node, kNodeOut) == 0 && at<short>(piece, 0) == kPieceFrame
                          ? at<char*>(piece, kPiecePicture)
                          : nullptr;
        if (shape == nullptr)
            continue;
        const int quads = at<int>(shape, kShapeCount);
        int seen = 0;
        for (char* part = at<char*>(shape, kShapeQuads); part != nullptr && seen < quads && seen < kListAtMost;
             part = at<char*>(part, 0), ++seen)
            if (char* quad = at<char*>(part, kNodePiece); quad != nullptr)
                fn(quad);
    }
}

// The title bars drawn as profile.title.* say, each kept in g_bars to be put back
void recolour(char* quad)
{
    if (g_bar_count >= static_cast<int>(std::size(g_bars)))
        return;
    Bar& bar = g_bars[g_bar_count++];
    bar.quad = quad;
    std::memcpy(bar.blend, quad + kBlend, sizeof bar.blend);
    for (int c = 0; c < 4; ++c) {
        std::uint32_t& colour = at<std::uint32_t>(quad, kCorners + c * kCorner + kColour);
        bar.colour[c] = colour;
        const std::uint32_t alpha = g_alpha >= 0 ? static_cast<std::uint32_t>(g_alpha) : colour >> 24;
        colour = alpha << 24 | (g_coloured ? g_colour : colour & 0xFFFFFF);
    }
    if (g_coloured)
        std::memcpy(quad + kBlend, kBlendOver, sizeof kBlendOver);
}

// A label's letter as the game lays it
struct Letter {
    char* quad;
    short left, right, top;
};

int row_of(short top)
{
    for (int r = 0; r < static_cast<int>(std::size(kWords)); ++r)
        if (top >= kWords[r].row && top <= kWords[r].row + kRowSlack)
            return kWords[r].row;
    for (short title : kTitleRows)
        if (top >= title && top <= title + kRowSlack)
            return -title;
    return 0;
}

void move(const Letter& letter, int dx, int dy)
{
    if (g_moved_count >= static_cast<int>(std::size(g_moved)))
        return;
    Moved& moved = g_moved[g_moved_count++];
    moved.quad = letter.quad;
    short* x = &at<short>(letter.quad, kQuadX);
    short* y = &at<short>(letter.quad, kQuadY);
    std::memcpy(moved.x, x, sizeof moved.x);
    std::memcpy(moved.y, y, sizeof moved.y);
    for (int c = 0; c < 4; ++c) {
        x[c] = static_cast<short>(x[c] + dx);
        y[c] = static_cast<short>(y[c] + dy);
    }
}

// One row's letters (in x order) put where kWords says, its words split by the gaps between them; false if it doesn't
// have the words kWords gives it (another layout: nothing moved)
bool lay_row(short row, const Letter* letters, int count, bool apply)
{
    int word = 0;
    while (word < static_cast<int>(std::size(kWords)) && kWords[word].row != row)
        ++word;
    for (int i = 0; i < count; ++word) {
        if (word >= static_cast<int>(std::size(kWords)) || kWords[word].row != row)
            return false;
        int end = i + 1;
        while (end < count && letters[end].left - letters[end - 1].right <= kWordGap)
            ++end;
        const int dx = kLabelLeft - letters[i].left, dy = kWords[word].to_y - row;
        for (int j = i; apply && j < end; ++j) {
            const bool colon = kWords[word].colon && j == end - 1 && j > i && letters[j].right - letters[j].left <= kColonWidest;
            move(letters[j], colon ? dx + letters[j - 1].right + 1 - letters[j].left : dx, dy);
        }
        i = end;
    }
    return word == static_cast<int>(std::size(kWords)) || kWords[word].row != row;
}

// profile.clean's labels: every letter of the window's pictures in a known row, then each row laid out (checked first,
// then moved, so a layout it doesn't know is left whole)
void lay_out(char* window)
{
    Letter letters[128];
    int count = 0;
    bool known = true;
    each_quad(window, [&](char* quad) {
        if (!named(quad, kLetterSheet) || count >= static_cast<int>(std::size(letters)))
            return;
        const Letter letter{quad, at<short>(quad, kQuadX), at<short>(quad, kQuadX + 2), at<short>(quad, kQuadY)};
        const int row = row_of(letter.top);
        known = known && row != 0;
        if (row > 0)
            letters[count++] = letter;
    });
    std::sort(letters, letters + count, [](const Letter& a, const Letter& b) {
        const int ra = row_of(a.top), rb = row_of(b.top);
        return ra != rb ? ra < rb : a.left < b.left;
    });
    for (int pass = 0; pass < 2 && known; ++pass)
        for (int i = 0; i < count && known;) {
            const short row = static_cast<short>(row_of(letters[i].top));
            int end = i;
            while (end < count && row_of(letters[end].top) == row)
                ++end;
            known = lay_row(row, letters + i, end - i, pass == 1);
            i = end;
        }
    if (!known && !g_odd_logged) {
        g_odd_logged = true;
        log::write(L"Profile: its labels aren't as profile.clean knows them (another menu DAT?): left where the game lays them");
    }
}

// The title rows' letters noted for TitleQuad (left alone by profile.clean: they're found by the rows the game lays them in)
void note_titles(char* window)
{
    g_title_count = 0;
    each_quad(window, [](char* quad) {
        if (g_title_count < static_cast<int>(std::size(g_titles)) && named(quad, kLetterSheet) && row_of(at<short>(quad, kQuadY)) < 0)
            g_titles[g_title_count++] = quad;
    });
}

// A kTitled window's title ("Job Levels", "Combat Skills"...) noted for TitleQuad: the letters on its title bar, the gauge
// at its top (Job Levels' 0..16 high, read live 2026-11-08; its rows' letters from 19 down), drawn in the Profile panel's
// titles' True Font as asked the same day
void note_bar_titles(char* window)
{
    g_title_count = 0;
    short bottom = -1;
    each_quad(window, [&bottom](char* quad) {
        if (named(quad, kBarName) && at<short>(quad, kQuadY) <= kTitleTop)
            bottom = std::max(bottom, at<short>(quad, kQuadY + 4)); // (its bottom left corner's y)
    });
    if (bottom < 0)
        return;
    each_quad(window, [bottom](char* quad) {
        if (g_title_count < static_cast<int>(std::size(g_titles)) && named(quad, kLetterSheet) && at<short>(quad, kQuadY) < bottom)
            g_titles[g_title_count++] = quad;
    });
}

// The window this is: the Profile panel, one of kTitled (its title bar alone) or neither
enum class Kind { None, Profile, Titled };

Kind kind_of(const void* ctrl)
{
    const char* layout = *reinterpret_cast<const char* const*>(static_cast<const char*>(ctrl) + kWindowLayout);
    if (layout == nullptr)
        return Kind::None;
    if (std::memcmp(layout + kLayoutName, kProfileName, 16) == 0)
        return Kind::Profile;
    for (const char* name : kTitled)
        if (std::memcmp(layout + kLayoutName, name, 16) == 0)
            return Kind::Titled;
    return Kind::None;
}

bool change(const void* ctrl)
{
    __try {
        const Kind kind = kind_of(ctrl);
        if (kind == Kind::None)
            return false;
        if (kind == Kind::Profile)
            sync_code();
        char* window = static_cast<char*>(const_cast<void*>(ctrl));
        if (g_coloured || g_alpha >= 0)
            each_quad(window, [kind](char* quad) {
                if (named(quad, kBarName) && (kind == Kind::Profile || at<short>(quad, kQuadY) <= kTitleTop))
                    recolour(quad);
            });
        if (kind == Kind::Profile && g_clean)
            lay_out(window);
        if (chatfont::area_on(VANA_FONT_PROFILE_TITLE)) {
            if (kind == Kind::Profile)
                note_titles(window);
            else
                note_bar_titles(window);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return g_moved_count > 0 || g_bar_count > 0 || g_title_count > 0;
}

void put_back()
{
    __try {
        for (int i = g_moved_count - 1; i >= 0; --i) {
            std::memcpy(g_moved[i].quad + kQuadX, g_moved[i].x, sizeof g_moved[i].x);
            std::memcpy(g_moved[i].quad + kQuadY, g_moved[i].y, sizeof g_moved[i].y);
        }
        for (int i = g_bar_count - 1; i >= 0; --i) {
            std::memcpy(g_bars[i].quad + kBlend, g_bars[i].blend, sizeof g_bars[i].blend);
            for (int c = 0; c < 4; ++c)
                at<std::uint32_t>(g_bars[i].quad, kCorners + c * kCorner + kColour) = g_bars[i].colour[c];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_moved_count = 0;
    g_bar_count = 0;
    g_title_count = 0;
}

std::uint32_t channel(float c) { return static_cast<std::uint32_t>(std::clamp(c, 0.0f, 255.0f) * 128.0f / 255.0f + 0.5f); }

} // namespace

void reload(const wchar_t* ini)
{
    g_clean = GetPrivateProfileIntW(L"client", L"profile.clean", 0, ini) != 0;
    g_brackets = GetPrivateProfileIntW(L"client", L"craft.brackets", 1, ini) != 0;
    wchar_t text[64] = {};
    GetPrivateProfileStringW(L"client", L"profile.title.color", L"", text, 64, ini);
    float colour[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    g_coloured = text[0] != L'\0' && text::parse_colour(text, colour);
    g_colour = channel(colour[0]) << 16 | channel(colour[1]) << 8 | channel(colour[2]);
    GetPrivateProfileStringW(L"client", L"profile.title.opacity", L"", text, 64, ini);
    // (unset: the game's for the game's colour, solid for one picked, so the bar is the colour picked)
    const int opacity = text[0] != L'\0' ? std::clamp(static_cast<int>(std::wcstol(text, nullptr, 10)), 0, 100)
                        : g_coloured     ? 100
                                         : -1;
    g_alpha = opacity < 0 ? -1 : (opacity * 0x80 + 50) / 100;
}

void install(const wchar_t* ini)
{
    reload(ini);
}

Drawing::Drawing(const void* ctrl)
{
    sync_brackets(); // (on the game's thread, before any window's draw: the craft panel's included)
    const bool wanted = g_clean || g_patched || g_coloured || g_alpha >= 0 || wants_quads();
    if (ctrl == nullptr || !wanted || g_moved_count > 0 || g_bar_count > 0 || g_title_count > 0)
        return;
    changed_ = change(ctrl);
}

Drawing::~Drawing()
{
    if (changed_)
        put_back();
}

TitleQuad::TitleQuad(const char* quad)
{
    for (int i = 0; i < g_title_count; ++i)
        if (g_titles[i] == quad) {
            was_ = vanacore_gfx_text_area(VANA_FONT_PROFILE_TITLE);
            return;
        }
}

TitleQuad::~TitleQuad()
{
    if (was_ >= 0)
        vanacore_gfx_text_area(was_);
}

bool wants_quads() { return chatfont::area_on(VANA_FONT_PROFILE_TITLE); }

} // namespace vanacore::profilewin
