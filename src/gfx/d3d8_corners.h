/* d3d8.c's include unit: [client] window.corners and window.corners.size, the windows' corners cut across (cut) or rounded
 * (round), window.corners.size of the interface's units from each corner. Only on a window whose frame is drawn as a solid
 * line (window.border.solid, the chat's, the chat line's) or not at all (its border opacity 0): the game's own frame has its
 * corners in its picture. window_alpha marks the draw (g_wc) and draw_packet's wc_apply draws it as triangles in its place:
 *  - a background's quad as the shape's polygon, with the colour and texture coordinates the quad has at each point;
 *  - a frame's corner piece as the line round its corner (down its side as far as the piece went, and two pixels on along
 *    both edges, so the edges' own pieces meet it on a straight), the edges' pieces kept clear of the corners (wc_solid).
 * The shape's edges are smoothed: the alpha fades out across a pixel (a background's from half a pixel in to half a pixel
 * out; a line's from half a pixel out to a pixel and a half in, full a half pixel in), which pixels along an edge on their
 * own boundaries sample at the fade's ends, so the straight edges come out as sharp as the game's. tips_gradient
 * (d3d8_tips.h) keeps the item details' gradient inside the shape (wc_band). */

#define WC_ARC 17                /* a corner's points at most (16 segments) */
#define WC_MOST (4 * WC_ARC)     /* the shape's */
#define WC_PATH (2 * WC_ARC + 6) /* a corner piece's line: two corners, their ends' straights, the side's stretch */

static int g_corner_shape;         /* 0 square (the game's), 1 cut across, 2 round */
static float g_corner_size = 6.0f; /* in the interface's units */

void d3d8_set_window_corners(int shape, float size)
{
    g_corner_shape = shape == 1 || shape == 2 ? shape : 0;
    g_corner_size = !(size >= 1.0f) ? 1.0f : size > 32.0f ? 32.0f : size;
}

/* The draw window_alpha marked (cleared for each draw by draw_packet) */
static struct
{
    int kind;               /* 0 none; 1 a window's background, drawn in the shape; 2 a frame's corner piece, as the line round it */
    float r[4];             /* the window's background: left, top, right, bottom */
    float c;                /* (1) the corners' size on it */
    float px[2];            /* one of the target's pixels in the interface's units, across and down */
    int right, top, bottom; /* (2) the piece's side (0 the left) and which of its corners it has */
    float reach[2];         /* (2) how far down its side the line goes from the top corner, and how far up from the bottom one */
    uint32_t colour;        /* (2) the line's colour and alpha */
    uint32_t quad;          /* (1) the background's four vertices as window_alpha left them (tips_gradient lays its own over them) */
    float in[WC_MOST][2];   /* (1) the shape half a pixel in from its edge, clockwise (tips_gradient keeps inside it) */
    int n_in;
} g_wc;

typedef struct
{
    float x, y, z, rhw; /* (a window's vertex: XYZRHW, diffuse, one texture's coordinates; 28 bytes) */
    uint32_t c;
    float u, v;
} WcVert;

static uint8_t g_wc_out[9 * WC_MOST * 28]; /* the triangles wc_apply draws in a quad's place */

/* Corners on a window here: the chat line's (in_input), a chat window's (in_chat) or any other's */
static int wc_on(int in_input, int in_chat)
{
    if (!g_corner_shape)
        return 0;
    const int solid = in_input ? g_input_border_solid : in_chat ? g_chat_border_solid : g_border_solid;
    const float a = in_input ? g_input_border_alpha : in_chat ? g_chat_border_alpha : g_border_alpha;
    return solid || a < 0.004f;
}

/* One of the draw's target's pixels in the interface's units (the target can be made bigger than the game asked: the draw's
 * viewport in its pixels over the game's) */
static void wc_pixel(const GfxDraw* dr, float px[2])
{
    for (int c = 0; c < 2; ++c)
    {
        const float k = dr->u.vp[2 + c] > 0 ? (float)dr->vp[2 + c] / dr->u.vp[2 + c] : 1.0f;
        px[c] = k > 0.1f ? 1.0f / k : 1.0f;
    }
}

/* The corners' size on window r (at most half its width or height: a pill), and (ext) how far past them along its edges a
 * corner's line goes: two pixels, short of the edge's middle */
static float wc_size(const float* r, const float px[2], float* ext)
{
    const float w = r[2] - r[0], h = r[3] - r[1];
    const float half = (w < h ? w : h) * 0.5f, c = g_corner_size < half ? g_corner_size : half;
    if (ext)
    {
        ext[0] = fminf(2.0f * px[0], fmaxf(0.0f, w * 0.5f - c));
        ext[1] = fminf(2.0f * px[1], fmaxf(0.0f, h * 0.5f - c));
    }
    return c;
}

/* A round corner's segments: one every 2.5 pixels or so of its radius (a cut's one) */
static int wc_segments(float c, const float px[2])
{
    if (g_corner_shape == 1)
        return 1;
    const float p = (px[0] + px[1]) * 0.5f;
    const int s = (int)(c / (p > 0.01f ? p : 1.0f) * 0.4f) + 2;
    return s < 3 ? 3 : s > WC_ARC - 1 ? WC_ARC - 1 : s;
}

/* A point onto p (m so far), unless it's the last one again. The new count */
static int wc_add(float (*p)[2], int m, float x, float y)
{
    if (m > 0 && fabsf(x - p[m - 1][0]) + fabsf(y - p[m - 1][1]) < 1e-3f)
        return m;
    p[m][0] = x, p[m][1] = y;
    return m + 1;
}

/* Corner k's points (0 the top left, 1 top right, 2 bottom right, 3 bottom left) clockwise onto p (m so far): the cut's two
 * ends or the round's arc, and with `ext` a point that far along the edge before and after it. The new count */
static int wc_corner(int k, const float* r, float c, const float px[2], const float* ext, float (*p)[2], int m)
{
    static const float ax[4] = { 1.0f, 0.0f, -1.0f, 0.0f }, ay[4] = { 0.0f, 1.0f, 0.0f, -1.0f };
    const int segs = wc_segments(c, px), q0 = (k + 2) & 3, q1 = (k + 3) & 3; /* its ends' directions from its centre */
    const float cx = k == 0 || k == 3 ? r[0] + c : r[2] - c, cy = k < 2 ? r[1] + c : r[3] - c;
    for (int i = -1; i <= segs + 1; ++i)
    {
        const int beyond = i < 0 || i > segs;
        if (beyond && !ext)
            continue;
        float x, y;
        if (i <= 0 || i >= segs) /* an end, exactly on its edge; beyond it along the edge, the other end's way back */
        {
            const int q = i <= 0 ? q0 : q1, o = i <= 0 ? q1 : q0;
            x = cx + c * ax[q] - (beyond ? ext[0] * ax[o] : 0.0f);
            y = cy + c * ay[q] - (beyond ? ext[1] * ay[o] : 0.0f);
        }
        else
        {
            const float t = 1.5707964f * ((float)(k + 2) + (float)i / (float)segs);
            x = cx + c * cosf(t), y = cy + c * sinf(t);
        }
        m = wc_add(p, m, x, y);
    }
    return m;
}

/* The unit normal of segment a - b, pointing into window r */
static void wc_inward(const float* a, const float* b, const float* r, float n[2])
{
    float x = a[1] - b[1], y = b[0] - a[0];
    const float len = sqrtf(x * x + y * y);
    n[0] = n[1] = 0.0f;
    if (!(len > 1e-6f))
        return;
    x /= len, y /= len;
    if (x * ((r[0] + r[2]) - (a[0] + b[0])) + y * ((r[1] + r[3]) - (a[1] + b[1])) < 0.0f)
        x = -x, y = -y;
    n[0] = x, n[1] = y;
}

/* Each of p's m points' inward offset: its two segments' normals met at a mitre (closed: the shape, round to its start; else a
 * line, square to its end segments) */
static void wc_normals(float (*p)[2], int m, int closed, const float* r, float (*nm)[2])
{
    for (int i = 0; i < m; ++i)
    {
        float a[2] = { 0.0f, 0.0f }, b[2] = { 0.0f, 0.0f };
        if (closed || i > 0)
            wc_inward(p[(i + m - 1) % m], p[i], r, a);
        if (closed || i < m - 1)
            wc_inward(p[i], p[(i + 1) % m], r, b);
        if (a[0] == 0.0f && a[1] == 0.0f)
            a[0] = b[0], a[1] = b[1];
        if (b[0] == 0.0f && b[1] == 0.0f)
            b[0] = a[0], b[1] = a[1];
        float x = a[0] + b[0], y = a[1] + b[1];
        const float len = sqrtf(x * x + y * y);
        x = len > 1e-6f ? x / len : 0.0f, y = len > 1e-6f ? y / len : 0.0f;
        float d = x * a[0] + y * a[1]; /* the half turn's cosine: the mitre that much longer (twice at most) */
        d = d < 0.5f ? 0.5f : d;
        nm[i][0] = x / d, nm[i][1] = y / d;
    }
}

static uint32_t wc_mix(uint32_t a, uint32_t b, float t)
{
    uint32_t o = 0;
    for (int s = 0; s < 32; s += 8)
    {
        const float x = (float)(a >> s & 0xFFu) + ((float)(b >> s & 0xFFu) - (float)(a >> s & 0xFFu)) * t;
        o |= (uint32_t)(x + 0.5f) << s;
    }
    return o;
}

static void wc_lerp(const WcVert* a, const WcVert* b, float t, WcVert* o)
{
    o->x = a->x + (b->x - a->x) * t, o->y = a->y + (b->y - a->y) * t;
    o->z = a->z + (b->z - a->z) * t, o->rhw = a->rhw + (b->rhw - a->rhw) * t;
    o->u = a->u + (b->u - a->u) * t, o->v = a->v + (b->v - a->v) * t;
    o->c = wc_mix(a->c, b->c, t);
}

/* Vertex v at (x, y), its alpha times f, written at `out`. Past it */
static uint8_t* wc_put(uint8_t* out, const WcVert* v, float x, float y, float f)
{
    WcVert o = *v;
    o.x = x, o.y = y;
    o.c = (o.c & 0xFFFFFFu) | (uint32_t)((float)(o.c >> 24) * f + 0.5f) << 24;
    memcpy(out, &o, 28);
    return out + 28;
}

/* The background's vertex at (x, y): its quad's corners' (q: top left, top right, bottom left, bottom right) blended there */
static void wc_sample(const WcVert* q, const float* r, float x, float y, WcVert* o)
{
    float s = (x - r[0]) / (r[2] - r[0]), t = (y - r[1]) / (r[3] - r[1]);
    s = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    WcVert top, bottom;
    wc_lerp(&q[0], &q[1], s, &top);
    wc_lerp(&q[2], &q[3], s, &bottom);
    wc_lerp(&top, &bottom, t, o);
}

/* (1) The background's quad at `data` as the shape: its polygon half a pixel in, and a ring faded out to half a pixel out.
 * Its vertices (0: not a quad lying square) */
static uint32_t wc_background(uint32_t data)
{
    const float* r = g_wc.r;
    WcVert v[4], q[4], at[WC_MOST], out_at[WC_MOST];
    float o[WC_MOST][2] = {{0.0f}}, nm[WC_MOST][2], out[WC_MOST][2]; /* (o set: cppcheck saw it read unset) */
    int have = 0;
    memcpy(v, GUEST_PTR(data), sizeof v);
    for (int i = 0; i < 4; ++i)
    {
        const int k = (v[i].x > (r[0] + r[2]) * 0.5f) | (v[i].y > (r[1] + r[3]) * 0.5f) << 1;
        q[k] = v[i], have |= 1 << k;
    }
    int m = 0;
    if (have == 15 && r[2] - r[0] > 1.0f && r[3] - r[1] > 1.0f)
        for (int k = 0; k < 4; ++k)
            m = wc_corner(k, r, g_wc.c, g_wc.px, NULL, o, m);
    if (m < 3)
        return 0;
    const int n = fabsf(o[0][0] - o[m - 1][0]) + fabsf(o[0][1] - o[m - 1][1]) < 1e-3f ? m - 1 : m;
    if (n < 3)
        return 0;
    wc_normals(o, n, 1, r, nm);
    for (int i = 0; i < n; ++i)
    {
        for (int c = 0; c < 2; ++c)
            g_wc.in[i][c] = o[i][c] + nm[i][c] * 0.5f * g_wc.px[c], out[i][c] = o[i][c] - nm[i][c] * 0.5f * g_wc.px[c];
        wc_sample(q, r, g_wc.in[i][0], g_wc.in[i][1], &at[i]);
        wc_sample(q, r, out[i][0], out[i][1], &out_at[i]);
    }
    g_wc.n_in = n;
    uint8_t* w = g_wc_out;
    for (int i = 1; i + 1 < n; ++i) /* the polygon: a fan from its first point */
    {
        w = wc_put(w, &at[0], g_wc.in[0][0], g_wc.in[0][1], 1.0f);
        w = wc_put(w, &at[i], g_wc.in[i][0], g_wc.in[i][1], 1.0f);
        w = wc_put(w, &at[i + 1], g_wc.in[i + 1][0], g_wc.in[i + 1][1], 1.0f);
    }
    for (int i = 0; i < n; ++i) /* the ring round it */
    {
        const int j = (i + 1) % n;
        w = wc_put(w, &at[i], g_wc.in[i][0], g_wc.in[i][1], 1.0f);
        w = wc_put(w, &out_at[i], out[i][0], out[i][1], 0.0f);
        w = wc_put(w, &at[j], g_wc.in[j][0], g_wc.in[j][1], 1.0f);
        w = wc_put(w, &out_at[i], out[i][0], out[i][1], 0.0f);
        w = wc_put(w, &out_at[j], out[j][0], out[j][1], 0.0f);
        w = wc_put(w, &at[j], g_wc.in[j][0], g_wc.in[j][1], 1.0f);
    }
    return (uint32_t)((w - g_wc_out) / 28);
}

/* (2) A corner piece's path, clockwise round the window: on the left up from the bottom corner (or the side's stretch below the
 * top one) to the top corner (or the stretch above the bottom one), on the right down the same way. Its points */
static int wc_path(float (*p)[2])
{
    const float* r = g_wc.r;
    float ext[2];
    const float c = wc_size(r, g_wc.px, ext), x = g_wc.right ? r[2] : r[0];
    const float below_top = r[1] + c + ext[1], above_bottom = r[3] - c - ext[1]; /* where a corner's line ends on the side */
    const int down = g_wc.right;                                                    /* (the right side goes down) */
    int m = 0;
    if (down ? g_wc.top : g_wc.bottom)
        m = wc_corner(down ? 1 : 3, r, c, g_wc.px, ext, p, m);
    else if (down ? g_wc.reach[1] < above_bottom : g_wc.reach[0] > below_top)
        m = wc_add(p, m, x, down ? g_wc.reach[1] : g_wc.reach[0]);
    if (down ? g_wc.bottom : g_wc.top)
        m = wc_corner(down ? 2 : 0, r, c, g_wc.px, ext, p, m);
    else if (down ? g_wc.reach[0] > below_top : g_wc.reach[1] < above_bottom)
        m = wc_add(p, m, x, down ? g_wc.reach[0] : g_wc.reach[1]);
    return m;
}

/* (2) The corner piece at `data` as the line along its path: half a pixel outside it faded out, full half a pixel in, faded out
 * a pixel and a half in. Its vertices */
static uint32_t wc_line(uint32_t data)
{
    static const float off[3] = { -0.5f, 0.5f, 1.5f }, fade[3] = { 0.0f, 1.0f, 0.0f };
    float p[WC_PATH][2], nm[WC_PATH][2];
    const int m = wc_path(p);
    WcVert v;
    memcpy(&v, GUEST_PTR(data), 28);
    v.c = g_wc.colour;
    uint8_t* w = g_wc_out;
    if (m < 2) /* (nothing to draw: one triangle of nothing) */
    {
        for (int i = 0; i < 3; ++i)
            w = wc_put(w, &v, v.x, v.y, 0.0f);
        return 3;
    }
    wc_normals(p, m, 0, g_wc.r, nm);
    for (int i = 0; i + 1 < m; ++i)
        for (int row = 0; row < 2; ++row)
        {
            float a[2][2], b[2][2]; /* point i, then i + 1, at this row and the next */
            for (int s = 0; s < 2; ++s)
                for (int c = 0; c < 2; ++c)
                {
                    a[s][c] = p[i][c] + nm[i][c] * off[row + s] * g_wc.px[c];
                    b[s][c] = p[i + 1][c] + nm[i + 1][c] * off[row + s] * g_wc.px[c];
                }
            w = wc_put(w, &v, a[0][0], a[0][1], fade[row]);
            w = wc_put(w, &v, a[1][0], a[1][1], fade[row + 1]);
            w = wc_put(w, &v, b[0][0], b[0][1], fade[row]);
            w = wc_put(w, &v, a[1][0], a[1][1], fade[row + 1]);
            w = wc_put(w, &v, b[1][0], b[1][1], fade[row + 1]);
            w = wc_put(w, &v, b[0][0], b[0][1], fade[row]);
        }
    return (uint32_t)((w - g_wc_out) / 28);
}

/* Each window's contents kept to its corners: while the game draws a window (d3d8_window_drawing: chat.cpp's window draw hook,
 * nested), the first background drawn in the corners' shape is its own, and what's drawn after it inside it (its text, list
 * rows, item squares) is drawn as its part inside the shape (wc_cut_draw); what crosses its edge (a title's tab on its top) is
 * left as it is. The chat's rows, drawn outside any window's draw, go by the chat window they're in (its background this
 * frame). True Font's letters, drawn when the interface is laid onto the screen, keep the shape they were kept in
 * (chatfont_keep: wc_current). Reported in game 2026-11-06: letters and item squares stood out past cut corners */
#define WC_NEST 8
static struct
{
    int depth;        /* windows being drawn, nested */
    int has;          /* the innermost one's own background drawn in the corners' shape, at r */
    float r[4];
    struct
    {
        int has;
        float r[4];
    } outer[WC_NEST]; /* the windows around it, to go back to */
    float chat[4][4]; /* the chat windows' backgrounds this frame */
    int chats;
} g_wscope;

void d3d8_window_drawing(int on)
{
    if (on)
    {
        if (g_wscope.depth < WC_NEST)
            g_wscope.outer[g_wscope.depth].has = g_wscope.has, memcpy(g_wscope.outer[g_wscope.depth].r, g_wscope.r, sizeof g_wscope.r);
        ++g_wscope.depth, g_wscope.has = 0;
    }
    else if (g_wscope.depth > 0 && --g_wscope.depth < WC_NEST)
        g_wscope.has = g_wscope.outer[g_wscope.depth].has, memcpy(g_wscope.r, g_wscope.outer[g_wscope.depth].r, sizeof g_wscope.r);
}

/* Present: the next frame's windows and chat */
static void wc_present(void) { g_wscope.depth = g_wscope.has = g_wscope.chats = 0, ws_present(); }

/* window_alpha: a window's background (lo, hi) is in the corners' shape, if they're on there */
static void wc_window(const GfxDraw* dr, const float lo[2], const float hi[2], int in_input, int in_chat)
{
    if (!wc_on(in_input, in_chat))
        return;
    g_wc.kind = 1;
    g_wc.r[0] = lo[0], g_wc.r[1] = lo[1], g_wc.r[2] = hi[0], g_wc.r[3] = hi[1];
    wc_pixel(dr, g_wc.px);
    g_wc.c = wc_size(g_wc.r, g_wc.px, NULL);
    if (in_chat && g_wscope.chats < 4)
        memcpy(g_wscope.chat[g_wscope.chats++], g_wc.r, sizeof g_wc.r);
    if (g_wscope.depth && !g_wscope.has)
        g_wscope.has = 1, memcpy(g_wscope.r, g_wc.r, sizeof g_wc.r);
}

/* window_alpha's frame piece on a window whose frame is drawn as a solid line */
typedef struct
{
    const float* r;                 /* the window's background */
    const float* lo;                /* the piece's box */
    const float* hi;
    int on_edge, left, top, bottom; /* window_alpha's: 1 a corner, 2 a strip along the top or bottom, 3 one down a side; where */
    int cornered;                   /* [client] window.corners on its window (wc_on) */
    uint32_t colour;                /* the line's colour and alpha */
} WcEdge;

/* A corner piece left for wc_apply to draw round its corner, its side's line from y0 to y1 */
static uint32_t wc_mark_corner(const WcEdge* e, const float px[2], float y0, float y1, uint32_t up_data, int* tinted)
{
    memcpy(g_wc.r, e->r, sizeof g_wc.r);
    g_wc.kind = 2, g_wc.px[0] = px[0], g_wc.px[1] = px[1];
    g_wc.right = !e->left, g_wc.top = e->top, g_wc.bottom = e->bottom;
    g_wc.reach[0] = y1, g_wc.reach[1] = y0;
    g_wc.colour = e->colour;
    *tinted = 3;
    return up_data;
}

/* A frame piece of a solid border ([client] window.border.solid and the chat's and the chat line's): drawn as a line along its
 * window's edge, in the frame's colour (the game's grey if none), with no texture; one of the target's pixels thick, across
 * and down, which covers one row of pixel centres wherever it lies (1.5 of the game's covered one or two, the top and left a
 * pixel and the right and bottom two). A corner is the line down its side, to the window's middle at most (the chat line's
 * 24-high end pieces on a 22-high window: top and bottom halves); a strip along the top or bottom reaches out to the sides
 * when a corner is beside it; a strip down a side is its stretch. With corners, a corner piece goes round its corner
 * (wc_line) and the strips stop where its line ends */
static uint32_t wc_solid(const GfxDraw* dr, uint32_t up_data, const WcEdge* e, int* tinted)
{
    static uint8_t line[4 * 28];
    const float* r = e->r;
    float px[2], ext[2] = { 0.0f, 0.0f };
    wc_pixel(dr, px);
    const float c = e->cornered ? wc_size(r, px, ext) : 0.0f, mid = (r[1] + r[3]) * 0.5f;
    float x0, x1, y0, y1;
    if (e->on_edge == 2)
    {
        const float in = e->cornered ? c + ext[0] : px[0], beside = fmaxf(32.0f, in); /* (a corner's piece, or line, beside it) */
        y0 = e->top ? r[1] : r[3] - px[1], y1 = y0 + px[1];
        x0 = e->lo[0] - r[0] <= beside ? r[0] + in : e->lo[0];
        x1 = r[2] - e->hi[0] <= beside ? r[2] - in : e->hi[0];
    }
    else
    {
        x0 = e->left ? r[0] : r[2] - px[0], x1 = x0 + px[0];
        y0 = e->lo[1] < r[1] ? r[1] : e->lo[1], y1 = e->hi[1] > r[3] ? r[3] : e->hi[1];
        if (e->on_edge == 1 && !(e->top && e->bottom))
        {
            if (e->top)
                y0 = r[1], y1 = y1 > mid ? mid : y1;
            else
                y1 = r[3], y0 = y0 < mid ? mid : y0;
        }
        if (e->cornered && e->on_edge == 1)
            return wc_mark_corner(e, px, y0, y1, up_data, tinted);
        if (e->cornered) /* a side's strip, clear of the corners' lines */
            y0 = fmaxf(y0, r[1] + c + ext[1]), y1 = fminf(y1, r[3] - c - ext[1]);
    }
    if (x1 <= x0 || y1 <= y0)
    {
        if (!e->cornered)
            return up_data;
        x1 = x0, y1 = y0; /* (nothing of it left beside the corners: drawn as nothing) */
    }
    memcpy(line, GUEST_PTR(up_data), 4 * 28);
    const float xs[4] = { x0, x1, x0, x1 }, ys[4] = { y0, y0, y1, y1 }; /* the strip's order: top left, top right, ... */
    for (int i = 0; i < 4; ++i)
    {
        memcpy(line + 28 * i, &xs[i], 4);
        memcpy(line + 28 * i + 4, &ys[i], 4);
        memcpy(line + 28 * i + 16, &e->colour, 4);
    }
    *tinted = 3; /* 3: a solid line, no texture */
    return (uint32_t)(uintptr_t)line;
}

/* draw_packet: the draw window_alpha marked (g_wc) as triangles in place of its quad, its primitive, count and vertices
 * changed to theirs. The vertices' address */
static uint32_t wc_apply(GfxDraw* d, uint32_t* prim, uint32_t* count, uint32_t* nverts, uint32_t up_data, uint32_t up_stride)
{
    if (!g_wc.kind)
        return up_data;
    const uint32_t n = d->indices || up_stride != 28 || *prim != GFX_TRIANGLESTRIP || *nverts != 4 ? 0
                       : g_wc.kind == 1                                                          ? wc_background(up_data)
                                                                                                  : wc_line(up_data);
    if (!n)
    {
        g_wc.kind = 0;
        return up_data;
    }
    g_wc.quad = up_data;
    d->cull = 1; /* D3DCULL_NONE: the triangles face either way */
    *prim = GFX_TRIANGLELIST, *count = n / 3, *nverts = n;
    return (uint32_t)(uintptr_t)g_wc_out;
}

/* The corners on an ordinary window now (wc_on: window.corners, where its frame is a solid line or none): 0 square, 1 cut,
 * 2 round, their size in `size` (vanacore_gfx_window_corners: VanaCore's pictures drawn to match, the lock-on glow) */
int d3d8_window_corners(float* size)
{
    if (!wc_on(0, 0))
        return 0;
    if (size)
        *size = g_corner_size;
    return g_corner_shape;
}

/* A picture drawn over a window kept to its corners (vanacore_gfx_clip_to_corners, the Target window's lock-on picture):
 * every quad drawn while it's set is drawn as its part inside window r's shape, taken as far out round the window as the
 * quad reaches past its edge (the lock's bars beside the window: the shape that much bigger) */
static struct
{
    int on;
    float r[4]; /* the window: left, top, right, bottom */
} g_wcut;

void d3d8_clip_to_corners(const float* rect)
{
    g_wcut.on = rect != NULL && rect[2] - rect[0] > 1.0f && rect[3] - rect[1] > 1.0f;
    if (g_wcut.on)
        memcpy(g_wcut.r, rect, sizeof g_wcut.r);
}

static float wc_side(const float* a, const float* b, const float* p) { return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]); }

/* Polygon `in` (n points) cut to the side of line a - b that `mid` is on, into `out`. Its points */
static int wc_clip(float (*in)[2], int n, const float* a, const float* b, const float* mid, float (*out)[2])
{
    const float s = wc_side(a, b, mid) < 0.0f ? -1.0f : 1.0f;
    int m = 0;
    for (int i = 0; i < n; ++i)
    {
        const float* p = in[i];
        const float* q = in[(i + 1) % n];
        const float sp = wc_side(a, b, p) * s, sq = wc_side(a, b, q) * s;
        if (sp >= 0.0f)
            out[m][0] = p[0], out[m][1] = p[1], ++m;
        if ((sp >= 0.0f) != (sq >= 0.0f))
        {
            const float t = sp / (sp - sq);
            out[m][0] = p[0] + (q[0] - p[0]) * t, out[m][1] = p[1] + (q[1] - p[1]) * t, ++m;
        }
    }
    return m;
}

/* tips_gradient: the band x[0]..x[1], y[0]..y[1] inside the shape of the background drawn before it (g_wc.in), as triangles at
 * `out` from the vertex at `proto` (its depth, rhw and texture coordinates), coloured c[0] at y[0] to c[1] at y[1]. Its vertices */
static uint32_t wc_band(uint8_t* out, uint32_t proto, const float x[2], const float y[2], const uint32_t c[2])
{
    float a[WC_MOST + 8][2] = { { x[0], y[0] }, { x[1], y[0] }, { x[1], y[1] }, { x[0], y[1] } }, b[WC_MOST + 8][2];
    const float mid[2] = { (g_wc.r[0] + g_wc.r[2]) * 0.5f, (g_wc.r[1] + g_wc.r[3]) * 0.5f };
    int n = 4;
    for (int i = 0; i < g_wc.n_in && n >= 3; ++i)
    {
        n = wc_clip(a, n, g_wc.in[i], g_wc.in[(i + 1) % g_wc.n_in], mid, b);
        memcpy(a, b, sizeof(float) * 2 * (size_t)n);
    }
    WcVert v;
    memcpy(&v, GUEST_PTR(proto), 28);
    uint8_t* w = out;
    for (int i = 1; i + 1 < n; ++i)
    {
        const int k[3] = { 0, i, i + 1 };
        for (int j = 0; j < 3; ++j)
        {
            const float t = y[1] > y[0] ? (a[k[j]][1] - y[0]) / (y[1] - y[0]) : 0.0f;
            v.c = wc_mix(c[0], c[1], t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t);
            w = wc_put(w, &v, a[k[j]][0], a[k[j]][1], 1.0f);
        }
    }
    return (uint32_t)((w - out) / 28);
}

/* The shape a quad is kept to (wc_current): window r's, or (grow: a picture drawn over the window) r's taken out round it as far
 * as the quad (lo, hi) reaches past its edge, its corners' line or arc moved out with it; into p, clockwise, `px` one of the
 * target's pixels. Its points; 0 when the quad is clear of every corner (nothing to cut) */
static int wc_shape(const float r[4], int grow, const float lo[2], const float hi[2], const float px[2], float (*p)[2])
{
    const float g = grow ? fmaxf(0.0f, fmaxf(fmaxf(r[0] - lo[0], hi[0] - r[2]), fmaxf(r[1] - lo[1], hi[1] - r[3]))) : 0.0f;
    const float o[4] = { r[0] - g, r[1] - g, r[2] + g, r[3] + g };
    const float half = fminf(o[2] - o[0], o[3] - o[1]) * 0.5f;
    float c = wc_size(r, px, NULL) + g * (g_corner_shape == 1 ? 0.5857864f : 1.0f); /* (a cut's line moved out g: 2 - sqrt 2) */
    c = c < half ? c : half;
    const int across = lo[0] >= o[0] + c && hi[0] <= o[2] - c, down = lo[1] >= o[1] + c && hi[1] <= o[3] - c;
    if ((across || down) && lo[0] >= o[0] && hi[0] <= o[2] && lo[1] >= o[1] && hi[1] <= o[3])
        return 0;
    int n = 0;
    for (int k = 0; k < 4; ++k)
        n = wc_corner(k, o, c, px, NULL, p, n);
    return n > 2 && fabsf(p[0][0] - p[n - 1][0]) + fabsf(p[0][1] - p[n - 1][1]) < 1e-3f ? n - 1 : n;
}

/* The window a quad (lo, hi: its box) drawn now is kept to, into r, and whether its shape is taken out round it (grow: the
 * picture vanacore_gfx_clip_to_corners holds); `chat` while the game draws the chat's rows. 0: none (no corners, no window,
 * or the quad crosses the window's edge: a title's tab on it) */
static int wc_current(const float lo[2], const float hi[2], int chat, float r[4], int* grow)
{
    *grow = 0;
    if (!g_corner_shape)
        return 0;
    if (g_wcut.on && wc_on(0, 0))
    {
        memcpy(r, g_wcut.r, sizeof g_wcut.r);
        return *grow = 1;
    }
    const float mid[2] = { (lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f };
    int found = 0;
    if (g_wscope.depth && g_wscope.has)
        memcpy(r, g_wscope.r, sizeof g_wscope.r), found = 1;
    for (int i = 0; i < g_wscope.chats && chat && !found; ++i)
    {
        const float* c = g_wscope.chat[i];
        if (mid[0] >= c[0] && mid[0] <= c[2] && mid[1] >= c[1] && mid[1] <= c[3])
            memcpy(r, c, 4 * sizeof(float)), found = 1;
    }
    /* (only what lies inside it, give or take a pixel) */
    return found && lo[0] >= r[0] - 1.0f && lo[1] >= r[1] - 1.0f && hi[0] <= r[2] + 1.0f && hi[1] <= r[3] + 1.0f;
}

/* Where a vertex's parts are: its stride, its position's offset (x, y floats), its texture coordinates' and its colour's (-1:
 * none) */
typedef struct
{
    uint32_t stride;
    int pos, tc, col;
} WcLayout;

/* A part's value at s across and t down a quad whose corners have a[0] (top left), a[1] (top right), a[2], a[3] (bottom) */
static float wc_bilerp(const float* a, float s, float t) { return (a[0] + (a[1] - a[0]) * s) * (1.0f - t) + (a[2] + (a[3] - a[2]) * s) * t; }

/* Vertex `w` at point pt of quad q (its corners' vertices, top left, top right, bottom left, bottom right; lo, hi its box): the
 * first corner's, its position pt, its texture coordinates (uv) and colour (col: each channel's) the quad's there */
static void wc_vertex(uint8_t* w, const uint8_t* const q[4], const WcLayout* lay, const float pt[2], const float lo[2], const float hi[2],
    float (*uv)[4], float (*col)[4])
{
    const float s = (pt[0] - lo[0]) / (hi[0] - lo[0]), t = (pt[1] - lo[1]) / (hi[1] - lo[1]);
    memcpy(w, q[0], lay->stride);
    memcpy(w + lay->pos, pt, 8);
    if (lay->tc >= 0)
    {
        const float at[2] = { wc_bilerp(uv[0], s, t), wc_bilerp(uv[1], s, t) };
        memcpy(w + lay->tc, at, 8);
    }
    if (lay->col >= 0)
    {
        uint32_t c = 0;
        for (int ch = 0; ch < 4; ++ch)
            c |= (uint32_t)(wc_bilerp(col[ch], s, t) + 0.5f) << (8 * ch);
        memcpy(w + lay->col, &c, 4);
    }
}

/* Quad v (4 vertices as `lay` has them, lying square) as its part inside convex shape p (n points; mid inside it), a triangle
 * list at `out`. Its vertices; 0 for a quad not lying square (drawn as it is), 3 of nothing for one wholly outside */
static uint32_t wc_cut_verts(const uint8_t* v, const WcLayout* lay, float (*p)[2], int n, const float mid[2], uint8_t* out)
{
    const uint8_t* q[4] = { NULL, NULL, NULL, NULL };
    float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f }, xy[4][2];
    for (int i = 0; i < 4; ++i)
    {
        memcpy(xy[i], v + i * lay->stride + lay->pos, 8);
        lo[0] = fminf(lo[0], xy[i][0]), lo[1] = fminf(lo[1], xy[i][1]), hi[0] = fmaxf(hi[0], xy[i][0]), hi[1] = fmaxf(hi[1], xy[i][1]);
    }
    if (!(hi[0] - lo[0] > 1e-3f) || !(hi[1] - lo[1] > 1e-3f))
        return 0;
    for (int i = 0; i < 4; ++i)
        q[(xy[i][0] > (lo[0] + hi[0]) * 0.5f) | (xy[i][1] > (lo[1] + hi[1]) * 0.5f) << 1] = v + i * lay->stride;
    if (!q[0] || !q[1] || !q[2] || !q[3])
        return 0;
    float a[WC_MOST + 8][2] = { { lo[0], lo[1] }, { hi[0], lo[1] }, { hi[0], hi[1] }, { lo[0], hi[1] } }, b[WC_MOST + 8][2];
    int m = 4;
    for (int i = 0; i < n && m >= 3; ++i)
    {
        m = wc_clip(a, m, p[i], p[(i + 1) % n], mid, b);
        memcpy(a, b, sizeof(float) * 2 * (size_t)m);
    }
    if (m < 3) /* (wholly outside: one triangle of nothing, on one point) */
    {
        for (int i = 0; i < 3; ++i)
            memcpy(out + i * lay->stride, q[0], lay->stride);
        return 3;
    }
    float uv[2][4] = { { 0 } }, col[4][4] = { { 0 } };
    for (int k = 0; k < 4; ++k)
    {
        float t2[2] = { 0.0f, 0.0f };
        uint32_t c = 0;
        if (lay->tc >= 0)
            memcpy(t2, q[k] + lay->tc, 8);
        if (lay->col >= 0)
            memcpy(&c, q[k] + lay->col, 4);
        uv[0][k] = t2[0], uv[1][k] = t2[1];
        for (int ch = 0; ch < 4; ++ch)
            col[ch][k] = (float)(c >> (8 * ch) & 0xFFu);
    }
    uint8_t* w = out;
    for (int i = 1; i + 1 < m; ++i) /* a fan from its first point */
        for (int j = 0; j < 3; ++j, w += lay->stride)
            wc_vertex(w, q, lay, a[j ? i + j - 1 : 0], lo, hi, uv, col);
    return (uint32_t)((w - out) / lay->stride);
}

/* draw_packet, once True Font has taken its letters: a quad drawn now in a window with corners (wc_current) as its part inside
 * the window's shape, the draw's vertices, primitive and count the triangles' (g_wc_out) */
static void wc_cut_draw(GfxDraw* d, int chat)
{
    const uint32_t stride = (uint32_t)d->u.stride[0];
    if (!g_corner_shape || !d->vs.rhw || d->prim != GFX_TRIANGLESTRIP || d->count != 2 || d->indices || !d->data[0] || stride < 16 ||
        stride > 64 || d->size[0] < 4 * stride)
        return;
    const WcLayout lay = { stride, (int)d->u.offset[GFX_R_POSITION],
                           d->vs.el[GFX_R_TEXCOORD0].used && !d->vs.el[GFX_R_TEXCOORD0].stream ? (int)d->u.offset[GFX_R_TEXCOORD0] : -1,
                           d->vs.el[GFX_R_DIFFUSE].used && !d->vs.el[GFX_R_DIFFUSE].stream ? (int)d->u.offset[GFX_R_DIFFUSE] : -1 };
    const uint8_t* v = (const uint8_t*)d->data[0];
    float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f }, r[4], px[2], p[WC_MOST][2];
    for (int i = 0; i < 4; ++i)
    {
        float xy[2];
        memcpy(xy, v + i * stride + lay.pos, 8);
        lo[0] = fminf(lo[0], xy[0]), lo[1] = fminf(lo[1], xy[1]), hi[0] = fmaxf(hi[0], xy[0]), hi[1] = fmaxf(hi[1], xy[1]);
    }
    int grow;
    if (!wc_current(lo, hi, chat, r, &grow))
        return;
    wc_pixel(d, px);
    const int n = wc_shape(r, grow, lo, hi, px, p);
    const float mid[2] = { (r[0] + r[2]) * 0.5f, (r[1] + r[3]) * 0.5f };
    const uint32_t m = n ? wc_cut_verts(v, &lay, p, n, mid, g_wc_out) : 0;
    if (!m)
        return;
    d->data[0] = d->cpu[0] = g_wc_out;
    d->size[0] = d->cpu_size[0] = m * stride;
    d->prim = GFX_TRIANGLELIST, d->count = m / 3, d->cull = 1; /* (D3DCULL_NONE: the triangles face either way) */
}

/* The window shapes the letters True Font keeps this frame were drawn in (wc_keep: CfGlyph.clip), drawn when the interface
 * is laid onto the screen (wc_cut_onto): each window's background (the interface's pixels), and whether its shape is taken
 * out round it as far as a letter reaches past it (the lock-on picture's: wc_current) */
static struct
{
    float r[4];
    int grow;
} wc_kept[64];
static int wc_nkept;

/* The window shape a letter kept now (lo, hi: its box) is drawn in, its index + 1 (CfGlyph.clip), 0 none; `first` the frame's
 * first letter kept (the shapes kept before it gone). Chosen by its box taken up to `cap`, its row's capitals' top (a label's
 * or a big letter's), so a word's letters go to one shape: the Target window's "Target" crosses the window's top edge with
 * its T's (no shape: a title's tab on it), while its short a, r, g and e lay inside it, were kept to the window, and were cut
 * flat at its edge where True Font drew them taller, as capitals or larger (reported 2026-11-07) */
static uint8_t wc_keep(const float lo[2], const float hi[2], float cap, int chat, int first)
{
    float r[4];
    int grow;
    if (first)
        wc_nkept = 0;
    const float up[2] = { lo[0], fminf(lo[1], cap) };
    if (!wc_current(up, hi, chat, r, &grow))
        return 0;
    for (int i = wc_nkept - 1; i >= 0; --i)
        if (wc_kept[i].grow == grow && !memcmp(wc_kept[i].r, r, sizeof r))
            return (uint8_t)(i + 1);
    if (wc_nkept == 64)
        return 0;
    memcpy(wc_kept[wc_nkept].r, r, sizeof r), wc_kept[wc_nkept].grow = grow;
    return (uint8_t)++wc_nkept;
}

/* A kept letter's draw `d` onto the screen (its quad v, the screen's pixels) cut to the window shape it was kept in (`slot`:
 * wc_keep's), the shape worked out in the interface's pixels and taken to the screen's as the letter's run is (aff: each way's
 * screen pixel at 0 and its pixels to one: d3d8_chatfont.h cf_affine). 0 when none of it shows */
static int wc_cut_onto(GfxDraw* d, const uint8_t* v, int slot, const float aff[4])
{
    static uint8_t out[3 * (WC_MOST + 8) * 64];
    const uint32_t stride = (uint32_t)d->u.stride[0];
    if (!slot || slot > wc_nkept || stride < 16 || stride > 64 || !(aff[1] > 0.0f) || !(aff[3] > 0.0f))
        return 1;
    const WcLayout lay = { stride, (int)d->u.offset[GFX_R_POSITION], (int)d->u.offset[GFX_R_TEXCOORD0],
                           d->vs.el[GFX_R_DIFFUSE].used && !d->vs.el[GFX_R_DIFFUSE].stream ? (int)d->u.offset[GFX_R_DIFFUSE] : -1 };
    float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f }, p[WC_MOST][2];
    for (int i = 0; i < 4; ++i) /* (its box back in the interface's pixels) */
        for (int c = 0; c < 2; ++c)
        {
            float xy[2];
            memcpy(xy, v + i * stride + lay.pos, 8);
            const float at = (xy[c] - aff[2 * c]) / aff[2 * c + 1];
            lo[c] = fminf(lo[c], at), hi[c] = fmaxf(hi[c], at);
        }
    const float* r = wc_kept[slot - 1].r;
    const float px[2] = { 1.0f / aff[1], 1.0f / aff[3] }; /* (one of the screen's pixels) */
    const int n = wc_shape(r, wc_kept[slot - 1].grow, lo, hi, px, p);
    if (!n)
        return 1;
    for (int i = 0; i < n; ++i)
        p[i][0] = aff[0] + p[i][0] * aff[1], p[i][1] = aff[2] + p[i][1] * aff[3];
    const float mid[2] = { aff[0] + (r[0] + r[2]) * 0.5f * aff[1], aff[2] + (r[1] + r[3]) * 0.5f * aff[3] };
    const uint32_t m = wc_cut_verts(v, &lay, p, n, mid, out);
    if (m == 3 && !memcmp(out, out + stride, stride)) /* (wholly outside) */
        return 0;
    if (!m)
        return 1;
    d->data[0] = d->cpu[0] = out;
    d->size[0] = d->cpu_size[0] = m * stride;
    d->prim = GFX_TRIANGLELIST, d->count = m / 3, d->indices = NULL, d->ibuf = NULL, d->cull = 1;
    return 1;
}
