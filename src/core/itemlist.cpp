#include "itemlist.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "chat.hpp"
#include "compat/ashita/host.hpp"
#include "hudwin.hpp"
#include "imports.hpp"
#include "log.hpp"
#include "scan.hpp"
#include "sigs.hpp"

namespace vanacore::itemlist {
namespace {

// The game's list windows (YkWndList in the PS2 client's names; the Items list is a YkWndItem, a storage's list in the
// Mog House a YkWndBank, both on the same code) show 10 rows: the list object's rows a page (+18h, SetLine's) and its
// window's menu resource, which lays out 10 button elements (one a row, 18 units apart) in a 190-unit frame, with the
// cursor element after them. The window (CTkMenuCtrlData) counts its buttons at +58h: the cursor's limit, the scroll
// bar's rows shown, and its mouse looks for the row under the pointer among the buttons. So, for more rows, the window
// gets more buttons: made as the game makes its own from the resource (SetResourceData, FUN_10117b40: new, the
// constructor, the resource's record, LocateCalc, ShapeCalc, AddTail), each a copy of the last row's record moved one
// pitch further down, then its frame is made taller with the game's own resize and the list told its new rows a page.
//
// The one place for it: YkWndList::OnInitialUpdatePrimitive (FUN_101f4fd0), which every list window runs through once
// its window is made, before it creates the scroll bar from the window's rect and rows: push esi; mov esi,ecx;
// mov ax,[esi+18h]; mov cx,[esi+20h]; push eax; push ecx; mov ecx,[esi+8]; call CreateScrollBar; mov edx,[esi+8];
// xor al,al; mov [edx+86h],al; mov ecx,[esi+8]; mov [ecx+85h],al
constexpr char kListInit[] = "56 8B F1 66 8B 46 18 66 8B 4E 20 50 51 8B 4E 08 E8 ?? ?? ?? ?? 8B 56 08 32 C0 88 82 86 00 "
                             "00 00 8B 4E 08 88 81 85 00 00 00";
constexpr std::size_t kListInitPrologue = 7;
// YkWndList::SetLine (FUN_101f4e20, __thiscall, ret 4: rows): the rows a page at +18h, the scroll room at +22h. The
// Items list's Open gives it 10 each time, before its window is made and again when the list is opened while it's
// up (another storage chosen in the Mog House), so it's hooked to keep a taller window's rows:
// mov dx,[esp+4]; mov ax,[ecx+20h]; sub ax,dx; mov [ecx+18h],dx; mov [ecx+22h],ax; jns; mov word ptr [ecx+22h],0
constexpr char kSetLine[] = "66 8B 54 24 04 66 8B 41 20 66 2B C2 66 89 51 18 66 89 41 22 79 06 66 C7 41 22 00 00 C2 04 00";
constexpr std::size_t kSetLinePrologue = 5;
// YkWndList::ClipCursor (FUN_101f4dd0, __thiscall): the rows drawn (+1Ah to +1Ch) from the cursor and the rows a page:
// mov ax,[ecx+1Eh]; mov dx,[ecx+18h]; push esi; push edi; mov di,[ecx+20h]; lea esi,[edx+eax]; cmp si,di; mov [ecx+1Ch],si
constexpr char kClipCursor[] = "66 8B 41 1E 66 8B 51 18 56 57 66 8B 79 20 8D 34 02 66 3B F7 66 89 71 1C 7E";
// CTkMenuCtrlData::SetResourceData's making of a button (FUN_10117b40 + 0D2h): push 60h; call new; add esp,4;
// test eax,eax; jz; push esi (the window); mov ecx,eax; call the constructor; mov edi,eax; test edi,edi; jz;
// inc word ptr [esi+58h]; push ebx (the resource's record); mov ecx,edi; call SetResourceData; movsx ecx,[esi+54h];
// mov al,[esi+99h]; movsx edx,[esi+52h]; push eax; push ecx; push edx; mov ecx,edi; call LocateCalc; mov al,[esi+99h];
// mov ecx,edi; push eax; call ShapeCalc; push edi; lea ecx,[esi+14h]; call AddTail
constexpr char kMakeButton[] = "6A 60 E8 ?? ?? ?? ?? 83 C4 04 85 C0 0F 84 ?? ?? ?? ?? 56 8B C8 E8 ?? ?? ?? ?? 8B F8 85 FF "
                               "0F 84 ?? ?? ?? ?? 66 FF 46 58 53 8B CF E8 ?? ?? ?? ?? 0F BF 4E 54 8A 86 99 00 00 00 "
                               "0F BF 56 52 50 51 52 8B CF E8 ?? ?? ?? ?? 8A 86 99 00 00 00 8B CF 50 E8 ?? ?? ?? ?? "
                               "57 8D 4E 14 E8";
constexpr std::ptrdiff_t kNewCall = 2, kCtorCall = 0x15, kSetResourceCall = 0x2B, kLocateCall = 0x43, kShapeCall = 0x51,
                         kAddTailCall = 0x5A;
constexpr unsigned kButtonSize = 0x60;
// The game's resize of a window (FUN_1011a1a0, __thiscall, ret 1Ch: x, y, width, height, then 1, 0, 0 to keep the
// size in its frame), as statuswindow.cpp uses it: sub esp,8; push esi; mov esi,ecx; lea ecx,[esi+14h]; call
constexpr char kResize[] = "83 EC 08 56 8B F1 8D 4E 14 E8 ?? ?? ?? ?? 85 C0 89 44 24 08 C7 44 24 04 00 00 00 00 0F 84";
// CTkMenuCtrlData::RePosition (FUN_10118d10, __thiscall, ret 8: x, y), which the item details window is placed with
// (16, 240: under a list's 190-unit window at 48) when it opens and whenever its lines change: sub esp,0Ch;
// mov ax,[esp+14h]; push ebx; push ebp; mov ebp,[esp+18h]; push esi; mov esi,ecx; push edi; xor edi,edi; lea ebx,[esi+14h];
// mov [esi+52h],bp; mov ecx,ebx; mov [esi+54h],ax
constexpr char kReposition[] = "83 EC 0C 66 8B 44 24 14 53 55 8B 6C 24 18 56 8B F1 57 33 FF 8D 5E 14 66 89 6E 52 8B CB "
                               "66 89 46 54";
constexpr std::size_t kRepositionPrologue = 8;
// YkWndItem::Open (FUN_101f0b00): the list's controller (+5Ch) is the one given, or the Items menu's own (g_pItemCtrlList):
// mov eax,[esp+4]; push esi; test eax,eax; mov esi,ecx; jnz; mov eax,[g_pItemCtrlList]; mov ecx,eax; mov [esi+5Ch],eax;
// mov edx,[ecx]; call [edx+54h]; mov [esi+64h],eax; dec eax; cmp eax,10h
constexpr char kOpen[] = "8B 44 24 04 56 85 C0 8B F1 75 05 A1 ?? ?? ?? ?? 8B C8 89 46 5C 8B 11 FF 52 54 89 46 64 48 83 F8 10";
constexpr std::ptrdiff_t kItemCtrlListAt = 0xC;
// Further in it, the item details window (g_pTkItemInfo) opened for the list: mov eax,[g_pTkItemInfo]; test eax,eax;
// jz; push 0; call IsEvent; add esp,4; test al,al; jnz; mov ecx,[esi+5Ch]; push 0; push 0; push 1; mov edx,[ecx];
// call [edx]; mov ecx,[g_pTkItemInfo]
constexpr char kItemInfo[] = "A1 ?? ?? ?? ?? 85 C0 74 3E 6A 00 E8 ?? ?? ?? ?? 83 C4 04 84 C0 75 30 8B 4E 5C 6A 00 6A 00 "
                             "6A 01 8B 11 FF 12 8B 0D";
constexpr std::ptrdiff_t kItemInfoAt = 1;
// The Mog House's opening of a storage (FUN_101d9570): its Move controller (g_pItemCtrlMove, the one the inventory's
// list beside the storage's is opened with) told the storage, then the storage's list (g_pYkWndBank) opened:
// mov ecx,[g_pItemCtrlMove]; push 0; mov edx,[ecx]; call [edx+58h]; mov eax,[g_pYkWndBank]; push 0;
// mov byte ptr [eax+70h],1; mov [eax+71h],bl; mov eax,[esp+14h]; mov ecx,[g_pYkWndBank]; push eax; call Open
constexpr char kViewHouse[] = "8B 0D ?? ?? ?? ?? 6A 00 8B 11 FF 52 58 A1 ?? ?? ?? ?? 6A 00 C6 40 70 01 88 58 71 8B 44 24 14 "
                              "8B 0D ?? ?? ?? ?? 50 E8";
constexpr std::ptrdiff_t kItemCtrlMoveAt = 2, kBankWindowAt = 0xE;
// The same routine's opening of the inventory's list without a storage (g_pYkWndItem, the Items list object): call;
// test al,al; jnz; mov ecx,[g_pYkWndItem]; push esi; call Open; mov ecx,[g_pTkMenuWind]; push 6; call
constexpr char kItemWindow[] = "E8 ?? ?? ?? ?? 84 C0 75 1E 8B 0D ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 6A 06 E8";
constexpr std::ptrdiff_t kItemWindowAt = 11, kItemWindowOpenCall = 0x10;
// The action menu's Items (FUN_1012b3c0: the list of what can be used, opened with the Use controller, g_pItemCtrlUse):
// ret 4; mov ecx,[the inventory store]; push 1; push 0; call (its sort key); mov eax,[g_pItemCtrlUse]; mov ecx,[g_pYkWndItem];
// push eax; call Open; add esp,40h; ret 4
constexpr char kItemsUse[] = "C2 04 00 8B 0D ?? ?? ?? ?? 6A 01 6A 00 E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? 83 C4 40 C2 04 00";
constexpr std::ptrdiff_t kItemCtrlUseAt = 19, kItemsUseOpenCall = 30;
// The auction house's Sell (at 0x101D8DC8: the inventory's list opened with the auction's own controller, then its menu
// "menu    auc2" made): add esp,8; push eax; call; mov eax,[the Sell controller]; mov ecx,[g_pYkWndItem]; push eax;
// call Open; ret 8; push 0; push 1; push "menu    auc2"
constexpr char kAuctionSell[] = "83 C4 08 50 E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? C2 08 00 6A 00 6A 01 68";
constexpr std::ptrdiff_t kItemCtrlSellAt = 10, kAuctionSellOpenCall = 0x15;
// The Equipment menu's list of what can go in a slot (at 0x10132DCA, before it makes "menu    equip   "): the inventory's
// list opened with the equipment's own controller (g_pTkEquipCtrl), which is told the slot (vtable +58h) first:
// mov ecx,[g_pTkEquipCtrl]; push 0; mov edx,[ecx]; call [edx+58h]; mov eax,[g_pTkEquipCtrl]; mov ecx,[g_pYkWndItem];
// push eax; call Open
constexpr char kEquipList[] = "8B 0D ?? ?? ?? ?? 6A 00 8B 11 FF 52 58 A1 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 50 E8";
constexpr std::ptrdiff_t kItemCtrlEquipAt = 14, kEquipOpenCall = 25;
// A shop's buying (YkWndShopBuyCtrl, ykWndShop.cpp in the PS2 client): its OnOK opens the quantity window with this
// callback (FUN_1020e4d0, __cdecl: the quantity, the window's user), which opens the Yes/No (g_pYkWndShopBuyYn) for
// that many; followed by the quantity window's cancel (the Buy controller g_pShopBuyCtrl's +10h cleared) and the Yes/No's
// callback: mov eax,[esp+4]; test eax,eax; jle; mov ecx,[g_pYkWndShopBuyYn]; push 0; push 1; push 0; push 0; push 0;
// push eax; push (the Yes/No's callback); call Open; ret; mov eax,[g_pShopBuyCtrl]; mov byte ptr [eax+10h],0; ret
constexpr char kShopBuy[] = "8B 44 24 04 85 C0 7E 1C 8B 0D ?? ?? ?? ?? 6A 00 6A 01 6A 00 6A 00 6A 00 50 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? C3 "
                            "A1 ?? ?? ?? ?? C6 40 10 00 C3";
constexpr std::size_t kShopBuyPrologue = 6;
constexpr std::ptrdiff_t kShopBuyCtrlAt = 0x25;
constexpr std::ptrdiff_t kShopSelectedAt = 0x11; // the Buy controller's row picked: the shop's own index of the item
// The same OnOK's looking up of that item (FUN_1020e3a0 + 3Ah): the shop's list (gcShopItemList, __cdecl: where to put
// the entry, its index; the entry +0 its price, +0Ch its item id), then the item's data (CTkItemData::GetItemDataMenber,
// __thiscall on g_pTkItemData: the id): lea edx,[esp+10h]; push ebx; push edx; call; add esp,8; test al,al; jz;
// mov ebp,[esp+10h]; mov eax,[esp+24h]; cmp [ebp],eax; ja; add ebp,0Ch; test ebp,ebp; jz; xor ecx,ecx; mov cx,[ebp];
// push ecx; mov ecx,[g_pTkItemData]; call
constexpr char kShopItem[] = "8D 54 24 10 53 52 E8 ?? ?? ?? ?? 83 C4 08 84 C0 74 ?? 8B 6C 24 10 8B 44 24 24 39 45 00 77 ?? "
                             "83 C5 0C 85 ED 74 ?? 33 C9 66 8B 4D 00 51 8B 0D ?? ?? ?? ?? E8";
constexpr std::ptrdiff_t kShopListCall = 6, kItemDataAt = 0x2F, kItemDataCall = 0x33;
constexpr std::ptrdiff_t kEntryIdAt = 0xC;
// (retail's build has the item's data fields 0x800 further on: sigs::item_data_shift)
constexpr std::ptrdiff_t kItemNameAt = 0xCC8; // an item's data: its name as the lists show it (the shop's own column)
// The windows' list, from the menu manager's drawing of them, as statuswindow.cpp finds it (node +0 next, +10h window)
constexpr char kWindows[] = "32 DB B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 89 44 24 0C 74 3A 8D 44 24 0C B9 ?? ?? ?? ?? 50 E8";
constexpr std::ptrdiff_t kWindowsAt = 3;
// KmItem::Sort (FUN_1021aea0, __thiscall on a bag's item store, ret 10h: filter, sort key, 12h, 0), which the Items
// window's Sort option runs for the bag (its MakeList: push 0; push 12h; push 1; push 0; mov ecx,[esi+60h]; call). A
// bag's store keeps a table of its items for each sort key (+0Ch, the one its list is drawn by; 1 is the sorted order),
// filled in slot order as the items come from the server and ordered only by this. Retail's server puts the slots in the
// sorted order when asked to sort, so a login comes sorted; this server's sort only merges stacks, so every login comes
// in slot order again. mov eax,[zone]; sub esp,8; test eax,eax; push ebx; push ebp; push esi; push edi; mov edx,ecx; jz;
// mov eax,[esp+1Ch]; mov ebx,[esp+20h]; mov [edx+10h],eax; mov al,[esp+28h]; test al,al; mov [edx+0Ch],ebx
constexpr char kSort[] = "A1 ?? ?? ?? ?? 83 EC 08 85 C0 53 55 56 57 8B D1 0F 84 ?? ?? ?? ?? 8B 44 24 1C 8B 5C 24 20 89 42 10 "
                        "8A 44 24 28 84 C0 89 5A 0C";
constexpr int kSortedKey = 1, kSortArg = 0x12;
// Further in it, the table's items (8 bytes each: +0 their kind, +4 the item's id, +6 its slot; the first is the table's
// own) put in order by qsort with the game's comparison (by kind, then id): and eax,0FFh; push (the comparison); dec eax;
// push 8; push eax
constexpr char kSortCompare[] = "25 FF 00 00 00 68 ?? ?? ?? ?? 48 6A 08 50";
constexpr std::ptrdiff_t kSortCompareAt = 6;
constexpr std::size_t kSortLength = 0x300; // how far into the sort its comparison is looked for
constexpr std::ptrdiff_t kEntryItemAt = 4;
// YkWndBank::MakeList (FUN_101da9d0, the storage list's rows made again each frame): a row's colour is asked of the
// controller (its filter: white, grey for an item that can't be moved...) only for rows from 10 before the first drawn
// row to 20 after it, the game's 10 rows with margins, and the first 10; every other row is left the grey default. For
// more rows the 20 is made the rows and the margin: movsx eax,[esp+14h] (the first drawn row); xor ebp,ebp;
// mov ebx,80404040h; lea ecx,[eax-0Ah]; cmp ecx,[esp+10h] (the row); jg; mov ecx,[esp+10h]; add eax,14h; cmp ecx,eax;
// jle; cmp dx,[esp+14h]; jl; cmp dx,0Ah; jge
constexpr char kBankColours[] = "0F BF 44 24 14 33 ED BB 40 40 40 80 8D 48 F6 3B 4C 24 10 7F 0B 8B 4C 24 10 83 C0 14 3B C8 7E 0D "
                                "66 3B 54 24 14 7C 2D 66 83 FA 0A 7D 27";
constexpr std::ptrdiff_t kBankColoursAt = 0x1B; // the 14h
constexpr int kBankColoursMargin = 10, kBankColoursMost = 127; // an imm8

// The lists over the chat (Magic, Abilities, Weapon Skills, Key Items...: the "Ka" menus, kamenu*.cpp in the PS2 client)
// are another list object on code of their own, with a window made from a menu resource that lays out 12 buttons (one a
// row, 16 units apart), placed by the layout over the chat (its bottom on the chat's top). Their list's setting up
// (FUN_1021ffe0, __thiscall, ret 14h: a row's drawing, the count, the room, 0, the rows a page) gives the rows a page
// (+48h) and tells the window's scroll bar; Magic's Create gives 12. So, for more rows, the window gets more buttons
// (as the item lists' do) and grows upward, and the list is given its new rows a page:
// push ebx; push ebp; push esi; mov esi,ecx; xor ebx,ebx; push edi; mov eax,[esi+20h]; cmp eax,ebx; jz; add eax,-4;
// push eax; call delete; add esp,4; mov [esi+20h],ebx; mov edi,[esp+1Ch]; xor eax,eax; cmp edi,ebx; mov [esi+44h],ebx
constexpr char kChatListSetup[] = "53 55 56 8B F1 33 DB 57 8B 46 20 3B C3 74 0F 83 C0 FC 50 E8 ?? ?? ?? ?? 83 C4 04 89 5E 20 "
                                  "8B 7C 24 1C 33 C0 3B FB 89 5E 44";
constexpr std::size_t kChatListSetupPrologue = 5;
// Their window made (FUN_102205b0, the list's): the scroll bar created empty, then the list's own (vtable +54h). A list
// set up before its window was made is caught up here: push esi; mov esi,ecx; mov ecx,[esi+8]; test ecx,ecx; jz;
// push 0; push 0; call; mov eax,[esi+8]; mov byte ptr [eax+85h],0; mov edx,[esi]; mov ecx,esi; pop esi; jmp [edx+54h]
constexpr char kChatListMade[] = "56 8B F1 8B 4E 08 85 C9 74 13 6A 00 6A 00 E8 ?? ?? ?? ?? 8B 46 08 C6 80 85 00 00 00 00 8B 16 "
                                 "8B CE 5E FF 62 54";
constexpr std::size_t kChatListMadePrologue = 6;
// Such a list: its window +8, its cursor's row +30h, the top row drawn +38h, the last top row +3Ch, the rows +40h, the
// rows a page +48h. Its window's scroll bar (+88h): the rows shown +0Ch, the rows +10h, the top row +14h
constexpr std::ptrdiff_t kChatTopAt = 0x38, kChatLastTopAt = 0x3C, kChatCountAt = 0x40, kChatPageAt = 0x48;
constexpr std::ptrdiff_t kBarAt = 0x88, kBarShownAt = 0xC, kBarCountAt = 0x10, kBarTopAt = 0x14;
// A window's flags (+34h): 1000h, placed by the layout over the window it goes with (the chat, for these)
constexpr std::ptrdiff_t kFlagsAt = 0x34;
constexpr std::uint32_t kOverAnchor = 0x1000;
constexpr int kChatGameRows = 12, kChatMaxRows = 40;
constexpr int kChatTopMost = 16; // the layout's highest top for a window over the chat
// The Currencies list (Status > Currencies and Currencies 2: "menu    evitem  ", one window for both) is a list on the
// same code (its vtable 0x1033A2D0, its setting up from 0x10228A20 with 8 rows a page; seen live 2026-11-09: 8 buttons
// 29 units apart, 99 rows), opened beside the Status sub-menu, not over the chat: it grows downward, as far as the
// interface's bottom less the same margin
constexpr char kCurrencyName[16] = {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 'e', 'v', 'i', 't', 'e', 'm', ' ', ' '};
constexpr int kCurrencyGameRows = 8, kCurrencyMaxRows = 30;

// A list object (YkWndList): its window at +8, rows a page +18h, its row pitch +26h, its controller (YkWndItem) +5Ch
// and the container it lists +64h (0 the inventory, else a Mog House storage: the Mog Safe viewed from outside the house)
constexpr std::ptrdiff_t kWindowAt = 8, kPageAt = 0x18, kPitchAt = 0x26, kControllerAt = 0x5C, kContainerAt = 0x64;
// The bag's item store (KmItem) a list draws: a YkWndItem's at +60h, a YkWndBank's (the Mog House's storage list) at +5Ch
constexpr std::ptrdiff_t kItemStoreAt = 0x60, kBankStoreAt = 0x5C;
// A window (CTkMenuCtrlData): its menu data at +4 (the resource's name 16 characters at its +46h), its primitive +0Ch,
// its elements' list +14h (node +0 next, +10h element), rect +3Ah (left, top, right, bottom), position +52h/+54h,
// buttons +58h, zoom +99h, the area it keeps clear +BCh (its top at +BEh)
constexpr std::ptrdiff_t kMenuDataAt = 4, kNameAt = 0x46, kPrimitiveAt = 0xC, kElementsAt = 0x14, kNodeNext = 0,
                         kNodeElement = 0x10, kRectAt = 0x3A, kXAt = 0x52, kYAt = 0x54, kButtonsAt = 0x58, kZoomAt = 0x99,
                         kAreaTopAt = 0xBE;
// A button element (CTkDrawCtrlButton): its type at +0 (1), the resource's record +0Ch, its rect in the window +32h
// (left, top, right, bottom), its number +3Ah (from 1)
constexpr std::ptrdiff_t kTypeAt = 0, kRecordAt = 0xC, kLeftAt = 0x32, kTopAt = 0x34, kBottomAt = 0x38, kNumberAt = 0x3A;
constexpr short kButtonType = 1;
constexpr char kInventoryName[16] = {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 'i', 'n', 'v', 'e', 'n', 't', 'o', 'r'};
constexpr char kBankName[16] = {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 'b', 'a', 'n', 'k', ' ', ' ', ' ', ' '};
// The auction house's list of a category's items (its list object on the same YkWndList code, 6 columns, the item
// details window under it at the same place)
constexpr char kAuctionName[16] = {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 'a', 'u', 'c', 'l', 'i', 's', 't', ' '};
// An NPC's shop (YkWndShop: its list object on the same YkWndList code, 4 columns), both what it sells and what of
// yours it buys (the one window, given the Buy or the Sell controller). Its OnCreate opens the item details window to
// the right (130, 240) and the gil window under the list (16, 240)
constexpr char kShopName[16] = {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 's', 'h', 'o', 'p', ' ', ' ', ' ', ' '};
constexpr char kMoneyName[16] = {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 'm', 'o', 'n', 'e', 'y', ' ', ' ', ' '};
// The quantity window (CTkItemCtrl: how many to buy, sell, move...), at the same place from its menu resource, over the
// gil window
constexpr char kQuantityName[16] = {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 'i', 't', 'e', 'm', 'c', 't', 'r', 'l'};
// The action menu's own window (Attack, Magic, Items...), at the screen's bottom left, which the Use list sits over
constexpr char kActionMenuName[16] = {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 'p', 'l', 'a', 'y', 'e', 'r', 'm', 'o'};
constexpr int kAboveActionMenu = 2; // units kept between the list's bottom and the action menu's top
constexpr int kGameRows = 10, kMaxRows = 50;
constexpr int kEquipKind = 5; // list_kind's 1 to 4, and the Equipment menu's list (the inventory's window: only extend's)
constexpr short kInfoX = 16, kInfoY = 240; // the item details window's place under a list
constexpr short kInfoRightX = 130;          // and beside it (a shop's: the gil window under the list)
constexpr int kListsAtMost = 256;           // nodes walked at most

// thiscall functions, called through __fastcall (the same stack; the unused edx)
using ListInit = void(__fastcall*)(char* self, void* edx);
using SetLine = void(__fastcall*)(char* self, void* edx, int rows);
using ClipCursor = void(__fastcall*)(char* self, void* edx);
using Reposition = void(__fastcall*)(char* ctrl, void* edx, int x, int y);
using Resize = void(__fastcall*)(char* ctrl, void* edx, int x, int y, int width, int height, int keep, int, int);
using New = void*(__cdecl*)(unsigned size);
using Ctor = char*(__fastcall*)(char* button, void* edx, char* ctrl);
using SetResource = void(__fastcall*)(char* button, void* edx, void* record);
using Locate = void(__fastcall*)(char* button, void* edx, int x, int y, int zoom);
using ShapeCalc = void(__fastcall*)(char* button, void* edx, int zoom);
using AddTail = void(__fastcall*)(char* list, void* edx, char* element);
using Sort = void(__fastcall*)(char* store, void* edx, int filter, int key, int arg, int flag);
using Compare = int(__cdecl*)(const void* a, const void* b);
using ShopBuy = void(__cdecl*)(int quantity, void* user);
using ShopEntry = bool(__cdecl*)(const char** entry, int index);
using ItemData = char*(__fastcall*)(void* data, void* edx, int id);
using ChatListSetup = void(__fastcall*)(char* self, void* edx, void* draw, int count, int room, int zero, int rows);
using ChatListMade = void(__fastcall*)(char* self, void* edx);

int g_rows = kGameRows;  // [client] items.rows
int g_chat_rows = kChatGameRows; // [client] lists.rows
int g_currency_rows = kCurrencyGameRows; // [client] currencies.rows
ChatListSetup g_chat_setup = nullptr; // the game's, through the trampolines
ChatListMade g_chat_made = nullptr;
bool g_chat_logged[2] = {}; // the first list over the chat grown logged, the first Currencies list
int g_auction_rows = kGameRows; // [client] auction.rows
int g_shop_rows = kGameRows;    // [client] shop.rows
int g_equip_rows = kGameRows;   // [client] equip.rows
bool g_autosort = true; // [client] items.autosort: a bag's list sorted as it opens
bool g_shop_total = true;       // [client] shop.total: what a shop's purchase will cost, in the chat, as it's asked
ShopBuy g_shop_buy = nullptr;   // the game's, through the trampoline
ShopEntry g_shop_entry = nullptr;
ItemData g_item_data = nullptr;
void* const* g_item_data_store = nullptr; // g_pTkItemData
char* const* g_shop_buy_ctrl = nullptr;   // g_pShopBuyCtrl
bool g_tried = false;
ListInit g_list_init = nullptr; // the game's, through the trampolines
SetLine g_set_line = nullptr;
Reposition g_reposition = nullptr;
ClipCursor g_clip = nullptr;
Resize g_resize = nullptr;
New g_new = nullptr;
Ctor g_ctor = nullptr;
SetResource g_set_resource = nullptr;
Locate g_locate = nullptr;
ShapeCalc g_shape = nullptr;
AddTail g_add_tail = nullptr;
Sort g_sort = nullptr; // null: items.autosort does nothing
bool g_az = false;                    // [client] items.sort=az: the sort puts items in A-Z order by name
std::uintptr_t g_sort_compare = 0;    // where the sort pushes its comparison (0: not found, items.sort does nothing)
Compare g_game_compare = nullptr;     // the game's comparison
bool g_names_warmed = false;          // the item names' DATs being read ahead, off the game's thread
std::uintptr_t g_bank_colours = 0; // the storage list's colour window's rows after the first drawn row (0: not found)
void* const* g_item_ctrl_list = nullptr; // the Items menu's controller
void* const* g_item_ctrl_move = nullptr; // the Mog House's (the inventory beside a storage)
void* const* g_item_ctrl_use = nullptr;  // the action menu's Items (what can be used)
void* const* g_item_ctrl_sell = nullptr; // the auction house's Sell (null: not found, its list keeps the game's 10)
void* const* g_item_ctrl_equip = nullptr; // the Equipment menu's (null: not found, its list keeps the game's 10)
char* const* g_item_info = nullptr;      // the item details window's object (its window at +8)
// The item details window's place as the game last gave it (reposition_hook), and the window it gave it for: its
// position (+52h/+54h) holds where hudwin moved it to instead (seen live 2026-10-22: 1554, 320 for the game's 16, 240),
// so move_item_info goes by these
char* g_info_ctrl = nullptr;
short g_info_x = 0, g_info_y = 0;
char* const* g_windows = nullptr;        // the windows' list's first node
char* const* g_item_window = nullptr;    // the Items list object (g_pYkWndItem) and the Mog House's storage list's
char* const* g_bank_window = nullptr;    // (g_pYkWndBank): the two whose bags items.autosort sorts
int g_pitch = 18;                        // a row, in the interface's units
bool g_logged[kEquipKind] = {};          // the first extension of each window logged

std::uintptr_t call_target(std::uintptr_t call)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), 4);
    return call + 5 + rel;
}

bool is_call(std::uintptr_t at) { return *reinterpret_cast<const std::uint8_t*>(at) == 0xE8; }

template <typename T> T operand(std::uintptr_t at)
{
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(at), sizeof value);
    return value;
}

template <typename T> T& field(char* base, std::ptrdiff_t offset) { return *reinterpret_cast<T*>(base + offset); }

// Which list window `ctrl` is: 1 the inventory's, 2 a storage's, 3 the auction house's, 4 a shop's, 0 none of them
int list_kind(char* ctrl)
{
    char* data = field<char*>(ctrl, kMenuDataAt);
    if (data == nullptr)
        return 0;
    if (std::memcmp(data + kNameAt, kInventoryName, sizeof kInventoryName) == 0)
        return 1;
    if (std::memcmp(data + kNameAt, kBankName, sizeof kBankName) == 0)
        return 2;
    if (std::memcmp(data + kNameAt, kAuctionName, sizeof kAuctionName) == 0)
        return 3;
    if (std::memcmp(data + kNameAt, kShopName, sizeof kShopName) == 0)
        return 4;
    return 0;
}

// The most rows any list is set to have
int most_rows()
{
    int most = g_rows > g_auction_rows ? g_rows : g_auction_rows;
    if (g_shop_rows > most)
        most = g_shop_rows;
    return g_equip_rows > most ? g_equip_rows : most;
}

// How much taller than the game's the list windows up now are (the tallest), in the interface's units
int extra_height()
{
    if (g_windows == nullptr)
        return 0;
    int extra = 0;
    int nodes = 0;
    // A window can be torn down (a character switch, a zone) while its node is still linked: its element and the node's
    // next are then unreadable, so the walk stops there
    __try {
        for (char* node = *g_windows; node != nullptr && nodes < kListsAtMost; node = field<char*>(node, kNodeNext), ++nodes) {
            char* ctrl = field<char*>(node, kNodeElement);
            if (ctrl == nullptr || list_kind(ctrl) == 0)
                continue;
            const int rows = field<short>(ctrl, kButtonsAt) - kGameRows;
            if (rows * g_pitch > extra)
                extra = rows * g_pitch;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return extra;
}

// The list window VanaCore gave rows past the game's ten whose menu data is `data`, if one is up
char* extended_window(const void* data)
{
    if (g_windows == nullptr || data == nullptr)
        return nullptr;
    int nodes = 0;
    // As extra_height: a node whose window is already gone (the key handler runs while one is torn down) faults here
    __try {
        for (char* node = *g_windows; node != nullptr && nodes < kListsAtMost; node = field<char*>(node, kNodeNext), ++nodes) {
            char* ctrl = field<char*>(node, kNodeElement);
            if (ctrl != nullptr && field<const void*>(ctrl, kMenuDataAt) == data && list_kind(ctrl) != 0 &&
                field<short>(ctrl, kButtonsAt) > kGameRows)
                return ctrl;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

void write_code(std::uintptr_t at, const void* bytes, size_t size)
{
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), size, PAGE_EXECUTE_READWRITE, &old))
        return;
    std::memcpy(reinterpret_cast<void*>(at), bytes, size);
    VirtualProtect(reinterpret_cast<void*>(at), size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), size);
}

// The storage list's colour window: the rows a page and the game's margin after the first drawn row (the game's 20)
void apply_bank_colours()
{
    if (g_bank_colours == 0)
        return;
    int rows = g_rows > kGameRows ? g_rows + kBankColoursMargin : kGameRows + kBankColoursMargin;
    if (rows > kBankColoursMost)
        rows = kBankColoursMost;
    const auto byte = static_cast<std::uint8_t>(rows);
    write_code(g_bank_colours + kBankColoursAt, &byte, 1);
}

// items.sort=az: two of a bag's sorted table's items by their names (as the item DATs have them in the game's language),
// the game's own order (kind, then id) between items of the same name or without one
int __cdecl az_compare(const void* a, const void* b)
{
    const auto id_a = *reinterpret_cast<const std::uint16_t*>(static_cast<const char*>(a) + kEntryItemAt);
    const auto id_b = *reinterpret_cast<const std::uint16_t*>(static_cast<const char*>(b) + kEntryItemAt);
    if (id_a != id_b) {
        const char* name_a = ashita::resource_item_name(id_a);
        const char* name_b = ashita::resource_item_name(id_b);
        if (name_a != nullptr && name_b != nullptr) {
            const int order = _stricmp(name_a, name_b);
            if (order != 0)
                return order;
        } else if (name_a != nullptr || name_b != nullptr)
            return name_a != nullptr ? -1 : 1; // named items first
    }
    return g_game_compare(a, b);
}

// The sort's comparison: A-Z for items.sort=az, else the game's
void apply_compare()
{
    if (g_sort_compare == 0)
        return;
    const auto compare = reinterpret_cast<std::uintptr_t>(g_az ? &az_compare : g_game_compare);
    if (operand<std::uintptr_t>(g_sort_compare + kSortCompareAt) != compare)
        write_code(g_sort_compare + kSortCompareAt, &compare, sizeof compare);
}

// The item names read ahead on a thread of their own, so the first A-Z sort doesn't wait for the DATs
DWORD WINAPI warm_names(void*)
{
    ashita::resource_item_name(0);
    return 0;
}

// The item details window, if it's up at its place under a list, moved down by the lists' extra height, and put by the
// list it's under once that list is placed. The game places it as a list opens, before the list's window is made, so
// the placing hook finds no taller list then, and hudwin finds no list to put it by (it went by the Inventory box, to
// the screen's far right, and stayed there until the game placed it again: a description with another number of lines,
// never for the auction house's fishing gear, seen 2026-10-22); this catches it up (from extend and each frame): where
// hudwin puts it now, if that isn't where its frame is. The place the game gave RePosition is what's looked at (the
// position, +52h/+54h, for a window placed without it), not its rect: RePosition sets the rect only through the
// window's frame (its primitive), which isn't made yet when it's first placed, and is laid out from the position once
// it is. Its init also puts the area's top back to the game's 240 after placing it.
// A shop's gil window and the quantity window, opened under a list at the item details window's place (16, 240) with
// no placing of their own (from their menu resources), are moved down the same way.
// The details' text box (CTkItemInfo +0x34E: left, top, right, bottom; its lines are laid in it), which it takes from its
// window as the game places the window, the window's rect then: kept on the rect each frame (the same size, elsewhere),
// for a rect moved after. equip.stats moves the windows under the Equipment window by their rect alone (equipstats.cpp
// lower_under) once the window has grown, which is after the game placed the details there: the frame went below the
// stats and the lines stayed under them (seen live 2026-11-06: box 130,265-496,353, position 130,265, rect 130,454-496,542;
// the box set to the rect by hand drew the lines in the frame at once). The rect already where hudwin puts it, the
// position (+52h/+54h) was left behind as well: it's moved there too, so the game's laying the rect from it keeps it.
constexpr std::ptrdiff_t kInfoTextAt = 0x34E;
void info_text_follows(char* info, char* ctrl)
{
    short* box = &field<short>(info, kInfoTextAt);
    const short* rect = &field<short>(ctrl, kRectAt);
    if (box[2] <= box[0] || box[3] <= box[1] || box[2] - box[0] != rect[2] - rect[0] || box[3] - box[1] != rect[3] - rect[1])
        return; // (none laid yet, or not laid for this frame)
    if (box[0] != rect[0] || box[1] != rect[1])
        std::memcpy(box, rect, 4 * sizeof(short));
}

// The item details window `ctrl` (of `info`), up at its place under a list, where hudwin puts it now
void place_item_info(char* info, char* ctrl)
{
    short x = field<short>(ctrl, kXAt), y = field<short>(ctrl, kYAt);
    if (ctrl == g_info_ctrl)
        x = g_info_x, y = g_info_y; // (the game's place, not where hudwin moved it to)
    if (x == kInfoX || x == kInfoRightX) {
        const bool placed = y == kInfoY;
        short& area_top = field<short>(ctrl, kAreaTopAt);
        const int extra = placed || area_top == kInfoY ? extra_height() : 0;
        if (placed && field<void*>(ctrl, kPrimitiveAt) != nullptr) {
            int to_x = x, to_y = kInfoY + extra;
            hudwin::locate(ctrl, to_x, to_y);
            if (to_x != field<short>(ctrl, kRectAt) || to_y != field<short>(ctrl, kRectAt + 2) || to_x != field<short>(ctrl, kXAt) ||
                to_y != field<short>(ctrl, kYAt))
                g_reposition(ctrl, nullptr, to_x, to_y);
        }
        if (extra != 0 && area_top == kInfoY)
            area_top = static_cast<short>(kInfoY + extra);
    }
    if (field<void*>(ctrl, kPrimitiveAt) != nullptr)
        info_text_follows(info, ctrl);
}

void move_item_info()
{
    char* info = g_item_info != nullptr ? *g_item_info : nullptr;
    if (char* ctrl = info != nullptr ? field<char*>(info, kWindowAt) : nullptr; ctrl != nullptr)
        place_item_info(info, ctrl);
    if (g_windows == nullptr)
        return;
    char* under[2] = {}; // the gil window, the quantity window: at the place under the game's list
    int nodes = 0;
    __try { // as extra_height: a window torn down while its node is still linked
        for (char* node = *g_windows; node != nullptr && nodes < kListsAtMost; node = field<char*>(node, kNodeNext), ++nodes) {
            char* other = field<char*>(node, kNodeElement);
            char* data = other != nullptr ? field<char*>(other, kMenuDataAt) : nullptr;
            if (data == nullptr || field<short>(other, kXAt) != kInfoX || field<short>(other, kYAt) != kInfoY)
                continue;
            if (std::memcmp(data + kNameAt, kMoneyName, sizeof kMoneyName) == 0)
                under[0] = other;
            else if (std::memcmp(data + kNameAt, kQuantityName, sizeof kQuantityName) == 0)
                under[1] = other;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (under[0] == nullptr && under[1] == nullptr)
        return;
    const int extra = extra_height();
    if (extra == 0)
        return;
    for (char* window : under)
        if (window != nullptr) {
            int to_x = kInfoX, to_y = kInfoY + extra;
            hudwin::locate(window, to_x, to_y);
            g_reposition(window, nullptr, to_x, to_y);
        }
}

// Buttons for window `ctrl`'s rows after its last, `last` (numbered `last_number`), up to `rows`: each a copy of the last's
// record `pitch` further down, made as the game makes its own. How many were made
int add_rows(char* ctrl, char* last, short last_number, int pitch, int rows)
{
    void* record = field<void*>(last, kRecordAt);
    if (record == nullptr)
        return 0;
    const int x = field<short>(ctrl, kXAt), y = field<short>(ctrl, kYAt);
    const int zoom = field<std::uint8_t>(ctrl, kZoomAt);
    const short top = field<short>(last, kTopAt), bottom = field<short>(last, kBottomAt);
    int added = 0;
    for (int i = 1; last_number + i <= rows; ++i) {
        auto* button = static_cast<char*>(g_new(kButtonSize));
        if (button == nullptr || g_ctor(button, nullptr, ctrl) == nullptr)
            break;
        g_set_resource(button, nullptr, record);
        field<short>(button, kTopAt) = static_cast<short>(top + pitch * i);
        field<short>(button, kBottomAt) = static_cast<short>(bottom + pitch * i);
        field<short>(button, kNumberAt) = static_cast<short>(last_number + i);
        g_locate(button, nullptr, x, y, zoom);
        g_shape(button, nullptr, zoom);
        g_add_tail(ctrl + kElementsAt, nullptr, button);
        ++field<short>(ctrl, kButtonsAt);
        ++added;
    }
    return added;
}

// `rows` rows for the list `self` in its window `ctrl`: buttons after the game's for the rows past them, the window
// that much taller, the list's rows a page
void extend(char* self, char* ctrl, int rows, int kind)
{
    char* last = nullptr;
    char* first = nullptr;
    char* second = nullptr;
    short last_number = 0;
    int buttons = 0;
    int nodes = 0;
    for (char* node = field<char*>(ctrl, kElementsAt); node != nullptr && nodes < kListsAtMost;
         node = field<char*>(node, kNodeNext), ++nodes) {
        char* element = field<char*>(node, kNodeElement);
        if (element == nullptr || field<short>(element, kTypeAt) != kButtonType)
            continue;
        const short number = field<short>(element, kNumberAt);
        if (number == 1)
            first = element;
        else if (number == 2)
            second = element;
        if (number > last_number) {
            last_number = number;
            last = element;
        }
        ++buttons;
    }
    // the game's rows only, numbered 1 to the count: not a window already extended (keep_rows sees to its list) or laid out
    // some other way
    if (last == nullptr || first == nullptr || second == nullptr || last_number != buttons || buttons != kGameRows ||
        buttons >= rows)
        return;
    int pitch = field<short>(second, kTopAt) - field<short>(first, kTopAt);
    if (pitch <= 0)
        pitch = field<short>(self, kPitchAt);
    if (pitch <= 0 || pitch > 100)
        return;
    const int added = add_rows(ctrl, last, last_number, pitch, rows);
    if (added == 0)
        return;
    const short* rect = &field<short>(ctrl, kRectAt);
    g_resize(ctrl, nullptr, rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1] + pitch * added, 1, 0, 0);
    g_set_line(self, nullptr, last_number + added);
    g_clip(self, nullptr);
    g_pitch = pitch;
    move_item_info();
    if (!g_logged[kind - 1]) {
        g_logged[kind - 1] = true;
        log::write(L"Item list: the %ls list's window given %d rows (%d units taller) ([client] %ls.rows)",
                   kind == 1 ? L"inventory" : kind == 2 ? L"storage" : kind == 3 ? L"auction house" : kind == 4 ? L"shop" : L"equipment",
                   last_number + added, pitch * added,
                   kind == 3 ? L"auction" : kind == 4 ? L"shop" : kind == kEquipKind ? L"equip" : L"items");
    }
}

// The rows the Use list's window `ctrl` can have without reaching the action menu's window under it (at most `rows`)
int rows_above_action_menu(char* ctrl, int rows)
{
    if (g_windows == nullptr)
        return rows;
    int nodes = 0;
    for (char* node = *g_windows; node != nullptr && nodes < kListsAtMost; node = field<char*>(node, kNodeNext), ++nodes) {
        char* other = field<char*>(node, kNodeElement);
        char* data = other != nullptr ? field<char*>(other, kMenuDataAt) : nullptr;
        if (data == nullptr || std::memcmp(data + kNameAt, kActionMenuName, sizeof kActionMenuName) != 0)
            continue;
        const short* rect = &field<short>(ctrl, kRectAt);
        const int margin = rect[3] - rect[1] - kGameRows * g_pitch; // the frame around the game's rows
        const int room = field<short>(other, kRectAt + 2) - kAboveActionMenu - rect[1] - margin;
        const int fit = g_pitch > 0 ? room / g_pitch : rows;
        return fit < rows ? fit : rows;
    }
    return rows;
}

// The list `self` whose window `ctrl` has rows past the game's ten already: told those rows a page, if it hasn't them.
// The game's CreateDrawMenu (0x1015d080) takes a window of the name that's still there (one closing: DestroyDrawMenu's
// wait), resets its primitive and sets it up again, with the buttons and the height it had; so a list opened while its
// last window was closing (an item picked, the list opened again; one storage after another) comes to the window VanaCore
// gave rows, and its Open gave it the game's 10 a page while it had no window to go by. With 10 a page in a 30-row window
// the list scrolled at the tenth row and the rows past it stayed empty (the stuck lists reported 2026-10-24)
void keep_rows(char* self, char* ctrl)
{
    const short buttons = field<short>(ctrl, kButtonsAt);
    if (buttons > kGameRows && field<short>(self, kPageAt) != buttons && g_set_line != nullptr && g_clip != nullptr) {
        g_set_line(self, nullptr, buttons);
        g_clip(self, nullptr);
    }
}

// Every list window, once made: the inventory's for the Items menu, the action menu, the Mog House or the auction
// house's Sell (not a trade's, the bazaar's...),
// a storage's contents in that window (viewed from outside the Mog House) and the Mog House's storage lists get their rows;
// the auction house's list and a shop's their own, as is the Equipment menu's list of what fits a slot (the inventory's
// window with the equipment's controller). A window given rows already, in use again, keeps its list to them, whatever
// the settings say now
void __fastcall list_init_hook(char* self, void* edx)
{
    char* ctrl = field<char*>(self, kWindowAt);
    const int kind = ctrl != nullptr ? list_kind(ctrl) : 0;
    void* controller = kind == 1 ? field<void*>(self, kControllerAt) : nullptr;
    const bool equip = controller != nullptr && g_item_ctrl_equip != nullptr && controller == *g_item_ctrl_equip;
    if (kind == 3 && g_auction_rows > kGameRows)
        extend(self, ctrl, g_auction_rows, kind);
    else if (kind == 4 && g_shop_rows > kGameRows)
        extend(self, ctrl, g_shop_rows, kind);
    else if (equip) {
        // The list's first row is CatsEyeXI's "Storage Options" entry (read live 2026-10-28: 20 buttons and a page of 20 held
        // 18 rows, it and 17 items), so a row more makes equip.rows the items shown
        if (g_equip_rows > kGameRows)
            extend(self, ctrl, g_equip_rows + 1, kEquipKind);
    } else if (g_rows > kGameRows && ctrl != nullptr) {
        if (kind == 2)
            extend(self, ctrl, g_rows, kind);
        else if (kind == 1) {
            if (controller != nullptr && (controller == *g_item_ctrl_list || controller == *g_item_ctrl_move ||
                                          controller == *g_item_ctrl_use ||
                                          (g_item_ctrl_sell != nullptr && controller == *g_item_ctrl_sell) ||
                                          field<int>(self, kContainerAt) != 0))
                extend(self, ctrl, controller == *g_item_ctrl_use ? rows_above_action_menu(ctrl, g_rows) : g_rows, kind);
        }
    }
    if (kind != 0)
        keep_rows(self, ctrl);
    g_list_init(self, edx);
}

// The game's 10 rows a page, given again to a list whose window has more rows (opened while it's up): kept
void __fastcall set_line_hook(char* self, void* edx, int rows)
{
    rows = static_cast<short>(rows); // pushed as a short
    // items.autosort: the Items list's or the storage list's Open (which gives the game's rows a page first, then makes
    // the list; its window may be up already, with another storage picked): the bag's store put in the sorted order, as
    // the Sort option does
    if (g_autosort && g_sort != nullptr && rows == kGameRows && g_item_window != nullptr && g_bank_window != nullptr) {
        char* store = self == *g_item_window   ? field<char*>(self, kItemStoreAt)
                      : self == *g_bank_window ? field<char*>(self, kBankStoreAt)
                                               : nullptr;
        if (store != nullptr)
            g_sort(store, nullptr, 0, kSortedKey, kSortArg, 0);
    }
    if (rows == kGameRows) {
        char* ctrl = field<char*>(self, kWindowAt);
        if (ctrl != nullptr && list_kind(ctrl) != 0 && field<short>(ctrl, kButtonsAt) > kGameRows)
            rows = field<short>(ctrl, kButtonsAt);
    }
    g_set_line(self, edx, rows);
}

// The item details window placed under a list (its place when it opens and whenever its lines change): under the
// taller list
void __fastcall reposition_hook(char* ctrl, void* edx, int x, int y)
{
    char* info = g_item_info != nullptr ? *g_item_info : nullptr;
    if (info != nullptr && ctrl == field<char*>(info, kWindowAt)) {
        g_info_ctrl = ctrl;
        g_info_x = static_cast<short>(x);
        g_info_y = static_cast<short>(y);
    }
    if (info != nullptr && ctrl == field<char*>(info, kWindowAt) &&
        (static_cast<short>(x) == kInfoX || static_cast<short>(x) == kInfoRightX) && static_cast<short>(y) == kInfoY) {
        const int extra = extra_height();
        if (extra != 0) {
            y = kInfoY + extra;
            if (field<short>(ctrl, kAreaTopAt) == kInfoY)
                field<short>(ctrl, kAreaTopAt) = static_cast<short>(y);
        }
    }
    hudwin::locate(ctrl, x, y); // a Menu Content window where the HUD Manager's box is (Status, Inventory... from Equipment)
    g_reposition(ctrl, edx, x, y);
}

// The windows of lists over the chat given rows past the game's (a few: one is up at a time), so another setting up of
// the list while its window is up (another category picked) keeps them
char* g_chat_grown[4] = {};
int g_chat_grown_next = 0;

bool chat_grown(char* ctrl)
{
    for (char* grown : g_chat_grown)
        if (grown == ctrl)
            return true;
    return false;
}

bool is_currency(char* ctrl)
{
    const char* data = field<char*>(ctrl, kMenuDataAt);
    return data != nullptr && std::memcmp(data + kNameAt, kCurrencyName, sizeof kCurrencyName) == 0;
}

// The rows, `pitch` apart, that fit under a window's bottom `bottom` above the interface's bottom (its margin kept)
int rows_below(int bottom, int pitch)
{
    const int ui_bottom = chat::ui_bottom();
    if (ui_bottom <= 0)
        return kCurrencyMaxRows; // (not known: as many as asked)
    const int room = (ui_bottom - kChatTopMost - bottom) / pitch;
    return room > 0 ? room : 0;
}

// The rows a page for the list over the chat `self`, which the game gives `rows`: its window, if it has the game's rows
// as one column of buttons, given lists.rows (as many as fit under the layout's highest top) and grown upward by as much,
// as the layout keeps its bottom on the chat's top; the Currencies list's given currencies.rows and grown downward. The
// rows of a window given them already
int grow_chat_list(char* self, int rows)
{
    char* ctrl = field<char*>(self, kWindowAt);
    if (ctrl == nullptr || rows < 2)
        return rows;
    const bool over_chat = (field<std::uint32_t>(ctrl, kFlagsAt) & kOverAnchor) != 0;
    if (!over_chat && !is_currency(ctrl))
        return rows;
    const int buttons = field<short>(ctrl, kButtonsAt);
    if (buttons > rows)
        return chat_grown(ctrl) ? buttons : rows;
    const int setting = over_chat ? g_chat_rows : g_currency_rows;
    if (setting <= rows || buttons != rows)
        return rows;
    char* first = nullptr;
    char* second = nullptr;
    char* last = nullptr;
    short last_number = 0;
    int count = 0, nodes = 0;
    for (char* node = field<char*>(ctrl, kElementsAt); node != nullptr && nodes < kListsAtMost;
         node = field<char*>(node, kNodeNext), ++nodes) {
        char* element = field<char*>(node, kNodeElement);
        if (element == nullptr || field<short>(element, kTypeAt) != kButtonType)
            continue;
        const short number = field<short>(element, kNumberAt);
        if (number == 1)
            first = element;
        else if (number == 2)
            second = element;
        if (number > last_number) {
            last_number = number;
            last = element;
        }
        ++count;
    }
    if (first == nullptr || second == nullptr || last == nullptr || last_number != count || count != rows)
        return rows;
    const int pitch = field<short>(second, kTopAt) - field<short>(first, kTopAt);
    // one column, a row each pitch (not a grid, as some menus' are)
    if (pitch <= 0 || pitch > 100 || field<short>(last, kTopAt) != field<short>(first, kTopAt) + pitch * (rows - 1) ||
        field<short>(last, kLeftAt) != field<short>(first, kLeftAt))
        return rows;
    const short* rect = &field<short>(ctrl, kRectAt);
    int want = setting;
    // rows that fit above it (over the chat) or under it (the Currencies list)
    const int room = over_chat ? (rect[1] - kChatTopMost) / pitch : rows_below(rect[3], pitch);
    if (want > rows + room)
        want = rows + room;
    const int added = want > rows ? add_rows(ctrl, last, last_number, pitch, want) : 0;
    if (added == 0)
        return rows;
    const int up = over_chat ? pitch * added : 0;
    g_resize(ctrl, nullptr, rect[0], rect[1] - up, rect[2] - rect[0], rect[3] - rect[1] + pitch * added, 1, 0, 0);
    g_chat_grown[g_chat_grown_next] = ctrl;
    g_chat_grown_next = (g_chat_grown_next + 1) % static_cast<int>(sizeof g_chat_grown / sizeof g_chat_grown[0]);
    if (!g_chat_logged[over_chat ? 0 : 1]) {
        g_chat_logged[over_chat ? 0 : 1] = true;
        const char* data = field<char*>(ctrl, kMenuDataAt);
        wchar_t name[17] = {};
        for (int i = 0; i < 16 && data != nullptr; ++i)
            name[i] = static_cast<unsigned char>(data[kNameAt + i]);
        log::write(L"Item list: the list \"%ls\" given %d rows (%d units taller, %ls; top %d) ([client] %ls.rows)", name,
                   rows + added, pitch * added, over_chat ? L"upward" : L"downward", rect[1],
                   over_chat ? L"lists" : L"currencies");
    }
    return rows + added;
}

// A list over the chat set up (opened, or another category picked): its rows a page those of its taller window
void __fastcall chat_setup_hook(char* self, void* edx, void* draw, int count, int room, int zero, int rows)
{
    g_chat_setup(self, edx, draw, count, room, zero, grow_chat_list(self, rows));
}

// A list over the chat's window made, after the list was set up: the same, and the list's top rows and scroll bar
// put right for its rows a page
void __fastcall chat_made_hook(char* self, void* edx)
{
    g_chat_made(self, edx);
    const int rows = field<int>(self, kChatPageAt);
    const int grown = grow_chat_list(self, rows);
    if (grown == rows)
        return;
    field<int>(self, kChatPageAt) = grown;
    const int count = field<int>(self, kChatCountAt);
    const int last_top = count > grown ? count - grown : 0;
    field<int>(self, kChatLastTopAt) = last_top;
    if (field<int>(self, kChatTopAt) > last_top)
        field<int>(self, kChatTopAt) = last_top;
    char* ctrl = field<char*>(self, kWindowAt);
    if (char* bar = field<char*>(ctrl, kBarAt)) {
        field<int>(bar, kBarShownAt) = grown;
        field<int>(bar, kBarCountAt) = count;
        field<int>(bar, kBarTopAt) = field<int>(self, kChatTopAt);
    }
}

// 1234567 as "1,234,567", as the shop's list shows prices
std::string with_commas(unsigned long long value)
{
    std::string digits = std::to_string(value);
    for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3)
        digits.insert(static_cast<std::size_t>(at), 1, ',');
    return digits;
}

// The item the Buy controller has picked: its name (`name`, `size` bytes) and price, false if there's none
bool shop_item(char* name, std::size_t size, std::uint32_t& price)
{
    __try { // the shop's data as the game left it: read, never trusted
        char* ctrl = *g_shop_buy_ctrl;
        const char* entry = nullptr;
        if (ctrl == nullptr || !g_shop_entry(&entry, field<std::uint8_t>(ctrl, kShopSelectedAt)) || entry == nullptr)
            return false;
        price = *reinterpret_cast<const std::uint32_t*>(entry);
        const auto id = *reinterpret_cast<const std::uint16_t*>(entry + kEntryIdAt);
        char* item = *g_item_data_store != nullptr ? g_item_data(*g_item_data_store, nullptr, id) : nullptr;
        const char* shown = item != nullptr ? field<const char*>(item, kItemNameAt + sigs::item_data_shift()) : nullptr;
        if (shown != nullptr)
            strncpy_s(name, size, shown, _TRUNCATE);
        else
            sprintf_s(name, size, "item %u", static_cast<unsigned>(id));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A shop's quantity picked (the Yes/No about to ask whether to buy that many): the total said in the chat, as the game
// doesn't show it, with the gil there is: "Purchase: 12 Toolbag (Ino) for 360,000 Gil? (6,114,942 G)", what's bought
// in green, the total in yellow
void __cdecl shop_buy_hook(int quantity, void* user)
{
    char name[64] = {};
    std::uint32_t price = 0;
    if (g_shop_total && quantity > 0 && shop_item(name, sizeof name, price)) {
        std::string line = "Purchase: " + chat::colour(chat::kSuccess, std::to_string(quantity) + " " + name) + " for " +
                           chat::colour(chat::kWarning, with_commas(static_cast<unsigned long long>(price) *
                                                                    static_cast<unsigned>(quantity)) + " Gil") + "?";
        ashita::ItemSlot gil{};
        if (ashita::container_item(0, 0, gil)) // the inventory's first slot is the gil
            line += " (" + with_commas(gil.count) + " G)";
        chat::write(chat::kDefaultMode, line, "Shop");
    }
    g_shop_buy(quantity, user);
}

void install_hook()
{
    g_tried = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return;
    const std::uintptr_t init = scan::find(game, kListInit);
    const std::uintptr_t set_line = scan::find(game, kSetLine);
    const std::uintptr_t clip = scan::find(game, kClipCursor);
    const std::uintptr_t make = scan::find(game, kMakeButton);
    const std::uintptr_t resize = scan::find(game, kResize);
    const std::uintptr_t reposition = scan::find(game, kReposition);
    const std::uintptr_t open = scan::find(game, kOpen);
    const std::uintptr_t info = scan::find(game, kItemInfo);
    const std::uintptr_t house = scan::find(game, kViewHouse);
    const std::uintptr_t use = scan::find(game, kItemsUse);
    const std::uintptr_t windows = scan::find(game, kWindows);
    if (init == 0 || set_line == 0 || clip == 0 || make == 0 || resize == 0 || reposition == 0 || open == 0 || info == 0 ||
        house == 0 || windows == 0 || use == 0 || !is_call(use + kItemsUseOpenCall) ||
        call_target(use + kItemsUseOpenCall) != open || !is_call(make + kNewCall) || !is_call(make + kCtorCall) ||
        !is_call(make + kSetResourceCall) || !is_call(make + kLocateCall) || !is_call(make + kShapeCall) ||
        !is_call(make + kAddTailCall)) {
        log::write(L"Item list: the game's list windows weren't found in FFXiMain.dll (%d%d%d%d%d%d%d%d%d%d): [client] items.rows does nothing",
                   init != 0, set_line != 0, clip != 0, make != 0, resize != 0, reposition != 0, open != 0, info != 0,
                   house != 0, windows != 0);
        return;
    }
    g_clip = reinterpret_cast<ClipCursor>(clip);
    g_resize = reinterpret_cast<Resize>(resize);
    g_new = reinterpret_cast<New>(call_target(make + kNewCall));
    g_ctor = reinterpret_cast<Ctor>(call_target(make + kCtorCall));
    g_set_resource = reinterpret_cast<SetResource>(call_target(make + kSetResourceCall));
    g_locate = reinterpret_cast<Locate>(call_target(make + kLocateCall));
    g_shape = reinterpret_cast<ShapeCalc>(call_target(make + kShapeCall));
    g_add_tail = reinterpret_cast<AddTail>(call_target(make + kAddTailCall));
    g_item_ctrl_list = operand<void* const*>(open + kItemCtrlListAt);
    g_item_ctrl_move = operand<void* const*>(house + kItemCtrlMoveAt);
    g_item_ctrl_use = operand<void* const*>(use + kItemCtrlUseAt);
    g_item_info = operand<char* const*>(info + kItemInfoAt);
    g_windows = operand<char* const*>(windows + kWindowsAt);
    const std::uintptr_t sell = scan::find(game, kAuctionSell);
    if (sell != 0 && is_call(sell + kAuctionSellOpenCall) && call_target(sell + kAuctionSellOpenCall) == open)
        g_item_ctrl_sell = operand<void* const*>(sell + kItemCtrlSellAt);
    else
        log::write(L"Item list: the auction house's Sell wasn't found in FFXiMain.dll: its list keeps the game's 10 rows");
    const std::uintptr_t equip = scan::find(game, kEquipList);
    if (equip != 0 && is_call(equip + kEquipOpenCall) && call_target(equip + kEquipOpenCall) == open)
        g_item_ctrl_equip = operand<void* const*>(equip + kItemCtrlEquipAt);
    else
        log::write(L"Item list: the Equipment menu's list wasn't found in FFXiMain.dll: it keeps the game's 10 rows");
    const std::uintptr_t colours = scan::find(game, kBankColours);
    if (colours != 0 && *reinterpret_cast<const std::uint8_t*>(colours + kBankColoursAt) == 0x14) {
        g_bank_colours = colours;
        apply_bank_colours();
    } else
        log::write(L"Item list: the storage list's colour window wasn't found in FFXiMain.dll: its rows past the 20th draw grey");
    const std::uintptr_t sort = scan::find(game, kSort);
    const std::uintptr_t items = scan::find(game, kItemWindow);
    if (sort != 0 && items != 0 && is_call(items + kItemWindowOpenCall) && call_target(items + kItemWindowOpenCall) == open) {
        g_sort = reinterpret_cast<Sort>(sort);
        g_item_window = operand<char* const*>(items + kItemWindowAt);
        g_bank_window = operand<char* const*>(house + kBankWindowAt);
        const std::uintptr_t compare = scan::find(sort, kSortLength, kSortCompare);
        if (compare != 0) {
            g_sort_compare = compare;
            g_game_compare = operand<Compare>(compare + kSortCompareAt);
            apply_compare();
        } else
            log::write(L"Item list: the game's sorting of a bag's comparison wasn't found: [client] items.sort does nothing");
    } else
        log::write(L"Item list: the game's sorting of a bag wasn't found in FFXiMain.dll (%d%d): [client] items.autosort does nothing",
                   sort != 0, items != 0);
    const std::uintptr_t buy = scan::find(game, kShopBuy);
    const std::uintptr_t shop_item_at = scan::find(game, kShopItem);
    if (buy != 0 && shop_item_at != 0 && is_call(shop_item_at + kShopListCall) && is_call(shop_item_at + kItemDataCall)) {
        g_shop_buy_ctrl = operand<char* const*>(buy + kShopBuyCtrlAt);
        g_shop_entry = reinterpret_cast<ShopEntry>(call_target(shop_item_at + kShopListCall));
        g_item_data = reinterpret_cast<ItemData>(call_target(shop_item_at + kItemDataCall));
        g_item_data_store = operand<void* const*>(shop_item_at + kItemDataAt);
        if (!imports::detour(reinterpret_cast<ShopBuy>(buy), kShopBuyPrologue, &shop_buy_hook, g_shop_buy))
            log::write(L"Item list: a shop's buying couldn't be hooked: [client] shop.total does nothing");
    } else
        log::write(L"Item list: a shop's buying wasn't found in FFXiMain.dll (%d%d): [client] shop.total does nothing",
                   buy != 0, shop_item_at != 0);
    if (!imports::detour(reinterpret_cast<ListInit>(init), kListInitPrologue, &list_init_hook, g_list_init) ||
        !imports::detour(reinterpret_cast<SetLine>(set_line), kSetLinePrologue, &set_line_hook, g_set_line) ||
        !imports::detour(reinterpret_cast<Reposition>(reposition), kRepositionPrologue, &reposition_hook, g_reposition)) {
        log::write(L"Item list: the game's list windows couldn't be hooked: [client] items.rows does nothing");
        return;
    }
    const std::uintptr_t chat_setup = scan::find(game, kChatListSetup);
    const std::uintptr_t chat_made = scan::find(game, kChatListMade);
    if (chat_setup == 0 || chat_made == 0)
        log::write(L"Item list: the lists over the chat weren't found in FFXiMain.dll (%d%d): [client] lists.rows and currencies.rows do nothing",
                   chat_setup != 0, chat_made != 0);
    else if (!imports::detour(reinterpret_cast<ChatListSetup>(chat_setup), kChatListSetupPrologue, &chat_setup_hook, g_chat_setup) ||
             !imports::detour(reinterpret_cast<ChatListMade>(chat_made), kChatListMadePrologue, &chat_made_hook, g_chat_made))
        log::write(L"Item list: the lists over the chat couldn't be hooked: [client] lists.rows and currencies.rows do nothing");
    log::write(L"Item list: the game's list windows hooked at +0x%X (rows), +0x%X (rows a page), +0x%X (placing): items.rows=%d auction.rows=%d shop.rows=%d equip.rows=%d lists.rows=%d currencies.rows=%d",
               static_cast<unsigned>(init - reinterpret_cast<std::uintptr_t>(game)),
               static_cast<unsigned>(set_line - reinterpret_cast<std::uintptr_t>(game)),
               static_cast<unsigned>(reposition - reinterpret_cast<std::uintptr_t>(game)), g_rows, g_auction_rows, g_shop_rows,
               g_equip_rows, g_chat_rows, g_currency_rows);
}

} // namespace

void reload(const wchar_t* ini)
{
    const int rows = static_cast<int>(GetPrivateProfileIntW(L"client", L"items.rows", kGameRows, ini));
    g_rows = rows < kGameRows ? kGameRows : rows > kMaxRows ? kMaxRows : rows;
    const int auction = static_cast<int>(GetPrivateProfileIntW(L"client", L"auction.rows", kGameRows, ini));
    g_auction_rows = auction < kGameRows ? kGameRows : auction > kMaxRows ? kMaxRows : auction;
    const int shop = static_cast<int>(GetPrivateProfileIntW(L"client", L"shop.rows", kGameRows, ini));
    g_shop_rows = shop < kGameRows ? kGameRows : shop > kMaxRows ? kMaxRows : shop;
    const int equip = static_cast<int>(GetPrivateProfileIntW(L"client", L"equip.rows", kGameRows, ini));
    g_equip_rows = equip < kGameRows ? kGameRows : equip > kMaxRows ? kMaxRows : equip;
    const int lists = static_cast<int>(GetPrivateProfileIntW(L"client", L"lists.rows", kChatGameRows, ini));
    g_chat_rows = lists < kChatGameRows ? kChatGameRows : lists > kChatMaxRows ? kChatMaxRows : lists;
    const int currencies = static_cast<int>(GetPrivateProfileIntW(L"client", L"currencies.rows", kCurrencyGameRows, ini));
    g_currency_rows = currencies < kCurrencyGameRows ? kCurrencyGameRows : currencies > kCurrencyMaxRows ? kCurrencyMaxRows : currencies;
    g_autosort = GetPrivateProfileIntW(L"client", L"items.autosort", 1, ini) != 0;
    g_shop_total = GetPrivateProfileIntW(L"client", L"shop.total", 1, ini) != 0;
    wchar_t order[16] = {};
    GetPrivateProfileStringW(L"client", L"items.sort", L"game", order, 16, ini);
    g_az = _wcsicmp(order, L"az") == 0;
    apply_bank_colours();
    apply_compare();
}

// The menu resource's jump tags (where each key takes the cursor from each button) stop at the game's ten rows, so from
// the tenth Down goes to the eleventh and on, Up back; the last row's Down, Left and Right are as the game's tenth row's
bool jump_tag(void* data, int cursor, int key, JumpTag game, short& result)
{
    if (most_rows() <= kGameRows || game == nullptr)
        return false;
    cursor = static_cast<short>(cursor); // the game pushes shorts: the words above are whatever was in the register
    key = static_cast<short>(key);
    char* ctrl = extended_window(data);
    if (ctrl == nullptr)
        return false;
    const int rows = field<short>(ctrl, kButtonsAt);
    if (cursor < 1 || cursor > rows)
        return false;
    // which key is which, from the game's own tags for its second row: Down goes to 3, Up to 1
    const short probe = game(data, nullptr, 2, key);
    const bool down = probe == 3, up = probe == 1;
    if (cursor <= kGameRows) {
        if (cursor != kGameRows || !down)
            return false; // the game's own tag
        result = static_cast<short>(kGameRows + 1);
        return true;
    }
    if (up) {
        result = static_cast<short>(cursor - 1);
        return true;
    }
    if (down && cursor < rows) {
        result = static_cast<short>(cursor + 1);
        return true;
    }
    const short tenth = game(data, nullptr, kGameRows, key); // itself: the list scrolls; or none
    result = tenth == kGameRows ? static_cast<short>(cursor) : tenth;
    return true;
}

void install(const wchar_t* ini)
{
    reload(ini);
}

void on_frame()
{
    if ((most_rows() > kGameRows || g_chat_rows > kChatGameRows || g_currency_rows > kCurrencyGameRows || g_autosort || g_az ||
         g_shop_total) &&
        !g_tried)
        install_hook();
    if (g_az && !g_names_warmed) {
        g_names_warmed = true;
        if (HANDLE thread = CreateThread(nullptr, 0, &warm_names, nullptr, 0, nullptr))
            CloseHandle(thread);
    }
    if (g_reposition != nullptr)
        move_item_info();
}

char* open_list()
{
    if (g_windows == nullptr)
        return nullptr;
    char* found = nullptr;
    int nodes = 0;
    __try { // as extra_height: a window torn down while its node is still linked
        for (char* node = *g_windows; node != nullptr && found == nullptr && nodes < kListsAtMost;
             node = field<char*>(node, kNodeNext), ++nodes) {
            char* ctrl = field<char*>(node, kNodeElement);
            if (ctrl != nullptr && list_kind(ctrl) != 0)
                found = ctrl;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return found;
}

} // namespace vanacore::itemlist
