/* VanaCore's grass (grass.c): tufts of grass scattered over a zone's grass-textured ground near the player. */
#pragma once

#include <stdint.h>
#include <wchar.h>

#include "gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* [graphics] grass*: enabled; textures the ground textures
 * that count as grass (hex hashes, as the texture packs name them; none listed = any mostly green one); radius how far round
 * the player it reaches; density tufts to a square unit of ground; height how tall they stand, variation (0 to 1) how much
 * that differs from tuft to tuft, width how wide they are (as a rule of its own: the height only changes how tall); brightness
 * against the ground's own colour; flowers (0 to 1) how many of the tufts have a flower, in patches, flower_brightness theirs,
 * flowers_only 1 leaves the tufts out; slope the steepest ground (degrees) it grows on; limit the most tufts in a frame (flowers a quarter of that);
 * log 1 writes what it finds to the log and the grass textures to logs\grass. The grass casts no shadow. */
typedef struct GrassSettings
{
    int enabled;
    const char* textures;
    float radius, density, height, width, variation, brightness, flowers, flower_brightness;
    int flowers_only, log;
    unsigned limit;
    float slope; /* the steepest ground it grows on, degrees */
    /* the rings (rings_set 1; 0, as a settings struct zeroed past slope: 25%, 50%, 75% of the radius keeping 80%, 50%, 30%): rings 1-3
     * start at ring[i] of the radius (0-1) and keep ring_amount[i] of the tufts (0-1, each no more than the ring before's); inside
     * ring 1 all of them */
    int rings_set;
    float ring[3], ring_amount[3];
    int show_rings; /* 1: the rings drawn round the player as coloured circles (//vana's Debug) */
    /* 1: near the brink of a drop steeper than slope (a ledge, a rock's top) the tufts grow smaller, none hanging out over it */
    int edges;
    /* 0 to 1: how far the tufts are made the ground's colour under them, lit as the game lights the ground (0: the sprites their
     * own colours, the drawn blades the ground's in their own shade, neither lit) */
    float match;
    /* 1: the yellow-olive of dry grass (East Sarutabaruta's) counts as grass ground, not only green */
    int dry;
    /* 1: tufts only on ground the player could stand on, by the zone's collision (grass_set_floors): not on a cart's or a crate's
     * top, a tree's boughs, a roof */
    int walkable;
    /* yalms (0 off): no tuft grows with a path's ground (not grass in its texture) this near, so a path's bare ground is wider */
    float pathpad;
    /* ms of a frame spent making squares of tufts (the rest of a square waits for the next frame, its older tufts drawn meanwhile;
     * the nearest squares first): 0 the default, 1.5; under 0 all of it at once */
    float build_ms;
    /* 1: dry grass ground grows on floors the game's footsteps call stone or metal throughout too (the Nyzul portal's meadow); 0,
     * a square of it grows only with soft ground (path, grass, sand, snow) under it somewhere */
    int overgrown;
    /* 1: no tufts on floors the game's footsteps call shallow water (Dragon's Aery's meadow, Pashhow's marsh), walkable or not;
     * 0, they grow there as on grass */
    int nowater;
    /* 1: on the shallow water nowater leaves bare, low cushions of moss in place of the tufts (grass_moss.h); 0 bare. Nothing
     * without nowater */
    int moss;
    /* the wind (0 to 4): how fast the grass sways and how much the sway differs from place to place, each times its own pace (1; 0 still,
     * and 0 a whole field moving as one) */
    float wind_speed, wind_turbulence;
    /* 1: the tufts drawn by what the zone's floors say of their place (//vana's Debug): red, covered by a floor over it (left out
     * otherwise: a building's floor laid over the ground); yellow, no floor at or under it to tell (they grow) */
    int show_cover;
    /* 1: in grass ground, the texels in shadow (too dark to read as any colour) and the yellow-olive of dry grass, as saturated as dry
     * grass is, grow too, not only the green ones: Bhaflau Thickets' forest floors are green on a third of their texels and dark or
     * olive on the rest, so a texture that counted as grass grew on a third of itself */
    int shade;
    /* the dry grass test's thresholds (grass.dry; 0 the defaults): the least mean saturation of a texture's yellow-olive for the texture
     * to be dry grass (0.25) and the reddest hue that is yellow-olive, degrees (46) */
    float dry_sat, dry_hue;
    /* 1 to 100 (the zone's dirt level, VanaCore's own: data\grass_dirt.json; 0 the default, 50): how colourful ground has to be to
     * grow grass. It scales every grass test's bar at once, the green test's lead over red and blue and dry grass's least
     * saturation for a texel and for a texture, by 2 to the (dirt - 50) / 50: higher leaves more ground bare as dirt, lower grows
     * on duller ground */
    float dirt;
    /* 1: grass-textured ground the game draws blended (SRCALPHA, INVSRCALPHA) is ground too, a layer laid over other ground (The
     * Sanctuary of Zi'Tah's moss over its soil), its tufts as many as its vertices' alpha shows of it; 0, opaque ground alone. The
     * zone's (data\grass_dirt.json) unless grass.blended says */
    int blended;
    /* the zone's own grass ground (data\grass_dirt.json's "ground"): hex hashes of textures that are grass ground in this zone
     * whatever their colours say, besides the ones found (The Sanctuary of Zi'Tah's dark leaf-litter floor); NULL or "" none */
    const char* ground;
} GrassSettings;

void grass_configure(const GrassSettings* settings);
/* the zone the player is in (0 while there's none) */
void grass_set_zone(int zone);
/* The graphics device lost and made again (d3d8_device_lost): everything the grass holds on the GPU went with it. The kept
 * tufts are let go as at a zone change and grow again from the world's triangles; the blades', flowers' and assets'
 * textures and the quads' indices are made again at the next draw. */
void grass_device_lost(void);

/* The zone's floors, from its collision, for walkable (VanaCore's grass_floors.cpp; vanacore_gfx.h's VanaFloors says what each
 * array holds). Copied. zone 0: none known; a zone with no pairs: it has none. */
typedef struct GrassFloors
{
    int zone;
    const float* tris;
    const unsigned char* terrain;
    unsigned ntris;
    const unsigned* mesh_first;
    unsigned nmesh;
    const float* matrices;
    unsigned nmatrices;
    const unsigned* pairs;
    unsigned npairs;
} GrassFloors;
void grass_set_floors(const GrassFloors* floors);
/* The terrain (the game's footsteps' codes: 0 object, 1 path, 2 grass, 3 sand, 4 snow, 5 stone, 6 metal, 7 wood, 8 shallow water,
 * 9 deep water) of the nearest of the zone's floors to p (the world, y down) within reach above or below it, and into *dy how
 * far below p it lies (under 0: above); -1 with none within reach, or the floors not known. For VanaCore's water (d3d8_water.h),
 * which asks for the zone's floors as the grass does (graphics.cpp g_water_floors). */
int grass_floor_near(const float p[3], float reach, float* dy);
/* Whether the zone's floors have any of the footsteps' water (terrain 8 or 9) in the squares the box lo..hi (the world) reaches:
 * 1 maybe, 0 none, -1 the floors not known; *set changes each time a zone's floors are handed over, so what was worked out from
 * them can be kept until then (water_sheet) */
int grass_floor_water(unsigned* set, const float lo[3], const float hi[3]);
/* The height (the world's y, down) of the top of the zone's floors at the middle of each of square (tx, tz)'s
 * GRASS_FLOOR_CELLS x GRASS_FLOOR_CELLS cells (the square GRASS_FLOOR_TILE yalms a side from tx, tz times that), 1e30 where
 * none; a floor more than 35 yalms over the next one under it is the zone's invisible lid, passed over. Laid on its own, so
 * the grass's squares stay as they are; 0 with the zone's floors not known. For the far rain (d3d8_rain_ground.h) */
#define GRASS_FLOOR_TILE 32.0f
#define GRASS_FLOOR_CELLS 8
int grass_floor_tops(int tx, int tz, float* top);
/* the light on the tufts and flowers, times their brightness (0.05 to 1; 1 as made): VanaCore's time of day
 * (grass.brightness.auto). The grass is drawn easing toward it, settled in about ten seconds; nothing is made again. */
void grass_set_light(float k);
/* where the tufts grow round in place of where the player stands: the free camera's eye with grass.freecam (VanaCore's track_focus,
 * each frame), the world as the game draws it; known 0, round the player again. They grow round the ground under it. */
void grass_set_centre(float x, float y, float z, int known);
/* 1 while grass is being made for this zone: the draws need looking at */
int grass_on(void);
/* the time spent making squares of tufts, and the tufts made, since the last call (VanaCore's /vana stats) */
void grass_made_take(uint64_t* ns, uint32_t* tufts);

/* The catalogue (VanaCore's grass_assets.cpp): asset `index` (2 to 63; 0 and 1 are the drawn blades, type 1 and type 2, made here) is
 * a picture (0xAARRGGBB rows, w x h; NULL clears it) with `n` sprites on it (x0 y0 x1 y1 each, pixels), standing `height` times
 * grass.height (its tallest sprite). grass_asset_weight: an asset's share of the tufts' places (0 none; the drawn blades of type 2
 * alone until it's first called). */
void grass_asset(unsigned index, const uint32_t* argb, unsigned w, unsigned h, const float* rects, unsigned n, float height);
void grass_asset_weight(unsigned index, float weight);
/* 1: asset `index` is flowers (the catalogue's kind "flower"): grass.match leaves their colours and light alone, as it does the
 * drawn flowers' */
void grass_asset_flower(unsigned index, int flower);

/* Capturing the plants round the player for the library: for the next frames, the sheets of cut-out sprites the game draws near the
 * player (grass_capture_note from d3d8.c, each of the zone's cut-out draws: d3d8.c picks them; sheets less than a fifth see-through
 * are left out) are kept; then grass_capture_count says how many (-1 still at it), and
 * grass_capture_result gives each (its hash, picture and up to 32 sprites found from its alpha) until grass_capture_clear. */
void grass_capture_start(void);
int grass_capturing(void);
void grass_capture_note(const GfxDraw* d, uint64_t hash, uint32_t d3dfmt, uint32_t w, uint32_t h, const void* mem, uint32_t pitch);
int grass_capture_count(void);
int grass_capture_result(unsigned i, uint64_t* hash, unsigned* w, unsigned* h, const uint32_t** argb, float* rects, unsigned* n);
void grass_capture_clear(void);
/* A picture (0xAARRGGBB rows) as a PNG file. 0 if it can't be written. */
int grass_write_png(const wchar_t* path, const uint32_t* argb, unsigned w, unsigned h);

/* A texture the game drew with (its first level, as the game wrote it): the grass reads its colours. 0 if the hash is new. */
void grass_texture(uint64_t hash, uint32_t d3dfmt, uint32_t w, uint32_t h, const void* mem, uint32_t pitch);

/* What d3d8.c knows about a draw beyond the GfxDraw: where its vertices came from. */
typedef struct GrassSrc
{
    uint64_t tex_hash;     /* stage 0's texture */
    uint32_t vb_id;        /* the vertex buffer (the game's COM pointer) */
    uint32_t first;        /* the vertex GfxDraw.cpu starts at */
    uint32_t nverts;       /* the vertices from there that the draw reaches */
    const void* indices;   /* the draw's indices, or NULL */
    uint32_t index_size;   /* 2 or 4 */
    uint32_t index_lo;     /* the smallest index: it names the first vertex of cpu */
    uint32_t prim, count;  /* D3D primitive type and primitive count */
} GrassSrc;

/* 1 while the log is asked for (grass.log) and the grass is on: d3d8.c counts the draws it can't hand over. */
int grass_logging(void);
/* A world draw of a texture the grass would have used, left out: reason 0 its vertices aren't from a static buffer, 1 it's
 * blended or writes no depth. */
void grass_reject(uint64_t tex_hash, int reason);
/* 1 while blended ground draws are ground too (GrassSettings.blended, the grass on): d3d8.c hands them over */
int grass_layers(void);

/* A world draw (opaque and depth-written, or with grass_layers a blended layer; static vertices) the game is making into the
 * scene's target. */
void grass_note(const GfxDraw* d, const GrassSrc* s);

/* The scene is finished (gfx_scene_done is next): the frame's grass is made and drawn into color/depth, the scene's
 * target and its depth, with the frame's camera and fog. */
void grass_scene(GfxTex* color, GfxTex* depth, const GfxScene* s);

#ifdef __cplusplus
}
#endif
