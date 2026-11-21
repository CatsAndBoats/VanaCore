#include "damagetext.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include "../gfx/vanacore_gfx.h"
#include "compat/ashita/host.hpp"
#include "d3d8.hpp"
#include "game.hpp"
#include "log.hpp"
#include "nameplates.hpp"
#include "scan.hpp"
#include "text.hpp"
#include "window.hpp"

namespace vanacore::damagetext {
namespace {

// FFXiMain.dll floats a number (or "Miss!") over a character as it's hit: a popup object, made by the hit's
// animation, holds the text the game printed for it ("%d", or ":" for a miss) and draws it each frame for its life
// (150 frames, or damage.duration's), rising and fading. Its draw works out the colour (the target's name colour, or the popup's own:
// a miss's green) and hands the text to a layout routine:
//   push 3DCCCCCDh (0.1); push eax; add edi,198h (the popup's text); push esi (the colour); lea ecx,[esp+20h];
//   push edi; push ecx (where); call (the layout, __cdecl); add esp,14h
constexpr char kPopupCall[] = "68 ?? ?? ?? ?? 50 81 C7 98 01 00 00 56 8D 4C 24 20 57 51 E8 ?? ?? ?? ?? 83 C4 14";
constexpr std::uintptr_t kPopupCallAt = 19;
constexpr std::uintptr_t kPopupText = 0x198;  // the text in the popup, which the layout gets
constexpr std::uintptr_t kPopupTarget = 0xFC; // a pointer to a pointer to its target's handle: the entity's
constexpr std::uintptr_t kHandleId = 0x58;    // server id in it (as the game checks it before drawing)
constexpr std::uintptr_t kHandleIndex = 0x54; // and its target index
// A popup's point is laid out through the projection the renderer holds (its object's +0x9BC, 16 floats) at that moment. The
// popup's own draw sets one first, and in one of its paths that is a fixed one (a focal length of 350, 57.5 degrees up and
// down: 2 atan(192 / 350)) where the scene is drawn with the camera's: the game's default zoom has the two the same, but any
// other focal length ([camera] fov, True Zoom, the game's option 0x49) moves the point from where the target is drawn by the
// ratio of the two tangents, more the further from the screen's middle it is. The scene's projection is the camera's
// getter's (mov eax,[scene]; add eax,154h; ret), its _11 and _22 (how far across the screen a unit at a distance of 1 is).
constexpr std::uintptr_t kRendererProjection = 0x9BC;
constexpr char kSceneGetter[] = "A1 ?? ?? ?? ?? 05 54 01 00 00 C3";
constexpr std::uintptr_t kSceneProjection = 0x154;
constexpr float kProjectionAgree = 0.003f;
constexpr float kMinPopupHeight = 1.5f;   // yalms: a number is kept at least this far above its target's feet // _11 and _22 this close (a share) are the same
constexpr int kProbeLines = 24;            // popups said in the log, once each: where the game put them, with which projection
// The layout centres the text's glyphs on the point, from sprite set "menu    dmgnum" (ROM\119\51.DAT: big digits for
// '0'-'9', "Miss!" for ':', each cut from the names' font sheet), a quad each, sized by the target's distance. Each
// glyph's sprite (__thiscall on the font: the glyph, its corners' texture coordinates and its size in units, both
// written; ret 14h):
//   push 0; push 0; push esi (its size); push ebp (its corners); push eax (the glyph); mov ecx,<font>; call;
//   movsx eax,word ptr [esi]; add ebx,eax (the text's width)
constexpr char kGlyphCall[] = "6A 00 6A 00 56 55 50 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F BF 06 03 D8";
constexpr std::uintptr_t kGlyphCallAt = 12;
// Each glyph's quad drawn, as the names' letters are: mov ecx,[<renderer>]; lea edx,[esp+...] (its 4 vertices);
// push 1Ch (their stride); push edx; push 2 (triangles); push 5 (a strip); call (DrawPrimitiveUP, __thiscall)
constexpr char kDrawCall[] = "8B 0D ?? ?? ?? ?? 8D 94 24 ?? ?? ?? ?? 6A 1C 52 6A 02 6A 05 E8";
constexpr std::uintptr_t kDrawCallAt = 20;
constexpr std::size_t kLayoutWithin = 0x400;  // the glyphs' and the draw's calls, from the layout's start
constexpr std::uintptr_t kRendererDevice = 0xC;
// The popups are drawn with the effects, and the names over heads after them, the last of what goes into the 3D scene's
// target; then the game lays the scene onto the frame: SetRenderState(ZWRITEENABLE, 1) (push 1; push 0Eh;
// mov eax,[edx+0Ch]; push eax; mov ecx,[eax]; call [ecx+0C8h]), then mov ecx,<renderer>; push ebx; push ebx; call (the
// stage's texture let go and the frame's target set: __thiscall, ret 8). The numbers over everything (damage.ontop) are
// kept till there and drawn before that call: over the names too.
constexpr char kLaidCall[] = "6A 01 6A 0E 8B 42 0C 50 8B 08 FF 91 C8 00 00 00 8B 0D ?? ?? ?? ?? 53 53 E8";
constexpr std::uintptr_t kLaidCallAt = 24;
constexpr std::size_t kMostKept = 512; // quads kept a frame; past that drawn at once
constexpr unsigned kUnlaidFrames = 3;  // frames in a row with kept quads never drawn: they're drawn at once from then
constexpr int kMostGlyphs = 16;               // the layout's own limit
// The multi-hit icon: crossed swords (U+2694) from this font, or the hits' count (U+00D7 and the number) in the
// numbers' own; its gap from the number as a share of its size
constexpr wchar_t kSymbolFont[] = L"Segoe UI Symbol";
constexpr char kSwords[] = "\xE2\x9A\x94";
constexpr char kTimes[] = "\xC3\x97";
constexpr float kIconGap = 0.25f;
// The game's glyphs: their texels' alpha (ffxi-hd's and the game's own: 136 in the fill, 119 the outline) and
// their heights in units (a digit 18, "Miss!" 15, both drawn 0.1 of a unit to the world's)
constexpr float kGlyphAlpha = 136.0f / 255.0f;
constexpr float kDigitUnits = 18.0f, kMissUnits = 15.0f;
// VanaCore's text over the game's glyphs: the font's size (its em) times the glyph's height. The game's glyphs fill
// their cells, outline and all; a font's digits are about 0.7 of its em.
constexpr float kTextShare = 1.2f;
constexpr float kEighths = 8.0f;        // pictures are made at sizes this many to a pixel
constexpr float kRemake = 0.08f;        // how much bigger or smaller than its picture a popup is drawn before it's made again
constexpr float kOnPixels = 0.02f;      // drawn at its picture's own size, within this: on the screen's whole pixels
constexpr int kPicturesPerFrame = 6;    // the rest are drawn from their picture at the size before, or as the game's
constexpr double kMakingMs = 3.0;       // and after the first a frame, only while they've taken less than this
constexpr std::size_t kMostPictures = 256;
constexpr ULONGLONG kPictureKept = 10000; // a picture unused this long is let go
constexpr ULONGLONG kPopupGone = 1000;    // a popup not drawn this long is gone (its object may be another's)
// The server's actions (0x028): each target's results, kept this long for the popups they make, which come with the
// hit's animation, a moment after
constexpr std::uint16_t kAction = 0x028;
constexpr std::size_t kHeader = 0x1C; // the received buffer's header
constexpr ULONGLONG kResultKept = 10000;
constexpr std::size_t kMostResults = 512;
// A critical hit: melee's message 67 ("scores a critical hit!"), ranged's 353; LandSandBoat's results carry the hit
// effect CRITICAL_HIT (0x22) as well
constexpr std::uint32_t kCritMelee = 67, kCritRanged = 353, kCritEffect = 0x22;
// An action's category (0x028), the skills that have a name: a weapon skill (its id in the resources' abilities, from
// 0), a spell (its id in the spells), a job ability, and the dancer's and rune fencer's (the ability's id; the
// resources' abilities have them from 0x200)
constexpr std::uint32_t kCategoryWeaponSkill = 3, kCategorySpell = 4, kCategoryAbility = 6, kCategoryDancer = 14,
                        kCategoryRune = 15;
constexpr std::uint32_t kAbilitiesFrom = 0x200;
// A weapon skill's category (3) is also the server's for the job abilities that hit (Jump, Steal, Shield Bash...), with the
// ability's id, which is a weapon skill's id too (Jump 66, Gale Axe 66): the result's message says which it is. Those a job
// ability's results carry; a weapon skill's are 185 to 189, 224 to 226, 238 and 263 to 264.
constexpr std::uint32_t kAbilityMessages[] = {100, 102, 110, 119, 123, 125, 127, 129, 156, 158, 266, 267, 316,
                                              317, 318, 320, 321, 323, 324, 379, 414, 415, 441, 451, 453, 608, 743};
// The messages for HP recovered (a spell's, a skill's, an ability's, an area's): their number is a heal. The result's
// message decides, not the spell: a Cure on an undead target comes as message 2 (casts, takes damage), so it's damage.
constexpr std::uint32_t kHealMessages[] = {7, 24, 26, 102, 103, 238, 263, 318, 357, 367};
// The messages a "Miss!" comes with: a melee, weapon skill, ranged and job ability miss, and shadows taking the hit (its
// param is how many)
constexpr std::uint32_t kMissMessages[] = {15, 31, 32, 158, 188, 324, 354};
// The messages whose param isn't a number the game shows: a status effect's id, a count, MP or TP, a stolen item. A result
// with one never gets a popup, so it's not kept for one to take (it would give its skill's name to a hit of the same amount)
constexpr std::uint32_t kNoAmountMessages[] = {25,  75,  83,  85,  93,  100, 101, 119, 123, 125, 127, 129, 142, 144, 146, 156,
                                               159, 186, 189, 224, 225, 226, 228, 230, 236, 237, 242, 243, 266, 267, 268, 271,
                                               276, 284, 316, 319, 320, 321, 323, 329, 330, 331, 332, 333, 334, 335, 341, 355,
                                               358, 360, 414, 415, 430, 431, 441, 451, 452, 453, 454, 533, 539, 571, 572, 608,
                                               655, 743};
// Popups made for one target within this of each other (ms) are stacked (damage.spawn=stacked); those drawn within the
// last kLive ms are the ones still showing
constexpr ULONGLONG kBurst = 700, kLive = 250;
// The game makes each number twice, a popup and its glow (drawn additively), at the same place; made within this (ms)
// of each other, for one target with one text, they're a pair
constexpr ULONGLONG kTwin = 100;
constexpr int kMostStack = 8;
// The preview and the test hits: a digit's cell the share of the screen's height the game's last numbers had (the middle of
// the last kSizesKept, as each was first drawn: the camera's distance in a fight), or before any this share (a
// melee, 2026-11-10: 0.05 drew them about 3 times too big); the samples' row this far down
constexpr float kPreviewCell = 0.017f, kPreviewRow = 0.38f;
constexpr int kSizesKept = 15;

using LayoutFn = void(__cdecl*)(const void* at, const char* text, std::uint32_t colour, const void* vec,
                                std::uint32_t scale);
using GlyphFn = bool(__fastcall*)(void* font, void* edx, unsigned code, float* uv, short* size, short* x, short* y);
using DrawFn = std::uint32_t(__fastcall*)(void* renderer, void* edx, unsigned prim, unsigned count,
                                          const void* vertices, unsigned stride);
using LaidFn = std::uint32_t(__fastcall*)(void* renderer, void* edx, std::uint32_t a, std::uint32_t b);

// A glyph's vertices as the game draws them (its FVF: position, rhw, colour, one set of texture coordinates)
struct Vertex {
    float x, y, z, rhw;
    DWORD colour;
    float u, v;
};

#include "damagetext_look.h" // Look, look_for, colour_times, shown_rgb, neutral_rgb, describe: the numbers' colours

struct Picture {
    text::Sprite sprite;
    std::string text; // UTF-8
    std::string look; // its Look's key
    int eighths = 0; // its font's size, in eighths of a pixel
    bool symbol = false; // in the symbol font (the multi-hit icon), not the numbers'
    ULONGLONG used = 0;
};

// A popup the game is drawing, by its object: what it shows, whether it's a critical hit or one of several, which
// skill did it and where it's placed (all decided once)
struct Known {
    std::string game;  // the game's text
    std::string shown; // VanaCore's (empty: the game's glyphs)
    std::string label; // the skill's name (damage.label)
    bool miss = false;
    bool crit = false;
    bool taken = false; // it's on you
    bool heal = false;  // HP recovered, not damage
    int hits = 0;      // the results the server's action had for its target: more than 1 is a multi-hit round (0: unknown)
    float dx = 0.0f, dy = 0.0f; // where it's placed, in digit heights: right and up
    std::uint32_t target = 0;
    int eighths = 0;    // its picture's size
    int label_eighths = 0;
    int icon_eighths = 0;
    bool twin = false;   // the game's second popup for a hit (its glow): placed and styled as the first
    bool paired = false; // and the first has its twin
    bool looked = false; // its pictures' colours are decided (Settings::baked: from its first draw's colour)
    Look look;
    ULONGLONG born = 0;
    ULONGLONG seen = 0;
};

// A result the server sent: a hit's amount on a target
struct Result {
    std::uint32_t target;
    std::uint32_t amount;
    bool crit;
    bool heal;  // HP recovered
    bool taken; // a popup has it
    ULONGLONG at;
    std::uint32_t category; // of the action, and its argument: the skill
    std::uint32_t param;
    int hits;               // the results the action had for the target
};

// The popup being laid out
struct Popup {
    bool on = false;     // its layout is followed (the glyphs' sizes noted, the game's quads drawn moved)
    bool sharp = false;  // and its text is VanaCore's
    Known* known = nullptr;
    int glyphs = 0;
    short width[kMostGlyphs] = {};
    short height[kMostGlyphs] = {};
    int total = 0;   // the glyphs' widths, in units
    int draws = 0;   // its quads drawn so far
    bool drawn = false; // VanaCore's text is: the glyphs after the first draw nothing
    bool on_top = false; // its draws without the depth test (damage.ontop), the game's glyphs or VanaCore's text
    bool keep = false;   // and kept to be drawn after the names (g_keep)
    bool probe = false;  // its first quad's place is noted (the log's lines about where the game puts popups)
    bool probed = false; // and is
    float probe_x = 0.0f, probe_y = 0.0f, probe_h = 0.0f;
    unsigned target = ~0u; // its target's index: whose name the number stays near
    float lift = 0.0f;     // how far up the number is moved (game pixels) to be near it
    bool lifted = false;   // that is worked out, from the first quad
    float name_bottom = 0.0f; // the name's, for the log
};

// What a popup's quad is drawn with: the device's state for it as the game has it (with VanaCore's text's changes)
constexpr DWORD kStates[] = {d3d8::rs::AlphaBlendEnable, d3d8::rs::SrcBlend,    d3d8::rs::DestBlend,
                             d3d8::rs::BlendOp,          d3d8::rs::AlphaTestEnable, d3d8::rs::AlphaFunc,
                             d3d8::rs::AlphaRef,         d3d8::rs::FogEnable,   d3d8::rs::CullMode,
                             d3d8::rs::TextureFactor,    d3d8::rs::ZEnable};
constexpr DWORD kStage0[] = {d3d8::tss::ColorOp,   d3d8::tss::ColorArg1,     d3d8::tss::ColorArg2,
                             d3d8::tss::AlphaOp,   d3d8::tss::AlphaArg1,     d3d8::tss::AlphaArg2,
                             d3d8::tss::TexCoordIndex, d3d8::tss::TextureTransformFlags, d3d8::tss::AddressU,
                             d3d8::tss::AddressV,  d3d8::tss::MagFilter,     d3d8::tss::MinFilter,
                             d3d8::tss::MipFilter};
constexpr DWORD kStage1[] = {d3d8::tss::ColorOp, d3d8::tss::AlphaOp};

struct DeviceState {
    DWORD states[std::size(kStates)] = {};
    DWORD stage0[std::size(kStage0)] = {};
    DWORD stage1[std::size(kStage1)] = {};
    DWORD vertex_shader = 0, pixel_shader = 0;
    d3d8::Viewport viewport{};
    void* texture = nullptr; // stage 0's, held
};

// A popup's quad kept to be drawn over everything once the names are (damage.ontop)
struct Kept {
    void* renderer;
    void* target; // the render target it's for (compared, not held)
    Vertex quad[4];
    DeviceState state;
};

// A test hit: a popup VanaCore makes and draws itself over the player (the buttons in /vana), placed as a hit's are
struct Fake {
    Known known;
    ULONGLONG start = 0; // when it appears
    bool placed = false;
};

constexpr float kTestRow = 0.42f;     // the middle of where they appear, down the screen
constexpr float kTestRise = 2.0f;     // how far they float up over their life, in digit heights

// Only touched on the game's thread.
Settings g_settings;
std::vector<Fake> g_tests;
bool g_tried = false;
bool g_hooked = false;       // the layout's call
bool g_sharp_ready = false;  // and the glyphs' and the draw's
LayoutFn g_layout = nullptr; // the game's
GlyphFn g_glyph = nullptr;
DrawFn g_draw = nullptr;
LaidFn g_laid = nullptr;     // the game's call laying the scene onto the frame
std::uintptr_t g_renderer_at = 0; // the global holding the renderer (the draw call's operand)
std::uintptr_t g_scene_at = 0;    // and the one holding the scene (the camera's getter's)
int g_probed = 0;            // popups said in the log so far
// How far below its target's name a number's middle may be, in the number's own heights: further down than this (the game
// hangs a number from a point of its target's model, which is at the feet or under them for a model that moves for its
// fight) and it is moved up to it. A global so it can be set in the running game.
float g_name_gap = 6.0f;
bool g_keep = false;         // the numbers over everything are kept and drawn there (it's VanaCore's)
std::vector<Kept> g_kept;
unsigned g_unlaid = 0;       // frames in a row that ended with kept quads not drawn
std::unordered_map<std::uintptr_t, Known> g_known;
std::deque<Result> g_results;
Popup g_popup;
std::unordered_map<std::string, std::unique_ptr<Picture>> g_pictures; // by "<look><eighths>|<text>"
std::unordered_map<std::string, Picture*> g_latest;                   // each text's last made, by "<look>|<text>"
std::wstring g_style;          // the look they're in
void* g_device = nullptr;      // the device their textures are on
int g_made_this_frame = 0;
double g_made_ms = 0.0;        // what making them has taken this frame
ULONGLONG g_swept_at = 0;
float g_shown_w = 0.0f, g_shown_h = 0.0f; // the size the 3D scene is shown at, this frame
std::wstring g_state;
unsigned g_crits_said = 0;

bool write_code(std::uintptr_t at, const void* bytes, std::size_t size)
{
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), size, PAGE_EXECUTE_READWRITE, &protect))
        return false;
    std::memcpy(reinterpret_cast<void*>(at), bytes, size);
    VirtualProtect(reinterpret_cast<void*>(at), size, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), size);
    return true;
}

std::uintptr_t target_of(std::uintptr_t call)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), 4);
    return call + 5 + rel;
}

bool redirect(std::uintptr_t call, const void* ours, void*& theirs)
{
    theirs = reinterpret_cast<void*>(target_of(call));
    const std::int32_t to = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(ours) - (call + 5));
    return write_code(call + 1, &to, 4);
}

// Reads `size` bytes of the game's memory at `at`; false where they can't be read
bool peek(std::uintptr_t at, void* out, std::size_t size)
{
    SIZE_T got = 0;
    return at != 0 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at), out, size, &got) &&
           got == size;
}

#include "damagetext_life.h" // install_life, stretch_life, note_size, preview_cell, test_life: damage.duration

// 0xRRGGBB in the game's colour scale (0x80 full)
std::uint32_t game_colour(std::uint32_t rgb)
{
    std::uint32_t out = 0;
    for (int shift = 0; shift < 24; shift += 8)
        out |= ((rgb >> shift & 0xFF) * 128 + 127) / 255 << shift;
    return out;
}

std::uint32_t bits(const std::uint8_t* packet, std::size_t at, unsigned count)
{
    std::uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i)
        value |= static_cast<std::uint32_t>((packet[(at + i) / 8] >> ((at + i) % 8)) & 1) << i;
    return value;
}

template <std::size_t N>
bool listed(const std::uint32_t (&list)[N], std::uint32_t value)
{
    return std::find(std::begin(list), std::end(list), value) != std::end(list);
}

// ---- critical hits ------------------------------------------------------------------------------------------------

// 0x028, bit-packed from its 5th byte, lowest bit first: the actor (32 bits), the targets (6), results (4), the
// category (4), its argument (32), the recast (32); each target's id (32) and results (4); each result's reaction
// (5), animation (12), effect (7), knockback (3), param (17: the amount), message (10), info (31), then an added
// effect (a bit; animation 6, effect 4, param 17, message 10) and a spike effect (a bit; 6, 4, 14, 10).
void on_action(const std::uint8_t* packet, std::size_t length, ULONGLONG now)
{
    const std::size_t end = length * 8;
    std::size_t at = 72;
    bool short_read = false;
    const auto take = [&](unsigned count) -> std::uint32_t {
        if (at + count > end) {
            short_read = true;
            return 0;
        }
        const std::uint32_t value = bits(packet, at, count);
        at += count;
        return value;
    };
    const std::uint32_t targets = take(6);
    take(4);
    const std::uint32_t category = take(4);
    const std::uint32_t param = take(32) & 0xFFFF; // the argument: its low 16 bits are the skill's id
    take(32);
    std::vector<Result> made;
    bool ability = false; // a job ability under a weapon skill's category
    for (std::uint32_t t = 0; t < targets && !short_read; ++t) {
        const std::uint32_t target = take(32);
        const std::uint32_t results = take(4);
        for (std::uint32_t r = 0; r < results && !short_read; ++r) {
            const std::uint32_t reaction = take(5); // the resolution (a miss is 1) in its low 3 bits
            take(12);
            const std::uint32_t effect = take(7);
            take(3);
            std::uint32_t amount = take(17);
            const std::uint32_t message = take(10);
            take(31);
            if (take(1) != 0) {
                take(6);
                take(4);
                take(17);
                take(10);
            }
            if (take(1) != 0) {
                take(6);
                take(4);
                take(14);
                take(10);
            }
            if (short_read)
                return;
            if (listed(kAbilityMessages, message))
                ability = true;
            // only a result the game draws a number for is kept for a popup to take: a miss (of no amount), or a hit with
            // one. A buff, a status effect or a steal has a param that isn't an amount (0, or an effect's id), and kept it
            // would be taken by a miss or by a hit of that number, which would then show its skill's name.
            if (listed(kMissMessages, message) || (amount == 0 && (reaction & 7) == 1))
                amount = 0;
            else if (amount == 0 || listed(kNoAmountMessages, message))
                continue;
            const bool crit = message == kCritMelee || message == kCritRanged || effect == kCritEffect;
            made.push_back({target, amount, crit, listed(kHealMessages, message), false, now, category, param,
                            static_cast<int>(results)});
        }
    }
    const std::uint32_t kind = category == kCategoryWeaponSkill && ability ? kCategoryAbility : category;
    for (Result& result : made) {
        result.category = kind;
        if (g_results.size() >= kMostResults)
            g_results.pop_front();
        g_results.push_back(result);
    }
}

// The popup's target's server id: 0 if it has none
std::uint32_t target_id(std::uintptr_t popup)
{
    std::uintptr_t handle = *reinterpret_cast<const std::uintptr_t*>(popup + kPopupTarget);
    if (handle == 0)
        return 0;
    handle = *reinterpret_cast<const std::uintptr_t*>(handle);
    return handle != 0 ? *reinterpret_cast<const std::uint32_t*>(handle + kHandleId) : 0;
}

// The result the popup's amount on its target is: the oldest the server sent for that target with that amount that no
// popup has yet. nullptr if there's none.
Result* take_result(std::uint32_t target, std::uint32_t amount, ULONGLONG now)
{
    if (target == 0)
        return nullptr;
    for (Result& result : g_results) {
        if (result.taken || result.target != target || result.amount != amount || now - result.at > kResultKept)
            continue;
        result.taken = true;
        if (result.crit && g_crits_said < 5) {
            ++g_crits_said;
            log::write(L"Graphics: %u on %08X was a critical hit: its number in #%06X ([graphics] damage.crit)",
                       amount, target, g_settings.crit_colour);
        }
        return &result;
    }
    return nullptr;
}

// The name of the weapon skill, spell or ability an action was, from the game's resources; empty for the others
// (auto-attacks, a monster's moves). Kept: the lookups are the game's strings.
std::string skill_name(std::uint32_t category, std::uint32_t param)
{
    const char* table = nullptr;
    std::uint32_t index = param;
    if (category == kCategoryWeaponSkill) {
        table = "abilities.names";
    } else if (category == kCategorySpell) {
        table = "spells.names";
    } else if (category == kCategoryAbility || category == kCategoryDancer || category == kCategoryRune) {
        table = "abilities.names";
        index += kAbilitiesFrom;
    } else {
        return {};
    }
    static std::unordered_map<std::uint64_t, std::string> names;
    const std::uint64_t key = static_cast<std::uint64_t>(category) << 32 | index;
    if (const auto found = names.find(key); found != names.end())
        return found->second;
    const char* text = ashita::resource_string(table, index, 0);
    std::string name = text != nullptr ? text : std::string();
    if (name.empty() || name[0] == '.' || name[0] == '#') // the tables' blanks: ".", "#0"
        return {};
    return names.emplace(key, std::move(name)).first->second;
}

float random_unit()
{
    static std::uint32_t state = 0;
    if (state == 0)
        state = static_cast<std::uint32_t>(GetTickCount64() * 2654435761u) | 1u;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<float>(state & 0xFFFFFF) / static_cast<float>(0x1000000);
}

// The first popup of the pair `known` (new, at `popup`) is the second of: nullptr if it's the first
Known* twin_of(std::uintptr_t popup, const Known& known, ULONGLONG now)
{
    if (known.target == 0)
        return nullptr;
    for (auto& [other_at, other] : g_known)
        if (other_at != popup && !other.twin && !other.paired && other.target == known.target &&
            other.game == known.game && now - other.born < kTwin)
            return &other;
    return nullptr;
}

// How many popups for the same target made a moment before are still showing (the ones a new popup stacks above)
int stack_slot(std::uintptr_t popup, const Known& known, ULONGLONG now)
{
    int slot = 0;
    for (const auto& [other_at, other] : g_known)
        if (other_at != popup && !other.twin && other.target == known.target && now - other.seen < kLive &&
            now - other.born < kBurst)
            ++slot;
    return slot;
}

// Where a new popup is placed, from damage.spawn and damage.offset.*, `slot` popups for its target showing before it
void place(Known& known, int slot)
{
    float dx = g_settings.offset_x, dy = g_settings.offset_y;
    if (g_settings.spawn == Spawn::Random) {
        const float angle = random_unit() * 6.2831853f;
        const float reach = std::sqrt(random_unit()) * g_settings.spread;
        dx += std::cos(angle) * reach;
        dy += std::sin(angle) * reach;
    } else if (g_settings.spawn == Spawn::Stacked && known.target != 0) {
        dy += static_cast<float>(std::min(slot, kMostStack)) * g_settings.spread;
    }
    known.dx = dx;
    known.dy = dy;
}

// What VanaCore draws for the game's text: its digits (with commas), or "Miss!"; empty for anything else (the game's
// glyphs then)
std::string shown_for(const std::string& game, bool& miss)
{
    miss = false;
    // ':' is the big "Miss!", 'E' the small; ';' to 'D' the small digits
    if (game == ":" || game == "E") {
        miss = true;
        return g_settings.miss_caps ? "MISS!" : "Miss!";
    }
    std::string digits;
    for (const char c : game) {
        if (c >= '0' && c <= '9')
            digits += c;
        else if (c >= ';' && c <= 'D')
            digits += static_cast<char>('0' + (c - ';'));
        else
            return {};
    }
    if (digits.empty() || !g_settings.commas)
        return digits;
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0)
            out += ',';
        out += digits[i];
    }
    return out;
}

// The popup at `popup` with the game's `text`: known from before, or met now
Known& know(std::uintptr_t popup, const char* text, ULONGLONG now)
{
    Known& known = g_known[popup];
    if (known.seen == 0 || now - known.seen > kPopupGone || known.game != text) {
        known = Known{};
        stretch_life(popup, now);
        known.game = text;
        known.shown = shown_for(known.game, known.miss);
        known.born = now;
        known.target = target_id(popup);
        known.taken = known.target != 0 && known.target == game::local_player_id();
        std::uint32_t amount = 0;
        bool number = !known.game.empty();
        for (const char c : known.game) {
            if (c < '0' || c > '9') {
                number = false;
                break;
            }
            amount = amount * 10 + static_cast<std::uint32_t>(c - '0');
        }
        // the game's second popup for a hit (its glow) is the first's twin: what the server's action gave the first, and
        // its place; else the action it comes from (a miss is a result of no amount)
        Known* const first = twin_of(popup, known, now);
        if (first != nullptr) {
            first->paired = true;
            known.twin = true;
            known.crit = first->crit;
            known.heal = first->heal;
            known.hits = first->hits;
            known.label = first->label;
        } else if ((number || known.miss) && g_settings.wants_actions()) {
            if (const Result* result = take_result(known.target, number ? amount : 0, now)) {
                known.crit = number && result->crit;
                known.heal = number && result->heal;
                known.hits = result->hits;
                if (g_settings.label && g_settings.sharp)
                    known.label = skill_name(result->category, result->param);
            }
        }
        if (known.crit && g_settings.crit && g_settings.crit_exclaim && !known.shown.empty())
            known.shown += "!!";
        if (known.heal && g_settings.heal_plus && !known.shown.empty())
            known.shown.insert(0, 1, '+');
        if (first != nullptr)
            known.dx = first->dx, known.dy = first->dy;
        else
            place(known, stack_slot(popup, known, now));
    }
    known.seen = now;
    return known;
}

// ---- the pictures -------------------------------------------------------------------------------------------------

// UTF-8 text as the wide text a picture is made from (a byte at a time if it isn't UTF-8)
std::wstring widen(const std::string& text)
{
    if (text.empty())
        return {};
    std::wstring out(text.size(), L'\0');
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                           out.data(), static_cast<int>(out.size()));
    if (length > 0) {
        out.resize(static_cast<std::size_t>(length));
        return out;
    }
    out.clear();
    for (const char c : text)
        out += static_cast<wchar_t>(static_cast<unsigned char>(c));
    return out;
}

void release_pictures()
{
    for (auto& [key, picture] : g_pictures)
        picture->sprite.release();
    g_pictures.clear();
    g_latest.clear();
}

// A picture's key in g_pictures, and in g_latest (any size)
std::string picture_key(const std::string& look, const std::string& text, int eighths, bool symbol)
{
    std::string key = look;
    key += symbol ? "s" : "";
    key += std::to_string(eighths);
    key += '|';
    key += text;
    return key;
}

std::string latest_key(const std::string& look, const std::string& text)
{
    return look + '|' + text;
}

void forget(std::unordered_map<std::string, std::unique_ptr<Picture>>::iterator at)
{
    Picture* const picture = at->second.get();
    if (const auto latest = g_latest.find(latest_key(picture->look, picture->text));
        latest != g_latest.end() && latest->second == picture)
        g_latest.erase(latest);
    picture->sprite.release();
    g_pictures.erase(at);
}

// The picture of `text` at `eighths` eighths of a pixel in `look`, made now if it isn't yet and there's room this frame
Picture* picture(const std::string& text, int eighths, bool symbol, const Look& look)
{
    const std::string look_key = look.key();
    std::string key = picture_key(look_key, text, eighths, symbol);
    auto found = g_pictures.find(key);
    if (found == g_pictures.end()) {
        if (g_made_this_frame >= kPicturesPerFrame || (g_made_this_frame > 0 && g_made_ms >= kMakingMs))
            return nullptr;
        ++g_made_this_frame;
        LARGE_INTEGER start{}, end{}, rate{};
        QueryPerformanceCounter(&start);
        if (g_pictures.size() >= kMostPictures) {
            auto oldest = g_pictures.begin();
            for (auto at = g_pictures.begin(); at != g_pictures.end(); ++at)
                if (at->second->used < oldest->second->used)
                    oldest = at;
            forget(oldest);
        }
        auto made = std::make_unique<Picture>();
        made->text = text;
        made->look = look_key;
        made->eighths = eighths;
        made->symbol = symbol;
        const float pixels = static_cast<float>(eighths) / kEighths;
        text::Style style;
        style.family = symbol ? std::wstring(kSymbolFont) : g_settings.family;
        style.size = std::max(1, static_cast<int>(std::lround(pixels)));
        style.exact_size = pixels;
        style.weight = g_settings.bold && !symbol ? FW_BOLD : FW_NORMAL;
        style.italic = g_settings.italic && !symbol;
        // an outline growing with the text: outline=1 is a pixel at 12 pixels high, two at 24
        style.exact_stroke = g_settings.outline > 0.0f ? std::max(1.0f, pixels / 12.0f) * g_settings.outline : 0.0f;
        style.stroke = static_cast<int>(std::ceil(style.exact_stroke));
        rgb_into(look.outline, style.stroke_color);
        style.stroke_color[3] = 0.9f;
        if (look.stops > 1) {
            style.gradient = look.stops;
            style.gradient_across = look.across;
            for (int i = 0; i < look.stops; ++i)
                rgb_into(look.fill[i], style.gradient_colors[i]);
        } else {
            rgb_into(look.fill[0], style.color);
        }
        style.background[3] = 0.0f;
        style.padding = 0;
        style.trim = true; // the box round the drawn pixels: centred on the game's point by them
        made->sprite.set(widen(text), style);
        QueryPerformanceCounter(&end);
        QueryPerformanceFrequency(&rate);
        g_made_ms += static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(rate.QuadPart);
        found = g_pictures.emplace(std::move(key), std::move(made)).first;
        g_latest[latest_key(look_key, text)] = found->second.get();
    }
    found->second->used = GetTickCount64();
    return found->second.get();
}

// The picture `text` is drawn from at `exact` pixels (its font's size), made again once it's drawn kRemake off the
// size it was made at (`held`, the popup's own, kept); the scale it's drawn at into `scale`. nullptr if there's none
// yet.
Picture* follow(const std::string& text, float exact, int& held, float& scale, bool symbol = false,
               const Look& look = Look{})
{
    Picture* drawn = nullptr;
    const std::string look_key = look.key();
    if (held > 0) {
        if (const auto found = g_pictures.find(picture_key(look_key, text, held, symbol)); found != g_pictures.end())
            drawn = found->second.get();
    }
    if (drawn == nullptr || std::fabs(exact / (static_cast<float>(drawn->eighths) / kEighths) - 1.0f) > kRemake) {
        const int eighths = std::max(8, static_cast<int>(std::lround(exact * kEighths)));
        if (Picture* fresh = picture(text, eighths, symbol, look)) {
            drawn = fresh;
            held = eighths;
        }
    }
    if (drawn == nullptr) {
        const auto latest = g_latest.find(latest_key(look_key, text));
        if (latest == g_latest.end())
            return nullptr;
        drawn = latest->second;
    }
    drawn->used = GetTickCount64();
    scale = exact / (static_cast<float>(drawn->eighths) / kEighths);
    return drawn;
}

// The text's size in the screen's pixels: the game's glyph height (`units` at `per_unit` pixels each) times the
// text's share, a critical hit's bigger, kept between damage.min and damage.max
float text_pixels(float per_unit, float units, bool crit)
{
    float pixels = per_unit * units * kTextShare * g_settings.text_size;
    if (crit && g_settings.crit)
        pixels *= g_settings.crit_size;
    if (g_settings.min > 0.0f)
        pixels = std::max(pixels, g_settings.min);
    if (g_settings.max > 0.0f)
        pixels = std::min(pixels, g_settings.max);
    return std::clamp(pixels, 4.0f, 512.0f);
}

// The colour (0xRRGGBB; kGameColour: the game's) a number is shown in: a critical hit's, a miss's, a heal's, a hit on
// you's, or a hit you deal's (kind_of)
std::uint32_t colour_for(bool crit, bool miss, bool taken, bool heal)
{
    return kind_of(g_settings, crit, miss, taken, heal).colour;
}

void use_device(void* device)
{
    if (device != g_device) {
        release_pictures();
        g_device = device;
    }
}

// ---- over the names ----------------------------------------------------------------------------------------------

void* device_of(void* renderer)
{
    return renderer != nullptr ? *reinterpret_cast<void* const*>(static_cast<const char*>(renderer) + kRendererDevice)
                               : nullptr;
}

void capture(void* device, DeviceState& state)
{
    using d3d8::call;
    namespace slot = d3d8::slot;
    for (std::size_t i = 0; i < std::size(kStates); ++i)
        call(device, slot::GetRenderState, kStates[i], &state.states[i]);
    for (std::size_t i = 0; i < std::size(kStage0); ++i)
        call(device, slot::GetTextureStageState, DWORD{0}, kStage0[i], &state.stage0[i]);
    for (std::size_t i = 0; i < std::size(kStage1); ++i)
        call(device, slot::GetTextureStageState, DWORD{1}, kStage1[i], &state.stage1[i]);
    call(device, slot::GetVertexShader, &state.vertex_shader);
    call(device, slot::GetPixelShader, &state.pixel_shader);
    call(device, slot::GetViewport, &state.viewport);
    state.texture = nullptr;
    call(device, slot::GetTexture, DWORD{0}, &state.texture);
}

void restore(void* device, const DeviceState& state)
{
    using d3d8::call;
    namespace slot = d3d8::slot;
    for (std::size_t i = 0; i < std::size(kStates); ++i)
        call(device, slot::SetRenderState, kStates[i], state.states[i]);
    for (std::size_t i = 0; i < std::size(kStage0); ++i)
        call(device, slot::SetTextureStageState, DWORD{0}, kStage0[i], state.stage0[i]);
    for (std::size_t i = 0; i < std::size(kStage1); ++i)
        call(device, slot::SetTextureStageState, DWORD{1}, kStage1[i], state.stage1[i]);
    call(device, slot::SetVertexShader, state.vertex_shader);
    call(device, slot::SetPixelShader, state.pixel_shader);
    call(device, slot::SetViewport, &state.viewport);
    call(device, slot::SetTexture, DWORD{0}, state.texture);
}

void let_go(DeviceState& state)
{
    if (state.texture != nullptr)
        d3d8::call<ULONG>(state.texture, d3d8::slot::Release);
    state.texture = nullptr;
}

void drop_kept()
{
    for (Kept& kept : g_kept)
        let_go(kept.state);
    g_kept.clear();
}

// A popup's quad: drawn now, or kept with the device's state for it to be drawn once the names are (Popup::keep)
std::uint32_t submit(void* renderer, const Vertex* quad)
{
    using d3d8::call;
    namespace slot = d3d8::slot;
    void* const device = device_of(renderer);
    void* target = nullptr;
    if (g_popup.keep && g_kept.size() < kMostKept && device != nullptr &&
        SUCCEEDED(call(device, slot::GetRenderTarget, &target)) && target != nullptr) {
        call<ULONG>(target, slot::Release);
        Kept& kept = g_kept.emplace_back();
        kept.renderer = renderer;
        kept.target = target;
        std::memcpy(kept.quad, quad, sizeof kept.quad);
        capture(device, kept.state);
        return 0;
    }
    return g_draw(renderer, nullptr, d3d8::kTriangleStrip, 2u, static_cast<const void*>(quad), sizeof(Vertex));
}

// The kept quads, as the game lays the scene onto the frame (the names drawn into it): each with the state it was kept
// with, without the depth test, into the target it was for; the device as it was after
void draw_kept()
{
    using d3d8::call;
    namespace slot = d3d8::slot;
    if (g_kept.empty())
        return;
    void* const device = device_of(g_kept.front().renderer);
    void* target = nullptr;
    if (device != nullptr && SUCCEEDED(call(device, slot::GetRenderTarget, &target)) && target != nullptr)
        call<ULONG>(target, slot::Release);
    if (target != nullptr) {
        DeviceState saved;
        capture(device, saved);
        static unsigned said = 0;
        for (const Kept& kept : g_kept) {
            if (kept.target != target || device_of(kept.renderer) != device) {
                if (said < 3) {
                    ++said;
                    log::write(L"Graphics: a number over a character was kept for another target than the scene's as "
                               L"it's laid onto the frame; left out ([graphics] damage.ontop)");
                }
                continue;
            }
            restore(device, kept.state);
            call(device, slot::SetRenderState, d3d8::rs::ZEnable, DWORD{FALSE});
            g_draw(kept.renderer, nullptr, d3d8::kTriangleStrip, 2u, static_cast<const void*>(kept.quad),
                   sizeof(Vertex));
        }
        restore(device, saved);
        let_go(saved);
    }
    drop_kept();
}

// What the game draws the glyphs with, logged the first few times it's seen different: if the text comes out wrong,
// what the draw was
void note_state(void* device, const Vertex* glyph)
{
    using d3d8::call;
    namespace slot = d3d8::slot;
    static unsigned said = 0;
    static DWORD last[14] = {};
    if (said >= 4)
        return;
    DWORD now[14] = {};
    call(device, slot::GetPixelShader, &now[0]);
    const DWORD stages[] = {d3d8::tss::ColorOp, d3d8::tss::ColorArg1, d3d8::tss::ColorArg2,
                            d3d8::tss::AlphaOp, d3d8::tss::AlphaArg1, d3d8::tss::AlphaArg2};
    for (unsigned i = 0; i < 6; ++i)
        call(device, slot::GetTextureStageState, DWORD{0}, stages[i], &now[1 + i]);
    const DWORD states[] = {d3d8::rs::AlphaBlendEnable, d3d8::rs::SrcBlend,   d3d8::rs::DestBlend, d3d8::rs::BlendOp,
                            d3d8::rs::AlphaTestEnable,  d3d8::rs::AlphaFunc, d3d8::rs::ZEnable};
    for (unsigned i = 0; i < 7; ++i)
        call(device, slot::GetRenderState, states[i], &now[7 + i]);
    if (said > 0 && std::memcmp(now, last, sizeof now) == 0)
        return;
    std::memcpy(last, now, sizeof now);
    ++said;
    log::write(L"Graphics: the numbers over characters are drawn with pixel shader %u, colour op %u(%X,%X), alpha op "
               L"%u(%X,%X), blend %u %u/%u op %u, alpha test %u func %u, depth %u; colour %08X ([graphics] "
               L"damage.font=sharp)",
               now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7], now[8], now[9], now[10], now[11], now[12],
               now[13], glyph[0].colour);
}

// What a popup's text is drawn against: the game's quad it takes its place from, and the device as it has it for it
struct Frame {
    void* renderer = nullptr;
    void* device = nullptr;
    const Vertex* glyph = nullptr;
    float kx = 1.0f, ky = 1.0f; // the game's pixels (the vertices', its target's) to the screen's
    float alpha_times = 0.0f;   // how many times the alpha op multiplies the vertices' alpha (0: it doesn't)
    float colour_times = 0.0f;  // and the colour op the vertices' colour (0: it doesn't take it)
    Look look;                  // the pictures' colours: not the default, they're drawn untinted
};

// The text's picture centred across on `cx` (the screen's pixels), or with `from_left` its left edge there, its middle
// at `y` or, with `bottom`, its bottom edge there, at `exact` pixels (its font's size), by the game's own draw with the
// device as it has it for the glyphs, only with the picture for its texture. Its size into `width` and `height`. False
// if there's no picture yet.
bool put(const Frame& frame, const std::string& text, float exact, int& held, float cx, float y, bool bottom,
         float& width, float& height, bool symbol = false, bool from_left = false)
{
    using d3d8::call;
    namespace slot = d3d8::slot;
    void* const device = frame.device;
    const Vertex* const glyph = frame.glyph;
    float scale = 1.0f;
    Picture* const drawn = follow(text, exact, held, scale, symbol, frame.look);
    if (drawn == nullptr)
        return false;
    text::Sprite& sprite = drawn->sprite;
    if (!sprite.upload(device) || sprite.texture() == nullptr || sprite.texture_width() == 0 ||
        sprite.texture_height() == 0)
        return false;
    // the game's pixel p (whole numbers at pixels' centres, as Direct3D 8 has them) is p + 0.5 of its pixels from the
    // left, times the scale on the screen
    const float w = static_cast<float>(sprite.width()), h = static_cast<float>(sprite.height());
    const float drawn_w = w * scale, drawn_h = h * scale;
    width = drawn_w;
    height = drawn_h;
    float left = from_left ? cx : cx - drawn_w * 0.5f;
    float top = bottom ? y - drawn_h : y - drawn_h * 0.5f;
    if (std::fabs(scale - 1.0f) <= kOnPixels) // at its own size: on the screen's whole pixels, sharp
        left = std::round(left), top = std::round(top);
    const float qx0 = left / frame.kx - 0.5f, qx1 = (left + drawn_w) / frame.kx - 0.5f;
    const float qy0 = top / frame.ky - 0.5f, qy1 = (top + drawn_h) / frame.ky - 0.5f;
    const float u = w / static_cast<float>(sprite.texture_width()), v = h / static_cast<float>(sprite.texture_height());

    // The game's glyphs' texels are about half opaque (kGlyphAlpha), made for the alpha op to multiply up (twice or
    // four times, by the vertices' alpha). The picture's are whole where the text is: its vertices' alpha set so its
    // fill shows as the game's would (fading as the popup does), with its smoothed edges its own, not cut hard where
    // the game's op takes it past opaque.
    // A picture in its own colours (Look) is drawn untinted: the vertices' colour one the colour op leaves a texel as it is
    const float alpha_times = frame.alpha_times;
    const bool untinted = !frame.look.plain() && frame.colour_times > 0.0f;
    const DWORD neutral = untinted ? neutral_rgb(frame.colour_times) : 0;
    const auto colour = [alpha_times, untinted, neutral](DWORD argb) {
        if (untinted)
            argb = (argb & 0xFF000000u) | neutral;
        if (alpha_times == 0.0f)
            return argb;
        const float shown = std::min(1.0f, kGlyphAlpha * static_cast<float>(argb >> 24) / 255.0f * alpha_times);
        return (argb & 0xFFFFFFu) | static_cast<DWORD>(std::lround(shown * 255.0f / alpha_times)) << 24;
    };
    const Vertex quad[4] = {{qx0, qy0, glyph[0].z, glyph[0].rhw, colour(glyph[0].colour), 0.0f, 0.0f},
                            {qx1, qy0, glyph[1].z, glyph[1].rhw, colour(glyph[1].colour), u, 0.0f},
                            {qx0, qy1, glyph[2].z, glyph[2].rhw, colour(glyph[2].colour), 0.0f, v},
                            {qx1, qy1, glyph[3].z, glyph[3].rhw, colour(glyph[3].colour), u, v}};
    // an alpha test the game may have for its glyphs would cut the picture's smoothed edges: only fully see-through
    // pixels left out; put back after
    DWORD alpha_ref = 0;
    call(device, slot::GetRenderState, d3d8::rs::AlphaRef, &alpha_ref);
    if (alpha_ref > 1)
        call(device, slot::SetRenderState, d3d8::rs::AlphaRef, DWORD{1});
    // the picture at its pixels, smoothly when it's scaled; put back after
    struct Saved {
        DWORD type, value;
    };
    Saved saved[] = {{d3d8::tss::MagFilter, 0}, {d3d8::tss::MinFilter, 0}, {d3d8::tss::MipFilter, 0},
                     {d3d8::tss::AddressU, 0},  {d3d8::tss::AddressV, 0}};
    for (Saved& s : saved)
        call(device, slot::GetTextureStageState, DWORD{0}, s.type, &s.value);
    void* texture = nullptr;
    call(device, slot::GetTexture, DWORD{0}, &texture);
    const auto stage = [&](DWORD type, DWORD value) { call(device, slot::SetTextureStageState, DWORD{0}, type, value); };
    stage(d3d8::tss::MagFilter, d3d8::kFilterLinear);
    stage(d3d8::tss::MinFilter, d3d8::kFilterLinear);
    stage(d3d8::tss::MipFilter, d3d8::kFilterNone);
    stage(d3d8::tss::AddressU, d3d8::kAddressClamp);
    stage(d3d8::tss::AddressV, d3d8::kAddressClamp);
    call(device, slot::SetTexture, DWORD{0}, sprite.texture());
    submit(frame.renderer, quad);
    call(device, slot::SetTexture, DWORD{0}, texture);
    if (texture != nullptr)
        call<ULONG>(texture, slot::Release);
    for (const Saved& s : saved)
        stage(s.type, s.value);
    if (alpha_ref > 1)
        call(device, slot::SetRenderState, d3d8::rs::AlphaRef, alpha_ref);
    return true;
}

// The popup's text where the game draws its first glyph: centred on the point the game centres the glyphs on (moved
// by where the popup was placed), at the glyphs' height (the first's), depth and colour; and above it, smaller, the
// name of the skill that did it. False (the game's glyphs) if there's no picture yet.
bool draw_text(void* renderer, const Vertex* glyph)
{
    using d3d8::call;
    namespace slot = d3d8::slot;
    Known* const known = g_popup.known;
    void* const device = *reinterpret_cast<void* const*>(static_cast<const char*>(renderer) + kRendererDevice);
    if (device == nullptr || known == nullptr || known->shown.empty() || g_popup.glyphs == 0 || g_popup.width[0] <= 0 ||
        g_popup.height[0] <= 0 || g_popup.total <= 0)
        return false;
    use_device(device);
    // the game's pixels (the vertices', its target's) to the screen's: the 3D scene's target shown at g_shown
    void* target = nullptr;
    if (FAILED(call(device, slot::GetRenderTarget, &target)) || target == nullptr)
        return false;
    d3d8::SurfaceDesc desc{};
    const HRESULT described = call(target, slot::SurfaceGetDesc, &desc);
    call<ULONG>(target, slot::Release);
    if (FAILED(described) || desc.width == 0 || desc.height == 0)
        return false;
    Frame frame;
    frame.renderer = renderer;
    frame.device = device;
    frame.glyph = glyph;
    frame.kx = g_shown_w > 0.0f ? g_shown_w / static_cast<float>(desc.width) : 1.0f;
    frame.ky = g_shown_h > 0.0f ? g_shown_h / static_cast<float>(desc.height) : 1.0f;
    float x0 = glyph[0].x, x1 = glyph[0].x, y0 = glyph[0].y, y1 = glyph[0].y;
    for (int k = 1; k < 4; ++k) {
        x0 = std::min(x0, glyph[k].x), x1 = std::max(x1, glyph[k].x);
        y0 = std::min(y0, glyph[k].y), y1 = std::max(y1, glyph[k].y);
    }
    const float across = (x1 - x0) / g_popup.width[0], down = (y1 - y0) / g_popup.height[0]; // game pixels a unit
    if (!(across > 0.0f) || !(down > 0.0f))
        return false;
    // the point: the first glyph's left side is half the glyphs' width left of it, its middle level with it; then
    // moved by the popup's place, in digit heights (the screen's pixels)
    const float middle_x = x0 + g_popup.total * 0.5f * across, middle_y = (y0 + y1) * 0.5f;
    const float digit = down * frame.ky * kDigitUnits;
    const float cx = (middle_x + 0.5f) * frame.kx + known->dx * digit;
    const float cy = (middle_y + 0.5f) * frame.ky - known->dy * digit - g_popup.lift * frame.ky;
    const float exact = text_pixels(down * frame.ky, static_cast<float>(g_popup.height[0]), known->crit);
    note_state(device, glyph);
    DWORD alpha_op = 0;
    call(device, slot::GetTextureStageState, DWORD{0}, d3d8::tss::AlphaOp, &alpha_op);
    frame.alpha_times = alpha_op == d3d8::kTopModulate4x   ? 4.0f
                        : alpha_op == d3d8::kTopModulate2x ? 2.0f
                        : alpha_op == d3d8::kTopModulate   ? 1.0f
                                                           : 0.0f;
    // its own colours (<kind>.gradient, damage.outline.color): decided at its first draw, its flat colour the one the game
    // draws it in (the hit's colour picked, or the game's: its glow's is its own), kept for its life
    frame.colour_times = colour_times(device);
    if (!known->looked) {
        if (!known->twin && g_shown_h > 0.0f) // its size as the game draws it: the preview's (preview_cell)
            note_size(digit / g_shown_h);
        known->looked = true;
        known->look = look_for(g_settings, known->crit, known->miss, known->taken, known->heal,
                               shown_rgb(glyph[0].colour, frame.colour_times));
    }
    frame.look = known->look;
    float width = 0.0f, height = 0.0f;
    if (!put(frame, known->shown, exact, known->eighths, cx, cy, false, width, height))
        return false;
    // the skill's name, a share of the number's size, its bottom edge a little above the number's top
    if (g_settings.label && !known->label.empty()) {
        const float label_pixels = std::clamp(exact * g_settings.label_size, 6.0f, 512.0f);
        float label_width = 0.0f, label_height = 0.0f;
        put(frame, known->label, label_pixels, known->label_eighths, cx, cy - height * 0.5f - label_pixels * 0.1f, true,
            label_width, label_height);
    }
    // a multi-hit round's icon: to the right of the number, a share of its size, level with its middle
    if (g_settings.multi && known->hits > 1 && !known->miss) {
        const float icon_pixels = std::clamp(exact * g_settings.multi_size, 6.0f, 512.0f);
        float icon_width = 0.0f, icon_height = 0.0f;
        const bool swords = g_settings.multi_icon == MultiIcon::Swords;
        const std::string icon = swords ? std::string(kSwords) : std::string(kTimes) + std::to_string(known->hits);
        put(frame, icon, icon_pixels, known->icon_eighths, cx + width * 0.5f + icon_pixels * kIconGap, cy, false,
            icon_width, icon_height, swords, true);
    }
    return true;
}

// ---- the game's calls ---------------------------------------------------------------------------------------------

// How far up (game pixels) the popup's number is to be moved, from its first glyph's quad: to be no lower than g_name_gap of
// its own heights below its target's name. 0 where the target's name isn't known (not drawn, or not by VanaCore) or the
// number is near enough.
float name_lift(const Vertex* glyph)
{
    float x = 0.0f, top = 0.0f, bottom = 0.0f;
    if (g_popup.target == ~0u || !nameplates::name_anchor(g_popup.target, x, top, bottom))
        return 0.0f;
    float y0 = glyph[0].y, y1 = glyph[0].y;
    for (int k = 1; k < 4; ++k)
        y0 = std::min(y0, glyph[k].y), y1 = std::max(y1, glyph[k].y);
    g_popup.name_bottom = bottom;
    const float middle = (y0 + y1) * 0.5f;
    const float lowest = bottom + g_name_gap * (y1 - y0);
    return middle > lowest ? middle - lowest : 0.0f;
}

// The glyph's quad: the text in the first's place, nothing for the rest.
std::uint32_t draw_quad(void* renderer, void* edx, unsigned prim, unsigned count, const void* vertices, unsigned stride)
{
    const bool quad = prim == d3d8::kTriangleStrip && count == 2 && stride == sizeof(Vertex) && vertices != nullptr;
    Vertex moved[4];
    if (g_popup.on && quad) {
        const int index = g_popup.draws++;
        if (index == 0)
            g_popup.lift = name_lift(static_cast<const Vertex*>(vertices));
        if (g_popup.sharp) {
            if (index == 0) {
                if (draw_text(renderer, static_cast<const Vertex*>(vertices))) {
                    g_popup.drawn = true;
                    return 0;
                }
            } else if (g_popup.drawn) {
                return 0;
            }
        }
        // the game's glyph, in the place the popup was given: dx and dy are in digit heights (this glyph's, scaled up
        // to a digit's)
        const Known* const known = g_popup.known;
        const bool shifted = known != nullptr && (known->dx != 0.0f || known->dy != 0.0f);
        if (shifted || g_popup.lift != 0.0f) {
            std::memcpy(moved, vertices, sizeof moved);
            float y0 = moved[0].y, y1 = moved[0].y;
            for (const Vertex& v : moved)
                y0 = std::min(y0, v.y), y1 = std::max(y1, v.y);
            const float digit = (y1 - y0) * kDigitUnits / (known != nullptr && known->miss ? kMissUnits : kDigitUnits);
            const float dx = shifted ? known->dx : 0.0f, dy = shifted ? known->dy : 0.0f;
            for (Vertex& v : moved) {
                v.x += dx * digit;
                v.y -= dy * digit + g_popup.lift;
            }
            vertices = moved;
        }
    }
    if (g_popup.keep && quad)
        return submit(renderer, static_cast<const Vertex*>(vertices));
    return g_draw(renderer, edx, prim, count, vertices, stride);
}

// In place of the game's call drawing each glyph's quad: drawn over everything (damage.ontop): kept till the names are
// drawn (Popup::keep), or now with its depth test off (and with it its depth write) and put back after.
std::uint32_t __fastcall draw_glyph(void* renderer, void* edx, unsigned prim, unsigned count, const void* vertices,
                                    unsigned stride)
{
    using d3d8::call;
    namespace slot = d3d8::slot;
    void* device = nullptr;
    DWORD depth = FALSE;
    if (g_popup.probe && !g_popup.probed && prim == d3d8::kTriangleStrip && count == 2 && stride == sizeof(Vertex) &&
        vertices != nullptr) {
        const Vertex* const corners = static_cast<const Vertex*>(vertices);
        g_popup.probe_x = (corners[0].x + corners[3].x) * 0.5f;
        g_popup.probe_y = (corners[0].y + corners[3].y) * 0.5f;
        g_popup.probe_h = std::fabs(corners[3].y - corners[0].y);
        g_popup.probed = true;
    }
    if (g_popup.on_top && !g_popup.keep && renderer != nullptr) {
        device = *reinterpret_cast<void* const*>(static_cast<const char*>(renderer) + kRendererDevice);
        if (device != nullptr && SUCCEEDED(call(device, slot::GetRenderState, d3d8::rs::ZEnable, &depth)) &&
            depth != FALSE)
            call(device, slot::SetRenderState, d3d8::rs::ZEnable, DWORD{FALSE});
        else
            device = nullptr;
    }
    const std::uint32_t drawn = draw_quad(renderer, edx, prim, count, vertices, stride);
    if (device != nullptr)
        call(device, slot::SetRenderState, d3d8::rs::ZEnable, depth);
    return drawn;
}

// In place of the game's call for each glyph's sprite: its size noted (the layout's).
bool __fastcall glyph(void* font, void* edx, unsigned code, float* uv, short* size, short* x, short* y)
{
    const bool ok = g_glyph(font, edx, code, uv, size, x, y);
    if (g_popup.on && g_popup.glyphs < kMostGlyphs) {
        g_popup.width[g_popup.glyphs] = ok ? size[0] : 0;
        g_popup.height[g_popup.glyphs] = ok ? size[1] : 0;
        g_popup.total += ok ? size[0] : 0;
        ++g_popup.glyphs;
    }
    return ok;
}

// The popup's target index in the game's handle (0xFFFFFFFF: none)
unsigned target_index(std::uintptr_t popup)
{
    std::uintptr_t handle = *reinterpret_cast<const std::uintptr_t*>(popup + kPopupTarget);
    if (handle == 0)
        return ~0u;
    handle = *reinterpret_cast<const std::uintptr_t*>(handle);
    return handle != 0 ? *reinterpret_cast<const std::uint16_t*>(handle + kHandleIndex) : ~0u;
}

// The projection the renderer holds now (its _11 and _22 are the floats 0 and 5), and the scene's; nullptr while either isn't
// found
float* renderer_projection()
{
    if (g_renderer_at == 0)
        return nullptr;
    const std::uintptr_t renderer = *reinterpret_cast<const std::uintptr_t*>(g_renderer_at);
    return renderer != 0 ? reinterpret_cast<float*>(renderer + kRendererProjection) : nullptr;
}

const float* scene_projection()
{
    if (g_scene_at == 0)
        return nullptr;
    const std::uintptr_t scene = *reinterpret_cast<const std::uintptr_t*>(g_scene_at);
    return scene != 0 ? reinterpret_cast<const float*>(scene + kSceneProjection) : nullptr;
}

// Whether the log hasn't said this popup yet. A popup is laid out every frame it lives, so it comes back for a couple of
// seconds; one that wasn't laid out for a while is another popup at the same address.
bool new_popup(std::uintptr_t popup, ULONGLONG now)
{
    constexpr int kSlots = 16;
    constexpr ULONGLONG kGap = 400;
    static std::uintptr_t seen[kSlots] = {};
    static ULONGLONG last[kSlots] = {};
    int free_slot = 0;
    for (int k = 0; k < kSlots; ++k) {
        if (seen[k] == popup && now - last[k] < kGap) {
            last[k] = now;
            return false;
        }
        if (last[k] < last[free_slot])
            free_slot = k;
    }
    seen[free_slot] = popup, last[free_slot] = now;
    return true;
}

// What the popup holds that places its point: its offset from the game's object (+0x54), its scale (through +0x108), its
// parent, and the words round them. Said once per popup, to tell the popups of a target that gets its numbers over its
// head from one that gets them at its feet.
void log_popup_fields(std::uintptr_t popup)
{
    std::wstring line;
    wchar_t part[96];
    const auto add = [&](const wchar_t* format, auto... values) {
        swprintf(part, sizeof part / sizeof *part, format, values...);
        line += part;
    };
    float head[8] = {};
    peek(popup + 0x50, head, sizeof head);
    add(L"offset %.3f %.3f %.3f (+50..6F:", head[1], head[2], head[3]);
    for (const float value : head)
        add(L" %g", value);
    std::uint32_t words[19] = {};
    peek(popup + 0xF8, words, sizeof words);
    add(L"); +F8..143:");
    for (const std::uint32_t word : words)
        add(L" %X", word);
    float scale[3] = {};
    if (words[4] != 0 && peek(words[4] + 0x40, scale, sizeof scale))
        add(L"; scale %.3f %.3f %.3f", scale[0], scale[1], scale[2]);
    float life[2] = {}; // its own life: left, of (kPopupLeft, kPopupLife)
    if (peek(popup + kPopupLeft, life, sizeof life))
        add(L"; life %.1f of %.1f", life[0], life[1]);
    const std::uintptr_t handle = words[2] != 0 ? words[2] : words[1];
    std::uintptr_t parent = 0;
    std::uint32_t parent_words[8] = {};
    if (peek(handle, &parent, sizeof parent) && peek(parent + 0xA0, parent_words, sizeof parent_words)) {
        add(L"; parent %X +A0..BF:", static_cast<unsigned>(parent));
        for (const std::uint32_t word : parent_words)
            add(L" %X", word);
    }
    log::write(L"Graphics: popup fields: %ls ([graphics] damage.*)", line.c_str());
}

// The game's layout of a popup's text, with its point projected as the scene is: the renderer's projection holds the
// scene's _11 and _22 (the focal terms) for the call, put back after. Where the two already agree this changes nothing. The
// first popups are said in the log with where the game put them, so a number off its target can be told apart from a
// projection that was.
void lay_out(const void* at, const char* text, std::uint32_t colour, const void* vec, std::uint32_t scale)
{
    float* const held = renderer_projection();
    const float* const scene = scene_projection();
    const float game_11 = held != nullptr ? held[0] : 0.0f, game_22 = held != nullptr ? held[5] : 0.0f;
    const bool differs = held != nullptr && scene != nullptr && scene[0] > 0.0f && scene[5] > 0.0f &&
                         (std::fabs(game_11 / scene[0] - 1.0f) > kProjectionAgree ||
                          std::fabs(game_22 / scene[5] - 1.0f) > kProjectionAgree);
    const bool probing = g_probed < kProbeLines && at != nullptr && text != nullptr &&
                         new_popup(reinterpret_cast<std::uintptr_t>(text) - kPopupText, GetTickCount64());
    float point[3] = {};
    if (probing) {
        std::memcpy(point, at, sizeof point);
        g_popup.probe = true;
        g_popup.probed = false;
    }
    if (differs) {
        held[0] = scene[0];
        held[5] = scene[5];
    }
    // The game hangs a number from a point of its target's model that some models have at their feet (Om'hpemde's, even
    // under them), where a close camera has it at the screen's bottom: kept at least a character's height above the feet,
    // or the hitbox's size when that is more. The height grows upward, so the lowest point is the greatest y.
    float lifted[3] = {};
    const void* placed = at;
    bool lift = false;
    if (at != nullptr && text != nullptr) {
        game::Entity target;
        const unsigned index = target_index(reinterpret_cast<std::uintptr_t>(text) - kPopupText);
        if (g_popup.on)
            g_popup.target = index;
        if (index != ~0u && game::entity(index, target)) {
            std::memcpy(lifted, at, sizeof lifted);
            const float lowest = target.z - std::max(kMinPopupHeight, target.hitbox_size);
            if (lifted[1] > lowest) {
                lifted[1] = lowest;
                placed = lifted;
                lift = true;
            }
        }
    }
    g_layout(placed, text, colour, vec, scale);
    if (differs) {
        held[0] = game_11;
        held[5] = game_22;
    }
    if (!probing)
        return;
    g_popup.probe = false;
    ++g_probed;
    game::Entity entity;
    const unsigned index = target_index(reinterpret_cast<std::uintptr_t>(text) - kPopupText);
    const bool known = index != ~0u && game::entity(index, entity);
    log::write(L"Graphics: popup \"%hs\" on %hs (target %u at %.1f %.1f %.1f, model size %.1f): the game's point %.2f %.2f "
               L"%.2f, first glyph at %.0f %.0f (scene shown %.0f x %.0f); projection %.4f %.4f, the scene's %.4f %.4f%ls%ls "
               L"([graphics] damage.*)",
               text, known ? entity.name.c_str() : "?", index, known ? entity.x : 0.0f, known ? entity.y : 0.0f,
               known ? entity.z : 0.0f, known ? entity.model_size : 0.0f, point[0], point[1], point[2],
               g_popup.probed ? g_popup.probe_x : -1.0f, g_popup.probed ? g_popup.probe_y : -1.0f, g_shown_w, g_shown_h,
               game_11, game_22, scene != nullptr ? scene[0] : 0.0f, scene != nullptr ? scene[5] : 0.0f,
               differs ? L"; laid out with the scene's" : L"", lift ? L"; lifted to the target's height" : L"");
    float name_x = 0.0f, name_top = 0.0f, name_bottom = 0.0f;
    const bool named = index != ~0u && nameplates::name_anchor(index, name_x, name_top, name_bottom);
    if (named && g_popup.probed && g_popup.probe_h > 0.0f)
        log::write(L"Graphics: popup name: %hs's name drawn at x %.0f, y %.0f to %.0f; the game's number at x %.0f, y %.0f is "
                   L"%.1f of its heights (%.0f px) below the name's bottom; moved up %.0f px (kept within %.1f heights) "
                   L"([graphics] damage.*)",
                   known ? entity.name.c_str() : "?", name_x, name_top, name_bottom, g_popup.probe_x, g_popup.probe_y,
                   (g_popup.probe_y - name_bottom) / g_popup.probe_h, g_popup.probe_h, g_popup.lift, g_name_gap);
    else
        log::write(L"Graphics: popup name: %hs's name isn't known (%ls) ([graphics] damage.*)", known ? entity.name.c_str() : "?",
                   g_popup.on ? L"not drawn by VanaCore lately" : L"the numbers' hook is off");
    log_popup_fields(reinterpret_cast<std::uintptr_t>(text) - kPopupText);
}

// In place of the game's call laying out a popup's text: a critical hit's colour, the text as VanaCore's, and over
// everything.
void __cdecl layout(const void* at, const char* text, std::uint32_t colour, const void* vec, std::uint32_t scale)
{
    if (text == nullptr || !g_settings.wants_hook()) {
        lay_out(at, text, colour, vec, scale);
        return;
    }
    const ULONGLONG now = GetTickCount64();
    Known& known = know(reinterpret_cast<std::uintptr_t>(text) - kPopupText, text, now);
    if (const std::uint32_t picked = colour_for(known.crit, known.miss, known.taken, known.heal);
        picked != kGameColour)
        colour = (colour & 0xFF000000) | game_colour(picked);
    const bool sharp = g_settings.sharp && g_sharp_ready && !known.shown.empty();
    const bool on_top = g_settings.on_top && g_sharp_ready;
    const bool moved = g_sharp_ready && (known.dx != 0.0f || known.dy != 0.0f);
    // a number kept near its target's name is moved at its quad's draw, when VanaCore is drawing the names
    const bool named = g_sharp_ready && nameplates::anchoring();
    if (sharp || on_top || moved || named) {
        g_popup = Popup{};
        g_popup.on = sharp || moved || named;
        g_popup.sharp = sharp;
        g_popup.on_top = on_top;
        g_popup.keep = on_top && g_keep;
        g_popup.known = &known;
        lay_out(at, text, colour, vec, scale);
        g_popup.on = g_popup.sharp = g_popup.on_top = g_popup.keep = false;
        return;
    }
    lay_out(at, text, colour, vec, scale);
}

// In place of the game's call as it lays the scene onto the frame: the numbers kept, drawn first.
std::uint32_t __fastcall laid(void* renderer, void* edx, std::uint32_t a, std::uint32_t b)
{
    draw_kept();
    return g_laid(renderer, edx, a, b);
}

bool install(HMODULE game)
{
    const std::uintptr_t site = scan::find(game, kPopupCall);
    if (site == 0) {
        log::write(L"Graphics: couldn't find where the game draws the numbers over characters as they're hit; they "
                   L"stay as it draws them ([graphics] damage.*)");
        return false;
    }
    const std::uintptr_t call = site + kPopupCallAt;
    const std::uintptr_t routine = target_of(call);
    const std::uintptr_t glyph_site = scan::find(routine, kLayoutWithin, kGlyphCall);
    const std::uintptr_t draw_site = scan::find(routine, kLayoutWithin, kDrawCall);
    void* theirs = nullptr;
    // the renderer's (the draw call's operand: mov ecx,[renderer]) and the scene's globals, for the projection a popup is laid
    // out with
    if (draw_site != 0)
        std::memcpy(&g_renderer_at, reinterpret_cast<const void*>(draw_site + 2), sizeof g_renderer_at);
    if (const std::uintptr_t getter = scan::find(game, kSceneGetter); getter != 0)
        std::memcpy(&g_scene_at, reinterpret_cast<const void*>(getter + 1), sizeof g_scene_at);
    if (g_renderer_at == 0 || g_scene_at == 0)
        log::write(L"Graphics: couldn't find the renderer's or the scene's projection; numbers over characters keep the "
                   L"projection the game lays them out with ([graphics] damage.*)");
    // the draw's and the glyphs' calls first: they do as the game's till the layout's call is VanaCore's too
    if (glyph_site != 0 && draw_site != 0) {
        bool ok = redirect(draw_site + kDrawCallAt, reinterpret_cast<const void*>(&draw_glyph), theirs);
        if (ok)
            g_draw = reinterpret_cast<DrawFn>(theirs);
        ok = ok && redirect(glyph_site + kGlyphCallAt, reinterpret_cast<const void*>(&glyph), theirs);
        if (ok)
            g_glyph = reinterpret_cast<GlyphFn>(theirs);
        g_sharp_ready = ok;
    }
    if (!g_sharp_ready)
        log::write(L"Graphics: couldn't find where the game draws each glyph of the numbers over characters; they "
                   L"keep its glyphs and its depth ([graphics] damage.font=sharp, damage.ontop)");
    if (!redirect(call, reinterpret_cast<const void*>(&layout), theirs)) {
        log::write(L"Graphics: couldn't change how the game draws the numbers over characters ([graphics] damage.*)");
        return false;
    }
    g_layout = reinterpret_cast<LayoutFn>(theirs);
    log::write(L"Graphics: the numbers over characters as they're hit are laid out at FFXiMain.dll+0x%X, each glyph "
               L"drawn at %ls ([graphics] damage.*)",
               static_cast<unsigned>(call - reinterpret_cast<std::uintptr_t>(game)),
               g_sharp_ready ? L"its own call" : L"(not found)");
    // over everything, the names too: kept till the game lays the scene onto the frame
    if (g_sharp_ready) {
        const std::uintptr_t laid_site = scan::find(game, kLaidCall);
        if (laid_site != 0 && redirect(laid_site + kLaidCallAt, reinterpret_cast<const void*>(&laid), theirs)) {
            g_laid = reinterpret_cast<LaidFn>(theirs);
            g_keep = true;
            log::write(L"Graphics: the numbers over everything are drawn after the names, as the game lays the scene "
                       L"onto the frame at FFXiMain.dll+0x%X ([graphics] damage.ontop)",
                       static_cast<unsigned>(laid_site + kLaidCallAt - reinterpret_cast<std::uintptr_t>(game)));
        } else {
            log::write(L"Graphics: couldn't find where the game lays the scene onto the frame; the numbers over "
                       L"everything are drawn under the names ([graphics] damage.ontop)");
        }
    }
    return true;
}

// Now and then: the pictures not drawn lately, the popups gone, the results too old to have one
void sweep(ULONGLONG now)
{
    while (!g_results.empty() && now - g_results.front().at > kResultKept)
        g_results.pop_front();
    if (now - g_swept_at < 1000)
        return;
    g_swept_at = now;
    for (auto at = g_pictures.begin(); at != g_pictures.end();) {
        const auto next = std::next(at);
        if (now - at->second->used > kPictureKept)
            forget(at);
        at = next;
    }
    std::erase_if(g_known, [now](const auto& known) { return now - known.second.seen > kPopupGone; });
    std::erase_if(g_stretched, [now](const auto& stretched) { return now - stretched.second.at > kStretchKept; });
}

} // namespace

void apply(const Settings& settings)
{
    g_made_this_frame = 0;
    g_made_ms = 0.0;
    // the frame's done (this is its Present): quads still kept weren't drawn, the scene not laid onto the frame after
    // them; a few frames of that and they're drawn at once again
    if (!g_kept.empty()) {
        drop_kept();
        if (++g_unlaid >= kUnlaidFrames && g_keep) {
            g_keep = false;
            log::write(L"Graphics: the numbers over everything weren't drawn after the names for %u frames; drawn "
                       L"under them from now ([graphics] damage.ontop)",
                       kUnlaidFrames);
        }
    } else {
        g_unlaid = 0;
    }
    // a change of the pictures' look lets them all go (the colours' too: the old ones would stay till they're swept)
    wchar_t look[160];
    const unsigned graded = (settings.hit_gradient ? 1u : 0u) | (settings.miss_gradient ? 2u : 0u) |
                            (settings.taken_gradient ? 4u : 0u) | (settings.heal_gradient ? 8u : 0u) |
                            (settings.crit_gradient ? 16u : 0u) | (settings.gradient_across ? 32u : 0u);
    swprintf_s(look, L"|%d%d|%g|%d|%02X|%06X|%06X|%06X|%06X|%06X|%06X", settings.bold ? 1 : 0, settings.italic ? 1 : 0,
               settings.outline, settings.baked() ? 1 : 0, graded, settings.hit_second, settings.miss_second,
               settings.taken_second, settings.heal_second, settings.crit_second, settings.outline_colour);
    std::wstring style = settings.family + look;
    if (style != g_style) {
        release_pictures();
        g_style = std::move(style);
    }
    g_settings = settings;
    const ULONGLONG now = GetTickCount64();
    sweep(now);
    if (!settings.sharp && !settings.preview && g_tests.empty() && !g_pictures.empty())
        release_pictures();
    if (settings.sharp || settings.preview) {
        std::uint32_t w = 0, h = 0;
        if (!vanacore_gfx_scene_shown(&w, &h)) {
            RECT client{};
            if (window::handle() != nullptr && GetClientRect(window::handle(), &client))
                w = static_cast<std::uint32_t>(client.right), h = static_cast<std::uint32_t>(client.bottom);
        }
        g_shown_w = static_cast<float>(w), g_shown_h = static_cast<float>(h);
    }
    if (settings.wants_hook() && !g_tried) {
        if (const HMODULE game = GetModuleHandleW(L"FFXiMain.dll")) {
            g_tried = true;
            g_hooked = install(game);
        }
    }
    // how long they stay: read by the game's update once it's pointed at these (a change takes the numbers showing too)
    const float life = std::clamp(settings.duration, kMinDuration, kMaxDuration) * 60.0f;
    g_life = life;
    g_life_share = 1.0f / life;
    if (settings.duration != kGameDuration && !g_life_tried) {
        if (const HMODULE game = GetModuleHandleW(L"FFXiMain.dll")) {
            g_life_tried = true;
            g_life_set = install_life(game);
        }
    }
    wchar_t state[512];
    describe(settings, g_keep, state);
    if (g_hooked && g_state != state) {
        g_state = state;
        log::write(L"Graphics: numbers over characters as they're hit: %ls ([graphics] damage.*)", state);
    }
}

void on_received(const std::uint8_t* buffer, std::size_t size)
{
    if (buffer == nullptr || size <= kHeader || !g_settings.wants_actions() || !g_hooked)
        return;
    const ULONGLONG now = GetTickCount64();
    for (std::size_t at = kHeader; at + 4 <= size;) {
        const std::uint8_t* packet = buffer + at;
        std::uint16_t head;
        std::memcpy(&head, packet, sizeof(head));
        const std::size_t length = static_cast<std::size_t>(head >> 9) * 4;
        if (length < 4 || at + length > size)
            break;
        if ((head & 0x1FF) == kAction)
            on_action(packet, length, now);
        at += length;
    }
}

// The preview's samples across the middle of the screen
static void draw_samples(void* device, unsigned width, unsigned height)
{
    use_device(device);
    struct Sample {
        const char* game;      // as the game prints it
        bool crit;
        bool taken;            // a hit on you
        bool heal;             // HP recovered
        int hits;              // the results of its round (0: not one of several)
        const char* label;     // the skill that did it
        std::uint32_t rgb;     // as the game shows it
    };
    const Sample samples[] = {{"1337", false, false, false, 0, "Savage Blade", 0xFF8282}, // on a monster you've claimed
                              {":", false, false, false, 0, nullptr, 0x9CFF9C},           // a miss
                              {"2048", true, false, false, 0, nullptr, 0xFF8282},         // a critical hit
                              {"412", false, false, false, 2, nullptr, 0xFF8282},         // a hit of a multi-hit round
                              {"86", false, true, false, 0, nullptr, 0xFFFFFF},           // a hit you take
                              {"530", false, false, true, 0, "Cure III", 0xB0FFB0}};      // HP recovered
    const float cell = preview_cell(height); // the screen's pixels a unit
    struct Placed {
        Picture* picture;
        Picture* label;
        Picture* icon;
        float scale;
        float label_scale;
        float icon_scale;
        DWORD argb;
    };
    Placed placed[std::size(samples)] = {};
    float total = 0.0f, gap = 0.0f;
    int count = 0;
    for (const Sample& sample : samples) {
        bool miss = false;
        std::string shown = shown_for(sample.game, miss);
        if (sample.crit && g_settings.crit && g_settings.crit_exclaim)
            shown += "!!";
        if (sample.heal && g_settings.heal_plus)
            shown.insert(0, 1, '+');
        const float exact = text_pixels(cell, miss ? kMissUnits : kDigitUnits, sample.crit);
        gap = std::max(gap, exact * 0.6f);
        std::uint32_t rgb = sample.rgb;
        if (const std::uint32_t picked = colour_for(sample.crit, miss, sample.taken, sample.heal); picked != kGameColour)
            rgb = picked;
        const Look look = look_for(g_settings, sample.crit, miss, sample.taken, sample.heal, rgb); // its own colours: drawn untinted
        int held = 0;
        float scale = 1.0f;
        Picture* const picture = follow(shown, exact, held, scale, false, look);
        if (picture == nullptr || !picture->sprite.upload(device) || picture->sprite.texture() == nullptr)
            continue;
        Placed& put = placed[count];
        put = {picture, nullptr, nullptr, scale, 1.0f, 1.0f, 0};
        if (g_settings.label && sample.label != nullptr) {
            const float label_pixels = std::clamp(exact * g_settings.label_size, 6.0f, 512.0f);
            int label_held = 0;
            float label_scale = 1.0f;
            if (Picture* const label = follow(sample.label, label_pixels, label_held, label_scale, false, look);
                label != nullptr && label->sprite.upload(device) && label->sprite.texture() != nullptr) {
                put.label = label;
                put.label_scale = label_scale;
            }
        }
        float room = picture->sprite.width() * scale;
        if (g_settings.multi && sample.hits > 1 && !miss) {
            const float icon_pixels = std::clamp(exact * g_settings.multi_size, 6.0f, 512.0f);
            const bool swords = g_settings.multi_icon == MultiIcon::Swords;
            const std::string icon_text = swords ? std::string(kSwords) : std::string(kTimes) + std::to_string(sample.hits);
            int icon_held = 0;
            float icon_scale = 1.0f;
            if (Picture* const icon = follow(icon_text, icon_pixels, icon_held, icon_scale, swords, look);
                icon != nullptr && icon->sprite.upload(device) && icon->sprite.texture() != nullptr) {
                put.icon = icon;
                put.icon_scale = icon_scale;
                room += icon_pixels * kIconGap + icon->sprite.width() * icon_scale;
            }
        }
        put.argb = 0xFF000000u | (look.plain() ? rgb : 0xFFFFFFu);
        ++count;
        total += room;
    }
    if (count == 0)
        return;
    total += gap * (count - 1);
    // where the numbers are placed (damage.offset.*), as they are for a hit
    const float digit = cell * kDigitUnits;
    float x = (static_cast<float>(width) - total) * 0.5f + g_settings.offset_x * digit;
    const float middle = static_cast<float>(height) * kPreviewRow - g_settings.offset_y * digit;
    const auto draw = [&](Picture* picture, float scale, float left, float centre, DWORD argb) {
        text::Sprite& sprite = picture->sprite;
        const float w = sprite.width() * scale, h = sprite.height() * scale;
        text::draw_texture(device, sprite.texture(), std::round(left), std::round(centre - h * 0.5f), w, h, 0.0f, 0.0f,
                           static_cast<float>(sprite.width()) / static_cast<float>(sprite.texture_width()),
                           static_cast<float>(sprite.height()) / static_cast<float>(sprite.texture_height()), argb);
        return w;
    };
    for (int i = 0; i < count; ++i) {
        const float w = draw(placed[i].picture, placed[i].scale, x, middle, placed[i].argb);
        if (placed[i].label != nullptr) {
            text::Sprite& name = placed[i].label->sprite;
            const float lw = name.width() * placed[i].label_scale, lh = name.height() * placed[i].label_scale;
            const float h = placed[i].picture->sprite.height() * placed[i].scale;
            draw(placed[i].label, placed[i].label_scale, x + (w - lw) * 0.5f, middle - h * 0.5f - lh * 0.5f - h * 0.05f,
                 placed[i].argb);
        }
        float room = w;
        if (placed[i].icon != nullptr) {
            const float icon_gap = placed[i].icon->sprite.height() * placed[i].icon_scale * kIconGap;
            room += icon_gap + draw(placed[i].icon, placed[i].icon_scale, x + w + icon_gap, middle, placed[i].argb);
        }
        x += room + gap;
    }
}

// One picture drawn at `left`, its middle at `centre`, in `argb`; its width
static float draw_picture(void* device, Picture* picture, float scale, float left, float centre, DWORD argb)
{
    text::Sprite& sprite = picture->sprite;
    const float w = sprite.width() * scale, h = sprite.height() * scale;
    text::draw_texture(device, sprite.texture(), left, centre - h * 0.5f, w, h, 0.0f, 0.0f,
                       static_cast<float>(sprite.width()) / static_cast<float>(sprite.texture_width()),
                       static_cast<float>(sprite.height()) / static_cast<float>(sprite.texture_height()), argb);
    return w;
}

// The test hits: each appears at its time, placed by the settings, then floats up and fades over its life
static void draw_tests(void* device, unsigned width, unsigned height)
{
    use_device(device);
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG life = test_life();
    const float cell = preview_cell(height); // the screen's pixels a unit
    const float digit = cell * kDigitUnits;
    for (Fake& fake : g_tests) {
        Known& known = fake.known;
        if (now < fake.start)
            continue;
        if (!fake.placed) {
            fake.placed = true;
            known.born = known.seen = fake.start;
            int slot = 0; // the tests made a moment before that are still showing
            for (const Fake& other : g_tests)
                if (&other != &fake && other.placed && other.start <= fake.start && fake.start - other.start < kBurst &&
                    now - other.start < life)
                    ++slot;
            place(known, slot);
        }
        const float age = static_cast<float>(now - fake.start) / static_cast<float>(life);
        const float fade = age < 0.6f ? 1.0f : std::max(0.0f, 1.0f - (age - 0.6f) / 0.4f);
        const float rise = 1.0f - (1.0f - age) * (1.0f - age);
        const float exact = text_pixels(cell, known.miss ? kMissUnits : kDigitUnits, known.crit);
        std::uint32_t rgb = known.miss ? 0x9CFF9C : known.taken ? 0xFFFFFF : 0xFF8282; // as the game's
        if (const std::uint32_t picked = colour_for(known.crit, known.miss, known.taken, known.heal);
            picked != kGameColour)
            rgb = picked;
        if (!known.looked) { // its own colours (<kind>.gradient, damage.outline.color): drawn untinted
            known.looked = true;
            known.look = look_for(g_settings, known.crit, known.miss, known.taken, known.heal, rgb);
        }
        const Look& look = known.look;
        float scale = 1.0f;
        Picture* const number = follow(known.shown, exact, known.eighths, scale, false, look);
        if (number == nullptr || !number->sprite.upload(device) || number->sprite.texture() == nullptr)
            continue;
        const DWORD argb = static_cast<DWORD>(std::lround(fade * 255.0f)) << 24 | (look.plain() ? rgb : 0xFFFFFFu);
        const float cx = static_cast<float>(width) * 0.5f + known.dx * digit;
        const float cy = static_cast<float>(height) * kTestRow - known.dy * digit - rise * kTestRise * digit;
        const float w = number->sprite.width() * scale, h = number->sprite.height() * scale;
        draw_picture(device, number, scale, std::round(cx - w * 0.5f), std::round(cy), argb);
        if (g_settings.label && !known.label.empty()) {
            const float pixels = std::clamp(exact * g_settings.label_size, 6.0f, 512.0f);
            float label_scale = 1.0f;
            if (Picture* const label = follow(known.label, pixels, known.label_eighths, label_scale, false, look);
                label != nullptr && label->sprite.upload(device) && label->sprite.texture() != nullptr) {
                const float lw = label->sprite.width() * label_scale, lh = label->sprite.height() * label_scale;
                draw_picture(device, label, label_scale, std::round(cx - lw * 0.5f),
                             std::round(cy - h * 0.5f - lh * 0.5f - pixels * 0.1f), argb);
            }
        }
        if (g_settings.multi && known.hits > 1 && !known.miss) {
            const float pixels = std::clamp(exact * g_settings.multi_size, 6.0f, 512.0f);
            const bool swords = g_settings.multi_icon == MultiIcon::Swords;
            const std::string text = swords ? std::string(kSwords) : std::string(kTimes) + std::to_string(known.hits);
            float icon_scale = 1.0f;
            if (Picture* const icon = follow(text, pixels, known.icon_eighths, icon_scale, swords, look);
                icon != nullptr && icon->sprite.upload(device) && icon->sprite.texture() != nullptr)
                draw_picture(device, icon, icon_scale, std::round(cx + w * 0.5f + pixels * kIconGap), std::round(cy), argb);
        }
    }
    std::erase_if(g_tests, [now, life](const Fake& fake) { return now - fake.start > life && now >= fake.start; });
}

void draw_preview(void* device, unsigned width, unsigned height)
{
    if (device == nullptr || width == 0 || height == 0)
        return;
    if (g_settings.preview && g_settings.sharp)
        draw_samples(device, width, height);
    if (!g_tests.empty())
        draw_tests(device, width, height);
}

void test(TestKind kind)
{
    const ULONGLONG now = GetTickCount64();
    if (g_tests.size() > 64)
        return;
    const auto add = [now](ULONGLONG delay, bool miss, bool crit, bool taken, int hits, bool skill) {
        Fake fake;
        Known& known = fake.known;
        std::uint32_t amount = 100 + static_cast<std::uint32_t>(random_unit() * 800.0f);
        if (crit)
            amount *= 2;
        known.game = miss ? ":" : std::to_string(amount);
        known.shown = shown_for(known.game, known.miss);
        known.crit = crit;
        known.taken = taken;
        known.hits = hits;
        known.target = 1; // the player's, as all of these are
        if (skill && g_settings.label)
            known.label = "Savage Blade";
        if (crit && g_settings.crit && g_settings.crit_exclaim && !known.shown.empty())
            known.shown += "!!";
        fake.start = now + delay;
        g_tests.push_back(std::move(fake));
    };
    switch (kind) {
    case TestKind::Hit:
        add(0, false, false, false, 0, true);
        break;
    case TestKind::Round: // three hits of one weapon skill, at once
        for (int i = 0; i < 3; ++i)
            add(0, false, false, false, 3, true);
        break;
    case TestKind::Barrage: // a fight: hits, a critical hit, a miss, a hit taken, a double attack
        add(0, false, false, false, 0, false);
        add(300, false, false, false, 0, false);
        add(600, false, true, false, 0, false);
        add(900, true, false, false, 0, false);
        add(1100, false, false, false, 2, false);
        add(1100, false, false, false, 2, false);
        add(1500, false, false, true, 0, false);
        add(1700, false, false, false, 0, false);
        add(1900, false, true, false, 0, false);
        break;
    }
}

} // namespace vanacore::damagetext
