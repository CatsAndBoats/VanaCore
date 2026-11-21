#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <list>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "gui.hpp"
#include "hideui.hpp"

namespace vanacore::ui {
namespace {

std::list<Box> g_boxes; // stable addresses, drawn front to back in creation order
std::unordered_map<int, Box*> g_by_id; // the same boxes by id: every property an addon sets finds its box
int g_next_id = 1;
unsigned g_width = 0;
unsigned g_height = 0;

// The block_mouse boxes as last drawn, for input.cpp (which may run on another thread).
std::mutex g_block_lock;
std::vector<RECT> g_block;

// A rect's colour as 0xAARRGGBB (its style.background: red, green, blue 0-255, opacity 0-1).
std::uint32_t rect_argb(const Box& box)
{
    const float* c = box.style.background;
    const auto channel = [](float v) { return static_cast<std::uint32_t>(std::lround(v < 0 ? 0 : v > 255 ? 255 : v)); };
    return channel(c[3] * 255.0f) << 24 | channel(c[0]) << 16 | channel(c[1]) << 8 | channel(c[2]);
}

// A rect with rounded corners, as a picture: each pixel covered by as much of the shape as its middle
// is inside the edge (a signed distance, so the edge is antialiased). Made again when its size,
// radius or colour changes.
void round_rect(Box& box)
{
    const std::uint32_t argb = rect_argb(box);
    const std::wstring key = L"round " + std::to_wstring(box.width) + L"x" + std::to_wstring(box.height) + L" " +
                             std::to_wstring(box.radius) + L" " + std::to_wstring(argb);
    if (key == box.loaded)
        return;
    box.loaded = key;
    const int w = box.width > 0 ? box.width : 0, h = box.height > 0 ? box.height : 0;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(w) * h);
    const float r = (std::max)((std::min)(static_cast<float>(box.radius), (std::min)(w, h) / 2.0f), 0.5f); // (a square one too)
    const float half_w = w / 2.0f, half_h = h / 2.0f;
    const float alpha = static_cast<float>(argb >> 24);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            // from the middle, how far past the straight edges' inner box, less the radius
            const float dx = (std::max)(std::fabs(x + 0.5f - half_w) - (half_w - r), 0.0f);
            const float dy = (std::max)(std::fabs(y + 0.5f - half_h) - (half_h - r), 0.0f);
            const float distance = std::sqrt(dx * dx + dy * dy) - r;
            const float cover = distance <= -0.5f ? 1.0f : distance >= 0.5f ? 0.0f : 0.5f - distance;
            pixels[static_cast<std::size_t>(y) * w + x] =
                static_cast<std::uint32_t>(std::lround(alpha * cover)) << 24 | (argb & 0xFFFFFF);
        }
    box.sprite.set_image(std::move(pixels), w, h);
}

// Whether the box is drawn now, at its x and y, in this layer
bool on_screen(const Box& box, bool hidden, int layer)
{
    return box.visible && !hidden && !box.placed && box.layer == layer;
}

} // namespace

Box& create(const std::string& owner)
{
    Box& box = g_boxes.emplace_back();
    box.id = g_next_id++;
    box.owner = owner;
    box.style.weight = FW_NORMAL;
    g_by_id[box.id] = &box;
    return box;
}

Box* find(int id)
{
    const auto found = g_by_id.find(id);
    return found != g_by_id.end() ? found->second : nullptr;
}

void destroy(int id)
{
    for (auto it = g_boxes.begin(); it != g_boxes.end(); ++it) {
        if (it->id == id) {
            it->sprite.release();
            g_by_id.erase(id);
            g_boxes.erase(it);
            return;
        }
    }
}

void destroy_owned(const std::string& owner)
{
    for (auto it = g_boxes.begin(); it != g_boxes.end();) {
        if (it->owner == owner) {
            it->sprite.release();
            g_by_id.erase(it->id);
            it = g_boxes.erase(it);
        } else {
            ++it;
        }
    }
}

void refresh(Box& box)
{
    if (box.rect) {
        if (box.radius > 0)
            round_rect(box);
        return;
    }
    if (box.image) {
        // its colour (not its opacity, which it's drawn with) made into the picture
        const std::uint32_t tint = rect_argb(box) & 0xFFFFFF;
        const int across = box.repeat_x > 1 ? box.repeat_x : 1, down = box.repeat_y > 1 ? box.repeat_y : 1;
        const std::wstring key = box.file + L"|" + std::to_wstring(box.width) + L"x" + std::to_wstring(box.height) + L"|" +
                                 std::to_wstring(across) + L"x" + std::to_wstring(down) + L"|" + std::to_wstring(tint);
        if (key == box.loaded)
            return;
        box.loaded = key;
        // one tile's size: the box's over the repeats (its own size when the box has none)
        int width = box.width > 0 ? std::max(1, box.width / across) : 0;
        int height = box.height > 0 ? std::max(1, box.height / down) : 0;
        std::vector<std::uint32_t> pixels;
        if (box.file.empty() || !text::load_image(box.file, width, height, pixels) || width <= 0 || height <= 0) {
            box.sprite.set_image({}, 0, 0);
            return;
        }
        if (across > 1 || down > 1) {
            std::vector<std::uint32_t> tiled(static_cast<size_t>(width) * across * height * down);
            for (int y = 0; y < height * down; ++y)
                for (int x = 0; x < width * across; ++x)
                    tiled[static_cast<size_t>(y) * width * across + x] = pixels[static_cast<size_t>(y % height) * width + x % width];
            pixels.swap(tiled);
            width *= across;
            height *= down;
        }
        if (tint != 0xFFFFFF) {
            const std::uint32_t tr = tint >> 16 & 0xFF, tg = tint >> 8 & 0xFF, tb = tint & 0xFF;
            for (std::uint32_t& p : pixels)
                p = (p & 0xFF000000) | ((p >> 16 & 0xFF) * tr / 255) << 16 | ((p >> 8 & 0xFF) * tg / 255) << 8 | (p & 0xFF) * tb / 255;
        }
        box.sprite.set_image(std::move(pixels), width, height);
        return;
    }
    text::Style style = box.style;
    style.align_right = box.right;
    box.sprite.set(box.text, style);
}

void refresh_placed(Box& box)
{
    if (box.rect)
        round_rect(box);
    else
        refresh(box);
}

void draw(void* device)
{
    std::vector<RECT> block;
    const bool hidden = hideui::hidden(); // the game's interface hidden ([ui] hide.key): none drawn, none taking the mouse
    // by layer, the lowest first (almost always the one, 0), each in creation order
    std::set<int> layers;
    for (const Box& box : g_boxes)
        layers.insert(box.layer);
    for (const int layer : layers)
    for (Box& box : g_boxes) {
        if (!on_screen(box, hidden, layer))
            continue;
        if (box.block_mouse) {
            if (!box.rect)
                refresh(box);
            const int w = box.rect ? box.width : box.sprite.width(), h = box.rect ? box.height : box.sprite.height();
            const int x = !box.rect && box.right ? box.x - w : box.x;
            block.push_back(RECT{x, box.y, x + w, box.y + h});
        }
        if (box.rect && box.radius <= 0) {
            std::uint32_t argb = rect_argb(box);
            if (box.opacity < 1.0f)
                argb = static_cast<std::uint32_t>(std::lround(static_cast<float>(argb >> 24) * box.opacity)) << 24 | (argb & 0xFFFFFF);
            if (box.width > 0 && box.height > 0 && (argb >> 24) != 0)
                text::fill(device, box.x, box.y, box.width, box.height, argb);
            continue;
        }
        if (box.rect) {
            round_rect(box);
            if (box.width > 0 && box.height > 0 && box.sprite.upload(device))
                box.sprite.draw(device, box.x, box.y, box.opacity);
            continue;
        }
        refresh(box);
        if (!box.image)
            box.sprite.set_dissolve(box.dissolve, box.dissolve_edge);
        if (box.sprite.upload(device))
            box.sprite.draw(device, box.right ? box.x - box.sprite.width() : box.x, box.y,
                            (box.image ? box.style.background[3] : 1.0f) * box.opacity, 1.0f,
                            {box.wave, box.wave_length, box.wave_phase});
    }
    const std::lock_guard lock(g_block_lock);
    g_block.swap(block);
}

bool boxes_block_mouse(int x, int y)
{
    const std::lock_guard lock(g_block_lock);
    for (const RECT& r : g_block)
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return true;
    return false;
}

bool blocks_mouse(int x, int y)
{
    return boxes_block_mouse(x, y) || gui::blocks_mouse(x, y);
}

void release()
{
    for (Box& box : g_boxes)
        box.sprite.release();
}

void set_screen(unsigned width, unsigned height)
{
    g_width = width;
    g_height = height;
}

void screen(unsigned& width, unsigned& height)
{
    width = g_width;
    height = g_height;
}

namespace {
unsigned g_game_width = 0, g_game_height = 0;
}

void set_game_screen(unsigned width, unsigned height)
{
    g_game_width = width;
    g_game_height = height;
}

void game_screen(unsigned& width, unsigned& height)
{
    width = g_game_width;
    height = g_game_height;
}

} // namespace vanacore::ui
