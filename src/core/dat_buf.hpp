#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// A bounds-checked little-endian reader over a DAT's bytes (zonefloor.cpp, zonemesh.cpp): a read past the end gives 0
// (an empty name), never a fault.
namespace vanacore::dat_buf {

struct Buf {
    const std::uint8_t* p;
    std::size_t n;
    bool in(std::size_t at, std::size_t len) const { return at <= n && len <= n - at; }
    std::uint32_t u32(std::size_t at) const
    {
        std::uint32_t v = 0;
        if (in(at, 4))
            std::memcpy(&v, p + at, 4);
        return v;
    }
    std::uint16_t u16(std::size_t at) const
    {
        std::uint16_t v = 0;
        if (in(at, 2))
            std::memcpy(&v, p + at, 2);
        return v;
    }
    float f32(std::size_t at) const
    {
        float v = 0.0f;
        if (in(at, 4))
            std::memcpy(&v, p + at, 4);
        return v;
    }
    std::string name(std::size_t at) const // 16 characters, cut at a NUL, trailing spaces dropped
    {
        std::string s;
        for (std::size_t i = 0; i < 16 && in(at + i, 1) && p[at + i] != 0; ++i)
            s += static_cast<char>(p[at + i]);
        while (!s.empty() && s.back() == ' ')
            s.pop_back();
        return s;
    }
};

} // namespace vanacore::dat_buf
