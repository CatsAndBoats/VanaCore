#include "image.hpp"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>

#include "d3d8.hpp"

namespace vanacore::image {
namespace {

using d3d8::call;
namespace slot = d3d8::slot;

// COM for WIC on the calling thread, undone afterwards if this set it up.
class ComScope {
public:
    ComScope() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope()
    {
        if (SUCCEEDED(result_))
            CoUninitialize();
    }

private:
    HRESULT result_;
};

bool decode_frame(IWICImagingFactory* factory, IWICBitmapDecoder* decoder, Pixels& out)
{
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool ok = false;
    if (SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeCustom))) {
        UINT width = 0, height = 0;
        converter->GetSize(&width, &height);
        if (width > 0 && height > 0 && width <= 8192 && height <= 8192) {
            out.width = static_cast<int>(width);
            out.height = static_cast<int>(height);
            out.argb.assign(static_cast<size_t>(width) * height, 0);
            ok = SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(out.argb.size() * 4),
                                                 reinterpret_cast<BYTE*>(out.argb.data())));
        }
    }
    if (converter != nullptr)
        converter->Release();
    if (frame != nullptr)
        frame->Release();
    return ok;
}

// Power-of-two sizes: the oldest D3D8 drivers need them.
UINT power_of_two(int size)
{
    UINT out = 1;
    while (out < static_cast<UINT>(size))
        out <<= 1;
    return out;
}

} // namespace

bool decode(const void* data, std::size_t size, Pixels& out)
{
    if (data == nullptr || size == 0)
        return false;
    ComScope com;
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
              SUCCEEDED(factory->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(data)), static_cast<DWORD>(size))) &&
              SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
              decode_frame(factory, decoder, out);
    if (decoder != nullptr)
        decoder->Release();
    if (stream != nullptr)
        stream->Release();
    if (factory != nullptr)
        factory->Release();
    return ok;
}

bool decode_file(const std::wstring& path, Pixels& out)
{
    ComScope com;
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
              SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                           &decoder)) &&
              decode_frame(factory, decoder, out);
    if (decoder != nullptr)
        decoder->Release();
    if (factory != nullptr)
        factory->Release();
    return ok;
}

bool decode_resource(HMODULE module, const char* name, Pixels& out)
{
    const char* const types[] = {MAKEINTRESOURCEA(10) /* RT_RCDATA */, "PNG", MAKEINTRESOURCEA(2) /* RT_BITMAP */};
    for (const char* type : types) {
        const HRSRC found = FindResourceA(module, name, type);
        if (found == nullptr)
            continue;
        const HGLOBAL loaded = LoadResource(module, found);
        const void* data = loaded != nullptr ? LockResource(loaded) : nullptr;
        const DWORD size = SizeofResource(module, found);
        if (data == nullptr)
            continue;
        if (type == MAKEINTRESOURCEA(2)) {
            // A bitmap resource has no file header; WIC wants one.
            std::vector<BYTE> file(sizeof(BITMAPFILEHEADER) + size);
            auto* header = reinterpret_cast<BITMAPFILEHEADER*>(file.data());
            const auto* info = static_cast<const BITMAPINFOHEADER*>(data);
            header->bfType = 0x4D42;
            header->bfSize = static_cast<DWORD>(file.size());
            const DWORD colours = info->biClrUsed != 0 ? info->biClrUsed : info->biBitCount <= 8 ? (1u << info->biBitCount) : 0;
            header->bfOffBits = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + info->biSize + colours * 4);
            std::copy_n(static_cast<const BYTE*>(data), size, file.data() + sizeof(BITMAPFILEHEADER));
            if (decode(file.data(), file.size(), out))
                return true;
            continue;
        }
        if (decode(data, size, out))
            return true;
    }
    return false;
}

void Texture::set(Pixels pixels)
{
    pixels_ = std::move(pixels);
    stale_ = true;
}

bool Texture::upload(void* device)
{
    if (!stale_)
        return texture_ != nullptr;
    if (pixels_.argb.empty())
        return false;
    const UINT width = power_of_two(pixels_.width);
    const UINT height = power_of_two(pixels_.height);
    if (texture_ == nullptr || width != texture_width_ || height != texture_height_) {
        release();
        if (FAILED(call(device, slot::CreateTexture, width, height, 1u, DWORD{0}, d3d8::kFormatA8R8G8B8, d3d8::kPoolManaged,
                        &texture_))) {
            texture_ = nullptr;
            return false;
        }
        texture_width_ = width;
        texture_height_ = height;
    }
    d3d8::LockedRect locked{};
    if (FAILED(call(texture_, slot::TextureLockRect, 0u, &locked, static_cast<const RECT*>(nullptr), DWORD{0})))
        return false;
    // The image in the top-left corner; its last row and column repeated into the padding, so
    // linear filtering at the edges doesn't pull in black.
    for (UINT y = 0; y < texture_height_; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(static_cast<BYTE*>(locked.bits) + y * locked.pitch);
        const UINT sy = std::min<UINT>(y, static_cast<UINT>(pixels_.height - 1));
        for (UINT x = 0; x < texture_width_; ++x) {
            const UINT sx = std::min<UINT>(x, static_cast<UINT>(pixels_.width - 1));
            row[x] = pixels_.argb[static_cast<size_t>(sy) * pixels_.width + sx];
        }
    }
    call(texture_, slot::TextureUnlockRect, 0u);
    stale_ = false;
    return true;
}

void Texture::draw(void* device, float x, float y, float width, float height, std::uint32_t argb, float repeat_x, float repeat_y)
{
    if (width <= 0.0f || height <= 0.0f || (argb >> 24) == 0 || !upload(device))
        return;
    struct Vertex {
        float x, y, z, rhw;
        DWORD colour;
        float u, v;
    };
    const float image_u = static_cast<float>(pixels_.width) / static_cast<float>(texture_width_);
    const float image_v = static_cast<float>(pixels_.height) / static_cast<float>(texture_height_);
    repeat_x = repeat_x > 0.0f ? std::min(repeat_x, 64.0f) : 1.0f;
    repeat_y = repeat_y > 0.0f ? std::min(repeat_y, 64.0f) : 1.0f;
    const float tile_w = width / repeat_x;
    const float tile_h = height / repeat_y;

    call(device, slot::SetTexture, DWORD{0}, texture_);
    call(device, slot::SetTextureStageState, DWORD{0}, d3d8::tss::ColorOp, d3d8::kTopModulate);
    call(device, slot::SetTextureStageState, DWORD{0}, d3d8::tss::ColorArg2, d3d8::kTaDiffuse);
    call(device, slot::SetTextureStageState, DWORD{0}, d3d8::tss::AlphaOp, d3d8::kTopModulate);
    call(device, slot::SetTextureStageState, DWORD{0}, d3d8::tss::AlphaArg2, d3d8::kTaDiffuse);
    call(device, slot::SetTextureStageState, DWORD{0}, d3d8::tss::MagFilter, d3d8::kFilterLinear);
    call(device, slot::SetTextureStageState, DWORD{0}, d3d8::tss::MinFilter, d3d8::kFilterLinear);
    call(device, slot::SetVertexShader, d3d8::kFvfXyzRhwDiffuseTex1);
    // A quad per tile, the last in each direction cut to what's left.
    for (float ty = 0.0f; ty < repeat_y - 0.001f; ty += 1.0f) {
        const float part_y = std::min(1.0f, repeat_y - ty);
        for (float tx = 0.0f; tx < repeat_x - 0.001f; tx += 1.0f) {
            const float part_x = std::min(1.0f, repeat_x - tx);
            const float left = x + tx * tile_w - 0.5f;
            const float top = y + ty * tile_h - 0.5f;
            const float right = left + tile_w * part_x;
            const float bottom = top + tile_h * part_y;
            const float u = image_u * part_x;
            const float v = image_v * part_y;
            const Vertex quad[4] = {{left, top, 0, 1, argb, 0, 0},
                                    {right, top, 0, 1, argb, u, 0},
                                    {left, bottom, 0, 1, argb, 0, v},
                                    {right, bottom, 0, 1, argb, u, v}};
            call(device, slot::DrawPrimitiveUP, d3d8::kTriangleStrip, 2u, static_cast<const void*>(quad),
                 static_cast<UINT>(sizeof(Vertex)));
        }
    }
    // Back to what text::begin set for sprites.
    call(device, slot::SetTextureStageState, DWORD{0}, d3d8::tss::ColorOp, d3d8::kTopSelectArg1);
    call(device, slot::SetTextureStageState, DWORD{0}, d3d8::tss::AlphaOp, d3d8::kTopSelectArg1);
    call(device, slot::SetTextureStageState, DWORD{0}, d3d8::tss::MagFilter, d3d8::kFilterPoint);
    call(device, slot::SetTextureStageState, DWORD{0}, d3d8::tss::MinFilter, d3d8::kFilterPoint);
    call(device, slot::SetVertexShader, d3d8::kFvfXyzRhwTex1);
}

void Texture::release()
{
    if (texture_ != nullptr)
        call<ULONG>(texture_, slot::Release);
    texture_ = nullptr;
    texture_width_ = 0;
    texture_height_ = 0;
    stale_ = true;
}

} // namespace vanacore::image
