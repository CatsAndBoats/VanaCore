/* The sun the shadows fall from, picked by vote, and the sky's sun or a zone's fixed light (split out of d3d8.c): the lit draws
 * d3d8.c sees vote for the directional lights they use (d3d8_sun_vote), and once a frame before the world's effects the sun is
 * picked from them and put into the scene (d3d8_sun_pick). */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "d3d8.h"
#include "d3d8_sun.h"
#include "gfx.h"
#include "gfx_shared.h"
#include "runtime.h"

static uint32_t g_frame; /* the frame d3d8_sun_pick is picking for (d3d8.c's g_scene.frame) */

/* The sun, kept steady. FFXI lights each object with directional lights of its own (characters of one
 * kind, the objects near the camera, ... differ by tens of degrees), so the frame's first lit draw's light
 * turned the shadows as the camera moved from one to another. Instead each directional light the world's
 * lit draws use is counted, in the world (so the camera turning doesn't move it): a vote a frame, the
 * scores fading over a second or two. The light most used is the sun; another takes over only once it's
 * clearly ahead (half as much again), and the sun moving through the day moves its light with it. */
#define SUN_VOTES 8
static struct
{
    float dir[3], color[4]; /* the world, toward the light; its diffuse */
    float score, frame;     /* votes, fading; this frame's so far */
    uint32_t draw;          /* this frame's first draw with it */
    uint32_t voted;         /* the last g_scene.frame it got a vote in (sun_pick) */
} g_sun_votes[SUN_VOTES];
static int g_sun_pick = -1;

/* The sky's sun, or a zone's fixed light (from XI on Anything, rubymatrix/xi-on-anything, d3d8.c sun_sky). Outdoors the
 * game's sun never stands still: it turns with the clock a little every scene, always the same way, in the world's x-y
 * plane (z = 0). A zone with no sky (Ru'Hmet, Garlaige Citadel, Ordelle's Caves, the Necropolis) lights everything from
 * one light that never moves - straight down, or slanted, often with z != 0 - and the sun's shadows from it are those of
 * a ceiling over everything: the zone goes black. After a zone change (half a second with no world drawn) the light
 * counts as fixed until the sun has moved like the sky's 4 times: by more than rounding and less than about 3 degrees
 * (more is another light winning the vote), the same way as the move before. A scene it didn't move in at all says
 * neither (a frame the game's clock didn't step). The game steps its sun about every 17 frames, so 30 moves kept the
 * shadows off for 8 to 9 seconds after every zone change; the light is the game's own, in the world, and a fixed one
 * doesn't change at all, so 4 are as sure. Then the sun's shadows ease in over a third of a second (20 scenes).
 * GfxScene.indoors: 1 in a Mog House (vanacore_gfx_set_moghouse) whatever the sun does, and in a zone whose model DAT says it is
 * lit by a fixed light alone (vanacore_gfx_set_fixed_light): Palborough Mines had the sun's shadows (a player, 2026-11-04) though
 * every environment its DAT has is a fixed light's. */
static struct
{
    float dir[3], step[2]; /* the sun's world direction at the last scene it was seen in, and its last move */
    int have, moved, seen;
    uint32_t moving;       /* moves in a row like the sky's sun's */
    uint32_t frame;        /* g_frame of the last world scene */
    uint32_t scenes;       /* world scenes since the zone change, while the sun hasn't moved */
    float indoors;         /* 1 at a zone change, easing to 0 once the sun has moved */
    uint64_t since;        /* gfx_now_ns at the zone change's first world scene (inside_ease) */
} g_sky;
static volatile int g_moghouse; /* d3d8_set_moghouse: in the player's Mog House (the zone-in packet says) */

void d3d8_set_moghouse(int in)
{
    if (in != g_moghouse)
        rt_log("[recomp] d3d8: %s a Mog House: the sun's shadows %s\n", in ? "in" : "out of",
            in ? "scaled by moghouse" : "as the zone's light has them");
    g_moghouse = in;
}

static volatile int g_fixed_light; /* d3d8_set_fixed_light: the zone's DAT lights it by a fixed light alone */

void d3d8_set_fixed_light(int on)
{
    if (on != g_fixed_light)
        rt_log("[recomp] d3d8: %s: the sun's shadows %s\n", on ? "a zone lit by a fixed light alone (its DAT)" :
            "a zone with a sky", on ? "scaled by moghouse" : "as the zone's light has them");
    g_fixed_light = on;
}

static volatile int g_warping; /* d3d8_set_warping: a home point's teleport under way (GfxScene.warping) */

void d3d8_set_warping(int on)
{
    g_warping = on;
}

static volatile float g_vana_minute; /* d3d8_set_vana_minute: the Vana'diel minute of the day (GfxScene.vana_minute) */

void d3d8_set_vana_minute(float minute)
{
    g_vana_minute = minute;
}

/* this world scene's GfxScene.indoors; w: the sun's world direction when this scene's lit draws voted for it, else NULL */
static float sun_indoors(const float* w)
{
    if (g_sky.seen && g_frame - g_sky.frame >= 30)
        memset(&g_sky, 0, sizeof g_sky); /* a zone change: its sun is still to be seen moving */
    if (!g_sky.seen)
        g_sky.indoors = 1.0f, g_sky.since = gfx_now_ns();
    g_sky.seen = 1, g_sky.frame = g_frame;
    if (w && g_sky.have && !g_sky.moved)
    {
        float dx = w[0] - g_sky.dir[0], dy = w[1] - g_sky.dir[1];
        float d2 = dx * dx + dy * dy;
        if (d2 > 1e-11f)
        {
            int sky = d2 < 0.05f * 0.05f && fabsf(w[2]) < 1e-3f && fabsf(g_sky.dir[2]) < 1e-3f &&
                      dx * g_sky.step[0] + dy * g_sky.step[1] >= 0.0f;
            g_sky.moving = sky ? g_sky.moving + 1 : 0;
            g_sky.moved = g_sky.moving >= 4;
            g_sky.step[0] = dx, g_sky.step[1] = dy;
            if (g_sky.moved)
                rt_log("[recomp] d3d8: the sun moves with the clock here (after %u scenes): its shadows come in\n", g_sky.scenes);
        }
    }
    if (w)
        memcpy(g_sky.dir, w, sizeof g_sky.dir), g_sky.have = 1;
    /* the shadows ease in over a third of a second, by time (upstream's 0.05 a scene at 60 a second): by frames, a high
     * frame rate made it a pop */
    static uint64_t was;
    const uint64_t now = gfx_now_ns();
    const double dt = was && now > was ? fmin((double)(now - was) * 1e-9, 0.1) : 1.0 / 60.0;
    was = now;
    if (g_sky.moved)
        g_sky.indoors = fmaxf(g_sky.indoors - (float)(dt * 3.0), 0.0f);
    else if (++g_sky.scenes == 600)
        rt_log("[recomp] d3d8: the sun hasn't moved with the clock in 600 scenes: a zone's fixed light, its shadows scaled by "
               "moghouse\n");
    return g_moghouse || g_fixed_light ? 1.0f : g_sky.indoors;
}

/* [graphics.effects] night: how far into the night the clock is (d3d8_night, GfxScene.night) - 0 from 07:00 to 17:00, 1 from 20:00
 * to 04:00, eased between (dusk 17:00 to 20:00, dawn 04:00 to 07:00: the game's own light hands over at 18:00 and 06:00, and the
 * sun's jump to full at 06:00 is mostly under the night still) - and 0 under a zone's fixed light or in a Mog House (indoors),
 * while the clock isn't known (0). Eased by time, a whole night a second: a zone's sun found moving (sun_indoors) brings the night
 * in instead of switching it on. g_outdoors: the same ease of 1 outdoors with the clock known, else 0; g_outdoors - 2 g_night is
 * GfxScene.day_night (1 by day, -1 at night, 0 indoors: day_night_warmth) */
static float g_night, g_outdoors;
/* [graphics.effects] day_bright: how far into the sun's hours the clock is, outdoors - 1 from 07:00 to 17:00, 0 before 06:00 and
 * after 18:00 (the game lights the world by the moon then), eased over the hour between, and by time as the night is */
static float g_day;
/* The game's sun and moon light round its turns at 06:00 and 18:00 (gfx_light_turn), outdoors: 0 at each turn, 1 half an hour
 * either side. The game turns its light 180 degrees there in a frame, and at 06:00 the sun's comes on at full: 0.02 to 1.0 of the
 * moon's going out, every slope it faces lit at once (a recording, 2026-11-13). Its light now goes out into each turn and
 * comes up out of it, as the shadows do. Eased by time as the night is; 1 indoors and while the clock isn't known */
static float g_turn = 1.0f;

static float night_by_clock(float m)
{
    if (!(m > 0.0f))
        return 0.0f;
    float t = m >= 1020.0f ? (m - 1020.0f) / 180.0f : 1.0f - (m - 240.0f) / 180.0f;
    t = fminf(fmaxf(t, 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static float day_by_clock(float m)
{
    if (!(m > 0.0f))
        return 0.0f;
    float t = fminf((m - 360.0f) / 60.0f, (1080.0f - m) / 60.0f);
    t = fminf(fmaxf(t, 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static void night_ease(float indoors)
{
    static uint64_t was;
    const uint64_t now = gfx_now_ns();
    const float dt = was && now > was ? (float)fmin((double)(now - was) * 1e-9, 0.1) : 1.0f / 60.0f;
    was = now;
    const float out = g_vana_minute > 0.0f ? 1.0f - fminf(fmaxf(indoors, 0.0f), 1.0f) : 0.0f;
    const float want = night_by_clock(g_vana_minute) * out;
    g_night += fminf(fmaxf(want - g_night, -dt), dt);
    g_outdoors += fminf(fmaxf(out - g_outdoors, -dt), dt);
    g_day += fminf(fmaxf(day_by_clock(g_vana_minute) * out - g_day, -dt), dt);
    g_turn += fminf(fmaxf(1.0f - out * (1.0f - gfx_light_turn(g_vana_minute)) - g_turn, -dt), dt);
}

float d3d8_night(void)
{
    return g_night;
}

/* [graphics.effects] indoor_bright: indoors (d3d8_inside), 1 in a Mog House, in a zone its DAT says is lit by a fixed light alone,
 * and in one whose sun still hasn't moved with the clock 10 seconds after it came in (a fixed light the DAT didn't say), else 0;
 * eased 2 a second. Not GfxScene.indoors, which is 1 for the first second or two of every zone, outdoors too, until its sun is
 * seen moving: the brightness would come on at each zone change */
static float g_inside;

static void inside_ease(void)
{
    static uint64_t was;
    const uint64_t now = gfx_now_ns();
    const float dt = was && now > was ? (float)fmin((double)(now - was) * 1e-9, 0.1) : 1.0f / 60.0f;
    was = now;
    const int fixed = !g_sky.moved && g_sky.since && now > g_sky.since && now - g_sky.since > 10000000000ull;
    const float want = g_moghouse || g_fixed_light || fixed ? 1.0f : 0.0f;
    g_inside += fminf(fmaxf(want - g_inside, -2.0f * dt), 2.0f * dt);
}

float d3d8_inside(void)
{
    return g_inside;
}

/* [graphics.effects] night_bright: the night lit as the day is, by its light's strength: the moon's (the directional light the game
 * lights its night by) to 1 + 2.5 x night_bright through the night's clock - Batallia's 0.18 to 0.63 at 1, toward the day's sun at
 * 1.06, its ambient 0.24 already about the day's share of it (0.38 to 1.06) - so slopes and walls the moon faces stand out from
 * those it doesn't, and the moon's shadows (sun_direct, GfxScene.brighten) come in as the sun's do. The ambient, which lit
 * everything the same and washed it out, only by night_bright_ambient (General lighting): 1 + 0.75 x it. indoor_bright: the
 * ambient to 1 + 0.75 x it and the zone's fixed light to 1 + 0.5 x it, indoors (d3d8_inside). day_bright (Sun strength): the
 * sun's light to 1 + day_bright through its hours outdoors (g_day) - Batallia's 1.06 to 2.12 at 1 over an ambient of 0.38 left as
 * it is, so what faces the sun reaches full light sooner and the shade stays where it was; the lit colour is clamped at full
 * (gfx_hlsl.c emit_lighting), so past that a face is as bright as its texture gets. kd the directional lights' factor, ka the
 * ambient's, 1 each for none */
void d3d8_brighten(float* kd, float* ka)
{
    const float moon = g_gfx_night_bright * g_night, general = g_gfx_night_bright_ambient * g_night;
    const float in = g_gfx_indoor_bright * g_inside;
    *kd = (1.0f + 2.5f * moon) * (1.0f + 0.5f * in) * (1.0f + g_gfx_day_bright * g_day) * g_turn;
    *ka = (1.0f + 0.75f * general) * (1.0f + 0.75f * in);
}

/* 1 through the moon's hours (clock_body: the sun from 06:00 to 18:00), outdoors: the shadows' night settings (gfx_sun_hours). A
 * zone's fixed light and a Mog House are lit by neither. After a zone change the night's come in as the sun is found moving,
 * while its shadows are all but out (indoors easing from 1) */
static int moon_hours(float indoors)
{
    const float m = g_vana_minute;
    return m > 0.0f && (m >= 1080.0f || m < 360.0f) && indoors < 0.99f;
}

/* one vote for the directional light w points toward (the world's, unit), its diffuse colour, from a lit draw (`draw`: g_scene.draws)
 * - the same light as one already counted within 2 degrees, else the weakest of the others */
void d3d8_sun_vote(const float w[3], const float color[4], float weight, uint32_t draw)
{
    int at = -1, low = -1;
    for (int k = 0; k < SUN_VOTES; ++k)
    {
        const float d = w[0] * g_sun_votes[k].dir[0] + w[1] * g_sun_votes[k].dir[1] + w[2] * g_sun_votes[k].dir[2];
        if (g_sun_votes[k].score + g_sun_votes[k].frame > 0.0f && d > 0.9995f) /* the same light, within 2 degrees */
        {
            at = k;
            break;
        }
        if (k != g_sun_pick && (low < 0 || g_sun_votes[k].score + g_sun_votes[k].frame <
                                               g_sun_votes[low].score + g_sun_votes[low].frame))
            low = k;
    }
    if (at < 0)
    {
        if (low < 0)
            return;
        at = low;
        memset(&g_sun_votes[at], 0, sizeof g_sun_votes[at]);
    }
    if (g_sun_votes[at].frame == 0.0f)
        g_sun_votes[at].draw = draw;
    memcpy(g_sun_votes[at].dir, w, 12); /* as it is now: the sun moves through the day */
    memcpy(g_sun_votes[at].color, color, 16);
    g_sun_votes[at].frame += weight;
}

/* the angle, in degrees, between the world directions two of the sun's votes are for */
static float sun_vote_angle(int a, int b)
{
    const float* p = g_sun_votes[a].dir;
    const float* q = g_sun_votes[b].dir;
    return acosf(fmaxf(fminf(p[0] * q[0] + p[1] * q[1] + p[2] * q[2], 1.0f), -1.0f)) * 57.29578f;
}

/* sun_pick: `best` is ahead of the sun in the vote by half as much again (or there is no sun yet). Where the sun is known to
 * move with the clock (sun_indoors: g_sky.moved), another light that gets ahead in the vote doesn't take it over while the sun
 * is still voted for: a camera panning from one lit part of the zone to another (a pillar's light 5 degrees from the sun's,
 * straight down at noon) swung the shadows' light to it, over a few seconds, and back - every shadow in view slid with it. The
 * sun's own light going on turning (the 180 degrees at 06:00 and 18:00 is one light, and nothing votes for the old direction
 * after it) or going quiet for 45 frames lets another take over as before. With the clock known the sun and the moon change
 * places by it alone (clock_pick), and this is for the other lights. */
static void sun_take_over(int best, int new_place)
{
    static int kept;
    const int rival = g_sun_pick >= 0 && best != g_sun_pick;
    const uint32_t quiet = rival ? g_frame - g_sun_votes[g_sun_pick].voted : 0;
    if (rival && g_sky.moved && quiet < 45 && !new_place)
    {
        if (!kept)
            gfx_watch("a light %.1f degrees from the clock's sun is ahead in the vote (%.1f against %.1f) and is kept out: the sun "
                      "was voted for %u frames ago", sun_vote_angle(g_sun_pick, best), g_sun_votes[best].score,
                g_sun_votes[g_sun_pick].score, quiet);
        kept = 1;
        return;
    }
    if (rival)
    {
        const float* a = g_sun_votes[g_sun_pick].dir;
        const float* b = g_sun_votes[best].dir;
        gfx_watch("the sun's light changed %.1f degrees, to (%.2f %.2f %.2f) from (%.2f %.2f %.2f): %.1f votes against the old "
                  "one's %.1f, which got its last vote %u frames ago%s", sun_vote_angle(g_sun_pick, best), b[0], b[1], b[2], a[0], a[1],
            a[2], g_sun_votes[best].score, g_sun_votes[g_sun_pick].score, quiet, g_sky.moved ? " (the sun moves with the clock here)" : "");
    }
    kept = 0;
    g_sun_pick = best;
}

/* The sky's light the clock says is up (b: the world, toward it): from 06:00 to 18:00 the sun, which rises in the east (+x) and
 * turns through the world's x-y plane, straight up at noon, to the west at 18:00, a quarter degree a minute; through the night the
 * moon, straight opposite where the sun would be (the game's own light turns 180 degrees at 06:00 and at 18:00, and every log of
 * 2026-11-03 and 04 had the moon 179.9 to 180.0 degrees from the sun). 0: the clock isn't known */
static int clock_body(float b[3])
{
    const float m = g_vana_minute;
    if (!(m > 0.0f))
        return 0;
    const float a = (m - 360.0f) * (3.14159265f / 720.0f);
    const float sun = m >= 360.0f && m < 1080.0f ? 1.0f : -1.0f;
    b[0] = sun * cosf(a), b[1] = -sun * sinf(a), b[2] = 0.0f;
    return 1;
}

/* light k stands within 5 degrees of the world direction b in the sky's plane (the world's x-y, as the sun and the moon do) */
static int sky_near(int k, const float* b)
{
    const float* d = g_sun_votes[k].dir;
    return fabsf(d[2]) < 1e-3f && d[0] * b[0] + d[1] * b[1] > 0.9962f; /* cos 5 degrees */
}

/* ... and has colour: after noon in Kazham (2026-11-03) a dark light straight down (its diffuse 0) that the sun had passed at noon
 * kept the pick by sun_take_over's guard, and the shadows fell from straight up, faded to nothing by the cloud's share (no colour,
 * no sun), until dusk. A light with no colour, or out of the sky's plane, isn't the sun or the moon */
static int sky_lit(int k, const float* b)
{
    return sky_near(k, b) && g_sun_votes[k].color[0] + g_sun_votes[k].color[1] + g_sun_votes[k].color[2] > 0.0f;
}

/* the voted light the clock's body (b) is: sky_lit, voted for in the last 45 frames and since the last new place (its score; a
 * new place's votes are this scene's alone), the nearest. -1: none this frame */
static int clock_light(const float* b)
{
    int at = -1;
    float closest = -1.0f;
    for (int k = 0; k < SUN_VOTES; ++k)
    {
        const float d = g_sun_votes[k].dir[0] * b[0] + g_sun_votes[k].dir[1] * b[1];
        if (g_frame - g_sun_votes[k].voted < 45 && g_sun_votes[k].score > 0.0f && sky_lit(k, b) && d > closest)
            closest = d, at = k;
    }
    return at;
}

static void sun_clock_take(int at)
{
    if (at == g_sun_pick)
        return;
    if (g_sun_pick >= 0)
        gfx_watch("the %s is the light the clock puts it at, %.1f degrees from the light picked before (%.1f votes against its "
                  "%.1f)", g_vana_minute >= 360.0f && g_vana_minute < 1080.0f ? "sun" : "moon", sun_vote_angle(g_sun_pick, at),
            g_sun_votes[at].score, g_sun_votes[g_sun_pick].score);
    g_sun_pick = at;
}

/* The pick while the clock is known (clock_body: b, the sun or the moon). The light the clock's body is whenever a lit draw voted
 * for it in the last 45 frames; while none has, the one picked kept if it is that body, turned with the clock; else the vote, which
 * never takes the other body. So the sun gives way to the moon at 18:00 and the moon to the sun at 06:00, and at no other time. By
 * day most lit draws take the moon (160,224 votes against the sun's 106,809, 2026-11-04 15:43), and once no lit draw in view had
 * voted for the sun for 45 frames, the clock's pick (by day only, and only of a light voted for in the last 45 frames) and
 * sun_take_over's guard let go of it together and the vote gave the shadows to the moon, under the horizon ("the sun's light
 * changed 179.9 degrees ... which got its last vote 45 frames ago", 2026-11-04 11:55): the whole ground in shadow, and back with
 * the next draw lit by the sun (a player's Jeuno on 0.8.152, 2026-11-04). 1: the pick is the body, kept so */
static int clock_pick(const float* b, int new_place)
{
    const int at = clock_light(b);
    if (at >= 0)
    {
        sun_clock_take(at);
        return 0;
    }
    if (g_sun_pick >= 0 && sky_lit(g_sun_pick, b))
    {
        memcpy(g_sun_votes[g_sun_pick].dir, b, 12); /* (a vote for it puts the game's own back) */
        return 1;
    }
    const float other[3] = { -b[0], -b[1], 0.0f };
    if (g_sun_pick >= 0 && sky_near(g_sun_pick, other))
        g_sun_pick = -1; /* the other body's: its time is over */
    int best = -1;
    for (int k = 0; k < SUN_VOTES; ++k)
        if (!sky_near(k, other) && g_sun_votes[k].score > 0.0f && (best < 0 || g_sun_votes[k].score > g_sun_votes[best].score))
            best = k;
    if (best >= 0 && (g_sun_pick < 0 || g_sun_votes[best].score > 1.5f * g_sun_votes[g_sun_pick].score))
        sun_take_over(best, new_place);
    return 0;
}

/* once a frame, before the world's effects: the votes counted, the sun picked, into the scene (its
 * camera's view) */
uint32_t d3d8_sun_pick(GfxScene* sc, uint32_t frame)
{
    g_frame = frame;
    /* A new place (a zone change: 30 frames without the world, as sun_indoors takes it; or the camera 50 yalms from where it
     * was, a warp): the votes from before are another place's lights, scored over hundreds of frames, and the new sun took
     * seconds to outscore them. Only this scene's votes count, and the sun is picked again (from XI on Anything,
     * rubymatrix/xi-on-anything, d3d8.c sun_new_place). */
    static float cam_was[3];
    static int have_cam;
    float cam[3];
    for (int j = 0; j < 3; ++j) /* the view's translation back through its rotation */
        cam[j] = -(sc->view[12] * sc->view[j * 4] + sc->view[13] * sc->view[j * 4 + 1] + sc->view[14] * sc->view[j * 4 + 2]);
    const float cdx = cam[0] - cam_was[0], cdy = cam[1] - cam_was[1], cdz = cam[2] - cam_was[2];
    const int new_place = (g_sky.seen && g_frame - g_sky.frame >= 30) ||
                          (have_cam && cdx * cdx + cdy * cdy + cdz * cdz > 50.0f * 50.0f);
    memcpy(cam_was, cam, sizeof cam);
    have_cam = 1;
    if (new_place)
        g_sun_pick = -1;
    int best = -1;
    uint32_t fresh = 0; /* the lights this frame's lit draws voted for (sun_indoors: the sun seen this frame) */
    for (int k = 0; k < SUN_VOTES; ++k)
    {
        if (g_sun_votes[k].frame > 0.0f)
            fresh |= 1u << k, g_sun_votes[k].voted = g_frame;
        g_sun_votes[k].score = new_place ? g_sun_votes[k].frame : g_sun_votes[k].score * 0.99f + g_sun_votes[k].frame;
        g_sun_votes[k].frame = 0.0f;
        if (best < 0 || g_sun_votes[k].score > g_sun_votes[best].score)
            best = k;
    }
    float body[3];
    int kept = 0; /* the clock's body kept with no vote for it (clock_pick): its light goes on to the shadows */
    if (clock_body(body))
        kept = clock_pick(body, new_place);
    else if (best >= 0 && (g_sun_pick < 0 || g_sun_votes[best].score > 1.5f * g_sun_votes[g_sun_pick].score))
        sun_take_over(best, new_place);
    uint32_t slots = 0;
    for (int k = 0; k < SUN_VOTES; ++k)
        slots += (fresh >> k) & 1u;
    sc->indoors = sun_indoors(g_sun_pick >= 0 && (fresh >> g_sun_pick & 1u) ? g_sun_votes[g_sun_pick].dir : NULL);
    sc->warping = (uint32_t)g_warping;
    sc->vana_minute = g_vana_minute;
    gfx_sun_hours(moon_hours(sc->indoors)); /* the shadows' night settings */
    night_ease(sc->indoors);
    inside_ease();
    {
        float kd, ka;
        d3d8_brighten(&kd, &ka);
        sc->brighten[0] = kd - 1.0f, sc->brighten[1] = ka - 1.0f;
    }
    sc->night = g_gfx_night > 0.0f ? g_night : 0.0f;
    sc->day_night = g_outdoors - 2.0f * g_night;
    sc->night_clock = g_night;
    if (g_sun_pick < 0 || !(kept || g_sun_votes[g_sun_pick].score > 1.0f))
        return slots; /* no lit draw for a while: the back end holds the last */
    float v[3];
    for (int k = 0; k < 3; ++k) /* the light's world direction through the view's rotation */
        v[k] = g_sun_votes[g_sun_pick].dir[0] * sc->view[k] + g_sun_votes[g_sun_pick].dir[1] * sc->view[4 + k] +
               g_sun_votes[g_sun_pick].dir[2] * sc->view[8 + k];
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(len > 0.0f))
        return slots;
    sc->sun_dir[0] = v[0] / len, sc->sun_dir[1] = v[1] / len, sc->sun_dir[2] = v[2] / len, sc->sun_dir[3] = 1.0f;
    memcpy(sc->sun_color, g_sun_votes[g_sun_pick].color, 16);
    if (!sc->sun_draw)
        sc->sun_draw = g_sun_votes[g_sun_pick].draw;
    return slots;
}
