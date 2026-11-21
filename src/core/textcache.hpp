#pragma once

// The pictures of texts drawn whole through True Font each frame (labeltext.cpp, macrotext.cpp): each made once and kept
// while it's drawn, the least lately used giving way to a new one, all let go when the device changes.

#include <cstddef>
#include <string>

#include "text.hpp"

namespace vanacore::text {

template <std::size_t N> class SpriteCache {
public:
    // The picture of `text` in `style`, made now unless one is kept.
    Sprite& get(const std::wstring& text, const Style& style)
    {
        Kept* oldest = &kept_[0];
        for (Kept& k : kept_) {
            if (k.used != 0 && k.text == text && k.style == style) {
                k.used = stamp_;
                return k.sprite;
            }
            if (k.used < oldest->used)
                oldest = &k;
        }
        oldest->sprite.set(text, style);
        oldest->text = text, oldest->style = style, oldest->used = stamp_;
        return oldest->sprite;
    }

    // A new draw on `device`: the pictures are let go when it's another device than the last draw's.
    void begin(void* device)
    {
        if (device != device_) {
            for (Kept& k : kept_)
                k.sprite.release();
            device_ = device;
        }
        ++stamp_;
    }

private:
    struct Kept {
        Sprite sprite;
        std::wstring text;
        Style style;
        unsigned used = 0; // the draw it was last used in, 0 never
    };
    Kept kept_[N];
    unsigned stamp_ = 0;
    void* device_ = nullptr;
};

} // namespace vanacore::text
