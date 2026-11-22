#include "itemslot.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "asm_text.hpp"
#include "buttons.hpp"
#include "d3d8.hpp"
#include "gamewin.hpp"
#include "imports.hpp"
#include "jplabels.hpp"
#include "labeltext.hpp"
#include "listtext.hpp"
#include "log.hpp"
#include "profilewin.hpp"
#include "scan.hpp"
#include "sliders.hpp"
#include "text.hpp"

namespace vanacore::itemslot {
namespace {

// The game's drawing of one quad of a menu picture (FUN_1011e580, __thiscall on the quad, ret 18h: x, y, scale x, scale y,
// colour, flags), which every window's frames, slots and buttons go through. The quad: the name of its texture at +0x32
// ("menu    itemslot"), the game's texture (loaded by that name the first time it's drawn) at +0x4C, then its four
// corners (x, y, z, rhw, colour, u, v) from +0x50, 0x1C apart. The game keeps each corner's colour and u, v as the menu
// resource gave them, works out the drawn ones from them and puts them back after.
// sub esp,44h; push esi; push edi; mov edi,ecx; mov eax,[edi+4Ch]; test eax,eax; jnz; lea esi,[edi+32h]; push esi; call
constexpr char kDrawQuad[] = "83 EC 44 56 57 8B F9 8B 47 4C 85 C0 75 ?? 8D 77 32 56 E8";
constexpr std::size_t kPrologue = 5; // sub esp,44h; push esi; push edi
constexpr std::ptrdiff_t kName = 0x32;
constexpr std::ptrdiff_t kSize = 0x08; // the picture's width and height, shorts (the corners are laid from them as it's drawn)
constexpr std::ptrdiff_t kTexture = 0x4C;
constexpr std::ptrdiff_t kCorners = 0x50;
constexpr std::ptrdiff_t kCorner = 0x1C;
constexpr std::ptrdiff_t kColour = 0x10; // a corner's colour: B, G, R, A
constexpr std::ptrdiff_t kAlpha = 0x13;  // its alpha
// The game's texture: its Direct3D texture (the quad's drawing takes +0x44 when +0x40 is null)
constexpr std::ptrdiff_t kD3d = 0x40;
constexpr std::ptrdiff_t kD3dElse = 0x44;
constexpr char kSlotName[16] = {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 'i', 't', 'e', 'm', 's', 'l', 'o', 't'};
constexpr DWORD kFormatDxt1 = 0x31545844; // 'DXT1': the slot is 32x32
constexpr DWORD kLockReadOnly = 0x10;     // D3DLOCK_READONLY
constexpr int kOpaque = 0x80;             // the game's alpha for opaque (doubled as it's drawn)
constexpr int kDetailsAlpha = 0x61;       // the item details window's square's alpha
// The quad's blending (its last argument, else its own at +0x2D..+0x30), as the PS2's (A - B) * alpha + D, each 0 the
// quad's colour, 1 what's under it, 2 none: A | B << 2 | D << 6. Squares come additive (0, 2, 1: most windows),
// subtracted (2, 0, 1) or blended (0, 1, 1: the item details window).
constexpr int kBlendOver = 0 | 1 << 2 | 1 << 6;
// Hidden squares leave the windows where items are put into empty slots (a trade, a delivery, a synthesis' crystal and
// ingredients: "menu    tskill1 ", and "tskill2" laid out under it, seen live 2026-10-23; the Mog House's delivery box as it's
// received, "menu    post1   ": its 8 slots, read live 2026-11-06, where "delivery" is the box being sent) with nothing to aim
// at: those draw a white outline of the square instead. The window being drawn (chat.cpp's draw of each window's control:
// its layout at +4, named at +0x46 as helpbar.cpp's) says which.
constexpr std::ptrdiff_t kWindowLayout = 4;
constexpr std::ptrdiff_t kLayoutName = 0x46;
constexpr char kOutlined[][16] = {
    {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 't', 'r', 'a', 'd', 'e', ' ', ' ', ' '},
    {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 'd', 'e', 'l', 'i', 'v', 'e', 'r', 'y'},
    {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 'p', 'o', 's', 't', '1', ' ', ' ', ' '},
    {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 't', 's', 'k', 'i', 'l', 'l', '1', ' '},
    {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 't', 's', 'k', 'i', 'l', 'l', '2', ' '},
};
constexpr int kOutlineTexels = 2;
// The same picture is the box of a text field (the recipient's name, a macro's lines and name), which hidden squares must
// not take away: windows whose squares are all drawn as the game has them, whatever items.slot says. The macro editor's
// six lines had no box to see with the squares hidden or restyled (seen 2026-10-22): its windows for the Ctrl and Alt
// palettes, short and long, a book's name, and the form a macro's lines are shown in before one is edited ("menu    macro
// ", area 0x89, seen live 2026-10-23)
constexpr char kFields[][16] = {
    {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 'm', 'a', 'c', 'r', 'o', ' ', ' ', ' '},
    {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 's', 't', 'r', 'i', 'n', 'g', 'd', 'l'},
    {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 'm', 'c', 'r', '1', 'e', 'd', 'i', 't'},
    {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 'm', 'c', 'r', '2', 'e', 'd', 'i', 't'},
    {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 'm', 'c', 'r', '1', 'e', 'd', 'l', 'o'},
    {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 'm', 'c', 'r', '2', 'e', 'd', 'l', 'o'},
    {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 'm', 'c', 'r', 'b', 'e', 'd', 'i', 't'},
};
// The Equipment window, whose squares items.equip draws with VanaCore's own picture (a background and a border) in
// place of the game's, whatever items.slot does to the rest; and the Check window (/check: another's gear, "menu
// inspect ", CTkEquip's subclass, vtable 0x103317E0, its draw 0x10133A30 the icons alone), whose squares go by the same
// rule (asked for 2026-11-06: with items.slot=hide they were gone there and the slots' names showed under the icons)
constexpr char kEquip[16] = {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 'e', 'q', 'u', 'i', 'p', ' ', ' ', ' '};
constexpr char kInspect[16] = {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 'i', 'n', 's', 'p', 'e', 'c', 't', ' '};
// And the Equipment Set window (Macros > Edit Equip Set, a set's Equipment: "menu    mcresed ", read live 2026-11-06),
// whose 16 slot buttons are the Check window's kind: an itemslot square and the slot's name in kLetters (asked for
// 2026-11-06: with items.slot=hide its squares were gone and only the names showed)
constexpr char kEquipSet[16] = {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 'm', 'c', 'r', 'e', 's', 'e', 'd', ' '};
// The Check window's slot buttons are its layout's "menu    framesus" picture -1: the square and the slot's name ("Main",
// "Range", ...) in "menu    menu2fon" letters at its bottom, drawn whether or not an item is there (the Equipment window's
// are picture 245, the square alone; read live 2026-11-06). The window draw (FUN_101173d0) draws its pieces, then its
// owner's draw (0x10117904), the icons over them, so with items.equip on a name isn't drawn where the last frame drew an
// item's icon (itemrarity.cpp's icon hook hands its places in): a slot with an item shows the item, as in the Equipment
// window, and an empty one its name
constexpr char kLetters[16] = {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 'm', 'e', 'n', 'u', '2', 'f', 'o', 'n'};
constexpr int kIconsAtMost = 32, kIconSide = 32, kIconSlack = 2;
struct IconPlaces {
    int count = 0;
    short x[kIconsAtMost] = {}, y[kIconsAtMost] = {};
};
// The Equipment window's draw (FUN_10132aa0) puts a slot's wardrobe number (a box with the digit, one menu picture the
// container picks, FUN_101328f0) over the top right of its square by drawing that picture (FUN_1011f760, __thiscall on
// the picture, ret 14h: x, y, colour, flags, first part), then the digit in it (FUN_101329a0, __thiscall on the window,
// ret 10h: slot, container, x, y; nothing for a container that isn't a wardrobe); items.equip.wardrobe=0 leaves both
// calls out.
// push ebx; call pick; add esp,4; test eax,eax; jz; movsx ecx,[esp+18h]; movsx edx,[esp+1Ah]; push 0; push 0;
// push 80808080h; add ecx,10h; push edx; push ecx; mov ecx,eax; call draw
constexpr char kBadgeDraw[] =
    "53 E8 ?? ?? ?? ?? 83 C4 04 85 C0 74 ?? 0F BF 4C 24 18 0F BF 54 24 1A 6A 00 6A 00 68 80 80 80 80 83 C1 10 52 51 8B C8 E8";
constexpr std::size_t kBadgeCall = 0x27; // the box's call in it
// after it: mov edx,[esp+1Ah]; mov eax,[esp+18h]; push edx; push eax; lea ecx,[ebp-1]; push ebx; push ecx; mov ecx,esi;
// call digit
constexpr std::size_t kDigitCall = 0x3D;
constexpr std::uint8_t kNeutral = 0x80;  // a corner's colour that leaves the texture's as it is
constexpr int kGetDevice = 3;           // IDirect3DResource8::GetDevice (AddRefs what it returns)
// [client] <id>.solid=1 (off by default; hudwin.cpp says which window, through frameless): a window drawn without its
// frame (<id>.background=0) has its buttons' bars drawn solid. The bars ("menu    buttonto", two quads a button; the Action
// Menu's, seen live 2026-10-23) are added onto what's under them (their blend 0, 2, 1), and the dark frame was what made
// them read: over the scene they wash out. Their picture has its own alpha (DXT1: the black round the rounded ends is
// clear), so a bar is drawn blended over (kBlendOver) instead, from a copy of its picture whose dim outline fades out
// (its alpha ramped by brightness from kSolidFrom to kSolidTo, times the picture's own): a dark backing drawn behind the
// bar showed that outline as black corners (2026-10-23). The copy is drawn kSolidLift brighter, what the frame's fill
// added to the bar (matched live: the bar's body (57, 77, 122) on its own, (75, 101, 160) on the frame), with its
// corners' alpha full so the copy's alpha is the bar's. Only the game's plain draws (flags 0) change: the selection's
// blur layers carry their own blending. The labels (font quads) and the arrows (keytop) stay as they are, over the bar.
constexpr char kButtonName[] = "menu    button"; // the bar pictures' names start so
constexpr int kSolidFrom = 40, kSolidTo = 80;     // brightness the copy's alpha ramps over (the bar's outline is 45-52)
constexpr float kSolidLift = 1.31f;
// [client] buttons.color, buttons.selected.color (empty, the default: the game's): every menu's button bars in the
// colours picked, framed or not. The game tints the bar picture by its quads' corners (B 0x80, G and R 0x61: its blue)
// times the colour it gives the draw (kGamePlain; the selected button kGameSelected, its gold; selected and greyed out
// kGameSelectedOff), so a recoloured bar is drawn from a grey copy of its picture (its brightness, scaled so the middle
// level of its texels is kGreyMid) with its corners the colour picked and the draw's colour neutral (kGameHalf for the
// greyed one): a corner times the grey, doubled as it's drawn, comes out as the colour picked at the bar's middle. On
// the frame the bar is added onto the frame's fill, so that's taken off the colour picked there: a colour picked is the
// bar as it looks on the frame, and drawn solid (<id>.solid) it's that colour as it is. /vana's game colours are the
// bars measured live on the frame: #4B65A0, and #4B4845 for the selected one (its label is what's gold).
constexpr int kGreyMid = 0x80;
constexpr int kFrameFill[3] = {0x26, 0x18, 0x12}; // B, G, R: the frame's fill under its bars (#121826)
constexpr std::uint32_t kGamePlain = 0x808080, kGameSelected = 0x805020, kGameSelectedOff = 0x402810, kGameHalf = 0x404040;
constexpr int kRamp = 1, kGrey = 2; // a copy's kinds (make_copy)

using DrawQuad = void(__thiscall*)(void* quad, int x, int y, float sx, float sy, std::uint32_t colour, int flags);

enum class Style { Game, Tint, Solid, Hide };

// [client] items.slot*
Style g_style = Style::Game;
float g_colour[3] = {1.0f, 1.0f, 1.0f}; // .color, 0-1: the tint, or the solid colour
float g_brightness = 1.0f;              // .brightness, percent / 100
float g_contrast = 1.0f;                // .contrast, percent / 100
float g_opacity = 1.0f;                 // .opacity, percent / 100 of the game's

// [client] items.equip*: the Equipment window's squares
bool g_equip = false;
std::uint32_t g_equip_fill = 0x80000000;   // .background and .background.opacity, ARGB
std::uint32_t g_equip_border = 0xFFFFFFFF; // .border and .border.opacity, ARGB
int g_equip_size = 1;                      // .border.size, texels of the 32 across
bool g_wardrobe = true;                    // .wardrobe: the slots' wardrobe numbers drawn

using DrawShape = void(__thiscall*)(void* shape, int x, int y, std::uint32_t colour, int flags, int first);
using DrawDigit = void(__thiscall*)(void* window, int slot, int container, int x, int y);
DrawShape g_draw_badge = nullptr; // the game's, called from the redirected calls
DrawDigit g_draw_digit = nullptr;
bool g_badge_tried = false;

// The picture equipment squares are drawn with (A8R8G8B8, the game's square's size), on the game's device
void* g_equip_texture = nullptr;
void* g_equip_device = nullptr;
bool g_equip_dirty = true;
// The widest square the Equipment window drew last frame and this one so far: its slots (the count badges are half that)
float g_equip_widest = 0, g_equip_widest_now = 0;
unsigned g_equip_frame = ~0u;
// The Check and Equipment Set windows' icons' places: the last frame's and this one's so far (kLetters)
IconPlaces g_icons_last, g_icons_now;
unsigned g_icons_frame = ~0u;
bool g_names_logged = false;

bool g_tried = false;
DrawQuad g_draw = nullptr; // the game's, through the trampoline
const char* g_window = nullptr; // the window whose control the game is drawing now

// The window being drawn has its buttons drawn solid (hudwin.cpp, around its draw); once one has, the drawing's hook is
// to be put in (on_frame, so the hook goes in the frame after the first)
bool g_frameless = false;
bool g_solid_wanted = false;
// (buttons.color and buttons.selected.color, or the main menu's: buttons.cpp's bar_colour, by the window being drawn)
// The copies of the bar pictures, one per picture and kind (by its texture, which the game keeps; its size and format
// checked each time, so one made again at the same address for another picture isn't taken for it), on the game's
// device: a device that goes (a reset to another) takes them with it
struct Made {
    void* game;
    int kind;
    void* copy;
    UINT width, height;
    DWORD format;
};
std::vector<Made> g_copies;
constexpr std::size_t kCopiesAtMost = 32;
void* g_copy_device = nullptr;

// The slots' texture: its blocks as the game made them, and what VanaCore wrote over them (empty: nothing)
void* g_d3d = nullptr;
std::vector<std::uint8_t> g_original, g_written;
bool g_dirty = true; // the settings changed since the texture was written
unsigned g_frame = 0, g_checked = ~0u;

struct Rgb {
    float r, g, b;
};

Rgb decode(std::uint16_t c)
{
    return {((c >> 11) & 31) / 31.0f, ((c >> 5) & 63) / 63.0f, (c & 31) / 31.0f};
}

std::uint16_t encode(Rgb c)
{
    const auto q = [](float v, int top) { return static_cast<unsigned>(std::lround(std::clamp(v, 0.0f, 1.0f) * top)); };
    return static_cast<std::uint16_t>(q(c.r, 31) << 11 | q(c.g, 63) << 5 | q(c.b, 31));
}

Rgb mix(Rgb a, Rgb b, float t)
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

// A DXT1 block: two RGB565 colours, then 16 two-bit indices. c0 > c1: four colours (c0, c1, then 1/3 and 2/3 of the
// way); otherwise three (c0, c1, halfway) and 3 is clear.
struct Block {
    std::uint16_t c0, c1;
    std::uint32_t bits;
};

Block read_block(const std::uint8_t* at)
{
    Block block{};
    std::memcpy(&block.c0, at, 2);
    std::memcpy(&block.c1, at + 2, 2);
    std::memcpy(&block.bits, at + 4, 4);
    return block;
}

// The mean level of the texture's drawn texels: what the contrast spreads from
float mean_level(const std::vector<std::uint8_t>& blocks)
{
    double sum = 0;
    int count = 0;
    for (std::size_t at = 0; at + 8 <= blocks.size(); at += 8) {
        const Block block = read_block(blocks.data() + at);
        const bool four = block.c0 > block.c1;
        const Rgb a = decode(block.c0), b = decode(block.c1);
        const Rgb colours[4] = {a, b, four ? mix(a, b, 1.0f / 3) : mix(a, b, 0.5f), mix(a, b, 2.0f / 3)};
        for (int i = 0; i < 16; ++i) {
            const unsigned index = block.bits >> (i * 2) & 3;
            if (!four && index == 3)
                continue;
            const Rgb& c = colours[index];
            sum += (c.r + c.g + c.b) / 3;
            ++count;
        }
    }
    return count > 0 ? static_cast<float>(sum / count) : 0.5f;
}

// The game's blocks recoloured: each block's two colours changed (the others are mixes of them), the block kept in its
// mode (four colours, or three and clear) by swapping them back into order if the change crossed them
std::vector<std::uint8_t> recolour(const std::vector<std::uint8_t>& original)
{
    std::vector<std::uint8_t> blocks = original;
    const float mean = mean_level(original);
    const float scale[3] = {g_colour[0] * g_brightness, g_colour[1] * g_brightness, g_colour[2] * g_brightness};
    const auto change = [&](std::uint16_t colour) {
        if (g_style == Style::Solid)
            return encode({scale[0], scale[1], scale[2]});
        Rgb c = decode(colour);
        c.r = ((c.r - mean) * g_contrast + mean) * scale[0];
        c.g = ((c.g - mean) * g_contrast + mean) * scale[1];
        c.b = ((c.b - mean) * g_contrast + mean) * scale[2];
        return encode(c);
    };
    for (std::size_t at = 0; at + 8 <= blocks.size(); at += 8) {
        Block block = read_block(blocks.data() + at);
        const bool four = block.c0 > block.c1;
        std::uint16_t c0 = change(block.c0), c1 = change(block.c1);
        if (four && c0 < c1) {
            std::swap(c0, c1);
            block.bits ^= 0x55555555; // 0 <-> 1, 2 <-> 3
        } else if (four && c0 == c1) {
            block.bits = 0; // one colour, which equal colours read as three colours and clear: no 3s
        } else if (!four && c0 > c1) {
            std::swap(c0, c1);
            block.bits ^= ~(block.bits >> 1) & 0x55555555; // 0 <-> 1; halfway and clear stay
        }
        std::memcpy(blocks.data() + at, &c0, 2);
        std::memcpy(blocks.data() + at + 2, &c1, 2);
        std::memcpy(blocks.data() + at + 4, &block.bits, 4);
    }
    return blocks;
}

// The slot as a white outline: kOutlineTexels deep round the edge, the rest clear (three-colour blocks, c0 <= c1, index 3
// clear). The texture is square, `original`'s blocks 8 bytes each in rows.
std::vector<std::uint8_t> outline(const std::vector<std::uint8_t>& original)
{
    const std::size_t count = original.size() / 8;
    std::size_t side = 0;
    while ((side + 1) * (side + 1) <= count)
        ++side;
    std::vector<std::uint8_t> blocks = original;
    const int texels = static_cast<int>(side) * 4;
    const std::uint16_t c0 = 0xFFFE, c1 = 0xFFFF;
    for (std::size_t by = 0; by < side; ++by) {
        for (std::size_t bx = 0; bx < side; ++bx) {
            std::uint32_t bits = 0;
            for (int i = 0; i < 16; ++i) {
                const int x = static_cast<int>(bx) * 4 + i % 4, y = static_cast<int>(by) * 4 + i / 4;
                const bool edge = x < kOutlineTexels || y < kOutlineTexels || x >= texels - kOutlineTexels ||
                                  y >= texels - kOutlineTexels;
                bits |= static_cast<std::uint32_t>(edge ? 0 : 3) << (i * 2);
            }
            std::uint8_t* at = blocks.data() + (by * side + bx) * 8;
            std::memcpy(at, &c0, 2);
            std::memcpy(at + 2, &c1, 2);
            std::memcpy(at + 4, &bits, 4);
        }
    }
    return blocks;
}

enum class Window { Other, Outlined, Field, Equip };

// Which window the square is drawn in: the Equipment window, one whose squares hidden ones keep as an outline (trade,
// delivery) or as the game has them (the recipient's field), or another
Window window_kind()
{
    if (g_window == nullptr)
        return Window::Other;
    __try {
        const char* layout = *reinterpret_cast<const char* const*>(g_window + kWindowLayout);
        if (layout == nullptr)
            return Window::Other;
        if (std::memcmp(layout + kLayoutName, kEquip, sizeof kEquip) == 0 ||
            std::memcmp(layout + kLayoutName, kInspect, sizeof kInspect) == 0 ||
            std::memcmp(layout + kLayoutName, kEquipSet, sizeof kEquipSet) == 0)
            return Window::Equip;
        for (const auto& name : kOutlined) {
            if (std::memcmp(layout + kLayoutName, name, sizeof name) == 0)
                return Window::Outlined;
        }
        for (const auto& name : kFields) {
            if (std::memcmp(layout + kLayoutName, name, sizeof name) == 0)
                return Window::Field;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return Window::Other;
}

// The window being drawn has its slots' names under the squares: the Check window or the Equipment Set window
bool in_named_slots()
{
    if (g_window == nullptr)
        return false;
    __try {
        const char* layout = *reinterpret_cast<const char* const*>(g_window + kWindowLayout);
        return layout != nullptr && (std::memcmp(layout + kLayoutName, kInspect, sizeof kInspect) == 0 ||
                                     std::memcmp(layout + kLayoutName, kEquipSet, sizeof kEquipSet) == 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Those windows' icons: this frame's become the last frame's at the first use in a new frame
void turn_icons()
{
    if (g_icons_frame == g_frame)
        return;
    g_icons_frame = g_frame;
    g_icons_last = g_icons_now;
    g_icons_now.count = 0;
}

// A Check or Equipment Set window slot's name letter (kLetters) whose picture is at `x`, `y` (the slot's corner, as its
// square's) is under an item's icon: not drawn (items.equip). Said once in the log, with where the icons were, for a name
// that stays
bool name_under_icon(const char* quad, int x, int y)
{
    if (!g_equip || std::memcmp(quad + kName, kLetters, sizeof kLetters) != 0 || !in_named_slots())
        return false;
    turn_icons();
    for (int i = 0; i < g_icons_last.count; ++i) {
        if (x >= g_icons_last.x[i] - kIconSlack && x < g_icons_last.x[i] + kIconSide && y >= g_icons_last.y[i] - kIconSlack &&
            y < g_icons_last.y[i] + kIconSide)
            return true;
    }
    if (!g_names_logged && g_icons_last.count > 0) {
        g_names_logged = true;
        log::write(L"Item slot: a Check / Equipment Set window slot's name at %d,%d drawn (no item's icon there; the first of "
                   L"%d at %d,%d)",
                   x, y, g_icons_last.count, g_icons_last.x[0], g_icons_last.y[0]);
    }
    return false;
}

// The first level of a DXT1 texture: its blocks read into `blocks`, or written from them. False if it isn't one, or
// can't be locked.
bool texture_blocks(void* d3d, std::vector<std::uint8_t>& blocks, bool write)
{
    d3d8::SurfaceDesc desc{};
    if (FAILED(d3d8::call(d3d, d3d8::slot::TextureGetLevelDesc, 0u, &desc)) || desc.format != kFormatDxt1 ||
        desc.width < 4 || desc.height < 4)
        return false;
    const std::size_t row = desc.width / 4 * 8, rows = desc.height / 4;
    if (write && blocks.size() != row * rows)
        return false;
    d3d8::LockedRect locked{};
    if (FAILED(d3d8::call(d3d, d3d8::slot::TextureLockRect, 0u, &locked, static_cast<RECT*>(nullptr),
                          write ? DWORD{0} : kLockReadOnly)))
        return false;
    blocks.resize(row * rows);
    auto* bits = static_cast<std::uint8_t*>(locked.bits);
    for (std::size_t y = 0; y < rows; ++y) {
        if (write)
            std::memcpy(bits + y * locked.pitch, blocks.data() + y * row, row);
        else
            std::memcpy(blocks.data() + y * row, bits + y * locked.pitch, row);
    }
    d3d8::call(d3d, d3d8::slot::TextureUnlockRect, 0u);
    return true;
}

// The slots' texture as the settings say: recoloured (tint, solid) or the game's own. Once a frame it's read back, so
// a texture the game made again (or another at the same address) is taken as the game's.
void sync_texture(const char* quad)
{
    const char* texture = *reinterpret_cast<char* const*>(quad + kTexture);
    if (texture == nullptr)
        return; // not loaded yet: the game loads it as it draws
    void* d3d = *reinterpret_cast<void* const*>(texture + kD3d);
    if (d3d == nullptr)
        d3d = *reinterpret_cast<void* const*>(texture + kD3dElse);
    if (d3d == nullptr)
        return;
    if (d3d != g_d3d || g_checked != g_frame) {
        g_checked = g_frame;
        std::vector<std::uint8_t> now;
        if (!texture_blocks(d3d, now, false))
            return;
        if (d3d != g_d3d || now != (g_written.empty() ? g_original : g_written)) {
            g_d3d = d3d;
            g_original = std::move(now);
            g_written.clear();
            g_dirty = true;
        }
    }
    if (!g_dirty)
        return;
    g_dirty = false;
    const bool recoloured = g_style == Style::Tint || g_style == Style::Solid || g_style == Style::Hide;
    if (!recoloured && g_written.empty())
        return;
    std::vector<std::uint8_t> blocks =
        g_style == Style::Hide ? outline(g_original) : recoloured ? recolour(g_original) : g_original;
    if (!texture_blocks(g_d3d, blocks, true))
        return;
    if (recoloured)
        g_written = std::move(blocks);
    else
        g_written.clear();
}

// Where the quad's game texture keeps the Direct3D texture its drawing takes (+0x40, else +0x44): null if it has none yet
void** d3d_slot(char* quad)
{
    char* texture = *reinterpret_cast<char**>(quad + kTexture);
    if (texture == nullptr)
        return nullptr;
    auto** slot = reinterpret_cast<void**>(texture + kD3d);
    if (*slot == nullptr)
        slot = reinterpret_cast<void**>(texture + kD3dElse);
    return *slot != nullptr ? slot : nullptr;
}

// The equipment squares' picture, made on the device of `game` (the game's square's texture) at its size and drawn as
// the settings say: the background, and the border's texels round the edge. False if it can't be made.
bool equip_texture(void* game)
{
    void* device = nullptr;
    if (FAILED(d3d8::call(game, kGetDevice, &device)) || device == nullptr)
        return false;
    d3d8::call<ULONG>(device, d3d8::slot::Release);
    if (device != g_equip_device || g_equip_texture == nullptr) {
        // one made on another device went with it
        g_equip_texture = nullptr;
        d3d8::SurfaceDesc desc{};
        void* texture = nullptr;
        if (FAILED(d3d8::call(game, d3d8::slot::TextureGetLevelDesc, 0u, &desc)) || desc.width == 0 || desc.height == 0 ||
            FAILED(d3d8::call(device, d3d8::slot::CreateTexture, desc.width, desc.height, 1u, DWORD{0},
                              d3d8::kFormatA8R8G8B8, d3d8::kPoolManaged, &texture)) ||
            texture == nullptr)
            return false;
        g_equip_texture = texture;
        g_equip_device = device;
        g_equip_dirty = true;
    }
    if (!g_equip_dirty)
        return true;
    d3d8::SurfaceDesc desc{};
    d3d8::LockedRect locked{};
    if (FAILED(d3d8::call(g_equip_texture, d3d8::slot::TextureGetLevelDesc, 0u, &desc)) ||
        FAILED(d3d8::call(g_equip_texture, d3d8::slot::TextureLockRect, 0u, &locked, static_cast<RECT*>(nullptr), DWORD{0})))
        return false;
    const int width = static_cast<int>(desc.width), height = static_cast<int>(desc.height), edge = g_equip_size;
    for (int y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(locked.bits) + y * locked.pitch);
        for (int x = 0; x < width; ++x) {
            const bool border = x < edge || y < edge || x >= width - edge || y >= height - edge;
            row[x] = border ? g_equip_border : g_equip_fill;
        }
    }
    d3d8::call(g_equip_texture, d3d8::slot::TextureUnlockRect, 0u);
    g_equip_dirty = false;
    return true;
}

// An Equipment window square, drawn with the equipment picture: swapped into the game's texture for this one draw, the
// corners' colours made neutral and the quad blended over the window (as the item details window draws its own)
void draw_equip(char* quad, int x, int y, float sx, float sy, std::uint32_t colour, int flags)
{
    void** slot = d3d_slot(quad);
    if (slot == nullptr || !equip_texture(*slot)) {
        g_draw(quad, x, y, sx, sy, colour, flags); // the game loads its texture as it draws
        return;
    }
    void* game = *slot;
    std::uint8_t corner[4][4];
    for (int i = 0; i < 4; ++i) {
        std::uint8_t* at = reinterpret_cast<std::uint8_t*>(quad + kCorners + i * kCorner + kColour);
        std::memcpy(corner[i], at, 4);
        std::memset(at, kNeutral, 4);
    }
    *slot = g_equip_texture;
    g_draw(quad, x, y, sx, sy, colour, kBlendOver);
    *slot = game;
    for (int i = 0; i < 4; ++i)
        std::memcpy(quad + kCorners + i * kCorner + kColour, corner[i], 4);
}

// How wide the quad is drawn: across its corners, times its scale. A quad never drawn yet has its corners all at 0 (the
// game lays them from its size, kSize, as it draws it): its size then. With items.slot=hide the Equipment window's squares
// are drawn only by draw_equip, so a square the game hadn't drawn before measured 0 wide, was taken for a badge and hidden,
// and never drawn: after a restart only one of the 16 showed (read live 2026-11-06: their corners all 0, size 32 x 32)
float quad_width(const char* quad, float sx)
{
    float low = 0, high = 0;
    for (int i = 0; i < 4; ++i) {
        float x = 0;
        std::memcpy(&x, quad + kCorners + i * kCorner, sizeof x);
        low = i == 0 ? x : std::min(low, x);
        high = i == 0 ? x : std::max(high, x);
    }
    if (!(high > low)) {
        std::int16_t size = 0;
        std::memcpy(&size, quad + kSize, sizeof size);
        return static_cast<float>(size) * std::fabs(sx);
    }
    return (high - low) * std::fabs(sx);
}

// An Equipment window square that's one of its slots: as wide as the widest it draws, not the small badge in a slot's
// corner that a count is printed on (which is left to items.slot)
bool equip_slot(const char* quad, float sx)
{
    if (g_equip_frame != g_frame) {
        g_equip_frame = g_frame;
        g_equip_widest = g_equip_widest_now;
        g_equip_widest_now = 0;
    }
    const float width = quad_width(quad, sx);
    g_equip_widest_now = std::max(g_equip_widest_now, width);
    return width >= std::max(g_equip_widest, g_equip_widest_now) * 0.75f;
}

// A picture's texels as ARGB, from its first level: DXT1 (the menu pictures'; a clear texel 0), 32-bit or 16-bit;
// false for a format not read here, or one that can't be locked
bool texels(void* game, const d3d8::SurfaceDesc& desc, std::vector<std::uint32_t>& argb)
{
    const DWORD f = desc.format;
    const bool dxt = f == kFormatDxt1;
    const int bytes = f == d3d8::kFormatA8R8G8B8 || f == d3d8::kFormatX8R8G8B8 ? 4
                      : f == d3d8::kFormatR5G6B5 || f == d3d8::kFormatX1R5G5B5 || f == d3d8::kFormatA1R5G5B5 ||
                              f == d3d8::kFormatA4R4G4B4
                          ? 2
                          : 0;
    if ((!dxt && bytes == 0) || (dxt && (desc.width % 4 != 0 || desc.height % 4 != 0)))
        return false;
    d3d8::LockedRect locked{};
    if (FAILED(d3d8::call(game, d3d8::slot::TextureLockRect, 0u, &locked, static_cast<RECT*>(nullptr), kLockReadOnly)))
        return false;
    const auto* bits = static_cast<const std::uint8_t*>(locked.bits);
    argb.assign(static_cast<std::size_t>(desc.width) * desc.height, 0);
    const auto pack = [](float a, float r, float g, float b) {
        const auto byte = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
        return byte(a) << 24 | byte(r) << 16 | byte(g) << 8 | byte(b);
    };
    if (dxt) {
        for (UINT by = 0; by < desc.height / 4; ++by) {
            for (UINT bx = 0; bx < desc.width / 4; ++bx) {
                const Block block = read_block(bits + by * locked.pitch + bx * 8);
                const bool four = block.c0 > block.c1;
                const Rgb a = decode(block.c0), b = decode(block.c1);
                const Rgb colours[4] = {a, b, four ? mix(a, b, 1.0f / 3) : mix(a, b, 0.5f), mix(a, b, 2.0f / 3)};
                for (int i = 0; i < 16; ++i) {
                    const unsigned index = block.bits >> (i * 2) & 3;
                    const Rgb& c = colours[index];
                    argb[(by * 4 + i / 4) * desc.width + bx * 4 + i % 4] = !four && index == 3 ? 0 : pack(1, c.r, c.g, c.b);
                }
            }
        }
    } else {
        for (UINT y = 0; y < desc.height; ++y) {
            for (UINT x = 0; x < desc.width; ++x) {
                const std::uint8_t* at = bits + y * locked.pitch + x * bytes;
                std::uint32_t& out = argb[y * desc.width + x];
                if (bytes == 4) {
                    std::memcpy(&out, at, 4);
                    if (f == d3d8::kFormatX8R8G8B8)
                        out |= 0xFF000000;
                    continue;
                }
                std::uint16_t c = 0;
                std::memcpy(&c, at, 2);
                if (f == d3d8::kFormatR5G6B5)
                    out = pack(1, (c >> 11 & 31) / 31.0f, (c >> 5 & 63) / 63.0f, (c & 31) / 31.0f);
                else if (f == d3d8::kFormatA4R4G4B4)
                    out = pack((c >> 12 & 15) / 15.0f, (c >> 8 & 15) / 15.0f, (c >> 4 & 15) / 15.0f, (c & 15) / 15.0f);
                else
                    out = pack(f == d3d8::kFormatA1R5G5B5 ? static_cast<float>(c >> 15) : 1.0f, (c >> 10 & 31) / 31.0f,
                               (c >> 5 & 31) / 31.0f, (c & 31) / 31.0f);
            }
        }
    }
    d3d8::call(game, d3d8::slot::TextureUnlockRect, 0u);
    return true;
}

int level_of(std::uint32_t c)
{
    return static_cast<int>((c >> 16 & 0xFF) + (c >> 8 & 0xFF) + (c & 0xFF)) / 3;
}

// A copy of the bar picture `game` (with `desc`, its first level), made on `device`: kRamp, its alpha the picture's
// own times its brightness ramped from kSolidFrom to kSolidTo, so its dim outline fades out (drawn solid); kGrey, its
// colours its brightness, scaled so the middle level of its drawn texels is kGreyMid (recoloured). Null if the picture
// can't be read or the copy made.
void* make_copy(void* device, void* game, const d3d8::SurfaceDesc& desc, int kind)
{
    std::vector<std::uint32_t> argb;
    if (!texels(game, desc, argb))
        return nullptr;
    int mid = kGreyMid;
    if (kind & kGrey) {
        std::vector<int> levels;
        for (std::uint32_t c : argb)
            if (c >> 24 != 0)
                levels.push_back(level_of(c));
        if (!levels.empty()) {
            std::nth_element(levels.begin(), levels.begin() + levels.size() / 2, levels.end());
            mid = std::max(1, levels[levels.size() / 2]);
        }
    }
    void* texture = nullptr;
    if (FAILED(d3d8::call(device, d3d8::slot::CreateTexture, desc.width, desc.height, 1u, DWORD{0}, d3d8::kFormatA8R8G8B8,
                          d3d8::kPoolManaged, &texture)) ||
        texture == nullptr)
        return nullptr;
    d3d8::LockedRect locked{};
    if (FAILED(d3d8::call(texture, d3d8::slot::TextureLockRect, 0u, &locked, static_cast<RECT*>(nullptr), DWORD{0}))) {
        d3d8::call<ULONG>(texture, d3d8::slot::Release);
        return nullptr;
    }
    for (UINT y = 0; y < desc.height; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(locked.bits) + y * locked.pitch);
        for (UINT x = 0; x < desc.width; ++x) {
            const std::uint32_t c = argb[y * desc.width + x];
            const int level = level_of(c);
            std::uint32_t alpha = c >> 24, rgb = c & 0xFFFFFF;
            if (kind & kRamp)
                alpha = alpha * static_cast<std::uint32_t>(std::clamp((level - kSolidFrom) * 255 / (kSolidTo - kSolidFrom), 0, 255)) / 255;
            if (kind & kGrey) {
                const auto grey = static_cast<std::uint32_t>(std::min(255, level * kGreyMid / mid));
                rgb = grey << 16 | grey << 8 | grey;
            }
            row[x] = alpha << 24 | rgb;
        }
    }
    d3d8::call(texture, d3d8::slot::TextureUnlockRect, 0u);
    return texture;
}

// The copy of `kind` a bar drawn with the picture `game` takes, made the first time (on the picture's device). Null if
// there's none to be had.
void* copy_texture(void* game, int kind)
{
    void* device = nullptr;
    if (FAILED(d3d8::call(game, kGetDevice, &device)) || device == nullptr)
        return nullptr;
    d3d8::call<ULONG>(device, d3d8::slot::Release);
    if (device != g_copy_device) {
        g_copies.clear(); // made on another device: went with it
        g_copy_device = device;
    }
    d3d8::SurfaceDesc desc{};
    if (FAILED(d3d8::call(game, d3d8::slot::TextureGetLevelDesc, 0u, &desc)) || desc.width == 0 || desc.height == 0)
        return nullptr;
    for (auto it = g_copies.begin(); it != g_copies.end(); ++it) {
        if (it->game != game || it->kind != kind)
            continue;
        if (it->width == desc.width && it->height == desc.height && it->format == desc.format)
            return it->copy;
        d3d8::call<ULONG>(it->copy, d3d8::slot::Release); // another picture at the address now
        g_copies.erase(it);
        break;
    }
    if (g_copies.size() >= kCopiesAtMost) {
        d3d8::call<ULONG>(g_copies.front().copy, d3d8::slot::Release);
        g_copies.erase(g_copies.begin());
    }
    void* copy = make_copy(device, game, desc, kind);
    if (copy != nullptr)
        g_copies.push_back({game, kind, copy, desc.width, desc.height, desc.format});
    return copy;
}

// The colour picked for a button bar the game draws with `colour` (plain, selected, or selected and greyed out), and
// the colour to give the draw for it; false for one that keeps the game's
bool picked(std::uint32_t colour, std::uint32_t& rgb, std::uint32_t& given)
{
    const std::uint32_t alpha = colour & 0xFF000000;
    switch (colour & 0xFFFFFF) {
    case kGamePlain:
        if (!buttons::bar_colour(false, rgb))
            return false;
        given = alpha | kGamePlain;
        return true;
    case kGameSelected:
        if (!buttons::bar_colour(true, rgb))
            return false;
        given = alpha | kGamePlain;
        return true;
    case kGameSelectedOff:
        if (!buttons::bar_colour(true, rgb))
            return false;
        given = alpha | kGameHalf;
        return true;
    default:
        return false;
    }
}

// A button bar drawn with `copy` in its picture's place (`slot`), its corners' colour `rgb` (if given) and their alpha
// full (if `opaque`), with `given` and `flags`; all put back after
void draw_copy(char* quad, void** slot, void* copy, int x, int y, float sx, float sy, std::uint32_t given, int flags,
               const std::uint32_t* rgb, bool opaque)
{
    void* game = *slot;
    std::uint8_t corner[4][4];
    for (int i = 0; i < 4; ++i) {
        auto* at = reinterpret_cast<std::uint8_t*>(quad + kCorners + i * kCorner + kColour);
        std::memcpy(corner[i], at, 4);
        if (rgb != nullptr) {
            at[0] = static_cast<std::uint8_t>(*rgb & 0xFF);       // B
            at[1] = static_cast<std::uint8_t>(*rgb >> 8 & 0xFF);  // G
            at[2] = static_cast<std::uint8_t>(*rgb >> 16 & 0xFF); // R
        }
        if (opaque)
            at[3] = 0xFF;
    }
    *slot = copy;
    g_draw(quad, x, y, sx, sy, given, flags);
    *slot = game;
    for (int i = 0; i < 4; ++i)
        std::memcpy(quad + kCorners + i * kCorner + kColour, corner[i], 4);
}

// A button bar: drawn solid (a frameless menu's <id>.solid: the game's plain draws, flags 0), in the colour picked
// (buttons.color, buttons.selected.color), both or neither. False for one the game draws as ever, or whose copy can't be
// had yet (the picture not loaded: the game loads it as it draws this first one).
bool draw_button(char* quad, int x, int y, float sx, float sy, std::uint32_t colour, int flags)
{
    const bool solid = g_frameless && flags == 0;
    std::uint32_t rgb = 0, given = colour;
    const bool recolour = picked(colour, rgb, given);
    if (!solid && !recolour)
        return false;
    void** slot = d3d_slot(quad);
    void* copy = slot != nullptr ? copy_texture(*slot, (solid ? kRamp : 0) | (recolour ? kGrey : 0)) : nullptr;
    if (copy == nullptr)
        return false;
    if (solid && !recolour) {
        const auto lift = [colour](int shift) {
            return static_cast<std::uint32_t>(std::min(255L, std::lround((colour >> shift & 0xFF) * kSolidLift))) << shift;
        };
        draw_copy(quad, slot, copy, x, y, sx, sy, (colour & 0xFF000000) | lift(16) | lift(8) | lift(0), kBlendOver, nullptr,
                  true);
    } else if (solid) {
        draw_copy(quad, slot, copy, x, y, sx, sy, given, kBlendOver, &rgb, true);
    } else {
        // added onto the frame: the frame's fill taken off, so the bar comes out the colour picked there
        std::uint32_t added = 0;
        for (int c = 0; c < 3; ++c)
            added |= static_cast<std::uint32_t>(std::max(0, static_cast<int>(rgb >> (c * 8) & 0xFF) - kFrameFill[c])) << (c * 8);
        draw_copy(quad, slot, copy, x, y, sx, sy, given, flags, &added, false);
    }
    return true;
}

void __fastcall draw_hook(char* quad, void*, int x, int y, float sx, float sy, std::uint32_t colour, int flags)
{
    const jplabels::Quad label(quad); // (a game in Japanese: which label's part it is, for True Font)
    const profilewin::TitleQuad title(quad); // (the Profile panel's titles: their own True Font, profile.title.font)
    gamewin::quad(quad, x, y, sx, sy, colour); // vana.gameui.window: the window drawn now's pictures and labels, when they're kept
    if (sliders::on_quad(quad, x, y, sx, sy, colour))
        return; // a slider's part with [client] sliders.style=flat: its own bar, fill and knob drawn (sliders.cpp)
    if (buttons::on_quad(quad, x, y, sx, sy, colour, flags))
        return; // a button's bar with [client] buttons.style=flat: its box drawn in its place (buttons.cpp)
    if (labeltext::letter(quad, x, y, sx, sy, colour))
        return; // a Config window's label's letter: its label is VanaCore's own text, drawn whole (labeltext.cpp)
    if (std::memcmp(quad + kName, kButtonName, sizeof kButtonName - 1) == 0 && draw_button(quad, x, y, sx, sy, colour, flags))
        return; // a button bar: drawn solid, recoloured, or both
    if (name_under_icon(quad, x, y))
        return; // a Check or Equipment Set window slot's name under its item's icon (items.equip)
    if (listtext::in_equip() && std::memcmp(quad + kName, kSlotName, sizeof kSlotName) == 0 && equip_slot(quad, sx))
        listtext::equip_square(x, y); // the Equipment window's slot names are VanaCore's own text at each square (listtext.cpp)
    if ((g_style == Style::Game && g_written.empty() && !g_equip) ||
        std::memcmp(quad + kName, kSlotName, sizeof kSlotName) != 0) {
        g_draw(quad, x, y, sx, sy, colour, flags);
        return;
    }
    const Window kind = g_style == Style::Hide || g_equip ? window_kind() : Window::Other;
    if (kind == Window::Equip && g_equip && equip_slot(quad, sx)) {
        draw_equip(quad, x, y, sx, sy, colour, flags);
        return;
    }
    if (g_style == Style::Hide && kind != Window::Outlined && kind != Window::Field)
        return;
    sync_texture(quad);
    if (kind == Window::Field) {
        // the game's own box: its texture put back for this draw and the outline's after
        const bool swapped = !g_written.empty() && g_d3d != nullptr && texture_blocks(g_d3d, g_original, true);
        g_draw(quad, x, y, sx, sy, colour, flags);
        if (swapped)
            texture_blocks(g_d3d, g_written, true);
        return;
    }
    if (g_style == Style::Game) {
        g_draw(quad, x, y, sx, sy, colour, flags);
        return;
    }
    // Most windows add their squares onto the window (or take them away), where a dark colour adds nothing and black
    // goes see-through: each is drawn blended over it instead, as the item details window draws its own, at its
    // strength times the opacity (an outline: full white)
    std::uint8_t alpha[4];
    const int strength = g_style == Style::Hide ? kOpaque : kDetailsAlpha;
    const auto faded = static_cast<std::uint8_t>(std::min<long>(kOpaque, std::lround(strength * g_opacity)));
    for (int i = 0; i < 4; ++i) {
        auto* a = reinterpret_cast<std::uint8_t*>(quad + kCorners + i * kCorner + kAlpha);
        alpha[i] = *a;
        *a = faded;
    }
    g_draw(quad, x, y, sx, sy, colour, kBlendOver);
    for (int i = 0; i < 4; ++i)
        *reinterpret_cast<std::uint8_t*>(quad + kCorners + i * kCorner + kAlpha) = alpha[i];
}

void install_hook()
{
    g_tried = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t at = game != nullptr ? scan::find(game, kDrawQuad) : 0;
    if (at == 0) {
        log::write(L"Item slot: the game's menu picture drawing wasn't found in FFXiMain.dll: [client] items.slot does nothing");
        return;
    }
    void* trampoline = imports::detour(reinterpret_cast<void*>(at), kPrologue, reinterpret_cast<void*>(&draw_hook));
    if (trampoline == nullptr) {
        log::write(L"Item slot: the game's menu picture drawing couldn't be hooked: [client] items.slot does nothing");
        return;
    }
    g_draw = reinterpret_cast<DrawQuad>(trampoline);
    log::write(L"Item slot: the menu picture drawing hooked at +0x%X",
               static_cast<unsigned>(at - reinterpret_cast<std::uintptr_t>(game)));
}

void __fastcall badge_hook(void* shape, void*, int x, int y, std::uint32_t colour, int flags, int first)
{
    if (g_wardrobe)
        g_draw_badge(shape, x, y, colour, flags, first);
}

void __fastcall digit_hook(void* window, void*, int slot, int container, int x, int y)
{
    if (g_wardrobe)
        g_draw_digit(window, slot, container, x, y);
}

std::uintptr_t call_target(std::uintptr_t call)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), sizeof rel);
    return call + 5 + rel;
}

bool is_call(std::uintptr_t at) { return *reinterpret_cast<const std::uint8_t*>(at) == 0xE8; }

void install_badge()
{
    g_badge_tried = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t at = game != nullptr ? scan::find(game, kBadgeDraw) : 0;
    if (at == 0 || !is_call(at + kBadgeCall) || !is_call(at + kDigitCall)) {
        log::write(L"Item slot: the Equipment window's wardrobe number wasn't found in FFXiMain.dll: [client] "
                   L"items.equip.wardrobe does nothing");
        return;
    }
    g_draw_badge = reinterpret_cast<DrawShape>(call_target(at + kBadgeCall));
    g_draw_digit = reinterpret_cast<DrawDigit>(call_target(at + kDigitCall));
    asm_text::branch(at + kBadgeCall, 0xE8, reinterpret_cast<std::uintptr_t>(&badge_hook), 0);
    asm_text::branch(at + kDigitCall, 0xE8, reinterpret_cast<std::uintptr_t>(&digit_hook), 0);
    log::write(L"Item slot: the Equipment window's wardrobe number drawing redirected at +0x%X",
               static_cast<unsigned>(at + kBadgeCall - reinterpret_cast<std::uintptr_t>(game)));
}

} // namespace

void reload(const wchar_t* ini)
{
    wchar_t text[64] = {};
    GetPrivateProfileStringW(L"client", L"items.slot", L"game", text, 64, ini);
    g_style = _wcsicmp(text, L"tint") == 0    ? Style::Tint
              : _wcsicmp(text, L"solid") == 0 ? Style::Solid
              : _wcsicmp(text, L"hide") == 0  ? Style::Hide
                                              : Style::Game;
    GetPrivateProfileStringW(L"client", L"items.slot.color", L"", text, 64, ini);
    float colour[4] = {255.0f, 255.0f, 255.0f, 1.0f};
    text::parse_colour(text, colour);
    for (int i = 0; i < 3; ++i)
        g_colour[i] = colour[i] / 255.0f;
    const auto percent = [ini](const wchar_t* key, int top) {
        const int value = static_cast<int>(GetPrivateProfileIntW(L"client", key, 100, ini));
        return (value < 0 ? 0 : value > top ? top : value) / 100.0f;
    };
    g_brightness = percent(L"items.slot.brightness", 200);
    g_contrast = percent(L"items.slot.contrast", 300);
    g_opacity = percent(L"items.slot.opacity", 200);
    g_dirty = true;

    g_equip = GetPrivateProfileIntW(L"client", L"items.equip", 0, ini) != 0;
    const auto argb = [ini, &text](const wchar_t* key, const wchar_t* fallback, const wchar_t* opacity, int opaque) {
        float c[4] = {255.0f, 255.0f, 255.0f, 1.0f};
        text::parse_colour(fallback, c);
        GetPrivateProfileStringW(L"client", key, L"", text, 64, ini);
        text::parse_colour(text, c);
        const int value = static_cast<int>(GetPrivateProfileIntW(L"client", opacity, opaque, ini));
        const auto byte = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 255.0f))); };
        return byte((value < 0 ? 0 : value > 100 ? 100 : value) * 2.55f) << 24 | byte(c[0]) << 16 | byte(c[1]) << 8 |
               byte(c[2]);
    };
    g_equip_fill = argb(L"items.equip.background", L"#000000", L"items.equip.background.opacity", 50);
    g_equip_border = argb(L"items.equip.border", L"#FFFFFF", L"items.equip.border.opacity", 60);
    const int size = static_cast<int>(GetPrivateProfileIntW(L"client", L"items.equip.border.size", 1, ini));
    g_equip_size = size < 0 ? 0 : size > 8 ? 8 : size;
    g_equip_dirty = true;
    g_wardrobe = GetPrivateProfileIntW(L"client", L"items.equip.wardrobe", 1, ini) != 0;
}

void install(const wchar_t* ini)
{
    reload(ini);
    buttons::install(ini); // [client] buttons.*: the menu buttons, drawn through this hook
    sliders::reload(ini);  // [client] sliders.*: the Config windows' sliders, the same
}

const void* enter_window(const void* ctrl)
{
    buttons::enter_window(ctrl); // [client] buttons.*: its style; flat, the window's labels drawn after its boxes
    sliders::enter_window(ctrl); // [client] sliders.*: a Config window's sliders kept while it's drawn
    const void* outer = g_window;
    g_window = static_cast<const char*>(ctrl);
    return outer;
}

void leave_window(const void* outer)
{
    buttons::leave_window(); // (while it's still the window they're drawn in)
    sliders::leave_window(); // (a slider at 0: its knob)
    g_window = static_cast<const char*>(outer);
}

void icon_drawn(int x, int y)
{
    if (!g_equip || !in_named_slots())
        return;
    turn_icons();
    if (g_icons_now.count < kIconsAtMost) {
        g_icons_now.x[g_icons_now.count] = static_cast<short>(x);
        g_icons_now.y[g_icons_now.count] = static_cast<short>(y);
        ++g_icons_now.count;
    }
}

bool wants_icons() { return g_equip; }

void draw_game(void* quad, int x, int y, float sx, float sy, std::uint32_t colour, int flags)
{
    if (g_draw != nullptr)
        g_draw(quad, x, y, sx, sy, colour, flags);
}

void frameless(bool on)
{
    g_frameless = on;
    if (on)
        g_solid_wanted = true;
}

void on_frame()
{
    ++g_frame;
    sliders::on_frame();
    if ((g_style != Style::Game || g_equip || g_solid_wanted || buttons::bars_coloured() || buttons::active() ||
         sliders::active() ||                        // (the Config windows' sliders drawn flat)
         gamewin::wants_quads() || jplabels::on() || // (a game in Japanese: True Font's labels' words, jplabels.cpp)
         labeltext::wants_quads() ||                 // (the Config windows' labels drawn whole)
         listtext::equip_wanted() ||                 // (the Equipment window's slot names drawn whole: its squares noted)
         profilewin::wants_quads()) &&               // (the Profile panel's titles' True Font)
        !g_tried)
        install_hook();
    if (!g_wardrobe && !g_badge_tried)
        install_badge();
}

} // namespace vanacore::itemslot
