/* Direct3D 8 for 64-bit hosts (d3d8.c): the D3D8 front end the Metal renderer goes under. */
#pragma once
#include <stdint.h>

#include "grass.h"

/* Registers the shims (before the images are mapped). */
void d3d8_init(void);
/* Builds the COM vtables in guest memory (once the guest heap is up). */
void d3d8_setup(void);
/* Called at every Present, on the game's thread with the guest lock held, before the frame goes
 * out: where the host adjusts per-frame game state (host64: the frame-rate divisor). */
void d3d8_set_present_hook(void (*fn)(void));
/* The graphics device lost (a driver timeout) and made again under the game, which never hears of it
 * (vanacore_gfx_device_recover). d3d8_device_lost lets go of every GPU object the front end holds: the
 * game's textures and static buffers (their pixels and vertices stay in guest memory), the texture packs'
 * replacements and normal maps, VanaCore's interface layer and raised copies, the grass's and the floors'.
 * Then the back end is torn down and brought up again (gfx_shutdown, gfx_init), and d3d8_device_restored
 * makes them again on the new device, each surface and static buffer uploaded again at its next draw.
 * d3d8_device_window and d3d8_device_vsync: what the game's CreateDevice gave gfx_init (NULL, 0 before it). */
void d3d8_device_lost(void);
void d3d8_device_restored(void);
void* d3d8_device_window(void);
int d3d8_device_vsync(void);
/* The size the frame is shown at: the device window's client area, else the back buffer's; 0x0
 * before the device exists. */
void d3d8_screen_size(uint32_t* w, uint32_t* h);
/* Adds a texture pack: <dir>/<hash>_<w>x<h>.dds replacements for the game's textures (see d3d8.c,
 * tools/make_texpack.py). Before the device is created. */
void d3d8_texture_pack(const char* dir);
#ifdef VANACORE
/* Direct3DCreate8 for a native process (native.c): the IDirect3D8's COM pointer, after d3d8_setup. */
uint32_t d3d8_create(void);
/* 0 while VanaCore draws its own overlay and its plugins draw theirs: those screen-space draws aren't
 * squeezed for [ui] aspect, so what's drawn is where the mouse says it is. 1 (the default) again after. */
void d3d8_set_squeeze(int on);
/* 1 when the game's interface, laid out w x h (the menu resolution), has been squeezed toward the middle this frame
 * ([ui] aspect): text VanaCore draws over the game's windows follows the squeeze then, and only then (the game's
 * interface isn't squeezed when its menu target is of another shape than the aspect's). 0 again at Present. */
int d3d8_ui_squeezed(uint32_t w, uint32_t h);
/* vanacore_gfx_set_mover and vanacore_gfx_mover_box */
void d3d8_set_mover(int slot, float ax, float ay, float scale);
int d3d8_mover_box(int slot, float* box);
/* where the player stands, in the world as the game draws it; known 0 when not (vanacore_gfx_set_focus) */
void d3d8_set_focus(float x, float y, float z, int known);
/* the characters and fixtures near the player, 4 floats each (vanacore_gfx_set_characters) */
void d3d8_set_characters(const float* xyzr, unsigned count);
/* the zone's lines, count boxes of 8 floats (vanacore_gfx_set_zonelines), their colour and strength, style, glow,
 * the line's height, their size and how far their ends fade */
void d3d8_set_zonelines(const float* boxes, uint32_t count, const float color[4], uint32_t style, int glow, float height,
                        float size, float fade);
/* the places sparkles rise from and characters' auras, count of GFX_SPARKLE_FLOATS floats (vanacore_gfx_set_sparkles), the
 * sparkles' size, density, speed, and match 1 for each the scene's colour where it was born */
void d3d8_set_sparkles(const float* places, uint32_t count, float size, float density, float speed, int match);
/* the rings of light on the ground, count of GFX_GROUND_MARK_FLOATS floats (vanacore_gfx_set_ground_marks) */
void d3d8_set_ground_marks(const float* marks, uint32_t count);
/* the trust and avatar glow ([graphics] glow): 0 off, 64-4096 the game's at that size, -1 VanaCore's own */
void d3d8_set_glow(int mode);
/* The size the world's 3D scene's whole target was last shown at (the drawn frame's), which the names drawn into it
 * after its effects are drawn over at with DLSS or names_native. 0 before there's been one. */
int d3d8_scene_shown(uint32_t* w, uint32_t* h);
/* the scene's camera (vanacore_gfx_camera) */
int d3d8_camera(float* view, float* proj, uint32_t* viewport);
/* the character whose name the game draws next, carried by its draws (GfxDraw.label); 0 none (vanacore_gfx_label) */
void d3d8_set_label(uint32_t id);
/* the light on the characters (players, NPCs, monsters) times k ([graphics] characters.brightness); 1 as the game has it */
void d3d8_set_character_light(float k);
/* the least ambient light on the characters, 0 to 1 ([graphics] characters.ambient); 0 off */
void d3d8_set_character_floor(float least);
/* 1 while the player is in their Mog House (vanacore_gfx_set_moghouse): the sun's shadows scaled by moghouse (GfxScene.indoors) */
void d3d8_set_moghouse(int in);
/* 1 in a zone its DAT says is lit by a fixed light alone (vanacore_gfx_set_fixed_light): the sun's shadows scaled by moghouse
 * there too (GfxScene.indoors) */
void d3d8_set_fixed_light(int on);
/* 1 while a home point's teleport is under way (vanacore_gfx_set_warping): the glow effects lighting toned down (GfxScene.warping) */
void d3d8_set_warping(int on);
/* The Vana'diel minute of the day, 0 to 1440 (vanacore_gfx_set_vana_minute): GfxScene.vana_minute */
void d3d8_set_vana_minute(float minute);
/* 1 leaves the game's own character shadow out draw by draw (vanacore_gfx_set_hide_game_shadows) */
void d3d8_set_hide_game_shadows(int on);
/* /fillmode: the world's draws with this D3DFILLMODE, 1 point or 2 wireframe; 0 (the default) the game's own
 * (vanacore_gfx_set_fill_mode) */
void d3d8_set_fill_mode(int mode);
/* [graphics] untextured: the world's opaque draws take a plain white texel for their base texture (stage 0), so only their
 * light and vertex colours show; cut-outs and blended draws keep theirs (vanacore_gfx_set_untextured) */
void d3d8_set_untextured(int on);
/* While VanaCore's caster pass has the game draw a piece of the zone whatever its camera sees (vanacore_gfx_seek), the piece
 * (nonzero), else 0: its draws go to the sun's maps alone (GfxDraw.sought, seek_note), never into the scene, each marked with the
 * piece and its place among the piece's draws (GfxDraw.piece, piece_draw) */
void d3d8_set_seek(uint32_t piece);
/* the grass ([graphics] grass*, grass.h): settings, then the zone the player is in (0: none) */
void d3d8_set_grass(const GrassSettings* settings);
void d3d8_set_grass_zone(int zone);
/* VanaCore's water ([graphics] water*, vanacore_gfx.h VanaWater) */
/* [graphics] rain* (d3d8_set_rain): the rain on the characters. wet 1 lays a wet look over players, NPCs and monsters while it
 * rains (0x057's rain and squall), 0 none; sheen (0 to 3) how much darker and glossier they go, with the sky in them; drops (0 to 3)
 * how strongly the drops are seen landing on them (rings spreading on what faces up, runnels down their sides); dry the seconds
 * they take to dry once the rain stops; force 1 is as if it rained, whatever the weather (for looking at it), 2 that and the
 * look's parts shown as colours (d3d8_rain.h) */
typedef struct RainSettings
{
    int wet;
    float sheen, drops, dry;
    int force;
    /* VanaCore's own rain (d3d8_rain.h rain_pass): rain 1 draws it - streaks that the world and the characters hide, splashes
     * where the drops land on what faces up (the ground, the shoulders) with a crown that rises off it, the ground's wet sheen;
     * density (0.2 to 3) how much falls, splash (0 to 3) how strong the splashes are, shelter 1 keeps it out from under roofs,
     * bridges and caves (the zone's collision cast down), thunder 1 rains in thunder weather too (a look alone) */
    int rain;
    float density, splash;
    int shelter, thunder;
    int game; /* 1 leaves the game's own rain drawn under VanaCore's; 0 drops it (its draws by their texture) */
    float lens; /* droplets on the lens while the rain reaches the camera (0 none to 3) */
    float spatter; /* how big the spatter the drops throw off the characters is (0 none to 3, 1 as made) */
    int lens_on;   /* the lens's droplets drawn at all (rain.lens.on): rain.lens kept for when they're back */
    int ground;    /* the wet ground and the puddles (rain.ground) */
    int freeze;    /* the rain held still, a picture (rain.freeze, /vana debug's Rain; this session only) */
    float range;   /* how far round the camera the drops splash, yalms (rain.range, Splash Range, 8 to 500) */
    float reflect; /* the wet ground mirrors the scene (rain.reflect, 0 to 2): Direct3D 12's floor reflections, by the rain's mask */
    float puddles; /* ... and its puddles, in the low ground and the corners, near mirrors (rain.puddles, 0 to 2) */
    float reach;   /* how far round the camera the rain falls at all, yalms (rain.far, Rain Range, 52 to 500) */
    int view;      /* 1 the rain red and the rest of the picture grey, 2 the rain's rings (32; Debug view 31, /vana debug's Rain; this session only) */
    float water;   /* the drops' rings on VanaCore's water (rain.water, 0 none to 3; d3d8_rain.h rain_water_bind) */
} RainSettings;
void d3d8_set_rain(const RainSettings* settings);
/* the zone's whole collision (vanacore_gfx_set_collision), for the rain's shelter map; NULL or zone 0: none */
void d3d8_set_cover(const GrassFloors* collision);
/* the zone's own meshes' solid triangles (in the world, vanacore_gfx_set_zone_mesh), roofs for the rain's shelter too; 0: none */
void d3d8_set_cover_mesh(int zone, const float* pos, uint32_t npos);
/* the weather the server last gave (0x057's id: 6 rain, 7 squall...), each frame; known 0 before any packet has */
void d3d8_set_weather(unsigned weather, int known);
/* the weathers the zone's model DAT has (bit n for 0x057's weather n, rain and squall only with rain in them; bit 31 known, 0
 * not known), each frame: VanaCore's rain falls only where the zone has rain or squall */
void d3d8_set_zone_weathers(uint32_t mask);
/* [graphics] weather* (d3d8_weather.h): particles in the air in the weathers other than rain. amount: the kinds, in order fire (hot
 * spell, heat wave), earth (dust and sand storms), wind (wind, gales), ice (snow, blizzards), light (auroras, stellar glare), dark
 * (gloom, darkness), 0 none to 3; preview a weather (0x057's id) shown whatever the server's, -1 none */
#define WX_KINDS 6
typedef struct WeatherSettings
{
    int on;
    float amount[WX_KINDS];
    int preview;
} WeatherSettings;
void d3d8_set_weather_fx(const WeatherSettings* settings);

typedef struct WaterSettings
{
    int enabled;
    float waves, speed, size, reflection, glint, brightness, colour, shallowness;
    float turbulence, direction, glint_size;
    int has_tint;
    unsigned tint;
    int detect; /* what is the water: 0 the shape test (water_tile_shape), 1 the game's water models (water_model), 2 either */
    int view;   /* 1: the debug view, the water found drawn in colour ([graphics.effects] debug=11) */
    /* the water pass (d3d8_water.h water_pass): 1 the water drawn after the world over copies of the scene's colour and depth
     * (water.pass), 0 in its place, opaque; how far into the water one sees, in yalms (water.depth); how much the waves bend
     * what shows through (water.refraction); how wide the soft edge and the foam at the shore are, in yalms (water.shore), and how
     * strong the foam (water.foam); how strong the fine ripples are and how large (water.ripples, water.ripple.size) */
    int pass;
    float clarity, refraction, shore, foam, ripples, ripple_size;
    int log;   /* water.log: the draws looked at for the water's sheets (water_sheet) written to the log, once each */
    int probe; /* 1 to 3 ([graphics] water.probe): the pass's water shows the scene's colour as it reads it, that
                * taken out of the fog, or how much of it shows through (gfx_hlsl_water.h emit_water_depth) */
    int horizon; /* water.horizon: the sea drawn on past the zone's edge to the horizon (d3d8_water.h water_horizon_pass) */
    float falls;  /* water.falls: how strongly a waterfall churns and foams the water it lands in (d3d8_water.h water_foot_note), 0 not at all */
    float caustics; /* water.caustics: the rippling light on the bed in shallow water, 0 none */
    float relief;   /* water.relief: the wavelets lit as slopes, the refraction with them, 0 flat */
    int shadows;    /* water.shadows: the sun's shadows on the pass's water; 0 (the default) none (water_pass: GfxScene.water_noshadow) */
    int sea;        /* water.sea: the game's sea tiles dropped and VanaCore's one sea plane drawn in their place (d3d8_water_sea.h) */
    /* the world mirrored in the water (d3d8_water_mirror.h): how strongly (water.mirror, 0 off), the mirror's size as a share of
     * the scene's each way (water.mirror.scale), how far from the camera what's mirrored reaches, in yalms (water.mirror.reach,
     * 0 all of it), and how far the waves push it about (water.mirror.waves) */
    float mirror, mirror_scale, mirror_reach, mirror_waves;
    int tris; /* 1: the triangles debug view ([graphics.effects] debug=34, d3d8_water_tris.h) */
} WaterSettings;
void d3d8_set_water(const WaterSettings* settings);
void d3d8_set_water_zones_off(const uint16_t* zones, unsigned n);
/* The zone's floors as the grass gets them (grass_set_floors): where the collision's water floors lie, so the ground's triangles
 * over them can be drawn as VanaCore's water where a zone bakes the water's look into the ground (d3d8_water.h water_ground).
 * zone 0, or NULL: none. */
void d3d8_set_water_floors(const GrassFloors* floors);
/* The game's water models (vanacore_gfx_water_loading): 1 while the calling thread loads one, 0 after; and how many
 * vertex buffers have been made while one loaded, so far */
int d3d8_water_loading(int on);
uint32_t d3d8_water_buffers(void);
void d3d8_set_window_opacity(float all, float chat);
void d3d8_set_border_opacity(float all, float chat);
void d3d8_set_border_colours(int all_on, uint32_t all_rgb, int chat_on, uint32_t chat_rgb);
void d3d8_set_window_colours(int all_on, uint32_t all_rgb, int chat_on, uint32_t chat_rgb);
void d3d8_set_window_image(const uint32_t* argb, uint32_t w, uint32_t h);
void d3d8_set_window_image_look(int fit, int chat, int input);
void d3d8_set_chat_rects(const float* rects, unsigned count);
void d3d8_set_input_rect(const float* rect);
void d3d8_set_border_solid(int all, int chat, int input);
void d3d8_set_window_corners(int shape, float size);
int d3d8_window_corners(float* size);
void d3d8_clip_to_corners(const float* rect);
void d3d8_window_drawing(int on);
void d3d8_window_scale(float s, float ox, float oy);
void d3d8_set_input_look(float alpha, float border_alpha, int tint_on, uint32_t tint_rgb, int border_on, uint32_t border_rgb);
void d3d8_set_chat_shadow(int mode, float opacity);
struct VanaTextFont;
struct VanaLabelGlyph;
void d3d8_set_chat_font(int which, const struct VanaTextFont* font);
uint32_t d3d8_wanted_glyphs(int which, uint16_t* codes, uint32_t most);
void d3d8_label_word(int what, int shadow, uint32_t code, float wide);
void d3d8_set_label_glyphs(const struct VanaLabelGlyph* list, uint32_t count);
void d3d8_chat_drawing(int on);
void d3d8_button_drawing(int on);
void d3d8_clock_drawing(int on);
void d3d8_game_letters(int on);
void d3d8_set_macrobar(uint32_t color, int prefix);
void d3d8_label_fixed(int on);
void d3d8_hide_letters(int which, int on);
void d3d8_text_rolling(int on);
void d3d8_text_centred(int on, float x);
void d3d8_text_right(int on);
void d3d8_text_whole(int on, int right, const char* text);
int d3d8_chat_scale(float* sx, float* sy);
void d3d8_set_interface_target(uint32_t texture, int known);
void d3d8_letter_draw(int on, int code, int x, int y, float sx, float sy);
void d3d8_input_line(int on);
void d3d8_set_input_line(const struct VanaLinePicture* line);
void d3d8_drop_draws(int on);
void d3d8_chat_font_debug(int on);
void d3d8_chat_font_report(char* text, unsigned size);
int d3d8_text_area(int area);
struct VanaTipPart;
void d3d8_set_tip_parts(const struct VanaTipPart* parts);
float d3d8_tips_extra(void);
float d3d8_tips_width(float left, float top, float right, float bottom);
void d3d8_set_tip_rarity(int name, float strength, float height);
void d3d8_tip_item(int drawing, int has, uint32_t rgb);
void d3d8_tip_type(int drawing);
void d3d8_tip_equipped(int on);
void d3d8_set_hidden_windows(const float* rects, unsigned count);
/* window: 1 draws the back buffer (and its depth buffer and the menu target) at the window's size,
 * 0 at the game's; scene: the 3D scene's target at that multiple of it (DLSS), 0 at the game's own size.
 * From the next Present. */
void d3d8_set_resolution(int window, float scene);
/* [graphics] background: the 3D scene's target drawn at w x h (each side at most 8192) while scene above is 0;
 * 0 x 0 at the game's own size (its background resolution). From the next Present. */
void d3d8_set_background(uint32_t w, uint32_t h);
/* the 3D scene's target as it's drawn (0 x 0 until the game has drawn one) */
void d3d8_scene_size(uint32_t* w, uint32_t* h);
/* 1 around VanaCore's own drawing: its draws meant for the back buffer go into a layer of the window's
 * size instead, in the window's pixels, which Present lays over the frame. */
void d3d8_set_own_pixels(int on);
/* any thread: 1 when (x, y), in the pixels plugins draw in, is over Ashita's Minimap as drawn in the last half second */
int d3d8_plugin_wheel_over(int x, int y);
/* That layer as drawn this frame (BGRA, premultiplied), into pixels with pitch (NULL: only its size);
 * 0 when nothing was drawn into it. */
int d3d8_ui_layer(uint32_t* w, uint32_t* h, void* pixels, uint32_t pitch);
/* vanacore_gfx_ui_split: the interface's draws from here to Present go into a second layer, over the first */
void d3d8_ui_split(void);
/* Rectangles of the game's picture kept over what's drawn after them: each take copies one from the back buffer (x, y,
 * w, h in its pixels, as the game knows them; D3D8_RAISE_MOST a frame at most), put lays the copies back there and cuts
 * their places out of the layer above, so the game's own pixels show over everything. Put does nothing unless a take
 * came first; once for the frame's takes. */
#define D3D8_RAISE_MOST 8
void d3d8_raise_take(int x, int y, int w, int h);
void d3d8_raise_put(void);
/* In full screen, the size the back buffer is drawn at with [graphics] resolution=game ([ffxi] resolution.game's, live;
 * stretched over the screen); 0 x 0 for the game's own resolution. resolution=window draws at the window's. */
void d3d8_set_render_size(uint32_t w, uint32_t h);
/* [graphics] drawlog, live: the next frame's draws are written to <path> while <path>.go exists; NULL or "" turns it off. */
void d3d8_set_drawlog(const char* path);
/* [graphics.effects] elight_lantern_tex, live: the hashes of the textures a lantern's flame is drawn with (drawlog's "hash"),
 * comma or space separated; "" none. */
void d3d8_set_lantern_textures(const char* list);
/* ... elight_dim_tex: an effect's that gives far more light than it looks to (taken at elight_dim's share) */
void d3d8_set_dim_textures(const char* list);
/* ... dlss5_skip_tex: an effect's DLSS 5 isn't to take as a light */
void d3d8_set_dlss5_skip_textures(const char* list);
#endif
