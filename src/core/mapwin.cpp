#include "mapwin.hpp"

#include <windows.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "d3d8.hpp"
#include "gui.hpp"
#include "hideui.hpp"
#include "imports.hpp"
#include "ini.hpp"
#include "input.hpp"
#include "log.hpp"
#include "outline.hpp"
#include "scan.hpp"
#include "text.hpp"
#include "ui.hpp"
#include "uispace.hpp"
#include "window.hpp"

namespace vanacore::mapwin {
namespace {

// The map (YkWndMapMain in the PS2 client's names, a YkWndImage) is a picture drawn straight onto the interface at
// (-pan x scale), the scale the interface's width / 512 (the map is 512 across, so it fills the width and, on a screen
// less wide than tall times 512/448, scrolls up and down). Nothing of it is a window's place: the pan's limits and the
// zoom's range are worked out from the interface's whole area. VanaCore keeps the game's own panning, zoom and markers
// and gives them a box: the limits worked out for the box (and moved by its place), the zoom's range for the box, and
// the map's drawing kept inside it with the device's viewport.
//
// CalcLimit (FUN_101EC7A0, __thiscall, no arguments): the pan's and the cursor's limits from the area, the picture's size
// and the scale. sub esp,8; push ebx; mov ebx,[ecx+8Ch]; test ebx,ebx; je ... (the 10 bytes before je, whole
// instructions, are the detour's prologue)
constexpr char kCalcLimit[] = "83 EC 08 53 8B 99 8C 00 00 00 85 DB 0F 84";
constexpr std::size_t kCalcPrologue = 10;
// The map's draw (FUN_101F90C0, its vtable's slot 4): push esi; mov esi,ecx; call <the picture and its markers, YkWndImage's
// draw>; mov ecx,esi; call <the grid square in the title bar ("(I-6)")>; mov ecx,esi; pop esi; jmp <the Metalworks' extra
// markers>
constexpr char kMapDraw[] = "56 8B F1 E8 ?? ?? ?? ?? 8B CE E8 ?? ?? ?? ?? 8B CE 5E E9";
constexpr std::ptrdiff_t kImageCall = 3, kMarksJump = 0x12;
constexpr std::size_t kVtableDraw = 4;
// YkWndImage's draw (FUN_101ED330): push ecx; mov eax,[the renderer's global]; push esi; push edi; push 2; mov eax,[eax+0Ch]
// (the device) ... Its pictures are drawn by the shape draw (FUN_1011F880, __thiscall shape: x, y, scale x, scale y, colour,
// 0, blend, 0, ret 20h): the backing at +0x134, the map at +0x215, colour 0xA0404040 and the window's own blend (+0xAC),
// or while it fades in, the colour's alpha the fade's and blend 0x44 (alpha blended)
constexpr char kImageDraw[] = "51 A1 ?? ?? ?? ?? 56 57 6A 02 8B 40 0C 8B F1 6A";
constexpr std::ptrdiff_t kRendererAt = 2, kRendererDevice = 0xC, kBackingCall = 0x134, kPictureCall = 0x215;
constexpr int kAlphaBlend = 0x44;
// The region maps (YkWndConquestMain in the PS2 client's names: Region Info > Conquest, Besieged, Campaign and
// Colonization, one window, which of them at +0x160) are YkWndImage too, their vtable's slot 4 YkWndImage's draw itself
// (their pictures and markers, the zones' names). Their open (FUN_101E60E0) sets the scale 2.3 x the area's width / 512
// and no zoom (the range 0 to 0: the game's debug mode alone has one). Their constructor (FUN_101E5710): call
// <YkWndImage's>; mov dword [esi],<the vtable>; mov [..],edi; mov byte [..],0; mov [..],edi; mov [esi+158h],edi
constexpr char kRegionCtor[] = "E8 ?? ?? ?? ?? C7 06 ?? ?? ?? ?? 89 3D ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 00 89 3D ?? ?? ?? ?? 89 BE 58 01 00 00";
constexpr std::ptrdiff_t kRegionVtableAt = 7;
constexpr float kRegionZoomRange = 4.0f; // the zoom VanaCore gives them: from filling the area's width to this many times that
// Their calc (FUN_101E7120, their vtable's slot 10, __thiscall: whether the window is the active one, ret 4) ends in
// YkWndImage's (FUN_101ED800). Their open sets +0x14 bit 0x100, the cursor the mouse's: that calc moves the cursor by the
// mouse, scrolls the map by what goes past the cursor's limits (FUN_101EC250), puts the mouse back on the cursor
// (0x101EE09C), and at a glide's end puts the cursor on its target; and bit 4, a magnet: a cursor at rest within 8 map units
// of a zone glides that zone under it (FUN_101EC140 -> SetPosAuto)
constexpr std::size_t kVtableCalc = 10;
constexpr std::uint16_t kMouseCursor = 0x100, kMagnet = 4;
// The zoom (FUN_101EC590, __thiscall: the change to the scale, ret 4): clamped to the zoom's range, and the pan moved to
// keep the point under the map's cursor where it is. push ecx; fld [esp+8]; push esi; mov esi,ecx; fadd [esi+58h] ...
constexpr char kZoom[] = "51 D9 44 24 08 56 8B F1 D8 46 58 8B 46 58 89 44 24 04 D9 56 58 D8 5E 60";
constexpr float kWheelStep = 1.15f; // a wheel notch zooms by this much
// SetPosAuto (FUN_101EE0F0, __thiscall: x, y, map units, ret 8): the pan glides until (x, y) is under the map's cursor (+0x14
// bit 8 while it does, the target at +0x2C; the window's cursor hidden meanwhile, its ctrl's +0x62). The map's open calls it
// with the player's place (+0x20, +0x22), as does the "centre" key; a region map's open with the zone it's on (your zone's,
// or its region's): push ecx; push esi; mov esi,ecx; movsx eax,[esi+7Ch] ...
constexpr char kSetPosAuto[] = "51 56 8B F1 0F BF 46 7C 0F BF 4E 7E 89 44 24 04 DB 44 24 04";
constexpr std::ptrdiff_t kStat = 0x14, kPlayer = 0x20, kTarget = 0x2C, kCtrl = 8, kCtrlCursorShown = 0x62;
constexpr std::uint16_t kGliding = 8;
// A home point crystal's teleport list (and the game's other pointers at a place) glides to the place picked: SetPosAuto
// from 0x101F7DEC, each time the pick changes. A glide whose target is this far from you (map units) isn't to you.
constexpr float kNearYou = 2.0f;
// The glide (the map's calc, 0x101EDB43) moves the pan by (target - the point under the cursor) x scale, the point under the
// cursor being cursor / scale + pan + 0.5 + the shorts at +0x6C, and stops once it's near: short of the middle, in a box
constexpr std::ptrdiff_t kGlideOffset = 0x6C;
constexpr float kFinishRate = 0.3f;  // the rest of a glide eased out at this much a draw
constexpr float kFinishNear = 0.25f; // map units: there
// the cursor's x the game's open and reset put it at, of the area's width (a region map's recentring on a zone picked too,
// 0x101E71C9)
constexpr float kGameMiddleX = 200.0f / 512.0f;
// The map's close (FUN_101F8B40, __thiscall: a flag, ret 4): saves the zoom as Config 0x8A, scale x 1000 / (the area's width
// / 512), and starts the picture's fade (CloseImageSet: +0x14 bit 0x40, the alpha +0xA6 from 0 to 255, 16 a frame, bit
// 0x20 while it does). The window closes once nothing is animating (bits 0x38) or 3 s after. mov al,[esp+4]; push esi;
// test al,al (the 7 bytes, whole instructions, are the detour's prologue); mov esi,ecx; je ...
constexpr char kClose[] = "8A 44 24 04 56 84 C0 8B F1 74 ?? 8B 46 08 66 C7 86 04 01 00 00 08 00";
constexpr std::size_t kClosePrologue = 7;
constexpr std::ptrdiff_t kAlpha = 0xA6;
constexpr std::uint16_t kClosing = 0x40, kAnimating = 0x38;
// The keys: the game reads one through GetAnalogKey (FUN_10123570, __cdecl: a level, the key's type; its tilt on the x87
// stack), 0 unless the key's kind ([kinds + type x 2]) is in the key level (a global, SetKeyLevel FUN_101232E0). The map's
// open (YkWndImage's, 0x101ECC5E; a region map's too) sets the level 0x26, so walking (types 4 and 5, kind 0x08) and the
// camera (6 and 7, kind 0x10) stop, and the map's calc (YkWndImage's, FUN_101ED800, a region map's as well) scrolls it by
// the same keys, types 0x79 and 0x7A (kind 0x04, as menus' cursors); its close sets 0xFFFF (every kind). push 79h; push 26h;
// call GetAnalogKey; fstp [esp+..]; push 7Ah; push 26h; call GetAnalogKey. GetAnalogKey: push ecx; mov eax,[esp+0Ch];
// mov edx,[the level] ...
constexpr char kPanKeys[] = "6A 79 6A 26 E8 ?? ?? ?? ?? D9 5C 24 ?? 6A 7A 6A 26 E8";
constexpr std::ptrdiff_t kPanKeyX = 4, kPanKeyY = 0x11;
constexpr char kAnalogKey[] = "51 8B 44 24 0C 8B 15";
// SetPlayer (FUN_101F75C0, __thiscall: a flag, ret 4): with 1, your marker (+0x674, its place +0x67A/+0x67C, its facing
// +0x688) from where you are now, the map's place for you (+0x20/+0x22), and the pan put on you; the map's markers of you
// (+0x98) made it. The map's open calls it, on your own zone's map only, and nothing after: the marker stays where you
// opened it. push ecx; push ebx; mov bl,[esp+0Ch]; push esi; mov esi,ecx; test bl,bl; mov byte [esp+0Bh],1; je ...;
// mov ecx,<the actors>; call <you>; test eax,eax; mov word [esi+674h],9Fh
constexpr char kSetPlayer[] =
    "51 53 8A 5C 24 0C 56 8B F1 84 DB C6 44 24 0B 01 0F 84 ?? ?? ?? ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 66 C7 86 74 06 00 00 9F 00";
constexpr std::ptrdiff_t kYouMarks = 0x98, kYouMarker = 0x674;
constexpr std::ptrdiff_t kKeyLevelAt = 7;
constexpr std::uint32_t kWalkKeys = 0x18, kAnyKey = 0xFFFF; // walking and the camera; the level with nothing open

// YkWndImage on the PC (XIClient's names): the cursor (shorts) at +0x1C, the pan (map units) +0x24, its limits +0x34
// (min x, min y, max x, max y), the cursor's +0x44 (the same), the map's offset (shorts) +0x54, the scale +0x58, the scale
// it's going to +0x5C, the zoom's range +0x60 and +0x64, the scale it opens at +0x68, the margins round the picture
// (shorts) +0x7C/+0x7E and +0x80/+0x82, its pictures +0x8C (the first picture's bounds: shorts left +0x20, right +0x22,
// top +0x24, bottom +0x26)
constexpr std::ptrdiff_t kCursor = 0x1C, kPan = 0x24, kPanLimits = 0x34, kCursorLimits = 0x44, kOffset = 0x54, kScale = 0x58,
                         kScaleTo = 0x5C, kScaleMin = 0x60, kScaleMax = 0x64, kScaleOpen = 0x68, kMarginA = 0x7C, kMarginB = 0x80,
                         kPictures = 0x8C, kBoundsX = 0x20, kBoundsY = 0x24;
constexpr float kCursorInset = 0.05f; // the cursor kept this much of the box inside its edges (past it the map scrolls)
constexpr ULONGLONG kShownFor = 250;  // ms: the map counts as open this long after it was last drawn

using Calc = void(__fastcall*)(char* self);
using Draw = void(__fastcall*)(char* self);
using Zoom = void(__fastcall*)(char* self, void* edx, float change);
using SetPosAuto = void(__fastcall*)(char* self, void* edx, float x, float y);
using Close = void(__fastcall*)(char* self, void* edx, int flag);
using AnalogKey = float(__cdecl*)(unsigned level, int type);
using SetPlayer = void(__fastcall*)(char* self, void* edx, int track);
using RegionCalc = void(__fastcall*)(char* self, void* edx, int active);
using Shape = void(__fastcall*)(void* shape, void* edx, int x, int y, float sx, float sy, std::uint32_t colour, int a, int blend,
                                int b);

struct D3DViewport8 {
    DWORD x, y, width, height;
    float min_z, max_z;
};

// A map VanaCore puts in a box: the zone map (Main menu > Map, [client] map.*) or the region maps (Region Info > Conquest
// ..., [client] regionmap.*), its settings and what VanaCore keeps of it
struct Map {
    Map(const wchar_t* prefix, const char* id, bool zone) : prefix(prefix), id(id), zone(zone) {}

    const wchar_t* prefix; // its settings' names: [client] <prefix>.fullscreen ...
    const char* id;        // its box's name (vana.layout.outline)
    bool zone;             // the zone map: your marker, the home point zoom and the zoom saved at its close are its alone

    // [client] <prefix>.*
    bool full = true;          // .fullscreen: the game's own map, over the whole screen
    double x = 30, y = 12;     // .x, .y: the box's left and top edges, percents of the interface
    double size = 70;          // .scale: the box's side, a percent of the interface's height (the box is square)
    float opacity = 1.0f;      // .opacity (a percent in the file)
    bool border = false;       // .border: a 2 px black border with rounded corners and a drop shadow round the box
    bool background = false;   // .background: the box filled with .background.color under the map
    std::uint32_t background_rgb = 0x000000;
    bool wheel = true;         // .wheel: the mouse wheel over the map zooms it
    bool center = true;        // .center: the map opens with you (a region map: your zone) in the middle of it
    bool drag = true;          // .drag: a left drag on the map scrolls it
    bool instant = false;      // .instantclose: the map goes at once when closed, not fading out
    bool walk = true;          // .walk: in the box, the map leaves you walking and the camera turning, its keys not scrolling it
    bool track = true;         // map.track: your marker on the map moves as you do, not only where you opened it
    float point_zoom = 1.6f;   // map.pointzoom (a percent in the file): a glide to a place, not you, zooms in to the opening zoom x this
    unsigned gen = 1;          // counts the settings' changes, for a map that's open as they change

    // The map whose scales VanaCore last changed: what it left in the zoom's lower end and the opening scale (a value
    // other than these is the game's, worked out from the interface's width), and the ratio it applied
    char* obj = nullptr;
    float min_left = 0, open_left = 0, ratio = 1.0f;
    bool left_set = false;
    char* gen_obj = nullptr;
    unsigned gen_applied = 0;

    // The map as last drawn, for the wheel (the mouse hook's thread reads it) and the border: until when it counts as open,
    // and its box in VanaCore's interface's pixels (ui::screen)
    std::atomic<ULONGLONG> shown_until{0};
    std::atomic<int> px_left{0}, px_top{0}, px_right{0}, px_bottom{0};
    std::atomic<int> notches{0}; // wheel notches waiting for the map's next draw (+ zooms in)
    // How much of the map shows, 0-1, as its last draw had it: its fade (+0xA6, 0 none; the picture drawn at 255 - it, at
    // most 160), closing or opening. The border and the background go with it, and the wheel is the camera's again at 0.
    std::atomic<float> visible{1.0f};

    // The border (.border): its picture, made again when the box's size changes, and the device it's on (the game's, as the
    // map's last draw had it)
    void* border_device = nullptr;
    text::Sprite border_sprite;
    int border_w = 0, border_h = 0;
    float border_k = 0;

    // A left drag's travel on it since its last draw, in the interface's units (ImGui's frame)
    float pan_x = 0, pan_y = 0;
    // The mouse, in the interface's units, and whether it's over the box (ImGui's frame): a region map's cursor in a box
    float mouse_x = 0, mouse_y = 0;
    bool mouse_in = false;
    std::uint16_t mode_taken = 0; // a region map's +0x14 bits VanaCore took off in a box (put back full screen)

    // A glide the game was making at the last draw, and one VanaCore is finishing (finish_glide); new_glide's last target
    // seen, and map.pointzoom's zoom from and to
    bool was_gliding = false, finishing = false;
    float seen_x = 0, seen_y = 0;
    float point_from = 0, point_to = 0;
};

enum { kZoneMap, kRegionMap, kMaps };
Map g_maps[kMaps] = {{L"map", "map", true}, {L"regionmap", "regionmap", false}};

std::wstring g_ini;

bool boxed(const Map& m) { return !m.full; }

bool g_tried = false;
bool g_hooked = false;
std::uintptr_t g_map_draw = 0;
std::uintptr_t g_region_vtable = 0;
Calc g_calc = nullptr;  // the trampoline into the game's CalcLimit
Draw g_image = nullptr; // the game's picture-and-markers draw
Draw g_marks = nullptr; // the game's extra markers
Zoom g_zoom = nullptr;
SetPosAuto g_set_pos = nullptr;
Close g_close = nullptr; // the trampoline into the map's close
Shape g_shape = nullptr;
char* const* g_renderer = nullptr;
AnalogKey g_analog = nullptr;
SetPlayer g_set_player = nullptr;
RegionCalc g_region_calc = nullptr; // the region maps' calc
volatile std::uint32_t* g_key_level = nullptr;
std::uint32_t g_level_added = 0, g_level_left = 0; // the kinds VanaCore added to the key level, and the level it left
const Map* g_drawing = nullptr; // the map whose picture is being drawn (the shape draw is its)

constexpr float kBorderWidth = 2.0f;   // screen pixels
constexpr float kBorderRadius = 4.5f;  // the inside of its corners (the outside 2 more): the map's corners go under it
constexpr float kShadowOffset = 0.0f;  // centred, as CSS box-shadow: 0 0 5px
constexpr float kShadowSoft = 2.5f;    // its blur: a 5 px blur radius (CSS: sigma = blur / 2)
constexpr float kShadowReach = 16.0f;  // the picture's margin round the box
constexpr float kShadowAlpha = 0.55f;

// The border's picture: (wide + 2 margin) x (tall + 2 margin), the box `margin` in from its edges. Clear inside the box but
// for its corners, which the border's rounded inside covers (so the map's square corners look round); a 2 px black line
// round it; a soft shadow all round it. Straight alpha. `k`: ImGui's pixels a screen pixel.
std::vector<std::uint32_t> border_image(int wide, int tall, int margin, float k)
{
    const int w = wide + margin * 2, h = tall + margin * 2;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(w) * h);
    const float line = kBorderWidth * k, radius = kBorderRadius * k, offset = kShadowOffset * k, soft = kShadowSoft * k;
    const float half_w = wide * 0.5f, half_h = tall * 0.5f;
    // signed distance from (px, py), relative to the box's middle, to the box with its corners rounded by `r`
    auto rounded = [](float px, float py, float hw, float hh, float r) {
        const float qx = std::fabs(px) - (hw - r), qy = std::fabs(py) - (hh - r);
        const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
        return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
    };
    for (int y = 0; y < h; ++y) {
        const float py = y + 0.5f - margin - half_h;
        for (int x = 0; x < w; ++x) {
            const float px = x + 0.5f - margin - half_w;
            const float d = rounded(px, py, half_w, half_h, radius); // < 0 inside the border's inside
            if (d < -1.0f)
                continue;
            // the line: from the inside's edge (d = 0, covering the box's corners past it) out to `line`, softened by a pixel
            const float edge = std::clamp(d + 0.5f, 0.0f, 1.0f) * std::clamp(line + 0.5f - d, 0.0f, 1.0f);
            // the shadow: the border's outside, blurred; only where the line and the box aren't
            const float s = rounded(px - offset, py - offset, half_w + line, half_h + line, radius + line);
            const float shade = s <= 0 ? 1.0f : std::exp(-(s * s) / (2.0f * soft * soft));
            const float outside = std::clamp(d - line + 0.5f, 0.0f, 1.0f);
            const float shadow = kShadowAlpha * shade * outside;
            const float alpha = edge + shadow * (1.0f - edge);
            if (alpha <= 0.002f)
                continue;
            pixels[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint32_t>(std::lround(std::min(alpha, 1.0f) * 255)) << 24;
        }
    }
    return pixels;
}

// A left drag on a map (ImGui's frame): which map, and the mouse as the map's last draw had it
Map* g_panning = nullptr;
ImVec2 g_pan_last;
ImVec2 g_pan_start; // where the press went down
int g_pan_tail = 0;       // frames left to keep the game's buttons cleared after a drag it didn't get
bool g_pan_taken = false; // ... and it has moved far enough to be a drag: the game's camera no longer has it

// Shift + drag on a map's box (ImGui's frame)
Map* g_held = nullptr;
ImVec2 g_grab;

// The box, in the interface's units
struct Box {
    float x = 0, y = 0, w = 0, h = 0;
    float area_x = 0, area_y = 0, area_w = 0, area_h = 0;
};

template <typename T> T& at(void* base, std::ptrdiff_t offset)
{
    return *reinterpret_cast<T*>(static_cast<char*>(base) + offset);
}

std::uintptr_t call_target(std::uintptr_t call)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), 4);
    return call + 5 + rel;
}

bool is_call(std::uintptr_t at) { return *reinterpret_cast<const std::uint8_t*>(at) == 0xE8; }

std::wstring key(const Map& m, const wchar_t* name) { return std::wstring(m.prefix) + L"." + name; }

double number(const std::wstring& key, double fallback, double low, double high, const wchar_t* ini)
{
    wchar_t text[32] = {};
    GetPrivateProfileStringW(L"client", key.c_str(), L"", text, 32, ini);
    if (text[0] == 0)
        return fallback;
    wchar_t* end = nullptr;
    const double value = std::wcstod(text, &end);
    return end != text ? std::clamp(value, low, high) : fallback;
}

bool on(const std::wstring& key, bool fallback, const wchar_t* ini)
{
    return GetPrivateProfileIntW(L"client", key.c_str(), fallback ? 1 : 0, ini) != 0;
}

// A call's (or jmp's) offset, written in one store (the game's thread runs this code)
bool write_offset(std::uintptr_t call, std::uintptr_t target)
{
    const std::uintptr_t where = call + 1;
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(where), 4, PAGE_EXECUTE_READWRITE, &protect)) {
        log::write(L"Map: the game's code at %p couldn't be written (error %lu)", reinterpret_cast<void*>(where), GetLastError());
        return false;
    }
    *reinterpret_cast<volatile std::int32_t*>(where) = static_cast<std::int32_t>(target - (call + 5));
    VirtualProtect(reinterpret_cast<void*>(where), 4, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(where), 4);
    return true;
}

// A vtable's slot, written in one store
bool write_slot(std::uintptr_t slot, std::uintptr_t target)
{
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(slot), 4, PAGE_READWRITE, &protect)) {
        log::write(L"Map: the game's vtable at %p couldn't be written (error %lu)", reinterpret_cast<void*>(slot), GetLastError());
        return false;
    }
    *reinterpret_cast<volatile std::uintptr_t*>(slot) = target;
    VirtualProtect(reinterpret_cast<void*>(slot), 4, protect, &protect);
    return true;
}

bool area(Box& b)
{
    int left = 0, top = 0, right = 0, bottom = 0;
    if (!uispace::area(left, top, right, bottom))
        return false;
    b.area_x = static_cast<float>(left), b.area_y = static_cast<float>(top);
    b.area_w = static_cast<float>(right - left), b.area_h = static_cast<float>(bottom - top);
    return true;
}

// The box: a square .scale percent of the interface's height, its left and top edges .x and .y percent across and down
// (kept on the screen); with .fullscreen, the whole area
bool box(const Map& m, Box& b)
{
    if (!area(b))
        return false;
    if (!boxed(m)) {
        b.x = b.area_x, b.y = b.area_y, b.w = b.area_w, b.h = b.area_h;
        return true;
    }
    const float side = std::clamp(std::round(static_cast<float>(b.area_h * m.size / 100.0)), 64.0f, std::min(b.area_w, b.area_h));
    b.w = b.h = side;
    b.x = b.area_x + std::clamp(std::round(static_cast<float>(b.area_w * m.x / 100.0)), 0.0f, b.area_w - side);
    b.y = b.area_y + std::clamp(std::round(static_cast<float>(b.area_h * m.y / 100.0)), 0.0f, b.area_h - side);
    return true;
}

// The map an object is: a YkWndImage whose draw (vtable slot 4) is the zone map's, or one of the region maps' vtable
Map* map_of(const char* self)
{
    if (self == nullptr || IsBadReadPtr(self, kPictures + 4))
        return nullptr;
    const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(self);
    if (vtable == nullptr)
        return nullptr;
    if (g_region_vtable != 0 && reinterpret_cast<std::uintptr_t>(vtable) == g_region_vtable)
        return &g_maps[kRegionMap];
    if (!IsBadReadPtr(vtable, (kVtableDraw + 1) * sizeof(void*)) && vtable[kVtableDraw] == g_map_draw)
        return &g_maps[kZoneMap];
    return nullptr;
}

// The ratio of the box's scale to the game's: the game's 1x fills the area's width, the box's fills the box. 1 full screen.
float wanted_ratio(const Map& m, Box& b)
{
    if (!boxed(m) || !box(m, b))
        return 1.0f;
    return std::max(b.w, b.h) / b.area_w;
}

// The first picture's size with its margins round it, and where it starts (map units)
bool content(char* self, float& start_x, float& start_y, float& wide, float& tall)
{
    char* const* pictures = at<char* const*>(self, kPictures);
    if (pictures == nullptr || IsBadReadPtr(pictures, 4) || *pictures == nullptr || IsBadReadPtr(*pictures, kBoundsY + 4))
        return false;
    const char* picture = *pictures;
    const float margin_x = static_cast<float>(at<short>(self, kMarginA) + at<short>(self, kMarginB));
    const float margin_y = static_cast<float>(at<short>(self, kMarginA + 2) + at<short>(self, kMarginB + 2));
    wide = static_cast<float>(*reinterpret_cast<const short*>(picture + kBoundsX + 2) -
                              *reinterpret_cast<const short*>(picture + kBoundsX)) + 2 * margin_x;
    tall = static_cast<float>(*reinterpret_cast<const short*>(picture + kBoundsY + 2) -
                              *reinterpret_cast<const short*>(picture + kBoundsY)) + 2 * margin_y;
    start_x = at<short>(self, kOffset) - margin_x, start_y = at<short>(self, kOffset + 2) - margin_y;
    return wide > 0 && tall > 0;
}

// A region map's zoom's range, VanaCore's alone: the game sets it only in its debug mode, and otherwise it holds whatever
// the window's memory had (0 to 0 one day; 9e37 to 0 another, seen 2026-10-23, where the wheel's zoom clamped the scale to
// 0, the pan went NaN and the map vanished). From the picture filling the box (`want` x the area's width) to
// kRegionZoomRange times that; 0 to 0 when the picture isn't there.
void region_range(char* self, float want)
{
    float& lo = at<float>(self, kScaleMin);
    float& hi = at<float>(self, kScaleMax);
    lo = hi = 0;
    Box a;
    float start_x = 0, start_y = 0, wide = 0, tall = 0;
    if (!area(a) || !content(self, start_x, start_y, wide, tall))
        return;
    lo = a.area_w / wide * want;
    hi = lo * kRegionZoomRange;
}

// A zoom's range a zoom can go by: the scale stays above 0 (the game clamps to it as it is)
bool zoomable(char* self)
{
    const float lo = at<float>(self, kScaleMin), hi = at<float>(self, kScaleMax);
    return std::isfinite(lo) && std::isfinite(hi) && lo > 0 && hi >= lo;
}


// The zoom's range, the opening scale and the scale now, from the game's (the area's) to the box's. The game sets the
// range and the opening scale from the area's width when it loads a map (LoadMap FUN_101F6A80 and the map's open) and
// then calls CalcLimit; a zoom changes only the scale. A value other than the one VanaCore left is the game's.
void rescale(Map& m, char* self, float want)
{
    float& lo = at<float>(self, kScaleMin);
    float& hi = at<float>(self, kScaleMax);
    float& open = at<float>(self, kScaleOpen);
    float& scale = at<float>(self, kScale);
    float& scale_to = at<float>(self, kScaleTo);
    if (self != m.obj) {
        m.obj = self;
        m.left_set = false;
        m.ratio = 1.0f;
    }
    const float change = want / m.ratio;
    if (!m.zone) {
        region_range(self, want);
    } else if (!m.left_set || lo != m.min_left) {
        lo *= want, hi *= want;
    } else if (change != 1.0f) {
        lo *= change, hi *= change;
    }
    if (!m.left_set || open != m.open_left) {
        open *= want, scale *= want;
    } else if (change != 1.0f) {
        open *= change, scale *= change, scale_to *= change;
    }
    if (lo > 0 && hi >= lo)
        scale = std::clamp(scale, lo, hi);
    m.min_left = lo, m.open_left = open, m.ratio = want;
    m.left_set = true;
}

// The cursor's limits, the box's (kCursorInset in from its edges). A region map's open sets its own after CalcLimit (the
// whole area's but its side windows'), so they're set again at each draw.
void cursor_limits(char* self, const Box& b)
{
    float* cursor = &at<float>(self, kCursorLimits);
    cursor[0] = b.x + b.w * kCursorInset;
    cursor[1] = b.y + b.h * kCursorInset;
    cursor[2] = b.x + b.w * (1.0f - kCursorInset);
    cursor[3] = b.y + b.h * (1.0f - kCursorInset);
}

// The pan's and the cursor's limits for the box: CalcLimit's for a box at the area's origin (the content, the picture and
// its margins, scrolls inside the box; one smaller than the box sits in its middle), moved by the box's place
void box_limits(char* self, const Box& b)
{
    float start_x = 0, start_y = 0, wide = 0, tall = 0;
    const float scale = at<float>(self, kScale);
    if (!(scale > 0) || !content(self, start_x, start_y, wide, tall))
        return;
    auto limits = [scale](float start, float content, float place, float size, float& lo, float& hi) {
        if (content * scale > size) {
            lo = start - place / scale;
            hi = start + content - size / scale - place / scale;
        } else {
            lo = hi = start - (place + (size - content * scale) * 0.5f) / scale;
        }
    };
    float* pan = &at<float>(self, kPanLimits); // min x, min y, max x, max y
    limits(start_x, wide, b.x, b.w, pan[0], pan[2]);
    limits(start_y, tall, b.y, b.h, pan[1], pan[3]);
    cursor_limits(self, b);
}

void __fastcall calc_hook(char* self)
{
    Map* m = nullptr;
    Box b;
    __try {
        m = map_of(self);
        if (m != nullptr)
            rescale(*m, self, wanted_ratio(*m, b));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        m = nullptr;
    }
    g_calc(self);
    if (m == nullptr || !boxed(*m) || b.w <= 0)
        return;
    __try {
        box_limits(self, b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// A map open as the settings change: its scales and limits worked out again (as the game's CalcLimit, hooked), and the
// pan and the cursor brought inside them
void reapply(Map& m, char* self)
{
    if (self == m.gen_obj && m.gen_applied == m.gen)
        return;
    // opened since (and its limits worked out by the game's CalcLimit, hooked): they had the settings as they are
    const bool fresh = self != m.gen_obj && self == m.obj;
    m.gen_obj = self, m.gen_applied = m.gen;
    if (fresh)
        return;
    calc_hook(self);
    const float* pan = &at<float>(self, kPanLimits);
    at<float>(self, kPan) = std::clamp(at<float>(self, kPan), pan[0], std::max(pan[0], pan[2]));
    at<float>(self, kPan + 4) = std::clamp(at<float>(self, kPan + 4), pan[1], std::max(pan[1], pan[3]));
    const float* cursor = &at<float>(self, kCursorLimits);
    short& cx = at<short>(self, kCursor);
    short& cy = at<short>(self, kCursor + 2);
    cx = static_cast<short>(std::clamp(static_cast<float>(cx), cursor[0], std::max(cursor[0], cursor[2])));
    cy = static_cast<short>(std::clamp(static_cast<float>(cy), cursor[1], std::max(cursor[1], cursor[3])));
}

// A scale or pan gone wrong (0, or not a number: a zoom by a bad range) put right: the opening scale in the zoom's range,
// the limits worked out again for it (as the game's CalcLimit, hooked), and the pan at their lower end
void repair(char* self)
{
    float& scale = at<float>(self, kScale);
    float* pan = &at<float>(self, kPan);
    if (std::isfinite(scale) && scale > 0 && std::isfinite(pan[0]) && std::isfinite(pan[1]))
        return;
    const float open = at<float>(self, kScaleOpen);
    if (std::isfinite(open) && open > 0)
        scale = zoomable(self) ? std::clamp(open, at<float>(self, kScaleMin), at<float>(self, kScaleMax)) : open;
    else if (zoomable(self))
        scale = at<float>(self, kScaleMin);
    at<float>(self, kScaleTo) = scale;
    calc_hook(self);
    const float* limits = &at<float>(self, kPanLimits);
    for (int i = 0; i < 2; ++i)
        if (!std::isfinite(pan[i]))
            pan[i] = std::isfinite(limits[i]) ? limits[i] : 0.0f;
}

// The wheel's notches since the last draw, as the game's zoom (towards the map's cursor, in its range; none by a range
// gone wrong, which would take the scale to 0)
void wheel_zoom(Map& m, char* self)
{
    const int notches = m.notches.exchange(0);
    if (notches == 0 || g_zoom == nullptr || !zoomable(self))
        return;
    const float scale = at<float>(self, kScale);
    if (!(scale > 0))
        return;
    const float change = scale * (std::pow(kWheelStep, static_cast<float>(std::clamp(notches, -10, 10))) - 1.0f);
    g_zoom(self, nullptr, change);
}

// The drag's travel since the last draw, as the pan (the map goes with the mouse), kept in the pan's limits; the game's own
// glide (SetPosAuto) stopped, as WASD's scrolling would leave it
void drag_pan(Map& m, char* self)
{
    const float dx = m.pan_x, dy = m.pan_y;
    m.pan_x = m.pan_y = 0;
    if (dx == 0 && dy == 0)
        return;
    m.was_gliding = m.finishing = false;
    const float scale = at<float>(self, kScale);
    if (!(scale > 0))
        return;
    const float* pan = &at<float>(self, kPanLimits);
    at<float>(self, kPan) = std::clamp(at<float>(self, kPan) - dx / scale, pan[0], std::max(pan[0], pan[2]));
    at<float>(self, kPan + 4) = std::clamp(at<float>(self, kPan + 4) - dy / scale, pan[1], std::max(pan[1], pan[3]));
    std::uint16_t& stat = at<std::uint16_t>(self, kStat);
    if ((stat & kGliding) != 0) {
        stat = static_cast<std::uint16_t>(stat & ~kGliding);
        if (char* ctrl = at<char*>(self, kCtrl); ctrl != nullptr)
            at<std::uint8_t>(ctrl, kCtrlCursorShown) = 1;
    }
}

// .instantclose: a map closing (its fade begun) faded out already, and nothing of it animating, so the game closes it at
// its next step and it isn't seen meanwhile (at alpha 255 its picture is drawn clear, its markers not at all)
void vanish(const Map& m, char* self)
{
    std::uint16_t& stat = at<std::uint16_t>(self, kStat);
    if (!m.instant || (stat & kClosing) == 0)
        return;
    at<std::uint16_t>(self, kAlpha) = 0xFF;
    stat = static_cast<std::uint16_t>(stat & ~kAnimating);
}

// .walk: a map in a box and drawn of late (the open map's) with it
bool walking()
{
    const ULONGLONG now = GetTickCount64();
    for (const Map& m : g_maps)
        if (m.walk && boxed(m) && now < m.shown_until.load())
            return true;
    return false;
}

// .walk: walking and the camera added to the key level the map's open set (the game's own values otherwise: a level other
// than the one VanaCore left is the game's), and taken out again once .walk or the box is off. Its close puts every kind
// back.
void let_walk(const Map& m)
{
    if (g_key_level == nullptr)
        return;
    const std::uint32_t level = *g_key_level;
    if (level != g_level_left)
        g_level_added = 0;
    std::uint32_t now = level;
    if (level != kAnyKey && m.walk && boxed(m)) {
        const std::uint32_t add = kWalkKeys & ~level;
        now = level | add, g_level_added |= add;
    } else if (level != kAnyKey && g_level_added != 0) {
        now = level & ~g_level_added, g_level_added = 0;
    }
    if (now != level)
        *g_key_level = now;
    g_level_left = now;
}

// map.track: your marker from where you are now, as the map's open puts it (on your own zone's map only: its markers of you
// are SetPlayer's), the pan left where it was (SetPlayer puts it on you)
void track(const Map& m, char* self)
{
    if (!m.zone || !m.track || g_set_player == nullptr || at<char*>(self, kYouMarks) != self + kYouMarker)
        return;
    const float pan_x = at<float>(self, kPan), pan_y = at<float>(self, kPan + 4);
    g_set_player(self, nullptr, 1);
    at<float>(self, kPan) = pan_x, at<float>(self, kPan + 4) = pan_y;
}

// The map's scrolling by its keys (types 0x79 and 0x7A): none with .walk, the keys walking you instead
float __cdecl pan_key_hook(unsigned level, int type)
{
    if (walking())
        return 0.0f;
    return g_analog(level, type);
}

// The map just opened: its cursor put in the middle of the box and the game's glide to the player (as its open does, which
// takes the player to wherever the cursor was), so you're in the middle of it. A glide the game has begun (a home point
// crystal's teleport list, to the home point picked; a region map's to your zone) is kept: it follows the cursor, now the
// box's middle.
void center(const Map& m, char* self, const Box& b)
{
    if (!m.center || g_set_pos == nullptr)
        return;
    const float* cursor = &at<float>(self, kCursorLimits);
    const float mid_x = b.x + b.w * 0.5f, mid_y = b.y + b.h * 0.5f;
    at<short>(self, kCursor) = static_cast<short>(std::lround(std::clamp(mid_x, cursor[0], std::max(cursor[0], cursor[2]))));
    at<short>(self, kCursor + 2) = static_cast<short>(std::lround(std::clamp(mid_y, cursor[1], std::max(cursor[1], cursor[3]))));
    if ((at<std::uint16_t>(self, kStat) & kGliding) == 0)
        g_set_pos(self, nullptr, static_cast<float>(at<short>(self, kPlayer)), static_cast<float>(at<short>(self, kPlayer + 2)));
}

// The cursor kept in the box: the game puts it in the middle of the whole area (the map's open, 0x101F85BE; its reset,
// FUN_101ECBA0, when a crystal's teleport list opens it; a region map's zone picked), at times after the map's first draw,
// and its glides (SetPosAuto: the player, the home point or zone picked) take their point to the cursor, so a cursor
// outside the box leaves the map against its edge. That place is the game's middle (its x the area's width x 200 / 512, its
// y half its height: (1125, 810) on a 2880 x 1620 interface), so one outside the cursor's limits, or at that place (a box
// over the screen's middle), is put in the middle of the box.
void keep_cursor(const Map& m, char* self, const Box& b)
{
    if (!boxed(m))
        return;
    const float* cursor = &at<float>(self, kCursorLimits);
    short& cx = at<short>(self, kCursor);
    short& cy = at<short>(self, kCursor + 2);
    const float x = cx, y = cy;
    const short mid_x = static_cast<short>(std::lround(std::clamp(b.x + b.w * 0.5f, cursor[0], std::max(cursor[0], cursor[2]))));
    const short mid_y = static_cast<short>(std::lround(std::clamp(b.y + b.h * 0.5f, cursor[1], std::max(cursor[1], cursor[3]))));
    const bool inside = x >= cursor[0] && x <= std::max(cursor[0], cursor[2]) && y >= cursor[1] && y <= std::max(cursor[1], cursor[3]);
    const bool games = std::fabs(x - b.area_w * kGameMiddleX) <= 1.0f && std::fabs(y - b.area_h * 0.5f) <= 1.0f;
    if ((inside && !games) || (cx == mid_x && cy == mid_y))
        return;
    cx = mid_x, cy = mid_y;
}

// A map in a box: a left press on it is VanaCore's (a drag), never the game's (a region map's pick of a zone, or the
// buzz of a click on nothing)
bool takes_presses(const Map& m) { return boxed(m); }

// A region map in a box: VanaCore puts its cursor under the mouse (follow_mouse), the game's following taken off
// (mouse_mode)
bool follows_mouse(const Map& m) { return !m.zone && boxed(m); }

// A region map in a box doesn't follow the mouse as the game has it (its bits taken off before the game's calc: no
// scrolling at the box's edges, no mouse put back on the cursor, no glide to a zone near the cursor); VanaCore puts the
// cursor under the mouse (follow_mouse). Full screen, the bits it took are put back.
void mouse_mode(Map& m, char* self)
{
    std::uint16_t& stat = at<std::uint16_t>(self, kStat);
    if (boxed(m)) {
        m.mode_taken = static_cast<std::uint16_t>(m.mode_taken | (stat & (kMouseCursor | kMagnet)));
        stat = static_cast<std::uint16_t>(stat & ~(kMouseCursor | kMagnet));
    } else if (m.mode_taken != 0) {
        stat = static_cast<std::uint16_t>(stat | m.mode_taken);
        m.mode_taken = 0;
    }
}

// A region map in a box: its cursor under the mouse while the mouse is over the box (the zone there named, its info
// shown; the wheel zooms about it), or in the box's middle while the map glides (a zone picked with the keys: the glide
// brings it there)
void follow_mouse(const Map& m, char* self, const Box& b)
{
    short& cx = at<short>(self, kCursor);
    short& cy = at<short>(self, kCursor + 2);
    if ((at<std::uint16_t>(self, kStat) & kGliding) != 0 || m.was_gliding || m.finishing) {
        cx = static_cast<short>(std::lround(b.x + b.w * 0.5f)), cy = static_cast<short>(std::lround(b.y + b.h * 0.5f));
        return;
    }
    if (!m.mouse_in)
        return;
    cx = static_cast<short>(std::lround(std::clamp(m.mouse_x, b.x, b.x + b.w - 1)));
    cy = static_cast<short>(std::lround(std::clamp(m.mouse_y, b.y, b.y + b.h - 1)));
}

// A new glide (its target changed, gliding): SetPosAuto adds the picture's margin (+0x7C, 30 on the zone map, 0 on a region
// map) to the place it's given, so its glide ends with that place up and to the left of the cursor by the margin x the
// scale (a crystal's home point, or you, off the box's middle); in a box the margin is taken off its target again. A target
// left from before the map opened (no glide) is only noted.
// map.pointzoom: a new glide on the zone map to a place that isn't you (a crystal's home point picked) zooms in to the
// opening zoom times map.pointzoom, about the cursor the glide goes to; a zoom further in already is kept. The zoom it came
// from is what the map's close saves, while it's still at the one it went to (a wheel's after it is saved as it is).
void new_glide(Map& m, char* self)
{
    float& target_x = at<float>(self, kTarget);
    float& target_y = at<float>(self, kTarget + 4);
    const bool fresh = target_x != m.seen_x || target_y != m.seen_y;
    if (!fresh || (at<std::uint16_t>(self, kStat) & kGliding) == 0) {
        m.seen_x = target_x, m.seen_y = target_y;
        return;
    }
    // the place it was given
    const float tx = target_x - at<short>(self, kMarginA), ty = target_y - at<short>(self, kMarginA + 2);
    if (boxed(m))
        target_x = tx, target_y = ty;
    m.seen_x = target_x, m.seen_y = target_y;
    if (!m.zone || m.point_zoom <= 1.0f || g_zoom == nullptr || !zoomable(self))
        return;
    if (std::fabs(tx - at<short>(self, kPlayer)) <= kNearYou && std::fabs(ty - at<short>(self, kPlayer + 2)) <= kNearYou)
        return;
    const float scale = at<float>(self, kScale), want = at<float>(self, kScaleOpen) * m.point_zoom;
    if (!(scale > 0) || want <= scale)
        return;
    g_zoom(self, nullptr, want - scale);
    if (m.point_to == 0)
        m.point_from = scale;
    m.point_to = at<float>(self, kScale);
}

// The game's glide stops near its target, which in a box leaves it visibly off the middle (a crystal's home point up and
// to the left of it): once it stops, the rest is eased out, the target put under the cursor (in the pan's limits). A drag
// stops it (drag_pan), as does a new glide.
void finish_glide(Map& m, char* self)
{
    if ((at<std::uint16_t>(self, kStat) & kGliding) != 0) {
        m.was_gliding = true, m.finishing = false;
        return;
    }
    if (m.was_gliding)
        m.was_gliding = false, m.finishing = boxed(m);
    const float scale = at<float>(self, kScale);
    if (!m.finishing || !(scale > 0))
        return;
    const float* pan = &at<float>(self, kPanLimits);
    float* now = &at<float>(self, kPan);
    bool there = true;
    for (int i = 0; i < 2; ++i) {
        const float under = at<short>(self, kCursor + i * 2) / scale + 0.5f + at<short>(self, kGlideOffset + i * 2);
        const float want = std::clamp(at<float>(self, kTarget + i * 4) - under, pan[i], std::max(pan[i], pan[i + 2]));
        const float left = want - now[i];
        if (std::fabs(left) <= kFinishNear) {
            now[i] = want;
        } else {
            now[i] += left * kFinishRate;
            there = false;
        }
    }
    if (there)
        m.finishing = false;
}

// The box as last drawn, in VanaCore's interface's pixels, for the wheel (the mouse hook's thread) and the border
void publish(Map& m, const Box& b)
{
    unsigned width = 0, height = 0;
    ui::screen(width, height);
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0, scale = 0;
    if (width == 0 || !uispace::to_pixels(b.x, b.y, width, height, x0, y0, scale) ||
        !uispace::to_pixels(b.x + b.w, b.y + b.h, width, height, x1, y1, scale))
        return;
    m.px_left = static_cast<int>(std::floor(x0)), m.px_top = static_cast<int>(std::floor(y0));
    m.px_right = static_cast<int>(std::ceil(x1)), m.px_bottom = static_cast<int>(std::ceil(y1));
    m.shown_until = GetTickCount64() + kShownFor;
}

// The device's viewport made the box's (and put back after): the map's draws are screen-space, clipped to it
struct Clip {
    void* device = nullptr;
    D3DViewport8 kept{};
};

bool clip_on(const Map* m, Clip& clip, bool fill)
{
    Box b;
    if (m == nullptr || !boxed(*m) || g_renderer == nullptr || !box(*m, b))
        return false;
    char* renderer = *g_renderer;
    if (renderer == nullptr)
        return false;
    void* device = at<void*>(renderer, kRendererDevice);
    if (device == nullptr)
        return false;
    D3DViewport8 vp{};
    if (FAILED(d3d8::call(device, d3d8::slot::GetViewport, &vp)) || vp.width == 0 || vp.height == 0)
        return false;
    // the area's units in the viewport's pixels (the menus' target is the area's size: 1)
    const float sx = vp.width / b.area_w, sy = vp.height / b.area_h;
    int left = static_cast<int>(vp.x + std::lround((b.x - b.area_x) * sx));
    int top = static_cast<int>(vp.y + std::lround((b.y - b.area_y) * sy));
    int right = left + static_cast<int>(std::lround(b.w * sx));
    int bottom = top + static_cast<int>(std::lround(b.h * sy));
    left = std::max(left, static_cast<int>(vp.x)), top = std::max(top, static_cast<int>(vp.y));
    right = std::min(right, static_cast<int>(vp.x + vp.width)), bottom = std::min(bottom, static_cast<int>(vp.y + vp.height));
    if (right <= left || bottom <= top)
        return false;
    D3DViewport8 inner = vp;
    inner.x = static_cast<DWORD>(left), inner.y = static_cast<DWORD>(top);
    inner.width = static_cast<DWORD>(right - left), inner.height = static_cast<DWORD>(bottom - top);
    if (FAILED(d3d8::call(device, d3d8::slot::SetViewport, &inner)))
        return false;
    clip.device = device;
    clip.kept = vp;
    // .background: the box filled with its colour under the map (the viewport is the box: Clear clears only it), as
    // see-through as the map
    if (fill && m->background) {
        const auto alpha = static_cast<std::uint32_t>(std::lround(255.0f * m->opacity * m->visible.load()));
        d3d8::call(device, d3d8::slot::Clear, DWORD{0}, static_cast<const void*>(nullptr), DWORD{1} /* D3DCLEAR_TARGET */,
                   static_cast<DWORD>(alpha << 24 | (m->background_rgb & 0xFFFFFFu)), 1.0f, DWORD{0});
    }
    return true;
}

void clip_off(Clip& clip)
{
    if (clip.device != nullptr)
        d3d8::call(clip.device, d3d8::slot::SetViewport, &clip.kept);
    clip.device = nullptr;
}

// The picture and its markers (YkWndImage's draw) of `self`, `m` if it's a map VanaCore boxes: its settings applied first,
// and drawn in its box
void draw_picture(char* self, Map* m)
{
    Clip clip;
    __try {
        if (m != nullptr) {
            reapply(*m, self);
            repair(self);
            vanish(*m, self);
            let_walk(*m);
            track(*m, self);
            const int fade = at<std::uint16_t>(self, kAlpha) & 0xFF;
            m->visible = fade == 0 ? 1.0f : std::clamp((255 - fade) / 160.0f, 0.0f, 1.0f);
            const bool opened = GetTickCount64() >= m->shown_until.load(); // not drawn for a while: just opened
            Box b;
            const bool placed = box(*m, b);
            if (placed && boxed(*m))
                cursor_limits(self, b);
            if (opened)
                m->point_to = 0, m->was_gliding = m->finishing = false;
            if (opened && placed) {
                m->pan_x = m->pan_y = 0, m->notches = 0;
                center(*m, self, b);
            }
            if (placed && follows_mouse(*m))
                follow_mouse(*m, self, b);
            else if (placed)
                keep_cursor(*m, self, b);
            new_glide(*m, self);
            wheel_zoom(*m, self);
            drag_pan(*m, self);
            finish_glide(*m, self);
            if (placed)
                publish(*m, b);
        }
        clip_on(m, clip, m != nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_drawing = m;
    g_image(self);
    g_drawing = nullptr;
    clip_off(clip);
}

// The zone map's draw's call of its picture's
void __fastcall image_hook(char* self)
{
    Map* m = nullptr;
    __try {
        m = map_of(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    draw_picture(self, m);
}

// The map's close: the zoom it saves as the game's (the box's scale over the ratio VanaCore applied: saved as it is, each
// open in the box would start further out; and the zoom before map.pointzoom's, while it's still at that one), and with
// map.instantclose its fade done at once
void __fastcall close_hook(char* self, void* edx, int flag)
{
    Map& m = g_maps[kZoneMap];
    float kept = 0;
    bool scaled = false;
    __try {
        if (map_of(self) == &m) {
            kept = at<float>(self, kScale);
            float save = kept;
            if (m.point_to > 0 && kept == m.point_to)
                save = m.point_from;
            if (self == m.obj && m.ratio > 0)
                save /= m.ratio;
            m.point_to = 0;
            if (save != kept) {
                at<float>(self, kScale) = save;
                scaled = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_close(self, edx, flag);
    __try {
        if (scaled)
            at<float>(self, kScale) = kept;
        if (map_of(self) == &m) {
            vanish(m, self);
            // closed at once: its border, background and wheel go now, not at a draw it may not have, and it counts as
            // closed (its next draw is an open)
            if (m.instant && (at<std::uint16_t>(self, kStat) & kClosing) != 0)
                m.visible = 0.0f, m.shown_until = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// .border: its picture (border_image) round the box, drawn as the map's last (after its markers), so the game's windows
// drawn after the map (a menu over it) cover it, as they do the map. In the render target's pixels (the viewport's, the
// whole area); its sizes are the screen's pixels, as many of the target's as one of those.
void draw_border(Map& m)
{
    Box b;
    if (!m.border || !boxed(m) || g_renderer == nullptr || !box(m, b) || b.area_w <= 0 || b.area_h <= 0)
        return;
    const float visible = m.visible.load();
    if (visible <= 0.0f)
        return;
    char* renderer = *g_renderer;
    void* device = renderer != nullptr ? at<void*>(renderer, kRendererDevice) : nullptr;
    D3DViewport8 vp{};
    if (device == nullptr || FAILED(d3d8::call(device, d3d8::slot::GetViewport, &vp)) || vp.width == 0 || vp.height == 0)
        return;
    const float sx = vp.width / b.area_w, sy = vp.height / b.area_h;
    const int left = static_cast<int>(vp.x + std::lround((b.x - b.area_x) * sx));
    const int top = static_cast<int>(vp.y + std::lround((b.y - b.area_y) * sy));
    const int wide = static_cast<int>(std::lround(b.w * sx)), tall = static_cast<int>(std::lround(b.h * sy));
    if (wide <= 0 || tall <= 0)
        return;
    unsigned screen_w = 0, screen_h = 0;
    ui::screen(screen_w, screen_h);
    const float k = screen_w > 0 ? std::max(0.5f, static_cast<float>(vp.width) / screen_w) : 1.0f;
    if (device != m.border_device) {
        m.border_sprite.release();
        m.border_w = m.border_h = 0;
        m.border_device = device;
    }
    const int margin = static_cast<int>(std::ceil(kShadowReach * k));
    if (wide != m.border_w || tall != m.border_h || k != m.border_k) {
        m.border_sprite.set_image(border_image(wide, tall, margin, k), wide + margin * 2, tall + margin * 2);
        m.border_w = wide, m.border_h = tall, m.border_k = k;
    }
    if (!m.border_sprite.upload(device) || m.border_sprite.texture() == nullptr)
        return;
    DWORD state = 0;
    if (!text::prepare(device, vp.x + vp.width, vp.y + vp.height, state))
        return;
    const auto alpha = static_cast<std::uint32_t>(std::lround(255.0f * std::min(visible, 1.0f)));
    text::draw_texture(device, m.border_sprite.texture(), static_cast<float>(left - margin), static_cast<float>(top - margin),
                       static_cast<float>(wide + margin * 2), static_cast<float>(tall + margin * 2), 0.0f, 0.0f,
                       static_cast<float>(m.border_sprite.width()) / m.border_sprite.texture_width(),
                       static_cast<float>(m.border_sprite.height()) / m.border_sprite.texture_height(), alpha << 24 | 0xFFFFFFu);
    text::restore(device, state);
}

// The zone map's extra markers (its draw's last), clipped to its box, and its border after them
void __fastcall marks_hook(char* self)
{
    Map& m = g_maps[kZoneMap];
    Clip clip;
    __try {
        clip_on(&m, clip, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_marks(self);
    clip_off(clip);
    __try {
        draw_border(m);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// A region map's draw (its vtable's slot 4, YkWndImage's draw itself): its picture, markers and zones' names in its box,
// and its border after them
void __fastcall region_hook(char* self)
{
    Map* m = nullptr;
    __try {
        m = map_of(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    draw_picture(self, m);
    if (m == nullptr)
        return;
    __try {
        draw_border(*m);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// A region map's calc (its vtable's slot 10): in a box, the cursor not the mouse's (mouse_mode) before the game's
void __fastcall region_calc_hook(char* self, void* edx, int active)
{
    __try {
        if (Map* m = map_of(self); m != nullptr)
            mouse_mode(*m, self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_region_calc(self, edx, active);
}

// A map's picture and its backing at its .opacity: their colour's alpha scaled, alpha blended (the blend the game's fade
// uses). Other YkWndImage windows' pictures (the shape draw is theirs too) as they are.
void __fastcall shape_hook(void* shape, void* edx, int x, int y, float sx, float sy, std::uint32_t colour, int a, int blend, int b)
{
    if (g_drawing != nullptr && g_drawing->opacity < 1.0f) {
        const auto alpha = static_cast<std::uint32_t>(std::lround((colour >> 24) * g_drawing->opacity));
        colour = (colour & 0x00FFFFFFu) | (std::min<std::uint32_t>(alpha, 0xFF) << 24);
        blend = kAlphaBlend;
    }
    g_shape(shape, edx, x, y, sx, sy, colour, a, blend, b);
}

bool wanted()
{
    for (const Map& m : g_maps)
        if (boxed(m) || m.opacity < 1.0f || m.wheel || m.center || m.drag || m.instant || (m.zone && m.track))
            return true;
    return false;
}

void install_hooks()
{
    g_tried = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr) {
        g_tried = false; // not loaded yet: tried again next frame
        return;
    }
    uispace::find(game);
    const std::uintptr_t calc = scan::find(game, kCalcLimit);
    const std::uintptr_t draw = scan::find(game, kMapDraw);
    if (calc == 0 || draw == 0) {
        log::write(L"Map: the game's map %ls wasn't found in FFXiMain.dll: [client] map.* does nothing",
                   calc == 0 ? L"limits" : L"drawing");
        return;
    }
    const std::uintptr_t image = call_target(draw + kImageCall);
    const std::uintptr_t marks = call_target(draw + kMarksJump);
    if (scan::find(image, 16, kImageDraw) != image) {
        log::write(L"Map: the map's picture drawing isn't as expected: [client] map.* does nothing");
        return;
    }
    g_map_draw = draw;
    g_renderer = *reinterpret_cast<char* const* const*>(image + kRendererAt);
    g_image = reinterpret_cast<Draw>(image);
    g_marks = reinterpret_cast<Draw>(marks);
    // the region maps: their vtable, its draw YkWndImage's (found before the limits are hooked: they're its maps' too)
    const std::uintptr_t ctor = scan::find(game, kRegionCtor);
    const std::uintptr_t region_vtable = ctor != 0 ? *reinterpret_cast<const std::uintptr_t*>(ctor + kRegionVtableAt) : 0;
    if (region_vtable != 0 && !IsBadReadPtr(reinterpret_cast<const void*>(region_vtable), (kVtableDraw + 1) * sizeof(void*)) &&
        reinterpret_cast<const std::uintptr_t*>(region_vtable)[kVtableDraw] == image)
        g_region_vtable = region_vtable;
    if (!imports::detour(reinterpret_cast<Calc>(calc), kCalcPrologue, &calc_hook, g_calc)) {
        log::write(L"Map: the game's map limits couldn't be hooked: [client] map.* does nothing");
        g_region_vtable = 0;
        return;
    }
    write_offset(draw + kImageCall, reinterpret_cast<std::uintptr_t>(&image_hook));
    write_offset(draw + kMarksJump, reinterpret_cast<std::uintptr_t>(&marks_hook));
    g_hooked = true;
    if (g_region_vtable == 0 || !write_slot(g_region_vtable + kVtableDraw * sizeof(void*), reinterpret_cast<std::uintptr_t>(&region_hook))) {
        log::write(L"Map: the region maps' window wasn't found in FFXiMain.dll: [client] regionmap.* does nothing");
        g_region_vtable = 0;
    } else {
        const std::uintptr_t calc_slot = g_region_vtable + kVtableCalc * sizeof(void*);
        g_region_calc = *reinterpret_cast<const RegionCalc*>(calc_slot);
        if (g_region_calc == nullptr || !write_slot(calc_slot, reinterpret_cast<std::uintptr_t>(&region_calc_hook))) {
            g_region_calc = nullptr;
            log::write(L"Map: the region maps' calc couldn't be hooked: in a box they follow the mouse as the game has them");
        }
    }
    // the opacity: the shape draw's two calls in the picture's draw
    const std::uintptr_t backing = image + kBackingCall, picture = image + kPictureCall;
    if (is_call(backing) && is_call(picture) && call_target(backing) == call_target(picture)) {
        g_shape = reinterpret_cast<Shape>(call_target(picture));
        write_offset(backing, reinterpret_cast<std::uintptr_t>(&shape_hook));
        write_offset(picture, reinterpret_cast<std::uintptr_t>(&shape_hook));
    } else {
        log::write(L"Map: the map's picture drawing isn't as expected: map.opacity does nothing");
    }
    const std::uintptr_t close = scan::find(game, kClose);
    if (close == 0 || !imports::detour(reinterpret_cast<Close>(close), kClosePrologue, &close_hook, g_close))
        log::write(L"Map: the map's close %ls: map.instantclose does nothing, and a zoom in the box is saved as it is",
                   close == 0 ? L"wasn't found in FFXiMain.dll" : L"couldn't be hooked");
    g_set_pos = reinterpret_cast<SetPosAuto>(scan::find(game, kSetPosAuto));
    if (g_set_pos == nullptr)
        log::write(L"Map: the map's centring wasn't found in FFXiMain.dll: map.center does nothing");
    const std::uintptr_t zoom = scan::find(game, kZoom);
    g_zoom = reinterpret_cast<Zoom>(zoom);
    if (zoom == 0)
        log::write(L"Map: the map's zoom wasn't found in FFXiMain.dll: map.wheel does nothing");
    // map.walk: the map's two reads of its scrolling keys, and the key level
    const std::uintptr_t keys = scan::find(game, kPanKeys);
    const std::uintptr_t analog = keys != 0 && is_call(keys + kPanKeyX) ? call_target(keys + kPanKeyX) : 0;
    if (analog != 0 && is_call(keys + kPanKeyY) && call_target(keys + kPanKeyY) == analog &&
        scan::find(analog, 16, kAnalogKey) == analog) {
        g_analog = reinterpret_cast<AnalogKey>(analog);
        g_key_level = *reinterpret_cast<volatile std::uint32_t* const*>(analog + kKeyLevelAt);
        write_offset(keys + kPanKeyX, reinterpret_cast<std::uintptr_t>(&pan_key_hook));
        write_offset(keys + kPanKeyY, reinterpret_cast<std::uintptr_t>(&pan_key_hook));
    } else {
        log::write(L"Map: the map's scrolling keys weren't found in FFXiMain.dll: map.walk does nothing");
    }
    g_set_player = reinterpret_cast<SetPlayer>(scan::find(game, kSetPlayer));
    if (g_set_player == nullptr)
        log::write(L"Map: the map's marker of you wasn't found in FFXiMain.dll: map.track does nothing");
    log::write(L"Map: hooked (limits +0x%X, drawing +0x%X, zoom +0x%X, region maps +0x%X)",
               static_cast<unsigned>(calc - reinterpret_cast<std::uintptr_t>(game)),
               static_cast<unsigned>(draw - reinterpret_cast<std::uintptr_t>(game)),
               zoom != 0 ? static_cast<unsigned>(zoom - reinterpret_cast<std::uintptr_t>(game)) : 0u,
               g_region_vtable != 0 ? static_cast<unsigned>(g_region_vtable - reinterpret_cast<std::uintptr_t>(game)) : 0u);
}

bool shown(const Map& m) { return g_hooked && GetTickCount64() < m.shown_until.load(); }

// .x and .y for a box whose left and top edges are (left, top), in the interface's units
void place_at(Map& m, float left, float top)
{
    Box b;
    if (!box(m, b) || b.area_w <= 0 || b.area_h <= 0)
        return;
    const double x = std::clamp((left - b.area_x) * 100.0 / b.area_w, 0.0, 100.0 * (b.area_w - b.w) / b.area_w);
    const double y = std::clamp((top - b.area_y) * 100.0 / b.area_h, 0.0, 100.0 * (b.area_h - b.h) / b.area_h);
    if (x != m.x || y != m.y)
        m.x = x, m.y = y, ++m.gen;
}

void save_place(const Map& m)
{
    if (g_ini.empty())
        return;
    wchar_t x[32], y[32];
    std::swprintf(x, 32, L"%.1f", m.x);
    std::swprintf(y, 32, L"%.1f", m.y);
    ini::set(g_ini, L"client", key(m, L"x").c_str(), x);
    ini::set(g_ini, L"client", key(m, L"y").c_str(), y);
}

void read(Map& m, const wchar_t* ini)
{
    const bool full = on(key(m, L"fullscreen"), true, ini);
    const double size = number(key(m, L"scale"), 70, 20, 100, ini);
    const auto opacity = static_cast<float>(number(key(m, L"opacity"), 100, 10, 100, ini) / 100.0);
    double x = m.x, y = m.y;
    if (g_held != &m) // (a drag's own place, not the file's a moment behind it)
        x = number(key(m, L"x"), 30, 0, 100, ini), y = number(key(m, L"y"), 12, 0, 100, ini);
    if (full != m.full || x != m.x || y != m.y || size != m.size)
        ++m.gen;
    m.full = full, m.x = x, m.y = y, m.size = size, m.opacity = opacity;
    m.border = on(key(m, L"border"), false, ini);
    m.background = on(key(m, L"background"), false, ini);
    wchar_t colour[32] = {};
    GetPrivateProfileStringW(L"client", key(m, L"background.color").c_str(), L"", colour, 32, ini);
    const wchar_t* digits = colour[0] == L'#' ? colour + 1 : colour;
    wchar_t* end = nullptr;
    const unsigned long rgb = std::wcstoul(digits, &end, 16);
    m.background_rgb = std::wcslen(digits) == 6 && end != nullptr && *end == 0 ? static_cast<std::uint32_t>(rgb) : 0x000000;
    m.wheel = on(key(m, L"wheel"), true, ini);
    m.center = on(key(m, L"center"), true, ini);
    m.drag = on(key(m, L"drag"), true, ini);
    m.instant = on(key(m, L"instantclose"), false, ini);
    m.walk = on(key(m, L"walk"), true, ini);
    if (m.zone) {
        m.track = on(key(m, L"track"), true, ini);
        m.point_zoom = static_cast<float>(number(key(m, L"pointzoom"), 160, 100, 400, ini) / 100.0);
    }
}

// One map's part of ImGui's frame: Shift + drag on its box moves it, a left drag scrolls it
void gui_map(Map& m, const ImGuiIO& io)
{
    m.mouse_in = false;
    const float visible = m.visible.load(); // the map faded out (closing, or closed at once): nothing of it here either
    if (!shown(m) || visible <= 0.0f) {
        if (g_held == &m)
            g_held = nullptr, save_place(m);
        if (g_panning == &m)
            g_panning = nullptr;
        return;
    }
    Box b;
    if (!box(m, b))
        return;
    const auto w = static_cast<unsigned>(io.DisplaySize.x), h = static_cast<unsigned>(io.DisplaySize.y);
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0, scale = 0;
    if (!uispace::to_pixels(b.x, b.y, w, h, x0, y0, scale) || !uispace::to_pixels(b.x + b.w, b.y + b.h, w, h, x1, y1, scale))
        return;
    // Shift + drag moves it (the map follows at its next draw)
    const ImVec2 mouse = io.MousePos;
    if (g_held == &m) {
        float ux = 0, uy = 0;
        if (uispace::to_units(mouse.x - g_grab.x, mouse.y - g_grab.y, w, h, ux, uy))
            place_at(m, ux, uy);
        if (!io.MouseDown[0])
            g_held = nullptr, save_place(m);
    }
    const bool over = mouse.x >= x0 && mouse.x < x1 && mouse.y >= y0 && mouse.y < y1;
    // where the mouse is on the map, for a region map's cursor (follow_mouse); not under one of ImGui's windows
    if (over && !io.WantCaptureMouse && uispace::to_units(mouse.x, mouse.y, w, h, m.mouse_x, m.mouse_y))
        m.mouse_in = true;
    // over one of ImGui's windows (the /vana window over the map), the press is that window's; a drag on another map's is
    // that one's
    const bool idle = g_panning == nullptr && g_held == nullptr;
    const bool on_window = io.WantCaptureMouse && idle;
    if (boxed(m) && over && io.KeyShift && io.MouseClicked[0] && idle && !on_window) {
        g_held = &m;
        g_grab = ImVec2(mouse.x - x0, mouse.y - y0);
    }
    if (boxed(m) && (g_held == &m || (over && io.KeyShift)))
        gui::take_mouse(x0, y0, x1, y1); // the press is the box's, not the game's
    // a left drag scrolls the map (full screen, the press is the game's too: a click is still a click; in a box it's
    // VanaCore's alone, takes_presses)
    if (g_panning == &m) {
        if (io.MouseDown[0] && x1 > x0 && y1 > y0) {
            // once it's a drag (not a click), the game's buttons read up till it's let go: the camera isn't dragged too
            if (!g_pan_taken && !takes_presses(m) &&
                (std::fabs(mouse.x - g_pan_start.x) > 4.0f || std::fabs(mouse.y - g_pan_start.y) > 4.0f)) {
                g_pan_taken = true;
                input::mouse_taken(true, false); // (DirectInput)
                window::release_game_buttons();  // (the window's messages)
            }
            if (g_pan_taken)
                hideui::clear_game_mouse_buttons();
            m.pan_x += (mouse.x - g_pan_last.x) * b.w / (x1 - x0);
            m.pan_y += (mouse.y - g_pan_last.y) * b.h / (y1 - y0);
            g_pan_last = mouse;
        } else {
            g_panning = nullptr;
            if (g_pan_taken)
                g_pan_tail = 12;
        }
    } else if (m.drag && over && !io.KeyShift && io.MouseClicked[0] && idle && !on_window) {
        g_panning = &m;
        g_pan_taken = false;
        g_pan_last = g_pan_start = mouse;
    }
}

} // namespace

void reload(const wchar_t* ini)
{
    g_ini = ini != nullptr ? ini : L"";
    for (Map& m : g_maps)
        read(m, ini);
}

void install(const wchar_t* ini)
{
    reload(ini);
}

void on_frame()
{
    if (!g_tried && wanted())
        install_hooks();
}

void gui_frame()
{
    if (ImGui::GetCurrentContext() == nullptr)
        return;
    // The drag's release still reaches the game's DirectInput and turns its held button back on (it toggles): until a few
    // frames after, the game's mouse forgets its buttons, or leaving the map would start the camera
    if (g_pan_tail > 0 && g_panning == nullptr) {
        --g_pan_tail;
        hideui::clear_game_mouse_buttons();
    }
    const ImGuiIO& io = ImGui::GetIO();
    for (Map& m : g_maps)
        gui_map(m, io);
}

bool wheel_over(int x, int y)
{
    if (g_zoom == nullptr)
        return false;
    const ULONGLONG now = GetTickCount64();
    for (const Map& m : g_maps)
        if (m.wheel && now < m.shown_until.load() && m.visible.load() > 0.0f && x >= m.px_left && x < m.px_right &&
            y >= m.px_top && y < m.px_bottom)
            return true;
    return false;
}

bool up(int which)
{
    return which >= 0 && which < kMaps && boxed(g_maps[which]) && shown(g_maps[which]);
}

bool outline(int which, double* moved_x, double* moved_y)
{
    if (which < 0 || which >= kMaps)
        return false;
    Map& m = g_maps[which];
    Box b;
    if (!boxed(m) || !box(m, b))
        return false; // (full screen: nothing to place)
    outline::Box place;
    place.left = static_cast<int>(std::lround(b.x)), place.top = static_cast<int>(std::lround(b.y));
    place.right = static_cast<int>(std::lround(b.x + b.w)), place.bottom = static_cast<int>(std::lround(b.y + b.h));
    outline::Drag drag;
    if (!outline::draw(m.id, place, 0, drag))
        return false;
    if (drag.held) // the map goes where the box is taken to, at its next draw
        place_at(m, static_cast<float>(drag.left), static_cast<float>(drag.top));
    if (drag.released && moved_x != nullptr && moved_y != nullptr) {
        *moved_x = std::round(m.x * 100.0) / 100.0;
        *moved_y = std::round(m.y * 100.0) / 100.0;
    }
    return true;
}

bool takes_press(int x, int y)
{
    if (!g_hooked)
        return false;
    const ULONGLONG now = GetTickCount64();
    for (const Map& m : g_maps)
        if (takes_presses(m) && now < m.shown_until.load() && m.visible.load() > 0.0f && x >= m.px_left && x < m.px_right &&
            y >= m.px_top && y < m.px_bottom)
            return true;
    return false;
}

bool on_wheel(int x, int y, int delta)
{
    if (g_zoom == nullptr)
        return false;
    const ULONGLONG now = GetTickCount64();
    for (Map& m : g_maps) {
        if (!m.wheel || now >= m.shown_until.load() || m.visible.load() <= 0.0f || x < m.px_left || x >= m.px_right ||
            y < m.px_top || y >= m.px_bottom)
            continue;
        const int notches = delta / WHEEL_DELTA;
        m.notches += notches != 0 ? notches : (delta > 0 ? 1 : -1);
        return true;
    }
    return false;
}

} // namespace vanacore::mapwin
