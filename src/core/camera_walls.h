// [camera] walls, and the game's own checks of its camera with an offset (camera.cpp): wall_ray, keep_out (the view as
// drawn kept out of walls), drawn_eye (the camera as the offset draws it), and the game's checks against the zone made on
// that (kWallSites: camera_ray, floor_drawn, ceiling_drawn, slide_drawn; find_walls, patch_walls). camera.cpp includes this
// inside its anonymous namespace, after the free camera and the trace, before shift_view; it is no file of its own.
#pragma once

float g_view_eye[3] = {}; // the game's camera when its view was last built (shift_view())

// The game's collision ray from `from` to `to`: true and `hit` where it first meets the zone (false if it faulted: the zone's
// collision going away under it)
bool wall_ray(const float* from, const float* to, float* hit)
{
    __try {
        void* const collision = *g_wall_collision;
        return collision != nullptr && g_wall_ray(collision, from, to, hit);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// walls: the view `m` (the camera's, `camera`, as drawn: the offset's already in it) pulled in toward the point the camera looks
// at (the character), to short of the first wall between them, at once, and eased back out over `seconds` as the way clears. The
// camera keeps its angle: moved along the line from that point, the point stays where it was on screen, offset or not
void keep_out(float* m, const std::uint8_t* camera, float seconds)
{
    // cppcheck-suppress invalidPointerCast ; the game's structures hold floats at these offsets
    const float* at = reinterpret_cast<const float*>(camera + kAt);
    // Locked on, the point the camera looks at is half way to the target, which with the target up a slope is the slope's own
    // surface: a ray from there met it at once and pulled the camera in to it, under the ground. The ray goes from the character's
    // head (where the point is when it looks at the character) instead.
    float head[3];
    if (g_lock_actor != nullptr) {
        head[0] = g_lock_actor[0];
        head[1] = g_lock_actor[1] - kWallHead;
        head[2] = g_lock_actor[2];
        const float hx = head[0] - at[0], hy = head[1] - at[1], hz = head[2] - at[2];
        if (hx * hx + hy * hy + hz * hz < kWallHeadReach * kWallHeadReach)
            at = head;
    }
    // the view's axes are its columns, its _41 to _43 the camera's place against them: the camera as drawn
    float drawn[3], out[3];
    for (int i = 0; i < 3; ++i) {
        drawn[i] = -(m[12] * m[i * 4] + m[13] * m[i * 4 + 1] + m[14] * m[i * 4 + 2]);
        out[i] = drawn[i] - at[i];
    }
    const float distance = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (!(distance > 0.01f))
        return;
    for (float& part : out)
        part /= distance;
    float want = 0.0f, hit_distance = -1.0f;
    const bool player = (g_player_camera == nullptr || *g_player_camera != 0) && (g_first_person == nullptr || *g_first_person == 0);
    if (g_settings.walls && player && g_wall_ray != nullptr && distance > kWallNearest) {
        float to[3], hit[8] = {};
        for (int i = 0; i < 3; ++i)
            to[i] = drawn[i] + out[i] * kWallGap; // a wall just behind the camera counts too
        if (wall_ray(at, to, hit)) {
            const float hx = hit[0] - at[0], hy = hit[1] - at[1], hz = hit[2] - at[2];
            hit_distance = std::sqrt(hx * hx + hy * hy + hz * hz);
            want = distance - (hit_distance - kWallGap);
            if (want > distance - kWallNearest)
                want = distance - kWallNearest;
            if (want < 0)
                want = 0;
        }
    }
    const float was = g_wall_pull;
    if (want >= g_wall_pull) {
        g_wall_pull = want;
    } else {
        g_wall_pull += (want - g_wall_pull) * (1.0f - std::exp(-kWallEase * seconds));
        if (g_wall_pull - want < 0.01f)
            g_wall_pull = want;
    }
    g_wall_easing = g_wall_pull > want;
    {
        TraceRow row{};
        row.kind = L'W';
        trace_add(row, at, drawn, distance, hit_distance, want, std::fmax(0.0f, std::fmin(g_wall_pull, distance - kWallNearest)));
        if (g_wall_pull - was > kTraceJump)
            trace_dump(L"the walls' pull jumped", false);
        else if (hit_distance >= 0.0f && hit_distance < 1.0f && distance > 2.0f)
            trace_dump(L"the wall ray hit beside the point the camera looks at", false);
    }
    if (!(g_wall_pull > 0))
        return;
    const float pull = g_wall_pull < distance - kWallNearest ? g_wall_pull : distance - kWallNearest;
    if (!(pull > 0))
        return;
    float eye[3];
    for (int i = 0; i < 3; ++i)
        eye[i] = drawn[i] - out[i] * pull;
    for (int axis = 0; axis < 3; ++axis)
        m[12 + axis] = -(eye[0] * m[axis] + eye[1] * m[4 + axis] + eye[2] * m[8 + axis]);
}

// The camera as drawn (the offset's move, before walls) for the game's at `eye` looking at `at`, into `drawn`: shift_view()'s move
// along the view's right and up, worked out as the game's D3DXMatrixLookAtRH has them (the world's up -y: the game's y points down).
// The move is a share of the distance, so the drawn camera's on the line from `at` through it at any distance along the game's
// line. False (`drawn` the eye) with no offset, the free camera on, or the camera straight above or below.
bool drawn_eye(const float* at, const float* eye, float* drawn)
{
    for (int i = 0; i < 3; ++i)
        drawn[i] = eye[i];
    const std::uint8_t* const scene = g_scene != 0 ? *reinterpret_cast<std::uint8_t* const*>(g_scene) : nullptr;
    if ((g_shift[0] == 0 && g_shift[1] == 0) || g_fc_on.load() || scene == nullptr)
        return false;
    // cppcheck-suppress invalidPointerCast ; the game's structures hold floats at these offsets
    const auto* projection = reinterpret_cast<const float*>(scene + kProjectionField);
    float z[3] = {eye[0] - at[0], eye[1] - at[1], eye[2] - at[2]};
    const float distance = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (!(distance > 0.01f) || !(projection[0] > 0) || !(projection[5] > 0))
        return false;
    for (float& part : z)
        part /= distance;
    const float across = std::sqrt(z[0] * z[0] + z[2] * z[2]);
    if (!(across > 0.0001f))
        return false;
    const float x[3] = {-z[2] / across, 0.0f, z[0] / across};                       // up cross z
    const float y[3] = {z[1] * x[2], z[2] * x[0] - z[0] * x[2], -z[1] * x[0]};      // z cross x
    const float right = 2.0f * g_shift[0] * distance / projection[0], up = 2.0f * g_shift[1] * distance / projection[5];
    for (int i = 0; i < 3; ++i)
        drawn[i] = eye[i] - right * x[i] - up * y[i];
    return true;
}

// The game's own checks of its camera against the zone, in its camera's frame (FUN_1001EC70; the camera there is
// offset_camera()'s), each made on its eye. With an offset the camera's drawn up or aside from that by a share of its distance,
// so the ground met the game's camera first: the game pulled it in, or kept it 0.17 over the floor under it, while the camera as
// drawn was still well clear, and moved up it couldn't go down as far. Each check's call goes through a hook of ours that makes
// it on the camera as drawn instead (drawn_eye()), and gives the game back what moves its own camera as far as the drawn one needs.
enum WallKind { WallRay, WallFloor, WallCeiling, WallSlide };
struct WallSite {
    const char* pattern;
    std::ptrdiff_t call; // the call's place in it
    WallKind kind;
};
constexpr WallSite kWallSites[] = {
    // after the follow, from the point the camera looks at out to it, put 0.5 short of a wall: mov ecx,[collision];
    // lea edx,[esp+8Ch]; lea ebp,[edi+44h]; push edx; lea eax,[esp+60h]; push ebp; push eax; call ray; test al,al; jnz
    {"8B 0D ?? ?? ?? ?? 8D 94 24 8C 00 00 00 8D 6F 44 52 8D 44 24 60 55 50 E8 ?? ?? ?? ?? 84 C0 75", 23, WallRay},
    // then from the character's body (1 over its feet), put 0.2 short: lea ecx,[esp+38h]; lea edi,[ebx+44h]; push eax;
    // push edi; push ecx; mov ecx,[collision]; call ray; test al,al; jz
    {"8D 4C 24 38 8D 7B 44 50 57 51 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 0F 84", 16, WallRay},
    // the floor under it (a ray 10 down), the camera kept 0.17 over it: mov ecx,[edi+4Ch]; mov edx,[edi+48h]; lea eax,[esp+14h];
    // push eax; mov eax,[esi]; push 10.0; push ecx; mov ecx,[collision]; push edx; push eax; call floor; cmp al,1
    {"8B 4F 4C 8B 57 48 8D 44 24 14 50 8B 06 68 00 00 20 41 51 8B 0D ?? ?? ?? ?? 52 50 E8 ?? ?? ?? ?? 3C 01", 27, WallFloor},
    // the ceiling over it (a ray 10 up), the camera kept 0.17 under it
    {"8B 57 4C 8B 47 48 8D 4C 24 14 51 8B 0E 68 00 00 20 41 52 50 51 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 3C 01", 27, WallCeiling},
    // the near plane's sides: rays across the camera's left and right, taken from its view (the drawn camera's already), and the
    // camera set where they slide it clear of a wall, worked out in the view's terms: lea ecx,[esp+48h]; push ecx; mov ecx,edi;
    // call set; cmp bl,1; jne
    {"8D 4C 24 48 51 8B CF E8 ?? ?? ?? ?? 80 FB 01 75", 7, WallSlide},
    //   ...; call set; mov ecx,edi; call view
    {"8D 4C 24 48 51 8B CF E8 ?? ?? ?? ?? 8B CF E8", 7, WallSlide},
    // last, from the body again while [10454C0Ch] counts down: mov ecx,[collision]; lea edx,[esp+12Ch]; push edx;
    // lea eax,[esp+0E4h]; push esi; push eax; call ray; test al,al; jz
    {"8B 0D ?? ?? ?? ?? 8D 94 24 2C 01 00 00 52 8D 84 24 E4 00 00 00 56 50 E8 ?? ?? ?? ?? 84 C0 0F 84", 23, WallRay},
};
constexpr std::size_t kWallSiteCount = sizeof kWallSites / sizeof kWallSites[0];
// the floor or ceiling over or under a point: thiscall on the zone's collision, true (1) and its height when one's within range
using HeightFn = std::uint8_t(__thiscall*)(void* collision, float x, float y, float z, float range, float* height);
using SetEyeFn = void(__thiscall*)(void* camera, const float* eye);
std::uintptr_t g_wall_site[kWallSiteCount] = {}; // each check's call; 0: not found
std::uintptr_t g_wall_game[kWallSiteCount] = {}; // where it called
bool g_walls_patched = false;
HeightFn g_floor = nullptr;
HeightFn g_ceiling = nullptr;
SetEyeFn g_set_eye = nullptr;

// The game's camera's eye (the point it looks at three floats on), or nullptr without a scene
float* offset_eye()
{
    std::uint8_t* const camera = offset_camera();
    // cppcheck-suppress invalidPointerCast ; the game's structures hold floats at these offsets
    return camera != nullptr ? reinterpret_cast<float*>(camera + kEye) : nullptr;
}

// A ray of the game's from `from` out to its camera (`to`, its eye) cast to the camera as drawn; a hit is given back at the same
// share of the way along the game's line, so the game pulls its camera in as far as the drawn one needs
bool __fastcall camera_ray(void* collision, void*, const float* from, const float* to, float* hit)
{
    float drawn[3];
    if (to != offset_eye() || !drawn_eye(to + 3, to, drawn))
        return g_wall_ray(collision, from, to, hit);
    if (!g_wall_ray(collision, from, drawn, hit))
        return false;
    float met = 0.0f, whole = 0.0f;
    for (int i = 0; i < 3; ++i) {
        met += (hit[i] - from[i]) * (hit[i] - from[i]);
        whole += (drawn[i] - from[i]) * (drawn[i] - from[i]);
    }
    const float share = whole > 0 ? std::sqrt(met / whole) : 0.0f;
    for (int i = 0; i < 3; ++i)
        hit[i] = from[i] + (to[i] - from[i]) * share;
    return true;
}

// The floor or ceiling under or over the game's camera (`x`, `y`, `z`: its eye) found under or over the camera as drawn, and its
// height given back as far from the game's camera as it is from the drawn one, so the game keeps the drawn camera clear of it
std::uint8_t height_drawn(HeightFn game, void* collision, float x, float y, float z, float range, float* height)
{
    const float* const eye = offset_eye();
    float drawn[3];
    if (eye == nullptr || x != eye[0] || y != eye[1] || z != eye[2] || !drawn_eye(eye + 3, eye, drawn))
        return game(collision, x, y, z, range, height);
    const std::uint8_t found = game(collision, drawn[0], drawn[1], drawn[2], range, height);
    if (found == 1)
        *height -= drawn[1] - y;
    return found;
}

std::uint8_t __fastcall floor_drawn(void* collision, void*, float x, float y, float z, float range, float* height)
{
    return height_drawn(g_floor, collision, x, y, z, range, height);
}

std::uint8_t __fastcall ceiling_drawn(void* collision, void*, float x, float y, float z, float range, float* height)
{
    return height_drawn(g_ceiling, collision, x, y, z, range, height);
}

// The camera set where the near plane's check slid it (`eye`: the camera of the view built just before, the drawn one, moved
// aside): the game's camera, as it was when that view was built (g_view_eye), moved by as much. Set straight, the game's camera
// went to the drawn one's place and had the offset added again.
void __fastcall slide_drawn(void* camera, void*, const float* eye)
{
    float* const game = offset_eye();
    float drawn[3];
    if (game == nullptr || camera != static_cast<void*>(reinterpret_cast<std::uint8_t*>(game) - kEye) ||
        !drawn_eye(game + 3, game, drawn)) {
        g_set_eye(camera, eye);
        return;
    }
    // cppcheck-suppress invalidPointerCast ; the game's structures hold floats at these offsets
    const auto* view = reinterpret_cast<const float*>(static_cast<std::uint8_t*>(camera) + 4);
    float to[3];
    for (int i = 0; i < 3; ++i) {
        const float seen = -(view[12] * view[i * 4] + view[13] * view[i * 4 + 1] + view[14] * view[i * 4 + 2]);
        to[i] = g_view_eye[i] + eye[i] - seen;
    }
    g_set_eye(camera, to);
}

// Each of the game's checks on its camera found, and what it calls checked (the rays: the one walls casts)
void find_walls(HMODULE module)
{
    int missing = 0;
    for (std::size_t i = 0; i < kWallSiteCount; ++i) {
        const WallSite& site = kWallSites[i];
        const std::uintptr_t at = scan::find(module, site.pattern);
        const std::uintptr_t target = at != 0 ? call_target(at + site.call) : 0;
        bool ok = target != 0;
        if (ok && site.kind == WallRay)
            ok = target == reinterpret_cast<std::uintptr_t>(g_wall_ray);
        else if (ok && site.kind == WallFloor)
            g_floor = reinterpret_cast<HeightFn>(target);
        else if (ok && site.kind == WallCeiling)
            g_ceiling = reinterpret_cast<HeightFn>(target);
        else if (ok && site.kind == WallSlide && (g_set_eye == nullptr || target == reinterpret_cast<std::uintptr_t>(g_set_eye)))
            g_set_eye = reinterpret_cast<SetEyeFn>(target);
        else
            ok = false; // the slides call different functions: neither's ours to know (or it failed already)
        if (ok) {
            g_wall_site[i] = at + site.call;
            g_wall_game[i] = target;
        } else {
            ++missing;
        }
    }
    if (missing != 0)
        log::write(L"Camera: %d of the game's %u checks on its camera weren't found; with [camera] offset those check the camera unmoved",
                   missing, static_cast<unsigned>(kWallSiteCount));
}

std::uintptr_t wall_hook(WallKind kind)
{
    switch (kind) {
    case WallRay:
        return reinterpret_cast<std::uintptr_t>(&camera_ray);
    case WallFloor:
        return reinterpret_cast<std::uintptr_t>(&floor_drawn);
    case WallCeiling:
        return reinterpret_cast<std::uintptr_t>(&ceiling_drawn);
    case WallSlide:
        return reinterpret_cast<std::uintptr_t>(&slide_drawn);
    }
    return 0;
}

// The game's checks on its camera through ours, or straight again
void patch_walls(bool on)
{
    if (on == g_walls_patched)
        return;
    for (std::size_t i = 0; i < kWallSiteCount; ++i) {
        if (g_wall_site[i] == 0)
            continue;
        std::uint8_t call[5];
        branch(call, 0xE8, g_wall_site[i], on ? wall_hook(kWallSites[i].kind) : g_wall_game[i]);
        write_code(g_wall_site[i], call, sizeof call);
    }
    g_walls_patched = on;
}
