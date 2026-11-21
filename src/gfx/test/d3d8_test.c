/* The D3D8 front end (d3d8.c) on the graphics back end, without the game (R3.2): the same COM
 * objects and thunks FFXiMain calls, driven from here the way the game drives them - a device on no
 * window (offscreen), then clears, locks of the back buffer, textures filled through LockRect,
 * vertex and index buffers, FVF and declared layouts, the ...UP draws, CopyRects from the back
 * buffer into a render-target texture - with the pixels read back through the back buffer's own
 * LockRect.
 *
 * No translated code is linked: the generated tables are empty stubs below.
 *
 * usage: d3d8_test      (exit status 0 when every check passes) */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "d3d8.h"
#include "gfx.h"
#include "gthread.h"
#include "gwin.h"
#include "plat.h"
#include "runtime.h"
#include "thunk.h"

#ifdef VANACORE
/* VanaCore.GfxTest: the COM objects are native, called through their vtables as the game calls them */
#include "vanacore_gfx.h"

static uint32_t guest_call(uint32_t fn, unsigned n, const uint32_t* args)
{
    uint32_t r;
    __asm {
        mov ecx, n
        mov esi, args
    more:
        test ecx, ecx
        jz go
        dec ecx
        push dword ptr [esi + ecx * 4]
        jmp more
    go:
        call fn
        mov r, eax
    }
    return r;
}

static void gt_lock(void) {}
static void gt_unlock(void) {}
#else
/* no translation in this test */
const RtEntry rt_table[1];
const unsigned rt_table_count = 0;
const unsigned char rt_table_patch[1];
const uint32_t rt_image_base = 0x10000000u, rt_image_timestamp, rt_image_size, rt_image_text_rva, rt_image_text_size,
               rt_image_pol1_rva, rt_image_pol1_src_len, rt_image_oep, rt_image_reloc_rva;
#endif

#define W 32
#define H 32

static int g_fails;
#define CHECK(cond, ...)                                                                                              \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond))                                                                                                  \
        {                                                                                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                              \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
            g_fails++;                                                                                                \
        }                                                                                                             \
    } while (0)

/* IDirect3DDevice8 vtable slots (the D3D8 ABI) */
enum
{
    D_Release = 2,
    D_Present = 15,
    D_GetBackBuffer = 16,
    D_CreateTexture = 20,
    D_CreateVertexBuffer = 23,
    D_CreateIndexBuffer = 24,
    D_CreateImageSurface = 27,
    D_CopyRects = 28,
    D_SetRenderTarget = 31,
    D_GetRenderTarget = 32,
    D_BeginScene = 34,
    D_EndScene = 35,
    D_Clear = 36,
    D_SetTransform = 37,
    D_SetRenderState = 50,
    D_GetRenderState = 51,
    D_BeginStateBlock = 52,
    D_EndStateBlock = 53,
    D_ApplyStateBlock = 54,
    D_DeleteStateBlock = 56,
    D_CreateStateBlock = 57,
    D_GetTexture = 60,
    D_SetTexture = 61,
    D_SetTextureStageState = 63,
    D_DrawPrimitive = 70,
    D_DrawIndexedPrimitive = 71,
    D_DrawPrimitiveUP = 72,
    D_CreateVertexShader = 75,
    D_SetVertexShader = 76,
    D_GetVertexShader = 77,
    D_DeleteVertexShader = 78,
    D_SetVertexShaderConstant = 79,
    D_SetStreamSource = 83,
    D_SetIndices = 85,
    D_GetIndices = 86,
    D_CreatePixelShader = 87,
    D_SetPixelShader = 88,
    D_DeletePixelShader = 90,
    D_SetPixelShaderConstant = 91,
};
/* IDirect3DTexture8 / IDirect3DSurface8 / buffers */
enum
{
    T_GetSurfaceLevel = 15,
    T_LockRect = 16,
    T_UnlockRect = 17,
    S_Release = 2,
    S_LockRect = 9,
    S_UnlockRect = 10,
    B_Lock = 11,
    B_Unlock = 12,
};

static uint32_t call(uint32_t obj, int slot, int n, ...)
{
    uint32_t args[12] = { obj };
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; ++i)
        args[1 + i] = va_arg(ap, uint32_t);
    va_end(ap);
    return guest_call(rd32(rd32(obj) + 4u * (uint32_t)slot), (unsigned)n + 1, args);
}

static uint32_t galloc(uint32_t n)
{
    gt_lock();
    uint32_t p = gheap_alloc(n, 1);
    gt_unlock();
    return p;
}

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static uint32_t g_dev, g_bb, g_out;

/* the back buffer's pixel through LockRect (A8R8G8B8 / X8R8G8B8 as D3D lays it out) */
static uint32_t pixel(int x, int y)
{
    uint32_t lr = galloc(8);
    call(g_bb, S_LockRect, 3, lr, 0, 0x10 /* READONLY */);
    uint32_t v = rd32(rd32(lr + 4) + (uint32_t)y * rd32(lr) + 4u * (uint32_t)x);
    call(g_bb, S_UnlockRect, 0);
    return v & 0xFFFFFF;
}

static void clear(uint32_t color)
{
    call(g_dev, D_Clear, 6, 0, 0, 3, color, fbits(1.0f), 0);
}

typedef struct
{
    float x, y, z, rhw;
    uint32_t c;
    float u, v;
} UiVert;

static void test_clear_and_lock(void)
{
    clear(0xFF336699u);
    CHECK(pixel(0, 0) == 0x336699 && pixel(31, 31) == 0x336699, "clear via LockRect: %06x", pixel(0, 0));
}

static void test_up_texture(void)
{
    clear(0xFF000000u);
    /* a 2x2 A8R8G8B8 texture filled through LockRect, the way the game loads its own */
    call(g_dev, D_CreateTexture, 7, 2, 2, 1, 0, 21, 1 /* MANAGED */, g_out);
    uint32_t tex = rd32(g_out);
    uint32_t lr = galloc(8);
    call(tex, T_LockRect, 4, 0, lr, 0, 0);
    uint32_t bits = rd32(lr + 4), pitch = rd32(lr);
    wr32(bits, 0xFFFF0000u), wr32(bits + 4, 0xFF00FF00u), wr32(bits + pitch, 0xFF0000FFu), wr32(bits + pitch + 4, 0xFFFFFFFFu);
    call(tex, T_UnlockRect, 1, 0);
    call(g_dev, D_SetTexture, 2, 0, tex);
    call(g_dev, D_SetTextureStageState, 3, 0, 16, 1); /* MAGFILTER point */
    call(g_dev, D_SetTextureStageState, 3, 0, 17, 1);
    call(g_dev, D_SetVertexShader, 1, 0x144);
    uint32_t v = galloc(4 * sizeof(UiVert));
    UiVert q[4] = { { 0, 0, 0.5f, 1, 0xFFFFFFFFu, 0, 0 }, { 16, 0, 0.5f, 1, 0xFFFFFFFFu, 1, 0 }, { 0, 16, 0.5f, 1, 0xFFFFFFFFu, 0, 1 },
                    { 16, 16, 0.5f, 1, 0xFFFFFFFFu, 1, 1 } };
    memcpy(GUEST_PTR(v), q, sizeof q);
    call(g_dev, D_BeginScene, 0);
    call(g_dev, D_DrawPrimitiveUP, 4, 5 /* TRIANGLESTRIP */, 2, v, (uint32_t)sizeof(UiVert));
    call(g_dev, D_EndScene, 0);
    CHECK(pixel(2, 2) == 0xFF0000 && pixel(12, 2) == 0x00FF00 && pixel(2, 12) == 0x0000FF && pixel(12, 12) == 0xFFFFFF,
        "UP draw with a locked texture: %06x %06x %06x %06x", pixel(2, 2), pixel(12, 2), pixel(2, 12), pixel(12, 12));

    /* relock, rewrite one texel: the next draw sees it */
    call(tex, T_LockRect, 4, 0, lr, 0, 0);
    wr32(rd32(lr + 4), 0xFFFFFF00u);
    call(tex, T_UnlockRect, 1, 0);
    call(g_dev, D_DrawPrimitiveUP, 4, 5, 2, v, (uint32_t)sizeof(UiVert));
    CHECK(pixel(2, 2) == 0xFFFF00, "texture relocked: %06x", pixel(2, 2));
    call(g_dev, D_SetTexture, 2, 0, 0);
    call(tex, D_Release, 0);
}

/* XYZ | DIFFUSE (0x042) in a vertex buffer, identity transforms, lighting off; then indexed */
static void test_buffers(void)
{
    clear(0xFF000000u);
    call(g_dev, D_SetRenderState, 2, 137, 0); /* LIGHTING off */
    call(g_dev, D_SetRenderState, 2, 22, 1);  /* CULL none */
    call(g_dev, D_SetTextureStageState, 3, 0, 1, 2 /* SELECTARG1 */);
    call(g_dev, D_SetTextureStageState, 3, 0, 2, 0 /* DIFFUSE */);
    call(g_dev, D_CreateVertexBuffer, 5, 4 * 16, 0x8, 0x42, 1, g_out);
    uint32_t vb = rd32(g_out);
    uint32_t pp = galloc(4);
    call(vb, B_Lock, 4, 0, 0, pp, 0);
    float vs[4][4] = { { -1, 1, 0.5f, 0 }, { 0, 1, 0.5f, 0 }, { -1, 0, 0.5f, 0 }, { 0, 0, 0.5f, 0 } };
    for (int i = 0; i < 4; ++i)
    {
        uint32_t c = 0xFFFF8000u;
        memcpy(&vs[i][3], &c, 4);
    }
    memcpy(GUEST_PTR(rd32(pp)), vs, sizeof vs);
    call(vb, B_Unlock, 0);
    call(g_dev, D_SetVertexShader, 1, 0x42);
    call(g_dev, D_SetStreamSource, 3, 0, vb, 16);
    call(g_dev, D_DrawPrimitive, 3, 5, 0, 2);
    CHECK(pixel(4, 4) == 0xFF8000 && pixel(20, 20) == 0, "vertex buffer: %06x %06x", pixel(4, 4), pixel(20, 20));

    /* the same four vertices through an index buffer: indices 1, 2, 3 from startIndex 1 */
    call(g_dev, D_CreateIndexBuffer, 5, 6 * 2, 0x8, 101, 1, g_out);
    uint32_t ib = rd32(g_out);
    call(ib, B_Lock, 4, 0, 0, pp, 0);
    uint16_t idx[6] = { 0, 1, 2, 3, 0, 0 };
    memcpy(GUEST_PTR(rd32(pp)), idx, sizeof idx);
    call(ib, B_Unlock, 0);
    clear(0xFF000000u);
    call(g_dev, D_SetIndices, 2, ib, 0);
    call(g_dev, D_DrawIndexedPrimitive, 5, 4 /* LIST */, 0, 4, 1, 1); /* indices 1, 2, 3 */
    /* triangle (0,1) (-1,0) (0,0): the lower-right half of the top-left quarter */
    CHECK(pixel(4, 4) == 0 && pixel(14, 14) == 0xFF8000, "indexed: %06x %06x", pixel(4, 4), pixel(14, 14));
    call(g_dev, D_SetIndices, 2, 0, 0);
    call(g_dev, D_SetStreamSource, 3, 0, 0, 0);
    call(ib, D_Release, 0);
    call(vb, D_Release, 0);
}

/* a declared layout on two streams: position in stream 0, color in stream 1 */
static void test_declaration(void)
{
    clear(0xFF000000u);
    uint32_t decl[] = {
        0x20000000u,                        /* STREAM(0) */
        0x40000000u | (2u << 16) | 0,       /* REG(v0, FLOAT3) */
        0x20000001u,                        /* STREAM(1) */
        0x40000000u | (4u << 16) | 5,       /* REG(v5, D3DCOLOR) */
        0xFFFFFFFFu,
    };
    uint32_t gd = galloc(sizeof decl);
    memcpy(GUEST_PTR(gd), decl, sizeof decl);
    call(g_dev, D_CreateVertexShader, 4, gd, 0, g_out, 0);
    uint32_t h = rd32(g_out);
    CHECK(h & 1, "declared shader handle %08x", h);
    uint32_t pp = galloc(4);
    call(g_dev, D_CreateVertexBuffer, 5, 4 * 12, 0x8, 0, 1, g_out);
    uint32_t pos = rd32(g_out);
    call(pos, B_Lock, 4, 0, 0, pp, 0);
    float p[4][3] = { { 0, 0, 0.5f }, { 1, 0, 0.5f }, { 0, -1, 0.5f }, { 1, -1, 0.5f } };
    memcpy(GUEST_PTR(rd32(pp)), p, sizeof p);
    call(pos, B_Unlock, 0);
    call(g_dev, D_CreateVertexBuffer, 5, 4 * 4, 0x8, 0, 1, g_out);
    uint32_t col = rd32(g_out);
    call(col, B_Lock, 4, 0, 0, pp, 0);
    uint32_t c[4] = { 0xFF00FFFFu, 0xFF00FFFFu, 0xFF00FFFFu, 0xFF00FFFFu };
    memcpy(GUEST_PTR(rd32(pp)), c, sizeof c);
    call(col, B_Unlock, 0);
    call(g_dev, D_SetVertexShader, 1, h);
    call(g_dev, D_SetStreamSource, 3, 0, pos, 12);
    call(g_dev, D_SetStreamSource, 3, 1, col, 4);
    call(g_dev, D_DrawPrimitive, 3, 5, 0, 2);
    CHECK(pixel(20, 20) == 0x00FFFF && pixel(4, 4) == 0, "two streams: %06x %06x", pixel(20, 20), pixel(4, 4));
    call(g_dev, D_SetStreamSource, 3, 0, 0, 0);
    call(g_dev, D_SetStreamSource, 3, 1, 0, 0);
    call(pos, D_Release, 0);
    call(col, D_Release, 0);
}

/* a declared layout on five streams, as the character creation's models draw (the colour in stream 4, past the back
 * ends' four): position, normal, two texture coordinates, colour, each in its own buffer, drawn from vertex 2 */
static void test_five_streams(void)
{
    clear(0xFF000000u);
    uint32_t decl[] = {
        0x20000000u, 0x40000000u | (2u << 16) | 0, /* STREAM(0) REG(v0, FLOAT3) */
        0x20000001u, 0x40000000u | (2u << 16) | 3, /* STREAM(1) REG(v3, FLOAT3) */
        0x20000002u, 0x40000000u | (1u << 16) | 7, /* STREAM(2) REG(v7, FLOAT2) */
        0x20000003u, 0x40000000u | (1u << 16) | 8, /* STREAM(3) REG(v8, FLOAT2) */
        0x20000004u, 0x40000000u | (4u << 16) | 5, /* STREAM(4) REG(v5, D3DCOLOR) */
        0xFFFFFFFFu,
    };
    uint32_t gd = galloc(sizeof decl);
    memcpy(GUEST_PTR(gd), decl, sizeof decl);
    call(g_dev, D_CreateVertexShader, 4, gd, 0, g_out, 0);
    uint32_t h = rd32(g_out);
    static const uint32_t sizes[5] = { 12, 12, 8, 8, 4 };
    uint32_t vb[5], pp = galloc(4);
    for (int s = 0; s < 5; ++s)
    {
        /* in the default pool: Direct3D 8 and 9 refuse a dynamic buffer that's managed */
        uint32_t hr = call(g_dev, D_CreateVertexBuffer, 5, 6 * sizes[s], 0x208 /* DYNAMIC | WRITEONLY */, 0, 0, g_out);
        vb[s] = hr == 0 ? rd32(g_out) : 0;
        CHECK(vb[s], "five streams: CreateVertexBuffer %d: %08x", s, hr);
        if (!vb[s])
        {
            while (s-- > 0)
                call(vb[s], D_Release, 0);
            call(g_dev, D_DeleteVertexShader, 1, h);
            return;
        }
        call(vb[s], B_Lock, 4, 0, 0, pp, 0x2000 /* DISCARD */);
        uint8_t* m = GUEST_PTR(rd32(pp));
        memset(m, 0, 6 * sizes[s]);
        if (s == 0) /* two vertices off the target, then the bottom-right quarter */
        {
            float p[6][3] = { { -9, -9, 0.5f }, { -9, -9, 0.5f }, { 0, 0, 0.5f }, { 1, 0, 0.5f }, { 0, -1, 0.5f }, { 1, -1, 0.5f } };
            memcpy(m, p, sizeof p);
        }
        else if (s == 4)
        {
            uint32_t c[6] = { 0xFFFF0000u, 0xFFFF0000u, 0xFF80FF00u, 0xFF80FF00u, 0xFF80FF00u, 0xFF80FF00u };
            memcpy(m, c, sizeof c);
        }
        call(vb[s], B_Unlock, 0);
    }
    call(g_dev, D_SetVertexShader, 1, h);
    for (int s = 0; s < 5; ++s)
        call(g_dev, D_SetStreamSource, 3, (uint32_t)s, vb[s], sizes[s]);
    call(g_dev, D_DrawPrimitive, 3, 5, 2, 2);
    CHECK(pixel(20, 20) == 0x80FF00 && pixel(4, 4) == 0, "five streams: %06x %06x", pixel(20, 20), pixel(4, 4));
    for (int s = 0; s < 5; ++s)
    {
        call(g_dev, D_SetStreamSource, 3, (uint32_t)s, 0, 0);
        call(vb[s], D_Release, 0);
    }
}

/* the back buffer copied into a render-target texture, which is then drawn */
static void test_copyrects(void)
{
    clear(0xFF123456u);
    call(g_dev, D_CreateTexture, 7, W, H, 1, 1 /* RENDERTARGET */, 22, 0, g_out);
    uint32_t tex = rd32(g_out);
    call(tex, T_GetSurfaceLevel, 2, 0, g_out);
    uint32_t surf = rd32(g_out);
    call(g_dev, D_CopyRects, 5, g_bb, 0, 0, surf, 0);
    clear(0xFF000000u);
    call(g_dev, D_SetTexture, 2, 0, tex);
    call(g_dev, D_SetTextureStageState, 3, 0, 1, 4 /* MODULATE */);
    call(g_dev, D_SetTextureStageState, 3, 0, 2, 2 /* TEXTURE */);
    call(g_dev, D_SetVertexShader, 1, 0x144);
    uint32_t v = galloc(4 * sizeof(UiVert));
    UiVert q[4] = { { 0, 0, 0.5f, 1, 0xFFFFFFFFu, 0, 0 }, { 32, 0, 0.5f, 1, 0xFFFFFFFFu, 1, 0 }, { 0, 32, 0.5f, 1, 0xFFFFFFFFu, 0, 1 },
                    { 32, 32, 0.5f, 1, 0xFFFFFFFFu, 1, 1 } };
    memcpy(GUEST_PTR(v), q, sizeof q);
    call(g_dev, D_DrawPrimitiveUP, 4, 5, 2, v, (uint32_t)sizeof(UiVert));
    CHECK(pixel(16, 16) == 0x123456, "CopyRects back buffer -> texture: %06x", pixel(16, 16));

    /* render into the texture itself, then read it through its surface */
    call(g_dev, D_SetRenderTarget, 2, surf, 0);
    clear(0xFF654321u);
    uint32_t lr = galloc(8);
    call(surf, S_LockRect, 3, lr, 0, 0x10);
    uint32_t got = rd32(rd32(lr + 4)) & 0xFFFFFF;
    call(surf, S_UnlockRect, 0);
    CHECK(got == 0x654321, "render-target texture readback: %06x", got);
    call(g_dev, D_SetRenderTarget, 2, g_bb, 0);
    call(g_dev, D_SetTexture, 2, 0, 0);
    call(surf, S_Release, 0);
    call(tex, D_Release, 0);
}

/* A render target drawn at another size (FFXI's trust and avatar glow: its 4096 picture of the character to 256x256
 * and back over the scene) lands where it was: today's pixel centres, not D3D8's, which moved it half a texel. A
 * 16x8 target (not the screen's shape: one that is, the menu target, is drawn at the screen's size) white on its left
 * half, stretched over the 32x32 back buffer through a linear filter: the pixels either side of the edge are 3/4 and
 * 1/4 white (D3D8's: all white, then half). VanaCore's own Direct3D 8 alone: Direct3D 9's keeps D3D8's. */
static void test_scaled_copy(void)
{
    call(g_dev, D_CreateTexture, 7, 16, 8, 1, 1 /* RENDERTARGET */, 22, 0, g_out);
    uint32_t tex = rd32(g_out);
    call(tex, T_GetSurfaceLevel, 2, 0, g_out);
    uint32_t surf = rd32(g_out);
    call(g_dev, D_SetRenderTarget, 2, surf, 0);
    clear(0xFF000000u);
    uint32_t rect = galloc(16);
    const int32_t left[4] = { 0, 0, 8, 8 };
    memcpy(GUEST_PTR(rect), left, sizeof left);
    call(g_dev, D_Clear, 6, 1, rect, 1 /* TARGET */, 0xFFFFFFFFu, fbits(1.0f), 0);
    call(g_dev, D_SetRenderTarget, 2, g_bb, 0);
    clear(0xFF000000u);
    call(g_dev, D_SetTexture, 2, 0, tex);
    call(g_dev, D_SetTextureStageState, 3, 0, 1, 2 /* SELECTARG1 */);
    call(g_dev, D_SetTextureStageState, 3, 0, 2, 2 /* TEXTURE */);
    call(g_dev, D_SetTextureStageState, 3, 0, 16, 2 /* MAGFILTER: LINEAR */);
    call(g_dev, D_SetTextureStageState, 3, 0, 17, 2 /* MINFILTER: LINEAR */);
    call(g_dev, D_SetTextureStageState, 3, 0, 13, 3 /* ADDRESSU: CLAMP */);
    call(g_dev, D_SetTextureStageState, 3, 0, 14, 3 /* ADDRESSV: CLAMP */);
    call(g_dev, D_SetVertexShader, 1, 0x144);
    uint32_t v = galloc(4 * sizeof(UiVert));
    UiVert q[4] = { { 0, 0, 0.5f, 1, 0xFFFFFFFFu, 0, 0 }, { W, 0, 0.5f, 1, 0xFFFFFFFFu, 1, 0 }, { 0, H, 0.5f, 1, 0xFFFFFFFFu, 0, 1 },
                    { W, H, 0.5f, 1, 0xFFFFFFFFu, 1, 1 } };
    memcpy(GUEST_PTR(v), q, sizeof q);
    call(g_dev, D_BeginScene, 0);
    call(g_dev, D_DrawPrimitiveUP, 4, 5, 2, v, (uint32_t)sizeof(UiVert));
    call(g_dev, D_EndScene, 0);
    uint32_t a = pixel(15, 8) & 0xFF, b = pixel(16, 8) & 0xFF;
    CHECK(a > 160 && a < 225 && b > 30 && b < 95,
        "a render target stretched 2x: either side of its edge %u, %u (want about 191 and 64: centred, not D3D8's half texel)", a, b);
    call(g_dev, D_SetTextureStageState, 3, 0, 1, 4 /* MODULATE, as it was */);
    call(g_dev, D_SetTextureStageState, 3, 0, 16, 1 /* POINT */);
    call(g_dev, D_SetTextureStageState, 3, 0, 17, 1 /* POINT */);
    call(g_dev, D_SetTextureStageState, 3, 0, 13, 1 /* WRAP */);
    call(g_dev, D_SetTextureStageState, 3, 0, 14, 1 /* WRAP */);
    call(g_dev, D_SetTexture, 2, 0, 0);
    call(surf, S_Release, 0);
    call(tex, D_Release, 0);
}

/* a solid 4x4 DXT1 block */
static void dxt1_block(uint8_t* b, uint16_t c565)
{
    b[0] = b[2] = (uint8_t)c565, b[1] = b[3] = (uint8_t)(c565 >> 8);
    b[4] = b[5] = b[6] = b[7] = 0;
}

static void draw_textured_quad(uint32_t tex)
{
    call(g_dev, D_SetTexture, 2, 0, tex);
    call(g_dev, D_SetVertexShader, 1, 0x144);
    uint32_t v = galloc(4 * sizeof(UiVert));
    UiVert q[4] = { { 0, 0, 0.5f, 1, 0xFFFFFFFFu, 0, 0 }, { 16, 0, 0.5f, 1, 0xFFFFFFFFu, 1, 0 }, { 0, 16, 0.5f, 1, 0xFFFFFFFFu, 0, 1 },
                    { 16, 16, 0.5f, 1, 0xFFFFFFFFu, 1, 1 } };
    memcpy(GUEST_PTR(v), q, sizeof q);
    call(g_dev, D_BeginScene, 0);
    call(g_dev, D_DrawPrimitiveUP, 4, 5, 2, v, (uint32_t)sizeof(UiVert));
    call(g_dev, D_EndScene, 0);
}

/* A texture pack entry: a 4x4 DXT1 texture the game fills with red is drawn with an 8x8 green one
 * (and its 4x4 mipmap) from <hash>_4x4.dds; once the game writes blue into it, its own again. */
static void test_texture_pack(void)
{
    uint8_t red[8], blue[8];
    dxt1_block(red, 0xF800);
    dxt1_block(blue, 0x001F);
    uint64_t hash = 0xcbf29ce484222325ull;
    for (int i = 0; i < 8; ++i)
        hash = (hash ^ red[i]) * 0x100000001b3ull;
    const char* dir = "build/d3d8test/textures";
    plat_mkdir("build/d3d8test");
    plat_mkdir(dir);
    char path[256];
    snprintf(path, sizeof path, "%s/%016llx_4x4.dds", dir, (unsigned long long)hash);
    uint8_t dds[128 + 32 + 8] = { 'D', 'D', 'S', ' ', 124 };
    dds[12] = dds[16] = 8;                                     /* height, width */
    dds[28] = 2;                                               /* mipmaps */
    dds[76] = 32, dds[80] = 4;                                 /* pixel format: FOURCC */
    memcpy(dds + 84, "DXT1", 4);
    for (int i = 0; i < 5; ++i)
        dxt1_block(dds + 128 + 8 * i, 0x07E0); /* 8x8: four blocks, then 4x4: one */
    FILE* f = fopen(path, "wb");
    CHECK(f && fwrite(dds, 1, sizeof dds, f) == sizeof dds, "cannot write %s", path);
    if (f)
        fclose(f);
    d3d8_texture_pack(dir);

    clear(0xFF000000u);
    call(g_dev, D_CreateTexture, 7, 4, 4, 1, 0, 0x31545844u /* DXT1 */, 1, g_out);
    uint32_t tex = rd32(g_out), lr = galloc(8);
    call(tex, T_LockRect, 4, 0, lr, 0, 0);
    memcpy(GUEST_PTR(rd32(lr + 4)), red, 8);
    call(tex, T_UnlockRect, 1, 0);
    draw_textured_quad(tex);
    CHECK(pixel(8, 8) == 0x00FF00, "texture pack: the replacement drawn: %06x", pixel(8, 8));

    call(tex, T_LockRect, 4, 0, lr, 0, 0);
    memcpy(GUEST_PTR(rd32(lr + 4)), blue, 8);
    call(tex, T_UnlockRect, 1, 0);
    draw_textured_quad(tex);
    CHECK(pixel(8, 8) == 0x0000FF, "texture pack: other contents, the game's own: %06x", pixel(8, 8));
    call(g_dev, D_SetTexture, 2, 0, 0);
    call(tex, D_Release, 0);
}

#ifdef VANACORE
/* within a step or so of each channel (a shader's 0.5 is 7F or 80) */
static int near_rgb(uint32_t got, uint32_t want)
{
    for (int s = 0; s < 24; s += 8)
    {
        int d = (int)((got >> s) & 255) - (int)((want >> s) & 255);
        if (d < -2 || d > 2)
            return 0;
    }
    return 1;
}

/* a vs.1.1 shader with its declaration: position and color in, the color times c0 out */
static void test_vertex_shader(void)
{
    clear(0xFF000000u);
    uint32_t decl[] = {
        0x20000000u,                  /* STREAM(0) */
        0x40000000u | (2u << 16) | 0, /* REG(v0, FLOAT3) */
        0x40000000u | (4u << 16) | 5, /* REG(v5, D3DCOLOR) */
        0xFFFFFFFFu,
    };
    uint32_t code[] = {
        0xFFFE0101u,                                        /* vs.1.1 */
        0x00000001u, 0xC00F0000u, 0x90E40000u,              /* mov oPos, v0 */
        0x00000005u, 0xD00F0000u, 0x90E40005u, 0xA0E40000u, /* mul oD0, v5, c0 */
        0x0000FFFFu,
    };
    uint32_t gd = galloc(sizeof decl), gc = galloc(sizeof code);
    memcpy(GUEST_PTR(gd), decl, sizeof decl);
    memcpy(GUEST_PTR(gc), code, sizeof code);
    uint32_t hr = call(g_dev, D_CreateVertexShader, 4, gd, gc, g_out, 0);
    uint32_t h = rd32(g_out);
    CHECK(hr == 0 && (h & 1), "vertex shader: CreateVertexShader %08x handle %08x", hr, h);
    if (hr != 0)
        return;
    typedef struct
    {
        float x, y, z;
        uint32_t c;
    } V;
    uint32_t v = galloc(4 * sizeof(V));
    V q[4] = { { 0, 0, 0.5f, 0xFFFFFFFFu }, { 1, 0, 0.5f, 0xFFFFFFFFu }, { 0, -1, 0.5f, 0xFFFFFFFFu }, { 1, -1, 0.5f, 0xFFFFFFFFu } };
    memcpy(GUEST_PTR(v), q, sizeof q);
    float c0[4] = { 1.0f, 0.5f, 0.0f, 1.0f };
    uint32_t gk = galloc(sizeof c0);
    memcpy(GUEST_PTR(gk), c0, sizeof c0);
    call(g_dev, D_SetVertexShader, 1, h);
    call(g_dev, D_SetVertexShaderConstant, 3, 0, gk, 1);
    call(g_dev, D_GetVertexShader, 1, g_out);
    CHECK(rd32(g_out) == h, "vertex shader: GetVertexShader %08x, not %08x", rd32(g_out), h);
    call(g_dev, D_BeginScene, 0);
    call(g_dev, D_DrawPrimitiveUP, 4, 5, 2, v, (uint32_t)sizeof(V));
    call(g_dev, D_EndScene, 0);
    CHECK(near_rgb(pixel(20, 20), 0xFF8000) && pixel(4, 4) == 0, "vertex shader: %06x %06x", pixel(20, 20), pixel(4, 4));
    call(g_dev, D_SetVertexShader, 1, 0x42);
    CHECK(call(g_dev, D_DeleteVertexShader, 1, h) == 0, "vertex shader: DeleteVertexShader");

    /* an input the code reads that the declaration hasn't (FFXI's zone shader reads v15) is 0, 0, 0, 1,
     * and a scalar instruction reads its source's w without a replicate swizzle (the zone shader's rsq):
     * rsq r0, v15 is 1 everywhere; add oD0, r0.wwwz, c0 with c0 (0, -0.5, -1, 1) is 1, 0.5, 0 */
    clear(0xFF000000u);
    uint32_t missing[] = {
        0xFFFE0101u,
        0x00000001u, 0xC00F0000u, 0x90E40000u,              /* mov oPos, v0 */
        0x00000007u, 0x800F0000u, 0x90E4000Fu,              /* rsq r0, v15 */
        0x00000002u, 0xD00F0000u, 0x80BF0000u, 0xA0E40000u, /* add oD0, r0.wwwz, c0 */
        0x0000FFFFu,
    };
    gc = galloc(sizeof missing);
    memcpy(GUEST_PTR(gc), missing, sizeof missing);
    hr = call(g_dev, D_CreateVertexShader, 4, gd, gc, g_out, 0);
    h = rd32(g_out);
    CHECK(hr == 0, "vertex shader reading an input not declared: CreateVertexShader %08x", hr);
    if (hr != 0)
        return;
    float c1[4] = { 0.0f, -0.5f, -1.0f, 1.0f };
    memcpy(GUEST_PTR(gk), c1, sizeof c1);
    call(g_dev, D_SetVertexShader, 1, h);
    call(g_dev, D_SetVertexShaderConstant, 3, 0, gk, 1);
    call(g_dev, D_BeginScene, 0);
    call(g_dev, D_DrawPrimitiveUP, 4, 5, 2, v, (uint32_t)sizeof(V));
    call(g_dev, D_EndScene, 0);
    CHECK(near_rgb(pixel(20, 20), 0xFF8000) && pixel(4, 4) == 0, "vertex shader reading an input not declared: %06x %06x",
        pixel(20, 20), pixel(4, 4));
    call(g_dev, D_SetVertexShader, 1, 0x42);
    call(g_dev, D_DeleteVertexShader, 1, h);
}

/* FFXI's zone shader (its base form, assembled by hand from FFXiMain's text) with its declaration
 * (position, normal, color, texture coordinate; no v15, which the code reads): the vertex color comes out */
static void test_zone_shader(void)
{
    clear(0xFF000000u);
    uint32_t decl[] = {
        0x20000000u,                  /* STREAM(0) */
        0x40000000u | (2u << 16) | 0, /* REG(v0, FLOAT3) */
        0x40000000u | (2u << 16) | 3, /* REG(v3, FLOAT3) */
        0x40000000u | (4u << 16) | 5, /* REG(v5, D3DCOLOR) */
        0x40000000u | (1u << 16) | 7, /* REG(v7, FLOAT2) */
        0xFFFFFFFFu,
    };
    uint32_t code[] = {
        0xFFFE0101u, 0x00000014u, 0x800F0001u, 0x90E40000u, 0xA0E40000u, 0x00000001u, 0xC00F0000u, 0x80E40001u,
        0x00000002u, 0x80010002u, 0x81FF0001u, 0xA0AA0009u, 0x00000005u, 0x80010002u, 0x80000002u, 0xA0000009u,
        0x0000000Bu, 0x80010002u, 0x80000002u, 0xA0000005u, 0x0000000Au, 0xC0010001u, 0x80000002u, 0xA000000Au,
        0x00000001u, 0x800F0003u, 0x90E4000Fu, 0x00000005u, 0x800F0000u, 0x81D20003u, 0x90C90003u, 0x00000004u,
        0x800F0004u, 0x81C90003u, 0x90D20003u, 0x81E40000u, 0x00000008u, 0x80010006u, 0x80E40003u, 0xA0E40004u,
        0x00000008u, 0x80020006u, 0x80E40004u, 0xA0E40004u, 0x00000008u, 0x80040006u, 0x90E40003u, 0xA0E40004u,
        0x00000004u, 0xD0070001u, 0x80A40006u, 0xA0E40008u, 0xA0E40008u, 0x00000001u, 0xD00F0000u, 0x90E40005u,
        0x00000001u, 0xE0030000u, 0x90540007u, 0x00000001u, 0xE0030001u, 0x90540007u, 0x00000005u, 0xE00F0002u,
        0xA0E40006u, 0x90E40005u, 0x00000005u, 0xE00F0003u, 0xA0E40007u, 0x90E40005u, 0x00000003u, 0x800F0000u,
        0xA0E4000Bu, 0x90E40000u, 0x00000008u, 0x800F0001u, 0x80E40000u, 0x80E40000u, 0x00000007u, 0x800F0002u,
        0x80E40001u, 0x00000008u, 0x80010003u, 0x80E40000u, 0x90E40003u, 0x0000000Du, 0x800F0005u, 0xA0E40012u,
        0x80E40001u, 0x0000FFFFu,
    };
    uint32_t gd = galloc(sizeof decl), gc = galloc(sizeof code);
    memcpy(GUEST_PTR(gd), decl, sizeof decl);
    memcpy(GUEST_PTR(gc), code, sizeof code);
    uint32_t hr = call(g_dev, D_CreateVertexShader, 4, gd, gc, g_out, 0);
    uint32_t h = rd32(g_out);
    CHECK(hr == 0, "zone shader: CreateVertexShader %08x", hr);
    if (hr != 0)
        return;
    /* c0-c3 the identity (clip space in, out); the rest what the fog and light terms read */
    float c[19][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
    for (int i = 4; i < 19; ++i)
        c[i][0] = c[i][1] = c[i][2] = c[i][3] = 0.25f;
    uint32_t gk = galloc(sizeof c);
    memcpy(GUEST_PTR(gk), c, sizeof c);
    typedef struct
    {
        float x, y, z, nx, ny, nz;
        uint32_t c;
        float u, v;
    } Z;
    Z q[4] = { { 0, 0, 0.5f, 0, 1, 0, 0xFF2080C0u, 0, 0 }, { 1, 0, 0.5f, 0, 1, 0, 0xFF2080C0u, 1, 0 },
               { 0, -1, 0.5f, 0, 1, 0, 0xFF2080C0u, 0, 1 }, { 1, -1, 0.5f, 0, 1, 0, 0xFF2080C0u, 1, 1 } };
    uint32_t v = galloc(sizeof q);
    memcpy(GUEST_PTR(v), q, sizeof q);
    call(g_dev, D_SetRenderState, 2, 137, 0); /* LIGHTING off */
    call(g_dev, D_SetRenderState, 2, 22, 1);  /* CULL none */
    call(g_dev, D_SetTexture, 2, 0, 0);
    call(g_dev, D_SetTextureStageState, 3, 0, 1, 2 /* SELECTARG1 */);
    call(g_dev, D_SetTextureStageState, 3, 0, 2, 0 /* DIFFUSE */);
    call(g_dev, D_SetVertexShader, 1, h);
    call(g_dev, D_SetVertexShaderConstant, 3, 0, gk, 19);
    call(g_dev, D_BeginScene, 0);
    call(g_dev, D_DrawPrimitiveUP, 4, 5, 2, v, (uint32_t)sizeof(Z));
    call(g_dev, D_EndScene, 0);
    CHECK(near_rgb(pixel(20, 20), 0x2080C0) && pixel(4, 4) == 0, "zone shader: %06x %06x", pixel(20, 20), pixel(4, 4));
    call(g_dev, D_SetVertexShader, 1, 0x42);
    call(g_dev, D_DeleteVertexShader, 1, h);
}

/* a ps.1.1 shader: the diffuse color times c0 */
static void test_pixel_shader(void)
{
    clear(0xFF000000u);
    uint32_t code[] = {
        0xFFFF0101u,                                        /* ps.1.1 */
        0x00000005u, 0x800F0000u, 0x90E40000u, 0xA0E40000u, /* mul r0, v0, c0 */
        0x0000FFFFu,
    };
    uint32_t gc = galloc(sizeof code);
    memcpy(GUEST_PTR(gc), code, sizeof code);
    uint32_t hr = call(g_dev, D_CreatePixelShader, 2, gc, g_out);
    uint32_t h = rd32(g_out);
    CHECK(hr == 0 && h != 0, "pixel shader: CreatePixelShader %08x handle %08x", hr, h);
    if (hr != 0)
        return;
    float c0[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    uint32_t gk = galloc(sizeof c0);
    memcpy(GUEST_PTR(gk), c0, sizeof c0);
    uint32_t v = galloc(4 * sizeof(UiVert));
    UiVert q[4] = { { 0, 0, 0.5f, 1, 0xFFFFFFFFu, 0, 0 }, { 16, 0, 0.5f, 1, 0xFFFFFFFFu, 1, 0 }, { 0, 16, 0.5f, 1, 0xFFFFFFFFu, 0, 1 },
                    { 16, 16, 0.5f, 1, 0xFFFFFFFFu, 1, 1 } };
    memcpy(GUEST_PTR(v), q, sizeof q);
    call(g_dev, D_SetTexture, 2, 0, 0);
    call(g_dev, D_SetVertexShader, 1, 0x144);
    call(g_dev, D_SetPixelShader, 1, h);
    call(g_dev, D_SetPixelShaderConstant, 3, 0, gk, 1);
    call(g_dev, D_BeginScene, 0);
    call(g_dev, D_DrawPrimitiveUP, 4, 5, 2, v, (uint32_t)sizeof(UiVert));
    call(g_dev, D_EndScene, 0);
    call(g_dev, D_SetPixelShader, 1, 0);
    CHECK(pixel(8, 8) == 0x00FF00 && pixel(24, 24) == 0, "pixel shader: %06x %06x", pixel(8, 8), pixel(24, 24));
    CHECK(call(g_dev, D_DeletePixelShader, 1, h) == 0, "pixel shader: DeletePixelShader");
}

/* Copies as FFXI's occlusion probe makes them: a render target into a texture of the default pool,
 * which is then locked and read; and a corner of the back buffer into an image surface. */
static void test_copies(void)
{
    call(g_dev, D_CreateTexture, 7, 8, 8, 1, 1 /* RENDERTARGET */, 22, 0, g_out);
    uint32_t rt = rd32(g_out);
    call(rt, T_GetSurfaceLevel, 2, 0, g_out);
    uint32_t rts = rd32(g_out);
    call(g_dev, D_CreateTexture, 7, 8, 8, 1, 0, 22, 0 /* DEFAULT */, g_out);
    uint32_t copy = rd32(g_out);
    call(copy, T_GetSurfaceLevel, 2, 0, g_out);
    uint32_t cs = rd32(g_out);
    call(g_dev, D_SetRenderTarget, 2, rts, 0);
    clear(0xFF0080FFu);
    call(g_dev, D_SetRenderTarget, 2, g_bb, 0);
    uint32_t hr = call(g_dev, D_CopyRects, 5, rts, 0, 0, cs, 0);
    uint32_t lr = galloc(8);
    uint32_t lock = call(cs, S_LockRect, 3, lr, 0, 0x10 /* READONLY */);
    uint32_t got = lock == 0 ? rd32(rd32(lr + 4) + 5 * rd32(lr) + 4 * 5) & 0xFFFFFF : 0;
    if (lock == 0)
        call(cs, S_UnlockRect, 0);
    CHECK(hr == 0 && lock == 0 && got == 0x0080FF, "render target into a default-pool texture: CopyRects %08x, lock %08x, %06x", hr, lock, got);

    clear(0xFF405060u);
    call(g_dev, D_CreateImageSurface, 4, 8, 8, 22, g_out);
    uint32_t image = rd32(g_out);
    uint32_t rect = galloc(16), point = galloc(8);
    wr32(rect, 8), wr32(rect + 4, 8), wr32(rect + 8, 16), wr32(rect + 12, 16);
    wr32(point, 0), wr32(point + 4, 0);
    hr = call(g_dev, D_CopyRects, 5, g_bb, rect, 1, image, point);
    lock = call(image, S_LockRect, 3, lr, 0, 0x10);
    got = lock == 0 ? rd32(rd32(lr + 4) + 3 * rd32(lr) + 4 * 3) & 0xFFFFFF : 0;
    if (lock == 0)
        call(image, S_UnlockRect, 0);
    CHECK(hr == 0 && lock == 0 && got == 0x405060, "a corner of the back buffer into an image surface: CopyRects %08x, lock %08x, %06x", hr,
        lock, got);
    call(image, S_Release, 0);
    call(cs, S_Release, 0);
    call(copy, D_Release, 0);
    call(rts, S_Release, 0);
    call(rt, D_Release, 0);
}

/* State blocks carry the index buffer's base vertex and the vertex shader; a recorded one doesn't
 * change the device while it records. GetTexture gives back the texture that was set; the render
 * states Direct3D 9 dropped (ZBIAS) read back. */
static void test_state(void)
{
    call(g_dev, D_CreateIndexBuffer, 5, 6 * 2, 0x8, 101, 1, g_out);
    uint32_t ib = rd32(g_out);
    uint32_t base = galloc(4);
    call(g_dev, D_SetIndices, 2, ib, 0);
    call(g_dev, D_SetVertexShader, 1, 0x42);
    call(g_dev, D_CreateStateBlock, 2, 1 /* ALL */, g_out);
    uint32_t all = rd32(g_out);
    call(g_dev, D_SetIndices, 2, ib, 2);
    call(g_dev, D_SetVertexShader, 1, 0x144);
    call(g_dev, D_ApplyStateBlock, 1, all);
    call(g_dev, D_GetIndices, 2, g_out, base);
    call(rd32(g_out), D_Release, 0);
    CHECK(rd32(base) == 0, "state block: base vertex %u after ApplyStateBlock, not 0", rd32(base));
    call(g_dev, D_GetVertexShader, 1, g_out);
    CHECK(rd32(g_out) == 0x42, "state block: vertex shader %08x after ApplyStateBlock, not 42", rd32(g_out));

    call(g_dev, D_BeginStateBlock, 0);
    call(g_dev, D_SetIndices, 2, ib, 3);
    call(g_dev, D_EndStateBlock, 1, g_out);
    uint32_t recorded = rd32(g_out);
    call(g_dev, D_GetIndices, 2, g_out, base);
    call(rd32(g_out), D_Release, 0);
    CHECK(rd32(base) == 0, "state block: recording changed the base vertex to %u", rd32(base));
    call(g_dev, D_ApplyStateBlock, 1, recorded);
    call(g_dev, D_GetIndices, 2, g_out, base);
    call(rd32(g_out), D_Release, 0);
    CHECK(rd32(base) == 3, "state block: recorded base vertex %u, not 3", rd32(base));
    call(g_dev, D_DeleteStateBlock, 1, all);
    call(g_dev, D_DeleteStateBlock, 1, recorded);
    call(g_dev, D_SetIndices, 2, 0, 0);
    call(ib, D_Release, 0);

    call(g_dev, D_CreateTexture, 7, 4, 4, 1, 0, 21, 1, g_out);
    uint32_t tex = rd32(g_out);
    call(g_dev, D_SetTexture, 2, 0, tex);
    call(g_dev, D_GetTexture, 2, 0, g_out);
    CHECK(rd32(g_out) == tex, "GetTexture: %08x, not the texture set (%08x)", rd32(g_out), tex);
    if (rd32(g_out))
        call(rd32(g_out), D_Release, 0);
    call(g_dev, D_SetTexture, 2, 0, 0);
    call(tex, D_Release, 0);

    /* a texture's surface and the back buffer outlive the game's references (their container's
     * don't end): the game keeps using ones it released */
    call(g_dev, D_CreateTexture, 7, 8, 8, 1, 0, 21, 1, g_out);
    tex = rd32(g_out);
    call(tex, T_GetSurfaceLevel, 2, 0, g_out);
    uint32_t level = rd32(g_out);
    call(level, S_Release, 0);
    uint32_t desc = galloc(32);
    CHECK(call(level, 8 /* GetDesc */, 1, desc) == 0 && rd32(desc + 24) == 8, "a released texture surface: GetDesc");
    call(tex, T_GetSurfaceLevel, 2, 0, g_out);
    CHECK(rd32(g_out) == level, "GetSurfaceLevel again: %08x, not %08x", rd32(g_out), level);
    call(rd32(g_out), S_Release, 0);
    call(tex, D_Release, 0);
    call(g_dev, D_GetBackBuffer, 3, 0, 0, g_out);
    uint32_t back = rd32(g_out);
    call(back, S_Release, 0);
    CHECK(back == g_bb && call(back, 8, 1, desc) == 0 && rd32(desc + 24) == W, "a released back buffer: GetDesc");

    call(g_dev, D_SetRenderState, 2, 47 /* ZBIAS */, 5);
    call(g_dev, D_GetRenderState, 2, 47, g_out);
    CHECK(rd32(g_out) == 5, "ZBIAS reads back %u, not 5", rd32(g_out));
    call(g_dev, D_SetRenderState, 2, 47, 0);
}

typedef struct
{
    float x, y, z, rhw;
    uint32_t c;
} RhwVert;

/* [graphics] background: the scene's target and its depth surface are drawn at twice the size. FFXI's
 * sun probe then draws a quad at the sky's depth into another target of the same size, with the
 * scene's depth surface: what passes is where the sky is. That target has to be drawn at the scene's
 * size too, or its pixels meet the wrong ones of the depth surface (the sun went missing). */
static void test_background_depth(void)
{
    call(g_dev, D_CreateTexture, 7, W, H, 1, 1 /* RENDERTARGET */, 22, 0, g_out);
    uint32_t scene = rd32(g_out);
    call(scene, T_GetSurfaceLevel, 2, 0, g_out);
    uint32_t scene_surf = rd32(g_out);
    call(g_dev, 26 /* CreateDepthStencilSurface */, 5, W, H, 75 /* D24S8 */, 0, g_out);
    uint32_t depth = rd32(g_out);
    call(g_dev, D_CreateTexture, 7, W, H, 1, 1 /* RENDERTARGET */, 22, 0, g_out);
    uint32_t probe = rd32(g_out);
    call(probe, T_GetSurfaceLevel, 2, 0, g_out);
    uint32_t probe_surf = rd32(g_out);

    /* the world: its left half at depth 0.5, the right half sky (1.0) */
    uint32_t world = galloc(4 * 16);
    float wv[4][4] = { { -1, 1, 0.5f, 0 }, { 0, 1, 0.5f, 0 }, { -1, -1, 0.5f, 0 }, { 0, -1, 0.5f, 0 } };
    for (int i = 0; i < 4; ++i)
    {
        uint32_t c = 0xFF808080u;
        memcpy(&wv[i][3], &c, 4);
    }
    memcpy(GUEST_PTR(world), wv, sizeof wv);
    /* the probe: the whole target at the sky's depth */
    uint32_t sky = galloc(4 * sizeof(RhwVert));
    const float z = 0.99999994f;
    RhwVert sv[4] = { { 0, 0, z, 1, 0xFFFFFFFFu }, { W, 0, z, 1, 0xFFFFFFFFu }, { 0, H, z, 1, 0xFFFFFFFFu },
                      { W, H, z, 1, 0xFFFFFFFFu } };
    memcpy(GUEST_PTR(sky), sv, sizeof sv);

    call(g_dev, D_SetRenderState, 2, 137, 0); /* LIGHTING off */
    call(g_dev, D_SetRenderState, 2, 22, 1);  /* CULL none */
    call(g_dev, D_SetRenderState, 2, 7, 1);   /* ZENABLE */
    call(g_dev, D_SetTextureStageState, 3, 0, 1, 2 /* SELECTARG1 */);
    call(g_dev, D_SetTextureStageState, 3, 0, 2, 0 /* DIFFUSE */);
    call(g_dev, D_SetTexture, 2, 0, 0);
    d3d8_set_background(2 * W, 2 * H);
    /* the first frames find the world's target, which grows with its depth surface; the probe's target,
     * seen drawn with that depth surface, grows at the Present after; the last frame is read */
    for (int frame = 0; frame < 5; ++frame)
    {
        call(g_dev, D_BeginScene, 0);
        call(g_dev, D_SetRenderTarget, 2, scene_surf, depth);
        clear(0xFF000000u);
        call(g_dev, D_SetRenderState, 2, 14, 1); /* ZWRITEENABLE */
        call(g_dev, D_SetVertexShader, 1, 0x42);
        call(g_dev, D_DrawPrimitiveUP, 4, 5, 2, world, 16u);
        if (frame >= 2)
        {
            call(g_dev, D_SetRenderTarget, 2, probe_surf, depth);
            call(g_dev, D_Clear, 6, 0, 0, 1 /* TARGET */, 0xFF000000u, fbits(1.0f), 0);
            call(g_dev, D_SetRenderState, 2, 14, 0);
            call(g_dev, D_SetVertexShader, 1, 0x44);
            call(g_dev, D_DrawPrimitiveUP, 4, 5, 2, sky, (uint32_t)sizeof(RhwVert));
        }
        call(g_dev, D_EndScene, 0);
        call(g_dev, D_SetRenderTarget, 2, g_bb, 0);
        call(g_dev, D_Present, 4, 0, 0, 0, 0);
    }
    uint32_t lr = galloc(8);
    call(probe_surf, S_LockRect, 3, lr, 0, 0x10 /* READONLY */);
    uint32_t bits = rd32(lr + 4), pitch = rd32(lr);
    uint32_t left = rd32(bits + 16u * pitch + 4u * 4u) & 0xFFFFFF, right = rd32(bits + 16u * pitch + 4u * 27u) & 0xFFFFFF;
    call(probe_surf, S_UnlockRect, 0);
    CHECK(left == 0 && right == 0xFFFFFF, "background: the sky probe passes where the sky is: left %06x right %06x",
        left, right);
    uint32_t sw = 0, sh = 0;
    d3d8_scene_size(&sw, &sh);
    CHECK(sw == 2 * W && sh == 2 * H, "background: the 3D scene drawn at %ux%u", sw, sh);

    d3d8_set_background(0, 0);
    call(g_dev, D_SetRenderState, 2, 14, 1);
    call(g_dev, D_Present, 4, 0, 0, 0, 0);
    call(probe_surf, S_Release, 0);
    call(probe, D_Release, 0);
    call(depth, S_Release, 0);
    call(scene_surf, S_Release, 0);
    call(scene, D_Release, 0);
}

/* [graphics] characters.brightness (d3d8_set_character_light): a lit draw from a dynamic buffer with no world matrix (a
 * character, skinned on the CPU) is lit that much more; the same from a static buffer, or placed by a world matrix (the
 * zone's lit objects), is not. The light: ambient 0x40 and a directional light of 0.25 straight onto the quad, 0x80 in all. */
static void test_character_light(void)
{
    call(g_dev, D_SetRenderState, 2, 137, 1);          /* LIGHTING */
    call(g_dev, D_SetRenderState, 2, 141, 0);          /* COLORVERTEX off: the material's colours */
    call(g_dev, D_SetRenderState, 2, 139, 0xFF404040u); /* AMBIENT */
    call(g_dev, D_SetRenderState, 2, 22, 1);           /* CULL none */
    call(g_dev, D_SetRenderState, 2, 7, 0);            /* ZENABLE off */
    call(g_dev, D_SetTextureStageState, 3, 0, 1, 2 /* SELECTARG1 */);
    call(g_dev, D_SetTextureStageState, 3, 0, 2, 0 /* DIFFUSE */);
    call(g_dev, D_SetTexture, 2, 0, 0);
    /* D3DMATERIAL8: diffuse, ambient white */
    uint32_t mat = galloc(68);
    float m[17] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    memcpy(GUEST_PTR(mat), m, sizeof m);
    call(g_dev, 42 /* SetMaterial */, 1, mat);
    /* D3DLIGHT8: directional, diffuse 0.25, along +z (onto the quad, whose normal faces -z) */
    uint32_t light = galloc(104);
    float l[26] = { 0 };
    uint32_t directional = 3;
    memcpy(&l[0], &directional, 4);
    l[1] = l[2] = l[3] = 0.25f, l[4] = 1.0f, l[18] = 1.0f;
    memcpy(GUEST_PTR(light), l, sizeof l);
    call(g_dev, 44 /* SetLight */, 2, 0, light);
    call(g_dev, 46 /* LightEnable */, 2, 0, 1);
    /* XYZ | NORMAL: the top-left quarter, at x0 (moved back by a world matrix's x0 for the placed one) */
    uint32_t pp = galloc(4), vb[2];
    for (int k = 0; k < 2; ++k)
    {
        call(g_dev, D_CreateVertexBuffer, 5, 4 * 24, k ? 0x8u : 0x208u /* WRITEONLY, DYNAMIC */, 0x12, k ? 1u : 0u, g_out);
        vb[k] = rd32(g_out);
    }
    float world[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    uint32_t gw = galloc(64);
    const char* what[3] = { "a character (dynamic, no world matrix)", "a static buffer", "placed by a world matrix" };
    uint32_t got[3][3];
    const float bright[3] = { 1.0f, 1.5f, 0.5f };
    for (int lit = 0; lit < 3; ++lit)
    {
        d3d8_set_character_light(bright[lit]);
        for (int k = 0; k < 3; ++k)
        {
            const float x0 = k == 2 ? -0.5f : 0.0f;
            call(vb[k == 1], B_Lock, 4, 0, 0, pp, k == 1 ? 0u : 0x2000u /* DISCARD */);
            float v[4][6] = { { -1 + x0, 1, 0.5f, 0, 0, -1 }, { x0, 1, 0.5f, 0, 0, -1 }, { -1 + x0, 0, 0.5f, 0, 0, -1 },
                              { x0, 0, 0.5f, 0, 0, -1 } };
            memcpy(GUEST_PTR(rd32(pp)), v, sizeof v);
            call(vb[k == 1], B_Unlock, 0);
            world[12] = -x0;
            memcpy(GUEST_PTR(gw), world, sizeof world);
            call(g_dev, D_SetTransform, 2, 256 /* WORLD */, gw);
            clear(0xFF000000u);
            call(g_dev, D_BeginScene, 0);
            call(g_dev, D_SetVertexShader, 1, 0x12);
            call(g_dev, D_SetStreamSource, 3, 0, vb[k == 1], 24);
            call(g_dev, D_DrawPrimitive, 3, 5, 0, 2);
            call(g_dev, D_EndScene, 0);
            got[k][lit] = pixel(4, 4);
        }
    }
    for (int k = 0; k < 3; ++k)
    {
        const int g0 = (int)((got[k][0] >> 8) & 255), g1 = (int)((got[k][1] >> 8) & 255);
        CHECK(g0 >= 126 && g0 <= 130, "character light, %s, as the game lights it: %06x (want 808080)", what[k], got[k][0]);
        const int g2 = (int)((got[k][2] >> 8) & 255);
        if (k == 0)
        {
            CHECK(g1 >= 189 && g1 <= 194, "character light 1.5, %s: %06x (want c0c0c0)", what[k], got[k][1]);
            CHECK(g2 >= 62 && g2 <= 66, "character light 0.5, %s: %06x (want 404040: the finished colour dimmed)", what[k], got[k][2]);
        }
        else
        {
            CHECK(g1 == g0, "character light 1.5, %s: %06x (want it as it was, %06x)", what[k], got[k][1], got[k][0]);
            CHECK(g2 == g0, "character light 0.5, %s: %06x (want it as it was, %06x)", what[k], got[k][2], got[k][0]);
        }
    }
    d3d8_set_character_light(1.0f);
    world[12] = 0.0f;
    memcpy(GUEST_PTR(gw), world, sizeof world);
    call(g_dev, D_SetTransform, 2, 256, gw);
    call(g_dev, 46 /* LightEnable */, 2, 0, 0);
    call(g_dev, D_SetRenderState, 2, 139, 0);
    call(g_dev, D_SetRenderState, 2, 141, 1);
    call(g_dev, D_SetRenderState, 2, 137, 0);
    call(g_dev, D_SetStreamSource, 3, 0, 0, 0);
    call(vb[0], D_Release, 0);
    call(vb[1], D_Release, 0);
}

#include "gfx_backend.h" /* gfx_selected_d3d11, gfx12_lose_device */

/* The device lost (gfx12_lose_device: ID3D12Device5::RemoveDevice, as a driver timeout) and made again under the game
 * (vanacore_gfx_device_recover, as graphics::close_if_lost does it): a managed texture the game filled and a static vertex
 * buffer draw as before on the new device, from guest memory, and the back buffer reads back */
static void test_device_lost(void)
{
    /* a 2x2 texture filled through LockRect, drawn over 16x16 (as test_up_texture) */
    call(g_dev, D_CreateTexture, 7, 2, 2, 1, 0, 21, 1 /* MANAGED */, g_out);
    uint32_t tex = rd32(g_out);
    uint32_t lr = galloc(8);
    call(tex, T_LockRect, 4, 0, lr, 0, 0);
    uint32_t bits = rd32(lr + 4), pitch = rd32(lr);
    wr32(bits, 0xFFFF0000u), wr32(bits + 4, 0xFF00FF00u), wr32(bits + pitch, 0xFF0000FFu), wr32(bits + pitch + 4, 0xFFFFFFFFu);
    call(tex, T_UnlockRect, 1, 0);
    uint32_t v = galloc(4 * sizeof(UiVert));
    UiVert q[4] = { { 0, 0, 0.5f, 1, 0xFFFFFFFFu, 0, 0 }, { 16, 0, 0.5f, 1, 0xFFFFFFFFu, 1, 0 }, { 0, 16, 0.5f, 1, 0xFFFFFFFFu, 0, 1 },
                    { 16, 16, 0.5f, 1, 0xFFFFFFFFu, 1, 1 } };
    memcpy(GUEST_PTR(v), q, sizeof q);
    /* and a static vertex buffer: XYZ | DIFFUSE, orange, the top-left quarter (as test_buffers) */
    call(g_dev, D_CreateVertexBuffer, 5, 4 * 16, 0x8, 0x42, 1, g_out);
    uint32_t vb = rd32(g_out);
    uint32_t pp = galloc(4);
    call(vb, B_Lock, 4, 0, 0, pp, 0);
    float vs[4][4] = { { -1, 1, 0.5f, 0 }, { 0, 1, 0.5f, 0 }, { -1, 0, 0.5f, 0 }, { 0, 0, 0.5f, 0 } };
    for (int i = 0; i < 4; ++i)
    {
        uint32_t c = 0xFFFF8000u;
        memcpy(&vs[i][3], &c, 4);
    }
    memcpy(GUEST_PTR(rd32(pp)), vs, sizeof vs);
    call(vb, B_Unlock, 0);
    call(g_dev, D_SetRenderState, 2, 137, 0); /* LIGHTING off */
    call(g_dev, D_SetRenderState, 2, 22, 1);  /* CULL none */
    call(g_dev, D_SetTextureStageState, 3, 0, 16, 1); /* MAGFILTER, MINFILTER point */
    call(g_dev, D_SetTextureStageState, 3, 0, 17, 1);
    for (int pass = 0; pass < 2; ++pass)
    {
        if (pass)
        {
            CHECK(gfx12_lose_device(), "device lost: the device couldn't be removed");
            CHECK(vanacore_gfx_device_lost(), "device lost: not found lost");
            CHECK(vanacore_gfx_device_recover() == 1, "device lost: not made again on Direct3D 12");
            CHECK(!vanacore_gfx_device_lost(), "device lost: still lost after the device was made again");
        }
        clear(0xFF000000u);
        call(g_dev, D_SetTextureStageState, 3, 0, 1, 2 /* SELECTARG1 */);
        call(g_dev, D_SetTextureStageState, 3, 0, 2, 2 /* TEXTURE */);
        call(g_dev, D_SetTexture, 2, 0, tex);
        call(g_dev, D_SetVertexShader, 1, 0x144);
        call(g_dev, D_BeginScene, 0);
        call(g_dev, D_DrawPrimitiveUP, 4, 5 /* TRIANGLESTRIP */, 2, v, (uint32_t)sizeof(UiVert));
        call(g_dev, D_EndScene, 0);
        CHECK(pixel(2, 2) == 0xFF0000 && pixel(12, 2) == 0x00FF00 && pixel(2, 12) == 0x0000FF && pixel(12, 12) == 0xFFFFFF,
            "device lost, pass %d: the texture: %06x %06x %06x %06x", pass, pixel(2, 2), pixel(12, 2), pixel(2, 12), pixel(12, 12));
        clear(0xFF000000u);
        call(g_dev, D_SetTexture, 2, 0, 0);
        call(g_dev, D_SetTextureStageState, 3, 0, 2, 0 /* DIFFUSE */);
        call(g_dev, D_SetVertexShader, 1, 0x42);
        call(g_dev, D_SetStreamSource, 3, 0, vb, 16);
        call(g_dev, D_BeginScene, 0);
        call(g_dev, D_DrawPrimitive, 3, 5, 0, 2);
        call(g_dev, D_EndScene, 0);
        CHECK(pixel(4, 4) == 0xFF8000 && pixel(20, 20) == 0, "device lost, pass %d: the static buffer: %06x %06x", pass, pixel(4, 4),
            pixel(20, 20));
        call(g_dev, D_SetStreamSource, 3, 0, 0, 0);
    }
    call(tex, D_Release, 0);
    call(vb, D_Release, 0);
}

/* renderer=d3d9's IDirect3D8 (d3d9_test.cpp) */
void* vanacore_d3d9_test_create(void);
#endif

/* The tests on one IDirect3D8; d3d12: VanaCore's own on Direct3D 12 (d3d8.c), which has tests of its own too */
static int run_tests(uint32_t d3d, const char* name, int d3d12)
{
    g_fails = 0;
    /* D3DPRESENT_PARAMETERS: 32x32 X8R8G8B8, windowed, auto depth D24S8 */
    uint32_t pp = galloc(52);
    uint32_t v[13] = { W, H, 22, 1, 0, 1, 0, 1, 1, 75, 0, 0, 0 };
    for (int i = 0; i < 13; ++i)
        wr32(pp + 4u * (uint32_t)i, v[i]);
    uint32_t hr = call(d3d, 15 /* CreateDevice */, 6, 0, 1, 0, 0x40, pp, g_out);
    g_dev = rd32(g_out);
    CHECK(hr == 0 && g_dev, "CreateDevice: %08x", hr);
    if (!g_dev)
        return 1;
    call(g_dev, D_GetBackBuffer, 3, 0, 0, g_out);
    g_bb = rd32(g_out);

    test_clear_and_lock();
    test_up_texture();
    test_buffers();
    test_declaration();
    test_five_streams();
    test_copyrects();
    if (d3d12)
        test_scaled_copy();
    if (d3d12)
        test_texture_pack();
#ifdef VANACORE
    test_vertex_shader();
    test_zone_shader();
    test_pixel_shader();
    test_copies();
    test_state();
    if (d3d12)
        test_background_depth();
    if (d3d12)
        test_character_light();
    if (d3d12 && !gfx_selected_d3d11())
        test_device_lost();
#endif
    call(g_dev, D_Present, 4, 0, 0, 0, 0);
#ifdef VANACORE
    if (!d3d12)
    {
        /* as the game at its exit: the device let go before the last of its resources (one still set) */
        call(g_dev, D_CreateTexture, 7, 8, 8, 1, 0, 21, 1, g_out);
        uint32_t tex = rd32(g_out);
        call(tex, T_GetSurfaceLevel, 2, 0, g_out);
        uint32_t level = rd32(g_out);
        call(g_dev, D_SetTexture, 2, 0, tex);
        call(g_bb, S_Release, 0);
        call(g_dev, D_Release, 0);
        call(level, S_Release, 0);
        call(tex, D_Release, 0);
    }
#endif
    printf(g_fails ? "d3d8_test (%s): %d failed\n" : "d3d8_test (%s): ok\n", name, g_fails);
    return g_fails != 0;
}

#ifdef VANACORE
int d3d8_test_main(void)
{
    gfx_set_sync_pipelines(1);
    g_out = galloc(16);
    uint32_t d3d = (uint32_t)(uintptr_t)vanacore_gfx_direct3d_create();
    if (!d3d)
    {
        printf("d3d8_test: no Direct3D 12\n");
        return 1;
    }
    int failed = run_tests(d3d, gfx_selected_d3d11() ? "Direct3D 11" : "Direct3D 12", 1);
    /* renderer=d3d9: the same Direct3D 8 calls through Direct3D 9 */
    uint32_t d3d9 = (uint32_t)(uintptr_t)vanacore_d3d9_test_create();
    if (!d3d9)
    {
        printf("d3d8_test: no Direct3D 9\n");
        return 1;
    }
    failed |= run_tests(d3d9, "Direct3D 9", 0);
    return failed;
}
#else
int main(void)
{
    if (!gwin_init())
    {
        printf("cannot reserve the guest window\n");
        return 1;
    }
    gt_init();
    rt_set_native_handler(thunk_dispatch);
    gfx_set_sync_pipelines(1);
    d3d8_init();
    d3d8_setup();
    g_out = galloc(16);

    uint32_t arg = 220;
    return run_tests(guest_call(thunk_for("d3d8.dll", "Direct3DCreate8"), 1, &arg), "d3d8", 1);
}
#endif
