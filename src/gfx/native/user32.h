/* The window calls d3d8.c makes, for the game's real window (native.c). Same names as upstream
 * (xi-on-mac runtime/portable/user32.h, which emulates USER32 on SDL3); HWNDs are the game's own. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A full-screen device (D3D8 Windowed = FALSE) on hwnd: its window covers its monitor, borderless,
 * in the desktop's own mode (the swap chain scales the back buffer to it). */
void user32_set_fullscreen(uint32_t hwnd, int on);
/* What gfx_init takes for hwnd's window: the HWND itself (gfx_d3d12.c's VANACORE build). */
void* user32_sdl_window(uint32_t hwnd);
/* A window's client size (unchanged if hwnd is no window or has no size); the desktop's mode. */
void user32_client_size(uint32_t hwnd, uint32_t* w, uint32_t* h);
/* Where hwnd's client area starts on the desktop (0, 0 if hwnd is no window). */
void user32_client_origin(uint32_t hwnd, int32_t* x, int32_t* y);
void user32_desktop_mode(uint32_t* w, uint32_t* h, uint32_t* hz);
/* [ui] aspect: the game's screen-space draws (its interface) keep width / height = aspect,
 * centered in a wider window, and the mouse is mapped to match. 0 (the default) is off. */
void user32_set_ui_aspect(float aspect);
/* The size the window was made at (its client area as asked for), which the interface's shape is fitted to in place of
 * the client area's: a window whose title bar and borders didn't fit the screen has a shorter client area than it was
 * made with, and it's still the game's resolution. 0, 0: the client area's. */
void user32_set_window_size(uint32_t w, uint32_t h);
/* The fraction of hwnd's width the interface keeps: 1 when off, or the window is not wider. */
float user32_ui_squeeze(uint32_t hwnd);
/* ... and its height, for a window taller than the interface's shape (1: not squeezed) */
float user32_ui_squeeze_y(uint32_t hwnd);
/* Set by d3d8.c: whether the interface covers this point of the window (0..1 across and down), as
 * drawn last frame. The mouse is unsqueezed only there. */
extern int (*user32_ui_hit)(float fx, float fy);
/* 1 when the game was last given the cursor as it is (over the world), 0 unsqueezed */
int user32_mouse_raw(void);
/* where the game was last given the cursor, 0..1 of its window across and down */
void user32_mouse_given(float* fx, float* fy);
/* The client x to give the game for the cursor at client (x, y) of hwnd: unsqueezed over the
 * interface, as it is over the world. `held`: a mouse button is down (a press keeps its mode until
 * the buttons are up, so a window dragged out over the world keeps following). */
int user32_unsqueeze_x(uint32_t hwnd, int x, int y, int held);
/* Both: *x, *y (client pixels) made the point to give the game, unsqueezed across and down */
void user32_unsqueeze(uint32_t hwnd, int* x, int* y, int held);

#ifdef __cplusplus
}
#endif
