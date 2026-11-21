#include "groundmarks.hpp"

#include <map>

#include "../gfx/vanacore_gfx.h"

namespace vanacore::groundmarks {
namespace {

constexpr unsigned kMost = 16;  // GFX_GROUND_MARKS
constexpr unsigned kFloats = 16; // GFX_GROUND_MARK_FLOATS

std::map<std::string, std::vector<Mark>> g_marks;

// every addon's marks, in the addons' order, to the graphics layer: each its centre (x, height, y as the game draws), its
// radius, its colour and strength, its shape, its column's height, its glyph, the glyph's size and height
void send()
{
    float out[kMost * kFloats];
    unsigned n = 0;
    for (const auto& [owner, marks] : g_marks) {
        for (const Mark& m : marks) {
            if (n == kMost)
                break;
            float* o = out + n++ * kFloats;
            o[0] = m.x, o[1] = m.z, o[2] = m.y, o[3] = m.radius;
            o[4] = m.r, o[5] = m.g, o[6] = m.b, o[7] = m.strength;
            o[8] = m.shape, o[9] = m.height, o[10] = m.glyph, o[11] = m.glyph_size;
            o[12] = m.glyph_height, o[13] = 0.0f, o[14] = 0.0f, o[15] = 0.0f;
        }
    }
    vanacore_gfx_set_ground_marks(out, n);
}

} // namespace

void set(const std::string& owner, const std::vector<Mark>& marks)
{
    if (marks.empty())
        g_marks.erase(owner);
    else
        g_marks[owner] = marks;
    send();
}

void release(const std::string& owner)
{
    if (g_marks.erase(owner) != 0)
        send();
}

} // namespace vanacore::groundmarks
