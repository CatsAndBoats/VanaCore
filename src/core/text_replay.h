// text.cpp's: record and replay (text.hpp), the pictures drawn while recording is on drawn again cut to boxes. Included in
// text.cpp's namespace, before Sprite::draw, which notes each picture it draws while g_recording is on.

namespace {

// What record() notes of a picture drawn: drawn again by replay() if its texture is still the one drawn
struct Recorded {
    const Sprite* sprite = nullptr;
    const void* texture = nullptr;
    int x = 0, y = 0;
    float opacity = 0.0f, scale = 0.0f;
    bool cropped = false;
    Crop crop;
};
bool g_recording = false;
std::vector<Recorded> g_recorded;

} // namespace

void record(bool on)
{
    if (on)
        g_recorded.clear();
    g_recording = on;
}

void replay(void* device, const Crop* boxes, int count)
{
    g_recording = false;
    for (const Recorded& r : g_recorded) {
        if (r.sprite->texture() != r.texture)
            continue; // (made again since: not this frame's picture)
        for (int i = 0; i < count; ++i) {
            Crop cut = boxes[i];
            if (r.cropped) {
                cut.left = std::max(cut.left, r.crop.left), cut.top = std::max(cut.top, r.crop.top);
                cut.right = std::min(cut.right, r.crop.right), cut.bottom = std::min(cut.bottom, r.crop.bottom);
            }
            if (cut.right > cut.left && cut.bottom > cut.top)
                r.sprite->draw(device, r.x, r.y, r.opacity, r.scale, Wave{}, &cut);
        }
    }
    g_recorded.clear();
}
