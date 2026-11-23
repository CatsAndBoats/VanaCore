/* The recompiler runtime's services, for d3d8.c and gfx_d3d12.c inside a native 32-bit game process
 * (VanaCore.Core), and the library's face to VanaCore (vanacore_gfx.h).
 *
 *   - COM methods: thunk_for gives each "IDirect3DDevice8::Clear" (and the rest) a 10-byte stub,
 *     `mov eax, <index>; jmp dispatch`, which hands the shim a Guest holding the real stack pointer.
 *     The shim reads its arguments off the stack and pops them (RET), as the upstream shims do for
 *     the recompiled game, so d3d8.c runs unchanged.
 *   - the guest heap is this process's heap; guest addresses are pointers (runtime.h);
 *   - the window calls (user32.h) act on the game's real window;
 *   - diagnostics go to VanaCore's log. */
#define VC_COMPAT_NO_FPRINTF
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../d3d8.h"
#include "../floors.h"
#include "../gfx.h"
#include "../gfx_backend.h"
#include "../gfx_shared.h"
#include "../snow.h"
#include "../sky.h"
#include "../vanacore_gfx.h"
#include "../zonemesh.h"
#include "gwin.h"
#include "plat.h"
#include "thunk.h"
#include "user32.h"
#include "vc_compat.h"

/* --- the log ------------------------------------------------------------------------------------ */
static void (*g_sink)(const char* line);

void vanacore_gfx_set_log(void (*sink)(const char* line)) { g_sink = sink; }

/* Each line of text to the sink, without upstream's "[recomp] " prefix. */
static void emit(char* text)
{
    for (char* line = text; line && *line;)
    {
        char* next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        if (*line)
        {
            if (!strncmp(line, "[recomp] ", 9))
                line += 9;
            if (g_sink)
                g_sink(line);
            else
                OutputDebugStringA(line), OutputDebugStringA("\n");
        }
        line = next;
    }
}

static void vlog(const char* fmt, va_list ap)
{
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, ap);
    buf[sizeof buf - 1] = 0;
    emit(buf);
}

void rt_log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

int vc_fprintf(FILE* f, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n;
    if (f == stderr || f == stdout)
        vlog(fmt, ap), n = 0;
    else
        n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

uint32_t rt_image_lo, rt_image_hi;

void vanacore_gfx_set_image(uint32_t lo, uint32_t hi) { rt_image_lo = lo, rt_image_hi = hi; }

/* --- the heap ------------------------------------------------------------------------------------ */
uint32_t gheap_alloc(uint32_t size, int zero)
{
    void* p = _aligned_malloc(size ? size : 1, 16);
    if (p && zero)
        memset(p, 0, size ? size : 1);
    return (uint32_t)(uintptr_t)p;
}

void gheap_free(uint32_t p)
{
    if (p)
        _aligned_free((void*)(uintptr_t)p);
}

/* --- files ---------------------------------------------------------------------------------------- */
const char plat_path_sep = '\\';

unsigned char* plat_read_file(const char* path, size_t* size)
{
    *size = 0;
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* p = n >= 0 ? (unsigned char*)malloc((size_t)n + 1) : NULL;
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n)
        free(p), p = NULL;
    fclose(f);
    if (p)
        *size = (size_t)n;
    return p;
}

int plat_mkdir(const char* path)
{
    return CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
}

struct PlatDir
{
    HANDLE h;
    WIN32_FIND_DATAA fd;
    int first;
};

PlatDir* plat_dir_open(const char* path)
{
    char pattern[MAX_PATH + 4];
    snprintf(pattern, sizeof pattern, "%s\\*", path);
    PlatDir* d = (PlatDir*)calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->h = FindFirstFileA(pattern, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE)
    {
        free(d);
        return NULL;
    }
    d->first = 1;
    return d;
}

const char* plat_dir_next(PlatDir* d)
{
    for (;;)
    {
        if (!d->first && !FindNextFileA(d->h, &d->fd))
            return NULL;
        d->first = 0;
        if (!(d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            return d->fd.cFileName;
    }
}

void plat_dir_close(PlatDir* d)
{
    if (!d)
        return;
    FindClose(d->h);
    free(d);
}

/* --- COM methods ------------------------------------------------------------------------------------- */
/* Arguments of every Direct3D 8 method (with `this`), by the D3D8 ABI: what a method without a shim
 * pops. */
static const struct
{
    const char* name;
    uint8_t nargs;
} ARGS_TABLE[] = {
    { "QueryInterface", 3 }, { "AddRef", 1 }, { "Release", 1 },
    /* IDirect3D8 */
    { "IDirect3D8::RegisterSoftwareDevice", 2 }, { "IDirect3D8::GetAdapterCount", 1 },
    { "IDirect3D8::GetAdapterIdentifier", 4 }, { "IDirect3D8::GetAdapterModeCount", 2 },
    { "IDirect3D8::EnumAdapterModes", 4 }, { "IDirect3D8::GetAdapterDisplayMode", 3 },
    { "IDirect3D8::CheckDeviceType", 6 }, { "IDirect3D8::CheckDeviceFormat", 7 },
    { "IDirect3D8::CheckDeviceMultiSampleType", 6 }, { "IDirect3D8::CheckDepthStencilMatch", 6 },
    { "IDirect3D8::GetDeviceCaps", 4 }, { "IDirect3D8::GetAdapterMonitor", 2 }, { "IDirect3D8::CreateDevice", 7 },
    /* IDirect3DDevice8 */
    { "IDirect3DDevice8::TestCooperativeLevel", 1 }, { "IDirect3DDevice8::GetAvailableTextureMem", 1 },
    { "IDirect3DDevice8::ResourceManagerDiscardBytes", 2 }, { "IDirect3DDevice8::GetDirect3D", 2 },
    { "IDirect3DDevice8::GetDeviceCaps", 2 }, { "IDirect3DDevice8::GetDisplayMode", 2 },
    { "IDirect3DDevice8::GetCreationParameters", 2 }, { "IDirect3DDevice8::SetCursorProperties", 4 },
    { "IDirect3DDevice8::SetCursorPosition", 4 }, { "IDirect3DDevice8::ShowCursor", 2 },
    { "IDirect3DDevice8::CreateAdditionalSwapChain", 3 }, { "IDirect3DDevice8::Reset", 2 },
    { "IDirect3DDevice8::Present", 5 }, { "IDirect3DDevice8::GetBackBuffer", 4 },
    { "IDirect3DDevice8::GetRasterStatus", 2 }, { "IDirect3DDevice8::SetGammaRamp", 3 },
    { "IDirect3DDevice8::GetGammaRamp", 2 }, { "IDirect3DDevice8::CreateTexture", 8 },
    { "IDirect3DDevice8::CreateVolumeTexture", 9 }, { "IDirect3DDevice8::CreateCubeTexture", 7 },
    { "IDirect3DDevice8::CreateVertexBuffer", 6 }, { "IDirect3DDevice8::CreateIndexBuffer", 6 },
    { "IDirect3DDevice8::CreateRenderTarget", 7 }, { "IDirect3DDevice8::CreateDepthStencilSurface", 6 },
    { "IDirect3DDevice8::CreateImageSurface", 5 }, { "IDirect3DDevice8::CopyRects", 6 },
    { "IDirect3DDevice8::UpdateTexture", 3 }, { "IDirect3DDevice8::GetFrontBuffer", 2 },
    { "IDirect3DDevice8::SetRenderTarget", 3 }, { "IDirect3DDevice8::GetRenderTarget", 2 },
    { "IDirect3DDevice8::GetDepthStencilSurface", 2 }, { "IDirect3DDevice8::BeginScene", 1 },
    { "IDirect3DDevice8::EndScene", 1 }, { "IDirect3DDevice8::Clear", 7 }, { "IDirect3DDevice8::SetTransform", 3 },
    { "IDirect3DDevice8::GetTransform", 3 }, { "IDirect3DDevice8::MultiplyTransform", 3 },
    { "IDirect3DDevice8::SetViewport", 2 }, { "IDirect3DDevice8::GetViewport", 2 },
    { "IDirect3DDevice8::SetMaterial", 2 }, { "IDirect3DDevice8::GetMaterial", 2 },
    { "IDirect3DDevice8::SetLight", 3 }, { "IDirect3DDevice8::GetLight", 3 },
    { "IDirect3DDevice8::LightEnable", 3 }, { "IDirect3DDevice8::GetLightEnable", 3 },
    { "IDirect3DDevice8::SetClipPlane", 3 }, { "IDirect3DDevice8::GetClipPlane", 3 },
    { "IDirect3DDevice8::SetRenderState", 3 }, { "IDirect3DDevice8::GetRenderState", 3 },
    { "IDirect3DDevice8::BeginStateBlock", 1 }, { "IDirect3DDevice8::EndStateBlock", 2 },
    { "IDirect3DDevice8::ApplyStateBlock", 2 }, { "IDirect3DDevice8::CaptureStateBlock", 2 },
    { "IDirect3DDevice8::DeleteStateBlock", 2 }, { "IDirect3DDevice8::CreateStateBlock", 3 },
    { "IDirect3DDevice8::SetClipStatus", 2 }, { "IDirect3DDevice8::GetClipStatus", 2 },
    { "IDirect3DDevice8::GetTexture", 3 }, { "IDirect3DDevice8::SetTexture", 3 },
    { "IDirect3DDevice8::GetTextureStageState", 4 }, { "IDirect3DDevice8::SetTextureStageState", 4 },
    { "IDirect3DDevice8::ValidateDevice", 2 }, { "IDirect3DDevice8::GetInfo", 4 },
    { "IDirect3DDevice8::SetPaletteEntries", 3 }, { "IDirect3DDevice8::GetPaletteEntries", 3 },
    { "IDirect3DDevice8::SetCurrentTexturePalette", 2 }, { "IDirect3DDevice8::GetCurrentTexturePalette", 2 },
    { "IDirect3DDevice8::DrawPrimitive", 4 }, { "IDirect3DDevice8::DrawIndexedPrimitive", 6 },
    { "IDirect3DDevice8::DrawPrimitiveUP", 5 }, { "IDirect3DDevice8::DrawIndexedPrimitiveUP", 9 },
    { "IDirect3DDevice8::ProcessVertices", 6 }, { "IDirect3DDevice8::CreateVertexShader", 5 },
    { "IDirect3DDevice8::SetVertexShader", 2 }, { "IDirect3DDevice8::GetVertexShader", 2 },
    { "IDirect3DDevice8::DeleteVertexShader", 2 }, { "IDirect3DDevice8::SetVertexShaderConstant", 4 },
    { "IDirect3DDevice8::GetVertexShaderConstant", 4 }, { "IDirect3DDevice8::GetVertexShaderDeclaration", 4 },
    { "IDirect3DDevice8::GetVertexShaderFunction", 4 }, { "IDirect3DDevice8::SetStreamSource", 4 },
    { "IDirect3DDevice8::GetStreamSource", 4 }, { "IDirect3DDevice8::SetIndices", 3 },
    { "IDirect3DDevice8::GetIndices", 3 }, { "IDirect3DDevice8::CreatePixelShader", 3 },
    { "IDirect3DDevice8::SetPixelShader", 2 }, { "IDirect3DDevice8::GetPixelShader", 2 },
    { "IDirect3DDevice8::DeletePixelShader", 2 }, { "IDirect3DDevice8::SetPixelShaderConstant", 4 },
    { "IDirect3DDevice8::GetPixelShaderConstant", 4 }, { "IDirect3DDevice8::GetPixelShaderFunction", 4 },
    { "IDirect3DDevice8::DrawRectPatch", 4 }, { "IDirect3DDevice8::DrawTriPatch", 4 },
    { "IDirect3DDevice8::DeletePatch", 2 },
    /* IDirect3DResource8, and the surfaces' share of it */
    { "GetDevice", 2 }, { "SetPrivateData", 5 }, { "GetPrivateData", 4 }, { "FreePrivateData", 2 },
    { "SetPriority", 2 }, { "GetPriority", 1 }, { "PreLoad", 1 }, { "GetType", 1 },
    /* textures */
    { "SetLOD", 2 }, { "GetLOD", 1 }, { "GetLevelCount", 1 },
    { "IDirect3DTexture8::GetLevelDesc", 3 }, { "IDirect3DTexture8::GetSurfaceLevel", 3 },
    { "IDirect3DTexture8::LockRect", 5 }, { "IDirect3DTexture8::UnlockRect", 2 },
    { "IDirect3DTexture8::AddDirtyRect", 2 }, { "IDirect3DCubeTexture8::GetLevelDesc", 3 },
    { "IDirect3DCubeTexture8::GetCubeMapSurface", 4 }, { "IDirect3DCubeTexture8::LockRect", 6 },
    { "IDirect3DCubeTexture8::UnlockRect", 3 }, { "IDirect3DCubeTexture8::AddDirtyRect", 3 },
    /* buffers */
    { "Lock", 5 }, { "Unlock", 1 }, { "GetDesc", 2 },
    /* surfaces */
    { "IDirect3DSurface8::GetContainer", 3 }, { "IDirect3DSurface8::LockRect", 4 },
    { "IDirect3DSurface8::UnlockRect", 1 },
};

/* the arguments a method takes: its full name, else the name after "::" (methods shared by name) */
static int method_args(const char* name)
{
    const char* bare = strstr(name, "::");
    bare = bare ? bare + 2 : name;
    for (size_t i = 0; i < sizeof ARGS_TABLE / sizeof ARGS_TABLE[0]; ++i)
        if (!strcmp(ARGS_TABLE[i].name, name))
            return ARGS_TABLE[i].nargs;
    for (size_t i = 0; i < sizeof ARGS_TABLE / sizeof ARGS_TABLE[0]; ++i)
        if (!strcmp(ARGS_TABLE[i].name, bare))
            return ARGS_TABLE[i].nargs;
    return -1;
}

typedef struct Registered
{
    const char* dll;
    const char* name;
    Shim fn;
} Registered;

static Registered* g_shims;
static uint32_t g_nshims;

void thunk_register(const ShimDef* defs)
{
    for (; defs->name; ++defs)
    {
        g_shims = (Registered*)realloc(g_shims, (g_nshims + 1) * sizeof *g_shims);
        g_shims[g_nshims].dll = defs->dll, g_shims[g_nshims].name = defs->name, g_shims[g_nshims].fn = defs->fn;
        g_nshims++;
    }
}

typedef struct Stub
{
    char name[96];
    Shim fn;       /* NULL: the fallback */
    int nargs;     /* what the fallback pops */
    int told;      /* the fallback was logged */
} Stub;

#define STUB_BYTES 10
#define MAX_STUBS 1024
static Stub g_stubs[MAX_STUBS];
static uint32_t g_nstubs;
static uint8_t* g_code; /* MAX_STUBS * STUB_BYTES of executable memory */

#define D3DERR_INVALIDCALL 0x8876086Cu

/* A method with no shim: D3DERR_INVALIDCALL, its arguments popped, its name logged once. */
static void fallback(Guest* g, Stub* s)
{
    if (!s->told)
    {
        s->told = 1;
        rt_log("Direct3D 8: %s isn't implemented; it returns D3DERR_INVALIDCALL\n", s->name);
    }
    RET(D3DERR_INVALIDCALL, (uint32_t)s->nargs);
}

/* Upstream's shims never run two at once: the recompiled game runs one guest thread at a time. The
 * real d3d8 serializes its calls only for a device made with D3DCREATE_MULTITHREADED (4); so do we,
 * from that CreateDevice on. */
static CRITICAL_SECTION g_lock;
static volatile LONG g_multithreaded;
static uint32_t g_create_device = ~0u; /* IDirect3D8::CreateDevice's stub */

/* Called from the stubs through dispatch(): edx:eax = the stack pointer to return with, the result */
static unsigned __int64 __cdecl native_dispatch(uint32_t index, uint32_t esp)
{
    Guest g;
    memset(&g, 0, sizeof g);
    g.esp = esp;
    Stub* s = &g_stubs[index];
    if (index == g_create_device && (rd32(esp + 4u + 4u * 4u) & 4u) && !g_multithreaded)
    {
        rt_log("Direct3D 8: the device is multithreaded; its calls are serialized\n");
        InterlockedExchange(&g_multithreaded, 1);
    }
    int locked = g_multithreaded != 0;
    if (locked)
        EnterCriticalSection(&g_lock);
    if (s->fn)
        s->fn(&g);
    else
        fallback(&g, s);
    if (locked)
        LeaveCriticalSection(&g_lock);
    return ((unsigned __int64)g.esp << 32) | g.eax;
}

/* eax = the stub's index; [esp] = the caller's return address, its arguments above */
static __declspec(naked) void dispatch(void)
{
    __asm {
        mov ecx, esp
        push ecx
        push eax
        call native_dispatch
        add esp, 8
        mov ecx, [esp]
        mov esp, edx
        jmp ecx
    }
}

uint32_t thunk_for(const char* dll, const char* name)
{
    (void)dll;
    for (uint32_t i = 0; i < g_nstubs; ++i)
        if (!strcmp(g_stubs[i].name, name))
            return (uint32_t)(uintptr_t)(g_code + STUB_BYTES * i);
    if (!g_code)
    {
        g_code = (uint8_t*)VirtualAlloc(NULL, MAX_STUBS * STUB_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        InitializeCriticalSection(&g_lock);
    }
    if (!g_code || g_nstubs == MAX_STUBS)
    {
        rt_log("Direct3D 8: no room for the method stub of %s\n", name);
        return 0;
    }
    uint32_t i = g_nstubs++;
    Stub* s = &g_stubs[i];
    snprintf(s->name, sizeof s->name, "%s", name);
    if (!strcmp(name, "IDirect3D8::CreateDevice"))
        g_create_device = i;
    for (uint32_t k = 0; k < g_nshims; ++k)
        if (!strcmp(g_shims[k].name, name))
            s->fn = g_shims[k].fn;
    s->nargs = method_args(name);
    if (!s->fn && s->nargs < 0)
    {
        rt_log("Direct3D 8: %s has neither a shim nor a known signature\n", name);
        s->nargs = 1;
    }
    DWORD old;
    VirtualProtect(g_code, MAX_STUBS * STUB_BYTES, PAGE_READWRITE, &old);
    uint8_t* p = g_code + STUB_BYTES * i;
    p[0] = 0xB8; /* mov eax, i */
    memcpy(p + 1, &i, 4);
    p[5] = 0xE9; /* jmp dispatch */
    int32_t rel = (int32_t)((uintptr_t)&dispatch - (uintptr_t)(p + 10));
    memcpy(p + 6, &rel, 4);
    VirtualProtect(g_code, MAX_STUBS * STUB_BYTES, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), p, STUB_BYTES);
    return (uint32_t)(uintptr_t)p;
}

/* --- the window ---------------------------------------------------------------------------------------- */
static float g_ui_aspect;
static uint32_t g_made_w, g_made_h; /* user32_set_window_size: the size the window was made at, 0 none */
int (*user32_ui_hit)(float fx, float fy);
static int g_mouse_raw = 1;
static float g_mouse_fx, g_mouse_fy;

static int client(uint32_t hwnd, int* w, int* h)
{
    RECT r;
    HWND wnd = (HWND)(uintptr_t)hwnd;
    if (!wnd || !IsWindow(wnd) || !GetClientRect(wnd, &r) || r.right <= 0 || r.bottom <= 0)
        return 0;
    *w = r.right, *h = r.bottom;
    return 1;
}

void user32_set_fullscreen(uint32_t hwnd, int on)
{
    HWND wnd = (HWND)(uintptr_t)hwnd;
    if (!on || !wnd || !IsWindow(wnd))
        return;
    MONITORINFO mi = { sizeof mi };
    if (!GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY), &mi))
        return;
    const RECT* m = &mi.rcMonitor;
    SetWindowLongW(wnd, GWL_STYLE, (GetWindowLongW(wnd, GWL_STYLE) & ~(WS_OVERLAPPEDWINDOW)) | WS_POPUP);
    SetWindowPos(wnd, HWND_TOP, m->left, m->top, m->right - m->left, m->bottom - m->top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    rt_log("Direct3D 8: full screen: the window covers its monitor (%ldx%ld), borderless\n", m->right - m->left,
        m->bottom - m->top);
}

void* user32_sdl_window(uint32_t hwnd) { return (void*)(uintptr_t)hwnd; }

void user32_client_size(uint32_t hwnd, uint32_t* w, uint32_t* h)
{
    int cw, ch;
    if (client(hwnd, &cw, &ch))
        *w = (uint32_t)cw, *h = (uint32_t)ch;
}

void user32_client_origin(uint32_t hwnd, int32_t* x, int32_t* y)
{
    POINT p = { 0, 0 };
    HWND wnd = (HWND)(uintptr_t)hwnd;
    if (wnd && IsWindow(wnd))
        ClientToScreen(wnd, &p);
    *x = p.x, *y = p.y;
}

void user32_desktop_mode(uint32_t* w, uint32_t* h, uint32_t* hz)
{
    DEVMODEW dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm))
        *w = dm.dmPelsWidth, *h = dm.dmPelsHeight, *hz = dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 60;
    else
        *w = (uint32_t)GetSystemMetrics(SM_CXSCREEN), *h = (uint32_t)GetSystemMetrics(SM_CYSCREEN), *hz = 60;
}

void user32_set_ui_aspect(float aspect) { g_ui_aspect = aspect; }

void user32_set_window_size(uint32_t w, uint32_t h) { g_made_w = w, g_made_h = h; }

/* The size the interface is fitted to: the window's as it was made (what Windows took of it for the title bar and
 * borders left out), else its client area's */
static int window_size(uint32_t hwnd, int* w, int* h)
{
    if (g_made_w && g_made_h)
    {
        *w = (int)g_made_w, *h = (int)g_made_h;
        return 1;
    }
    return client(hwnd, w, h);
}

float user32_ui_squeeze(uint32_t hwnd)
{
    int w, h;
    if (!(g_ui_aspect > 0) || !window_size(hwnd, &w, &h))
        return 1.0f;
    float s = g_ui_aspect * (float)h / (float)w;
    return s < 1.0f ? s : 1.0f;
}

/* A window taller than the interface's shape (16:10 for a 16:9 interface): the interface's height
 * squeezed toward the middle by this much (1: not) */
float user32_ui_squeeze_y(uint32_t hwnd)
{
    int w, h;
    if (!(g_ui_aspect > 0) || !window_size(hwnd, &w, &h) || h <= 0)
        return 1.0f;
    float s = (float)w / (g_ui_aspect * (float)h);
    return s < 1.0f ? s : 1.0f;
}

int user32_mouse_raw(void) { return g_mouse_raw; }

void user32_mouse_given(float* fx, float* fy) { *fx = g_mouse_fx, *fy = g_mouse_fy; }

/* As upstream's ui_unsqueeze_x (user32.c): over the interface (where d3d8.c drew it last frame) the
 * cursor goes to the game as the x the game drew there before its draws were squeezed toward the
 * middle, so its hit tests line up; over the world it goes as it is, so picking and the target
 * arrow do. A press keeps its mode until the buttons are up. */
void user32_unsqueeze(uint32_t hwnd, int* x, int* y, int held)
{
    int w, h;
    float s = user32_ui_squeeze(hwnd), t = user32_ui_squeeze_y(hwnd);
    if ((s >= 1.0f && t >= 1.0f) || !client(hwnd, &w, &h) || w <= 0 || h <= 0)
    {
        g_mouse_raw = 1;
        return;
    }
    if (!held)
        g_mouse_raw = !(user32_ui_hit && user32_ui_hit(((float)*x + 0.5f) / (float)w, ((float)*y + 0.5f) / (float)h));
    if (!g_mouse_raw && s < 1.0f)
    {
        float c = (float)w * 0.5f, gx = c + ((float)*x - c) / s;
        *x = gx < 0 ? 0 : gx > (float)(w - 1) ? w - 1 : (int)(gx + 0.5f);
    }
    if (!g_mouse_raw && t < 1.0f)
    {
        float c = (float)h * 0.5f, gy = c + ((float)*y - c) / t;
        *y = gy < 0 ? 0 : gy > (float)(h - 1) ? h - 1 : (int)(gy + 0.5f);
    }
    g_mouse_fx = ((float)*x + 0.5f) / (float)w;
    g_mouse_fy = ((float)*y + 0.5f) / (float)h;
}

int user32_unsqueeze_x(uint32_t hwnd, int x, int y, int held)
{
    user32_unsqueeze(hwnd, &x, &y, held);
    return x;
}

/* --- VanaCore's calls ------------------------------------------------------------------------------------ */
static int g_started, g_start_ok, g_vtables;

void vanacore_gfx_set_d3d11(int on)
{
    if (!g_started)
        gfx_select(on);
}

int vanacore_gfx_start(void)
{
    if (!g_started)
    {
        g_started = 1;
        /* Direct3D 12 only exists on Windows 10 and later: without it the game keeps the real d3d8
         * (d3d11.dll, d3d12.dll and d3dcompiler_47.dll are delay-loaded, so this DLL loads without them) */
        const int d3d11 = gfx_selected_d3d11();
        if (!LoadLibraryW(d3d11 ? L"d3d11.dll" : L"d3d12.dll") || !LoadLibraryW(L"dxgi.dll") || !LoadLibraryW(L"d3dcompiler_47.dll"))
            rt_log(d3d11 ? "Direct3D 11 isn't on this machine (d3d11.dll, dxgi.dll, d3dcompiler_47.dll)\n"
                         : "Direct3D 12 isn't on this machine (d3d12.dll, dxgi.dll, d3dcompiler_47.dll)\n");
        else if (!(g_start_ok = gfx_init(NULL, 1)) && !d3d11 && (gfx12_shaders_failed() || gfx12_device_refused()) &&
            LoadLibraryW(L"d3d11.dll"))
        {
            /* Direct3D 12 came up but its shaders don't compile here (Wine, Proton), or its device can't take the back
             * end (below resource binding tier 2: Wine's vkd3d on a Mac under CrossOver): Direct3D 11 needs neither, so
             * it draws the game in its place, as with renderer=d3d11, rather than a black window or the game's own
             * Direct3D 8 */
            rt_log(gfx12_shaders_failed() ? "Direct3D 12 can't draw here (its shaders don't compile); Direct3D 11 in its place\n"
                                          : "Direct3D 12 can't draw here (no device it can use); Direct3D 11 in its place\n");
            gfx_select(1);
            g_start_ok = gfx_init(NULL, 1);
        }
    }
    return g_start_ok;
}

int vanacore_gfx_d3d11(void) { return gfx_selected_d3d11(); }

void* vanacore_gfx_direct3d_create(void)
{
    if (!vanacore_gfx_start())
        return NULL;
    if (!g_vtables)
    {
        d3d8_init();
        d3d8_setup();
        g_vtables = 1;
    }
    return (void*)(uintptr_t)d3d8_create();
}

void vanacore_gfx_set_vsync(int on) { gfx_set_vsync(on); }

void vanacore_gfx_fx_set(const char* key, float value) { gfx_fx_set(key, value); }
int vanacore_gfx_fx_rebuild(void)
{
    if (!g_start_ok)
        return 0;
    gfx_fx_rebuild();
    return 1;
}

void vanacore_gfx_set_ui_aspect(float aspect) { user32_set_ui_aspect(aspect); }

void vanacore_gfx_set_window_size(uint32_t w, uint32_t h) { user32_set_window_size(w, h); }

int vanacore_gfx_unsqueeze_x(void* window, int x, int y, int held)
{
    return user32_unsqueeze_x((uint32_t)(uintptr_t)window, x, y, held);
}

void vanacore_gfx_unsqueeze(void* window, int* x, int* y, int held)
{
    user32_unsqueeze((uint32_t)(uintptr_t)window, x, y, held);
}

void vanacore_gfx_texture_pack(const char* folder) { d3d8_texture_pack(folder); }

void vanacore_gfx_screen_size(uint32_t* w, uint32_t* h) { d3d8_screen_size(w, h); }
int vanacore_gfx_scene_shown(uint32_t* w, uint32_t* h) { return d3d8_scene_shown(w, h); }
int vanacore_gfx_camera(float* view, float* proj, uint32_t* viewport) { return d3d8_camera(view, proj, viewport); }
void vanacore_gfx_label(uint32_t id) { d3d8_set_label(id); }

void vanacore_gfx_set_squeeze(int on) { d3d8_set_squeeze(on); }
void vanacore_gfx_set_focus(float x, float y, float z, int known) { d3d8_set_focus(x, y, z, known); }
int vanacore_gfx_sun_record(const char* path, unsigned max_mb) { return g_start_ok ? gfx_sun_record(path, max_mb) : 0; }
float vanacore_gfx_seek_reach(float* stretch)
{
    if (g_start_ok)
        return gfx_seek_reach(stretch);
    if (stretch)
        *stretch = 0.0f;
    return 0.0f;
}
void vanacore_gfx_seek(uint32_t piece) { d3d8_set_seek(piece); }
void vanacore_gfx_batch(int open) { gfx_batch(open); }
int vanacore_gfx_batch_wanted(void) { return g_gfx_batch && !gfx_selected_d3d11(); }
void vanacore_gfx_occl_boxes(const float* boxes, unsigned count) { gfx_occl_boxes(boxes, count); }
void vanacore_gfx_occl_set(int on, float margin)
{
    g_gfx_occl.on = on != 0 && !gfx_selected_d3d11();
    g_gfx_occl.margin = margin;
}
const unsigned char* vanacore_gfx_occl_result(unsigned* gen, unsigned* seq, unsigned* tested, unsigned* hidden)
{
    const GfxOccl* o = &g_gfx_occl;
    *gen = o->result_gen, *seq = o->result_seq, *tested = o->tested, *hidden = o->hidden;
    return o->result_gen && o->result_gen == o->gen ? o->result : NULL;
}
unsigned vanacore_gfx_occl_gen(void) { return g_gfx_occl.gen; }
void vanacore_gfx_set_characters(const float* xyzr, unsigned count) { d3d8_set_characters(xyzr, count); }
void vanacore_gfx_set_zonelines(const float* boxes, unsigned count, const float color[4], unsigned style, int glow,
                                float height, float size, float fade)
{
    d3d8_set_zonelines(boxes, count, color, style, glow, height, size, fade);
}
void vanacore_gfx_set_sparkles(const float* places, unsigned count, float size, float density, float speed, int match)
{
    d3d8_set_sparkles(places, count, size, density, speed, match);
}
void vanacore_gfx_set_ground_marks(const float* marks, unsigned count) { d3d8_set_ground_marks(marks, count); }
void vanacore_gfx_set_glow(int mode) { d3d8_set_glow(mode); }
void vanacore_gfx_set_character_light(float k) { d3d8_set_character_light(k); }
void vanacore_gfx_set_moghouse(int in) { d3d8_set_moghouse(in); }
void vanacore_gfx_set_fixed_light(int on) { d3d8_set_fixed_light(on); }
void vanacore_gfx_set_warping(int on) { d3d8_set_warping(on); }
void vanacore_gfx_set_vana_minute(float minute) { d3d8_set_vana_minute(minute); }
void vanacore_gfx_set_hide_game_shadows(int on) { d3d8_set_hide_game_shadows(on); }
void vanacore_gfx_set_fill_mode(int mode) { d3d8_set_fill_mode(mode); }
void vanacore_gfx_set_untextured(int on) { d3d8_set_untextured(on); }
void vanacore_gfx_set_grass(const VanaGrass* in)
{
    GrassSettings s = { in->enabled, in->textures, in->radius, in->density, in->height, in->width, in->variation,
        in->brightness, in->flowers, in->flower_brightness, in->flowers_only, in->log, in->limit, in->slope, in->rings_set };
    for (int i = 0; i < 3; ++i)
        s.ring[i] = in->ring[i], s.ring_amount[i] = in->ring_amount[i];
    s.show_rings = in->show_rings;
    s.edges = in->edges;
    s.match = in->match;
    s.dry = in->dry;
    s.walkable = in->walkable;
    s.pathpad = in->pathpad;
    s.build_ms = in->build_ms;
    s.overgrown = in->overgrown;
    s.nowater = in->nowater;
    s.moss = in->moss;
    s.wind_speed = in->wind_speed;
    s.wind_turbulence = in->wind_turbulence;
    s.show_cover = in->show_cover;
    s.shade = in->shade;
    s.dry_sat = in->dry_sat;
    s.dry_hue = in->dry_hue;
    s.dirt = in->dirt;
    s.blended = in->blended;
    s.ground = in->ground, s.not_ground = in->not_ground;
    s.edge_fade = in->edge_fade, s.edge_low = in->edge_low;
    s.cull = in->cull, s.mega = in->mega;
    d3d8_set_grass(&s);
}
void vanacore_gfx_set_grass_zone(int zone)
{
    d3d8_set_grass_zone(zone);
    floors_set_zone(zone); /* the reflections' floors layer lets a zone's floors go as the zone changes (floors.c) */
    snow_set_zone(zone);   /* ... and the snow's trails (snow.c) */
    zonemesh_set_zone(zone); /* ... and the sun's shadows the zone's own meshes (zonemesh.c) */
}
void vanacore_gfx_set_zone_mesh(const VanaZoneMesh* in)
{
    zonemesh_set(in);
    d3d8_set_cover_mesh(in ? in->zone : 0, in ? in->pos : NULL, in ? in->npos : 0); /* the rain's roofs too (d3d8_rain.h) */
}
static GrassFloors floors_of(const VanaFloors* in)
{
    const GrassFloors f = { in->zone,  in->tris,     in->terrain,   in->ntris, in->mesh_first,
                            in->nmesh, in->matrices, in->nmatrices, in->pairs, in->npairs };
    return f;
}
void vanacore_gfx_set_grass_floors(const VanaFloors* in)
{
    const GrassFloors f = floors_of(in);
    grass_set_floors(&f);
    floors_set(&f); /* ... and the reflections' layer of them (floors.c) */
    snow_set_floors(&f); /* ... and the snow ground for the footprints (snow.c) */
    d3d8_set_water_floors(&f); /* ... and the deep water's surface for VanaCore's water (d3d8_water.h) */
}
void vanacore_gfx_show_collision(int on) { floors_show_collision(on); }
void vanacore_gfx_set_collision(const VanaFloors* in)
{
    const GrassFloors f = floors_of(in);
    floors_set_collision(&f); /* the collision view (floors.c) */
    d3d8_set_cover(&f);       /* the rain's shelter (d3d8_rain.h) */
}
void vanacore_gfx_set_snow(const VanaSnow* in)
{
    const SnowSettings s = { in->enabled, in->depth, in->strength, in->fade, in->sink, in->soft };
    snow_configure(&s);
}
void vanacore_gfx_set_sky(const VanaSky* in)
{
    const SkySettings s = { in->enabled, in->log };
    sky_configure(&s);
}
int vanacore_gfx_floors_wanted(void) { return gfx_scene_floors_wanted(); }
void vanacore_gfx_set_grass_light(float k) { grass_set_light(k); }
void vanacore_gfx_set_grass_centre(float x, float y, float z, int known) { grass_set_centre(x, y, z, known); }
void vanacore_gfx_grass_asset(unsigned index, const uint32_t* argb, unsigned w, unsigned h, const float* rects, unsigned n, float height)
{
    grass_asset(index, argb, w, h, rects, n, height);
}
void vanacore_gfx_grass_weight(unsigned index, float weight) { grass_asset_weight(index, weight); }
void vanacore_gfx_grass_flower(unsigned index, int flower) { grass_asset_flower(index, flower); }
void vanacore_gfx_grass_capture(void) { grass_capture_start(); }
int vanacore_gfx_grass_capture_count(void) { return grass_capture_count(); }
int vanacore_gfx_grass_capture_result(unsigned i, unsigned long long* hash, unsigned* w, unsigned* h, const uint32_t** argb, float* rects,
                                      unsigned* n)
{
    uint64_t hv = 0;
    const int ok = grass_capture_result(i, &hv, w, h, argb, rects, n);
    *hash = hv;
    return ok;
}
void vanacore_gfx_grass_capture_clear(void) { grass_capture_clear(); }
int vanacore_gfx_write_png(const wchar_t* path, const uint32_t* argb, unsigned w, unsigned h) { return grass_write_png(path, argb, w, h); }
void vanacore_gfx_set_rain(const VanaRain* in)
{
    RainSettings s = { in->wet, in->sheen, in->drops, in->dry, in->force, in->rain, in->density, in->splash, in->shelter, in->thunder, in->game, in->lens, in->spatter, in->lens_on, in->ground, in->freeze, in->range, in->reflect, in->puddles, in->reach, in->view, in->water };
    d3d8_set_rain(&s);
}
void vanacore_gfx_set_weather(unsigned weather, int known) { d3d8_set_weather(weather, known); }
void vanacore_gfx_set_zone_weathers(uint32_t mask) { d3d8_set_zone_weathers(mask); }
void vanacore_gfx_set_water(const VanaWater* in)
{
    WaterSettings s = { in->enabled, in->waves, in->speed, in->size, in->reflection, in->glint, in->brightness, in->colour,
        in->shallowness, in->turbulence, in->direction, in->glint_size, in->has_tint, in->tint, in->detect, in->view, in->pass,
        in->clarity, in->refraction, in->shore, in->foam, in->ripples, in->ripple_size, in->log, in->probe, in->horizon, in->falls, in->caustics, in->relief, in->shadows, in->sea,
        in->mirror, in->mirror_scale, in->mirror_reach, in->mirror_waves };
    d3d8_set_water(&s);
}
void vanacore_gfx_set_water_zones_off(const uint16_t* zones, unsigned n) { d3d8_set_water_zones_off(zones, n); }
int vanacore_gfx_water_loading(int on) { return d3d8_water_loading(on); }
unsigned vanacore_gfx_water_buffers(void) { return d3d8_water_buffers(); }
void vanacore_gfx_set_window_opacity(float all, float chat) { d3d8_set_window_opacity(all, chat); }
void vanacore_gfx_set_border_opacity(float all, float chat) { d3d8_set_border_opacity(all, chat); }
void vanacore_gfx_set_border_colours(int all_on, uint32_t all_rgb, int chat_on, uint32_t chat_rgb) { d3d8_set_border_colours(all_on, all_rgb, chat_on, chat_rgb); }
void vanacore_gfx_set_window_colours(int all_on, uint32_t all_rgb, int chat_on, uint32_t chat_rgb) { d3d8_set_window_colours(all_on, all_rgb, chat_on, chat_rgb); }
void vanacore_gfx_set_window_image(const uint32_t* argb, unsigned w, unsigned h) { d3d8_set_window_image(argb, w, h); }
void vanacore_gfx_set_window_image_look(int fit, int chat, int input) { d3d8_set_window_image_look(fit, chat, input); }
void vanacore_gfx_set_chat_rects(const float* rects, unsigned count) { d3d8_set_chat_rects(rects, count); }
void vanacore_gfx_set_input_rect(const float* rect) { d3d8_set_input_rect(rect); }
void vanacore_gfx_set_border_solid(int all, int chat, int input) { d3d8_set_border_solid(all, chat, input); }
void vanacore_gfx_set_window_corners(int shape, float size) { d3d8_set_window_corners(shape, size); }
int vanacore_gfx_window_corners(float* size) { return d3d8_window_corners(size); }
void vanacore_gfx_clip_to_corners(const float* rect) { d3d8_clip_to_corners(rect); }
void vanacore_gfx_window_drawing(int on) { d3d8_window_drawing(on); }
void vanacore_gfx_set_input_look(float alpha, float border_alpha, int tint_on, uint32_t tint_rgb, int border_on, uint32_t border_rgb)
{
    d3d8_set_input_look(alpha, border_alpha, tint_on, tint_rgb, border_on, border_rgb);
}
void vanacore_gfx_set_chat_shadow(int mode, float opacity) { d3d8_set_chat_shadow(mode, opacity); }
void vanacore_gfx_set_chat_font(int which, const VanaTextFont* font) { d3d8_set_chat_font(which, font); }
void vanacore_gfx_set_label_glyphs(const VanaLabelGlyph* list, uint32_t count) { d3d8_set_label_glyphs(list, count); }
void vanacore_gfx_chat_drawing(int on) { d3d8_chat_drawing(on); }
void vanacore_gfx_button_drawing(int on) { d3d8_button_drawing(on); }
void vanacore_gfx_clock_drawing(int on) { d3d8_clock_drawing(on); }
void vanacore_gfx_game_letters(int on) { d3d8_game_letters(on); }
uint32_t vanacore_gfx_wanted_glyphs(int which, uint16_t* codes, uint32_t most) { return d3d8_wanted_glyphs(which, codes, most); }
void vanacore_gfx_label_word(int what, int shadow, uint32_t code, float wide) { d3d8_label_word(what, shadow, code, wide); }
void vanacore_gfx_set_macrobar(uint32_t color, int prefix) { d3d8_set_macrobar(color, prefix); }
void vanacore_gfx_label_fixed(int on) { d3d8_label_fixed(on); }
void vanacore_gfx_hide_letters(int which, int on) { d3d8_hide_letters(which, on); }
void vanacore_gfx_text_rolling(int on) { d3d8_text_rolling(on); }
void vanacore_gfx_text_centred(int on, float x) { d3d8_text_centred(on, x); }
void vanacore_gfx_text_right(int on) { d3d8_text_right(on); }
void vanacore_gfx_text_whole(int on, int right, const char* text) { d3d8_text_whole(on, right, text); }
int vanacore_gfx_chat_scale(float* sx, float* sy) { return d3d8_chat_scale(sx, sy); }
void vanacore_gfx_set_interface_target(uint32_t texture, int known) { d3d8_set_interface_target(texture, known); }
void vanacore_gfx_letter_draw(int on, int code, int x, int y, float sx, float sy) { d3d8_letter_draw(on, code, x, y, sx, sy); }
void vanacore_gfx_input_line(int on) { d3d8_input_line(on); }
void vanacore_gfx_set_input_line(const VanaLinePicture* line) { d3d8_set_input_line(line); }
void vanacore_gfx_drop_draws(int on) { d3d8_drop_draws(on); }
void vanacore_gfx_chat_font_debug(int on) { d3d8_chat_font_debug(on); }
void vanacore_gfx_chat_font_report(char* text, unsigned size) { d3d8_chat_font_report(text, size); }
int vanacore_gfx_text_area(int area) { return d3d8_text_area(area); }
void vanacore_gfx_set_tip_parts(const VanaTipPart* parts) { d3d8_set_tip_parts(parts); }
float vanacore_gfx_tips_extra(void) { return d3d8_tips_extra(); }
float vanacore_gfx_tips_width(float left, float top, float right, float bottom) { return d3d8_tips_width(left, top, right, bottom); }
void vanacore_gfx_set_tip_rarity(int name, float strength, float height) { d3d8_set_tip_rarity(name, strength, height); }
void vanacore_gfx_tip_item(int drawing, int has, uint32_t rgb) { d3d8_tip_item(drawing, has, rgb); }
void vanacore_gfx_tip_type(int drawing) { d3d8_tip_type(drawing); }
void vanacore_gfx_tip_equipped(int on) { d3d8_tip_equipped(on); }
void vanacore_gfx_set_hidden_windows(const float* rects, unsigned count) { d3d8_set_hidden_windows(rects, count); }

void vanacore_gfx_set_resolution(int window, float scene) { d3d8_set_resolution(window, scene); }
void vanacore_gfx_set_background(uint32_t w, uint32_t h) { d3d8_set_background(w, h); }
void vanacore_gfx_scene_size(uint32_t* w, uint32_t* h) { d3d8_scene_size(w, h); }
void vanacore_gfx_set_dlss(int quality, const wchar_t* helper, const wchar_t* log) { gfx_dlss_set(quality, helper, log); }
int vanacore_gfx_dlss_state(char* message, int size) { return gfx_dlss_state(message, size); }
void vanacore_gfx_set_framegen(int count) { gfx_dlss_framegen(count); }
int vanacore_gfx_framegen_state(char* message, int size) { return gfx_dlss_framegen_state(message, size); }

_Static_assert(VANA_GFX_WAIT_DLSS == GFX_WAIT_DLSS && VANA_GFX_WAIT_DLSS5 == GFX_WAIT_DLSS5 && VANA_GFX_WAIT_FRAMEGEN == GFX_WAIT_FRAMEGEN &&
                   VANA_GFX_WAIT_EFFECTS == GFX_WAIT_EFFECTS && VANA_GFX_WAIT_SHADOWS == GFX_WAIT_SHADOWS,
               "vanacore_gfx.h's wait bits are gfx.h's");
unsigned vanacore_gfx_waiting(void) { return g_start_ok ? gfx_waiting() : 0; }
int vanacore_gfx_wait_progress(unsigned bit, unsigned* done, unsigned* total)
{
    return g_start_ok && done && total ? gfx_wait_progress(bit, done, total) : 0;
}
int vanacore_gfx_wait_step(unsigned bit, char* words, int size)
{
    if (!words || size <= 0)
        return 0;
    words[0] = 0;
    return g_start_ok ? gfx_wait_step(bit, words, size) : 0;
}

void vanacore_gfx_set_own_pixels(int on) { d3d8_set_own_pixels(on); }

int vanacore_gfx_ui_squeezed(uint32_t w, uint32_t h) { return d3d8_ui_squeezed(w, h); }

int vanacore_gfx_plugin_wheel_over(int x, int y) { return d3d8_plugin_wheel_over(x, y); }

void vanacore_gfx_set_mover(int slot, float ax, float ay, float scale) { d3d8_set_mover(slot, ax, ay, scale); }

int vanacore_gfx_mover_box(int slot, float* box) { return d3d8_mover_box(slot, box); }

int vanacore_gfx_ui_layer(uint32_t* w, uint32_t* h, void* pixels, uint32_t pitch) { return d3d8_ui_layer(w, h, pixels, pitch); }

void vanacore_gfx_raise_take(int x, int y, int w, int h) { d3d8_raise_take(x, y, w, h); }
void vanacore_gfx_raise_put(void) { d3d8_raise_put(); }

void vanacore_gfx_set_render_size(uint32_t w, uint32_t h) { d3d8_set_render_size(w, h); }
void vanacore_gfx_set_drawlog(const char* path) { d3d8_set_drawlog(path); }
void vanacore_gfx_set_lantern_textures(const char* list) { d3d8_set_lantern_textures(list); }
void vanacore_gfx_set_dim_textures(const char* list) { d3d8_set_dim_textures(list); }
void vanacore_gfx_set_dlss5_skip_textures(const char* list) { d3d8_set_dlss5_skip_textures(list); }

const char* vanacore_gfx_adapter(void) { return g_start_ok ? gfx_adapter_name() : ""; }
int vanacore_gfx_video_memory(unsigned long long* used, unsigned long long* budget) { return g_start_ok ? gfx_video_memory(used, budget) : 0; }

int vanacore_gfx_device_lost(void) { return g_start_ok ? gfx_lost() : 0; }

int vanacore_gfx_device_recover(void)
{
    if (!g_start_ok || !gfx_lost())
        return 0;
    void* window = d3d8_device_window();
    const int vsync = d3d8_device_vsync();
    d3d8_device_lost();
    int on = 0, waited = 0;
    if (gfx_shutdown())
    {
        /* the driver may still be coming back (AMD's takes seconds), and the card the device was on may not be listed till
         * then (gfx_card_find): tried every half second for 15 s */
        for (int tries = 0; !on && tries < 30; ++tries)
        {
            if (tries)
                Sleep(500);
            on = gfx_init(window, vsync) ? 1 : 0;
        }
        waited = 1;
        if (!on)
            rt_log("gfx: no Direct3D 12 device could be made again in 15 s\n");
    }
    else
        rt_log("gfx: the lost Direct3D 12 device is still held somewhere (an overlay's hook, a leak): no new one can be made\n");
    if (!on && !gfx_selected_d3d11() && LoadLibraryW(L"d3d11.dll"))
    {
        /* Direct3D 11 draws the game from here, as with renderer=d3d11: it makes a device of its own, whatever holds
         * Direct3D 12's. On the card Direct3D 12 was on, given 15 s to come back unless Direct3D 12 waited them; then on
         * whichever card Windows offers (maybe the processor's own, slow) rather than no picture at all */
        gfx_select(1);
        for (int tries = 0; !on && tries < (waited ? 1 : 30); ++tries)
        {
            if (tries)
                Sleep(500);
            on = gfx_init(window, vsync) ? 2 : 0;
        }
        if (!on && gfx_card_known())
        {
            rt_log("gfx: the graphics card the game was on didn't come back: Direct3D 11 takes whichever card Windows offers\n");
            gfx_card_forget();
            on = gfx_init(window, vsync) ? 2 : 0;
        }
        rt_log(on ? "gfx: Direct3D 11 draws the game in Direct3D 12's place from here\n"
                  : "gfx: Direct3D 11 couldn't come up in Direct3D 12's place either\n");
        if (!on)
            gfx_select(0);
    }
    if (on)
        d3d8_device_restored();
    return on;
}
