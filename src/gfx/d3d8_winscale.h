/* d3d8.c's include unit: [client] <id>.scale, a game window drawn bigger or smaller (hudwin.cpp's table, the HUD Manager's
 * Scale). While chat.cpp's window draw hook draws one (d3d8_window_scale, nested), each of its screen-space (XYZRHW) draws is
 * mapped x -> o + (x - o) s about the window's anchor point o (its top right corner for one anchored there), by its viewport
 * (ui_map_axis) as the aspect's squeeze is: its vertices, the clip the game gave it (a list's rows) and True Font's letters
 * (laid out by the draw's mapping) all go with it, and the vertices themselves are left as the game made them, so what
 * looks at them (window_alpha, window.corners) sees the window as laid out. What the game asks of the mouse is mapped back
 * by hudwin (IsOnWindow, IsOnButton). */

#define WS_NEST 8
/* the mapping at each depth (0: none), the windows' own composed: x -> a + b x */
static struct
{
    int depth;
    float a[WS_NEST + 1][2];
    float b[WS_NEST + 1];
} g_ws = { 0, { { 0.0f, 0.0f } }, { 1.0f } };

void d3d8_window_scale(float s, float ox, float oy)
{
    if (!(s > 0.0f))
    {
        if (g_ws.depth > 0)
            --g_ws.depth;
        return;
    }
    const int out = g_ws.depth < WS_NEST ? g_ws.depth : WS_NEST;
    ++g_ws.depth;
    if (g_ws.depth > WS_NEST)
        return; /* (deeper than kept: the outer one's mapping stays) */
    const float b = g_ws.b[out];
    g_ws.a[g_ws.depth][0] = g_ws.a[out][0] + b * ox * (1.0f - s);
    g_ws.a[g_ws.depth][1] = g_ws.a[out][1] + b * oy * (1.0f - s);
    g_ws.b[g_ws.depth] = b * s;
}

/* The mapping now: 0 for none */
static int ws_now(float a[2], float* b)
{
    if (g_ws.depth <= 0)
        return 0;
    const int at = g_ws.depth < WS_NEST ? g_ws.depth : WS_NEST;
    a[0] = g_ws.a[at][0], a[1] = g_ws.a[at][1], *b = g_ws.b[at];
    return *b != 1.0f || a[0] != 0.0f || a[1] != 0.0f;
}

/* draw_packet: a screen-space draw of a scaled window mapped to where it's drawn */
static void ws_apply(GfxDraw* d)
{
    float a[2], b;
    if (!d->vs.rhw || !ws_now(a, &b))
        return;
    ui_map_axis(d, 0, a[0], b);
    ui_map_axis(d, 1, a[1], b);
}

/* A box (x0, y0, x1, y1) of the game's units drawn now taken to where it's drawn */
static void ws_box(float box[4])
{
    float a[2], b;
    if (!ws_now(a, &b))
        return;
    for (int i = 0; i < 4; ++i)
        box[i] = a[i & 1] + b * box[i];
}

/* Present: the next frame's windows (a draw left open by a fault doesn't carry over) */
static void ws_present(void) { g_ws.depth = 0; }
