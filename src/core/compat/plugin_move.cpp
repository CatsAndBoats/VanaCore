#include "plugin_move.hpp"

#include <cmath>
#include <mutex>

#include "../graphics.hpp"

namespace vanacore::plugin_move {
namespace {

constexpr size_t kSlots = 32; // the renderer's (d3d8.c's MOVERS)

std::vector<std::string> g_slots; // the plugins that have drawn, each at its slot
std::string g_drawing;            // the plugin drawing now (drawing_as), or empty
int g_slot = -1;                  // its slot
std::mutex g_moves_lock;          // (unmoved: the mouse's thread)
std::map<std::string, Move> g_moves;

int slot_of(const std::string& owner)
{
    for (size_t i = 0; i < g_slots.size(); ++i)
        if (g_slots[i] == owner)
            return static_cast<int>(i);
    if (g_slots.size() >= kSlots)
        return -1;
    g_slots.push_back(owner);
    return static_cast<int>(g_slots.size() - 1);
}

Move move_of(const std::string& owner)
{
    const std::lock_guard lock(g_moves_lock);
    const auto found = g_moves.find(owner);
    return found != g_moves.end() ? found->second : Move{};
}

// The renderer told whose draws come and how they move: x to ox + (x - ox) * scale + dx, y the same
void apply()
{
    if (g_slot < 0) {
        graphics::set_mover(-1, 0, 0, 1);
        return;
    }
    const Move m = move_of(g_drawing);
    graphics::set_mover(g_slot, m.ox * (1 - m.scale) + m.dx, m.oy * (1 - m.scale) + m.dy, m.scale);
}

} // namespace

void drawing_as(const std::string& owner)
{
    g_drawing = owner;
    g_slot = owner.empty() ? -1 : slot_of(owner);
    apply();
}

void pause()
{
    if (g_slot >= 0)
        graphics::set_mover(-1, 0, 0, 1);
}

void resume()
{
    if (g_slot >= 0)
        apply();
}

std::vector<std::pair<std::string, Box>> boxes()
{
    std::vector<std::pair<std::string, Box>> out;
    for (size_t i = 0; i < g_slots.size(); ++i) {
        float b[4] = {};
        if (graphics::mover_box(static_cast<int>(i), b) && b[2] > b[0] && b[3] > b[1])
            out.push_back({g_slots[i], Box{b[0], b[1], b[2], b[3]}});
    }
    return out;
}

void set_moves(std::map<std::string, Move> moves)
{
    const std::lock_guard lock(g_moves_lock);
    g_moves = std::move(moves);
}

void unmoved(const std::string& owner, int& x, int& y)
{
    const Move m = move_of(owner);
    if (m.dx == 0 && m.dy == 0 && m.scale == 1)
        return;
    const float scale = m.scale > 0 ? m.scale : 1.0f;
    x = static_cast<int>(std::lround(m.ox + (static_cast<float>(x) - m.dx - m.ox) / scale));
    y = static_cast<int>(std::lround(m.oy + (static_cast<float>(y) - m.dy - m.oy) / scale));
}

} // namespace vanacore::plugin_move
