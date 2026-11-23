/* VanaCore's snow trails (snow.c): footprints pressed into a zone's snowfields, drawn as shading on the ground the game
 * already shows, where players, NPCs and monsters have walked. */
#pragma once

#include "gfx.h"
#include "grass.h"

#ifdef __cplusplus
extern "C" {
#endif

/* [graphics] snow*: enabled; depth how deep a print is pressed (yalms, 0.02 to 0.5: how steep its walls look); strength (0 to 2) how
 * strongly it's shaded; fade the seconds a print takes to fill in with snow (0: never); sink how far (yalms, 0 off) a character
 * standing in snow is lowered into it, the snow's surface hiding the rest; soft (0 to 1) how soft the prints' edges are (a blur of
 * their heights and wider walls). A print is shaded once, as it's pressed in, with the settings and the light of that moment. */
typedef struct SnowSettings
{
    int enabled;
    float depth, strength, fade;
    float sink;
    float soft;
    int sand; /* trails in the zone's sand ground (terrain 3) too; read when a zone's floors are laid out, so on the next zone */
} SnowSettings;

void snow_configure(const SnowSettings* settings);
/* The zone the player is in, each frame (0 while there's none): a zone's snow and trails are let go as it changes. */
void snow_set_zone(int zone);
/* The zone's floors, the same hand-over as the grass's (grass_set_floors): the snow ground among them is laid out once, kept
 * on the GPU, and the footprints are drawn over it. zone 0, or none: let go. */
void snow_set_floors(const GrassFloors* floors);
/* The characters and fixtures near the player, each frame (vanacore_gfx_set_characters): 4 floats each, (x, height, y) and a
 * character's hitbox size or -1 for a fixture. Their steps are what's pressed into the snow. */
void snow_set_characters(const float* xyzr, unsigned count);
/* A draw of a character (a player, NPC or monster and what it wears: d3d8.c's is_char), before it's drawn: its model's lowest vertices
 * are kept as that character's feet (their size and where they are, for the prints), and with sinking on its matrices are moved down
 * into the snow it stands in. Nothing unless trails are on and the zone has snow. */
void snow_character_draw(GfxDraw* d);
/* The graphics device lost and made again (d3d8_device_lost): the trail's texture and the snow's buffer went with it; the floors
 * are handed over again, as for a zone not seen yet. */
void snow_device_lost(void);
/* The frame's 3D scene is finished in color, drawn with depth (before grass_scene and gfx_scene_done): the trails are laid over
 * the snow ground, with the scene's camera. */
void snow_scene(GfxTex* color, GfxTex* depth, const GfxScene* s);

#ifdef __cplusplus
}
#endif
