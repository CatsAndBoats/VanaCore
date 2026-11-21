#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Text on the game's screen: rendered with GDI into a texture when it changes, drawn from the
// texture every frame. Only used from the game's render thread (Present).
namespace vanacore::text {

// How text looks. Colours are red, green, blue (0-255) and opacity (0-1).
struct Style {
    std::wstring family = L"Segoe UI";
    int size = 16; // pixels
    int weight = FW_SEMIBOLD;
    bool italic = false;
    float color[4] = {255.0f, 255.0f, 255.0f, 1.0f};
    // The text's fill from top to bottom instead of `color` (its opacity still `color`'s): 0 none, 2 from gradient[0] to
    // gradient[1], 3 through gradient[1] to gradient[2]. It runs from the first line's highest drawn pixel to its baseline
    // (a digit's height), the colours past those ends kept; `gradient_across`, from the leftmost drawn pixel to the
    // rightmost.
    int gradient = 0;
    float gradient_colors[3][3] = {};
    bool gradient_across = false;
    int stroke = 0; // outline around the text, in pixels
    float stroke_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float background[4] = {0.0f, 0.0f, 0.0f, 0.55f};
    int padding = -1; // between the text and the edge of its box, in pixels; -1 grows it with the font
    bool align_right = false; // lines line up on the right
    bool color_codes = false; // "\cs(r,g,b)" colours the text after it, "\cr" goes back to `color`
    bool trim = false; // the box cut to the text's drawn pixels (its outline too), not its lines, then `padding` round them
    float spacing = 0.0f; // pixels more between letters (below 0, closer), on top of the font's own
    // Between whole pixels, for text that grows smoothly: the font's size and the outline's (0: `size`'s and `stroke`'s;
    // `stroke` at least as wide). GDI's fonts take `size`.
    float exact_size = 0.0f;
    float exact_stroke = 0.0f;
    // For text drawn scaled: the letters' shapes as the font has them at any size, not fitted to whole pixels (which
    // makes each size's a little different) (DirectWrite on Windows 8.1 on)
    bool scalable = false;
    // Each letter on a whole pixel and fitted to the pixels, as Windows' own text boxes draw (DirectWrite's GDI-compatible
    // widths and hinting, in grey): sharper upright strokes, no part-pixel gaps between letters (the chat line's: chatline.cpp)
    bool crisp = false;

    bool operator==(const Style&) const = default;
};

// An image file (PNG, JPEG, BMP, GIF: whatever Windows' WIC reads) as straight-alpha ARGB, scaled
// with a high-quality filter to `width` x `height` (0 for its own size; one of them 0 keeps its shape).
// False if it can't be read.
bool load_image(const std::wstring& path, int& width, int& height, std::vector<std::uint32_t>& pixels);

// "#RRGGBB" (opacity unchanged) or "#RRGGBBAA", the # optional, into `colour`. False, changing
// nothing, if it's neither.
bool parse_colour(std::wstring_view text, float (&colour)[4]);

// 0xAARRGGBB into `colour`.
void argb_colour(std::uint32_t argb, float (&colour)[4]);

// "#RRGGBBAA".
std::wstring hex(const float (&colour)[4]);

// The face Windows uses for `family` (which differs when it isn't installed).
std::wstring installed_face(const Style& style);

// Every text drawn with GDI (each letter on a whole pixel), none with DirectWrite ([client] truefont.gdi=1): a font installed
// in Windows is then drawn as one in VanaCore's fonts folder always is. Pictures made before keep their look; the kept
// ones are let go.
void set_gdi_only(bool on);

// Text (lines split at \n) on its background box.
// A picture drawn as a wave (Sprite::draw): each column of it moved up or down by amplitude * sin(2 pi x / length + phase)
// pixels, x the column's distance from the picture's left edge. No amplitude or no length: flat.
struct Wave {
    float amplitude = 0.0f; // pixels
    float length = 0.0f;    // crest to crest, in pixels
    float phase = 0.0f;     // radians, where along the wave the left edge is
};

// The part of a picture drawn (Sprite::draw): the pixels of the draw's own space from `left` to `right` and `top` to `bottom`,
// the rest cut off. Not for a wave.
struct Crop {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

class Sprite {
public:
    // Re-renders the image if the text or style changed.
    void set(std::wstring text, const Style& style);
    // A picture instead of text: `pixels` (straight-alpha ARGB, width x height, top row first).
    void set_image(std::vector<std::uint32_t> pixels, int width, int height);
    const std::wstring& text() const { return text_; }
    const std::vector<std::uint32_t>& pixels() const { return pixels_; } // the image: straight-alpha ARGB, width() x height()
    int width() const { return width_; }
    int height() const { return height_; }
    // The text's first line's baseline, in the image's rows from its top (0 for a picture)
    int baseline() const { return baseline_; }
    // Where the text's first line is written from (its pen's start), in the image's columns from its left (past the
    // padding; less with `trim`, which cuts to the drawn pixels), and how far the pen moves over the line: the font's
    // advance, which two sprites' text is set end to end by, as if one string (0 for a picture)
    int pen_left() const { return pen_left_; }
    int advance() const { return advance_; }
    // Unrounded, for placing the text by its letters rather than its image's box: its first line's middle (half the
    // font's line height above its bottom), in the image's rows from its top, and its advance (0 for a picture)
    float middle() const { return middle_; }
    float exact_advance() const { return exact_advance_; }

    // Fills the texture from the image if it changed. False if there's nothing to draw or no texture.
    bool upload(void* device);
    // Draws the texture with its top-left corner at (x, y), `opacity` (0-1) times as opaque as it is, `scale` times its size
    // (filtered when it isn't 1). Between begin() and end().
    // `wave`: the picture as a wave, one string whose letters move together (cut into columns, so it costs a few hundred vertices).
    // `crop`: only that part of it (its edges' pixels as the picture's own are placed), none of it when it holds nothing.
    void draw(void* device, int x, int y, float opacity = 1.0f, float scale = 1.0f, const Wave& wave = {},
              const Crop* crop = nullptr) const;
    // Draws it dissolved: `amount` 0 (whole) to 1 (gone), its pixels eaten away in a noise (the same one each time, a patchy one
    // that takes the picture a speck at a time as it grows) with a burning edge along the front, in `edge` (r, g, b 0-255, a 0-1:
    // a 0 alpha is no edge) toward white-hot. Costs the texture being filled again each time it changes.
    void set_dissolve(float amount, const float (&edge)[4]);
    // Frees the texture (the device is going away or changing); upload() makes a new one.
    void release();
    // The texture upload() filled (IDirect3DTexture8; nullptr before), for drawing it some other way; the image is
    // its top-left width() x height(), of texture_width() x texture_height().
    void* texture() const { return texture_; }
    unsigned texture_width() const { return texture_width_; }
    unsigned texture_height() const { return texture_height_; }

private:
    void draw_quad(void* device, int x, int y, float opacity, float scale, const Wave& wave, const Crop* crop) const;
    std::wstring text_;
    Style style_;
    bool rendered_ = false;
    int width_ = 0;
    int height_ = 0;
    int baseline_ = 0;
    int pen_left_ = 0;
    int advance_ = 0;
    float middle_ = 0.0f;
    float exact_advance_ = 0.0f;
    std::vector<std::uint32_t> pixels_; // straight-alpha ARGB
    void* texture_ = nullptr;           // IDirect3DTexture8, D3DPOOL_MANAGED so it survives Reset
    UINT texture_width_ = 0;
    UINT texture_height_ = 0;
    bool stale_ = true; // image changed since the texture was filled
    float dissolve_ = 0.0f;
    float edge_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<std::uint8_t> noise_; // the dissolve's: one 0-255 a pixel, evenly spread, for noise_width_ x noise_height_
    int noise_width_ = 0;
    int noise_height_ = 0;
};

// Starts drawing sprites on `device`'s `width` x `height` back buffer: saves the game's render
// state and sets VanaCore's. Returns false (and draws nothing) if it can't; otherwise call end().
bool begin(void* device, UINT width, UINT height, DWORD& saved_state);
// Fills a `width` x `height` rectangle at (x, y) with `argb` (0xAARRGGBB, blended). Between begin()
// and end().
void fill(void* device, int x, int y, int width, int height, std::uint32_t argb);

// Draws `texture` (an IDirect3DTexture8) stretched over a `width` x `height` rectangle at (x, y), from texture
// coordinates (u0, v0) to (u1, v1) (repeating past 0-1), times `argb` (0xAARRGGBB), filtered. Between begin() and end().
void draw_texture(void* device, void* texture, float x, float y, float width, float height, float u0, float v0, float u1,
                  float v1, std::uint32_t argb);

// Finishes drawing and puts the game's render state back.
void end(void* device, DWORD saved_state);

// The pictures drawn while recording is on (not a wave) noted, to be drawn again by replay(): VanaCore's text over the game's
// windows (the macro bar's names, the item lists', the Config windows'...) where raise.cpp then lays a window back over
// everything, which covered that text (reported 2026-11-14: the macro bar's names and key labels gone, the bar meeting an
// addon's window under it). record(true) forgets the last frame's.
void record(bool on);
// The pictures recorded drawn again, each cut to each of `boxes` (the draw's pixels), then forgotten. Between begin() and end();
// with no boxes, only forgotten.
void replay(void* device, const Crop* boxes, int count);

// As begin() and end(), inside a scene someone else began (an Ashita addon's font drawn by hand, in its
// present event): the state alone, saved and set, then put back.
bool prepare(void* device, UINT width, UINT height, DWORD& saved_state);
void restore(void* device, DWORD saved_state);

} // namespace vanacore::text
