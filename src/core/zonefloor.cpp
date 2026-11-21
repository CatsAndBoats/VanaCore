#include "zonefloor.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "dat_buf.hpp"
#include "dats.hpp"

namespace vanacore::zonefloor {
namespace {

// The zone's model DAT (xi-tools docs/zone/format.md, subareas.md, collision.md): sections with a 16-byte header each
// (a name, then the type in the low 7 bits of a u32 and the size in 16-byte units in the next 19). The RID section
// (0x36, "RID"): offsets to its tables, the first at +0x10, a count there, 12 bytes on, then that many 0x40-byte
// boxes: the centre (3 floats, x, height, y as the game draws), the turn about the up axis at +0x10, the full size
// (3 floats) at +0x18, its id at +0x24 (four letters, 'z' first for a zone line) and the line it leads to at +0x28
// (0: an entrance, where one arrives).
//
// The ZoneDef section (0x1C), body XORed with 0xFF in a stream the key table of FFXiMain.dll drives (only when its
// mode, the top byte of its first u32, is over 0x1A), holds the collision: at +0x08 the offset of a header whose
// +0x08 and +0x0C say how many (matrix, mesh) pairs there are and where; each a u32 matrix offset (0xC0 bytes:
// to world, 16 floats, then to collision space) and a u32 mesh offset (a position pool, a normal pool, an index
// buffer, a u16 triangle count and flags); each triangle 8 bytes, four u16: three vertex indexes (the first
// & 0x7FFF, the others & 0x3FFF) and a normal's, whose top nibbles make a material word: a wall when it has 0x40.
// World coordinates are the game's: y counts down.
constexpr std::uint32_t kRid = 0x36;
constexpr std::uint32_t kZoneDef = 0x1C;
constexpr std::size_t kBox = 0x40;
constexpr int kSamples = 7;           // places along each line's middle
constexpr float kRange = 0.5f;        // how far past the box's top and bottom a floor still counts
constexpr float kUnder = 1.0f;        // the zones' boxes start about this far under their ground
constexpr float kFacingUp = -0.5f;    // a floor's normal's y (it counts down) is at most this, about 60 degrees from flat
constexpr float kEdge = 0.5f;         // a mesh's bounds past a place and the place is still looked at
constexpr std::size_t kTableAt = 0x30000; // where in FFXiMain.dll its key table is looked for from
constexpr std::uint8_t kTableSig[4] = {0xE2, 0xE5, 0x06, 0xA9};

using Buf = vanacore::dat_buf::Buf;

// The first section of `type`: where it starts and its length (the header's included).
bool find_section(const std::vector<std::uint8_t>& dat, std::uint32_t type, std::size_t& at_out, std::size_t& length_out)
{
    const Buf b{dat.data(), dat.size()};
    for (std::size_t at = 0; at + 16 <= dat.size();) {
        const std::uint32_t info = b.u32(at + 4);
        const std::size_t length = static_cast<std::size_t>(info >> 7 & 0x7FFFF) * 16;
        if (length < 16 || at + length > dat.size())
            break;
        if ((info & 0x7F) == type) {
            at_out = at;
            length_out = length;
            return true;
        }
        at += length;
    }
    return false;
}

// The ZoneDef section's body decrypted in place (a copy of the section: `ds` is where its data starts in it,
// the section's end its buffer's).
bool decrypt(std::vector<std::uint8_t>& s, std::size_t ds, const std::uint8_t* table1)
{
    const Buf b{s.data(), s.size()};
    const std::uint32_t meta = b.u32(ds), second = b.u32(ds + 4);
    if ((meta >> 24) <= 0x1A)
        return true; // not encrypted
    if (table1 == nullptr)
        return false;
    std::size_t length = meta & 0xFFFFFF;
    std::size_t p = ds + 8;
    if (p > s.size())
        return false;
    length = std::min(length, s.size() - p);
    unsigned key = table1[(second >> 24 & 0xFF) ^ 0xFF], counter = 0;
    std::size_t consumed = 0;
    while (consumed < length) {
        const std::size_t run = ((key >> 4) & 7) + 16;
        if ((key & 1) != 0 && consumed + run < length) {
            for (std::size_t i = 0; i < run; ++i)
                s[p + i] ^= 0xFF;
        }
        p += run;
        consumed += run;
        key += ++counter;
    }
    return true;
}

// A mesh's bounds in its own space, from the vertices its triangles use.
struct Mesh {
    bool ok = false;
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
};

Mesh measure(const Buf& b, std::size_t ds, std::uint32_t mesh_rel)
{
    Mesh m;
    const std::size_t mesh = ds + mesh_rel;
    if (!b.in(mesh, 0x10))
        return m;
    const std::size_t pos = ds + b.u32(mesh), index = ds + b.u32(mesh + 8);
    const unsigned count = b.u16(mesh + 0xC);
    for (unsigned t = 0; t < count; ++t) {
        const std::size_t rec = index + static_cast<std::size_t>(t) * 8;
        if (!b.in(rec, 8))
            break;
        for (int k = 0; k < 3; ++k) {
            const std::size_t v = pos + static_cast<std::size_t>(b.u16(rec + 2 * k) & (k == 0 ? 0x7FFF : 0x3FFF)) * 12;
            if (!b.in(v, 12))
                continue;
            for (int a = 0; a < 3; ++a) {
                const float c = b.f32(v + 4 * a);
                if (!(c > -1e6f && c < 1e6f))
                    continue;
                if (!m.ok)
                    m.lo[a] = m.hi[a] = c;
                m.lo[a] = std::min(m.lo[a], c);
                m.hi[a] = std::max(m.hi[a], c);
            }
            m.ok = true;
        }
    }
    return m;
}

struct Sample {
    float x, z;      // where, in the game's world
    float lo, hi;    // the heights a floor counts between (they count down)
    float best = 0;  // the highest floor found: what the player's feet land on (smallest y)
    bool found = false;
    unsigned line;
};

void world(const float* m, const float* p, float* out)
{
    out[0] = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
    out[1] = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
    out[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
}

} // namespace

void read_lines(const std::vector<std::uint8_t>& dat, unsigned max, std::vector<float>& lines)
{
    lines.clear();
    std::size_t at = 0, length = 0;
    if (!find_section(dat, kRid, at, length))
        return;
    const Buf b{dat.data(), dat.size()};
    const std::size_t ds = at + 16;
    if (length < 16 + 0x14 || std::memcmp(dat.data() + ds, "RID", 3) != 0)
        return;
    const std::size_t table = ds + b.u32(ds + 0x10);
    if (table + 16 > at + length)
        return;
    const std::uint32_t count = b.u32(table);
    for (std::uint32_t i = 0; i < count && lines.size() / kStride < max; ++i) {
        const std::size_t e = table + 16 + static_cast<std::size_t>(i) * kBox;
        if (e + kBox > at + length)
            break;
        if (dat[e + 0x24] != 'z' || b.u32(e + 0x28) == 0)
            continue;
        float box[kStride] = {b.f32(e),        b.f32(e + 4),    b.f32(e + 8),    b.f32(e + 0x10),
                              b.f32(e + 0x18), b.f32(e + 0x1C), b.f32(e + 0x20), 0.0f};
        bool sane = true;
        for (int k = 0; k < 7; ++k)
            sane = sane && box[k] == box[k] && box[k] > -1e5f && box[k] < 1e5f;
        if (!sane)
            continue;
        box[7] = -0.5f * box[5] + kUnder; // the ground a unit over the box's bottom
        lines.insert(lines.end(), box, box + kStride);
    }
}

unsigned find_floors(const std::vector<std::uint8_t>& dat, const std::uint8_t* table1, std::vector<float>& lines)
{
    const unsigned count = static_cast<unsigned>(lines.size() / kStride);
    std::size_t at = 0, length = 0;
    if (count == 0 || !find_section(dat, kZoneDef, at, length) || length < 16 + 0x10)
        return 0;
    std::vector<std::uint8_t> section(dat.begin() + static_cast<std::ptrdiff_t>(at),
                                      dat.begin() + static_cast<std::ptrdiff_t>(at + length));
    const std::size_t ds = 16;
    if (!decrypt(section, ds, table1))
        return 0;
    const Buf b{section.data(), section.size()};
    const std::uint32_t collision = b.u32(ds + 8);
    if (collision == 0)
        return 0;
    const std::size_t head = ds + collision;
    const std::uint32_t pair_count = b.u32(head + 8);
    std::size_t p = ds + b.u32(head + 0xC);

    // seven places along each line's middle, down its longer side
    std::vector<Sample> samples;
    for (unsigned i = 0; i < count; ++i) {
        const float* l = &lines[static_cast<std::size_t>(i) * kStride];
        const float c = std::cos(l[3]), s = std::sin(l[3]);
        const bool swap = l[6] > l[4];
        const float axis[2] = {swap ? s : c, swap ? c : -s};
        const float half = 0.5f * (swap ? l[6] : l[4]);
        for (int k = 0; k < kSamples; ++k) {
            const float along = -half + 2.0f * half * (static_cast<float>(k) + 0.5f) / kSamples;
            Sample sm;
            sm.x = l[0] + axis[0] * along;
            sm.z = l[2] + axis[1] * along;
            sm.lo = l[1] - 0.5f * l[5] - kRange;
            sm.hi = l[1] + 0.5f * l[5] + kRange;
            sm.line = i;
            samples.push_back(sm);
        }
    }

    std::unordered_map<std::uint32_t, Mesh> meshes;
    std::unordered_map<std::uint64_t, bool> seen;
    std::vector<unsigned> reach;
    for (std::uint32_t g = 0; g < pair_count && b.in(p, 4); ++g) {
        const std::uint32_t group = b.u32(p) & 0x7FF;
        p += 4;
        for (std::uint32_t j = 0; j < group && b.in(p, 8); ++j, p += 8) {
            const std::uint32_t matrix_rel = b.u32(p), mesh_rel = b.u32(p + 4);
            if (!seen.emplace(static_cast<std::uint64_t>(matrix_rel) << 32 | mesh_rel, true).second)
                continue;
            if (!b.in(ds + matrix_rel, 0x80))
                continue;
            auto found = meshes.find(mesh_rel);
            if (found == meshes.end())
                found = meshes.emplace(mesh_rel, measure(b, ds, mesh_rel)).first;
            const Mesh& m = found->second;
            if (!m.ok)
                continue;
            float to_world[16], to_collision[16];
            for (int k = 0; k < 16; ++k) {
                to_world[k] = b.f32(ds + matrix_rel + 4 * static_cast<std::size_t>(k));
                to_collision[k] = b.f32(ds + matrix_rel + 0x40 + 4 * static_cast<std::size_t>(k));
            }
            // the mesh's bounds in the world, and which places they reach
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (int corner = 0; corner < 8; ++corner) {
                const float c[3] = {corner & 1 ? m.hi[0] : m.lo[0], corner & 2 ? m.hi[1] : m.lo[1],
                                    corner & 4 ? m.hi[2] : m.lo[2]};
                float w[3];
                world(to_world, c, w);
                for (int a = 0; a < 3; ++a) {
                    lo[a] = std::min(lo[a], w[a]);
                    hi[a] = std::max(hi[a], w[a]);
                }
            }
            reach.clear();
            for (unsigned k = 0; k < samples.size(); ++k) {
                const Sample& sm = samples[k];
                if (sm.x >= lo[0] - kEdge && sm.x <= hi[0] + kEdge && sm.z >= lo[2] - kEdge && sm.z <= hi[2] + kEdge &&
                    hi[1] >= sm.lo && lo[1] <= sm.hi)
                    reach.push_back(k);
            }
            if (reach.empty())
                continue;

            const std::size_t mesh = ds + mesh_rel;
            const std::size_t pos = ds + b.u32(mesh), normals = ds + b.u32(mesh + 4), index = ds + b.u32(mesh + 8);
            const unsigned triangles = b.u16(mesh + 0xC);
            for (unsigned t = 0; t < triangles; ++t) {
                const std::size_t rec = index + static_cast<std::size_t>(t) * 8;
                if (!b.in(rec, 8))
                    break;
                const std::uint16_t r0 = b.u16(rec), r1 = b.u16(rec + 2), r2 = b.u16(rec + 4), rd = b.u16(rec + 6);
                const unsigned material = (r0 >> 12) << 12 | (r1 >> 12) << 8 | (r2 >> 12) << 4 | (rd >> 12);
                if (material & 0x40)
                    continue; // a wall
                const std::size_t vi[3] = {pos + static_cast<std::size_t>(r0 & 0x7FFF) * 12,
                                           pos + static_cast<std::size_t>(r1 & 0x3FFF) * 12,
                                           pos + static_cast<std::size_t>(r2 & 0x3FFF) * 12};
                const std::size_t ni = normals + static_cast<std::size_t>(rd & 0x7FFF) * 12;
                if (!b.in(vi[0], 12) || !b.in(vi[1], 12) || !b.in(vi[2], 12) || !b.in(ni, 12))
                    continue;
                // its normal to the world: by the transpose of the matrix to collision space's rotation
                const float n[3] = {b.f32(ni), b.f32(ni + 4), b.f32(ni + 8)};
                const float wx = to_collision[0] * n[0] + to_collision[1] * n[1] + to_collision[2] * n[2];
                const float wy = to_collision[4] * n[0] + to_collision[5] * n[1] + to_collision[6] * n[2];
                const float wz = to_collision[8] * n[0] + to_collision[9] * n[1] + to_collision[10] * n[2];
                const float length_n = std::sqrt(wx * wx + wy * wy + wz * wz);
                if (!(length_n > 1e-9f) || wy / length_n > kFacingUp)
                    continue;
                float v[3][3];
                for (int k = 0; k < 3; ++k) {
                    const float local[3] = {b.f32(vi[k]), b.f32(vi[k] + 4), b.f32(vi[k] + 8)};
                    world(to_world, local, v[k]);
                }
                const float d0x = v[1][0] - v[0][0], d0z = v[1][2] - v[0][2];
                const float d1x = v[2][0] - v[0][0], d1z = v[2][2] - v[0][2];
                const float den = d0x * d1z - d1x * d0z;
                if (std::fabs(den) < 1e-9f)
                    continue;
                for (unsigned k : reach) {
                    Sample& sm = samples[k];
                    const float qx = sm.x - v[0][0], qz = sm.z - v[0][2];
                    const float u = (qx * d1z - d1x * qz) / den, w = (d0x * qz - qx * d0z) / den;
                    if (u < -1e-4f || w < -1e-4f || u + w > 1.0001f)
                        continue;
                    const float y = v[0][1] + u * (v[1][1] - v[0][1]) + w * (v[2][1] - v[0][1]);
                    if (y < sm.lo || y > sm.hi)
                        continue;
                    // Where floors stack in a box the player walks on the top one (Al'Taieu: a floor at the walking
                    // height, the zone's own ground 3 to 4 yalms under it); the box's bottom doesn't say which.
                    if (!sm.found || y < sm.best) {
                        sm.best = y;
                        sm.found = true;
                    }
                }
            }
        }
        p += 4; // the group's terminator
    }

    // each line's: the middle of what its places found
    unsigned got = 0;
    for (unsigned i = 0; i < count; ++i) {
        float ys[kSamples];
        int n = 0;
        for (const Sample& sm : samples)
            if (sm.line == i && sm.found)
                ys[n++] = sm.best;
        if (n == 0)
            continue;
        std::sort(ys, ys + n);
        float* l = &lines[static_cast<std::size_t>(i) * kStride];
        l[7] = l[1] - ys[n / 2]; // above the middle: the ground's y counts down
        ++got;
    }
    return got;
}

namespace {

// The collision mesh at section offset mesh_rel: its floors (its walls too if `walls`) onto out's triangles, as a mesh of
// out's if it has any (true)
bool read_mesh(const Buf& b, std::size_t ds, std::uint32_t mesh_rel, Floors& out, bool walls)
{
    const std::uint32_t first = static_cast<std::uint32_t>(out.terrain.size());
    const std::size_t m = ds + mesh_rel;
    const std::size_t pos = ds + b.u32(m), index = ds + b.u32(m + 8);
    const unsigned triangles = b.in(m, 0x10) ? b.u16(m + 0xC) : 0u;
    for (unsigned t = 0; t < triangles; ++t) {
        const std::size_t rec = index + static_cast<std::size_t>(t) * 8;
        if (!b.in(rec, 8))
            break;
        const std::uint16_t r0 = b.u16(rec), r1 = b.u16(rec + 2), r2 = b.u16(rec + 4), rd = b.u16(rec + 6);
        const unsigned material = (r0 >> 12) << 12 | (r1 >> 12) << 8 | (r2 >> 12) << 4 | (rd >> 12);
        const bool wall = (material & 0x40) != 0;
        if (wall && !walls)
            continue;
        const std::size_t vi[3] = {pos + static_cast<std::size_t>(r0 & 0x7FFF) * 12, pos + static_cast<std::size_t>(r1 & 0x3FFF) * 12,
                                   pos + static_cast<std::size_t>(r2 & 0x3FFF) * 12};
        if (!b.in(vi[0], 12) || !b.in(vi[1], 12) || !b.in(vi[2], 12))
            continue;
        float v[3][3];
        bool sane = true;
        for (int k = 0; k < 3; ++k)
            for (int a = 0; a < 3; ++a) {
                v[k][a] = b.f32(vi[k] + 4 * static_cast<std::size_t>(a));
                sane = sane && v[k][a] > -1e5f && v[k][a] < 1e5f;
            }
        if (!sane)
            continue;
        for (int k = 0; k < 3; ++k)
            out.tris.insert(out.tris.end(), v[k], v[k] + 3);
        // the terrain: the top bit of each of the four words (xi-tools docs/zone/collision.md)
        out.terrain.push_back(
            static_cast<std::uint8_t>((r0 >> 15) | (r1 >> 15) << 1 | (r2 >> 15) << 2 | (rd >> 15) << 3 | (wall ? kWall : 0)));
    }
    if (out.terrain.size() == first)
        return false;
    if (out.mesh_first.empty())
        out.mesh_first.push_back(first);
    out.mesh_first.push_back(static_cast<std::uint32_t>(out.terrain.size()));
    return true;
}

} // namespace

bool read_floors(const std::vector<std::uint8_t>& dat, const std::uint8_t* table1, Floors& out, bool walls)
{
    out = Floors{};
    std::size_t at = 0, length = 0;
    if (!find_section(dat, kZoneDef, at, length) || length < 16 + 0x10)
        return false;
    std::vector<std::uint8_t> section(dat.begin() + static_cast<std::ptrdiff_t>(at),
                                      dat.begin() + static_cast<std::ptrdiff_t>(at + length));
    const std::size_t ds = 16;
    if (!decrypt(section, ds, table1))
        return false;
    const Buf b{section.data(), section.size()};
    const std::uint32_t collision = b.u32(ds + 8);
    if (collision == 0)
        return false;
    const std::size_t head = ds + collision;
    const std::uint32_t pair_count = b.u32(head + 8);
    std::size_t p = ds + b.u32(head + 0xC);

    std::unordered_map<std::uint32_t, std::uint32_t> mesh_of, matrix_of; // section offset -> index (UINT32_MAX: no floors)
    std::unordered_map<std::uint64_t, bool> seen;
    for (std::uint32_t g = 0; g < pair_count && b.in(p, 4); ++g) {
        const std::uint32_t group = b.u32(p) & 0x7FF;
        p += 4;
        for (std::uint32_t j = 0; j < group && b.in(p, 8); ++j, p += 8) {
            const std::uint32_t matrix_rel = b.u32(p), mesh_rel = b.u32(p + 4);
            if (!seen.emplace(static_cast<std::uint64_t>(matrix_rel) << 32 | mesh_rel, true).second)
                continue;
            if (!b.in(ds + matrix_rel, 0x40))
                continue;
            // the mesh's floors, the first time it's placed
            auto mesh = mesh_of.find(mesh_rel);
            if (mesh == mesh_of.end()) {
                const bool any = read_mesh(b, ds, mesh_rel, out, walls);
                mesh = mesh_of.emplace(mesh_rel, any ? static_cast<std::uint32_t>(out.mesh_first.size() - 2) : UINT32_MAX).first;
            }
            if (mesh->second == UINT32_MAX)
                continue;
            auto matrix = matrix_of.find(matrix_rel);
            if (matrix == matrix_of.end()) {
                float w[16];
                bool sane = true;
                for (int k = 0; k < 16; ++k) {
                    w[k] = b.f32(ds + matrix_rel + 4 * static_cast<std::size_t>(k));
                    sane = sane && w[k] > -1e6f && w[k] < 1e6f;
                }
                if (!sane)
                    continue;
                // column-major (zonefloor's world()): each axis's three floats, then the origin's
                const float m12[12] = {w[0], w[1], w[2], w[4], w[5], w[6], w[8], w[9], w[10], w[12], w[13], w[14]};
                out.matrices.insert(out.matrices.end(), m12, m12 + 12);
                matrix = matrix_of.emplace(matrix_rel, static_cast<std::uint32_t>(out.matrices.size() / 12 - 1)).first;
            }
            out.pairs.push_back(matrix->second);
            out.pairs.push_back(mesh->second);
        }
        p += 4; // the group's terminator
    }
    return !out.pairs.empty();
}

int fixed_light(const std::vector<std::uint8_t>& dat)
{
    constexpr std::uint32_t kEnvironment = 0x2F;
    const Buf b{dat.data(), dat.size()};
    int found = -1;
    std::size_t at = 0;
    while (at + 16 <= dat.size()) {
        const std::uint32_t info = b.u32(at + 4);
        const std::size_t length = static_cast<std::size_t>(info >> 7 & 0x7FFFF) * 16;
        if (length < 16 || at + length > dat.size())
            break;
        if ((info & 0x7F) == kEnvironment && length >= 16 + 4) {
            if ((b.u32(at + 16) & 1u) == 0)
                return 0; // one lit by the sky is enough
            found = 1;
        }
        at += length;
    }
    return at + 16 <= dat.size() ? -1 : found; // a walk cut short may have missed one lit by the sky
}

bool key_table(const std::vector<std::uint8_t>& dll, std::uint8_t out[256])
{
    if (dll.size() < kTableAt + 4)
        return false;
    const auto from = dll.begin() + static_cast<std::ptrdiff_t>(kTableAt);
    const auto at = std::search(from, dll.end(), kTableSig, kTableSig + 4);
    if (at == dll.end() || static_cast<std::size_t>(dll.end() - at) < 256)
        return false;
    std::memcpy(out, &*at, 256);
    return true;
}

const std::uint8_t* game_key_table()
{
    static std::mutex lock;
    static bool tried = false, have = false;
    static std::uint8_t table[256];
    const std::lock_guard guard(lock);
    if (!tried) {
        const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
        if (game == nullptr)
            return nullptr; // the game isn't loaded yet: looked for again next time
        tried = true;
        wchar_t path[MAX_PATH] = L"";
        std::vector<std::uint8_t> dll;
        have = GetModuleFileNameW(game, path, MAX_PATH) != 0 && dats::read_file(path, dll) && key_table(dll, table);
    }
    return have ? table : nullptr;
}

} // namespace vanacore::zonefloor
