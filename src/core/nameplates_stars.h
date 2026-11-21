// nameplates.cpp's, inside its namespace: nameplates.stars.spacing and .size. The line of stars under a job master's name
// with its stars that share of the game's distance apart (1: the game's), about the line's middle (between its first star
// and its last), and each that many times its size, the line's stars grown about its middle: its quads are held as the
// game draws them (draw_stars) and drawn moved once the line is done (draw_line). Each keeps its own texture and alpha
// reference; the rest of the state is the line's. Asked for in game 2026-11-07: the stars stood far apart, the small ones
// (Star style) keeping the big star's room; the size 2026-11-08.

struct HeldStar {
    void* renderer = nullptr;
    LetterVertex quad[4] = {};
    void* texture = nullptr; // held
    DWORD alpha_ref = 0;
};
constexpr int kMostHeldStars = 16;
HeldStar g_held_stars[kMostHeldStars];
int g_held_star_count = 0;

// A quad's edges
struct QuadBox {
    float left, right, top, bottom;
};
QuadBox box_of(const LetterVertex* quad)
{
    QuadBox s{quad[0].x, quad[0].x, quad[0].y, quad[0].y};
    for (int i = 1; i < 4; ++i) {
        s.left = std::min(s.left, quad[i].x);
        s.right = std::max(s.right, quad[i].x);
        s.top = std::min(s.top, quad[i].y);
        s.bottom = std::max(s.bottom, quad[i].y);
    }
    return s;
}

// nameplates.stars.style=small: the big star drawn as big as the small stars and level with them. The game doesn't stand
// the small ones on the big one's foot, so the big one made half its size about its foot sat lower than them (in game
// 2026-11-08): it's made the size of the line's small star before it, about that one's middle (a small star comes first,
// 0xD3), and half its size on its own foot when none has.
float g_small_star_middle = 0.0f;
float g_small_star_height = 0.0f; // 0: none in this line yet
void note_small_star(const LetterVertex* quad)
{
    const QuadBox s = box_of(quad);
    g_small_star_middle = (s.top + s.bottom) * 0.5f;
    g_small_star_height = s.bottom - s.top;
}

void shrink_big_star(LetterVertex (&quad)[4])
{
    const QuadBox s = box_of(quad);
    const float height = s.bottom - s.top;
    const bool level = g_small_star_height > 0.5f && height > 0.5f;
    const float scale = level ? std::clamp(g_small_star_height / height, 0.1f, 1.0f) : 0.5f;
    const float middle_x = (s.left + s.right) * 0.5f;
    const float middle_y = (s.top + s.bottom) * 0.5f;
    const float to_y = level ? g_small_star_middle : s.bottom - height * scale * 0.5f;
    for (LetterVertex& corner : quad) {
        corner.x = middle_x + (corner.x - middle_x) * scale;
        corner.y = to_y + (corner.y - middle_y) * scale;
    }
}

void* device_of(const void* renderer)
{
    return renderer != nullptr ? *reinterpret_cast<void* const*>(static_cast<const char*>(renderer) + kRendererDevice) : nullptr;
}

float quad_middle(const LetterVertex (&quad)[4])
{
    return (quad[0].x + quad[1].x + quad[2].x + quad[3].x) * 0.25f;
}

// A star's quad of the line being drawn, held to be drawn moved when the line is done: false to draw it now (the spacing
// and size the game's, not in a line, or nothing to see)
bool hold_star(void* renderer, const LetterVertex* quad, float width)
{
    if ((g_names.stars_spacing == 1.0f && g_names.stars_size == 1.0f) || !g_in_line ||
        g_held_star_count >= kMostHeldStars || !(width > 0.5f))
        return false;
    void* const device = device_of(renderer);
    if (device == nullptr)
        return false;
    HeldStar& held = g_held_stars[g_held_star_count++];
    held.renderer = renderer;
    std::memcpy(held.quad, quad, sizeof held.quad);
    held.texture = nullptr;
    d3d8::call(device, d3d8::slot::GetTexture, DWORD{0}, &held.texture);
    d3d8::call(device, d3d8::slot::GetRenderState, d3d8::rs::AlphaRef, &held.alpha_ref);
    return true;
}

// The held stars' quads placed: each star's middle moved toward (or from) the line's middle by nameplates.stars.spacing
// times .size, and its corners grown about that by .size, across from its own middle and down from the line's
void place_held_stars(int count)
{
    const float middle = (quad_middle(g_held_stars[0].quad) + quad_middle(g_held_stars[count - 1].quad)) * 0.5f;
    QuadBox line = box_of(g_held_stars[0].quad);
    for (int i = 1; i < count; ++i) {
        const QuadBox s = box_of(g_held_stars[i].quad);
        line.top = std::min(line.top, s.top);
        line.bottom = std::max(line.bottom, s.bottom);
    }
    const float level = (line.top + line.bottom) * 0.5f;
    const float size = g_names.stars_size;
    const float apart = size * g_names.stars_spacing;
    for (int i = 0; i < count; ++i) {
        LetterVertex(&quad)[4] = g_held_stars[i].quad;
        const float own = quad_middle(quad);
        const float to = middle + (own - middle) * apart;
        for (LetterVertex& corner : quad) {
            corner.x = to + (corner.x - own) * size;
            corner.y = level + (corner.y - level) * size;
        }
    }
}

// The line's held stars drawn, placed by nameplates.stars.spacing and .size; the device's texture and alpha reference put
// back after
void draw_held_stars(DrawFn draw)
{
    using d3d8::call;
    namespace slot = d3d8::slot;
    const int count = g_held_star_count;
    g_held_star_count = 0;
    if (count == 0)
        return;
    void* const device = device_of(g_held_stars[0].renderer);
    if (device != nullptr && draw != nullptr) {
        place_held_stars(count);
        void* texture = nullptr;
        DWORD alpha_ref = 0;
        call(device, slot::GetTexture, DWORD{0}, &texture);
        call(device, slot::GetRenderState, d3d8::rs::AlphaRef, &alpha_ref);
        for (int i = 0; i < count; ++i) {
            HeldStar& held = g_held_stars[i];
            call(device, slot::SetTexture, DWORD{0}, held.texture);
            call(device, slot::SetRenderState, d3d8::rs::AlphaRef, held.alpha_ref);
            draw(held.renderer, nullptr, d3d8::kTriangleStrip, 2u, static_cast<const void*>(held.quad), kLetterStride);
        }
        call(device, slot::SetRenderState, d3d8::rs::AlphaRef, alpha_ref);
        call(device, slot::SetTexture, DWORD{0}, texture);
        if (texture != nullptr)
            call<ULONG>(texture, slot::Release);
    }
    for (int i = 0; i < count; ++i)
        if (g_held_stars[i].texture != nullptr)
            call<ULONG>(g_held_stars[i].texture, slot::Release);
}
