#include "zonemesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <unordered_map>
#include <utility>

#include "dat_buf.hpp"
#include "dat_textures.hpp"

namespace vanacore::zonemesh {
namespace {

constexpr std::uint32_t kZoneDef = 0x1C, kZoneMesh = 0x2E;
constexpr std::size_t kRecord = 0x64;     // a placement (retail's; ROM/0's prototypes have 0x54 and aren't encrypted or read)
constexpr std::size_t kTableAt = 0x30000; // where in FFXiMain.dll its key tables are looked for from
constexpr std::uint8_t kTable2Sig[4] = {0xB8, 0xC5, 0xF7, 0x84};
constexpr float kCell = 64.0f; // the meshes are split by where they're placed, cells this wide, for the maps to skip

using Buf = vanacore::dat_buf::Buf;

struct Section {
    std::size_t at, length;
    std::uint32_t type;
};

std::vector<Section> sections(const std::vector<std::uint8_t>& dat)
{
    std::vector<Section> out;
    const Buf b{dat.data(), dat.size()};
    for (std::size_t at = 0; at + 16 <= dat.size();) {
        const std::uint32_t info = b.u32(at + 4);
        const std::size_t length = static_cast<std::size_t>(info >> 7 & 0x7FFFF) * 16;
        if (length < 16 || at + length > dat.size())
            break;
        out.push_back({at, length, info & 0x7F});
        at += length;
    }
    return out;
}

// A copy of the section, its data from 16 on
std::vector<std::uint8_t> copy(const std::vector<std::uint8_t>& dat, const Section& s)
{
    return {dat.begin() + static_cast<std::ptrdiff_t>(s.at), dat.begin() + static_cast<std::ptrdiff_t>(s.at + s.length)};
}

// xi_decrypt.py decrypt_zone_objects: the XOR-0xFF stream over the body (mode over 0x1A), then the names unmasked
bool decrypt_def(std::vector<std::uint8_t>& s, const std::uint8_t* table1)
{
    constexpr std::size_t ds = 16;
    const Buf b{s.data(), s.size()};
    const std::uint32_t meta = b.u32(ds), second = b.u32(ds + 4);
    if ((meta >> 24) <= 0x1A)
        return true;
    if (table1 == nullptr || s.size() < ds + 8)
        return false;
    std::size_t length = meta & 0xFFFFFF, p = ds + 8;
    length = std::min(length, s.size() - p);
    unsigned key = table1[(second >> 24 & 0xFF) ^ 0xFF];
    for (std::size_t consumed = 0, step = 1; consumed < length; ++step) { // the key grows by 1, 2, 3...
        const std::size_t run = ((key >> 4) & 7) + 16;
        if ((key & 1) != 0 && consumed + run < length)
            for (std::size_t i = 0; i < run; ++i)
                s[p + i] ^= 0xFF;
        p += run, consumed += run;
        key += static_cast<unsigned>(step);
    }
    const std::size_t nodes = second & 0xFFFFFF;
    for (std::size_t i = 0; i < nodes; ++i) {
        const std::size_t at = ds + 0x20 + i * kRecord;
        if (at + 16 > s.size())
            break;
        for (std::size_t j = 0; j < 16; ++j)
            s[at + j] ^= 0x55;
    }
    return true;
}

// xi_decrypt.py decrypt_zone_mesh: a keyed XOR stream (mode 5 and up), then 8-byte swaps between the body's halves (+6 0xFFFF)
bool decrypt_mesh(std::vector<std::uint8_t>& s, const std::uint8_t* table1, const std::uint8_t* table2)
{
    constexpr std::size_t ds = 16;
    const Buf b{s.data(), s.size()};
    const std::uint32_t meta = b.u32(ds);
    if (s.size() < ds + 8 || (meta & 0xFFFFFF) < 8)
        return false;
    const std::size_t length = std::min<std::size_t>((meta & 0xFFFFFF) - 8, s.size() - (ds + 8));
    const unsigned index = s[ds + 5];
    if ((meta >> 24) >= 5) {
        if (table1 == nullptr)
            return false;
        unsigned key = table1[index ^ 0xF0];
        for (std::size_t i = 0; i < length; ++i) { // the key grows by 1, 2, 3... twice a byte: 2i + 1, then 2i + 2
            const unsigned step = 2 * static_cast<unsigned>(i);
            const unsigned mod = ((key & 0xFF) << 8) | (key & 0xFF);
            key += step + 1;
            s[ds + 8 + i] ^= static_cast<std::uint8_t>((mod >> (key & 7)) & 0xFF);
            key += step + 2;
        }
    }
    if (b.u16(ds + 6) == 0xFFFF) {
        if (table2 == nullptr)
            return false;
        unsigned key1 = index ^ 0xF0, key2 = table2[key1 & 0xFF];
        const std::size_t half = (length & ~static_cast<std::size_t>(0xF)) / 2;
        for (std::size_t i = 0; i < half; i += 8) {
            if (key2 & 1)
                std::swap_ranges(s.begin() + static_cast<std::ptrdiff_t>(ds + 8 + i),
                                 s.begin() + static_cast<std::ptrdiff_t>(ds + 8 + i + 8),
                                 s.begin() + static_cast<std::ptrdiff_t>(ds + 8 + i + half));
            key1 += 9, key2 += key1;
        }
    }
    return true;
}

// A mesh in its own space: each part's triangles, three corners of (x, y, z, u, v) each
struct Part {
    std::string texture;
    bool blended = false;
    std::vector<float> tris;
    std::vector<float> nrm; // 4 floats a corner beside tris': the vertex's normal (the mesh's space), and its curve (curve_of)
};
struct MeshDef {
    bool cutout = false; // its name starts with '_': the game alpha-tests it
    std::vector<Part> parts;
    std::size_t vertices = 0;
};

// How far the surface at each of a mesh's vertices is curved and smoothly shaded, 0 to 1, into the corners' nrm[3]. The game
// lights a mesh by its vertices' normals; on a rounded low-polygon shape (a rock dome, a hillside) each is the mean of the faces
// round it, a few degrees to some 30 off each of them, and the light fades smoothly across polygons that each lie flat. The sun's
// shadow maps go by the polygons: there the shadow's own edge comes out a polygon at a time, where the normals say a curve. A
// wall's vertices have its faces' own normal (0 off: flat, nothing to round), and where a building's are averaged across a corner
// (45 degrees off each wall and more) the normal is no surface's: neither is curved. So by vertex (the same place and normal,
// across the mesh's parts): the largest angle between its normal and a face round it, 1 from 5 degrees to 36, 0 under 1.5 and
// from 44. Measured (Konschtat's Crag of Dem, Southern San d'Oria's walls, 2026-11-11): a mesh's share of such vertices, or of
// smooth edges, doesn't tell the crag's meshes from the walls'; the vertices themselves do.
void curve_of(MeshDef& m)
{
    struct Key {
        std::int64_t a[6];
        bool operator==(const Key& o) const { return std::memcmp(a, o.a, sizeof a) == 0; }
    };
    struct Hash {
        std::size_t operator()(const Key& k) const
        {
            std::uint64_t h = 1469598103934665603ull;
            for (const std::int64_t v : k.a)
                h = (h ^ static_cast<std::uint64_t>(v)) * 1099511628211ull;
            return static_cast<std::size_t>(h);
        }
    };
    const auto key = [](const float* p, const float* n) {
        return Key{{std::llround(p[0] * 200.0), std::llround(p[1] * 200.0), std::llround(p[2] * 200.0), std::llround(n[0] * 50.0),
                    std::llround(n[1] * 50.0), std::llround(n[2] * 50.0)}};
    };
    // by vertex: the least cosine between its normal and a face round it; its edges' lengths, and how far along its normal the
    // vertices at their other ends lie under it (a bulge: under; a hollow: over), summed
    struct At {
        float least = 1.0f, len = 0.0f, under = 0.0f;
        int edges = 0;
    };
    std::unordered_map<Key, At, Hash> least;
    std::size_t total = 0, curved = 0;          // the mesh's corners, and those curved
    for (int pass = 0; pass < 2; ++pass)
        for (Part& part : m.parts)
            for (std::size_t t = 0; t + 15 <= part.tris.size() && (t / 5 + 3) * 4 <= part.nrm.size(); t += 15) {
                const float* p[3] = {&part.tris[t], &part.tris[t + 5], &part.tris[t + 10]};
                float* n[3] = {&part.nrm[t / 5 * 4], &part.nrm[t / 5 * 4 + 4], &part.nrm[t / 5 * 4 + 8]};
                const float e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
                const float e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
                const float f[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
                for (int c = 0; c < 3; ++c) {
                    const float nl = std::sqrt(n[c][0] * n[c][0] + n[c][1] * n[c][1] + n[c][2] * n[c][2]);
                    if (pass == 0) {
                        if (fl < 1e-9f || nl < 1e-6f)
                            continue; // (a sliver has no facing; a vertex without a normal stays at 0)
                        // (either side: a strip's triangles turn over one after the other)
                        const float cs = std::fabs(n[c][0] * f[0] + n[c][1] * f[1] + n[c][2] * f[2]) / (nl * fl);
                        At& at = least.try_emplace(key(p[c], n[c])).first->second;
                        at.least = std::min(at.least, cs);
                        for (int o = 1; o < 3; ++o) {
                            const float* q = p[(c + o) % 3];
                            const float d[3] = {q[0] - p[c][0], q[1] - p[c][1], q[2] - p[c][2]};
                            at.len += std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                            at.under -= (d[0] * n[c][0] + d[1] * n[c][1] + d[2] * n[c][2]) / nl;
                            at.edges++;
                        }
                    } else {
                        const auto it = nl < 1e-6f ? least.end() : least.find(key(p[c], n[c]));
                        const float off = it == least.end() ? 0.0f : std::acos(std::min(it->second.least, 1.0f)) * 57.29578f;
                        const auto step = [](float lo, float hi, float x) {
                            const float k = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
                            return k * k * (3.0f - 2.0f * k);
                        };
                        float& curve = part.nrm[(t / 5 + static_cast<std::size_t>(c)) * 4 + 3]; // (n[c][3])
                        curve = it == least.end() ? 0.0f : step(1.5f, 5.0f, off) * (1.0f - step(36.0f, 44.0f, off));
                        total++, curved += curve > 0.5f;
                        if (it != least.end() && off < 5.0f)
                            curve = -1.0f - curve; // (flat: settled below)
                        // the bulge's height there (Place::normal's `lift`): the mean edge's length by the mean slope its
                        // neighbours lie under it at, a fifth of the edge at most; none in a hollow. In the normal's own length
                        if (nl >= 1e-6f) {
                            float lift = 0.0f;
                            if (it != least.end() && it->second.len > 1e-6f) {
                                const At& at = it->second;
                                lift = at.len / static_cast<float>(at.edges) * std::clamp(at.under / at.len, 0.0f, 0.2f);
                            }
                            for (int k = 0; k < 3; ++k)
                                n[c][k] *= (1.0f + std::min(lift, 30.0f)) / nl;
                        }
                    }
                }
            }
    // Only a mesh that is curved nearly all over counts (the crag's dome 78% of its corners, a hillside 75 to 95; Southern San
    // d'Oria's houses 5 to 15, its main wall 38): rock and ground. Its flat vertices are among the curved ones, a facet of the
    // same rock, which the game shades with the rest (left flat they kept the maps' shadow, whole dark triangles inside the
    // ramp). A building's mesh has none: its walls lean and their normals are smoothed across the bends (curved, by the vertex,
    // over whole faces), and shaded by them a wall under a high sun went dark in bands where the maps and sun_map's edge-on fade
    // had it lit (Southern San d'Oria, 2026-11-11)
    const bool rock = total > 0 && curved * 10 >= total * 6;
    for (Part& part : m.parts)
        for (std::size_t i = 3; i < part.nrm.size(); i += 4) {
            float* n = &part.nrm[i - 3];
            if (rock) {
                if (n[3] < 0.0f)
                    n[3] = 1.0f;
                continue;
            }
            const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            for (int k = 0; k < 3; ++k)
                n[k] = l > 1e-6f ? n[k] / l : 0.0f; // (no bulge's height either)
            n[3] = 0.0f;
        }
}

// xi_export.py parse_zone_mesh_section, retail's layout (xim: the part count at +0x40, no chained groups)
bool parse_mesh(const std::vector<std::uint8_t>& s, std::string& name, MeshDef& m)
{
    constexpr std::size_t ds = 16;
    const Buf b{s.data(), s.size()};
    const unsigned config = s.size() > ds + 4 ? s[ds + 4] : 0;
    const bool strip = (config & 1) != 0;
    const std::size_t stride = (config & 2) != 0 ? 48 : 36;
    name = b.name(ds + 0x10);
    m.cutout = !name.empty() && name[0] == '_';
    if (b.u32(ds + 0x20) == 0)
        return false; // a collision model: a box, no geometry
    const std::uint32_t parts = b.u32(ds + 0x40);
    std::size_t p = ds + 0x20 + b.u32(ds + 0x3C);
    for (std::uint32_t k = 0; k < parts && k < 256; ++k) {
        if (!b.in(p, 20))
            return !m.parts.empty();
        Part part;
        part.texture = b.name(p);
        const unsigned nverts = b.u16(p + 16), flags = b.u16(p + 18);
        part.blended = (flags & 0x8000) != 0;
        const std::size_t verts = p + 20, after = verts + nverts * stride;
        if (!b.in(verts, nverts * stride + 4))
            return !m.parts.empty();
        const unsigned nidx = b.u16(after);
        const std::size_t idx = after + 4;
        if (!b.in(idx, nidx * 2u))
            return !m.parts.empty();
        auto corner = [&](unsigned i) {
            if (i >= nverts)
                return false;
            const std::size_t v = verts + i * stride, n = v + stride - 24; // (position [, a second], normal, colour, uv)
            part.tris.insert(part.tris.end(), {b.f32(v), b.f32(v + 4), b.f32(v + 8), b.f32(v + stride - 8), b.f32(v + stride - 4)});
            part.nrm.insert(part.nrm.end(), {b.f32(n), b.f32(n + 4), b.f32(n + 8), 0.0f});
            return true;
        };
        auto triangle = [&](unsigned a, unsigned c, unsigned d) {
            const std::size_t was = part.tris.size(), was_n = part.nrm.size();
            if (!(corner(a) && corner(c) && corner(d)))
                part.tris.resize(was), part.nrm.resize(was_n);
        };
        if (strip) {
            // degenerate triangles restart the strip; the winding doesn't matter to a shadow (drawn both sides)
            for (unsigned t = 0; t + 2 < nidx; ++t) {
                const unsigned i0 = b.u16(idx + t * 2u), i1 = b.u16(idx + t * 2u + 2), i2 = b.u16(idx + t * 2u + 4);
                if (i0 != i1 && i1 != i2 && i0 != i2)
                    triangle(i0, i1, i2);
            }
        } else {
            for (unsigned t = 0; t + 2 < nidx; t += 3)
                triangle(b.u16(idx + t * 2u), b.u16(idx + t * 2u + 2), b.u16(idx + t * 2u + 4));
        }
        m.vertices += nverts;
        m.parts.push_back(std::move(part));
        p = (idx + nidx * 2u + 3) & ~static_cast<std::size_t>(3);
    }
    curve_of(m);
    return !m.parts.empty();
}

// A placement's matrix (xi_export.py trs_matrix, xim's rotateZYX): translate * Rz * Ry * Rx * scale, as 3 columns and the origin
struct Place {
    float c[3][3], o[3];
    void at(const float* v, float* out) const
    {
        for (int k = 0; k < 3; ++k)
            out[k] = c[0][k] * v[0] + c[1][k] * v[1] + c[2][k] * v[2] + o[k];
    }
    // a normal: through the matrix's inverse transpose (its columns over their lengths squared: a scale, mirrored or not), unit -
    // but for what its length carries past one, the bulge's height at the vertex (curve_of), which comes out in the world's
    // units (by the mean of the three scales) the same way: 1 + height long
    void normal(const float* n, float* out) const
    {
        const float lift = std::max(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) - 1.0f, 0.0f);
        float k2[3];
        for (int i = 0; i < 3; ++i)
            k2[i] = std::max(c[i][0] * c[i][0] + c[i][1] * c[i][1] + c[i][2] * c[i][2], 1e-20f);
        for (int k = 0; k < 3; ++k)
            out[k] = c[0][k] * n[0] / k2[0] + c[1][k] * n[1] / k2[1] + c[2][k] * n[2] / k2[2];
        const float l = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
        const float to = 1.0f + lift * (std::sqrt(k2[0]) + std::sqrt(k2[1]) + std::sqrt(k2[2])) / 3.0f;
        for (int k = 0; k < 3; ++k)
            out[k] = l > 1e-12f ? out[k] / l * to : 0.0f;
    }
};

Place place_of(const Buf& b, std::size_t r)
{
    const float rx = b.f32(r + 0x1C), ry = b.f32(r + 0x20), rz = b.f32(r + 0x24);
    const float sx = std::sin(rx), sy = std::sin(ry), sz = std::sin(rz), cx = std::cos(rx), cy = std::cos(ry), cz = std::cos(rz);
    const float k[3] = {b.f32(r + 0x28), b.f32(r + 0x2C), b.f32(r + 0x30)};
    const float cols[3][3] = {{cy * cz, cy * sz, -sy},
                              {sx * sy * cz - cx * sz, sx * sy * sz + cx * cz, sx * cy},
                              {cx * sy * cz + sx * sz, cx * sy * sz - sx * cz, cx * cy}};
    Place pl{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            pl.c[i][j] = cols[i][j] * k[i];
    pl.o[0] = b.f32(r + 0x10), pl.o[1] = b.f32(r + 0x14), pl.o[2] = b.f32(r + 0x18);
    return pl;
}

// a placement's mesh: a level of detail's the most detailed there is, else its own name (xi_export.py resolve_mesh_name, the
// high one first). A placement named _m or _l is drawn by the game as its _h within its high-detail distance (+0x38): Ru'Lude
// Gardens' palace hall is placed as dai_in_m (645 triangles, its roof at y -12) within 50 of which the game draws dai_in_h (2,335,
// the hall's ceiling at -19); taken as named, the hall had no ceiling in the shadows (2026-11-04)
const MeshDef* resolve(const std::unordered_map<std::string, MeshDef>& meshes, const std::string& id)
{
    const bool lod = id.size() > 2 && id[id.size() - 2] == '_' && (id.back() == 'l' || id.back() == 'm' || id.back() == 'h');
    const std::string base = lod ? id.substr(0, id.size() - 2) : id;
    for (const char* suffix : {"_h", "_m", "_l"}) {
        const auto it = meshes.find(base + suffix);
        if (it != meshes.end())
            return &it->second;
    }
    const auto it = meshes.find(id);
    return it != meshes.end() ? &it->second : nullptr;
}

// The triangles of a cell: solid, or one cut-out texture's
struct Bucket {
    std::vector<float> v; // 5 floats a corner: x, y, z, u, v
    std::vector<float> n; // 4 floats a corner, solid buckets: its normal in the world and its curve
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
};

std::uint64_t bucket_key(const float* o, std::int32_t texture)
{
    const auto cx = static_cast<std::int32_t>(std::floor(o[0] / kCell)) & 0xFFFF;
    const auto cz = static_cast<std::int32_t>(std::floor(o[2] / kCell)) & 0xFFFF;
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(texture + 1)) << 32 | static_cast<std::uint32_t>(cx) << 16 |
           static_cast<std::uint32_t>(cz);
}

// the meshes' sections, decrypted and parsed; a name had twice keeps the one with more vertices
void read_meshes(const std::vector<std::uint8_t>& dat, const std::vector<Section>& all, const std::uint8_t* table1,
                 const std::uint8_t* table2, std::unordered_map<std::string, MeshDef>& meshes)
{
    for (const Section& sec : all) {
        if (sec.type != kZoneMesh)
            continue;
        std::vector<std::uint8_t> s = copy(dat, sec);
        std::string name;
        MeshDef m;
        if (!decrypt_mesh(s, table1, table2) || !parse_mesh(s, name, m) || name.empty())
            continue;
        const auto it = meshes.find(name);
        if (it == meshes.end() || it->second.vertices < m.vertices)
            meshes[name] = std::move(m);
    }
}

// the cut-out textures by name, decoded from the DAT once each: their index in out.textures, -1 when it hasn't it
std::int32_t texture_of(const std::vector<std::uint8_t>& dat, const std::string& name, Mesh& out,
                        std::unordered_map<std::string, std::int32_t>& known)
{
    const auto it = known.find(name);
    if (it != known.end())
        return it->second;
    std::int32_t index = -1;
    dat_textures::Header h;
    std::vector<std::uint32_t> argb;
    if (!name.empty() && dat_textures::find(dat, name, h) && dat_textures::decode(dat, h, argb) && h.width && h.height) {
        index = static_cast<std::int32_t>(out.textures.size());
        out.textures.push_back({name, h.width, h.height, std::move(argb)});
    }
    known[name] = index;
    return index;
}

// what a placement leaves out: a collision stand-in (draw distance 1.0: the client never draws it), and what moves (a door,
// '_' block id, and a lift, '@': their shadows would stay where they first stood)
int left_out(const Buf& b, std::size_t r)
{
    if (b.f32(r + 0x40) == 1.0f)
        return 1;
    const std::uint8_t block = b.in(r + 0x34, 1) ? b.p[r + 0x34] : 0;
    return block == '_' || block == '@' ? 2 : 0;
}

void lay(const MeshDef& m, const Place& pl, const std::vector<std::uint8_t>& dat, Mesh& out,
         std::unordered_map<std::uint64_t, Bucket>& buckets, std::unordered_map<std::string, std::int32_t>& textures)
{
    for (const Part& part : m.parts) {
        if (part.blended) {
            out.blended++;
            continue;
        }
        const std::int32_t texture = m.cutout ? texture_of(dat, part.texture, out, textures) : -1;
        Bucket& bk = buckets[bucket_key(pl.o, texture)];
        for (std::size_t i = 0; i + 5 <= part.tris.size(); i += 5) {
            float w[3];
            pl.at(&part.tris[i], w);
            for (int k = 0; k < 3; ++k)
                bk.lo[k] = std::min(bk.lo[k], w[k]), bk.hi[k] = std::max(bk.hi[k], w[k]);
            bk.v.insert(bk.v.end(), {w[0], w[1], w[2], part.tris[i + 3], part.tris[i + 4]});
            if (texture < 0) { // (the solid ones': Mesh::nrm goes beside Mesh::pos)
                const std::size_t c = i / 5 * 4;
                float n[3] = {0, 0, 0};
                if (c + 4 <= part.nrm.size())
                    pl.normal(&part.nrm[c], n);
                bk.n.insert(bk.n.end(), {n[0], n[1], n[2], c + 4 <= part.nrm.size() ? part.nrm[c + 3] : 0.0f});
            }
        }
        out.triangles += part.tris.size() / 15;
    }
}

void flatten(std::unordered_map<std::uint64_t, Bucket>& buckets, Mesh& out)
{
    std::vector<std::pair<std::uint64_t, Bucket*>> order;
    for (auto& [key, bk] : buckets)
        order.emplace_back(key, &bk);
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    // (each one's room taken once: grown as it filled, Zi'Tah's 90 MB of cut-outs wanted a block half as big again beside it)
    std::size_t solid = 0, cut = 0;
    for (const auto& [key, bk] : order)
        (static_cast<std::int32_t>(key >> 32) - 1 < 0 ? solid : cut) += bk->v.size() / 5;
    out.pos.reserve(solid * 3), out.nrm.reserve(solid * 4), out.cut.reserve(cut * 5);
    for (const auto& [key, bk] : order) {
        if (bk->v.empty())
            continue;
        Range r;
        r.texture = static_cast<std::int32_t>(key >> 32) - 1;
        std::memcpy(r.lo, bk->lo, sizeof r.lo), std::memcpy(r.hi, bk->hi, sizeof r.hi);
        const auto corners = static_cast<std::uint32_t>(bk->v.size() / 5);
        r.count = corners;
        if (r.texture < 0) {
            r.first = static_cast<std::uint32_t>(out.pos.size() / 3);
            for (std::size_t i = 0; i < bk->v.size(); i += 5)
                out.pos.insert(out.pos.end(), {bk->v[i], bk->v[i + 1], bk->v[i + 2]});
            bk->n.resize(bk->v.size() / 5 * 4, 0.0f);
            out.nrm.insert(out.nrm.end(), bk->n.begin(), bk->n.end());
        } else {
            r.first = static_cast<std::uint32_t>(out.cut.size() / 5);
            out.cut.insert(out.cut.end(), bk->v.begin(), bk->v.end());
        }
        out.ranges.push_back(r);
        std::vector<float>().swap(bk->v), std::vector<float>().swap(bk->n);
    }
}

} // namespace

bool read(const std::vector<std::uint8_t>& dat, const std::uint8_t* table1, const std::uint8_t* table2, Mesh& out, std::string& why)
{
    out = Mesh{};
    const std::vector<Section> all = sections(dat);
    const auto def = std::find_if(all.begin(), all.end(), [](const Section& s) { return s.type == kZoneDef; });
    if (def == all.end()) {
        why = "no ZoneDef section";
        return false;
    }
    std::vector<std::uint8_t> zd = copy(dat, *def);
    if (!decrypt_def(zd, table1)) {
        why = "its ZoneDef is encrypted and FFXiMain.dll's key table wasn't found";
        return false;
    }
    std::unordered_map<std::string, MeshDef> meshes;
    read_meshes(dat, all, table1, table2, meshes);
    out.meshes = static_cast<unsigned>(meshes.size());
    const Buf b{zd.data(), zd.size()};
    const std::size_t nodes = b.u32(16 + 4) & 0xFFFFFF;
    std::unordered_map<std::uint64_t, Bucket> buckets;
    std::unordered_map<std::string, std::int32_t> textures;
    for (std::size_t i = 0; i < nodes; ++i) {
        const std::size_t r = 16 + 0x20 + i * kRecord;
        if (!b.in(r, kRecord))
            break;
        out.placements++;
        if (const int why_out = left_out(b, r)) {
            (why_out == 1 ? out.stand_ins : out.moving)++;
            continue;
        }
        const MeshDef* m = resolve(meshes, b.name(r));
        if (m == nullptr) {
            if (out.unfound++ < 8)
                out.unfound_names.push_back(b.name(r));
            continue;
        }
        out.placed++;
        lay(*m, place_of(b, r), dat, out, buckets, textures);
    }
    flatten(buckets, out);
    if (out.ranges.empty()) {
        why = "nothing placed";
        return false;
    }
    return true;
}

bool key_table2(const std::vector<std::uint8_t>& dll, std::uint8_t out[256])
{
    if (dll.size() < kTableAt + 4)
        return false;
    const auto from = dll.begin() + static_cast<std::ptrdiff_t>(kTableAt);
    const auto at = std::search(from, dll.end(), kTable2Sig, kTable2Sig + 4);
    if (at == dll.end() || static_cast<std::size_t>(dll.end() - at) < 256)
        return false;
    std::memcpy(out, &*at, 256);
    return true;
}

} // namespace vanacore::zonemesh
