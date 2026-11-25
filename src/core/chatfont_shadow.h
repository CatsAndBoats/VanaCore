// chatfont.cpp's, inside its namespace after Cut and cut: a letter's soft shadow (<area>.font.shadow), its ink as coverage
// (Layer), blurred and moved, with the outline's stroke over it, as one Cut (shadow_cut, chatfont_wide.h add_glyph).

// One picture's ink as coverage 0..1, with where its top-left is from the pen (x) and the baseline (y), screen pixels
struct Layer {
    std::vector<float> a;
    int w = 0, h = 0, x = 0, y = 0;
};

Layer layer_of(const text::Sprite& sprite)
{
    Layer l;
    const int w = sprite.width(), h = sprite.height();
    const std::vector<std::uint32_t>& pixels = sprite.pixels();
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if ((pixels[static_cast<std::size_t>(y) * w + x] >> 24) != 0)
                x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1, y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
    if (x1 < 0)
        return l;
    l.w = x1 - x0 + 1, l.h = y1 - y0 + 1, l.x = x0 - sprite.pen_left(), l.y = y0 - sprite.baseline();
    l.a.resize(static_cast<std::size_t>(l.w) * l.h);
    for (int y = 0; y < l.h; ++y)
        for (int x = 0; x < l.w; ++x)
            l.a[static_cast<std::size_t>(y) * l.w + x] = static_cast<float>(pixels[static_cast<std::size_t>(y0 + y) * w + x0 + x] >> 24) / 255.0f;
    return l;
}

// `l` blurred (a Gaussian, sigma `blur` screen pixels, the layer grown by its reach) and moved (dx, dy) whole pixels
Layer blurred(const Layer& l, float blur, int dx, int dy)
{
    Layer out;
    if (l.w == 0)
        return out;
    const int r = blur > 0.05f ? static_cast<int>(std::ceil(blur * 3.0f)) : 0;
    const int bw = l.w + 2 * r, bh = l.h + 2 * r;
    std::vector<float> a(static_cast<std::size_t>(bw) * bh, 0.0f), t(a.size(), 0.0f);
    for (int y = 0; y < l.h; ++y)
        for (int x = 0; x < l.w; ++x)
            a[static_cast<std::size_t>(y + r) * bw + x + r] = l.a[static_cast<std::size_t>(y) * l.w + x];
    if (r > 0) {
        std::vector<float> k(static_cast<std::size_t>(2 * r + 1));
        float sum = 0.0f;
        for (int i = -r; i <= r; ++i)
            sum += k[static_cast<std::size_t>(i + r)] = std::exp(-0.5f * static_cast<float>(i * i) / (blur * blur));
        for (float& v : k)
            v /= sum;
        for (int y = 0; y < bh; ++y) // across
            for (int x = 0; x < bw; ++x) {
                float v = 0.0f;
                for (int i = -r; i <= r; ++i)
                    if (x + i >= 0 && x + i < bw)
                        v += a[static_cast<std::size_t>(y) * bw + x + i] * k[static_cast<std::size_t>(i + r)];
                t[static_cast<std::size_t>(y) * bw + x] = v;
            }
        for (int y = 0; y < bh; ++y) // down
            for (int x = 0; x < bw; ++x) {
                float v = 0.0f;
                for (int i = -r; i <= r; ++i)
                    if (y + i >= 0 && y + i < bh)
                        v += t[static_cast<std::size_t>(y + i) * bw + x] * k[static_cast<std::size_t>(i + r)];
                a[static_cast<std::size_t>(y) * bw + x] = v;
            }
    }
    out.a = std::move(a), out.w = bw, out.h = bh, out.x = l.x - r + dx, out.y = l.y - r + dy;
    return out;
}

// The letter's soft shadow as a Cut (premultiplied white, drawn black by the outline's pass at full strength): `thick` (the
// letter's picture with a stroke, or the letter's) blurred and moved, times `shadow_opacity`, and `edge` (the letter with its
// outline's stroke; empty: none) times `edge_opacity` over it, the darker of the two where they meet
Cut shadow_cut(const text::Sprite& thick, const text::Sprite* edge, float shift, float blur, int dx, int dy, float shadow_opacity,
               float edge_opacity)
{
    Cut out;
    Layer sh = blurred(layer_of(thick), blur, dx, dy);
    Layer ed = edge != nullptr ? layer_of(*edge) : Layer{};
    if (sh.w == 0 && ed.w == 0)
        return out;
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    for (const Layer* l : {&sh, &ed})
        if (l->w > 0)
            x0 = std::min(x0, l->x), y0 = std::min(y0, l->y), x1 = std::max(x1, l->x + l->w), y1 = std::max(y1, l->y + l->h);
    out.w = x1 - x0, out.h = y1 - y0;
    std::vector<float> a(static_cast<std::size_t>(out.w) * out.h, 0.0f);
    for (int pass = 0; pass < 2; ++pass) {
        const Layer& l = pass == 0 ? sh : ed;
        const float k = pass == 0 ? shadow_opacity : edge_opacity;
        for (int y = 0; y < l.h; ++y)
            for (int x = 0; x < l.w; ++x) {
                float& d = a[static_cast<std::size_t>(l.y + y - y0) * out.w + (l.x + x - x0)];
                d = std::max(d, l.a[static_cast<std::size_t>(y) * l.w + x] * k);
            }
    }
    out.x = static_cast<float>(x0) + shift, out.y = static_cast<float>(y0);
    out.ink.resize(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        const std::uint32_t v = static_cast<std::uint32_t>(std::lround(a[i] > 1.0f ? 255.0f : a[i] * 255.0f));
        out.ink[i] = v << 24 | v << 16 | v << 8 | v;
    }
    return out;
}
