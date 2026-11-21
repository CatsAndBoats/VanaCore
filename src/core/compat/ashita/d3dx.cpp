// D3DX 8's texture loading and sprites, for Ashita addons: their `d3d8` library declares D3DX's
// functions in ffi.C, where LuaJIT finds them among the exports of the module it's linked into
// (VanaCore.Core.dll). Ashita links D3DX 8 into its core; the Windows SDK no longer has it, so the part
// addons use is here: textures from image files and memory (WIC: PNG, BMP, JPEG, GIF, TIFF, and the
// game's item icons, whose bitmaps come without a file header), with a full mip chain and D3DX's colour
// key, and ID3DXSprite, drawn through the game's Direct3D 8 device with DrawPrimitiveUP.

#include <windows.h>
#include <wincodec.h>

#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <cwctype>

#include "../../d3d8.hpp"
#include "../../layout.hpp"
#include "../../log.hpp"
#include "backend.hpp"
#include "host.hpp"

#pragma comment(lib, "windowscodecs.lib")

namespace vanacore::ashita {
namespace {

using d3d8::call;

constexpr UINT kDefault = 0xFFFFFFFFu;           // D3DX_DEFAULT
constexpr HRESULT kInvalidCall = static_cast<HRESULT>(0x8876086Cu); // D3DERR_INVALIDCALL
constexpr HRESULT kInvalidData = static_cast<HRESULT>(0x88760B59u); // D3DXERR_INVALIDDATA
constexpr DWORD kPoolDefault = 0;
constexpr DWORD kUsageDynamic = 0x200;
constexpr DWORD kTextureResource = 3; // D3DRTYPE_TEXTURE
constexpr int kAddRef = 1;
constexpr int kTextureGetLevelDesc = 14;

// D3DXIMAGE_INFO
struct ImageInfo {
    UINT width, height, depth, mip_levels;
    DWORD format, resource_type, file_format; // D3DFORMAT, D3DRESOURCETYPE, D3DXIMAGE_FILEFORMAT
};

struct Image {
    UINT width = 0, height = 0;
    DWORD file_format = 0;            // D3DXIFF_*: 0 BMP, 1 JPG, 3 PNG, 6 DIB
    std::vector<std::uint32_t> pixels; // 0xAARRGGBB, straight alpha
};

// A DIB (BITMAPINFOHEADER first, no file header: the game's item icons) as a BMP file WIC reads.
bool dib_to_bmp(const std::uint8_t* data, size_t size, std::vector<std::uint8_t>& out)
{
    if (size < sizeof(BITMAPINFOHEADER))
        return false;
    BITMAPINFOHEADER h;
    std::memcpy(&h, data, sizeof h);
    if (h.biSize < sizeof(BITMAPINFOHEADER) || h.biSize > 124 || h.biWidth <= 0 || h.biWidth > 4096 || h.biHeight == 0 ||
        std::abs(h.biHeight) > 4096 || h.biPlanes != 1)
        return false;
    DWORD palette = h.biClrUsed != 0 ? h.biClrUsed : h.biBitCount <= 8 ? 1u << h.biBitCount : 0;
    DWORD masks = h.biCompression == BI_BITFIELDS && h.biSize == sizeof(BITMAPINFOHEADER) ? 12 : 0;
    BITMAPFILEHEADER f{};
    f.bfType = 0x4D42; // "BM"
    f.bfSize = static_cast<DWORD>(sizeof f + size);
    f.bfOffBits = static_cast<DWORD>(sizeof f + h.biSize + masks + palette * 4);
    out.resize(sizeof f + size);
    std::memcpy(out.data(), &f, sizeof f);
    std::memcpy(out.data() + sizeof f, data, size);
    return true;
}

// Decodes an image file (from `path`, or `size` bytes at `data`), `want_w` x `want_h` if they're given.
// On the decoder's thread (Decoder), with its WIC factory.
bool decode_now(IWICImagingFactory* factory, const std::wstring& path, const std::uint8_t* data, size_t size, UINT want_w, UINT want_h,
                bool header_only, Image& out)
{
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    std::vector<std::uint8_t> bmp;
    bool ok = factory != nullptr;
    if (ok && data != nullptr) {
        // a bitmap without its file header (the game's item icons: from their BITMAPINFOHEADER, or after
        // the icon's type byte and 16-byte name before it)
        const bool bm = size >= 2 && data[0] == 'B' && data[1] == 'M';
        if (!bm && (dib_to_bmp(data, size, bmp) || (size > 17 && dib_to_bmp(data + 17, size - 17, bmp)))) {
            data = bmp.data(), size = bmp.size();
            out.file_format = 6; // D3DXIFF_DIB
        }
        ok = SUCCEEDED(factory->CreateStream(&stream)) &&
             SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size))) &&
             SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder));
    } else if (ok) {
        ok = SUCCEEDED(factory->CreateDecoderFromFilename(layout_path(path).c_str(), nullptr, GENERIC_READ,
                                                          WICDecodeMetadataCacheOnDemand, &decoder));
    }
    UINT w = 0, h = 0;
    ok = ok && SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w > 0 && h > 0;
    if (ok && out.file_format != 6) {
        GUID container{};
        decoder->GetContainerFormat(&container);
        out.file_format = container == GUID_ContainerFormatPng ? 3 : container == GUID_ContainerFormatJpeg ? 1 : 0;
    }
    if (ok) {
        out.width = want_w != 0 && want_w != kDefault ? want_w : w;
        out.height = want_h != 0 && want_h != kDefault ? want_h : h;
        if (want_w != 0 && want_w != kDefault && (want_h == 0 || want_h == kDefault))
            out.height = h, out.width = want_w;
        out.width = out.width > 4096 ? 4096 : out.width;
        out.height = out.height > 4096 ? 4096 : out.height;
    }
    if (ok && !header_only) {
        ok = SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
             SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                             WICBitmapPaletteTypeCustom));
        IWICBitmapSource* source = converter;
        if (ok && (out.width != w || out.height != h)) {
            ok = SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
                 SUCCEEDED(scaler->Initialize(converter, out.width, out.height, WICBitmapInterpolationModeFant));
            source = scaler;
        }
        if (ok) {
            out.pixels.assign(static_cast<size_t>(out.width) * out.height, 0);
            ok = SUCCEEDED(source->CopyPixels(nullptr, out.width * 4, static_cast<UINT>(out.pixels.size() * 4),
                                              reinterpret_cast<BYTE*>(out.pixels.data())));
        }
    }
    for (IUnknown* object : {static_cast<IUnknown*>(scaler), static_cast<IUnknown*>(converter), static_cast<IUnknown*>(frame),
                             static_cast<IUnknown*>(decoder), static_cast<IUnknown*>(stream)})
        if (object != nullptr)
            object->Release();
    return ok;
}

// The thread images are decoded on, for as long as the game runs, so the game thread's COM stays as the game set it. COM
// (multithreaded) and WIC's factory are set up on it once: a thread made for each image, starting and stopping COM, took
// about a millisecond, and XIUI loads 1,554 images on its first frame (two seconds of a frozen game).
class Decoder {
public:
    // Runs `job` there with the factory (nullptr if WIC couldn't start) and waits for it: false if it threw.
    bool run(const std::function<bool(IWICImagingFactory*)>& job)
    {
        std::lock_guard<std::mutex> one(caller_); // an image at a time
        std::unique_lock<std::mutex> lock(mutex_);
        if (!started_) {
            started_ = true;
            std::thread([this] { loop(); }).detach();
        }
        job_ = &job;
        done_ = false;
        wake_.notify_one();
        finished_.wait(lock, [this] { return done_; });
        return result_;
    }

private:
    void loop()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* factory = nullptr;
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return job_ != nullptr; });
            if (factory == nullptr)
                CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
            try {
                result_ = (*job_)(factory);
            } catch (...) {
                result_ = false; // out of memory for a big image's pixels
            }
            job_ = nullptr;
            done_ = true;
            finished_.notify_one();
        }
    }

    std::mutex caller_, mutex_;
    std::condition_variable wake_, finished_;
    const std::function<bool(IWICImagingFactory*)>* job_ = nullptr;
    bool started_ = false, done_ = false, result_ = false;
};

// Never destroyed: its thread waits on it until the process ends
Decoder& decoder()
{
    static Decoder* const one = new Decoder;
    return *one;
}

// A DIB's bytes by its header: info, masks, palette and pixels (0 if the header doesn't say)
size_t dib_bytes(const std::uint8_t* data)
{
    BITMAPINFOHEADER h;
    std::memcpy(&h, data, sizeof h);
    if (h.biSize < sizeof(BITMAPINFOHEADER) || h.biSize > 124 || h.biWidth <= 0 || h.biWidth > 4096 || h.biHeight == 0 ||
        std::abs(h.biHeight) > 4096 || h.biPlanes != 1 || h.biBitCount == 0)
        return 0;
    const size_t palette = h.biClrUsed != 0 ? h.biClrUsed : h.biBitCount <= 8 ? 1u << h.biBitCount : 0;
    const size_t masks = h.biCompression == BI_BITFIELDS && h.biSize == sizeof(BITMAPINFOHEADER) ? 12 : 0;
    const size_t row = (static_cast<size_t>(h.biWidth) * h.biBitCount + 31) / 32 * 4;
    const size_t pixels = h.biCompression == BI_RGB || h.biCompression == BI_BITFIELDS ? row * std::abs(h.biHeight) : h.biSizeImage;
    return pixels == 0 ? 0 : h.biSize + masks + palette * 4 + pixels;
}

// D3DX 8 reads a BMP by its header, so an addon can give D3DX_DEFAULT for a bitmap's size (tCrossBar's item and status
// icons on interface 4.16): the BMP's own size then (or the DIB's, after an icon's type and name), no more than can be
// read at `data`
size_t memory_size(const void* data, UINT size)
{
    if (size != kDefault)
        return size;
    size_t readable = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                PAGE_EXECUTE_WRITECOPY;
    if (VirtualQuery(data, &mbi, sizeof mbi) != 0 && mbi.State == MEM_COMMIT && (mbi.Protect & kReadable) != 0 &&
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0)
        readable = static_cast<const std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize - static_cast<const std::uint8_t*>(data);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    size_t own = 0;
    if (readable >= sizeof(BITMAPFILEHEADER) && bytes[0] == 'B' && bytes[1] == 'M') {
        BITMAPFILEHEADER f;
        std::memcpy(&f, bytes, sizeof f);
        own = f.bfSize;
    } else if (readable >= sizeof(BITMAPINFOHEADER)) {
        own = dib_bytes(bytes);
        if (own == 0 && readable >= 17 + sizeof(BITMAPINFOHEADER)) {
            own = dib_bytes(bytes + 17);
            own = own != 0 ? 17 + own : 0;
        }
    }
    return own != 0 && own < readable ? own : readable;
}

// A Targa image (D3DX reads them, Windows' own decoders don't: Deeps's bar is one), `size` bytes at `data`:
// 8-bit grey or colour-mapped, 16-, 24- or 32-bit colour, plain or run-length packed, either way up. With
// `header_only` just its size. False if it isn't one, or is cut short.
bool decode_tga(const std::uint8_t* data, size_t size, bool header_only, Image& out)
{
    if (data == nullptr || size < 18)
        return false;
    const unsigned id_length = data[0], map_type = data[1], type = data[2];
    const unsigned map_length = data[5] | data[6] << 8, map_bits = data[7];
    const unsigned width = data[12] | data[13] << 8, height = data[14] | data[15] << 8;
    const unsigned bits = data[16], descriptor = data[17];
    const bool packed = type >= 9;
    const unsigned kind = packed ? type - 8 : type; // 1 colour-mapped, 2 colour, 3 grey
    if (map_type > 1 || kind < 1 || kind > 3 || width == 0 || height == 0 || width > 4096 || height > 4096 ||
        (descriptor & 0xC0) != 0)
        return false;
    if ((kind == 1 && (bits != 8 || map_type != 1 || (map_bits != 16 && map_bits != 24 && map_bits != 32))) ||
        (kind == 2 && bits != 16 && bits != 24 && bits != 32) || (kind == 3 && bits != 8))
        return false;
    const unsigned bytes = (bits + 7) / 8, map_entry = (map_bits + 7) / 8;
    size_t at = 18 + id_length;
    const std::uint8_t* map = data + at;
    if (map_type == 1)
        at += static_cast<size_t>(map_length) * map_entry;
    if (at > size)
        return false;
    out.width = width;
    out.height = height;
    out.file_format = 2; // D3DXIFF_TGA
    if (header_only)
        return true;
    // a pixel's bytes (blue first) as 0xAARRGGBB; 16-bit is 1-5-5-5, opaque unless the descriptor gives it its alpha bit
    const bool alpha16 = (descriptor & 0x0F) != 0;
    const auto colour = [&](const std::uint8_t* p, unsigned depth) -> std::uint32_t {
        switch (depth) {
        case 32:
            return p[0] | p[1] << 8 | p[2] << 16 | static_cast<std::uint32_t>(p[3]) << 24;
        case 24:
            return 0xFF000000u | p[0] | p[1] << 8 | p[2] << 16;
        case 16: {
            const unsigned v = p[0] | p[1] << 8;
            const unsigned r = (v >> 10 & 31) * 255 / 31, g = (v >> 5 & 31) * 255 / 31, b = (v & 31) * 255 / 31;
            const std::uint32_t a = alpha16 && (v & 0x8000) == 0 ? 0 : 0xFF000000u;
            return a | r << 16 | g << 8 | b;
        }
        default:
            return 0xFF000000u | p[0] * 0x010101u;
        }
    };
    const auto pixel = [&](const std::uint8_t* p) -> std::uint32_t {
        if (kind == 1)
            return p[0] < map_length ? colour(map + p[0] * map_entry, map_bits) : 0;
        return colour(p, bits);
    };
    const bool top_down = (descriptor & 0x20) != 0, right_to_left = (descriptor & 0x10) != 0;
    const size_t count = static_cast<size_t>(width) * height;
    out.pixels.assign(count, 0);
    const auto put = [&](size_t i, std::uint32_t v) {
        unsigned x = static_cast<unsigned>(i % width), y = static_cast<unsigned>(i / width);
        if (!top_down)
            y = height - 1 - y;
        if (right_to_left)
            x = width - 1 - x;
        out.pixels[static_cast<size_t>(y) * width + x] = v;
    };
    size_t i = 0;
    if (!packed) {
        if (at + count * bytes > size)
            return false;
        for (; i < count; ++i)
            put(i, pixel(data + at + i * bytes));
        return true;
    }
    while (i < count) {
        if (at >= size)
            return false;
        const unsigned packet = data[at++], run = (packet & 0x7F) + 1;
        if ((packet & 0x80) != 0) {
            if (at + bytes > size)
                return false;
            const std::uint32_t v = pixel(data + at);
            at += bytes;
            for (unsigned k = 0; k < run && i < count; ++k)
                put(i++, v);
        } else {
            if (at + static_cast<size_t>(run) * bytes > size)
                return false;
            for (unsigned k = 0; k < run && i < count; ++k, at += bytes)
                put(i++, pixel(data + at));
        }
    }
    return true;
}

// The image at the size asked for (D3DX scales to it; here nearest pixel), its own when none is.
void resize_to(Image& out, UINT want_w, UINT want_h)
{
    UINT w = want_w != 0 && want_w != kDefault ? want_w : out.width;
    UINT h = want_h != 0 && want_h != kDefault ? want_h : out.height;
    w = w > 4096 ? 4096 : w;
    h = h > 4096 ? 4096 : h;
    if ((w == out.width && h == out.height) || w == 0 || h == 0 || out.width == 0 || out.height == 0)
        return;
    if (!out.pixels.empty()) {
        std::vector<std::uint32_t> pixels(static_cast<size_t>(w) * h);
        for (UINT y = 0; y < h; ++y) {
            const std::uint32_t* row = out.pixels.data() + static_cast<size_t>(y * out.height / h) * out.width;
            for (UINT x = 0; x < w; ++x)
                pixels[static_cast<size_t>(y) * w + x] = row[x * out.width / w];
        }
        out.pixels = std::move(pixels);
    }
    out.width = w;
    out.height = h;
}

bool is_tga(const std::wstring& path)
{
    if (path.size() < 4)
        return false;
    std::wstring end = path.substr(path.size() - 4);
    for (wchar_t& c : end)
        c = static_cast<wchar_t>(std::towlower(c));
    return end == L".tga";
}

bool decode(const std::wstring& path, const void* data, size_t size, UINT want_w, UINT want_h, bool header_only, Image& out)
{
    if (!path.empty() && is_tga(path)) {
        std::ifstream file(layout_path(path), std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (decode_tga(bytes.data(), bytes.size(), header_only, out)) {
            resize_to(out, want_w, want_h);
            return true;
        }
    }
    bool ok = decoder().run([&](IWICImagingFactory* factory) {
        return decode_now(factory, path, static_cast<const std::uint8_t*>(data), size, want_w, want_h, header_only, out);
    });
    if (!ok && data != nullptr && decode_tga(static_cast<const std::uint8_t*>(data), size, header_only, out)) {
        resize_to(out, want_w, want_h);
        ok = true;
    }
    return ok;
}

std::wstring wide(const char* text)
{
    if (text == nullptr)
        return {};
    const int length = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    std::wstring out(length > 0 ? length - 1 : 0, L'\0');
    if (length > 1)
        MultiByteToWideChar(CP_ACP, 0, text, -1, out.data(), length);
    return out;
}

void fill_info(const Image& image, ImageInfo* info)
{
    if (info == nullptr)
        return;
    *info = {image.width, image.height, 1, 1, d3d8::kFormatA8R8G8B8, kTextureResource, image.file_format};
}

// The image as a texture on `device`: D3DX's CreateTextureFromFileEx after the decoding.
HRESULT make_texture(void* device, Image& image, UINT levels, DWORD usage, DWORD format, DWORD pool, DWORD colour_key, void** texture)
{
    if (device == nullptr || texture == nullptr)
        return kInvalidCall;
    *texture = nullptr;
    // D3DX's colour key: that colour (opaque, as the file has it) becomes transparent black
    if (colour_key != 0)
        for (std::uint32_t& p : image.pixels)
            if (p == colour_key)
                p = 0;
    UINT most = 1;
    for (UINT s = image.width > image.height ? image.width : image.height; s > 1; s >>= 1)
        ++most;
    if (levels == 0 || levels == kDefault || levels > most)
        levels = most;
    if (format != d3d8::kFormatX8R8G8B8)
        format = d3d8::kFormatA8R8G8B8; // the one format made here
    if (pool == kPoolDefault && (usage & kUsageDynamic) == 0)
        pool = d3d8::kPoolManaged; // D3DX fills a default-pool texture through another; managed does as well
    void* made = nullptr;
    HRESULT hr = call(device, d3d8::slot::CreateTexture, image.width, image.height, levels, usage, format, pool, &made);
    if (FAILED(hr) || made == nullptr)
        return FAILED(hr) ? hr : kInvalidCall;
    // each level from the one before: a 2x2 box
    std::vector<std::uint32_t> level = image.pixels, next;
    UINT w = image.width, h = image.height;
    for (UINT l = 0; l < levels; ++l) {
        d3d8::LockedRect locked{};
        if (SUCCEEDED(call(made, d3d8::slot::TextureLockRect, l, &locked, static_cast<const RECT*>(nullptr), DWORD{0}))) {
            for (UINT y = 0; y < h; ++y)
                std::memcpy(static_cast<std::uint8_t*>(locked.bits) + static_cast<size_t>(y) * locked.pitch, level.data() + static_cast<size_t>(y) * w,
                            static_cast<size_t>(w) * 4);
            call(made, d3d8::slot::TextureUnlockRect, l);
        }
        if (l + 1 == levels)
            break;
        const UINT nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        next.assign(static_cast<size_t>(nw) * nh, 0);
        for (UINT y = 0; y < nh; ++y)
            for (UINT x = 0; x < nw; ++x) {
                std::uint32_t sum[4] = {};
                for (UINT dy = 0; dy < 2; ++dy)
                    for (UINT dx = 0; dx < 2; ++dx) {
                        const UINT sx = x * 2 + dx < w ? x * 2 + dx : w - 1, sy = y * 2 + dy < h ? y * 2 + dy : h - 1;
                        const std::uint32_t p = level[static_cast<size_t>(sy) * w + sx];
                        for (int c = 0; c < 4; ++c)
                            sum[c] += (p >> (c * 8)) & 0xFF;
                    }
                std::uint32_t p = 0;
                for (int c = 0; c < 4; ++c)
                    p |= ((sum[c] + 2) / 4) << (c * 8);
                next[static_cast<size_t>(y) * nw + x] = p;
            }
        level.swap(next);
        w = nw, h = nh;
    }
    *texture = made;
    return S_OK;
}

HRESULT from_file(void* device, const std::wstring& path, UINT width, UINT height, UINT levels, DWORD usage, DWORD format, DWORD pool,
                  DWORD colour_key, ImageInfo* info, void** texture)
{
    Image image;
    if (!decode(path, nullptr, 0, width, height, false, image)) {
        log::write(L"Ashita addons: D3DX couldn't load the image %ls", path.c_str());
        return kInvalidData;
    }
    fill_info(image, info);
    return make_texture(device, image, levels, usage, format, pool, colour_key, texture);
}

HRESULT from_memory(void* device, const void* data, UINT size, UINT width, UINT height, UINT levels, DWORD usage, DWORD format, DWORD pool,
                    DWORD colour_key, ImageInfo* info, void** texture)
{
    Image image;
    if (data == nullptr || size == 0 || !decode({}, data, memory_size(data, size), width, height, false, image))
        return kInvalidData;
    fill_info(image, info);
    return make_texture(device, image, levels, usage, format, pool, colour_key, texture);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// ID3DXSprite: screen-space quads of (part of) a texture, scaled, turned and moved, tinted by a colour.

struct Vector2 {
    float x, y;
};

struct Sprite {
    const void* const* vtable;
    LONG references = 1;
    void* device = nullptr;
    DWORD state = 0;
    bool begun = false;
};

// Sprites' drawing state (D3DX's): the texture times the colour, blended, no depth, the game's state
// saved first (put back at End).
HRESULT sprite_begin(Sprite* s)
{
    if (s->begun)
        return kInvalidCall;
    void* device = s->device;
    if (FAILED(call(device, d3d8::slot::CreateStateBlock, d3d8::kStateBlockAll, &s->state)))
        return kInvalidCall;
    const auto rs = [&](DWORD type, DWORD value) { call(device, d3d8::slot::SetRenderState, type, value); };
    rs(d3d8::rs::ZEnable, FALSE);
    rs(d3d8::rs::ZWriteEnable, FALSE);
    rs(d3d8::rs::FillMode, d3d8::kFillSolid);
    rs(d3d8::rs::CullMode, d3d8::kCullNone);
    rs(d3d8::rs::AlphaTestEnable, TRUE);
    rs(24, 0); // D3DRS_ALPHAREF
    rs(25, 5); // D3DRS_ALPHAFUNC: D3DCMP_GREATER
    rs(d3d8::rs::AlphaBlendEnable, TRUE);
    rs(d3d8::rs::SrcBlend, d3d8::kBlendSrcAlpha);
    rs(d3d8::rs::DestBlend, d3d8::kBlendInvSrcAlpha);
    rs(d3d8::rs::BlendOp, d3d8::kBlendOpAdd);
    rs(d3d8::rs::FogEnable, FALSE);
    rs(d3d8::rs::SpecularEnable, FALSE);
    rs(d3d8::rs::StencilEnable, FALSE);
    rs(d3d8::rs::Lighting, FALSE);
    rs(d3d8::rs::Clipping, TRUE);
    rs(d3d8::rs::VertexBlend, 0);
    rs(d3d8::rs::Wrap0, 0);
    rs(d3d8::rs::ColorWriteEnable, 0xF);
    const auto ts = [&](DWORD stage, DWORD type, DWORD value) { call(device, d3d8::slot::SetTextureStageState, stage, type, value); };
    ts(0, d3d8::tss::ColorOp, d3d8::kTopModulate);
    ts(0, d3d8::tss::ColorArg1, d3d8::kTaTexture);
    ts(0, d3d8::tss::ColorArg2, d3d8::kTaDiffuse);
    ts(0, d3d8::tss::AlphaOp, d3d8::kTopModulate);
    ts(0, d3d8::tss::AlphaArg1, d3d8::kTaTexture);
    ts(0, d3d8::tss::AlphaArg2, d3d8::kTaDiffuse);
    ts(0, d3d8::tss::TexCoordIndex, 0);
    ts(0, d3d8::tss::TextureTransformFlags, 0);
    ts(0, d3d8::tss::AddressU, d3d8::kAddressClamp);
    ts(0, d3d8::tss::AddressV, d3d8::kAddressClamp);
    ts(0, d3d8::tss::MagFilter, d3d8::kFilterLinear);
    ts(0, d3d8::tss::MinFilter, d3d8::kFilterLinear);
    ts(0, d3d8::tss::MipFilter, d3d8::kFilterNone);
    ts(1, d3d8::tss::ColorOp, d3d8::kTopDisable);
    ts(1, d3d8::tss::AlphaOp, d3d8::kTopDisable);
    call(device, d3d8::slot::SetVertexShader, d3d8::kFvfXyzRhwDiffuseTex1);
    call(device, d3d8::slot::SetPixelShader, DWORD{0});
    s->begun = true;
    return S_OK;
}

HRESULT sprite_end(Sprite* s)
{
    if (!s->begun)
        return kInvalidCall;
    call(s->device, d3d8::slot::ApplyStateBlock, s->state);
    call(s->device, d3d8::slot::DeleteStateBlock, s->state);
    s->state = 0;
    s->begun = false;
    return S_OK;
}

// A quad an Ashita addon draws over one of its ImGui windows that the Layout tab or the HUD Manager scales (fancychat's
// chat lines and their background, gdifonts' textures over its window): drawn scaled with that window, from its top-left
// corner, as the window's own drawing is (layout.hpp). Its scale reached only the ImGui part (reported in game 2026-11-10:
// fancychat's chat at 0.75 kept its text and background full size, its tab bar shrank).
void window_scale(float (&x)[4], float (&y)[4])
{
    constexpr std::string_view kAddon = "addon ";
    const std::string_view who = calling(); // "addon fancychat" while its Lua runs (its present event)
    if (!who.starts_with(kAddon))
        return;
    float origin_x = 0, origin_y = 0, scale = 1;
    const float mid_x = (x[0] + x[1] + x[2] + x[3]) * 0.25f, mid_y = (y[0] + y[1] + y[2] + y[3]) * 0.25f;
    if (!layout::drawing_scale("ashita/" + std::string(who.substr(kAddon.size())), mid_x, mid_y, origin_x, origin_y, scale))
        return;
    for (int i = 0; i < 4; ++i) {
        x[i] = origin_x + (x[i] - origin_x) * scale;
        y[i] = origin_y + (y[i] - origin_y) * scale;
    }
}

// The texture's rectangle (all of it without one) as a quad, each corner through `corner` (from the
// rectangle's own pixels to the screen's).
template <class Corner>
HRESULT sprite_quad(Sprite* s, void* texture, const RECT* rect, DWORD colour, Corner corner)
{
    if (texture == nullptr)
        return kInvalidCall;
    d3d8::SurfaceDesc desc{};
    if (FAILED(call(texture, kTextureGetLevelDesc, UINT{0}, &desc)) || desc.width == 0 || desc.height == 0)
        return kInvalidCall;
    const RECT r = rect != nullptr ? *rect : RECT{0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height)};
    const float w = static_cast<float>(r.right - r.left), h = static_cast<float>(r.bottom - r.top);
    struct Vertex {
        float x, y, z, rhw;
        DWORD colour;
        float u, v;
    } quad[4];
    const float cx[4] = {0, w, 0, w}, cy[4] = {0, 0, h, h};
    float x[4] = {}, y[4] = {};
    for (int i = 0; i < 4; ++i)
        corner(cx[i], cy[i], x[i], y[i]);
    window_scale(x, y);
    for (int i = 0; i < 4; ++i) {
        // D3D8's pixel centres sit on whole coordinates: -0.5 maps texels 1:1
        quad[i] = {x[i] - 0.5f, y[i] - 0.5f, 0.0f, 1.0f, colour, (static_cast<float>(r.left) + cx[i]) / static_cast<float>(desc.width),
                   (static_cast<float>(r.top) + cy[i]) / static_cast<float>(desc.height)};
    }
    const bool implicit = !s->begun; // D3DX draws a sprite outside Begin and End in a Begin and End of its own
    if (implicit && FAILED(sprite_begin(s)))
        return kInvalidCall;
    call(s->device, d3d8::slot::SetTexture, DWORD{0}, texture);
    call(s->device, d3d8::slot::DrawPrimitiveUP, d3d8::kTriangleStrip, 2u, static_cast<const void*>(quad), static_cast<UINT>(sizeof(Vertex)));
    call(s->device, d3d8::slot::SetTexture, DWORD{0}, static_cast<void*>(nullptr));
    if (implicit)
        sprite_end(s);
    return S_OK;
}

HRESULT __stdcall sprite_query(Sprite* s, const IID*, void** out)
{
    if (out == nullptr)
        return E_POINTER;
    InterlockedIncrement(&s->references);
    *out = s;
    return S_OK;
}

ULONG __stdcall sprite_add_ref(Sprite* s)
{
    return static_cast<ULONG>(InterlockedIncrement(&s->references));
}

ULONG __stdcall sprite_release(Sprite* s)
{
    const LONG left = InterlockedDecrement(&s->references);
    if (left == 0) {
        if (s->begun)
            sprite_end(s);
        if (s->device != nullptr)
            call<ULONG>(s->device, d3d8::slot::Release);
        delete s;
    }
    return static_cast<ULONG>(left < 0 ? 0 : left);
}

HRESULT __stdcall sprite_get_device(Sprite* s, void** out)
{
    if (out == nullptr)
        return kInvalidCall;
    call<ULONG>(s->device, kAddRef);
    *out = s->device;
    return S_OK;
}

HRESULT __stdcall sprite_begin_method(Sprite* s)
{
    return sprite_begin(s);
}

HRESULT __stdcall sprite_draw(Sprite* s, void* texture, const RECT* rect, const Vector2* scaling, const Vector2* centre, float rotation,
                              const Vector2* translation, DWORD colour)
{
    const float sx = scaling != nullptr ? scaling->x : 1.0f, sy = scaling != nullptr ? scaling->y : 1.0f;
    const float ox = centre != nullptr ? centre->x : 0.0f, oy = centre != nullptr ? centre->y : 0.0f;
    const float tx = translation != nullptr ? translation->x : 0.0f, ty = translation != nullptr ? translation->y : 0.0f;
    const float c = std::cos(rotation), sn = std::sin(rotation);
    // scaled, turned about the centre, then moved (D3DX 8's order)
    return sprite_quad(s, texture, rect, colour, [&](float x, float y, float& out_x, float& out_y) {
        const float px = x * sx - ox, py = y * sy - oy;
        out_x = px * c - py * sn + ox + tx;
        out_y = px * sn + py * c + oy + ty;
    });
}

HRESULT __stdcall sprite_draw_transform(Sprite* s, void* texture, const RECT* rect, const float* m, DWORD colour)
{
    // a D3DXMATRIX: row vectors, (x, y, 0, 1) times it
    return sprite_quad(s, texture, rect, colour, [&](float x, float y, float& out_x, float& out_y) {
        const float w = m != nullptr ? x * m[3] + y * m[7] + m[15] : 1.0f;
        out_x = m != nullptr ? (x * m[0] + y * m[4] + m[12]) / (w != 0.0f ? w : 1.0f) : x;
        out_y = m != nullptr ? (x * m[1] + y * m[5] + m[13]) / (w != 0.0f ? w : 1.0f) : y;
    });
}

HRESULT __stdcall sprite_end_method(Sprite* s)
{
    return sprite_end(s);
}

HRESULT __stdcall sprite_lost(Sprite*)
{
    return S_OK;
}

const void* const kSpriteVtable[] = {
    reinterpret_cast<const void*>(&sprite_query),        reinterpret_cast<const void*>(&sprite_add_ref),
    reinterpret_cast<const void*>(&sprite_release),      reinterpret_cast<const void*>(&sprite_get_device),
    reinterpret_cast<const void*>(&sprite_begin_method), reinterpret_cast<const void*>(&sprite_draw),
    reinterpret_cast<const void*>(&sprite_draw_transform), reinterpret_cast<const void*>(&sprite_end_method),
    reinterpret_cast<const void*>(&sprite_lost),         reinterpret_cast<const void*>(&sprite_lost),
};

} // namespace

void* load_texture(void* device, const std::wstring& path, const void* data, UINT size, DWORD colour_key, UINT& width, UINT& height)
{
    if (device == nullptr)
        return nullptr;
    ImageInfo info{};
    void* texture = nullptr;
    const HRESULT hr = path.empty() ? from_memory(device, data, size, kDefault, kDefault, 1, 0, d3d8::kFormatA8R8G8B8,
                                                  d3d8::kPoolManaged, colour_key, &info, &texture)
                                    : from_file(device, path, kDefault, kDefault, 1, 0, d3d8::kFormatA8R8G8B8, d3d8::kPoolManaged,
                                                colour_key, &info, &texture);
    if (FAILED(hr) || texture == nullptr)
        return nullptr;
    width = info.width;
    height = info.height;
    return texture;
}

} // namespace vanacore::ashita

using vanacore::ashita::ImageInfo;

// The exports, by D3DX 8's names (undecorated: LuaJIT looks them up by name)
#pragma comment(linker, "/EXPORT:D3DXCreateTextureFromFileA=_D3DXCreateTextureFromFileA@12")
#pragma comment(linker, "/EXPORT:D3DXCreateTextureFromFileW=_D3DXCreateTextureFromFileW@12")
#pragma comment(linker, "/EXPORT:D3DXCreateTextureFromFileExA=_D3DXCreateTextureFromFileExA@56")
#pragma comment(linker, "/EXPORT:D3DXCreateTextureFromFileExW=_D3DXCreateTextureFromFileExW@56")
#pragma comment(linker, "/EXPORT:D3DXCreateTextureFromFileInMemory=_D3DXCreateTextureFromFileInMemory@16")
#pragma comment(linker, "/EXPORT:D3DXCreateTextureFromFileInMemoryEx=_D3DXCreateTextureFromFileInMemoryEx@60")
#pragma comment(linker, "/EXPORT:D3DXGetImageInfoFromFileA=_D3DXGetImageInfoFromFileA@8")
#pragma comment(linker, "/EXPORT:D3DXGetImageInfoFromFileW=_D3DXGetImageInfoFromFileW@8")
#pragma comment(linker, "/EXPORT:D3DXGetImageInfoFromFileInMemory=_D3DXGetImageInfoFromFileInMemory@12")
#pragma comment(linker, "/EXPORT:D3DXCreateSprite=_D3DXCreateSprite@8")

extern "C" {

HRESULT __stdcall D3DXCreateTextureFromFileExW(void* device, const wchar_t* file, UINT width, UINT height, UINT levels, DWORD usage,
                                               DWORD format, DWORD pool, DWORD, DWORD, DWORD colour_key, ImageInfo* info, void*,
                                               void** texture)
{
    if (file == nullptr)
        return vanacore::ashita::kInvalidCall;
    return vanacore::ashita::from_file(device, file, width, height, levels, usage, format, pool, colour_key, info, texture);
}

HRESULT __stdcall D3DXCreateTextureFromFileExA(void* device, const char* file, UINT width, UINT height, UINT levels, DWORD usage,
                                               DWORD format, DWORD pool, DWORD filter, DWORD mip_filter, DWORD colour_key,
                                               ImageInfo* info, void* palette, void** texture)
{
    if (file == nullptr)
        return vanacore::ashita::kInvalidCall;
    const std::wstring path = vanacore::ashita::wide(file);
    return D3DXCreateTextureFromFileExW(device, path.c_str(), width, height, levels, usage, format, pool, filter, mip_filter, colour_key,
                                        info, palette, texture);
}

HRESULT __stdcall D3DXCreateTextureFromFileW(void* device, const wchar_t* file, void** texture)
{
    using vanacore::ashita::kDefault;
    return D3DXCreateTextureFromFileExW(device, file, kDefault, kDefault, kDefault, 0, 0, vanacore::d3d8::kPoolManaged, kDefault, kDefault, 0,
                                        nullptr, nullptr, texture);
}

HRESULT __stdcall D3DXCreateTextureFromFileA(void* device, const char* file, void** texture)
{
    using vanacore::ashita::kDefault;
    return D3DXCreateTextureFromFileExA(device, file, kDefault, kDefault, kDefault, 0, 0, vanacore::d3d8::kPoolManaged, kDefault, kDefault, 0,
                                        nullptr, nullptr, texture);
}

HRESULT __stdcall D3DXCreateTextureFromFileInMemoryEx(void* device, const void* data, UINT size, UINT width, UINT height, UINT levels,
                                                      DWORD usage, DWORD format, DWORD pool, DWORD, DWORD, DWORD colour_key,
                                                      ImageInfo* info, void*, void** texture)
{
    return vanacore::ashita::from_memory(device, data, size, width, height, levels, usage, format, pool, colour_key, info, texture);
}

HRESULT __stdcall D3DXCreateTextureFromFileInMemory(void* device, const void* data, UINT size, void** texture)
{
    using vanacore::ashita::kDefault;
    return D3DXCreateTextureFromFileInMemoryEx(device, data, size, kDefault, kDefault, kDefault, 0, 0, vanacore::d3d8::kPoolManaged, kDefault,
                                               kDefault, 0, nullptr, nullptr, texture);
}

HRESULT __stdcall D3DXGetImageInfoFromFileW(const wchar_t* file, ImageInfo* info)
{
    vanacore::ashita::Image image;
    if (file == nullptr || info == nullptr || !vanacore::ashita::decode(file, nullptr, 0, 0, 0, true, image))
        return vanacore::ashita::kInvalidData;
    vanacore::ashita::fill_info(image, info);
    return S_OK;
}

HRESULT __stdcall D3DXGetImageInfoFromFileA(const char* file, ImageInfo* info)
{
    if (file == nullptr)
        return vanacore::ashita::kInvalidCall;
    const std::wstring path = vanacore::ashita::wide(file);
    return D3DXGetImageInfoFromFileW(path.c_str(), info);
}

HRESULT __stdcall D3DXGetImageInfoFromFileInMemory(const void* data, UINT size, ImageInfo* info)
{
    vanacore::ashita::Image image;
    if (data == nullptr || info == nullptr ||
        !vanacore::ashita::decode({}, data, vanacore::ashita::memory_size(data, size), 0, 0, true, image))
        return vanacore::ashita::kInvalidData;
    vanacore::ashita::fill_info(image, info);
    return S_OK;
}

HRESULT __stdcall D3DXCreateSprite(void* device, void** sprite)
{
    if (device == nullptr || sprite == nullptr)
        return vanacore::ashita::kInvalidCall;
    auto* s = new vanacore::ashita::Sprite();
    s->vtable = vanacore::ashita::kSpriteVtable;
    s->device = device;
    vanacore::d3d8::call<ULONG>(device, vanacore::ashita::kAddRef);
    *sprite = s;
    return S_OK;
}

} // extern "C"
