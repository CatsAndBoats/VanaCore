// targetwin.cpp's, inside its namespace: the Target window's name drawn whole in its True Font (target.font on), one picture of
// VanaCore's text in place of the game's letters. 2026-11-07: "we should be replacing the whole target name ... and just
// writing it as pure true font". The game draws a name's ':', '"' and ''' from the labels' smaller letters, which True Font
// gave the title's font ("Door:\"Neptune's Spire\"" came out with them small and apart), and puts a name with a ':' on two
// lines, split there. Now every letter is the font's, at its size, on one line, by the font's own advances: from the game's
// pen (aligned by target.name.align by the picture's own width), its baseline kNameBaseline below it, in the game's colour for
// the name, with the font's outline, drawn in VanaCore's interface pass (draw).

constexpr float kNameBaseline = 10.5f; // interface units from the pen down to the name's baseline (the big letters' 21 texels, at half)

struct NamePending {
    bool set = false;
    int x = 0, y = 0, room = 0; // the game's pen (padding and spacing in) and the name's room, before alignment
    std::string text;
    std::uint32_t colour = 0;   // the game's: ARGB, each to 0x80
};
NamePending g_name_pending;
struct NamePicture {
    text::Sprite sprite;
    std::wstring text;
    text::Style style;
    void* device = nullptr;
} g_name_picture;

// The name's True Font is on (its letters not left out)
bool name_font_on()
{
    text::Style letters, stroked;
    bool upper = false;
    float opacity = 0.0f;
    return chatfont::area_style(VANA_FONT_TARGET, letters, stroked, upper, opacity);
}

// The name kept to be drawn whole this frame (true: the game's letters are left out), while its True Font is on
bool name_whole(int x, int y, int room, const char* text, std::uint32_t colour)
{
    if (text == nullptr || !name_font_on())
        return false;
    g_name_pending.set = true;
    g_name_pending.x = x, g_name_pending.y = y, g_name_pending.room = room;
    g_name_pending.text.assign(text, strnlen(text, kNameMost));
    g_name_pending.colour = colour;
    return true;
}

// The name's style: the area's letters in the game's colour for the name (its channels to 0x80, doubled), with the font's
// outline in the same picture
text::Style name_style(const text::Style& letters, const text::Style& stroked, float opacity, std::uint32_t colour)
{
    text::Style style = letters;
    const auto channel = [colour](int shift) { return std::min(255.0f, static_cast<float>((colour >> shift) & 0xFF) * 2.0f); };
    style.color[0] = channel(16), style.color[1] = channel(8), style.color[2] = channel(0);
    style.color[3] = channel(24) / 255.0f;
    style.stroke = stroked.stroke, style.exact_stroke = stroked.exact_stroke;
    style.stroke_color[3] = stroked.stroke > 0 ? opacity : 0.0f;
    style.background[3] = 0.0f;
    return style;
}

// The name kept this frame, drawn: its picture made again when its text or style changed (or the device did)
void draw_name(void* device, unsigned width, unsigned height)
{
    const NamePending pending = g_name_pending;
    g_name_pending.set = false;
    text::Style letters, stroked;
    bool upper = false;
    float opacity = 0.0f;
    if (!pending.set || !chatfont::area_style(VANA_FONT_TARGET, letters, stroked, upper, opacity))
        return;
    std::wstring shown = gametext::to_wide(pending.text);
    if (upper)
        for (wchar_t& c : shown)
            c = static_cast<wchar_t>(std::towupper(c));
    const text::Style style = name_style(letters, stroked, opacity, pending.colour);
    NamePicture& p = g_name_picture;
    if (device != p.device)
        p.sprite.release(), p.device = device;
    if (shown != p.text || !(style == p.style))
        p.sprite.set(shown, style), p.text = shown, p.style = style;
    float sx = 0.0f, sy = 0.0f, scale = 0.0f;
    if (!uispace::to_pixels(static_cast<float>(pending.x), static_cast<float>(pending.y) + kNameBaseline, width, height, sx, sy, scale) ||
        !(scale > 0.0f) || !p.sprite.upload(device))
        return;
    // its pen across: the game's, or where target.name.align puts the picture's own width in the name's room
    float pen = static_cast<float>(pending.x);
    if (g_name_align != 0) {
        const float wide = p.sprite.exact_advance() / scale;
        const float left = static_cast<float>(pending.x - kNameLeft), window = static_cast<float>(pending.room + kNameRoom);
        pen = g_name_align == 1 ? left + (window - wide) * 0.5f : left + window - static_cast<float>(kNameLeft) - wide;
    }
    float px = 0.0f, unused = 0.0f;
    if (!uispace::to_pixels(pen + static_cast<float>(g_name_x), static_cast<float>(pending.y) + kNameBaseline, width, height, px, unused,
                            scale))
        return;
    p.sprite.draw(device, static_cast<int>(std::lround(px)) - p.sprite.pen_left(), static_cast<int>(std::lround(sy)) - p.sprite.baseline());
}
