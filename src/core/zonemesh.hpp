#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The zone's own meshes for the sun's shadows ([graphics.effects] sun_zone_mesh): read from its model DAT, every mesh where
// the zone places it, as the game loads them - so the shadows come from the zone as it is, not from what the game happens to
// draw (casters.cpp's pass). No Windows calls: it works on the DAT's bytes, so it can be tested apart from the game.
//
// The DAT (xi-tools docs/zone/format.md, src/xi/zone/xi_decrypt.py, xi_export.py; xim's ZoneDefParser and ZoneMeshSection):
// its ZoneDef section (0x1C) places the zone's meshes, 0x64 bytes a placement from +0x20: the mesh's name (16 characters), its
// position, turn (x, y, z, radians) and scale, 3 floats each, from +0x10, a block id at +0x34 (0: a mesh that stands still), its
// draw distance at +0x40 (1.0: a collision stand-in, never drawn). Each mesh is a section of its own (0x2E): its name at +0x10,
// strips or lists (+4 bit 0), 36- or 48-byte vertices (+4 bit 1), its parts from +0x20 + the u32 at +0x3C, as many as the u32 at
// +0x40: a texture's name (16), the vertex count and flags (u16 each; 0x8000 blended), the vertices (position first, uv last),
// the index count and a u16, the u16 indices, then to 4 bytes. Both sections are encrypted with FFXiMain.dll's two key tables.
// A mesh whose name starts with '_' is cut out by its textures' alpha (the game's alpha test); a blended part casts nothing.
namespace vanacore::zonemesh {

// The meshes laid in the world (the game's: y counts down), as triangle lists, split by where they stand
struct Range {
    std::uint32_t first = 0, count = 0; // vertices in Mesh::pos (texture < 0) or Mesh::cut
    std::int32_t texture = -1;          // -1 solid; else the cut-out's texture in Mesh::textures
    bool rt_only = false;               // for the ray-traced reflections alone (read's `shade`): a part that moves or is blended
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
};

struct Texture {
    std::string name;
    unsigned width = 0, height = 0;
    std::vector<std::uint32_t> argb; // rows top first; alpha as the DAT has it (0x80 is solid)
};

struct Mesh {
    std::vector<float> pos; // 3 floats a vertex, solid
    // 4 floats for each of pos's vertices: its normal in the world, as the DAT has it for the game's own smooth lighting, and
    // how far the surface there is curved and smoothly shaded (0 to 1; curve_of): for the sun's shading of curved rock
    std::vector<float> nrm;
    std::vector<float> cut; // 5 a vertex: x, y, z, u, v (cut out)
    // for the ray-traced reflections (read's `shade`): what colours a surface they find. The solid vertices' uv (2 a vertex) and
    // vertex colour (0xAARRGGBB, the zone's baked light), and each solid triangle's texture (an index in `textures`, -1 none); the
    // cut-out vertices' colour. Left empty without `shade`
    std::vector<float> uv;
    std::vector<std::uint32_t> col, cut_col;
    std::vector<std::int32_t> tex;
    std::vector<Range> ranges;
    std::vector<Texture> textures;
    // what was read and left out, for the log
    unsigned placements = 0, placed = 0, stand_ins = 0, moving = 0, unfound = 0, meshes = 0, blended = 0;
    std::uint64_t triangles = 0;
    std::vector<std::string> unfound_names; // the first few placements' names with no mesh
};

// The DAT's meshes where its ZoneDef places them. `table1` and `table2`: FFXiMain.dll's key tables (null: read as they are,
// for a zone that isn't encrypted). False, and `why` said, when it has no ZoneDef or nothing placed.
bool read(const std::vector<std::uint8_t>& dat, const std::uint8_t* table1, const std::uint8_t* table2, Mesh& out, std::string& why,
          bool shade = false);

// The second key table from the bytes of FFXiMain.dll's file (zonefloor::key_table has the first). False if it isn't there.
bool key_table2(const std::vector<std::uint8_t>& dll, std::uint8_t out[256]);

} // namespace vanacore::zonemesh
