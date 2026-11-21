#include "phraselist.hpp"

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "addons.hpp"
#include "chat.hpp"
#include "imports.hpp"
#include "log.hpp"
#include "pluginapi.hpp"
#include "raise.hpp"
#include "scan.hpp"
#include "uispace.hpp"

namespace vanacore::phraselist {
namespace {

// The chat line's making (FUN_101b7640): the list made with the line, `new` of 2224h bytes, then FsCandidateMenu's
// constructor with the text input and the line's FsCandidateInputContext (the line object + 14h), held at the global
// after the line's: push 2224h; mov [esi+1Ch],eax; call new; add esp,4; cmp eax,ebx; je; mov ecx,[line]; cmp ecx,ebx;
// je; lea edx,[ecx+14h]; jmp; xor edx,edx; mov ecx,[input]; push edx; push ecx; mov ecx,eax; call; mov [list],eax
constexpr char kMade[] = "68 24 22 00 00 89 46 1C E8 ?? ?? ?? ?? 83 C4 04 3B C3 74 ?? 8B 0D ?? ?? ?? ?? 3B CB 74 ?? "
                         "8D 51 ?? EB ?? 33 D2 8B 0D ?? ?? ?? ?? 52 51 8B C8 E8 ?? ?? ?? ?? A3";
constexpr std::ptrdiff_t kLineAt = 22;    // the global holding the chat line
constexpr std::ptrdiff_t kContextAt = 32; // the context's offset in the line (a byte)
constexpr std::ptrdiff_t kListAt = 53;    // the global holding the list

// The list (FsCandidateMenu): its window (CTkMenuCtrlData, +3Ah the rect: left, top, right, bottom), the two contexts it
// may be made with (the one it asks is the first that isn't null), and its height, kept from its window at its making.
// Each frame its calc (FUN_1018de60) puts the window at (context's left + context's caret offset, context's top) and
// sizes it; its update (FUN_1018dad0) then pulls it left if its right is past the interface's right - 16.
constexpr std::ptrdiff_t kListCtrl = 8;
constexpr std::ptrdiff_t kListContexts[2] = {0x2218, 0x221C};
constexpr std::ptrdiff_t kListHeight = 0x2222;
constexpr int kGameHeight = 134; // its height, if that can't be read

// The chat line's context (its vtable, FFXiMain 0x103358E8): [0] the left (FUN_101b6c30: 15, the line's text's left - 1),
// [1] the top (FUN_101b6c40: the chat log's bottom - 157, so the list's bottom is the line's top - 1), [2] the caret's
// offset (FUN_101b6cd0: how wide the line's text is up to where the phrase starts). __thiscall, no arguments.
using ContextCall = int(__fastcall*)(void* context, void* edx);
constexpr int kTopSlot = 1, kOffsetSlot = 2;
ContextCall g_left = nullptr;   // the game's, called as it is
ContextCall g_top = nullptr;    // the game's, through the hooks
ContextCall g_offset = nullptr;

char** g_line = nullptr;
char** g_list = nullptr;
std::ptrdiff_t g_context = 0;
bool g_found = false;
bool g_hooked = false;
bool g_said = false;

// Where the list goes while fancychat's bar stands for the line, in the interface's units: its left at the phrase's
// column in the bar, and its bottom on the bar's top (under the bar when there's no room above)
struct Place {
    bool on = false;
    int left = 0, top = 0;
};
Place g_place;

char* list_object();
int list_height(const char* list);

// The chat line's window while it's open and on the screen: the game puts the list over the line at the line's own place
// (its context's left is a fixed 15, its top the chat log's bottom - 157), so a line put elsewhere ([client] input.move,
// reported 2026-11-06) left the list behind. The list goes with the line's window instead: its left moved as far as the
// window is from where the game made it (+42h, the layout's rectangle), its bottom a pixel over the window's top (under it
// when there's no room above), as the game has it over its own line. The window is the line's only while its +0Ch is the
// line (chat.cpp's place_input checks the same); one put off the screen (input.hide) leaves the game's place.
struct LineAt {
    bool on = false;
    int shift = 0, top = 0;
};

LineAt line_at(void* context)
{
    LineAt at;
    int area_left = 0, area_top = 0, area_right = 0, area_bottom = 0;
    if (!uispace::area(area_left, area_top, area_right, area_bottom))
        return at;
    short rect[4] = {}, made_left = 0;
    __try {
        char* line = static_cast<char*>(context) - g_context;
        const char* ctrl = *reinterpret_cast<char* const*>(line + 8);
        if (ctrl == nullptr || *reinterpret_cast<char* const*>(ctrl + 0xC) != line)
            return at;
        std::memcpy(rect, ctrl + 0x3A, sizeof rect);
        made_left = *reinterpret_cast<const short*>(ctrl + 0x42);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return at;
    }
    if (rect[2] <= area_left || rect[0] >= area_right || rect[3] <= area_top || rect[1] >= area_bottom)
        return at;
    const char* list = list_object();
    const int list_h = list != nullptr ? list_height(list) : kGameHeight;
    int top = rect[1] - 1 - list_h;
    if (top < area_top)
        top = rect[3] + 1;
    if (top + list_h > area_bottom)
        top = area_bottom - list_h;
    at.shift = rect[0] - made_left;
    at.top = top;
    at.on = true;
    return at;
}

int __fastcall top_hook(void* context, void* edx)
{
    if (g_place.on)
        return g_place.top;
    const LineAt at = line_at(context);
    return at.on ? at.top : g_top(context, edx);
}

int __fastcall offset_hook(void* context, void* edx)
{
    if (g_place.on)
        return g_place.left - g_left(context, edx);
    const LineAt at = line_at(context);
    return g_offset(context, edx) + (at.on ? at.shift : 0);
}

// The chat line's text as it's drawn (the chat manager's GetDisplayText, vtable 0x10335950 slot 4, FUN_101b3bd0; kept at
// +EC34h too, which Ashita's GetInputTextDisplay reads): the typed text with the game's 7F codes. While the list is open
// (+8C81h, FUN_101b55f0) the phrase it offers is in it at the caret, an auto-translate one as 7F FF (the caret) 7F 66 (its
// colour) phrase ... 7F 64 7F FF (its end). The line's draw (FUN_101b6dc0) puts the caret, a '|', at the first 7F FF and
// passes over the rest and over 7F FE, so the caret stood before the phrase (reported 2026-11-06). That first 7F FF is made
// 7F FE, which the game itself puts before a plain-text phrase with the caret after it (FUN_101b5a50): the caret is then
// drawn at the phrase's end. Everything else reads 7F FE as it read the 7F FF there: the line's scrolling (FUN_1018ed40)
// and the list's place (the context's caret offset, FUN_101b6cd0) stop at either. Tried live first.
using DisplayCall = int(__fastcall*)(void* manager, void* edx, char* out, int size);
constexpr int kDisplaySlot = 4;
constexpr std::ptrdiff_t kListOpen = 0x8C81;
constexpr std::ptrdiff_t kDisplayKept = 0xEC34;
constexpr int kDisplayMost = 0x41A;
DisplayCall g_display = nullptr;
bool g_display_tried = false;

// `text`'s (`length` bytes) caret before the offered phrase made 7F FE, when the phrase's end (7F FF) follows
void caret_after(unsigned char* text, int length)
{
    for (int i = 0; i + 3 < length; ++i) {
        if (text[i] != 0x7F || text[i + 1] != 0xFF || text[i + 2] != 0x7F || text[i + 3] != 0x66)
            continue;
        for (int j = i + 4; j + 1 < length; ++j)
            if (text[j] == 0x7F && text[j + 1] == 0xFF) {
                text[i + 1] = 0xFE;
                return;
            }
        return;
    }
}

void caret_after_phrase(char* manager, char* out, int length)
{
    __try {
        if (manager[kListOpen] == 0)
            return;
        caret_after(reinterpret_cast<unsigned char*>(out), length < kDisplayMost ? length : kDisplayMost);
        char* kept = manager + kDisplayKept;
        caret_after(reinterpret_cast<unsigned char*>(kept), static_cast<int>(strnlen(kept, kDisplayMost)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

int __fastcall display_hook(void* manager, void* edx, char* out, int size)
{
    const int length = g_display(manager, edx, out, size);
    if (out != nullptr && length > 0)
        caret_after_phrase(static_cast<char*>(manager), out, length < size ? length : size);
    return length;
}

// The display's hook goes in once the chat manager is made (at the first login)
void hook_display()
{
    const std::uintptr_t manager = chat::input_object();
    if (manager == 0)
        return;
    g_display_tried = true;
    const bool hooked = imports::hook_vtable(reinterpret_cast<void*>(manager), kDisplaySlot, &display_hook, g_display);
    log::write(hooked ? L"Phrase list: the caret is drawn after the phrase the auto-translate list offers"
                      : L"Phrase list: the chat line's text couldn't be hooked: the caret stays before an offered phrase");
}

// fancychat's input bar (lib/render.lua): its plate (position, size; its text starts 8 pixels in, a Consolas cell of
// the measured advance a glyph) and the first glyph of the line it shows (it scrolls to keep the caret in view). Nothing
// while the bar isn't drawn.
constexpr char kBarAsk[] =
    "local ok, x, y, w, h, glyph, first = pcall(function()\n"
    "    local state = package.loaded['lib.state']\n"
    "    local fcw1 = state and state.fcw and state.fcw[1]\n"
    "    local plate = state and state.ro and state.ro.InputBarBG\n"
    "    local settings = state and state.allSettings\n"
    "    if fcw1 == nil or not fcw1.InputBarActive or plate == nil or plate.settings == nil or settings == nil then return nil end\n"
    "    local glyph = fcw1.InputBarGlyphW\n"
    "    if glyph == nil or glyph <= 0 or fcw1.InputBarGlyphFH ~= settings.InputBarFontHeight then\n"
    "        glyph = (settings.InputBarFontHeight or 0) * 0.59\n"
    "    end\n"
    "    local s = plate.settings\n"
    "    return s.position_x, s.position_y, s.width, s.height, glyph, fcw1.InputBarWinStart or 0\n"
    "end)\n"
    "if ok then return x, y, w, h, glyph, first end";
constexpr int kBarValues = 6;
constexpr float kBarTextInset = 8.0f;

// The chat line's text the game edits, and its caret in it (bytes), in the chat manager (Ashita's [chat.inputtext])
constexpr std::uintptr_t kRawText = 0x7E30 + 0x06C4;
constexpr std::uintptr_t kRawCaret = 0x7E30 + 0x0A58;
constexpr std::uint32_t kRawMost = 0x888 - 0x6C4;

// The glyphs fancychat's bar draws for `raw`, the line's own bytes: auto-translate phrases spelled out in brackets, a
// Shift-JIS character one, colour codes and the game's 7F codes none
int glyphs(std::string_view raw)
{
    const std::string text = chat::parse_auto_translate(raw, true);
    int count = 0;
    for (size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == 0x1E || c == 0x1F || c == 0x7F) {
            i += 2;
            continue;
        }
        i += c == 0xEF || (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC) ? 2 : 1;
        ++count;
    }
    return count;
}

// The line's text up to its caret into `out` (kRawMost bytes): its length, or -1
int before_caret(std::uintptr_t manager, char* out)
{
    __try {
        std::uint32_t caret = *reinterpret_cast<const std::uint32_t*>(manager + kRawCaret);
        caret = caret < kRawMost ? caret : kRawMost;
        const size_t length = strnlen(reinterpret_cast<const char*>(manager + kRawText), caret);
        std::memcpy(out, reinterpret_cast<const char*>(manager + kRawText), length);
        return static_cast<int>(length);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// The glyphs before the line's caret: the game puts the phrase it offers there
bool caret_glyphs(int& count)
{
    const std::uintptr_t manager = chat::input_object();
    char before[kRawMost];
    const int length = manager != 0 ? before_caret(manager, before) : -1;
    if (length < 0)
        return false;
    count = glyphs(std::string_view(before, static_cast<size_t>(length)));
    return true;
}

char* list_object()
{
    __try {
        return g_list != nullptr ? *g_list : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

int list_height(const char* list)
{
    __try {
        const short height = *reinterpret_cast<const short*>(list + kListHeight);
        return height >= 16 && height <= 1000 ? height : kGameHeight;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kGameHeight;
    }
}

void find()
{
    g_found = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t made = game != nullptr ? scan::find(game, kMade) : 0;
    if (made == 0 || !uispace::find(game)) {
        log::write(L"Phrase list: FFXI's auto-translate list wasn't found in FFXiMain.dll: it stays where the game puts it, "
                   L"under addons' drawing");
        return;
    }
    g_line = *reinterpret_cast<char***>(made + kLineAt);
    g_context = *reinterpret_cast<const std::uint8_t*>(made + kContextAt);
    g_list = *reinterpret_cast<char***>(made + kListAt);
}

// The hooks of the line's context go in once the line and its list are made (at the first login), and the list is
// seen to ask that context
void hook()
{
    char* list = list_object();
    char* line = nullptr;
    __try {
        line = *g_line;
        if (list == nullptr || line == nullptr)
            return;
        void* context = line + g_context;
        if (*reinterpret_cast<void**>(list + kListContexts[0]) != context &&
            *reinterpret_cast<void**>(list + kListContexts[1]) != context)
            return;
        g_left = reinterpret_cast<ContextCall>((*static_cast<void***>(context))[0]);
        g_hooked = imports::hook_vtable(context, kTopSlot, &top_hook, g_top) &&
                   imports::hook_vtable(context, kOffsetSlot, &offset_hook, g_offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_hooked = false;
    }
    if (!g_said) {
        g_said = true;
        log::write(g_hooked ? L"Phrase list: the auto-translate list follows the chat line's window (or fancychat's input bar)"
                            : L"Phrase list: the auto-translate list's place couldn't be hooked: it stays where the game puts it");
    }
    if (!g_hooked)
        g_line = nullptr; // not tried again
}

// The list at a line drawn by an addon or a plugin: its caret at x, the line from top to bottom (the game's pixels)
bool place_at(double x, double line_top, double line_bottom, unsigned width, unsigned height, Place& place)
{
    float left = 0, bar_top = 0, unused = 0, bar_bottom = 0;
    int area_left = 0, area_top = 0, area_right = 0, area_bottom = 0;
    if (!uispace::to_units(static_cast<float>(x), static_cast<float>(line_top), width, height, left, bar_top) ||
        !uispace::to_units(static_cast<float>(x), static_cast<float>(line_bottom), width, height, unused, bar_bottom) ||
        !uispace::area(area_left, area_top, area_right, area_bottom))
        return false;
    const char* list = list_object();
    const int list_h = list != nullptr ? list_height(list) : kGameHeight;
    // as the game has it over its own line: the list's bottom a pixel over the line's top
    int top = static_cast<int>(std::floor(bar_top)) - 1 - list_h;
    if (top < area_top)
        top = static_cast<int>(std::ceil(bar_bottom)) + 1;
    if (top + list_h > area_bottom)
        top = area_bottom - list_h;
    place.left = static_cast<int>(std::lround(left));
    place.top = top;
    place.on = true;
    return true;
}

// fancychat's bar, from its Lua, as the list's place
bool bar_place(unsigned width, unsigned height, Place& place)
{
    double bar[kBarValues] = {};
    int column = 0;
    if (addons::ask_numbers("fancychat", kBarAsk, bar, kBarValues) < kBarValues || bar[2] <= 0 || bar[3] <= 0 ||
        !caret_glyphs(column))
        return false;
    const double plate_x = bar[0], plate_y = bar[1], plate_w = bar[2], plate_h = bar[3], glyph = bar[4], first = bar[5];
    double x = plate_x + kBarTextInset + (column - first) * glyph;
    x = x < plate_x ? plate_x : x > plate_x + plate_w - kBarTextInset ? plate_x + plate_w - kBarTextInset : x;
    return place_at(x, plate_y, plate_y + plate_h, width, height, place);
}

} // namespace

void on_frame(unsigned width, unsigned height)
{
    if (!g_found)
        find();
    if (g_line != nullptr && !g_hooked)
        hook();
    if (!g_display_tried)
        hook_display();
    Place place;
    if (g_hooked && chat::input_state() == 4) {
        float caret_x = 0, caret_top = 0, caret_bottom = 0;
        if (chat::input_bar_addon())
            bar_place(width, height, place);
        else if (pluginapi::caret(caret_x, caret_top, caret_bottom)) // a plugin's line (VanaCore's plugin API)
            place_at(caret_x, caret_top, caret_bottom, width, height, place);
    }
    g_place = place;
}

void on_draw(const void* ctrl)
{
    if (g_list == nullptr || ctrl == nullptr)
        return;
    short rect[4] = {};
    __try {
        const char* list = *g_list;
        if (list == nullptr || *reinterpret_cast<const void* const*>(list + kListCtrl) != ctrl ||
            *reinterpret_cast<const void* const*>(static_cast<const char*>(ctrl) + 0xC) != list)
            return;
        const short* drawn = reinterpret_cast<const short*>(static_cast<const char*>(ctrl) + 0x3A);
        for (int i = 0; i < 4; ++i)
            rect[i] = drawn[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    raise::add(rect);
}

} // namespace vanacore::phraselist
