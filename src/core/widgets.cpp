#include "widgets.hpp"

#include <windows.h>

#include <lua.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "../gfx/vanacore_gfx.h"
#include "camera.hpp"
#include "d3d8.hpp"
#include "game.hpp"
#include "groundmarks.hpp"
#include "hideui.hpp"
#include "log.hpp"
#include "lua_api.hpp"
#include "text.hpp"
#include "ui.hpp"

namespace vanacore::widgets {
namespace {

using d3d8::call;
namespace slot = d3d8::slot;

constexpr char kBoxType[] = "vana.ui.box"; // lua_api.cpp's boxes: a userdata holding the box's id
constexpr DWORD kZFunc = 23, kCmpLessEqual = 4;
constexpr std::uint32_t kLabelBase = 0xE0000000u; // frame generation's label for a widget in the world: this | the box's id
constexpr float kNearestScale = 0.25f, kFarthestScale = 4.0f; // how far distance scales a widget in the world
constexpr float kPullBack = 0.3f;                              // yalms the sight line stops short of its point
constexpr float kPickReach = 150.0f;                           // yalms a pick (vana.world.pick) looks from the camera
constexpr float kOffscreen = 2.0f;                             // past this many screens off the middle, not drawn
constexpr float kNearest = 0.05f; // yalms in front of the camera a laid box's corners must be (a corner behind it: not drawn)
constexpr float kMostYalms = 500.0f; // the most a laid box's width or height is, yalms
// A laid box is drawn nearer the camera than it is, by this many yalms and this share of its distance (each corner along its
// line to the eye, so it covers the same pixels): the ground or wall it lies on doesn't cut through it where the depth is a
// little off (uneven ground under the point, the scene's depth moved back for frame generation's made frames)
constexpr float kNearer = 0.08f, kNearerShare = 0.015f;

enum class Side { above, below, left, right, over };

// vana.world.place's facing: a picture facing the screen (camera), or a box laid in the world, sized in yalms: standing on
// its point and turned round its upright line to the camera (upright: a beam, a post), lying on the ground with its top
// toward `heading` (ground), or standing and facing `heading` (wall: a sign, a panel)
enum class Facing { camera, upright, ground, wall };
constexpr const char* kFacings[] = {"camera", "upright", "ground", "wall"};

// One placed box. The positions are in the units of the call that made them (see docs/addons/18-widgets.md).
struct Place {
    int box = 0;
    bool world = false;
    // vana.world.attach: the character's target index, and the side of its name
    unsigned index = 0;
    Side side = Side::above;
    bool follow = false; // grows and shrinks with the name
    // vana.world.place: the point as vana.game.entity has it (x, y across the ground, z the height), yalms above it, and
    // which part of the box sits on it, 0 to 1 across and down
    float x = 0.0f, y = 0.0f, z = 0.0f, up = 0.0f;
    float pivot_x = 0.5f, pivot_y = 1.0f;
    float distance = 0.0f; // its own size at this many yalms, bigger nearer and smaller farther (0: the same size)
    float range = 0.0f;    // not drawn farther than this many yalms (0: any)
    bool over_ui = false;  // drawn over the interface, not under it
    // laid in the world (not facing the camera): radians as vana.game.entity's heading has them (0 east, turning clockwise
    // as seen from above), and its size in yalms (0: from the other one and the picture's shape; 1 across when neither)
    Facing facing = Facing::camera;
    float heading = 0.0f, width = 0.0f, height = 0.0f;
    // yalms toward the camera (negative: away): a picture facing it and a box on the ground or a wall only count as nearer
    // for the walls (the same pixels); an upright one stands out from its line, level (a column's front, or its back)
    float toward = 0.0f;
    // both: pixels from there (right and down), the box's size times scale, and whether walls hide it
    float dx = 0.0f, dy = 0.0f, scale = 1.0f;
    bool walls = true;
};
std::vector<Place> g_places;

// The world's camera as the scene tracked it this frame: its eye, and the viewport the 3D drew in
struct Camera {
    float view[16] = {}, proj[16] = {};
    std::uint32_t viewport[6] = {};
    float min_z = 0.0f, max_z = 1.0f;
    float eye[3] = {};
};

bool read_camera(Camera& camera)
{
    if (!vanacore_gfx_camera(camera.view, camera.proj, camera.viewport) || camera.viewport[2] == 0 || camera.viewport[3] == 0)
        return false;
    std::memcpy(&camera.min_z, &camera.viewport[4], sizeof camera.min_z);
    std::memcpy(&camera.max_z, &camera.viewport[5], sizeof camera.max_z);
    if (!(camera.max_z > camera.min_z)) {
        camera.min_z = 0.0f;
        camera.max_z = 1.0f;
    }
    // the view's axes are its columns, its _41 to _43 the camera's place against them
    for (int i = 0; i < 3; ++i)
        camera.eye[i] = -(camera.view[12] * camera.view[i * 4] + camera.view[13] * camera.view[i * 4 + 1] +
                          camera.view[14] * camera.view[i * 4 + 2]);
    return true;
}

// A point of the world on the screen: how far across and down the viewport it is (0 to 1; the viewport the scene recorded is in
// the target's pixels, not the game's, so only these fractions mean the same everywhere), the depth buffer's value there, the
// rhw the game's name quads have (1), and its distance in front of the camera
struct Projected {
    float fx, fy, z, rhw, depth;
};

// The point (x, y, z as vana.game.entity has them; its z, the height, is the game's draw y: up is smaller) through the view
// and the projection as Direct3D 8 has them, row vectors: its clip space (w is its distance in front of the camera)
void clip_space(const Camera& camera, float x, float y, float z, float (&clip)[4])
{
    const float* const v = camera.view;
    const float* const p = camera.proj;
    const float world[3] = {x, z, y};
    float view[4];
    for (int j = 0; j < 4; ++j)
        view[j] = world[0] * v[j] + world[1] * v[4 + j] + world[2] * v[8 + j] + v[12 + j];
    for (int j = 0; j < 4; ++j)
        clip[j] = view[0] * p[j] + view[1] * p[4 + j] + view[2] * p[8 + j] + view[3] * p[12 + j];
}

// The point on the screen (clip_space's). False behind the camera, past its planes, or far off the screen.
bool project(const Camera& camera, float x, float y, float z, Projected& out)
{
    float clip[4];
    clip_space(camera, x, y, z, clip);
    if (!(clip[3] > 1e-4f))
        return false;
    const float nx = clip[0] / clip[3], ny = clip[1] / clip[3], nz = clip[2] / clip[3];
    if (!(nz >= 0.0f && nz <= 1.0f) || std::fabs(nx) > kOffscreen || std::fabs(ny) > kOffscreen)
        return false;
    out.fx = nx * 0.5f + 0.5f;
    out.fy = 0.5f - ny * 0.5f;
    out.z = camera.min_z + nz * (camera.max_z - camera.min_z);
    out.rhw = 1.0f; // as the game's name quads have it
    out.depth = clip[3];
    return true;
}

// 1 where nothing of the zone's collision is between the camera and the point, 0 where something is, -1 not known
int clear_sight(const Camera& cam, float x, float y, float z)
{
    float to[3] = {x, z, y};
    float length = 0.0f;
    for (int i = 0; i < 3; ++i)
        length += (cam.eye[i] - to[i]) * (cam.eye[i] - to[i]);
    length = std::sqrt(length);
    if (length > kPullBack)
        for (int i = 0; i < 3; ++i)
            to[i] += (cam.eye[i] - to[i]) * (kPullBack / length);
    return camera::sight(cam.eye, to);
}

// A box the addon hasn't destroyed, ready to draw: its picture made and on the device. nullptr otherwise.
ui::Box* prepared(const Place& place, void* device)
{
    ui::Box* const box = ui::find(place.box);
    if (box == nullptr || !box->visible)
        return nullptr;
    ui::refresh_placed(*box);
    if (box->sprite.width() <= 0 || box->sprite.height() <= 0 || !box->sprite.upload(device) ||
        box->sprite.texture() == nullptr || box->sprite.texture_width() == 0 || box->sprite.texture_height() == 0)
        return nullptr;
    return box;
}

float box_alpha(const ui::Box& box)
{
    return std::clamp(box.opacity * (box.image ? box.style.background[3] : 1.0f), 0.0f, 1.0f);
}

// The device's state kept for the draws and put back after
struct Kept {
    explicit Kept(void* device) : device(device)
    {
        ok = SUCCEEDED(call(device, slot::CreateStateBlock, d3d8::kStateBlockAll, &block));
    }
    ~Kept()
    {
        if (ok) {
            call(device, slot::ApplyStateBlock, block);
            call(device, slot::DeleteStateBlock, block);
        }
    }
    void* device;
    DWORD block = 0;
    bool ok = false;
};

// The state a widget's quad is drawn with: the texture times the vertices' colour, blended, tested against the depth
// buffer (but not written to it) unless walls don't hide it
void set_state(void* device, bool walls)
{
    const auto render = [&](DWORD type, DWORD value) { call(device, slot::SetRenderState, type, value); };
    const auto stage = [&](DWORD number, DWORD type, DWORD value) {
        call(device, slot::SetTextureStageState, number, type, value);
    };
    render(d3d8::rs::ZEnable, walls ? TRUE : FALSE);
    render(d3d8::rs::ZWriteEnable, FALSE);
    render(kZFunc, kCmpLessEqual);
    render(d3d8::rs::AlphaTestEnable, FALSE);
    render(d3d8::rs::AlphaBlendEnable, TRUE);
    render(d3d8::rs::SrcBlend, d3d8::kBlendSrcAlpha);
    render(d3d8::rs::DestBlend, d3d8::kBlendInvSrcAlpha);
    render(d3d8::rs::BlendOp, d3d8::kBlendOpAdd);
    render(d3d8::rs::FogEnable, FALSE);
    render(d3d8::rs::Lighting, FALSE);
    render(d3d8::rs::CullMode, d3d8::kCullNone);
    render(d3d8::rs::SpecularEnable, FALSE);
    render(d3d8::rs::StencilEnable, FALSE);
    render(d3d8::rs::ColorWriteEnable, 0xF);
    stage(0, d3d8::tss::ColorOp, d3d8::kTopModulate);
    stage(0, d3d8::tss::ColorArg1, d3d8::kTaTexture);
    stage(0, d3d8::tss::ColorArg2, d3d8::kTaDiffuse);
    stage(0, d3d8::tss::AlphaOp, d3d8::kTopModulate);
    stage(0, d3d8::tss::AlphaArg1, d3d8::kTaTexture);
    stage(0, d3d8::tss::AlphaArg2, d3d8::kTaDiffuse);
    stage(0, d3d8::tss::TexCoordIndex, 0);
    stage(0, d3d8::tss::TextureTransformFlags, 0);
    stage(0, d3d8::tss::AddressU, d3d8::kAddressClamp);
    stage(0, d3d8::tss::AddressV, d3d8::kAddressClamp);
    stage(0, d3d8::tss::MagFilter, d3d8::kFilterLinear);
    stage(0, d3d8::tss::MinFilter, d3d8::kFilterLinear);
    stage(0, d3d8::tss::MipFilter, d3d8::kFilterNone);
    stage(1, d3d8::tss::ColorOp, d3d8::kTopDisable);
    stage(1, d3d8::tss::AlphaOp, d3d8::kTopDisable);
    call(device, slot::SetVertexShader, d3d8::kFvfXyzRhwDiffuseTex1);
    call(device, slot::SetPixelShader, DWORD{0});
}

// A corner of a widget's quad in the game's pixels, its depth and rhw (XYZRHW): top left, top right, bottom left, bottom right
struct Corner {
    float x, y, z, rhw;
};

// The box's picture on the quad `corners`
void draw_corners(const nameplates::Pass& pass, const ui::Box& box, const Corner (&corners)[4])
{
    struct Vertex {
        float x, y, z, rhw;
        DWORD colour;
        float u, v;
    };
    const text::Sprite& sprite = box.sprite;
    const float u = static_cast<float>(sprite.width()) / static_cast<float>(sprite.texture_width());
    const float v = static_cast<float>(sprite.height()) / static_cast<float>(sprite.texture_height());
    const DWORD colour = static_cast<DWORD>(std::lround(box_alpha(box) * 255.0f)) << 24 | 0xFFFFFF;
    Vertex quad[4];
    for (int i = 0; i < 4; ++i) {
        const Corner& c = corners[i];
        quad[i] = {c.x, c.y, c.z, c.rhw, colour, i & 1 ? u : 0.0f, i & 2 ? v : 0.0f};
    }
    call(pass.device, slot::SetTexture, DWORD{0}, sprite.texture());
    call(pass.device, slot::DrawPrimitiveUP, d3d8::kTriangleStrip, 2u, static_cast<const void*>(quad),
         static_cast<UINT>(sizeof(Vertex)));
}

// The box's picture as one quad, its top left at (left, top) in the scene's shown pixels and `scale` times its size, at depth
// z (rhw as the game's name quads have it) in the game's pixels
void draw_quad(const nameplates::Pass& pass, const ui::Box& box, float left, float top, float scale, float z, float rhw)
{
    const text::Sprite& sprite = box.sprite;
    // the game's pixel p is at (p + 0.5) of its pixels from the left, times the scale on the screen
    const float x0 = left / pass.kx - 0.5f, x1 = (left + static_cast<float>(sprite.width()) * scale) / pass.kx - 0.5f;
    const float y0 = top / pass.ky - 0.5f, y1 = (top + static_cast<float>(sprite.height()) * scale) / pass.ky - 0.5f;
    draw_corners(pass, box, {{x0, y0, z, rhw}, {x1, y0, z, rhw}, {x0, y1, z, rhw}, {x1, y1, z, rhw}});
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Boxes laid in the world (facing upright, ground or wall)

// A direction or point in the world as vana.game.entity has positions (x, y across the ground, z the height: up is smaller)
struct Vec {
    float x, y, z;
};
Vec operator+(const Vec& a, const Vec& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec operator*(const Vec& a, float k) { return {a.x * k, a.y * k, a.z * k}; }

// The point on the screen in normalised coordinates (y up), or false behind the camera
bool ndc(const Camera& camera, const Vec& p, float& x, float& y)
{
    float clip[4];
    clip_space(camera, p.x, p.y, p.z, clip);
    if (!(clip[3] > kNearest))
        return false;
    x = clip[0] / clip[3], y = clip[1] / clip[3];
    return true;
}

// The laid box's axes in the world, a yalm long: across it (its picture's left to right) and up it (bottom to top). Upright,
// across is the camera's right made level, so the box turns round its upright line to face the camera; ground and wall turn
// by the heading. Then across is turned round if the picture would be seen mirrored from the side it faces (whichever way the
// game's world and projection are handed): from the front it reads as it was drawn, from behind it is mirrored, as a pane
// of glass is. False when that can't be told (the point behind the camera).
bool laid_axes(const Camera& camera, const Place& place, const Vec& base, Vec& across, Vec& up)
{
    const Vec forward = {std::cos(place.heading), -std::sin(place.heading), 0.0f};
    const Vec eye = {camera.eye[0], camera.eye[2], camera.eye[1]};
    bool front = true; // whether the camera is on the side the picture faces
    if (place.facing == Facing::upright) {
        const float level = std::hypot(camera.view[0], camera.view[8]); // the view's first column: the camera's right
        across = level > 1e-4f ? Vec{camera.view[0] / level, camera.view[8] / level, 0.0f} : Vec{forward.y, -forward.x, 0.0f};
        up = {0.0f, 0.0f, -1.0f};
    } else if (place.facing == Facing::ground) {
        across = {forward.y, -forward.x, 0.0f}; // the heading a quarter turn clockwise: right of its top, from above
        up = forward;
        front = eye.z < base.z;
    } else {
        across = {forward.y, -forward.x, 0.0f};
        up = {0.0f, 0.0f, -1.0f};
        front = (eye.x - base.x) * forward.x + (eye.y - base.y) * forward.y > 0.0f;
    }
    float bx, by, rx, ry, ux, uy;
    if (!ndc(camera, base, bx, by) || !ndc(camera, base + across, rx, ry) || !ndc(camera, base + up, ux, uy))
        return false;
    // unmirrored, across goes right on the screen and up goes up: their cross product is positive (y up)
    const bool unmirrored = (rx - bx) * (uy - by) - (ry - by) * (ux - bx) > 0.0f;
    if (place.facing == Facing::upright ? rx < bx : unmirrored != front)
        across = across * -1.0f;
    return true;
}

// A box laid in the world: its four corners are points of the world, each projected with its own depth and rhw, so the quad
// has the world's perspective across it and walls hide the parts of it behind them. False when it isn't drawn
bool draw_laid(const nameplates::Pass& pass, const Camera& camera, const Place& place, const ui::Box& box)
{
    Vec base = {place.x, place.y, place.z - place.up};
    float base_clip[4];
    clip_space(camera, base.x, base.y, base.z, base_clip);
    if (!(base_clip[3] > kNearest) || (place.range > 0.0f && base_clip[3] > place.range))
        return false;
    if (place.facing == Facing::upright && place.toward != 0.0f) {
        const float dx = camera.eye[0] - base.x, dy = camera.eye[2] - base.y, level = std::hypot(dx, dy);
        if (level > 1e-3f)
            base = base + Vec{dx, dy, 0.0f} * (std::min(place.toward, 0.9f * level) / level);
    }
    Vec across, up;
    if (!laid_axes(camera, place, base, across, up))
        return false;
    // its size in yalms: the width and height given, or the one given and the picture's shape, or a yalm across
    const float shape = static_cast<float>(box.sprite.height()) / static_cast<float>(box.sprite.width());
    float width = place.width, height = place.height;
    if (width <= 0.0f)
        width = height > 0.0f ? height / shape : 1.0f;
    if (height <= 0.0f)
        height = width * shape;
    width *= place.scale, height *= place.scale;
    // On the ground or a wall, drawn nearer (kNearer, and `toward`): the whole quad shrunk toward the eye by one share, so its
    // picture is as it was. Upright, it stands in the air: `toward` moves it out from its line, level, to the camera.
    const Vec eye = {camera.eye[0], camera.eye[2], camera.eye[1]};
    const float away = std::hypot(base.x - eye.x, base.y - eye.y, base.z - eye.z);
    float keep = 1.0f;
    if (place.facing != Facing::upright && away > 1e-3f)
        keep = 1.0f - std::clamp(kNearer + kNearerShare * away + place.toward, -0.5f * away, 0.5f * away) / away;
    Corner corners[4];
    for (int i = 0; i < 4; ++i) {
        const float u = i & 1 ? 1.0f : 0.0f, v = i & 2 ? 1.0f : 0.0f;
        const Vec at = base + across * ((u - place.pivot_x) * width) + up * ((place.pivot_y - v) * height);
        const Vec p = eye + (at + eye * -1.0f) * keep;
        float clip[4];
        clip_space(camera, p.x, p.y, p.z, clip);
        if (!(clip[3] > kNearest))
            return false;
        const float w = clip[3];
        corners[i] = {(clip[0] / w * 0.5f + 0.5f) * pass.game_w, (0.5f - clip[1] / w * 0.5f) * pass.game_h,
                      camera.min_z + clip[2] / w * (camera.max_z - camera.min_z), 1.0f / w};
    }
    vanacore_gfx_label(kLabelBase | static_cast<std::uint32_t>(place.box & 0xFFFFFFF));
    set_state(pass.device, place.walls);
    draw_corners(pass, box, corners);
    vanacore_gfx_label(0);
    return true;
}

// A picture facing the camera: the depth of its point moved `toward` yalms along its line to the eye (the same pixels), or
// `z` as it is
float nearer_z(const Camera& camera, const Place& place, float z)
{
    if (place.toward == 0.0f)
        return z;
    const Vec point = {place.x, place.y, place.z - place.up};
    const Vec eye = {camera.eye[0], camera.eye[2], camera.eye[1]};
    const float away = std::hypot(point.x - eye.x, point.y - eye.y, point.z - eye.z);
    if (!(away > 1e-3f))
        return z;
    const Vec p = point + (eye + point * -1.0f) * (std::clamp(place.toward, -away, 0.9f * away) / away);
    Projected moved;
    return project(camera, p.x, p.y, p.z, moved) ? moved.z : z;
}

// Whether the box is drawn in the world's pass, under the interface: over_ui is for a picture facing the camera
bool in_world(const Place& place) { return place.world && (!place.over_ui || place.facing != Facing::camera); }

// Whole pixels for a box drawn at its own size, which is then as sharp as the interface's
float crisp(float value, float scale) { return scale == 1.0f ? std::round(value) : value; }

// Widgets are let go with the boxes their addons destroy
void prune()
{
    std::erase_if(g_places, [](const Place& place) { return ui::find(place.box) == nullptr; });
}

Place* find_place(int box)
{
    for (Place& place : g_places)
        if (place.box == box)
            return &place;
    return nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// vana.world

struct BoxHandle {
    int id;
};

ui::Box* box_argument(lua_State* L, int index)
{
    const auto* handle = static_cast<const BoxHandle*>(luaL_checkudata(L, index, kBoxType));
    ui::Box* const box = ui::find(handle->id);
    if (box == nullptr)
        luaL_error(L, "the box was destroyed");
    return box;
}

float field_number(lua_State* L, int table, const char* key, float fallback)
{
    lua_getfield(L, table, key);
    const float value = lua_type(L, -1) == LUA_TNUMBER ? static_cast<float>(lua_tonumber(L, -1)) : fallback;
    lua_pop(L, 1);
    return value;
}

bool field_flag(lua_State* L, int table, const char* key, bool fallback)
{
    lua_getfield(L, table, key);
    const bool value = lua_type(L, -1) == LUA_TBOOLEAN ? lua_toboolean(L, -1) != 0 : fallback;
    lua_pop(L, 1);
    return value;
}

std::string field_text(lua_State* L, int table, const char* key)
{
    lua_getfield(L, table, key);
    std::string value = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return value;
}

// The options table at `index` on `place`: only the options it has change (the rest stay as `place` has them: the defaults for
// attach and place, what the box has for update). The parts both kinds share.
void read_common(lua_State* L, int index, Place& place)
{
    place.dx = field_number(L, index, "x", place.dx);
    place.dy = field_number(L, index, "y", place.dy);
    place.scale = std::clamp(field_number(L, index, "scale", place.scale), 0.05f, 20.0f);
    place.walls = field_flag(L, index, "walls", place.walls);
}

// A nameplate's own options: side, distance and (for update) the target's index. What's wrong with them, or nullptr.
const char* read_attach(lua_State* L, int index, Place& place)
{
    const std::string side = field_text(L, index, "side");
    if (side == "above")
        place.side = Side::above;
    else if (side == "below")
        place.side = Side::below;
    else if (side == "left")
        place.side = Side::left;
    else if (side == "right")
        place.side = Side::right;
    else if (side == "over")
        place.side = Side::over;
    else if (!side.empty())
        return "side is 'above', 'below', 'left', 'right' or 'over'";
    place.follow = field_flag(L, index, "distance", place.follow);
    const float target = field_number(L, index, "index", static_cast<float>(place.index));
    if (!(target >= 0.0f && target <= 4095.0f))
        return "the target index is 0 to 4095";
    place.index = static_cast<unsigned>(target);
    return nullptr;
}

// How a box at a point faces (facing, and heading, width and height for one laid in the world). What's wrong, or nullptr.
const char* read_facing(lua_State* L, int index, Place& place)
{
    const std::string facing = field_text(L, index, "facing");
    if (!facing.empty()) {
        const auto* const found = std::find(std::begin(kFacings), std::end(kFacings), facing);
        if (found == std::end(kFacings))
            return "facing is 'camera', 'upright', 'ground' or 'wall'";
        place.facing = static_cast<Facing>(found - std::begin(kFacings));
    }
    place.heading = field_number(L, index, "heading", place.heading);
    place.width = std::clamp(field_number(L, index, "width", place.width), 0.0f, kMostYalms);
    place.height = std::clamp(field_number(L, index, "height", place.height), 0.0f, kMostYalms);
    place.toward = std::clamp(field_number(L, index, "toward", place.toward), -kMostYalms, kMostYalms);
    return nullptr;
}

// A point in the world's own options (`at`, {x, y, z}, moves the point). What's wrong with them, or nullptr.
const char* read_world(lua_State* L, int index, Place& place)
{
    if (const char* problem = read_facing(L, index, place))
        return problem;
    place.up = field_number(L, index, "up", place.up);
    place.distance = std::max(0.0f, field_number(L, index, "distance", place.distance));
    place.range = std::max(0.0f, field_number(L, index, "range", place.range));
    place.over_ui = field_flag(L, index, "over_ui", place.over_ui);
    const std::string pivot = field_text(L, index, "pivot");
    if (pivot == "bottom")
        place.pivot_x = 0.5f, place.pivot_y = 1.0f;
    else if (pivot == "center")
        place.pivot_x = 0.5f, place.pivot_y = 0.5f;
    else if (pivot == "top")
        place.pivot_x = 0.5f, place.pivot_y = 0.0f;
    else if (pivot == "left")
        place.pivot_x = 0.0f, place.pivot_y = 0.5f;
    else if (pivot == "right")
        place.pivot_x = 1.0f, place.pivot_y = 0.5f;
    else if (!pivot.empty())
        return "pivot is 'bottom', 'center', 'top', 'left' or 'right'";
    lua_getfield(L, index, "at");
    if (lua_type(L, -1) == LUA_TTABLE) {
        place.x = field_number(L, lua_gettop(L), "x", place.x);
        place.y = field_number(L, lua_gettop(L), "y", place.y);
        place.z = field_number(L, lua_gettop(L), "z", place.z);
    }
    lua_pop(L, 1);
    return nullptr;
}

// A new place for `box`, every option at its default: attach and place replace what the box had; update changes some of it
Place fresh(const ui::Box& box, bool world)
{
    Place place;
    place.box = box.id;
    place.world = world;
    return place;
}

// The place of `box` made or changed: its box no longer drawn at its x and y
Place& claim(ui::Box& box)
{
    Place* place = find_place(box.id);
    if (place == nullptr) {
        g_places.emplace_back();
        place = &g_places.back();
        place->box = box.id;
    }
    box.placed = true;
    return *place;
}

// vana.world.attach(box, index[, options]): the box at the name over target `index`
int world_attach(lua_State* L)
{
    ui::Box* const box = box_argument(L, 1);
    const lua_Integer index = luaL_checkinteger(L, 2);
    if (index < 0 || index > 0xFFF)
        return luaL_error(L, "the target index is 0 to 4095");
    Place place = fresh(*box, false);
    if (lua_type(L, 3) == LUA_TTABLE) {
        read_common(L, 3, place);
        if (const char* problem = read_attach(L, 3, place))
            return luaL_error(L, "%s", problem);
    }
    place.index = static_cast<unsigned>(index); // (the argument, not an `index` option)
    claim(*box) = place;
    return 0;
}

// vana.world.place(box, x, y, z[, options]): the box at a point in the world
int world_place(lua_State* L)
{
    ui::Box* const box = box_argument(L, 1);
    Place place = fresh(*box, true);
    if (lua_type(L, 5) == LUA_TTABLE) {
        read_common(L, 5, place);
        if (const char* problem = read_world(L, 5, place))
            return luaL_error(L, "%s", problem);
    }
    place.x = static_cast<float>(luaL_checknumber(L, 2)); // (the arguments, not an `at` option)
    place.y = static_cast<float>(luaL_checknumber(L, 3));
    place.z = static_cast<float>(luaL_checknumber(L, 4));
    claim(*box) = place;
    return 0;
}

// vana.world.update(box, options): changes some options of a box that is placed, without placing it again: what attach and place
// take, and `index` (another character's name) or `at` (another point). Cheap enough for every frame.
int world_update(lua_State* L)
{
    const ui::Box* const box = box_argument(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    Place* const found = find_place(box->id);
    if (found == nullptr)
        return luaL_error(L, "the box isn't placed: vana.world.attach or vana.world.place it first");
    Place place = *found;
    read_common(L, 2, place);
    if (const char* problem = place.world ? read_world(L, 2, place) : read_attach(L, 2, place))
        return luaL_error(L, "%s", problem);
    *found = place;
    return 0;
}

void set_number(lua_State* L, const char* key, float value)
{
    lua_pushnumber(L, value);
    lua_setfield(L, -2, key);
}

void set_flag(lua_State* L, const char* key, bool value)
{
    lua_pushboolean(L, value);
    lua_setfield(L, -2, key);
}

// vana.world.placement(box) -> table | nil: how the box is placed now, as update takes it (`index` for a nameplate, `at` for a point)
int world_placement(lua_State* L)
{
    const ui::Box* const box = box_argument(L, 1);
    const Place* const place = find_place(box->id);
    if (place == nullptr)
        return 0;
    lua_newtable(L);
    set_number(L, "x", place->dx), set_number(L, "y", place->dy), set_number(L, "scale", place->scale);
    set_flag(L, "walls", place->walls);
    if (!place->world) {
        static const char* const kSides[] = {"above", "below", "left", "right", "over"};
        lua_pushstring(L, kSides[static_cast<int>(place->side)]);
        lua_setfield(L, -2, "side");
        set_flag(L, "distance", place->follow);
        set_number(L, "index", static_cast<float>(place->index));
        return 1;
    }
    set_number(L, "up", place->up), set_number(L, "distance", place->distance), set_number(L, "range", place->range);
    set_flag(L, "over_ui", place->over_ui);
    lua_pushstring(L, kFacings[static_cast<int>(place->facing)]);
    lua_setfield(L, -2, "facing");
    set_number(L, "heading", place->heading), set_number(L, "width", place->width), set_number(L, "height", place->height);
    set_number(L, "toward", place->toward);
    lua_newtable(L);
    set_number(L, "x", place->x), set_number(L, "y", place->y), set_number(L, "z", place->z);
    lua_setfield(L, -2, "at");
    return 1;
}

// vana.world.detach(box): back to being drawn at its x and y
int world_detach(lua_State* L)
{
    const auto* handle = static_cast<const BoxHandle*>(luaL_checkudata(L, 1, kBoxType));
    if (ui::Box* const box = ui::find(handle->id))
        box->placed = false;
    std::erase_if(g_places, [&](const Place& place) { return place.box == handle->id; });
    return 0;
}

// vana.world.to_screen(x, y, z) -> x, y, distance | nil: a point of the world in the interface's pixels (vana.ui's), and how
// far in front of the camera it is. nil behind the camera, off the screen, or before the world has a camera.
int world_to_screen(lua_State* L)
{
    const float x = static_cast<float>(luaL_checknumber(L, 1));
    const float y = static_cast<float>(luaL_checknumber(L, 2));
    const float z = static_cast<float>(luaL_checknumber(L, 3));
    unsigned width = 0, height = 0;
    ui::screen(width, height);
    Camera camera;
    Projected at;
    if (width == 0 || !read_camera(camera) || !project(camera, x, y, z, at))
        return 0;
    lua_pushnumber(L, at.fx * static_cast<float>(width));
    lua_pushnumber(L, at.fy * static_cast<float>(height));
    lua_pushnumber(L, at.depth);
    return 3;
}

// vana.world.pick(x, y) -> x, y, z | nil: the point of the zone under a pixel of the interface (vana.ui's): where a line from the
// camera through that pixel first meets the zone's collision (the ground, a floor, a wall), as vana.game.entity has positions
// (z the height), for an addon putting something down where the pointer is. nil where nothing is met within 150 yalms, before
// the world has a camera, or where the zone's collision isn't there. It's the game's collision, not what's drawn: a tree without
// collision isn't met, and the ground under it is.
int world_pick(lua_State* L)
{
    const float px = static_cast<float>(luaL_checknumber(L, 1));
    const float py = static_cast<float>(luaL_checknumber(L, 2));
    unsigned width = 0, height = 0;
    ui::screen(width, height);
    Camera camera;
    if (width == 0 || height == 0 || !read_camera(camera))
        return 0;
    const float* const p = camera.proj;
    const float* const v = camera.view;
    if (!(std::fabs(p[0]) > 1e-6f) || !(std::fabs(p[5]) > 1e-6f))
        return 0;
    // the pixel in the projection's normalised coordinates, then project() backwards for a direction in view space at depth
    // `hand` (P23, the projection's hand: clip w is 1 there, clip x is vx * P00 + hand * P20), then into the world: the view's
    // 3 by 3 is a rotation, undone by its transpose (the rows are the world's axes in view space)
    const float nx = px / static_cast<float>(width) * 2.0f - 1.0f;
    const float ny = 1.0f - py / static_cast<float>(height) * 2.0f;
    const float hand = p[11] < 0.0f ? -1.0f : 1.0f;
    const float dir_view[3] = {(nx - hand * p[8]) / p[0], (ny - hand * p[9]) / p[5], hand};
    float dir[3];
    for (int i = 0; i < 3; ++i)
        dir[i] = dir_view[0] * v[i * 4] + dir_view[1] * v[i * 4 + 1] + dir_view[2] * v[i * 4 + 2];
    const float length = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!(length > 1e-6f))
        return 0;
    float to[3], hit[8] = {};
    for (int i = 0; i < 3; ++i)
        to[i] = camera.eye[i] + dir[i] / length * kPickReach;
    if (camera::cast(camera.eye, to, hit) != 1)
        return 0;
    lua_pushnumber(L, hit[0]);
    lua_pushnumber(L, hit[2]);
    lua_pushnumber(L, hit[1]);
    return 3;
}

// vana.world.visible(x, y, z) -> true | false | nil: whether the zone's collision (walls, floors, doors) leaves the camera a
// clear line to the point. nil where that isn't known: no camera yet, or the zone's collision isn't loaded.
int world_visible(lua_State* L)
{
    const float x = static_cast<float>(luaL_checknumber(L, 1));
    const float y = static_cast<float>(luaL_checknumber(L, 2));
    const float z = static_cast<float>(luaL_checknumber(L, 3));
    Camera camera;
    if (!read_camera(camera))
        return 0;
    const int clear = clear_sight(camera, x, y, z);
    if (clear < 0)
        return 0;
    lua_pushboolean(L, clear);
    return 1;
}

// vana.world.marks(list): rings of light on the ground, this addon's whole set (groundmarks.hpp), each { x, y, z, radius,
// color, strength, shape, height, glyph, glyph_size }: its centre as the game has positions (z the height), its radius in
// world units (1), its colour '#RRGGBB' (white), how strongly it's drawn, 0 to 4 (1), its shape, 'round' (the default) or
// 'square' (a square of that half-width), the height of a column of light rising from it, 0 to 30 world units (0, none: a
// waymark saying where to stand has one), and a glyph over the column's top, 'A', 'B', 'C', 'D', '1', '2', '3' or '4', with
// glyph_size half its height in world units (0.8) and glyph_height how high over the ring its centre floats, 0 to 30 (0:
// just over the column's top). nil or an empty list takes them all off; the first 16 of every addon's together are drawn. The scene effects (Direct3D 12 and 11) lay them onto the ground the scene's depth shows: a
// slope carries one, and what stands in front of it hides it. On the game's own Direct3D 8 nothing is drawn
// (vana.graphics.info().active says which).
int world_marks(lua_State* L)
{
    std::vector<groundmarks::Mark> marks;
    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TTABLE);
        const int n = static_cast<int>(lua_objlen(L, 1));
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, 1, i);
            if (lua_type(L, -1) != LUA_TTABLE)
                return luaL_error(L, "vana.world.marks: entry %d isn't a table", i);
            const int at = lua_gettop(L);
            groundmarks::Mark m;
            m.x = field_number(L, at, "x", 0.0f);
            m.y = field_number(L, at, "y", 0.0f);
            m.z = field_number(L, at, "z", 0.0f);
            m.radius = std::clamp(field_number(L, at, "radius", 1.0f), 0.1f, 50.0f);
            m.strength = std::clamp(field_number(L, at, "strength", 1.0f), 0.0f, 4.0f);
            m.height = std::clamp(field_number(L, at, "height", 0.0f), 0.0f, 30.0f);
            const std::string shape = field_text(L, at, "shape");
            if (shape == "square")
                m.shape = 1.0f;
            else if (!shape.empty() && shape != "round")
                return luaL_error(L, "vana.world.marks: entry %d's shape isn't 'round' or 'square'", i);
            const std::string glyph = field_text(L, at, "glyph");
            if (!glyph.empty()) {
                static const char* const kGlyphs[] = {"A", "B", "C", "D", "1", "2", "3", "4"};
                for (int g = 0; g < 8; ++g)
                    if (glyph == kGlyphs[g] || (glyph.size() == 1 && std::toupper(static_cast<unsigned char>(glyph[0])) == kGlyphs[g][0]))
                        m.glyph = static_cast<float>(g + 1);
                if (m.glyph == 0.0f)
                    return luaL_error(L, "vana.world.marks: entry %d's glyph isn't A, B, C, D, 1, 2, 3 or 4", i);
                m.glyph_size = std::clamp(field_number(L, at, "glyph_size", 0.8f), 0.1f, 10.0f);
                m.glyph_height = std::clamp(field_number(L, at, "glyph_height", 0.0f), 0.0f, 30.0f);
            }
            const std::string colour = field_text(L, at, "color");
            if (!colour.empty()) {
                const char* hex = colour[0] == '#' ? colour.c_str() + 1 : colour.c_str();
                char* end = nullptr;
                const unsigned long rgb = std::strtoul(hex, &end, 16);
                if (end == hex || *end != '\0' || end - hex != 6)
                    return luaL_error(L, "vana.world.marks: entry %d's color isn't '#RRGGBB'", i);
                m.r = static_cast<float>(rgb >> 16 & 0xFF) / 255.0f;
                m.g = static_cast<float>(rgb >> 8 & 0xFF) / 255.0f;
                m.b = static_cast<float>(rgb & 0xFF) / 255.0f;
            }
            marks.push_back(m);
            lua_pop(L, 1);
        }
    }
    groundmarks::set(lua_api::context(L).name, marks);
    return 0;
}

// vana.world.name_at(index) -> x, top, bottom | nil: where the name over target `index` is on the screen, in the interface's
// pixels (vana.ui.screen's), for an addon drawing by it with its own drawing: a mark over a monster sits where the game puts
// its name, not at the top of its model's box (a dragon's wings are far above its head). The top and bottom are the
// letters' cells as the game drew them within the last moments (nameplates' name_anchor, in its layout space, which the
// frame stretches over the window); the x is the target's own place on the screen, which its name is centred on (the
// anchor's middle isn't the drawn text's: a player's icon and the sharp names' own spacing move it). nil while the name
// wasn't drawn lately (far off, hidden, or the names aren't VanaCore's to see), or the target isn't there.
int world_name_at(lua_State* L)
{
    const lua_Integer index = luaL_checkinteger(L, 1);
    float x = 0, top = 0, bottom = 0;
    game::Entity entity;
    if (index < 0 || !game::entity(static_cast<unsigned>(index), entity) ||
        !name_on_screen(static_cast<unsigned>(index), entity.x, entity.y, entity.z, x, top, bottom))
        return 0;
    lua_pushnumber(L, x);
    lua_pushnumber(L, top);
    lua_pushnumber(L, bottom);
    return 3;
}

constexpr luaL_Reg kFunctions[] = {{"attach", world_attach},       {"place", world_place},     {"update", world_update},
                                   {"placement", world_placement}, {"detach", world_detach},   {"to_screen", world_to_screen},
                                   {"visible", world_visible},     {"marks", world_marks},     {"name_at", world_name_at},
                                   {"pick", world_pick},           {nullptr, nullptr}};

////////////////////////////////////////////////////////////////////////////////////////////////////
// Drawing

// The name's side the box sits on: where the box's own point is, and the point on the name it goes to
struct Anchoring {
    float name_x, name_y; // across (0 left to 1 right) and down (0 top to 1 bottom) of the name
    float box_x, box_y;   // ... and of the box
};

Anchoring anchoring(Side side)
{
    switch (side) {
    case Side::below:
        return {0.5f, 1.0f, 0.5f, 0.0f};
    case Side::left:
        return {0.0f, 0.5f, 1.0f, 0.5f};
    case Side::right:
        return {1.0f, 0.5f, 0.0f, 0.5f};
    case Side::over:
        return {0.5f, 0.5f, 0.5f, 0.5f};
    default:
        return {0.5f, 0.0f, 0.5f, 1.0f};
    }
}

} // namespace

void push(lua_State* L)
{
    lua_newtable(L);
    luaL_register(L, nullptr, kFunctions);
}

bool any()
{
    prune();
    return !g_places.empty();
}

bool any_world()
{
    prune();
    return std::any_of(g_places.begin(), g_places.end(), in_world);
}

bool attached(unsigned index)
{
    return std::any_of(g_places.begin(), g_places.end(),
                       [index](const Place& place) { return !place.world && place.index == index; });
}

bool name_on_screen(unsigned index, float x, float y, float z, float& across, float& top, float& bottom)
{
    float ax = 0, name_top = 0, name_bottom = 0; // the anchor first: most entities have no name drawn lately
    if (!nameplates::name_anchor(index, ax, name_top, name_bottom))
        return false;
    nameplates::Pass pass;
    unsigned width = 0, height = 0;
    ui::screen(width, height);
    Camera camera;
    Projected at;
    if (width == 0 || height == 0 || !nameplates::pass(pass) || !read_camera(camera) || !project(camera, x, y, z, at))
        return false;
    across = at.fx * static_cast<float>(width);
    top = (name_top + 0.5f) / pass.game_h * static_cast<float>(height);
    bottom = (name_bottom + 0.5f) / pass.game_h * static_cast<float>(height);
    return true;
}

void draw_at_name(unsigned index, const nameplates::Slot& name)
{
    nameplates::Pass pass;
    if (!nameplates::pass(pass))
        return;
    const Kept kept(pass.device);
    if (!kept.ok)
        return;
    for (const Place& place : g_places) {
        if (place.world || place.index != index)
            continue;
        const ui::Box* const box = prepared(place, pass.device);
        if (box == nullptr)
            continue;
        const float scale = place.scale * (place.follow ? name.size : 1.0f);
        const Anchoring at = anchoring(place.side);
        // the name's edges on the screen (the game's pixel p is at (p + 0.5) of its pixels from the left)
        const float left = (name.left + 0.5f) * pass.kx, right = (name.right + 0.5f) * pass.kx;
        const float top = (name.top + 0.5f) * pass.ky, bottom = (name.bottom + 0.5f) * pass.ky;
        const float x = left + (right - left) * at.name_x + place.dx - at.box_x * static_cast<float>(box->sprite.width()) * scale;
        const float y = top + (bottom - top) * at.name_y + place.dy - at.box_y * static_cast<float>(box->sprite.height()) * scale;
        set_state(pass.device, place.walls);
        draw_quad(pass, *box, crisp(x, scale), crisp(y, scale), scale, name.z, name.rhw);
    }
}

namespace {

// The widget watch, for a widget in the world that flashes for a frame now and then (a beam of light seen doubled, or
// brighter, once in some hundreds of frames): between two of the interface's frames (draw_over_ui), how many times the
// names' pass drew the widgets, how many it drew, the sizes it drew them at and the camera it placed them by. A frame
// unlike the one before goes to the log (a few a minute at most).
struct Watch {
    unsigned passes = 0, skipped = 0, drawn = 0, drawn_before = 0;
    float game_w = 0.0f, game_h = 0.0f, kx = 0.0f, ky = 0.0f;
    float view[16] = {};
    bool resized = false;
    unsigned lines = 0;
    ULONGLONG minute = 0;
};
Watch g_watch;
bool g_world_drawn = false; // the widgets in the world drawn since the last Present (on_present)
bool g_watch_on = false;    // [graphics] widgets.log (set_watch)

void watch_pass(const nameplates::Pass& pass, const Camera& camera)
{
    Watch& w = g_watch;
    ++w.passes;
    if (w.game_w != 0.0f && (pass.game_w != w.game_w || pass.game_h != w.game_h || pass.kx != w.kx || pass.ky != w.ky))
        w.resized = true;
    w.game_w = pass.game_w, w.game_h = pass.game_h, w.kx = pass.kx, w.ky = pass.ky;
    std::memcpy(w.view, camera.view, sizeof w.view);
}

void watch_frame()
{
    Watch& w = g_watch;
    if (!g_watch_on) {
        w.passes = 0, w.skipped = 0, w.drawn = 0, w.drawn_before = 0, w.game_w = 0.0f, w.resized = false;
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (now - w.minute > 60000)
        w.minute = now, w.lines = 0;
    Camera camera;
    const bool moved = w.passes != 0 && read_camera(camera) && std::memcmp(camera.view, w.view, sizeof w.view) != 0;
    const bool odd = any_world() && (w.passes != 1 || w.skipped != 0 || w.resized || moved ||
                                     (w.drawn_before != 0 && w.drawn != w.drawn_before));
    if (odd && w.lines < 20) {
        ++w.lines;
        log::write(L"Widgets: watch: the names' pass drew the widgets %u times this frame (%u more walks left out), %u boxes "
                   L"(%u the frame before)%ls%ls; the game's pixels %.0fx%.0f, kx %.4f ky %.4f",
                   w.passes, w.skipped, w.drawn, w.drawn_before, w.resized ? L", at other sizes than the frame before" : L"",
                   moved ? L", the camera moved between the names' pass and the interface" : L"", w.game_w, w.game_h, w.kx,
                   w.ky);
    }
    w.drawn_before = w.passes != 0 ? w.drawn : w.drawn_before;
    w.passes = 0, w.skipped = 0, w.drawn = 0, w.resized = false;
}

} // namespace

void on_present() { g_world_drawn = false; }

void set_watch(bool on) { g_watch_on = on; }

void draw_world()
{
    nameplates::Pass pass;
    Camera camera;
    if (g_world_drawn) { // a second walk of the names this frame (on_present)
        ++g_watch.skipped;
        return;
    }
    if (g_places.empty() || !nameplates::pass(pass) || !read_camera(camera))
        return;
    const Kept kept(pass.device);
    if (!kept.ok)
        return;
    g_world_drawn = true;
    watch_pass(pass, camera);
    for (const Place& place : g_places) {
        if (!in_world(place))
            continue;
        if (place.facing != Facing::camera) {
            if (const ui::Box* const box = prepared(place, pass.device))
                g_watch.drawn += draw_laid(pass, camera, place, *box) ? 1 : 0;
            continue;
        }
        Projected at;
        if (!project(camera, place.x, place.y, place.z - place.up, at) || (place.range > 0.0f && at.depth > place.range))
            continue;
        const ui::Box* const box = prepared(place, pass.device);
        if (box == nullptr)
            continue;
        const float scale = place.scale * (place.distance > 0.0f ? std::clamp(place.distance / at.depth, kNearestScale, kFarthestScale) : 1.0f);
        // the point on the screen: its share of the game's pixels, then the scene's shown ones
        const float sx = (at.fx * pass.game_w + 0.5f) * pass.kx, sy = (at.fy * pass.game_h + 0.5f) * pass.ky;
        const float x = sx + place.dx - place.pivot_x * static_cast<float>(box->sprite.width()) * scale;
        const float y = sy + place.dy - place.pivot_y * static_cast<float>(box->sprite.height()) * scale;
        vanacore_gfx_label(kLabelBase | static_cast<std::uint32_t>(place.box & 0xFFFFFFF));
        set_state(pass.device, place.walls);
        draw_quad(pass, *box, crisp(x, scale), crisp(y, scale), scale, nearer_z(camera, place, at.z), at.rhw);
        vanacore_gfx_label(0);
        ++g_watch.drawn;
    }
}

void draw_over_ui(void* device)
{
    watch_frame();
    Camera camera;
    unsigned width = 0, height = 0;
    ui::screen(width, height);
    if (g_places.empty() || width == 0 || hideui::hidden() || !read_camera(camera))
        return;
    for (const Place& place : g_places) {
        if (!place.world || in_world(place))
            continue;
        Projected at;
        if (!project(camera, place.x, place.y, place.z - place.up, at) || (place.range > 0.0f && at.depth > place.range))
            continue;
        // hidden by the zone's collision, as far as it goes (nothing known: shown)
        if (place.walls && clear_sight(camera, place.x, place.y, place.z - place.up) == 0)
            continue;
        const ui::Box* const box = prepared(place, device);
        if (box == nullptr)
            continue;
        const float scale = place.scale * (place.distance > 0.0f ? std::clamp(place.distance / at.depth, kNearestScale, kFarthestScale) : 1.0f);
        const float sx = at.fx * static_cast<float>(width), sy = at.fy * static_cast<float>(height);
        const float x = sx + place.dx - place.pivot_x * static_cast<float>(box->sprite.width()) * scale;
        const float y = sy + place.dy - place.pivot_y * static_cast<float>(box->sprite.height()) * scale;
        box->sprite.draw(device, static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)), box_alpha(*box), scale);
    }
}

} // namespace vanacore::widgets
