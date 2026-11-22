/* The graphics library's face to VanaCore.Core (graphics.cpp): Direct3D 8 for the game, implemented
 * on Direct3D 12 (d3d8.c over gfx_d3d12.c, ported from xi-on-mac), with its scene effects, the
 * interface kept at a fixed shape in a wider window, texture packs and the faster occlusion probe.
 *
 * Everything here runs on the game's thread (the one that makes the Direct3D device), except the
 * log sink, which any thread may call. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where the library's diagnostics go: one call per line, UTF-8, no newline. */
void vanacore_gfx_set_log(void (*sink)(const char* line));

/* 1 puts the game's Direct3D 8 on Direct3D 11 (gfx_d3d11.c, renderer=d3d11) where it would be on
 * Direct3D 12. Before vanacore_gfx_start. */
void vanacore_gfx_set_d3d11(int on);

/* Brings up the Direct3D 12 device (or Direct3D 11's: vanacore_gfx_set_d3d11) with no window yet, so a
 * machine that can't run it is known before the game is handed our Direct3D 8. 1 when it's up. */
int vanacore_gfx_start(void);

/* Direct3DCreate8 for the game: our IDirect3D8 (a COM pointer), or NULL when vanacore_gfx_start
 * failed. */
void* vanacore_gfx_direct3d_create(void);

/* 1 when Direct3D 11 draws the game: renderer=d3d11, or in Direct3D 12's place where Direct3D 12 came up but its
 * shaders don't compile (Wine's and Proton's d3dcompiler_47), or its device is below resource binding tier 2. After vanacore_gfx_start. */
int vanacore_gfx_d3d11(void);

/* FFXiMain.dll's address range, for FFXI_DRAWLOG's return addresses. */
void vanacore_gfx_set_image(uint32_t lo, uint32_t hi);

/* 1 waits for the display at Present (the default), 0 doesn't (tears where the display allows it). */
void vanacore_gfx_set_vsync(int on);

/* The game's frame rate cap ([client] fps.max as applied: 0 when it's unlocked), for frame generation's log (the frames
 * shown a second against the display's refresh rate). */
void vanacore_gfx_set_fps_max(int fps);

/* A scene-effect setting by its key (fx, ao, radius, grade, sat, contrast, brightness, shadows, highlights, sharpen, filter, aniso,
 * fog, fog_falloff, fog_height, fog_max, bloom, threshold, rays, rays_decay, rays_length, light, shadow,
 * sun, ..., debug: gfx_d3d12.c's FX_SETTINGS). NaN puts it back to its default. */
void vanacore_gfx_fx_set(const char* key, float value);
/* The scene effects' shaders compiled again from the end of this frame (Client > VanaCore > Recompile shaders): every one
 * let go and the build started over at the next scene, the notice showing it. 1 when the renderer took it, 0 without one. */
int vanacore_gfx_fx_rebuild(void);

/* Where the player stands, in the world as the game draws it (x, height, y), each frame; known 0 while
 * there's no player (the title screen, zoning). The shadows' sharp map reaches [graphics.effects] sun_near
 * past it. */
void vanacore_gfx_set_focus(float x, float y, float z, int known);
/* VanaCore's caster pass (casters.cpp): how far round the player the zone's pieces are wanted in the sun's maps whatever the camera
 * sees (0: not now - no shadows, Direct3D 11), and in *stretch (when wanted) how far a piece's shadow reaches along the ground for
 * each unit of its height, a tall piece wanted from that much farther; and, while the game's own renderer draws one of them for it,
 * the piece (nonzero: the game's own, its address) - each draw kept for those maps alone, by that piece, and none drawn into the
 * scene - 0 after */
float vanacore_gfx_seek_reach(float* stretch);
/* The shadows' record (/vana stats, core/stats_shadows.hpp): every frame's shadow state into the CSV file at `path` (UTF-8, as fopen
 * takes it), at most max_mb MB (0: 40), until a call with a null path closes it. 1 when it started: Direct3D 12 alone */
int vanacore_gfx_sun_record(const char* path, unsigned max_mb);
void vanacore_gfx_seek(uint32_t piece);
/* The game begins drawing its zone (1) and has drawn it (0): FFXiMain's zone draw, hooked by core/zonebatch.cpp. Between them,
 * with [graphics.effects] batch on, Direct3D 12 sends the zone's draws of one mesh together (gfx_batch) */
void vanacore_gfx_batch(int open);
/* ... 1 while [graphics.effects] batch is on and the renderer can (Direct3D 12): the hook goes in the first time */
int vanacore_gfx_batch_wanted(void);
/* The occlusion test ([graphics] occl, core/occlusion.cpp): the zone's first slot's pieces as boxes in the world (count * 6
 * floats: low x, height, y, then high; low x over high for a piece with none), a new set each zone (its gen goes up); on or off,
 * and the margin round each box on the screen (thousandths of the scene's width). Each scene (Direct3D 12) tests each box against the depth just
 * drawn; the result comes back frames later, one byte a piece (0 off the screen or not tested, 128 seen, 255 hidden), NULL until
 * there's one for the boxes given last. *seq goes up with each; *tested and *hidden count its pieces on the screen and of them
 * hidden. Game thread. */
void vanacore_gfx_occl_boxes(const float* boxes, unsigned count);
void vanacore_gfx_occl_set(int on, float margin);
const unsigned char* vanacore_gfx_occl_result(unsigned* gen, unsigned* seq, unsigned* tested, unsigned* hidden);
unsigned vanacore_gfx_occl_gen(void);
/* The entities near the player, each frame: count of them (up to 256), 4 floats each - where it stands in the world as
 * the game draws it (x, height, y), then a character's (a player, NPC or monster) hitbox size, or -1 for a fixture (a home
 * point, a waypoint, a streetlamp). The glow effects lighting takes an effect drawn on a character (a spell, an ability)
 * at [graphics.effects] elight_actor's share (GfxDraw.el_lantern 3); one on a fixture, nearer it than any character, as
 * any other. */
void vanacore_gfx_set_characters(const float* xyzr, unsigned count);

/* The zone's lines, drawn by the effects (Direct3D 12 and 11) as glowing curtains: count boxes (up to 64), each
 * 8 floats - its centre in the world as the game draws it (x, height, y), its turn about the up axis (radians),
 * its full width, height and depth, and how far above its middle the ground is under it (0: at the middle) - and
 * color: rgb, then how strongly (0: none drawn); style 0 a curtain, 1 a line floating across it, 2 that line in
 * dots, 3 sparkles drifting up it, 4 dots on the floor inside it; glow 1 a halo round them; height the line's
 * above the ground, where the sparkles start (world units); size the lines', dots' and sparkles' size, times their own; fade how far
 * (world units) their ends fade out toward where they stop, the box's ends or the walls (0: none). Kept until
 * changed. */
void vanacore_gfx_set_zonelines(const float* boxes, unsigned count, const float color[4], unsigned style, int glow,
                                float height, float size, float fade);

/* Sparkles rising out of places on the ground, and auras round characters, drawn by the effects (Direct3D 12 and 11) with
 * the zone lines: count places (up to 16), each 12 floats - its centre on the ground in the world as the game draws it (x,
 * height, y), the radius of the disc they rise from (an aura's ring), their colour (rgb, 0 to 1), how high they rise (world
 * units), its style (0 sparkles, 1 an aura round a character standing there), its size and amount (times their own) and
 * strength - then the sparkles' size and density (how many to a square unit of the disc), times their places' own, and
 * speed, how fast they rise and come and go, times their own; match 1: each sparkle the colour of the scene where it was
 * born, the place's colour where that isn't seen. Kept until changed. */
void vanacore_gfx_set_sparkles(const float* places, unsigned count, float size, float density, float speed, int match);

/* Rings of light on the ground (addons' vana.world.marks), drawn by the effects (Direct3D 12 and 11) with the sparkles: count
 * rings (up to 16), each 16 floats - its centre on the ground in the world as the game draws it (x, height, y), its radius
 * (world units), its colour (rgb), how strongly (0: not drawn), its shape (0 a circle, 1 a square of that half-width), the
 * height of a column of light rising from it (0: none), the glyph over that (0 none, 1 to 8: A, B, C, D, 1, 2, 3, 4) and
 * the glyph's size (half its height, world units), how high over the ring the glyph's centre floats (0: just over the
 * column's top) and three spare. Each is laid onto the ground the scene's depth
 * shows, so a slope carries it and what stands in front of it (a character, the monster it's under) hides it; the column is
 * cut the same way by what stands in front of it. Kept until changed; none without the scene effects. */
void vanacore_gfx_set_ground_marks(const float* marks, unsigned count);

/* The glow round trusts and avatars ([graphics] glow): 0 off, 64-4096 the game's own with its small picture that
 * size (256 as the game has it), -1 VanaCore's own. */
void vanacore_gfx_set_glow(int mode);

/* The light on the characters - players, NPCs, monsters, what they wear - times k ([graphics] characters.brightness): their
 * ambient and their lights' ambient and diffuse, so they come up out of the dark without the zone around them. 1 is the game's.
 * Below 1 their finished colour is dimmed instead (the light taken down before the shader's clamps let the game's light stepping
 * show on white clothing). */
void vanacore_gfx_set_character_light(float k);

/* 1 while the player is in their own Mog House (the zone-in packet 0x00A's byte 0x80: 1 there, 2 elsewhere), each frame: the
 * room's ceiling stands between the sun and everything in it, so the sun's shadows there are scaled by [graphics.effects]
 * moghouse, as in a zone lit by a fixed light. */
void vanacore_gfx_set_moghouse(int in);
/* 1 while the player's zone is lit by a fixed light alone, as its model DAT says (every environment, its 0x2F sections, with
 * bit 0 set: no sky, as in Palborough Mines), each frame: the sun's shadows there are scaled by [graphics.effects] moghouse,
 * as in a Mog House, without waiting to see whether the zone's light moves with the clock. 0 leaves that to the light. */
void vanacore_gfx_set_fixed_light(int on);
/* 1 while a home point's teleport is under way (the destination picked, the zone not yet left), each frame: its effect's
 * bright pillars round the character light everything near, so the glow effects lighting is toned down to [graphics.effects]
 * elight_warp times its strength meanwhile (GfxScene.warping). */
void vanacore_gfx_set_warping(int on);
/* The Vana'diel minute of the day (0 to 1440, the server's clock), each frame: the sun's shadows come in through the hour
 * before the sun is up, when the game's light is the moon's going out (sun_dusk, GfxScene.vana_minute). 0: not known. */
void vanacore_gfx_set_vana_minute(float minute);
/* 1 leaves the game's own character shadow (Config > Shadows: the character projected onto the ground from a target of its
 * own) out, draw by draw: while VanaCore's sun shadows are on and characters cast them ([graphics] gameshadows=auto), so it
 * never shows for a moment before the game's option is held at Off again. 0 (the default) leaves it as the game draws it. */
void vanacore_gfx_set_hide_game_shadows(int on);
/* /fillmode: the world's draws (the zone, the characters, the effects: every 3D draw of the game's) with this D3DFILLMODE in
 * place of the game's: 1 point (Direct3D 11 and 12 have no point fill, so a triangle draw goes as its vertices, a point
 * list, a pixel each), 2 wireframe. 0 (the default) leaves the game's own. The interface's draws and VanaCore's own stay as
 * they are. */
void vanacore_gfx_set_fill_mode(int mode);
/* [graphics] untextured: 1 draws the world's opaque surfaces plain white where the game puts a texture on them, so only the
 * lighting and the vertex colours show (for looking at the light and the shadows without the pattern under them). Cut-outs
 * (leaves, fences) and blended draws (effects, water) keep their textures so their shapes stay; the interface and VanaCore's
 * own draws stay as they are. 0 (the default) leaves the game's. */
void vanacore_gfx_set_untextured(int on);

/* The grass (VanaCore.ini [graphics] grass*): tufts on the zone's grass-textured ground near the player. enabled; textures the hex hashes of the ground textures that are grass (""
 * any mostly green one); radius how far from the player; density tufts to a square unit of ground; height how tall,
 * variation (0 to 1) how much that differs tuft to tuft, width how wide; brightness against the ground's colour; flowers (0 to 1)
 * the share of tufts with one, in patches, flower_brightness theirs, flowers_only 1 leaves the tufts out; limit the most tufts
 * in a frame; slope the steepest ground (degrees) it grows on; log 1 writes what it finds to the log and the grass textures to
 * logs\grass. The grass casts no shadow. */
typedef struct VanaGrass
{
    int enabled;
    const char* textures;
    float radius, density, height, width, variation, brightness, flowers, flower_brightness;
    int flowers_only, log;
    unsigned limit;
    float slope; /* the steepest ground it grows on, degrees */
    /* the rings (rings_set 1; 0 grass.c's defaults): rings 1-3 start at ring[i] of the radius (0-1) and keep ring_amount[i] of the
     * tufts (0-1); inside ring 1 all of them */
    int rings_set;
    float ring[3], ring_amount[3];
    int show_rings; /* 1: the rings drawn round the player as coloured circles */
    int edges; /* 1: the tufts near the brink of a drop (a ledge, a rock's top) grow smaller, none hanging out over it */
    float match; /* 0 to 1: how far the assets' sprites are coloured like the ground under them */
    int dry; /* 1: the yellow-olive of dry grass counts as grass ground, not only green */
    int walkable; /* 1: tufts only on ground the zone's collision says the player could stand on (vanacore_gfx_set_grass_floors) */
    float pathpad; /* yalms, 0 off: no tuft grows with a path's ground this near it */
    float build_ms; /* ms a frame spent making squares of tufts, the rest waiting for the next frame; 0 the default (1.5), under 0 no limit */
    int overgrown; /* 1: dry grass ground grows on stone and metal floors too (the Nyzul portal's meadow) */
    int nowater;   /* 1: no tufts on floors the game's footsteps call shallow water, walkable or not */
    int moss;      /* 1: moss in their place there (with nowater) */
    float wind_speed, wind_turbulence; /* 0 to 4, 1 the usual: how fast the grass sways, and how much the sway differs across the field */
    int show_cover; /* 1: tufts covered by a floor over them drawn red (left out otherwise), those with no floor under them to tell yellow */
    float dirt; /* 1 to 100 (0: 50, the usual): how colourful ground has to be to grow grass (higher, more of it bare as dirt), the zone's */
    int shade;     /* 1: in grass ground, texels in shadow and dry grass's yellow-olive grow too, not only the green ones */
    float dry_sat, dry_hue; /* the dry grass test's least mean saturation (0: 0.25) and reddest hue, degrees (0: 46) */
    int blended; /* 1: grass-textured ground drawn blended is ground too, a layer over other ground (the zone's, or grass.blended) */
    const char* ground; /* hex hashes of textures that are grass ground in this zone whatever their colours say (the zone's) */
} VanaGrass;
void vanacore_gfx_set_grass(const VanaGrass* settings);
/* The zone the player is in, each frame (0 while there's none). */
void vanacore_gfx_set_grass_zone(int zone);
/* A zone's floors, from its collision (grass_floors.cpp, zonefloor::read_floors): each mesh's triangles (9 floats each, in its own
 * space) with their ground (the game's footsteps: 0 object, 1 path, 2 grass, 3 sand, 4 snow, 5 stone, 6 metal, 7 wood, 8 shallow
 * water, 9 deep), mesh m's from mesh_first[m] up to mesh_first[m + 1]; the placements' matrices (12 floats: where x, y and z go,
 * then the origin); pairs (2 each: a matrix and a mesh it places). zone 0: none known; a zone with no pairs: it has none. */
typedef struct VanaFloors
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
} VanaFloors;
/* The zone's floors for VanaGrass.walkable: the triangles that aren't walls. Copied. */
void vanacore_gfx_set_grass_floors(const VanaFloors* floors);
/* The zone's whole collision for /vana debug's collision view ([graphics] collision.show; grass_floors::update_collision): as
 * vanacore_gfx_set_grass_floors, its walls too, with 0x10 on their terrain. Drawn over the world each frame while it's held (floors.c):
 * walls red, floors by their ground. zone 0, or none: let go, nothing drawn. */
void vanacore_gfx_set_collision(const VanaFloors* collision);
/* 1 while the collision view is wanted ([graphics] collision.show); the collision itself is handed over for the rain's shelter too */
void vanacore_gfx_show_collision(int on);
/* The zone's own meshes for the sun's shadows ([graphics.effects] sun_zone_mesh; core/zone_casters.cpp, zonemesh::read): every
 * mesh where the zone's DAT places it, laid in the world (the game's: y counts down) as triangle lists. pos: the solid ones, 3
 * floats a vertex; cut: those cut out by their texture's alpha, 5 floats a vertex (x, y, z, u, v). Each range a part of them
 * standing together: `count` vertices from `first` in pos (texture -1) or cut (textures[texture]), its world box. The textures'
 * pixels 0xAARRGGBB, rows top first, alpha as the DAT has it (0x80 solid). Copied to the GPU (Direct3D 12) and given to the
 * sun's maps in place of the caster pass while they're held. zone 0, or none: let go. */
typedef struct VanaZoneMeshRange
{
    unsigned first, count;
    int texture;
    float lo[3], hi[3];
} VanaZoneMeshRange;
typedef struct VanaZoneMeshTexture
{
    const unsigned* argb;
    unsigned width, height;
} VanaZoneMeshTexture;
typedef struct VanaZoneMesh
{
    int zone;
    const float* pos;
    unsigned npos;
    const float* cut;
    unsigned ncut;
    const VanaZoneMeshRange* ranges;
    unsigned nranges;
    const VanaZoneMeshTexture* textures;
    unsigned ntextures;
    const float* nrm; /* 4 floats for each of pos's vertices, or NULL: its normal in the world, 1 + the bulge's height there long
                       * (world units), and how far the surface there is curved and smoothly shaded, 0 to 1 (core/zonemesh.cpp
                       * curve_of): the sun's shading of curved rock */
} VanaZoneMesh;
void vanacore_gfx_set_zone_mesh(const VanaZoneMesh* mesh);
/* Bit 1 while the reflections want the zone's floors too (Direct3D 12, [graphics.effects] refl and refl_floors on): they keep to
 * the kinds of ground the collision has under each pixel (floors.c), so the floors are read for them as for the grass. */
int vanacore_gfx_floors_wanted(void);
/* The snow trails (VanaCore.ini [graphics] snow*): footprints pressed into ground the zone's collision calls snow (its terrain 4),
 * where players, NPCs and monsters walk, as shading over the ground the game draws (snow.c). enabled; depth how deep (yalms: how
 * steep the walls look); strength (0 to 2) how dark; fade the seconds a footprint takes to fill in (0: never). It needs the zone's
 * floors, as the grass's walkable does (vanacore_gfx_set_grass_floors). */
typedef struct VanaSnow
{
    int enabled;
    float depth, strength, fade;
    float sink; /* yalms a character standing in snow is lowered into it, 0 off */
    float soft; /* 0 to 1: how soft the prints' edges are */
} VanaSnow;
void vanacore_gfx_set_snow(const VanaSnow* settings);
/* VanaCore's sky (VanaCore.ini [graphics] sky, sky.log): drawn in place of the game's dome (sky.c; Direct3D 12). enabled; log 1: a line
 * as it takes the dome or leaves it */
typedef struct VanaSky
{
    int enabled;
    int log;
} VanaSky;
void vanacore_gfx_set_sky(const VanaSky* settings);
/* The light on the grass, each frame: its brightness times k (0.05 to 1; 1 as set), for the time of day
 * ([graphics] grass.brightness.auto); with grass.match only the share the match leaves (none at 1: the ground's own light dims the
 * tufts then), the flowers all of it. Nothing is made again for it. */
void vanacore_gfx_set_grass_light(float k);
/* Where the grass grows round in place of the player, each frame: the free camera's eye with [graphics] grass.freecam (x, height,
 * y as the game draws the world); known 0, round the player. It grows round the ground under that place. */
void vanacore_gfx_set_grass_centre(float x, float y, float z, int known);
/* Mass Grass's catalogue (grass.h grass_asset): asset index 2-63 a picture (0xAARRGGBB rows) with n sprites (x0 y0 x1 y1, pixels) and
 * its height next to grass.height; assets 0 and 1 are the drawn blades, type 1 and type 2. _weight: an asset's share of the tufts
 * (0 none). */
void vanacore_gfx_grass_asset(unsigned index, const uint32_t* argb, unsigned w, unsigned h, const float* rects, unsigned n, float height);
void vanacore_gfx_grass_weight(unsigned index, float weight);
/* 1: asset `index` is flowers (the catalogue's kind "flower"): Match ground colour leaves them alone */
void vanacore_gfx_grass_flower(unsigned index, int flower);
/* Capturing the plant sheets drawn near the player (grass.h): start; then the count (-1 while capturing, 0 none) and each sheet's
 * hash, picture and sprites (rects: room for 32 x 4 floats); clear when done; a picture written as a PNG. */
void vanacore_gfx_grass_capture(void);
int vanacore_gfx_grass_capture_count(void);
int vanacore_gfx_grass_capture_result(unsigned i, unsigned long long* hash, unsigned* w, unsigned* h, const uint32_t** argb, float* rects,
                                      unsigned* n);
void vanacore_gfx_grass_capture_clear(void);
int vanacore_gfx_write_png(const wchar_t* path, const uint32_t* argb, unsigned w, unsigned h);

/* VanaCore's water (VanaCore.ini [graphics] water*), in place of the game's layered water on the zone's flat water tiles
 * (Bastok Markets so far). enabled 0 leaves the game's; waves how steep (1 as made, 0 flat); speed how fast they travel;
 * size how long they are; reflection how much of the sky shows in it; glint the sun's and moon's; brightness the water's own
 * colour; colour from 0 blue through 0.5 teal to 1 green, or tint (0xRRGGBB, has_tint 1) the water's own colour as it shows in the
 * shallows (the deep is a darker one of it); shallowness how much of the lighter, shallow colour shows; turbulence how much
 * the small ripples add to the swell (1 as made); direction the waves' way, in degrees; glint_size how wide the glint is.
 * detect what counts as the water: 0 the flat tiles found by their shape (as before), 1 the game's water models
 * (vanacore_gfx_water_loading), 2 either. view 1 is the debug view: the water found drawn in colour, the water left as the
 * game draws it meanwhile. */
/* [graphics] rain* (graphics_rain.h): the rain on the characters - the wet look (wet, sheen), the drops seen landing (drops), how
 * long they take to dry (dry, seconds), and force (as if it rained, for looking at it) */
typedef struct VanaRain
{
    int wet;
    float sheen, drops, dry;
    int force;
    int rain;      /* VanaCore's own rain: streaks, splashes, the wet ground (rain) */
    float density, splash;
    int shelter, thunder;
    int game;      /* the game's own rain kept under it (rain.game) */
    float lens;    /* droplets on the lens (rain.lens) */
    float spatter; /* the size of the spatter off the characters (rain.spatter) */
    int lens_on;   /* droplets on the lens at all (rain.lens.on) */
    int ground;    /* the wet ground and the puddles (rain.ground) */
    int freeze;    /* the rain held still (rain.freeze) */
    float range;   /* how far round the camera the drops splash (rain.range) */
    float reflect; /* the wet ground's reflections of the scene, Direct3D 12's floor reflections (rain.reflect, 0 none) */
    float puddles; /* the puddles' share of them: low ground and corners (rain.puddles, 0 none) */
    float reach;   /* how far round the camera the rain falls at all (rain.far) */
    int view;      /* the rain red over the picture in grey (Debug view 31) */
} VanaRain;
void vanacore_gfx_set_rain(const VanaRain* settings);
/* the weather (0x057's id) each frame, and whether a packet has given one yet */
void vanacore_gfx_set_weather(unsigned weather, int known);
/* the weathers the zone's model DAT has (zoneweather: bit n for 0x057's weather n, rain and squall only where their folder
 * carries rain; bit 31 set once read, 0 not known), each frame: VanaCore's rain, in the server's rain as in thunder weather
 * (rain.thunder), falls only where the zone has rain or squall of its own */
void vanacore_gfx_set_zone_weathers(uint32_t mask);

typedef struct VanaWater
{
    int enabled;
    float waves, speed, size, reflection, glint, brightness, colour, shallowness;
    float turbulence, direction, glint_size;
    int has_tint;
    unsigned tint;
    int detect;
    int view;
    /* the water pass: drawn after the world over copies of the scene (1) or in its place, opaque (0); how far into it one
     * sees (yalms), the waves' refraction, the shore's width (yalms) and its foam, the fine ripples' strength and size */
    int pass;
    float clarity, refraction, shore, foam, ripples, ripple_size;
    int log;   /* water.log: the sheets looked at go to the log too */
    int probe; /* [graphics] water.probe 1 to 3: the pass's water shows what it reads from the scene (gfx_hlsl_water.h) */
    int horizon; /* water.horizon: the sea drawn on past the zone's edge to the horizon */
    float falls; /* water.falls: how strongly a waterfall churns and foams the water it lands in, 0 not at all */
    float caustics; /* water.caustics: the rippling light on the bed in shallow water */
    float relief;   /* water.relief: the wavelets lit as slopes */
    int shadows;    /* water.shadows: the sun's shadows fall on VanaCore's water (0, the default: none) */
    int sea;        /* water.sea: VanaCore's own sea plane in place of the game's sea tiles (1, the default) */
} VanaWater;
void vanacore_gfx_set_water(const VanaWater* settings);
/* The zones (the game's zone ids) VanaCore's water is never drawn in, the game's own left there as with water off; copied, and
 * the zone the player is in taken again with them. NULL or 0 none. */
void vanacore_gfx_set_water_zones_off(const uint16_t* zones, unsigned n);
/* The game's water models (from XI on Anything): 1 while the calling thread loads a map model under one of a zone DAT's
 * water directories, 0 when it's done; the vertex buffers the thread makes meanwhile are that water's. Any thread (it sets
 * the caller's own flag). Returns what it was, to put back after. */
int vanacore_gfx_water_loading(int on);
/* How many vertex buffers have been made while a water model loaded, so far (for the log). */
unsigned vanacore_gfx_water_buffers(void);

/* The interface's window backgrounds (the 128x128 window texture's quads, 64x24 pixels or more) drawn with their alpha times
 * `all` (0..1), except the ones inside one of the chat windows' rectangles (set with vanacore_gfx_set_chat_rects: left, top,
 * right, bottom each, in the interface's pixels), which take `chat` in its place. 1 and 1 leave them as the game draws them. */
void vanacore_gfx_set_window_opacity(float all, float chat);
/* The same for the windows' frames (the 32x32 frame texture's pieces at a background's edge): the chat windows' own. */
void vanacore_gfx_set_border_opacity(float all, float chat);
/* A flat colour (0xRRGGBB) for the windows' frames (the frame texture's shape kept, its colour left out): `all` for every
 * window, `chat` for the chat windows' (over the other); off leaves the game's own. */
void vanacore_gfx_set_border_colours(int all_on, uint32_t all_rgb, int chat_on, uint32_t chat_rgb);
/* A colour (0xRRGGBB) for the window backgrounds drawn solid, as the game draws them with its window texture's pattern
 * left out: `all` for every window, `chat` for the chat windows' (over the other); off leaves the game's own. */
void vanacore_gfx_set_window_colours(int all_on, uint32_t all_rgb, int chat_on, uint32_t chat_rgb);
/* A picture (0xAARRGGBB, w x h, rows top down; copied) drawn in place of the window backgrounds, with their opacity; NULL
 * gives the game's back. How each window shows it (`fit`): 0 filled (cropped to the window's shape), 1 stretched, 2 tiled at
 * its own pixels, 3 one picture over the whole interface that every window shows its part of. The chat windows (`chat`) and
 * the chat line (`input`) take it too when set. */
void vanacore_gfx_set_window_image(const uint32_t* argb, unsigned w, unsigned h);
void vanacore_gfx_set_window_image_look(int fit, int chat, int input);
void vanacore_gfx_set_chat_rects(const float* rects, unsigned count);
/* The chat line's window (left, top, right, bottom in the interface's pixels; NULL while it's closed), which takes these
 * opacities (0..1) and colours (0xRRGGBB; off leaves the game's) in place of the chat windows' and every window's. */
void vanacore_gfx_set_input_rect(const float* rect);
void vanacore_gfx_set_input_look(float alpha, float border_alpha, int tint_on, uint32_t tint_rgb, int border_on, uint32_t border_rgb);
/* The windows' frames drawn as a solid line along their edges in their frame colour (the frame texture left out): every
 * window's, the chat windows', the chat line's. */
void vanacore_gfx_set_border_solid(int all, int chat, int input);
/* The windows' corners (on those with a solid frame or none): `shape` 0 square (the game's), 1 cut across, 2 rounded;
 * `size` how far from each corner, in the interface's pixels (1 to 32). */
void vanacore_gfx_set_window_corners(int shape, float size);
/* The corners on the windows now: 0 square (off, or on no window: they need a solid frame or none), 1 cut, 2 round, their
 * size in the interface's pixels in `size`. VanaCore's pictures drawn round a window (the lock-on glow) take their shape. */
int vanacore_gfx_window_corners(float* size);
/* Each quad drawn until this is called again with NULL is drawn as its part inside window `rect`'s corners (left, top,
 * right, bottom in the interface's pixels, as the game's vertices lie), the shape taken as far out round the window as the
 * quad reaches past its edge: a picture drawn over a window kept to its shape (the Target window's lock-on picture).
 * Nothing changes while the corners are square. */
void vanacore_gfx_clip_to_corners(const float* rect);
/* Around the game's drawing of one of its windows (1 before, 0 after; nested: chat.cpp's window draw hook): with the corners
 * cut or round on it, what it draws inside its background after the background (its text, list rows, item squares) is kept
 * inside their shape; what crosses its edge (a title's tab) is left as it is. The chat's rows go by the chat window they're in. */
void vanacore_gfx_window_drawing(int on);
/* The chat's text (the glyphs inside the chat windows' rectangles) drawn over a shadow of itself: mode 1 a pixel down and to
 * the right, 2 an outline all round, 0 none; `opacity` (0..1) how dark it is. */
void vanacore_gfx_set_chat_shadow(int mode, float opacity);
/* [client] chat.font=sharp: VanaCore's glyph for one character of the game's font sheet: its code (the cell, row * 64 + column
 * of the sheet's 64 x 128; ASCII is the byte less 0x20), its picture's place in the atlas (texels), where the picture goes in
 * the screen's pixels (x from the pen, y from the baseline: its top-left corner's), and what the game's whole-pixel width for
 * it left off its advance (the interface's pixels). w or h 0: nothing to draw (a space). Its outline (chat.font.outline) the
 * same: oax, oay, ow, oh in the atlas, ox, oy from the pen and the baseline; ow or oh 0, none. */
typedef struct VanaChatGlyph
{
    uint16_t code, ax, ay, w, h, pad;
    float x, y;
    float frac;
    uint16_t oax, oay, ow, oh;
    float ox, oy;
    float adv, lx; /* the font's advance (screen pixels), and x before any centring in the game's width (x less lx: that) */
} VanaChatGlyph;
/* A letter of the menus' labels: the game's buttons and titles ("Status", "Equipment") are sprites of the 256 x 256 "font
 * font" sheet, one per letter, drawn a texel to a pixel: the letter's code (ASCII less 0x20), its sprite's place and size in
 * the sheet (texels), its baseline below the sprite's top and its capitals' height (texels: 9 the labels', 19 the name
 * plates' big letters on the same sheet, which the Status window and the clock draw at about half). `sheet`: 0 the "font
 * font" sheet's, 1 the "menu menu2fon" sheet's (256 x 256 too: the bold slanted letters in the windows' pictures, the chat
 * line's Say / Shout / Linkshell tab, "Target" and "Help"), told apart by the sprite's place and size alone; 2 the "menu
 * keytop" sheet's (the macro bar's keys' numbers), taken only while a window of VANA_FONT_MACROBAR's is drawn (that sheet's
 * other sprites are the macros' names, which aren't labels). */
typedef struct VanaLabelGlyph
{
    uint8_t code, u, v, w, h, base, cap, sheet;
} VanaLabelGlyph;
/* The menus' labels drawn from VANA_FONT_BUTTONS' font (vanacore_gfx_set_chat_font) where a sprite is one of these letters'
 * and a button is being drawn (vanacore_gfx_button_drawing; VANA_FONT_MAINMENU's in the main menu's windows,
 * VANA_FONT_ACTIONMENU's in the Action Menu's), VANA_FONT_TITLES'
 * where none is; the big letters (cap above 9) from VANA_FONT_STATUS' */
void vanacore_gfx_set_label_glyphs(const VanaLabelGlyph* list, uint32_t count);
/* Around the game's drawing of a menu button's shape (1 before, 0 after; nested: buttons.cpp's shape hooks): the labels'
 * letters drawn meanwhile are a button's (VANA_FONT_BUTTONS), the others a title's (VANA_FONT_TITLES) */
void vanacore_gfx_button_drawing(int on);
/* Around the game's drawing of the clock under the compass (1 before, 0 after; chat.cpp's hook of KaCompass::DrawTime): the
 * labels' and big letters drawn meanwhile are the clock's (VANA_FONT_CLOCK: its digits and its ":", "(" and ")" one font) */
void vanacore_gfx_clock_drawing(int on);
/* Around the game's drawing of letters that are no label's (1 before, 0 after; nested: chatfont.cpp's hook of the macro menus'
 * name printer, while it leaves the names to the game): no sprite drawn meanwhile is taken for a label's letter (those names
 * are sprites of the "menu keytop" sheet, whose R, N, U and M stand where the "menu2fon" sheet's 6, 3, 8 and 2 do) */
void vanacore_gfx_game_letters(int on);
/* The macro bar's keys' labels (VANA_FONT_MACROBAR): `color` their True Font letters' colour (0xFFRRGGBB; 0 the game's:
 * [client] macrobar.font.color), `prefix` 0 to leave out each key's "Ctrl" / "Alt", its number alone left where it is, the
 * game's letters' or True Font's ([client] macrobar.prefix; 1 as the game has them) */
void vanacore_gfx_set_macrobar(uint32_t color, int prefix);
/* A main menu label the game squeezes across to fit its button ("Communication", Macros' "Edit Equip. Set"; VANA_FONT_MAINMENU)
 * in True Font: `on` 1 at the font's full size, past its button as a long label in a larger font runs; 0 its letters narrowed
 * as the game's are, to 85% of the font's width at most ([client] mainmenu.font.fixed; 1 unset) */
void vanacore_gfx_label_fixed(int on);
/* Area `which`'s letters left out altogether (`on` 1 or 2; 0 drawn again: the game's or True Font's): neither the game's nor
 * True Font's drawn. 2: every letter of the area (its True Font size 0; the lists' numbers' areas, their numbers). 1:
 * VANA_FONT_EQUIP's alone ([client] equip.font=hide): the Equipment window's slot names' letters (the labels' letters, and
 * their small digits, "Ear1"), not the stats' numbers nor their signs */
void vanacore_gfx_hide_letters(int which, int on);
/* Around the game's drawing of a line of text that scrolls across its box (1 before, 0 after; nested: chatfont.cpp's hook of the
 * help line's message draw while it scrolls): its letters are laid each at the game's own place, not a word at a time from the
 * word's first letter (as a letter scrolled off, the rest of its word jumped to the next one's place: reported 2026-11-06) */
void vanacore_gfx_text_rolling(int on);
/* Around the game's drawing of a text it centres on `x` (`on` 1 before, 0 after: chatfont.cpp's hook of the game's text draw
 * 0x101C7F10 while its last argument is set: the status icons' timers, each centred under its icon; the interface's pixels):
 * its letters, the labels' sheet's sprites it draws one at a time, are one word, laid centred on x, not from the game's left
 * (a font narrower than the game's sat left of its icon's middle: reported 2026-11-07) */
void vanacore_gfx_text_centred(int on, float x);
/* Around the game's drawing of a text it right-aligns (`on` 1 before, 0 after: chatfont.cpp's hook of its right-aligned text draw
 * 0x101C82B0: the skill panels' levels, "(26)", the Status window's "1234/", merits' "[3]"): its letters, big ones and the labels'
 * smaller ones it draws last first, are one text in one font, laid by the font's own advances to end where the game's does (laid
 * as words by the big letters' rules, the Craft Skills' "(7)" came out "( )7": reported 2026-11-08). Only while every letter of it
 * is the titles' or Status & Numbers'; a text of another area's (the clock, the lists, a trade's) keeps its own rules. */
void vanacore_gfx_text_right(int on);
/* Around VanaCore's own drawing of a text through the game's font sheet printer (`on` 1 before with its `text`, 0 after:
 * equipstats.cpp's stats, each label and each number): its letters are one text in their area's font, laid by the font's own
 * advances with the font's space where `text` has one, from the game's left or (`right` 1) ending where the game's ends. Not a
 * word at a time by the game's gaps, which split "Magic" into "M agic" and "Ma gic" (reported 2026-11-08). A text whose letters
 * aren't all drawn (one covered) goes by the game's gaps for its spaces. */
void vanacore_gfx_text_whole(int on, int right, const char* text);
/* A font's glyphs: `atlas` is w x h premultiplied white (0xAARRGGBB, every channel the ink); `baseline` is where the glyphs
 * stand below a cell's top, in the interface's pixels; `outline` (0..1) how opaque their outlines are, black; `line` the rows'
 * spacing to the game's (1 as it draws them; the tooltips' only: tips.font.lineheight / 16); `size` the font's (its em, the
 * interface's pixels); `pad` how far its rows are moved down (the tooltips' only: tips.font.padding.top); `upper` 1 when its
 * lower-case letters' glyphs are their capitals (<area>.uppercase: a big letters' word of them isn't squeezed into the game's). */
typedef struct VanaTextFont
{
    const uint32_t* atlas;
    uint32_t w, h;
    const VanaChatGlyph* glyphs;
    uint32_t count;
    float baseline, outline;
    float line;
    float size, pad;
    uint32_t upper;
} VanaTextFont;
/* The interface's text areas, each its own font ([client] chat.font, menu.font, buttons.font, titles.font, items.font,
 * help.font, status.font, input.font, party.font, alliance.font, phrases.font, trade.font, gil.font, profile.font, profile.title.font, mainmenu.font, equip.font,
 * macrobar.font, buffs.font, action.font, target.font, target.title.font, tips.font): the chat rows (around
 * vanacore_gfx_chat_drawing); the font sheet's text in the windows, by the window it's drawn in (vanacore_gfx_text_area): the
 * menus and lists, the item lists, the help, the chat input line, the auto-translate list, an item's details (the tooltips); the big letters and labels' letters in the party list and the
 * alliance lists, by their window too; the menus' labels, a button's and the rest (the windows' titles), and the name plates' big letters (the Status
 * window's numbers and words, the clock: all vanacore_gfx_set_label_glyphs); a tooltip's parts (VANA_TIP_*) each at its own size, VANA_FONT_TIPS_TITLE
 * + the part (tips.font.title.size and the rest: the tooltips' font otherwise, its letters centred in the game's widths made
 * larger as much as the font is) */
enum
{
    VANA_FONT_CHAT,
    VANA_FONT_MENUS,
    VANA_FONT_BUTTONS,
    VANA_FONT_TITLES,
    VANA_FONT_ITEMS,
    VANA_FONT_HELP,
    VANA_FONT_STATUS,
    VANA_FONT_CLOCK, /* the clock under the compass: its letters (vanacore_gfx_clock_drawing), the big ones and the labels' */
    VANA_FONT_INPUT, /* the chat input line's window ("menu    inline"): the menus' until input.font is set */
    VANA_FONT_PARTY, /* the party list's window ("menu    partywin"): its names and numbers, the name plates' big letters and the
                      * labels' (else the Status area's and the titles'); status.font's keys until party.font's are set */
    VANA_FONT_ALLIANCE, /* the alliance lists' windows ("menu    raid1", "raid2"): the same, alliance.font */
    VANA_FONT_PHRASES,  /* the auto-translate list's window ("menu    fep", Tab while typing): its rows; the menus' until
                         * phrases.font is set */
    VANA_FONT_TRADE,    /* the trade windows' ("menu    trade", "delivery", "stringdl", "post1", "post2": a trade's, the
                         * delivery box's): every letter in them, their font sheet text, their buttons' and labels' letters and
                         * their amounts (the name plates' big letters) */
    VANA_FONT_GIL,      /* Current Gil's window ("menu    money", beside Equipment, Items, a trade, the delivery box): the
                         * same, gil.font (it was the trade windows' until 2026-11-06: Trade Windows sized it everywhere) */
    VANA_FONT_PROFILE,  /* the Status screen's Profile panel ("menu    profile": Nation, Race, Home Point, Rank, Mastery Rank,
                         * Monstrosity): the same, profile.font (asked for 2026-11-08: its labels were the titles' and its
                         * values the menus', two sizes on one row) */
    VANA_FONT_PROFILE_TITLE, /* its two titles' letters ("Profile", "Monstrosity" on its title bars), switched to around each of
                              * their quads' draws (profilewin.cpp TitleQuad): profile.title.font, profile.font's keys until set */
    VANA_FONT_MAINMENU, /* the main menu's windows ("menu    menuwind", "socialme"): its buttons' labels (Status, Equipment),
                         * apart from every other menu's buttons; buttons.font's keys until mainmenu.font's are set */
    VANA_FONT_EQUIP,    /* the Equipment window's ("menu    equip   ") slot names: the labels' letters in it (laid as the
                         * titles' are), equip.font's keys alone; drawn into the interface under an item's icon where the game
                         * draws them under it (d3d8_chatfont.h cf_under); or left out (vanacore_gfx_hide_letters) */
    VANA_FONT_EQUIP_STATS, /* the Equipment window's stats: its big letters (the element numbers, Attack and Defense) and font
                            * sheet text (the stats under it, equipstats.cpp), their signs the labels' letters taken in with them,
                            * as the trade windows' are; equip.stats.font's keys alone (one area with the slot names until
                            * 2026-11-06, split as asked) */
    VANA_FONT_MACROBAR, /* the macro bar's windows ("menu    mcr1pall", "mcr2pall", "mcr1long", "mcr2long"): its keys' labels,
                         * "Ctrl" / "Alt" and the number (the "menu2fon" sheet's letters, the "keytop" sheet's digits: sheet 2,
                         * taken in these windows alone), macrobar.font's keys alone; the macros' names stay the menus' */
    VANA_FONT_BUFFS,    /* the status icons' window ("menu    buff    "): their timers ("29m", "3h"), which the game centres under
                         * their icons (vanacore_gfx_text_centred); the menus' until buffs.font is set (they were the
                         * menus' until 2026-11-07, reported: a player wanted them apart) */
    VANA_FONT_ACTIONMENU, /* the Action Menu's windows ("menu    playermo", Monstrosity's "mp_pmode", the Magic and Abilities
                           * lists it opens beside it, "magselec", "abiselec", "mp_abisc"): its buttons' labels (Attack, Magic),
                           * as VANA_FONT_MAINMENU's are the main menu's; buttons.font's keys until action.font's are set */
    VANA_FONT_TARGET,       /* the Target window's ("menu    targetwi"): the target's name, the name plates' big letters the
                             * game draws at about half (target.font, status.font's keys until set: they were the Status area's
                             * until 2026-11-07, asked for apart), and any font sheet text in it */
    VANA_FONT_TARGET_TITLE, /* its labels' letters: the "Target" title (target.title.font, titles.font's keys until set, and
                             * those buttons.font's; the lock picture's "Locked" stays the titles': targetwin.cpp lock_hook) */
    VANA_FONT_CHATTABS,     /* the tabs over the chat log open to scroll (Window, Say, Tell...): never the graphics layer's
                             * letters, VanaCore draws each tab's name whole in it (chattabs.cpp; chattabs.font, titles.font's
                             * keys until set) */
    VANA_FONT_STATUS_NAME,  /* the Status window's player name (status=1): never the graphics layer's letters, VanaCore draws the
                             * name whole in it (statuswindow_name.h; status.name.font, its own keys alone) */
    VANA_FONT_LISTS,        /* Menu Lists: the font sheet text of the windows given it (VANA_TEXT_LISTS: the Merit Points menu's,
                             * "menu    merit1" ... "meritcat", "meritinf"), their buttons and labels still their window's area's;
                             * lists.font, menu.font's keys until its own are set (asked for apart from General 2026-11-08) */
    VANA_FONT_MACRO_NAMES,  /* the macro bar's macros' names ("DLSS5", the book's name), never the graphics layer's letters,
                             * VanaCore draws each name whole in it (macrotext.cpp; macrobar.name.font, menu.font's keys until
                             * set: they were the menus' until 2026-11-13, when the key labels' font got a name's beside it) */
    VANA_FONT_TIPS,
    VANA_FONT_TIPS_TITLE,
    VANA_FONT_TIPS_TYPE,
    VANA_FONT_TIPS_STATS,
    VANA_FONT_TIPS_JOBS,
    VANA_FONT_TIPS_SIGNATURE,
    VANA_FONT_TIPS_TIMER,
    VANA_FONT_PARTY_NUMBERS,    /* the party list's numbers (HP, MP, TP) at their own size (party.font.numbers.size), the party
                                 * list's font otherwise; on only while that size is set and isn't the names' */
    VANA_FONT_ALLIANCE_NUMBERS, /* the alliance lists' (alliance.font.numbers.size). These two last: d3d8_chatfont.h cf_worded */
    VANA_FONTS
};
/* An item's details (the tooltips) in parts, each found by its rows' text (d3d8_tips.h): the top row, the item's name; the
 * row after it when it starts with '[' or '(' ("[Body]All Races"), its type; the job list (the row starting "Lv.", "Lv.57
 * WAR/MNK/...", and the rows of job names after it); the last row when it's a name in brackets ("[Voltiz]", right-aligned:
 * its crafter's signature); a row in angle brackets ("<1/1 0:30/[20:00:00, 0:30]>", right-aligned: a usable item's charges
 * and timers); the rest, its stats and description */
enum
{
    VANA_TIP_TITLE,
    VANA_TIP_TYPE,
    VANA_TIP_STATS,
    VANA_TIP_JOBS,
    VANA_TIP_SIGNATURE,
    VANA_TIP_TIMER,
    VANA_TIP_PARTS
};
/* A tooltip part's look besides its size: its letters' colour (0xFFRRGGBB; 0 the game's), the interface's pixels of space
 * above it (the title's none: tips.font.padding.top), its numbers' colour (digits, the + or - just before one, a % just after
 * one, a . or , between two; 0 the part's), how see-through its letters are (0 as the game's, 1 not drawn: tips.font.opacity
 * and tips.font.<part>.opacity together; their outlines as much) */
typedef struct VanaTipPart
{
    uint32_t color;
    float space;
    uint32_t numbers;
    float fade;
    uint32_t upper; /* its letters in capitals, laid as wide as the game lays capitals (tips.font.<part>.uppercase) */
} VanaTipPart;
/* The tooltips' parts' look (VANA_TIP_PARTS of them; copied). With VANA_FONT_TIPS drawn by True Font only. */
void vanacore_gfx_set_tip_parts(const VanaTipPart* parts);
/* The interface's pixels the tooltips last laid onto the screen needed beyond their rows' spacing (their parts' sizes over the
 * rows' and the space above each), the most of them; -1 when the last laying had none (the box is made that much taller:
 * chatfont.cpp's SetLines hook) */
float vanacore_gfx_tips_extra(void);
/* How wide (the interface's pixels) the box at left, top, right, bottom needs to be for the tooltip laid last in it: its widest
 * row as its parts' sizes lay it, from the box's left, and a margin. No wider than the box when its rows grew no wider than
 * the game's, nor than the box more than they grew; 0 with no tooltip laid in it (chatfont.cpp's SetLines hook makes it so) */
float vanacore_gfx_tips_width(float left, float top, float right, float bottom);
/* [client] tips.rarity.*: an item's details in its rarity's colour (items.rarity.color1-4): `name` its name with True Font's
 * letters over tips.font.title.color (the game's letters are coloured where the game prints the name: itemrarity.cpp), and
 * that colour laid over the top of their box, `strength` (0..1, 0 none) at its top and fading out `height` of the
 * interface's pixels lower, under its frame, its icon and its text */
void vanacore_gfx_set_tip_rarity(int name, float strength, float height);
/* Around the game's drawing of an item's details window (1 before, 0 after: chat.cpp's window draw): its item's rarity colour
 * (0xRRGGBB); `has` 0 for an item without one */
void vanacore_gfx_tip_item(int drawing, int has, uint32_t rgb);
/* Around the game's print of an item's details' type row's text (1 before, 0 after: tipicons.cpp, [client] tips.type): its
 * letters kept now are the type's, whatever they say */
void vanacore_gfx_tip_type(int drawing);
/* Around the game's print of an item's details' name when the item is worn and shown in its equipped colour (1 before, 0
 * after: itemrarity.cpp, [client] items.equipped not off): its letters keep that colour, the game's, over the rarity's and the
 * title part's own */
void vanacore_gfx_tip_equipped(int on);
/* The interface's text drawn from these glyphs, at the screen's own pixels, over the interface once it's laid onto the screen
 * (the characters with none stay the game's), every glyph's outline first, then the letters: `which` the area (VANA_FONT_*).
 * Copied; NULL goes back to the game's. */
void vanacore_gfx_set_chat_font(int which, const VanaTextFont* font);
/* The font sheet's cells past ASCII (kana, kanji, the European letters' row) and the Japanese labels' words drawn in area
 * `which` since last asked whose font had no glyph for them, up to `most` into `codes` (the rest kept for the next ask): what
 * to build into it next. A glyph's code is a font sheet cell's (row * 64 + column) below VANA_LABEL_WORDS, from there a
 * Japanese menu label's words (jplabels.cpp's label index on from it), below VANA_GLYPH_CODES. */
#define VANA_LABEL_WORDS 8192
#define VANA_GLYPH_CODES 10240
uint32_t vanacore_gfx_wanted_glyphs(int which, uint16_t* codes, uint32_t most);
/* Around the game's drawing of a quad of a label sheet of a game in Japanese (jplabels.cpp): what it is. `what` 0 none (after
 * it), 1 a picture to leave the game's (not a label known, and never an English label's letter), 2 a label's first part
 * (`code` its words' glyph, VANA_LABEL_WORDS on; `wide` the whole label's width to this part's), 3 a later part of it (left
 * out while its first part's words are True Font's); `shadow` 1 for the label's shadow copy, 0 its letters. */
void vanacore_gfx_label_word(int what, int shadow, uint32_t code, float wide);
/* The area (VANA_FONT_MENUS, _ITEMS, _HELP, _INPUT, _PHRASES or _TIPS) of the font sheet's text drawn from now on, or VANA_FONT_PARTY /
 * _ALLIANCE of the big letters and labels' letters: around a window's draw. | VANA_TEXT_LISTS: the window's font sheet text
 * is Menu Lists' (VANA_FONT_LISTS), its buttons and labels still the area's (the Merit Points menu's: the main menu's).
 * Gives the one it was (with its VANA_TEXT_LISTS), to put back after. */
#define VANA_TEXT_LISTS 0x100
int vanacore_gfx_text_area(int area);
/* Around the game's drawing of chat rows (1 before, 0 after; nested): their glyphs are the ones vanacore_gfx_set_chat_font takes
 * and chat.shadow shadows, wherever they are (the chat windows', the chat log maximized's). */
void vanacore_gfx_chat_drawing(int on);
/* The screen's pixels per interface pixel, across and down, where the chat windows' interface was last laid onto the screen
 * (the glyphs' size); 0 until that has been seen. */
int vanacore_gfx_chat_scale(float* sx, float* sy);
/* The game's own word on where it draws its interface (its menu target at gfx+0x1e0, uiscale.cpp): `texture` the render-target
 * texture (the COM pointer the game holds), 0 for none (its interface goes straight onto the back buffer); `known` 0 when the
 * game's display can't be read (the target is then found from the chat's rows and the game's letters). Each frame. */
void vanacore_gfx_set_interface_target(uint32_t texture, int known);
/* The game's letter printer is about to draw (on 1), or has drawn (on 0), letter `code` of the font sheet (its cell, row * 64 +
 * column) at pen x, y (the interface's pixels), sx x sy times 16 pixels (chatfont.cpp's hook of FFXiMain's 0x1011B7D0): the draw
 * between the two is a letter, that one, whatever its shape. */
void vanacore_gfx_letter_draw(int on, int code, int x, int y, float sx, float sy);
/* The chat input line as one picture (chatline.cpp): its visible text laid out whole in the input font (DirectWrite: the
 * letters where the font puts them, between whole pixels, kerned) and a caret of VanaCore's, in place of a picture per letter
 * laid where the game's stand. `pixels` is w x h premultiplied white: the letters' picture from its top-left (`fill_w` x
 * `fill_h`; its pen starts `pen_x` in, its baseline `base_y` down), their outline's from row `out_y` (`out_w` x `out_h`, 0 none;
 * `out_pen_x`, `out_base_y`), and a white 3 x 3 in its top-right corner (the caret's ink). `serial` changes when the pixels do.
 * The line's pen (`x`, `y`: the cell's top-left of its first letter as the game prints it, the interface's pixels); the caret
 * `caret` screen pixels on from the pen (< 0: none), lit this frame or not (`caret_lit`: it blinks), `caret_top` to
 * `caret_bottom` from the baseline (the screen's pixels, up negative), `caret_w` wide, `caret_pad` more each way under it in
 * the outline's pass. A line whose caret would pass its window's right edge is moved left until it doesn't. */
typedef struct VanaLinePicture
{
    const uint32_t* pixels;
    uint32_t w, h, serial, caret_lit;
    uint32_t fill_w, fill_h, out_y, out_w, out_h;
    float pen_x, base_y, out_pen_x, out_base_y;
    float x, y;
    float caret, caret_top, caret_bottom, caret_w, caret_pad;
} VanaLinePicture;
/* Around the game's draw of the chat input line's text (1 before, 0 after: chatline.cpp's hook of FsTextInputDisplay; a text
 * box's, the macro editor's line, by its twin): the font sheet letters drawn between are the line's, kept and not drawn one by
 * one (the ones the input font has no letter for, ñ and the rest, too), for the picture vanacore_gfx_set_input_line gives
 * after; without one they're laid where the game's are. One line a frame: only one is typed into at a time */
void vanacore_gfx_input_line(int on);
/* This frame's picture of the chat line (copied when its serial is new), after its draw; NULL none (its letters then each at
 * the game's place) */
void vanacore_gfx_set_input_line(const VanaLinePicture* line);
/* Around one of the game's windows drawn into nothing (1 before, 0 after; nested): every draw between the two is dropped, the
 * game's state calls kept (gamewin.cpp: a window an addon hides by its layout's name, still drawn so that what it draws is
 * read) */
void vanacore_gfx_drop_draws(int on);
/* [client] truefont.debug: True Font's steps counted and the first of each kind of draw described (rt_log) */
void vanacore_gfx_chat_font_debug(int on);
/* True Font's state and the counts since the last report, one line into `text` (size bytes); the counts start again */
void vanacore_gfx_chat_font_report(char* text, unsigned size);
/* Windows left bare: their background and frame pieces (as above) drawn with no alpha, `count` (up to 2) rectangles of left,
 * top, right, bottom in the interface's pixels. 0 leaves every window as it is. */
void vanacore_gfx_set_hidden_windows(const float* rects, unsigned count);

/* The interface's shape (width / height) in a wider window; 0 is off. */
void vanacore_gfx_set_ui_aspect(float aspect);
/* The size the window was made at, whose shape is the window's for the interface (what Windows took of it for the
 * title bar and borders left out); 0, 0 is its client area's (see user32.h). */
void vanacore_gfx_set_window_size(uint32_t w, uint32_t h);

/* The client x to give the game for the cursor at client (x, y) of `window` (see user32.h). */
int vanacore_gfx_unsqueeze_x(void* window, int x, int y, int held);
/* Both axes: a window taller than the interface's shape squeezes it up and down */
void vanacore_gfx_unsqueeze(void* window, int* x, int* y, int held);

/* Adds a texture pack folder (<hash>_<w>x<h>.dds replacements). Before the device is created. */
void vanacore_gfx_texture_pack(const char* folder);

/* The size the frame is shown at (the window's client area); 0x0 before the device exists. */
void vanacore_gfx_screen_size(uint32_t* w, uint32_t* h);
/* The size the world's 3D scene's whole target was last shown at: the names over heads drawn into it are drawn at that
 * size with DLSS or the names_native effect setting (Direct3D 12). 0 (w and h 0) before there's been one. */
int vanacore_gfx_scene_shown(uint32_t* w, uint32_t* h);
/* The world's camera as this frame's scene tracked it (vana.world): the view and projection matrices (16 floats each, as
 * Direct3D 8 has them) and the viewport its 3D draws used, in the target's pixels (X, Y, width, height; then MinZ and MaxZ as
 * float bits): not the game's pixels, so take a point's share of it, not its pixel. 0 until the
 * scene has one, and with the renderers that don't track it (d3d9). */
int vanacore_gfx_camera(float* view, float* proj, uint32_t* viewport);
/* The character whose name over its head the game draws next (nameplates.cpp, around the game's name and the line under
 * it): its draws carry `id` (GfxDraw.label; 0 after, none), so frame generation can find each name again the next frame
 * and draw it where it would be in the frames it makes (Direct3D 12, [graphics] nameplates.framegen). */
void vanacore_gfx_label(uint32_t id);

/* 0 around VanaCore's own drawing (its overlay, addons, plugins): those screen-space draws aren't
 * squeezed for [ui] aspect. 1 (the default) for the game's. */
void vanacore_gfx_set_squeeze(int on);

/* window: 1 draws the frame at the window's size (the back buffer, its depth buffer, the menu target;
 * the game still sees its own sizes), 0 at the game's. scene: the 3D scene at that multiple of the
 * drawn size (DLSS's share), 0 at vanacore_gfx_set_background's size. Applied at the next
 * Present, so it can change while the game runs. */
void vanacore_gfx_set_resolution(int window, float scene);
/* [graphics] background: the 3D scene drawn at w x h (each side at most 8192) while DLSS doesn't size it; 0 x 0 at
 * the game's own background resolution. Applied at the next Present. */
void vanacore_gfx_set_background(uint32_t w, uint32_t h);
/* the 3D scene's size as it's drawn now; 0 x 0 before the game has drawn one */
void vanacore_gfx_scene_size(uint32_t* w, uint32_t* h);

/* NVIDIA DLSS ([graphics] dlss; Direct3D 12): quality -1 off, else an NVSDK_NGX_PerfQuality_Value (0 performance,
 * 1 balanced, 2 quality, 3 ultra performance, 5 DLAA); helper VanaCore.Dlss.exe's path, log its log file. The
 * scene's size is set apart (vanacore_gfx_set_resolution's scene: the quality's share of the drawn size). */
void vanacore_gfx_set_dlss(int quality, const wchar_t* helper, const wchar_t* log);
/* 0 off, 1 starting, 2 on, 3 not available here, 4 failed; the helper's message into message */
int vanacore_gfx_dlss_state(char* message, int size);
/* DLSS frame generation ([graphics] dlss.framegen; with DLSS on): count frames made between two, 0 off, 1 (2x) to 3
 * (4x); its state as vanacore_gfx_dlss_state's (1 waiting for DLSS), what it's doing or why not into message */
void vanacore_gfx_set_framegen(int count);
int vanacore_gfx_framegen_state(char* message, int size);
/* Which frame generation vanacore_gfx_set_framegen's count asks for, with either upscaler (on its depth and motion):
 * VANA_GFX_FG_DLSS (NVIDIA's, with DLSS: [graphics] dlss.framegen) or VANA_GFX_FG_FSR (AMD's, one frame between two:
 * [graphics] fsr.framegen, or dlss.framegen=fsr; FSR FG 4 on a Radeon RX 9000 under Windows 11, else FSR FG 3.1) */
#define VANA_GFX_FG_DLSS 0
#define VANA_GFX_FG_FSR 1
void vanacore_gfx_set_framegen_kind(int kind);
/* Whether DLSS ([graphics] dlss) can run on this card, known before it's turned on: 1 on Direct3D 12 with an NVIDIA RTX 20
 * or later (Turing's RTX cards, PCI device ids 0x1E00 to 0x1F7F but for the GTX 1660 SUPER and GTX 1650 among them, and
 * from 0x2200). Else 0, and why into why; DLSS asked for there stays off. */
int vanacore_gfx_dlss_supported(char* why, int size);
/* Whether DLSS 5 ([graphics] dlss5) can run here, known before it's turned on: 1 on Direct3D 12 with an NVIDIA RTX 40 or
 * later (Ada, Blackwell: PCI device ids from 0x2680) and NVIDIA's nvngx_dlssnr.dll beside the DLSS helper - and once the
 * helper has started, NVIDIA's NGX giving it. Else 0, and why into why. */
int vanacore_gfx_dlss5_supported(char* why, int size);
/* The upscaler vanacore_gfx_set_dlss's quality turns on: VANA_GFX_UPSCALER_DLSS (NVIDIA DLSS, [graphics] dlss) or
 * VANA_GFX_UPSCALER_FSR (AMD FSR, [graphics] fsr: FSR 4 where AMD's DLL runs it, a Radeon RX 7000 or 9000; FSR 3.1 on any
 * other Direct3D 12 card), through the same helper (VanaCore.Dlss.exe, started again for the other one when it changes).
 * FSR runs on Direct3D 11 too (gfx_d3d11.c, without frame generation); DLSS on Direct3D 12 only.
 * FSR's own: sharpness, its sharpening, 0 (off) to 1; version, the FSR version asked for, 0 FSR's own choice or 2, 3 or
 * 4 (that major version, where AMD's DLL has it for the card), or 1: FSR 1, VanaCore's own (EASU and RCAS in this process,
 * no helper), which also stands in for AMD's FSR wherever that doesn't run. DLSS 5 is DLSS's alone; frame generation, see
 * vanacore_gfx_set_framegen_kind. */
#define VANA_GFX_UPSCALER_DLSS 0
#define VANA_GFX_UPSCALER_FSR 1
void vanacore_gfx_set_upscaler(int upscaler, float sharpness, int version);
/* Whether FSR ([graphics] fsr) can run here, known before it's turned on: 1 on Direct3D 12 or 11 (FSR 1 where AMD's FSR
 * doesn't run); without FSR 1's passes, only with AMD's FSR DLLs beside the helper - and once the helper has started for
 * FSR, AMD's FSR having an upscaler for the card. Else 0, and why into why. */
int vanacore_gfx_fsr_supported(char* why, int size);

/* What the picture is still waiting on as of the last frames, VANA_GFX_WAIT_* bits (gfx.h's GFX_WAIT_*), 0 for nothing:
 * VanaCore's notice while it's made (overlay.cpp) */
#define VANA_GFX_WAIT_DLSS 1u     /* DLSS starting: the scene drawn small and stretched meanwhile */
#define VANA_GFX_WAIT_DLSS5 2u    /* ... and DLSS 5 with it */
#define VANA_GFX_WAIT_FRAMEGEN 4u /* frame generation starting */
#define VANA_GFX_WAIT_EFFECTS 8u  /* the scene effects' own shaders compiling */
#define VANA_GFX_WAIT_SHADOWS 16u /* many of the sun's shadow casters left out while their pipelines build */
unsigned vanacore_gfx_waiting(void);
/* How far one of those bits has come: *done of *total steps (shaders compiled of all of them; shadow casters ready of
 * those drawn). 1 when it has a count, 0 when it doesn't (or hasn't started) */
int vanacore_gfx_wait_progress(unsigned bit, unsigned* done, unsigned* total);
/* What one of those bits is at now, in words for the notice's line under its title ("Ambient occlusion": the shader
 * compiling), into words: 1 when it has them, 0 (words "") when it doesn't */
int vanacore_gfx_wait_step(unsigned bit, char* words, int size);

/* 1 around VanaCore's own interface: its draws go into a layer of the window's size, in the window's
 * pixels (vanacore_gfx_screen_size), laid over the frame at Present: sharp and square whatever size the
 * frame is drawn at. 0 (the default) after. */
void vanacore_gfx_set_own_pixels(int on);

/* 1 when the game's interface, laid out w x h (its menu resolution), has been squeezed toward the middle this frame
 * (vanacore_gfx_set_ui_aspect's shape in a window of another): text VanaCore places over the game's windows (the item
 * count, the Network window's dot) follows the squeeze then, and only then. The game's interface isn't squeezed when
 * its menu target ([ffxi] resolution.ui) is of another shape than the aspect's. 0 again at Present. */
int vanacore_gfx_ui_squeezed(uint32_t w, uint32_t h);

/* Any thread. 1 when (x, y), in the pixels plugins draw in (the game's), is over Ashita's Minimap, drawn in the last half
 * second: a wheel turn there is the plugin's (it zooms its map), not the camera's. */
int vanacore_gfx_plugin_wheel_over(int x, int y);

/* Around a plugin's drawing (its Direct3D or frame callbacks), `slot` its own (0..31; -1 after, and while VanaCore draws a font
 * of its by hand): its screen-space draws onto the back buffer go x to ax + scale x and y to ay + scale y (Addons > Layout's
 * move of its "Text and shapes", the HUD Manager's Addon View), and the box of where it put them is noted. */
void vanacore_gfx_set_mover(int slot, float ax, float ay, float scale);
/* That slot's box in the last finished frame, as the plugin put it (unmoved): x0, y0, x1, y1 in the game's pixels. 0 when it
 * drew nothing on the screen then. */
int vanacore_gfx_mover_box(int slot, float* box);

/* That layer as drawn this frame (BGRA, premultiplied alpha) into pixels with pitch, for a screenshot
 * with VanaCore's drawing (NULL pixels: only its size). 0 when nothing was drawn into it. */
int vanacore_gfx_ui_layer(uint32_t* w, uint32_t* h, void* pixels, uint32_t pitch);

/* Game thread. Rectangles of the game's picture kept over everything drawn after them (the auto-translate list and the
 * windows over fancychat's chat, over addons' drawing): each take copies one from the back buffer (x, y, w, h in the
 * game's pixels; at the game's scene end, before addons draw there; 8 a frame at most), put lays them back after
 * VanaCore's own drawing and cuts their places out of the layer that drawing is in. Put does nothing without a take
 * this frame. */
void vanacore_gfx_raise_take(int x, int y, int w, int h);
void vanacore_gfx_raise_put(void);

/* In the full-screen modes, the size the frame is drawn at with [graphics] resolution=game ([ffxi] resolution.game's,
 * which changes while the game runs: the frame stretched over the screen); 0 x 0 for the game's own resolution. */
void vanacore_gfx_set_render_size(uint32_t w, uint32_t h);
/* [graphics] drawlog's path, live (d3d8.c cap_present): copied by the renderer; NULL or "" turns the capture off. */
void vanacore_gfx_set_drawlog(const char* path);
/* [graphics.effects] elight_lantern_tex, live: the hashes of the textures a lantern's flame is drawn with (drawlog's "hash"),
 * comma or space separated; "" none (d3d8.c d3d8_set_lantern_textures). */
void vanacore_gfx_set_lantern_textures(const char* list);
/* ... elight_dim_tex: an effect's that gives far more light than it looks to, taken at elight_dim's share
 * (d3d8.c d3d8_set_dim_textures). */
void vanacore_gfx_set_dim_textures(const char* list);
/* ... dlss5_skip_tex: an effect's DLSS 5 isn't to take as a light (dlss5_fx_light), Al'Taieu's portal pools
 * (d3d8.c d3d8_set_dlss5_skip_textures). */
void vanacore_gfx_set_dlss5_skip_textures(const char* list);

/* The graphics card Direct3D 12 runs on ("" before vanacore_gfx_start, or without it). */
const char* vanacore_gfx_adapter(void);
/* This process's video memory in use and Windows' budget for it, in bytes (DXGI's: any card). 1 when known. */
int vanacore_gfx_video_memory(unsigned long long* used, unsigned long long* budget);

/* Direct3D 12's device lost (a driver timeout, a bad address, the driver restarting): why, and with [logging] gpu
 * (FFXI_D3D12_DRED=1) the pass and operation the graphics card stopped at, each line to out - once, whoever finds it
 * first (Present and the fence waits log it themselves; crash.cpp asks as the game crashes). 1 if it's lost. */
int vanacore_gfx_gpu_lost(void (*out)(const char* line));
/* 1 once the device (Direct3D 12's or 11's) has been lost, the loss logged already: nothing drawn after it shows, ever.
 * Cheap: a flag, each frame. */
int vanacore_gfx_device_lost(void);
/* The device lost: made again under the game, which never hears of it (graphics::close_if_lost, at Present on the game's
 * thread). The front end lets go of every GPU object it holds, the back end is torn down and brought up again on the
 * game's window (tried for up to 15 s while the driver comes back), and the front end makes its objects again, each
 * uploaded from guest memory at its next draw. 1 when the game can draw again on Direct3D 12; 2 when Direct3D 12's device
 * couldn't be made again (the old one still held somewhere, or none to be had) and Direct3D 11 draws the game from here, as
 * with renderer=d3d11, without what is Direct3D 12's alone (DLSS, frame generation); 0 when neither could be brought up:
 * nothing draws any more, and the game closes as before. The settings go up again after (apply_live). */
int vanacore_gfx_device_recover(void);

/* The frame profile frame by frame (VanaCore's /vana debug): on (1) for a recording, whether or not [graphics]
 * profile turned it on at the start, and back (0) as it was. Direct3D 12's; nothing on Direct3D 11. */
void vanacore_gfx_record(int on);

/* The last frame the profile saw, in ms: the game's Direct3D calls (encode: drawing them), Present (waiting for the
 * GPU, the swap chain), waits on the DLSS helper; GPU time (a frame or two behind: its timestamps are read back);
 * its draws, upload KB and pipelines made. 0 when there's none (the profile off, Direct3D 11). */
typedef struct VanaGfxFrame
{
    float d3d_ms, encode_ms, present_ms, gpu_wait_ms, swap_ms, helper_wait_ms, gpu_ms;
    uint32_t draws, upload_kb, pipelines, skipped;
    float grass_ms;       /* of d3d_ms: making squares of grass tufts (grass.c, in the scene's end) */
    uint32_t grass_tufts; /* ... the tufts and flowers it made */
    uint32_t redrawn;     /* the moving things drawn again for DLSS's or FSR's motion and masks: their GPU time is the game's draws' */
} VanaGfxFrame;
int vanacore_gfx_frame_stats(VanaGfxFrame* out);

/* The GPU's time by what it was doing (VanaCore's /vana stats; Direct3D 12's profile), in ms a frame, named by
 * vanacore_gfx_scope_name: the game's scene, the sun's shadow maps, each scene effect, DLSS, and the interface drawn
 * after them. The average of the frames since the last call (which starts the next); 0 when there are none (the
 * profile off, Direct3D 11). */
#define VANA_GPU_SCOPES 13
#define VANA_GPU_SCOPE_DLSS 10 /* the scope DLSS and its neural rendering run in */
#define VANA_GPU_SCOPE_COMPOSITE 6
/* Each scope by part (vanacore_gfx_part_name(scope, part)): its passes, the GPU idle between command lists, and part 0
 * the rest of it (what isn't one of its passes) */
#define VANA_GPU_PARTS 45
#define VANA_GPU_PART_WAIT_DLSS 34 /* DLSS's: waiting on the helper's DLSS (the helper's own timing is in that) */
#define VANA_GPU_PART_COMP 19      /* the composite's shader, which vanacore_gfx_measure breaks down by feature */
/* The composite's shader by feature (vanacore_gfx_measure): what each costs, its draw with everything less its draw with
 * that one off, ms a frame, < 0 till it's been measured (feature_off: it's off, so it costs nothing). Feature 0 is the
 * whole draw as measured, VANA_GPU_FEATURES - 1 the draw with all of them off (reading the scene and depth, writing it).
 * Named by vanacore_gfx_feature_name, whose parent is the feature it's a part of (-1: none). */
#define VANA_GPU_FEATURES 10
typedef struct VanaGfxScopes
{
    float ms[VANA_GPU_SCOPES];
    uint32_t frames;
    float part_ms[VANA_GPU_SCOPES][VANA_GPU_PARTS];
    float feature_ms[VANA_GPU_FEATURES];
    uint8_t feature_off[VANA_GPU_FEATURES];
    int measuring; /* the composite's being measured (all < 0 when it isn't) */
    uint32_t full_frames; /* ... of the frames, those whose parts' timestamps ran out: their parts are lumped together */
} VanaGfxScopes;
int vanacore_gfx_scopes(VanaGfxScopes* out);
const char* vanacore_gfx_part_name(int scope, int part);
const char* vanacore_gfx_feature_name(int feature, int* parent);
/* The composite measured by feature for the next half second (call it each frame while it's wanted): its shader drawn
 * again once a frame, into a target of its own, with everything or one feature off in turn. That draw is GPU time of its
 * own (the last scope's), not the game's. */
void vanacore_gfx_measure(void);

/* What DLSS's jobs take of the GPU, in ms, from timestamps the DLSS helper writes round them on its own queue (the
 * game's can't see them: its queue only waits): DLSS 5 (neural rendering), DLSS's upscale, and one frame made by frame
 * generation. 0 until the helper has timed some (or there is no DLSS). */
typedef struct VanaGfxDlssTimes
{
    float nr_ms, sr_ms, fg_ms;
} VanaGfxDlssTimes;
int vanacore_gfx_dlss_times(VanaGfxDlssTimes* out);
/* How many times the DLSS helper (DLSS's, FSR's, frame generation's; Direct3D 12) has been lost: it crashed, ended or
 * stopped answering, and what it ran was turned off. The last time's words into why ("DLSS and frame generation stopped:
 * the helper crashed (0xC0000005, an access violation)") */
int vanacore_gfx_helper_lost(char* why, int size);
/* The DLSS helper made to fail at its next frame, as kind says (1 an access violation, 2 one on another thread, 3 an
 * unhandled C++ exception, 4 a fail-fast, 5 a hang: dlss_ipc.h's VANA_CRASH_TEST_*), to try what a real crash leaves. 1
 * asked; 0 with no helper running (Direct3D 12, with DLSS or FSR on), why into why */
int vanacore_gfx_dlss_crash_test(int kind, char* why, int size);
const char* vanacore_gfx_scope_name(int i);

#ifdef __cplusplus
}
#endif
