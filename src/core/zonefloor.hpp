#pragma once

#include <cstdint>
#include <vector>

// The zone lines of a zone's model DAT, and where the ground is under each ([graphics] zonelines). No Windows
// calls: it works on the DAT's bytes, so it can be tested apart from the game.
namespace vanacore::zonefloor {

// Each line is kStride floats: the box's centre (x, y, z as the game draws: y counts down), its turn about
// the up axis, its full width, height and depth, then how far above its middle the ground is under it (below
// 0: below it), measured from the zone's collision where that gives one, else taken as a unit above the box's
// bottom (the boxes the zones have start about a unit under the ground they're on).
constexpr unsigned kStride = 8;

// The DAT's lines (its RID section's 'z' entries with a destination, at most `max`; none without a RID section), the
// ground not yet found: the last float is the guess from the box's bottom.
void read_lines(const std::vector<std::uint8_t>& dat, unsigned max, std::vector<float>& lines);

// The ground under each of the lines, from the collision in the DAT's ZoneDef section: its triangles that face
// up and aren't walls, at seven places along each line's middle, the highest one at each (where floors stack the
// player walks on the top one), and the middle of those. `table1` is the 256-byte key table of FFXiMain.dll that decrypts the section
// (null: it's read as it is, for a zone that isn't encrypted). Returns how many lines got one.
unsigned find_floors(const std::vector<std::uint8_t>& dat, const std::uint8_t* table1, std::vector<float>& lines);

// How the DAT's environments light the zone (its 0x2F sections, xi-tools docs/zone/inject-legacy.md): 1 when every one has
// bit 0 of its first u32 set, its sun's and moon's colours a fixed light's direction instead (the zone has no sky: Palborough
// Mines, Castle Zvahl Keep, the Grand Palace of Hu'Xzoi), 0 when any lights by the sky (Norg, Waughroon Shrine and Yughott
// Grotto have both), -1 when it has none.
int fixed_light(const std::vector<std::uint8_t>& dat);

// The key table from the bytes of FFXiMain.dll's file (256 bytes; false if it isn't there).
bool key_table(const std::vector<std::uint8_t>& dll, std::uint8_t out[256]);

// The running game's key table: FFXiMain.dll's file read once for the session, the first time it's asked for once the game
// has loaded it (the zone lines and the grass's floors share it). Null if it couldn't be found. Any thread.
const std::uint8_t* game_key_table();

// The floors of the zone's collision (what the player stands on), for the grass ([graphics] grass.walkable): the
// collision's meshes, their triangles that aren't walls, and where each mesh is placed. A mesh's triangles stay in its own
// space (one mesh is placed many times). Which way a floor faces isn't kept: a tenth of the zones' floors, ground the player
// stands on, have their normals stored facing down.
struct Floors {
    std::vector<float> tris;               // 9 floats a triangle: its corners in its mesh's space
    std::vector<std::uint8_t> terrain;     // each triangle's ground, as the game's footsteps know it: 0 object, 1 path,
                                           // 2 grass, 3 sand, 4 snow, 5 stone, 6 metal, 7 wood, 8 shallow water, 9 deep water;
                                           // + kWall for a wall (read_floors' walls only)
    std::vector<std::uint32_t> mesh_first; // mesh m's triangles: mesh_first[m] up to mesh_first[m + 1]
    std::vector<float> matrices;           // 12 floats a placement: where a mesh's x, y and z axes go in the world, then its origin
    std::vector<std::uint32_t> pairs;      // 2 each: a matrix and a mesh it places
};

// A wall's mark in Floors::terrain, over its ground
constexpr std::uint8_t kWall = 0x10;

// The floors from the collision in the DAT's ZoneDef section (`table1` as find_floors). False if it has none. `walls`: the
// walls too, kWall in their terrain: the whole collision, for /vana debug's collision view ([graphics] collision.show).
bool read_floors(const std::vector<std::uint8_t>& dat, const std::uint8_t* table1, Floors& out, bool walls = false);

} // namespace vanacore::zonefloor
