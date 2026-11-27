#include "hudwin.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

#include "asm_text.hpp"
#include "buttonspace.hpp"
#include "chat.hpp"
#include "equipstats.hpp"
#include "imports.hpp"
#include "itemlist.hpp"
#include "itemslot.hpp"
#include "itemtips.hpp"
#include "log.hpp"
#include "outline.hpp"
#include "partyrows.hpp"
#include "scan.hpp"
#include "statuswindow.hpp"
#include "../gfx/vanacore_gfx.h"

namespace vanacore::hudwin {
namespace {

// The windows are the menu manager's (CTkMenuMng, a global in FFXiMain.dll), each found by its layout's name, 16
// characters at the layout's +0x46, a window's +4 being its layout, as castbar.cpp finds the cast bar. Each is put before
// it's drawn, so the frame the game makes one again (a zone change, a party change) shows it in its place too.
constexpr std::ptrdiff_t kWindowLayout = 4;
constexpr std::ptrdiff_t kLayoutName = 0x46;

// The manager's global, from the window that has the help line's text (FUN_101182a0), and SetLocate (FUN_10118d10:
// thiscall window, x, y, ret 8); see targetwin.cpp for both.
constexpr char kHelp[] = "53 55 56 8B E9 57 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 3B C5 0F 85 ?? ?? ?? ?? 66 8B 45 4C 8B CD 50 E8";
constexpr std::ptrdiff_t kManagerAt = 7;
constexpr char kSetLocate[] = "53 55 8B 6C 24 18 56 8B F1 57 33 FF 8D 5E 14 66 89 6E 52 8B CB 66 89 46 54";
constexpr std::ptrdiff_t kSetLocatePrologue = 8, kResizeAt = 0x6C;

// The manager: the area its windows are laid out in (left, top, right, bottom, 16 bits each) at +0x7C. A window:
// +0x3A..+0x40 its frame (left, top, right, bottom).
constexpr std::ptrdiff_t kAreaLeft = 0x7C;
constexpr std::ptrdiff_t kWinFrame = 0x3A;
// A window's +0x38: its area (a word); the main menu and the menus it opens in its place are 0x81 (alliancelist.cpp)
constexpr std::ptrdiff_t kWinArea = 0x38;
constexpr std::uint16_t kMenuArea = 0x81;
// A window's +0x10: its state, 0xE once it's closed (DestroyDrawMenu; alliancelist.cpp)
constexpr std::ptrdiff_t kWinState = 0x10;
constexpr std::uint32_t kWinClosed = 0xE;
// A window's +0x34: its flags; 0x1000 placed by the layout over the window it goes with (the lists the Commands menu opens
// over the chat, an NPC's selection menu: itemlist.cpp's kOverAnchor)
constexpr std::ptrdiff_t kWinFlags = 0x34;
constexpr std::uint32_t kWinOverAnchor = 0x1000;

// The window draw (FUN_101173D0, chat.cpp's ctrl_draw_hook) starts with the window's calc (FUN_10117120: __thiscall
// window; its close timers, and its delete once they're over) and counts its input delay (+0x56) down. push esi; push edi;
// mov ebp,ecx; call; mov ax,[ebp+56h]; xor edi,edi; cmp ax,di; jle; dec eax; mov [ebp+56h],ax: matched past the 5 bytes
// chat.cpp's detour puts over its start
constexpr char kWinDraw[] = "56 57 8B E9 E8 ?? ?? ?? ?? 66 8B 45 56 33 FF 66 3B C7 7E 05 48 66 89 45 56";
constexpr std::ptrdiff_t kWinCalcCall = 4;
constexpr std::ptrdiff_t kWinInputDelay = 0x56;
using WinCalc = void(__thiscall*)(void* window);
// The manager's DestroyDrawMenu (FUN_1015D4C0, __thiscall manager: window, wait): push esi; mov esi,[esp+8]; push edi;
// mov edi,ecx; test esi,esi; je; mov al,[esi+6Eh]; test al,al; jne; mov ecx,esi; mov byte ptr [esi+6Eh],1; call. It marks
// the window destroyed (+0x6E) and closes it; the manager deletes it once its frame has gone
constexpr char kDestroyMenu[] = "56 8B 74 24 08 57 8B F9 85 F6 74 ?? 8A 46 6E 84 C0 75 ?? 8B CE C6 46 6E 01 E8";
constexpr std::ptrdiff_t kWinDestroyed = 0x6E;
using DestroyMenu = void(__thiscall*)(void* manager, void* window, int wait);

using SetLocate = void(__thiscall*)(void* window, int x, int y);
// The game's resize (0x1011A1A0, the call at SetLocate+0x6C: __thiscall window, x, y, w, h, 1, 0, 0), the frame stored as
// it's given and kept, as chat.cpp moves the chat with it. The help line's "Help" caption is drawn at its right edge
// each frame, so it goes with it (seen live 2026-10-21).
using Resize = void(__thiscall*)(void* window, int x, int y, int w, int h, char a, char b, char c);

// Menu Content (menucontent.*): the windows the game opens at the interface's top left, Status, Equipment, Inventory,
// Quests, an item's description... A window is made from its layout by SetResourceData (FUN_10117b40: thiscall window,
// layout; ret 4), which places its frame part with FUN_10118FF0 (thiscall part, x, y, scaled; ret 0Ch, found 2026-10-21:
// the part's place at +0x1A/+0x1C, x and y added to the layout's, then moved by the edges of the area its layout is
// anchored to, in a 512 x 448 space; the layout's place, without x and y, at +0x1E/+0x20; its size at +0x22/+0x24) and
// goes on from there: its place as opened, its parts, and what its handler works out once (where Equipment's numbers
// are drawn) all come from that place. So one placed in the area's top left 512 x 448 has both places moved by as much
// as Menu Content's box is from the area's top left (kept in the area), as if its layout were there. The Equipment
// screen then puts Status, Inventory and an item's description at places of its own in the top left with SetLocate
// (16, 48; 314, 48; 130, 240): itemlist.cpp's hook of it asks locate, which moves those by as much. Moved after opening
// instead, Equipment's numbers stayed behind. Only in the world (the manager's phase, hideui.cpp's), never the windows
// other modules move (the HUD Manager's own above, the Target window, the cast bar) or the lobby's (laid out in
// proportion, +0x99). The windows already open stay where they are: the place counts from the next one opened.
// push ebx; push ebp; push esi; mov esi,ecx; mov ecx,[esp+10h]; push edi; mov [esi+4],ecx; call
constexpr char kSetResource[] = "53 55 56 8B F1 8B 4C 24 10 57 89 4E 04 E8";
constexpr size_t kSetResourcePrologue = 5;
// sub esp,14h; push esi; mov esi,ecx; push edi; mov eax,[esi+30h]; test eax,eax; jz
constexpr char kPlaceFrame[] = "83 EC 14 56 8B F1 57 8B 46 30 85 C0 0F 84";
constexpr size_t kPlaceFramePrologue = 6;
constexpr std::ptrdiff_t kPartX = 0x1A, kPartWidth = 0x22;
constexpr std::ptrdiff_t kWinScaled = 0x99;
constexpr std::ptrdiff_t kManagerPhase = 0x64;
constexpr std::int32_t kWorldPhases[] = {0x60, 0xD0};
constexpr int kDesignW = 512, kDesignH = 448;
// where the game puts the first of them (the Status, Items and Quests lists; the Equipment screen puts Status there):
// under the help bar, a margin in. Menu Content's box is the space from there: its top left corner is the windows'
constexpr int kItemsX = 16, kItemsY = 48;
// the Status screen's places for the Status window and the one under it (status_drop)
constexpr int kStatusY = 16, kUnderStatusY = 208;
// the gap the Level / EXP window's layout (16, 208) leaves under the Status window's (16, 16, 190 tall): where it sits
// under the Status window as it is now (kStatus)
constexpr int kUnderStatusGap = kUnderStatusY - kStatusY - 190;
constexpr int kContentW = kDesignW - kItemsX, kContentH = kDesignH - kItemsY;
constexpr const char* kOwnPlaced[] = {"menu    targetwi", "menu    casttime"};
using SetResource = void(__fastcall*)(char* window, void* edx, char* layout);
using PlaceFrame = void(__fastcall*)(char* part, void* edx, int x, int y, int scaled);

// The Quests window ("menu    quest00", a list of the kind over the chat) lays its buttons out itself once it's made: a
// button's rect asked for (FUN_10116c20: __thiscall on the button's handle, button +54h, the rect; as drawn), its top and
// bottom set to places on the screen (Current and Completed at 62 and 94, Records of Eminence's rows at 158, 190, 222; 100
// and 130 on its other page: the game's place, 48, and below), its left and right kept, and given back (FUN_10116c70, the
// same: SetButtonRect 0x10118580, the rect as drawn). Moved by Menu Content, the window and the buttons' left went and
// their tops stayed (Current and Completed above the window, seen 2026-10-22). Records of Eminence's text (FUN_10231b00,
// each frame) takes a greyed Vana'Bout's place in the window from button 5's left as drawn (54 at the game's place) and
// draws it that far into the window, so moved it went as far again, over "Plaudits :". So while those two pages lay out
// (FUN_1022dd30, FUN_1022e0d0; its third, FUN_1022df60, goes by the window's rect) and while that text is drawn, their
// window's buttons are given as at the game's place and put back as far as the window went (g_shifts).
// mov al,[...]; sub esp,8; push ebx; xor ebx,ebx; push esi; cmp al,bl; mov esi,ecx; jnz; mov eax,[...]; push 1;
// lea ecx,[eax+20h]; call
constexpr char kQuestPage[] = "A0 ?? ?? ?? ?? 83 EC 08 53 33 DB 56 3A C3 8B F1 75 1F A1 ?? ?? ?? ?? 6A 01 8D 48 20 E8";
constexpr size_t kQuestPagePrologue = 5;
// sub esp,8; push esi; push edi; mov esi,ecx; call; xor edi,edi; push 2; push edi (3); mov ecx,esi; call; mov ecx,[esi+8];
// push 3; push 10h; call; mov ecx,[esi+8]; push 4; push 10h; call; push edi; push 2; mov ecx,esi; call; mov ecx,[esi+8];
// push 1; mov dword ptr [esi+1Ch], ...
constexpr char kQuestPage2[] = "83 EC 08 56 57 8B F1 E8 ?? ?? ?? ?? 33 FF 6A 02 57 57 57 8B CE E8 ?? ?? ?? ?? 8B 4E 08 6A 03 6A 10 "
                               "E8 ?? ?? ?? ?? 8B 4E 08 6A 04 6A 10 E8 ?? ?? ?? ?? 57 6A 02 8B CE E8 ?? ?? ?? ?? 8B 4E 08 6A 01 "
                               "C7 46 1C";
constexpr size_t kQuestPage2Prologue = 7;
// sub esp,88h; push ebp; push esi; mov esi,ecx; xor ebp,ebp; mov eax,[esi+8]; cmp eax,ebp; jz; push edi;
// push 80808080h; push 6Ch; push 0Ch (the "Records of Eminence" string)
constexpr char kQuestText[] = "81 EC 88 00 00 00 55 56 8B F1 33 ED 8B 46 08 3B C5 0F 84 ?? ?? ?? ?? 57 68 80 80 80 80 6A 6C 6A 0C";
constexpr size_t kQuestTextPrologue = 6;
// mov eax,[esp+4]; test eax,eax; jz; mov edx,[ecx]; sub edx,0; jz (get 35h, set 31h); dec edx; jz; mov word ptr [eax],0
constexpr char kButtonRectGet[] = "8B 44 24 04 85 C0 74 21 8B 11 83 EA 00 74 35 4A 74 1A 66 C7 00 00 00";
constexpr char kButtonRectSet[] = "8B 44 24 04 85 C0 74 21 8B 11 83 EA 00 74 31 4A 74 1A 66 C7 00 00 00";
constexpr size_t kButtonRectPrologue = 6;
// a handle: its kind (1, a button) and its button; a button's window +5Ch; a list's window +8
constexpr std::ptrdiff_t kHandleKind = 0, kHandleButton = 4, kButtonWindow = 0x5C, kListWindow = 8;
using QuestPage = void(__fastcall*)(char* list, void* edx);
using ButtonRect = void(__fastcall*)(char* handle, void* edx, short* rect);

// The help bar's title box ("menu    titlewin"): its handler (window +0xC; made in 0x1014F800, 0x450 bytes) keeps where
// the page's name is drawn, worked out once as the box opens (its table's slot 1, 0x1014F9A0): the window's rect (its
// rect getter, 0x10118B70: __thiscall window, short[4]*, the frame as drawn) at +0x14, then the text's left and top
// (rect left + 4, top + 8) at +0x9C/+0xA0 and its text object's +0x58/+0x5C (+0xFC/+0x100), and the text's room (rect
// width - 8) at +0x104. Moved after that, the box goes and the name stays, so the same is worked out again here after
// each move (not the slot itself: it also starts the title's animation over).
// push esi; mov esi,ecx; mov ecx,[esi+8]; test ecx,ecx; jz; push edi; lea edi,[esi+14h]; push edi; call <the rect>;
// movsx eax,word ptr [edi]; movsx ecx,word ptr [esi+16h]; add eax,4; add ecx,8; mov [esi+9Ch],eax; mov [esi+0A0h],ecx;
// mov [esi+0FCh],eax; mov [esi+100h],ecx
constexpr char kTitleOpened[] = "56 8B F1 8B 4E 08 85 C9 74 42 57 8D 7E 14 57 E8 ?? ?? ?? ?? 0F BF 07 0F BF 4E 16 83 C0 04 83 C1 08 "
                                "89 86 9C 00 00 00 89 8E A0 00 00 00 89 86 FC 00 00 00 89 8E 00 01 00 00";
constexpr std::ptrdiff_t kTitleRectCall = 0xF;
// The help line (helpwind): its handler (CTkHelp) draws the description at a place of its own, taken from the window's
// rect by its update (slot 5, 0x10139F20, CTkHelp::OnUpdatePrimitive in the PS2 client): the left + 6 and top + 8 at
// +0x14/+0x18 and again at the message's +0x78/+0x7C, and the room for the text, the width - 12, at +0x80. The game runs
// it as the window is made, before it's placed here, so logging the character in again in the same game left the text
// at the game's place beside the moved frame (reported 2026-10-24: the first login's update came after the move). It's
// run again after each move and resize (help_moved). sub esp,8; xor eax,eax; push esi; mov esi,ecx; mov ecx,[esi+8];
// mov dword ptr [esi+14h],6; cmp ecx,eax; mov dword ptr [esi+18h],8; jz; (the rect cleared); lea eax,[esp+4]; push eax;
// call <the window's rect>; movsx eax,[esp+4]; movsx edx,[esp+6]; lea ecx,[eax+6]; add edx,8; mov [esi+14h],ecx;
// mov [esi+18h],edx; mov [esi+78h],ecx; mov [esi+7Ch],edx
constexpr char kHelpUpdated[] = "83 EC 08 33 C0 56 8B F1 8B 4E 08 C7 46 14 06 00 00 00 3B C8 C7 46 18 08 00 00 00 74 ?? "
                                "66 89 44 24 04 66 89 44 24 06 66 89 44 24 08 66 89 44 24 0A 8D 44 24 04 50 E8 ?? ?? ?? ?? "
                                "0F BF 44 24 04 0F BF 54 24 06 8D 48 06 83 C2 08 89 4E 14 89 56 18 89 4E 78 89 56 7C";
constexpr size_t kHelpUpdatedSlot = 5;
using Updated = void(__thiscall*)(void* handler);
// The chat log maximized (fulllog): its handler, the log windows' class (vtable 0x10331B68), works out as it opens
// (slot 1, 0x101371E0) how many rows it shows (+0x58, and again at +0x1C) and its height (+0x5C): 23 rows and 448 units
// on the 512 x 448 layout, one row more for each 16 units taller the interface is, made to reach the chat's bottom. Its
// scroll bar is drawn by them and the scroll stops by them, so fulllog.height's resize sets them for the new height too:
// as it was, the bar ran past its bottom and the top of the log couldn't be scrolled to (seen 2026-10-22: 70 rows and 1136
// kept for a window made 672 tall, which shows 41). sub esp,8; push ebx; push esi; push edi; mov esi,ecx; call;
// mov ax,[esi+58h]; xor ebx,ebx; mov byte ptr [esi+60h],1; mov [esi+64h],ebx; mov [esi+20h],ebx
// The HP gauge (gaugewin): its handler draws its bars and numbers in the rect it took from the window as it opened (+0x1C,
// its slot 1, 0x10138E90), so a moved gauge left them behind (seen 2026-10-22: the frame moved, the bars stayed): it's
// taken again from where the window is (gauge_moved). push esi; push 4; mov esi,ecx; call; add esp,4; test eax,eax; jz;
// mov eax,[eax]; mov ecx,[eax+20h]; mov [esi+14h],ecx; push 1; call; ... mov [esi+18h],eax; lea ecx,[esi+1Ch]; push ecx;
// mov ecx,[esi+8]; call <the window's rect>
constexpr char kGaugeOpened[] = "56 6A 04 8B F1 E8 ?? ?? ?? ?? 83 C4 04 85 C0 74 08 8B 00 8B 48 20 89 4E 14 6A 01 E8 ?? ?? ?? ?? "
                                "83 C4 04 85 C0 74 0B 8B 10 8B 82 84 01 00 00 89 46 18 8D 4E 1C 51 8B 4E 08 E8";
constexpr std::ptrdiff_t kGaugeRect = 0x1C, kGaugeRectCall = 0x39;
// The Linkshell menu ("menu    link4   ": Link List, Unequip, which the main menu's Linkshell opens in its place): its
// handler (vtable 0x10337258) draws the orange Unequip over that button while it's the one chosen (its slot 4,
// 0x101E47F0: the sprite at +0x1C or +0x20 by +0x16, at +0x18/+0x1A), at the place its slot 1 (0x101E46F0) took once as
// it opened: the left and top of its button's (+0x14, 2) rect from the game's button place (0x10118500: __thiscall window,
// short[4]*, button, 1, 1). follow_menu moves the menu after that, so with mainmenu.move on the button went with the
// window and the orange Unequip stayed at the game's place (reported 2026-11-04: drawn at 1808, 70 for the button at
// 1808, 141 of 1920 x 1200; written there live, it sat on the button). It's taken again after the move (link_moved).
// sub esp,0Ch; push ebx; push esi; push 1; mov esi,ecx; call; mov ecx,[eax]; add esp,4; xor ebx,ebx; mov edx,[ecx+1E8h];
// push 1; mov [esi+1Ch],edx; mov eax,[eax]; movsx edx,word ptr [esi+14h]; mov ecx,[eax+0B04h]; push 1; lea eax,[esp+14h];
// mov [esi+20h],ecx; mov ecx,[esi+8]; push edx; push eax
constexpr char kLinkOpened[] = "83 EC 0C 53 56 6A 01 8B F1 E8 ?? ?? ?? ?? 8B 08 83 C4 04 33 DB 8B 91 E8 01 00 00 6A 01 89 56 1C "
                               "8B 00 0F BF 56 14 8B 88 04 0B 00 00 6A 01 8D 44 24 14 89 4E 20 8B 4E 08 52 50";
constexpr std::ptrdiff_t kLinkPlaceCall = 0x4E;
constexpr std::ptrdiff_t kLinkButton = 0x14, kLinkDrawn = 0x18;
using ButtonPlace = void(__thiscall*)(void* window, short* rect, int button, int pressed, int nudge);
constexpr char kLogOpened[] = "83 EC 08 53 56 57 8B F1 E8 ?? ?? ?? ?? 66 8B 46 58 33 DB C6 46 60 01 89 5E 64 89 5E 20";
constexpr std::ptrdiff_t kLogRows = 0x58, kLogRowsShown = 0x1C, kLogHeight = 0x5C;
constexpr std::ptrdiff_t kWinOwner = 0xC, kOwnerWindow = 8;
constexpr std::ptrdiff_t kTitleRect = 0x14, kTitleText = 0x9C, kTitleTextAgain = 0xFC, kTitleRoom = 0x104;
using WindowRect = void(__thiscall*)(void* window, short* rect);

// One window: its id ([client] <id>.*), what it can be (kMove: <id>.move, .x, .y; kHide: <id>.hide; kWidth: <id>.width;
// kHeight: <id>.height; kFollow: with its move off, it goes with the main menu, its right edge and top at the main
// menu's, as the game opens it there; kContent: with its move off, Menu Content moves it as any other top-left window,
// and an item description goes with its list, as out of the table; kFrame: <id>.background, a kFollow window without it
// going by mainmenu.background; kChat: chat.<id>, with its move off, it's pinned to the chat at the chat's left; kStatus: with its
// move off, it sits under the Status window, wherever that is and however tall; kOnTop: with its move on, it's drawn over
// every menu, on a layer of its own: raise_moved), its layouts' names
// (up to 24: the macro bar's palettes, the auction house's menus, the item descriptions), which point of it X and Y are
// (0 its left or top, 1 its right or bottom), and X and Y's defaults (the game's own place, percents of the interface)
enum : unsigned {
    kMove = 1, kHide = 2, kWidth = 4, kFollow = 8, kHeight = 16, kContent = 32, kFrame = 64, kChat = 128, kStatus = 256, kOnTop = 512
};
struct Window {
    const char* id;
    const wchar_t* name;
    unsigned can;
    char layouts[24][16 + 1];
    double anchor_x, anchor_y;
    double default_x, default_y;
    int default_w, default_h; // its size for the box before it's been seen

    // [client] <id>.*
    bool move = false;
    bool hide = false;
    double x = 0, y = 0;
    double width = 0; // a percent of the interface's width, 0 the game's
    double height = 0; // a percent of the interface's height, 0 the game's
    double scale = 1;  // <id>.scale: drawn this many times its size, about its anchor point (hudwin_scale.h)
    bool frame = true; // <id>.background (kFrame; kFollow without kFrame: mainmenu.background)
    bool frame_logged = false;
    bool solid = false; // <id>.solid: without its frame, its buttons drawn solid (kFrame; kFollow without kFrame: mainmenu.solid)
    bool on_chat = false; // chat.<id> (kChat)
    bool over_chat = false; // as last seen, the layout stands it over the chat (kWinOverAnchor): with move off, the Interaction Menu's

    // as last seen: its size, where the game put it (its anchor point, in the interface's units, as first drawn, before it's
    // moved; while only the game places it, as drawn now), and whether it's been put somewhere (to go back when move is off)
    int seen_w = 0, seen_h = 0;
    bool seen_game = false;
    double game_x = 0, game_y = 0;
    int game_w = 0, game_h = 0; // its size as the game made it
    int game_rows = 0;          // the chat log's rows as the game made it (log_resized), 0 for any other
    const char* window = nullptr;
    bool placed = false;
    bool sized = false; // made another size (to go back when width and height are 0)
    bool logged = false;
    bool hide_logged = false;
};

#include "hudwin_windows.hpp" // g_windows: the windows it moves, with their RE notes
constexpr char kItemsName[] = "menu    inventor";
constexpr char kItemInfoName[] = "menu    iteminfo";

// each window placed (place_frame_hook, locate): how far it went from the game's place (0, 0 for not at all), so an
// item description goes as far as the list it's under did, whichever box that list went by (the auction house's Sell
// list went by Menu Content and its description by Inventory, to the screen's far right, 2026-10-21)
std::unordered_map<const char*, std::pair<int, int>> g_shifts;
constexpr size_t kShiftsAtMost = 256;

// menucontent.hidegil: the gil window ("menu    money") left out of the draw while Status is open (the Status and
// Equipment screens put it under Status); a shop's, under its list, still shows
constexpr char kGilName[] = "menu    money   ";
constexpr char kStatusName[] = "menu    persona ";
bool g_hide_gil = false;

// <id>.background=0 (kFrame; the menus the main menu opens in its place, kFollow without kFrame and any other laid out
// there, in_menu_slot, go by mainmenu.background): the window drawn without the layout's pieces, its frame and title, and its owner's drawing (the
// commands) and the cursor as ever. The game's window draw (FUN_101173D0) draws those pieces only while the window's
// +0x69 is set, as alliancelist.cpp's party.background (seen live 2026-10-23). frameless clears it for the one draw and
// frame_back sets it again, so nothing else sees it changed and a switch turned back on shows the frame at once. The
// buttons' bars are added onto what's under them, so without the frame they wash out over the scene (the Action Menu,
// 2026-10-23): with [client] <id>.solid=1 (off by default; HUD Manager > the element > Solid buttons) itemslot.cpp's
// frameless, on for the draw, has them drawn solid.
constexpr std::ptrdiff_t kWinPieces = 0x69;
// The pieces the draw walks: a window's +0x14, a list (node +0 the next, +0x10 the piece, +0x14 set for one taken out); a
// piece's +0 its type (0 the frame or title, 1 a button, 2 the cursor's place), +0x44 1 for a frame the draw passes over.
// A window's +0x66: the wait before a closed one's frame starts going
constexpr std::ptrdiff_t kWinPieceList = 0x14, kNodePiece = 0x10, kNodeOut = 0x14, kPieceUnseen = 0x44;
constexpr std::ptrdiff_t kWinCloseWait = 0x66;
constexpr short kPieceFrame = 0, kPieceButton = 1;
// A frame piece's alpha (a float the draw scales its 128 by) and its fade mode (kPieceUnseen: 1 for one the draw passes
// over), which the game's help bar windows are switched off by
constexpr std::ptrdiff_t kPieceAlpha = 0xC;

// [client] titles.hide=1: the menus' title tabs ("Commands" over the main menu, "Magic", "Linkshell", ...) left out of the
// draw. A title tab is a button piece the game marks Inactive (status 0x10 at the piece's +0x10; greyed buttons are 0x10
// too, so that alone isn't one): the menu manager's CreateDrawMenu (0x1015D1E0, through 0x10118CF0) marks a window's last
// item (its id the window's item count, +0x58) so when the window's menu table flags (+0x34) have 0x200000 (Magic,
// Abilities, the Party, Linkshell, Search, Macro and Config menus, the auction house's, ...: read from the table live
// 2026-11-05), and the main menu's two pages (flags 2) mark their item 15 themselves, the title still with singlemenu.cpp's
// 27 items. The Action Menu (playermo) has none. The window draw (FUN_101173D0) passes over a button whose +0x3C is 0: it's
// cleared for the one draw and set back after (title_back), as frameless does the frame, so nothing else sees it changed
constexpr std::ptrdiff_t kWinCount = 0x58, kPieceStatus = 0x10, kPieceId = 0x3A, kPieceShown = 0x3C;
constexpr std::uint32_t kWinTitled = 0x200000, kButtonInactive = 0x10;
constexpr short kMainMenuTitle = 15;
bool g_hide_titles = false;
// The other titles are in a window's picture, not a button: "Status" over the Status window, "Network" over the Network
// window, the party list's (read live 2026-11-05). A frame piece's picture (YmMenuShape, at the piece's +8: its quads'
// list at +4, their count at +0x14; a node's next at +0, its quad at +0x10) has a "menu    hfr1" tab and
// "menu    menu2fon" letters at its top. A quad's texture is named at +0x32, its corners' places in the window are the
// shorts at +0xC0 (x) and +0xC8 (y), the draw working out where they go from those each time (statuswindow.cpp,
// targetwin.cpp). A title's top is the window's or above it (the letters from -3); a quad of those lower down isn't one
// (the Buffs window's hfr1 bar, at 28). They're moved off the window and the screen for the one draw, not made
// see-through (True Font's Titles font would still draw them), and put back after. The Target window's caption is
// target.label's: targetwin.cpp places it each draw
constexpr std::ptrdiff_t kPiecePicture = 8, kShapeQuads = 4, kShapeCount = 0x14, kQuadName = 0x32, kQuadY = 0xC8;
constexpr char kTitleTab[16 + 1] = "menu    hfr1    ", kTitleLetters[16 + 1] = "menu    menu2fon";
constexpr char kTargetName[16 + 1] = "menu    targetwi";
constexpr short kTitleAway = 0x1000;
// The main menu's place: its layout's right edge and top (384, 48 to 496 in the 512 x 448, layout +0x10: left, top,
// right, bottom). Every list the game lays out there is one it opens in the main menu's place (read live 2026-11-05 from
// the manager's layouts: its second page "socialme", Linkshell's, Friends', Config's Windows "conf5m", Log "conf11m",
// "conf11l", Font Colors "conftxtc", ... over a hundred), so out of the table they go by mainmenu.background (the Windows
// list kept its frame on a frameless main menu, reported 2026-11-05). At most 182 wide (the Blue Magic and puppet lists,
// 314 to 496): the macro bars' 16 to 496 aren't the main menu's
constexpr std::ptrdiff_t kLayoutRect = 0x10;
constexpr short kMenuSlotRight = 496, kMenuSlotTop = 48, kMenuSlotWidest = 182;
bool g_menu_frame = true;            // mainmenu.background
bool g_menu_solid = false;           // mainmenu.solid
// the window skip has just seen is to be drawn without its frame, and whose key that is (said once in the log); whether
// its buttons are drawn solid (<id>.solid)
const void* g_frameless = nullptr;
const wchar_t* g_frameless_name = nullptr;
const char* g_frameless_key = nullptr;
bool* g_frameless_logged = nullptr;
bool g_frameless_solid = false;

// Windows another place puts (the Items list goes with Menu Content or Inventory's box, the Target window is
// targetwin.cpp's, the Interaction Menu's chat.cpp's menus.*), out of the table so as not to change that, whose <id>.hide
// (kHide) and <id>.background (kFrame) are the table's all the same. One with no layout name is every window the layout
// stands over the chat (over_anchor: an NPC's selection menu, the lists the Commands menu opens)
struct Look {
    const char* id;
    const wchar_t* name;
    unsigned can;
    char layout[16 + 1];
    bool hide = false;
    bool frame = true;
    bool solid = false;
    bool hide_logged = false;
    bool frame_logged = false;
};
Look g_looks[] = {
    {"inventory", L"Inventory", kHide | kFrame, "menu    inventor"},
    {"target", L"Target window", kFrame, "menu    targetwi"},
    {"menus", L"Interaction menu", kHide | kFrame, ""},
};

// menucontent.background=0: Menu Content's windows (Status, Equipment, Quests, ...: any laid out in the 512 x 448 at the
// top left that has no place of its own), noted as each is placed (place_frame_hook) whether Menu Content moves them or
// not, with its layout then (a window made again at a freed one's address has another)
std::unordered_map<const void*, const char*> g_content;
constexpr size_t kContentAtMost = 256;

// A layout the game puts in the main menu's place (kMenuSlotRight)
bool in_menu_slot(const char* layout)
{
    const short* rect = reinterpret_cast<const short*>(layout + kLayoutRect);
    const short left = rect[0], top = rect[1], right = rect[2];
    return right == kMenuSlotRight && top == kMenuSlotTop && right - left > 0 && right - left <= kMenuSlotWidest;
}

void mark_frameless(const void* ctrl, const wchar_t* name, const char* key, bool& logged, bool solid)
{
    g_frameless = ctrl;
    g_frameless_name = name;
    g_frameless_key = key;
    g_frameless_logged = &logged;
    g_frameless_solid = solid;
}

bool g_searched = false;
char* g_manager = nullptr;
SetLocate g_set_locate = nullptr;
Resize g_resize = nullptr;
SetResource g_set_resource = nullptr;
PlaceFrame g_place_frame = nullptr;
char* g_opening = nullptr; // the window SetResourceData is making
QuestPage g_quest_page = nullptr, g_quest_page2 = nullptr, g_quest_text = nullptr;
ButtonRect g_button_rect_get = nullptr, g_button_rect_set = nullptr;
char* g_quest_window = nullptr; // the Quests window while a page of it lays its buttons out

void __fastcall set_resource_hook(char* window, void* edx, char* layout);
void __fastcall place_frame_hook(char* part, void* edx, int x, int y, int scaled);
void __fastcall quest_page_hook(char* list, void* edx);
void __fastcall quest_page2_hook(char* list, void* edx);
void __fastcall quest_text_hook(char* list, void* edx);
void __fastcall button_rect_get_hook(char* handle, void* edx, short* rect);
void __fastcall button_rect_set_hook(char* handle, void* edx, short* rect);
void* g_title_opened = nullptr; // the title box's handler's slot 1, to know its handler by
void* g_log_opened = nullptr;   // the log windows' slot 1, to know the chat log maximized's handler by
void* g_gauge_opened = nullptr; // the HP gauge's handler's slot 1, to know its handler by
WindowRect g_gauge_rect = nullptr; // the rect it takes from its window
void* g_link_opened = nullptr;  // the Linkshell menu's handler's slot 1, to know its handler by
ButtonPlace g_link_place = nullptr; // the button place it takes Unequip's from
WindowRect g_window_rect = nullptr;
Updated g_help_updated = nullptr; // the help line's handler's update (its slot 5), to know its handler by and to run
WinCalc g_win_calc = nullptr;
DestroyMenu g_destroy_menu = nullptr;

template <typename T> T& at(void* base, std::ptrdiff_t offset)
{
    return *reinterpret_cast<T*>(static_cast<char*>(base) + offset);
}

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

// The Linkshell menu's open and the button place it calls (kLinkOpened)
void find_link_menu(HMODULE game)
{
    const std::uintptr_t link = scan::find(game, kLinkOpened);
    if (link == 0) {
        log::write(L"HUD windows: the Linkshell menu's open wasn't found in FFXiMain.dll: with mainmenu.move on, its Unequip "
                   L"is drawn at the game's place");
        return;
    }
    g_link_opened = reinterpret_cast<void*>(link);
    g_link_place = reinterpret_cast<ButtonPlace>(link + kLinkPlaceCall + 5 + *reinterpret_cast<const std::int32_t*>(link + kLinkPlaceCall + 1));
}

bool found()
{
    if (g_searched)
        return g_set_locate != nullptr;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return false;
    g_searched = true;
    const std::uintptr_t help = scan::find(game, kHelp);
    const std::uintptr_t match = scan::find(game, kSetLocate);
    if (help != 0 && match != 0) {
        const std::uintptr_t locate = match - kSetLocatePrologue;
        const auto* start = reinterpret_cast<const std::uint8_t*>(locate);
        if ((start[0] == 0x83 || start[0] == 0xE9) && start[kResizeAt] == 0xE8) {
            std::uintptr_t manager = 0;
            std::memcpy(&manager, reinterpret_cast<const void*>(help + kManagerAt), sizeof manager);
            g_manager = reinterpret_cast<char*>(manager);
            g_set_locate = reinterpret_cast<SetLocate>(locate);
            g_resize = reinterpret_cast<Resize>(locate + kResizeAt + 5 + *reinterpret_cast<const std::int32_t*>(locate + kResizeAt + 1));
        }
    }
    if (const std::uintptr_t gauge = scan::find(game, kGaugeOpened)) {
        g_gauge_opened = reinterpret_cast<void*>(gauge);
        g_gauge_rect = reinterpret_cast<WindowRect>(gauge + kGaugeRectCall + 5 + *reinterpret_cast<const std::int32_t*>(gauge + kGaugeRectCall + 1));
    } else {
        log::write(L"HUD windows: the HP gauge's open wasn't found in FFXiMain.dll: a moved gauge leaves its bars behind");
    }
    find_link_menu(game);
    g_log_opened = reinterpret_cast<void*>(scan::find(game, kLogOpened));
    if (g_log_opened == nullptr)
        log::write(L"HUD windows: the log windows' open wasn't found in FFXiMain.dll: a resized chat log keeps its scroll bar");
    if (const std::uintptr_t title = scan::find(game, kTitleOpened)) {
        g_title_opened = reinterpret_cast<void*>(title);
        g_window_rect = reinterpret_cast<WindowRect>(title + kTitleRectCall + 5 + *reinterpret_cast<const std::int32_t*>(title + kTitleRectCall + 1));
    } else {
        log::write(L"HUD windows: the help bar title box's handler wasn't found in FFXiMain.dll: a moved title box leaves its text behind");
    }
    g_help_updated = reinterpret_cast<Updated>(scan::find(game, kHelpUpdated));
    if (g_help_updated == nullptr)
        log::write(L"HUD windows: the help line's update wasn't found in FFXiMain.dll: a moved help line leaves its text behind");
    if (const std::uintptr_t draw = scan::find(game, kWinDraw))
        g_win_calc = reinterpret_cast<WinCalc>(draw + kWinCalcCall + 5 + *reinterpret_cast<const std::int32_t*>(draw + kWinCalcCall + 1));
    else
        log::write(L"HUD windows: the game's window calc wasn't found in FFXiMain.dll: with <id>.background=0, a closed menu's "
                   L"commands stay up under the menu it opened");
    if (const std::uintptr_t destroy = scan::find(game, kDestroyMenu))
        g_destroy_menu = reinterpret_cast<DestroyMenu>(destroy);
    else
        log::write(L"HUD windows: the game's window destroy wasn't found in FFXiMain.dll: an Options menu left open without its "
                   L"list stays on the screen");
    if (g_set_locate == nullptr)
        log::write(L"HUD windows: the game's window placing wasn't found in FFXiMain.dll: [client] party.move, alliance1.move "
                   L"and alliance2.move do nothing");
    // Menu Content (from the game's thread, which alone makes windows; the placing does nothing without the making's)
    const std::uintptr_t resource = scan::find(game, kSetResource), frame = scan::find(game, kPlaceFrame);
    if (g_manager == nullptr || resource == 0 || frame == 0 ||
        !imports::detour(reinterpret_cast<PlaceFrame>(frame), kPlaceFramePrologue, &place_frame_hook, g_place_frame) ||
        !imports::detour(reinterpret_cast<SetResource>(resource), kSetResourcePrologue, &set_resource_hook, g_set_resource))
        log::write(L"HUD windows: the game's window making wasn't found in FFXiMain.dll: [client] menucontent.move does nothing");
    // the Quests window's buttons with it (the rects' hooks pass everything through but the pages')
    const std::uintptr_t page = scan::find(game, kQuestPage), page2 = scan::find(game, kQuestPage2);
    const std::uintptr_t text = scan::find(game, kQuestText);
    const std::uintptr_t get = scan::find(game, kButtonRectGet), set = scan::find(game, kButtonRectSet);
    if (g_place_frame == nullptr || page == 0 || page2 == 0 || text == 0 || get == 0 || set == 0 ||
        !imports::detour(reinterpret_cast<ButtonRect>(get), kButtonRectPrologue, &button_rect_get_hook, g_button_rect_get) ||
        !imports::detour(reinterpret_cast<ButtonRect>(set), kButtonRectPrologue, &button_rect_set_hook, g_button_rect_set) ||
        !imports::detour(reinterpret_cast<QuestPage>(page), kQuestPagePrologue, &quest_page_hook, g_quest_page) ||
        !imports::detour(reinterpret_cast<QuestPage>(page2), kQuestPage2Prologue, &quest_page2_hook, g_quest_page2) ||
        !imports::detour(reinterpret_cast<QuestPage>(text), kQuestTextPrologue, &quest_text_hook, g_quest_text))
        log::write(L"HUD windows: the Quests window's button layout wasn't found in FFXiMain.dll: with menucontent.move on, "
                   L"its Current and Completed buttons stay at the game's place");
    return g_set_locate != nullptr;
}

// The manager's area: false while it's empty
bool area(int& left, int& top, int& width, int& height)
{
    left = at<short>(g_manager, kAreaLeft);
    top = at<short>(g_manager, kAreaLeft + 2);
    width = at<short>(g_manager, kAreaLeft + 4) - left;
    height = at<short>(g_manager, kAreaLeft + 6) - top;
    return width > 0 && height > 0;
}

// The top-left corner of window `w`, `width` x `height`, with its anchor point at `px`, `py` (the interface's units), kept
// in the area
void corner(const Window& w, double px, double py, int width, int height, int& left, int& top)
{
    int area_left = 0, area_top = 0, area_w = 0, area_h = 0;
    area(area_left, area_top, area_w, area_h);
    left = static_cast<int>(std::lround(px - w.anchor_x * width));
    top = static_cast<int>(std::lround(py - w.anchor_y * height));
    left = std::clamp(left, area_left, std::max(area_left, area_left + area_w - width));
    top = std::clamp(top, area_top, std::max(area_top, area_top + area_h - height));
}

Window* window_named(const char* id);
char* open_window(const char* name);

// Where window `w` goes, `width` x `height`: its top-left corner, by its X and Y while move is on, else where the main
// menu is for one that follows it (while the main menu is moved), else on the chat at its left for one pinned there
// (chat.<id>), else the game's place; false if the area is empty
bool wanted(const Window& w, bool move, int width, int height, int& left, int& top)
{
    int area_left = 0, area_top = 0, area_w = 0, area_h = 0;
    if (!area(area_left, area_top, area_w, area_h))
        return false;
    // its bottom on the chat's top, as the game stands it there, and its left at the chat's (the menus over the chat with
    // chat.menus, chat.cpp's menu_left); with no chat shown, the game's place
    short chat[4] = {};
    if (!move && w.on_chat && chat::top_chat(chat)) {
        left = std::clamp(static_cast<int>(chat[0]), area_left, std::max(area_left, area_left + area_w - width));
        top = std::clamp(chat[1] - height, area_top, std::max(area_top, area_top + area_h - height));
        return true;
    }
    double px = 0, py = 0;
    if (!move && (w.can & kFollow) != 0) {
        const Window* menu = window_named("mainmenu");
        int menu_left = 0, menu_top = 0;
        if (menu != nullptr && menu->move && menu != &w &&
            wanted(*menu, true, menu->seen_w > 0 ? menu->seen_w : menu->default_w,
                   menu->seen_h > 0 ? menu->seen_h : menu->default_h, menu_left, menu_top)) {
            // its right edge and top where the main menu's are (its anchor point is its top right, as the main menu's)
            corner(w, menu_left + (menu->seen_w > 0 ? menu->seen_w : menu->default_w), menu_top, width, height, left, top);
            return true;
        }
    }
    // under the Status window, its left at the Status window's and its top the game's 2 under its bottom (the EXP window,
    // kStatus): the Status window as it is now, wherever Menu Content put it and however tall status.taller made it.
    // With no Status window up, the game's place
    if (!move && (w.can & kStatus) != 0) {
        if (char* status = open_window(kStatusName); status != nullptr) {
            corner(w, at<short>(status, kWinFrame), at<short>(status, kWinFrame + 6) + kUnderStatusGap, width, height, left, top);
            return true;
        }
    }
    if (!move && std::strcmp(w.id, "menucontent") == 0) { // (never seen: the game's place is the first window's)
        px = area_left + kItemsX;
        py = area_top + kItemsY;
    } else if (move || !w.seen_game) {
        px = area_left + area_w * (move ? w.x : w.default_x) / 100.0;
        py = area_top + area_h * (move ? w.y : w.default_y) / 100.0;
    } else {
        px = w.game_x;
        py = w.game_y;
    }
    corner(w, px, py, width, height, left, top);
    return true;
}

// The title box moved: where its handler draws the page's name worked out again from where it is now, as its slot 1 does
void title_moved(char* window)
{
    char* handler = at<char*>(window, kWinOwner);
    if (handler == nullptr || g_window_rect == nullptr || at<char*>(handler, kOwnerWindow) != window ||
        at<void**>(handler, 0)[1] != g_title_opened)
        return;
    short* rect = &at<short>(handler, kTitleRect);
    g_window_rect(window, rect);
    const int left = rect[0] + 4, top = rect[1] + 8;
    at<int>(handler, kTitleText) = left, at<int>(handler, kTitleText + 4) = top;
    at<int>(handler, kTitleTextAgain) = left, at<int>(handler, kTitleTextAgain + 4) = top;
    at<int>(handler, kTitleRoom) = rect[2] - rect[0] - 8;
}

// The Level / EXP window moved: its handler (CTkPersonaLevel) draws its rows and bar in the rect it took from the window
// as it opened (its +0x14, by the game's rect getter after its own SetLocate, 0x10147088), so the frame went under the
// Status window and the text stayed at the game's place, behind the Status sub-menu (seen 2026-10-24): taken again from
// where the window is now, as the help bar's title box's is. The handler is known by its window (+8) being this one
void level_moved(char* window)
{
    char* handler = at<char*>(window, kWinOwner);
    if (handler == nullptr || g_window_rect == nullptr || at<char*>(handler, kOwnerWindow) != window)
        return;
    g_window_rect(window, &at<short>(handler, kTitleRect));
}

// The HP gauge moved: the rect its handler draws its bars in taken again from where the window is now (kGaugeOpened)
void gauge_moved(char* window)
{
    char* handler = at<char*>(window, kWinOwner);
    if (handler == nullptr || g_gauge_rect == nullptr || at<char*>(handler, kOwnerWindow) != window ||
        at<void**>(handler, 0)[1] != g_gauge_opened)
        return;
    g_gauge_rect(window, &at<short>(handler, kGaugeRect));
}

// The Linkshell menu moved: where its handler draws the orange Unequip taken again from where that button is now, as its
// slot 1 does (kLinkOpened). The handler is known by its window (+8) being this one and its slot 1 being that open
void link_moved(char* window)
{
    char* handler = at<char*>(window, kWinOwner);
    if (handler == nullptr || g_link_place == nullptr || at<char*>(handler, kOwnerWindow) != window ||
        at<void**>(handler, 0)[1] != g_link_opened)
        return;
    short rect[4] = {};
    g_link_place(window, rect, at<short>(handler, kLinkButton), 1, 1);
    if (rect[2] <= rect[0]) // no such button: left as it was
        return;
    at<short>(handler, kLinkDrawn) = rect[0];
    at<short>(handler, kLinkDrawn + 2) = rect[1];
}

// The help line moved or resized: its handler's update run again, which takes the text's place and room from the window
// as it is now (kHelpUpdated). The handler is known by its window (+8) being this one and its slot 5 being that update
void help_moved(char* window)
{
    char* handler = at<char*>(window, kWinOwner);
    if (handler == nullptr || g_help_updated == nullptr || at<char*>(handler, kOwnerWindow) != window ||
        at<void**>(handler, 0)[kHelpUpdatedSlot] != reinterpret_cast<void*>(g_help_updated))
        return;
    g_help_updated(handler);
}

// The chat log maximized's handler (kLogOpened), or null for any other window
char* log_handler(const Window& w, char* window)
{
    if (g_log_opened == nullptr || std::strcmp(w.id, "fulllog") != 0)
        return nullptr;
    char* handler = at<char*>(window, kWinOwner);
    if (handler == nullptr || at<char*>(handler, kOwnerWindow) != window || at<void**>(handler, 0)[1] != g_log_opened)
        return nullptr;
    return handler;
}

// The chat log resized to `height`: its rows and height as its open works them out for that height, a row more or fewer
// for each row's units (a part row left out), from what the game made it with (w.game_rows, w.game_h). A row is 16 units,
// or chat.lineheight's (the open's rows are by it then too): by 16, 20 px rows came 4 short of the window, a gap over the
// first line (seen 2026-11-26: made 1136 tall with 56 rows, resized to 876 it kept 39, where 43 fit)
void log_resized(const Window& w, char* window, int height)
{
    char* handler = log_handler(w, window);
    if (handler == nullptr || w.game_rows <= 0)
        return;
    const int grown = height - w.game_h;
    const int unit = std::max(1, chat::line_height()); // the game's 16, or chat.lineheight's (its rows then are too)
    const int rows = std::max(1, w.game_rows + (grown >= 0 ? grown / unit : -((-grown + unit - 1) / unit)));
    at<int>(handler, kLogRows) = rows;
    at<int>(handler, kLogRowsShown) = rows;
    at<int>(handler, kLogHeight) = height;
}

#include "hudwin_under.h" // kUnder: the macro bar's Alt row under its Ctrl row while it's edited

// Window `w` (its `window`), before it's drawn: seen, and put where its move says, as wide as its width says. Not one
// that's closed (state 0xE: not drawn, but still in the manager's list and its draw still called): the auction house's
// menus stay there as the next opens, and each taken as the one seen flipped the box's size every frame
void place(Window& w, char* window)
{
    if (at<std::uint32_t>(window, kWinState) == kWinClosed || went_under(window))
        return;
    const int left = at<short>(window, kWinFrame), top = at<short>(window, kWinFrame + 2);
    const int width = at<short>(window, kWinFrame + 4) - left, height = at<short>(window, kWinFrame + 6) - top;
    if (width <= 0 || height <= 0)
        return;
    int below_h = 0; // (what goes under it: in its box)
    char* below = below_of(window, below_h);
    w.seen_w = width, w.seen_h = height + below_h;
    w.over_chat = (at<std::uint32_t>(window, kWinFlags) & kWinOverAnchor) != 0;
    if (window != w.window) { // a new window, where the game puts it
        w.window = window;
        w.game_x = left + w.anchor_x * width;
        w.game_y = top + w.anchor_y * height;
        w.game_w = width, w.game_h = height;
        w.seen_game = true;
        w.sized = false;
        char* log = log_handler(w, window);
        w.game_rows = log != nullptr ? at<int>(log, kLogRows) : 0;
    }
    // the game's (and the other modules') to place: its box where it is now, not where it was first drawn. An item's
    // description put under the Equipment window's stats after that had its box left where it was (reported 2026-11-06)
    if (!w.move && !w.on_chat && !w.placed && w.width <= 0 && w.height <= 0 && !w.sized && (w.can & (kFollow | kStatus)) == 0) {
        w.game_x = left + w.anchor_x * width;
        w.game_y = top + w.anchor_y * height;
        return;
    }
    if (!found() || g_manager == nullptr)
        return;
    int area_left = 0, area_top = 0, area_w = 0, area_h = 0;
    if (!area(area_left, area_top, area_w, area_h))
        return;
    // its size: the settings', or back to the game's
    int want_w = width, want_h = height;
    if (w.width > 0 && g_resize != nullptr)
        want_w = std::clamp(static_cast<int>(std::lround(area_w * w.width / 100.0)), 16, area_w);
    else if (w.sized)
        want_w = w.game_w;
    if (w.height > 0 && g_resize != nullptr)
        want_h = std::clamp(static_cast<int>(std::lround(area_h * w.height / 100.0)), 16, area_h);
    else if (w.sized)
        want_h = w.game_h;
    int want_x = left, want_y = top;
    if (!wanted(w, w.move, want_w, want_h + below_h, want_x, want_y)) // (kept in the area with what goes under it)
        return;
    if (w.move && !w.logged) {
        w.logged = true;
        log::write(L"%ls: placed at %.1f%%, %.1f%% ([client] %hs.move=1)", w.name, w.x, w.y, w.id);
    }
    w.placed = w.move || w.on_chat; // (either turned off, it goes back to the game's place once)
    if ((want_w != width || want_h != height) && g_resize != nullptr) {
        g_resize(window, want_x, want_y, want_w, want_h, 1, 0, 0);
        w.seen_w = want_w, w.seen_h = want_h;
        w.sized = w.width > 0 || w.height > 0;
        if (want_h != height)
            log_resized(w, window, want_h);
    } else if (left != want_x || top != want_y) {
        g_set_locate(window, want_x, want_y);
    } else {
        return;
    }
    if (below != nullptr)
        put_under(below, window); // (at once: it may be drawn before this one)
    // what the window's handler took from where the window was, taken again from where it is now
    if (std::strcmp(w.id, "helptitle") == 0)
        title_moved(window);
    else if (std::strcmp(w.id, "helpbar") == 0)
        help_moved(window);
    else if (std::strcmp(w.id, "hpgauge") == 0)
        gauge_moved(window);
    else if ((w.can & kStatus) != 0)
        level_moved(window);
}

Window* window_named(const char* id);

// A menu the main menu opens (Search, Map, Linkshell, ...): the game puts it where the main menu was, its right edge and
// top at the main menu's own, in the main menu's area. With mainmenu.move on, one there is moved by as much as the main
// menu is (once: moved, it's no longer at the game's place). A bigger window the game puts elsewhere is left alone.
// (The Status sub-menu is the table's, kFollow: place puts it with the main menu every frame instead.)
void follow_menu(char* window)
{
    Window* menu = window_named("mainmenu");
    if (menu == nullptr || !menu->move || !menu->seen_game || at<std::uint16_t>(window, kWinArea) != kMenuArea)
        return;
    const int left = at<short>(window, kWinFrame), top = at<short>(window, kWinFrame + 2);
    const int right = at<short>(window, kWinFrame + 4), bottom = at<short>(window, kWinFrame + 6);
    if (right - left <= 0 || bottom - top <= 0 || right != std::lround(menu->game_x) || top != std::lround(menu->game_y))
        return;
    if (!found() || g_manager == nullptr)
        return;
    int menu_left = 0, menu_top = 0;
    if (!wanted(*menu, true, menu->seen_w, menu->seen_h, menu_left, menu_top))
        return;
    // its right edge and top where the main menu's are
    int want_x = 0, want_y = 0;
    corner(*menu, menu_left + menu->seen_w, menu_top, right - left, bottom - top, want_x, want_y);
    if (want_x == left && want_y == top)
        return;
    g_set_locate(window, want_x, want_y);
    link_moved(window);
}

Window* window_named(const char* id)
{
    for (auto& w : g_windows)
        if (std::strcmp(w.id, id) == 0)
            return &w;
    return nullptr;
}

// A window of layout `name` is in the manager's list (a node: +0 the next, +0x10 its window)
bool window_open(const char* name)
{
    if (!found() || g_manager == nullptr)
        return false;
    int count = 0;
    for (char* node = at<char*>(g_manager, 0); node != nullptr && count < 256; node = at<char*>(node, 0), ++count) {
        char* window = at<char*>(node, 0x10);
        const char* layout = window != nullptr ? at<char*>(window, kWindowLayout) : nullptr;
        if (layout != nullptr && std::memcmp(layout + kLayoutName, name, 16) == 0)
            return true;
    }
    return false;
}

// The window of layout `name` that's up (in the manager's list and not closed), or null
char* open_window(const char* name)
{
    if (!found() || g_manager == nullptr)
        return nullptr;
    int count = 0;
    for (char* node = at<char*>(g_manager, 0); node != nullptr && count < 256; node = at<char*>(node, 0), ++count) {
        char* window = at<char*>(node, 0x10);
        const char* layout = window != nullptr ? at<char*>(window, kWindowLayout) : nullptr;
        if (layout != nullptr && std::memcmp(layout + kLayoutName, name, 16) == 0 &&
            at<std::uint32_t>(window, kWinState) != kWinClosed)
            return window;
    }
    return nullptr;
}

// The table's entry for `window`, by its layout's name, or null
Window* table_window(char* window)
{
    const char* layout = at<char*>(window, kWindowLayout);
    if (layout == nullptr)
        return nullptr;
    for (auto& w : g_windows)
        for (const char* name : w.layouts)
            if (name[0] != 0 && std::memcmp(layout + kLayoutName, name, 16) == 0)
                return &w;
    return nullptr;
}

// The list an Options box belongs to: the Magic list's Create (KaMenuMagicSel::Create, FUN_1022AA60) makes "mgcsortw" or
// "blusortw" beside "menu    magic   " and keeps it at +0xC0; the list's destroy (its vtable's 0x1022B050) destroys the
// box with the list. Abilities' and Emotes' lists go the same way with theirs
const char* options_list(const char* layout)
{
    if (std::memcmp(layout + kLayoutName, "menu    mgcsortw", 16) == 0 || std::memcmp(layout + kLayoutName, "menu    blusortw", 16) == 0)
        return "menu    magic   ";
    if (std::memcmp(layout + kLayoutName, "menu    abisortw", 16) == 0)
        return "menu    ability ";
    if (std::memcmp(layout + kLayoutName, "menu    emosortw", 16) == 0)
        return "menu    emote   ";
    return nullptr;
}

// An Options box (+ :Sort, Auto, Manual, Set Spells) left open after its list went (reported 2026-10-23 with the box moved
// to the far left: Blue Magic's stayed on the screen, the current window, taking no key: its cancel only hands the keys
// to a list that's no longer there, and nothing else closes it). The list always makes it and always takes it along, so
// one open while no window of its list is in the manager's list at all, three draws running (the list and the box are
// put back together after the Set Spells screen, which the gap covers), is destroyed as the list would have, once; the
// windows there at the time are logged, to tell what left it behind
char* g_orphan = nullptr;
int g_orphan_draws = 0;
void close_orphan(char* window, const char* layout)
{
    const char* list = options_list(layout);
    if (list == nullptr || g_destroy_menu == nullptr || !found() || g_manager == nullptr)
        return;
    if (at<std::uint32_t>(window, kWinState) != 0 || at<std::uint8_t>(window, kWinDestroyed) != 0 || window_open(list)) {
        g_orphan = nullptr;
        return;
    }
    if (window != g_orphan) {
        g_orphan = window;
        g_orphan_draws = 0;
    }
    if (++g_orphan_draws < 3)
        return;
    std::string others;
    int count = 0;
    for (char* node = at<char*>(g_manager, 0); node != nullptr && count < 256; node = at<char*>(node, 0), ++count) {
        char* other = at<char*>(node, 0x10);
        const char* name = other != nullptr ? at<char*>(other, kWindowLayout) : nullptr;
        if (name == nullptr || other == window)
            continue;
        others += (others.empty() ? "" : ", ") + std::string(name + kLayoutName + 8, 8) + (at<std::uint32_t>(other, kWinState) == kWinClosed ? " (closed)" : "");
    }
    log::write(L"Options menu: \"%.16hs\" was left open at %d, %d with no \"%.16hs\" list: closed (the windows up: %hs)",
               layout + kLayoutName, at<short>(window, kWinFrame), at<short>(window, kWinFrame + 2), list, others.c_str());
    g_destroy_menu(g_manager, window, 1);
    g_orphan = nullptr;
}

// A closed window (state 0xE: the game still draws it, as the main menu under a menu it opened in its place or the
// Commands menu under the list it opened). Once its close wait is over, the game's draw draws its frame going (shrinking
// to nothing) and stops there, its buttons left out (PS2 CTkMenuCtrlData::OnDraw, the same on PC). Drawn without the
// frame, the draw went on to the buttons and a closed menu's commands stayed up: Magic's kinds over the main menu's list,
// the Commands menu's under Abilities, the main menu's beside Synthesis (reported 2026-10-23). So <id>.background=0 leaves
// one out of the draw but for what the draw does before its pieces, left_out's: its calc (its timers, its delete) and
// the input delay's count. One the game would draw no frame for (+0x69 clear, no frame piece) is drawn as ever.
bool closed(const void* ctrl)
{
    char* window = static_cast<char*>(const_cast<void*>(ctrl));
    if (g_win_calc == nullptr || at<std::uint32_t>(window, kWinState) != kWinClosed || at<short>(window, kWinCloseWait) > 0 ||
        at<std::uint8_t>(window, kWinPieces) == 0)
        return false;
    bool frame = false;
    int count = 0;
    for (char* node = at<char*>(window, kWinPieceList); node != nullptr && !frame && count < 256;
         node = at<char*>(node, 0), ++count) {
        char* piece = at<char*>(node, kNodePiece);
        frame = at<std::uint8_t>(node, kNodeOut) == 0 && piece != nullptr && at<short>(piece, 0) == kPieceFrame &&
                at<std::uint8_t>(piece, kPieceUnseen) != 1;
    }
    return frame;
}

bool status_open()
{
    return window_open(kStatusName);
}

bool named(char* window, const char* name)
{
    const char* layout = at<char*>(window, kWindowLayout);
    return layout != nullptr && std::memcmp(layout + kLayoutName, name, 16) == 0;
}

bool in_world()
{
    const std::int32_t phase = at<std::int32_t>(g_manager, kManagerPhase);
    return std::find(std::begin(kWorldPhases), std::end(kWorldPhases), phase) != std::end(kWorldPhases);
}

// A window the layout stands over the chat (kWinOverAnchor): the Interaction Menu's. The layout puts it where chat.cpp's
// menu_place says (menus.move: its left at menus.x, its bottom at menus.y; else at its own left on the chat's top), and
// again whenever the chat or the box moves. Moved by Menu Content as well (as any window not in the table, through
// SetLocate's hook and locate), one put at a box in the top-left 512 x 448 went by the box and then by Menu Content's
// too: the list came up mid-screen, its selection still at the box, and it jumped as the box was dragged in and out of
// that corner (the spell list from the Commands menu's Magic, reported 2026-10-23). The layout's placing wins: never
// moved here, wherever Menu Content's box is
bool over_anchor(char* window)
{
    return (at<std::uint32_t>(window, kWinFlags) & kWinOverAnchor) != 0;
}

// A window Menu Content may move: not the lobby's (laid out in proportion), not one the layout stands over the chat, not
// one with a Position of its own here, and not one another module moves
bool content_window(char* window)
{
    const char* layout = at<char*>(window, kWindowLayout);
    if (layout == nullptr || at<std::uint8_t>(window, kWinScaled) != 0 || over_anchor(window))
        return false;
    for (const auto& w : g_windows) {
        if ((w.can & kContent) != 0 && !w.move)
            continue; // (not placed by its own: as if out of the table)
        for (const char* name : w.layouts)
            if (name[0] != 0 && std::memcmp(layout + kLayoutName, name, 16) == 0)
                return false;
    }
    for (const char* name : kOwnPlaced)
        if (std::memcmp(layout + kLayoutName, name, 16) == 0)
            return false;
    return true;
}

// Window `window`, `width` wide, being placed at (x, y), the game's place: one of Menu Content's or not
// (menucontent.background), as content_shift tells them
void note_content(char* window, int x, int y, int width)
{
    int area_left = 0, area_top = 0, area_w = 0, area_h = 0;
    if (!area(area_left, area_top, area_w, area_h) || !in_world() || !content_window(window) || x < area_left ||
        y < area_top || x + width > area_left + kDesignW || y >= area_top + kDesignH) {
        g_content.erase(window);
        return;
    }
    if (g_content.size() >= kContentAtMost && g_content.find(window) == g_content.end())
        g_content.clear(); // (windows come and go)
    g_content[window] = at<char*>(window, kWindowLayout);
}

bool content_member(const void* window, const char* layout)
{
    const auto noted = g_content.find(window);
    return noted != g_content.end() && noted->second == layout;
}

void __fastcall set_resource_hook(char* window, void* edx, char* layout)
{
    char* outer = g_opening;
    g_opening = window;
    g_set_resource(window, edx, layout);
    g_opening = outer;
    buttonspace::made(window); // [client] buttons.spacing: its buttons moved apart at its first draw
}

// Window `window`, `width` x `height`, about to be put at (x, y): by how much it goes elsewhere instead, kept in the area.
// The Items list from the main menu and its item descriptions (put at the list's own places), by as much as Inventory's
// box is from the list's place (16, 48), while Inventory has a Position. Else a Menu Content one put at the area's top left (its top
// left corner and its width in the 512 x 448: a list made taller by items.rows still is; an item description under it,
// wherever), by as much as Menu Content's box is from the area's top left. False if it stays.
// A window Menu Content left where the game put it, with menucontent.move on: said once per layout name (to see why one
// isn't moved: the auction house's from its NPC before the main menu's first open, 2026-10-22)
void stayed(char* window, const wchar_t* why, int x, int y, int width, int height)
{
    static char said[16][16];
    static int count = 0;
    const char* layout = at<char*>(window, kWindowLayout);
    if (layout == nullptr || count >= 16)
        return;
    for (int i = 0; i < count; ++i)
        if (std::memcmp(said[i], layout + kLayoutName, 16) == 0)
            return;
    std::memcpy(said[count++], layout + kLayoutName, 16);
    log::write(L"Menu content: \"%.16hs\" stays at %d, %d (%d x %d): %ls (phase %d, area %d x %d)", layout + kLayoutName, x, y,
               width, height, why, at<std::int32_t>(g_manager, kManagerPhase), at<short>(g_manager, kAreaLeft + 4),
               at<short>(g_manager, kAreaLeft + 6));
}

// The Status screen puts the Status window at 16, 16 (its layout's 16, 48 less 32: the top of the interface, with the main
// menu at the top right) and its EXP window (Merit Points': Limit Points) under it, at 16, 208. Menu Content's box begins
// at the first window's corner (16, 48), so taken with the box, the Status window stood 32 over the rest (seen
// 2026-10-22, over the main menu's top beside it). Both go 32 lower: the Status window's top at the box's
int status_drop(char* window, int x, int y, int area_left, int area_top)
{
    if (x != area_left + kItemsX)
        return 0;
    const bool status = named(window, kStatusName);
    if (status ? y == area_top + kStatusY : y == area_top + kUnderStatusY && window_open(kStatusName))
        return kItemsY - kStatusY;
    return 0;
}

bool content_shift(char* window, int x, int y, int width, int height, int& ax, int& ay)
{
    if (window == nullptr || g_manager == nullptr)
        return false;
    int area_left = 0, area_top = 0, area_w = 0, area_h = 0;
    Window* content_on = window_named("menucontent");
    const bool watch = content_on != nullptr && content_on->move;
    if (!area(area_left, area_top, area_w, area_h) || !in_world()) {
        if (watch)
            stayed(window, L"not in the world, or no area", x, y, width, height);
        return false;
    }
    if (!content_window(window)) {
        if (watch && at<std::uint8_t>(window, kWinScaled) != 0)
            stayed(window, L"a scaled window", x, y, width, height);
        else if (watch && over_anchor(window))
            stayed(window, L"laid out over the chat (the Interaction Menu's, menus.*)", x, y, width, height);
        return false;
    }
    const bool info = named(window, kItemInfoName);
    // an item description: as far as the list that's up went (it's placed under the list's game place, 16, 48), so it
    // stays with the list whichever box the list went by; a list placed before this (none recorded) goes as below
    if (info) {
        char* list = itemlist::open_list();
        const auto shifted = list != nullptr ? g_shifts.find(list) : g_shifts.end();
        if (shifted != g_shifts.end()) {
            int dx = shifted->second.first, dy = shifted->second.second;
            // no room under the list (a tall one low on the screen): beside it instead, level with its top
            const int list_top = at<short>(list, kWinFrame + 2), list_right = at<short>(list, kWinFrame + 4);
            if (y + dy + height > area_top + area_h && list_right > at<short>(list, kWinFrame) && x + dx < list_right)
                dx = list_right - x, dy = list_top - y;
            ax = std::clamp(x + dx, area_left, std::max(area_left, area_left + area_w - width)) - x;
            ay = std::clamp(y + dy, area_top, std::max(area_top, area_top + area_h - height)) - y;
            return ax != 0 || ay != 0;
        }
    }
    Window* items = window_named("inventory");
    Window* content = window_named("menucontent");
    // the Items list at its own place (the Equipment screen puts the same window beside itself, at 314, 48, with
    // SetLocate; Equipment's own window isn't in the manager's list yet then), and an item description under it (16,
    // 240 and lower; beside Equipment or a shop's list it's at 130)
    const bool own = info ? x == area_left + kItemsX
                          : named(window, kItemsName) && x == area_left + kItemsX && y == area_top + kItemsY;
    int dx = 0, dy = 0;
    if (items != nullptr && items->move && own) {
        if (!info)
            items->seen_w = width, items->seen_h = height; // (its box the list's size)
        int left = 0, top = 0;
        if (!wanted(*items, true, items->seen_w > 0 ? items->seen_w : items->default_w,
                    items->seen_h > 0 ? items->seen_h : items->default_h, left, top))
            return false;
        dx = left - (area_left + kItemsX), dy = top - (area_top + kItemsY);
        if (!items->logged) {
            items->logged = true;
            log::write(L"Inventory: the Items list goes %d, %d from the game's place ([client] inventory.move=1)", dx, dy);
        }
    } else if (content != nullptr && content->move) {
        if (!info && (x < area_left || y < area_top || x + width > area_left + kDesignW || y >= area_top + kDesignH)) {
            stayed(window, L"outside the 512 x 448 at the top left", x, y, width, height);
            return false;
        }
        int left = 0, top = 0;
        if (!wanted(*content, true, kContentW, kContentH, left, top))
            return false;
        dx = left - (area_left + kItemsX), dy = top - (area_top + kItemsY); // (the box's corner is the first window's)
        dy += status_drop(window, x, y, area_left, area_top);
        if (!content->logged) {
            content->logged = true;
            log::write(L"Menu content: the windows at the top left go %d, %d from the game's place ([client] "
                       L"menucontent.move=1)", dx, dy);
        }
    } else {
        return false;
    }
    ax = std::clamp(x + dx, area_left, std::max(area_left, area_left + area_w - width)) - x;
    ay = std::clamp(y + dy, area_top, std::max(area_top, area_top + area_h - height)) - y;
    return ax != 0 || ay != 0;
}

// Window `window` placed, gone `ax`, `ay` from the game's place: remembered for an item description under it
void shifted(char* window, int ax, int ay)
{
    if (window == nullptr || named(window, kItemInfoName))
        return;
    if (g_shifts.size() >= kShiftsAtMost && g_shifts.find(window) == g_shifts.end())
        g_shifts.clear(); // (windows come and go; a reused address is written over)
    g_shifts[window] = {ax, ay};
}

// A window's frame part placed (its first, the window's place): a Menu Content one moved where content_shift says, its
// place and its layout's place (+0x1E/+0x20, the place less x and y) both, as if its layout put it there. (Moved by x
// and y instead, the game keeps it that far from its layout's place: SetLocate put Status back by as much, and
// Equipment's Attack and Defense captions, drawn from the layout's place, stayed behind.)
void __fastcall place_frame_hook(char* part, void* edx, int x, int y, int scaled)
{
    g_place_frame(part, edx, x, y, scaled);
    char* window = g_opening;
    g_opening = nullptr; // (its other parts are placed from the window's place)
    if (window == nullptr || x != 0 || y != 0)
        return;
    __try {
        int ax = 0, ay = 0;
        note_content(window, at<short>(part, kPartX), at<short>(part, kPartX + 2), at<short>(part, kPartWidth));
        const bool moved = content_shift(window, at<short>(part, kPartX), at<short>(part, kPartX + 2),
                                         at<short>(part, kPartWidth), at<short>(part, kPartWidth + 2), ax, ay);
        shifted(window, ax, ay);
        if (moved)
            for (int k = 0; k < 4; ++k) // +0x1A, +0x1C, +0x1E, +0x20
                at<short>(part, kPartX + 2 * k) = static_cast<short>(at<short>(part, kPartX + 2 * k) + (k & 1 ? ay : ax));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// A page of the Quests window laying its buttons out, or its text drawn
void quest_page(QuestPage page, char* list, void* edx)
{
    char* outer = g_quest_window;
    g_quest_window = list != nullptr ? at<char*>(list, kListWindow) : nullptr;
    page(list, edx);
    g_quest_window = outer;
}

void __fastcall quest_page_hook(char* list, void* edx)
{
    quest_page(g_quest_page, list, edx);
}

void __fastcall quest_page2_hook(char* list, void* edx)
{
    quest_page(g_quest_page2, list, edx);
}

void __fastcall quest_text_hook(char* list, void* edx)
{
    quest_page(g_quest_text, list, edx);
}

// How far the window of button handle `handle` went from the game's place, if it's the Quests window laying out and it
// went
bool quest_shift(char* handle, int& dx, int& dy)
{
    if (g_quest_window == nullptr || handle == nullptr || at<std::uint32_t>(handle, kHandleKind) != 1)
        return false;
    char* button = at<char*>(handle, kHandleButton);
    if (button == nullptr || at<char*>(button, kButtonWindow) != g_quest_window)
        return false;
    const auto shifted = g_shifts.find(g_quest_window);
    if (shifted == g_shifts.end())
        return false;
    dx = shifted->second.first, dy = shifted->second.second;
    return dx != 0 || dy != 0;
}

// A button's rect as at the game's place
void __fastcall button_rect_get_hook(char* handle, void* edx, short* rect)
{
    g_button_rect_get(handle, edx, rect);
    int dx = 0, dy = 0;
    if (rect != nullptr && quest_shift(handle, dx, dy))
        for (int k = 0; k < 4; ++k)
            rect[k] = static_cast<short>(rect[k] - (k & 1 ? dy : dx));
}

// A button's rect, given as at the game's place, put as far as its window went (a copy: the page goes on from its own,
// the next button 32 lower)
void __fastcall button_rect_set_hook(char* handle, void* edx, short* rect)
{
    int dx = 0, dy = 0;
    if (rect == nullptr || !quest_shift(handle, dx, dy)) {
        g_button_rect_set(handle, edx, rect);
        return;
    }
    short moved[4];
    for (int k = 0; k < 4; ++k)
        moved[k] = static_cast<short>(rect[k] + (k & 1 ? dy : dx));
    g_button_rect_set(handle, edx, moved);
}

} // namespace

#include "hudwin_comment.h" // commentwin.keep: the bazaar message kept up over an item
#include "hudwin_hide.h"    // hide.windows: any window by its layout's name

void reload(const wchar_t* ini)
{
    hidden_read(ini);
    partyrows::reload(ini);
    for (auto& w : g_windows) {
        const std::wstring id(w.id, w.id + std::strlen(w.id));
        w.move = (w.can & kMove) != 0 && GetPrivateProfileIntW(L"client", (id + L".move").c_str(), 0, ini) != 0;
        w.hide = (w.can & kHide) != 0 && GetPrivateProfileIntW(L"client", (id + L".hide").c_str(), 0, ini) != 0;
        w.x = number(id + L".x", w.default_x, 0, 100, ini);
        w.y = number(id + L".y", w.default_y, 0, 100, ini);
        w.width = (w.can & kWidth) != 0 ? number(id + L".width", 0, 0, 100, ini) : 0;
        w.height = (w.can & kHeight) != 0 ? number(id + L".height", 0, 0, 100, ini) : 0;
        w.scale = number(id + L".scale", 100, 50, 200, ini) / 100.0;
        w.frame = (w.can & kFrame) == 0 || GetPrivateProfileIntW(L"client", (id + L".background").c_str(), 1, ini) != 0;
        w.solid = (w.can & kFrame) != 0 && GetPrivateProfileIntW(L"client", (id + L".solid").c_str(), 0, ini) != 0;
        w.on_chat = (w.can & kChat) != 0 && GetPrivateProfileIntW(L"client", (L"chat." + id).c_str(), 0, ini) != 0;
        if (!w.move)
            w.logged = false;
        if (!w.hide)
            w.hide_logged = false;
    }
    const Window* menu = window_named("mainmenu");
    g_menu_frame = menu == nullptr || menu->frame;
    g_menu_solid = menu != nullptr && menu->solid;
    for (auto& w : g_windows) {
        if ((w.can & (kFollow | kFrame)) == kFollow) // (one with kFrame has its own: statusmenu)
            w.frame = g_menu_frame, w.solid = g_menu_solid;
        if (w.frame)
            w.frame_logged = false;
    }
    for (auto& look : g_looks) {
        const std::wstring id(look.id, look.id + std::strlen(look.id));
        look.hide = (look.can & kHide) != 0 && GetPrivateProfileIntW(L"client", (id + L".hide").c_str(), 0, ini) != 0;
        look.frame = (look.can & kFrame) == 0 || GetPrivateProfileIntW(L"client", (id + L".background").c_str(), 1, ini) != 0;
        look.solid = (look.can & kFrame) != 0 && GetPrivateProfileIntW(L"client", (id + L".solid").c_str(), 0, ini) != 0;
        if (!look.hide)
            look.hide_logged = false;
        if (look.frame)
            look.frame_logged = false;
    }
    g_hide_gil = GetPrivateProfileIntW(L"client", L"menucontent.hidegil", 0, ini) != 0;
    g_hide_titles = GetPrivateProfileIntW(L"client", L"titles.hide", 0, ini) != 0;
    g_keep_comment = GetPrivateProfileIntW(L"client", L"commentwin.keep", 0, ini) != 0;
}

void install(const wchar_t* ini)
{
    reload(ini);
}

bool skip(const void* ctrl)
{
    g_frameless = nullptr;
    if (ctrl == nullptr)
        return false;
    found(); // (Menu Content's hooks, from the first window drawn)
    comment_hooks();
    __try {
        const char* layout = *reinterpret_cast<const char* const*>(static_cast<const char*>(ctrl) + kWindowLayout);
        if (layout == nullptr)
            return false;
        if (hidden_named(layout)) // [client] hide.windows
            return true;
        for (auto& w : g_windows) {
            for (const char* name : w.layouts) {
                if (name[0] == 0 || std::memcmp(layout + kLayoutName, name, 16) != 0)
                    continue;
                if (std::strcmp(w.id, "sortmenu") == 0)
                    close_orphan(static_cast<char*>(const_cast<void*>(ctrl)), layout);
                if (w.can & kMove)
                    place(w, static_cast<char*>(const_cast<void*>(ctrl)));
                partyrows::space(w.id, static_cast<char*>(const_cast<void*>(ctrl)), g_resize, w.move); // [client] party.spacing
                if (!w.hide) {
                    if (!w.frame) {
                        if (closed(ctrl))
                            return true;
                        mark_frameless(ctrl, w.name, (w.can & (kFollow | kFrame)) == kFollow ? "mainmenu" : w.id, w.frame_logged, w.solid);
                    }
                    return false;
                }
                if (!w.hide_logged) {
                    w.hide_logged = true;
                    log::write(L"%ls: left out of the window draw ([client] %hs.hide=1)", w.name, w.id);
                }
                return true;
            }
        }
        if (g_hide_gil && std::memcmp(layout + kLayoutName, kGilName, 16) == 0 && status_open())
            return true;
        bool looked = false; // one of g_looks: its own switches, not Menu Content's
        for (auto& look : g_looks) {
            if (look.layout[0] == 0 ? !over_anchor(static_cast<char*>(const_cast<void*>(ctrl)))
                                    : std::memcmp(layout + kLayoutName, look.layout, 16) != 0)
                continue;
            looked = true;
            if (look.hide) {
                if (!look.hide_logged) {
                    look.hide_logged = true;
                    log::write(L"%ls: left out of the window draw ([client] %hs.hide=1)", look.name, look.id);
                }
                return true;
            }
            if (!look.frame) {
                if (closed(ctrl))
                    return true;
                mark_frameless(ctrl, look.name, look.id, look.frame_logged, look.solid);
            }
            break;
        }
        Window* menu = window_named("mainmenu");
        Window* content = window_named("menucontent");
        if (!g_menu_frame && menu != nullptr && !looked && in_menu_slot(layout)) {
            if (closed(ctrl))
                return true;
            mark_frameless(ctrl, menu->name, menu->id, menu->frame_logged, menu->solid);
        } else if (!looked && content != nullptr && !content->frame && content_member(ctrl, layout)) {
            if (closed(ctrl))
                return true;
            mark_frameless(ctrl, content->name, content->id, content->frame_logged, content->solid);
        }
        follow_menu(static_cast<char*>(const_cast<void*>(ctrl)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

void left_out(const void* ctrl)
{
    if (ctrl == nullptr || g_win_calc == nullptr)
        return;
    char* window = static_cast<char*>(const_cast<void*>(ctrl));
    __try {
        if (at<std::uint32_t>(window, kWinState) != kWinClosed)
            return;
        g_win_calc(window); // (the game's own draw reads the window's input delay right after it, as here)
        if (at<short>(window, kWinInputDelay) > 0)
            --at<short>(window, kWinInputDelay);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

std::uint8_t frameless(const void* ctrl)
{
    if (ctrl == nullptr || ctrl != g_frameless)
        return 0;
    g_frameless = nullptr;
    __try {
        std::uint8_t& pieces = at<std::uint8_t>(const_cast<void*>(ctrl), kWinPieces);
        const std::uint8_t was = pieces;
        pieces = 0;
        if (was != 0 && g_frameless_solid)
            itemslot::frameless(true); // <id>.solid: its buttons' bars drawn solid, as on the frame
        if (was != 0 && g_frameless_logged != nullptr && !*g_frameless_logged) {
            *g_frameless_logged = true;
            log::write(L"%ls: drawn without its frame ([client] %hs.background=0)", g_frameless_name, g_frameless_key);
        }
        return was;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

void frame_back(const void* ctrl, std::uint8_t pieces)
{
    itemslot::frameless(false);
    if (ctrl == nullptr || pieces == 0)
        return;
    __try {
        at<std::uint8_t>(const_cast<void*>(ctrl), kWinPieces) = pieces;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The menu's title tab (its inactive last item, or the main menu's item 15) left out of the draw: its shown flag cleared,
// kept in `back`
void title_button(char* window, TitleBack& back)
{
    short title = 0;
    if ((at<std::uint32_t>(window, kWinFlags) & kWinTitled) != 0)
        title = at<short>(window, kWinCount);
    else if (named(window, "menu    menuwind") || named(window, "menu    socialme"))
        title = kMainMenuTitle;
    if (title <= 0)
        return;
    int count = 0;
    for (char* node = at<char*>(window, kWinPieceList); node != nullptr && count < 256; node = at<char*>(node, 0), ++count) {
        char* piece = at<char*>(node, kNodePiece);
        if (piece == nullptr || at<std::uint8_t>(node, kNodeOut) != 0 || at<short>(piece, 0) != kPieceButton ||
            at<short>(piece, kPieceId) != title)
            continue;
        std::uint8_t& shown = at<std::uint8_t>(piece, kPieceShown);
        if (at<std::uint32_t>(piece, kPieceStatus) != kButtonInactive || shown == 0)
            return;
        back.was = shown;
        back.shown = &shown;
        shown = 0;
        return;
    }
}

bool title_quad(char* quad)
{
    return at<short>(quad, kQuadY) <= 0 &&
           (std::memcmp(quad + kQuadName, kTitleTab, 16) == 0 || std::memcmp(quad + kQuadName, kTitleLetters, 16) == 0);
}

// The title in the window's frame pictures moved off it: each quad and its corners' places kept in `back` before it's moved
void picture_title(char* window, TitleBack& back)
{
    if (named(window, kTargetName))
        return;
    int count = 0;
    for (char* node = at<char*>(window, kWinPieceList); node != nullptr && count < 256; node = at<char*>(node, 0), ++count) {
        char* piece = at<char*>(node, kNodePiece);
        char* shape = piece != nullptr && at<std::uint8_t>(node, kNodeOut) == 0 && at<short>(piece, 0) == kPieceFrame
                          ? at<char*>(piece, kPiecePicture)
                          : nullptr;
        if (shape == nullptr)
            continue;
        const int quads = at<int>(shape, kShapeCount);
        int seen = 0;
        for (char* part = at<char*>(shape, kShapeQuads); part != nullptr && seen < quads && seen < 128 && back.quads < kTitleQuadsAtMost;
             part = at<char*>(part, 0), ++seen) {
            char* quad = at<char*>(part, kNodePiece);
            if (quad == nullptr || !title_quad(quad))
                continue;
            short* y = &at<short>(quad, kQuadY);
            const int i = back.quads;
            std::memcpy(back.y[i], y, sizeof back.y[i]);
            back.quad[i] = quad;
            back.quads = i + 1;
            for (int c = 0; c < 4; ++c)
                y[c] = static_cast<short>(back.y[i][c] + kTitleAway);
        }
    }
}

void untitled(const void* ctrl, TitleBack& back)
{
    if (!g_hide_titles || ctrl == nullptr)
        return;
    __try {
        char* window = static_cast<char*>(const_cast<void*>(ctrl));
        title_button(window, back);
        picture_title(window, back);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void title_back(TitleBack& back)
{
    __try {
        if (back.shown != nullptr && back.was != 0)
            *back.shown = back.was;
        for (int i = back.quads - 1; i >= 0; --i)
            std::memcpy(back.quad[i] + kQuadY, back.y[i], sizeof back.y[i]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    back.shown = nullptr;
    back.quads = 0;
}

bool faded(const void* ctrl)
{
    if (ctrl == nullptr)
        return false;
    __try {
        char* window = static_cast<char*>(const_cast<void*>(ctrl));
        bool frames = false, shown = false;
        int count = 0;
        for (char* node = at<char*>(window, kWinPieceList); node != nullptr && count < 256; node = at<char*>(node, 0), ++count) {
            char* piece = at<char*>(node, kNodePiece);
            if (piece == nullptr || at<std::uint8_t>(node, kNodeOut) != 0)
                continue;
            const short kind = at<short>(piece, 0);
            if (kind == kPieceButton)
                return false; // (commands to draw: not just a frame)
            if (kind != kPieceFrame)
                continue;
            frames = true;
            shown = shown || (at<std::uint8_t>(piece, kPieceUnseen) != 1 && at<float>(piece, kPieceAlpha) > 0.0f);
        }
        return frames && !shown;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

#include "hudwin_place.h" // beside, moved, raise_moved

void locate(const void* window, int& x, int& y)
{
    if (window == nullptr || g_set_locate == nullptr || window == g_placing)
        return;
    __try {
        auto* w = static_cast<char*>(const_cast<void*>(window));
        const int left = at<short>(w, kWinFrame), top = at<short>(w, kWinFrame + 2);
        // the EXP window (kStatus): its handler puts it back where it was made every frame; it goes where its element
        // says instead, under the Status window or at its X and Y (place puts it there before each draw as well)
        if (Window* table = table_window(w); table != nullptr && (table->can & kStatus) != 0) {
            const int width = at<short>(w, kWinFrame + 4) - left, height = at<short>(w, kWinFrame + 6) - top;
            int want_x = x, want_y = y;
            if (width > 0 && height > 0 && wanted(*table, table->move, width, height, want_x, want_y))
                x = want_x, y = want_y;
            return;
        }
        // put where it already is: the Status screen puts the EXP window back at its layout's place once it's open,
        // which place_frame_hook had moved with the window, so it went by the box twice (seen 2026-10-21: EXP at 286,
        // 486 for a box at 135, 139 and a layout at 16, 208). Status itself is put at 16, 16 there, away from its layout's
        // 16, 48, and moves once.
        if (x == left && y == top)
            return;
        // put where it was before status.taller moved it down (statuswindow.cpp: the EXP window under the taller Status
        // window, which the game puts back at its layout's place): taken for a place of the game's, it went by the box a
        // second time and stayed there, away from the Status window (reported 2026-10-24: 382, 356 from it, as for a box
        // 382, 164 from the game's place). It stays where status.taller put it
        // (the same for a window under the Equipment window made taller by equip.stats: an item's description)
        if (const int lowered = statuswindow::lowered(w) + equipstats::lowered(w) + itemtips::lowered(w);
            lowered != 0 && x == left && y == top - lowered) {
            y = top;
            return;
        }
        int ax = 0, ay = 0;
        const bool moved = content_shift(w, x, y, at<short>(w, kWinFrame + 4) - left, at<short>(w, kWinFrame + 6) - top, ax, ay);
        shifted(w, ax, ay);
        if (moved)
            x += ax, y += ay;
        x = beside(w, x, y); // (a window Menu Content moved is in the area already)
        if (const Window* table = table_window(w); table != nullptr && std::strcmp(table->id, "itemdetails") == 0 && !table->move)
            y = equipstats::below(x, x + at<short>(w, kWinFrame + 4) - left, y); // (under the item list: below the stats too)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

#include "hudwin_up.h" // up: the HUD Manager's Open only

#include "hudwin_scale.h" // <id>.scale: a window drawn bigger or smaller, the mouse's tests of it mapped back

#include "hudwin_outline.h" // outline: a table window's box, Shift + drag moving it

char* manager()
{
    return g_manager; // as skip's found() left it: no search of its own (one too early would be the only one)
}

} // namespace vanacore::hudwin
