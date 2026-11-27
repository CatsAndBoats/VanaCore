# Changelog

What's new in each VanaCore release. A release's notes on GitHub are its section here (`## 0.9.12

### New
- VanaCore App: VanaCore updated by itself as the app opens, where VanaCoreInstaller.exe put it (unins000.dat): the glass bar says Checking for updates in place of the profile and START, downloads a newer release's installer with a bar, checks its signature is this app's signer, runs it silently over the folder (the app's own exe and DLL moved aside to .old first, so it's never closed), then the profile and START come back with the new version. A green UP TO DATE pill beside the head's version when nothing's newer or it just updated; a failure gives START back with a red line. Only this install's own VanaCore in use holds it back. Tested on a retail install: 0.9.10 to 0.9.11

### Changed
- VanaCore App 0.1.1
- Ray-traced reflections from stairs and hills: the sky kept, effects on the floor in view, Whitegate textured
- /vana Reset: a panel's Reset leaves what each setting's own reset would
- VanaCore App: the HTTPS download and file checks shared (app\shared\net.cpp, net.h: WinHTTP, the system's proxy, TLS 1.2 and 1.3, WinTrust) by VanaCoreApp.exe's update of the DLL (exe\update.cpp) and the DLL's update of VanaCore from its home screen (dll\home_update.cpp). Not built or tested yet
- /vana: one layout, the one with the pages as tabs in a header. It was behind /vana3 and kept per person (layout = 'hybrid' in config\addons\vana\settings.lua), so players kept the old window, with the list of every page and the tools at the left, since 0.9.10. The old layout is gone: its title line, its list, its minimize and close; so are layout in settings.lua, /vana3 and /vana classic
- /vana Reset: an effect's strength back to its slider's default in the key's units (Reflections' Strength 5, not 10)

## 0.9.11

### Changed
- mcp 1.2: sym(name) and vcall(name, signature, ...): VanaCore.Core.dll's symbols from the .map beside the loaded DLL, refused unless the .map's timestamp is the loaded DLL's, at the base it's loaded at; C names with or without the leading _, C++ by a mangled name's start (one match only). An address carried over from another build called into some other function and took the game down
- Reflections: the bed under deep water takes none. floor_ok took the deep water's floor (kind 9, at its surface) over a river's drawn bed as collision hiding the floor, and the bed borrowed its neighbours' answer from a ring 8% of the screen across: from afar the ring reached the stone banks and the bed under East Sarutabaruta's river took their sky, milky water; zoomed in, none
- Water: a pond's flood stops at a fall. East Sarutabaruta's upper river flooded the river below its fall at its own height, 4-5 yalms over the game's water, and a 22,000-cell flood hung 8-13 yalms over the rivers north of it: the deep water's floor (terrain 9) counts only within FILL_SURFACE (1 yalm) under the level, a cell is made water from a neighbour only if its floor is no more than FILL_STEP (2.5) under the neighbour's, a flood's cell with a point over ground more than FILL_SHELF_DOWN (1.25) or another water's surface more than FILL_STEP under the level is none (the stair of water hanging beside the G-8 fall), shelves lie no deeper than FILL_SHELF_DOWN, and a flood's shelves reach FLOOD_SHELF (2) cells, not 8 (a bank's top a hand under the level chained water down it past the lip)
- /exec and /include: scripts in subfolders (/exec folder/script runs scripts\folder\script.txt), as Windower has them; each part checked, so no '..', drive or leading slash gets outside scripts\
- Ray-traced reflections: characters reflected from the feet up, not cut off at the knees
- Build Installer: AzureSignTool unquoted in Inno's sign command (pwsh passed the quotes to ISCC escaped: "Failed to execute Sign Tool command")

### Fixed
- Folders: addons\ashita, addons\windower, plugins\ashita and plugins\windower made at start if they aren't there, each with a README.txt saying what goes in it (written when the folder has none), so a new install shows where things go (folders.cpp; plugins.cpp no longer makes the plugin folders itself)
- Login: xiloader 3.0 (LandSandBoat from 2026-11-05), the lobby's data and view connections over TLS. VanaCore reports 3.0.0 first and, refused for another version it speaks (the server's message names it: "requires version 2.1.x"), logs in again once with that, the lobby plain: the version is checked before the password, so the refusal isn't a failed login. [server] loginversion= reports that version only. The data link is TLS end to end; the game's view connection goes to a relay on this machine (connect() redirected) that carries it to the server over TLS, as xiloader 3.0's does (loader_lobby.h). The launcher's Schannel client is shared (vanacore/tls.hpp: chunked writes, read what's there, no time limit after the handshake, the Windows 7 TLS 1.2 retry). The data link's packets are framed by their fixed sizes (5-byte requests, the 0x148 character list), as LandSandBoat writes them at 2.1 and since. GameLogin::lobby_tls, BootInfo 7

### Misc
- README: the Retail section's installer download is a button (assets/download_retail.png)

## 0.9.10

### New
- VanaCore App (app/): a C++ GUI in two signed files, VanaCoreApp.exe (the splash, the update check to come, then loads the DLL; changed rarely, as a new signed exe starts its reputation over) and VanaCoreApp.dll (the views and logic, updated freely), as the CatsEyeXI launcher's bootstrap and App.dll split. Its own solution (app/VanaCoreApp.sln, x86, the CRT linked in), a plain C interface between them with a version the exe checks (shared/app_api.h), each file's own version (shared/version.h), and the .vcaddon card's look (shared/ui.cpp). The first view: Play (VanaCore.exe --retail beside it) and Close
- /vana: Graphics pages tidied. Draw distance: the hide toggles sit with the distances, Fog is above the Advanced Settings box, no Distance or Hiding heading. Shadows: the day and night rows (Distance, Shadow Quality, Softness, Underside Shadows) at the top with no heading; Shadow Quality is Low, Medium, High, Max (512 to 4096; 8192 is an ini edit) and Softness is Sharp, Low, Medium, High (0, 0.05, 0.1, 0.2). Ambient occlusion, Reflections and Glow quality name their top level Max (Ultra gone: Max is the 24 taps, the 32-tap level is ini-only); AO's On characters is Characters Strength. Lighting: Ambient Light Strength, Sun Strength and Indoor Brightness are plain sliders at the top with no heading or switch (above 0 is on; M.unswitched switches on one an old <key>.enabled=0 left off), the two character light rows under them, and Temperature is Day/night warmth in Colour grade under Warmth. The Toggles page's chips for those follow the sliders (value off/on). The new layout's green headings have no reset arrow; Enable Advanced Graphics Settings is red. docs/reference.md and vanacore.example.ini follow
- Sound: 100 sound slots for everyone, and sound.max_sounds a cap changed while the game runs. The slots are made as the sound thread starts whatever the setting; max sounds stays what's asked for (the game's 12 or 20 as before, the slots past it unused) and soundslots::reload (overlay.cpp's settings reload) gives the game a new one at once: vana.config.set, or the file once taken up, logged as Sound: max sounds N. The max sounds check's forced 20 follows it, never under 20. Tested in game: 100 sounds at once; set to 20 live, 150 asked for over 30 frames peaked at 21; back to 100, 91. Docs: reference.md, ffximain-offsets.md (also kPosition, kStop), vanacore.example.ini

### Changed
- bundle_app.ps1: the App release in $appRelease, not $app: PowerShell's names ignore case, so $app was the [string] -App and the release became text (app.yaml prod: "has no VanaCoreApp-*.zip")
- /vana: the Client page opens at its VanaCore sub-tab at each game's start; the other pages still open at the sub-tab they were last on. vana 2.4
- HUD windows: [client] <id>.scale draws one of the game's windows bigger or smaller, 50 to 200 percent about its anchor point (the HUD Manager's Scale under Position; Party List and Main Menu so far, the menus the main menu opens in its place taking mainmenu.scale). The game lays the window out as ever: the renderer maps its screen-space draws by their viewport (d3d8_winscale.h, vanacore_gfx_window_scale, nested scales composed; ui_map_axis keeps the target's edge under a second mapping), and the mouse's tests of it are asked where the game laid it out (hudwin_scale.h: IsOnWindow's hook, IsOnButton through chat.cpp's on_button_hook); the HUD Manager's box is as big as the window is drawn. /vana: Frame rate's rows are above the window's place, and a section's sliders share a value column as wide as their widest value, so the tracks end together. In the docs and example ini
- Reflections: wetness is a look of its own: wet ground darker with its colour deeper and the reflection sharp from above, dry ground softer (more blur, ray-traced blur raised as refl_wet goes under half) and reflecting at low angles. With the ray-traced reflections' blur on, the floor's grooves are taken off after the blur (refl_relief), so Surface Depth still shows. The tree's ray keeps the first of two surfaces at one place (a triangle stored twice), which traded places ray to ray and flickered; the screen's nearer hit is shown only where the tree has no surface at that point. Render visual 33 shows what the tree's walk met (magenta out of steps, blue the sky). In the docs and example ini
- Screenshots without the interface: the MCP screenshot tool's ui false gives the 3D scene alone as the frame shows it (effects, upscaler, colour grade), copied just before the game draws its interface (gfx_scene_shot.h, d3d12 and d3d11). Water's triangles debug view ([graphics.effects] debug=34, d3d8_water_tris.h): each triangle a flat colour, its edges in black over it; on /vana's Debug view. Ray-traced reflections: the game's effects mirrored about a level raised by refl_rt_fx_level, a batch's pieces each a caster of its own, the ambient light and the sky's colour on what a ray finds. mo_draw's screen-fill tests moved to gfx_d3d12_mo_fill.h for gfx_d3d12.c's length; in the docs and example ini
- actorscale: a VanaCore addon that makes the target bigger or smaller, on your screen only (/scale: a window with a size slider; /scale <0.1-10>, up, down, reset, resetall, list)
- The log window's Maximum lines displayed (Config > Windows > Window Settings) goes down to 1, not 4 (game_config.cpp): option 15's least in its table, the Window Settings' Left key and the log window's own setter, which took anything under 4 as 24; in the docs
- Chat log: the expanded log's line selector in chat.lineheight's rows (chat_lineheight.h apply_selector): drawn and picked by the mouse by L pixels, not 16, and its box L tall from L - 1 over the row's bottom. With 20 px rows it stepped 16, fell between lines a few rows up and sat 5 px low, clipping the line's top
- A welcome line in the chat log once per game start, when the character is first in: [VanaCore] Thank you for using VanaCore! Type /vana in chat to get started (/vana in yellow)
- Ray-traced reflections on by default (refl_rt 2), worked out at a share of the scene's size (refl_rt_res, 0.5 by default; /vana Low to Max) with an optional Gaussian blur (refl_rt_blur) and the game's effects mirrored into them (refl_rt_fx), the tracing's shader and effects in gfx_d3d12_rt_hlsl.h and gfx_d3d12_rtfx.h. VanaCore's weathers other than rain (d3d8_weather.h, graphics_weather.h): embers, dust, leaves and wind, snow, motes of light and ash round the camera in the server's weather where the zone's files have it, none under shelter (weather, weather.fire/dust/wind/snow/light/dark, weather.preview for the session); on /vana and in the example ini and docs
- Chat log resized by chat.lineheight's rows (chat::line_height), not the game's 16 units: with 20 px rows a resized log kept 4 rows too few and left a gap over its first line
- Water: the whole sea (water.sea's plane over a zone whose water reaches its rim) drawn as a 24 x 24 grid (SEA_GRID), its cells finest round the camera, in place of one quad out to the reach (50,000 yalms with water.horizon): clipped to the view, that quad gave the pixels near the camera a surface place and depth a few centimetres off that moved with the camera, and in Bastok Markets' canal the shore's teal fade and foam rim came and went along the whole bank with every step
- Ray-traced reflections on /vana (Graphics > Reflections > Method: Screen Space or Ray Tracing, refl_rt 0 or 2; docs), the zone's own meshes traced with their textures and vertex colours. A True Font area's shadow switch is its own (only the buttons' chain, titles, main menu and Action Menu, shares one). gfx_d3d12.c and graphics.cpp over the filelen lint and gfx_scene_done over lizard's allowance (370 of 365) until they're split
- VanaCoreApp: Graphics has Mode, Resolution and UI resolution drop-downs as /vana lists them ([ffxi] mode, resolution.game, resolution.ui; a resolution pick takes out [window] lastsize), the scene effects switch and the renderers' notes gone. Profiles before Pivot; a Logs tab (every .txt and .csv under logs\ and chatlogs\, newest first, opened in Notepad); each profile a desktop shortcut "FFXI <name>" (VanaCore.exe --retail --ini, VanaCore's icon); the App's own tooltips on its icon buttons
- Ray-traced reflections, work in progress: a zone's blended parts and movers go into the zone mesh for the reflections alone (rt_only, their own buckets; blended parts as cut-outs by their texture's alpha, movers where they stand at the start), kept out of the sun's shadows. graphics.cpp and gfx_d3d12.c are still over the filelen lint's allowance (2492 of 2486, 17558 of 17454) until they're split
- Ray-traced reflections, work in progress (refl_rt): a bounding-volume tree of the zone's triangles (rtbvh.c) built from the zone mesh and walked in the pixel shader (the game is 32-bit: no DXR), the D3D12 side in gfx_d3d12_rt.h; with the zone mesh, floors and casters' changes it needs. graphics.cpp and gfx_d3d12.c are over the filelen lint's allowance (2492 of 2486, 17551 of 17454) until it's split
- True Font shadow on /vana: each area's Shadow on one line under its font line (a switch, Opacity a bar, Blur and Thickness drop-downs, what's drawn unset shown from the area it follows); blur up to 10, thickness up to 5, the shadow straight under the letters by default (x, y 0). Chat line height ([client] chat.lineheight, chat_lineheight.h: the chat draw's four row steps patched). Shadows' Time of day on /vana (freecam's daytime), freecam's panel on the shared tab bar
- VanaCoreApp: the home on the background picture (assets\appbg.png) under a dark tint, a frosted glass bar at the bottom with the status on the left (icon, title, detail, a bar for downloads) and START on the right: a drawn button, a drop shadow, a 1 px cyan border, blue left to right, PT Serif Bold (res\ptserif_bold.ttf, OFL) a pixel spaced over a dark shadow, white to a cool grey. Icons from Material Symbols Outlined (res\symbols.ttf, cut by res\make_symbols.py), loaded from memory. The head reads "v<VanaCore's version>  -  by: Vekien & Carver": VanaCore.Core.dll now carries a VERSIONINFO (ProductVersion from version.hpp's VANACORE_VERSION_TEXT, which release.yaml sets with kVersion)
- True Font shadow: a thickness (<area>.font.shadow.thickness, the shadow's letters fatter), the area's outline kept over it, down follows across until .shadow.y is set; the main menu's font line and its shadow in one box on /vana (box_span). The shadow's code into chatfont_shadow.h (chatfont.cpp back under the 1500 lines filelen allows). Ambient occlusion and reflections on /vana in 1 to 10 steps (Thickness, Character Smoothing and Sun Glint Off/Low/High, the reflections' reach under an advanced group); ao_chars 1.0, ao_soft_chars 0.5 and refl_dist 100 by default; Smoothing over frames off the panel and every file's temporal put back to 0.99 once (registry.cpp reset step 4)
- Water: the zone's floors and the sea's state (g_wfloor, g_sea, SEA_*) into d3d8_water_level.h, with the code that keeps them: d3d8_water.h under the 1500 lines filelen allows (1528 on main)
- VanaCoreApp: the splash checks GitHub for a newer App release and swaps in its DLL (SHA-256, signature, version checked; the old DLL kept to roll back); app.json carries the DLL's hash
- /vana window: green switches, a hide-the-list button at the bottom left, Debug window's tab bar sized to its tabs
- True Font: a soft shadow under the letters (<area>.font.shadow, blur, offset, opacity) in the outline's place; /vana rows for it
- AO rings removed (ao_rings, ao_ring_spacing): in a GPU-bound A/B they saved 0.66 ms a frame (3.6%), nothing at all when CPU-bound; ao_quality already sets the taps. Shadow, rain and grass rings stay
- Ambient occlusion rings cut harder: past each ring 60%, 40% and 25% of the taps (was 75%, 50%, 30%), and the floor for the blur's block a quarter of what it was (0.05 per pixel of the taps' disc), so the rings take effect from a disc of ~9 pixels out rather than doing nothing inside ~100 to 200 world units. Ring spacing 2 to 150 (was 5 to 60)
- /vana search: a row is found by its name only, not by its tooltip or its section's title (a font or colours row by its parts' names too)
- Launcher: --gui taken out (the start screen becomes a retail start app of its own; its last version is the commit before this)
- Launcher: --gui start screen drawn as the .vcaddon card (the /vana palette, VanaCore's logo, round corners, its own text boxes), and its console let go while it's up (Windows Terminal ignores ShowWindow). Kept here for the retail start app's own repo; taken out next
- Launcher: --gui without --ini reads vanacore.ini beside VanaCore.exe (its path had "\v", a vertical tab, for the backslash)
- Launcher: --gui start screen (work in progress): Retail / Private server, the server and a settings file picked before the game starts, handed on as --retail, --server and --ini (gui.cpp, gui.hpp)
- /vana: hybrid layout work in progress, and the Loading Screen and Zones pages named under Mods (loadscreen.lua, zonename.lua, reference.md, vanacore.example.ini: /vana > Mods > Loading Screen, Mods > Zones)
- /vana: hybrid layout work in progress (/vana3): a section drawn as panels (M.hy: card, begin, span, open, close, row, finish), a line across under a panel's heading and a faint line between its rows, flat by default (M.hy.FLAT, no box). /vana dlss and /vana fsr open Graphics at DLSS / FSR, and a sub-tab's name alone (/vana windows, /vana shadows) opens its page at it
- /vana: HUD Settings and HUD Presets are windows of the HUD Manager (gear and pen icons, resizable), the HUD page leaves the list of pages and Game Options' list opens the HUD Manager from its first entry. Preset names are cyan with a green tick on the loaded one, Bundled presets load from their names, Disable True Font in its own box before Default Font, Addon View icon green when on
- Debug window: no /vana debug label in the title bar

### Fixed
- /vana: the Experimental page is gone. Water Shader is the Mods page's (after Weather Effects, "(Experimental)" off its name, its banner in place), Normal maps are Graphics' own as Normals (WIP), after Depth of Field, and the Layers list names them so too. The Graphics page's search takes glow, elight, reflections, refl, normalmap and normals; the Mods page's water. A Graphics preset, /vana preset and the Copy Graphics Settings tip no longer speak of Experimental settings. vana 2.3
- /vana tidy, height fog keys and defaults, Texture Filtering's mode, weather looks and wind, Bounce Lighting off at night, fight watch, HUD scales. /vana: a warning banner heads every Advanced Settings box, the banner standard lives in draw.lua (M.banner, M.banner_height, M.wrap_lines, spacing in M.BANNER; text lines 17 px apart), Weather Effects and Snow Trails are the Mods page's with an Experimental banner in place of "(Experimental)" in their names, panels named in title case (Ambient Occlusion, Bounce Lighting, Height Fog, God Rays, Texture Filtering, Ink Lines), Character brightness without its x, Bounce Lighting's Quality is Detail (Soft to Sharp). Height Fog: Density and Thinning 0 to 100, Thickness under Density, Distance from Camera, Thickening stops below under Advanced Settings; defaults Density 0.01, Thickness 0.6, Glow Towards Sun low; fog_sun is fog_sunlight (0 to 10, as /vana shows it) and fog_g fog_glow (low, medium, high or a number), an old key moved on start (registry.cpp move_old_keys, times 5 for the sunlight) and in a preset as it's read (vana.lua M.old_preset_keys). Texture Filtering: Filtering (aniso: 0 the game's, 1 trilinear, 2 to 16 anisotropic) laid over whatever filter the game asks for, down a chain as far as it was filled, and Texture Sharpness (tex_sharp, up to 2 mip levels finer), both in gfx.h gfx_world_filter for both renderers; neither reaches the ground yet, and a temporary probe logs which check it fails (not built). Weather: Weather preview shows the game's own sky, fog and light for the picked weather (weatherpreview.lua), a weather the zone has no look for borrows another zone's (weatherlook.cpp, d3d8_weather_look.h), and the particles' wind has Fixed direction, Direction, Drift and Strength. Bounce Lighting: Off at Night fades it out 17:00-18:00 and in 05:30-06:30 outdoors (gfx_d3d12_gi_clock.h). Fight watch near an Aern logs the frame's lighting for the Al'Taieu white-out reports (fightwatch.cpp, d3d8_fightwatch.h). HUD scales for the cast bar and target window (hudwin_scale.h). In the docs and example ini
- Reflections: the ray-traced reflections' tree is split by the surface area heuristic (rtbvh.c: 16 slices an axis; the median where it finds no split, or where a leaf could pass 28 deep, the shader's walk keeping 30 nodes by) and the walk no longer reads and tests a node twice: the rays took about 10 ms at Bastok's Metalworks gate and take about 4. Surface Depth works with Ray Tracing as with Screen Space (the ray is bent by the floor's relief), is 0 to 20 in /vana for 0 to 2.0, and fades in from the player's feet over refl_bump_near (Surface Depth Fade). What moves (a door, a lift) is marked in the tree and the screen's picture of it stands over the tree's; a surface under cover that the sun doesn't reach (an arch, a recess) takes half the light, by a short ray up. The floor mask's narrow tops are found along the floor's own plane, so the mask no longer goes black as the camera pitches. Ray Tracing's rows are under a heading of their own in /vana. Defaults: Quality High, Roughness 5, Surface Depth 2 fading in over 30 yalms, Sun Glint High, Ray Tracing Resolution High with the effects reflected, Reflective Distance 150, no lift. In the docs and example ini
- VanaCore's installer is Inno Setup's (installer/vanacore.iss) and VanaCoreApp's own installer is gone (install.cpp). The setup is dark, on the App's backdrop with VanaCore's logo, titled "Install VanaCore by Vekien & Carver", each page headed with VanaCore's name and version; it suggests VanaCore beside FINAL FANTASY XI (the registry's install, one folder up) when the player can write there, else their own Programs folder, and turns down a folder they can't write to (Program Files). Build Installer (.github/workflows/installer.yaml) makes it from a release's zip, signed with the setup's uninstaller, onto that release as VanaCoreInstaller.exe: after release.yaml's draft, again after promote.yaml publishes it, or by hand
- Reflections: how wet the ground looks goes with the strength (refl: low a soft sheen at low angles, high a dark sharp mirror); refl_wet is read and no longer used, and Wetness is off /vana. The reflections' mask is a debug view of its own (14, Reflection mask), the narrow tops of things kept out of it; the mirrored effects' level is raised by the blur's radius too
- Water: the whole sea's mesh is a geometry clipmap (d3d8_water_sea.h sea_clipmap): even 2-yalm cells round the camera, 16 a side, then levels of cells twice the size, each with the finer level's square cut out and held to the world on its own grid so the triangles don't slide as the camera moves; each cell four triangles to its middle, the coarser cells along a finer square split at their sides' middles to meet its corners (no T joins). It was a 24 x 24 grid in cells by the cube of the step from the camera: a few cells 30 to 1,000 yalms across in view, long slivers to one point under the camera
- VanaCoreApp as VanaCore's installer (app/dll/install.cpp): run from a folder without VanaCore.exe, it asks where to put VanaCore, takes the newest release's zip from GitHub, unpacks it there with VanaCoreApp beside it, makes the shortcuts and an Apps & features entry, then opens the App; --uninstall takes it away again. installer/vanacore.iss: an Inno Setup 6 script for one setup exe with the whole package in it
- VanaCoreApp: tabs under the logo and profiles. FFXI (the install from the registry with a tick, else Locate: [app] ffxi and each profile's [client] retail.pol; VanaCore beside the app; Gamepad Config, the game's folder), Graphics ([graphics] renderer, effects), Pivot ([pivot] enabled and rootfolder, [pivot.order] on/off and up/down), Profiles (a named settings file each, vanacore.profile-<n>.ini copied from the one in use, VanaCore.exe --ini; vanacore.ini's [app.profiles] and [app] profile; renamed in place, deleted to the Recycle Bin) and About; Errors, red with its count, only when something's wrong and opened by itself, each with what fixes it. The footer: the profile (its pick-list above it) and START side by side in the middle. The tab panel darker with a soft inset shadow, the background 5% past covering the card (its border lines cut off); icons for them in symbols.ttf
- Water: a sea drawn twice, one sheet over the other, is the upper one's: Port Jeuno's under-sheet no longer the sea's level
- Target window: a name too long for the window goes onto two lines again with its True Font on, as the game has it (after its ':', else at the last space the first line fits by, the second 12 lower), where it was always one line and ran past a short window's edge. target.name.oneline (/vana > Target > Name > Name on one line) keeps it on one line, True Font or not. Tested in game. vanacore.example.ini: target.name.oneline
- Macro bar: Esc leaves a moved Button Edit box again. With macroeditoptions.move on, the box (Copy, Paste, Delete) was left on layer 5 so it's drawn over the macro bar; the layer (window +0x60) also orders the menu manager's list (0x1015C840 keeps 5 or more ahead of the rest), and each frame the manager gives the keys to the first window of it that can take them (0x1015E9B7). Esc handed the keys to the row, the row went in behind the box, and the next frame the box had them back: the menu couldn't be left without a relog. The layer is now the window's only while the manager draws its windows (hudwin_place.h: kDrawAll, 0x1015E3D0 detoured, raise before, lower after), for the item details too (itemdetails.move). Reported by Draugr; tested in game. docs/ffximain-offsets.md: the new pattern
- Sound: a sound that couldn't start no longer moves or fades out every sound. With max sounds reached the game hands a sound the handle -1, which the sound thread takes as every sound playing; a world sound's position (FUN_10038250, command 3) and stop (FUN_100369F0, command 5) send their kept handle checking it only for 0, so one that never started moved, quieted or faded out all of them (sounds cutting out on an AoE weaponskill on a pack). They send 0, no sound, for it instead. Whatever sound.max_sounds is, hooked as the sound thread starts (soundslots.cpp). Found by Settra. docs/reference.md: Sound slots

### Misc
- README: a Retail section (in active testing) with the latest installer's download (VanaCoreInstaller.exe on the latest release)
- Lint: back under the baseline. graphics.cpp's apply_grass is an include unit of its own (graphics_grass.h), draw_packet's half-pixel check is uncentre_half_pixels, and the zone DAT's section walk is one (zoneweather.cpp walk_sections, a Section each), which read_weathers and weatherlook.cpp's read_zone both take. cppcheck: members given initializers (registry.cpp's Value, Named, Move, StandInValue and Reset, hudwin_scale.h's Extra, targetwin_name.h's NameFrame), consts where nothing writes, the screenshot callback's vector and string by reference, the comma statements split, casts to float through void*, the RT up vector's local renamed and copied in, the backdrop strength's NaN case said as such, and the fight watch's draw-log hook only in VanaCore's build (its stub always said no)
- Release: VanaCore App released on its own (app.yaml: dev builds, signs, pushes VanaCoreApp.exe and .dll to vcdev and drafts an app-v<version> release; prod puts the tested draft's files into the vanacore build and the latest VanaCore release's zip, then publishes it, never marked latest). The exe is the last published App's while VCAPP_EXE_VERSION is unchanged. VanaCore carries the newest published App: release.yaml bundles it into every package, promote.yaml again after promoting (bundle_app.ps1). push_build.py --partial pushes a few files into a build without listing or deleting the rest
- docs/reference.md: Sound slots, what sound.max_sounds above 20 changes (the 26 walks VanaCore rewrites, the cache check before a loaded sound is freed among them; the status list kept at 20), and what more slots don't change: sound groups stopping each other, the 0x20 once-at-a-time bit, the 22 loaded sound files, voice stealing. World Sounds Volume covers the weather and the zones' and events' ambient loops (in soundmix.cpp since f2e4f0d), weather changing at once as it moves
- docs/ffximain-offsets.md: every place VanaCore hooks, patches or reads in FFXiMain.dll, feature by feature (132 sections, about 390 patterns): its setting, what it changes, each pattern with its source line, what's read back from the match, the CatsEyeXI addresses the source records, and how it copes with retail. Above the catalogue: how a signature becomes a hook (scan::find, scan::operand, detours, call rewrites, byte patches), what keeps the patterns working in retail (wildcards, read-back, alternatives, sigs::item_data_shift), and a watch list of what the audit can't see: the gamepad's fixed addresses in input.cpp, operands read past a pattern's end, fixed distances (kTargetPointer 0x2F4), structure fields written into patterns, and the patterns added since the 2026-10-31 audit. Linked from retail-signatures.md and reference.md

## <version>`), under New, Changed, Fixed and Misc: write the next version's section with its version bump.

## 0.9.9

### New
- **Snow Trails:** A new snow.sand setting (off by default) lets sand ground take footprints too, from the next zone.

### Changed
- **/vana:** Advanced Settings groups now show when Enable Advanced Graphics Settings is on, DLSS 5 has a heading of its own, and only the panels' Reset buttons remain.

### Fixed
- **Water:** The far sea now fogs to the sky over the horizon instead of the zone's flat grey, and a tree on the shore no longer leaves its crown in navy on the sea.
- **Name icons:** Name icons now stay clear of the name on ultrawide screens.
- **/vana:** Banners no longer tint their row under the mouse.

## 0.9.8

### New
- **Water:** VanaCore's water now mirrors the world around it, the shore, trees, walls, cliffs and characters, broken up by the waves, with the sky's own clouds and dusk colours in it (by Carver).
- **Water:** Rain now leaves spreading rings where it lands on VanaCore's water, none under roofs or bridges (by Carver).

### Fixed
- **Water:** Lake Mechieume should now be VanaCore's water from shore to shore, without a jagged line across it, and Jugner Forest's and East Sarutabaruta's rivers should run on unbroken (by Carver).
- **Water:** Fountains should no longer show bands of water by the screen's right edge with a large Background resolution, as in Bastok Markets (by Carver).

## 0.9.7

### New
- **Mass Grass:** A new Mega dense option under Coverage fills all the ground the game calls grass, not only its green spots, for thicker grass in places like Konschtat Highlands.
- **Shadows:** A new Sharp shadow fade setting under Advanced Settings sets how gradually the sharp shadows near you blend into the softer ones farther out.

### Changed
- **Shadows:** Sharp shadow range is back to 10 by default, and thin shadows past it should now soften gradually as you walk instead of popping in and out.
- **Mass Grass:** Dry grass now only grows outdoors.

### Fixed
- **Mass Grass:** Grass should no longer grow on the floor of the Windurst Mog House.

## 0.9.6

### New
- **Ink Lines:** Mass Grass can now have its own ink lines, with their own darkness, width, edge sensitivity and fade, under Ink lines > Grass (Grass darkness starts at Off).
- **/vana:** Every panel has a Reset button on its heading (click it twice) that puts everything in the panel back to its default, switches included.

### Changed
- **Addons:** VanaStore now loads on its own at start, like /vana, and stays off once you switch it off.

### Fixed
- **Shadows:** Shadows on snow and other rounded ground should no longer be cut off or vanish as the camera moves under a low sun, such as at dawn and dusk in Qufim Island, Xarcabard, the Dunes, Sarutabaruta and Windurst Woods.
- **/vana:** Resetting a panel should no longer leave switches such as Blend with the game's shading or Dry grass on, and Mass Grass's reset should now also clear its old hidden settings, such as the old tuft limit that thinned the grass.

## 0.9.5

### New
- **Mass Grass:** Grass now grows shorter toward where it ends and as it climbs toward its steepest slope, instead of stopping at full height in a line (along La Theine Plateau's hills, for one).
- **Mass Grass:** Grass wholly outside the camera's view is now skipped.
- **Mass Grass:** An eye on each Grass Library row shows that plant alone.
- **Addons:** Addons can now tell which server the game is on.

### Changed
- **Mass Grass:** How much grass is drawn now follows Range and Quantity alone.
- **Mass Grass:** Grass is now lit by its ground's slope as the game lights it, so it should match the ground through dawn, dusk and night.
- **Mass Grass:** Some zones now have their own path padding (none in Wajaom Woodlands), and the slider is locked there.
- **Mass Grass:** New defaults: 150 yalm range, full quantity, shorter grass, no flowers and dry grass off.
- **Water:** VanaCore's water is now off in 87 zones with no water of their own, such as the Promyvions, Castle Zvahl, Crawlers' Nest, Xarcabard and the battlefields.
- **/vana:** The Mass Grass panel is reworked: Range in tens of yalms, Quantity and Height as percents, and an Advanced Settings group.

### Fixed
- **Shadows:** La Theine Plateau's hills should no longer have dark squares down their slopes under a high sun.
- **Shadows:** Mass Grass should no longer look grey all over with shadows on and ambient occlusion off.
- **Shadows:** Shadows under and on trees should no longer flicker with Shadows from things off screen turned off (Wajaom Woodlands).
- **Water:** The water's glint should no longer come and go as the camera moves where the walls are unlit (Lower Jeuno).
- **Sliders:** With flat sliders on, the party list's HP, MP and TP bars should no longer be drawn as sliders.

## 0.9.4

### Changed
- **Water:** VanaCore's water is now also off in the Sea of Al'Taieu's zones (Al'Taieu, the Grand Palace of Hu'Xzoi, the Garden of Ru'Hmet and Empyreal Paradox), leaving the game's own water there.

### Fixed
- **Tooltips:** The chat logs should no longer drop down under an item's details when they're taller than 12 rows.

## 0.9.3

### Changed
- **/vana:** Client's About and VanaCore tabs are now one, Client > VanaCore, with the banner and the version at the top.
- **Water:** VanaCore's water is off in Alzadaal Undersea Ruins, leaving the game's own water there.

### Fixed
- **Rain:** Rain should no longer fall in indoor zones such as Monastic Cavern, Qulun Dome, the Boyahda Tree and the Temple of Uggalepih.
- **Mass Grass:** Beadeaux and Beadeaux [S] should have grass again.

## 0.9.1

### Changed
- **Rain:** VanaCore's rain now falls only in zones that have rain of their own, in the server's rain as in thunder.

### Fixed
- **Rain:** Phomiuna Aqueducts should no longer have rain indoors.
- **Water:** Selbina's ferry's lower deck should no longer be drawn as water, or leave patches of water behind as it sails.

### Misc
- **Source:** VanaCore's source is now on GitHub.
