#include "dat_textures.hpp"

#include <cstring>

namespace vanacore::dat_textures {
namespace {

constexpr unsigned kTexture = 0x20;
constexpr unsigned kLargest = 4096;

std::uint32_t u32(const std::uint8_t* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t u16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

bool known_type(std::uint8_t t)
{
    return t == 0x01 || t == 0x05 || t == 0x81 || t == 0x91 || t == 0xA1 || t == 0xB1;
}

void rgb565(std::uint16_t v, int* c)
{
    c[0] = ((v >> 11) & 31) * 255 / 31;
    c[1] = ((v >> 5) & 63) * 255 / 63;
    c[2] = (v & 31) * 255 / 31;
}

// DXT1 (kind 1) or DXT3 (kind 3), the blocks as DirectX lays them out
bool dxt(const std::uint8_t* p, std::size_t left, unsigned w, unsigned h, int kind, std::vector<std::uint32_t>& out)
{
    const unsigned bw = (w + 3) / 4, bh = (h + 3) / 4, bs = kind == 1 ? 8 : 16;
    if (static_cast<std::size_t>(bw) * bh * bs > left)
        return false;
    out.assign(static_cast<std::size_t>(w) * h, 0);
    for (unsigned by = 0; by < bh; ++by)
        for (unsigned bx = 0; bx < bw; ++bx, p += bs) {
            const std::uint8_t* c = p + bs - 8;
            const std::uint16_t c0 = u16(c), c1 = u16(c + 2);
            int col[4][3];
            rgb565(c0, col[0]);
            rgb565(c1, col[1]);
            const bool four = kind != 1 || c0 > c1;
            for (int k = 0; k < 3; ++k) {
                col[2][k] = four ? (2 * col[0][k] + col[1][k]) / 3 : (col[0][k] + col[1][k]) / 2;
                col[3][k] = four ? (col[0][k] + 2 * col[1][k]) / 3 : 0;
            }
            const std::uint32_t bits = u32(c + 4);
            for (unsigned i = 0; i < 16; ++i) {
                const unsigned x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= w || y >= h)
                    continue;
                const unsigned s = (bits >> (2 * i)) & 3;
                std::uint32_t a = 255;
                if (kind == 3)
                    a = ((p[i / 2] >> (4 * (i & 1))) & 15) * 17;
                else if (!four && s == 3)
                    a = 0;
                out[static_cast<std::size_t>(y) * w + x] =
                    a << 24 | static_cast<std::uint32_t>(col[s][0]) << 16 | static_cast<std::uint32_t>(col[s][1]) << 8 | col[s][2];
            }
        }
    return true;
}

} // namespace

std::string trimmed(const std::string& name)
{
    std::size_t n = name.size();
    while (n > 0 && (name[n - 1] == ' ' || name[n - 1] == '\0'))
        --n;
    return name.substr(0, n);
}

std::vector<Header> list(const std::vector<std::uint8_t>& dat)
{
    std::vector<Header> out;
    for (std::size_t pos = 0; pos + 16 <= dat.size();) {
        const std::uint32_t meta = u32(&dat[pos + 4]);
        const std::size_t size = static_cast<std::size_t>((meta >> 7) & 0x7FFFF) * 16;
        if (size < 16)
            break;
        const std::size_t o = pos + 16;
        if ((meta & 0x7F) == kTexture && o + 57 <= dat.size() && known_type(dat[o])) {
            Header h;
            h.name.assign(reinterpret_cast<const char*>(&dat[o + 1]), 16);
            h.width = u32(&dat[o + 21]);
            h.height = u32(&dat[o + 25]);
            h.offset = o;
            if (h.width > 0 && h.height > 0 && h.width <= kLargest && h.height <= kLargest)
                out.push_back(std::move(h));
        }
        pos = (pos + size + 15) & ~static_cast<std::size_t>(15);
    }
    return out;
}

bool decode(const std::vector<std::uint8_t>& dat, const Header& header, std::vector<std::uint32_t>& argb)
{
    const std::size_t o = header.offset;
    if (o + 57 > dat.size())
        return false;
    const std::uint8_t type = dat[o];
    const unsigned w = header.width, h = header.height;
    const std::uint16_t bits = u16(&dat[o + 31]);
    const std::uint32_t palette_bits = u32(&dat[o + 53]);
    std::size_t p = o + 57;
    if (type == 0xA1) {
        if (p + 12 > dat.size())
            return false;
        const bool one = std::memcmp(&dat[p], "1TXD", 4) == 0, three = std::memcmp(&dat[p], "3TXD", 4) == 0;
        p += 12;
        if (!one && !three)
            return false;
        return dxt(&dat[p], dat.size() - p, w, h, one ? 1 : 3, argb);
    }
    if (type == 0xB1)
        p += 4;
    const std::size_t count = static_cast<std::size_t>(w) * h;
    std::uint32_t palette[256] = {};
    if (bits != 32) {
        if (palette_bits == 16) {
            if (p + 512 > dat.size())
                return false;
            for (int i = 0; i < 256; ++i, p += 2) {
                const std::uint16_t v = u16(&dat[p]);
                palette[i] = (v & 0x8000 ? 0xFF000000u : 0u) | static_cast<std::uint32_t>(((v >> 10) & 31) * 255 / 31) << 16 |
                             static_cast<std::uint32_t>(((v >> 5) & 31) * 255 / 31) << 8 | (v & 31) * 255 / 31;
            }
        } else {
            if (p + 1024 > dat.size())
                return false;
            for (int i = 0; i < 256; ++i, p += 4)
                palette[i] = u32(&dat[p]); // stored B G R A: 0xAARRGGBB read little-endian
        }
    }
    if (p + count * (bits == 32 ? 4 : 1) > dat.size())
        return false;
    argb.assign(count, 0);
    // rows are stored bottom first
    for (unsigned y = 0; y < h; ++y) {
        std::uint32_t* row = &argb[static_cast<std::size_t>(h - 1 - y) * w];
        for (unsigned x = 0; x < w; ++x)
            if (bits == 32)
                row[x] = u32(&dat[p]), p += 4;
            else
                row[x] = palette[dat[p++]];
    }
    return true;
}

bool find(const std::vector<std::uint8_t>& dat, const std::string& name, Header& header)
{
    const std::string want = trimmed(name);
    for (Header& h : list(dat))
        if (trimmed(h.name) == want) {
            header = std::move(h);
            return true;
        }
    return false;
}

} // namespace vanacore::dat_textures
