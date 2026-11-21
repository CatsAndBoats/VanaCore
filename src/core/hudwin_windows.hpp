/* The windows hudwin moves, hides and sizes ([client] <id>.*, the HUD Manager): g_windows, the table of Window (its id, name,
 * what it can, its layouts, its anchor and the game's own place). hudwin.cpp includes this inside its namespace just after
 * Window and its flags (kMove, ...), whose names it uses. It is no file of its own. */
#pragma once

// The party list sits at the bottom right, growing up as members join; the alliance lists stand on it, 1 then 2. (The
// game's places on a 2880 x 1620 interface, seen live 2026-10-20: the party list's right and bottom at 2864, 1604.) The
// action menu (Attack, Magic, ...) is at the left, X and Y its top left corner: the game stands it on the chat, at its
// own left, as the menus over the chat (its menu table entry's flags have 0x1000, as "magic" and "ability" do, read
// from the dump 2026-10-23), and so does Monstrosity's ("mp_pmode", flags 0x4011C02 to its 0x8011C02). The status icons
// and the macro bar are moved by buffwin.cpp and macrobar.cpp; here they're only hidden (their keys still work, the
// palettes still choose).
Window g_windows[] = {
    {"party", L"Party list", kMove, {"menu    partywin"}, 1, 1, 99.4, 99.0, 112, 34},
    {"alliance1", L"Alliance list 1", kMove, {"menu    raid1   "}, 1, 1, 99.4, 80.0, 112, 120},
    {"alliance2", L"Alliance list 2", kMove, {"menu    raid2   "}, 1, 1, 99.4, 60.0, 112, 120},
    {"action", L"Action menu", kMove | kHide | kFrame | kChat, {"menu    playermo", "menu    mp_pmode"}, 0, 0, 1.7, 60.0, 112, 88},
    // the list the Action menu's Magic opens when it's pressed to the right (Black Magic, Songs, Ninjutsu, ...; "menu
    // magselec", area 8, flags 0x20001A00 with 0x1000 as the Action menu's, seen live 2026-10-28): the game stands it over the
    // chat with the other windows there, so it came up in the Interaction Menu's box. In the table it has a box of its own,
    // X and Y its top left (the game's place on a 1920 x 1200 interface: 15, 847); off, it's the Interaction Menu's as before.
    // Its <id>.hide and .background are its own: the Interaction Menu's (menus.hide, menus.background) no longer reach it
    {"actionmagic", L"Action menu: Magic sub-menu", kMove | kHide | kFrame, {"menu    magselec"}, 0, 0, 0.8, 70.5, 112, 120},
    // the list the Action menu's Abilities opens (Job Abil., Weapon Skl., Ranged Atk., Mount, Pet Command: "menu
    // abiselec", area 4, flags 0x20201200 with 0x1000 as the Action menu's, 15, 702 - 127, 824 of 1921 x 1163 with
    // menus.move's bottom at 70.8%, seen live 2026-11-06; Monstrosity's "mp_abisc", closed with it at 0x10223693): as the
    // Magic sub-menu, a box of its own, X and Y its top left; off, it's the Interaction Menu's as before
    {"actionability", L"Action menu: Abilities sub-menu", kMove | kHide | kFrame, {"menu    abiselec", "menu    mp_abisc"}, 0, 0, 0.8, 70.5, 112, 122},
    // the lists the Action menu's Magic and Abilities open once a kind is chosen (the spells; the abilities: "menu    magic   "
    // and "menu    ability ", the same slot, 15, 662 - 197, 1150 of 1920 x 1200 at the bottom left over the chat; "magic" seen
    // live 2026-10-29: area 0xC, flags 0x20001002 with 0x1000, and "ability" has the 0x1000 too, read from the dump of
    // 2026-10-23): the game stands them over the chat with the other windows there, so they came up in the Interaction
    // Menu's box. In the table they have a box of their own, X their left edge, Y their bottom (the rows make the list
    // taller or shorter); off, they're the Interaction Menu's as before. Their <id>.hide and .background are their own:
    // the Interaction Menu's (menus.hide, menus.background) no longer reach them. Their Options box (sortmenu) and the
    // cost box (spellinfo) keep theirs
    {"actionlist", L"Action menu: Magic/Abilities", kMove | kHide | kFrame, {"menu    magic   ", "menu    ability "}, 0, 1, 0.8, 95.8, 182, 488},
    // the Action menu's Mounts list (Raptor, Tiger, Coeurl, Gyokko...: "menu    mount   ", in the Magic and Abilities lists'
    // slot, 15, 662 - 197, 1150 of 1920 x 1200 at the bottom left over the chat, area 0xC, flags 0x20001002 with 0x1000, seen
    // live 2026-10-29): a box of its own, as theirs, X its left edge, Y its bottom; off, it's the Interaction Menu's as before.
    // Its <id>.hide and .background are its own
    {"actionmount", L"Action menu: Mounts", kMove | kHide | kFrame, {"menu    mount   "}, 0, 1, 0.8, 95.8, 182, 488},
    // the Change Jobs list the Mog House's moogle opens (Warrior ... Rune Fencer and their levels: "menu    jobcselu", laid
    // out at 16, 48 - 284, 234, area 0x10, flags 0x21000 with 0x1000, seen live 2026-11-06): the game stands it over the
    // chat with the other windows there (22, 855 - 290, 1121 of 2881 x 1583), its bottom on the Main Job / Support Job
    // box's ("menu    jobchang", which stays the Interaction Menu's), so it came up in the Interaction Menu's box. In the
    // table it has a box of its own, X and Y its top left; off, it's the Interaction Menu's as before. Its <id>.hide and
    // .background are its own
    {"jobchange", L"Change Jobs list", kMove | kHide | kFrame, {"menu    jobcselu"}, 0, 0, 0.8, 54.0, 268, 266},
    // the auction house's menus (Buy/Sell/History, the categories and their lists, the sort menus, a bid's Yes/No; all
    // in one slot, one up at a time, area 0x81), X their right edge, Y their top: the game puts them at the top right
    // (1792, 48 of 1920 x 1200, seen live 2026-10-22). In the table, Menu Content leaves them alone (it had taken one
    // along to its box). The item list ("auclist", "auchisto") isn't one of them: it stays Menu Content's
    {"auction", L"Auction menu", kMove | kHide | kFrame,
     {"menu    auc1    ", "menu    auc2    ", "menu    auc3    ", "menu    auc4    ", "menu    aucweapo", "menu    aucarmor",
      "menu    aucmagic", "menu    aucammo ", "menu    aucmater", "menu    aucfood ", "menu    aucmeals", "menu    aucitem ",
      "menu    aucyn   ", "menu    wepsort ", "menu    armsort "},
     1, 0, 99.2, 4.0, 112, 154},
    // the main menu (Status, Equipment, ... Map) at the top right, X its right edge, Y its top (2864, 48 of 2880 x 1620);
    // the menus it opens take its place (follow_menu)
    {"mainmenu", L"Main menu", kMove | kHide | kFrame, {"menu    menuwind"}, 1, 0, 99.4, 3.0, 112, 202},
    // the list the main menu's Status opens in its place (Combat Skl., Magic Skl., Craft Skl., Currencies, ...; "menu
    // statcom2", area 0x81, seen live 2026-10-21), X its right edge, Y its top; off, it goes with the main menu
    // (kFollow: every frame, where follow_menu moves the other menus once, so a main menu moved while it's open takes it).
    // With it the lists it opens in its own place: Unity's ("ut_menu", from its handler at 0x10148771) and Merit Points'
    // (Categories, Mode Switch: "merit1", its page table at 0x1036F6C0, and its "merit2", "merit3"). Out of the table,
    // Menu Content took them as far again as the main menu, over the Status window (seen 2026-10-22). Its frame is its
    // own (kFrame: statusmenu.background, .solid): mainmenu.background no longer reaches it (asked for 2026-11-08)
    {"statusmenu", L"Status sub-menu", kMove | kHide | kFollow | kFrame,
     {"menu    statcom2", "menu    ut_menu ", "menu    merit1  ", "menu    merit2  ", "menu    merit3  "},
     1, 0, 99.4, 3.0, 112, 202},
    // the list the main menu's Missions opens in its place (Nations, Zilart, Promathia, ... Add-ons; "menu    missionm",
    // area 0x81, made at 0x1014D5C1, seen live 2026-10-22), as the Status sub-menu. In the table, Menu Content leaves it
    // alone: the game opens it at the main menu's place, which mainmenu.move can put in the top left 512 x 448, and Menu
    // Content took it 119, 91 further. The mission lists it opens ("miss00", "missiton") stay Menu Content's
    {"missions", L"Mission menu", kMove | kHide | kFollow, {"menu    missionm"}, 1, 0, 99.4, 3.0, 112, 122},
    // the other lists the main menu opens in its place (CreateDrawMenu from its handler, 0x1013FFD3 on), as the Mission
    // menu: Magic's kinds (White Magic, ... Trust), Abilities' (Job Traits, ... Pet Command; Monstrosity's), Synthesis'
    // (Crystal Synthesis, History, and History's list), Party's (Seek Party, Find Member, ...; "prty1", and the lists it
    // opens in its place: Languages', Alarm Type's, a party's or an alliance's leader's). Out of the table, Menu Content
    // took them as far again as the main menu (Party at 131, 230 for a main menu at 12, 139, seen live 2026-10-22). The
    // invite's Yes/No ("prtyjoin") comes up without the main menu and is left as the game puts it
    {"magicmenu", L"Magic menu", kMove | kHide | kFollow, {"menu    mgcmenu "}, 1, 0, 99.4, 3.0, 112, 138},
    {"abilitymenu", L"Abilities menu", kMove | kHide | kFollow, {"menu    abimenu ", "menu    mp_mnabi"}, 1, 0, 99.4, 3.0, 112, 106},
    {"synthmenu", L"Synthesis menu", kMove | kHide | kFollow, {"menu    cmbmenu ", "menu    cmbhwin "}, 1, 0, 99.4, 3.0, 112, 42},
    {"partymenu", L"Party menu", kMove | kHide | kFollow,
     {"menu    prty1   ", "menu    prty2   ", "menu    prty3   ", "menu    prty4   ", "menu    prty5   ", "menu    prty5us ",
      "menu    prty5fr ", "menu    prty5de ", "menu    prty6   ", "menu    prty7   ", "menu    prty8   "},
     1, 0, 99.4, 3.0, 112, 122},
    // the Options box a list puts in the main menu's place beside it (+ :Sort, Auto, Manual: the Magic, Blue Magic,
    // Abilities, Items and Emotes lists' sort windows, "mgcsortw", ...; 1792, 48 of 1920 x 1200, seen 2026-10-22), as the
    // lists above: off, it goes with the main menu
    {"sortmenu", L"Options menu", kMove | kHide | kFollow,
     {"menu    mgcsortw", "menu    blusortw", "menu    abisortw", "menu    itmsortw", "menu    itmsort2", "menu    emosortw"},
     1, 0, 99.4, 3.0, 112, 58},
    // the Yes/No an Options box's Auto asks before it sorts ("menu    sortyn  ", the title "Commands"; one Yes/No class's,
    // made at 0x10220E5E and 0x10220FCC, for every list's Options box), X its right edge, Y its top: the game lays it out
    // at 384, 124 anchored to the right, 1792, 124 of 1920 x 1200 (seen live 2026-11-03), under the Options box's place
    // whether or not that was moved. Off, it stays there. Its top isn't the main menu's slot's (in_menu_slot), so
    // mainmenu.background never reached it: sortconfirm.background and .solid are its own (asked for 2026-11-06)
    {"sortconfirm", L"Options confirm", kMove | kHide | kFrame, {"menu    sortyn  "}, 1, 0, 99.2, 10.3, 112, 42},
    // the rest of the lists the main menu opens in its place, as Magic's (pushed to CreateDrawMenu from its two pages'
    // handlers: 0x1013FFD3 on, and the second page's around 0x1014D5C1). Out of the table, Menu Content took them as far
    // again as the main menu, over its own box (seen 2026-10-22): Search's (Area, Name, ... Edit Comment: "searchma", and
    // the lists it opens in its place, a job's, a level's, a race's, a comment's...), Region Info's (Conquest, Besieged,
    // Campaign, Colonization: "region"), View House's (Mog Safe, ... Mog Wardrobe 8: "mogcont"), Bazaar's (Set Prices,
    // Edit Comment: "bazaar") and Macros' (Edit Macro, Edit Equip Set: "mcrmenu", and the books' and equip sets' lists it
    // opens in its place, "mcrselec", "mcres20", "mcrmogc")
    {"searchmenu", L"Search menu", kMove | kHide | kFollow,
     {"menu    searchma", "menu    searchjo", "menu    searchle", "menu    searchna", "menu    searchra", "menu    scindex ",
      "menu    scsibori", "menu    scchar  ", "menu    scname  ", "menu    scmlev  ", "menu    scoption", "menu    sccomite",
      "menu    sccomls ", "menu    sccompar"},
     1, 0, 99.4, 3.0, 112, 282},
    {"regionmenu", L"Region Info menu", kMove | kHide | kFollow, {"menu    region  "}, 1, 0, 99.4, 3.0, 112, 74},
    // the map's controls (Markers, Wide Scan, Change Map: "menu    map0    ", area 0x81, in the main menu's place, 2751, 49
    // of 2880 x 1620, seen live 2026-10-23) and the lists laid out in the same place (Wide Scan's "mapscan", "mapv1",
    // "mapv3"), as the Region Info menu
    {"mapcontrols", L"Map controls", kMove | kHide | kFollow,
     {"menu    map0    ", "menu    mapscan ", "menu    mapv1   ", "menu    mapv3   "}, 1, 0, 99.4, 3.0, 112, 58},
    {"housemenu", L"View House menu", kMove | kHide | kFollow, {"menu    mogcont "}, 1, 0, 99.4, 3.0, 112, 250},
    {"bazaarmenu", L"Bazaar menu", kMove | kHide | kFollow, {"menu    bazaar  "}, 1, 0, 99.4, 3.0, 112, 42},
    {"macromenu", L"Macro menu", kMove | kHide | kFollow,
     {"menu    mcrmenu ", "menu    mcrselec", "menu    mcres20 ", "menu    mcrmogc "}, 1, 0, 99.4, 3.0, 112, 330},
    // the options box an equip set's row opens beside the list (Name, Equipment, ... Back: "menu    mcresmn ", made at
    // 0x101A564E) and a macro book's ("menu    mcrselop", 0x101A5064): laid out at 0, 0 and put at the chosen row once as
    // it opens, its right edge 17 into the list (1809, 55 of 1920 x 1200 beside the list at the top right). X its right
    // edge, Y its top. Off, it's where the game puts it, on the list's other side when that's off the screen (beside:
    // with the Macro menu at the left edge it came up at -80, reported 2026-11-06)
    {"macrooptions", L"Macro options", kMove | kHide | kFrame, {"menu    mcresmn ", "menu    mcrselop"}, 1, 0, 94.2, 4.6, 112, 138},
    // the Config list the main menu's Config opens in its place (Gameplay, Chat Filters, ... and configmenu.cpp's VanaCore
    // items: "menu    configwi"), as the lists above. Out of the table, mainmenu.background=0 left its frame on (reported
    // 2026-11-05, flat buttons on a frameless main menu)
    {"configmenu", L"Config menu", kMove | kHide | kFollow, {"menu    configwi"}, 1, 0, 99.4, 3.0, 112, 298},
    // the bags list Equipment opens beside it to pick gear from (Inventory, Mog Wardrobe ... 8: "mnstorag", made at
    // 0x1013489B) and Items' list of bags ("itmstora"): at the top right, X their right edge, Y their top
    {"storagemenu", L"Storage list", kMove | kHide | kFrame, {"menu    mnstorag", "menu    itmstora"}, 1, 0, 99.4, 3.0, 112, 154},
    // the chat log maximized ("menu    fulllog ": the chat's own windows are closed while it's up, 0x101605A2), X and Y
    // its top left, <id>.height its height. Laid out at the top left (16, 48), Menu Content took it lower, and the game
    // makes it reach the chat's bottom, so it went off the screen (seen 2026-10-22); in the table it stays the game's
    {"fulllog", L"Full chat log", kMove | kHide | kHeight, {"menu    fulllog "}, 0, 0, 0.8, 4.0, 366, 384},
    // the bar across the top while the main menu is up (helpbar.cpp): the box with the page's name at the left and the
    // help line beside it, X and Y their top left; the Network window at the right, X its right edge (16, 130 and 2864,
    // 16 of 2880 x 1620). helptitle.hide=1: the title box left out (helpbar.hide takes just the help line, helpbar.cpp;
    // helpbar.title=0, as it was hidden before, is helpbar.cpp's too). helptitle.background=0: the title box without its
    // frame, the page's name (its owner's draw, PS2 CTkTitle::OnDrawPrimitive) staying
    {"helptitle", L"Help bar title", kMove | kHide | kFrame, {"menu    titlewin"}, 0, 0, 0.6, 1.0, 112, 30},
    {"helpbar", L"Help bar", kMove | kWidth, {"menu    helpwind"}, 0, 0, 4.5, 1.0, 2620, 30},
    {"netwin", L"Network window", kMove, {"menu    netstat "}, 1, 0, 99.4, 1.0, 112, 30},
    // the box at the top right beside the Magic and Abilities lists (a spell's MP, its or an ability's recast, "Next";
    // "menu    subwindo", area 0x80), X its right edge, Y its top (1792, 16 - 1904, 58 of 1920 x 1200, seen 2026-10-23,
    // over the Network window)
    {"spellinfo", L"Spell info", kMove | kHide | kFrame, {"menu    subwindo"}, 1, 0, 99.2, 1.3, 112, 42},
    // the conquest map's boxes (Region Info > Conquest, YkWndConquestMain's; seen live 2026-10-23 on 2880 x 1620): Balance
    // of Power at the top right ("menu    conq6sta", while no zone is under the cursor), X its right edge, Y its top
    // (2751, 49 - 2863, 121); Conquest Points over the party list ("menu    conq3pts", 2752, 1524 - 2864, 1564), X its
    // right edge, Y its bottom; and Regional Control, a zone's under the cursor ("menu    conquer1", in Balance's place
    // while it's up), which the game lays out at the top left (16, 48) with Menu Content's windows: X and Y its top left,
    // off it goes with Menu Content (kContent)
    {"conqbalance", L"Conquest: Balance", kMove | kHide | kFrame, {"menu    conq6sta"}, 1, 0, 99.4, 3.0, 112, 72},
    {"conqpoints", L"Conquest: Points", kMove | kHide | kFrame, {"menu    conq3pts"}, 1, 1, 99.4, 96.5, 112, 40},
    {"conqcontrol", L"Conquest: Regional Control", kMove | kHide | kFrame | kContent, {"menu    conquer1"}, 0, 0, 0.6, 3.0, 112, 176},
    // the Besieged map's (Region Info > Besieged): Orders and Battle Status at the left ("menu    beseige1"; a beastmen's
    // stronghold under the cursor, Halvung, Arrapago or the Mamool Ja Savagelands, puts "menu    beseige2" in its place,
    // 0x101E6D8D), laid out at the top left (16, 48) with Menu Content's windows (240, 226 with Menu Content there, seen
    // live 2026-10-23): X and Y its top left, off it goes with Menu Content; and Imperial Standing over the party list
    // ("menu    imperial", 2752, 1524 - 2864, 1564), X its right edge, Y its bottom
    {"besorders", L"Besieged: Orders", kMove | kHide | kFrame | kContent, {"menu    beseige1", "menu    beseige2"}, 0, 0, 0.6, 3.0, 136, 206},
    {"besstanding", L"Besieged: Standing", kMove | kHide | kFrame, {"menu    imperial"}, 1, 1, 99.4, 96.5, 112, 40},
    // the Colonization map's (Region Info > Colonization): Coalition Rank at the left while Adoulin is under the cursor
    // ("menu    colorank"; another zone puts "menu    coloresu" in its place, 0x101E70C3), laid out at the top left with
    // Menu Content's windows (240, 226 with Menu Content there, seen live 2026-10-23): X and Y its top left, off it goes
    // with Menu Content; and Bayld over the party list ("menu    colopoin", 2752, 1524 - 2864, 1564), X its right edge, Y
    // its bottom
    {"colorank", L"Colonization: Rank", kMove | kHide | kFrame | kContent, {"menu    colorank", "menu    coloresu"}, 0, 0, 0.6, 3.0, 112, 120},
    {"colobayld", L"Colonization: Bayld", kMove | kHide | kFrame, {"menu    colopoin"}, 1, 1, 99.4, 96.5, 112, 40},
    {"buffs", L"Status icons", kHide, {"menu    buff    "}, 0, 0, 0, 0, 0, 0},
    {"macrobar", L"Macro bar", kHide | kFrame, {"menu    mcr1pall", "menu    mcr2pall", "menu    mcr1long", "menu    mcr2long"}, 0, 0, 0, 0, 0, 0},
    // the macro bar while Macros > Edit Macro edits it (seen live 2026-11-11): its Ctrl row ("menu    mcr1edit", the long
    // bar's "mcr1edlo") with its Alt row under it ("mcr2edit", "mcr2edlo": kUnder), and the editor a macro opens in their
    // place (Name, Line 1 ... 6: "menu    macro   ", 0x101A2D7A, which closes the rows). All laid out at the top left (16,
    // 48) with Menu Content's windows: X and Y their top left; off, they go with Menu Content as before (kContent)
    {"macroedit", L"Macro bar: editing", kMove | kHide | kFrame | kContent,
     {"menu    mcr1edit", "menu    mcr2edit", "menu    mcr1edlo", "menu    mcr2edlo", "menu    macro   "}, 0, 0, 0.8, 4.0, 480, 84},
    // the box at the right beside it: a macro's Button Edit (Copy, Paste, Delete: "menu    mcrbedit", laid out at 384, 134
    // on the right) and, once a macro is opened, the editor's ("menu    mcrledit", at 384, 48 in its place; area 0, so never
    // the main menu's follower). X its right edge, Y its top; off, each is where the game puts it. Moved, it's drawn over
    // every menu (kOnTop): put over the bar, the rows the game makes again as the cursor changes row went over it (reported
    // 2026-11-14: "that edit option panel should always be above the macro panel")
    {"macroeditoptions", L"Macro bar: edit options", kMove | kHide | kFrame | kOnTop, {"menu    mcrbedit", "menu    mcrledit"}, 1, 0, 99.2, 11.5, 112, 93},
    // the space the top-left windows are laid out in, from the first window's corner (16, 48), X and Y its top left
    // (place_frame_hook), no layout of its own; at the game's place while it's not moved (wanted), not its X and Y
    {"menucontent", L"Menu content", kMove | kFrame, {}, 0, 0, 0.0, 0.0, kContentW, kContentH},
    // the Items list opened from the main menu ("menu    inventor" at its own place; the Equipment screen puts it beside
    // itself), its item descriptions with it, X and Y its top left (content_shift); the game's place 16, 48
    {"inventory", L"Inventory", kMove, {}, 0, 0, 0.6, 3.0, 182, 190},
    // the Check window (/check: another's gear, "menu    inspect ", laid out at 16, 48 with the top-left windows), X and
    // Y its top left. Off, Menu Content moves it as it did (kContent)
    {"checkwin", L"Check window", kMove | kHide | kFrame | kContent, {"menu    inspect "}, 0, 0, 0.6, 3.0, 182, 190},
    // the comment box at the bottom left, over the chat's top rows ("menu    comment ", area 0xC: a checked player's bazaar
    // message under the Check window, made at 0x1013347A, and a search result's comment, 0x1020BFE6...; laid out at 16,
    // 240 of the 512 x 448 and anchored to the bottom, 16, 992 - 382, 1048 of 1920 x 1200, seen live 2026-10-24), X its
    // left edge, Y its bottom. Its text goes with its window (SetLocate from Lua, seen live), and the game doesn't put it
    // back. Its bottom anchor keeps it out of the top-left 512 x 448, so Menu Content never took it
    {"commentwin", L"Comment window", kMove | kHide | kFrame, {"menu    comment "}, 0, 1, 0.8, 87.3, 366, 56},
    // the gauge of your HP, MP and TP the Commands menu puts up while you fight ("menu    gaugewin", laid out at 16, 48
    // with the top-left windows: 139, 135 with Menu Content moved, seen 2026-10-22), X and Y its top left; hpgauge.hide
    // leaves it out. Off, Menu Content moves it as it did (kContent)
    {"hpgauge", L"HP gauge", kMove | kHide | kFrame | kContent, {"menu    gaugewin"}, 0, 0, 0.6, 3.0, 128, 44},
    // the time left in a battlefield or an instance (an hourglass and 29:27: "menu    cnttime ", and "menu    eventtim" of
    // the same size; both laid out at 16, 94 - 144, 118 of the 512 x 448, read from ROM\119\51.DAT 2026-11-11, not seen
    // live), X and Y its top left. In the top-left 512 x 448, so Menu Content took it along: a player who had moved Menu
    // Content found it mid-screen with no element of its own (reported 2026-11-11). Off, it goes with Menu Content (kContent).
    // "menu    holdtime" too: in the game's menu table, and in its own 51.DAT laid out byte for byte as "eventtim" (same
    // place, frame and piece; read 2026-11-13), what for not known; the ffxi-hd overlay's 51.DAT hasn't it
    {"battletimer", L"Battle timer", kMove | kHide | kFrame | kContent, {"menu    cnttime ", "menu    eventtim", "menu    holdtime"}, 0, 0, 0.6, 6.0, 128, 24},
    // a shop's Yes/No before it sells or buys ("menu    shopsell", "menu    shopbuy ": 112 x 40, laid out at 16, 256 of the
    // 512 x 448 under the shop's list, read from ROM\119\51.DAT 2026-11-11, not seen live), X and Y its top left. It came up
    // behind a player's chat with no element to move it (reported 2026-11-11). Off, it's where the game and Menu Content put
    // it as before (kContent). No Hide: a Yes/No left out would still be asking
    {"shopconfirm", L"Shop confirm", kMove | kFrame | kContent, {"menu    shopsell", "menu    shopbuy "}, 0, 0, 0.6, 40.0, 112, 40},
    // the Level / EXP box under the Status window ("menu    level   ", CTkPersonaLevel's: the game lays it out at 16, 208,
    // 2 under the Status window's 16, 16 + 190, and its handler (0x10147010) puts it back where it was made every frame),
    // X and Y its top left. Off, it sits under the Status window and goes with it (kStatus: wherever Menu Content put
    // that and however tall status.taller made it; place before each draw, and locate on the handler's SetLocate). Out of
    // the table it was Menu Content's, made before the Status window: status_drop's 32 never came (no Status window up
    // yet) and the taller Status window covered its top rows (seen 2026-10-24: 150, 295 under a Status window at 150,
    // 135 - 336), or made after it, it went by the box twice
    {"expwin", L"EXP window", kMove | kHide | kFrame | kStatus, {"menu    level   "}, 0, 0, 0.6, 13.0, 112, 88},
    // an item's description (under a list, the Check window's, a shop's...: "menu    iteminfo" and the taller ones for
    // more lines, "item4inf" ... "item12in", and those that grow up from their bottom, "itemxinf", "itemx4in" ...), X and
    // Y its top left. Off, it goes with its list as it did (content_shift, kContent). Moved, it's drawn over every menu
    // (kOnTop): under the Equipment window, its stats and the Items list, where it was put, it was cut by them
    {"itemdetails", L"Item details", kMove | kHide | kFrame | kContent | kOnTop,
     {"menu    iteminfo", "menu    item4inf", "menu    item5inf", "menu    item6inf", "menu    item7inf", "menu    item8inf",
      "menu    item9inf", "menu    item10in", "menu    item11in", "menu    item12in", "menu    itemxinf", "menu    itemx4in",
      "menu    itemx5in", "menu    itemx6in", "menu    itemx7in", "menu    itemx8in", "menu    itemx9in", "menu    itemx10i",
      "menu    itemx11i", "menu    itemx12i"},
     0, 0, 0.6, 20.0, 366, 56},
};
