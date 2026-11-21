// lua_api.cpp's: a text box's gradient property. Included in its unnamed namespace, after read_colour.

// A text box's gradient (the style's, text.hpp Style::gradient): `gradient` "down" (the first colour at the top of the letters,
// the second at their baseline), "across" (the first at the left of the text, the second at its right, shaded by the pixel, not
// by the letter as color_codes does) or "none" (or false); `gradient_from` and `gradient_to` its two colours. `is_gradient` is
// whether `key` is one of them; returns what's wrong, or nullptr.
const char* gradient_property(lua_State* L, ui::Box& box, const char* key, int index, bool& is_gradient)
{
    is_gradient = !box.rect && !box.image &&
                  (std::strcmp(key, "gradient") == 0 || std::strcmp(key, "gradient_from") == 0 || std::strcmp(key, "gradient_to") == 0);
    if (!is_gradient)
        return nullptr;
    text::Style& style = box.style;
    if (std::strcmp(key, "gradient") != 0) {
        float colour[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        if (!read_colour(L, index, colour))
            return "expected \"#RRGGBB\", \"#RRGGBBAA\" or 0xAARRGGBB";
        std::memcpy(style.gradient_colors[std::strcmp(key, "gradient_from") == 0 ? 0 : 1], colour, 3 * sizeof(float));
        return nullptr;
    }
    if (lua_type(L, index) == LUA_TBOOLEAN && lua_toboolean(L, index) == 0) {
        style.gradient = 0;
        return nullptr;
    }
    const char* mode = lua_type(L, index) == LUA_TSTRING ? lua_tostring(L, index) : "";
    if (std::strcmp(mode, "none") == 0) {
        style.gradient = 0;
    } else if (std::strcmp(mode, "down") == 0 || std::strcmp(mode, "across") == 0) {
        style.gradient = 2;
        style.gradient_across = mode[0] == 'a';
    } else {
        return "expected \"down\", \"across\" or \"none\"";
    }
    return nullptr;
}
