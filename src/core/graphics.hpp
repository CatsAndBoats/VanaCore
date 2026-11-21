#pragma once

#include <windows.h>

#include <string>
#include <vector>

// [graphics] in vanacore.ini: the game drawn through VanaCore's own Direct3D 8 on Direct3D 12
// (src/gfx, ported from xi-on-mac), with its scene effects ([graphics.effects]), the interface kept
// at a fixed shape in a wider window, texture packs and the faster occlusion probe; and the 3D
// scene's aspect ratio, which works on either renderer.
namespace vanacore::graphics {

// Reads [graphics] and [graphics.effects]. Call from VanaCoreInstall, before overlay::install.
void install(const wchar_t* ini, const wchar_t* install_dir);

// Direct3DCreate8 for the game: VanaCore's IDirect3D8 when renderer=d3d12 and Direct3D 12 comes up, or
// renderer=d3d9 and Direct3D 9 does (d3d9.hpp), else nullptr (the game gets the real one). Called from
// the game's Direct3DCreate8.
void* direct3d_create();

// True once the game has VanaCore's Direct3D 8, on Direct3D 12 or 11 (everything below that needs it).
bool active();

// Each frame, first thing in Present: once the graphics card has been lost (a driver timeout, the driver restarting),
// on Direct3D 12 VanaCore makes its device again under the game and the game carries on (vanacore_gfx_device_recover:
// at most three times a session, five minutes apart; the player told in the chat). Where it can't (Direct3D 11, a fourth
// loss, no device to be had), the game is closed - addons and plugins unloaded, its window hidden, the player told why -
// with exit code kExitGraphicsCardLost. Nothing it draws can show again, and left running it sat frozen over the screen
// for hours. Lost on Direct3D 12, the settings file is set to renderer=d3d11 first, for the next start. The message is
// shown once for every game on the PC (a timeout stops them all); the others close without it.
void close_if_lost();

// True once the game has VanaCore's Direct3D 8 on Direct3D 9 (renderer=d3d9): the game's own drawing,
// with Present waiting for the display only with vsync=1.
bool d3d9_active();

// True while VanaCore's sun shadows are on and characters cast them (in place of the game's own) at every hour:
// renderer=d3d12 or d3d11 running, effects=1, [graphics.effects] sun above 0 (0.6 unless set), sun_casters not 2 (the
// environment alone) and, with shadows at night (sun_moon), sun_moon_casters not 2 either.
bool sun_shadows();

// True when renderer=d3d12 couldn't draw here this session (its shaders don't compile: Wine, Proton; or the card is below
// resource binding tier 2: a Mac under CrossOver) and Direct3D 11 took its place, vanacore.ini switched to d3d11 with it:
// the overlay says so at the start.
bool switched_to_d3d11();

// The /vana window's Render visual ([graphics.effects] debug) as the renderer has it while its scene effects run: the
// part of them it shows alone in place of the picture (1 occlusion, 2 fog, 3 bloom, 4 god rays, 5 shadows, 6 ink lines,
// 7 depth of field, 8 bounce light, 9 glow effects lighting, 10 zone lines), for the overlay to say so; 0 none.
int render_visual();

// [graphics] gameshadows=keep: the game's own shadows are left as its Config menu has them, whatever
// VanaCore draws (auto, the default, turns them off while VanaCore's sun shadows are on).
bool keep_game_shadows();

// Ashita's /fillmode (addons.cpp) and IProperties::SetD3DFillMode (compat/ashita/core.cpp): how the world's triangles are
// drawn, a D3DFILLMODE: 1 point, 2 wireframe, 3 solid (as the game draws them). The game's interface and VanaCore's own
// drawing stay solid. Not a setting: it holds till the game closes. set_fill_mode is false for a value that isn't one of
// the three, or without VanaCore's renderer (the game's own Direct3D 8, or Direct3D 9, draws as the game says); fill_mode
// is what was last set, 3 to start.
int fill_mode();
bool set_fill_mode(int mode);

// What the /vana window shows: the renderer asked for and the one running, and the graphics card.
struct Info {
    std::string renderer = "d3d8"; // vanacore.ini's: d3d8, d3d9, d3d11 or d3d12
    std::string running = "d3d8";  // what the game draws with (d3d8 when the one asked for didn't come up)
    bool active = false;           // VanaCore's own Direct3D 8 is running (on Direct3D 12 or 11)
    bool effects = false;          // ... and its scene effects can run (on either)
    std::string adapter;           // UTF-8; empty with the game's own Direct3D 8
    std::string driver;            // its driver, by its maker's number (NVIDIA's 616.92); empty until looked up (and with no card), Unknown when Windows has none
    std::string driver_version;    // ... Windows' number for it (32.0.16.1692)
    std::string driver_date;       // ... its own date, YYYY-MM-DD
    std::string driver_installed;  // ... when this PC got it, YYYY-MM-DD
    int driver_days = -1;          // ... days since then (-1 not known)
    std::string dlss;              // DLSS's state: off, starting, its sizes and frames, or why it isn't there
    std::string framegen;          // DLSS frame generation's: off, waiting, its frame rates, or why it isn't there
    bool dlss_card = false;        // DLSS can run on this card (an NVIDIA RTX 20 or later), known before it's on ...
    std::string dlss_card_why;     // ... or why not
    bool dlss5 = false;            // DLSS 5 can run here (an RTX 40 or later, its DLL, NGX), known before it's on ...
    std::string dlss5_why;         // ... or why not
    std::string fsr;               // AMD FSR's state, as dlss's (the version it runs: "Game 1280x720 -> FSR 4.1.1 ...")
    bool fsr_card = false;         // FSR can run here (DirectX 12, AMD's DLLs beside the helper), known before it's on ...
    std::string fsr_card_why;      // ... or why not
    std::string upscaler;          // the upscaler running or asked for: "dlss", "fsr", or "" with both off
    unsigned scene_width = 0;      // the 3D scene as it's drawn now (0 before the game has drawn one, or with no
    unsigned scene_height = 0;     // VanaCore renderer)
    bool dlss_sized = false;       // DLSS (or FSR) sizes the scene, not [graphics] background
    std::string stopped;           // "dlss" or "fsr": on in vanacore.ini, but stopped (its helper crashed) or not there
                                   // after all; "" otherwise. /vana shows its switch off; retry() tries it again
    bool d3d12_unsupported = false; // Direct3D 11 runs because Direct3D 12 can't draw here (its shaders don't compile:
                                    // Wine, Proton; or binding tier 1: a Mac under CrossOver), switched to this session
                                    // or before ([graphics] d3d12.unsupported)
};
Info info();

// "3D scene 4096x4096 (the launcher's background resolution)": the size it's drawn at now and what set it, for /vana
// stats; empty without VanaCore's renderer or before the game has drawn a scene.
std::string scene_words();

// What the picture is still waiting on, for the overlay's toast: DLSS or DLSS 5 starting (the scene drawn small and
// stretched meanwhile), frame generation starting, the effects' shaders compiling, the shadows' pipelines building.
// The title ("Compiling shaders"; empty when nothing is waited on), what it's at now under it ("Ambient occlusion";
// empty without), and how far it has come: done of total steps (total 0 without a count).
struct Waiting {
    std::wstring title;
    std::wstring detail;
    unsigned done = 0;
    unsigned total = 0;
    bool alert = false; // not a wait: something went wrong ("DLSS crashed"), shown a few seconds, done/total the time left
};
Waiting waiting();

// For timing (abtest.cpp): DLSS or its frame generation is still starting (the frames now aren't representative),
// and the frames shown for each of the game's (frame generation running: 2 to 4, else 1).
bool settling();
int shown_per_frame();

// NVIDIA's own DLSS indicator (its version, sizes and mode over the picture): a machine-wide setting of NVIDIA's,
// HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore ShowDlssIndicator (every game that runs DLSS shows it while it's
// set). Reading it needs no rights; setting it asks Windows for an administrator's (a UAC prompt), on a thread of its
// own. DLSS reads it when it starts: the next game start.
bool dlss_indicator();
void set_dlss_indicator(bool on);

// Applies vanacore.ini's live settings now, without logging (the /vana window just wrote them).
void reload();

// DLSS (or FSR) that stopped or wasn't there (Info::stopped) tried again, as switching it off and on does, without
// vanacore.ini changing (/vana's switch turned back on).
void retry();

// The scene effects' shaders compiled again from the end of this frame (Client > VanaCore > Recompile shaders): every one
// let go and built over at the next scene, the overlay's toast showing it; the effects are off meanwhile. True when the
// renderer took it, false without VanaCore's renderer.
bool recompile_effects();

// The debug window's DLSS crash tests (/vana debug): the DLSS helper fails at its next frame as `kind` says (1 an access
// violation, 2 one on another thread, 3 an unhandled C++ exception, 4 a fail-fast, 5 a hang), to try what a real crash
// leaves: the chat's line, its report in vanacore.log, the dump. False, and why, with no helper running to ask.
bool dlss_crash_test(int kind, std::string& why);

// true around VanaCore's own drawing (its overlay, addons' boxes, plugins' drawing): not squeezed for
// [ui] aspect, so it's where the mouse finds it. false again after.
void own_drawing(bool on);

// true around VanaCore's own interface (its label, FPS counter, addons' boxes, the /vana window): drawn
// into a layer of the window's size (ui_size), in its pixels, laid over the frame at Present, so it's
// sharp and square whatever size the game's frame is drawn at. false again after.
void own_pixels(bool on);

// Any thread (the mouse hook's). True when (x, y), in the game's pixels, is over Ashita's Minimap as the renderer last saw it
// drawn: a wheel turn there is the plugin's (it zooms its map), and the game doesn't get it to zoom its camera too. False
// with a renderer other than d3d11 or d3d12 (the draw isn't seen).
bool plugin_wheel_over(int x, int y);

// Around a plugin's drawing (compat/plugin_move.cpp), `slot` its own (0..31; -1 after): its screen-space draws onto the back
// buffer go x to ax + scale x and y to ay + scale y (Addons > Layout's move of its "Text and shapes"), and where it draws them
// is noted: mover_box, that slot's box in the last finished frame as it put it (x0, y0, x1, y1, the game's pixels), false
// when it drew nothing then or with a renderer that doesn't see draws (d3d9).
void set_mover(int slot, float ax, float ay, float scale);
bool mover_box(int slot, float box[4]);

// The size VanaCore's own interface is drawn at, with VanaCore's Direct3D 8: the window's; false with
// Windows' own Direct3D 8 (the game's back buffer size is all there is).
bool ui_size(unsigned& width, unsigned& height);

// [ui] aspect's shape for the game's interface (width over height) while it squeezes the interface toward the middle;
// 0 when it doesn't.
float ui_aspect();

// True when the renderer squeezed the game's interface, laid out `ui_width` x `ui_height` (its menu resolution), toward the
// middle this frame for that shape. It doesn't when the interface's own target ([ffxi] resolution.ui) is of another shape
// than the aspect's, whatever [ui] aspect says; text VanaCore puts over the game's windows (uispace::to_pixels) follows the
// squeeze only when this is true.
bool ui_squeezed(unsigned ui_width, unsigned ui_height);

// That interface as drawn this frame (BGRA, premultiplied alpha, width x height, top row first), for
// a screenshot with VanaCore's drawing; false when there's none.
bool ui_layer(unsigned& width, unsigned& height, std::vector<unsigned char>& pixels);

// Rectangles of the game's picture kept over everything drawn after them, with VanaCore's Direct3D 8 (raise.cpp): each
// raise_take copies one from the back buffer (in the game's pixels) at the game's scene end, 8 a frame at most; raise_put
// lays them back after VanaCore's own drawing and cuts their places out of the layer that drawing is in. raise_take is
// false with Windows' Direct3D 8 or Direct3D 9.
bool raise_take(int x, int y, int width, int height);
void raise_put();

// In the full-screen modes, the size the frame is drawn at; 0 x 0 for the window's.
void render_size(unsigned width, unsigned height);

// Every frame, on the game's thread, before the frame is presented: the 3D scene's aspect ratio,
// and the settings again when vanacore.ini changed.
void on_frame();

// A mouse message's lParam as the game should see it: with [ui] aspect, over the interface the point
// is the one the game drew there before its interface was squeezed toward the middle (across in a
// wider window, up and down in a taller one).
LPARAM mouse(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

} // namespace vanacore::graphics
