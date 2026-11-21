/* VanaCore.GfxTest: the graphics library's offscreen tests in a 32-bit process, as it runs in the
 * game - the Direct3D 12 back end alone (gfx_test.c), then Direct3D 8 on it through its COM
 * vtables (d3d8_test.c), and the same Direct3D 8 tests on renderer=d3d9 (src/core/d3d9.cpp). Both
 * are upstream's tests (xi-on-mac tests/), with VanaCore's additions.
 *
 * usage: VanaCore.GfxTest.exe          (exit status 0 when every check passes)
 *        VanaCore.GfxTest.exe d3d11    the same on the Direct3D 11 back end (renderer=d3d11)
 *        VanaCore.GfxTest.exe framegen DLSS frame generation alone, on a hidden window's swap chain (an RTX 40 or
 *                                      later; skipped elsewhere) */
#include <stdio.h>
#include <string.h>

#include <windows.h>

#include "vanacore_gfx.h"

int gfx_test_main(void);
int d3d8_test_main(void);
int gfx_test_framegen_main(void* window, void (*pump)(void));

static void pump(void)
{
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&m);
}

/* frame generation's test: a window for its swap chain, never shown */
static int framegen(void)
{
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"VanaCoreGfxTest";
    RegisterClassW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"VanaCore.GfxTest", WS_OVERLAPPEDWINDOW, 0, 0, 640, 400, NULL, NULL,
        wc.hInstance, NULL);
    if (!window)
    {
        printf("framegen: no window\n");
        return 1;
    }
    int failed = gfx_test_framegen_main(window, pump);
    printf(failed ? "VanaCore.GfxTest: FAILED\n" : "VanaCore.GfxTest: all passed\n");
    return failed;
}

static void print_line(const char* line) { printf("  [gfx] %s\n", line); }

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0); /* every line out before a crash */
    vanacore_gfx_set_log(print_line);
    if (argc > 1 && !strcmp(argv[1], "framegen"))
        return framegen();
    if (argc > 1 && !strcmp(argv[1], "d3d11"))
    {
        vanacore_gfx_set_d3d11(1);
        printf("VanaCore.GfxTest: on Direct3D 11\n");
    }
    int failed = gfx_test_main();
    vanacore_gfx_fx_set("fx", 0.0f); /* gfx_test turned the scene effects on; upstream runs each test alone */
    failed |= d3d8_test_main();
    printf(failed ? "VanaCore.GfxTest: FAILED\n" : "VanaCore.GfxTest: all passed\n");
    return failed;
}
