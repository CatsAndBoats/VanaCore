/* VanaCore's grass: tufts scattered over the zone's grass-textured ground, round the player.
 *
 * The game draws a zone's ground as meshes from static vertex buffers, placed by its vertex shaders' constants. d3d8.c hands
 * every such opaque world draw to grass_note, which keeps those that carry a grass texture:
 *
 *   - the mesh, once: its triangles in model space (position, texture coordinates, vertex colour);
 *   - this frame's model-to-clip transform of the draw, found by running the draw's vertex function on the CPU over four points
 *     (gfx_draw_clip_pos). The camera of the frame isn't known yet while the ground is drawn (the scene's first fixed-function
 *     draw sets it), so the transform stays in clip space until grass_scene.
 *
 * grass_scene, when the scene is finished, takes the clip transform back through the frame's camera to the world (clip_to_world:
 * from the transform's x, y and w alone, which is exact; through the projection's inverse it was a hair off that grew with the
 * camera's distance from the zone's origin), which makes an instance of the mesh (its up-facing triangles, in world space, kept
 * while the mesh is drawn from the same place; one that moves while the camera stands still grows nothing for a while). The
 * instances drawn THIS frame are the ground the game shows - its culling, its level-of-detail picks, its draw distance - and tufts
 * go onto those triangles within grass.radius of the player: the same places every frame, as each triangle's tufts come from a
 * hash of the triangle. A tuft stands where the ground's texture, looked up at that spot, is green (or, on dry grass ground, the
 * yellow-olive of dry grass: tex_sort). The tufts are made once, a square of the world at a time, and kept on the GPU; each frame
 * draws the squares in reach, alpha-tested into the scene's target before the scene effects (AO, fog, DLSS) run on it, through a
 * vertex function that thins them in rings toward the radius and sways them (see "the kept tufts"). The grass casts no shadow:
 * it's never one of the sun's casters, and the effects' occlusion and contact shadows leave it out (gfx_scene_world_depth), so the
 * pixels it covers skip them.
 *
 * The world as the game draws it has y down: up is -y. */
#include "grass.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "gfx_hlsl.h"
#include "runtime.h"

#define FMT_A8R8G8B8 21u
#define FMT_X8R8G8B8 22u
#define FMT_R5G6B5 23u
#define FMT_X1R5G5B5 24u
#define FMT_A1R5G5B5 25u
#define FMT_A4R4G4B4 26u
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define FMT_DXT1 FOURCC('D', 'X', 'T', '1')
#define FMT_DXT2 FOURCC('D', 'X', 'T', '2')
#define FMT_DXT3 FOURCC('D', 'X', 'T', '3')
#define FMT_DXT4 FOURCC('D', 'X', 'T', '4')
#define FMT_DXT5 FOURCC('D', 'X', 'T', '5')

#define MAPN 64        /* a texture's colours, kept as MAPN x MAPN texels */
#define TEXMAX 1024
#define MESHMAX 8192
#define INSTMAX 16384
#define DRAWMAX 4096
#define TRIMAX 400000  /* model-space and world-space triangles each */
#define TUFT_TEX 64
#define MOVING_RETRY 3600 /* scenes a piece seen moving is left before a new one is made there and judged afresh (a minute at 60 FPS),
                           * twice as long each time it's seen moving again, up to 32 minutes: nothing is left out for good, and a
                           * real mover (a tree in the wind) doesn't fill the pools with a new piece a minute */

typedef struct GTex
{
    uint64_t hash;
    uint32_t w, h, fmt;
    float avg[3];
    float lit[3];     /* the mean colour of its grass texels, green or yellow-olive (tex_sort): a tuft in its shade takes this darkened */
    uint8_t autog;    /* ground that's grass (tex_sort): green or dry */
    uint8_t green;    /* a quarter of its colours green: green grass, where yellow-olive is a path's dirt */
    uint8_t dry;      /* not green, but a quarter of its colours green or yellow-olive, its yellow-olive as saturated as dry grass (grass.dry) */
    uint8_t foliage;  /* black cut-outs: a tree's leaves and bark, not ground */
    uint8_t cut_drawn; /* seen drawn with the alpha test (a plant's cut-outs) and never solid: foliage (note_one) */
    uint8_t solid;     /* seen drawn solid (opaque, depth written, no alpha test): ground for good, whatever its colours or later draws */
    uint8_t* map;  /* MAPN * MAPN rgb, or NULL */
} GTex;

typedef struct GTri /* a mesh's triangle, model space */
{
    float p[3][3], uv[3][2];
    uint32_t c[3];
    float n[3];   /* its corners' normals summed, model space (the game lights the ground by them): which face the game shows */
    uint8_t hasn; /* n is the draw's: its vertices carry normals */
} GTri;

typedef struct GWTri /* an instance's triangle, in the world */
{
    float p[3][3], uv[3][2];
    uint32_t c[3];
    float ctr[3], rad, area;
    float up; /* how upright its ground is: 1 flat, 0 a wall */
    /* the ground falling away past each edge (p0-p1, p1-p2, p2-p0) and from each corner: how upright it is (up times 254; 255, none
     * falls away there), find_edges */
    uint8_t enup[3], vnup[3];
    /* how upright the ground is at each corner: the mean of the mesh's triangles meeting there, steep ones too (up times 254),
     * smooth_corners */
    uint8_t vup[3];
    uint8_t lnx, lnz; /* the normal the game lights it by (its corners', else its face's), up, x and z as 0 to 255: grass_slope.h */
} GWTri;

typedef struct GMesh
{
    uint64_t key;
    uint32_t chk, first, n, tex; /* the triangles are tris[first .. first + n) */
    uint32_t inst_head;          /* its instances, newest first: inst index + 1, 0 for none */
    float lo[3], hi[3];          /* its bounds, model space */
    uint8_t bad;
    uint8_t blended;             /* drawn blended, never opaque (grass.blended): a layer laid over other ground */
    uint8_t layer;               /* ... its tufts as many as its vertices' alpha shows of it, times this (layer_scale: 1, 2 or 4); 0 all */
    uint32_t retry;              /* bad: tried again from this scene on (a mesh is never left out for good) */
} GMesh;

typedef struct GInst
{
    uint32_t mesh, first, n, seen; /* seen: the scene (g.scene) it was last drawn in */
    uint32_t next;                 /* the mesh's next instance + 1 */
    float t[3];                    /* where the mesh stands: its transform's translation, as last found (instance_for) */
    float w3[9];                   /* ... and the rest of it (rotation, scale) */
    uint32_t cam;                  /* the camera (g.cam_id) t and w3 were found under: under the same one a still piece lands on the same numbers */
    uint8_t moving;                /* seen moving (a tree's sway, a platform): it grows nothing */
    uint32_t retry;                /* moving: from this scene on it's left out of the matching, and a new piece is made there and judged afresh */
    uint8_t tries;                 /* the pieces made where this one stands before it that were seen moving: its wait doubles with each */
    float ctr[3], rad;
    uint32_t stamp;                /* this piece and no other made since (its kept tufts say whose they are by it) */
    uint32_t kept;                 /* its kept squares of tufts: a slot in g_kept + 1, 0 for none */
} GInst;

typedef struct GDraw /* a draw this frame: the mesh and its transform to clip space */
{
    uint32_t mesh;
    float M[16];
} GDraw;

/* --- the catalogue's assets -------------------------------------------------------------------------------
 * The tufts are drawn from assets: the drawn blades (assets 0 and 1, made here) and pictures the catalogue hands over (grass_asset,
 * from 2: VanaCore's own and the player's library, plants captured from the game's sprite sheets by grass_capture_start), each a
 * picture with the rectangles of the sprites on it. Each has a weight: a tuft's place takes an asset by weight, so 70 and 30 grow
 * seven of the one for every three of the other. */
#define ASSETMAX 64
#define DRAWN 2 /* assets 0 and 1: the drawn blades, type 1 (make_tuft1_texture) and type 2 (make_tuft_texture) */
#define SPRITEMAX 32
#define CAPMAX 32

typedef struct SRect
{
    float x0, y0, x1, y1;
} SRect;

typedef struct GAsset
{
    uint32_t* px;   /* the picture, 0xAARRGGBB rows; NULL: the drawn blades (assets 0 and 1), or none */
    uint32_t w, h;
    SRect r[SPRITEMAX];
    float avg[SPRITEMAX][3]; /* each sprite's colour, its solid pixels' mean (0 to 1): grass.match tints it toward the ground's */
    int n;
    float tallest;  /* the tallest sprite, pixels */
    float height;   /* how tall it stands next to grass.height */
    float weight;   /* its share of the tufts' places (0 none) */
    uint8_t dirty;  /* a new picture: its texture is made again */
    uint8_t flower; /* flowers (the catalogue's kind "flower"): grass.match leaves their colours and light alone */
    GfxTex* tex;
} GAsset;

/* a sheet seen during a capture */
typedef struct GCap
{
    uint64_t hash;
    uint32_t fmt, w, h, pitch;
    uint8_t* bytes; /* its first level as the game uploaded it */
    float dist;     /* the nearest a draw of it stood to the player */
    uint32_t* px;   /* decoded, 0xAARRGGBB, when the capture is done */
    SRect r[SPRITEMAX];
    int n;
} GCap;

static struct
{
    int enabled, log;
    int zone, zone_ok;
    uint64_t listed[64];
    int nlisted;
    /* the zone's own grass ground (data\grass_dirt.json's "ground"): textures that are grass there whatever their colours say, as
     * The Sanctuary of Zi'Tah's dark leaf-litter floor, too dark for the colour tests and taken for a plant's cut-outs; zsum their sum */
    uint64_t zlisted[64];
    int nzlisted;
    uint64_t zsum;
    /* ... and the textures never grass ground here ("not_grass", the zone's and every zone's: a Mog House's tan stone floor passed for
     * dry grass); in zsum too */
    uint64_t znot[256];
    int nznot;
    float radius, density, height, width, variation, brightness, flowers, flower_brightness;
    int flowers_only;
    int edges; /* the tufts kept off the brink of drops (find_edges, brink) */
    float edge_fade, edge_low; /* grass.edge.fade, .height: the tufts shorter near where the grass ends, to edge_low (grass_edges.h) */
    int cull;                  /* grass.cull: a square's layer wholly outside the view not drawn (layer_in_view) */
    int mega;                  /* grass.mega: every spot of grass ground on a grass floor grows, whatever its colour (build_step) */
    uint32_t quads_in, quads_all; /* the last frame's quads drawn and in reach, for the report */
    float match; /* how far the tufts are made the ground's colour under them, lit as it is (0 to 1) */
    float wind_speed, wind_turb; /* the sway's pace and how much it differs across the field, times its own (0 to 4; 0 still, 0 a field moving as one) */
    float blade_avg[DRAWN]; /* the drawn blades' pictures' mean shade (blade_quad_shade): 0 until they're made */
    /* the light the game puts on the ground (ground_light_note): summed over the frame's ground draws, then the last frame's */
    float lsum[3], ground_light[3];
    uint32_t nlit;
    int ground_known;
    /* the light the tufts are drawn with, eased toward g_light and ground_light (light_ease), and when it last was */
    float light_shown, ground_shown[3];
    uint64_t ease_ns;
    int dry; /* the yellow-olive of dry grass counts as grass too, in a texture that is dry grass (dry_ok, tex_sort) */
    int walkable; /* tufts only on ground the player could stand on (floor_grows) */
    int overgrown; /* dry grass ground grows on stone and metal floors throughout too (soil_ok left out): the Nyzul portal's meadow */
    int nowater; /* no tufts on the footsteps' shallow water (floor_grows, GRASS_WET), walkable or not */
    int moss;    /* ... and with nowater, moss there in their place (grass_moss.h) */
    int shade; /* in grass ground, texels in shadow and dry grass's yellow-olive grow too, not only the green ones (ground_ok) */
    float dry_sat; /* the least mean saturation of a texture's yellow-olive (dry_ok) for it to be dry grass (tex_sort): Sarutabaruta's grass is
                    * about 0.39, Batallia Downs' and Wajaom Woodlands' olive 0.28 and 0.29, the olive-grey dirt of a forest path about 0.24;
                    * grass.dry.saturation, 0.25 unless set */
    float dry_hue; /* the reddest hue that is yellow-olive (dry_ok), degrees: grass.dry.hue, 46 unless set */
    /* the zone's dirt level (1 to 100, 50 unless given: data\grass_dirt.json), and dirt_k, 2 to the (dirt - 50) / 50: every grass
     * test's bar times it (green_ok's lead over red and blue, dry_ok's least saturation for a texel, dry_sat for a texture), so a
     * higher one leaves more ground bare */
    float dirt, dirt_k;
    int blended; /* blended ground draws are ground too, layers over other ground (grass_layers, layer_scale) */
    float pathpad; /* yalms: no tuft grows with a path's ground this near it (path_near) */
    float slope_cos; /* the steepest ground it grows on, as the cosine of its angle */
    float cell;      /* the side of the squares of the world the tufts are kept in */
    /* the rings (grass.ring1-3, .amount): each one's ramp (from, to: shares of the radius) and the share of tufts it takes away there
     * (rings 1-3 keep 1 - the steps before them); the shares they keep: 1, then each ring's */
    float ring_from[3], ring_to[3], ring_step[3], ring_keep[4];
    float ring_at[3]; /* where rings 1-3 start, of the radius (their ramps' middles) */
    int show_rings;   /* draw them round the player (draw_rings) */
    int show_cover;   /* the tufts drawn by what the floors say of their place (grass.cover.show: floor_grows) */
    uint32_t limit; /* about the most tufts drawn at once: what the density needs, grass.tufts a ceiling (tuft_limit; kept_density) */
    int want_reset; /* a pool or table ran out: everything is made again at the start of the next frame */
    uint32_t placed; /* tufts, sprites and flowers made this frame, against limit */

    /* the catalogue's assets (0 and 1: the drawn blades) and their weights' sum (the assets that can be drawn) */
    GAsset asset[ASSETMAX];
    float wsum;
    int weights_set; /* grass_asset_weight was called: until then the drawn blades of type 2 alone */

    /* a capture (grass_capture_start): the cut-out sheets drawn near the player over the next frames */
    int cap_frames, cap_done;
    uint32_t ncap;
    GCap cap[CAPMAX];
    float cw_last[16], focus_last[4]; /* the last scene's camera (clip space to the world) and where the player stood */
    int cw_known;
    /* the scene's camera as instance_for takes a draw's clip transform back to the world (cam_prepare, clip_to_world): the view's
     * inverse, and the projection's x, y and w columns over its first three rows, inverted; cam_id goes up each scene the camera
     * differs from the last scene's (GInst.cam) */
    struct { double vinv[16], ai[9], p3[3]; } cam;
    uint32_t cam_id;
    float view_last[16], proj_last[16];

    GTex tex[TEXMAX];
    uint32_t ntex;
    uint16_t texh[2048];
    int tex_full; /* a texture didn't fit: room is made at the end of the scene (tex_make_room) */

    GTri* tris;
    uint32_t ntris, captris;
    GWTri* wtris;
    uint32_t nwtris, capwtris;
    GMesh mesh[MESHMAX];
    uint32_t nmesh;
    uint16_t meshh[MESHMAX * 2];
    GInst inst[INSTMAX];
    uint32_t ninst;

    GDraw draws[DRAWMAX];
    uint32_t ndraws;
    uint64_t frame;
    uint32_t scene;
    uint32_t stamps; /* the pieces made so far: each its own stamp */

    uint32_t nflowers;
    GfxTex *tuft[DRAWN], *flower, *moss_tex; /* tuft[t]: the drawn blades of asset t; moss_tex the moss's (make_moss_texture) */
    float moss_avg;                           /* the moss's picture's mean shade, as blade_avg: 0 until it's made */
    float near_d;
    uint64_t near_tex;
    uint32_t reach_tris;

    /* the log's counters */
    uint64_t next_report;
    uint32_t seen_draws, new_meshes, bad_meshes, new_insts, tufts, capped, covered;
    /* the tufts' places grown, by their floor's height from the ground (grass.log, floor_grows): over 0.25 up, 0.05 to 0.25 up, within
     * 0.05, 0.05 to 0.25 below, further below; and on no floor */
    uint32_t grew[6];
    uint32_t rejected[2], frames, moving;
    uint64_t note_ns, scene_ns;
    float build_ms;        /* a frame's time for making squares of tufts (grass.build); under 0: no limit */
    uint64_t made_ns;      /* ... spent on it, and the tufts made, since grass_made_take */
    uint32_t made_tufts;
} g; /* zero until grass_configure: it sets every setting (the ini's defaults are in graphics.cpp) */

/* --- small things -------------------------------------------------------------------------------------- */
static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16, x *= 0x7feb352dU, x ^= x >> 15, x *= 0x846ca68bU, x ^= x >> 16;
    return x;
}

static float rnd01(uint32_t x) { return (float)(hash32(x) >> 8) * (1.0f / 16777216.0f); }

static uint64_t hash64(uint64_t x)
{
    x ^= x >> 33, x *= 0xff51afd7ed558ccdull, x ^= x >> 33, x *= 0xc4ceb9fe1a85ec53ull, x ^= x >> 33;
    return x;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
/* the CRT's fmaxf / fminf aren't inlined (each classifies its arguments first): a frame making a square called them a hundred
 * thousand times */
static float maxf(float a, float b) { return a > b ? a : b; }
static float minf(float a, float b) { return a < b ? a : b; }

static void mat_mul(float* o, const float* a, const float* b)
{
    float t[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            t[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
    memcpy(o, t, 64);
}

static int mat_inv_d(double* out, const double* m)
{
    double inv[16], det;
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabs(det) < 1e-30)
        return 0;
    for (int i = 0; i < 16; ++i)
        out[i] = inv[i] / det;
    return 1;
}

static int mat_inv(float* out, const float* mf)
{
    double m[16], inv[16];
    for (int i = 0; i < 16; ++i)
        m[i] = mf[i];
    if (!mat_inv_d(inv, m))
        return 0;
    for (int i = 0; i < 16; ++i)
        out[i] = (float)inv[i];
    return 1;
}

/* --- settings ------------------------------------------------------------------------------------------ */
static void kept_clear(void);
static void floor_wait(void);

static void reset_world(void)
{
    kept_clear();
    g.ntris = g.nwtris = g.nmesh = g.ninst = g.ndraws = 0;
    memset(g.meshh, 0, sizeof g.meshh);
}

static void zone_check(void)
{
    g.zone_ok = g.enabled && g.zone != 0;
}

static void relog_textures(void);
static void retag_textures(void);
/* a line about something that leaves grass out or starts it over: always the first 30 a session (grass.log off, as it is for most
 * players), then only with grass.log, so a report of vanished grass says why */
static int g_said;
#define SAY(...)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (g.log || g_said++ < 30)                                                                                    \
            rt_log(__VA_ARGS__);                                                                                       \
    } while (0)

/* a setting whose 0 is its default (a settings struct zeroed past what it sets): v within lo to hi, or def */
static float or_default(float v, float lo, float hi, float def) { return v > 0.0f ? clampf(v, lo, hi) : def; }

/* grass.textures: the hex hashes of the ground textures that are grass (8 digits at least each; most kept: 64, not_grass's 256) */
static int parse_listed(const char* textures, uint64_t* out, int most)
{
    int n = 0;
    for (const char* p = textures ? textures : ""; *p;)
    {
        char* e;
        unsigned long long v = strtoull(p, &e, 16);
        if (e == p || e - p < 8)
            p = e == p ? p + 1 : e;
        else
            p = e, n < most ? (void)(out[n++] = (uint64_t)v) : (void)0;
    }
    return n;
}

static void set_listed(const char* textures) { g.nlisted = parse_listed(textures, g.listed, 64); }

/* GrassSettings.ground and .not_ground: the zone's own grass ground, and what's never grass, the same way */
static void set_zone_listed(const char* textures, const char* never)
{
    g.nzlisted = parse_listed(textures, g.zlisted, 64), g.nznot = parse_listed(never, g.znot, 256), g.zsum = 0;
    for (int i = 0; i < g.nzlisted; ++i)
        g.zsum += hash64(g.zlisted[i]);
    for (int i = 0; i < g.nznot; ++i)
        g.zsum += hash64(~g.znot[i]);
}

#include "grass_rings.h" /* set_rings, ring_share, ring_level, kept_area, tuft_limit, kept_density: the rings and the most tufts */

void grass_configure(const GrassSettings* in)
{
    const int was = g.enabled, was_log = g.log;
    /* what the kept tufts were made with: they're made again when any of it changes */
    const float made_with[] = { g.radius, g.density, g.height, g.width, g.variation, g.brightness, g.flowers, g.flower_brightness,
        g.slope_cos, (float)g.limit, (float)g.flowers_only, g.ring_from[0], g.ring_from[1], g.ring_from[2], g.ring_keep[1], g.ring_keep[2],
        g.ring_keep[3], (float)g.edges, g.match, (float)g.walkable, g.pathpad, (float)g.overgrown, (float)g.shade, (float)g.show_cover,
        (float)g.nowater, (float)g.moss, g.edge_fade, g.edge_low, (float)g.mega };
    g.enabled = in->enabled != 0;
    g.mega = in->mega != 0;
    g.radius = clampf(in->radius, 4.0f, 500.0f);
    g.density = clampf(in->density, 0.1f, 100.0f);
    g.height = clampf(in->height, 0.05f, 10.0f);
    g.width = clampf(in->width, 0.1f, 10.0f);
    g.variation = clampf(in->variation, 0.0f, 1.0f);
    g.brightness = clampf(in->brightness, 0.05f, 10.0f);
    g.flowers = clampf(in->flowers, 0.0f, 1.0f);
    g.flower_brightness = clampf(in->flower_brightness, 0.05f, 10.0f);
    g.flowers_only = in->flowers_only != 0;
    g.edges = in->edges != 0;
    g.edge_fade = clampf(in->edge_fade, 0.0f, 10.0f), g.edge_low = or_default(in->edge_low, 0.05f, 1.0f, 0.25f);
    g.cull = in->cull != 0;
    g.match = clampf(in->match, 0.0f, 1.0f);
    g.wind_speed = clampf(in->wind_speed, 0.0f, 4.0f), g.wind_turb = clampf(in->wind_turbulence, 0.0f, 4.0f);
    /* what tex_sort sorts the textures by, and what's ground (grass.blended's layers, the zone's own ground textures) */
    const float sorted_with[] = { (float)g.dry, g.dry_sat, g.dry_hue, g.dirt, (float)g.blended, (float)(g.zsum & 0xFFFFFF) };
    g.dry = in->dry != 0;
    g.dry_sat = or_default(in->dry_sat, 0.05f, 1.0f, 0.25f);
    g.dry_hue = or_default(in->dry_hue, 1.0f, 70.0f, 46.0f);
    g.dirt = or_default(in->dirt, 1.0f, 100.0f, 50.0f), g.dirt_k = exp2f((g.dirt - 50.0f) / 50.0f);
    g.blended = in->blended != 0;
    g.shade = in->shade != 0;
    g.walkable = in->walkable != 0;
    g.overgrown = in->overgrown != 0;
    g.nowater = in->nowater != 0, g.moss = in->moss != 0;
    g.pathpad = clampf(in->pathpad, 0.0f, 4.0f);
    g.build_ms = in->build_ms == 0.0f ? 1.5f : in->build_ms < 0.0f ? -1.0f : clampf(in->build_ms, 0.1f, 50.0f);
    g.slope_cos = cosf(clampf(in->slope, 5.0f, 85.0f) * 0.017453292f);

    g.log = in->log != 0;
    set_listed(in->textures);
    set_zone_listed(in->ground, in->not_ground);
    /* the side of the squares tufts are kept in: about a fifth of the reach's width */
    g.cell = clampf(g.radius * 0.4f, 8.0f, 32.0f);
    set_rings(in);
    g.limit = tuft_limit(in->limit); /* (after the radius, the density and the rings, which it follows) */
    g.show_cover = in->show_cover != 0;
    const float sorted_now[] = { (float)g.dry, g.dry_sat, g.dry_hue, g.dirt, (float)g.blended, (float)(g.zsum & 0xFFFFFF) };
    const float now_with[] = { g.radius, g.density, g.height, g.width, g.variation, g.brightness, g.flowers, g.flower_brightness,
        g.slope_cos, (float)g.limit, (float)g.flowers_only, g.ring_from[0], g.ring_from[1], g.ring_from[2], g.ring_keep[1], g.ring_keep[2],
        g.ring_keep[3], (float)g.edges, g.match, (float)g.walkable, g.pathpad, (float)g.overgrown, (float)g.shade, (float)g.show_cover,
        (float)g.nowater, (float)g.moss, g.edge_fade, g.edge_low, (float)g.mega };
    if (memcmp(made_with, now_with, sizeof made_with))
        kept_clear();
    zone_check();
    if (memcmp(sorted_with, sorted_now, sizeof sorted_with)) /* what counts as grass changed: the textures sorted again, the world made again */
        retag_textures(), reset_world();
    if (!g.enabled && was)
        reset_world();
    if (g.enabled && g.log && !was_log) /* the log asked for: what was found before it is found again, and said */
        reset_world(), relog_textures();
}

void grass_set_zone(int zone)
{
    if (zone == g.zone)
        return;
    g.zone = zone;
    g.ground_known = 0, g.nlit = 0, memset(g.lsum, 0, sizeof g.lsum); /* the new zone's light on its ground taken at once */
    reset_world();
    floor_wait();
    zone_check();
    if (g.enabled && g.log)
        rt_log("[recomp] grass: zone %d, %s\n", zone, g.zone_ok ? "on" : "off");
}

int grass_on(void) { return g.zone_ok; }

/* the light on the tufts (grass_set_light), the vertex function's c17: apart from g, which is zero until grass_configure */
static float g_light = 1.0f;

void grass_set_light(float k) { g_light = isfinite(k) ? clampf(k, 0.05f, 1.0f) : g_light; }

/* --- textures ------------------------------------------------------------------------------------------ */
static void rgb565(uint16_t c, uint8_t o[3])
{
    o[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31), o[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63), o[2] = (uint8_t)((c & 31) * 255 / 31);
}

static int texel_rgb(uint32_t fmt, const uint8_t* mem, uint32_t pitch, uint32_t w, uint32_t h, uint32_t x, uint32_t y, uint8_t o[3])
{
    if (x >= w || y >= h)
        return 0;
    switch (fmt)
    {
    case FMT_A8R8G8B8:
    case FMT_X8R8G8B8:
    {
        const uint8_t* p = mem + (size_t)y * pitch + (size_t)x * 4;
        o[0] = p[2], o[1] = p[1], o[2] = p[0];
        return 1;
    }
    case FMT_R5G6B5:
    {
        uint16_t c;
        memcpy(&c, mem + (size_t)y * pitch + (size_t)x * 2, 2);
        rgb565(c, o);
        return 1;
    }
    case FMT_X1R5G5B5:
    case FMT_A1R5G5B5:
    {
        uint16_t c;
        memcpy(&c, mem + (size_t)y * pitch + (size_t)x * 2, 2);
        o[0] = (uint8_t)(((c >> 10) & 31) * 255 / 31), o[1] = (uint8_t)(((c >> 5) & 31) * 255 / 31), o[2] = (uint8_t)((c & 31) * 255 / 31);
        return 1;
    }
    case FMT_A4R4G4B4:
    {
        uint16_t c;
        memcpy(&c, mem + (size_t)y * pitch + (size_t)x * 2, 2);
        o[0] = (uint8_t)(((c >> 8) & 15) * 17), o[1] = (uint8_t)(((c >> 4) & 15) * 17), o[2] = (uint8_t)((c & 15) * 17);
        return 1;
    }
    case FMT_DXT1:
    case FMT_DXT2:
    case FMT_DXT3:
    case FMT_DXT4:
    case FMT_DXT5:
    {
        const uint32_t blk = fmt == FMT_DXT1 ? 8u : 16u;
        const uint8_t* b = mem + (size_t)(y / 4) * pitch + (size_t)(x / 4) * blk + (blk == 16 ? 8 : 0);
        uint16_t c0, c1;
        uint32_t bits;
        memcpy(&c0, b, 2), memcpy(&c1, b + 2, 2), memcpy(&bits, b + 4, 4);
        uint8_t p[4][3];
        rgb565(c0, p[0]), rgb565(c1, p[1]);
        for (int k = 0; k < 3; ++k)
        {
            if (c0 > c1 || blk == 16)
                p[2][k] = (uint8_t)((2 * p[0][k] + p[1][k]) / 3), p[3][k] = (uint8_t)((p[0][k] + 2 * p[1][k]) / 3);
            else
                p[2][k] = (uint8_t)((p[0][k] + p[1][k]) / 2), p[3][k] = 0;
        }
        const uint32_t idx = (bits >> (2 * ((y & 3) * 4 + (x & 3)))) & 3;
        o[0] = p[idx][0], o[1] = p[idx][1], o[2] = p[idx][2];
        return 1;
    }
    }
    return 0;
}

static GTex* tex_find(uint64_t hash)
{
    for (uint32_t h = (uint32_t)(hash ^ (hash >> 32)) & 2047, n = 0; n < 2048; h = (h + 1) & 2047, ++n)
    {
        if (!g.texh[h])
            return NULL;
        if (g.tex[g.texh[h] - 1].hash == hash)
            return &g.tex[g.texh[h] - 1];
    }
    return NULL;
}

static int is_listed(uint64_t hash)
{
    for (int i = 0; i < g.nlisted; ++i)
        if (g.listed[i] == hash)
            return 1;
    return 0;
}

static int zone_listed(uint64_t hash)
{
    for (int i = 0; i < g.nzlisted; ++i)
        if (g.zlisted[i] == hash)
            return 1;
    return 0;
}

static int never_grass(uint64_t hash)
{
    for (int i = 0; i < g.nznot; ++i)
        if (g.znot[i] == hash)
            return 1;
    return 0;
}

static int tex_is_grass(const GTex* t)
{
    return !never_grass(t->hash) && ((g.nlisted ? is_listed(t->hash) : t->autog) || zone_listed(t->hash));
}

int grass_logging(void) { return g.zone_ok && g.log; }
int grass_layers(void) { return g.zone_ok && g.blended; }
int grass_rings_shown(void) { return g.zone_ok && g.show_rings; }

void grass_rings_view(const float view[16], const float focus[4], float at[4], float off[4])
{
    memset(at, 0, 4 * sizeof(float)), memset(off, 0, 4 * sizeof(float));
    if (focus[3] == 0.0f)
        return;
    for (int i = 0; i < 3; ++i)
        at[i] = focus[0] * view[i] + focus[1] * view[4 + i] + focus[2] * view[8 + i] + view[12 + i];
    at[3] = g.radius;
}

void grass_reject(uint64_t tex_hash, int reason)
{
    const GTex* t = tex_find(tex_hash);
    if (t && tex_is_grass(t) && reason >= 0 && reason < 2)
        ++g.rejected[reason];
}

/* green grass: green over red and blue (West Ronfaure's grass is about 75, 95, 50; its dirt paths 82, 76, 65), by 4% and 22% at the
 * zone's dirt level 50, those leads times dirt_k */
static int green_ok(const uint8_t rgb[3])
{
    return rgb[1] >= 28 && (float)rgb[1] * 100.0f >= (float)rgb[0] * (100.0f + 4.0f * g.dirt_k) &&
        (float)rgb[1] * 100.0f >= (float)rgb[2] * (100.0f + 22.0f * g.dirt_k);
}

/* the yellow-olive of dry grass (grass.dry): a hue of 46 to 70 degrees, not grey; its saturation ((max - min) / max) in *sat. East
 * Sarutabaruta's grass is about 120, 111, 73 (48 degrees, saturation 0.39), half its texels past 46; its dirt and tan ground under
 * 42, as Ronfaure's paths (39). The olive-grey dirt of some paths is in these hues too, only greyer (about 0.24): which of the two
 * a texture holds is tex_sort's, from all its colours in these hues together; Batallia Downs' and Wajaom Woodlands' floors are olive
 * at 0.28 and 0.29, duller than Sarutabaruta's, and were taken for dirt until the bar came down to 0.25 */
static int dry_ok(const uint8_t rgb[3], float* sat)
{
    const int r = rgb[0], gr = rgb[1], b = rgb[2], mx = r > gr ? (r > b ? r : b) : (gr > b ? gr : b), mn = r < gr ? (r < b ? r : b) : (gr < b ? gr : b);
    if (mx < 28 || (float)(mx - mn) * 100.0f < (float)mx * 15.0f * g.dirt_k || b == mx) /* saturation 0.15 at dirt level 50 */
        return 0;
    /* the hue, from red (0) through yellow (60) to green (120): red highest, 60 (g - b) / (mx - mn); green highest, 60 (2 + (b - r) /
     * (mx - mn)) */
    const int hue = mx == r ? 60 * (gr - b) / (mx - mn) : 120 + 60 * (b - r) / (mx - mn);
    if ((float)hue < g.dry_hue || hue > 70)
        return 0;
    *sat = (float)(mx - mn) / (float)mx;
    return 1;
}

/* grass ground or not, from the texture's colours (foliage known): green grass, a twelfth of them green; or with grass.dry, dry
 * grass, a twelfth of them green or yellow-olive and that yellow-olive saturated as dry grass is, not greyed as a path's dirt in the
 * same hues is. A twelfth, not a quarter: a ground tile of dirt with patches of grass in it (Windurst Woods' middle, Konschtat
 * Highlands' paler tiles at 14-19% green) is grass ground too, and its tufts stand where its map says grass (ground_ok), so the
 * tile isn't left bare as a square among the grown ones; stone and dirt alone are 0-5% (mossy walls up to 5%). In green grass,
 * yellow-olive is dirt (a path through it), so tufts there take green alone (ground_ok); in a texture of olive-grey dirt alone (a
 * path drawn on its own), none grow */
#define GRASS_SHARE 12
static void tex_sort(GTex* t)
{
    uint32_t greens = 0, drys = 0;
    float sat = 0.0f;
    double lit[3] = { 0, 0, 0 };
    for (uint32_t k = 0; t->map && k < MAPN * MAPN; ++k)
    {
        const uint8_t* c = t->map + k * 3;
        float s;
        if (green_ok(c))
            ++greens;
        else if (g.dry && dry_ok(c, &s))
            ++drys, sat += s;
        else
            continue;
        lit[0] += c[0], lit[1] += c[1], lit[2] += c[2];
    }
    for (int i = 0; i < 3; ++i)
        t->lit[i] = (greens + drys) ? (float)(lit[i] / (greens + drys)) : (float)(i == 0 ? 90 : i == 1 ? 120 : 60);
    /* green when a quarter is; under that, the larger of the two shares says which grass it is (dry grass with a little green in it
     * grows on its yellow-olive) */
    const int dry_too = t->map && drys && sat >= g.dry_sat * g.dirt_k * (float)drys && (greens + drys) * GRASS_SHARE >= MAPN * MAPN;
    t->green = t->map && (greens * 4 >= MAPN * MAPN || (greens * GRASS_SHARE >= MAPN * MAPN && !(dry_too && drys > greens)));
    t->dry = !t->green && dry_too;
    t->autog = !t->foliage && (t->green || t->dry);
}

/* a tuft's spot of ground is grass: green, or with dry (its texture's: ground_dry) the yellow-olive of dry grass; with grass.shade, in
 * any grass ground, a spot in shadow (too dark to read as any colour) or yellow-olive as saturated as dry grass is too. Bhaflau
 * Thickets' forest floors are green on a third of their texels and dark or olive on the rest, and grew on the third; a path's
 * olive-grey dirt through green grass is greyer than dry grass (about 0.24 to its 0.28 and up) and still grows nothing */
static int ground_ok(const uint8_t rgb[3], int dry)
{
    float s;
    if (green_ok(rgb) || (dry && dry_ok(rgb, &s)))
        return 1;
    if (!g.shade)
        return 0;
    if (rgb[0] < 28 && rgb[1] < 28 && rgb[2] < 28)
        return 1;
    return dry_ok(rgb, &s) && s >= g.dry_sat * g.dirt_k;
}

/* a tuft on a spot in shadow (grass by grass.shade alone: ground_ok) is tinted with its ground's grass colour at 0.6 (tex_sort's lit),
 * not the shadow's own: tinted with a near-black texel it stood as a black silhouette (Bhaflau Thickets' floors by day) */
static void shade_tint(const GTex* t, uint8_t rgb[3], int dry)
{
    float s;
    if (!g.shade || green_ok(rgb) || (dry && dry_ok(rgb, &s)))
        return;
    for (int i = 0; i < 3; ++i)
        rgb[i] = (uint8_t)clampf(t->lit[i] * 0.6f, 0.0f, 255.0f);
}

/* the tufts on texture t take yellow-olive for dry grass: with grass.dry, in dry grass (tex_sort), or in a listed texture (or the
 * zone's own ground) that isn't green grass (it's grass, whatever its colours say) */
static int ground_dry(const GTex* t)
{
    return g.dry && !t->green && (t->dry || (g.nlisted && is_listed(t->hash)) || zone_listed(t->hash));
}

/* every texture's ground-or-not again (grass.dry changed): the world made from them is made again */
static void retag_textures(void)
{
    for (uint32_t i = 0; i < g.ntex; ++i)
        tex_sort(&g.tex[i]);
}

static void dump_bmp(const GTex* t)
{
    if (!t->map)
        return;
#ifdef _WIN32
    _mkdir("logs");
    _mkdir("logs\\grass");
#endif
    char path[128];
    snprintf(path, sizeof path, "logs/grass/%016llx.bmp", (unsigned long long)t->hash);
    FILE* f = fopen(path, "wb");
    if (!f)
        return;
    const uint32_t S = MAPN * 2, bytes = S * S * 3;
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t v = 54 + bytes;
    memcpy(hdr + 2, &v, 4);
    v = 54, memcpy(hdr + 10, &v, 4);
    v = 40, memcpy(hdr + 14, &v, 4);
    v = S, memcpy(hdr + 18, &v, 4), memcpy(hdr + 22, &v, 4);
    hdr[26] = 1, hdr[28] = 24;
    v = bytes, memcpy(hdr + 34, &v, 4);
    fwrite(hdr, 1, sizeof hdr, f);
    for (int y = (int)S - 1; y >= 0; --y)
        for (uint32_t x = 0; x < S; ++x)
        {
            const uint8_t* m = t->map + ((y / 2) * MAPN + x / 2) * 3;
            const uint8_t px[3] = { m[2], m[1], m[0] };
            fwrite(px, 1, 3, f);
        }
    fclose(f);
}

/* The alpha of a texel (255 for a format without): what a cut-out sprite's shape is */
static uint8_t texel_alpha(uint32_t fmt, const uint8_t* mem, uint32_t pitch, uint32_t x, uint32_t y)
{
    switch (fmt)
    {
    case FMT_A8R8G8B8:
        return mem[(size_t)y * pitch + (size_t)x * 4 + 3];
    case FMT_A1R5G5B5:
    {
        uint16_t c;
        memcpy(&c, mem + (size_t)y * pitch + (size_t)x * 2, 2);
        return c >> 15 ? 255 : 0;
    }
    case FMT_A4R4G4B4:
    {
        uint16_t c;
        memcpy(&c, mem + (size_t)y * pitch + (size_t)x * 2, 2);
        return (uint8_t)(((c >> 12) & 15) * 17);
    }
    case FMT_DXT1:
    {
        const uint8_t* b = mem + (size_t)(y / 4) * pitch + (size_t)(x / 4) * 8;
        uint16_t c0, c1;
        uint32_t bits;
        memcpy(&c0, b, 2), memcpy(&c1, b + 2, 2), memcpy(&bits, b + 4, 4);
        return c0 <= c1 && ((bits >> (2 * ((y & 3) * 4 + (x & 3)))) & 3) == 3 ? 0 : 255;
    }
    case FMT_DXT2:
    case FMT_DXT3:
    {
        const uint8_t* b = mem + (size_t)(y / 4) * pitch + (size_t)(x / 4) * 16;
        const uint8_t v = b[(y & 3) * 2 + (x & 3) / 2];
        return (uint8_t)(((x & 1) ? v >> 4 : v & 15) * 17);
    }
    case FMT_DXT4:
    case FMT_DXT5:
    {
        const uint8_t* b = mem + (size_t)(y / 4) * pitch + (size_t)(x / 4) * 16;
        uint64_t bits = 0;
        for (int i = 0; i < 6; ++i)
            bits |= (uint64_t)b[2 + i] << (8 * i);
        const uint32_t idx = (uint32_t)(bits >> (3 * ((y & 3) * 4 + (x & 3)))) & 7, a0 = b[0], a1 = b[1];
        if (idx == 0)
            return (uint8_t)a0;
        if (idx == 1)
            return (uint8_t)a1;
        if (a0 > a1)
            return (uint8_t)(((8 - idx) * a0 + (idx - 1) * a1) / 7);
        if (idx == 6)
            return 0;
        if (idx == 7)
            return 255;
        return (uint8_t)(((6 - idx) * a0 + (idx - 1) * a1) / 5);
    }
    }
    return 255;
}

static uint32_t crc32_of(uint32_t crc, const uint8_t* p, size_t n)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i)
        crc = table[(crc ^ p[i]) & 255] ^ (crc >> 8);
    return ~crc;
}

static void png_chunk(FILE* f, const char* type, const uint8_t* data, uint32_t n)
{
    uint8_t len[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
    fwrite(len, 1, 4, f), fwrite(type, 1, 4, f);
    if (n)
        fwrite(data, 1, n, f);
    uint32_t crc = crc32_of(0, (const uint8_t*)type, 4);
    crc = crc32_of(crc, data, n);
    const uint8_t c[4] = { (uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc };
    fwrite(c, 1, 4, f);
}

/* An RGBA picture (w x h, bytes R G B A row by row) as a PNG, uncompressed. 0 if it can't be written. */
static int write_png(FILE* f, const uint8_t* rgba, uint32_t w, uint32_t h)
{
    const uint32_t row = w * 4 + 1;
    const size_t raw = (size_t)row * h;
    uint8_t* px = (uint8_t*)malloc(raw);
    if (!px)
        return 0;
    for (uint32_t y = 0; y < h; ++y)
    {
        px[(size_t)y * row] = 0;
        memcpy(px + (size_t)y * row + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
    }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    fwrite(sig, 1, 8, f);
    const uint8_t ihdr[13] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 24), (uint8_t)(h >> 16),
        (uint8_t)(h >> 8), (uint8_t)h, 8, 6, 0, 0, 0 };
    png_chunk(f, "IHDR", ihdr, 13);
    /* zlib with stored blocks: the 2-byte header, 64 KB blocks, then the adler checksum */
    const size_t blocks = (raw + 65534) / 65535;
    const size_t total = 2 + raw + blocks * 5 + 4;
    uint8_t* z = (uint8_t*)malloc(total);
    int ok = 0;
    if (z)
    {
        size_t at = 0;
        z[at++] = 0x78, z[at++] = 0x01;
        uint32_t a = 1, b = 0;
        for (size_t off = 0; off < raw; off += 65535)
        {
            const size_t n = raw - off < 65535 ? raw - off : 65535;
            z[at++] = off + n >= raw, z[at++] = (uint8_t)n, z[at++] = (uint8_t)(n >> 8), z[at++] = (uint8_t)~n, z[at++] = (uint8_t)(~n >> 8);
            memcpy(z + at, px + off, n), at += n;
            for (size_t i = 0; i < n; ++i)
                a = (a + px[off + i]) % 65521u, b = (b + a) % 65521u;
        }
        const uint32_t adler = (b << 16) | a;
        z[at++] = (uint8_t)(adler >> 24), z[at++] = (uint8_t)(adler >> 16), z[at++] = (uint8_t)(adler >> 8), z[at++] = (uint8_t)adler;
        png_chunk(f, "IDAT", z, (uint32_t)at);
        free(z);
        ok = 1;
    }
    png_chunk(f, "IEND", NULL, 0);
    free(px);
    return ok;
}

/* a level the game uploaded, decoded: 0xAARRGGBB rows (malloc'd), or NULL */
static uint32_t* decode_level(uint32_t fmt, const uint8_t* mem, uint32_t pitch, uint32_t w, uint32_t h)
{
    uint32_t* out = (uint32_t*)malloc((size_t)w * h * 4);
    if (!out)
        return NULL;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
            uint8_t rgb[3] = { 0, 0, 0 };
            texel_rgb(fmt, mem, pitch, w, h, x, y, rgb);
            out[(size_t)y * w + x] = (uint32_t)texel_alpha(fmt, mem, pitch, x, y) << 24 | (uint32_t)rgb[0] << 16 | (uint32_t)rgb[1] << 8 | rgb[2];
        }
    return out;
}

/* 0xAARRGGBB rows as R G B A bytes (malloc'd) */
static uint8_t* to_rgba(const uint32_t* argb, uint32_t w, uint32_t h)
{
    uint8_t* o = (uint8_t*)malloc((size_t)w * h * 4);
    if (!o)
        return NULL;
    for (size_t i = 0; i < (size_t)w * h; ++i)
        o[4 * i] = (uint8_t)(argb[i] >> 16), o[4 * i + 1] = (uint8_t)(argb[i] >> 8), o[4 * i + 2] = (uint8_t)argb[i], o[4 * i + 3] = (uint8_t)(argb[i] >> 24);
    return o;
}

int grass_write_png(const wchar_t* path, const uint32_t* argb, unsigned w, unsigned h)
{
#ifdef _WIN32
    FILE* f = _wfopen(path, L"wb");
#else
    FILE* f = NULL;
    (void)path;
#endif
    if (!f)
        return 0;
    uint8_t* rgba = to_rgba(argb, w, h);
    const int ok = rgba && write_png(f, rgba, w, h);
    free(rgba);
    fclose(f);
    return ok;
}

/* The texture in full as an RGBA PNG (logs\grass\<hash>.png): the grass log's look at the sheets it sees. At most 120 a session. */
static void dump_png(const GTex* t, const void* mem, uint32_t pitch)
{
    static int dumped;
    if (dumped >= 120 || t->w > 2048 || t->h > 2048)
        return;
    ++dumped;
#ifdef _WIN32
    _mkdir("logs");
    _mkdir("logs\\grass");
#endif
    char path[128];
    snprintf(path, sizeof path, "logs/grass/%016llx.png", (unsigned long long)t->hash);
    FILE* f = fopen(path, "wb");
    if (!f)
        return;
    uint32_t* argb = decode_level(t->fmt, (const uint8_t*)mem, pitch, t->w, t->h);
    uint8_t* rgba = argb ? to_rgba(argb, t->w, t->h) : NULL;
    if (rgba)
        write_png(f, rgba, t->w, t->h);
    free(rgba);
    free(argb);
    fclose(f);
}

/* the log: what the texture is to the grass, and its colours counted (green_ok, dry_ok with its mean saturation, in shadow, the
 * rest) as shares of its map, so ground that reads as neither green nor dry grass can be seen for what it is */
static void log_texture(const GTex* t)
{
    uint32_t greens = 0, drys = 0, darks = 0;
    float sat = 0.0f;
    for (uint32_t k = 0; t->map && k < MAPN * MAPN; ++k)
    {
        const uint8_t* c = t->map + k * 3;
        float s;
        if (green_ok(c))
            ++greens;
        else if (dry_ok(c, &s))
            ++drys, sat += s;
        else if (c[0] < 28 && c[1] < 28 && c[2] < 28)
            ++darks;
    }
    const float per = 100.0f / (float)(MAPN * MAPN);
    rt_log("[recomp] grass: texture %016llx %ux%u format %08x average %d,%d,%d%s%s: green %.0f%%, yellow-olive %.0f%% at %.2f, dark %.0f%%, other %.0f%%\n",
        (unsigned long long)t->hash, t->w, t->h, t->fmt, (int)t->avg[0], (int)t->avg[1], (int)t->avg[2],
        t->autog ? (t->green ? " (green)" : " (dry grass)") : t->foliage ? " (cut-outs: plants or trees)" : " (not grass)",
        never_grass(t->hash) ? " (not_grass: never)" : is_listed(t->hash) ? " (listed)" : zone_listed(t->hash) ? " (the zone's ground)" : "", (float)greens * per, (float)drys * per, drys ? sat / (float)drys : 0.0f, (float)darks * per,
        (float)(MAPN * MAPN - greens - drys - darks) * per);
    dump_bmp(t);
}

/* texture i into the hash table tex_find looks in */
static void tex_slot(uint32_t i)
{
    for (uint32_t k = (uint32_t)(g.tex[i].hash ^ (g.tex[i].hash >> 32)) & 2047;; k = (k + 1) & 2047)
        if (!g.texh[k])
        {
            g.texh[k] = (uint16_t)(i + 1);
            break;
        }
}

void grass_texture(uint64_t hash, uint32_t fmt, uint32_t w, uint32_t h, const void* mem, uint32_t pitch)
{
    if (!g.enabled || tex_find(hash) || !mem || w < 16 || h < 16)
        return;
    if (g.ntex >= TEXMAX)
    {
        g.tex_full = 1; /* d3d8.c hands it over again the next time it's drawn */
        return;
    }
    GTex* t = &g.tex[g.ntex];
    memset(t, 0, sizeof *t);
    t->hash = hash, t->w = w, t->h = h, t->fmt = fmt;
    /* the map: MAPN x MAPN colours, one sample from the middle of each cell; the average from those */
    uint8_t* map = (uint8_t*)malloc(MAPN * MAPN * 3);
    uint32_t got = 0, cut = 0;
    double sum[3] = { 0, 0, 0 };
    if (map)
    {
        for (uint32_t cy = 0; cy < MAPN; ++cy)
            for (uint32_t cx = 0; cx < MAPN; ++cx)
            {
                uint8_t* o = map + (cy * MAPN + cx) * 3;
                if (!texel_rgb(fmt, (const uint8_t*)mem, pitch, w, h, (cx * w + w / 2) / MAPN, (cy * h + h / 2) / MAPN, o))
                    o[0] = o[1] = o[2] = 0;
                else
                {
                    ++got, sum[0] += o[0], sum[1] += o[1], sum[2] += o[2];
                    cut += texel_alpha(fmt, (const uint8_t*)mem, pitch, (cx * w + w / 2) / MAPN, (cy * h + h / 2) / MAPN) < 64;
                }
            }
    }
    if (!got)
    {
        free(map);
        map = NULL;
    }
    else
        t->avg[0] = (float)(sum[0] / got), t->avg[1] = (float)(sum[1] / got), t->avg[2] = (float)(sum[2] / got);
    t->map = map;
    const float r = t->avg[0], gr = t->avg[1], b = t->avg[2];
    /* a tree's texture (leaves and bark on black) is no ground; any other whose colours are grass's is (tex_sort) */
    uint32_t black = 0;
    for (uint32_t i = 0; map && i < MAPN * MAPN; ++i)
    {
        const uint8_t* c = map + i * 3;
        black += c[0] < 14 && c[1] < 14 && c[2] < 14;
    }
    /* ... or one with cut-outs, small: a tree's canopy cards are 256 across or less (their colours under the cut-outs aren't black) */
    t->foliage = map && (black * 16 > MAPN * MAPN /* more than 6% */ || (cut * 25 > MAPN * MAPN && w <= 256 && h <= 256));
    tex_sort(t);
    (void)r, (void)gr, (void)b;
    tex_slot(g.ntex);
    ++g.ntex;
    if (g.log) /* every texture looked at, the ground that isn't grass too: its colours counted say why */
    {
        log_texture(t);
        if (t->foliage || t->autog)
            dump_png(t, mem, pitch);
    }
}

static void relog_textures(void)
{
    for (uint32_t i = 0; i < g.ntex; ++i)
        log_texture(&g.tex[i]);
}

/* The texture table full (tex_full; a long session's zones, or a busy one): the textures that are no grass, no plants and none a
 * mesh was read from go, the rest moved down (the meshes told their new places). d3d8.c hands a texture over each time a draw that
 * could be ground draws with it, so what went is read again when it's drawn. Freeing less than a quarter, all of them go, and the
 * world made from them. */
static void tex_make_room(void)
{
    static uint32_t to[TEXMAX];
    static uint8_t used[TEXMAX];
    g.tex_full = 0;
    memset(used, 0, sizeof used);
    for (uint32_t i = 0; i < g.nmesh; ++i)
        if (g.mesh[i].tex < TEXMAX)
            used[g.mesh[i].tex] = 1;
    uint32_t n = 0;
    for (uint32_t i = 0; i < g.ntex; ++i)
        if (used[i] || g.tex[i].autog || g.tex[i].foliage || is_listed(g.tex[i].hash))
            to[i] = n++;
    const uint32_t was = g.ntex;
    if (n > TEXMAX * 3 / 4)
    {
        for (uint32_t i = 0; i < g.ntex; ++i)
            free(g.tex[i].map);
        g.ntex = 0;
        reset_world();
    }
    else
    {
        uint32_t k = 0;
        for (uint32_t i = 0; i < g.ntex; ++i)
            if (used[i] || g.tex[i].autog || g.tex[i].foliage || is_listed(g.tex[i].hash))
                g.tex[k++] = g.tex[i];
            else
                free(g.tex[i].map);
        g.ntex = n;
        for (uint32_t i = 0; i < g.nmesh; ++i)
            if (g.mesh[i].tex < TEXMAX)
                g.mesh[i].tex = to[g.mesh[i].tex];
    }
    memset(g.texh, 0, sizeof g.texh);
    for (uint32_t i = 0; i < g.ntex; ++i)
        tex_slot(i);
    SAY("[recomp] grass: the texture table filled up (%u): kept %u%s\n", was, g.ntex, g.ntex ? "" : ", starting over");
}

/* the texture's colour at (u, v), wrapped; 0 if it has none to give */
static int tex_color(const GTex* t, float u, float v, uint8_t rgb[3])
{
    if (!t->map)
        return 0;
    u -= floorf(u), v -= floorf(v);
    const int x = (int)(u * MAPN) & (MAPN - 1), y = (int)(v * MAPN) & (MAPN - 1);
    memcpy(rgb, t->map + (y * MAPN + x) * 3, 3);
    return 1;
}

/* the ground's colour round (u, v) as it's seen: the mean of the 3 x 3 colours of the map there, wrapped. A tuft took its one texel,
 * and only a grass texel grows one: on Batallia Downs' olive grass over grey gravel those were darker and more olive than the
 * ground they stood on */
static int tex_near(const GTex* t, float u, float v, uint8_t rgb[3])
{
    if (!t->map)
        return 0;
    u -= floorf(u), v -= floorf(v);
    const int x = (int)(u * MAPN), y = (int)(v * MAPN);
    uint32_t sum[3] = { 0, 0, 0 };
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
        {
            const uint8_t* c = t->map + ((((y + dy) & (MAPN - 1)) * MAPN + ((x + dx) & (MAPN - 1))) * 3);
            sum[0] += c[0], sum[1] += c[1], sum[2] += c[2];
        }
    for (int i = 0; i < 3; ++i)
        rgb[i] = (uint8_t)(sum[i] / 9);
    return 1;
}

/* --- meshes and instances ------------------------------------------------------------------------------- */
static int pool_room(void** pool, uint32_t* cap, uint32_t need, size_t size)
{
    if (need <= *cap)
        return 1;
    uint32_t n = *cap ? *cap : 4096;
    while (n < need)
        n *= 2;
    if (n > 16000000)
        return 0;
    void* p = realloc(*pool, (size_t)n * size);
    if (!p)
        return 0;
    *pool = p, *cap = n;
    return 1;
}

/* vertex v of the draw's own copy: position, texture coordinate, colour */
static int vertex_at(const GfxDraw* d, uint32_t v, float p[3], float uv[2], uint32_t* col)
{
    const GfxElem* ep = &d->vs.el[GFX_R_POSITION];
    if (!ep->used || ep->type < GFX_FLOAT3 || ep->type > GFX_FLOAT4 || !d->cpu[ep->stream])
        return 0;
    {
        const uint32_t stride = (uint32_t)d->u.stride[ep->stream], at = v * stride + (uint32_t)d->u.offset[GFX_R_POSITION];
        if (!stride || at + 12 > d->cpu_size[ep->stream])
            return 0;
        memcpy(p, (const uint8_t*)d->cpu[ep->stream] + at, 12);
    }
    uv[0] = uv[1] = 0.0f;
    const GfxElem* et = &d->vs.el[GFX_R_TEXCOORD0];
    if (et->used && et->type >= GFX_FLOAT2 && et->type <= GFX_FLOAT4 && d->cpu[et->stream])
    {
        const uint32_t stride = (uint32_t)d->u.stride[et->stream], at = v * stride + (uint32_t)d->u.offset[GFX_R_TEXCOORD0];
        if (stride && at + 8 <= d->cpu_size[et->stream])
            memcpy(uv, (const uint8_t*)d->cpu[et->stream] + at, 8);
    }
    *col = 0xFF808080u;
    const GfxElem* ec = &d->vs.el[GFX_R_DIFFUSE];
    if (ec->used && ec->type == GFX_D3DCOLOR && d->cpu[ec->stream])
    {
        const uint32_t stride = (uint32_t)d->u.stride[ec->stream], at = v * stride + (uint32_t)d->u.offset[GFX_R_DIFFUSE];
        if (stride && at + 4 <= d->cpu_size[ec->stream])
            memcpy(col, (const uint8_t*)d->cpu[ec->stream] + at, 4);
    }
    return 1;
}

/* vertex v's normal of the draw's own copy, 0 if its vertices carry none */
static int normal_at(const GfxDraw* d, uint32_t v, float n[3])
{
    const GfxElem* en = &d->vs.el[GFX_R_NORMAL];
    if (!en->used || en->type < GFX_FLOAT3 || en->type > GFX_FLOAT4 || !d->cpu[en->stream])
        return 0;
    const uint32_t stride = (uint32_t)d->u.stride[en->stream], at = v * stride + (uint32_t)d->u.offset[GFX_R_NORMAL];
    if (!stride || at + 12 > d->cpu_size[en->stream])
        return 0;
    memcpy(n, (const uint8_t*)d->cpu[en->stream] + at, 12);
    return n[0] == n[0] && n[1] == n[1] && n[2] == n[2]; /* not NaN */
}

static uint32_t mesh_checksum(const GfxDraw* d, const GrassSrc* s)
{
    const GfxElem* ep = &d->vs.el[GFX_R_POSITION];
    uint32_t h = 2166136261u ^ s->nverts;
    const uint8_t* p = (const uint8_t*)d->cpu[ep->stream];
    uint32_t n = d->cpu_size[ep->stream];
    n = n > 96 ? 96 : n;
    for (uint32_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static uint32_t index_at(const GrassSrc* s, uint32_t i)
{
    return s->index_size == 4 ? ((const uint32_t*)s->indices)[i] : ((const uint16_t*)s->indices)[i];
}

static void capture_mesh(const GfxDraw* d, const GrassSrc* s, GMesh* m)
{
    m->first = g.ntris, m->n = 0;
    for (int j = 0; j < 3; ++j)
        m->lo[j] = 1e30f, m->hi[j] = -1e30f;
    for (uint32_t t = 0; t < s->count; ++t)
    {
        uint32_t i[3];
        if (s->prim == GFX_TRIANGLELIST)
            i[0] = 3 * t, i[1] = 3 * t + 1, i[2] = 3 * t + 2;
        else
            i[0] = t, i[1] = t + 1, i[2] = t + 2;
        GTri tri;
        int ok = 1, hasn = 1;
        tri.n[0] = tri.n[1] = tri.n[2] = 0.0f;
        for (int k = 0; k < 3 && ok; ++k)
        {
            uint32_t v = s->indices ? index_at(s, i[k]) - s->index_lo : i[k];
            ok = v < s->nverts && vertex_at(d, v, tri.p[k], tri.uv[k], &tri.c[k]);
            float nv[3];
            if (ok && hasn && normal_at(d, v, nv))
                tri.n[0] += nv[0], tri.n[1] += nv[1], tri.n[2] += nv[2];
            else
                hasn = 0;
        }
        tri.hasn = (uint8_t)hasn;
        if (!ok)
            continue;
        const float ax = tri.p[1][0] - tri.p[0][0], ay = tri.p[1][1] - tri.p[0][1], az = tri.p[1][2] - tri.p[0][2];
        const float bx = tri.p[2][0] - tri.p[0][0], by = tri.p[2][1] - tri.p[0][1], bz = tri.p[2][2] - tri.p[0][2];
        const float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
        if (nx * nx + ny * ny + nz * nz < 1e-12f)
            continue;
        if (!pool_room((void**)&g.tris, &g.captris, g.ntris + 1, sizeof(GTri)) || g.ntris >= TRIMAX)
        {
            g.want_reset = 1;
            break;
        }
        g.tris[g.ntris++] = tri;
        ++m->n;
        for (int k = 0; k < 3; ++k)
            for (int j = 0; j < 3; ++j)
                m->lo[j] = tri.p[k][j] < m->lo[j] ? tri.p[k][j] : m->lo[j], m->hi[j] = tri.p[k][j] > m->hi[j] ? tri.p[k][j] : m->hi[j];
    }
    if (!m->n)
        m->bad = 1, m->retry = g.scene + 600;
}

/* the draw's transform from model space to clip space, row-vector layout (clip = [x y z 1] * M); 0 if its function isn't one */
static int clip_matrix(const GfxDraw* d, float M[16])
{
    const float step = 16.0f;
    static const float pts[4][3] = { { 0, 0, 0 }, { 16, 0, 0 }, { 0, 16, 0 }, { 0, 0, 16 } };
    float c[4][4];
    for (int i = 0; i < 4; ++i)
        if (!gfx_draw_clip_pos(d, pts[i], c[i]))
            return 0;
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k)
            M[r * 4 + k] = (c[r + 1][k] - c[0][k]) / step;
    for (int k = 0; k < 4; ++k)
        M[12 + k] = c[0][k];
    /* the function has to be a transform: a point off the axes lands where the matrix says */
    static const float probe[3] = { 7.3f, -4.1f, 9.7f };
    float q[4];
    if (!gfx_draw_clip_pos(d, probe, q))
        return 0;
    for (int k = 0; k < 4; ++k)
    {
        const float want = probe[0] * M[k] + probe[1] * M[4 + k] + probe[2] * M[8 + k] + M[12 + k];
        const float scale = fabsf(c[0][k]) + fabsf(M[k]) + fabsf(M[4 + k]) + fabsf(M[8 + k]) + 1.0f;
        if (fabsf(q[k] - want) > 2e-3f * scale)
            return 0;
    }
    return 1;
}

/* a mesh read again: the pieces made from its old reading grow nothing more */
static void drop_instances(GMesh* m)
{
    for (uint32_t i = m->inst_head; i; i = g.inst[i - 1].next)
        g.inst[i - 1].n = 0;
    m->inst_head = 0;
}

#include "grass_slope.h" /* slope_note, slope_light: the tufts lit by their ground's slope, as the game lights it */

/* The light the game puts on a ground draw's upward faces, as D3D lights them: the ambient, and each directional light's (the sun's,
 * the moon's) ambient and its diffuse on a face looking up, a factor on the vertex colour (a zone's ground takes its diffuse and
 * ambient from it). Lamps (points, spots) light only near them: left out. Summed for the scene; grass.match lights the tufts by it.
 * A light the game left unreadable (a NaN direction, which the GPU's max(0, n.l) takes as 0 and lights the ground fine) is taken as
 * the GPU takes it, and a draw whose light still isn't a number counts for nothing: one NaN in the sum stayed in ground_shown until
 * the next zone, every tuft's c17 NaN and drawn black (Crescelius, Windurst by the cooking guild, 2026-11-05) */
static void ground_light_note(const GfxDraw* d)
{
    if (!d->vs.lighting || d->vs.src_diffuse != 1 || d->vs.src_ambient != 1)
        return;
    float up[3], len = 0.0f;
    for (int j = 0; j < 3; ++j)
        up[j] = -d->u.wv[4 + j], len += up[j] * up[j]; /* up (y down, as the game draws) into camera space */
    if (len < 1e-12f)
        return;
    len = 1.0f / sqrtf(len);
    float L[3];
    for (int k = 0; k < 3; ++k)
        L[k] = d->u.ambient[k];
    for (int i = 0; i < d->vs.nlights && i < GFX_NLIGHTS; ++i)
    {
        if (d->vs.light_type[i] != 3)
            continue;
        const GfxLight* li = &d->u.light[i];
        /* maxf(x, 0): a NaN x gives 0 (maxf(0, x) gave the NaN) */
        const float ndl = maxf((up[0] * li->dir[0] + up[1] * li->dir[1] + up[2] * li->dir[2]) * len, 0.0f);
        for (int k = 0; k < 3; ++k)
            L[k] += li->ambient[k] + li->diffuse[k] * ndl;
    }
    for (int k = 0; k < 3; ++k)
        if (!isfinite(L[k]) || fabsf(L[k]) > 64.0f)
            return;
    for (int k = 0; k < 3; ++k)
        g.lsum[k] += L[k];
    ++g.nlit;
    slope_note(d);
}

#include "grass_layers.h" /* layer_new, layer_seen, layer_keeps: the blended ground's layers (grass.blended) */

static void note_one(const GfxDraw* d, const GrassSrc* s)
{
    if (!g.zone_ok || !s->tex_hash || d->vs.rhw || !s->nverts || !s->count)
        return;
    if (s->prim != GFX_TRIANGLELIST && s->prim != GFX_TRIANGLESTRIP)
        return;
    GTex* tx = tex_find(s->tex_hash);
    /* How the game draws a texture says plant or ground better than its colours do. A plant's cut-outs are drawn with the alpha
     * test, and a texture seen so is foliage (Caedarva Mire's leaf and bark atlases, 3103ffca48549d6a and others, were green or dry
     * grass by their colours). Drawn solid (opaque, depth written, no alpha test), it's ground, foliage by its colours or not: Windurst Woods' grass (da73145b62e8fe9b) was a canopy by its alpha, Caedarva Mire's dark forest
     * floors (b2c9c97623da3594, 4d34639a18c5554e) by their black, and the tiles drawn with them were bare. Seen solid once, it's
     * ground for good, whatever draws with the test come after: Vunkerl Inlet [S]'s meadow (ad413131cf4604b7, 61% green) is drawn
     * solid, and somewhere with the test too - taken for cut-outs from then on, its meadow was bare but round the trees (Araedi,
     * 2026-11-21). The test makes plants of textures never seen solid */
    if (tx && !tx->solid)
    {
        const int tested = d->fs.alpha_func != 0 && d->fs.alpha_func != 8;
        const int solid = !tested && !d->pipe.blend && d->depth.zwrite;
        if ((tested && !tx->cut_drawn) || solid)
        {
            tx->cut_drawn = (uint8_t)tested, tx->solid = (uint8_t)!tested, tx->foliage = (uint8_t)tested;
            tex_sort(tx);
            if (g.log)
                rt_log("[recomp] grass: texture %016llx is drawn %s: %s\n", (unsigned long long)tx->hash,
                    tested ? "with the alpha test" : "solid (no alpha test)",
                    tested ? "a plant's cut-outs, unless it's seen solid" : tx->autog ? "ground for good, and grass" : "ground for good, not a plant's cut-outs");
        }
    }
    if (!tx || !tex_is_grass(tx))
        return;
    {
        const GfxElem* ep = &d->vs.el[GFX_R_POSITION];
        if (!ep->used || ep->type < GFX_FLOAT3 || ep->type > GFX_FLOAT4 || !d->cpu[ep->stream])
            return;
    }
    const uint64_t fr = gfx_frame();
    if (fr != g.frame)
    {
        g.frame = fr, g.ndraws = 0;
        if (g.want_reset)
        {
            SAY("[recomp] grass: a table filled up (%u meshes, %u model and %u world triangles, %u pieces): starting over\n", g.nmesh, g.ntris,
                g.nwtris, g.ninst);
            g.want_reset = 0;
            reset_world();
        }
    }
    ground_light_note(d);
    if (g.ndraws >= DRAWMAX)
        return;
    ++g.seen_draws;

    uint64_t key = ((uint64_t)s->vb_id << 32 | s->first) * 0x9E3779B97F4A7C15ull;
    key ^= hash64((uint64_t)s->nverts << 40 ^ (uint64_t)s->count << 8 ^ s->prim ^ (uint64_t)(uintptr_t)s->indices * 31u);
    key ^= hash64(s->tex_hash);
    if (!key)
        key = 1;
    const uint32_t chk = mesh_checksum(d, s);
    GMesh* m = NULL;
    uint32_t h = (uint32_t)(key ^ (key >> 32)) & (MESHMAX * 2 - 1);
    for (uint32_t n = 0; n < MESHMAX * 2; h = (h + 1) & (MESHMAX * 2 - 1), ++n)
    {
        if (!g.meshh[h])
        {
            if (g.nmesh >= MESHMAX)
            {
                g.want_reset = 1;
                return;
            }
            m = &g.mesh[g.nmesh];
            memset(m, 0, sizeof *m);
            m->key = key, m->chk = chk, m->tex = (uint32_t)(tx - g.tex);
            g.meshh[h] = (uint16_t)(++g.nmesh);
            capture_mesh(d, s, m);
            ++g.new_meshes;
            layer_new(d, m);
            break;
        }
        if (g.mesh[g.meshh[h] - 1].key == key)
        {
            m = &g.mesh[g.meshh[h] - 1];
            break;
        }
    }
    if (!m)
        return;
    layer_seen(d, m);
    if (m->bad)
    {
        if (g.scene < m->retry)
            return;
        m->bad = 0, m->chk = chk; /* tried again: its pieces are made afresh */
        drop_instances(m);
        m->first = g.ntris, m->n = 0;
        capture_mesh(d, s, m);
        if (m->bad)
            return;
    }
    if (m->chk != chk) /* the buffer holds something else now: the mesh is read again, its pieces made afresh */
    {
        SAY("[recomp] grass: a %u-triangle ground mesh (texture %016llx) changed in its buffer; read again\n", m->n, (unsigned long long)s->tex_hash);
        drop_instances(m);
        m->chk = chk;
        m->first = g.ntris, m->n = 0;
        capture_mesh(d, s, m);
        if (m->bad)
            return;
    }
    GDraw* dr = &g.draws[g.ndraws];
    dr->mesh = (uint32_t)(m - g.mesh);
    if (!clip_matrix(d, dr->M))
    {
        m->bad = 1, m->retry = g.scene + 600;
        ++g.bad_meshes;
        SAY("[recomp] grass: a %u-triangle ground mesh (texture %016llx) is drawn by a function that isn't a plain transform; left out for now\n",
            m->n, (unsigned long long)s->tex_hash);
        return;
    }
    ++g.ndraws;
}

void grass_note(const GfxDraw* d, const GrassSrc* s)
{
    const uint64_t t0 = g.log ? gfx_now_ns() : 0;
    note_one(d, s);
    if (g.log)
        g.note_ns += gfx_now_ns() - t0;
}

/* --- the brink of a drop ---------------------------------------------------------------------------------- */
/* A tuft on the brink of a drop (a ledge, a rock's top) would hang out over it. So a piece's triangles note, edge by edge and corner
 * by corner, how upright the ground past them is where it falls away (GWTri.enup, vnup), and with grass.edges the tufts near a face
 * steeper than grass.slope grow smaller, none hanging past it (kept_make, brink). A triangle's neighbours are the mesh's triangles
 * (the steep ones too, which grow nothing) with an edge whose corners are at the same places, exactly: a corner shared is the same
 * vertex, or a copy of it. An edge with no neighbour isn't a brink: ground pieces meet that way. */
typedef struct GEdge
{
    uint64_t key; /* its corners' places, hashed, in either order */
    uint32_t tri, e; /* the mesh's triangle and its edge (corners e and e + 1) */
} GEdge;
typedef struct GCorner
{
    uint64_t key;
    uint32_t up;
} GCorner;
static GEdge* g_edges;
static GCorner* g_corners;
static uint32_t* g_emap; /* the mesh's triangle's GWTri (in g.wtris), or UINT32_MAX for none */
static uint32_t g_capedges, g_capcorners, g_capemap;

static uint64_t corner_key(const float p[3])
{
    uint32_t b[3];
    memcpy(b, p, sizeof b);
    return hash64(((uint64_t)b[0] << 32 | b[1]) ^ hash64(0x9E3779B97F4A7C15ull + b[2]));
}

static int edge_cmp(const void* a, const void* b)
{
    const uint64_t x = ((const GEdge*)a)->key, y = ((const GEdge*)b)->key;
    return x < y ? -1 : x > y;
}

static int corner_cmp(const void* a, const void* b)
{
    const uint64_t x = ((const GCorner*)a)->key, y = ((const GCorner*)b)->key;
    return x < y ? -1 : x > y;
}

/* mesh m's triangles laid by W, those made into GWTris (g_emap) told where the ground past them falls away */
static void find_edges(const GMesh* m, const float W[16])
{
    const uint32_t ne = m->n * 3;
    if (!ne || !pool_room((void**)&g_edges, &g_capedges, ne, sizeof(GEdge)))
        return;
    for (uint32_t t = 0; t < m->n; ++t)
    {
        const GTri* tr = &g.tris[m->first + t];
        const uint64_t k[3] = { corner_key(tr->p[0]), corner_key(tr->p[1]), corner_key(tr->p[2]) };
        for (uint32_t e = 0; e < 3; ++e)
        {
            const uint64_t a = k[e], b = k[(e + 1) % 3];
            GEdge* ge = &g_edges[t * 3 + e];
            ge->key = a < b ? hash64(a) ^ b : hash64(b) ^ a;
            ge->tri = t, ge->e = e;
        }
    }
    qsort(g_edges, ne, sizeof(GEdge), edge_cmp);
    uint32_t nbrink = 0;
    for (uint32_t i = 0, j; i < ne; i = j)
    {
        for (j = i + 1; j < ne && g_edges[j].key == g_edges[i].key; ++j)
            ;
        for (uint32_t a = i; a < j && j - i > 1; ++a)
        {
            if (g_emap[g_edges[a].tri] == UINT32_MAX)
                continue;
            GWTri* w = &g.wtris[g_emap[g_edges[a].tri]];
            const uint32_t e = g_edges[a].e;
            const float low = maxf(w->p[e][1], w->p[(e + 1) % 3][1]); /* y is down: the edge's lower end */
            for (uint32_t b = i; b < j; ++b)
            {
                if (g_edges[b].tri == g_edges[a].tri)
                    continue;
                const GTri* nb = &g.tris[m->first + g_edges[b].tri];
                float q[3][3];
                for (int c = 0; c < 3; ++c)
                    for (int k = 0; k < 3; ++k)
                        q[c][k] = nb->p[c][0] * W[k] + nb->p[c][1] * W[4 + k] + nb->p[c][2] * W[8 + k] + W[12 + k];
                /* the neighbour's corner off the edge, below it: the ground falls away there */
                if (!(q[(g_edges[b].e + 2) % 3][1] > low + 0.05f))
                    continue;
                const float ax = q[1][0] - q[0][0], ay = q[1][1] - q[0][1], az = q[1][2] - q[0][2];
                const float bx = q[2][0] - q[0][0], by = q[2][1] - q[0][1], bz = q[2][2] - q[0][2];
                const float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
                const float len = sqrtf(nx * nx + ny * ny + nz * nz);
                if (!(len > 1e-9f))
                    continue;
                const uint8_t up = (uint8_t)(clampf(fabsf(ny) / len, 0.0f, 1.0f) * 254.0f);
                if (up < w->enup[e])
                    w->enup[e] = up, ++nbrink;
            }
        }
    }
    if (!nbrink || !pool_room((void**)&g_corners, &g_capcorners, nbrink * 2, sizeof(GCorner)))
        return;
    /* the corners on a brink: the least upright ground falling away at each, for the triangles that only touch it there */
    uint32_t nc = 0;
    for (uint32_t t = 0; t < m->n && nc + 2 <= nbrink * 2; ++t)
    {
        if (g_emap[t] == UINT32_MAX)
            continue;
        const GWTri* w = &g.wtris[g_emap[t]];
        const GTri* tr = &g.tris[m->first + t];
        for (uint32_t e = 0; e < 3 && nc + 2 <= nbrink * 2; ++e)
            if (w->enup[e] != 255)
            {
                g_corners[nc].key = corner_key(tr->p[e]), g_corners[nc++].up = w->enup[e];
                g_corners[nc].key = corner_key(tr->p[(e + 1) % 3]), g_corners[nc++].up = w->enup[e];
            }
    }
    qsort(g_corners, nc, sizeof(GCorner), corner_cmp);
    for (uint32_t t = 0; t < m->n; ++t)
    {
        if (g_emap[t] == UINT32_MAX)
            continue;
        GWTri* w = &g.wtris[g_emap[t]];
        const GTri* tr = &g.tris[m->first + t];
        for (int c = 0; c < 3; ++c)
        {
            const GCorner key = { corner_key(tr->p[c]), 0 };
            const GCorner* f = (const GCorner*)bsearch(&key, g_corners, nc, sizeof(GCorner), corner_cmp);
            if (!f)
                continue;
            while (f > g_corners && f[-1].key == key.key) /* the first of its run */
                --f;
            for (; f < g_corners + nc && f->key == key.key; ++f)
                if (f->up < w->vnup[c])
                    w->vnup[c] = (uint8_t)f->up;
        }
    }
}

/* A piece's triangles are often metres wide, so a slope's limit taken triangle by triangle stops the tufts in a straight line along
 * a cliff's foot. Each corner is told how upright the ground meeting there is (the area-weighted mean of the mesh's triangles
 * sharing it, the steep ones it doesn't grow on too), and kept_make takes a spot's from its corners': the tufts thin out across the
 * triangles by a steep face, not at their edges. */
typedef struct GSmooth
{
    uint64_t key;
    float wu, w; /* the triangles' up times area, summed; their areas */
} GSmooth;
static GSmooth* g_smooth;
static uint32_t g_capsmooth;

static int smooth_cmp(const void* a, const void* b)
{
    const uint64_t x = ((const GSmooth*)a)->key, y = ((const GSmooth*)b)->key;
    return x < y ? -1 : x > y;
}

/* mesh m's triangles laid by W, those made into GWTris (g_emap) told how upright the ground is at their corners */
static void smooth_corners(const GMesh* m, const float W[16])
{
    const uint32_t nc = m->n * 3;
    if (!nc || !pool_room((void**)&g_smooth, &g_capsmooth, nc, sizeof(GSmooth)))
        return;
    for (uint32_t t = 0; t < m->n; ++t)
    {
        const GTri* tr = &g.tris[m->first + t];
        float q[3][3];
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k)
                q[c][k] = tr->p[c][0] * W[k] + tr->p[c][1] * W[4 + k] + tr->p[c][2] * W[8 + k] + W[12 + k];
        const float ax = q[1][0] - q[0][0], ay = q[1][1] - q[0][1], az = q[1][2] - q[0][2];
        const float bx = q[2][0] - q[0][0], by = q[2][1] - q[0][1], bz = q[2][2] - q[0][2];
        const float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
        const float len = sqrtf(nx * nx + ny * ny + nz * nz);
        const float up = len > 1e-9f ? fabsf(ny) / len : 0.0f, area = 0.5f * len;
        for (int c = 0; c < 3; ++c)
            g_smooth[t * 3 + c].key = corner_key(tr->p[c]), g_smooth[t * 3 + c].wu = up * area, g_smooth[t * 3 + c].w = area;
    }
    qsort(g_smooth, nc, sizeof(GSmooth), smooth_cmp);
    uint32_t u = 0; /* the runs of one corner summed into one each */
    for (uint32_t i = 0; i < nc; ++i)
        if (u && g_smooth[u - 1].key == g_smooth[i].key)
            g_smooth[u - 1].wu += g_smooth[i].wu, g_smooth[u - 1].w += g_smooth[i].w;
        else
            g_smooth[u++] = g_smooth[i];
    for (uint32_t t = 0; t < m->n; ++t)
    {
        if (g_emap[t] == UINT32_MAX)
            continue;
        GWTri* w = &g.wtris[g_emap[t]];
        const GTri* tr = &g.tris[m->first + t];
        for (int c = 0; c < 3; ++c)
        {
            const GSmooth key = { corner_key(tr->p[c]), 0, 0 };
            const GSmooth* f = (const GSmooth*)bsearch(&key, g_smooth, u, sizeof(GSmooth), smooth_cmp);
            if (f && f->w > 1e-9f)
                w->vup[c] = (uint8_t)(clampf(f->wu / f->w, 0.0f, 1.0f) * 254.0f + 0.5f);
        }
    }
}

/* how far p is across the ground (x, z) from the brink of a drop its triangle has (find_edges) steeper than grass.slope: 1e30 none */
static float brink(const GWTri* t, const float p[3])
{
    const float lim = g.slope_cos * 254.0f;
    float d = 1e30f;
    for (int k = 0; k < 3; ++k)
    {
        if ((float)t->enup[k] < lim)
        {
            const float* a = t->p[k];
            const float* b = t->p[(k + 1) % 3];
            const float ex = b[0] - a[0], ez = b[2] - a[2], px = p[0] - a[0], pz = p[2] - a[2], ll = ex * ex + ez * ez;
            const float s = ll > 1e-12f ? clampf((px * ex + pz * ez) / ll, 0.0f, 1.0f) : 0.0f;
            const float dx = px - s * ex, dz = pz - s * ez;
            d = minf(d, sqrtf(dx * dx + dz * dz));
        }
        if ((float)t->vnup[k] < lim)
        {
            const float dx = p[0] - t->p[k][0], dz = p[2] - t->p[k][2];
            d = minf(d, sqrtf(dx * dx + dz * dz));
        }
    }
    return d;
}

/* with grass.pathpad: whether ground that isn't grass (a path's dirt, ground_ok) lies within g.pathpad of p, on the triangle's own
 * texture: the triangle's texture coordinates carried on past its edges (the plane they make, in x and z) to a ring of points round
 * p, two rings (half the padding and all of it) so a thin path isn't stepped over. A path laid as a tile of its own isn't found
 * from the next tile's grass: it keeps its own tufts out (tex_color's miss), the padding is for paths drawn in the grass's texture. */
static int path_near(const GWTri* t, const GTex* tx, const float p[3], int dry)
{
    const float x0 = t->p[0][0], z0 = t->p[0][2];
    const float ax = t->p[1][0] - x0, az = t->p[1][2] - z0, bx = t->p[2][0] - x0, bz = t->p[2][2] - z0;
    const float den = ax * bz - bx * az;
    if (!(fabsf(den) > 1e-9f))
        return 0;
    const float inv = 1.0f / den;
    for (int ring = 0; ring < 2; ++ring)
    {
        const float r = g.pathpad * (ring ? 1.0f : 0.5f), turn = ring ? 0.0f : 0.3926991f;
        for (int k = 0; k < 8; ++k)
        {
            const float a = (float)k * 0.7853982f + turn;
            const float qx = p[0] + r * cosf(a) - x0, qz = p[2] + r * sinf(a) - z0;
            const float u = (qx * bz - bx * qz) * inv, w = (ax * qz - qx * az) * inv;
            if (u < -4.0f || w < -4.0f || u + w > 5.0f) /* far off a small triangle: its picture says nothing of the ground there */
                continue;
            const float c0 = 1.0f - u - w;
            uint8_t rgb[3];
            if (tex_color(tx, c0 * t->uv[0][0] + u * t->uv[1][0] + w * t->uv[2][0],
                    c0 * t->uv[0][1] + u * t->uv[1][1] + w * t->uv[2][1], rgb) &&
                !ground_ok(rgb, dry))
                return 1;
        }
    }
    return 0;
}

/* --- the camera ------------------------------------------------------------------------------------------- */
/* The scene's camera, for taking a draw's clip transform back to the world (clip_to_world). Taken back through the projection's
 * inverse (its z and w columns: the near plane's 1 / 0.1 in them) and then the view's, a placement landed a hair off, and the hair
 * was the camera's distance from the zone's origin times the piece's distance from the camera, times about 1e-6: a yalm or more
 * in a large zone (Rolanberry), different each frame the camera moved. Past the matching's tolerance, a piece of ground was taken
 * for a mover the moment the player walked, or made again every frame. So the placement is found from the clip transform's x, y
 * and w alone, where it stands exactly: each row of the model's transform has a known w in view space (1 the origin, 0 a
 * direction), and the projection's first three rows, x, y and w columns, take a row's view-space x, y, z to its clip x, y, w.
 * Inverted (3 x 3, in double) they give the view-space row from the clip row, and the view's inverse the world's. 0: the
 * projection isn't a perspective one. */
static int cam_prepare(const GfxScene* s)
{
    const float* p = s->proj;
    const double a[9] = { p[0], p[1], p[3], p[4], p[5], p[7], p[8], p[9], p[11] };
    const double det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
    double v[16];
    for (int i = 0; i < 16; ++i)
        v[i] = s->view[i];
    if (!(fabs(det) > 1e-30) || !mat_inv_d(g.cam.vinv, v))
        return 0;
    double* ai = g.cam.ai;
    ai[0] = (a[4] * a[8] - a[5] * a[7]) / det, ai[1] = (a[2] * a[7] - a[1] * a[8]) / det, ai[2] = (a[1] * a[5] - a[2] * a[4]) / det;
    ai[3] = (a[5] * a[6] - a[3] * a[8]) / det, ai[4] = (a[0] * a[8] - a[2] * a[6]) / det, ai[5] = (a[2] * a[3] - a[0] * a[5]) / det;
    ai[6] = (a[3] * a[7] - a[4] * a[6]) / det, ai[7] = (a[1] * a[6] - a[0] * a[7]) / det, ai[8] = (a[0] * a[4] - a[1] * a[3]) / det;
    g.cam.p3[0] = p[12], g.cam.p3[1] = p[13], g.cam.p3[2] = p[15];
    return 1;
}

/* the draw's transform from model space to the world (row-vector layout; W[15] 1, the rows' w 0), from its clip transform M
 * (clip_matrix) under the scene's camera (cam_prepare) */
static void clip_to_world(const float M[16], float W[16])
{
    double mv[16], w[16];
    for (int r = 0; r < 4; ++r)
    {
        const double vw = r == 3 ? 1.0 : 0.0;
        const double b[3] = { M[r * 4] - vw * g.cam.p3[0], M[r * 4 + 1] - vw * g.cam.p3[1], M[r * 4 + 3] - vw * g.cam.p3[2] };
        for (int j = 0; j < 3; ++j)
            mv[r * 4 + j] = b[0] * g.cam.ai[j] + b[1] * g.cam.ai[3 + j] + b[2] * g.cam.ai[6 + j];
        mv[r * 4 + 3] = vw;
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            w[r * 4 + c] = mv[r * 4] * g.cam.vinv[c] + mv[r * 4 + 1] * g.cam.vinv[4 + c] + mv[r * 4 + 2] * g.cam.vinv[8 + c] +
                mv[r * 4 + 3] * g.cam.vinv[12 + c];
    for (int i = 0; i < 16; ++i)
        W[i] = (float)w[i];
    W[3] = W[7] = W[11] = 0.0f, W[15] = 1.0f;
}

#define CEILING_COS 0.25f /* a face whose normal looks down by more than this (cosine to straight down) is a ceiling */

/* with grass.log, a ground piece as made (instance_for): its height over the player's (y down) tells the ground underfoot from a
 * log's top or a ledge */
static void inst_log(const GInst* in, const GMesh* m, uint32_t ceilings, const float* focus)
{
    rt_log("[recomp] grass: ground piece %u (texture %016llx%s): %u of %u triangles face up (%u looked down by their normals), at "
           "%.1f %.1f %.1f, %.1f over you, %.1f wide\n", g.ninst, (unsigned long long)g.tex[m->tex].hash,
        m->blended ? ", a blended layer" : "", in->n, m->n, ceilings, in->ctr[0], in->ctr[1], in->ctr[2], focus[1] - in->ctr[1],
        2.0f * in->rad);
}

/* an instance of the mesh drawn with this clip transform, made on first sight; one already made stands within a couple of
 * units of where this one does (the same tile of ground is never laid that close to itself). Seen under the same camera as its
 * placement was found under, a still piece lands on the very same numbers (the game's constants are the same, and all that's
 * made from them), so one that doesn't moves: a tree's sway shears its transform, a platform carries it; it grows nothing
 * until MOVING_RETRY scenes on. Under a camera that has changed there's nothing to judge it by (the game's numbers for a still
 * piece change with the camera, and the recovery's own wobble, though under a hundredth, is the camera's), so its placement is
 * found afresh. */
static GInst* instance_for(const GDraw* dr, float minup, const float* focus, float reach)
{
    float W[16];
    clip_to_world(dr->M, W);
    GMesh* m = &g.mesh[dr->mesh];
    /* a piece farther than the grass reaches is neither made nor judged: far off, the transform found from the game's own (a
     * zone's distant ground, its level of detail) differs from frame to frame, and the pools would fill with what grows nothing */
    float dist;
    {
        float wc[3], scale = 0.0f;
        const float hx = m->hi[0] - m->lo[0], hy = m->hi[1] - m->lo[1], hz = m->hi[2] - m->lo[2];
        for (int j = 0; j < 3; ++j)
            wc[j] = 0.5f * (m->lo[0] + m->hi[0]) * W[j] + 0.5f * (m->lo[1] + m->hi[1]) * W[4 + j] + 0.5f * (m->lo[2] + m->hi[2]) * W[8 + j] + W[12 + j];
        for (int r = 0; r < 3; ++r)
        {
            const float len = sqrtf(W[4 * r] * W[4 * r] + W[4 * r + 1] * W[4 * r + 1] + W[4 * r + 2] * W[4 * r + 2]);
            scale = len > scale ? len : scale;
        }
        const float dx = wc[0] - focus[0], dz = wc[2] - focus[2];
        dist = sqrtf(dx * dx + dz * dz);
        if (dist - 0.5f * sqrtf(hx * hx + hy * hy + hz * hz) * scale > reach)
            return NULL;
    }
    /* the same piece as one made before: within tol + kNear of where that one was last found (one mesh is often many pieces of
     * ground, the same tile laid many times, never that close to itself); under the same camera as that, still within tol of it
     * and turned the same to within tol3, else it moves */
    const float tol = 0.33f + 0.01f * dist, tol3 = 0.04f + 0.001f * dist;
    static const float kNear = 2.0f;
    uint8_t tries = 0; /* the pieces seen moving where this one is: a new piece made here waits longer if it moves too */
    for (uint32_t i = m->inst_head; i; i = g.inst[i - 1].next)
    {
        GInst* in = &g.inst[i - 1];
        if (fabsf(in->t[0] - W[12]) > tol + kNear || fabsf(in->t[1] - W[13]) > tol + kNear || fabsf(in->t[2] - W[14]) > tol + kNear)
            continue;
        if (in->moving && g.scene >= in->retry)
        {
            /* its time out is over: as if it weren't there, a new piece is made where this draw is and judged afresh */
            tries = in->tries < 255 && in->tries + 1 > tries ? (uint8_t)(in->tries + 1) : tries;
            continue;
        }
        if (in->moving)
            return NULL;
        if (in->cam != g.cam_id)
        {
            memcpy(in->t, &W[12], sizeof in->t);
            for (int k = 0; k < 9; ++k)
                in->w3[k] = W[k + k / 3];
            in->cam = g.cam_id;
            return in;
        }
        float dm = 0.0f;
        for (int k = 0; k < 9; ++k)
            dm = maxf(dm, fabsf(in->w3[k] - W[k + k / 3]));
        if (fabsf(in->t[0] - W[12]) <= tol && fabsf(in->t[1] - W[13]) <= tol && fabsf(in->t[2] - W[14]) <= tol && dm <= tol3)
            return in;
        const unsigned wait = (unsigned)MOVING_RETRY << (in->tries < 5 ? in->tries : 5);
        in->moving = 1, in->n = 0, in->retry = g.scene + wait;
        ++g.moving;
        SAY("[recomp] grass: a %u-triangle piece (texture %016llx) moved at %.1f %.1f %.1f (by %.2f, turned %.3f) with the camera still; it "
            "grows nothing for %u scenes\n",
            m->n, (unsigned long long)g.tex[m->tex].hash, W[12], W[13], W[14],
            maxf(maxf(fabsf(in->t[0] - W[12]), fabsf(in->t[1] - W[13])), fabsf(in->t[2] - W[14])), dm, wait);
        return NULL;
    }
    if (g.ninst >= INSTMAX)
    {
        g.want_reset = 1;
        return NULL;
    }
    {
        GInst* in = &g.inst[g.ninst];
        memset(in, 0, sizeof *in);
        in->stamp = ++g.stamps ? g.stamps : ++g.stamps;
        in->mesh = dr->mesh, in->first = g.nwtris, in->t[0] = W[12], in->t[1] = W[13], in->t[2] = W[14];
        for (int k = 0; k < 9; ++k)
            in->w3[k] = W[k + k / 3];
        in->cam = g.cam_id, in->tries = tries;
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        uint32_t ceilings = 0; /* the triangles left out for looking down */
        const int edges = pool_room((void**)&g_emap, &g_capemap, m->n ? m->n : 1, sizeof(uint32_t));
        if (edges)
            memset(g_emap, 0xFF, (size_t)m->n * sizeof(uint32_t));
        for (uint32_t t = 0; t < m->n; ++t)
        {
            const GTri* src = &g.tris[m->first + t];
            GWTri w;
            memset(w.enup, 255, sizeof w.enup), memset(w.vnup, 255, sizeof w.vnup);
            for (int k = 0; k < 3; ++k)
            {
                const float* p = src->p[k];
                for (int j = 0; j < 3; ++j)
                    w.p[k][j] = p[0] * W[j] + p[1] * W[4 + j] + p[2] * W[8 + j] + W[12 + j];
                w.uv[k][0] = src->uv[k][0], w.uv[k][1] = src->uv[k][1], w.c[k] = src->c[k];
            }
            const float ax = w.p[1][0] - w.p[0][0], ay = w.p[1][1] - w.p[0][1], az = w.p[1][2] - w.p[0][2];
            const float bx = w.p[2][0] - w.p[0][0], by = w.p[2][1] - w.p[0][1], bz = w.p[2][2] - w.p[0][2];
            const float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
            const float len = sqrtf(nx * nx + ny * ny + nz * nz);
            if (!(len > 1e-6f) || fabsf(ny) / len < minup)
                continue;
            /* a ceiling: the winding is no telling of which way a face looks (ground is drawn both ways), but its normals, the ones the
             * game lights it by, are the face the game shows - one looking down (y is down: toward +y) is a roof, a cave's, a bough's
             * underside, and grows nothing */
            if (src->hasn)
            {
                const float wy = src->n[0] * W[1] + src->n[1] * W[5] + src->n[2] * W[9];
                const float wx = src->n[0] * W[0] + src->n[1] * W[4] + src->n[2] * W[8];
                const float wz = src->n[0] * W[2] + src->n[1] * W[6] + src->n[2] * W[10];
                const float wl = sqrtf(wx * wx + wy * wy + wz * wz);
                if (wl > 1e-6f && wy / wl > CEILING_COS)
                {
                    ++ceilings;
                    continue;
                }
            }
            slope_normal(&w, src, W, nx / len, ny / len, nz / len); /* the normal it's lit by (grass_slope.h) */
            w.area = 0.5f * len;
            w.up = fabsf(ny) / len;
            memset(w.vup, (int)(w.up * 254.0f + 0.5f), sizeof w.vup); /* its own, until smooth_corners */
            for (int j = 0; j < 3; ++j)
                w.ctr[j] = (w.p[0][j] + w.p[1][j] + w.p[2][j]) / 3.0f;
            w.rad = 0.0f;
            for (int k = 0; k < 3; ++k)
            {
                const float dx = w.p[k][0] - w.ctr[0], dy = w.p[k][1] - w.ctr[1], dz = w.p[k][2] - w.ctr[2];
                const float r = sqrtf(dx * dx + dy * dy + dz * dz);
                w.rad = r > w.rad ? r : w.rad;
                for (int j = 0; j < 3; ++j)
                    lo[j] = w.p[k][j] < lo[j] ? w.p[k][j] : lo[j], hi[j] = w.p[k][j] > hi[j] ? w.p[k][j] : hi[j];
            }
            if (!pool_room((void**)&g.wtris, &g.capwtris, g.nwtris + 1, sizeof(GWTri)) || g.nwtris >= TRIMAX)
            {
                g.want_reset = 1;
                break;
            }
            if (edges)
                g_emap[t] = g.nwtris;
            g.wtris[g.nwtris++] = w;
            ++in->n;
        }
        if (edges && in->n)
            find_edges(m, W), smooth_corners(m, W);
        for (int j = 0; j < 3; ++j)
            in->ctr[j] = 0.5f * (lo[j] + hi[j]);
        in->rad = in->n ? 0.5f * sqrtf((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2])) : 0.0f;
        ++g.ninst;
        in->next = m->inst_head, m->inst_head = g.ninst;
        ++g.new_insts;
        if (g.log)
            inst_log(in, m, ceilings, focus);
        return in;
    }
}

/* --- the ground the player can stand on ------------------------------------------------------------------- */
/* With grass.walkable the tufts grow only where the player could stand: the zone's collision (VanaCore reads it from the zone's
 * DAT: grass_set_floors) has a floor at the tuft's place, of ground the game's footsteps call path, grass, sand, snow or stone -
 * not "object" or wood (a crate's top, a cart's), metal, or water (a river's bed, a marsh: GRASS_WET). With no floor at its place
 * but one below it (a tree's boughs, a cart's top with no floor of its own, a roof) none grows; with none at all (ground out of
 * reach: the hills round a zone, its far background) it grows. In the zones looked at, the collision lies on the drawn ground to
 * the hundredth: "at" is within FLOOR_NEAR.
 *
 * Walkable or not, none grows where a floor lies over its place more than FLOOR_OVER up (a hard floor, FLOOR_HARD) and within the
 * tallest tuft's reach: a building's floor laid over the ground it stands on (the ground drawn on under it, hidden), which the
 * tufts came up through; nor on a floor the footsteps call shallow water (GRASS_WET).
 *
 * The floors are kept as the DAT has them (each mesh once, placed many times) and laid in the world a FLOOR_TILE square at a
 * time when a tuft first looks there: the triangles that aren't upright, listed by cell. */
#define FLOOR_TILE 32.0f
#define FLOOR_CELLS 8   /* a square's cells each way */
#define FLOOR_NEAR 0.5f  /* a floor this near a tuft's place, above or below, is the ground it stands on */
#define FLOOR_DEEP 60.0f /* ... one further below than that, to this, a floor the place stands above */
#define FLOOR_OVER 0.25f /* a floor more than this over the place is over it, not the ground it stands on */
#define FLOOR_HARD 0.05f /* ... a hard one (stone, wood, metal, an object's), or one over the ground's own, more than this */
static float g_floor_at; /* floor_terrain's floor the place stands on: how far below it (y down; under 0 over it), for grass.log */
/* The footsteps' shallow water: with grass.nowater (the default) no tufts on it, walkable or not. The game splashes there whatever
 * the ground looks like: Dragon's Aery's meadow under its waterfalls is a green grass texture with no water drawn over it, all of
 * it this (2026-11-06), and it's better bare than grassed. (Pashhow's marsh is grass ground tagged so too: bare as well.) */
#define GRASS_WET 8
#define FLOOR_TILES 64   /* the squares kept laid; the one looked at longest ago goes for a new one */
#define FLOOR_WAIT 600   /* scenes a new zone's tufts wait for its floors (ten seconds at 60 FPS), then grow without them */

typedef struct FTri /* a floor's triangle in the world: a corner (x, z), its edges from there, their cross's inverse; the heights */
{
    float ax, az, e1x, e1z, e2x, e2z, inv, y0, dy1, dy2;
    uint8_t terrain;
} FTri;

typedef struct FTile
{
    int32_t tx, tz;
    uint32_t used; /* the scene it was last looked at in, + 1 (0: not laid) */
    FTri* tri;
    uint32_t* list; /* cell c's triangles: list[cell[c] .. cell[c + 1]) */
    uint32_t cell[FLOOR_CELLS * FLOOR_CELLS + 1];
} FTile;

static struct
{
    int zone; /* the zone they're for (0: none known) */
    float* tris;
    uint8_t* terrain;
    uint32_t *mesh_first, nmesh;
    float* mat;
    uint32_t *pairs, npairs;
    /* the placements by the FLOOR_TILE squares their bounds reach: square (x0 + i, z0 + j)'s are bidx[bfirst[j * nx + i] .. + 1]) */
    int32_t x0, z0, nx, nz;
    uint32_t *bfirst, *bidx;
    FTile tile[FLOOR_TILES];
    uint32_t wait_from; /* the scene the zone changed in (FLOOR_WAIT) */
    uint32_t wet;       /* the triangles of the footsteps' water (terrain 8, 9): none, and no sheet of the zone's can be water */
    uint8_t* bwet;      /* by square as bfirst: 1 where a placement with the footsteps' water reaches */
    uint32_t set;       /* counts the hand-overs (grass_floor_water) */
} gf;

static void floor_free(void)
{
    free(gf.tris), free(gf.terrain), free(gf.mesh_first), free(gf.mat), free(gf.pairs), free(gf.bfirst), free(gf.bidx), free(gf.bwet);
    for (int i = 0; i < FLOOR_TILES; ++i)
        free(gf.tile[i].tri), free(gf.tile[i].list);
    const uint32_t wait = gf.wait_from, set = gf.set;
    memset(&gf, 0, sizeof gf);
    gf.wait_from = wait, gf.set = set + 1;
}

static void floor_wait(void) { gf.wait_from = g.scene; }

/* point p of a mesh through placement matrix m */
static void floor_place(const float* m, const float* p, float o[3])
{
    for (int j = 0; j < 3; ++j)
        o[j] = p[0] * m[j] + p[1] * m[3 + j] + p[2] * m[6 + j] + m[9 + j];
}

/* pair k's bounds in the world, as FLOOR_TILE squares (x0, z0, x1, z1), from its mesh's bounds (lo, hi: 6 floats a mesh); 0 for
 * a placement no floor of a zone could be (wider than 64 squares, or none): it's left out */
static int floor_reach(const float* bounds, uint32_t k, int32_t r[4])
{
    const float* m = &gf.mat[gf.pairs[2 * k] * 12];
    const float* b = &bounds[gf.pairs[2 * k + 1] * 6];
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (int c = 0; c < 8; ++c)
    {
        const float p[3] = { b[(c & 1) ? 3 : 0], b[(c & 2) ? 4 : 1], b[(c & 4) ? 5 : 2] };
        float w[3];
        floor_place(m, p, w);
        for (int j = 0; j < 3; ++j)
            lo[j] = minf(lo[j], w[j]), hi[j] = maxf(hi[j], w[j]);
    }
    r[0] = (int32_t)floorf(clampf(lo[0], -1e5f, 1e5f) / FLOOR_TILE), r[1] = (int32_t)floorf(clampf(lo[2], -1e5f, 1e5f) / FLOOR_TILE);
    r[2] = (int32_t)floorf(clampf(hi[0], -1e5f, 1e5f) / FLOOR_TILE), r[3] = (int32_t)floorf(clampf(hi[2], -1e5f, 1e5f) / FLOOR_TILE);
    return r[2] >= r[0] && r[3] >= r[1] && r[2] - r[0] <= 64 && r[3] - r[1] <= 64;
}

/* gf.bwet: the squares a placement of a mesh with the footsteps' water reaches (none: gf.bwet stays NULL, and every square is
 * taken to have it) */
static void floor_mark_wet(const float* bounds)
{
    if (!gf.wet || !gf.bfirst || !(gf.bwet = (uint8_t*)calloc((size_t)gf.nx * gf.nz, 1)))
        return;
    for (uint32_t k = 0; k < gf.npairs; ++k)
    {
        const uint32_t mesh = gf.pairs[2 * k + 1];
        int wet = 0;
        for (uint32_t t = gf.mesh_first[mesh]; !wet && t < gf.mesh_first[mesh + 1]; ++t)
            wet = gf.terrain[t] == 8 || gf.terrain[t] == 9;
        int32_t r[4];
        if (!wet || !floor_reach(bounds, k, r))
            continue;
        for (int32_t z = r[1]; z <= r[3]; ++z)
            for (int32_t x = r[0]; x <= r[2]; ++x)
                gf.bwet[(size_t)(z - gf.z0) * gf.nx + (x - gf.x0)] = 1;
    }
}

void grass_set_floors(const GrassFloors* f)
{
    floor_free();
    if (!f || !f->zone)
        return;
    gf.zone = f->zone;
    /* what it holds checked: anything out of place and the zone has none */
    int ok = f->npairs && f->nmesh && f->ntris && f->tris && f->terrain && f->mesh_first && f->matrices && f->pairs &&
        f->mesh_first[f->nmesh] <= f->ntris;
    for (uint32_t m = 0; ok && m < f->nmesh; ++m)
        ok = f->mesh_first[m] <= f->mesh_first[m + 1];
    for (uint32_t k = 0; ok && k < f->npairs; ++k)
        ok = f->pairs[2 * k] < f->nmatrices && f->pairs[2 * k + 1] < f->nmesh;
    float* bounds = ok ? (float*)malloc((size_t)f->nmesh * 6 * sizeof(float)) : NULL;
    gf.tris = bounds ? (float*)malloc((size_t)f->ntris * 9 * sizeof(float)) : NULL;
    gf.terrain = gf.tris ? (uint8_t*)malloc(f->ntris) : NULL;
    gf.mesh_first = gf.terrain ? (uint32_t*)malloc(((size_t)f->nmesh + 1) * sizeof(uint32_t)) : NULL;
    gf.mat = gf.mesh_first ? (float*)malloc((size_t)f->nmatrices * 12 * sizeof(float)) : NULL;
    gf.pairs = gf.mat ? (uint32_t*)malloc((size_t)f->npairs * 2 * sizeof(uint32_t)) : NULL;
    if (!gf.pairs)
    {
        free(bounds);
        floor_free();
        gf.zone = f->zone;
        SAY("[recomp] grass: zone %d's floors left out (%s): its tufts grow on all its grass ground\n", f->zone, ok ? "no memory" : "not whole");
        return;
    }
    memcpy(gf.tris, f->tris, (size_t)f->ntris * 9 * sizeof(float));
    memcpy(gf.terrain, f->terrain, f->ntris);
    for (uint32_t t = 0; t < f->ntris; ++t)
        gf.wet += gf.terrain[t] == 8 || gf.terrain[t] == 9;
    memcpy(gf.mesh_first, f->mesh_first, ((size_t)f->nmesh + 1) * sizeof(uint32_t));
    memcpy(gf.mat, f->matrices, (size_t)f->nmatrices * 12 * sizeof(float));
    memcpy(gf.pairs, f->pairs, (size_t)f->npairs * 2 * sizeof(uint32_t));
    gf.nmesh = f->nmesh, gf.npairs = f->npairs;
    for (uint32_t m = 0; m < gf.nmesh; ++m)
    {
        float* b = &bounds[m * 6];
        b[0] = b[1] = b[2] = 1e30f, b[3] = b[4] = b[5] = -1e30f;
        for (uint32_t t = gf.mesh_first[m]; t < gf.mesh_first[m + 1]; ++t)
            for (int k = 0; k < 9; ++k)
                b[k % 3] = minf(b[k % 3], gf.tris[t * 9 + k]), b[3 + k % 3] = maxf(b[3 + k % 3], gf.tris[t * 9 + k]);
    }
    /* the placements by square: how many each square has, then each square's list */
    int32_t lo[2] = { INT32_MAX, INT32_MAX }, hi[2] = { INT32_MIN, INT32_MIN };
    for (uint32_t k = 0; k < gf.npairs; ++k)
    {
        int32_t r[4];
        if (!floor_reach(bounds, k, r))
            continue;
        lo[0] = r[0] < lo[0] ? r[0] : lo[0], lo[1] = r[1] < lo[1] ? r[1] : lo[1];
        hi[0] = r[2] > hi[0] ? r[2] : hi[0], hi[1] = r[3] > hi[1] ? r[3] : hi[1];
    }
    gf.x0 = lo[0], gf.z0 = lo[1];
    gf.nx = lo[0] <= hi[0] ? hi[0] - lo[0] + 1 : 0, gf.nz = lo[1] <= hi[1] ? hi[1] - lo[1] + 1 : 0;
    size_t total = 0;
    if (gf.nx > 0 && gf.nz > 0 && (size_t)gf.nx * (size_t)gf.nz <= (1u << 20))
        gf.bfirst = (uint32_t*)calloc((size_t)gf.nx * gf.nz + 1, sizeof(uint32_t));
    for (uint32_t k = 0; gf.bfirst && k < gf.npairs; ++k)
    {
        int32_t r[4];
        if (!floor_reach(bounds, k, r))
            continue;
        for (int32_t z = r[1]; z <= r[3]; ++z)
            for (int32_t x = r[0]; x <= r[2]; ++x)
                ++gf.bfirst[(size_t)(z - gf.z0) * gf.nx + (x - gf.x0) + 1], ++total;
    }
    if (gf.bfirst && total < (1u << 26))
        gf.bidx = (uint32_t*)malloc((total ? total : 1) * sizeof(uint32_t));
    if (!gf.bidx)
    {
        free(bounds);
        floor_free();
        gf.zone = f->zone;
        SAY("[recomp] grass: zone %d's floors left out (they reach too far): its tufts grow on all its grass ground\n", f->zone);
        return;
    }
    for (size_t b = 1; b <= (size_t)gf.nx * gf.nz; ++b)
        gf.bfirst[b] += gf.bfirst[b - 1];
    {
        uint32_t* at = (uint32_t*)malloc((size_t)gf.nx * gf.nz * sizeof(uint32_t));
        if (at)
        {
            memcpy(at, gf.bfirst, (size_t)gf.nx * gf.nz * sizeof(uint32_t));
            for (uint32_t k = 0; k < gf.npairs; ++k)
            {
                int32_t r[4];
                if (!floor_reach(bounds, k, r))
                    continue;
                for (int32_t z = r[1]; z <= r[3]; ++z)
                    for (int32_t x = r[0]; x <= r[2]; ++x)
                        gf.bidx[at[(size_t)(z - gf.z0) * gf.nx + (x - gf.x0)]++] = k;
            }
            free(at);
        }
        else
            gf.npairs = 0; /* nothing to lay them with: none */
    }
    floor_mark_wet(bounds);
    free(bounds);
    if (g.log)
        rt_log("[recomp] grass: zone %d's floors: %u placed, %u meshes, %u triangles, over %d x %d squares\n", gf.zone, gf.npairs, gf.nmesh,
            f->ntris, gf.nx, gf.nz);
    if ((g.walkable || g.nowater) && gf.zone == g.zone) /* the tufts made without them are made again */
        kept_clear();
}

/* square (tx, tz) of the world's floors laid in t (emptied first) */
static void floor_lay(FTile* t, int32_t tx, int32_t tz)
{
    free(t->tri), free(t->list);
    memset(t, 0, sizeof *t);
    t->tx = tx, t->tz = tz, t->used = g.scene + 1;
    const int32_t bx = tx - gf.x0, bz = tz - gf.z0;
    if (bx < 0 || bz < 0 || bx >= gf.nx || bz >= gf.nz)
        return;
    const size_t b = (size_t)bz * gf.nx + bx;
    uint32_t most = 0;
    for (uint32_t i = gf.bfirst[b]; i < gf.bfirst[b + 1]; ++i)
        most += gf.mesh_first[gf.pairs[2 * gf.bidx[i] + 1] + 1] - gf.mesh_first[gf.pairs[2 * gf.bidx[i] + 1]];
    if (!most || !(t->tri = (FTri*)malloc(most * sizeof(FTri))))
        return;
    const float sx = (float)tx * FLOOR_TILE, sz = (float)tz * FLOOR_TILE;
    uint32_t n = 0;
    for (uint32_t i = gf.bfirst[b]; i < gf.bfirst[b + 1]; ++i)
    {
        const uint32_t k = gf.bidx[i], mesh = gf.pairs[2 * k + 1];
        const float* m = &gf.mat[gf.pairs[2 * k] * 12];
        for (uint32_t tr = gf.mesh_first[mesh]; tr < gf.mesh_first[mesh + 1]; ++tr)
        {
            float w[3][3];
            for (int c = 0; c < 3; ++c)
                floor_place(m, &gf.tris[tr * 9 + c * 3], w[c]);
            const float ax = w[1][0] - w[0][0], ay = w[1][1] - w[0][1], az = w[1][2] - w[0][2];
            const float bx2 = w[2][0] - w[0][0], by = w[2][1] - w[0][1], bz2 = w[2][2] - w[0][2];
            const float nx = ay * bz2 - az * by, ny = az * bx2 - ax * bz2, nz = ax * by - ay * bx2;
            const float len = sqrtf(nx * nx + ny * ny + nz * nz);
            /* a wall's steepness is no floor; which way a floor faces isn't asked: a tenth of the zones' floors, the same ground
             * the player stands on, have their normals stored facing down */
            if (!(len > 1e-9f) || fabsf(ny) / len < 0.1f)
                continue;
            const float x0 = minf(w[0][0], minf(w[1][0], w[2][0])), x1 = maxf(w[0][0], maxf(w[1][0], w[2][0]));
            const float z0 = minf(w[0][2], minf(w[1][2], w[2][2])), z1 = maxf(w[0][2], maxf(w[1][2], w[2][2]));
            if (x1 < sx || x0 > sx + FLOOR_TILE || z1 < sz || z0 > sz + FLOOR_TILE)
                continue;
            const float den = ax * bz2 - bx2 * az;
            if (!(fabsf(den) > 1e-9f))
                continue;
            FTri* o = &t->tri[n++];
            o->ax = w[0][0], o->az = w[0][2], o->e1x = ax, o->e1z = az, o->e2x = bx2, o->e2z = bz2, o->inv = 1.0f / den;
            o->y0 = w[0][1], o->dy1 = ay, o->dy2 = by, o->terrain = gf.terrain[tr];
        }
    }
    /* the cells each triangle's bounds reach: how many each has, then the lists */
    const float per = FLOOR_CELLS / FLOOR_TILE;
    uint32_t total = 0, at[FLOOR_CELLS * FLOOR_CELLS] = { 0 };
    for (int pass = 0; pass < 2; ++pass)
    {
        if (pass)
        {
            for (int c = 1; c <= FLOOR_CELLS * FLOOR_CELLS; ++c)
                t->cell[c] += t->cell[c - 1];
            total = t->cell[FLOOR_CELLS * FLOOR_CELLS];
            if (!total || !(t->list = (uint32_t*)malloc(total * sizeof(uint32_t))))
            {
                memset(t->cell, 0, sizeof t->cell);
                return;
            }
            memcpy(at, t->cell, sizeof at);
        }
        for (uint32_t i = 0; i < n; ++i)
        {
            const FTri* o = &t->tri[i];
            const float x0 = minf(0.0f, minf(o->e1x, o->e2x)) + o->ax, x1 = maxf(0.0f, maxf(o->e1x, o->e2x)) + o->ax;
            const float z0 = minf(0.0f, minf(o->e1z, o->e2z)) + o->az, z1 = maxf(0.0f, maxf(o->e1z, o->e2z)) + o->az;
            const int cx0 = (int)clampf(floorf((x0 - sx) * per), 0.0f, FLOOR_CELLS - 1.0f), cx1 = (int)clampf(floorf((x1 - sx) * per), 0.0f, FLOOR_CELLS - 1.0f);
            const int cz0 = (int)clampf(floorf((z0 - sz) * per), 0.0f, FLOOR_CELLS - 1.0f), cz1 = (int)clampf(floorf((z1 - sz) * per), 0.0f, FLOOR_CELLS - 1.0f);
            for (int cz = cz0; cz <= cz1; ++cz)
                for (int cx = cx0; cx <= cx1; ++cx)
                {
                    if (pass)
                        t->list[at[cz * FLOOR_CELLS + cx]++] = i;
                    else
                        ++t->cell[cz * FLOOR_CELLS + cx + 1];
                }
        }
    }
}

/* square (tx, tz) of the world's floors, laid now if it isn't */
static const FTile* floor_tile(int32_t tx, int32_t tz)
{
    FTile* t = NULL;
    for (int i = 0; i < FLOOR_TILES; ++i)
    {
        FTile* s = &gf.tile[i];
        if (s->used && s->tx == tx && s->tz == tz)
        {
            s->used = g.scene + 1;
            return s;
        }
        if (!t || s->used < t->used)
            t = s;
    }
    floor_lay(t, tx, tz);
    return t;
}

int grass_floor_tops(int tx, int tz, float* top)
{
    if (!gf.zone || gf.zone != g.zone)
        return 0;
    FTile t;
    memset(&t, 0, sizeof t);
    floor_lay(&t, tx, tz);
    const float cell = FLOOR_TILE / FLOOR_CELLS;
    for (int c = 0; c < FLOOR_CELLS * FLOOR_CELLS; ++c)
    {
        const float px = (float)tx * FLOOR_TILE + ((float)(c % FLOOR_CELLS) + 0.5f) * cell;
        const float pz = (float)tz * FLOOR_TILE + ((float)(c / FLOOR_CELLS) + 0.5f) * cell;
        float hi = 1e30f, next = 1e30f; /* the top floor (the least y) and the next under it */
        for (uint32_t k = t.list ? t.cell[c] : 0; t.list && k < t.cell[c + 1]; ++k)
        {
            const FTri* o = &t.tri[t.list[k]];
            const float qx = px - o->ax, qz = pz - o->az;
            const float u = (qx * o->e2z - o->e2x * qz) * o->inv, w = (o->e1x * qz - qx * o->e1z) * o->inv;
            if (u < -1e-4f || w < -1e-4f || u + w > 1.0001f)
                continue;
            const float y = o->y0 + u * o->dy1 + w * o->dy2;
            if (y < hi)
                next = hi, hi = y;
            else if (y < next && y > hi + 0.05f)
                next = y;
        }
        top[c] = next < 1e29f && next - hi > 35.0f ? next : hi;
    }
    free(t.tri), free(t.list);
    return 1;
}

/* the terrain of the floor p (the world, y down) stands on, within FLOOR_NEAR above or below it, or -1 for none; *below: a floor
 * further below it than that, to FLOOR_DEEP; *over: how far up the lowest floor over it is (below; 1e30: none). The zone's floors
 * must be known (gf.zone == g.zone) */
static int floor_terrain(const float p[3], int* below, float* over)
{
    *below = 0, *over = 1e30f;
    const int32_t tx = (int32_t)floorf(p[0] / FLOOR_TILE), tz = (int32_t)floorf(p[2] / FLOOR_TILE);
    const FTile* t = floor_tile(tx, tz);
    if (!t->list)
        return -1; /* no floor anywhere here */
    const float per = FLOOR_CELLS / FLOOR_TILE;
    const int cx = (int)clampf(floorf((p[0] - (float)tx * FLOOR_TILE) * per), 0.0f, FLOOR_CELLS - 1.0f);
    const int cz = (int)clampf(floorf((p[2] - (float)tz * FLOOR_TILE) * per), 0.0f, FLOOR_CELLS - 1.0f);
    const int c = cz * FLOOR_CELLS + cx;
    float best = 1e30f, hard = 1e30f, soft = 1e30f;
    int terrain = -1, grounded = 0;
    for (uint32_t k = t->cell[c]; k < t->cell[c + 1]; ++k)
    {
        const FTri* o = &t->tri[t->list[k]];
        const float qx = p[0] - o->ax, qz = p[2] - o->az;
        const float u = (qx * o->e2z - o->e2x * qz) * o->inv, w = (o->e1x * qz - qx * o->e1z) * o->inv;
        if (u < -1e-4f || w < -1e-4f || u + w > 1.0001f)
            continue;
        const float d = o->y0 + u * o->dy1 + w * o->dy2 - p[1]; /* above 0: the floor is below the place (y is down) */
        if (fabsf(d) < best)
            best = fabsf(d), terrain = o->terrain, g_floor_at = d;
        *below |= d > FLOOR_NEAR && d < FLOOR_DEEP;
        grounded |= fabsf(d) <= FLOOR_HARD;
        if (d < -FLOOR_HARD && (o->terrain == 0 || (o->terrain >= 5 && o->terrain <= 7)))
            hard = minf(hard, -d);
        else if (d < -FLOOR_HARD)
            soft = minf(soft, -d);
    }
    /* over it: a hard floor (an object's, stone, metal, wood) more than FLOOR_HARD up, a building's laid over the ground drawn on under
     * it (Windurst Waters' rooms: the ground's own collision under it there, or none); a soft one more than FLOOR_OVER up, or more than
     * FLOOR_HARD up over the ground's own floor (two floors, one over the other). A soft floor alone a little over its drawn ground is
     * that ground's collision lying over it in places, and grows */
    *over = minf(hard, (soft > FLOOR_OVER || grounded) ? soft : 1e30f);
    return best <= FLOOR_NEAR ? terrain : -1;
}

/* grass.h: the terrain of the nearest of the zone's floors to p (the world, y down) within reach above or below it, and into *dy
 * how far below p it is (under 0: above it); -1 with none within reach, or the zone's floors not known. For the water
 * (d3d8_water.h water_sheet): the footsteps' deep water is a floor at the surface, their shallow water a floor on the bed */
int grass_floor_near(const float p[3], float reach, float* dy)
{
    *dy = 0.0f;
    if (!gf.zone || gf.zone != g.zone)
        return -1;
    const int32_t tx = (int32_t)floorf(p[0] / FLOOR_TILE), tz = (int32_t)floorf(p[2] / FLOOR_TILE);
    const FTile* t = floor_tile(tx, tz);
    if (!t->list)
        return -1;
    const float per = FLOOR_CELLS / FLOOR_TILE;
    const int cx = (int)clampf(floorf((p[0] - (float)tx * FLOOR_TILE) * per), 0.0f, FLOOR_CELLS - 1.0f);
    const int cz = (int)clampf(floorf((p[2] - (float)tz * FLOOR_TILE) * per), 0.0f, FLOOR_CELLS - 1.0f);
    const int c = cz * FLOOR_CELLS + cx;
    float best = reach;
    int terrain = -1;
    for (uint32_t k = t->cell[c]; k < t->cell[c + 1]; ++k)
    {
        const FTri* o = &t->tri[t->list[k]];
        const float qx = p[0] - o->ax, qz = p[2] - o->az;
        const float u = (qx * o->e2z - o->e2x * qz) * o->inv, w = (o->e1x * qz - qx * o->e1z) * o->inv;
        if (u < -1e-4f || w < -1e-4f || u + w > 1.0001f)
            continue;
        const float d = o->y0 + u * o->dy1 + w * o->dy2 - p[1];
        if (fabsf(d) <= best)
            best = fabsf(d), terrain = o->terrain, *dy = d;
    }
    return terrain;
}

/* grass.h */
int grass_floor_water(unsigned* set, const float lo[3], const float hi[3])
{
    *set = gf.set;
    if (!gf.zone || gf.zone != g.zone)
        return -1;
    if (!gf.wet)
        return 0;
    if (!gf.bwet)
        return 1;
    const int32_t x0 = (int32_t)floorf(clampf(lo[0], -1e5f, 1e5f) / FLOOR_TILE) - gf.x0, z0 = (int32_t)floorf(clampf(lo[2], -1e5f, 1e5f) / FLOOR_TILE) - gf.z0;
    const int32_t x1 = (int32_t)floorf(clampf(hi[0], -1e5f, 1e5f) / FLOOR_TILE) - gf.x0, z1 = (int32_t)floorf(clampf(hi[2], -1e5f, 1e5f) / FLOOR_TILE) - gf.z0;
    for (int32_t z = z0 < 0 ? 0 : z0; z <= z1 && z < gf.nz; ++z)
        for (int32_t x = x0 < 0 ? 0 : x0; x <= x1 && x < gf.nx; ++x)
            if (gf.bwet[(size_t)z * gf.nx + x])
                return 1;
    return 0;
}

/* With grass.cover.show (//vana's Debug) the tufts of the place floor_grows looked at last are drawn this colour (kvert; 0 their
 * own): red, covered by a floor over it (grown only to be shown); yellow, no floor at or under it to tell, or the floors not known.
 * Twice these, drawn MODULATE2X */
#define COVER_RED 0xFF730A08u
#define COVER_YELLOW 0xFF736608u
static uint32_t g_cover_tint;

/* g.grew's count a place growing on terrain (floor_terrain's: -1 none) goes in, by its floor's height from the ground (g_floor_at) */
static int grew_by(int terrain)
{
    if (terrain < 0)
        return 5;
    const float at = g_floor_at;
    return at < -FLOOR_OVER ? 0 : at < -FLOOR_HARD ? 1 : at <= FLOOR_HARD ? 2 : at <= FLOOR_OVER ? 3 : 4;
}

/* whether a tuft may stand at p (the world, y down) by the zone's floors: 1 it may (or they aren't known), 0 it may not (covered,
 * or grass.walkable's ground it can't), GROW_MOSS moss in its place (the shallow water grass.nowater leaves bare, with grass.moss) */
#define GROW_MOSS 2
/* Dry grass's yellow-olive is gold's, brass's and sandstone's too (the Nyzul portal's disc in Alzadaal is dry grass by its colours
 * alone, on a floor the game's footsteps call stone): with soil (dry grass ground, not listed, no grass.overgrown), a spot whose
 * floor is known and isn't soft ground (path, grass, sand or snow; shallow water without grass.nowater or with moss) grows nothing.
 * Spot by spot, not a square at a time: a square's vote left whole squares bare among grown ones */
static int soil_ok(int terrain)
{
    return terrain < 0 || (terrain >= 1 && terrain <= 4) || (terrain == GRASS_WET && (!g.nowater || g.moss));
}

/* grass.mega: p's floor (the zone's collision) is grass (the game's footsteps' terrain 2); 0 where the floors aren't known */
static int grass_floor(const float p[3])
{
    int below;
    float over;
    return g.mega && gf.zone == g.zone && gf.npairs && floor_terrain(p, &below, &over) == 2;
}

static int floor_grows(const float p[3], int soil)
{
    g_cover_tint = g.show_cover ? COVER_YELLOW : 0;
    if (gf.zone != g.zone || !gf.npairs)
        return 1;
    int below;
    float over;
    const int terrain = floor_terrain(p, &below, &over);
    if (over <= g.height * (1.0f + g.variation) * 1.3f + FLOOR_OVER) /* the tallest tuft (an asset's sprite 1.3 of it), and a gap */
    {
        ++g.covered, g_cover_tint = g.show_cover ? COVER_RED : 0;
        return g.show_cover;
    }
    if (g_cover_tint && (terrain >= 0 || below))
        g_cover_tint = 0;
    const int wet = terrain == GRASS_WET;
    if (soil && !soil_ok(terrain))
        return 0;
    const int grows = !(wet && g.nowater) && (!g.walkable || (terrain >= 0 ? (terrain >= 1 && terrain <= 5) || wet : !below));
    if (grows && g.log)
        ++g.grew[grew_by(terrain)];
    return wet && g.nowater && g.moss ? GROW_MOSS : grows;
}


/* the squares of tufts may be made: not while a new zone's floors are on their way (FLOOR_WAIT at most), when they're asked for */
static int floor_ready(void)
{
    return !(g.walkable || g.nowater) || gf.zone == g.zone || g.scene - gf.wait_from > FLOOR_WAIT;
}

#include "grass_centre.h" /* grass_set_centre, centre_ground: the grass round the free camera (grass.freecam) */

/* --- the textures and the tufts -------------------------------------------------------------------------- */
static uint32_t g_level[7][TUFT_TEX * TUFT_TEX];

/* g_level[0] painted: its mips made by box filter, and the texture */
static GfxTex* upload_mipped(void)
{
    int levels = 1;
    for (int s = TUFT_TEX; s > 1; s >>= 1, ++levels)
    {
        const int ds = s / 2;
        for (int y = 0; y < ds; ++y)
            for (int x = 0; x < ds; ++x)
            {
                uint32_t sum[4] = { 0, 0, 0, 0 };
                for (int k = 0; k < 4; ++k)
                {
                    const uint32_t c = g_level[levels - 1][(2 * y + k / 2) * s + 2 * x + k % 2];
                    sum[0] += c >> 24, sum[1] += (c >> 16) & 255, sum[2] += (c >> 8) & 255, sum[3] += c & 255;
                }
                g_level[levels][y * ds + x] = (sum[0] / 4 << 24) | (sum[1] / 4 << 16) | (sum[2] / 4 << 8) | sum[3] / 4;
            }
    }
    GfxTex* t = gfx_tex_create(GFX_TEX_2D, FMT_A8R8G8B8, TUFT_TEX, TUFT_TEX, (uint32_t)levels, GFX_USE_SAMPLE);
    if (!t)
        return NULL;
    for (int l = 0; l < levels; ++l)
        gfx_tex_upload(t, 0, (uint32_t)l, g_level[l], (uint32_t)((TUFT_TEX >> l) > 0 ? (TUFT_TEX >> l) : 1) * 4u);
    return t;
}

#include "grass_tufts.h" /* blade_quad_shade, make_tuft1_texture, make_tuft_texture: the drawn blades' pictures */

/* A flower: its bloom in the top 44% of the picture (six petals round a centre), a stem and a leaf below. The picture is drawn as
 * two quads, the bloom tinted its colour and the stem green (keep_flower), so the petals are white and the stem grey. */
typedef struct Shape
{
    int kind; /* 0 triangle (x0 y0 x1 y1 x2 y2), 1 ellipse (cx cy rx ry angle), 2 rectangle (x0 y0 x1 y1) */
    float a[6];
    uint32_t rgb;
} Shape;

static int shape_has(const Shape* s, float X, float Y)
{
    if (s->kind == 2)
        return X >= s->a[0] && X <= s->a[2] && Y >= s->a[1] && Y <= s->a[3];
    if (s->kind == 1)
    {
        const float dx = X - s->a[0], dy = Y - s->a[1], ca = cosf(s->a[4]), sa = sinf(s->a[4]);
        const float u = (dx * ca + dy * sa) / s->a[2], v = (-dx * sa + dy * ca) / s->a[3];
        return u * u + v * v <= 1.0f;
    }
    const float d0 = (s->a[2] - s->a[0]) * (Y - s->a[1]) - (s->a[3] - s->a[1]) * (X - s->a[0]);
    const float d1 = (s->a[4] - s->a[2]) * (Y - s->a[3]) - (s->a[5] - s->a[3]) * (X - s->a[2]);
    const float d2 = (s->a[0] - s->a[4]) * (Y - s->a[5]) - (s->a[1] - s->a[5]) * (X - s->a[4]);
    return (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
}

static void make_flower_texture(void)
{
    Shape sh[12];
    int n = 0;
    sh[n++] = (Shape){ 2, { 30.4f, 26.0f, 33.6f, 64.0f }, 0xFFB4B4B4u };             /* the stem */
    sh[n++] = (Shape){ 0, { 33.0f, 55.0f, 47.0f, 44.0f, 33.0f, 48.0f }, 0xFFA0A0A0u }; /* a leaf */
    for (int k = 0; k < 6; ++k)
    {
        const float a = (float)k * 1.0471975f;
        sh[n++] = (Shape){ 1, { 32.0f + cosf(a) * 7.0f, 14.0f + sinf(a) * 7.0f, 5.6f, 3.6f, a }, 0xFFFFFFFFu }; /* a petal */
    }
    sh[n++] = (Shape){ 1, { 32.0f, 14.0f, 4.4f, 4.4f, 0.0f }, 0xFFFFE08Au }; /* the centre */
    uint32_t* px = g_level[0];
    for (int i = 0; i < TUFT_TEX * TUFT_TEX; ++i)
        px[i] = 0x00D2D2D2u;
    for (int i = 0; i < n; ++i) /* each over what's under it, in order */
        for (int y = 0; y < TUFT_TEX; ++y)
            for (int x = 0; x < TUFT_TEX; ++x)
            {
                int in = 0;
                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx)
                        in += shape_has(&sh[i], (float)x + ((float)sx + 0.5f) * 0.25f, (float)y + ((float)sy + 0.5f) * 0.25f);
                if (!in)
                    continue;
                const uint32_t old = px[y * TUFT_TEX + x], cov = (uint32_t)(in * 255 / 16), oa = old >> 24;
                const uint32_t a = oa + cov > 255 ? 255 : oa + cov;
                const uint32_t w = cov * 255 / (a ? a : 1); /* the new colour's share of what the pixel is now */
                uint32_t out = a << 24;
                for (int sft = 0; sft <= 16; sft += 8)
                    out |= (((old >> sft & 255) * (255 - w) + (sh[i].rgb >> sft & 255) * w) / 255) << sft;
                px[y * TUFT_TEX + x] = out;
            }
    g.flower = upload_mipped();
}

static uint32_t pack_color(float r, float gr, float b)
{
    return 0xFF000000u | ((uint32_t)(clampf(r, 0.0f, 1.0f) * 255.0f + 0.5f) << 16) | ((uint32_t)(clampf(gr, 0.0f, 1.0f) * 255.0f + 0.5f) << 8) |
        (uint32_t)(clampf(b, 0.0f, 1.0f) * 255.0f + 0.5f);
}

/* smooth value noise on a unit lattice, 0 to 1: where the flowers grow in patches */
static float vnoise(float x, float z)
{
    const float fx = floorf(x), fz = floorf(z), tx = x - fx, tz = z - fz;
    const float sx = tx * tx * (3.0f - 2.0f * tx), sz = tz * tz * (3.0f - 2.0f * tz);
    const uint32_t ix = (uint32_t)(int)fx, iz = (uint32_t)(int)fz;
    const float a = rnd01(ix * 73856093u ^ iz * 19349663u), b = rnd01((ix + 1u) * 73856093u ^ iz * 19349663u);
    const float c = rnd01(ix * 73856093u ^ (iz + 1u) * 19349663u), d = rnd01((ix + 1u) * 73856093u ^ (iz + 1u) * 19349663u);
    const float top = a + (b - a) * sx, bottom = c + (d - c) * sx;
    return top + (bottom - top) * sz;
}

/* --- the kept tufts ---------------------------------------------------------------------------------------------------------
 * A piece's tufts are made once, a square of the world (g.cell a side) at a time, into buffers on the GPU, and drawn from them
 * every frame after: nothing about a tuft is made or sent again while its square stays in reach (making and sending every tuft
 * every frame took 8 ms a frame at a radius of 60). What changes with the frame is the tufts' vertex function's (g_tuft_vs): a
 * tuft grown whole within 0.8 of the radius and to nothing at it, thinned past 0.65 of it, left out away from the player's height,
 * and swaying with the time. A square is made from the frame it's in reach, the nearest first, within a frame's time for it
 * (grass.build: the rest of a square waits for the next frame, the square's older layers drawn meanwhile: walking into a field or
 * zoning in doesn't stall a frame), a layer for each ring it's brought up to (the tufts the nearer ring adds, never the square
 * again), and let go when its piece goes or it hasn't been drawn for KEPT_IDLE scenes. */
#define KEPTMAX 4096
#define KLAYERS 4               /* a square's layers at most: one a ring level (ring_level's four), each adding to the last's */
#define KEPT_IDLE 18000         /* scenes a square not drawn is kept (five minutes at 60 FPS): the tufts round where the player has been
                                 * stay for the way back; past the budget below they go, the ones drawn longest ago first */
#define KEPT_FLOOR (64u << 20) /* the GPU memory the squares may hold: 320 bytes a tuft of the limit, not less than this; past */
#define KEPT_PER_TUFT 320u      /* it, the squares drawn longest ago are let go (a tuft is 192 bytes, a flower 384; the squares
                                 * hold more than the limit shows: their rings' shares rounded up, the edge's thinning) */
#define QUADS_DRAW 16384        /* quads a draw at most: their corners fit the 16-bit quad indices */
#define KINDS (ASSETMAX + 2)   /* what a square draws, each its own texture: i asset i (0 and 1 the drawn blades), FLOWERS the flowers, */
#define FLOWERS ASSETMAX        /* MOSS the moss (grass_moss.h) */
#define MOSS (ASSETMAX + 1)

typedef struct KVert /* a kept tuft's corner, as a square is made */
{
    float x, y, z;       /* at rest: the tuft whole */
    uint32_t col;
    float u, v;
    float rx, ry, rz;    /* the tuft's root, which it grows and shrinks about */
    uint32_t keep;       /* D3DCOLOR: red its keep draw (it's left out past the radius's share it's under), green its height band / 32 */
} KVert;

/* ... and as it's kept on the GPU, 24 bytes: a, the root from the square's middle (GChunk.org, in org[3] steps) and u; b, the corner
 * from the root (in GChunk.step steps) and v; u and v in 1/32767 steps. A corner sways by its height over the root times its kind's
 * share (kept_sway): nothing at the bottom. Four corners a quad, drawn through the shared quad indices (quad_indices). */
typedef struct PVert
{
    int16_t a[4], b[4];
    uint32_t col, keep;
} PVert;

typedef struct GLayer /* a square's tufts of one range of keep draws (the ring level that added them), on the GPU */
{
    GfxBuf* buf;
    float org[4], step;   /* its corners' (PVert): the roots' middle and step, the step of the corners from their roots */
    uint32_t bytes;
    uint32_t first[KINDS], n[KINDS]; /* each kind's vertices in buf */
} GLayer;

typedef struct GChunk /* a square of a piece's tufts */
{
    uint32_t inst, stamp; /* the piece (its index and stamp; stamp 0: the slot is free) */
    int32_t cx, cz;       /* the square: x from cx * cell, z from cz * cell */
    uint32_t next;        /* the piece's next square: slot + 1 */
    uint32_t drawn;       /* the scene it was last drawn in */
    float share;          /* the share of its tufts it's made to (its ring's, ring_level, when its last layer was begun; 0 none yet) */
    GLayer lay[KLAYERS];  /* its layers, in the order made (nlay of them; none: nothing grows in it, or it's still being made) */
    uint32_t nlay;
    uint32_t tufts, flowers, bytes;
} GChunk;

static struct
{
    GChunk slot[KEPTMAX];
    uint32_t hi;              /* the slots used: below this */
    uint32_t free[KEPTMAX], nfree;
    size_t bytes;             /* on the GPU */
    KVert* v[KINDS];          /* a layer being made: each kind's vertices */
    uint32_t cap[KINDS], n[KINDS];
    PVert* all;               /* ... one after another, packed, to send */
    uint32_t capall;
} g_kept;

static int chunk_has(const GChunk* c, int kind)
{
    for (uint32_t l = 0; l < c->nlay; ++l)
        if (c->lay[l].n[kind] && c->lay[l].buf)
            return 1;
    return 0;
}

#include "grass_vs.h" /* g_tuft_vs: the tufts' vertex function */

/* room for `verts` more of a kind's vertices in the square being made, or NULL */
static KVert* kept_room(int kind, uint32_t verts)
{
    if (!pool_room((void**)&g_kept.v[kind], &g_kept.cap[kind], g_kept.n[kind] + verts, sizeof(KVert)))
        return NULL;
    KVert* v = g_kept.v[kind] + g_kept.n[kind];
    g_kept.n[kind] += verts;
    return v;
}

#include "grass_keep.h" /* kvert, lean, set_lean, tuft_colour, keep_blades, keep_sprite, keep_flower: the tufts' quads */
#include "grass_moss.h" /* make_moss_texture, moss_spot: grass.moss's cushions */

static void sum_weights(void);

static int16_t q16(float x)
{
    const float c = clampf(x, -32767.0f, 32767.0f);
    return (int16_t)(int)(c + (c >= 0.0f ? 0.5f : -0.5f));
}

/* --- making a square -------------------------------------------------------------------------------------------------------
 * Square c of piece `in` is made from the piece's triangles in it, each's tufts as spawn made them, a layer at a time: the tufts
 * whose keep draws are over the share it was made to and within its ring's now (build_begin), so a nearer ring adds a layer and
 * never makes the square again. The work is spread over frames: build_step runs the triangles until the frame's deadline, the
 * square's older layers drawn meanwhile, and the layer is sent to the GPU with its last triangle (build_finish). One square is in
 * hand at a time (g_build). */
static struct
{
    int active;
    uint32_t slot, inst, stamp; /* the square (its slot, and what it must still hold: build_chunk) */
    int32_t cx, cz;
    float lo, hi;               /* the layer: keep draws over lo's share (the square's before) to hi's (its ring's now) */
    uint32_t k;                 /* the piece's next triangle */
    uint32_t tufts, flowers;    /* the layer's so far */
    float density, lift[DRAWN], moss_lift;
    int dry;
    int soil; /* dry grass ground not listed: its spots on stone or metal grow nothing (floor_grows, soil_ok) */
} g_build;

/* the square being made, or NULL when it's gone (let go, or its slot another's) */
static GChunk* build_chunk(void)
{
    GChunk* c = &g_kept.slot[g_build.slot];
    if (!g_build.active || !c->stamp || c->inst != g_build.inst || c->stamp != g_build.stamp || c->cx != g_build.cx ||
        c->cz != g_build.cz || c->inst >= g.ninst || g.inst[c->inst].stamp != c->stamp)
        return NULL;
    return c;
}

/* square c's layers let go: nothing grows in it until it's made again */
static void layers_free(GChunk* c)
{
    for (uint32_t l = 0; l < c->nlay; ++l)
        if (c->lay[l].buf)
            gfx_buf_destroy(c->lay[l].buf);
    g_kept.bytes -= c->bytes;
    memset(c->lay, 0, sizeof c->lay);
    c->nlay = 0, c->bytes = 0, c->share = 0.0f, c->tufts = c->flowers = 0;
}

/* square c of piece `in` begun: the layer that brings it up to `share` */
static void build_begin(GChunk* c, const GInst* in, float share)
{
    if (c->nlay == KLAYERS) /* no room for a layer (the rings changed under it): made again from nothing */
        layers_free(c);
    g_build.active = 1;
    g_build.slot = (uint32_t)(c - g_kept.slot), g_build.inst = c->inst, g_build.stamp = c->stamp, g_build.cx = c->cx, g_build.cz = c->cz;
    g_build.lo = c->share, g_build.hi = share;
    g_build.k = 0, g_build.tufts = g_build.flowers = 0;
    g_build.density = kept_density();
    if (!g.weights_set)
        sum_weights();
    /* with grass.match, the drawn blades lifted so their picture's mean shade (its quads' shading in) is the ground's colour: their
     * pictures made now if they aren't yet, for it */
    for (int a = 0; a < DRAWN; ++a)
    {
        if (g.match > 0.0f && g.asset[a].weight > 0.0f && !g.tuft[a])
            a ? make_tuft_texture() : make_tuft1_texture();
        g_build.lift[a] = g.match > 0.0f ? powf(1.0f / (g.blade_avg[a] > 0.05f ? g.blade_avg[a] : 0.55f), g.match) : 1.0f;
    }
    if (g.match > 0.0f && g.moss && !g.moss_tex) /* the moss's too (grass.moss) */
        make_moss_texture();
    g_build.moss_lift = g.match > 0.0f ? powf(1.0f / (g.moss_avg > 0.05f ? g.moss_avg : 0.6f), g.match) : 1.0f;
    g_build.dry = ground_dry(&g.tex[g.mesh[in->mesh].tex]);
    /* a listed texture (or the zone's own ground) is grass whatever its floors are; the others' dry grass only on soft ground, spot
     * by spot */
    g_build.soil = g_build.dry && !g.overgrown && !g.nlisted && !zone_listed(g.tex[g.mesh[in->mesh].tex].hash);
    memset(g_kept.n, 0, sizeof g_kept.n);
}

/* the layer made (its kinds one after another in one buffer, packed: the roots from their middle, the corners from their roots,
 * each in the steps that span them) and sent to the GPU; the square brought up to the layer's share */
static void build_finish(GChunk* c)
{
    uint32_t total = 0, first[KINDS];
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }, reach = 0.0f;
    for (int k = 0; k < KINDS; ++k)
    {
        first[k] = total, total += g_kept.n[k];
        for (uint32_t i = 0; i < g_kept.n[k]; ++i)
        {
            const KVert* kv = &g_kept.v[k][i];
            const float r[3] = { kv->rx, kv->ry, kv->rz }, o[3] = { kv->x - kv->rx, kv->y - kv->ry, kv->z - kv->rz };
            for (int j = 0; j < 3; ++j)
                lo[j] = minf(lo[j], r[j]), hi[j] = maxf(hi[j], r[j]), reach = maxf(reach, fabsf(o[j]));
        }
    }
    c->share = g_build.hi;
    c->tufts += g_build.tufts, c->flowers += g_build.flowers;
    g_build.active = 0;
    if (!total || !pool_room((void**)&g_kept.all, &g_kept.capall, total, sizeof(PVert)))
        return;
    GLayer* L = &c->lay[c->nlay];
    memcpy(L->first, first, sizeof L->first), memcpy(L->n, g_kept.n, sizeof L->n);
    float span = 0.0f;
    for (int j = 0; j < 3; ++j)
        L->org[j] = 0.5f * (lo[j] + hi[j]), span = maxf(span, 0.5f * (hi[j] - lo[j]));
    L->org[3] = maxf(span, 1e-3f) / 32000.0f, L->step = maxf(reach, 1e-3f) / 32000.0f;
    const float ir = 1.0f / L->org[3], io = 1.0f / L->step;
    for (int k = 0; k < KINDS; ++k)
        for (uint32_t i = 0; i < g_kept.n[k]; ++i)
        {
            const KVert* kv = &g_kept.v[k][i];
            PVert* pv = &g_kept.all[L->first[k] + i];
            pv->a[0] = q16((kv->rx - L->org[0]) * ir), pv->a[1] = q16((kv->ry - L->org[1]) * ir), pv->a[2] = q16((kv->rz - L->org[2]) * ir);
            pv->a[3] = q16(clampf(kv->u, 0.0f, 1.0f) * 32767.0f);
            pv->b[0] = q16((kv->x - kv->rx) * io), pv->b[1] = q16((kv->y - kv->ry) * io), pv->b[2] = q16((kv->z - kv->rz) * io);
            pv->b[3] = q16(clampf(kv->v, 0.0f, 1.0f) * 32767.0f);
            pv->col = kv->col, pv->keep = kv->keep;
        }
    L->bytes = total * (uint32_t)sizeof(PVert);
    L->buf = gfx_buf_create(L->bytes);
    if (!L->buf)
    {
        memset(L, 0, sizeof *L);
        return;
    }
    gfx_buf_upload(L->buf, g_kept.all, L->bytes);
    g_kept.bytes += L->bytes, c->bytes += L->bytes;
    ++c->nlay;
}

#include "grass_edges.h" /* edge_grow: the tufts shorter near where the grass ends (grass.edge.fade) */

/* a tuft's spot at uv (pos in the world): its colour in texel, the ground's round it, and 1; 0 where it isn't grass ground (in green
 * grass, yellow-olive is a path's dirt). grass.mega: on a grass floor (*mega) every spot is, a bare one tinted with its texture's grass
 * colour (tex_sort's lit) */
static int spot_colour(const GTex* tx, const float uv[2], const float pos[3], int dry, uint8_t texel[3], int* mega)
{
    *mega = grass_floor(pos);
    if (!tex_color(tx, uv[0], uv[1], texel) || ground_ok(texel, dry))
    {
        tuft_colour(tx, uv, texel, dry);
        return 1;
    }
    for (int i = 0; i < 3 && *mega; ++i)
        texel[i] = (uint8_t)clampf(tx->lit[i], 0.0f, 255.0f);
    return *mega;
}

/* the square in hand made on, triangle by triangle, until `deadline` (gfx_now_ns) or the layer is done: 1 done with it (made,
 * or the square is gone), 0 the rest of it waits for the next frame */
static int build_step(uint64_t deadline)
{
    static const float palette[5][3] = { { 1.0f, 1.0f, 0.95f }, { 1.0f, 0.82f, 0.15f }, { 1.0f, 0.45f, 0.65f }, { 0.72f, 0.55f, 1.0f },
        { 0.45f, 0.65f, 1.0f } };
    GChunk* c = build_chunk();
    if (!c)
    {
        g_build.active = 0;
        return 1;
    }
    const GInst* in = &g.inst[c->inst];
    const GTex* tx = &g.tex[g.mesh[in->mesh].tex];
    const float layer = (float)g.mesh[in->mesh].layer; /* a blended layer (grass.blended): its tufts by its vertices' alpha */
    const float density = g_build.density, lo = g_build.lo * 255.0f, hi = g_build.hi * 255.0f;
    const float* lift = g_build.lift;
    const int dry = g_build.dry;
    uint32_t tufts = g_build.tufts, flowers = g_build.flowers, k = g_build.k;
    for (; k < in->n; ++k)
    {
        const GWTri* tri = &g.wtris[in->first + k];
        if ((int32_t)floorf(tri->ctr[0] / g.cell) != c->cx || (int32_t)floorf(tri->ctr[2] / g.cell) != c->cz)
            continue;
        /* the ground steepening toward the limit: fewer, shorter tufts, none at the limit (cliffs, rock faces); each spot's from its
         * corners' (smooth_corners), so the most any spot of it has */
        float cu[3], top = tri->up;
        for (int v = 0; v < 3; ++v)
            cu[v] = (float)tri->vup[v] * (1.0f / 254.0f), top = maxf(top, cu[v]);
        const float most = clampf(1.3f * (top - g.slope_cos) / 0.12f, 0.0f, 1.0f);
        if (most <= 0.0f)
            continue;
        set_lean(tri); /* its slope, for its tufts' quads to follow */
        /* the triangle's tufts come from where it is in the world, so they stand in the same places when its piece is made again; a
         * blended layer's triangles lie on the ground's own, so theirs are seeded apart (the same spots, the same quads, z-fought:
         * Windurst Woods' grass layers flickered) */
        const uint32_t id = hash32((uint32_t)(int)floorf(tri->ctr[0] * 8.0f) * 73856093u ^ (uint32_t)(int)floorf(tri->ctr[2] * 8.0f) * 19349663u ^
            (uint32_t)(int)floorf(tri->ctr[1] * 8.0f) * 83492791u ^ (g.mesh[in->mesh].blended ? 0x5BD1E995u : 0u));
        const float expect = tri->area * density * most;
        int n = (int)expect;
        if (rnd01(id * 3u + 1u) < expect - (float)n)
            ++n;
        if (n > 400)
            n = 400, ++g.capped;
        /* how far above or below the player its tufts still grow (with 0.3 of their distance, the vertex function's): the ground near
         * the player's height, not roofs, not canopies */
        const uint32_t band = (uint32_t)clampf((3.5f + 0.5f * tri->rad) * (255.0f / 32.0f) + 0.5f, 0.0f, 255.0f);
        /* the brink of a drop on its edges or corners (find_edges): the tufts near it no wider than their way to it */
        int edgy = 0;
        for (int k = 0; k < 3 && g.edges; ++k)
            edgy |= (float)tri->enup[k] < g.slope_cos * 254.0f || (float)tri->vnup[k] < g.slope_cos * 254.0f;
        for (int j = 0; j < n; ++j)
        {
            const uint32_t sd = hash32(id ^ ((uint32_t)j * 0x9E3779B9u));
            /* its keep draw: it's left out where the share kept is below it, so past the square's share it's never shown from here */
            const uint32_t kd = 1u + (uint32_t)(rnd01(sd + 2u) * 254.0f + 0.5f); /* 1 to 255: a ring keeping 0 shows none, 1 all */
            if ((float)kd > hi || (float)kd <= lo)
                continue;
            const float r1 = rnd01(sd), r2 = rnd01(sd + 1u), a = sqrtf(r1);
            const float b[3] = { 1.0f - a, a * (1.0f - r2), a * r2 };
            float pos[3], uv[2] = { 0, 0 }, vc[3] = { 0, 0, 0 };
            for (int i = 0; i < 3; ++i)
                pos[i] = b[0] * tri->p[0][i] + b[1] * tri->p[1][i] + b[2] * tri->p[2][i];
            /* its spot's steepness: kept as its share of the triangle's most, eased in, the fade's start ragged (value noise a
             * couple of units across) so it follows no line */
            float flat = 1.3f * (b[0] * cu[0] + b[1] * cu[1] + b[2] * cu[2] - g.slope_cos) / 0.12f - 0.3f * vnoise(pos[0] * 0.4f, pos[2] * 0.4f);
            flat = clampf(flat, 0.0f, 1.0f);
            flat = flat * flat * (3.0f - 2.0f * flat);
            if (rnd01(sd + 16u) * most >= flat || !layer_keeps(tri, b, layer, sd))
                continue;
            for (int i = 0; i < 2; ++i)
                uv[i] = b[0] * tri->uv[0][i] + b[1] * tri->uv[1][i] + b[2] * tri->uv[2][i];
            for (int v = 0; v < 3; ++v)
            {
                vc[0] += b[v] * (float)((tri->c[v] >> 16) & 255), vc[1] += b[v] * (float)((tri->c[v] >> 8) & 255),
                    vc[2] += b[v] * (float)(tri->c[v] & 255);
            }
            uint8_t texel[3] = { 90, 120, 60 };
            int mega;
            if (!spot_colour(tx, uv, pos, dry, texel, &mega))
                continue; /* this part of the ground isn't grass */
            if (g.pathpad > 0.0f && !mega && path_near(tri, tx, pos, dry))
                continue; /* a path's ground within grass.pathpad of it: the path's edge kept bare */
            const int grows = floor_grows(pos, g_build.soil);
            if (!grows)
                continue; /* no ground the player could stand on (grass.walkable): a cart's top, a crate's, a tree's boughs */
            const uint32_t keep = (uint32_t)tri->lnz << 24 | kd << 16 | band << 8 | tri->lnx; /* (the slope's normal: grass_slope.h) */
            const float tone = 0.88f + 0.24f * rnd01(sd + 3u);
            float vf[3], col[3];
            for (int i = 0; i < 3; ++i)
                vf[i] = clampf(vc[i] / 255.0f * 2.0f, 0.4f, 1.3f), col[i] = (float)texel[i] / 255.0f * vf[i] * tone * g.brightness * 1.15f;
            /* how far it may reach out across the ground before the brink: a tuft wider than twice that is made smaller to fit, none
             * under a third of its size */
            const float room = edgy ? brink(tri, pos) : 1e30f;
            const float grow = edge_grow(tri, tx, b, pos, flat, room); /* the slope's and the edge's share; the radius's is the vertex function's */
            if (grows == GROW_MOSS) /* shallow water with grass.moss: the spot's cushions of moss, and nothing else */
            {
                tufts += moss_spot(pos, col, g_build.moss_lift, keep, sd, grow);
                continue;
            }
            const float h = maxf(0.04f, g.height * (1.0f + (2.0f * rnd01(sd + 4u) - 1.0f) * g.variation)) * grow;
            const float light = (vf[0] + vf[1] + vf[2]) * (1.0f / 3.0f);
            /* the place's tuft: an asset chosen by weight, the drawn blades in the ground's own colour or a picture's sprite */
            if (!g.flowers_only && g.wsum > 0.0f)
            {
                float pick = rnd01(sd + 14u) * g.wsum;
                int ai = -1;
                for (int k2 = 0; k2 < ASSETMAX; ++k2)
                {
                    const GAsset* a2 = &g.asset[k2];
                    if (a2->weight <= 0.0f || (k2 >= DRAWN && !a2->n))
                        continue;
                    ai = k2;
                    if ((pick -= a2->weight) < 0.0f)
                        break;
                }
                const float wv = g.width * (0.85f + 0.3f * rnd01(sd + 13u));
                if (ai >= 0 && ai < DRAWN)
                {
                    const float fit = minf(1.0f, room / (0.5f * wv * grow));
                    const float bc[3] = { col[0] * lift[ai], col[1] * lift[ai], col[2] * lift[ai] };
                    if (fit >= 0.33f)
                        keep_blades(ai, pos, rnd01(sd + 5u) * 3.14159265f, h * fit, wv * grow * fit, bc, keep, hash32(sd + 30u)), ++tufts;
                }
                else if (ai >= DRAWN)
                {
                    const GAsset* as = &g.asset[ai];
                    const int ri = (int)(rnd01(sd + 24u) * (float)as->n) % as->n;
                    const float ph = (as->r[ri].y1 - as->r[ri].y0) / as->tallest;
                    const float sh = maxf(0.04f, h * as->height * ph); /* keep_sprite's width follows the height */
                    const float fit = minf(1.0f, room / (0.5f * sh * (as->r[ri].x1 - as->r[ri].x0) / (as->r[ri].y1 - as->r[ri].y0) * wv / 0.8f));
                    if (fit >= 0.33f)
                    {
                        /* the light on it (twice the picture at most, as the game draws its plants), and with grass.match its colour
                         * taken toward the ground's under it: the sprite's own colour times (ground / sprite) to that power */
                        const float lit = minf(light * g.brightness, 2.0f);
                        float sc[3];
                        for (int k = 0; k < 3; ++k)
                            sc[k] = lit * (g.match > 0.0f && !as->flower ? powf(clampf((float)texel[k] / 255.0f / as->avg[ri][k], 0.1f, 8.0f), g.match) : 1.0f);
                        keep_sprite(ai, ri, pos, rnd01(sd + 5u) * 3.14159265f, sh * fit, wv / 0.8f, sc, keep);
                        ++tufts;
                    }
                }
            }
            /* flowers grow in patches, a share of the tufts' places, in the colour of their patch (mostly); alone, they
             * spread beyond the patches too, thinner */
            if (g.flowers > 0.0f && room > 0.3f)
            {
                const float patch = vnoise(pos[0] * (1.0f / 7.0f), pos[2] * (1.0f / 7.0f));
                const float k2 = clampf((patch - 0.5f) / 0.35f, 0.0f, 1.0f);
                if (rnd01(sd + 6u) < g.flowers * (g.flowers_only ? 0.3f + 0.7f * k2 : k2))
                {
                    const uint32_t cell = hash32((uint32_t)(int)floorf(pos[0] / 7.0f) * 73856093u ^ (uint32_t)(int)floorf(pos[2] / 7.0f) * 19349663u);
                    const int ci = rnd01(sd + 7u) < 0.75f ? (int)(cell % 5u) : (int)(hash32(sd + 8u) % 5u);
                    const float lum = (vf[0] + vf[1] + vf[2]) * (1.0f / 3.0f) * g.flower_brightness * 1.1f;
                    float bloom[3], stem[3];
                    for (int i = 0; i < 3; ++i)
                        bloom[i] = palette[ci][i] * lum, stem[i] = col[i] * 0.9f;
                    const float fh = maxf(0.05f, g.height * (1.0f + (2.0f * rnd01(sd + 9u) - 1.0f) * g.variation) * 1.15f) * grow;
                    const float ox = (rnd01(sd + 10u) - 0.5f) * 0.3f, oz = (rnd01(sd + 11u) - 0.5f) * 0.3f;
                    keep_flower(pos[0] + ox, pos[1] + lean(ox, oz), pos[2] + oz, rnd01(sd + 12u) * 3.14159265f, fh, stem, bloom, keep);
                    ++flowers;
                }
            }
        }
        if (gfx_now_ns() >= deadline) /* the frame's time for it is up: the rest next frame */
        {
            ++k;
            break;
        }
    }
    g.made_tufts += tufts - g_build.tufts + flowers - g_build.flowers;
    g_build.k = k, g_build.tufts = tufts, g_build.flowers = flowers;
    if (k < in->n)
        return 0;
    build_finish(c);
    return 1;
}

/* piece ii's square (cx, cz), or NULL when it isn't kept */
static GChunk* kept_find(uint32_t ii, int32_t cx, int32_t cz)
{
    for (uint32_t s = g.inst[ii].kept; s; s = g_kept.slot[s - 1].next)
        if (g_kept.slot[s - 1].cx == cx && g_kept.slot[s - 1].cz == cz)
            return &g_kept.slot[s - 1];
    return NULL;
}

static void kept_free(uint32_t s);

/* a new square of piece ii (made by kept_make), or NULL when the slots are all this frame's. With every slot used, the square
 * drawn longest ago is let go for it: squares are kept long out of sight (KEPT_IDLE), and the empty ones (ground that grows
 * nothing there) count against no budget, so a long ride could otherwise fill the slots and leave the ground ahead bare */
static GChunk* kept_new(uint32_t ii, int32_t cx, int32_t cz)
{
    uint32_t s;
    if (g_kept.nfree)
        s = g_kept.free[--g_kept.nfree];
    else if (g_kept.hi < KEPTMAX)
        s = g_kept.hi++;
    else
    {
        uint32_t oldest = KEPTMAX;
        for (uint32_t k = 0; k < g_kept.hi; ++k)
            if (g_kept.slot[k].stamp && g_kept.slot[k].drawn != g.scene &&
                (oldest == KEPTMAX || g.scene - g_kept.slot[k].drawn > g.scene - g_kept.slot[oldest].drawn))
                oldest = k;
        if (oldest == KEPTMAX)
            return NULL;
        kept_free(oldest);
        s = g_kept.free[--g_kept.nfree];
    }
    GChunk* c = &g_kept.slot[s];
    memset(c, 0, sizeof *c);
    c->inst = ii, c->stamp = g.inst[ii].stamp, c->cx = cx, c->cz = cz;
    c->next = g.inst[ii].kept, g.inst[ii].kept = s + 1;
    return c;
}

/* square s let go: its buffer, and its place in its piece's list while the piece is still there */
static void kept_free(uint32_t s)
{
    GChunk* c = &g_kept.slot[s];
    for (uint32_t l = 0; l < c->nlay; ++l)
        if (c->lay[l].buf)
            gfx_buf_destroy(c->lay[l].buf);
    g_kept.bytes -= c->bytes;
    if (c->inst < g.ninst && g.inst[c->inst].stamp == c->stamp)
        for (uint32_t* at = &g.inst[c->inst].kept; *at; at = &g_kept.slot[*at - 1].next)
            if (*at == s + 1)
            {
                *at = c->next;
                break;
            }
    memset(c, 0, sizeof *c);
    g_kept.free[g_kept.nfree++] = s;
}

static void kept_clear(void)
{
    for (uint32_t s = 0; s < g_kept.hi; ++s)
        for (uint32_t l = 0; l < g_kept.slot[s].nlay; ++l)
            if (g_kept.slot[s].lay[l].buf)
                gfx_buf_destroy(g_kept.slot[s].lay[l].buf);
    memset(g_kept.slot, 0, sizeof g_kept.slot);
    g_kept.hi = g_kept.nfree = 0, g_kept.bytes = 0;
    g_build.active = 0; /* the square in hand with them */
    for (uint32_t i = 0; i < g.ninst; ++i)
        g.inst[i].kept = 0;
}

/* the squares let go: those of pieces gone or that grow nothing now, those not drawn for KEPT_IDLE scenes, and past the budget
 * (KEPT_PER_TUFT of the limit) the ones drawn longest ago */
static void kept_sweep(void)
{
    for (uint32_t s = 0; s < g_kept.hi; ++s)
    {
        const GChunk* c = &g_kept.slot[s];
        if (!c->stamp)
            continue;
        const GInst* in = c->inst < g.ninst ? &g.inst[c->inst] : NULL;
        if (!in || in->stamp != c->stamp || !in->n || in->moving || g.scene - c->drawn > KEPT_IDLE)
            kept_free(s);
    }
    const size_t budget = (size_t)g.limit * KEPT_PER_TUFT > KEPT_FLOOR ? (size_t)g.limit * KEPT_PER_TUFT : KEPT_FLOOR;
    while (g_kept.bytes > budget)
    {
        uint32_t oldest = KEPTMAX;
        for (uint32_t s = 0; s < g_kept.hi; ++s)
            if (g_kept.slot[s].bytes && g_kept.slot[s].drawn != g.scene &&
                (oldest == KEPTMAX || g.scene - g_kept.slot[s].drawn > g.scene - g_kept.slot[oldest].drawn))
                oldest = s;
        if (oldest == KEPTMAX)
            break; /* all of it is this frame's: kept over the budget rather than let go and made again next frame */
        kept_free(oldest);
    }
}

/* the quads' indices, two triangles of four corners each (0 1 2, 1 3 2), QUADS_DRAW of them: every square's corners are drawn
 * through these, a GPU buffer made once */
static uint16_t g_quad_idx[QUADS_DRAW * 6];
static GfxBuf* g_quad_ibuf;

static GfxBuf* quad_indices(void)
{
    if (!g_quad_ibuf)
    {
        for (uint32_t q = 0; q < QUADS_DRAW; ++q)
        {
            const uint16_t b = (uint16_t)(4 * q);
            uint16_t* i = &g_quad_idx[6 * q];
            i[0] = b, i[1] = (uint16_t)(b + 1), i[2] = (uint16_t)(b + 2), i[3] = (uint16_t)(b + 1), i[4] = (uint16_t)(b + 3), i[5] = (uint16_t)(b + 2);
        }
        if ((g_quad_ibuf = gfx_buf_create((uint32_t)sizeof g_quad_idx)) != NULL)
            gfx_buf_upload(g_quad_ibuf, g_quad_idx, (uint32_t)sizeof g_quad_idx);
    }
    return g_quad_ibuf;
}

void grass_device_lost(void)
{
    reset_world(); /* the kept tufts, as at a zone change: grown again from the world's triangles as the game draws them */
    for (int t = 0; t < DRAWN; ++t)
        gfx_tex_destroy(g.tuft[t]), g.tuft[t] = NULL;
    gfx_tex_destroy(g.flower), g.flower = NULL;
    gfx_tex_destroy(g.moss_tex), g.moss_tex = NULL;
    for (unsigned i = 0; i < ASSETMAX; ++i) /* (an asset keeps its picture, as->px: made again as with a new one) */
        gfx_tex_destroy(g.asset[i].tex), g.asset[i].tex = NULL;
    gfx_buf_destroy(g_quad_ibuf), g_quad_ibuf = NULL;
}

/* the tufts' draw as the scene has it: alpha-tested, fogged, into the scene's target before the scene effects (AO, fog, DLSS) run
 * on it, through the vertex function with this frame's constants; kept_draw sets each kind's texture and sway, and each square */
static void kept_state(GfxDraw* d, const GfxScene* s)
{
    static uint32_t prog; /* the vertex function's hash, as d3d8.c hashes the game's */
    if (!prog)
    {
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < sizeof g_tuft_vs / sizeof g_tuft_vs[0]; ++i)
            for (int b = 0; b < 32; b += 8)
                h = (h ^ ((g_tuft_vs[i] >> b) & 255)) * 16777619u;
        prog = h ? h : 1;
    }
    memset(d, 0, sizeof *d);
    d->vs.prog = prog, d->vs_tokens = g_tuft_vs;
    d->vs.el[GFX_R_POSITION] = (GfxElem){ 1, 0, GFX_SHORT4, 0 };
    d->vs.el[GFX_R_DIFFUSE] = (GfxElem){ 1, 0, GFX_D3DCOLOR, 0 };
    d->vs.el[GFX_R_TEXCOORD0 + 1] = (GfxElem){ 1, 0, GFX_SHORT4, 0 };
    d->vs.el[GFX_R_TEXCOORD0 + 2] = (GfxElem){ 1, 0, GFX_D3DCOLOR, 0 };
    d->u.offset[GFX_R_POSITION] = (int32_t)offsetof(PVert, a), d->u.offset[GFX_R_DIFFUSE] = (int32_t)offsetof(PVert, col);
    d->u.offset[GFX_R_TEXCOORD0 + 1] = (int32_t)offsetof(PVert, b), d->u.offset[GFX_R_TEXCOORD0 + 2] = (int32_t)offsetof(PVert, keep);
    d->u.stride[0] = (int32_t)sizeof(PVert);
    d->vs.ntex = 1;
    d->fs.nstages = 1;
    d->fs.alpha_func = 5; /* GREATER */
    if (s->fog[2] != 0.0f)
        d->fs.fog = 3, d->u.params[2] = s->fog[0], d->u.params[3] = s->fog[1];
    memcpy(d->u.fogcolor, s->fogcolor, sizeof d->u.fogcolor);
    for (int i = 0; i < 4; ++i)
        d->u.mat_d[i] = d->u.mat_a[i] = d->u.ambient[i] = d->u.tfactor[i] = 1.0f;
    for (int i = 0; i < 4; ++i)
        d->u.vp[i] = (float)s->vp[i];
    memcpy(d->vp, s->vp, sizeof d->vp);
    float jit[2];
    if (gfx_dlss_jitter(jit))
        memcpy(&d->u.pad0[0], &jit[0], 4), memcpy(&d->u.pad0[1], &jit[1], 4);
    float vpm[16];
    mat_mul(vpm, s->view, s->proj);
    memcpy(d->u.wvp, vpm, 64);
    memcpy(d->u.wv, s->view, 64);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            d->u.vsc[i][j] = vpm[j * 4 + i]; /* column i */
    const float R = g.radius;
    const float c[][4] = { { s->focus[0], s->focus[1], s->focus[2], 0 }, { 0, 0, 0, 0 },
        { -1.0f / (0.2f * R), 5.0f, -1.0f / ((1.0f - KEEP_EDGE) * R), 1.0f / (1.0f - KEEP_EDGE) }, { 0, 1, 0.3f, 1e-6f } };
    memcpy(d->u.vsc[4], c, sizeof c);
    const float c11[4] = { 2, -1, 4, 32 };
    memcpy(d->u.vsc[11], c11, sizeof c11);
    d->u.vsc[16][1] = 1.0f / 32767.0f;
    for (int i = 0; i < 3; ++i)
        d->u.vsc[17][i] = g.light_shown;
    d->u.vsc[17][3] = 1.0f;
    for (int i = 0; i < 3; ++i)
    {
        const float span = g.ring_to[i] - g.ring_from[i];
        d->u.vsc[12][i] = -g.ring_from[i] / span, d->u.vsc[13][i] = 1.0f / (span * R), d->u.vsc[14][i] = g.ring_step[i];
    }
    d->pipe.write_mask = 0xF;
    d->depth.zenable = 1, d->depth.zwrite = 1, d->depth.zfunc = 4; /* LESSEQUAL */
    d->cull = 1;                                                    /* NONE */
    d->fill = 3;                                                    /* SOLID */
    d->caster = 0; /* the grass casts no shadow */
    d->prim = GFX_TRIANGLELIST;
    d->indices = g_quad_idx, d->index_size = 2, d->ibuf = g_quad_ibuf, d->ibuf_off = 0;
}

/* a kind's sway, spawn's waves: sin(t a + x b + z c) over x and cos(t d + x e + z f) over z, as the vertex function takes them (in
 * turns; its sine of turns + 0.5 is sin, of turns + 0.75 cos) */
static void kept_sway(GfxDraw* d, int kind, float t)
{
    static const float wave[3][6] = { { 1.7f, 0.31f, 0.23f, 1.3f, 0.19f, -0.29f },  /* the drawn blades */
        { 1.9f, 0.37f, 0.21f, 1.4f, 0.17f, -0.31f },                                    /* flowers */
        { 1.6f, 0.29f, 0.25f, 1.2f, 0.21f, -0.27f } };                                  /* the assets' sprites */
    const float* w = wave[kind < DRAWN ? 0 : kind == FLOWERS ? 1 : 2];
    const float turn = 1.0f / 6.2831853f, tb = g.wind_turb; /* the waves' frequency over the field, times grass.wind.turbulence */
    d->u.vsc[16][2] = kind == MOSS ? 0.0f : kind < DRAWN ? 0.09f : 0.07f; /* how far a corner sways, of its height over the root (moss: still) */
    const float c[3][4] = { { w[1] * turn * tb, w[4] * turn * tb, 0, 0 }, { w[2] * turn * tb, w[5] * turn * tb, 0, 0 },
        { fmodf(t * w[0] * turn, 1.0f) + 0.5f, fmodf(t * w[3] * turn, 1.0f) + 0.75f, 0, 0 } };
    memcpy(d->u.vsc[8], c, sizeof c);
}

/* the sway's clock, in seconds of wind: real time times grass.wind.speed, added up frame by frame so a change of speed bends no tuft
 * (the time times the speed would jump); a stall of over a quarter of a second adds no more than that; round at 600 s as it always was */
static float wind_clock(void)
{
    static uint64_t last;
    static double wind;
    const uint64_t now = gfx_now_ns();
    const double dt = last && now > last ? fmin((double)(now - last) * 1e-9, 0.25) : 0.0;
    last = now;
    wind = fmod(wind + dt * (double)g.wind_speed, 600.0);
    return (float)wind;
}

/* an asset's picture as a texture, with its mips (a power-of-two picture; another keeps one level) */
static void asset_texture(GAsset* as)
{
    as->dirty = 0;
    if (as->tex)
        gfx_tex_destroy(as->tex), as->tex = NULL;
    if (!as->px || !as->w || !as->h)
        return;
    const int pot = !(as->w & (as->w - 1)) && !(as->h & (as->h - 1));
    uint32_t levels = 1;
    for (uint32_t w = as->w, h = as->h; pot && (w > 1 || h > 1); w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1)
        ++levels;
    GfxTex* t = gfx_tex_create(GFX_TEX_2D, FMT_A8R8G8B8, as->w, as->h, levels, GFX_USE_SAMPLE);
    if (!t)
        return;
    uint32_t* cur = (uint32_t*)malloc((size_t)as->w * as->h * 4);
    if (!cur)
    {
        gfx_tex_destroy(t);
        return;
    }
    memcpy(cur, as->px, (size_t)as->w * as->h * 4);
    uint32_t w = as->w, h = as->h;
    for (uint32_t l = 0; l < levels; ++l)
    {
        gfx_tex_upload(t, 0, l, cur, w * 4);
        if (l + 1 == levels)
            break;
        const uint32_t nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        for (uint32_t y = 0; y < nh; ++y)
            for (uint32_t x = 0; x < nw; ++x)
            {
                uint32_t sum[4] = { 0, 0, 0, 0 };
                for (int k = 0; k < 4; ++k)
                {
                    const uint32_t sx = x * 2 + (uint32_t)(k & 1) < w ? x * 2 + (uint32_t)(k & 1) : w - 1;
                    const uint32_t sy = y * 2 + (uint32_t)(k >> 1) < h ? y * 2 + (uint32_t)(k >> 1) : h - 1;
                    const uint32_t c = cur[(size_t)sy * w + sx];
                    sum[0] += c >> 24, sum[1] += (c >> 16) & 255, sum[2] += (c >> 8) & 255, sum[3] += c & 255;
                }
                cur[(size_t)y * nw + x] = (sum[0] / 4 << 24) | (sum[1] / 4 << 16) | (sum[2] / 4 << 8) | sum[3] / 4;
            }
        w = nw, h = nh;
    }
    free(cur);
    as->tex = t;
}

/* a square in reach: not kept yet (c NULL), or kept thinner than its ring now wants */
typedef struct KWant
{
    uint32_t inst;
    int32_t cx, cz;
    float d, share;
    GChunk* c;
} KWant;

static int want_nearer(const void* a, const void* b)
{
    const float da = ((const KWant*)a)->d, db = ((const KWant*)b)->d;
    return da < db ? -1 : da > db;
}

/* the light on a kind's tufts (c17): with grass.match, the light the game puts on the ground (ground_light_note) on them too, as much,
 * and the time of day's dimming (grass.brightness.auto) only for what's left of the match: the ground's own light already dims at
 * night, and both at once made the tufts half the ground's brightness at midnight (Batallia Downs). Not on flowers (the drawn ones,
 * the catalogue's kind "flower"), which keep their own colours and the time of day's light */
static void kind_light(int flower, float c17[4])
{
    const float m = g.match > 0.0f && g.ground_known && !flower ? g.match : 0.0f;
    for (int i = 0; i < 3; ++i)
        c17[i] = powf(g.light_shown, 1.0f - m) * (m > 0.0f ? powf(clampf(g.ground_shown[i], 0.05f, 4.0f), m) : 1.0f);
}

/* the camera's four side planes (its view times projection, row vectors: a point p is in view while p . plane + plane[3] >= 0 for
 * each, w + x, w - x, w + y and w - y), for grass.cull; behind the camera fails them all */
static void view_planes(const GfxScene* s, float pl[4][4])
{
    float m[16];
    mat_mul(m, s->view, s->proj);
    for (int i = 0; i < 4; ++i)
        for (int r = 0; r < 4; ++r)
            pl[i][r] = m[r * 4 + 3] + ((i & 1) ? -1.0f : 1.0f) * m[r * 4 + (i >> 1)];
}

/* a layer of a square in view (grass.cull): its roots' box (their middle and half span, a cube) widened by its tallest corner's
 * reach from its root and the sway, outside none of the planes */
static int layer_in_view(const GLayer* L, const float pl[4][4])
{
    const float e = 32000.0f * (L->org[3] + 1.2f * L->step) + 0.25f;
    for (int i = 0; i < 4; ++i)
        if (pl[i][0] * L->org[0] + pl[i][1] * L->org[1] + pl[i][2] * L->org[2] + pl[i][3] +
                (fabsf(pl[i][0]) + fabsf(pl[i][1]) + fabsf(pl[i][2])) * e < 0.0f)
            return 0;
    return 1;
}

/* this frame's tufts: the kept squares in reach drawn, those not kept yet (or too thin for their ring now) made first, the nearest
 * first; t the time, for the sway */
static void kept_draw(GfxTex* color, GfxTex* depth, const GfxScene* s, float t)
{
    static KWant want[KEPTMAX];
    static uint32_t drawn[KEPTMAX];
    uint32_t nwant = 0, ndrawn = 0;
    const float R = g.radius, cell = g.cell, fx = s->focus[0], fz = s->focus[2];
    g.near_d = 1e30f;
    for (uint32_t i = 0; i < g.ninst; ++i)
    {
        const GInst* in = &g.inst[i];
        if (g.scene - in->seen > 12 || !in->n || in->moving)
            continue;
        {
            const float dx = in->ctr[0] - fx, dz = in->ctr[2] - fz, d = sqrtf(dx * dx + dz * dz) - in->rad;
            if (d < g.near_d)
                g.near_d = d, g.near_tex = g.tex[g.mesh[in->mesh].tex].hash;
            if (d > R)
                continue;
        }
        const int32_t x0 = (int32_t)floorf((in->ctr[0] - in->rad) / cell), x1 = (int32_t)floorf((in->ctr[0] + in->rad) / cell);
        const int32_t z0 = (int32_t)floorf((in->ctr[2] - in->rad) / cell), z1 = (int32_t)floorf((in->ctr[2] + in->rad) / cell);
        for (int32_t cz = z0; cz <= z1; ++cz)
            for (int32_t cx = x0; cx <= x1; ++cx)
            {
                /* the square's nearest point to the player */
                const float qx = clampf(fx, (float)cx * cell, (float)(cx + 1) * cell) - fx, qz = clampf(fz, (float)cz * cell, (float)(cz + 1) * cell) - fz;
                const float d = sqrtf(qx * qx + qz * qz);
                if (d >= R)
                    continue;
                const float share = ring_level(d);
                GChunk* c = kept_find(i, cx, cz);
                if (c && ndrawn < KEPTMAX)
                    c->drawn = g.scene, drawn[ndrawn++] = (uint32_t)(c - g_kept.slot);
                if ((!c || c->share < share) && nwant < KEPTMAX)
                {
                    const KWant w = { i, cx, cz, d, share, c };
                    want[nwant++] = w;
                }
            }
    }
    /* made, within the frame's time for it (grass.build): the square in hand first, then the wanted squares, the nearest first (once
     * a new zone's floors are known: floor_ready) */
    if ((g_build.active || nwant) && floor_ready())
    {
        const uint64_t t0 = gfx_now_ns(), deadline = g.build_ms < 0.0f ? UINT64_MAX : t0 + (uint64_t)(g.build_ms * 1e6f);
        if (nwant)
            qsort(want, nwant, sizeof *want, want_nearer);
        uint32_t w = 0;
        for (;;)
        {
            if (!g_build.active)
            {
                /* the next square wanted (not one brought up to its share since the list was made) */
                while (w < nwant && want[w].c && want[w].c->share >= want[w].share)
                    ++w;
                if (w >= nwant)
                    break;
                GChunk* c = want[w].c;
                if (!c)
                {
                    if (!(c = kept_new(want[w].inst, want[w].cx, want[w].cz)))
                        break;
                    c->drawn = g.scene;
                    if (ndrawn < KEPTMAX)
                        drawn[ndrawn++] = (uint32_t)(c - g_kept.slot);
                }
                build_begin(c, &g.inst[want[w].inst], want[w].share);
                ++w;
            }
            if (!build_step(deadline) || gfx_now_ns() >= deadline)
                break;
        }
        g.made_ns += gfx_now_ns() - t0;
    }
    kept_sweep();
    g.tufts = g.nflowers = 0;
    for (uint32_t k = 0; k < ndrawn; ++k)
        g.tufts += g_kept.slot[drawn[k]].tufts, g.nflowers += g_kept.slot[drawn[k]].flowers;
    g.placed = g.tufts + g.nflowers, g.reach_tris = ndrawn;
    if (!ndrawn)
        return;
    /* each kind's texture, all made before the first draw (one made between draws isn't drawn from that frame) */
    GfxTex* tex_of[KINDS];
    uint32_t first_of[KINDS];
    for (int kind = 0; kind < KINDS; ++kind)
    {
        uint32_t first = 0;
        while (first < ndrawn && !chunk_has(&g_kept.slot[drawn[first]], kind))
            ++first;
        first_of[kind] = first, tex_of[kind] = NULL;
        if (first == ndrawn)
            continue;
        if (kind < DRAWN)
        {
            if (!g.tuft[kind])
            {
                if (kind)
                    make_tuft_texture();
                else
                    make_tuft1_texture();
            }
            tex_of[kind] = g.tuft[kind];
        }
        else if (kind == MOSS)
        {
            if (!g.moss_tex)
                make_moss_texture();
            tex_of[kind] = g.moss_tex;
        }
        else if (kind == FLOWERS)
        {
            if (!g.flower)
                make_flower_texture();
            tex_of[kind] = g.flower;
        }
        else
        {
            GAsset* as = &g.asset[kind];
            if (as->dirty || !as->tex)
                asset_texture(as);
            tex_of[kind] = as->tex;
        }
    }
    if (!quad_indices())
        return;
    /* the scene's depth as it is before the tufts: the scene effects' occlusion and contact shadows read the world from it, so the
     * grass casts neither and the pixels it covers skip them */
    gfx_scene_world_depth(depth);
    static GfxDraw d;
    kept_state(&d, s);
    gfx_set_targets(color, 0, 0, depth);
    float pl[4][4];
    view_planes(s, pl);
    g.quads_in = g.quads_all = 0;
    for (int kind = 0; kind < KINDS; ++kind)
    {
        GfxTex* tex = tex_of[kind];
        const uint32_t first = first_of[kind];
        if (!tex)
            continue;
        const int sprite = kind >= DRAWN && kind < ASSETMAX;
        /* the assets' sprites are drawn as the game draws its plants: texture times light, doubled (more, to match the ground's
         * colour: grass.match); sharper alpha edges */
        d.fs.st[0] = (GfxStage){ (uint8_t)(sprite ? 6 /* MODULATE4X: keep_sprite's colours are a quarter */
                                                  : kind < DRAWN || kind == MOSS ? 5 /* MODULATE2X: keep_blades' and keep_moss's are halved */ : 4 /* MODULATE */),
            2 /* TEXTURE */, 0 /* DIFFUSE */, 1, 4, 2, 0, 1, 1, 1, 0, 2 };
        kind_light(kind == FLOWERS || (sprite && g.asset[kind].flower), d.u.vsc[17]);
        slope_light(kind == FLOWERS || (sprite && g.asset[kind].flower), s, d.u.vsc);
        d.u.params[1] = sprite ? 120.0f : 100.0f;
        d.tex[0] = tex;
        /* clamped, not wrapped: with wrap the linear filter at a quad's top edge blends in the opaque blade bases from the picture's
         * bottom row, a dark line along every tuft's top; a sheet's small mips blend its neighbouring sprites together: no coarser
         * than the fourth */
        d.samp[0] = (GfxSampler){ 3, 3, 3, 2, 2, 2, 1, 0, (uint8_t)(sprite ? 4 : 0), 0, { 0, 0 }, 0 };
        kept_sway(&d, kind, t);
        for (uint32_t k = first; k < ndrawn; ++k)
        {
            const GChunk* c = &g_kept.slot[drawn[k]];
            for (uint32_t l = 0; l < c->nlay; ++l)
            {
                const GLayer* L = &c->lay[l];
                if (!L->n[kind] || !L->buf)
                    continue;
                g.quads_all += L->n[kind] / 4;
                if (g.cull && !layer_in_view(L, pl))
                    continue;
                g.quads_in += L->n[kind] / 4;
                memcpy(d.u.vsc[15], L->org, sizeof L->org);
                d.u.vsc[16][0] = L->step;
                /* the kind's corners, four a quad: where they start in the buffer, as d3d8.c gives a static buffer's (not
                 * vertex_start), QUADS_DRAW quads a draw at most */
                for (uint32_t q = 0, quads = L->n[kind] / 4; q < quads; q += QUADS_DRAW)
                {
                    const uint32_t nq = quads - q < QUADS_DRAW ? quads - q : QUADS_DRAW;
                    d.buf[0] = L->buf, d.buf_off[0] = (L->first[kind] + 4 * q) * (uint32_t)sizeof(PVert),
                    d.size[0] = 4 * nq * (uint32_t)sizeof(PVert);
                    d.count = 2 * nq;
                    gfx_draw(&d);
                }
            }
        }
    }
    /* the glow round an avatar or a trust, laid partway through the world, now under the tufts drawn over it: laid again over them */
    gfx_glow_over_grass(color, depth);
}

/* //vana's Debug: the rings as flat bands round where the player stands, over everything (no depth test): ring 1 green, 2 yellow, 3
 * orange, where the tufts start to shrink (0.8 of the radius) blue, the radius red. A band widens with its distance, to stay seen. */
static void draw_rings(GfxTex* color, GfxTex* depth, const GfxScene* s)
{
    enum { SEG = 128, LINES = 5 };
    typedef struct
    {
        float x, y, z;
        uint32_t col;
    } RVert;
    static RVert v[LINES * SEG * 6];
    static const uint32_t col[LINES] = { 0xFF40FF40u, 0xFFFFE040u, 0xFFFF8020u, 0xFF40A0FFu, 0xFFFF3030u }; /* (gfx_hlsl.h GRASS_RINGS_HLSL's) */
    const float R = g.radius, at[LINES] = { g.ring_at[0] * R, g.ring_at[1] * R, g.ring_at[2] * R, 0.8f * R, R };
    const float fx = s->focus[0], fy = s->focus[1] - 0.05f, fz = s->focus[2];
    uint32_t n = 0;
    for (int l = 0; l < LINES; ++l)
    {
        const float hw = maxf(0.04f, at[l] * 0.006f), r0 = maxf(0.0f, at[l] - hw), r1 = at[l] + hw;
        if (at[l] <= 0.0f || at[l] > R + 1e-3f)
            continue;
        for (int k = 0; k < SEG; ++k)
        {
            const float a0 = (float)k * (6.2831853f / SEG), a1 = (float)(k + 1) * (6.2831853f / SEG);
            const float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
            const RVert i0 = { fx + c0 * r0, fy, fz + s0 * r0, col[l] }, o0 = { fx + c0 * r1, fy, fz + s0 * r1, col[l] };
            const RVert i1 = { fx + c1 * r0, fy, fz + s1 * r0, col[l] }, o1 = { fx + c1 * r1, fy, fz + s1 * r1, col[l] };
            v[n++] = i0, v[n++] = o0, v[n++] = i1, v[n++] = o0, v[n++] = o1, v[n++] = i1;
        }
    }
    if (!n)
        return;
    static GfxDraw d;
    memset(&d, 0, sizeof d);
    d.vs.el[GFX_R_POSITION] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d.vs.el[GFX_R_DIFFUSE] = (GfxElem){ 1, 0, GFX_D3DCOLOR, 0 };
    d.u.offset[GFX_R_POSITION] = (int32_t)offsetof(RVert, x), d.u.offset[GFX_R_DIFFUSE] = (int32_t)offsetof(RVert, col);
    d.u.stride[0] = (int32_t)sizeof(RVert);
    d.fs.nstages = 1;
    d.fs.st[0] = (GfxStage){ 2 /* SELECTARG1 */, 0 /* DIFFUSE */, 0, 1, 2, 0, 0, 1, 1, 0, 0, 2 };
    for (int i = 0; i < 4; ++i)
        d.u.mat_d[i] = d.u.mat_a[i] = d.u.ambient[i] = d.u.tfactor[i] = 1.0f;
    for (int i = 0; i < 4; ++i)
        d.u.vp[i] = (float)s->vp[i];
    memcpy(d.vp, s->vp, sizeof d.vp);
    float jit[2];
    if (gfx_dlss_jitter(jit))
        memcpy(&d.u.pad0[0], &jit[0], 4), memcpy(&d.u.pad0[1], &jit[1], 4);
    float vpm[16];
    mat_mul(vpm, s->view, s->proj);
    memcpy(d.u.wvp, vpm, 64);
    memcpy(d.u.wv, s->view, 64);
    d.pipe.write_mask = 0xF;
    d.cull = 1; /* NONE */
    d.fill = 3; /* SOLID */
    d.prim = GFX_TRIANGLELIST;
    d.count = n / 3;
    d.data[0] = v, d.size[0] = n * (uint32_t)sizeof(RVert);
    gfx_set_targets(color, 0, 0, depth);
    gfx_draw(&d);
}

/* A report of grass gone when the character blinks (the game deleting and setting up its actor again): the scenes with no tuft after
 * some were drawn, said once a gap has lasted GAP_SCENES, with which of the causes it is (the player's place unknown, so nothing is
 * grown; no ground noted before the scene ended; or ground noted and none in reach, which is also what walking off the grass
 * looks like: said only with grass.log), and once more when the tufts return. `ground` is the ground draws noted this scene. */
#define GAP_SCENES 6
static void gap_note(const GfxScene* s, uint32_t ground)
{
    static uint32_t gap;
    static int had, said;
    if (g.tufts + g.nflowers)
    {
        if (said)
            SAY("[recomp] grass: tufts back after %u scenes without\n", gap);
        had = 1, gap = 0, said = 0;
        return;
    }
    if (!had || ++gap < GAP_SCENES || said)
        return;
    const int unknown = s->focus[3] == 0.0f;
    if (!unknown && ground && !g.log)
        return;
    said = 1;
    SAY("[recomp] grass: no tufts for %u scenes after some: %s (%u ground draws noted this scene)\n", gap,
        unknown ? "the player's place is unknown" : ground ? "ground noted, none in reach" : "no ground noted before the scene ended", ground);
}

static void scene_one(GfxTex* color, GfxTex* depth, const GfxScene* s)
{
    if (!g.zone_ok)
    {
        g.ndraws = 0;
        return;
    }
    ++g.scene;
    g.tufts = g.nflowers = g.placed = 0;
    const uint32_t ground = g.ndraws;
    /* the camera's count: up when it differs from the last scene's, so instance_for knows a piece is seen under the same one as
     * its placement was found under (GInst.cam) */
    if (!g.cam_id || memcmp(g.view_last, s->view, sizeof g.view_last) || memcmp(g.proj_last, s->proj, sizeof g.proj_last))
    {
        memcpy(g.view_last, s->view, sizeof g.view_last), memcpy(g.proj_last, s->proj, sizeof g.proj_last);
        if (!++g.cam_id)
            ++g.cam_id;
    }
    if (g.ndraws && color && depth && s->focus[3] != 0.0f && cam_prepare(s))
    {
        for (uint32_t i = 0; i < g.ndraws; ++i)
        {
            GInst* in = instance_for(&g.draws[i], 0.2f, s->focus, g.radius + 6.0f);
            if (in)
                in->seen = g.scene;
        }
        kept_draw(color, depth, s, wind_clock());
    }
    if (g.show_rings && color && depth && s->focus[3] != 0.0f)
        draw_rings(color, depth, s);
    gap_note(s, ground);
    if (g.log)
    {
        const uint64_t now = gfx_now_ns();
        if (now >= g.next_report)
        {
            g.next_report = now + 3000000000ull;
            const double per = g.frames ? 1e-6 / g.frames : 0.0;
            char nearest[224];
            if (g.near_d < 1e29f)
                snprintf(nearest, sizeof nearest, "nearest ground piece %.1f from you (texture %016llx), %u squares of tufts in reach (%.1f MB kept, "
                    "%u%% of their quads in view%s)", maxf(g.near_d, 0.0f), (unsigned long long)g.near_tex, g.reach_tris,
                    (double)g_kept.bytes / 1048576.0, g.quads_all ? (unsigned)((uint64_t)g.quads_in * 100u / g.quads_all) : 100u,
                    g.cull ? "" : ", grass.cull off");
            else
                snprintf(nearest, sizeof nearest, "no ground piece near you");
            rt_log("[recomp] grass: %u ground draws, %u meshes (%u new, %u unusable), %u pieces (%u new), %u tufts, %u flowers%s, textures %u; %s; "
                   "green-textured draws left out: %u not from a static buffer, %u blended; tufts' places covered by a floor over them: %u; "
                   "grown, by their floor's height from the ground: over 0.25 up %u, 0.05-0.25 up %u, level %u, 0.05-0.25 below %u, "
                   "further below %u, no floor %u; %.2f ms noting draws and %.2f ms making tufts a frame\n",
                g.ndraws, g.nmesh, g.new_meshes, g.bad_meshes, g.ninst, g.new_insts, g.tufts, g.nflowers, g.capped ? " (some triangles capped)" : "",
                g.ntex, nearest, g.rejected[0], g.rejected[1], g.covered, g.grew[0], g.grew[1], g.grew[2], g.grew[3], g.grew[4], g.grew[5],
                (double)g.note_ns * per, (double)g.scene_ns * per);
            g.new_meshes = g.new_insts = g.frames = 0, g.note_ns = g.scene_ns = 0, g.rejected[0] = g.rejected[1] = 0, g.capped = g.covered = 0;
            memset(g.grew, 0, sizeof g.grew);
        }
    }
    g.ndraws = 0;
}

static void capture_finish(void);

/* the light the tufts are drawn with (c17), eased toward VanaCore's time of day (g_light: grass.brightness.auto) and the light on
 * the ground (ground_light, with grass.match): settled in about ten seconds, so the grass never changes suddenly (the game's own
 * light turns from the sun to the moon in a moment at 18:00). The first, and the first after two seconds without grass, at once.
 * One that isn't a number (or infinite, which the next step makes NaN) is never eased from: it's set to its light at once, so it
 * can't stay */
static void light_ease(void)
{
    const uint64_t now = gfx_now_ns();
    const double dt = g.ease_ns && now > g.ease_ns ? (double)(now - g.ease_ns) * 1e-9 : 1e9;
    g.ease_ns = now;
    const float k = dt > 2.0 ? 1.0f : 1.0f - expf(-(float)dt / 3.3f);
    g.light_shown = isfinite(g.light_shown) ? g.light_shown + (g_light - g.light_shown) * k : g_light;
    for (int i = 0; i < 3; ++i)
        g.ground_shown[i] = isfinite(g.ground_shown[i]) ? g.ground_shown[i] + (g.ground_light[i] - g.ground_shown[i]) * k : g.ground_light[i];
}

void grass_scene(GfxTex* color, GfxTex* depth, const GfxScene* s)
{
    const uint64_t t0 = g.log ? gfx_now_ns() : 0;
    /* with the free camera's centre given (grass.freecam), the scene as the grass takes it has the ground under the camera for
     * where the player stands; nothing else's (the shadows', the snow's) changes */
    static GfxScene at_camera;
    if (g_centre[3] != 0.0f && g.zone_ok)
    {
        at_camera = *s;
        centre_ground(at_camera.focus);
        s = &at_camera;
    }
    if (g.nlit) /* the light on the ground this frame (ground_light_note); a frame with no ground keeps the last */
    {
        for (int k = 0; k < 3; ++k)
            g.ground_light[k] = g.lsum[k] / (float)g.nlit, g.lsum[k] = 0.0f;
        if (!g.ground_known) /* the zone's first: taken at once */
            memcpy(g.ground_shown, g.ground_light, sizeof g.ground_shown);
        g.nlit = 0, g.ground_known = 1;
    }
    light_ease();
    scene_one(color, depth, s);
    if (g.tex_full) /* the scene drawn: nothing of this frame's still points at the table */
        tex_make_room();
    {
        float pinv[16], vinv[16];
        g.cw_known = mat_inv(pinv, s->proj) && mat_inv(vinv, s->view) && s->focus[3] != 0.0f;
        if (g.cw_known)
            mat_mul(g.cw_last, pinv, vinv), memcpy(g.focus_last, s->focus, sizeof g.focus_last);
    }
    if (g.cap_frames > 0 && --g.cap_frames == 0)
        capture_finish();
    if (g.log)
        g.scene_ns += gfx_now_ns() - t0, ++g.frames;
}

void grass_made_take(uint64_t* ns, uint32_t* tufts)
{
    *ns = g.made_ns, *tufts = g.made_tufts;
    g.made_ns = 0, g.made_tufts = 0;
}

/* --- the catalogue's assets, from VanaCore (grass_assets.cpp) -------------------------------------------------------------- */
static void sum_weights(void)
{
    if (!g.weights_set)
        g.asset[1].weight = 1.0f; /* nothing said yet: the drawn blades of type 2 */
    g.wsum = 0.0f;
    for (int i = 0; i < ASSETMAX; ++i)
        if (g.asset[i].weight > 0.0f && (i < DRAWN || g.asset[i].n))
            g.wsum += g.asset[i].weight;
}

void grass_asset(unsigned index, const uint32_t* argb, unsigned w, unsigned h, const float* rects, unsigned n, float height)
{
    if (index < DRAWN || index >= ASSETMAX)
        return;
    GAsset* as = &g.asset[index];
    free(as->px);
    as->px = NULL, as->w = as->h = 0, as->n = 0, as->tallest = 1.0f, as->dirty = 1;
    as->height = clampf(height, 0.05f, 10.0f);
    if (argb && w && h && w <= 4096 && h <= 4096 && (as->px = (uint32_t*)malloc((size_t)w * h * 4)) != NULL)
    {
        memcpy(as->px, argb, (size_t)w * h * 4);
        as->w = w, as->h = h;
        for (unsigned i = 0; i < n && as->n < SPRITEMAX; ++i)
        {
            SRect r = { clampf(rects[4 * i], 0.0f, (float)w), clampf(rects[4 * i + 1], 0.0f, (float)h), clampf(rects[4 * i + 2], 0.0f, (float)w),
                clampf(rects[4 * i + 3], 0.0f, (float)h) };
            if (r.x1 - r.x0 < 2.0f || r.y1 - r.y0 < 2.0f)
                continue;
            double sum[3] = { 0, 0, 0 };
            uint32_t got = 0;
            for (uint32_t y = (uint32_t)r.y0; y < (uint32_t)r.y1; ++y)
                for (uint32_t x = (uint32_t)r.x0; x < (uint32_t)r.x1; ++x)
                {
                    const uint32_t c = as->px[(size_t)y * w + x];
                    if ((c >> 24) >= 128)
                        sum[0] += (c >> 16) & 255, sum[1] += (c >> 8) & 255, sum[2] += c & 255, ++got;
                }
            for (int k = 0; k < 3; ++k)
                as->avg[as->n][k] = got ? maxf(0.02f, (float)(sum[k] / got / 255.0)) : 0.5f;
            as->r[as->n++] = r;
            as->tallest = maxf(as->tallest, r.y1 - r.y0);
        }
    }
    sum_weights();
    kept_clear(); /* the kept tufts drew the old picture's sprites */
}

void grass_asset_flower(unsigned index, int flower)
{
    if (index < DRAWN || index >= ASSETMAX || g.asset[index].flower == (flower != 0))
        return;
    g.asset[index].flower = flower != 0;
    kept_clear(); /* the kept sprites were coloured with it */
}

void grass_asset_weight(unsigned index, float weight)
{
    if (index >= ASSETMAX)
        return;
    const int was_set = g.weights_set;
    const float was = g.asset[index].weight;
    g.weights_set = 1;
    g.asset[index].weight = clampf(weight, 0.0f, 1000.0f);
    sum_weights();
    if (g.asset[index].weight != was || !was_set) /* the kept tufts were made with the old mix (set again each settings read) */
        kept_clear();
}

/* --- capture: the cut-out sheets drawn near the player, for the library -------------------------------------------------- */
static void capture_clear(void)
{
    for (uint32_t i = 0; i < g.ncap; ++i)
        free(g.cap[i].bytes), free(g.cap[i].px);
    memset(g.cap, 0, sizeof g.cap);
    g.ncap = 0;
}

void grass_capture_start(void)
{
    capture_clear();
    g.cap_done = 0;
    g.cap_frames = 45; /* most of a second: every plant on screen is drawn at least once */
}

int grass_capturing(void) { return g.cap_frames > 0; }

void grass_capture_note(const GfxDraw* d, uint64_t hash, uint32_t fmt, uint32_t w, uint32_t h, const void* mem, uint32_t pitch)
{
    if (g.cap_frames <= 0 || !mem || !g.cw_known)
        return;
    float clip[4];
    if (!gfx_draw_clip0(d, clip) || !(clip[3] > 1e-4f))
        return;
    float wp[4];
    for (int j = 0; j < 4; ++j)
        wp[j] = clip[0] * g.cw_last[j] + clip[1] * g.cw_last[4 + j] + clip[2] * g.cw_last[8 + j] + clip[3] * g.cw_last[12 + j];
    if (!(fabsf(wp[3]) > 1e-8f))
        return;
    const float dx = wp[0] / wp[3] - g.focus_last[0], dz = wp[2] / wp[3] - g.focus_last[2], dist = sqrtf(dx * dx + dz * dz);
    for (uint32_t i = 0; i < g.ncap; ++i)
        if (g.cap[i].hash == hash)
        {
            g.cap[i].dist = minf(g.cap[i].dist, dist);
            return;
        }
    if (g.ncap >= CAPMAX || w > 2048 || h > 2048 || w < 16 || h < 16)
        return;
    const int dxt = fmt == FMT_DXT1 || fmt == FMT_DXT2 || fmt == FMT_DXT3 || fmt == FMT_DXT4 || fmt == FMT_DXT5;
    const size_t bytes = (size_t)pitch * (dxt ? (h + 3) / 4 : h);
    GCap* c = &g.cap[g.ncap];
    memset(c, 0, sizeof *c);
    if (!(c->bytes = (uint8_t*)malloc(bytes)))
        return;
    memcpy(c->bytes, mem, bytes);
    c->hash = hash, c->fmt = fmt, c->w = w, c->h = h, c->pitch = pitch, c->dist = dist;
    ++g.ncap;
}

/* the sprites on a sheet: the islands of its alpha (looked at in 4-pixel cells, parts a few cells apart joined), largest first up to
 * SPRITEMAX, small specks left out */
static int find_sprites(const uint32_t* px, uint32_t w, uint32_t h, SRect* out)
{
    const uint32_t cw = (w + 3) / 4, ch = (h + 3) / 4, cells = cw * ch;
    uint8_t* m = (uint8_t*)calloc(cells, 1);
    uint8_t* grown = (uint8_t*)calloc(cells, 1);
    int32_t* stack = (int32_t*)malloc(cells * sizeof(int32_t));
    uint32_t* count = (uint32_t*)calloc(cells, sizeof(uint32_t));
    int n = 0;
    if (!m || !grown || !stack || !count)
        goto done;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            if ((px[(size_t)y * w + x] >> 24) > 40)
                m[(y / 4) * cw + x / 4] = 1, ++count[(y / 4) * cw + x / 4];
    /* join parts a cell or two apart (a stem and its flower, a blade's tip): each part grown by a cell */
    for (uint32_t y = 0; y < ch; ++y)
        for (uint32_t x = 0; x < cw; ++x)
        {
            int on = 0;
            for (int dy = -1; dy <= 1 && !on; ++dy)
                for (int dx = -1; dx <= 1 && !on; ++dx)
                {
                    const int yy = (int)y + dy, xx = (int)x + dx;
                    on = yy >= 0 && xx >= 0 && yy < (int)ch && xx < (int)cw && m[yy * cw + xx];
                }
            grown[y * cw + x] = (uint8_t)on;
        }
    SRect found[256];
    uint32_t area[256];
    int nf = 0;
    for (uint32_t i = 0; i < cells; ++i)
    {
        if (grown[i] != 1)
            continue;
        int sp = 0;
        stack[sp++] = (int32_t)i;
        grown[i] = 2;
        uint32_t x0 = cw, y0 = ch, x1 = 0, y1 = 0, a = 0;
        while (sp)
        {
            const uint32_t c = (uint32_t)stack[--sp], cx = c % cw, cy = c / cw;
            a += count[c];
            x0 = cx < x0 ? cx : x0, x1 = cx > x1 ? cx : x1, y0 = cy < y0 ? cy : y0, y1 = cy > y1 ? cy : y1;
            const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
            for (int k = 0; k < 4; ++k)
            {
                const int nx = (int)cx + nb[k][0], ny = (int)cy + nb[k][1];
                if (nx < 0 || ny < 0 || nx >= (int)cw || ny >= (int)ch || grown[ny * cw + nx] != 1)
                    continue;
                grown[ny * cw + nx] = 2;
                stack[sp++] = ny * (int32_t)cw + nx;
            }
        }
        if (nf < 256 && a * 500 >= w * h) /* not a speck: at least 0.2% of the sheet */
        {
            /* the box of the sprite's own texels inside the joined cells */
            SRect r = { (float)(x0 * 4), (float)(y0 * 4), (float)((x1 + 1) * 4 < w ? (x1 + 1) * 4 : w), (float)((y1 + 1) * 4 < h ? (y1 + 1) * 4 : h) };
            found[nf] = r, area[nf] = a, ++nf;
        }
    }
    /* largest first */
    for (int i = 0; i < nf; ++i)
        for (int j = i + 1; j < nf; ++j)
            if (area[j] > area[i])
            {
                const SRect tr = found[i];
                const uint32_t ta = area[i];
                found[i] = found[j], area[i] = area[j], found[j] = tr, area[j] = ta;
            }
    for (int i = 0; i < nf && n < SPRITEMAX; ++i)
        out[n++] = found[i];
done:
    free(m), free(grown), free(stack), free(count);
    return n;
}

/* a sheet of sprites: a fifth of it or more see-through, and something left to see (a wall's or the ground's texture drawn
 * alpha-tested is all but solid) */
static int cut_out(const uint32_t* px, uint32_t w, uint32_t h)
{
    const size_t n = (size_t)w * h;
    size_t clear = 0;
    for (size_t i = 0; i < n; ++i)
        clear += (px[i] >> 24) < 128;
    return clear * 5 >= n && clear * 50 <= n * 49;
}

static void capture_finish(void)
{
    uint32_t kept = 0;
    for (uint32_t i = 0; i < g.ncap; ++i)
    {
        GCap c = g.cap[i];
        if (c.dist > 25.0f || !(c.px = decode_level(c.fmt, c.bytes, c.pitch, c.w, c.h)))
        {
            free(c.bytes);
            continue;
        }
        free(c.bytes), c.bytes = NULL;
        if (!cut_out(c.px, c.w, c.h))
        {
            free(c.px);
            continue;
        }
        c.n = find_sprites(c.px, c.w, c.h, c.r);
        if (!c.n)
        {
            free(c.px);
            continue;
        }
        g.cap[kept++] = c;
    }
    for (uint32_t i = kept; i < g.ncap; ++i)
        memset(&g.cap[i], 0, sizeof g.cap[i]);
    g.ncap = kept;
    g.cap_done = 1;
    SAY("[recomp] grass: captured %u sheet%s of cut-out sprites near you\n", kept, kept == 1 ? "" : "s");
}

int grass_capture_count(void) { return g.cap_frames > 0 ? -1 : g.cap_done ? (int)g.ncap : 0; }

int grass_capture_result(unsigned i, uint64_t* hash, unsigned* w, unsigned* h, const uint32_t** argb, float* rects, unsigned* n)
{
    if (!g.cap_done || i >= g.ncap)
        return 0;
    const GCap* c = &g.cap[i];
    *hash = c->hash, *w = c->w, *h = c->h, *argb = c->px, *n = (unsigned)c->n;
    for (int k = 0; k < c->n; ++k)
        rects[4 * k] = c->r[k].x0, rects[4 * k + 1] = c->r[k].y0, rects[4 * k + 2] = c->r[k].x1, rects[4 * k + 3] = c->r[k].y1;
    return 1;
}

void grass_capture_clear(void)
{
    capture_clear();
    g.cap_done = 0;
}
