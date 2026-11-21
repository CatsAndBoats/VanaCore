// statuswindow.cpp's, inside its namespace: the Status window's player name (status=1) drawn whole in its own True Font
// (status.name.font on), one picture of VanaCore's text where the menu font's big name stood. The Name row's Large and Game
// (status.name) went for it, 2026-11-08: "a bit redundant when we have true font now". From the row's left, its
// baseline where the menu font's capitals stood (kMenuCellBaseline below the row's top), made smaller to fit the window's
// room if it must, in status.name.color with the font's outline, drawn in VanaCore's interface pass (draw). Its True Font
// off: the menu font's name as before (draw_big_name).

constexpr float kMenuCellBaseline = 14.0f; // the menu font's 16 x 16 cell: its capitals on rows 2 to 13
constexpr std::size_t kNameLetters = 16;  // a character's name: 15 letters at most

struct NameRow {
    bool set = false;
    int x = 0, y = 0, room = 0; // the row's left and top, and how wide the name may be (the interface's units)
    std::string text;
};
NameRow g_name_row; // this frame's, kept by the window's draw for draw
struct NameSprite {
    text::Sprite sprite;
    std::wstring text;
    text::Style style;
    void* device = nullptr;
} g_name_sprite;

// The name's True Font as built, in status.name.color (the game's 80h full, so doubled), its outline in the same picture.
// False while it's off (or its letters are left out: size 0)
bool name_style(text::Style& style, bool& upper)
{
    text::Style letters, stroked;
    float opacity = 0.0f;
    if (!chatfont::area_style(VANA_FONT_STATUS_NAME, letters, stroked, upper, opacity))
        return false;
    style = letters;
    for (int i = 0; i < 3; ++i)
        style.color[i] = std::min(255.0f, static_cast<float>((g_name_colour >> (16 - 8 * i)) & 0xFF) * 2.0f);
    style.color[3] = 1.0f;
    style.stroke = stroked.stroke, style.exact_stroke = stroked.exact_stroke;
    style.stroke_color[3] = stroked.stroke > 0 ? opacity : 0.0f;
    style.background[3] = 0.0f;
    return true;
}

// The name kept to be drawn whole this frame while its True Font is on (true: the window draws no name of its own; its
// size 0, none at all)
bool keep_name(const char* name, int x, int y, int room)
{
    text::Style style;
    bool upper = false;
    if (name == nullptr || !chatfont::area_on(VANA_FONT_STATUS_NAME))
        return false;
    if (!name_style(style, upper))
        return true;
    g_name_row.set = true;
    g_name_row.x = x, g_name_row.y = y, g_name_row.room = room;
    g_name_row.text.assign(name, strnlen(name, kNameLetters));
    return true;
}

// The name kept this frame, drawn: its picture made again when its text or style changed (or the device did)
void draw_name(void* device, unsigned width, unsigned height)
{
    const NameRow row = g_name_row;
    g_name_row.set = false;
    text::Style style;
    bool upper = false;
    if (!row.set || !name_style(style, upper))
        return;
    std::wstring shown = gametext::to_wide(row.text);
    if (upper)
        for (wchar_t& c : shown)
            c = static_cast<wchar_t>(std::towupper(c));
    NameSprite& s = g_name_sprite;
    if (device != s.device)
        s.sprite.release(), s.device = device;
    if (shown != s.text || !(style == s.style))
        s.sprite.set(shown, style), s.text = shown, s.style = style;
    float px = 0.0f, py = 0.0f, scale = 0.0f;
    if (!uispace::to_pixels(static_cast<float>(row.x), static_cast<float>(row.y) + kMenuCellBaseline, width, height, px, py, scale) ||
        !(scale > 0.0f) || !s.sprite.upload(device))
        return;
    // narrower than the window's room: as it is; wider, drawn smaller, on the same baseline
    const float wide = s.sprite.exact_advance() / scale;
    const float fit = wide > static_cast<float>(row.room) && wide > 0.0f ? static_cast<float>(row.room) / wide : 1.0f;
    s.sprite.draw(device, static_cast<int>(std::lround(px - s.sprite.pen_left() * fit)),
                  static_cast<int>(std::lround(py - s.sprite.baseline() * fit)), 1.0f, fit);
}
