#include "camera.hpp"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>

#include "addons.hpp"
#include "binds.hpp"
#include "chat.hpp"
#include "game.hpp"
#include "gui.hpp"
#include "hideui.hpp"
#include "ini.hpp"
#include "log.hpp"
#include "packets.hpp"
#include "scan.hpp"
#include "window.hpp"

namespace vanacore::camera {
namespace {

// FFXiMain.dll's camera (FUN_1001ec70; YmCameraManager::Follow in the PS2 client) keeps itself, each 60th of a second, between 3 and 6
// units from the point it looks at: farther than 6 it's pulled in, nearer than 3 pushed out (and kept 3 out to the side, so it isn't over
// the character). The game's zoom (the wheel, its zoom keys; both keys at once go back to the default) doesn't move it: it changes the 3D
// scene's focal length, "scrz", the projection's 192 / tan(fov / 2), from 242 (77 degrees) to 900 (24), 350 (57.5) the default, kept in the
// game's option 0x49. Locked on to a target the camera's distance is worked out from the focal length instead (a longer one, the camera
// farther back), and the zoom doesn't read the wheel.
//
// zoom=distance: the zoom moves the camera. The code that reads the wheel and the keys jumps to zoom_free() instead, which changes a
// distance (6, the game's, unless [camera] distance says), and the camera's own rules are scaled by distance / 6: its 3 and 6, the 100 past
// which it's pulled in fast, the probe behind it for a wall, and the rates it's pulled in and pushed out at (each a share of its distance,
// so divided by the scale for the same speed), through floats of ours where the game reads its constants (shared with other code: the 6.0
// is also the zoom's speed, the 3.0 the aspect ratio's and the walking animations'). The camera is moved along its line to the point it
// looks at as the distance changes. Locked on, the focal length its distance comes from is the game's times the scale (locked_scrz()), and
// the wheel works there too. The vertical turning (a height per 60th of a second) and the locked-on turning (its speed times 6 / the
// camera's distance) are scaled with it, so the camera turns as fast at any distance.

// The zoom: mov al,[resetting]; test al,al; jz +67h; call scrz; fsubr [350]; fmul [0.25]; fstp [esp+10h];
// fld [-1]. It's kAfterZoom bytes long, to mov edx,[esi]: the free camera's code after it.
constexpr char kZoom[] = "A0 ?? ?? ?? ?? 84 C0 74 67 E8 ?? ?? ?? ?? D8 2D ?? ?? ?? ?? D8 0D ?? ?? ?? ?? D9 5C 24 10 D9 05";
constexpr std::ptrdiff_t kAfterZoom = 0x1FA;

// The game's functions the zoom calls, by where it calls them (from the zoom's start)
constexpr std::ptrdiff_t kCallScrz = 0x9;         // the focal length now: mov eax,[ymdb]; fld [eax+2F4h]
constexpr std::ptrdiff_t kCallKey = 0x78;         // a key or button of the game's (group, key, 4: held, -1)
constexpr std::ptrdiff_t kCallTicks = 0xC1;       // 60ths of a second since the last frame, at least 1
constexpr std::ptrdiff_t kCallClearWheel = 0x139; // the wheel's count back to 0
constexpr std::ptrdiff_t kCallMouse = 0x148;      // the mouse is the game's (not a controller's cursor)
constexpr std::ptrdiff_t kCallWheel = 0x155;      // the wheel's count: 2 a notch (+ away, - toward), 1 off a frame
constexpr std::ptrdiff_t kCallSetScrz = 0x1E1;    // the focal length to go to (the next frame's projection)
constexpr std::ptrdiff_t kCallSetConfig = 0x1F2;  // a game option (option, value), as its Config menu sets it

// The game's zoom speed: its focal length changes by the frame's 60ths times the shared 6.0 (fmul [6]), read at four places
// from the zoom's start (the zoom keys in and out, the wheel in and out: the operand, after D8 0D). [camera] zoom.speed points
// them at a float of ours (as many times 6); zoom=distance jumps past all four.
constexpr std::ptrdiff_t kRateSites[] = {0xC8, 0x118, 0x16E, 0x1B6};
constexpr float kGameRate = 6.0f;

// The locked-on camera's distance: call scrz; fmul [0.001]; ... (6.8 + a) * that, (8.2 + a) * that
constexpr std::ptrdiff_t kLockedOn = 0xC57;
// How far behind the camera it looks for a wall before pushing it out: push 3.1 (68 imm32)
constexpr std::ptrdiff_t kProbe = 0x27F;
constexpr std::uint32_t kGameProbe = 0x40466666; // 3.1f
// mov [controlled],ebx (89 1D): the camera was moved, so it doesn't put its height back for 10 frames
constexpr std::ptrdiff_t kControlled = 0x660;

constexpr std::uint8_t kScrzGetter[] = {0xA1, 0, 0, 0, 0, 0xD9, 0x80, 0xF4, 0x02, 0x00, 0x00, 0xC3}; // ymdb at +1
constexpr std::ptrdiff_t kCameraField = 0x50;  // ymdb: the camera
constexpr std::ptrdiff_t kScrzTarget = 0x2F8;  // ymdb: the focal length it's going to
constexpr std::ptrdiff_t kEye = 0x44, kAt = 0x50; // the camera: where it is, the point it looks at

// What each of the game's constants we take over is, as the camera has it at the game's distance
enum Value { Max, Min, Far, Pull, Push, VerticalPan, HorizontalPan, LockOnRange, kValues };
constexpr float kGame[kValues] = {6.0f, 3.0f, 100.0f, 0.012f, -0.125f, 0.10666667f, 0.03f, 4.0f};
constexpr bool kChecked[kValues] = {false, false, true, true, true, false, false, false}; // xicamera changes the others
// The 6 and the 3 are the game's shared constants, which other code reads too (the 3: the walking animations). xicamera sets them in place:
// loaded after the camera's code reads ours, it takes our distance for the game's 6 and puts its distance less (ours - 3) in the 3, which
// stops the characters' walking animations. While the zoom is ours they're kept at the game's values (keep_shared()).
constexpr Value kShared[] = {Max, Min};

// Where the game reads them: the instruction's operand, from the zoom's start, and the two bytes before it
struct Site {
    std::ptrdiff_t at;
    std::uint8_t op[2];
    Value value;
};
constexpr Site kSites[] = {
    {-0x740, {0xD8, 0x0D}, HorizontalPan}, // the turning, an angle (fmul [0.03]): [camera] turn
    {-0x6DC, {0xD9, 0x05}, Max},         // locked on: the turning times 6 / the camera's distance (fld [6])
    {-0x66D, {0xD8, 0x0D}, VerticalPan}, // the vertical turning, a height (fmul [0.1067])
    {-0x574, {0xD9, 0x05}, Max},         // the locked-on turning, by the right stick or the mouse
    {+0x257, {0xD8, 0x15}, Min},         // fcom [3]: nearer, with a wall behind, it isn't pushed out
    {+0x597, {0xD8, 0x1D}, Far},         // fcomp [100]: pulled in fast past it
    {+0x5B4, {0xD8, 0x25}, Far},         // fsub [100]
    {+0x5C6, {0xD8, 0x1D}, Max},         // fcomp [6]: pulled in past it
    {+0x5D9, {0xD8, 0x25}, Max},         // fsub [6]
    {+0x5E0, {0xD8, 0x0D}, Pull},        // fmul [0.012]: (distance - 6) times it, a share of the distance
    {+0x61D, {0xD8, 0x1D}, Min},         // fcomp [3]: pushed out nearer than it
    {+0x62A, {0xD9, 0x05}, Min},         // fld [3]
    {+0x639, {0xD8, 0x0D}, Push},        // fmul [-0.125]: (3 - distance) times it
    {+0x698, {0xD8, 0x1D}, Min},         // fcomp [3]: out to the side
    {+0x6AF, {0xD9, 0x05}, Min},         // fld [3]
    // locked on, the camera turns to keep the target in view once the angle between its line and yours to the target
    // passes atan(this / (its distance + yours to the target)): fdivr [4], shared with 29 others ([camera] battle.range)
    {+0xCA6, {0xD8, 0x3D}, LockOnRange},
};
constexpr std::size_t kSiteCount = sizeof kSites / sizeof kSites[0];

// The Home key: while the game's key 0x51 is down its camera code turns the camera round behind the character (camera
// mode 8: push -1; push 4; push 51h; push 3Fh; call key; ...; mov [mode],8). The key function finds a key's keyboard keys
// in a table, three to a key (mov ax,[ebx*2+table]); 0x51's is Home alone (80C7: DIK_HOME), set once in the file and
// never by the game (its setters only change other keys'). home.reset=0 clears it there.
constexpr char kBehind[] = "6A FF 6A 04 6A 51 6A 3F E8 ?? ?? ?? ?? 83 C4 10 84 C0 0F 84 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 08 00 00 00";
// The register that holds the key is the compiler's pick (ebx in CatsEyeXI's build, edx in retail's), so the index byte is
// left open; retail's also moves test ax,ax away from between the two, so the second alternative is the same code without it
constexpr char kKeyTable[] = "66 8B 04 ?? ?? ?? ?? ?? 66 85 C0 8D 34 ?? | 66 8B 04 ?? ?? ?? ?? ?? 8D 34 ?? ?? ?? ?? ??";
constexpr std::ptrdiff_t kBehindCall = 8;
constexpr int kBehindKey = 0x51;
constexpr std::uint16_t kHome = 0x80C7;

// view.reset: the controller's Change View button holds the game's key 0x51 down in place of its first-person view (gamepad.cpp
// hides the button from the game; the camera code's call above goes through behind_key()), so the camera turns round behind the
// character as Home has it. The game reads the key as held, so a press holds it down at least kViewHold, then, once the button's
// let go, until the camera's direction has kept within kViewMoved for kViewSettle (it has come round), for at most kViewTurn.
constexpr ULONGLONG kViewHold = 100;
constexpr ULONGLONG kViewSettle = 120;
constexpr ULONGLONG kViewTurn = 1500;
constexpr float kViewMoved = 0.99985f; // cos 1 degree
constexpr ULONGLONG kViewStale = 500;  // a controller not read for this long (unplugged) has let go
constexpr int kViewSources = 5;        // view_button()'s: XInput's 0-3, DirectInput

// offset.x, offset.y: the camera moved to its side and up or down, so the character (the point it turns round) sits that share of
// the screen right of and above its middle, as Guild Wars 2's camera position. The game builds the camera's view (FUN_10021A20,
// for the frame's drawing and for its own camera code: D3DXMatrixLookAt from its eye and the point it looks at, kept at the
// camera's +4 when it's a valid matrix). The copy into +4 goes through shift_view() first, which moves the camera along its own right and
// up (the view matrix's _41, _42), by a share of its distance, so the character keeps its place on screen at any zoom. The eye
// the game's camera code keeps (and turns round the character) isn't moved, though its checks against the zone are made on
// the moved one (camera_walls.h, kWallSites); what's drawn, the names
// over heads and VanaCore's effects all take the moved one from the view.
//   ...; call D3DXMatrixLookAt; lea eax,[esp+8Ch]; push eax; call valid; add esp,4; test al,al; je; lea ecx,[esp+8Ch]; push ecx;
//   lea ecx,[esi+4]; call copy; ...; lea eax,[esi+4]; pop edi; pop esi; pop ebx; add esp,0C0h; ret
constexpr char kView[] = "E8 ?? ?? ?? ?? 83 C4 04 84 C0 74 10 8D 8C 24 8C 00 00 00 51 8D 4E 04 E8 ?? ?? ?? ?? 8D 4C 24 4C E8 ?? ?? ?? ?? "
                         "8D 46 04 5F 5E 5B 81 C4 C0 00 00 00 C3";
constexpr std::ptrdiff_t kViewCopy = 23;
// The projection (its _11 and _22: how far across the screen a unit at a distance of 1 is): mov eax,[scene]; add eax,154h; ret
constexpr char kProjectionGetter[] = "A1 ?? ?? ?? ?? 05 54 01 00 00 C3";
constexpr std::ptrdiff_t kProjectionField = 0x154;
constexpr std::ptrdiff_t kViewChanged = 0xF4; // the camera: the frame's drawing takes its view again (else only once it moves)
// The offset eases out while the player hasn't the camera (an event's) or the view is first person. The function that takes the
// camera for an event clears the game's flag for it, after first person's: mov al,[first person]; ...; mov byte [player's],0; ret
constexpr char kCameraTaken[] = "B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 74 1A A0 ?? ?? ?? ?? 84 C0 74 11 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
                                "80 A0 B2 00 00 00 FB C6 05 ?? ?? ?? ?? 00 C3";
constexpr std::ptrdiff_t kFirstPerson = 15, kPlayerCamera = 42;
constexpr float kOffset = 45.0f;      // offset.x, offset.y: percent of the screen, either way (the /vana window's sliders stop short)
constexpr float kOffsetEase = 8.0f;   // the shift follows: 1 - e^-8 of the way a second

// walls: the camera as drawn kept on the character's side of walls, floors and ceilings. The game keeps its camera out of a wall only as it
// pushes it out from the character (the probe above); pulled back by the zoom, or dragged along by the character, it goes through one, and
// far out in a small room it's outside (the zone's backs and the sky in view). shift_view() casts the game's own collision ray (the camera
// code's: thiscall on [collision], from, to, hit; true and the hit point when it meets the zone) from the character's head (keep_out()) out
// to the camera as drawn, and on a hit pulls the view in along its own line to kWallGap short of it: at once, and back out eased as the way
// clears. The game's camera itself isn't moved. Only while the player has the camera (not an event's, not first person).
//   lea ecx,[esp+5Ch]; push edx; push eax; push ecx; mov ecx,[collision]; call ray; test al,al; je +9; mov byte [esp+17h],0
constexpr char kWallRay[] = "8D 4C 24 5C 52 50 51 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 09 C6 44 24 17 00";
constexpr std::ptrdiff_t kWallCollision = 9, kWallCall = 13;
constexpr float kWallGap = 0.3f;     // the camera this far short of the wall (the near plane's corners clear it)
constexpr float kWallNearest = 0.5f; // nor nearer the point it looks at than this
constexpr float kWallEase = 4.0f;    // back out as the way clears: 1 - e^-4 of the way a second
constexpr float kWallHead = 1.2f;    // the point the camera looks at over the character's feet when it looks at the character (the game's y points down)
constexpr float kWallHeadReach = 40.0f; // the character's place further from the point than this is a stale one (a zone's): the point is used

// lock: the camera goes with the character at once as it moves, without the game's give. Each frame the game's camera code eases the point
// it looks at a quarter of the way to the character (per 60th of a second) and leaves the camera itself where it is until the character
// gets 6 from it (then pulls it along) or 3 (pushes it away): a leash. The call that copies the point it looks at, before the easing, goes
// through lock_follow() first, which moves the point and the camera by as far as the character went since the last frame, up and down too,
// so the easing has nothing to catch up and the leash never pulls. (Left to the game's easing, the height tilted the camera on slopes and
// stairs: the point rose or fell toward the character while the camera kept its height.) Pushed in by a wall, the camera eases back out to
// the zoom's distance (6 with the game's zoom); the game's own wall check, later in the frame, keeps it on this side of one.
//   call camera; add eax,50h; lea ecx,[esp+5Ch]; push eax; push ecx; call copy; add esp,8; cmp bl,1; jne
constexpr char kLock[] = "E8 ?? ?? ?? ?? 83 C0 50 8D 4C 24 5C 50 51 E8 ?? ?? ?? ?? 83 C4 08 80 FB 01 75";
constexpr std::ptrdiff_t kLockCopy = 14;
// The character's place as the camera code keeps it, earlier in the frame: mov edx,[esi]; mov ecx,esi; call [edx+1BCh] (its
// position); push eax; push offset; call copy; mov eax,[esi]; add esp,8
constexpr char kLockActor[] = "8B 16 8B CE FF 92 BC 01 00 00 50 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 06 83 C4 08";
constexpr std::ptrdiff_t kLockActorAt = 12;
constexpr float kLockEase = 6.0f; // pushed in by a wall, the camera eases back out: 1 - e^-6 of the way a second
// nor past the wall that pushed it in: the game puts it 0.5 short of the wall (fadd [0.5]) once its ray from the point the camera
// looks at meets one; easing out through the wall had the game put it back each few frames, the camera shaking in and out
constexpr float kLockWallGap = 0.6f;

// behind: as the character moves, the camera swings round behind it (from lock_follow(), so the lock's at-once follow comes with
// it: the game's leash would turn the camera as it drags it, which reads as the player's turn). The way it's going (not its
// facing: the game moves the character the way the keys say from the camera), unless that's toward the camera (walking backward,
// down the screen). It holds off for behind.delay seconds after the camera was turned other than by the swing (the mouse, the
// stick, the keys, a wall it slid along) and while locked on, where the game turns it to keep the target in view. Not with free
// look on (/freelook), where the camera's yours to turn as the character walks.
constexpr float kBehindRate = 2.0f;    // the swing at full speed: 1 - e^-2 of the way a second, times behind.speed
constexpr float kBehindMoving = 0.5f;  // units a second: slower is standing
constexpr float kBehindFull = 4.0f;    // units a second: the swing's full rate from here (a walk swings it slower)
constexpr float kBehindAhead = -0.2f;  // the way it's going against the camera's (cos): less is toward the camera
constexpr float kBehindTurned = 0.002f; // radians since the last frame that weren't the swing's: the camera was turned
constexpr ULONGLONG kBehindStill = 300; // ms not moving: moving again is setting off, and behind.delay counts from it
constexpr float kBehindSpeed[2] = {0.1f, 10.0f};
constexpr float kBehindDelay[2] = {0.0f, 10.0f};

// mouse.edgeturn=0: the mouse's frame (FUN_10124700; the mouse object's, [0x104DFBDC]) turns the camera while the cursor is within a pixel
// of the window's edges ([0x105ECC14]: left, top, right, bottom). Past its key 0x8F and a mode check ([0x1062DACC]+0x44 == 2, FUN_1025C600)
// it compares the cursor with the edges: call; test al,al; jnz past them (75 54). jmp there instead (EB): no edge, no turn, no edge arrow.
//   push 8Fh; call key; add esp,4; test eax,eax; jnz; call; test al,al; jnz +54h; movsx eax,word [left]
constexpr char kEdge[] = "68 8F 00 00 00 E8 ?? ?? ?? ?? 83 C4 04 85 C0 0F 85 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 75 54 0F BF 05";
constexpr std::ptrdiff_t kEdgeJump = 0x1C;

// mouse.noleftdrag: the mouse frame's camera drag (FUN_10124CB0). It turns the camera while the right button (key 0x8C) is held;
// in the mouse mode where FUN_1025C600 is true it asks for the left button too (key 0x8B) when the right isn't held, and a left
// drag turns the camera as well. The mode check's je +1Fh skips the left button's check (the game's own, mode off); jmp there
// instead (EB) and the left button never turns the camera, the right still does.
//   mov bl,al; call mode; test al,al; je +1Fh; test bl,bl; jnz +15h; push -1; push 4; push 8Bh; push 3Eh; call key
constexpr char kLeftDrag[] = "8A D8 E8 ?? ?? ?? ?? 84 C0 74 1F 84 DB 75 15 6A FF 6A 04 68 8B 00 00 00 6A 3E";
constexpr std::ptrdiff_t kLeftDragJump = 9;
constexpr std::uint8_t kLeftDragGame[2] = {0x74, 0x1F}, kLeftDragOff[2] = {0xEB, 0x1F};

// /freelook: the game walks the character the way the keys say from the camera. FUN_100A77A0 (XiControlActor::FreeMove in the
// PS2 client) turns the keys' way by the camera's angle, atan2(-_33, _31) of its view (the camera's +0x24, +0x2C), and the
// character faces the way it walks, so turning the camera while walking turns the character. The turn on the spot (in
// FUN_100A6040, StepControl: a key down, the character not moving) reads the same. Both ask the camera's getter for the camera
// twice, then read the two: call camera; mov edi,eax; call camera; mov ecx,[edi+24h]; fld [eax+2Ch]. Free look sends the four
// calls through free_camera(), which gives a camera of ours holding the two as they were when free look came on (taken again
// after a zone), so the character keeps walking that way however the camera's turned. Standing still, the game never turns
// the character to the camera; first person keeps the game's (the character turns with the view there).
constexpr char kFreeLook[] = "E8 ?? ?? ?? ?? 8B F8 E8 ?? ?? ?? ?? 8B 4F 24 D9 40 2C";
constexpr std::ptrdiff_t kFreeCalls[] = {0, 7};
constexpr std::size_t kFreeSites = 2;  // the walk, the turn on the spot: 0x9CE apart
constexpr std::size_t kFreeReach = 0x4000; // the second this far past the first
constexpr std::ptrdiff_t kFreeX = 0x24, kFreeZ = 0x2C; // the camera's view: _31, _33
constexpr std::uint8_t kCameraGetter[] = {0xA1, 0, 0, 0, 0, 0x8B, 0x40, 0x50, 0xC3}; // mov eax,[scene]; mov eax,[eax+50h]; ret

constexpr float kGameDistance = 6.0f;
constexpr float kGameFov = 57.5f; // degrees, up and down: the game's default focal length, 350
// How far the settings go (the /vana window's sliders stop short; a value typed in goes on to these): fov in degrees (170 is
// a focal length of 17, the game's zoom 242 to 900), distance.max in units, turn and turn.vertical, battle.distance (times)
constexpr float kFov[2] = {5.0f, 170.0f};
constexpr float kFarthest = 500.0f;
constexpr float kTurn[2] = {0.01f, 20.0f};
constexpr float kBattleDistance[2] = {0.1f, 10.0f};
constexpr float kFovEase = 20.0f; // the field of view follows fov: 1 - e^-20 of the way a second
constexpr float kNotch = 1.15f;   // a notch of the wheel: 15% nearer or farther (to the power zoom.speed)
constexpr float kKeyRate = 0.9f;  // the zoom keys held: e^0.9, 2.5 times nearer or farther a second (times zoom.speed)
constexpr float kEase = 14.0f;    // the camera follows the zoom: 1 - e^-14 of the way a second
constexpr float kZoomSpeed[2] = {0.1f, 10.0f};
constexpr ULONGLONG kPause = 2000;  // ms without the camera (a zone, a cutscene, first person): put at the distance
constexpr ULONGLONG kRemember = 2000; // distance.remember: the zoom kept this long is written to the settings file

using FloatFn = float(__cdecl*)();
using KeyFn = bool(__cdecl*)(int group, int key, int how, int player);
using BoolFn = bool(__cdecl*)();
using IntFn = int(__cdecl*)();
using VoidFn = void(__cdecl*)();
using SetScrzFn = void(__cdecl*)(float scrz);
using SetConfigFn = int(__cdecl*)(int option, int value);

struct Game {
    std::uintptr_t zoom = 0;  // the zoom's code
    std::uintptr_t ymdb = 0;  // the address of the pointer to the game's 3D scene (its projection, its camera)
    std::int32_t* controlled = nullptr;
    FloatFn scrz = nullptr;
    FloatFn ticks = nullptr;
    KeyFn key = nullptr;
    BoolFn mouse = nullptr;
    IntFn wheel = nullptr;
    VoidFn clear_wheel = nullptr;
    SetScrzFn set_scrz = nullptr;
    SetConfigFn set_config = nullptr;
};

struct Settings {
    bool distance = false;  // zoom=distance
    float start = kGameDistance; // distance: the zoom's distance
    float nearest = 1.5f;   // distance.min
    float farthest = 30.0f; // distance.max
    float fov = kGameFov;   // fov: degrees, up and down, with zoom=distance; 0 where the game's zoom put it
    // What xicamera did besides the distance, with zoom=distance (xicamera is held off then)
    float turn = 1.0f;          // turn: the camera's turning speed, times the game's
    float turn_vertical = 1.0f; // turn.vertical: up and down, times the game's (which also follows the distance)
    float battle_distance = 1.0f; // battle.distance: the locked-on camera's distance, times the zoom's
    float battle_range = 4.0f;  // battle.range: how far the target can get round before the locked-on camera turns
    bool home_reset = false;    // home.reset: Home turns the camera behind the character, as the game has it
    float offset[2] = {};       // offset.x, offset.y: the character's place on screen, a share of it right of and above the middle
    bool lock = false;          // lock: the camera goes with the character at once
    bool view_reset = false;    // view.reset: the controller's Change View button turns the camera behind the character
    float zoom_speed = 1.0f;    // zoom.speed: the zoom's speed, either zoom, times the game's
    bool remember = false;      // distance.remember: the zoom's distance written back to distance as it changes
    bool edge_turn = true;      // mouse.edgeturn: the cursor at the window's edge turns the camera, as the game has it
    bool no_left_drag = true;   // mouse.noleftdrag: a left-button drag doesn't turn the camera
    bool behind = false;        // behind: the camera swings round behind the character as it moves
    float behind_speed = 1.0f;  // behind.speed: how fast, times kBehindRate
    float behind_delay = 1.5f;  // behind.delay: seconds after the camera was turned before it swings
    bool walls = true;          // walls: the camera as drawn kept out of walls, floors and ceilings
    bool free_look = false;     // freelook: the camera turns without turning the character as it walks (/freelook)
    bool fly_float = false;     // freecam.float: the free camera glides and turns with some lag
    float fly_lag = 0.35f;      // freecam.lag: seconds
};

// Only touched on the game's thread.
std::wstring g_ini;
Settings g_settings;
FILETIME g_ini_time{};
ULONGLONG g_next_check = 0;
bool g_searched = false;
bool g_found = false;
bool g_patched = false;
Game g_game;
std::uint8_t* g_stubs = nullptr;
std::uint8_t g_zoom_code[5] = {};      // the game's, where the jump to ours goes
std::uint8_t g_locked_code[5] = {};
std::uint32_t g_site_code[kSiteCount] = {}; // the game's operands
float g_values[kValues] = {};          // read by the game's code in place of its constants
float g_base[kValues] = {};            // the game's, as found before an addon could change them (else kGame)
float* g_shared[kValues] = {};         // the game's own constants (kShared's)
bool g_shared_said = false;            // the log said one was changed
float g_probe = 0.0f;                  // the probe's distance as written into the game's code

float g_target = kGameDistance; // the zoom's distance
float g_now = kGameDistance;    // the camera's, following it
ULONGLONG g_last_camera = 0;    // the camera's code last ran
float g_fov_scrz = 0.0f;        // fov's focal length; 0 the game's
ULONGLONG g_fov_settle = 0;     // when fov is said in the log and kept in the game's option 0x49 (0: done)
ULONGLONG g_fov_first = 0;      // when keep_fov() last ran from the frame, in first person (0: not in it)
bool g_release = false;         // xicamera to be let load again, once the game's code is back
bool g_wheel_searched = false;
bool g_home_searched = false;
std::uint16_t* g_home = nullptr; // the key table's Home for the game's key 0x51; nullptr: not found, home.reset does nothing
bool g_offset_searched = false;
std::uintptr_t g_view = 0;             // the call to the view's copy (kView + kViewCopy); 0: not found
std::uintptr_t g_view_copy = 0;        // where it called
std::uintptr_t g_scene = 0;            // the address of the pointer to the game's 3D scene (the projection's, the camera's)
std::uint8_t* g_offset_stub = nullptr;
bool g_offset_patched = false;
const std::uint8_t* g_first_person = nullptr; // the game's flags: nullptr if they weren't found (the offset always applies)
const std::uint8_t* g_player_camera = nullptr;
float g_shift[2] = {};                 // the offset as drawn, following g_settings.offset
LARGE_INTEGER g_shift_time{};          // when the view was last built
using WallRayFn = bool(__thiscall*)(void* collision, const float* from, const float* to, float* hit);
WallRayFn g_wall_ray = nullptr;        // the game's collision ray (kWallRay); nullptr: not found, walls does nothing
void* const* g_wall_collision = nullptr; // the pointer to the zone's collision it's called on
float g_wall_pull = 0.0f;              // walls: how far the view is pulled in, as drawn
bool g_wall_easing = false;            // it's easing back out (the view's taken again each frame till it's out)
// The camera trace: the last few seconds of what keep_out() and lock_follow() saw and did, written to the log (as a block, then live
// for a few seconds more) when the walls' pull jumps, the wall ray hits beside the point the camera looks at, lock's ease moves the
// camera far in a frame, or /camtrace asks. For a camera that goes wrong when the target changes.
struct TraceRow {
    ULONGLONG tick;
    wchar_t kind;       // 'W' keep_out(): the camera as drawn; 'L' lock_follow(): the game's, after it followed; 'E' lock's ease
    bool locked;        // locked on to a target
    float at[3], eye[3];
    float distance;     // from at to eye
    float hit;          // the wall ray's hit, from at (-1: none or not cast)
    float want, pull;   // W: where the pull's going, and as applied. L: the zoom's distance. E: the zoom's, and where the ease goes
};
constexpr unsigned kTraceRows = 512;
constexpr ULONGLONG kTraceBack = 8000;  // ms of rows a dump goes back
constexpr ULONGLONG kTraceLive = 4000;  // ms after a dump that rows are written as they come
constexpr ULONGLONG kTraceQuiet = 10000; // ms after a dump before another by itself
constexpr float kTraceJump = 1.5f;      // a pull or an ease's step this much in a frame is a jump
TraceRow g_trace[kTraceRows];
unsigned g_trace_next = 0;             // rows ever added (the next goes in g_trace[g_trace_next % kTraceRows])
ULONGLONG g_trace_origin = 0;          // when the last dump was (rows say how long before or after it)
ULONGLONG g_trace_live_until = 0;
ULONGLONG g_trace_quiet_until = 0;
std::uintptr_t g_lock_site = 0;        // the call to the copy of the point the camera looks at (kLock + kLockCopy); 0: not found
std::uintptr_t g_lock_copy = 0;        // where it called
const float* g_lock_actor = nullptr;   // the character's place, as the camera code keeps it
std::uint8_t* g_lock_stub = nullptr;
bool g_lock_patched = false;
float g_lock_last[3] = {};             // the character's place the last frame
LARGE_INTEGER g_lock_time{};           // when that was (0: not yet, or not the last frame: the next starts afresh)
bool g_lock_ran = false;               // lock_follow() ran since the last on_frame()
std::uintptr_t g_behind_site = 0;      // the camera code's call asking for key 0x51 (kBehind + kBehindCall); 0: not found
KeyFn g_behind_key = nullptr;          // where it called
bool g_behind_patched = false;
bool g_view_turning = false;           // behind_key() has key 0x51 down
ULONGLONG g_view_since = 0;            // the press of Change View the camera turns for (0: none)
ULONGLONG g_view_let_go = 0;           // when its button was let go (0: still held)
unsigned g_view_seen = 0;              // g_view_presses as view_turn() last took it
float g_view_dir[3] = {};              // the camera's direction from the point it looks at, when it last moved
ULONGLONG g_view_still = 0;            // since when it hasn't (0: not yet looked at)
// view_button()'s, from the controllers' reads (any thread): a bit per source
std::atomic<bool> g_view_on{false};             // view.reset, with the behind call ours
std::atomic<unsigned> g_view_hidden{0};         // the button is down, its press hidden from the game
std::atomic<unsigned> g_view_passed{0};         // the button is down, its press the game's
std::atomic<unsigned> g_view_presses{0};        // presses hidden, counted
std::atomic<ULONGLONG> g_view_read[kViewSources]{}; // when each source last said where its button is
std::uint32_t g_rate_code = 0;         // the game's operand at kRateSites (its 6.0); 0: not found, zoom.speed only with zoom=distance
float g_rate = kGameRate;              // read by the game's zoom in place of its 6.0
bool g_rate_patched = false;
ULONGLONG g_remember_at = 0;           // distance.remember: when to write the zoom's distance (0: nothing to write)
float g_remember_last = 0.0f;          // the zoom's distance as last seen
bool g_edge_searched = false;
std::uint8_t* g_edge = nullptr;        // the mouse frame's jump past the edges; nullptr: not found, mouse.edgeturn does nothing
bool g_left_drag_searched = false;
std::uint8_t* g_left_drag = nullptr;   // the left drag's jump past its start; nullptr: not found, mouse.noleftdrag does nothing
float g_behind_left = NAN;            // behind: the camera's angle round the character as the last frame left it (NAN: afresh)
ULONGLONG g_behind_turned = 0;         // when the camera was last turned other than by the swing, or the character set off
ULONGLONG g_behind_moving = 0;         // when the character was last seen moving (0: not since a pause)
bool g_locked_on = false;              // locked on to a target (on_frame)
bool g_behind_locked = false;          // behind, and locked on
bool g_free_searched = false;
std::uintptr_t g_free_site[kFreeSites] = {}; // the walk's and the turn's (kFreeLook); 0: not found, /freelook does nothing
std::uintptr_t g_free_getter = 0;      // the camera's getter they call
std::uint8_t* g_free_stub = nullptr;
bool g_free_patched = false;
bool g_free_on = false;                // /freelook
bool g_free_take = false;              // the camera's angle to be taken (on, and after a zone) at the next frame
float g_free_camera[16] = {};          // the camera free_camera() gives: the two the game reads at kFreeX, kFreeZ
// The zoom, offset, walls, lock, behind and view.reset go in only with a character in the world (on_frame): the title screen and
// the character select draw their scenes with the game's camera too, which ours pulled out, pushed aside and turned
bool g_world = false;                  // a character in the world as of the last frame
bool g_was_in_world = false;           // one has been: going out and in again (a zone, a logout) isn't said in the log

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

std::uintptr_t call_target(std::uintptr_t call)
{
    std::int32_t offset = 0;
    std::memcpy(&offset, reinterpret_cast<const void*>(call + 1), sizeof offset);
    return call + 5 + offset;
}

// jmp or call (E9, E8) from `at` to `to`
void branch(std::uint8_t* out, std::uint8_t opcode, std::uintptr_t at, std::uintptr_t to)
{
    out[0] = opcode;
    const auto offset = static_cast<std::int32_t>(to - (at + 5));
    std::memcpy(out + 1, &offset, sizeof offset);
}

std::uint8_t* scene()
{
    return g_game.ymdb != 0 ? *reinterpret_cast<std::uint8_t**>(g_game.ymdb) : nullptr;
}

// The game's rules at `scale` times its distance (scale = distance / 6), in the floats its code reads
void scale_rules(float scale)
{
    g_values[Max] = kGame[Max] * scale;
    g_values[Min] = kGame[Min] * scale;
    g_values[Far] = kGame[Far] * scale;
    g_values[Pull] = kGame[Pull] / scale;
    g_values[Push] = kGame[Push] / scale;
    g_values[VerticalPan] = g_base[VerticalPan] * scale * g_settings.turn_vertical;
    g_values[HorizontalPan] = g_base[HorizontalPan] * g_settings.turn;
    g_values[LockOnRange] = g_settings.battle_range * scale * g_settings.battle_distance; // the same angles at any distance
    const float probe = 3.1f * scale;
    if (g_patched && probe != g_probe && write_code(g_game.zoom + kProbe + 1, &probe, sizeof probe))
        g_probe = probe;
}

// The wheel and the zoom keys, as the game reads them for its zoom: the distance they ask for
void read_zoom(float ticks)
{
    const bool in = g_game.key(0x3F, 0x4F, 4, -1), out = g_game.key(0x3F, 0x50, 4, -1);
    if (in && out) {
        g_target = kGameDistance; // both: the game's own, as both keys put its zoom back
    } else if (in || out) {
        g_target *= std::exp((in ? -kKeyRate : kKeyRate) * g_settings.zoom_speed * ticks / 60.0f);
    } else if (g_game.mouse() && !hideui::mouse_over_window()) { // (over a menu, the wheel scrolls it and doesn't zoom)
        const int wheel = g_game.wheel();
        if (wheel != 0) {
            const int notches = (std::abs(wheel) + 1) / 2;
            g_target *= std::pow(kNotch, g_settings.zoom_speed * static_cast<float>(wheel > 0 ? -notches : notches));
            g_game.clear_wheel();
        }
    }
    if (g_target < g_settings.nearest)
        g_target = g_settings.nearest;
    if (g_target > g_settings.farthest)
        g_target = g_settings.farthest;
    if (g_target != g_remember_last) {
        g_remember_last = g_target;
        g_remember_at = GetTickCount64() + kRemember;
    }
}

// The camera's distance toward the zoom's, evenly in its ratio; true if it moved
bool follow(float ticks)
{
    if (g_now == g_target)
        return false;
    const float share = 1.0f - std::exp(-kEase * ticks / 60.0f);
    float next = std::exp(std::log(g_now) + (std::log(g_target) - std::log(g_now)) * share);
    if (std::fabs(next - g_target) < g_target * 0.002f)
        next = g_target;
    g_now = next;
    return true;
}

// The camera along its line to the point it looks at: `ratio` times as far, or (ratio < 0) at g_now
void move_camera(std::uint8_t* camera, float ratio)
{
    if (camera == nullptr)
        return;
    auto* eye = reinterpret_cast<float*>(camera + kEye);
    const auto* at = reinterpret_cast<const float*>(camera + kAt);
    if (ratio < 0) {
        const float x = eye[0] - at[0], y = eye[1] - at[1], z = eye[2] - at[2];
        const float length = std::sqrt(x * x + y * y + z * z);
        if (!(length > 0.01f))
            return;
        ratio = g_now / length;
    }
    for (int i = 0; i < 3; ++i)
        eye[i] = at[i] + (eye[i] - at[i]) * ratio;
    *g_game.controlled = 10;
}

// fov: the focal length kept while the player has the camera (an event's camera sets its own), eased to a new one
// (the /vana window's slider, as it's dragged; back from an event's) so the view widens and narrows smoothly
void keep_fov(float ticks)
{
    std::uint8_t* const ymdb = scene();
    if (!(g_fov_scrz > 0) || ymdb == nullptr)
        return;
    const float now = *reinterpret_cast<const float*>(ymdb + kScrzTarget);
    if (now == g_fov_scrz || !(now > 0))
        return;
    const float share = 1.0f - std::exp(-kFovEase * ticks / 60.0f);
    float next = std::exp(std::log(now) + (std::log(g_fov_scrz) - std::log(now)) * share);
    if (std::fabs(next - g_fov_scrz) < g_fov_scrz * 0.001f)
        next = g_fov_scrz;
    g_game.set_scrz(next);
}

// fov in first person: the game sets its own focal length (280) as the view turns first person, and the camera code that runs
// keep_fov() (the free camera's zoom, the lock-on's) doesn't run there, so it's kept from the frame while the player has the camera
void keep_fov_first_person(ULONGLONG now)
{
    const bool first = g_first_person != nullptr && *g_first_person != 0 && (g_player_camera == nullptr || *g_player_camera != 0);
    if (!first || g_game.set_scrz == nullptr) {
        g_fov_first = 0;
        return;
    }
    // the frame's 60ths of a second (one for the first frame, and after a pause)
    const ULONGLONG passed = g_fov_first != 0 ? now - g_fov_first : 0;
    g_fov_first = now;
    keep_fov(passed > 0 && passed <= 100 ? static_cast<float>(passed) * 0.06f : 1.0f);
}

// Where the game read its zoom, in the free camera: the camera's code, as the stub's this (edi)
void __cdecl zoom_free(std::uint8_t* camera)
{
    const ULONGLONG now = GetTickCount64();
    const bool back = now - g_last_camera > kPause;
    g_last_camera = now;
    const float ticks = g_game.ticks();
    read_zoom(ticks);
    const float before = g_now;
    if (back) {
        g_now = g_target;
        move_camera(camera, -1.0f);
    } else if (follow(ticks)) {
        move_camera(camera, g_now / before);
    }
    keep_fov(ticks);
    scale_rules(g_now / kGameDistance);
}

// Locked on: the focal length the camera's distance is worked out from, the game's times the zoom's scale
float __cdecl locked_scrz()
{
    g_last_camera = GetTickCount64();
    const float ticks = g_game.ticks();
    read_zoom(ticks);
    follow(ticks);
    keep_fov(ticks);
    scale_rules(g_now / kGameDistance);
    return g_game.scrz() * (g_now / kGameDistance) * g_settings.battle_distance;
}

bool find()
{
    const HMODULE module = GetModuleHandleW(L"FFXiMain.dll");
    if (module == nullptr)
        return false;
    g_searched = true;
    const std::uintptr_t zoom = scan::find(module, kZoom);
    const auto* code = reinterpret_cast<const std::uint8_t*>(zoom);
    bool ok = zoom != 0;
    for (const std::ptrdiff_t call : {kCallScrz, kCallKey, kCallTicks, kCallClearWheel, kCallMouse, kCallWheel,
                                      kCallSetScrz, kCallSetConfig, kLockedOn})
        ok = ok && code[call] == 0xE8;
    ok = ok && code[kProbe] == 0x68 && std::memcmp(code + kProbe + 1, &kGameProbe, 4) == 0 &&
         code[kControlled] == 0x89 && code[kControlled + 1] == 0x1D;
    // The locked-on camera reads the same focal length; the getter is mov eax,[ymdb]; fld [eax+2F4h]; ret
    const std::uintptr_t getter = ok ? call_target(zoom + kCallScrz) : 0;
    ok = ok && call_target(zoom + kLockedOn) == getter &&
         std::memcmp(reinterpret_cast<const void*>(getter), kScrzGetter, 1) == 0 &&
         std::memcmp(reinterpret_cast<const void*>(getter + 5), kScrzGetter + 5, sizeof kScrzGetter - 5) == 0;
    // Each constant read where the game's code is as it should be, the same one at each of its sites
    std::uint32_t constant[kValues] = {};
    for (std::size_t i = 0; i < kSiteCount && ok; ++i) {
        const Site& site = kSites[i];
        std::uint32_t operand = 0;
        std::memcpy(&operand, code + site.at, sizeof operand);
        ok = code[site.at - 2] == site.op[0] && code[site.at - 1] == site.op[1] &&
             (constant[site.value] == 0 || constant[site.value] == operand);
        constant[site.value] = operand;
        ok = ok && (!kChecked[site.value] ||
                    *reinterpret_cast<const float*>(static_cast<std::uintptr_t>(operand)) == kGame[site.value]);
    }
    if (!ok) {
        log::write(L"Camera: couldn't find the game's zoom in FFXiMain.dll; it stays the game's ([camera] zoom=distance)");
        return false;
    }
    g_game.zoom = zoom;
    // the zoom's speed: the same operand, the game's 6.0, at each of its four places
    std::uint32_t rate = 0;
    for (const std::ptrdiff_t at : kRateSites) {
        std::uint32_t operand = 0;
        std::memcpy(&operand, code + at, sizeof operand);
        if (code[at - 2] != 0xD8 || code[at - 1] != 0x0D || (rate != 0 && operand != rate)) {
            rate = 0;
            break;
        }
        rate = operand;
    }
    g_rate_code = rate;
    if (rate == 0)
        log::write(L"Camera: the game's zoom speed wasn't found; [camera] zoom.speed works with zoom=distance only");
    std::memcpy(&g_game.ymdb, reinterpret_cast<const void*>(getter + 1), sizeof g_game.ymdb);
    std::uintptr_t controlled = 0;
    std::memcpy(&controlled, code + kControlled + 2, sizeof controlled);
    g_game.controlled = reinterpret_cast<std::int32_t*>(controlled);
    g_game.scrz = reinterpret_cast<FloatFn>(getter);
    g_game.ticks = reinterpret_cast<FloatFn>(call_target(zoom + kCallTicks));
    g_game.key = reinterpret_cast<KeyFn>(call_target(zoom + kCallKey));
    g_game.mouse = reinterpret_cast<BoolFn>(call_target(zoom + kCallMouse));
    g_game.wheel = reinterpret_cast<IntFn>(call_target(zoom + kCallWheel));
    g_game.clear_wheel = reinterpret_cast<VoidFn>(call_target(zoom + kCallClearWheel));
    g_game.set_scrz = reinterpret_cast<SetScrzFn>(call_target(zoom + kCallSetScrz));
    g_game.set_config = reinterpret_cast<SetConfigFn>(call_target(zoom + kCallSetConfig));
    for (const Value value : kShared)
        g_shared[value] = reinterpret_cast<float*>(static_cast<std::uintptr_t>(constant[value]));
    // The turning speeds as the game has them (found at its title screen, before an addon can change them)
    for (int value = 0; value < kValues; ++value)
        g_base[value] = kGame[value];
    for (const Value value : {HorizontalPan, VerticalPan}) {
        const float found = *reinterpret_cast<const float*>(static_cast<std::uintptr_t>(constant[value]));
        if (found > kGame[value] * 0.5f && found < kGame[value] * 2.0f)
            g_base[value] = found;
    }
    return true;
}

// The wheel's two readers (the mouse message and the DirectInput one) add a notch to the game's wheel count only when no menu is current:
// mov ecx,[menus]; call IsMenuOpen; (movsx ecx,[esp+0Ch]); test al,al; jz adds. Engaged, the game keeps a menu (id 0) current, so the
// camera never saw the wheel. The DirectInput reader's call (kWheelCalls[0]) is replaced with xor eax,eax: a notch always counts, and that
// reader still hands it to the menu as well. The message reader's (kWheelCalls[1], the one with the movsx) hands a notch to the menu or to
// the camera, never both, so xor eax,eax there left every menu and list without the wheel; its call is to wheel_menu_open instead: a menu
// takes the notch when the pointer is over it (camera.cpp's read_zoom leaves a notch over a menu to it), the camera otherwise.
using MenuOpenFn = bool(__fastcall*)(void* menus);
MenuOpenFn g_menu_open = nullptr; // the game's IsMenuOpen

bool __fastcall wheel_menu_open(void* menus)
{
    return g_menu_open(menus) && hideui::mouse_over_window();
}
constexpr char kWheelCalls[2][80] = {"B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 8B 44 24 0C 8B 4C 24 08",
                                     "B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F BF 4C 24 0C 84 C0 74"};
constexpr std::uint8_t kMenuOpenCode[] = {0x8B, 0x41, 0x54, 0x85, 0xC0}; // mov eax,[ecx+54h]; test eax,eax

// The wheel always zooms, as the game does with no menu up (the inventory's list doesn't use it for the camera)
void patch_wheel()
{
    g_wheel_searched = true;
    const HMODULE module = GetModuleHandleW(L"FFXiMain.dll");
    if (module == nullptr)
        return;
    std::uintptr_t site[2] = {};
    std::uintptr_t menu_open = 0;
    bool ok = true;
    for (int i = 0; i < 2; ++i) {
        const std::uintptr_t found = scan::find(module, kWheelCalls[i]);
        ok = ok && found != 0;
        site[i] = found + 5;
        if (!ok)
            break;
        const std::uintptr_t target = call_target(site[i]);
        ok = ok && (menu_open == 0 || menu_open == target) &&
             std::memcmp(reinterpret_cast<const void*>(target), kMenuOpenCode, sizeof kMenuOpenCode) == 0;
        menu_open = target;
    }
    constexpr std::uint8_t kXor[5] = {0x31, 0xC0, 0x90, 0x90, 0x90};
    std::uint8_t jump[5] = {};
    if (ok) {
        g_menu_open = reinterpret_cast<MenuOpenFn>(menu_open);
        branch(jump, 0xE8, site[1], reinterpret_cast<std::uintptr_t>(&wheel_menu_open));
        ok = write_code(site[0], kXor, sizeof kXor) && write_code(site[1], jump, sizeof jump);
    }
    if (ok)
        log::write(L"Camera: the mouse wheel zooms with a menu up too, and scrolls the menu under the pointer (the game's menu "
                   L"check at FFXiMain.dll+0x%X)",
                   static_cast<unsigned>(menu_open - reinterpret_cast<std::uintptr_t>(module)));
    else
        log::write(L"Camera: the game's wheel code wasn't found; the wheel still doesn't zoom while engaged");
}

// The key table's entry that gives the game's key 0x51 (the camera behind the character) Home
void find_home()
{
    g_home_searched = true;
    const HMODULE module = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t behind = module != nullptr ? scan::find(module, kBehind) : 0;
    const std::uintptr_t key = behind != 0 ? call_target(behind + kBehindCall) : 0;
    if (key != 0) {
        g_behind_site = behind + kBehindCall;
        g_behind_key = reinterpret_cast<KeyFn>(key);
    } else {
        log::write(L"Camera: the game's camera behind the character wasn't found; [camera] view.reset does nothing");
    }
    const std::uintptr_t lookup = key != 0 ? scan::find(key, 0x200, kKeyTable) : 0;
    std::uintptr_t table = 0;
    if (lookup != 0)
        std::memcpy(&table, reinterpret_cast<const void*>(lookup + 4), sizeof table);
    auto* const entry = table != 0 ? reinterpret_cast<std::uint16_t*>(table) + kBehindKey * 3 : nullptr;
    if (entry == nullptr || *entry != kHome) {
        log::write(L"Camera: the game's Home key for the camera wasn't found; [camera] home.reset does nothing");
        return;
    }
    g_home = entry;
}

// Home in the key table, or not, as home.reset says
void apply_home()
{
    const std::uint16_t want = g_settings.home_reset ? kHome : 0;
    if (g_home == nullptr || *g_home == want)
        return;
    if (!write_code(reinterpret_cast<std::uintptr_t>(g_home), &want, sizeof want))
        return;
    log::write(want != 0 ? L"Camera: Home turns the camera behind your character, as the game has it ([camera] home.reset=1)"
                         : L"Camera: Home leaves the camera alone ([camera] home.reset=0)");
}

// The game's zoom reading our speed (zoom.speed times its 6.0) while it isn't 1, its own again at 1
void apply_rate()
{
    g_rate = kGameRate * g_settings.zoom_speed;
    const bool want = g_settings.zoom_speed != 1.0f;
    if (g_rate_code == 0 || want == g_rate_patched)
        return;
    const auto ours = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&g_rate));
    bool ok = true;
    for (const std::ptrdiff_t at : kRateSites)
        ok &= write_code(g_game.zoom + at, want ? &ours : &g_rate_code, sizeof ours);
    if (!ok) {
        log::write(L"Camera: couldn't change the game's zoom speed; [camera] zoom.speed works with zoom=distance only");
        g_rate_code = 0; // not tried again
        return;
    }
    g_rate_patched = want;
    log::write(want ? L"Camera: the zoom is %g times the game's speed ([camera] zoom.speed)"
                    : L"Camera: the zoom is the game's speed ([camera] zoom.speed=1)",
               g_settings.zoom_speed);
}

// distance.remember: the zoom's distance, once it has stayed put for kRemember, written to [camera] distance, where the next
// game's zoom starts
void remember_zoom(ULONGLONG now)
{
    if (g_remember_at == 0 || now < g_remember_at)
        return;
    g_remember_at = 0;
    if (!g_settings.remember || !g_settings.distance || std::fabs(g_target - g_settings.start) < 0.005f)
        return;
    wchar_t text[32];
    std::swprintf(text, 32, L"%.2f", g_target);
    if (ini::set(g_ini, L"camera", L"distance", text))
        g_settings.start = std::wcstof(text, nullptr); // read back as this: not a change of the setting's (read_settings)
}

// The mouse frame's jump past the window's edges
void find_edge()
{
    g_edge_searched = true;
    const HMODULE module = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t edge = module != nullptr ? scan::find(module, kEdge) : 0;
    if (edge == 0) {
        log::write(L"Camera: the game's camera turn at the window's edge wasn't found; [camera] mouse.edgeturn does nothing");
        return;
    }
    g_edge = reinterpret_cast<std::uint8_t*>(edge + kEdgeJump);
}

// The cursor at the window's edge turning the camera, or not, as mouse.edgeturn says
void apply_edge()
{
    const std::uint8_t want = g_settings.edge_turn ? 0x75 : 0xEB; // jnz (the game's), jmp
    if (g_edge == nullptr || *g_edge == want)
        return;
    if (!write_code(reinterpret_cast<std::uintptr_t>(g_edge), &want, sizeof want))
        return;
    log::write(g_settings.edge_turn ? L"Camera: the mouse at the window's edge turns the camera, as the game has it ([camera] mouse.edgeturn=1)"
                                    : L"Camera: the mouse at the window's edge leaves the camera alone ([camera] mouse.edgeturn=0)");
}

void find_left_drag()
{
    g_left_drag_searched = true;
    const HMODULE module = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t found = module != nullptr ? scan::find(module, kLeftDrag) : 0;
    if (found == 0) {
        log::write(L"Camera: the game's left-button drag wasn't found; [camera] mouse.noleftdrag does nothing");
        return;
    }
    g_left_drag = reinterpret_cast<std::uint8_t*>(found + kLeftDragJump);
}

// A left-button drag moving the character and the camera, or not, as mouse.noleftdrag says
void apply_left_drag()
{
    const std::uint8_t* want = g_settings.no_left_drag ? kLeftDragOff : kLeftDragGame;
    if (g_left_drag == nullptr || std::memcmp(g_left_drag, want, sizeof kLeftDragOff) == 0)
        return;
    if (!write_code(reinterpret_cast<std::uintptr_t>(g_left_drag), want, sizeof kLeftDragOff))
        return;
    log::write(g_settings.no_left_drag ? L"Camera: dragging with the left button doesn't move the camera ([camera] mouse.noleftdrag=1)"
                                       : L"Camera: dragging with the left button moves the camera, as the game has it ([camera] mouse.noleftdrag=0)");
}

// The camera code's call asking whether the game's key 0x51 (the camera behind the character) is down, with view.reset: down too
// while the controller's Change View button turns the camera
bool __cdecl behind_key(int group, int key, int how, int player)
{
    const bool down = g_behind_key(group, key, how, player);
    return down || g_view_turning;
}

// The call asking for key 0x51 through behind_key(), or straight again
bool patch_behind(bool on)
{
    std::uint8_t call[5];
    branch(call, 0xE8, g_behind_site,
           on ? reinterpret_cast<std::uintptr_t>(&behind_key) : reinterpret_cast<std::uintptr_t>(g_behind_key));
    if (!write_code(g_behind_site, call, sizeof call))
        return false;
    g_behind_patched = on;
    g_view_on = on;
    g_view_turning = false;
    g_view_since = 0;
    return true;
}

// The offset the camera goes to: the setting's, none while an event has the camera or the view is first person
float offset_wanted(int axis)
{
    const bool player = (g_player_camera == nullptr || *g_player_camera != 0) && (g_first_person == nullptr || *g_first_person == 0);
    return player ? g_settings.offset[axis] : 0.0f;
}

// The game's camera (its eye, the point it looks at, its view at +4), or nullptr
std::uint8_t* offset_camera()
{
    std::uint8_t* const scene = g_scene != 0 ? *reinterpret_cast<std::uint8_t**>(g_scene) : nullptr;
    return scene != nullptr ? *reinterpret_cast<std::uint8_t**>(scene + kCameraField) : nullptr;
}

void trace_line(const TraceRow& row)
{
    log::write(L"Camera trace %lc t%+lldms at %.2f %.2f %.2f eye %.2f %.2f %.2f d %.2f hit %.2f want %.2f pull %.2f locked %d", row.kind,
               static_cast<long long>(row.tick) - static_cast<long long>(g_trace_origin), row.at[0], row.at[1], row.at[2], row.eye[0],
               row.eye[1], row.eye[2], row.distance, row.hit, row.want, row.pull, row.locked ? 1 : 0);
}

// The last kTraceBack ms of rows to the log, oldest first, and from then on the rows as they come for kTraceLive ms. By itself no
// more often than kTraceQuiet; `manual` (/camtrace) always.
void trace_dump(const wchar_t* why, bool manual)
{
    const ULONGLONG now = GetTickCount64();
    if (!manual && now < g_trace_quiet_until)
        return;
    g_trace_origin = now;
    g_trace_live_until = now + kTraceLive;
    g_trace_quiet_until = now + kTraceQuiet;
    const unsigned count = g_trace_next < kTraceRows ? g_trace_next : kTraceRows;
    log::write(L"Camera trace: %ls (the last %u rows within %llu ms, then the next %llu ms as they come)", why, count,
               static_cast<unsigned long long>(kTraceBack), static_cast<unsigned long long>(kTraceLive));
    for (unsigned i = g_trace_next - count; i != g_trace_next; ++i) {
        const TraceRow& row = g_trace[i % kTraceRows];
        if (now - row.tick <= kTraceBack)
            trace_line(row);
    }
}

void trace_add(TraceRow& row, const float* at, const float* eye, float distance, float hit, float want, float pull)
{
    row.tick = GetTickCount64();
    row.locked = g_locked_on;
    for (int i = 0; i < 3; ++i) {
        row.at[i] = at[i];
        row.eye[i] = eye[i];
    }
    row.distance = distance;
    row.hit = hit;
    row.want = want;
    row.pull = pull;
    g_trace[g_trace_next % kTraceRows] = row;
    ++g_trace_next;
    if (row.tick < g_trace_live_until)
        trace_line(row);
}

// /freecam: the camera detached from the character. The view the game builds each frame (shift_view() below) is replaced with one from a
// pose of ours, taken from the game's own view in the frame the free camera comes on and moved from there: W A S D along where it looks and
// across, E or Space up, Q or C down, Shift four times as fast, the wheel's notches the speed, the mouse (the right button held) or the
// arrow keys turning it. The game's camera code goes on beneath, following the character as ever, so turning the free camera off is the
// game's camera where it was. The keys it takes (takes_key(), asked by the game's DirectInput keyboard read) and the right button, the
// mouse's movement while it's held and the wheel (fly_mouse(), asked by the game window's procedure: the game makes no DirectInput mouse)
// are kept from the game: the character stands still, and the game's camera neither turns nor zooms. The view's axes are its columns: the
// game builds it with D3DXMatrixLookAt (z = the way it looks, or back from it for the right-handed one, x = up cross z, y = z cross x; _41
// to _43 the eye against them). The pose is the way it looks, the world's up (the vertical axis, signed by the game's own y, which always
// points up from it) and the eye. Taken from the game's view, then rebuilt and compared with it: a view of any other shape leaves the free
// camera off.
enum FlyAction { Forward, Back, Left, Right, Up, Down, TurnLeft, TurnRight, LookUp, LookDown, Still };
struct FlyKey {
    unsigned dik; // DirectInput's key code
    int vk;       // Windows'
    FlyAction action;
};
// Still: the numpad's arrows, which walk the character too; the arrows turn the camera
constexpr FlyKey kFlyKeys[] = {
    {0x11, 'W', Forward},   {0x1F, 'S', Back},       {0x1E, 'A', Left},       {0x20, 'D', Right},
    {0x12, 'E', Up},        {0x39, VK_SPACE, Up},    {0x10, 'Q', Down},       {0x2E, 'C', Down},
    {0xCB, VK_LEFT, TurnLeft}, {0xCD, VK_RIGHT, TurnRight}, {0xC8, VK_UP, LookUp}, {0xD0, VK_DOWN, LookDown},
    {0x4B, VK_NUMPAD4, Still}, {0x4D, VK_NUMPAD6, Still}, {0x48, VK_NUMPAD8, Still}, {0x50, VK_NUMPAD2, Still},
};
constexpr float kFlySpeed = 10.0f;        // units a second to start with
constexpr float kFlyFast = 4.0f;          // Shift
constexpr float kFlySpeeds[2] = {0.05f, 500.0f};
constexpr float kFlyNotch = 1.25f;        // a notch of the wheel: 25% faster or slower
constexpr float kFlyMouse = 0.0022f;      // radians a count of the mouse
constexpr float kFlyArrows = 1.6f;        // radians a second with the arrow keys
constexpr float kFlySteepest = 0.99985f;  // sin of the steepest it looks up or down (89 degrees)
constexpr float kFlyLag[2] = {0.05f, 3.0f}; // floaty: the seconds the camera takes to get most of the way (to 63%)
constexpr float kFlyLook = 0.7f;          // ... the turn's share of that
constexpr float kFlyMatch = 0.02f;       // how near the rebuilt view must be to the game's for the pose to be taken from it
constexpr int kFlyTries = 90;             // views that weren't the shape before it gives up

std::atomic<bool> g_fc_on{false};         // /freecam
std::atomic<long> g_fc_dx{0}, g_fc_dy{0}, g_fc_dz{0}; // the mouse's movement and wheel taken since the last frame
std::atomic<bool> g_fc_drag{false};       // the right button's held, its press kept from the game
POINT g_fc_cursor{};                      // where the cursor was when it went down
bool g_fc_cursor_ok = false;              // ... and was read: put back when the button's let go
POINT g_fc_center{};                      // where the cursor is held while it drags: where it went down (fly_drag)
POINT g_fc_rest{};                        // ... where it really is after it's put there (DPI scaling rounds it, the screen's edge
                                          // stops it): the movement counts from here, else the gap was a turn every message
bool g_fc_centered = false;               // ... and it's been put there
bool g_fc_have = false;                   // the pose is taken
bool g_fc_was = false;                    // it was on at the last frame
bool g_fc_exit = false;                   // just off: the game's view is built again
int g_fc_fails = 0;
float g_fc_speed = kFlySpeed;
float g_fc_velocity[3] = {};              // floaty: the camera's speed, units a second
float g_fc_pending[2] = {};               // floaty: the turn (yaw, tilt; radians) asked for and not yet made
LARGE_INTEGER g_fc_time{};
float g_fc_eye[3] = {};
float g_fc_fwd[3] = {};                   // the way it looks
float g_fc_up[3] = {};                    // the world's up
float g_fc_hand = 1.0f;                   // 1: the view's z is the way it looks (left-handed), -1: back from it
std::atomic<bool> g_fc_hold{false};       // an addon sets the pose (fly_set_pose()): the keys, the mouse and the wheel move nothing

float dot3(const float* a, const float* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void cross3(const float* a, const float* b, float* out)
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

bool unit3(float* v)
{
    const float length = std::sqrt(dot3(v, v));
    if (!(length > 1e-6f))
        return false;
    for (int i = 0; i < 3; ++i)
        v[i] /= length;
    return true;
}

// The view's three axes for the pose
bool fly_basis(float* x, float* y, float* z)
{
    for (int i = 0; i < 3; ++i)
        z[i] = g_fc_hand * g_fc_fwd[i];
    cross3(g_fc_up, z, x);
    if (!unit3(x))
        return false;
    cross3(z, x, y);
    return true;
}

// The pose from the game's view `m` (the camera's, `camera`); false if the game's view isn't one the pose makes
bool fly_take(const float* m, const std::uint8_t* camera)
{
    const float x[3] = {m[0], m[4], m[8]}, y[3] = {m[1], m[5], m[9]}, z[3] = {m[2], m[6], m[10]};
    // the game's up is a vertical axis: its x axis is level, its y axis points up from it (never level itself)
    if (!(std::fabs(x[1]) < kFlyMatch) || !(std::fabs(y[1]) > 0.05f))
        return false;
    float eye[3];
    for (int i = 0; i < 3; ++i)
        eye[i] = -(m[12] * m[i * 4] + m[13] * m[i * 4 + 1] + m[14] * m[i * 4 + 2]);
    const auto* from = reinterpret_cast<const float*>(camera + kEye);
    const auto* at = reinterpret_cast<const float*>(camera + kAt);
    const float look[3] = {at[0] - from[0], at[1] - from[1], at[2] - from[2]};
    const float hand = dot3(z, look) < 0.0f ? -1.0f : 1.0f;
    const float up[3] = {0.0f, y[1] > 0.0f ? 1.0f : -1.0f, 0.0f};
    float rebuilt[3][3];
    std::memcpy(g_fc_up, up, sizeof up);
    g_fc_hand = hand;
    for (int i = 0; i < 3; ++i)
        g_fc_fwd[i] = hand * z[i];
    if (!unit3(g_fc_fwd) || !fly_basis(rebuilt[0], rebuilt[1], rebuilt[2]))
        return false;
    const float* const game[3] = {x, y, z};
    for (int axis = 0; axis < 3; ++axis)
        for (int i = 0; i < 3; ++i)
            if (!(std::fabs(rebuilt[axis][i] - game[axis][i]) < kFlyMatch))
                return false;
    std::memcpy(g_fc_eye, eye, sizeof eye);
    return true;
}

// `m` the view of the pose
void fly_write(float* m)
{
    float x[3], y[3], z[3];
    if (!fly_basis(x, y, z))
        return;
    const float* const axes[3] = {x, y, z};
    for (int axis = 0; axis < 3; ++axis) {
        for (int i = 0; i < 3; ++i)
            m[i * 4 + axis] = axes[axis][i];
        m[12 + axis] = -dot3(axes[axis], g_fc_eye);
    }
}

// The free camera's view in place of the game's `m` (the camera's, `camera`); false when it isn't on, or the pose can't be taken
// from the game's view (drawn as it is)
bool fly_view(float* m, const std::uint8_t* camera)
{
    if (!g_fc_on.load())
        return false;
    if (!g_fc_have) {
        g_fc_have = fly_take(m, camera);
        if (!g_fc_have) {
            if (++g_fc_fails >= kFlyTries) {
                g_fc_on = false;
                log::write(L"Camera: the free camera is off: the game's view isn't one it can take its pose from");
            }
            return false;
        }
    }
    fly_write(m);
    return true;
}

// The pose turned: `yaw` radians to the right about the world's up, `tilt` up
void fly_turn(float yaw, float tilt)
{
    float x[3], y[3], z[3];
    if (!fly_basis(x, y, z))
        return;
    float* const f = g_fc_fwd;
    if (yaw != 0.0f) {
        // about up, toward the view's right (x = up cross z: the way the right-handed view's z points back is the one sign)
        const float angle = yaw * g_fc_hand, c = std::cos(angle), s = std::sin(angle);
        float side[3];
        cross3(g_fc_up, f, side);
        const float along = dot3(g_fc_up, f) * (1.0f - c);
        for (int i = 0; i < 3; ++i)
            f[i] = f[i] * c + side[i] * s + g_fc_up[i] * along;
        fly_basis(x, y, z);
    }
    if (tilt != 0.0f) {
        const float c = std::cos(tilt), s = std::sin(tilt);
        for (int i = 0; i < 3; ++i)
            f[i] = f[i] * c + y[i] * s;
    }
    if (!unit3(f))
        return;
    const float rise = dot3(f, g_fc_up);
    if (std::fabs(rise) > kFlySteepest) {
        float level[3];
        for (int i = 0; i < 3; ++i)
            level[i] = f[i] - g_fc_up[i] * rise;
        if (unit3(level)) {
            const float across = std::sqrt(1.0f - kFlySteepest * kFlySteepest), top = rise < 0.0f ? -kFlySteepest : kFlySteepest;
            for (int i = 0; i < 3; ++i)
                f[i] = level[i] * across + g_fc_up[i] * top;
        }
    }
}

// The mouse's movement while the right button is held (counts from where the cursor is held, turned into yaw and tilt by fly_step())
void fly_look(long dx, long dy)
{
    g_fc_dx += dx;
    g_fc_dy += dy;
}

// The wheel's turn (120 a notch), the camera's speed
void fly_wheel(long dz)
{
    g_fc_dz += dz;
}

// Where the cursor is held for a drag that began at `at`: there, kept kFlyHoldEdge pixels in from its monitor's edges (so the
// mouse still turns the camera that way). Not the window's middle: a mouse movement already on its way when the cursor was
// taken there (Windows had worked out where it ends, from where the button went down, while VanaCore's mouse hook held it)
// landed back by `at`, and the camera turned by all of the way between: the jump on the first click (2026-11-12, seen with a
// breakpoint on SetCursorPos: the free camera's were the only calls)
constexpr LONG kFlyHoldEdge = 32;
POINT fly_hold_point(POINT at)
{
    MONITORINFO monitor{};
    monitor.cbSize = sizeof monitor;
    if (!GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &monitor))
        return at;
    const RECT& r = monitor.rcMonitor;
    if (r.right - r.left > kFlyHoldEdge * 2)
        at.x = std::min(std::max(at.x, r.left + kFlyHoldEdge), r.right - 1 - kFlyHoldEdge);
    if (r.bottom - r.top > kFlyHoldEdge * 2)
        at.y = std::min(std::max(at.y, r.top + kFlyHoldEdge), r.bottom - 1 - kFlyHoldEdge);
    return at;
}

// The right button down or up. Down, the cursor is hidden at once and held where it went down (fly_hold_point: it's put back
// there after each movement, so the mouse turns the camera as far as it goes), and the window captures the mouse; up, it shows
// again there (the move makes Windows ask the window for its cursor), unless the game's gone to the background meanwhile
// (Alt+Tab): it's another program's cursor by then, and putting it back pulled it into the game's window
void fly_drag(bool down)
{
    if (g_fc_drag.exchange(down) == down)
        return;
    const HWND window = window::handle();
    if (down) {
        g_fc_cursor_ok = GetCursorPos(&g_fc_cursor) != FALSE;
        SetCursor(nullptr);
        g_fc_centered = false;
        if (window != nullptr && g_fc_cursor_ok) {
            const POINT hold = fly_hold_point(g_fc_cursor);
            g_fc_center = hold;
            g_fc_centered = true;
            SetCapture(window);
            if (hold.x != g_fc_cursor.x || hold.y != g_fc_cursor.y)
                SetCursorPos(hold.x, hold.y);
            if (!GetCursorPos(&g_fc_rest))
                g_fc_rest = hold;
        }
    } else {
        if (window != nullptr && GetCapture() == window)
            ReleaseCapture();
        if (g_fc_cursor_ok && !window::in_background())
            SetCursorPos(g_fc_cursor.x, g_fc_cursor.y);
        g_fc_cursor_ok = false;
    }
}

// The keys' answer to a press of the game's: typing in the chat line or an ImGui box, or the game in the background, leaves them
// the game's
bool fly_typing()
{
    if (window::in_background() || gui::wants_keyboard())
        return true;
    bool open = false;
    std::string line;
    return chat::input_line(open, line) && open;
}

// One frame of the free camera: the keys and mouse taken since the last, applied to the pose
void fly_step(float seconds)
{
    // a drag whose release never came (the window lost the mouse), or the game gone to the background with the button held
    // (Alt+Tab): it ends here
    if (g_fc_drag.load() && ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) == 0 || window::in_background()))
        fly_drag(false);
    const long dx = g_fc_dx.exchange(0), dy = g_fc_dy.exchange(0), dz = g_fc_dz.exchange(0);
    if (g_fc_hold.load()) { // what the mouse and keys asked meanwhile is dropped, so none of it is left to glide on after
        std::memset(g_fc_velocity, 0, sizeof g_fc_velocity);
        std::memset(g_fc_pending, 0, sizeof g_fc_pending);
        return;
    }
    if (dz != 0)
        g_fc_speed = std::min(kFlySpeeds[1], std::max(kFlySpeeds[0], g_fc_speed * std::pow(kFlyNotch, static_cast<float>(dz) / 120.0f)));
    float move[3] = {}; // right, up, forward
    float turn = static_cast<float>(dx) * kFlyMouse, tilt = -static_cast<float>(dy) * kFlyMouse;
    bool fast = false;
    if (!fly_typing() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0 && (GetAsyncKeyState(VK_MENU) & 0x8000) == 0) {
        fast = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        for (const FlyKey& key : kFlyKeys) {
            if ((GetAsyncKeyState(key.vk) & 0x8000) == 0)
                continue;
            switch (key.action) {
            case Forward: move[2] += 1.0f; break;
            case Back: move[2] -= 1.0f; break;
            case Right: move[0] += 1.0f; break;
            case Left: move[0] -= 1.0f; break;
            case Up: move[1] += 1.0f; break;
            case Down: move[1] -= 1.0f; break;
            case TurnRight: turn += kFlyArrows * seconds; break;
            case TurnLeft: turn -= kFlyArrows * seconds; break;
            case LookUp: tilt += kFlyArrows * seconds; break;
            case LookDown: tilt -= kFlyArrows * seconds; break;
            case Still: break;
            }
        }
    }
    // floaty: the turn asked for is eased in (what's left of it waits in g_fc_pending, so none of it is lost), and the camera
    // glides to the speed the keys ask for and on past them; not floaty, both are at once
    const bool floaty = g_settings.fly_float;
    const float lag = g_settings.fly_lag;
    g_fc_pending[0] += turn;
    g_fc_pending[1] += tilt;
    float share = floaty ? 1.0f - std::exp(-seconds / (lag * kFlyLook)) : 1.0f;
    float yaw = g_fc_pending[0] * share, lift = g_fc_pending[1] * share;
    if (std::fabs(g_fc_pending[0] - yaw) < 1e-5f && std::fabs(g_fc_pending[1] - lift) < 1e-5f) {
        yaw = g_fc_pending[0]; // (the last of it)
        lift = g_fc_pending[1];
    }
    g_fc_pending[0] -= yaw;
    g_fc_pending[1] -= lift;
    if (yaw != 0.0f || lift != 0.0f)
        fly_turn(yaw, lift);
    float x[3], y[3], z[3];
    if (!fly_basis(x, y, z))
        return;
    const float speed = g_fc_speed * (fast ? kFlyFast : 1.0f);
    share = floaty ? 1.0f - std::exp(-seconds / lag) : 1.0f;
    for (int i = 0; i < 3; ++i) {
        const float want = (x[i] * move[0] + g_fc_up[i] * move[1] + g_fc_fwd[i] * move[2]) * speed;
        g_fc_velocity[i] += (want - g_fc_velocity[i]) * share;
        g_fc_eye[i] += g_fc_velocity[i] * seconds;
    }
}

// Once a frame: the free camera stepped, or put away (off when asked, or with the character in the world gone: a zone)
void fly_frame(bool world)
{
    if (g_fc_on && (!world || g_view == 0)) {
        g_fc_on = false;
        g_fc_hold = false;
        log::write(world ? L"Camera: the free camera is off: the game's camera view can't be changed"
                         : L"Camera: the free camera is off (the character left the world)");
    }
    if (!g_fc_on) {
        if (g_fc_was) {
            g_fc_was = false;
            g_fc_have = false;
            g_fc_exit = true; // the game's view built again at once, not once the camera moves
            fly_drag(false);
        }
        return;
    }
    g_fc_was = true;
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    float seconds = g_fc_time.QuadPart != 0 ? static_cast<float>(static_cast<double>(now.QuadPart - g_fc_time.QuadPart) / frequency.QuadPart)
                                            : 0.0f;
    g_fc_time = now;
    if (seconds > 0.1f)
        seconds = 0.1f;
    if (!g_fc_have) { // the pose comes with the game's view
        g_fc_dx = 0;
        g_fc_dy = 0;
        g_fc_dz = 0;
        return;
    }
    fly_step(seconds);
    // the wheel and the mouse's reads the game still gets (the window's messages) are cleared, so it doesn't zoom
    if (g_found && g_game.clear_wheel != nullptr)
        g_game.clear_wheel();
}

#include "camera_walls.h" // wall_ray, keep_out, drawn_eye, find_walls, patch_walls: walls, and the game's checks on its camera with an offset

// The camera's view as the game built it, before its copy into the camera (`field`, the camera's +4): the camera moved along its
// own right and up by the offset, eased toward it, then kept out of walls; the free camera's pose instead, if it's on
void __cdecl shift_view(float* m, std::uint8_t* field)
{
    if (fly_view(m, field - 4))
        return;
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    float seconds = g_shift_time.QuadPart != 0
                        ? static_cast<float>(static_cast<double>(now.QuadPart - g_shift_time.QuadPart) / frequency.QuadPart)
                        : 0.0f;
    if (seconds > 0.25f)
        seconds = 0.25f;
    g_shift_time = now;
    const float share = 1.0f - std::exp(-kOffsetEase * seconds);
    for (int axis = 0; axis < 2; ++axis) {
        const float want = offset_wanted(axis);
        g_shift[axis] += (want - g_shift[axis]) * share;
        if (std::fabs(want - g_shift[axis]) < 0.0005f)
            g_shift[axis] = want;
    }
    const std::uint8_t* const scene = g_scene != 0 ? *reinterpret_cast<std::uint8_t* const*>(g_scene) : nullptr;
    if (scene == nullptr)
        return;
    const auto* projection = reinterpret_cast<const float*>(scene + kProjectionField);
    const auto* eye = reinterpret_cast<const float*>(field - 4 + kEye);
    const auto* at = reinterpret_cast<const float*>(field - 4 + kAt);
    for (int i = 0; i < 3; ++i)
        g_view_eye[i] = eye[i];
    const float x = eye[0] - at[0], y = eye[1] - at[1], z = eye[2] - at[2];
    const float distance = std::sqrt(x * x + y * y + z * z);
    if (!(distance > 0.01f))
        return;
    // the point d ahead (the character) is drawn t _11 / d across from the screen's middle (2 across) once the view moves it t to
    // the side (its _41: x after the view), so 2 shift d / _11 puts it at the offset whatever the distance; up the same with _22
    if ((g_shift[0] != 0 || g_shift[1] != 0) && projection[0] > 0 && projection[5] > 0) {
        m[12] += 2.0f * g_shift[0] * distance / projection[0];
        m[13] += 2.0f * g_shift[1] * distance / projection[5];
    }
    keep_out(m, field - 4, seconds);
}

// The calls offset and lock go through, the scene, and the game's camera flags
void find_offset()
{
    const HMODULE module = GetModuleHandleW(L"FFXiMain.dll");
    if (module == nullptr)
        return;
    g_offset_searched = true;
    const std::uintptr_t lock = scan::find(module, kLock);
    const std::uintptr_t actor = scan::find(module, kLockActor);
    if (lock != 0 && actor != 0) {
        g_lock_site = lock + kLockCopy;
        g_lock_copy = call_target(g_lock_site);
        std::uintptr_t place = 0;
        std::memcpy(&place, reinterpret_cast<const void*>(actor + kLockActorAt), sizeof place);
        g_lock_actor = reinterpret_cast<const float*>(place);
    } else {
        log::write(L"Camera: the game's camera follow wasn't found; [camera] lock does nothing");
    }
    const std::uintptr_t ray = scan::find(module, kWallRay);
    if (ray != 0) {
        g_wall_ray = reinterpret_cast<WallRayFn>(call_target(ray + kWallCall));
        std::uintptr_t collision = 0;
        std::memcpy(&collision, reinterpret_cast<const void*>(ray + kWallCollision), sizeof collision);
        g_wall_collision = reinterpret_cast<void* const*>(collision);
        find_walls(module);
    } else {
        log::write(L"Camera: the game's camera wall check wasn't found; [camera] walls does nothing");
    }
    const std::uintptr_t view = scan::find(module, kView);
    const std::uintptr_t getter = scan::find(module, kProjectionGetter);
    if (view == 0 || getter == 0) {
        log::write(L"Camera: the game's camera view wasn't found; [camera] offset.x, offset.y and walls do nothing");
        return;
    }
    g_view = view + kViewCopy;
    g_view_copy = call_target(g_view);
    std::memcpy(&g_scene, reinterpret_cast<const void*>(getter + 1), sizeof g_scene);
    const std::uintptr_t taken = scan::find(module, kCameraTaken);
    if (taken == 0) {
        log::write(L"Camera: the game's camera flags weren't found; [camera] offset stays in events and first person");
        return;
    }
    std::uintptr_t flag = 0;
    std::memcpy(&flag, reinterpret_cast<const void*>(taken + kFirstPerson), sizeof flag);
    g_first_person = reinterpret_cast<const std::uint8_t*>(flag);
    std::memcpy(&flag, reinterpret_cast<const void*>(taken + kPlayerCamera), sizeof flag);
    g_player_camera = reinterpret_cast<const std::uint8_t*>(flag);
}

// The view's copy called through shift_view() (push ecx; push ecx; push [esp+0Ch]; call shift_view; add esp,8; pop ecx;
// jmp copy: the copy is thiscall, the camera's +4 in ecx and the view on the stack), or straight again
bool patch_offset(bool on)
{
    if (on && g_offset_stub == nullptr) {
        g_offset_stub = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (g_offset_stub == nullptr)
            return false;
        std::uint8_t* const stub = g_offset_stub;
        const auto base = reinterpret_cast<std::uintptr_t>(stub);
        stub[0] = 0x51;
        stub[1] = 0x51;
        stub[2] = 0xFF, stub[3] = 0x74, stub[4] = 0x24, stub[5] = 0x0C;
        branch(stub + 6, 0xE8, base + 6, reinterpret_cast<std::uintptr_t>(&shift_view));
        stub[11] = 0x83, stub[12] = 0xC4, stub[13] = 0x08;
        stub[14] = 0x59;
        branch(stub + 15, 0xE9, base + 15, g_view_copy);
        FlushInstructionCache(GetCurrentProcess(), stub, 32);
    }
    std::uint8_t call[5];
    branch(call, 0xE8, g_view, on ? reinterpret_cast<std::uintptr_t>(g_offset_stub) : g_view_copy);
    if (!write_code(g_view, call, sizeof call))
        return false;
    g_offset_patched = on;
    patch_walls(on);
    // the shift's kept: back in the world (a zone, a login) the character's at its offset at once, not eased over from the middle
    g_shift_time.QuadPart = 0;
    g_wall_pull = 0.0f; // the last zone's wall
    g_wall_easing = false;
    return true;
}

// An angle from -pi to pi
float wrap(float angle)
{
    constexpr float kPi = 3.14159265f;
    while (angle > kPi)
        angle -= 2.0f * kPi;
    while (angle < -kPi)
        angle += 2.0f * kPi;
    return angle;
}

// behind: the camera (`eye`) swung round the point it looks at toward behind the way the character went this frame (`moved`, in
// `seconds`), about the up and down (the vertical is the second of the three), keeping its height and its distance across
void swing_behind(const float* at, float* eye, const float* moved, float seconds)
{
    const float x = eye[0] - at[0], z = eye[2] - at[2];
    const float across = std::sqrt(x * x + z * z);
    if (!(across > 0.01f)) {
        g_behind_left = NAN;
        return;
    }
    const float angle = std::atan2(z, x);
    const ULONGLONG now = GetTickCount64();
    // turned since the last frame, not by us: the player's (or the game's, locked on); waits behind.delay
    if (g_behind_locked || (!std::isnan(g_behind_left) && std::fabs(wrap(angle - g_behind_left)) > kBehindTurned))
        g_behind_turned = now;
    g_behind_left = angle;
    const float step = std::sqrt(moved[0] * moved[0] + moved[2] * moved[2]);
    const float speed = seconds > 0 ? step / seconds : 0.0f;
    // setting off (after standing kBehindStill, or a pause): behind.delay counts from here too, so the camera you turned while
    // standing stays put that long as you go
    if (speed >= kBehindMoving) {
        if (g_behind_moving == 0 || now - g_behind_moving > kBehindStill)
            g_behind_turned = now;
        g_behind_moving = now;
    }
    if (!g_settings.behind || g_free_on || speed < kBehindMoving ||
        now - g_behind_turned < static_cast<ULONGLONG>(g_settings.behind_delay * 1000.0f))
        return;
    // the way it went against the camera's own (from it to the point it looks at): toward the camera, left alone
    if (-(moved[0] * x + moved[2] * z) / (step * across) < kBehindAhead)
        return;
    const float want = std::atan2(-moved[2], -moved[0]);
    const float share = (1.0f - std::exp(-kBehindRate * g_settings.behind_speed * seconds)) *
                        (speed < kBehindFull ? speed / kBehindFull : 1.0f);
    const float next = angle + wrap(want - angle) * share;
    eye[0] = at[0] + across * std::cos(next);
    eye[2] = at[2] + across * std::sin(next);
    g_behind_left = next;
    if (g_game.controlled != nullptr)
        *g_game.controlled = 10;
}

// The point the camera looks at (`at`, the camera's +0x50; the camera itself at +0x44) before the game eases it toward the
// character: both moved as far as the character went since the last frame (its height too, so the camera's angle stays on a slope),
// and the camera back out to the zoom's distance if a wall pushed it in
void __cdecl lock_follow(float* at)
{
    float* const eye = at - 3;
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const float seconds = g_lock_time.QuadPart != 0
                              ? static_cast<float>(static_cast<double>(now.QuadPart - g_lock_time.QuadPart) / frequency.QuadPart)
                              : 1.0f;
    g_lock_time = now;
    g_lock_ran = true;
    const bool player = (g_player_camera == nullptr || *g_player_camera != 0) && (g_first_person == nullptr || *g_first_person == 0);
    float moved[3];
    for (int i = 0; i < 3; ++i) {
        moved[i] = g_lock_actor[i] - g_lock_last[i];
        g_lock_last[i] = g_lock_actor[i];
    }
    // not across a pause (a zone, an event's camera, first person) or a jump across the map
    if (!player || seconds > 0.5f || !(std::fabs(moved[0]) < 20.0f && std::fabs(moved[1]) < 20.0f && std::fabs(moved[2]) < 20.0f)) {
        g_behind_left = NAN;
        g_behind_moving = 0;
        return;
    }
    for (int i = 0; i < 3; ++i) {
        at[i] += moved[i];
        eye[i] += moved[i];
    }
    swing_behind(at, eye, moved, seconds);
    const float want = g_patched ? g_now : kGameDistance;
    const float x = eye[0] - at[0], y = eye[1] - at[1], z = eye[2] - at[2];
    const float distance = std::sqrt(x * x + y * y + z * z);
    {
        TraceRow row{};
        row.kind = L'L';
        trace_add(row, at, eye, distance, -1.0f, want, 0.0f);
    }
    // locked on, the game keeps the camera its own distance from the target's (shorter than the zoom's): easing out against it
    // each frame shook the camera in and out
    if (g_locked_on)
        return;
    if (!(distance > 0.01f) || !(distance < want * 0.99f))
        return;
    float next = distance + (want - distance) * (1.0f - std::exp(-kLockEase * seconds));
    float hit_distance = -1.0f;
    if (g_wall_ray != nullptr) {
        // the game's own ray, out to where the camera's going and a little past: a wall there stops it short. Along the camera as
        // drawn (with an offset, as the game's checks have it: camera_ray()), the hit as far along the game's line
        float to[3], hit[8] = {}, drawn[3];
        drawn_eye(at, eye, drawn);
        const float dx = drawn[0] - at[0], dy = drawn[1] - at[1], dz = drawn[2] - at[2];
        const float stretch = std::sqrt(dx * dx + dy * dy + dz * dz) / distance; // the drawn line's length against the game's
        const float reach = (next + kLockWallGap) / distance;
        for (int i = 0; i < 3; ++i)
            to[i] = at[i] + (drawn[i] - at[i]) * reach;
        if (wall_ray(at, to, hit) && stretch > 0) {
            const float hx = hit[0] - at[0], hy = hit[1] - at[1], hz = hit[2] - at[2];
            hit_distance = std::sqrt(hx * hx + hy * hy + hz * hz) / stretch;
            const float clear = hit_distance - kLockWallGap;
            if (clear < next)
                next = clear;
        }
        if (!(next > distance))
            return;
    }
    {
        TraceRow row{};
        row.kind = L'E';
        trace_add(row, at, eye, distance, hit_distance, want, next);
        if (next - distance > kTraceJump)
            trace_dump(L"lock moved the camera out far in a frame", false);
    }
    for (int i = 0; i < 3; ++i)
        eye[i] = at[i] + (eye[i] - at[i]) * (next / distance);
    if (g_game.controlled != nullptr)
        *g_game.controlled = 10; // moved: the game doesn't put its height back, or follow the character's path round a wall
}

// The copy of the point the camera looks at called through lock_follow() (push [esp+8]; call lock_follow; add esp,4; jmp copy),
// or straight again
bool patch_lock(bool on)
{
    if (on && g_lock_stub == nullptr) {
        g_lock_stub = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (g_lock_stub == nullptr)
            return false;
        std::uint8_t* const stub = g_lock_stub;
        const auto base = reinterpret_cast<std::uintptr_t>(stub);
        stub[0] = 0xFF, stub[1] = 0x74, stub[2] = 0x24, stub[3] = 0x08;
        branch(stub + 4, 0xE8, base + 4, reinterpret_cast<std::uintptr_t>(&lock_follow));
        stub[9] = 0x83, stub[10] = 0xC4, stub[11] = 0x04;
        branch(stub + 12, 0xE9, base + 12, g_lock_copy);
        FlushInstructionCache(GetCurrentProcess(), stub, 32);
    }
    std::uint8_t call[5];
    branch(call, 0xE8, g_lock_site, on ? reinterpret_cast<std::uintptr_t>(g_lock_stub) : g_lock_copy);
    if (!write_code(g_lock_site, call, sizeof call))
        return false;
    g_lock_patched = on;
    g_lock_time.QuadPart = 0;
    return true;
}

// The game's camera, as its getter has it (nullptr without a scene: the title screen)
std::uint8_t* game_camera()
{
    std::uintptr_t scene = 0;
    std::memcpy(&scene, reinterpret_cast<const void*>(g_free_getter + 1), sizeof scene);
    const auto* const ymdb = *reinterpret_cast<std::uint8_t* const*>(scene);
    return ymdb != nullptr ? *reinterpret_cast<std::uint8_t* const*>(ymdb + kCameraField) : nullptr;
}

// The walk's and the turn's calls for the camera (kFreeLook), each to the game's getter
void find_free()
{
    g_free_searched = true;
    const HMODULE module = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t first = module != nullptr ? scan::find(module, kFreeLook) : 0;
    const std::uintptr_t second = first != 0 ? scan::find(first + 1, kFreeReach, kFreeLook) : 0;
    const std::uintptr_t getter = first != 0 ? call_target(first) : 0;
    bool ok = second != 0 && std::memcmp(reinterpret_cast<const void*>(getter), kCameraGetter, 1) == 0 &&
              std::memcmp(reinterpret_cast<const void*>(getter + 5), kCameraGetter + 5, sizeof kCameraGetter - 5) == 0;
    for (const std::uintptr_t site : {first, second})
        for (const std::ptrdiff_t call : kFreeCalls)
            ok = ok && call_target(site + call) == getter;
    if (!ok) {
        log::write(L"Camera: the game's walking by the camera wasn't found; /freelook does nothing");
        return;
    }
    g_free_site[0] = first;
    g_free_site[1] = second;
    g_free_getter = getter;
}

// The walk's and the turn's camera: with free look on, ours (the camera's angle as it was), else the game's. First person, or
// before the angle's taken, the game's
std::uint8_t* __cdecl free_camera()
{
    std::uint8_t* const camera = game_camera();
    const bool first_person = g_first_person != nullptr && *g_first_person != 0;
    if (!g_free_on || g_free_take || camera == nullptr || first_person)
        return camera;
    return reinterpret_cast<std::uint8_t*>(g_free_camera);
}

// The four calls through free_camera() (push ecx; push edx; call free_camera; pop edx; pop ecx; ret), or to the getter again
bool patch_free(bool on)
{
    if (on && g_free_stub == nullptr) {
        g_free_stub = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (g_free_stub == nullptr)
            return false;
        std::uint8_t* const stub = g_free_stub;
        stub[0] = 0x51;
        stub[1] = 0x52;
        branch(stub + 2, 0xE8, reinterpret_cast<std::uintptr_t>(stub) + 2, reinterpret_cast<std::uintptr_t>(&free_camera));
        stub[7] = 0x5A;
        stub[8] = 0x59;
        stub[9] = 0xC3;
        FlushInstructionCache(GetCurrentProcess(), stub, 16);
    }
    bool ok = true;
    for (const std::uintptr_t site : g_free_site) {
        for (const std::ptrdiff_t at : kFreeCalls) {
            std::uint8_t call[5];
            branch(call, 0xE8, site + at, on ? reinterpret_cast<std::uintptr_t>(g_free_stub) : g_free_getter);
            ok &= write_code(site + at, call, sizeof call);
        }
    }
    g_free_patched = on;
    return ok;
}

// free look, once a frame: the camera's angle taken when it came on, and again once back from a zone (the game puts the camera
// behind the character there)
void free_frame()
{
    if (!g_free_on)
        return;
    if (packets::zoning(!game::local_player_name().empty())) {
        g_free_take = true;
        return;
    }
    if (!g_free_take)
        return;
    const std::uint8_t* const camera = game_camera();
    if (camera == nullptr)
        return;
    std::memcpy(&g_free_camera[kFreeX / 4], camera + kFreeX, sizeof(float));
    std::memcpy(&g_free_camera[kFreeZ / 4], camera + kFreeZ, sizeof(float));
    g_free_take = false;
}

// freelook: the walk's and the turn's camera ours or the game's, as it says
void apply_free()
{
    if (g_settings.free_look == g_free_on)
        return;
    if (!g_free_searched) {
        if (GetModuleHandleW(L"FFXiMain.dll") == nullptr)
            return;
        find_free();
    }
    if (g_free_getter == 0)
        return;
    const bool on = g_settings.free_look;
    if (on != g_free_patched && !patch_free(on)) {
        log::write(L"Camera: couldn't change the game's walking by the camera at FFXiMain.dll+0x%X; [camera] freelook does nothing",
                   static_cast<unsigned>(g_free_site[0] - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"FFXiMain.dll"))));
        patch_free(false);
        g_free_getter = 0; // not tried again
        g_free_on = false;
        return;
    }
    g_free_on = on;
    g_free_take = on;
    free_frame(); // the angle now, if there's a camera
    log::write(on ? L"Camera: free look: your character walks the way the camera was facing, however it's turned ([camera] freelook=1)"
                  : L"Camera: your character walks the way the camera faces, as the game has it ([camera] freelook=0)");
}

// view.reset, once a frame: whether behind_key() has key 0x51 down, for the presses of Change View view_button() hid
void view_turn(ULONGLONG now)
{
    // a controller that's stopped being read (unplugged) has let go
    for (int source = 0; source < kViewSources; ++source) {
        if (now > g_view_read[source].load(std::memory_order_relaxed) + kViewStale) {
            g_view_hidden.fetch_and(~(1u << source));
            g_view_passed.fetch_and(~(1u << source));
        }
    }
    const unsigned presses = g_view_presses.load();
    if (presses != g_view_seen) {
        g_view_seen = presses;
        g_view_since = now;
        g_view_let_go = 0;
        g_view_still = 0;
    }
    if (g_view_since == 0) {
        g_view_turning = false;
        return;
    }
    const bool held = g_view_hidden.load() != 0;
    g_view_let_go = held ? 0 : g_view_let_go != 0 ? g_view_let_go : now;
    // come round: the camera's direction from the point it looks at kept within kViewMoved for kViewSettle (without the camera,
    // the press holds the key kViewHold only)
    bool settled = true;
    if (const std::uint8_t* const camera = offset_camera()) {
        const auto* eye = reinterpret_cast<const float*>(camera + kEye);
        const auto* at = reinterpret_cast<const float*>(camera + kAt);
        float direction[3] = {eye[0] - at[0], eye[1] - at[1], eye[2] - at[2]};
        const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
        if (length > 0.01f) {
            for (float& part : direction)
                part /= length;
            const float same = direction[0] * g_view_dir[0] + direction[1] * g_view_dir[1] + direction[2] * g_view_dir[2];
            if (g_view_still == 0 || same < kViewMoved) {
                std::memcpy(g_view_dir, direction, sizeof g_view_dir);
                g_view_still = now;
            }
            settled = now - g_view_still >= kViewSettle;
        }
    }
    g_view_turning = held || now - g_view_since < kViewHold || (!settled && now - g_view_let_go < kViewTurn);
    if (!g_view_turning)
        g_view_since = 0;
}

// kShared's constants at the game's values (xicamera, loaded while the zoom is ours, sets them from ours)
void keep_shared()
{
    for (const Value value : kShared) {
        float* const shared = g_shared[value];
        if (shared == nullptr || *shared == kGame[value])
            continue;
        const float was = *shared;
        if (write_code(reinterpret_cast<std::uintptr_t>(shared), &kGame[value], sizeof kGame[value]) && !g_shared_said) {
            g_shared_said = true;
            log::write(L"Camera: the game's %g, which its walking animations read too, was %g (xicamera's? it sets the "
                       L"camera's distance there, and the zoom is VanaCore's: unload it); put back",
                       kGame[value], was);
        }
    }
}

// The stubs the game's code jumps to: the zoom (pushad; push edi; call zoom_free; add esp,4; popad; jmp after
// the zoom) and the locked-on focal length (push ecx; push edx; call locked_scrz; pop edx; pop ecx; ret).
bool make_stubs()
{
    if (g_stubs != nullptr)
        return true;
    g_stubs = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (g_stubs == nullptr)
        return false;
    std::uint8_t* zoom = g_stubs;
    const auto base = reinterpret_cast<std::uintptr_t>(zoom);
    zoom[0] = 0x60;
    zoom[1] = 0x57;
    branch(zoom + 2, 0xE8, base + 2, reinterpret_cast<std::uintptr_t>(&zoom_free));
    zoom[7] = 0x83, zoom[8] = 0xC4, zoom[9] = 0x04;
    zoom[10] = 0x61;
    branch(zoom + 11, 0xE9, base + 11, g_game.zoom + kAfterZoom);
    std::uint8_t* locked = g_stubs + 32;
    const auto at = reinterpret_cast<std::uintptr_t>(locked);
    locked[0] = 0x51;
    locked[1] = 0x52;
    branch(locked + 2, 0xE8, at + 2, reinterpret_cast<std::uintptr_t>(&locked_scrz));
    locked[7] = 0x5A;
    locked[8] = 0x59;
    locked[9] = 0xC3;
    FlushInstructionCache(GetCurrentProcess(), g_stubs, 64);
    return true;
}

bool patch(bool on)
{
    const std::uintptr_t zoom = g_game.zoom;
    bool ok = true;
    if (on) {
        if (!make_stubs())
            return false;
        std::memcpy(g_zoom_code, reinterpret_cast<const void*>(zoom), sizeof g_zoom_code);
        std::memcpy(g_locked_code, reinterpret_cast<const void*>(zoom + kLockedOn), sizeof g_locked_code);
        g_now = g_target;
        scale_rules(g_now / kGameDistance);
        for (std::size_t i = 0; i < kSiteCount; ++i) {
            std::memcpy(&g_site_code[i], reinterpret_cast<const void*>(zoom + kSites[i].at), sizeof g_site_code[i]);
            const auto ours = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&g_values[kSites[i].value]));
            ok &= write_code(zoom + kSites[i].at, &ours, sizeof ours);
        }
        std::uint8_t jump[5], call[5];
        branch(jump, 0xE9, zoom, reinterpret_cast<std::uintptr_t>(g_stubs));
        branch(call, 0xE8, zoom + kLockedOn, reinterpret_cast<std::uintptr_t>(g_stubs + 32));
        ok &= write_code(zoom + kLockedOn, call, sizeof call);
        ok &= write_code(zoom, jump, sizeof jump);
        g_patched = true;
        keep_shared(); // xicamera's, loaded while the zoom was the game's
        g_probe = 0.0f;
        scale_rules(g_now / kGameDistance); // the probe, now it's ours to write
        g_last_camera = 0; // the camera put at the distance the first time it runs
    } else {
        ok &= write_code(zoom, g_zoom_code, sizeof g_zoom_code);
        ok &= write_code(zoom + kLockedOn, g_locked_code, sizeof g_locked_code);
        for (std::size_t i = 0; i < kSiteCount; ++i)
            ok &= write_code(zoom + kSites[i].at, &g_site_code[i], sizeof g_site_code[i]);
        ok &= write_code(zoom + kProbe + 1, &kGameProbe, sizeof kGameProbe);
        g_patched = false;
    }
    return ok;
}

FILETIME ini_time()
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    GetFileAttributesExW(g_ini.c_str(), GetFileExInfoStandard, &data);
    return data.ftLastWriteTime;
}

std::wstring read(const wchar_t* key, const wchar_t* fallback)
{
    wchar_t value[64] = {};
    GetPrivateProfileStringW(L"camera", key, fallback, value, 64, g_ini.c_str());
    return value;
}

// A number from `low` to `high`, or `fallback` (said in the log when it isn't one)
float number(const wchar_t* key, float fallback, float low, float high)
{
    const std::wstring text = read(key, L"");
    if (text.empty())
        return fallback;
    wchar_t* end = nullptr;
    const float value = std::wcstof(text.c_str(), &end);
    if (end == text.c_str() || *end != L'\0' || !(value >= low && value <= high)) {
        log::write(L"Camera: %ls=%ls isn't a number from %g to %g; %g", key, text.c_str(), low, high, fallback);
        return fallback;
    }
    return value;
}

void read_settings(bool first)
{
    Settings next;
    // zoom=distance|game, or 1|0 as the /vana window's "Use True Zoom" writes it
    const std::wstring zoom = read(L"zoom", L"game");
    const wchar_t* const mode = zoom.c_str();
    next.distance = _wcsicmp(mode, L"distance") == 0 || _wcsicmp(mode, L"1") == 0 || _wcsicmp(mode, L"on") == 0;
    if (!next.distance && _wcsicmp(mode, L"game") != 0 && _wcsicmp(mode, L"0") != 0 && _wcsicmp(mode, L"off") != 0)
        log::write(L"Camera: zoom=%ls isn't distance or game; game", mode);
    // the /vana window's sliders stop short of these; a value typed into their boxes can go on to them, for fun
    next.nearest = number(L"distance.min", 1.5f, 0.5f, 50.0f);
    next.farthest = number(L"distance.max", 30.0f, 0.5f, kFarthest);
    if (next.farthest < next.nearest)
        next.farthest = next.nearest;
    next.start = number(L"distance", kGameDistance, 0.5f, kFarthest);
    next.turn = number(L"turn", 1.0f, kTurn[0], kTurn[1]);
    next.turn_vertical = number(L"turn.vertical", 1.0f, kTurn[0], kTurn[1]);
    next.battle_distance = number(L"battle.distance", 1.0f, kBattleDistance[0], kBattleDistance[1]);
    next.battle_range = number(L"battle.range", 4.0f, 0.0f, 100.0f);
    const std::wstring home = read(L"home.reset", L"0");
    next.home_reset = _wcsicmp(home.c_str(), L"1") == 0 || _wcsicmp(home.c_str(), L"on") == 0;
    const std::wstring lock = read(L"lock", L"0");
    next.lock = _wcsicmp(lock.c_str(), L"1") == 0 || _wcsicmp(lock.c_str(), L"on") == 0;
    if (next.lock != g_settings.lock)
        log::write(next.lock ? L"Camera: the camera goes with your character at once ([camera] lock=1)"
                             : L"Camera: the camera follows your character as the game has it ([camera] lock=0)");
    const std::wstring view = read(L"view.reset", L"0");
    next.view_reset = _wcsicmp(view.c_str(), L"1") == 0 || _wcsicmp(view.c_str(), L"on") == 0;
    if (next.view_reset != g_settings.view_reset)
        log::write(next.view_reset ? L"Camera: the controller's Change View button turns the camera round behind your character "
                                     L"([camera] view.reset=1)"
                                   : L"Camera: the controller's Change View button switches to first person, as the game has it "
                                     L"([camera] view.reset=0)");
    next.zoom_speed = number(L"zoom.speed", 1.0f, kZoomSpeed[0], kZoomSpeed[1]);
    const std::wstring remember = read(L"distance.remember", L"0");
    next.remember = _wcsicmp(remember.c_str(), L"1") == 0 || _wcsicmp(remember.c_str(), L"on") == 0;
    const std::wstring edge = read(L"mouse.edgeturn", L"1");
    next.edge_turn = !(_wcsicmp(edge.c_str(), L"0") == 0 || _wcsicmp(edge.c_str(), L"off") == 0);
    const std::wstring no_left_drag = read(L"mouse.noleftdrag", L"1");
    next.no_left_drag = !(_wcsicmp(no_left_drag.c_str(), L"0") == 0 || _wcsicmp(no_left_drag.c_str(), L"off") == 0);
    const std::wstring behind = read(L"behind", L"0");
    next.behind = _wcsicmp(behind.c_str(), L"1") == 0 || _wcsicmp(behind.c_str(), L"on") == 0;
    next.behind_speed = number(L"behind.speed", 1.0f, kBehindSpeed[0], kBehindSpeed[1]);
    next.behind_delay = number(L"behind.delay", 1.5f, kBehindDelay[0], kBehindDelay[1]);
    if (next.behind != g_settings.behind || (next.behind && (next.behind_speed != g_settings.behind_speed ||
                                                             next.behind_delay != g_settings.behind_delay)))
        log::write(next.behind ? L"Camera: the camera swings round behind your character as it moves, %g times the speed, %g s "
                                 L"after you turn it ([camera] behind=1)"
                               : L"Camera: the camera stays where you turn it as your character moves ([camera] behind=0)",
                   next.behind_speed, next.behind_delay);
    const std::wstring free_look = read(L"freelook", L"0");
    next.free_look = _wcsicmp(free_look.c_str(), L"1") == 0 || _wcsicmp(free_look.c_str(), L"on") == 0;
    const std::wstring fly_float = read(L"freecam.float", L"0");
    next.fly_float = _wcsicmp(fly_float.c_str(), L"1") == 0 || _wcsicmp(fly_float.c_str(), L"on") == 0;
    next.fly_lag = number(L"freecam.lag", 0.35f, kFlyLag[0], kFlyLag[1]);
    if (next.fly_float != g_settings.fly_float || (next.fly_float && next.fly_lag != g_settings.fly_lag))
        log::write(next.fly_float ? L"Camera: the free camera floats, %g s of lag ([camera] freecam.float=1)"
                                  : L"Camera: the free camera moves and turns at once ([camera] freecam.float=0)",
                   next.fly_lag);
    const std::wstring walls = read(L"walls", L"1");
    next.walls = !(_wcsicmp(walls.c_str(), L"0") == 0 || _wcsicmp(walls.c_str(), L"off") == 0);
    if (next.walls != g_settings.walls || (first && !next.walls))
        log::write(next.walls ? L"Camera: the camera is kept out of walls, floors and ceilings ([camera] walls=1)"
                              : L"Camera: the camera goes through walls as the game has it ([camera] walls=0)");
    next.offset[0] = number(L"offset.x", 0.0f, -kOffset, kOffset) / 100.0f;
    next.offset[1] = number(L"offset.y", 0.0f, -kOffset, kOffset) / 100.0f;
    if ((next.offset[0] != g_settings.offset[0] || next.offset[1] != g_settings.offset[1]) &&
        (!first || next.offset[0] != 0 || next.offset[1] != 0))
        log::write(L"Camera: the camera moves so your character is %g%% of the screen right of its middle and %g%% above it "
                   L"([camera] offset.x, offset.y)",
                   next.offset[0] * 100.0f, next.offset[1] * 100.0f);
    // fov: the game's default (57.5) unless set; game leaves it where the game's zoom put it
    const std::wstring fov = read(L"fov", L"");
    next.fov = _wcsicmp(fov.c_str(), L"game") == 0 ? 0.0f : number(L"fov", kGameFov, kFov[0], kFov[1]);
    // the distance in the file: where every game's zoom starts (with distance.remember, where the last left it); changed while
    // the game runs (the /vana window's slider, an edit), the zoom goes there too. remember_zoom()'s own write isn't a change
    if (first || next.start != g_settings.start) {
        g_target = next.start < next.nearest ? next.nearest : next.start > next.farthest ? next.farthest : next.start;
        g_remember_last = g_target;
        if (first)
            g_now = g_target;
    }
    // xicamera sets the same things (and the walking animations' constant with them): held off while the zoom is ours,
    // before the camera's code is ours (unloaded, it puts back the code it changed)
    if (first || next.distance != g_settings.distance) {
        if (next.distance)
            addons::hold("xicamera", "True Zoom is on, which moves the camera itself (/vana, World, Camera); it loads "
                                     "again when True Zoom is off");
        else if (!first)
            g_release = true; // once the game's code is back (on_frame), or it would find ours
    }
    if (first || next.distance != g_settings.distance || next.nearest != g_settings.nearest ||
        next.farthest != g_settings.farthest)
        log::write(next.distance ? L"Camera: the zoom moves the camera, %g to %g units away, now %g ([camera] zoom=distance)"
                                 : L"Camera: the zoom is the game's, the field of view ([camera] zoom=game)",
                   next.nearest, next.farthest, g_target);
    if (first || next.fov != g_settings.fov) {
        // the projection's height is 192 at the focal length's distance
        g_fov_scrz = next.fov > 0 ? 192.0f / std::tan(next.fov * 3.14159265f / 360.0f) : 0.0f;
        g_fov_settle = GetTickCount64() + (first ? 0 : 1000);
    }
    g_settings = next;
}

// fov said in the log and kept in the game's option 0x49 (which it goes back to after an event, and starts the next
// session with) once it has settled: not as the /vana window's slider is dragged
void settle_fov()
{
    if (g_fov_settle == 0 || GetTickCount64() < g_fov_settle)
        return;
    if (g_settings.fov > 0) {
        if (!g_patched || scene() == nullptr)
            return; // for when the zoom is ours
        g_game.set_config(0x49, static_cast<int>(std::lround(g_fov_scrz)));
        log::write(L"Camera: the field of view is %g degrees up and down with zoom=distance (the game's focal length %.0f; "
                   L"[camera] fov)",
                   g_settings.fov, g_fov_scrz);
    } else {
        log::write(L"Camera: the field of view stays where the game's zoom put it ([camera] fov=game)");
    }
    g_fov_settle = 0;
}

} // namespace

void install(const wchar_t* ini)
{
    g_ini = ini != nullptr ? ini : L"";
    if (g_ini.empty())
        return;
    g_ini_time = ini_time();
    read_settings(true);
}

void preview(const std::string& key, float value)
{
    const auto within = [value](float low, float high) { return value < low ? low : value > high ? high : value; };
    if (key == "fov") {
        g_settings.fov = within(kFov[0], kFov[1]);
        g_fov_scrz = 192.0f / std::tan(g_settings.fov * 3.14159265f / 360.0f);
        g_fov_settle = GetTickCount64() + 1000;
    } else if (key == "turn") {
        g_settings.turn = within(kTurn[0], kTurn[1]);
    } else if (key == "turn.vertical") {
        g_settings.turn_vertical = within(kTurn[0], kTurn[1]);
    } else if (key == "battle.distance") {
        g_settings.battle_distance = within(kBattleDistance[0], kBattleDistance[1]);
    } else if (key == "battle.range") {
        g_settings.battle_range = within(0.0f, 100.0f);
    } else if (key == "distance.max") {
        g_settings.farthest = within(g_settings.nearest, kFarthest); // the zoom's distance follows it in, if it's farther
    } else if (key == "distance") {
        g_target = within(g_settings.nearest, g_settings.farthest);
        g_remember_last = g_target;
    } else if (key == "zoom.speed") {
        g_settings.zoom_speed = within(kZoomSpeed[0], kZoomSpeed[1]);
    } else if (key == "freecam.lag") {
        g_settings.fly_lag = within(kFlyLag[0], kFlyLag[1]);
    } else if (key == "behind.speed") {
        g_settings.behind_speed = within(kBehindSpeed[0], kBehindSpeed[1]);
    } else if (key == "behind.delay") {
        g_settings.behind_delay = within(kBehindDelay[0], kBehindDelay[1]);
    } else if (key == "offset.x" || key == "offset.y") {
        g_settings.offset[key == "offset.x" ? 0 : 1] = within(-kOffset, kOffset) / 100.0f;
    }
}

bool view_button(int source, bool down)
{
    if (source < 0 || source >= kViewSources)
        return false;
    g_view_read[source].store(GetTickCount64(), std::memory_order_relaxed);
    const unsigned bit = 1u << source;
    if (!down) {
        g_view_passed.fetch_and(~bit);
        return (g_view_hidden.fetch_and(~bit) & bit) != 0;
    }
    if ((g_view_hidden.load() & bit) != 0)
        return true;
    if ((g_view_passed.load() & bit) != 0)
        return false;
    // a press: the game's while view.reset is off or the view is first person
    const bool first_person = g_first_person != nullptr && *g_first_person != 0;
    if (!g_view_on.load() || first_person) {
        g_view_passed.fetch_or(bit);
        return false;
    }
    g_view_hidden.fetch_or(bit);
    g_view_presses.fetch_add(1);
    return true;
}

bool fly_on()
{
    return g_fc_on.load();
}

FlyState fly_state()
{
    FlyState state;
    state.on = g_fc_on.load();
    state.have = state.on && g_fc_have;
    state.hold = state.on && g_fc_hold.load();
    state.floaty = g_settings.fly_float;
    state.lag = g_settings.fly_lag;
    state.speed = g_fc_speed;
    return state;
}

bool fly_pose(float* out)
{
    if (!g_fc_on.load() || !g_fc_have)
        return false;
    const float rise = std::max(-1.0f, std::min(1.0f, dot3(g_fc_fwd, g_fc_up)));
    float level[3];
    for (int i = 0; i < 3; ++i)
        level[i] = g_fc_fwd[i] - g_fc_up[i] * rise;
    std::memcpy(out, g_fc_eye, sizeof g_fc_eye);
    out[3] = std::atan2(level[2], level[0]); // the world's z over its x
    out[4] = std::asin(rise);
    return true;
}

bool fly_set_pose(const float* in)
{
    if (!g_fc_on.load() || !g_fc_have)
        return false;
    for (int i = 0; i < 5; ++i)
        if (!std::isfinite(in[i]))
            return false;
    const float limit = std::asin(kFlySteepest);
    const float tilt = std::max(-limit, std::min(limit, in[4]));
    const float across = std::cos(tilt), rise = std::sin(tilt);
    const float level[3] = {std::cos(in[3]), 0.0f, std::sin(in[3])}; // the world's x and z
    float forward[3];
    for (int i = 0; i < 3; ++i)
        forward[i] = level[i] * across + g_fc_up[i] * rise;
    if (!unit3(forward))
        return false;
    std::memcpy(g_fc_fwd, forward, sizeof forward);
    std::memcpy(g_fc_eye, in, sizeof g_fc_eye);
    std::memset(g_fc_velocity, 0, sizeof g_fc_velocity);
    std::memset(g_fc_pending, 0, sizeof g_fc_pending);
    return true;
}

void fly_hold(bool on)
{
    g_fc_hold = on && g_fc_on.load();
}

float fly_speed(float want)
{
    if (want > 0.0f)
        g_fc_speed = std::min(kFlySpeeds[1], std::max(kFlySpeeds[0], want));
    return g_fc_speed;
}

bool takes_key(unsigned dik, unsigned modifiers)
{
    if (!g_fc_on.load() || (modifiers & (binds::kCtrl | binds::kAlt)) != 0)
        return false;
    for (const FlyKey& key : kFlyKeys)
        if (key.dik == dik)
            return !fly_typing();
    return false;
}

bool takes_letter(unsigned dik, unsigned modifiers)
{
    for (const FlyKey& key : kFlyKeys)
        if (key.dik == dik && key.vk >= 'A' && key.vk <= 'Z')
            return takes_key(dik, modifiers);
    return false;
}

bool fly_dragging()
{
    return g_fc_on.load() && g_fc_drag.load();
}

bool fly_mouse(UINT message, WPARAM wparam)
{
    if (!g_fc_on.load())
        return false;
    switch (message) {
    case WM_MOUSEWHEEL:
        fly_wheel(GET_WHEEL_DELTA_WPARAM(wparam));
        return true;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        fly_drag(true);
        return true;
    case WM_RBUTTONUP:
        if (!g_fc_drag.load())
            return false; // the release of a press the game had, from before the free camera came on
        fly_drag(false);
        return true;
    case WM_MOUSEMOVE: {
        if (!g_fc_drag.load())
            return false;
        if (window::in_background()) { // (the game's window gone behind another's: the cursor is that one's now)
            fly_drag(false);
            return false;
        }
        // how far the cursor is from where it was put back now, not where this message says it was: messages that were queued
        // before the cursor was put back find it there and add nothing
        POINT now{};
        if (g_fc_centered && GetCursorPos(&now) && (now.x != g_fc_rest.x || now.y != g_fc_rest.y)) {
            fly_look(now.x - g_fc_rest.x, now.y - g_fc_rest.y);
            SetCursorPos(g_fc_center.x, g_fc_center.y);
            if (!GetCursorPos(&g_fc_rest))
                g_fc_rest = g_fc_center;
        }
        return true;
    }
    default:
        return false;
    }
}

int free_cam_float(int want)
{
    const bool on = want < 0 ? !g_settings.fly_float : want != 0;
    g_settings.fly_float = on;
    if (!ini::set(g_ini, L"camera", L"freecam.float", on ? L"1" : L"0"))
        log::write(L"Camera: couldn't write [camera] freecam.float; the free camera floats: %ls till the game closes", on ? L"yes" : L"no");
    return on ? 1 : 0;
}

int free_cam(int want)
{
    const bool on = want < 0 ? !g_fc_on.load() : want != 0;
    if (on == g_fc_on.load())
        return on ? 1 : 0;
    if (on) {
        if (!g_offset_searched)
            find_offset();
        if (g_view == 0)
            return -1;
        if (!g_world)
            return -2;
        g_fc_have = false;
        g_fc_fails = 0;
        g_fc_time.QuadPart = 0;
        std::memset(g_fc_velocity, 0, sizeof g_fc_velocity);
        std::memset(g_fc_pending, 0, sizeof g_fc_pending);
        fly_drag(false);
    }
    g_fc_hold = false;
    g_fc_on = on;
    log::write(on ? L"Camera: the free camera is on (/freecam)" : L"Camera: the free camera is off (/freecam)");
    return on ? 1 : 0;
}

void dump_trace()
{
    trace_dump(L"asked for with /camtrace", true);
}

int free_look(int want)
{
    const bool on = want < 0 ? !g_settings.free_look : want != 0;
    if (!g_free_searched)
        find_free();
    if (g_free_getter == 0)
        return -1;
    g_settings.free_look = on;
    if (!ini::set(g_ini, L"camera", L"freelook", on ? L"1" : L"0"))
        log::write(L"Camera: couldn't write [camera] freelook; free look is %ls till the game closes", on ? L"on" : L"off");
    apply_free();
    return g_free_on ? 1 : g_free_getter == 0 ? -1 : 0;
}

void on_frame()
{
    if (g_ini.empty())
        return;
    const ULONGLONG now = GetTickCount64();
    const FILETIME time = ini_time(); // each frame: a change shows at once
    if (CompareFileTime(&time, &g_ini_time) != 0) {
        g_ini_time = time;
        read_settings(false);
    }
    if (now >= g_next_check) {
        g_next_check = now + 100;
        if (g_patched)
            keep_shared();
        settle_fov();
        remember_zoom(now);
    }
    if (g_patched)
        keep_fov_first_person(now);
    if (!g_searched) // at the title screen, so the game's own turning speeds are seen before an addon changes them
        g_found = find();
    if (!g_wheel_searched)
        patch_wheel();
    if (!g_home_searched)
        find_home();
    apply_home();
    if (g_found)
        apply_rate();
    if (!g_edge_searched)
        find_edge();
    apply_edge();
    if (!g_left_drag_searched)
        find_left_drag();
    apply_left_drag();
    if (!g_offset_searched)
        find_offset();
    // a character in the world (g_world), else the game's own camera; going out and back in again is quiet in the log
    const bool world = !game::local_player_name().empty();
    const bool quiet = world != g_world && g_was_in_world;
    g_world = world;
    g_was_in_world = g_was_in_world || world;
    // patched while there's an offset, or one easing out, or walls (and its pull easing out)
    fly_frame(world);
    const bool offset = world && (g_fc_on.load() || g_settings.offset[0] != 0 || g_settings.offset[1] != 0 || g_shift[0] != 0 ||
                                  g_shift[1] != 0 || (g_wall_ray != nullptr && (g_settings.walls || g_wall_pull != 0)));
    if (g_view != 0 && offset != g_offset_patched) {
        const std::uintptr_t game = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"FFXiMain.dll"));
        if (!patch_offset(offset)) {
            log::write(L"Camera: couldn't change the game's camera view at FFXiMain.dll+0x%X; [camera] offset and walls do nothing",
                       static_cast<unsigned>(g_view - game));
            g_view = 0; // not tried again
        } else if (offset && !quiet) {
            log::write(L"Camera: the camera is moved by [camera] offset and walls through its view at FFXiMain.dll+0x%X",
                       static_cast<unsigned>(g_view - game));
        }
    }
    // behind goes through the lock's follow (and takes the camera along at once with it)
    const bool lock = world && (g_settings.lock || g_settings.behind);
    if (g_lock_site != 0 && lock != g_lock_patched) {
        const std::uintptr_t game = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"FFXiMain.dll"));
        if (!patch_lock(lock)) {
            log::write(L"Camera: couldn't change the game's camera follow at FFXiMain.dll+0x%X; [camera] lock does nothing",
                       static_cast<unsigned>(g_lock_site - game));
            g_lock_site = 0; // not tried again
        } else if (g_lock_patched && !quiet) {
            log::write(L"Camera: the camera follow at FFXiMain.dll+0x%X goes with your character at once",
                       static_cast<unsigned>(g_lock_site - game));
        }
    }
    const bool view_reset = world && g_settings.view_reset;
    if (g_behind_site != 0 && view_reset != g_behind_patched) {
        const std::uintptr_t game = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"FFXiMain.dll"));
        if (!patch_behind(view_reset)) {
            log::write(L"Camera: couldn't change the game's camera behind the character at FFXiMain.dll+0x%X; [camera] view.reset "
                       L"does nothing",
                       static_cast<unsigned>(g_behind_site - game));
            g_behind_site = 0; // not tried again
        } else if (g_behind_patched && !quiet) {
            log::write(L"Camera: the controller's Change View button turns the camera through the game's camera behind the "
                       L"character at FFXiMain.dll+0x%X",
                       static_cast<unsigned>(g_behind_site - game));
        }
    }
    if (g_behind_patched)
        view_turn(now);
    apply_free();
    free_frame();
    // a frame the camera code didn't run (first person, an event's camera, a zone): the character's place since is no step of
    // the camera's, so the next frame takes it afresh
    if (g_lock_patched) {
        if (!g_lock_ran) {
            g_lock_time.QuadPart = 0;
            g_behind_left = NAN;
            g_behind_moving = 0;
        }
        g_lock_ran = false;
    }
    // locked on, the game turns the camera to keep the target in view: no swing behind, and its distance is the game's
    game::Target target;
    g_locked_on = (g_settings.behind || g_lock_patched) && game::target(target) && target.locked;
    g_behind_locked = g_settings.behind && g_locked_on;
    // while the offset or the walls' pull eases, the frame's drawing takes the view each frame (it does only when the camera moves,
    // else)
    if (g_offset_patched && (g_shift[0] != offset_wanted(0) || g_shift[1] != offset_wanted(1) || g_wall_easing)) {
        if (std::uint8_t* const camera = offset_camera())
            camera[kViewChanged] = 1;
    }
    // the free camera moves every frame, and the game's view is built again at once when it's off (else the game builds it only
    // when its own camera moves, and ours would stay)
    if (g_fc_on.load() || g_fc_exit) {
        g_fc_exit = false;
        if (std::uint8_t* const camera = offset_camera())
            camera[kViewChanged] = 1;
    }
    const bool distance = world && g_settings.distance;
    if (g_found && distance != g_patched) {
        if (!patch(distance))
            log::write(L"Camera: couldn't change the game's camera code; [camera] zoom may be half applied");
        else if (!quiet)
            log::write(g_patched ? L"Camera: the game's zoom at FFXiMain.dll+0x%X moves the camera now"
                                 : L"Camera: the game's zoom at FFXiMain.dll+0x%X is its own again",
                       static_cast<unsigned>(g_game.zoom - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"FFXiMain.dll"))));
    }
    if (g_release && !g_patched) {
        g_release = false;
        addons::release("xicamera");
    }
}

int sight(const float* from, const float* to)
{
    float hit[8] = {};
    const int met = cast(from, to, hit);
    return met < 0 ? -1 : 1 - met;
}

int cast(const float* from, const float* to, float* hit)
{
    return g_wall_ray == nullptr || g_wall_collision == nullptr || *g_wall_collision == nullptr ? -1 : wall_ray(from, to, hit) ? 1 : 0;
}

} // namespace vanacore::camera
