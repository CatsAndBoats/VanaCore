#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

// Pictures on the game's screen: PNG, BMP, JPEG and the like decoded with WIC, drawn from a
// texture as a quad of any size, tinted. Drawing is on the game's render thread (Present), between
// text::begin and text::end.
namespace vanacore::image {

// An image's pixels, 0xAARRGGBB (straight alpha), row by row.
struct Pixels {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> argb;
};

// Decodes an image file, or one in memory. False if WIC can't read it.
bool decode_file(const std::wstring& path, Pixels& out);
bool decode(const void* data, std::size_t size, Pixels& out);
// A module's resource `name` (a string or MAKEINTRESOURCE id), of type RCDATA, PNG or BITMAP.
bool decode_resource(HMODULE module, const char* name, Pixels& out);

class Texture {
public:
    ~Texture() { release(); }
    // New pixels; the texture is refilled on the next draw.
    void set(Pixels pixels);
    bool empty() const { return pixels_.argb.empty(); }
    int width() const { return pixels_.width; }
    int height() const { return pixels_.height; }

    // Draws the image stretched over `width` x `height` at (x, y), tinted by `argb` (0xAARRGGBB,
    // multiplied with the image's colours), repeated `repeat_x` x `repeat_y` times across it.
    void draw(void* device, float x, float y, float width, float height, std::uint32_t argb, float repeat_x = 1.0f,
              float repeat_y = 1.0f);
    // Frees the texture (the device is going away or changing).
    void release();

private:
    bool upload(void* device);
    Pixels pixels_;
    void* texture_ = nullptr; // IDirect3DTexture8, managed pool, sized to the next powers of two
    UINT texture_width_ = 0;
    UINT texture_height_ = 0;
    bool stale_ = true;
};

} // namespace vanacore::image
