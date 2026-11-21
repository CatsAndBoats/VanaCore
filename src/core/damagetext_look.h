// The damage numbers' look (damagetext.cpp): the colours a number's pictures are made in (Look: its kind's gradient,
// <kind>.gradient, or a flat colour, on the outline's, damage.outline.color), how the game's draw tints them and what leaves them untinted,
// and the log's line saying how the numbers are drawn. damagetext.cpp includes this inside its anonymous namespace, after
// Vertex, before Picture; it is no file of its own.
#pragma once

// The colours a picture is made in: its fill (`stops` colours from top to bottom, or left to right `across`; 1 a flat
// colour) and its outline. The default, white on the dark outline, is tinted by the draw (the game's colour, or the one
// picked); any other is drawn untinted (Settings::baked).
struct Look {
    int stops = 1;
    std::uint32_t fill[3] = {0xFFFFFF, 0xFFFFFF, 0xFFFFFF};
    std::uint32_t outline = 0x000000;
    bool across = false;

    bool plain() const { return stops == 1 && fill[0] == 0xFFFFFF && outline == 0x000000; }
    // Its part of a picture's key: empty for the default
    std::string key() const
    {
        if (plain())
            return {};
        char text[48];
        std::snprintf(text, sizeof text, "%d%c%06X,%06X,%06X/%06X|", stops, across ? '>' : ':', fill[0],
                      stops > 1 ? fill[1] : 0u, stops > 2 ? fill[2] : 0u, outline);
        return text;
    }
};

// 0xRRGGBB into a text style's colour (0-255 each)
void rgb_into(std::uint32_t rgb, float* colour)
{
    colour[0] = static_cast<float>(rgb >> 16 & 0xFF);
    colour[1] = static_cast<float>(rgb >> 8 & 0xFF);
    colour[2] = static_cast<float>(rgb & 0xFF);
}

// A number's kind's colours: its colour (0xRRGGBB; kGameColour: the game's) and its gradient (sharp only) to `second`.
// A critical hit's while they're picked out, a miss's, a heal's (the hit colours never reach it), a hit on you's while it
// has a colour or a gradient of its own (its colour unset: a hit's), else a hit's.
struct Kind {
    std::uint32_t colour;
    bool gradient;
    std::uint32_t second;
};
Kind kind_of(const Settings& s, bool crit, bool miss, bool taken, bool heal)
{
    if (crit && s.crit)
        return {s.crit_colour, s.crit_gradient, s.crit_second};
    if (miss)
        return {s.miss_colour, s.miss_gradient, s.miss_second};
    if (heal)
        return {s.heal_colour, s.heal_gradient, s.heal_second};
    if (taken && (s.taken_colour != kGameColour || s.taken_gradient))
        return {s.taken_colour != kGameColour ? s.taken_colour : s.hit_colour, s.taken_gradient, s.taken_second};
    return {s.hit_colour, s.hit_gradient, s.hit_second};
}

// The colours a number's pictures are made in: the default (tinted as it's drawn) unless Settings::baked; then `rgb`
// (0xRRGGBB: the colour it's drawn in) flat, or its kind's gradient, from its colour (`rgb` while that's the game's) to
// its second, on the outline's colour
Look look_for(const Settings& settings, bool crit, bool miss, bool taken, bool heal, std::uint32_t rgb)
{
    Look look;
    if (!settings.baked())
        return look;
    look.outline = settings.outline_colour;
    look.fill[0] = rgb & 0xFFFFFF;
    const Kind kind = kind_of(settings, crit, miss, taken, heal);
    if (!kind.gradient)
        return look;
    if (kind.colour != kGameColour)
        look.fill[0] = kind.colour;
    look.stops = 2;
    look.fill[1] = kind.second;
    look.across = settings.gradient_across;
    return look;
}

// How many times the glyphs' colour op multiplies the vertices' colour (the diffuse) into the texture's: 0 if it doesn't
// take it
float colour_times(void* device)
{
    using d3d8::call;
    namespace slot = d3d8::slot;
    DWORD op = 0, arg1 = 0, arg2 = 0;
    call(device, slot::GetTextureStageState, DWORD{0}, d3d8::tss::ColorOp, &op);
    call(device, slot::GetTextureStageState, DWORD{0}, d3d8::tss::ColorArg1, &arg1);
    call(device, slot::GetTextureStageState, DWORD{0}, d3d8::tss::ColorArg2, &arg2);
    constexpr DWORD kSelect = 0xF; // the argument's source, without its modifiers
    if ((arg1 & kSelect) != d3d8::kTaDiffuse && (arg2 & kSelect) != d3d8::kTaDiffuse)
        return 0.0f;
    return op == d3d8::kTopModulate4x ? 4.0f : op == d3d8::kTopModulate2x ? 2.0f : op == d3d8::kTopModulate ? 1.0f : 0.0f;
}

// A vertex colour's red, green and blue as the colour they make of a white texel, `times` times (0xRRGGBB); white for 0
std::uint32_t shown_rgb(DWORD argb, float times)
{
    if (times <= 0.0f)
        return 0xFFFFFF;
    std::uint32_t out = 0;
    for (int shift = 0; shift < 24; shift += 8)
        out |= static_cast<std::uint32_t>(std::min(255.0f, std::round(static_cast<float>(argb >> shift & 0xFF) * times)))
               << shift;
    return out;
}

// The vertex colour that leaves a texel as it is, `times` times (0xRRGGBB)
std::uint32_t neutral_rgb(float times)
{
    const std::uint32_t c = static_cast<std::uint32_t>(std::min(255.0f, std::round(255.0f / times)));
    return c << 16 | c << 8 | c;
}

// The log's line on how the numbers are drawn, into `state`; `kept`: the numbers over everything are drawn after the names
void describe(const Settings& settings, bool kept, wchar_t (&state)[512])
{
    const wchar_t* const crits = settings.crit ? L"picked out" : L"as the game has them";
    const wchar_t* const spawned = settings.spawn == Spawn::Stacked  ? L"stacked"
                                   : settings.spawn == Spawn::Random ? L"random spots"
                                                                     : L"the game's spots";
    const bool coloured = settings.hit_colour != kGameColour || settings.miss_colour != kGameColour ||
                          settings.taken_colour != kGameColour || settings.heal_colour != kGameColour;
    const wchar_t* const graded = !settings.sharp || !settings.any_gradient() ? L""
                                  : settings.gradient_across              ? L"; gradients left to right"
                                                                          : L"; gradients top to bottom";
    wchar_t extras[240];
    swprintf_s(extras, L"; placed at %ls%ls%ls%ls%ls%ls; up %.1f s", spawned,
               settings.multi && settings.sharp ? L"; multi-hit icon" : L"",
               settings.label && settings.sharp ? L"; skill names shown" : L"", coloured ? L"; own colours" : L"", graded,
               settings.sharp && settings.outline_colour != 0x000000 ? L"; a coloured outline" : L"", settings.duration);
    const wchar_t* const over = !settings.on_top ? L"behind what's in front"
                                : kept           ? L"over everything, the names too"
                                                 : L"over everything but the names";
    if (settings.sharp)
        _snwprintf_s(state, _TRUNCATE, L"VanaCore's text, %ls%ls%ls; critical hits %ls; %ls%ls", settings.family.c_str(),
                   settings.bold ? L" bold" : L"", settings.italic ? L" italic" : L"", crits, over, extras);
    else
        _snwprintf_s(state, _TRUNCATE, L"the game's glyphs; critical hits %ls; %ls%ls", crits, over, extras);
}
