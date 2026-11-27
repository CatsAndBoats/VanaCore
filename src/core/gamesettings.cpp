#include "gamesettings.hpp"

#include <windows.h>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

#include "configmenu.hpp"
#include "dats.hpp"
#include "ini.hpp"
#include "log.hpp"
#include "menufont.hpp"
#include "pivot.hpp"
#include "scan.hpp"
#include "soundmix.hpp"

namespace vanacore::gamesettings {
namespace {

// ---- the game's Gameplay window (FFXiMain 2025-12-26, ROM\119\51.DAT; read live 2026-11-23) ----
//
// Its layout, "menu    conf2win": a window 366 x 296 whose one picture is "menu    windowps" sprite 465 (the panel: its
// background, title, bullets, captions, Min and Max, the buttons' wells and the sliders' tracks, 158 parts), and eleven
// items: 1 and 2 Auto target ON and OFF, 3 Sound Effects Volume and 4 Music Volume (sliders 192 wide, 32 apart), 5 to 7
// Gamma R, G and B, 8 and 9 Inventory Sort ON and OFF, 10 and 11 Type 1 and 2. What each does is the handler's (its table
// below), by the item's number: the game's items keep it. A layout payload is configmenu.cpp's: its name (16), 1, the count
// of blocks after the window's, to +0x20; then blocks: +0 their size, +2 x, +4 y, +0xA width, +0xC height; the window's
// +0x14 its count of pictures; an item's +0x12 its number, +0x15 the items before, after, up, down, right and left of it,
// +0x1B its count of pictures, +0x1D and +0x1E its strings' lengths; at +0x20 the pictures (20 bytes each: a slot, the
// sprite's number, the sheet's name), then the strings (the help line's and the title's, numbers of the game's texts).
constexpr char kPage[17] = "menu    conf2win";
constexpr int kGameItems = 11;
constexpr int kSoundFirst = 3, kSoundLast = 4; // Sound Effects Volume and Music Volume: the new rows go under the second
constexpr std::size_t kHeader = 0x20, kCountAt = 0x11;
constexpr std::size_t kBlockX = 2, kBlockY = 4, kBlockWidth = 0xA, kBlockHeight = 0xC;
constexpr std::size_t kWindowPictures = 0x14;
constexpr std::size_t kItemId = 0x12, kItemLinks = 0x15, kItemPictures = 0x1B, kItemStrings = 0x1D;
constexpr std::size_t kPictures = 0x20, kPictureSize = 20, kPictureNumber = 2, kPictureSheet = 4;
enum Link { kBefore, kAfter, kUp, kDown, kRight, kLeft };
constexpr std::size_t kPayloadMax = 0x800;
constexpr std::size_t kNoStrings = 6; // "-1", "-1": no help line or title from the game's texts (ours is set as it's drawn)

// Where the game's menus keep a window's layout (+4), the item its cursor is on (+0x4C) and whether the cursor moves and
// keys go on to the items (+0x62, +0x85: the game clears both while one of its sliders moves); a layout's parse (+8, whose
// first is the window block in the payload) and name (+0x46); a list's node (+0 the next, +0x10 its item, +0x14 skipped);
// a handler's window (+8, 0 while it has none); a sheet's name (+0x24).
constexpr std::ptrdiff_t kWindowLayout = 4, kWindowCursor = 0x4C, kWindowMoves = 0x62, kWindowKeys = 0x85;
constexpr std::ptrdiff_t kLayoutParse = 8, kLayoutName = 0x46;
constexpr std::ptrdiff_t kNodeNext = 0, kNodeItem = 0x10, kNodeSkip = 0x14;
constexpr std::ptrdiff_t kHandlerWindow = 8, kSheetName = 0x24, kPartHasHelp = 0x4A, kPartHelp = 0x4C;
constexpr std::ptrdiff_t kLayouts = 0x1C;
constexpr int kNodesMost = 65536;
constexpr int kEventRight = 3, kEventLeft = 4, kEventSelect = 5, kEventCancel = 6; // a window's input (1 down, 2 up)

// The menus' table entry: the name, 16 zero bytes, the global holding the handler (+0x20)
constexpr char kEntry[] = "6D 65 6E 75 20 20 20 20 63 6F 6E 66 32 77 69 6E 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00";
constexpr std::ptrdiff_t kEntryGlobal = 0x20;
// The handler's table (0x10334980, set by 0x10195A50): 17 slots, then a null. 4 is drawn each frame its window is
// (FUN_10195ac0: push esi; mov esi,ecx; push 2; push 1; mov eax,[esi+2Ch]; push eax; call; ...; each slider's fill, the
// first's call at +0x27), 6 is the input (FUN_10195bc0, ret 8: the event, the item: push ebx; push esi; mov esi,ecx; push
// edi; mov al,[esi+20h]; test al,al; jnz <a slider moving>; ...; after a slider's step at +0x22A: mov edi,[esi+8];
// mov ecx,edi; call <the cursor drawn again: the window, the item, 0>; mov ecx,edi; call <the help line>)
constexpr int kSlots = 17, kDrawSlot = 4, kInputSlot = 6;
constexpr std::uint8_t kDrawStart[] = {0x56, 0x8B, 0xF1, 0x6A, 0x02, 0x6A, 0x01, 0x8B, 0x46, 0x2C, 0x50, 0xE8};
constexpr std::ptrdiff_t kDrawFillCall = 0x27;
constexpr std::uint8_t kInputStart[] = {0x53, 0x56, 0x8B, 0xF1, 0x57, 0x8A, 0x46, 0x20, 0x84, 0xC0, 0x0F, 0x85};
constexpr std::ptrdiff_t kInputStep = 0x22A, kStepCursorCall = 5, kStepHelpCall = 0xC;
constexpr std::uint8_t kStep[] = {0x8B, 0x7E, 0x08, 0x8B, 0xCF, 0xE8};
// A slider's fill (FUN_101952d0, __thiscall, ret 14h: the item, moving, its value, its value while moving, the most; nothing
// at 0): mov al,[esp+8]; test al,al; jnz
constexpr std::uint8_t kFillStart[] = {0x8A, 0x44, 0x24, 0x08, 0x84, 0xC0, 0x75};

// The menu DAT the panel's parts are read from (pivot's overlay's, when it has one), English only; its chunks: a 4-byte
// name, then 32 bits: the type in the low 7, the size over 16 (header and all) in the 19 above; the payload after 16 bytes.
// A sheet's payload: its name, a count of images and their names (16 each), a count of sprites (16 bits), then each: a
// count of parts, the parts. A part: 61 bytes, 12 shorts (the corners: left, top, right, top, left, bottom, right, bottom;
// the texels' size and place), a byte, the corners' colours, the blending, the image's name.
constexpr wchar_t kMenuDat[] = L"ROM\\119\\51.DAT";
constexpr unsigned kLayoutChunk = 0x30, kSheetChunk = 0x31;
constexpr std::size_t kPartSize = 61, kPartImage = 45;
enum Corner { kX0 = 0, kY0 = 1, kY0b = 3, kY1 = 5, kY1b = 7, kTexW = 8, kTexH = 9, kTexU = 10, kTexV = 11 };
constexpr char kFont[17] = "font    font    ";
constexpr int kWordGap = 5; // between a caption's words, as the game's captions have it
constexpr int kPartsMost = 255;

// VanaCore's sheet: its one sprite, the panel
constexpr char kSheet[17] = "menu    vcgamset";
constexpr int kImagesMost = 32;
constexpr std::size_t kSheetMost = 16 + 1 + kImagesMost * 16 + 2 + 1 + kPartsMost * kPartSize;

// ---- VanaCore's rows ----

struct Row {
    const char* label; // English menu font letters (menufont.hpp)
    const wchar_t* key; // [client]
    const char* help;
};
constexpr Row kRows[] = {
    {"World Sounds Volume", L"sound.world", "How loud the world's and battle's sounds are."},
    {"Interface Sounds Volume", L"sound.interface", "How loud the menus' sounds are: the cursor, select and cancel."},
};
constexpr int kRowCount = static_cast<int>(std::size(kRows));
constexpr int kFirstRow = kGameItems + 1; // the first new slider's item number
constexpr int kMost = 100;                // a slider's range, 0 to 100, as the game's

using Payload = configmenu::Menus::Payload;
using Draw = void(__thiscall*)(void* handler);
using Input = void(__thiscall*)(void* handler, int event, int item);
using Fill = void(__thiscall*)(void* handler, int item, int moving, int value, int moving_value, int most);
using Cursor = void(__thiscall*)(void* window, int item, int zero);

std::wstring g_ini;
bool g_on = false;          // [client] gamesettings.extended
int g_values[kRowCount] = {}; // sound.world, sound.interface

const configmenu::Menus* g_menus = nullptr;
bool g_searched = false, g_found = false, g_failed = false;
ULONGLONG g_next = 0;
char** g_handler = nullptr; // the global holding the window's handler
void* g_table = nullptr;    // its table, the game's
void* g_ours[kSlots + 1] = {};
Draw g_draw = nullptr;
Input g_input = nullptr;
Fill g_fill = nullptr;
Cursor g_cursor = nullptr;

alignas(16) std::uint8_t g_layout[kPayloadMax];   // the window with VanaCore's rows
alignas(16) std::uint8_t g_original[kPayloadMax]; // the game's, as it was: put back when the setting goes off
std::size_t g_original_size = 0;
alignas(16) std::uint8_t g_sheet[kSheetMost];
std::size_t g_sheet_size = 0;
bool g_added = false;
char* g_added_at = nullptr;
bool g_sheet_in = false;

// A slider of VanaCore's moving (its item number; 0 none), its value as it moves, in which window
int g_moving = 0;
int g_moving_value = 0;
char* g_moving_window = nullptr;

template <typename T> T& field(void* base, std::ptrdiff_t offset)
{
    return *reinterpret_cast<T*>(static_cast<char*>(base) + offset);
}

int get16(const std::uint8_t* p)
{
    return static_cast<std::int16_t>(p[0] | p[1] << 8);
}

void set16(std::uint8_t* p, int value)
{
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8);
}

std::uintptr_t call_target(std::uintptr_t call)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), 4);
    return call + 5 + rel;
}

void say_failed(const wchar_t* what)
{
    log::write(L"Game settings: the game's Gameplay window isn't the one VanaCore knows (%ls): it's left as it is "
               L"([client] gamesettings.extended)",
               what);
    g_failed = true;
}

// ---- the game's lists ----

// The layouts of the window's name: the first (the one the game opens, and the one taken off by name), ours, and how many
struct Layouts {
    char* first = nullptr;
    char* ours = nullptr;
    int count = 0;
};

const std::uint8_t* payload_of(char* layout)
{
    const char* parse = field<char*>(layout, kLayoutParse);
    const auto* block = parse != nullptr ? *reinterpret_cast<const std::uint8_t* const*>(parse) : nullptr;
    return block != nullptr ? block - kHeader : nullptr;
}

Layouts layouts()
{
    Layouts out;
    char* node = field<char*>(g_menus->manager, kLayouts);
    for (int n = 0; node != nullptr && n < kNodesMost; ++n, node = field<char*>(node, kNodeNext)) {
        if (field<std::uint8_t>(node, kNodeSkip) != 0)
            continue;
        char* layout = field<char*>(node, kNodeItem);
        if (layout == nullptr || std::memcmp(layout + kLayoutName, kPage, 16) != 0)
            continue;
        out.first = out.first != nullptr ? out.first : layout;
        out.ours = payload_of(layout) == g_layout ? layout : out.ours;
        ++out.count;
    }
    return out;
}

// A window open (or closing) from `layout`
bool open_from(const char* layout)
{
    char* node = field<char*>(g_menus->manager, 0);
    for (int n = 0; node != nullptr && n < kNodesMost; ++n, node = field<char*>(node, kNodeNext)) {
        char* window = field<char*>(node, kNodeItem);
        if (window != nullptr && field<char*>(window, kWindowLayout) == layout)
            return true;
    }
    return false;
}

bool sheet_listed()
{
    char* node = field<char*>(g_menus->sheets, 0);
    for (int n = 0; node != nullptr && n < kNodesMost; ++n, node = field<char*>(node, kNodeNext)) {
        const char* sheet = field<std::uint8_t>(node, kNodeSkip) == 0 ? field<char*>(node, kNodeItem) : nullptr;
        if (sheet != nullptr && std::memcmp(sheet + kSheetName, kSheet, 16) == 0)
            return true;
    }
    return false;
}

void list_call(Payload function, void* list, const void* payload)
{
    function(list, &payload);
}

// ---- the game's window read ----

struct Page {
    std::size_t block[kGameItems + 1] = {}; // 0 the window's, 1 to 11 the items'
    std::size_t size = 0;
    char sheet[16] = {}; // the window's panel: its sheet and sprite
    int sprite = 0;
    int pitch = 0; // between the Sound sliders
    int cut = 0;   // the bottom of the last: what's lower moves down
    int below = 0; // the item down from it
};

const std::uint8_t* item_block(const std::uint8_t* p, const Page& page, int id)
{
    return p + page.block[id];
}

const wchar_t* read_blocks(const std::uint8_t* p, std::size_t room, Page& page)
{
    if (room < kHeader || std::memcmp(p, kPage, 16) != 0 || p[0x10] != 1 || p[kCountAt] != kGameItems)
        return L"its header";
    std::size_t offset = kHeader;
    for (int b = 0; b <= kGameItems; ++b) {
        const std::size_t size = offset + 2 <= room ? static_cast<std::size_t>(p[offset] | p[offset + 1] << 8) : 0;
        if (size < kPictures || size % 16 != 0 || offset + size > room)
            return L"its blocks";
        page.block[b] = offset;
        offset += size;
    }
    page.size = offset;
    for (int id = 1; id <= kGameItems; ++id)
        if (get16(item_block(p, page, id) + kItemId) != id)
            return L"its items";
    if (page.size + kRowCount * 0x40 > kPayloadMax)
        return L"its size";
    return nullptr;
}

const wchar_t* read_page(const std::uint8_t* p, std::size_t room, Page& page)
{
    if (const wchar_t* odd = read_blocks(p, room, page))
        return odd;
    const std::uint8_t* window = p + page.block[0];
    if (window[kWindowPictures] < 1 || get16(window + kPictures) != 0)
        return L"its panel";
    std::memcpy(page.sheet, window + kPictures + kPictureSheet, 16);
    page.sprite = get16(window + kPictures + kPictureNumber);
    const std::uint8_t* first = item_block(p, page, kSoundFirst);
    const std::uint8_t* last = item_block(p, page, kSoundLast);
    page.pitch = get16(last + kBlockY) - get16(first + kBlockY);
    page.cut = get16(last + kBlockY) + get16(last + kBlockHeight);
    page.below = last[kItemLinks + kDown];
    const bool sliders = get16(first + kBlockX) == get16(last + kBlockX) && get16(first + kBlockWidth) == get16(last + kBlockWidth) &&
                         get16(first + kBlockHeight) == get16(last + kBlockHeight) && get16(first + kBlockWidth) > 100 &&
                         first[kItemPictures] == 1 && last[kItemPictures] == 1 && page.pitch > get16(last + kBlockHeight);
    if (!sliders || page.below < 1 || page.below > kGameItems || get16(item_block(p, page, page.below) + kBlockY) < page.cut)
        return L"its sliders";
    return nullptr;
}

// ---- the panel, from the menu DAT ----

struct Span {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

Span chunk(const std::vector<std::uint8_t>& dat, unsigned type, const char* name)
{
    for (std::size_t at = 0; at + 16 <= dat.size();) {
        std::uint32_t info = 0;
        std::memcpy(&info, dat.data() + at + 4, 4);
        const std::size_t length = ((info >> 7) & 0x7FFFF) * 16;
        if (length < 16 || at + length > dat.size())
            break;
        if ((info & 0x7F) == type && length - 16 >= 16 && std::memcmp(dat.data() + at + 16, name, 16) == 0)
            return {dat.data() + at + 16, length - 16};
        at += length;
    }
    return {};
}

// Sprite `number`'s parts in a sheet's payload (count x 61 bytes); empty if it hasn't one
std::vector<std::uint8_t> sprite_parts(Span sheet, int number)
{
    std::size_t at = 16;
    if (at >= sheet.size)
        return {};
    at += 1 + 16 * static_cast<std::size_t>(sheet.data[at]);
    if (at + 2 > sheet.size || number >= (sheet.data[at] | sheet.data[at + 1] << 8))
        return {};
    at += 2;
    for (int s = 0; s < number && at < sheet.size; ++s)
        at += 1 + kPartSize * sheet.data[at];
    if (at >= sheet.size || at + 1 + kPartSize * sheet.data[at] > sheet.size)
        return {};
    return std::vector<std::uint8_t>(sheet.data + at + 1, sheet.data + at + 1 + kPartSize * sheet.data[at]);
}

const wchar_t* read_panel(const Page& page, const std::uint8_t* game, std::vector<std::uint8_t>& parts)
{
    const std::wstring folder = dats::ffxi_folder();
    std::vector<std::uint8_t> dat;
    if (folder.empty() || !dats::read_file(pivot::resolve(folder + kMenuDat), dat))
        return L"the English menu DAT couldn't be read";
    const Span layout = chunk(dat, kLayoutChunk, kPage);
    if (layout.size < page.size || std::memcmp(layout.data, game, page.size) != 0)
        return L"the English menu DAT hasn't the window the game has: another language, or another DAT";
    parts = sprite_parts(chunk(dat, kSheetChunk, page.sheet), page.sprite);
    return parts.empty() ? L"its panel's sprite" : nullptr;
}

// ---- VanaCore's panel ----

std::uint8_t* part(std::vector<std::uint8_t>& parts, std::size_t index)
{
    return parts.data() + index * kPartSize;
}

const std::uint8_t* part(const std::vector<std::uint8_t>& parts, std::size_t index)
{
    return parts.data() + index * kPartSize;
}

void move_down(std::uint8_t* p, int dy, bool whole)
{
    for (const int corner : {kY1, kY1b})
        set16(p + 2 * corner, get16(p + 2 * corner) + dy);
    if (whole)
        for (const int corner : {kY0, kY0b})
            set16(p + 2 * corner, get16(p + 2 * corner) + dy);
}

// A letter of a caption: `model` (one of the game's caption's letters: its colours, blending and image) at x, y
void add_letter(std::vector<std::uint8_t>& out, const std::uint8_t* model, const menufont::Glyph& g, int x, int y)
{
    std::uint8_t p[kPartSize];
    std::memcpy(p, model, kPartSize);
    const int corners[12] = {x, y, x + g.w, y, x, y + g.h, x + g.w, y + g.h, g.w, g.h, g.u, g.v};
    for (int i = 0; i < 12; ++i)
        set16(p + 2 * i, corners[i]);
    out.insert(out.end(), p, p + kPartSize);
}

// The Music Volume row's parts (under the slider above it, to the cut): its caption's letters (above its slider), and the
// rest (the track, Min and Max). The caption's top and left.
struct Model {
    std::vector<std::uint8_t> rest;
    const std::uint8_t* letter = nullptr;
    int left = INT_MAX, top = INT_MAX;
};

Model model_row(const std::vector<std::uint8_t>& game, const std::uint8_t* layout, const Page& page)
{
    Model m;
    const std::uint8_t* above = item_block(layout, page, kSoundFirst);
    const int band = get16(above + kBlockY) + get16(above + kBlockHeight);
    const int slider = get16(item_block(layout, page, kSoundLast) + kBlockY);
    for (std::size_t i = 0; i < game.size() / kPartSize; ++i) {
        const std::uint8_t* p = part(game, i);
        const int top = get16(p + 2 * kY0);
        if (top < band || top >= page.cut)
            continue;
        if (top < slider && std::memcmp(p + kPartImage, kFont, 16) == 0) {
            m.letter = m.letter != nullptr ? m.letter : p;
            m.left = std::min(m.left, get16(p + 2 * kX0));
            m.top = std::min(m.top, top - 1); // a tall letter sits 1 under its caption's top
        } else {
            m.rest.insert(m.rest.end(), p, p + kPartSize);
        }
    }
    return m;
}

// The panel: the game's parts, those under the cut moved down by the new rows (the background, across it, made taller);
// then each new row: the model row's track, Min and Max, and its caption in the font's letters
std::vector<std::uint8_t> make_panel(const std::vector<std::uint8_t>& game, const std::uint8_t* layout, const Page& page)
{
    const int dy = page.pitch * kRowCount;
    std::vector<std::uint8_t> out(game);
    for (std::size_t i = 0; i < out.size() / kPartSize; ++i) {
        std::uint8_t* p = part(out, i);
        if (get16(p + 2 * kY0) >= page.cut)
            move_down(p, dy, true);
        else if (get16(p + 2 * kY1) > page.cut)
            move_down(p, dy, false);
    }
    const Model m = model_row(game, layout, page);
    if (m.letter == nullptr || m.rest.empty())
        return {};
    for (int r = 0; r < kRowCount; ++r) {
        const int dr = page.pitch * (r + 1);
        std::vector<std::uint8_t> rest(m.rest);
        for (std::size_t i = 0; i < rest.size() / kPartSize; ++i)
            move_down(part(rest, i), dr, true);
        out.insert(out.end(), rest.begin(), rest.end());
        int x = m.left;
        for (const char* c = kRows[r].label; *c != '\0'; ++c) {
            const menufont::Glyph* g = menufont::glyph(*c);
            if (g != nullptr)
                add_letter(out, m.letter, *g, x, m.top + dr + g->y);
            x += g != nullptr ? g->advance : kWordGap;
        }
    }
    return out;
}

void sheet_put(const void* bytes, std::size_t size)
{
    std::memcpy(g_sheet + g_sheet_size, bytes, size);
    g_sheet_size += size;
}

// The sheet's payload: its name, its images (each part's, once), one sprite: the panel
bool make_sheet(const std::vector<std::uint8_t>& panel)
{
    const std::size_t count = panel.size() / kPartSize;
    if (count == 0 || count > kPartsMost)
        return false;
    std::vector<std::string> images;
    for (std::size_t i = 0; i < count; ++i) {
        const std::string name(reinterpret_cast<const char*>(panel.data() + i * kPartSize + kPartImage), 16);
        if (std::find(images.begin(), images.end(), name) == images.end())
            images.push_back(name);
    }
    if (images.size() > kImagesMost)
        return false;
    g_sheet_size = 0;
    sheet_put(kSheet, 16);
    const auto image_count = static_cast<std::uint8_t>(images.size());
    sheet_put(&image_count, 1);
    for (const std::string& name : images)
        sheet_put(name.data(), 16);
    const std::uint8_t sprites[3] = {1, 0, static_cast<std::uint8_t>(count)};
    sheet_put(sprites, sizeof sprites);
    sheet_put(panel.data(), panel.size());
    return true;
}

// ---- VanaCore's layout ----

std::size_t copy_block(const std::uint8_t* from, std::uint8_t* to)
{
    const std::size_t size = static_cast<std::size_t>(from[0] | from[1] << 8);
    std::memcpy(to, from, size);
    return size;
}

// A new slider: as the last Sound slider's block (its picture, the game's slider), at row `r`'s place, linked between the
// one before it and the one after, with no strings
std::size_t new_slider(const std::uint8_t* model, const Page& page, int r, std::uint8_t* to)
{
    const std::size_t pictures = kPictures + kPictureSize * model[kItemPictures];
    const std::size_t size = (pictures + kNoStrings + 15) & ~static_cast<std::size_t>(15);
    std::memset(to, 0, size);
    std::memcpy(to, model, pictures);
    set16(to, static_cast<int>(size));
    set16(to + kBlockY, get16(model + kBlockY) + page.pitch * (r + 1));
    const int id = kFirstRow + r;
    set16(to + kItemId, id);
    const auto before = static_cast<std::uint8_t>(r == 0 ? kSoundLast : id - 1);
    const auto after = static_cast<std::uint8_t>(r == kRowCount - 1 ? page.below : id + 1);
    const std::uint8_t links[6] = {before, after, before, after, static_cast<std::uint8_t>(id), static_cast<std::uint8_t>(id)};
    std::memcpy(to + kItemLinks, links, sizeof links);
    to[kItemStrings] = 2;
    to[kItemStrings + 1] = 2;
    std::memcpy(to + pictures, "-1\0-1", kNoStrings);
    return size;
}

// The window as the game's: a row taller for each new one, its panel VanaCore's; the game's items, those under the cut
// moved down, the last Sound slider and the one under it linked to the new ones; then the new sliders. Its size.
std::size_t make_layout(const std::uint8_t* game, const Page& page)
{
    const int dy = page.pitch * kRowCount;
    std::memset(g_layout, 0, sizeof g_layout);
    std::memcpy(g_layout, game, kHeader);
    g_layout[kCountAt] = static_cast<std::uint8_t>(kGameItems + kRowCount);
    std::size_t at = kHeader;
    std::uint8_t* window = g_layout + at;
    at += copy_block(game + page.block[0], window);
    set16(window + kBlockHeight, get16(window + kBlockHeight) + dy);
    set16(window + kPictures + kPictureNumber, 0);
    std::memcpy(window + kPictures + kPictureSheet, kSheet, 16);
    for (int id = 1; id <= kGameItems; ++id) {
        std::uint8_t* block = g_layout + at;
        at += copy_block(item_block(game, page, id), block);
        const bool under = get16(block + kBlockY) >= page.cut;
        if (under)
            set16(block + kBlockY, get16(block + kBlockY) + dy);
        for (const Link link : {kBefore, kUp})
            if (under && block[kItemLinks + link] == kSoundLast)
                block[kItemLinks + link] = static_cast<std::uint8_t>(kFirstRow + kRowCount - 1);
        if (id == kSoundLast)
            block[kItemLinks + kAfter] = block[kItemLinks + kDown] = static_cast<std::uint8_t>(kFirstRow);
    }
    for (int r = 0; r < kRowCount; ++r)
        at += new_slider(item_block(game, page, kSoundLast), page, r, g_layout + at);
    return at;
}

// ---- putting it in and taking it out ----

// The game's window's payload and how long it is (its DAT chunk's length, from the chunk's header 12 bytes before it), or
// the copy VanaCore put back
const std::uint8_t* game_payload(char* layout, std::size_t& room)
{
    const std::uint8_t* from = payload_of(layout);
    if (from == nullptr || from == g_original) {
        room = g_original_size;
        return from;
    }
    std::uint32_t info = 0;
    std::memcpy(&info, from - 12, 4);
    const std::size_t length = ((info >> 7) & 0x7FFFF) * 16;
    room = (info & 0x7F) == kLayoutChunk && length > 16 ? length - 16 : 0;
    return room > 0 && room <= kPayloadMax ? from : nullptr;
}

// What's built from the game's window: the sheet and the layout. Its reason when it can't be.
const wchar_t* build(const std::uint8_t* from, std::size_t room, Page& page)
{
    if (const wchar_t* odd = read_page(from, room, page))
        return odd;
    std::vector<std::uint8_t> game;
    if (from != g_original) {
        if (const wchar_t* odd = read_panel(page, from, game))
            return odd;
        std::memcpy(g_original, from, page.size);
        g_original_size = page.size;
    } else if (const wchar_t* odd = read_panel(page, g_original, game)) {
        return odd;
    }
    const std::vector<std::uint8_t> panel = make_panel(game, g_original, page);
    if (panel.empty() || !make_sheet(panel))
        return L"its panel: more parts than a sprite holds, or no Music Volume caption";
    make_layout(g_original, page);
    return nullptr;
}

void put_in(const Layouts& found)
{
    if (found.first == nullptr || open_from(found.first))
        return; // the menus aren't loaded, or the window is open: at a later look
    if (found.count != 1)
        return; // taken off by name: with two, the game's might not be the one (at a later look)
    std::size_t room = 0;
    const std::uint8_t* from = game_payload(found.first, room);
    Page page;
    if (from == nullptr) {
        say_failed(L"its DAT chunk");
        return;
    }
    if (const wchar_t* odd = build(from, room, page)) {
        say_failed(odd);
        return;
    }
    list_call(g_menus->sheet_add, g_menus->sheets, g_sheet);
    g_sheet_in = sheet_listed();
    if (!g_sheet_in) {
        say_failed(L"VanaCore's sheet wasn't taken");
        return;
    }
    list_call(g_menus->layout_remove, g_menus->manager, kPage);
    if (layouts().first == nullptr)
        list_call(g_menus->layout_add, g_menus->manager, g_layout);
    const Layouts now = layouts();
    if (now.ours == nullptr || now.first != now.ours) {
        if (layouts().first == nullptr)
            list_call(g_menus->layout_add, g_menus->manager, g_original);
        if (layouts().ours == nullptr) {
            list_call(g_menus->sheet_remove, g_menus->sheets, kSheet);
            g_sheet_in = false;
        }
        say_failed(L"its layout wasn't replaced");
        return;
    }
    g_added = true;
    g_added_at = now.ours;
    log::write(L"Game settings: Config > Gameplay extended with %d rows ([client] gamesettings.extended)", kRowCount);
}

void take_out(const Layouts& found)
{
    if (open_from(found.ours) || found.first != found.ours)
        return; // at a later look
    list_call(g_menus->layout_remove, g_menus->manager, kPage);
    if (layouts().first == nullptr)
        list_call(g_menus->layout_add, g_menus->manager, g_original);
    list_call(g_menus->sheet_remove, g_menus->sheets, kSheet);
    g_sheet_in = false;
    g_added = false;
    g_added_at = nullptr;
    log::write(L"Game settings: Config > Gameplay is the game's again ([client] gamesettings.extended=0)");
}

// ---- the handler ----

bool ours_open(char* window)
{
    char* layout = window != nullptr ? field<char*>(window, kWindowLayout) : nullptr;
    return layout != nullptr && payload_of(layout) == g_layout;
}

int row_of(int item)
{
    const int r = item - kFirstRow;
    return r >= 0 && r < kRowCount ? r : -1;
}

void apply_sound()
{
    soundmix::set(g_on ? g_values[0] : kMost, g_on ? g_values[1] : kMost);
}

void save(int r)
{
    if (!g_ini.empty())
        ini::set(g_ini, L"client", kRows[r].key, std::to_wstring(g_values[r]).c_str());
}

// The window's cursor and keys stopped while a slider moves (the game's own sliders do it so), and back
void hold(char* window, bool moving)
{
    field<std::uint8_t>(window, kWindowMoves) = moving ? 0 : 1;
    field<std::uint8_t>(window, kWindowKeys) = moving ? 0 : 1;
}

// An event while one of VanaCore's sliders moves: Right and Left a step (heard at once), select or cancel the end of it
void moving_input(char* window, int event)
{
    const int r = row_of(g_moving);
    if (event == kEventRight || event == kEventLeft) {
        g_moving_value = std::clamp(g_moving_value + (event == kEventRight ? 1 : -1), 0, kMost);
        const int kept = g_values[r];
        g_values[r] = g_moving_value;
        apply_sound();
        g_values[r] = kept;
        g_cursor(window, g_moving, 0);
        g_menus->window_help(window);
    } else if (event == kEventSelect || event == kEventCancel) {
        g_values[r] = g_moving_value;
        save(r);
        apply_sound();
        hold(window, false);
        g_moving = 0;
    }
}

void __fastcall page_input(char* handler, void*, int event, int item)
{
    char* window = field<char*>(handler, kHandlerWindow);
    if (window != g_moving_window)
        g_moving = 0;
    if (ours_open(window)) {
        const auto ev = static_cast<short>(event);
        if (g_moving != 0) {
            moving_input(window, ev);
            return;
        }
        const int r = row_of(static_cast<short>(item));
        if (ev == kEventSelect && r >= 0) {
            g_moving = kFirstRow + r;
            g_moving_value = g_values[r];
            g_moving_window = window;
            hold(window, true);
            return;
        }
    }
    g_input(handler, event, item);
}

// Drawn each frame its window is: as the game draws it, then each new slider's fill and help line (set while the cursor is
// on it already: the line is made again now)
void __fastcall page_draw(char* handler, void*)
{
    g_draw(handler);
    char* window = field<char*>(handler, kHandlerWindow);
    if (window != g_moving_window)
        g_moving = 0;
    if (!ours_open(window))
        return;
    for (int r = 0; r < kRowCount; ++r) {
        const int id = kFirstRow + r;
        const bool moving = g_moving == id;
        g_fill(handler, id, moving ? 1 : 0, g_values[r], moving ? g_moving_value : g_values[r], kMost);
        char* part_of = g_menus->window_part(window, id);
        if (part_of == nullptr)
            continue;
        if (field<std::uint8_t>(part_of, kPartHasHelp) == 1 && field<const char*>(part_of, kPartHelp) == kRows[r].help)
            continue;
        field<std::uint8_t>(part_of, kPartHasHelp) = 1;
        field<const char*>(part_of, kPartHelp) = kRows[r].help;
        if (field<short>(window, kWindowCursor) == id)
            g_menus->window_help(window);
    }
}

void set_table(bool ours)
{
    char* handler = g_handler != nullptr ? *g_handler : nullptr;
    if (handler == nullptr)
        return;
    void*& table = field<void*>(handler, 0);
    if (ours && table == g_table)
        table = g_ours;
    else if (!ours && table == static_cast<void*>(g_ours))
        table = g_table;
}

// ---- finding it all ----

bool in_image(std::uintptr_t address, std::uintptr_t base, std::size_t size)
{
    return address >= base && address < base + size;
}

// The handler's table read and checked, and ours made. `global` holds the handler, made by now.
const wchar_t* find_handler(std::uintptr_t global, std::uintptr_t base, std::size_t size)
{
    const auto* handler = *reinterpret_cast<char* const*>(global);
    const auto table = *reinterpret_cast<const std::uintptr_t*>(handler);
    if (!in_image(table, base, size))
        return L"its handler";
    const auto* slots = reinterpret_cast<const std::uintptr_t*>(table);
    const std::uintptr_t draw = slots[kDrawSlot], input = slots[kInputSlot];
    if (slots[kSlots] != 0 || !in_image(slots[kSlots - 1], base, size) || !in_image(draw, base, size) || !in_image(input, base, size) ||
        std::memcmp(reinterpret_cast<const void*>(draw), kDrawStart, sizeof kDrawStart) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(input), kInputStart, sizeof kInputStart) != 0)
        return L"its handler's table";
    const std::uintptr_t fill = call_target(draw + kDrawFillCall);
    const std::uintptr_t step = input + kInputStep;
    if (*reinterpret_cast<const std::uint8_t*>(draw + kDrawFillCall) != 0xE8 || !in_image(fill, base, size) ||
        std::memcmp(reinterpret_cast<const void*>(fill), kFillStart, sizeof kFillStart) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(step), kStep, sizeof kStep) != 0 ||
        call_target(step + kStepHelpCall) != reinterpret_cast<std::uintptr_t>(g_menus->window_help))
        return L"its sliders' drawing";
    g_handler = reinterpret_cast<char**>(global);
    g_table = reinterpret_cast<void*>(table);
    for (int slot = 0; slot <= kSlots; ++slot)
        g_ours[slot] = reinterpret_cast<void*>(slots[slot]);
    g_ours[kDrawSlot] = reinterpret_cast<void*>(&page_draw);
    g_ours[kInputSlot] = reinterpret_cast<void*>(&page_input);
    g_draw = reinterpret_cast<Draw>(draw);
    g_input = reinterpret_cast<Input>(input);
    g_fill = reinterpret_cast<Fill>(fill);
    g_cursor = reinterpret_cast<Cursor>(call_target(step + kStepCursorCall));
    return nullptr;
}

bool found()
{
    if (g_searched)
        return g_found;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    g_menus = game != nullptr ? configmenu::menus() : nullptr;
    if (g_menus == nullptr)
        return false; // not loaded yet, or the menus aren't as known (configmenu.cpp says)
    const auto base = reinterpret_cast<std::uintptr_t>(game);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew);
    const std::size_t size = nt->OptionalHeader.SizeOfImage;
    static std::uintptr_t global = 0; // looked for once
    if (global == 0) {
        const std::uintptr_t entry = scan::find(base, size, kEntry);
        global = entry != 0 ? scan::operand(entry, kEntryGlobal) : 0;
        if (!in_image(global, base, size)) {
            g_searched = true;
            say_failed(L"its menu");
            return false;
        }
    }
    if (*reinterpret_cast<char* const*>(global) == nullptr)
        return false; // the menus' handlers aren't made yet
    g_searched = true;
    if (const wchar_t* missing = find_handler(global, base, size)) {
        say_failed(missing);
        return false;
    }
    g_found = true;
    return true;
}

} // namespace

void reload(const wchar_t* ini)
{
    g_ini = ini != nullptr ? ini : L"";
    g_on = GetPrivateProfileIntW(L"client", L"gamesettings.extended", 0, ini) != 0;
    for (int r = 0; r < kRowCount; ++r)
        g_values[r] = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"client", kRows[r].key, kMost, ini)), 0, kMost);
}

void install(const wchar_t* ini)
{
    reload(ini);
}

void on_frame()
{
    if (g_found)
        set_table(g_on && g_added);
    const ULONGLONG now = GetTickCount64();
    if (now < g_next)
        return;
    g_next = now + 500;
    if (g_moving == 0)
        apply_sound(); // the shares hold whether or not the window could be put in
    if (g_failed || (!g_on && !g_added && !g_sheet_in) || !found())
        return;
    const Layouts lists = layouts();
    if (g_added && lists.ours == nullptr) {
        g_added = false; // the game let its menus go (logging out, a reload): put in again when they're back
        log::write(L"Game settings: the game's menus were let go; the extended Gameplay window goes back in when they're loaded");
    }
    if (lists.ours == nullptr && g_sheet_in && (g_added_at == nullptr || !open_from(g_added_at))) {
        list_call(g_menus->sheet_remove, g_menus->sheets, kSheet);
        g_sheet_in = false;
        g_added_at = nullptr;
    }
    if (g_on && lists.ours == nullptr && !g_sheet_in && !g_failed)
        put_in(lists);
    else if (!g_on && lists.ours != nullptr)
        take_out(lists);
    set_table(g_on && g_added);
}

} // namespace vanacore::gamesettings
