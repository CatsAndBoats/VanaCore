// [graphics] renderer=d3d9 (see d3d9.hpp): an IDirect3D8 whose device and resources hand each call to
// Direct3D 9.
//
// Every object the game gets is a wrapper: its vtable is Direct3D 8's, and right after it come the
// Direct3D 9 object it stands for and a tag, so a pointer the game hands back (a texture to SetTexture)
// is unwrapped without knowing its class. A Direct3D 9 object has one wrapper at a time (the device's
// table), so GetTexture, GetRenderTarget and the rest give back the pointer the game was given.
//
// A wrapper's count is its object's (AddRef and Release go to it), and the wrapper lives as long as the
// object does, not as long as the game's references: a texture's surface or a back buffer lives as long
// as its texture or device, and the game keeps using ones it has released, as Direct3D 8 let it. Each
// wrapper is deleted when its object goes, by a Tracker kept in the object's private data, which
// Direct3D 9 releases then.
//
// Where Direct3D 9 differs from Direct3D 8:
//   - vertex shaders: Direct3D 8 gives a shader's inputs as a declaration of registers beside it;
//     Direct3D 9 wants a vertex declaration, and a dcl in the shader's code for each input. An FVF code
//     is a shader handle in Direct3D 8: even handles are FVF codes, the shaders' are odd
//   - the base vertex moved from SetIndices to DrawIndexedPrimitive, the sampler settings left the
//     texture stage states, ZBIAS became DEPTHBIAS
//   - CopyRects became StretchRect, UpdateSurface and GetRenderTargetData, by where the surfaces live;
//     GetFrontBuffer became GetFrontBufferData; image surfaces are offscreen plain ones in system memory
//   - state blocks are objects, not DWORD tokens; the Direct3D 8 state Direct3D 9 doesn't keep (the
//     vertex and pixel shader handles, the base vertex) travels beside each one
//   - a windowed Present doesn't wait for the display in Direct3D 8; in Direct3D 9 it does unless
//     asked not to: [graphics] vsync decides, windowed or not
//   - Direct3D 8 locked render targets Direct3D 9 won't (a back buffer): such a lock goes through a
//     copy in system memory
#include "d3d9.hpp"

#include <windows.h>

#include <d3d9.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <tuple>
#include <utility>
#include <string>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "log.hpp"

namespace vanacore::d3d9 {
namespace {

// ---- Direct3D 8's own types and values (the Windows SDK has only Direct3D 9's; the rest are shared) ----

struct D3DPRESENT_PARAMETERS8 {
    UINT BackBufferWidth;
    UINT BackBufferHeight;
    D3DFORMAT BackBufferFormat;
    UINT BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    DWORD SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed;
    BOOL EnableAutoDepthStencil;
    D3DFORMAT AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz;
    UINT FullScreen_PresentationInterval;
};

// D3DCAPS9 begins with the same fields
struct D3DCAPS8 {
    D3DDEVTYPE DeviceType;
    UINT AdapterOrdinal;
    DWORD Caps;
    DWORD Caps2;
    DWORD Caps3;
    DWORD PresentationIntervals;
    DWORD CursorCaps;
    DWORD DevCaps;
    DWORD PrimitiveMiscCaps;
    DWORD RasterCaps;
    DWORD ZCmpCaps;
    DWORD SrcBlendCaps;
    DWORD DestBlendCaps;
    DWORD AlphaCmpCaps;
    DWORD ShadeCaps;
    DWORD TextureCaps;
    DWORD TextureFilterCaps;
    DWORD CubeTextureFilterCaps;
    DWORD VolumeTextureFilterCaps;
    DWORD TextureAddressCaps;
    DWORD VolumeTextureAddressCaps;
    DWORD LineCaps;
    DWORD MaxTextureWidth;
    DWORD MaxTextureHeight;
    DWORD MaxVolumeExtent;
    DWORD MaxTextureRepeat;
    DWORD MaxTextureAspectRatio;
    DWORD MaxAnisotropy;
    float MaxVertexW;
    float GuardBandLeft;
    float GuardBandTop;
    float GuardBandRight;
    float GuardBandBottom;
    float ExtentsAdjust;
    DWORD StencilCaps;
    DWORD FVFCaps;
    DWORD TextureOpCaps;
    DWORD MaxTextureBlendStages;
    DWORD MaxSimultaneousTextures;
    DWORD VertexProcessingCaps;
    DWORD MaxActiveLights;
    DWORD MaxUserClipPlanes;
    DWORD MaxVertexBlendMatrices;
    DWORD MaxVertexBlendMatrixIndex;
    float MaxPointSize;
    DWORD MaxPrimitiveCount;
    DWORD MaxVertexIndex;
    DWORD MaxStreams;
    DWORD MaxStreamStride;
    DWORD VertexShaderVersion;
    DWORD MaxVertexShaderConst;
    DWORD PixelShaderVersion;
    float MaxPixelShaderValue;
};
static_assert(sizeof(D3DCAPS8) == 212 && offsetof(D3DCAPS9, PixelShader1xMaxValue) == 208);

struct D3DADAPTER_IDENTIFIER8 {
    char Driver[MAX_DEVICE_IDENTIFIER_STRING];
    char Description[MAX_DEVICE_IDENTIFIER_STRING];
    LARGE_INTEGER DriverVersion;
    DWORD VendorId;
    DWORD DeviceId;
    DWORD SubSysId;
    DWORD Revision;
    GUID DeviceIdentifier;
    DWORD WHQLLevel;
};

struct D3DSURFACE_DESC8 {
    D3DFORMAT Format;
    D3DRESOURCETYPE Type;
    DWORD Usage;
    D3DPOOL Pool;
    UINT Size;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    UINT Width;
    UINT Height;
};

struct D3DVOLUME_DESC8 {
    D3DFORMAT Format;
    D3DRESOURCETYPE Type;
    DWORD Usage;
    D3DPOOL Pool;
    UINT Size;
    UINT Width;
    UINT Height;
    UINT Depth;
};

constexpr DWORD kSwapEffectCopyVsync = 4;
constexpr DWORD kCaps2CanRenderWindowed = 0x00080000;
constexpr DWORD kRasterCapsZBias = 0x00004000;
constexpr HRESULT kMoreData = MAKE_D3DHRESULT(2151);
// render states Direct3D 9 dropped
constexpr DWORD kRsLinePattern = 10;
constexpr DWORD kRsZVisible = 30;
constexpr DWORD kRsEdgeAntialias = 40;
constexpr DWORD kRsZBias = 47;
constexpr DWORD kRsSoftwareVertexProcessing = 153;
constexpr DWORD kRsPatchSegments = 164;
// texture stage states that are sampler states in Direct3D 9
constexpr DWORD kTssAddressU = 13;
constexpr DWORD kTssMaxAnisotropy = 21;
constexpr DWORD kTssAddressW = 25;

constexpr GUID kIIDDirect3D8 = {0x1dd9e8da, 0x1c77, 0x4d40, {0xb0, 0xcf, 0x98, 0xfe, 0xfd, 0xff, 0x95, 0x12}};
constexpr GUID kIIDDevice8 = {0x7385e5df, 0x8fe8, 0x41d5, {0x86, 0xb6, 0xd7, 0xb4, 0x85, 0x47, 0xb6, 0xcf}};
constexpr GUID kIIDResource8 = {0x1b36bb7b, 0x09b7, 0x410a, {0xb4, 0x45, 0x7d, 0x14, 0x30, 0xd7, 0xb3, 0x3f}};
constexpr GUID kIIDBaseTexture8 = {0xb4211cfa, 0x51b9, 0x4a9f, {0xab, 0x78, 0xdb, 0x99, 0xb2, 0xbb, 0x67, 0x8e}};
constexpr GUID kIIDTexture8 = {0xe4cdd575, 0x2866, 0x4f01, {0xb1, 0x2e, 0x7e, 0xec, 0xe1, 0xec, 0x93, 0x58}};
constexpr GUID kIIDCubeTexture8 = {0x3ee5b968, 0x2aca, 0x4c34, {0x8b, 0xb5, 0x7e, 0x0c, 0x3d, 0x19, 0xb7, 0x50}};
constexpr GUID kIIDVolumeTexture8 = {0x4b8aaafa, 0x140f, 0x42ba, {0x91, 0x31, 0x59, 0x7e, 0xaf, 0xaa, 0x2e, 0xad}};
constexpr GUID kIIDVertexBuffer8 = {0x8aeeeac7, 0x05f9, 0x44d4, {0xb5, 0x91, 0x00, 0x0b, 0x0d, 0xf1, 0xcb, 0x95}};
constexpr GUID kIIDIndexBuffer8 = {0x0e689c9a, 0x053d, 0x44a0, {0x9d, 0x92, 0xdb, 0x0e, 0x3d, 0x75, 0x0f, 0x86}};
constexpr GUID kIIDSurface8 = {0xb96eebca, 0xb326, 0x4ea5, {0x88, 0x2f, 0x2f, 0xf5, 0xba, 0xe0, 0x21, 0xdd}};
constexpr GUID kIIDVolume8 = {0xbd7349f5, 0x14f1, 0x42e4, {0x9c, 0x79, 0x97, 0x23, 0x80, 0xdb, 0x40, 0xc0}};
constexpr GUID kIIDSwapChain8 = {0x928c088b, 0x76b9, 0x4c6b, {0xa5, 0x36, 0xa5, 0x90, 0x85, 0x38, 0x76, 0xcd}};

// ---- Direct3D 8's interfaces, in its vtable order ----

struct IDirect3DDevice8;
struct IDirect3DSurface8;
struct IDirect3DVolume8;
struct IDirect3DBaseTexture8;
struct IDirect3DVertexBuffer8;
struct IDirect3DIndexBuffer8;

struct IDirect3D8 : IUnknown {
    virtual HRESULT __stdcall RegisterSoftwareDevice(void* initialize) = 0;
    virtual UINT __stdcall GetAdapterCount() = 0;
    virtual HRESULT __stdcall GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER8* identifier) = 0;
    virtual UINT __stdcall GetAdapterModeCount(UINT adapter) = 0;
    virtual HRESULT __stdcall EnumAdapterModes(UINT adapter, UINT mode, D3DDISPLAYMODE* out) = 0;
    virtual HRESULT __stdcall GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE* out) = 0;
    virtual HRESULT __stdcall CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT display, D3DFORMAT back_buffer, BOOL windowed) = 0;
    virtual HRESULT __stdcall CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_format, DWORD usage,
                                                D3DRESOURCETYPE resource, D3DFORMAT format) = 0;
    virtual HRESULT __stdcall CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT format, BOOL windowed,
                                                         D3DMULTISAMPLE_TYPE multisample) = 0;
    virtual HRESULT __stdcall CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_format, D3DFORMAT target,
                                                     D3DFORMAT depth) = 0;
    virtual HRESULT __stdcall GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS8* caps) = 0;
    virtual HMONITOR __stdcall GetAdapterMonitor(UINT adapter) = 0;
    virtual HRESULT __stdcall CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behavior, D3DPRESENT_PARAMETERS8* params,
                                           IDirect3DDevice8** out) = 0;
};

struct IDirect3DSwapChain8 : IUnknown {
    virtual HRESULT __stdcall Present(const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty) = 0;
    virtual HRESULT __stdcall GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface8** out) = 0;
};

struct IDirect3DResource8 : IUnknown {
    virtual HRESULT __stdcall GetDevice(IDirect3DDevice8** out) = 0;
    virtual HRESULT __stdcall SetPrivateData(REFGUID guid, const void* data, DWORD size, DWORD flags) = 0;
    virtual HRESULT __stdcall GetPrivateData(REFGUID guid, void* data, DWORD* size) = 0;
    virtual HRESULT __stdcall FreePrivateData(REFGUID guid) = 0;
    virtual DWORD __stdcall SetPriority(DWORD priority) = 0;
    virtual DWORD __stdcall GetPriority() = 0;
    virtual void __stdcall PreLoad() = 0;
    virtual D3DRESOURCETYPE __stdcall GetType() = 0;
};

struct IDirect3DBaseTexture8 : IDirect3DResource8 {
    virtual DWORD __stdcall SetLOD(DWORD lod) = 0;
    virtual DWORD __stdcall GetLOD() = 0;
    virtual DWORD __stdcall GetLevelCount() = 0;
};

struct IDirect3DTexture8 : IDirect3DBaseTexture8 {
    virtual HRESULT __stdcall GetLevelDesc(UINT level, D3DSURFACE_DESC8* desc) = 0;
    virtual HRESULT __stdcall GetSurfaceLevel(UINT level, IDirect3DSurface8** out) = 0;
    virtual HRESULT __stdcall LockRect(UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) = 0;
    virtual HRESULT __stdcall UnlockRect(UINT level) = 0;
    virtual HRESULT __stdcall AddDirtyRect(const RECT* rect) = 0;
};

struct IDirect3DVolumeTexture8 : IDirect3DBaseTexture8 {
    virtual HRESULT __stdcall GetLevelDesc(UINT level, D3DVOLUME_DESC8* desc) = 0;
    virtual HRESULT __stdcall GetVolumeLevel(UINT level, IDirect3DVolume8** out) = 0;
    virtual HRESULT __stdcall LockBox(UINT level, D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags) = 0;
    virtual HRESULT __stdcall UnlockBox(UINT level) = 0;
    virtual HRESULT __stdcall AddDirtyBox(const D3DBOX* box) = 0;
};

struct IDirect3DCubeTexture8 : IDirect3DBaseTexture8 {
    virtual HRESULT __stdcall GetLevelDesc(UINT level, D3DSURFACE_DESC8* desc) = 0;
    virtual HRESULT __stdcall GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface8** out) = 0;
    virtual HRESULT __stdcall LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) = 0;
    virtual HRESULT __stdcall UnlockRect(D3DCUBEMAP_FACES face, UINT level) = 0;
    virtual HRESULT __stdcall AddDirtyRect(D3DCUBEMAP_FACES face, const RECT* rect) = 0;
};

struct IDirect3DVertexBuffer8 : IDirect3DResource8 {
    virtual HRESULT __stdcall Lock(UINT offset, UINT size, BYTE** data, DWORD flags) = 0;
    virtual HRESULT __stdcall Unlock() = 0;
    virtual HRESULT __stdcall GetDesc(D3DVERTEXBUFFER_DESC* desc) = 0;
};

struct IDirect3DIndexBuffer8 : IDirect3DResource8 {
    virtual HRESULT __stdcall Lock(UINT offset, UINT size, BYTE** data, DWORD flags) = 0;
    virtual HRESULT __stdcall Unlock() = 0;
    virtual HRESULT __stdcall GetDesc(D3DINDEXBUFFER_DESC* desc) = 0;
};

struct IDirect3DSurface8 : IUnknown {
    virtual HRESULT __stdcall GetDevice(IDirect3DDevice8** out) = 0;
    virtual HRESULT __stdcall SetPrivateData(REFGUID guid, const void* data, DWORD size, DWORD flags) = 0;
    virtual HRESULT __stdcall GetPrivateData(REFGUID guid, void* data, DWORD* size) = 0;
    virtual HRESULT __stdcall FreePrivateData(REFGUID guid) = 0;
    virtual HRESULT __stdcall GetContainer(REFIID riid, void** out) = 0;
    virtual HRESULT __stdcall GetDesc(D3DSURFACE_DESC8* desc) = 0;
    virtual HRESULT __stdcall LockRect(D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) = 0;
    virtual HRESULT __stdcall UnlockRect() = 0;
};

struct IDirect3DVolume8 : IUnknown {
    virtual HRESULT __stdcall GetDevice(IDirect3DDevice8** out) = 0;
    virtual HRESULT __stdcall SetPrivateData(REFGUID guid, const void* data, DWORD size, DWORD flags) = 0;
    virtual HRESULT __stdcall GetPrivateData(REFGUID guid, void* data, DWORD* size) = 0;
    virtual HRESULT __stdcall FreePrivateData(REFGUID guid) = 0;
    virtual HRESULT __stdcall GetContainer(REFIID riid, void** out) = 0;
    virtual HRESULT __stdcall GetDesc(D3DVOLUME_DESC8* desc) = 0;
    virtual HRESULT __stdcall LockBox(D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags) = 0;
    virtual HRESULT __stdcall UnlockBox() = 0;
};

struct IDirect3DDevice8 : IUnknown {
    virtual HRESULT __stdcall TestCooperativeLevel() = 0;
    virtual UINT __stdcall GetAvailableTextureMem() = 0;
    virtual HRESULT __stdcall ResourceManagerDiscardBytes(DWORD bytes) = 0;
    virtual HRESULT __stdcall GetDirect3D(IDirect3D8** out) = 0;
    virtual HRESULT __stdcall GetDeviceCaps(D3DCAPS8* caps) = 0;
    virtual HRESULT __stdcall GetDisplayMode(D3DDISPLAYMODE* mode) = 0;
    virtual HRESULT __stdcall GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* params) = 0;
    virtual HRESULT __stdcall SetCursorProperties(UINT x, UINT y, IDirect3DSurface8* bitmap) = 0;
    virtual void __stdcall SetCursorPosition(int x, int y, DWORD flags) = 0;
    virtual BOOL __stdcall ShowCursor(BOOL show) = 0;
    virtual HRESULT __stdcall CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS8* params, IDirect3DSwapChain8** out) = 0;
    virtual HRESULT __stdcall Reset(D3DPRESENT_PARAMETERS8* params) = 0;
    virtual HRESULT __stdcall Present(const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty) = 0;
    virtual HRESULT __stdcall GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface8** out) = 0;
    virtual HRESULT __stdcall GetRasterStatus(D3DRASTER_STATUS* status) = 0;
    virtual void __stdcall SetGammaRamp(DWORD flags, const D3DGAMMARAMP* ramp) = 0;
    virtual void __stdcall GetGammaRamp(D3DGAMMARAMP* ramp) = 0;
    virtual HRESULT __stdcall CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                            IDirect3DTexture8** out) = 0;
    virtual HRESULT __stdcall CreateVolumeTexture(UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format,
                                                  D3DPOOL pool, IDirect3DVolumeTexture8** out) = 0;
    virtual HRESULT __stdcall CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                                IDirect3DCubeTexture8** out) = 0;
    virtual HRESULT __stdcall CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer8** out) = 0;
    virtual HRESULT __stdcall CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer8** out) = 0;
    virtual HRESULT __stdcall CreateRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample,
                                                 BOOL lockable, IDirect3DSurface8** out) = 0;
    virtual HRESULT __stdcall CreateDepthStencilSurface(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample,
                                                        IDirect3DSurface8** out) = 0;
    virtual HRESULT __stdcall CreateImageSurface(UINT width, UINT height, D3DFORMAT format, IDirect3DSurface8** out) = 0;
    virtual HRESULT __stdcall CopyRects(IDirect3DSurface8* source, const RECT* rects, UINT count, IDirect3DSurface8* dest,
                                        const POINT* points) = 0;
    virtual HRESULT __stdcall UpdateTexture(IDirect3DBaseTexture8* source, IDirect3DBaseTexture8* dest) = 0;
    virtual HRESULT __stdcall GetFrontBuffer(IDirect3DSurface8* dest) = 0;
    virtual HRESULT __stdcall SetRenderTarget(IDirect3DSurface8* target, IDirect3DSurface8* depth) = 0;
    virtual HRESULT __stdcall GetRenderTarget(IDirect3DSurface8** out) = 0;
    virtual HRESULT __stdcall GetDepthStencilSurface(IDirect3DSurface8** out) = 0;
    virtual HRESULT __stdcall BeginScene() = 0;
    virtual HRESULT __stdcall EndScene() = 0;
    virtual HRESULT __stdcall Clear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) = 0;
    virtual HRESULT __stdcall SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) = 0;
    virtual HRESULT __stdcall GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX* matrix) = 0;
    virtual HRESULT __stdcall MultiplyTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) = 0;
    virtual HRESULT __stdcall SetViewport(const D3DVIEWPORT9* viewport) = 0;
    virtual HRESULT __stdcall GetViewport(D3DVIEWPORT9* viewport) = 0;
    virtual HRESULT __stdcall SetMaterial(const D3DMATERIAL9* material) = 0;
    virtual HRESULT __stdcall GetMaterial(D3DMATERIAL9* material) = 0;
    virtual HRESULT __stdcall SetLight(DWORD index, const D3DLIGHT9* light) = 0;
    virtual HRESULT __stdcall GetLight(DWORD index, D3DLIGHT9* light) = 0;
    virtual HRESULT __stdcall LightEnable(DWORD index, BOOL enable) = 0;
    virtual HRESULT __stdcall GetLightEnable(DWORD index, BOOL* enable) = 0;
    virtual HRESULT __stdcall SetClipPlane(DWORD index, const float* plane) = 0;
    virtual HRESULT __stdcall GetClipPlane(DWORD index, float* plane) = 0;
    virtual HRESULT __stdcall SetRenderState(D3DRENDERSTATETYPE state, DWORD value) = 0;
    virtual HRESULT __stdcall GetRenderState(D3DRENDERSTATETYPE state, DWORD* value) = 0;
    virtual HRESULT __stdcall BeginStateBlock() = 0;
    virtual HRESULT __stdcall EndStateBlock(DWORD* token) = 0;
    virtual HRESULT __stdcall ApplyStateBlock(DWORD token) = 0;
    virtual HRESULT __stdcall CaptureStateBlock(DWORD token) = 0;
    virtual HRESULT __stdcall DeleteStateBlock(DWORD token) = 0;
    virtual HRESULT __stdcall CreateStateBlock(D3DSTATEBLOCKTYPE type, DWORD* token) = 0;
    virtual HRESULT __stdcall SetClipStatus(const D3DCLIPSTATUS9* status) = 0;
    virtual HRESULT __stdcall GetClipStatus(D3DCLIPSTATUS9* status) = 0;
    virtual HRESULT __stdcall GetTexture(DWORD stage, IDirect3DBaseTexture8** out) = 0;
    virtual HRESULT __stdcall SetTexture(DWORD stage, IDirect3DBaseTexture8* texture) = 0;
    virtual HRESULT __stdcall GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD* value) = 0;
    virtual HRESULT __stdcall SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) = 0;
    virtual HRESULT __stdcall ValidateDevice(DWORD* passes) = 0;
    virtual HRESULT __stdcall GetInfo(DWORD id, void* info, DWORD size) = 0;
    virtual HRESULT __stdcall SetPaletteEntries(UINT palette, const PALETTEENTRY* entries) = 0;
    virtual HRESULT __stdcall GetPaletteEntries(UINT palette, PALETTEENTRY* entries) = 0;
    virtual HRESULT __stdcall SetCurrentTexturePalette(UINT palette) = 0;
    virtual HRESULT __stdcall GetCurrentTexturePalette(UINT* palette) = 0;
    virtual HRESULT __stdcall DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count) = 0;
    virtual HRESULT __stdcall DrawIndexedPrimitive(D3DPRIMITIVETYPE type, UINT min_index, UINT vertices, UINT start, UINT count) = 0;
    virtual HRESULT __stdcall DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) = 0;
    virtual HRESULT __stdcall DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT min_index, UINT vertices, UINT count,
                                                     const void* indices, D3DFORMAT format, const void* data, UINT stride) = 0;
    virtual HRESULT __stdcall ProcessVertices(UINT source, UINT dest, UINT count, IDirect3DVertexBuffer8* buffer, DWORD flags) = 0;
    virtual HRESULT __stdcall CreateVertexShader(const DWORD* declaration, const DWORD* function, DWORD* handle, DWORD usage) = 0;
    virtual HRESULT __stdcall SetVertexShader(DWORD handle) = 0;
    virtual HRESULT __stdcall GetVertexShader(DWORD* handle) = 0;
    virtual HRESULT __stdcall DeleteVertexShader(DWORD handle) = 0;
    virtual HRESULT __stdcall SetVertexShaderConstant(DWORD reg, const void* data, DWORD count) = 0;
    virtual HRESULT __stdcall GetVertexShaderConstant(DWORD reg, void* data, DWORD count) = 0;
    virtual HRESULT __stdcall GetVertexShaderDeclaration(DWORD handle, void* data, DWORD* size) = 0;
    virtual HRESULT __stdcall GetVertexShaderFunction(DWORD handle, void* data, DWORD* size) = 0;
    virtual HRESULT __stdcall SetStreamSource(UINT stream, IDirect3DVertexBuffer8* buffer, UINT stride) = 0;
    virtual HRESULT __stdcall GetStreamSource(UINT stream, IDirect3DVertexBuffer8** out, UINT* stride) = 0;
    virtual HRESULT __stdcall SetIndices(IDirect3DIndexBuffer8* buffer, UINT base_vertex) = 0;
    virtual HRESULT __stdcall GetIndices(IDirect3DIndexBuffer8** out, UINT* base_vertex) = 0;
    virtual HRESULT __stdcall CreatePixelShader(const DWORD* function, DWORD* handle) = 0;
    virtual HRESULT __stdcall SetPixelShader(DWORD handle) = 0;
    virtual HRESULT __stdcall GetPixelShader(DWORD* handle) = 0;
    virtual HRESULT __stdcall DeletePixelShader(DWORD handle) = 0;
    virtual HRESULT __stdcall SetPixelShaderConstant(DWORD reg, const void* data, DWORD count) = 0;
    virtual HRESULT __stdcall GetPixelShaderConstant(DWORD reg, void* data, DWORD count) = 0;
    virtual HRESULT __stdcall GetPixelShaderFunction(DWORD handle, void* data, DWORD* size) = 0;
    virtual HRESULT __stdcall DrawRectPatch(UINT handle, const float* segments, const D3DRECTPATCH_INFO* info) = 0;
    virtual HRESULT __stdcall DrawTriPatch(UINT handle, const float* segments, const D3DTRIPATCH_INFO* info) = 0;
    virtual HRESULT __stdcall DeletePatch(UINT handle) = 0;
};

// ---- what's shared ----

bool g_vsync = false;
std::string g_adapter;
std::wstring g_library;

DWORD float_bits(float value)
{
    DWORD bits;
    std::memcpy(&bits, &value, 4);
    return bits;
}

DWORD token_of(const void* pointer)
{
    return static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(pointer));
}

// Bytes per pixel, or per 4x4 block of the compressed formats (block set).
UINT format_bytes(D3DFORMAT format, bool& block)
{
    block = false;
    switch (format) {
    case D3DFMT_DXT1:
        block = true;
        return 8;
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:
        block = true;
        return 16;
    case D3DFMT_R8G8B8:
        return 3;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_A8R3G3B2:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_A8P8:
    case D3DFMT_A8L8:
    case D3DFMT_V8U8:
    case D3DFMT_L6V5U5:
    case D3DFMT_D16_LOCKABLE:
    case D3DFMT_D16:
    case D3DFMT_D15S1:
    case D3DFMT_UYVY:
    case D3DFMT_YUY2:
    case D3DFMT_L16:
        return 2;
    case D3DFMT_R3G3B2:
    case D3DFMT_A8:
    case D3DFMT_P8:
    case D3DFMT_L8:
    case D3DFMT_A4L4:
        return 1;
    default:
        return 4;
    }
}

// Direct3D 8's Size of a surface or volume
UINT level_size(D3DFORMAT format, UINT width, UINT height, UINT depth)
{
    bool block = false;
    const UINT bytes = format_bytes(format, block);
    return block ? (width + 3) / 4 * ((height + 3) / 4) * bytes * depth : width * height * depth * bytes;
}

void to_desc8(const D3DSURFACE_DESC& in, D3DSURFACE_DESC8* out)
{
    out->Format = in.Format;
    out->Type = in.Type;
    out->Usage = in.Usage;
    out->Pool = in.Pool;
    out->Size = level_size(in.Format, in.Width, in.Height, 1);
    out->MultiSampleType = in.MultiSampleType;
    out->Width = in.Width;
    out->Height = in.Height;
}

void to_desc8(const D3DVOLUME_DESC& in, D3DVOLUME_DESC8* out)
{
    out->Format = in.Format;
    out->Type = in.Type;
    out->Usage = in.Usage;
    out->Pool = in.Pool;
    out->Size = level_size(in.Format, in.Width, in.Height, in.Depth);
    out->Width = in.Width;
    out->Height = in.Height;
    out->Depth = in.Depth;
}

void to_caps8(const D3DCAPS9& in, D3DCAPS8* out)
{
    std::memcpy(out, &in, sizeof(D3DCAPS8));
    out->Caps2 |= kCaps2CanRenderWindowed; // every Direct3D 9 device can
    if ((in.RasterCaps & D3DPRASTERCAPS_DEPTHBIAS) != 0)
        out->RasterCaps |= kRasterCapsZBias;
    out->VertexShaderVersion = std::min<DWORD>(in.VertexShaderVersion, D3DVS_VERSION(1, 1));
    out->PixelShaderVersion = std::min<DWORD>(in.PixelShaderVersion, D3DPS_VERSION(1, 4));
    if (out->MaxVertexShaderConst > 0)
        out->MaxVertexShaderConst -= 1; // the last is VanaCore's: see to_vs_1_1
}

// The game's presentation, as Direct3D 9 takes it. [graphics] vsync decides whether Present waits for
// the display: windowed, Direct3D 8 didn't (unless COPY_VSYNC), where Direct3D 9 would.
D3DPRESENT_PARAMETERS to_params9(const D3DPRESENT_PARAMETERS8& in)
{
    D3DPRESENT_PARAMETERS out{};
    out.BackBufferWidth = in.BackBufferWidth;
    out.BackBufferHeight = in.BackBufferHeight;
    out.BackBufferFormat = in.BackBufferFormat;
    out.BackBufferCount = in.BackBufferCount;
    out.MultiSampleType = in.MultiSampleType;
    out.SwapEffect = in.SwapEffect == kSwapEffectCopyVsync ? D3DSWAPEFFECT_COPY : static_cast<D3DSWAPEFFECT>(in.SwapEffect);
    out.hDeviceWindow = in.hDeviceWindow;
    out.Windowed = in.Windowed;
    out.EnableAutoDepthStencil = in.EnableAutoDepthStencil;
    out.AutoDepthStencilFormat = in.AutoDepthStencilFormat;
    out.Flags = in.Flags;
    out.FullScreen_RefreshRateInHz = in.Windowed ? 0 : in.FullScreen_RefreshRateInHz;
    out.PresentationInterval = g_vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    return out;
}

// What Direct3D 9 made of them, back into the game's (a windowed back buffer's size from the window)
void from_params9(const D3DPRESENT_PARAMETERS& in, D3DPRESENT_PARAMETERS8& out)
{
    out.BackBufferWidth = in.BackBufferWidth;
    out.BackBufferHeight = in.BackBufferHeight;
    out.BackBufferFormat = in.BackBufferFormat;
    out.BackBufferCount = in.BackBufferCount;
}

// A window for a device made with none (VanaCore.GfxTest's offscreen device): Direct3D 9 needs one.
HWND hidden_window()
{
    static const HWND window = CreateWindowExW(0, L"STATIC", L"VanaCore Direct3D 9", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr,
                                               nullptr, nullptr);
    return window;
}

// ---- vertex shaders ----

// Direct3D 8's input registers, as Direct3D 9's usages: the fixed-function ones (D3DVSDE_*), which a
// shader's registers take too
struct Input {
    BYTE usage;
    BYTE index;
};
constexpr Input kInputs[] = {
    {D3DDECLUSAGE_POSITION, 0}, {D3DDECLUSAGE_BLENDWEIGHT, 0}, {D3DDECLUSAGE_BLENDINDICES, 0}, {D3DDECLUSAGE_NORMAL, 0},
    {D3DDECLUSAGE_PSIZE, 0},    {D3DDECLUSAGE_COLOR, 0},       {D3DDECLUSAGE_COLOR, 1},        {D3DDECLUSAGE_TEXCOORD, 0},
    {D3DDECLUSAGE_TEXCOORD, 1}, {D3DDECLUSAGE_TEXCOORD, 2},    {D3DDECLUSAGE_TEXCOORD, 3},     {D3DDECLUSAGE_TEXCOORD, 4},
    {D3DDECLUSAGE_TEXCOORD, 5}, {D3DDECLUSAGE_TEXCOORD, 6},    {D3DDECLUSAGE_TEXCOORD, 7},     {D3DDECLUSAGE_POSITION, 1},
    {D3DDECLUSAGE_NORMAL, 1},
};
constexpr UINT kInputCount = static_cast<UINT>(std::size(kInputs));
// D3DVSDT_* are D3DDECLTYPE_* with these sizes
constexpr WORD kTypeBytes[] = {4, 8, 12, 16, 4, 4, 4, 8};

// Constants a declaration loads with its shader (D3DVSD_CONST)
struct Constants {
    DWORD reg;
    std::vector<float> values;
};

struct VertexShader8 {
    std::vector<DWORD> declaration; // the game's tokens, for GetVertexShaderDeclaration
    std::vector<DWORD> function;    // empty: the fixed function, with this declaration's layout
    std::vector<Constants> constants;
    IDirect3DVertexDeclaration9* declaration9 = nullptr;
    IDirect3DVertexShader9* shader9 = nullptr;
};

struct PixelShader8 {
    std::vector<DWORD> function;
    IDirect3DPixelShader9* shader9 = nullptr; // nullptr: Direct3D 9 refused it (drawn without)
};

// How many tokens a shader's code takes, its end token included (comments and def's constants skipped,
// so a value in them isn't taken for the end).
size_t code_length(const DWORD* code)
{
    size_t i = 1; // the version
    for (;;) {
        const DWORD token = code[i];
        if (token == 0x0000FFFF)
            return i + 1;
        if ((token & 0xFFFF) == D3DSIO_COMMENT)
            i += 1 + ((token >> 16) & 0x7FFF);
        else if ((token & 0xFFFF) == D3DSIO_DEF)
            i += 6; // def c#, x, y, z, w
        else
            i += 1;
    }
}

DWORD param_type(DWORD param)
{
    return ((param >> 28) & 7) | ((param >> 8) & 0x18);
}

// A parameter token pointed at another register (its swizzle, write mask and modifiers kept; relative
// addressing dropped)
DWORD retarget(DWORD param, DWORD type, DWORD reg)
{
    return (param & ~0x70003FFFu) | ((type & 7) << 28) | ((type & 0x18) << 8) | reg;
}

// Which component a source's swizzle puts in `slot` (0 x ... 3 w)
DWORD swizzled(DWORD param, int slot)
{
    return (param >> (D3DVS_SWIZZLE_SHIFT + 2 * slot)) & 3;
}

// A source reading `component` (0-3) in every slot
DWORD replicate(DWORD param, DWORD component)
{
    const DWORD c = component & 3;
    return (param & ~D3DVS_SWIZZLE_MASK) | ((c | c << 2 | c << 4 | c << 6) << D3DVS_SWIZZLE_SHIFT);
}

// A shader's code, instruction by instruction: where each starts, how many tokens it takes, and whether
// it's an operation (an opcode and its parameters) rather than a comment or a def.
struct Instruction {
    size_t at;
    size_t size;
    bool operation;
};

std::vector<Instruction> instructions(const DWORD* code, size_t length)
{
    std::vector<Instruction> out;
    for (size_t i = 1; i < length && code[i] != 0x0000FFFF;) {
        const DWORD opcode = code[i] & 0xFFFF;
        size_t size = 1;
        bool operation = false;
        if (opcode == D3DSIO_COMMENT) {
            size += (code[i] >> 16) & 0x7FFF;
        } else if (opcode == D3DSIO_DEF) {
            size = 6; // def c#, x, y, z, w
        } else {
            operation = true;
            while (i + size < length && (code[i + size] & 0x80000000) != 0)
                ++size;
        }
        out.push_back({i, size, operation});
        i += size;
    }
    return out;
}

// Direct3D 8's vs.1.x code as Direct3D 9's vs_1_1: the same instructions, but where Direct3D 9 checks
// what Direct3D 8 let through (Windows' Direct3D 9 measured 2026-09-29: FFXI's zone shader was refused
// for its sub and its oFog.x):
//   - a dcl for each input the declaration gives
//   - an input the code reads that the declaration doesn't give is (0, 0, 0, 1) in Direct3D 8 (and
//     VanaCore's Direct3D 12), and refused by Direct3D 9 (FFXI's zone shader reads v15, which its
//     declaration hasn't): read from a spare temporary register set from `spare`, a constant the game
//     isn't told of
//   - no sub: add, with the second source negated
//   - a scalar instruction's source (rcp, rsq, exp, log, expp, logp) names its component (a replicate
//     swizzle): the one Direct3D 8 read, which its swizzle puts in w
//   - oFog and oPts are written whole (Direct3D 8's assembler wrote .x), from the sources' x
//   - one input register and one constant register per instruction: another is moved into a spare
//     temporary register first
std::vector<DWORD> to_vs_1_1(const DWORD* function, size_t length, UINT declared, DWORD spare, UINT& missing_out)
{
    const std::vector<Instruction> list = instructions(function, length);
    UINT read = 0, temps = 0;
    for (const Instruction& in : list) {
        for (size_t k = 1; in.operation && k < in.size; ++k) {
            const DWORD param = function[in.at + k], type = param_type(param), reg = param & 0x7FF;
            if (type == D3DSPR_INPUT && reg < 16)
                read |= 1u << reg;
            else if (type == D3DSPR_TEMP && reg < 12)
                temps |= 1u << reg;
        }
    }
    const UINT missing = read & ~declared;
    missing_out = missing;
    UINT free = ~temps & 0xFFF;
    const auto take = [&]() -> int { // a temporary register the code doesn't use, from r11 down; -1 for none
        for (int temp = 11; temp >= 0; --temp) {
            if ((free & (1u << temp)) != 0) {
                free &= ~(1u << temp);
                return temp;
            }
        }
        return -1;
    };
    const auto mov = [](std::vector<DWORD>& out, int temp, DWORD source) {
        out.insert(out.end(), {static_cast<DWORD>(D3DSIO_MOV),
                               0x80000000u | (D3DSPR_TEMP << 28) | D3DSP_WRITEMASK_ALL | static_cast<DWORD>(temp), source});
    };

    std::vector<DWORD> code;
    code.reserve(2 * length + 64);
    code.push_back(D3DVS_VERSION(1, 1));
    for (UINT reg = 0; reg < 16; ++reg) {
        if ((declared & (1u << reg)) == 0)
            continue;
        code.push_back(D3DSIO_DCL);
        code.push_back(0x80000000u | kInputs[reg].usage | (static_cast<DWORD>(kInputs[reg].index) << 16));
        code.push_back(0x80000000u | (D3DSPR_INPUT << 28) | D3DSP_WRITEMASK_ALL | reg);
    }
    DWORD stand_in[16] = {}; // per missing input: the register read instead (type << 16 | number)
    if (missing != 0) {
        const DWORD constant = 0x80000000u | (D3DSPR_CONST << 28) | spare;
        code.insert(code.end(), {static_cast<DWORD>(D3DSIO_DEF), constant | D3DSP_WRITEMASK_ALL, 0, 0, 0, float_bits(1.0f)});
        for (UINT reg = 0; reg < 16; ++reg) {
            if ((missing & (1u << reg)) == 0)
                continue;
            const int temp = take();
            if (temp >= 0) {
                mov(code, temp, constant | D3DVS_NOSWIZZLE);
                stand_in[reg] = (D3DSPR_TEMP << 16) | static_cast<DWORD>(temp);
            } else {
                stand_in[reg] = (D3DSPR_CONST << 16) | spare;
            }
        }
    }
    const int scratch[2] = {take(), take()}; // for an instruction's second input or constant register

    for (const Instruction& in : list) {
        if (!in.operation) {
            code.insert(code.end(), function + in.at, function + in.at + in.size);
            continue;
        }
        const DWORD token = function[in.at];
        DWORD opcode = token & 0xFFFF;
        std::vector<DWORD> params(function + in.at + 1, function + in.at + in.size);
        for (size_t k = 1; k < params.size(); ++k) {
            const DWORD reg = params[k] & 0x7FF;
            if (param_type(params[k]) == D3DSPR_INPUT && reg < 16 && (missing & (1u << reg)) != 0)
                params[k] = retarget(params[k], stand_in[reg] >> 16, stand_in[reg] & 0xFFFF);
        }
        if (opcode == D3DSIO_SUB && params.size() == 3) {
            opcode = D3DSIO_ADD;
            params[2] ^= D3DSPSM_NEG; // a vertex shader's sources are negated or not: flipped
        }
        const bool scalar = opcode == D3DSIO_RCP || opcode == D3DSIO_RSQ || opcode == D3DSIO_EXP || opcode == D3DSIO_LOG ||
                            opcode == D3DSIO_EXPP || opcode == D3DSIO_LOGP;
        if (scalar && params.size() >= 2)
            params[1] = replicate(params[1], swizzled(params[1], 3));
        if (!params.empty() && param_type(params[0]) == D3DSPR_RASTOUT && (params[0] & 0x7FF) != 0) { // oFog, oPts
            params[0] |= D3DSP_WRITEMASK_ALL;
            for (size_t k = 1; k < params.size(); ++k)
                params[k] = replicate(params[k], swizzled(params[k], 0));
        }
        int used = 0;
        for (const DWORD type : {static_cast<DWORD>(D3DSPR_INPUT), static_cast<DWORD>(D3DSPR_CONST)}) {
            bool seen = false;
            DWORD first = 0; // the register number, its type's high bits and relative addressing
            for (size_t k = 1; k < params.size(); ++k) {
                if (param_type(params[k]) != type)
                    continue;
                const DWORD key = params[k] & 0x3FFF;
                if (!seen) {
                    seen = true, first = key;
                    continue;
                }
                if (key == first || used == 2 || scratch[used] < 0)
                    continue; // the same register again; or nothing spare, left for Direct3D 9 to judge
                const int temp = scratch[used++];
                mov(code, temp, (params[k] & ~(D3DVS_SWIZZLE_MASK | D3DSP_SRCMOD_MASK)) | D3DVS_NOSWIZZLE);
                params[k] = retarget(params[k], D3DSPR_TEMP, static_cast<DWORD>(temp));
            }
        }
        code.push_back((token & ~0xFFFFu) | opcode);
        code.insert(code.end(), params.begin(), params.end());
    }
    code.push_back(0x0000FFFF);
    return code;
}

// ---- the wrappers ----

class Direct3D8;
class Device8;

constexpr DWORD kTag = 0x39384356; // 'VC89'

// What every wrapper has right after its vtable
struct Header {
    void* vtable;
    IUnknown* object;
    DWORD tag;
};

// The Direct3D 9 object behind a pointer the game handed back; nullptr for nullptr (or not ours).
template <class Object9>
Object9* unwrap(const void* wrapper)
{
    const auto* header = static_cast<const Header*>(wrapper);
    if (header == nullptr || header->tag != kTag)
        return nullptr;
    return static_cast<Object9*>(header->object);
}

// A state block: Direct3D 9's, and the Direct3D 8 state it doesn't keep.
struct Shadow {
    DWORD vs = 0;
    DWORD ps = 0;
    UINT base = 0;
};

struct StateBlock8 {
    IDirect3DStateBlock9* block = nullptr; // nullptr once dropped for a Reset
    D3DSTATEBLOCKTYPE type{};              // 0: recorded
    bool has_vs = false;
    bool has_ps = false;
    bool has_base = false;
    Shadow values;
};

class Device8 final : public IDirect3DDevice8 {
public:
    Device8(Direct3D8* d3d, IDirect3DDevice9* device, DWORD behavior, bool depth);

    HRESULT __stdcall QueryInterface(REFIID riid, void** out) override;
    ULONG __stdcall AddRef() override;
    ULONG __stdcall Release() override;

    HRESULT __stdcall TestCooperativeLevel() override;
    UINT __stdcall GetAvailableTextureMem() override;
    HRESULT __stdcall ResourceManagerDiscardBytes(DWORD bytes) override;
    HRESULT __stdcall GetDirect3D(IDirect3D8** out) override;
    HRESULT __stdcall GetDeviceCaps(D3DCAPS8* caps) override;
    HRESULT __stdcall GetDisplayMode(D3DDISPLAYMODE* mode) override;
    HRESULT __stdcall GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* params) override;
    HRESULT __stdcall SetCursorProperties(UINT x, UINT y, IDirect3DSurface8* bitmap) override;
    void __stdcall SetCursorPosition(int x, int y, DWORD flags) override;
    BOOL __stdcall ShowCursor(BOOL show) override;
    HRESULT __stdcall CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS8* params, IDirect3DSwapChain8** out) override;
    HRESULT __stdcall Reset(D3DPRESENT_PARAMETERS8* params) override;
    HRESULT __stdcall Present(const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty) override;
    HRESULT __stdcall GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface8** out) override;
    HRESULT __stdcall GetRasterStatus(D3DRASTER_STATUS* status) override;
    void __stdcall SetGammaRamp(DWORD flags, const D3DGAMMARAMP* ramp) override;
    void __stdcall GetGammaRamp(D3DGAMMARAMP* ramp) override;
    HRESULT __stdcall CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                    IDirect3DTexture8** out) override;
    HRESULT __stdcall CreateVolumeTexture(UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format,
                                          D3DPOOL pool, IDirect3DVolumeTexture8** out) override;
    HRESULT __stdcall CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                        IDirect3DCubeTexture8** out) override;
    HRESULT __stdcall CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer8** out) override;
    HRESULT __stdcall CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer8** out) override;
    HRESULT __stdcall CreateRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample, BOOL lockable,
                                         IDirect3DSurface8** out) override;
    HRESULT __stdcall CreateDepthStencilSurface(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample,
                                                IDirect3DSurface8** out) override;
    HRESULT __stdcall CreateImageSurface(UINT width, UINT height, D3DFORMAT format, IDirect3DSurface8** out) override;
    HRESULT __stdcall CopyRects(IDirect3DSurface8* source, const RECT* rects, UINT count, IDirect3DSurface8* dest,
                                const POINT* points) override;
    HRESULT __stdcall UpdateTexture(IDirect3DBaseTexture8* source, IDirect3DBaseTexture8* dest) override;
    HRESULT __stdcall GetFrontBuffer(IDirect3DSurface8* dest) override;
    HRESULT __stdcall SetRenderTarget(IDirect3DSurface8* target, IDirect3DSurface8* depth) override;
    HRESULT __stdcall GetRenderTarget(IDirect3DSurface8** out) override;
    HRESULT __stdcall GetDepthStencilSurface(IDirect3DSurface8** out) override;
    HRESULT __stdcall BeginScene() override;
    HRESULT __stdcall EndScene() override;
    HRESULT __stdcall Clear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) override;
    HRESULT __stdcall SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) override;
    HRESULT __stdcall GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX* matrix) override;
    HRESULT __stdcall MultiplyTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) override;
    HRESULT __stdcall SetViewport(const D3DVIEWPORT9* viewport) override;
    HRESULT __stdcall GetViewport(D3DVIEWPORT9* viewport) override;
    HRESULT __stdcall SetMaterial(const D3DMATERIAL9* material) override;
    HRESULT __stdcall GetMaterial(D3DMATERIAL9* material) override;
    HRESULT __stdcall SetLight(DWORD index, const D3DLIGHT9* light) override;
    HRESULT __stdcall GetLight(DWORD index, D3DLIGHT9* light) override;
    HRESULT __stdcall LightEnable(DWORD index, BOOL enable) override;
    HRESULT __stdcall GetLightEnable(DWORD index, BOOL* enable) override;
    HRESULT __stdcall SetClipPlane(DWORD index, const float* plane) override;
    HRESULT __stdcall GetClipPlane(DWORD index, float* plane) override;
    HRESULT __stdcall SetRenderState(D3DRENDERSTATETYPE state, DWORD value) override;
    HRESULT __stdcall GetRenderState(D3DRENDERSTATETYPE state, DWORD* value) override;
    HRESULT __stdcall BeginStateBlock() override;
    HRESULT __stdcall EndStateBlock(DWORD* token) override;
    HRESULT __stdcall ApplyStateBlock(DWORD token) override;
    HRESULT __stdcall CaptureStateBlock(DWORD token) override;
    HRESULT __stdcall DeleteStateBlock(DWORD token) override;
    HRESULT __stdcall CreateStateBlock(D3DSTATEBLOCKTYPE type, DWORD* token) override;
    HRESULT __stdcall SetClipStatus(const D3DCLIPSTATUS9* status) override;
    HRESULT __stdcall GetClipStatus(D3DCLIPSTATUS9* status) override;
    HRESULT __stdcall GetTexture(DWORD stage, IDirect3DBaseTexture8** out) override;
    HRESULT __stdcall SetTexture(DWORD stage, IDirect3DBaseTexture8* texture) override;
    HRESULT __stdcall GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD* value) override;
    HRESULT __stdcall SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) override;
    HRESULT __stdcall ValidateDevice(DWORD* passes) override;
    HRESULT __stdcall GetInfo(DWORD id, void* info, DWORD size) override;
    HRESULT __stdcall SetPaletteEntries(UINT palette, const PALETTEENTRY* entries) override;
    HRESULT __stdcall GetPaletteEntries(UINT palette, PALETTEENTRY* entries) override;
    HRESULT __stdcall SetCurrentTexturePalette(UINT palette) override;
    HRESULT __stdcall GetCurrentTexturePalette(UINT* palette) override;
    HRESULT __stdcall DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count) override;
    HRESULT __stdcall DrawIndexedPrimitive(D3DPRIMITIVETYPE type, UINT min_index, UINT vertices, UINT start, UINT count) override;
    HRESULT __stdcall DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) override;
    HRESULT __stdcall DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT min_index, UINT vertices, UINT count, const void* indices,
                                             D3DFORMAT format, const void* data, UINT stride) override;
    HRESULT __stdcall ProcessVertices(UINT source, UINT dest, UINT count, IDirect3DVertexBuffer8* buffer, DWORD flags) override;
    HRESULT __stdcall CreateVertexShader(const DWORD* declaration, const DWORD* function, DWORD* handle, DWORD usage) override;
    HRESULT __stdcall SetVertexShader(DWORD handle) override;
    HRESULT __stdcall GetVertexShader(DWORD* handle) override;
    HRESULT __stdcall DeleteVertexShader(DWORD handle) override;
    HRESULT __stdcall SetVertexShaderConstant(DWORD reg, const void* data, DWORD count) override;
    HRESULT __stdcall GetVertexShaderConstant(DWORD reg, void* data, DWORD count) override;
    HRESULT __stdcall GetVertexShaderDeclaration(DWORD handle, void* data, DWORD* size) override;
    HRESULT __stdcall GetVertexShaderFunction(DWORD handle, void* data, DWORD* size) override;
    HRESULT __stdcall SetStreamSource(UINT stream, IDirect3DVertexBuffer8* buffer, UINT stride) override;
    HRESULT __stdcall GetStreamSource(UINT stream, IDirect3DVertexBuffer8** out, UINT* stride) override;
    HRESULT __stdcall SetIndices(IDirect3DIndexBuffer8* buffer, UINT base_vertex) override;
    HRESULT __stdcall GetIndices(IDirect3DIndexBuffer8** out, UINT* base_vertex) override;
    HRESULT __stdcall CreatePixelShader(const DWORD* function, DWORD* handle) override;
    HRESULT __stdcall SetPixelShader(DWORD handle) override;
    HRESULT __stdcall GetPixelShader(DWORD* handle) override;
    HRESULT __stdcall DeletePixelShader(DWORD handle) override;
    HRESULT __stdcall SetPixelShaderConstant(DWORD reg, const void* data, DWORD count) override;
    HRESULT __stdcall GetPixelShaderConstant(DWORD reg, void* data, DWORD count) override;
    HRESULT __stdcall GetPixelShaderFunction(DWORD handle, void* data, DWORD* size) override;
    HRESULT __stdcall DrawRectPatch(UINT handle, const float* segments, const D3DRECTPATCH_INFO* info) override;
    HRESULT __stdcall DrawTriPatch(UINT handle, const float* segments, const D3DTRIPATCH_INFO* info) override;
    HRESULT __stdcall DeletePatch(UINT handle) override;

    // The wrapper for a Direct3D 9 object, whose reference it takes: the one it has, or a new one.
    template <class W, class Object9>
    W* wrap(Object9* object);
    IDirect3DBaseTexture8* wrap_texture(IDirect3DBaseTexture9* texture);
    // A wrapper is going: its object gets a new one next time.
    void forget(IUnknown* object);
    IDirect3DDevice9* device9() const { return device_; }
    // Wrappers made, and gone: the device (this object) outlives the game's last reference while any
    // are left, as their objects can still go (and their Trackers still call forget), until the last.
    void attach() { ++wrapper_count_; }
    void detach();
    // A call Direct3D 9 refused: logged the first time for each call and result, then counted, the
    // counts logged every 10 seconds (at Present) while they grow. `detail` only makes the first line.
    template <class Detail>
    HRESULT note(const wchar_t* call, HRESULT result, Detail&& detail)
    {
        if (SUCCEEDED(result))
            return result;
        unsigned& count = failures_[{call, result}];
        if (count++ == 0)
            log::write(L"Direct3D 9: %ls refused (0x%08lX)%ls", call, static_cast<unsigned long>(result), detail().c_str());
        return result;
    }
    HRESULT note(const wchar_t* call, HRESULT result)
    {
        return note(call, result, [] { return std::wstring(); });
    }

private:
    ~Device8();
    void close();
    void report_failures();
    // A texture's usage, made dynamic in the default pool: Direct3D 8 let the game lock a texture there,
    // Direct3D 9 only a dynamic one (render targets and depth surfaces aside)
    DWORD lockable(DWORD usage, D3DPOOL pool) const
    {
        const bool plain = pool == D3DPOOL_DEFAULT && (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) == 0;
        return plain && dynamic_textures_ ? usage | D3DUSAGE_DYNAMIC : usage;
    }
    // A system-memory surface holding `rect` of `source`, a surface on the graphics card that won't lock:
    // `rect` StretchRect'd into a render target its size, and that read back (only the rect crosses);
    // else the whole surface read back. `at` is where the rect is in it. Kept for the next copy.
    IDirect3DSurface9* read_back(IDirect3DSurface9* source, const D3DSURFACE_DESC& from, const RECT& rect, RECT& at);
    IDirect3DSurface9* staging(D3DFORMAT format, UINT width, UINT height, bool target);
    void release_staging();
    VertexShader8* vertex_shader(DWORD handle);
    PixelShader8* pixel_shader(DWORD handle);
    StateBlock8* state_block(DWORD token);
    // Where a Set* call's Direct3D 8 state goes: into the block being recorded (Direct3D 9 records the
    // call without doing it), else the device's.
    Shadow& shadow(bool StateBlock8::*part);
    HRESULT copy_rects(IDirect3DSurface8* source, const RECT* rects, UINT count, IDirect3DSurface8* dest, const POINT* points);
    HRESULT copy_rect(IDirect3DSurface9* source, const D3DSURFACE_DESC& from, const RECT& rect, IDirect3DSurface9* dest,
                      const D3DSURFACE_DESC& to, POINT at);
    HRESULT copy_locked(IDirect3DSurface9* source, const D3DSURFACE_DESC& from, const RECT& rect, IDirect3DSurface9* dest,
                        const D3DSURFACE_DESC& to, POINT at);

    LONG refs_ = 1;            // the game's
    LONG wrapper_count_ = 0;   // wrappers not yet gone
    bool closing_ = false;     // in close(): what it releases may take wrappers with it
    bool closed_ = false;      // the game's last reference is gone
    Direct3D8* const d3d_;
    IDirect3DDevice9* const device_;
    std::map<std::pair<const wchar_t*, HRESULT>, unsigned> failures_; // see note
    std::map<std::pair<const wchar_t*, HRESULT>, unsigned> reported_; // the counts last logged
    ULONGLONG next_report_ = 0;
    const DWORD behavior_; // the game's (without PUREDEVICE, which would refuse the Get* calls)
    std::mutex mutex_;
    std::unordered_map<IUnknown*, void*> wrappers_;
    std::unordered_set<VertexShader8*> vertex_shaders_;
    std::unordered_set<PixelShader8*> pixel_shaders_;
    std::unordered_set<StateBlock8*> state_blocks_;
    StateBlock8* recording_ = nullptr;
    Shadow current_;
    DWORD spare_constant_ = 95; // the last vertex shader constant, VanaCore's (to_vs_1_1)
    bool dynamic_textures_ = false; // the device has them (D3DCAPS2_DYNAMICTEXTURES)
    // copy_locked's surfaces, by format, size and kind (render target, or system memory); the render
    // targets go before a Reset
    std::map<std::tuple<D3DFORMAT, UINT, UINT, bool>, IDirect3DSurface9*> staging_;
    bool has_depth_; // a depth surface is set: Direct3D 9 refuses a Clear of depth without one
    // render states Direct3D 9 dropped, as the game set them
    DWORD zbias_ = 0;
    DWORD line_pattern_ = 0;
    DWORD zvisible_ = 0;
    DWORD edge_antialias_ = 0;
    DWORD patch_segments_ = float_bits(1.0f);
    bool logged_foreign_ = false;
    std::unordered_set<UINT> logged_missing_; // the missing inputs' masks logged (to_vs_1_1)
};

// Every wrapper: Direct3D 8's interface, then (right after the vtable) the Direct3D 9 object and the
// tag, and its device (which lives until it's gone: see attach).
template <class Self, class Interface, class Object9>
class Wrapper : public Interface {
public:
    Wrapper(Device8* device, Object9* object) : object_(object), device_(device)
    {
        assert(static_cast<const Header*>(static_cast<const void*>(this))->tag == kTag); // where unwrap reads it
        device_->attach();
    }
    Wrapper(const Wrapper&) = delete;
    Wrapper& operator=(const Wrapper&) = delete;

    Object9* get() const { return static_cast<Object9*>(object_); }

    HRESULT __stdcall QueryInterface(REFIID riid, void** out) override
    {
        if (out == nullptr)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || Self::is(riid)) {
            this->AddRef();
            *out = static_cast<Interface*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG __stdcall AddRef() override { return object_->AddRef(); }
    ULONG __stdcall Release() override
    {
        // a tracked wrapper may be gone once its object is (its Tracker deletes it): nothing of it after
        const bool tracked = tracked_;
        IUnknown* const object = object_;
        const ULONG left = object->Release();
        if (left == 0 && !tracked)
            destroy(this);
        return left;
    }

    // The wrapper's end: its object is gone (a Tracker's call), or the game's last reference to one that
    // can't be tracked (a swap chain) is.
    static void destroy(void* wrapper)
    {
        auto* const self = static_cast<Self*>(static_cast<Wrapper*>(wrapper));
        Device8* const device = self->device_;
        device->forget(self->object_);
        delete self;
        device->detach();
    }

protected:
    HRESULT give_device(IDirect3DDevice8** out) const
    {
        if (out == nullptr)
            return D3DERR_INVALIDCALL;
        device_->AddRef();
        *out = device_;
        return D3D_OK;
    }

    IUnknown* const object_; // right after the vtable: see Header
    const DWORD tag_ = kTag;
    Device8* const device_;

public:
    bool tracked_ = false; // a Tracker deletes it with its object; else its last Release does (after Header's fields)
};

template <class Self, class Interface, class Object9>
class Resource : public Wrapper<Self, Interface, Object9> {
public:
    using Wrapper<Self, Interface, Object9>::Wrapper;

    HRESULT __stdcall GetDevice(IDirect3DDevice8** out) override { return this->give_device(out); }
    HRESULT __stdcall SetPrivateData(REFGUID guid, const void* data, DWORD size, DWORD flags) override
    {
        return this->get()->SetPrivateData(guid, data, size, flags);
    }
    HRESULT __stdcall GetPrivateData(REFGUID guid, void* data, DWORD* size) override { return this->get()->GetPrivateData(guid, data, size); }
    HRESULT __stdcall FreePrivateData(REFGUID guid) override { return this->get()->FreePrivateData(guid); }
    DWORD __stdcall SetPriority(DWORD priority) override { return this->get()->SetPriority(priority); }
    DWORD __stdcall GetPriority() override { return this->get()->GetPriority(); }
    void __stdcall PreLoad() override { this->get()->PreLoad(); }
    D3DRESOURCETYPE __stdcall GetType() override { return this->get()->GetType(); }
};

template <class Self, class Interface, class Object9>
class BaseTexture : public Resource<Self, Interface, Object9> {
public:
    using Resource<Self, Interface, Object9>::Resource;

    DWORD __stdcall SetLOD(DWORD lod) override { return this->get()->SetLOD(lod); }
    DWORD __stdcall GetLOD() override { return this->get()->GetLOD(); }
    DWORD __stdcall GetLevelCount() override { return this->get()->GetLevelCount(); }
};

class Surface8 final : public Wrapper<Surface8, IDirect3DSurface8, IDirect3DSurface9> {
public:
    using Wrapper::Wrapper;
    ~Surface8()
    {
        if (copy_ != nullptr)
            copy_->Release();
    }
    static bool is(REFIID riid) { return riid == kIIDSurface8; }

    HRESULT __stdcall GetDevice(IDirect3DDevice8** out) override { return give_device(out); }
    HRESULT __stdcall SetPrivateData(REFGUID guid, const void* data, DWORD size, DWORD flags) override
    {
        return get()->SetPrivateData(guid, data, size, flags);
    }
    HRESULT __stdcall GetPrivateData(REFGUID guid, void* data, DWORD* size) override { return get()->GetPrivateData(guid, data, size); }
    HRESULT __stdcall FreePrivateData(REFGUID guid) override { return get()->FreePrivateData(guid); }
    HRESULT __stdcall GetContainer(REFIID riid, void** out) override;
    HRESULT __stdcall GetDesc(D3DSURFACE_DESC8* desc) override
    {
        if (desc == nullptr)
            return D3DERR_INVALIDCALL;
        D3DSURFACE_DESC in{};
        const HRESULT result = get()->GetDesc(&in);
        if (SUCCEEDED(result))
            to_desc8(in, desc);
        return result;
    }
    HRESULT __stdcall LockRect(D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) override
    {
        const HRESULT result = get()->LockRect(locked, rect, flags);
        if (SUCCEEDED(result) || copy_locked_)
            return result;
        // A render target Direct3D 9 won't lock (Direct3D 8 locked a back buffer): its copy in system memory,
        // written back at UnlockRect unless it was read only
        D3DSURFACE_DESC desc{};
        if (FAILED(get()->GetDesc(&desc)) || desc.Pool != D3DPOOL_DEFAULT || (desc.Usage & D3DUSAGE_RENDERTARGET) == 0 ||
            desc.MultiSampleType != D3DMULTISAMPLE_NONE)
            return result;
        IDirect3DDevice9* device = device_->device9();
        if (copy_ == nullptr &&
            FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &copy_, nullptr)))
            return result;
        if (FAILED(device->GetRenderTargetData(get(), copy_)) || FAILED(copy_->LockRect(locked, rect, flags)))
            return result;
        copy_locked_ = true;
        copy_flags_ = flags;
        return D3D_OK;
    }
    HRESULT __stdcall UnlockRect() override
    {
        if (!copy_locked_)
            return get()->UnlockRect();
        copy_locked_ = false;
        copy_->UnlockRect();
        if ((copy_flags_ & D3DLOCK_READONLY) != 0)
            return D3D_OK;
        return device_->device9()->UpdateSurface(copy_, nullptr, get(), nullptr);
    }

private:
    IDirect3DSurface9* copy_ = nullptr; // a render target's copy in system memory, kept for its next lock
    bool copy_locked_ = false;          // the lock is the copy's
    DWORD copy_flags_ = 0;
};

class Volume8 final : public Wrapper<Volume8, IDirect3DVolume8, IDirect3DVolume9> {
public:
    using Wrapper::Wrapper;
    static bool is(REFIID riid) { return riid == kIIDVolume8; }

    HRESULT __stdcall GetDevice(IDirect3DDevice8** out) override { return give_device(out); }
    HRESULT __stdcall SetPrivateData(REFGUID guid, const void* data, DWORD size, DWORD flags) override
    {
        return get()->SetPrivateData(guid, data, size, flags);
    }
    HRESULT __stdcall GetPrivateData(REFGUID guid, void* data, DWORD* size) override { return get()->GetPrivateData(guid, data, size); }
    HRESULT __stdcall FreePrivateData(REFGUID guid) override { return get()->FreePrivateData(guid); }
    HRESULT __stdcall GetContainer(REFIID riid, void** out) override;
    HRESULT __stdcall GetDesc(D3DVOLUME_DESC8* desc) override
    {
        if (desc == nullptr)
            return D3DERR_INVALIDCALL;
        D3DVOLUME_DESC in{};
        const HRESULT result = get()->GetDesc(&in);
        if (SUCCEEDED(result))
            to_desc8(in, desc);
        return result;
    }
    HRESULT __stdcall LockBox(D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags) override { return get()->LockBox(locked, box, flags); }
    HRESULT __stdcall UnlockBox() override { return get()->UnlockBox(); }
};

class Texture8 final : public BaseTexture<Texture8, IDirect3DTexture8, IDirect3DTexture9> {
public:
    using BaseTexture::BaseTexture;
    static bool is(REFIID riid) { return riid == kIIDTexture8 || riid == kIIDBaseTexture8 || riid == kIIDResource8; }

    HRESULT __stdcall GetLevelDesc(UINT level, D3DSURFACE_DESC8* desc) override
    {
        if (desc == nullptr)
            return D3DERR_INVALIDCALL;
        D3DSURFACE_DESC in{};
        const HRESULT result = get()->GetLevelDesc(level, &in);
        if (SUCCEEDED(result))
            to_desc8(in, desc);
        return result;
    }
    HRESULT __stdcall GetSurfaceLevel(UINT level, IDirect3DSurface8** out) override
    {
        if (out == nullptr)
            return D3DERR_INVALIDCALL;
        IDirect3DSurface9* surface = nullptr;
        const HRESULT result = get()->GetSurfaceLevel(level, &surface);
        *out = SUCCEEDED(result) ? device_->wrap<Surface8>(surface) : nullptr;
        return result;
    }
    HRESULT __stdcall LockRect(UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) override
    {
        return device_->note(L"IDirect3DTexture8::LockRect", get()->LockRect(level, locked, rect, flags));
    }
    HRESULT __stdcall UnlockRect(UINT level) override { return get()->UnlockRect(level); }
    HRESULT __stdcall AddDirtyRect(const RECT* rect) override { return get()->AddDirtyRect(rect); }
};

class CubeTexture8 final : public BaseTexture<CubeTexture8, IDirect3DCubeTexture8, IDirect3DCubeTexture9> {
public:
    using BaseTexture::BaseTexture;
    static bool is(REFIID riid) { return riid == kIIDCubeTexture8 || riid == kIIDBaseTexture8 || riid == kIIDResource8; }

    HRESULT __stdcall GetLevelDesc(UINT level, D3DSURFACE_DESC8* desc) override
    {
        if (desc == nullptr)
            return D3DERR_INVALIDCALL;
        D3DSURFACE_DESC in{};
        const HRESULT result = get()->GetLevelDesc(level, &in);
        if (SUCCEEDED(result))
            to_desc8(in, desc);
        return result;
    }
    HRESULT __stdcall GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface8** out) override
    {
        if (out == nullptr)
            return D3DERR_INVALIDCALL;
        IDirect3DSurface9* surface = nullptr;
        const HRESULT result = get()->GetCubeMapSurface(face, level, &surface);
        *out = SUCCEEDED(result) ? device_->wrap<Surface8>(surface) : nullptr;
        return result;
    }
    HRESULT __stdcall LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) override
    {
        return get()->LockRect(face, level, locked, rect, flags);
    }
    HRESULT __stdcall UnlockRect(D3DCUBEMAP_FACES face, UINT level) override { return get()->UnlockRect(face, level); }
    HRESULT __stdcall AddDirtyRect(D3DCUBEMAP_FACES face, const RECT* rect) override { return get()->AddDirtyRect(face, rect); }
};

class VolumeTexture8 final : public BaseTexture<VolumeTexture8, IDirect3DVolumeTexture8, IDirect3DVolumeTexture9> {
public:
    using BaseTexture::BaseTexture;
    static bool is(REFIID riid) { return riid == kIIDVolumeTexture8 || riid == kIIDBaseTexture8 || riid == kIIDResource8; }

    HRESULT __stdcall GetLevelDesc(UINT level, D3DVOLUME_DESC8* desc) override
    {
        if (desc == nullptr)
            return D3DERR_INVALIDCALL;
        D3DVOLUME_DESC in{};
        const HRESULT result = get()->GetLevelDesc(level, &in);
        if (SUCCEEDED(result))
            to_desc8(in, desc);
        return result;
    }
    HRESULT __stdcall GetVolumeLevel(UINT level, IDirect3DVolume8** out) override
    {
        if (out == nullptr)
            return D3DERR_INVALIDCALL;
        IDirect3DVolume9* volume = nullptr;
        const HRESULT result = get()->GetVolumeLevel(level, &volume);
        *out = SUCCEEDED(result) ? device_->wrap<Volume8>(volume) : nullptr;
        return result;
    }
    HRESULT __stdcall LockBox(UINT level, D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags) override
    {
        return get()->LockBox(level, locked, box, flags);
    }
    HRESULT __stdcall UnlockBox(UINT level) override { return get()->UnlockBox(level); }
    HRESULT __stdcall AddDirtyBox(const D3DBOX* box) override { return get()->AddDirtyBox(box); }
};

class VertexBuffer8 final : public Resource<VertexBuffer8, IDirect3DVertexBuffer8, IDirect3DVertexBuffer9> {
public:
    using Resource::Resource;
    static bool is(REFIID riid) { return riid == kIIDVertexBuffer8 || riid == kIIDResource8; }

    HRESULT __stdcall Lock(UINT offset, UINT size, BYTE** data, DWORD flags) override
    {
        return device_->note(L"IDirect3DVertexBuffer8::Lock", get()->Lock(offset, size, reinterpret_cast<void**>(data), flags));
    }
    HRESULT __stdcall Unlock() override { return get()->Unlock(); }
    HRESULT __stdcall GetDesc(D3DVERTEXBUFFER_DESC* desc) override { return get()->GetDesc(desc); }
};

class IndexBuffer8 final : public Resource<IndexBuffer8, IDirect3DIndexBuffer8, IDirect3DIndexBuffer9> {
public:
    using Resource::Resource;
    static bool is(REFIID riid) { return riid == kIIDIndexBuffer8 || riid == kIIDResource8; }

    HRESULT __stdcall Lock(UINT offset, UINT size, BYTE** data, DWORD flags) override
    {
        return device_->note(L"IDirect3DIndexBuffer8::Lock", get()->Lock(offset, size, reinterpret_cast<void**>(data), flags));
    }
    HRESULT __stdcall Unlock() override { return get()->Unlock(); }
    HRESULT __stdcall GetDesc(D3DINDEXBUFFER_DESC* desc) override { return get()->GetDesc(desc); }
};

class SwapChain8 final : public Wrapper<SwapChain8, IDirect3DSwapChain8, IDirect3DSwapChain9> {
public:
    using Wrapper::Wrapper;
    static bool is(REFIID riid) { return riid == kIIDSwapChain8; }

    HRESULT __stdcall Present(const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty) override
    {
        return get()->Present(source, dest, window, dirty, 0);
    }
    HRESULT __stdcall GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface8** out) override
    {
        if (out == nullptr)
            return D3DERR_INVALIDCALL;
        IDirect3DSurface9* surface = nullptr;
        const HRESULT result = get()->GetBackBuffer(index, type, &surface);
        *out = SUCCEEDED(result) ? device_->wrap<Surface8>(surface) : nullptr;
        return result;
    }
};

HRESULT __stdcall Surface8::GetContainer(REFIID riid, void** out)
{
    if (out == nullptr)
        return D3DERR_INVALIDCALL;
    *out = nullptr;
    const bool any = riid == __uuidof(IUnknown) || riid == kIIDResource8 || riid == kIIDBaseTexture8;
    if (any || riid == kIIDTexture8) {
        IDirect3DTexture9* texture = nullptr;
        if (SUCCEEDED(get()->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&texture)))) {
            *out = static_cast<IDirect3DTexture8*>(device_->wrap<Texture8>(texture));
            return D3D_OK;
        }
    }
    if (any || riid == kIIDCubeTexture8) {
        IDirect3DCubeTexture9* cube = nullptr;
        if (SUCCEEDED(get()->GetContainer(__uuidof(IDirect3DCubeTexture9), reinterpret_cast<void**>(&cube)))) {
            *out = static_cast<IDirect3DCubeTexture8*>(device_->wrap<CubeTexture8>(cube));
            return D3D_OK;
        }
    }
    if (riid == __uuidof(IUnknown) || riid == kIIDDevice8) { // a back buffer's, or one of the device's own
        device_->AddRef();
        *out = static_cast<IDirect3DDevice8*>(device_);
        return D3D_OK;
    }
    return E_NOINTERFACE;
}

HRESULT __stdcall Volume8::GetContainer(REFIID riid, void** out)
{
    if (out == nullptr)
        return D3DERR_INVALIDCALL;
    *out = nullptr;
    if (riid == __uuidof(IUnknown) || riid == kIIDResource8 || riid == kIIDBaseTexture8 || riid == kIIDVolumeTexture8) {
        IDirect3DVolumeTexture9* texture = nullptr;
        if (SUCCEEDED(get()->GetContainer(__uuidof(IDirect3DVolumeTexture9), reinterpret_cast<void**>(&texture)))) {
            *out = static_cast<IDirect3DVolumeTexture8*>(device_->wrap<VolumeTexture8>(texture));
            return D3D_OK;
        }
    }
    if (riid == kIIDDevice8) {
        device_->AddRef();
        *out = static_cast<IDirect3DDevice8*>(device_);
        return D3D_OK;
    }
    return E_NOINTERFACE;
}

// ---- IDirect3D8 ----

// The display formats Direct3D 8 lists its modes in (Direct3D 9 lists one format at a time)
constexpr D3DFORMAT kModeFormats[] = {D3DFMT_X8R8G8B8, D3DFMT_R5G6B5, D3DFMT_X1R5G5B5};

class Direct3D8 final : public IDirect3D8 {
public:
    explicit Direct3D8(IDirect3D9* d3d) : d3d_(d3d) {}
    Direct3D8(const Direct3D8&) = delete;
    Direct3D8& operator=(const Direct3D8&) = delete;

    HRESULT __stdcall QueryInterface(REFIID riid, void** out) override
    {
        if (out == nullptr)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == kIIDDirect3D8) {
            AddRef();
            *out = static_cast<IDirect3D8*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG __stdcall AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG __stdcall Release() override
    {
        const LONG left = InterlockedDecrement(&refs_);
        if (left == 0) {
            d3d_->Release();
            delete this;
        }
        return static_cast<ULONG>(left);
    }

    HRESULT __stdcall RegisterSoftwareDevice(void* initialize) override { return d3d_->RegisterSoftwareDevice(initialize); }
    UINT __stdcall GetAdapterCount() override { return d3d_->GetAdapterCount(); }
    HRESULT __stdcall GetAdapterIdentifier(UINT adapter, DWORD, D3DADAPTER_IDENTIFIER8* identifier) override
    {
        if (identifier == nullptr)
            return D3DERR_INVALIDCALL;
        D3DADAPTER_IDENTIFIER9 in{}; // without the WHQL level, which can take seconds to find
        const HRESULT result = d3d_->GetAdapterIdentifier(adapter, 0, &in);
        if (FAILED(result))
            return result;
        std::memcpy(identifier->Driver, in.Driver, sizeof identifier->Driver);
        std::memcpy(identifier->Description, in.Description, sizeof identifier->Description);
        identifier->DriverVersion = in.DriverVersion;
        identifier->VendorId = in.VendorId;
        identifier->DeviceId = in.DeviceId;
        identifier->SubSysId = in.SubSysId;
        identifier->Revision = in.Revision;
        identifier->DeviceIdentifier = in.DeviceIdentifier;
        identifier->WHQLLevel = in.WHQLLevel;
        return result;
    }
    UINT __stdcall GetAdapterModeCount(UINT adapter) override
    {
        UINT count = 0;
        for (const D3DFORMAT format : kModeFormats)
            count += d3d_->GetAdapterModeCount(adapter, format);
        return count;
    }
    HRESULT __stdcall EnumAdapterModes(UINT adapter, UINT mode, D3DDISPLAYMODE* out) override
    {
        for (const D3DFORMAT format : kModeFormats) {
            const UINT count = d3d_->GetAdapterModeCount(adapter, format);
            if (mode < count)
                return d3d_->EnumAdapterModes(adapter, format, mode, out);
            mode -= count;
        }
        return D3DERR_INVALIDCALL;
    }
    HRESULT __stdcall GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE* out) override { return d3d_->GetAdapterDisplayMode(adapter, out); }
    HRESULT __stdcall CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT display, D3DFORMAT back_buffer, BOOL windowed) override
    {
        return d3d_->CheckDeviceType(adapter, type, display, back_buffer, windowed);
    }
    HRESULT __stdcall CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_format, DWORD usage, D3DRESOURCETYPE resource,
                                        D3DFORMAT format) override
    {
        return d3d_->CheckDeviceFormat(adapter, type, adapter_format, usage, resource, format);
    }
    HRESULT __stdcall CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT format, BOOL windowed,
                                                 D3DMULTISAMPLE_TYPE multisample) override
    {
        return d3d_->CheckDeviceMultiSampleType(adapter, type, format, windowed, multisample, nullptr);
    }
    HRESULT __stdcall CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_format, D3DFORMAT target,
                                             D3DFORMAT depth) override
    {
        return d3d_->CheckDepthStencilMatch(adapter, type, adapter_format, target, depth);
    }
    HRESULT __stdcall GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS8* caps) override
    {
        if (caps == nullptr)
            return D3DERR_INVALIDCALL;
        D3DCAPS9 in{};
        const HRESULT result = d3d_->GetDeviceCaps(adapter, type, &in);
        if (SUCCEEDED(result))
            to_caps8(in, caps);
        return result;
    }
    HMONITOR __stdcall GetAdapterMonitor(UINT adapter) override { return d3d_->GetAdapterMonitor(adapter); }
    HRESULT __stdcall CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behavior, D3DPRESENT_PARAMETERS8* params,
                                   IDirect3DDevice8** out) override
    {
        if (params == nullptr || out == nullptr)
            return D3DERR_INVALIDCALL;
        *out = nullptr;
        D3DPRESENT_PARAMETERS params9 = to_params9(*params);
        if (focus == nullptr && params9.hDeviceWindow == nullptr)
            focus = hidden_window();
        IDirect3DDevice9* device = nullptr;
        const HRESULT result = d3d_->CreateDevice(adapter, type, focus, behavior & ~D3DCREATE_PUREDEVICE, &params9, &device);
        if (FAILED(result)) {
            log::write(L"Direct3D 9: CreateDevice failed (0x%08lX) for a %ux%u %ls back buffer", static_cast<unsigned long>(result),
                       params->BackBufferWidth, params->BackBufferHeight, params->Windowed ? L"windowed" : L"full-screen");
            return result;
        }
        from_params9(params9, *params);
        log::write(L"Direct3D 9: device created (%ux%u, %ls, vsync %ls)", params9.BackBufferWidth, params9.BackBufferHeight,
                   params9.Windowed ? L"windowed" : L"full screen", g_vsync ? L"on" : L"off");
        *out = new Device8(this, device, behavior, params9.EnableAutoDepthStencil != FALSE);
        return D3D_OK;
    }

private:
    LONG refs_ = 1;
    IDirect3D9* const d3d_;
};

// ---- IDirect3DDevice8 ----

Device8::Device8(Direct3D8* d3d, IDirect3DDevice9* device, DWORD behavior, bool depth)
    : d3d_(d3d), device_(device), behavior_(behavior), has_depth_(depth)
{
    d3d_->AddRef();
    D3DCAPS9 caps{};
    if (SUCCEEDED(device_->GetDeviceCaps(&caps))) {
        if (caps.MaxVertexShaderConst > 0)
            spare_constant_ = caps.MaxVertexShaderConst - 1;
        dynamic_textures_ = (caps.Caps2 & D3DCAPS2_DYNAMICTEXTURES) != 0;
    }
}

Device8::~Device8()
{
    d3d_->Release();
}

// The game's last reference is gone: VanaCore's Direct3D 9 objects go, and the device. Its resources the
// game still has keep Direct3D 9's device (and so this object: detach) until they go too.
void Device8::close()
{
    closing_ = true;
    for (StateBlock8* block : state_blocks_) {
        if (block->block != nullptr)
            block->block->Release();
        delete block;
    }
    state_blocks_.clear();
    delete recording_;
    recording_ = nullptr;
    for (VertexShader8* shader : vertex_shaders_) {
        if (shader->declaration9 != nullptr)
            shader->declaration9->Release();
        if (shader->shader9 != nullptr)
            shader->shader9->Release();
        delete shader;
    }
    vertex_shaders_.clear();
    for (PixelShader8* shader : pixel_shaders_) {
        if (shader->shader9 != nullptr)
            shader->shader9->Release();
        delete shader;
    }
    pixel_shaders_.clear();
    release_staging();
    device_->Release(); // its resources nobody has go now, and their wrappers (detach doesn't delete meanwhile)
    closing_ = false;
    closed_ = true;
    if (wrapper_count_ == 0)
        delete this;
}

void Device8::detach()
{
    if (--wrapper_count_ == 0 && closed_ && !closing_)
        delete this; // the last wrapper of a device the game let go (Direct3D 9's goes after this returns)
}

void Device8::report_failures()
{
    const ULONGLONG now = GetTickCount64();
    if (failures_.empty() || now < next_report_)
        return;
    next_report_ = now + 10000;
    if (failures_ == reported_)
        return;
    std::wstring line;
    for (const auto& [key, count] : failures_) {
        const auto before = reported_.find(key);
        const unsigned grown = count - (before != reported_.end() ? before->second : 0);
        if (grown == 0)
            continue;
        wchar_t item[96];
        swprintf_s(item, L"%ls%ls 0x%08lX x%u", line.empty() ? L"" : L", ", key.first, static_cast<unsigned long>(key.second), grown);
        line += item;
    }
    reported_ = failures_;
    log::write(L"Direct3D 9: refused since the last count: %ls", line.c_str());
}

// {5C0C3B7E-7A1E-4C39-9D8E-2B6F4E1A9D31}: the private data a Tracker is kept in
constexpr GUID kTrackerGuid = {0x5c0c3b7e, 0x7a1e, 0x4c39, {0x9d, 0x8e, 0x2b, 0x6f, 0x4e, 0x1a, 0x9d, 0x31}};

// Deletes a wrapper when its object goes: kept in the object's private data (D3DSPD_IUNKNOWN), which
// Direct3D 9 releases as it destroys the object.
class Tracker final : public IUnknown {
public:
    Tracker(void* wrapper, void (*destroy)(void*)) : wrapper_(wrapper), destroy_(destroy) {}
    Tracker(const Tracker&) = delete;
    Tracker& operator=(const Tracker&) = delete;

    HRESULT __stdcall QueryInterface(REFIID riid, void** out) override
    {
        if (out == nullptr)
            return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            AddRef();
            *out = static_cast<IUnknown*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG __stdcall AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG __stdcall Release() override
    {
        const LONG left = InterlockedDecrement(&refs_);
        if (left == 0) {
            destroy_(wrapper_);
            delete this;
        }
        return static_cast<ULONG>(left);
    }

private:
    LONG refs_ = 1;
    void* const wrapper_;
    void (*const destroy_)(void*);
};

// A Tracker for a wrapper, in its object's private data; false for an object without any (a swap chain),
// or if Direct3D 9 wouldn't keep it.
template <class W, class Object9>
bool track(W* wrapper, Object9* object)
{
    if constexpr (requires(Object9* o) { o->SetPrivateData(kTrackerGuid, nullptr, DWORD{0}, DWORD{0}); }) {
        auto* const tracker = new Tracker(wrapper, &W::destroy);
        const bool kept = SUCCEEDED(object->SetPrivateData(kTrackerGuid, static_cast<IUnknown*>(tracker), sizeof(IUnknown*),
                                                           D3DSPD_IUNKNOWN));
        if (kept)
            tracker->Release(); // the object's reference is the one left
        else
            delete tracker;
        return kept;
    } else {
        return false;
    }
}

template <class W, class Object9>
W* Device8::wrap(Object9* object)
{
    if (object == nullptr)
        return nullptr;
    IUnknown* const key = object;
    {
        std::lock_guard lock(mutex_);
        const auto found = wrappers_.find(key);
        if (found != wrappers_.end())
            return static_cast<W*>(found->second); // the game's new reference is the object's
    }
    auto* wrapper = new W(this, object);
    wrapper->tracked_ = track(wrapper, object);
    if (wrapper->tracked_) { // an untracked one isn't kept: it can't be known to be gone
        std::lock_guard lock(mutex_);
        wrappers_[key] = wrapper;
    }
    return wrapper;
}

IDirect3DBaseTexture8* Device8::wrap_texture(IDirect3DBaseTexture9* texture)
{
    if (texture == nullptr)
        return nullptr;
    switch (texture->GetType()) {
    case D3DRTYPE_TEXTURE: return wrap<Texture8>(static_cast<IDirect3DTexture9*>(texture));
    case D3DRTYPE_CUBETEXTURE: return wrap<CubeTexture8>(static_cast<IDirect3DCubeTexture9*>(texture));
    case D3DRTYPE_VOLUMETEXTURE: return wrap<VolumeTexture8>(static_cast<IDirect3DVolumeTexture9*>(texture));
    default: texture->Release(); return nullptr;
    }
}

void Device8::forget(IUnknown* object)
{
    std::lock_guard lock(mutex_);
    wrappers_.erase(object);
}

VertexShader8* Device8::vertex_shader(DWORD handle)
{
    auto* shader = reinterpret_cast<VertexShader8*>(static_cast<std::uintptr_t>(handle & ~1u));
    return (handle & 1) != 0 && vertex_shaders_.count(shader) != 0 ? shader : nullptr;
}

PixelShader8* Device8::pixel_shader(DWORD handle)
{
    auto* shader = reinterpret_cast<PixelShader8*>(static_cast<std::uintptr_t>(handle));
    return pixel_shaders_.count(shader) != 0 ? shader : nullptr;
}

StateBlock8* Device8::state_block(DWORD token)
{
    auto* block = reinterpret_cast<StateBlock8*>(static_cast<std::uintptr_t>(token));
    return state_blocks_.count(block) != 0 ? block : nullptr;
}

Shadow& Device8::shadow(bool StateBlock8::*part)
{
    if (recording_ == nullptr)
        return current_;
    recording_->*part = true;
    return recording_->values;
}

HRESULT __stdcall Device8::QueryInterface(REFIID riid, void** out)
{
    if (out == nullptr)
        return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == kIIDDevice8) {
        AddRef();
        *out = static_cast<IDirect3DDevice8*>(this);
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG __stdcall Device8::AddRef()
{
    return static_cast<ULONG>(InterlockedIncrement(&refs_));
}

ULONG __stdcall Device8::Release()
{
    const LONG left = InterlockedDecrement(&refs_);
    if (left == 0)
        close();
    return static_cast<ULONG>(left);
}

HRESULT __stdcall Device8::TestCooperativeLevel()
{
    return device_->TestCooperativeLevel();
}

UINT __stdcall Device8::GetAvailableTextureMem()
{
    return device_->GetAvailableTextureMem();
}

HRESULT __stdcall Device8::ResourceManagerDiscardBytes(DWORD)
{
    return device_->EvictManagedResources();
}

HRESULT __stdcall Device8::GetDirect3D(IDirect3D8** out)
{
    if (out == nullptr)
        return D3DERR_INVALIDCALL;
    d3d_->AddRef();
    *out = d3d_;
    return D3D_OK;
}

HRESULT __stdcall Device8::GetDeviceCaps(D3DCAPS8* caps)
{
    if (caps == nullptr)
        return D3DERR_INVALIDCALL;
    D3DCAPS9 in{};
    const HRESULT result = device_->GetDeviceCaps(&in);
    if (SUCCEEDED(result))
        to_caps8(in, caps);
    return result;
}

HRESULT __stdcall Device8::GetDisplayMode(D3DDISPLAYMODE* mode)
{
    return device_->GetDisplayMode(0, mode);
}

HRESULT __stdcall Device8::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* params)
{
    const HRESULT result = device_->GetCreationParameters(params);
    if (SUCCEEDED(result))
        params->BehaviorFlags = behavior_;
    return result;
}

HRESULT __stdcall Device8::SetCursorProperties(UINT x, UINT y, IDirect3DSurface8* bitmap)
{
    return device_->SetCursorProperties(x, y, unwrap<IDirect3DSurface9>(bitmap));
}

void __stdcall Device8::SetCursorPosition(int x, int y, DWORD flags)
{
    device_->SetCursorPosition(x, y, flags);
}

BOOL __stdcall Device8::ShowCursor(BOOL show)
{
    return device_->ShowCursor(show);
}

HRESULT __stdcall Device8::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS8* params, IDirect3DSwapChain8** out)
{
    if (params == nullptr || out == nullptr)
        return D3DERR_INVALIDCALL;
    D3DPRESENT_PARAMETERS params9 = to_params9(*params);
    IDirect3DSwapChain9* chain = nullptr;
    const HRESULT result = device_->CreateAdditionalSwapChain(&params9, &chain);
    if (SUCCEEDED(result))
        from_params9(params9, *params);
    *out = SUCCEEDED(result) ? wrap<SwapChain8>(chain) : nullptr;
    return result;
}

HRESULT __stdcall Device8::Reset(D3DPRESENT_PARAMETERS8* params)
{
    if (params == nullptr)
        return D3DERR_INVALIDCALL;
    D3DPRESENT_PARAMETERS params9 = to_params9(*params);
    release_staging(); // its render targets would stop the Reset
    HRESULT result = device_->Reset(&params9);
    if (result == D3DERR_INVALIDCALL) {
        // State blocks can hold what Direct3D 9 wants gone before a Reset (Direct3D 8's survived it):
        // dropped (a captured one is made again at its next capture), and Reset again
        bool dropped = false;
        for (StateBlock8* block : state_blocks_) {
            if (block->block != nullptr) {
                block->block->Release();
                block->block = nullptr;
                dropped = true;
            }
        }
        if (dropped) {
            params9 = to_params9(*params);
            result = device_->Reset(&params9);
        }
    }
    if (FAILED(result)) {
        log::write(L"Direct3D 9: Reset failed (0x%08lX)", static_cast<unsigned long>(result));
        return result;
    }
    from_params9(params9, *params);
    current_ = Shadow{};
    has_depth_ = params9.EnableAutoDepthStencil != FALSE;
    zbias_ = line_pattern_ = zvisible_ = edge_antialias_ = 0;
    patch_segments_ = float_bits(1.0f);
    log::write(L"Direct3D 9: device reset (%ux%u, %ls)", params9.BackBufferWidth, params9.BackBufferHeight,
               params9.Windowed ? L"windowed" : L"full screen");
    return result;
}

HRESULT __stdcall Device8::Present(const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty)
{
    report_failures();
    return device_->Present(source, dest, window, dirty);
}

HRESULT __stdcall Device8::GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface8** out)
{
    if (out == nullptr)
        return D3DERR_INVALIDCALL;
    IDirect3DSurface9* surface = nullptr;
    const HRESULT result = device_->GetBackBuffer(0, index, type, &surface);
    *out = SUCCEEDED(result) ? wrap<Surface8>(surface) : nullptr;
    return result;
}

HRESULT __stdcall Device8::GetRasterStatus(D3DRASTER_STATUS* status)
{
    return device_->GetRasterStatus(0, status);
}

void __stdcall Device8::SetGammaRamp(DWORD flags, const D3DGAMMARAMP* ramp)
{
    device_->SetGammaRamp(0, flags, ramp);
}

void __stdcall Device8::GetGammaRamp(D3DGAMMARAMP* ramp)
{
    device_->GetGammaRamp(0, ramp);
}

HRESULT __stdcall Device8::CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                         IDirect3DTexture8** out)
{
    if (out == nullptr)
        return D3DERR_INVALIDCALL;
    IDirect3DTexture9* texture = nullptr;
    const HRESULT result = note(L"CreateTexture", device_->CreateTexture(width, height, levels, lockable(usage, pool), format, pool, &texture, nullptr));
    *out = SUCCEEDED(result) ? wrap<Texture8>(texture) : nullptr;
    return result;
}

HRESULT __stdcall Device8::CreateVolumeTexture(UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format,
                                               D3DPOOL pool, IDirect3DVolumeTexture8** out)
{
    if (out == nullptr)
        return D3DERR_INVALIDCALL;
    IDirect3DVolumeTexture9* texture = nullptr;
    const HRESULT result =
        note(L"CreateVolumeTexture", device_->CreateVolumeTexture(width, height, depth, levels, lockable(usage, pool), format, pool, &texture, nullptr));
    *out = SUCCEEDED(result) ? wrap<VolumeTexture8>(texture) : nullptr;
    return result;
}

HRESULT __stdcall Device8::CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                             IDirect3DCubeTexture8** out)
{
    if (out == nullptr)
        return D3DERR_INVALIDCALL;
    IDirect3DCubeTexture9* texture = nullptr;
    const HRESULT result = note(L"CreateCubeTexture", device_->CreateCubeTexture(edge, levels, lockable(usage, pool), format, pool, &texture, nullptr));
    *out = SUCCEEDED(result) ? wrap<CubeTexture8>(texture) : nullptr;
    return result;
}

HRESULT __stdcall Device8::CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer8** out)
{
    if (out == nullptr)
        return D3DERR_INVALIDCALL;
    IDirect3DVertexBuffer9* buffer = nullptr;
    const HRESULT result = note(L"CreateVertexBuffer", device_->CreateVertexBuffer(length, usage, fvf, pool, &buffer, nullptr));
    *out = SUCCEEDED(result) ? wrap<VertexBuffer8>(buffer) : nullptr;
    return result;
}

HRESULT __stdcall Device8::CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer8** out)
{
    if (out == nullptr)
        return D3DERR_INVALIDCALL;
    IDirect3DIndexBuffer9* buffer = nullptr;
    const HRESULT result = note(L"CreateIndexBuffer", device_->CreateIndexBuffer(length, usage, format, pool, &buffer, nullptr));
    *out = SUCCEEDED(result) ? wrap<IndexBuffer8>(buffer) : nullptr;
    return result;
}

HRESULT __stdcall Device8::CreateRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample, BOOL lockable,
                                              IDirect3DSurface8** out)
{
    if (out == nullptr)
        return D3DERR_INVALIDCALL;
    IDirect3DSurface9* surface = nullptr;
    const HRESULT result = note(L"CreateRenderTarget", device_->CreateRenderTarget(width, height, format, multisample, 0, lockable, &surface, nullptr));
    *out = SUCCEEDED(result) ? wrap<Surface8>(surface) : nullptr;
    return result;
}

HRESULT __stdcall Device8::CreateDepthStencilSurface(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample,
                                                     IDirect3DSurface8** out)
{
    if (out == nullptr)
        return D3DERR_INVALIDCALL;
    IDirect3DSurface9* surface = nullptr;
    const HRESULT result = note(L"CreateDepthStencilSurface", device_->CreateDepthStencilSurface(width, height, format, multisample, 0, FALSE, &surface, nullptr));
    *out = SUCCEEDED(result) ? wrap<Surface8>(surface) : nullptr;
    return result;
}

HRESULT __stdcall Device8::CreateImageSurface(UINT width, UINT height, D3DFORMAT format, IDirect3DSurface8** out)
{
    if (out == nullptr)
        return D3DERR_INVALIDCALL;
    IDirect3DSurface9* surface = nullptr;
    const HRESULT result = note(L"CreateImageSurface", device_->CreateOffscreenPlainSurface(width, height, format, D3DPOOL_SYSTEMMEM, &surface, nullptr));
    *out = SUCCEEDED(result) ? wrap<Surface8>(surface) : nullptr;
    return result;
}

// Rows of pixels (or of 4x4 blocks) from one lock to another
void copy_rows(const D3DLOCKED_RECT& from, const D3DLOCKED_RECT& to, D3DFORMAT format, LONG width, LONG height)
{
    bool block = false;
    const UINT bytes = format_bytes(format, block);
    const size_t row = block ? static_cast<size_t>((width + 3) / 4) * bytes : static_cast<size_t>(width) * bytes;
    const LONG rows = block ? (height + 3) / 4 : height;
    for (LONG y = 0; y < rows; ++y)
        std::memcpy(static_cast<BYTE*>(to.pBits) + static_cast<ptrdiff_t>(y) * to.Pitch,
                    static_cast<const BYTE*>(from.pBits) + static_cast<ptrdiff_t>(y) * from.Pitch, row);
}

HRESULT Device8::copy_locked(IDirect3DSurface9* source, const D3DSURFACE_DESC& from, const RECT& rect, IDirect3DSurface9* dest,
                             const D3DSURFACE_DESC& to, POINT at)
{
    const LONG width = rect.right - rect.left, height = rect.bottom - rect.top;
    // the source: its own lock, or the rect read back from the graphics card
    IDirect3DSurface9* read = source;
    D3DLOCKED_RECT in{};
    if (FAILED(source->LockRect(&in, &rect, D3DLOCK_READONLY))) {
        RECT within{};
        read = read_back(source, from, rect, within);
        if (read == nullptr || FAILED(read->LockRect(&in, &within, D3DLOCK_READONLY)))
            return D3DERR_INVALIDCALL;
    }
    // the destination: its own lock, or a copy sent up to it after
    const RECT target{at.x, at.y, at.x + width, at.y + height};
    D3DLOCKED_RECT out{};
    HRESULT result = dest->LockRect(&out, &target, 0);
    if (SUCCEEDED(result)) {
        copy_rows(in, out, from.Format, width, height);
        dest->UnlockRect();
    } else if (to.Pool == D3DPOOL_DEFAULT) {
        IDirect3DSurface9* const write = staging(to.Format, static_cast<UINT>(width), static_cast<UINT>(height), false);
        result = write != nullptr ? write->LockRect(&out, nullptr, 0) : D3DERR_INVALIDCALL;
        if (SUCCEEDED(result)) {
            copy_rows(in, out, from.Format, width, height);
            write->UnlockRect();
            result = device_->UpdateSurface(write, nullptr, dest, &at);
        }
    }
    read->UnlockRect();
    return result;
}

IDirect3DSurface9* Device8::staging(D3DFORMAT format, UINT width, UINT height, bool target)
{
    IDirect3DSurface9*& surface = staging_[{format, width, height, target}];
    if (surface == nullptr) {
        const HRESULT result = target ? device_->CreateRenderTarget(width, height, format, D3DMULTISAMPLE_NONE, 0, FALSE, &surface, nullptr)
                                      : device_->CreateOffscreenPlainSurface(width, height, format, D3DPOOL_SYSTEMMEM, &surface, nullptr);
        if (FAILED(result))
            surface = nullptr;
    }
    return surface;
}

void Device8::release_staging()
{
    for (auto& [key, surface] : staging_)
        if (surface != nullptr)
            surface->Release();
    staging_.clear();
}

IDirect3DSurface9* Device8::read_back(IDirect3DSurface9* source, const D3DSURFACE_DESC& from, const RECT& rect, RECT& at)
{
    const UINT width = static_cast<UINT>(rect.right - rect.left), height = static_cast<UINT>(rect.bottom - rect.top);
    if (width != from.Width || height != from.Height) {
        IDirect3DSurface9* const target = staging(from.Format, width, height, true);
        IDirect3DSurface9* const memory = staging(from.Format, width, height, false);
        if (target != nullptr && memory != nullptr && SUCCEEDED(device_->StretchRect(source, &rect, target, nullptr, D3DTEXF_NONE)) &&
            SUCCEEDED(device_->GetRenderTargetData(target, memory))) {
            at = RECT{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
            return memory;
        }
    }
    IDirect3DSurface9* const memory = staging(from.Format, from.Width, from.Height, false);
    if (memory == nullptr || FAILED(device_->GetRenderTargetData(source, memory)))
        return nullptr;
    at = rect;
    return memory;
}

HRESULT Device8::copy_rect(IDirect3DSurface9* source, const D3DSURFACE_DESC& from, const RECT& rect, IDirect3DSurface9* dest,
                           const D3DSURFACE_DESC& to, POINT at)
{
    const RECT target{at.x, at.y, at.x + rect.right - rect.left, at.y + rect.bottom - rect.top};
    if (from.Pool == D3DPOOL_DEFAULT && to.Pool == D3DPOOL_DEFAULT) {
        if (SUCCEEDED(device_->StretchRect(source, &rect, dest, &target, D3DTEXF_NONE)))
            return D3D_OK;
    } else if (from.Pool == D3DPOOL_SYSTEMMEM && to.Pool == D3DPOOL_DEFAULT) {
        if (SUCCEEDED(device_->UpdateSurface(source, &rect, dest, &at)))
            return D3D_OK;
    } else if (from.Pool == D3DPOOL_DEFAULT && to.Pool == D3DPOOL_SYSTEMMEM && from.Width == to.Width && from.Height == to.Height &&
               rect.left == 0 && rect.top == 0 && rect.right == static_cast<LONG>(from.Width) &&
               rect.bottom == static_cast<LONG>(from.Height) && at.x == 0 && at.y == 0) {
        if (SUCCEEDED(device_->GetRenderTargetData(source, dest)))
            return D3D_OK;
    }
    // the other pairs (a managed texture's level, an image surface), or where those were refused
    return copy_locked(source, from, rect, dest, to, at);
}

HRESULT __stdcall Device8::CopyRects(IDirect3DSurface8* source8, const RECT* rects, UINT count, IDirect3DSurface8* dest8,
                                     const POINT* points)
{
    return note(L"CopyRects", copy_rects(source8, rects, count, dest8, points), [&] {
        D3DSURFACE_DESC from{}, to{};
        IDirect3DSurface9* const source = unwrap<IDirect3DSurface9>(source8);
        IDirect3DSurface9* const dest = unwrap<IDirect3DSurface9>(dest8);
        if (source != nullptr)
            source->GetDesc(&from);
        if (dest != nullptr)
            dest->GetDesc(&to);
        wchar_t text[256];
        swprintf_s(text, L": %ux%u format %d pool %d usage %lX to %ux%u format %d pool %d usage %lX, %u rects%ls", from.Width, from.Height,
                   static_cast<int>(from.Format), static_cast<int>(from.Pool), static_cast<unsigned long>(from.Usage), to.Width, to.Height,
                   static_cast<int>(to.Format), static_cast<int>(to.Pool), static_cast<unsigned long>(to.Usage), rects != nullptr ? count : 1,
                   rects != nullptr && count > 0
                       ? (L", the first " + std::to_wstring(rects[0].left) + L"," + std::to_wstring(rects[0].top) + L"-" +
                          std::to_wstring(rects[0].right) + L"," + std::to_wstring(rects[0].bottom))
                             .c_str()
                       : L"");
        return std::wstring(text);
    });
}

HRESULT Device8::copy_rects(IDirect3DSurface8* source8, const RECT* rects, UINT count, IDirect3DSurface8* dest8, const POINT* points)
{
    IDirect3DSurface9* const source = unwrap<IDirect3DSurface9>(source8);
    IDirect3DSurface9* const dest = unwrap<IDirect3DSurface9>(dest8);
    if (source == nullptr || dest == nullptr || source == dest)
        return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC from{}, to{};
    if (FAILED(source->GetDesc(&from)) || FAILED(dest->GetDesc(&to)) || from.Format != to.Format)
        return D3DERR_INVALIDCALL;
    const RECT whole{0, 0, static_cast<LONG>(from.Width), static_cast<LONG>(from.Height)};
    if (rects == nullptr) {
        rects = &whole;
        count = 1;
    }
    for (UINT i = 0; i < count; ++i) {
        const POINT at = points != nullptr ? points[i] : POINT{rects[i].left, rects[i].top};
        const HRESULT result = copy_rect(source, from, rects[i], dest, to, at);
        if (FAILED(result))
            return result;
    }
    return D3D_OK;
}

HRESULT __stdcall Device8::UpdateTexture(IDirect3DBaseTexture8* source, IDirect3DBaseTexture8* dest)
{
    return device_->UpdateTexture(unwrap<IDirect3DBaseTexture9>(source), unwrap<IDirect3DBaseTexture9>(dest));
}

HRESULT __stdcall Device8::GetFrontBuffer(IDirect3DSurface8* dest)
{
    return device_->GetFrontBufferData(0, unwrap<IDirect3DSurface9>(dest));
}

HRESULT __stdcall Device8::SetRenderTarget(IDirect3DSurface8* target, IDirect3DSurface8* depth)
{
    if (target != nullptr) { // nullptr keeps the render target (the depth surface is still set)
        IDirect3DSurface9* const target9 = unwrap<IDirect3DSurface9>(target);
        if (target9 == nullptr)
            return D3DERR_INVALIDCALL;
        const HRESULT result = note(L"SetRenderTarget", device_->SetRenderTarget(0, target9));
        if (FAILED(result))
            return result;
    }
    IDirect3DSurface9* const depth9 = unwrap<IDirect3DSurface9>(depth);
    const HRESULT result = note(L"SetDepthStencilSurface", device_->SetDepthStencilSurface(depth9));
    if (SUCCEEDED(result))
        has_depth_ = depth9 != nullptr;
    return result;
}

HRESULT __stdcall Device8::GetRenderTarget(IDirect3DSurface8** out)
{
    if (out == nullptr)
        return D3DERR_INVALIDCALL;
    IDirect3DSurface9* surface = nullptr;
    const HRESULT result = device_->GetRenderTarget(0, &surface);
    *out = SUCCEEDED(result) ? wrap<Surface8>(surface) : nullptr;
    return result;
}

HRESULT __stdcall Device8::GetDepthStencilSurface(IDirect3DSurface8** out)
{
    if (out == nullptr)
        return D3DERR_INVALIDCALL;
    IDirect3DSurface9* surface = nullptr;
    const HRESULT result = device_->GetDepthStencilSurface(&surface);
    *out = SUCCEEDED(result) ? wrap<Surface8>(surface) : nullptr;
    return result;
}

HRESULT __stdcall Device8::BeginScene()
{
    return device_->BeginScene();
}

HRESULT __stdcall Device8::EndScene()
{
    return device_->EndScene();
}

HRESULT __stdcall Device8::Clear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
    if (!has_depth_) { // the color alone, where Direct3D 9 would clear nothing
        flags &= ~static_cast<DWORD>(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
        if (flags == 0)
            return D3D_OK;
    }
    return device_->Clear(count, rects, flags, color, z, stencil);
}

HRESULT __stdcall Device8::SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix)
{
    return device_->SetTransform(state, matrix);
}

HRESULT __stdcall Device8::GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX* matrix)
{
    return device_->GetTransform(state, matrix);
}

HRESULT __stdcall Device8::MultiplyTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix)
{
    return device_->MultiplyTransform(state, matrix);
}

HRESULT __stdcall Device8::SetViewport(const D3DVIEWPORT9* viewport)
{
    return device_->SetViewport(viewport);
}

HRESULT __stdcall Device8::GetViewport(D3DVIEWPORT9* viewport)
{
    return device_->GetViewport(viewport);
}

HRESULT __stdcall Device8::SetMaterial(const D3DMATERIAL9* material)
{
    return device_->SetMaterial(material);
}

HRESULT __stdcall Device8::GetMaterial(D3DMATERIAL9* material)
{
    return device_->GetMaterial(material);
}

HRESULT __stdcall Device8::SetLight(DWORD index, const D3DLIGHT9* light)
{
    return device_->SetLight(index, light);
}

HRESULT __stdcall Device8::GetLight(DWORD index, D3DLIGHT9* light)
{
    return device_->GetLight(index, light);
}

HRESULT __stdcall Device8::LightEnable(DWORD index, BOOL enable)
{
    return device_->LightEnable(index, enable);
}

HRESULT __stdcall Device8::GetLightEnable(DWORD index, BOOL* enable)
{
    return device_->GetLightEnable(index, enable);
}

HRESULT __stdcall Device8::SetClipPlane(DWORD index, const float* plane)
{
    return device_->SetClipPlane(index, plane);
}

HRESULT __stdcall Device8::GetClipPlane(DWORD index, float* plane)
{
    return device_->GetClipPlane(index, plane);
}

HRESULT __stdcall Device8::SetRenderState(D3DRENDERSTATETYPE state, DWORD value)
{
    switch (static_cast<DWORD>(state)) {
    case kRsZBias: // 0-16, toward the viewer; Direct3D 9's is a fraction of the depth range
        zbias_ = value;
        return device_->SetRenderState(D3DRS_DEPTHBIAS, float_bits(static_cast<float>(value) * -0.000005f));
    case kRsSoftwareVertexProcessing:
        return device_->SetSoftwareVertexProcessing(value != 0);
    case kRsLinePattern: line_pattern_ = value; return D3D_OK;
    case kRsZVisible: zvisible_ = value; return D3D_OK;
    case kRsEdgeAntialias: edge_antialias_ = value; return D3D_OK;
    case kRsPatchSegments: patch_segments_ = value; return D3D_OK;
    default: return device_->SetRenderState(state, value);
    }
}

HRESULT __stdcall Device8::GetRenderState(D3DRENDERSTATETYPE state, DWORD* value)
{
    if (value == nullptr)
        return D3DERR_INVALIDCALL;
    switch (static_cast<DWORD>(state)) {
    case kRsZBias: *value = zbias_; return D3D_OK;
    case kRsSoftwareVertexProcessing: *value = device_->GetSoftwareVertexProcessing(); return D3D_OK;
    case kRsLinePattern: *value = line_pattern_; return D3D_OK;
    case kRsZVisible: *value = zvisible_; return D3D_OK;
    case kRsEdgeAntialias: *value = edge_antialias_; return D3D_OK;
    case kRsPatchSegments: *value = patch_segments_; return D3D_OK;
    default: return device_->GetRenderState(state, value);
    }
}

HRESULT __stdcall Device8::BeginStateBlock()
{
    if (recording_ != nullptr)
        return D3DERR_INVALIDCALL;
    const HRESULT result = device_->BeginStateBlock();
    if (SUCCEEDED(result))
        recording_ = new StateBlock8{};
    return result;
}

HRESULT __stdcall Device8::EndStateBlock(DWORD* token)
{
    if (recording_ == nullptr || token == nullptr)
        return D3DERR_INVALIDCALL;
    std::unique_ptr<StateBlock8> block(recording_);
    recording_ = nullptr;
    const HRESULT result = device_->EndStateBlock(&block->block);
    if (FAILED(result))
        return result;
    *token = token_of(block.get());
    state_blocks_.insert(block.release());
    return result;
}

HRESULT __stdcall Device8::ApplyStateBlock(DWORD token)
{
    StateBlock8* const block = state_block(token);
    if (block == nullptr)
        return D3DERR_INVALIDCALL;
    if (block->block != nullptr)
        block->block->Apply();
    if (block->has_vs)
        current_.vs = block->values.vs;
    if (block->has_ps)
        current_.ps = block->values.ps;
    if (block->has_base)
        current_.base = block->values.base;
    return D3D_OK;
}

HRESULT __stdcall Device8::CaptureStateBlock(DWORD token)
{
    StateBlock8* const block = state_block(token);
    if (block == nullptr)
        return D3DERR_INVALIDCALL;
    if (block->block == nullptr && block->type != 0) // dropped at a Reset: made again
        device_->CreateStateBlock(block->type, &block->block);
    if (block->block != nullptr)
        block->block->Capture();
    if (block->has_vs)
        block->values.vs = current_.vs;
    if (block->has_ps)
        block->values.ps = current_.ps;
    if (block->has_base)
        block->values.base = current_.base;
    return D3D_OK;
}

HRESULT __stdcall Device8::DeleteStateBlock(DWORD token)
{
    StateBlock8* const block = state_block(token);
    if (block == nullptr)
        return D3DERR_INVALIDCALL;
    state_blocks_.erase(block);
    if (block->block != nullptr)
        block->block->Release();
    delete block;
    return D3D_OK;
}

HRESULT __stdcall Device8::CreateStateBlock(D3DSTATEBLOCKTYPE type, DWORD* token)
{
    if (token == nullptr)
        return D3DERR_INVALIDCALL;
    auto block = std::make_unique<StateBlock8>();
    const HRESULT result = device_->CreateStateBlock(type, &block->block);
    if (FAILED(result))
        return result;
    block->type = type;
    block->has_vs = type == D3DSBT_ALL || type == D3DSBT_VERTEXSTATE;
    block->has_ps = type == D3DSBT_ALL || type == D3DSBT_PIXELSTATE;
    block->has_base = type == D3DSBT_ALL;
    block->values = current_;
    *token = token_of(block.get());
    state_blocks_.insert(block.release());
    return result;
}

HRESULT __stdcall Device8::SetClipStatus(const D3DCLIPSTATUS9* status)
{
    return device_->SetClipStatus(status);
}

HRESULT __stdcall Device8::GetClipStatus(D3DCLIPSTATUS9* status)
{
    return device_->GetClipStatus(status);
}

HRESULT __stdcall Device8::GetTexture(DWORD stage, IDirect3DBaseTexture8** out)
{
    if (out == nullptr)
        return D3DERR_INVALIDCALL;
    IDirect3DBaseTexture9* texture = nullptr;
    const HRESULT result = device_->GetTexture(stage, &texture);
    *out = SUCCEEDED(result) ? wrap_texture(texture) : nullptr;
    return result;
}

HRESULT __stdcall Device8::SetTexture(DWORD stage, IDirect3DBaseTexture8* texture)
{
    IDirect3DBaseTexture9* const texture9 = unwrap<IDirect3DBaseTexture9>(texture);
    if (texture != nullptr && texture9 == nullptr && !logged_foreign_) {
        logged_foreign_ = true;
        log::write(L"Direct3D 9: SetTexture was given a texture this device didn't make; drawn without it");
    }
    return device_->SetTexture(stage, texture9);
}

// Direct3D 8's texture stage states that are sampler states in Direct3D 9: that state, else 0
DWORD sampler_state(DWORD type)
{
    if (type == kTssAddressW)
        return D3DSAMP_ADDRESSW;
    if (type < kTssAddressU || type > kTssMaxAnisotropy)
        return 0;
    // ADDRESSU, ADDRESSV, BORDERCOLOR, MAGFILTER, MINFILTER, MIPFILTER, MIPMAPLODBIAS, MAXMIPLEVEL, MAXANISOTROPY
    static constexpr DWORD kSamplers[] = {D3DSAMP_ADDRESSU,  D3DSAMP_ADDRESSV,      D3DSAMP_BORDERCOLOR,
                                          D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER,     D3DSAMP_MIPFILTER,
                                          D3DSAMP_MIPMAPLODBIAS, D3DSAMP_MAXMIPLEVEL, D3DSAMP_MAXANISOTROPY};
    return kSamplers[type - kTssAddressU];
}

HRESULT __stdcall Device8::GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD* value)
{
    if (const DWORD sampler = sampler_state(type); sampler != 0)
        return device_->GetSamplerState(stage, static_cast<D3DSAMPLERSTATETYPE>(sampler), value);
    return device_->GetTextureStageState(stage, type, value);
}

HRESULT __stdcall Device8::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
    if (const DWORD sampler = sampler_state(type); sampler != 0) {
        // Direct3D 8's cubic filters (4, 5) are Direct3D 9's pyramidal and gaussian quad ones (6, 7) at best
        if ((sampler == D3DSAMP_MAGFILTER || sampler == D3DSAMP_MINFILTER || sampler == D3DSAMP_MIPFILTER) && value > D3DTEXF_ANISOTROPIC)
            value = D3DTEXF_LINEAR;
        return device_->SetSamplerState(stage, static_cast<D3DSAMPLERSTATETYPE>(sampler), value);
    }
    return device_->SetTextureStageState(stage, type, value);
}

HRESULT __stdcall Device8::ValidateDevice(DWORD* passes)
{
    return device_->ValidateDevice(passes);
}

HRESULT __stdcall Device8::GetInfo(DWORD, void*, DWORD)
{
    return S_FALSE; // Direct3D 9 has queries instead
}

HRESULT __stdcall Device8::SetPaletteEntries(UINT palette, const PALETTEENTRY* entries)
{
    return device_->SetPaletteEntries(palette, entries);
}

HRESULT __stdcall Device8::GetPaletteEntries(UINT palette, PALETTEENTRY* entries)
{
    return device_->GetPaletteEntries(palette, entries);
}

HRESULT __stdcall Device8::SetCurrentTexturePalette(UINT palette)
{
    return device_->SetCurrentTexturePalette(palette);
}

HRESULT __stdcall Device8::GetCurrentTexturePalette(UINT* palette)
{
    return device_->GetCurrentTexturePalette(palette);
}

// A draw's state, for the first line of a refused one
std::wstring draw_detail(const Shadow& state, D3DPRIMITIVETYPE type, UINT count)
{
    wchar_t text[128];
    swprintf_s(text, L": primitive type %d, %u primitives, vertex shader %08lX, pixel shader %08lX, base vertex %u", static_cast<int>(type),
               count, static_cast<unsigned long>(state.vs), static_cast<unsigned long>(state.ps), state.base);
    return text;
}

HRESULT __stdcall Device8::DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count)
{
    return note(L"DrawPrimitive", device_->DrawPrimitive(type, start, count), [&] { return draw_detail(current_, type, count); });
}

HRESULT __stdcall Device8::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, UINT min_index, UINT vertices, UINT start, UINT count)
{
    return note(L"DrawIndexedPrimitive",
                device_->DrawIndexedPrimitive(type, static_cast<INT>(current_.base), min_index, vertices, start, count),
                [&] { return draw_detail(current_, type, count); });
}

HRESULT __stdcall Device8::DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride)
{
    return note(L"DrawPrimitiveUP", device_->DrawPrimitiveUP(type, count, data, stride),
                [&] { return draw_detail(current_, type, count); });
}

HRESULT __stdcall Device8::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT min_index, UINT vertices, UINT count, const void* indices,
                                                  D3DFORMAT format, const void* data, UINT stride)
{
    return note(L"DrawIndexedPrimitiveUP", device_->DrawIndexedPrimitiveUP(type, min_index, vertices, count, indices, format, data, stride),
                [&] { return draw_detail(current_, type, count); });
}

HRESULT __stdcall Device8::ProcessVertices(UINT source, UINT dest, UINT count, IDirect3DVertexBuffer8* buffer, DWORD flags)
{
    return device_->ProcessVertices(source, dest, count, unwrap<IDirect3DVertexBuffer9>(buffer), nullptr, flags);
}

HRESULT __stdcall Device8::CreateVertexShader(const DWORD* declaration, const DWORD* function, DWORD* handle, DWORD)
{
    if (declaration == nullptr || handle == nullptr)
        return D3DERR_INVALIDCALL;
    auto shader = std::make_unique<VertexShader8>();

    // The declaration: its elements, as a vertex declaration, and the constants it loads
    std::vector<D3DVERTEXELEMENT9> elements;
    UINT declared = 0; // input registers, as a mask
    WORD stream = 0, offset = 0;
    size_t i = 0;
    for (;;) {
        const DWORD token = declaration[i++];
        if (token == 0xFFFFFFFF) // D3DVSD_END
            break;
        switch (token >> 29) {
        case 1: // D3DVSD_STREAM (a tessellator's stream is left out)
            if ((token & 0x10000000) == 0)
                stream = static_cast<WORD>(token & 0xF), offset = 0;
            break;
        case 2: // D3DVSD_REG, or D3DVSD_SKIP (DWORDs)
            if ((token & 0x10000000) != 0) {
                offset = static_cast<WORD>(offset + 4 * ((token >> 16) & 0xF));
            } else {
                const UINT reg = token & 0x1F;
                const UINT type = (token >> 16) & 0xF;
                if (reg < kInputCount && type < std::size(kTypeBytes)) {
                    elements.push_back({stream, offset, static_cast<BYTE>(type), D3DDECLMETHOD_DEFAULT, kInputs[reg].usage,
                                        kInputs[reg].index});
                    declared |= 1u << reg;
                    offset = static_cast<WORD>(offset + kTypeBytes[type]);
                }
            }
            break;
        case 4: { // D3DVSD_CONST: count registers of 4 floats from reg, loaded at SetVertexShader
            const DWORD count = (token >> 25) & 0xF;
            Constants constants{token & 0x7F, std::vector<float>(4 * count)};
            std::memcpy(constants.values.data(), declaration + i, 16 * count);
            shader->constants.push_back(std::move(constants));
            i += 4 * count;
            break;
        }
        case 5: // D3DVSD_EXT, and its data
            i += (token >> 24) & 0x1F;
            break;
        default: // D3DVSD_NOP; D3DVSD_TESSNORMAL, D3DVSD_TESSUV (no patches here)
            break;
        }
    }
    shader->declaration.assign(declaration, declaration + i);
    elements.push_back(D3DDECL_END());
    HRESULT result = device_->CreateVertexDeclaration(elements.data(), &shader->declaration9);
    if (FAILED(result)) {
        log::write(L"Direct3D 9: a vertex declaration of %u elements was refused (0x%08lX)", static_cast<unsigned>(elements.size() - 1),
                   static_cast<unsigned long>(result));
        return result;
    }

    // The shader: Direct3D 8's vs.1.x code as vs_1_1
    if (function != nullptr) {
        const size_t length = code_length(function);
        shader->function.assign(function, function + length);
        UINT missing = 0;
        const std::vector<DWORD> code = to_vs_1_1(function, length, declared & 0xFFFF, spare_constant_, missing);
        if (missing != 0 && logged_missing_.insert(missing).second) {
            std::wstring inputs;
            for (UINT reg = 0; reg < 16; ++reg)
                if ((missing & (1u << reg)) != 0)
                    inputs += L" v" + std::to_wstring(reg);
            log::write(L"Direct3D 9: a vertex shader reads%ls, which its declaration doesn't give: read as 0, 0, 0, 1, as "
                       L"Direct3D 8 does",
                       inputs.c_str());
        }
        result = device_->CreateVertexShader(code.data(), &shader->shader9);
        if (FAILED(result)) {
            log::write(L"Direct3D 9: a vertex shader (%u tokens) was refused (0x%08lX); its Direct3D 8 code and Direct3D 9's:",
                       static_cast<unsigned>(length), static_cast<unsigned long>(result));
            const std::vector<DWORD>* const both[] = {&shader->function, &code};
            for (const std::vector<DWORD>* tokens : both) {
                for (size_t at = 0; at < tokens->size(); at += 16) {
                    wchar_t line[16 * 9 + 1] = {};
                    for (size_t k = at; k < tokens->size() && k < at + 16; ++k)
                        swprintf_s(line + (k - at) * 9, 10, L"%08lX ", static_cast<unsigned long>((*tokens)[k]));
                    log::write(L"  %ls", line);
                }
                log::write(L"  --");
            }
            shader->declaration9->Release();
            return result;
        }
    }
    *handle = token_of(shader.get()) | 1; // odd: not an FVF code
    vertex_shaders_.insert(shader.release());
    return D3D_OK;
}

HRESULT __stdcall Device8::SetVertexShader(DWORD handle)
{
    HRESULT result;
    if ((handle & 1) == 0) { // an FVF code: the fixed function
        result = device_->SetVertexShader(nullptr);
        if (SUCCEEDED(result))
            result = device_->SetFVF(handle);
    } else {
        VertexShader8* const shader = vertex_shader(handle);
        if (shader == nullptr)
            return D3DERR_INVALIDCALL;
        result = device_->SetVertexDeclaration(shader->declaration9);
        if (SUCCEEDED(result))
            result = device_->SetVertexShader(shader->shader9);
        for (const Constants& constants : shader->constants)
            device_->SetVertexShaderConstantF(constants.reg, constants.values.data(), static_cast<UINT>(constants.values.size() / 4));
    }
    if (SUCCEEDED(result))
        shadow(&StateBlock8::has_vs).vs = handle;
    return note(L"SetVertexShader", result);
}

HRESULT __stdcall Device8::GetVertexShader(DWORD* handle)
{
    if (handle == nullptr)
        return D3DERR_INVALIDCALL;
    *handle = current_.vs;
    return D3D_OK;
}

HRESULT __stdcall Device8::DeleteVertexShader(DWORD handle)
{
    VertexShader8* const shader = vertex_shader(handle);
    if (shader == nullptr)
        return D3DERR_INVALIDCALL;
    vertex_shaders_.erase(shader);
    if (shader->declaration9 != nullptr)
        shader->declaration9->Release();
    if (shader->shader9 != nullptr)
        shader->shader9->Release();
    delete shader;
    if (current_.vs == handle)
        current_.vs = 0;
    return D3D_OK;
}

HRESULT __stdcall Device8::SetVertexShaderConstant(DWORD reg, const void* data, DWORD count)
{
    return device_->SetVertexShaderConstantF(reg, static_cast<const float*>(data), count);
}

HRESULT __stdcall Device8::GetVertexShaderConstant(DWORD reg, void* data, DWORD count)
{
    return device_->GetVertexShaderConstantF(reg, static_cast<float*>(data), count);
}

// Tokens out, as Direct3D 8's Get*Declaration and Get*Function give them
HRESULT give_tokens(const std::vector<DWORD>& tokens, void* data, DWORD* size)
{
    if (size == nullptr)
        return D3DERR_INVALIDCALL;
    const DWORD bytes = static_cast<DWORD>(tokens.size() * sizeof(DWORD));
    if (data == nullptr) {
        *size = bytes;
        return D3D_OK;
    }
    if (*size < bytes) {
        *size = bytes;
        return kMoreData;
    }
    std::memcpy(data, tokens.data(), bytes);
    *size = bytes;
    return D3D_OK;
}

HRESULT __stdcall Device8::GetVertexShaderDeclaration(DWORD handle, void* data, DWORD* size)
{
    VertexShader8* const shader = vertex_shader(handle);
    return shader != nullptr ? give_tokens(shader->declaration, data, size) : D3DERR_INVALIDCALL;
}

HRESULT __stdcall Device8::GetVertexShaderFunction(DWORD handle, void* data, DWORD* size)
{
    VertexShader8* const shader = vertex_shader(handle);
    return shader != nullptr ? give_tokens(shader->function, data, size) : D3DERR_INVALIDCALL;
}

HRESULT __stdcall Device8::SetStreamSource(UINT stream, IDirect3DVertexBuffer8* buffer, UINT stride)
{
    return device_->SetStreamSource(stream, unwrap<IDirect3DVertexBuffer9>(buffer), 0, stride);
}

HRESULT __stdcall Device8::GetStreamSource(UINT stream, IDirect3DVertexBuffer8** out, UINT* stride)
{
    if (out == nullptr || stride == nullptr)
        return D3DERR_INVALIDCALL;
    IDirect3DVertexBuffer9* buffer = nullptr;
    UINT offset = 0;
    const HRESULT result = device_->GetStreamSource(stream, &buffer, &offset, stride);
    *out = SUCCEEDED(result) ? wrap<VertexBuffer8>(buffer) : nullptr;
    return result;
}

HRESULT __stdcall Device8::SetIndices(IDirect3DIndexBuffer8* buffer, UINT base_vertex)
{
    const HRESULT result = device_->SetIndices(unwrap<IDirect3DIndexBuffer9>(buffer));
    if (SUCCEEDED(result))
        shadow(&StateBlock8::has_base).base = base_vertex;
    return result;
}

HRESULT __stdcall Device8::GetIndices(IDirect3DIndexBuffer8** out, UINT* base_vertex)
{
    if (out == nullptr || base_vertex == nullptr)
        return D3DERR_INVALIDCALL;
    IDirect3DIndexBuffer9* buffer = nullptr;
    const HRESULT result = device_->GetIndices(&buffer);
    *out = SUCCEEDED(result) ? wrap<IndexBuffer8>(buffer) : nullptr;
    *base_vertex = current_.base;
    return result;
}

HRESULT __stdcall Device8::CreatePixelShader(const DWORD* function, DWORD* handle)
{
    if (function == nullptr || handle == nullptr)
        return D3DERR_INVALIDCALL;
    auto shader = std::make_unique<PixelShader8>();
    shader->function.assign(function, function + code_length(function));
    // ps.1.x is the same code in Direct3D 9
    const HRESULT result = device_->CreatePixelShader(function, &shader->shader9);
    if (FAILED(result))
        log::write(L"Direct3D 9: a pixel shader (version %lu.%lu, %u tokens) was refused (0x%08lX); drawn without it",
                   (function[0] >> 8) & 0xFF, function[0] & 0xFF, static_cast<unsigned>(shader->function.size()),
                   static_cast<unsigned long>(result));
    *handle = token_of(shader.get());
    pixel_shaders_.insert(shader.release());
    return D3D_OK;
}

HRESULT __stdcall Device8::SetPixelShader(DWORD handle)
{
    IDirect3DPixelShader9* shader9 = nullptr;
    if (handle != 0) {
        PixelShader8* const shader = pixel_shader(handle);
        if (shader == nullptr)
            return D3DERR_INVALIDCALL;
        shader9 = shader->shader9;
    }
    const HRESULT result = device_->SetPixelShader(shader9);
    if (SUCCEEDED(result))
        shadow(&StateBlock8::has_ps).ps = handle;
    return result;
}

HRESULT __stdcall Device8::GetPixelShader(DWORD* handle)
{
    if (handle == nullptr)
        return D3DERR_INVALIDCALL;
    *handle = current_.ps;
    return D3D_OK;
}

HRESULT __stdcall Device8::DeletePixelShader(DWORD handle)
{
    PixelShader8* const shader = pixel_shader(handle);
    if (shader == nullptr)
        return D3DERR_INVALIDCALL;
    pixel_shaders_.erase(shader);
    if (shader->shader9 != nullptr)
        shader->shader9->Release();
    delete shader;
    if (current_.ps == handle)
        current_.ps = 0;
    return D3D_OK;
}

HRESULT __stdcall Device8::SetPixelShaderConstant(DWORD reg, const void* data, DWORD count)
{
    return device_->SetPixelShaderConstantF(reg, static_cast<const float*>(data), count);
}

HRESULT __stdcall Device8::GetPixelShaderConstant(DWORD reg, void* data, DWORD count)
{
    return device_->GetPixelShaderConstantF(reg, static_cast<float*>(data), count);
}

HRESULT __stdcall Device8::GetPixelShaderFunction(DWORD handle, void* data, DWORD* size)
{
    PixelShader8* const shader = pixel_shader(handle);
    return shader != nullptr ? give_tokens(shader->function, data, size) : D3DERR_INVALIDCALL;
}

HRESULT __stdcall Device8::DrawRectPatch(UINT handle, const float* segments, const D3DRECTPATCH_INFO* info)
{
    return device_->DrawRectPatch(handle, segments, info);
}

HRESULT __stdcall Device8::DrawTriPatch(UINT handle, const float* segments, const D3DTRIPATCH_INFO* info)
{
    return device_->DrawTriPatch(handle, segments, info);
}

HRESULT __stdcall Device8::DeletePatch(UINT handle)
{
    return device_->DeletePatch(handle);
}

} // namespace

void* direct3d_create(bool vsync)
{
    g_vsync = vsync;
    const HMODULE library = LoadLibraryW(L"d3d9.dll"); // the game's folder first: ReShade's, DXVK's
    if (library == nullptr) {
        log::write(L"Direct3D 9: d3d9.dll didn't load (error %lu)", GetLastError());
        return nullptr;
    }
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(library, path, MAX_PATH);
    g_library = path;
    using Create = IDirect3D9*(WINAPI*)(UINT sdk_version);
    const auto create = reinterpret_cast<Create>(GetProcAddress(library, "Direct3DCreate9"));
    IDirect3D9* const d3d = create != nullptr ? create(D3D_SDK_VERSION) : nullptr;
    if (d3d == nullptr) {
        log::write(L"Direct3D 9: %ls didn't give a Direct3D 9", path);
        return nullptr;
    }
    D3DADAPTER_IDENTIFIER9 identifier{};
    if (SUCCEEDED(d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &identifier)))
        g_adapter = identifier.Description;
    log::write(L"Direct3D 9: %ls, on %hs", path, g_adapter.c_str());
    return static_cast<IDirect3D8*>(new Direct3D8(d3d));
}

void set_vsync(bool on)
{
    if (on != g_vsync)
        log::write(L"Direct3D 9: vsync %ls from the next start", on ? L"on" : L"off");
    g_vsync = on;
}

std::string adapter()
{
    return g_adapter;
}

std::wstring library()
{
    return g_library;
}

} // namespace vanacore::d3d9
