// targetwin.cpp's, inside its namespace: the Target window's name drawn whole in its True Font (target.font on), one picture of
// VanaCore's text in place of the game's letters. 2026-11-07: "we should be replacing the whole target name ... and just
// writing it as pure true font". The game draws a name's ':', '"' and ''' from the labels' smaller letters, which True Font
// gave the title's font ("Door:\"Neptune's Spire\"" came out with them small and apart). Now every letter is the font's, at
// its size, by the font's own advances: from the game's pen (aligned by target.name.align by the picture's own width), its
// baseline kNameBaseline below it, in the game's colour for the name, with the font's outline, drawn in VanaCore's interface
// pass (draw). A name that fits the window is one line, 6 lower with `drop` (as the game's one-line draw puts one, a door's
// "Door:..." too); one that doesn't is two, as the game wraps it (2026-11-25: one line ran past a short window's edge): split
// after its ':', else at the last space the first line fits by, the second kWrapStep lower, both from the window's left.

constexpr float kNameBaseline = 10.5f; // interface units from the pen down to the name's baseline (the big letters' 21 texels, at half)
constexpr int kWrapStep = 12;          // the game's second line of a wrapped name, this much lower (kNameCall's note)

struct NamePending {
    bool set = false;
    int x = 0, y = 0, room = 0; // the game's pen (padding and spacing in) and the name's room, before alignment
    bool drop = false;          // the game's `drop`: a one-line name 6 lower
    std::string text;
    std::uint32_t colour = 0;   // the game's: ARGB, each to 0x80
};
NamePending g_name_pending;
struct NameLine {
    text::Sprite sprite;
    std::wstring text;
    text::Style style;
};
struct NamePicture {
    NameLine line[2];
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
bool name_whole(int x, int y, int room, bool drop, const char* text, std::uint32_t colour)
{
    if (text == nullptr || !name_font_on())
        return false;
    g_name_pending.set = true;
    g_name_pending.x = x, g_name_pending.y = y, g_name_pending.room = room, g_name_pending.drop = drop;
    g_name_pending.text.assign(text, strnlen(text, kNameMost));
    g_name_pending.colour = colour;
    return true;
}

// Where a name too wide for `room` (interface units, by the True Font's advances) is split: the length of its first line,
// and where its second starts; 0, 0 when it fits (or has nowhere to split)
std::pair<std::size_t, std::size_t> name_split(const std::string& name, int room)
{
    const auto wide = [](const std::string& part) { return chatfont::text_width(VANA_FONT_TARGET, part.c_str()); };
    if (room <= 0 || wide(name) <= static_cast<float>(room))
        return {0, 0};
    const std::size_t colon = name.find(':');
    if (colon != std::string::npos && colon + 1 < name.size())
        return {colon + 1, colon + 1};
    std::size_t best = std::string::npos;
    for (std::size_t at = name.find(' '); at != std::string::npos; at = name.find(' ', at + 1)) {
        if (best != std::string::npos && wide(name.substr(0, at)) > static_cast<float>(room))
            break;
        best = at; // (the first space even when its line doesn't fit: as close as the name comes)
    }
    if (best == std::string::npos || best == 0)
        return {0, 0};
    return {best, best + 1};
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

// One line of the name drawn, its pen at `pen`, its baseline kNameBaseline below `y` (interface units); its picture made
// again when its text or style changed. Its picture's own width, interface units, for the alignment (-1: not drawn)
struct NameFrame {
    void* device;
    unsigned width, height;
    const NamePending& pending;
    const text::Style& style;
};
void draw_name_line(const NameFrame& f, NameLine& line, const std::wstring& shown, float y, bool aligned)
{
    const NamePending& pending = f.pending;
    void* const device = f.device;
    const unsigned width = f.width, height = f.height;
    float pen = static_cast<float>(pending.x);
    if (shown != line.text || !(f.style == line.style))
        line.sprite.set(shown, f.style), line.text = shown, line.style = f.style;
    float sx = 0.0f, sy = 0.0f, scale = 0.0f;
    if (!uispace::to_pixels(pen, y + kNameBaseline, width, height, sx, sy, scale) || !(scale > 0.0f) || !line.sprite.upload(device))
        return;
    // its pen across: the game's, or where target.name.align puts the picture's own width in the name's room
    if (aligned && g_name_align != 0) {
        const float wide = line.sprite.exact_advance() / scale;
        const float left = static_cast<float>(pending.x - kNameLeft), window = static_cast<float>(pending.room + kNameRoom);
        pen = g_name_align == 1 ? left + (window - wide) * 0.5f : left + window - static_cast<float>(kNameLeft) - wide;
    }
    float px = 0.0f, unused = 0.0f;
    if (!uispace::to_pixels(pen + static_cast<float>(g_name_x), y + kNameBaseline, width, height, px, unused, scale))
        return;
    line.sprite.draw(device, static_cast<int>(std::lround(px)) - line.sprite.pen_left(),
                     static_cast<int>(std::lround(sy)) - line.sprite.baseline());
}

// The name kept this frame, drawn: on one line when it fits the window, else on two (the device's change remakes them)
void draw_name(void* device, unsigned width, unsigned height)
{
    const NamePending pending = g_name_pending;
    g_name_pending.set = false;
    text::Style letters, stroked;
    bool upper = false;
    float opacity = 0.0f;
    if (!pending.set || !chatfont::area_style(VANA_FONT_TARGET, letters, stroked, upper, opacity))
        return;
    std::string name = pending.text;
    if (upper)
        for (char& c : name)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const text::Style style = name_style(letters, stroked, opacity, pending.colour);
    NamePicture& p = g_name_picture;
    if (device != p.device) {
        for (NameLine& line : p.line)
            line.sprite.release(), line.text.clear();
        p.device = device;
    }
    const std::wstring whole = gametext::to_wide(pending.text);
    const auto shown = [upper](std::wstring s) {
        if (upper)
            for (wchar_t& c : s)
                c = static_cast<wchar_t>(std::towupper(c));
        return s;
    };
    // (target.name.oneline: always one line, past the window's edge when it's wider)
    const auto [first, second] = g_one_line ? std::pair<std::size_t, std::size_t>{0, 0} : name_split(name, pending.room);
    const NameFrame frame{device, width, height, pending, style};
    const float y = static_cast<float>(pending.y);
    if (first == 0) {
        draw_name_line(frame, p.line[0], shown(whole), y + (pending.drop ? kLineDrop : 0), true);
        return;
    }
    draw_name_line(frame, p.line[0], shown(gametext::to_wide(pending.text.substr(0, first))), y, false);
    draw_name_line(frame, p.line[1], shown(gametext::to_wide(pending.text.substr(second))), y + static_cast<float>(kWrapStep), false);
}
