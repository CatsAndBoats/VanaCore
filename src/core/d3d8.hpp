#pragma once

// The slice of the Direct3D 8 ABI that VanaCore uses. The Windows SDK no longer ships d3d8.h, so
// vtable slots, structs and constants are declared here with the DirectX 8 SDK's layout and values.

#include <windows.h>

namespace vanacore::d3d8 {

// Calls vtable slot `index` of a COM object. On x86, COM methods are __stdcall with `this` first,
// and every argument VanaCore passes is 4 bytes.
template <typename Result = HRESULT, typename... Args>
Result call(void* object, int index, Args... args)
{
    static_assert(((sizeof(Args) == 4) && ...), "D3D8 methods called here take 4-byte arguments");
    using Method = Result(__stdcall*)(void*, Args...);
    return reinterpret_cast<Method>((*static_cast<void***>(object))[index])(object, args...);
}

namespace slot {
inline constexpr int Release = 2;
// IDirect3D8
inline constexpr int CreateDevice = 15;
// IDirect3DDevice8
inline constexpr int TestCooperativeLevel = 3;
inline constexpr int Reset = 14;
inline constexpr int Present = 15;
inline constexpr int GetBackBuffer = 16;
inline constexpr int CreateTexture = 20;
inline constexpr int CreateImageSurface = 27;
inline constexpr int CopyRects = 28;
inline constexpr int GetFrontBuffer = 30;
inline constexpr int GetRenderTarget = 32;
inline constexpr int BeginScene = 34;
inline constexpr int EndScene = 35;
inline constexpr int Clear = 36;
inline constexpr int SetViewport = 40;
inline constexpr int GetViewport = 41;
inline constexpr int SetRenderState = 50;
inline constexpr int GetRenderState = 51;
inline constexpr int ApplyStateBlock = 54;
inline constexpr int DeleteStateBlock = 56;
inline constexpr int CreateStateBlock = 57;
inline constexpr int GetTexture = 60; // AddRefs what it returns
inline constexpr int SetTexture = 61;
inline constexpr int GetTextureStageState = 62;
inline constexpr int SetTextureStageState = 63;
inline constexpr int DrawPrimitive = 70;
inline constexpr int DrawIndexedPrimitive = 71;
inline constexpr int DrawPrimitiveUP = 72;
inline constexpr int DrawIndexedPrimitiveUP = 73;
inline constexpr int SetVertexShader = 76;
inline constexpr int GetVertexShader = 77;
inline constexpr int SetPixelShader = 88;
inline constexpr int GetPixelShader = 89;
// IDirect3DSurface8
inline constexpr int SurfaceGetDesc = 8;
inline constexpr int SurfaceLockRect = 9;
inline constexpr int SurfaceUnlockRect = 10;
// IDirect3DTexture8
inline constexpr int TextureGetLevelDesc = 14;
inline constexpr int TextureGetSurfaceLevel = 15; // AddRefs what it returns
inline constexpr int TextureLockRect = 16;
inline constexpr int TextureUnlockRect = 17;
} // namespace slot

struct PresentParameters { // D3DPRESENT_PARAMETERS
    UINT back_buffer_width;
    UINT back_buffer_height;
    DWORD back_buffer_format;
    UINT back_buffer_count;
    DWORD multisample_type;
    DWORD swap_effect;
    HWND device_window;
    BOOL windowed;
    BOOL enable_auto_depth_stencil;
    DWORD auto_depth_stencil_format;
    DWORD flags;
    UINT refresh_rate;
    UINT presentation_interval;
};

struct SurfaceDesc { // D3DSURFACE_DESC
    DWORD format;
    DWORD type;
    DWORD usage;
    DWORD pool;
    UINT size;
    DWORD multisample_type;
    UINT width;
    UINT height;
};

struct Viewport { // D3DVIEWPORT8
    DWORD x;
    DWORD y;
    DWORD width;
    DWORD height;
    float min_z;
    float max_z;
};

struct LockedRect { // D3DLOCKED_RECT
    INT pitch;
    void* bits;
};

inline constexpr UINT kSdkVersion = 220; // D3D_SDK_VERSION
inline constexpr DWORD kFormatA8R8G8B8 = 21;
inline constexpr DWORD kFormatX8R8G8B8 = 22;
inline constexpr DWORD kFormatR5G6B5 = 23;
inline constexpr DWORD kFormatX1R5G5B5 = 24;
inline constexpr DWORD kFormatA1R5G5B5 = 25;
inline constexpr DWORD kFormatA4R4G4B4 = 26;
inline constexpr DWORD kPoolDefault = 0;
inline constexpr DWORD kPoolManaged = 1;
inline constexpr DWORD kUsageRenderTarget = 1; // D3DUSAGE_RENDERTARGET
inline constexpr DWORD kBackBufferMono = 0;
inline constexpr DWORD kStateBlockAll = 1;
inline constexpr DWORD kTriangleList = 4;
inline constexpr DWORD kTriangleStrip = 5;
inline constexpr DWORD kFormatIndex16 = 101;
inline constexpr DWORD kFvfXyzRhwTex1 = 0x004 | 0x100; // D3DFVF_XYZRHW | D3DFVF_TEX1
inline constexpr DWORD kFvfXyzRhwDiffuse = 0x004 | 0x040; // D3DFVF_XYZRHW | D3DFVF_DIFFUSE
inline constexpr DWORD kFvfXyzRhwDiffuseTex1 = 0x004 | 0x040 | 0x100;

namespace rs { // D3DRENDERSTATETYPE
inline constexpr DWORD ZEnable = 7;
inline constexpr DWORD FillMode = 8;
inline constexpr DWORD ZWriteEnable = 14;
inline constexpr DWORD AlphaTestEnable = 15;
inline constexpr DWORD SrcBlend = 19;
inline constexpr DWORD AlphaRef = 24;
inline constexpr DWORD AlphaFunc = 25;
inline constexpr DWORD DestBlend = 20;
inline constexpr DWORD CullMode = 22;
inline constexpr DWORD AlphaBlendEnable = 27;
inline constexpr DWORD FogEnable = 28;
inline constexpr DWORD SpecularEnable = 29;
inline constexpr DWORD StencilEnable = 52;
inline constexpr DWORD TextureFactor = 60;
inline constexpr DWORD Wrap0 = 128;
inline constexpr DWORD Clipping = 136;
inline constexpr DWORD Lighting = 137;
inline constexpr DWORD VertexBlend = 151;
inline constexpr DWORD ColorWriteEnable = 168;
inline constexpr DWORD BlendOp = 171;
} // namespace rs

namespace tss { // D3DTEXTURESTAGESTATETYPE (D3D8 keeps sampler states here too)
inline constexpr DWORD ColorOp = 1;
inline constexpr DWORD ColorArg1 = 2;
inline constexpr DWORD ColorArg2 = 3;
inline constexpr DWORD AlphaOp = 4;
inline constexpr DWORD AlphaArg1 = 5;
inline constexpr DWORD AlphaArg2 = 6;
inline constexpr DWORD TexCoordIndex = 11;
inline constexpr DWORD AddressU = 13;
inline constexpr DWORD AddressV = 14;
inline constexpr DWORD MagFilter = 16;
inline constexpr DWORD MinFilter = 17;
inline constexpr DWORD MipFilter = 18;
inline constexpr DWORD TextureTransformFlags = 24;
} // namespace tss

inline constexpr DWORD kFillSolid = 3;
inline constexpr DWORD kCullNone = 1;
inline constexpr DWORD kBlendSrcAlpha = 5;
inline constexpr DWORD kBlendInvSrcAlpha = 6;
inline constexpr DWORD kBlendOpAdd = 1;
inline constexpr DWORD kTopDisable = 1;
inline constexpr DWORD kTopSelectArg1 = 2;
inline constexpr DWORD kTopModulate = 4;
inline constexpr DWORD kTopModulate2x = 5;
inline constexpr DWORD kTopModulate4x = 6;
inline constexpr DWORD kTaDiffuse = 0;
inline constexpr DWORD kTaTexture = 2;
inline constexpr DWORD kAddressWrap = 1;
inline constexpr DWORD kAddressClamp = 3;
inline constexpr DWORD kFilterNone = 0;
inline constexpr DWORD kFilterPoint = 1;
inline constexpr DWORD kFilterLinear = 2;

} // namespace vanacore::d3d8
