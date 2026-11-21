/* VanaCore's MassBirds (birds.c): flocks of birds that cross the sky round the player from time to time, and birds of prey that
 * circle high over them, to give the world a little more life. The birds are VanaCore's own (a picture drawn in code: no art from
 * the game), drawn into the 3D scene with its camera, behind what stands in front of them. */
#pragma once

#include "gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* the kinds of flock (birds_send's kind): small birds in a loose, bobbing cloud; crows, a few, flapping and gliding; geese in a V,
 * high; a bird of prey or two circling high, mostly gliding */
enum
{
    BIRDS_ANY = -1,
    BIRDS_SMALL = 0,
    BIRDS_CROWS,
    BIRDS_GEESE,
    BIRDS_HAWKS,
    BIRDS_KINDS,
};

/* [graphics] birds*: enabled; every the seconds between one flock and the next on average (5 to 900); amount the birds to a flock,
 * times a kind's own (0.25 to 3); size their size, times their own (0.25 to 4); height how high they fly, times their own (0.25 to
 * 3); range how far from the player (yalms, 30 to 250) a flock comes in from and leaves at; speed how fast they fly, times their own
 * (0.25 to 3); brightness the light on them (0 to 3); circling 1: birds of prey come too; night 1: they fly at night too (else they
 * keep to the day); log 1: a line in the log as each flock comes */
typedef struct BirdsSettings
{
    int enabled;
    float every, amount, size, height, range, speed, brightness;
    int circling, night, log;
} BirdsSettings;

void birds_configure(const BirdsSettings* settings);
/* The zone the player is in, each frame: the birds in the sky are let go as it changes. */
void birds_set_zone(int zone);
/* A flock of that kind (BIRDS_*, BIRDS_ANY: one picked as they come) sent on its way at the next scene, whatever the time: for
 * trying them. 1 sent; 0 MassBirds is off; -1 the player is indoors (none fly there) */
int birds_send(int kind);
/* How many birds are in the sky now, and the flocks they're in (the /vana panel's line) */
void birds_count(unsigned* birds, unsigned* flocks);
/* The graphics device lost and made again (d3d8_device_lost): the birds' picture went with it. */
void birds_device_lost(void);
/* The frame's 3D scene is finished in color, drawn with depth (after grass_scene, before gfx_scene_done): the birds are moved on
 * and drawn into it with the scene's camera, depth-tested, writing none. */
void birds_scene(GfxTex* color, GfxTex* depth, const GfxScene* s);

#ifdef __cplusplus
}
#endif
