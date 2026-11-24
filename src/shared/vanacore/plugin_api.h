/*
 * VanaCore's plugin API: what VanaCore does to the game's interface for an Ashita plugin, the same as it does for an addon
 * through vana.gameui and vana.ui.game_on_top (docs/addons/07-game-interface.md). One C header, nothing to link: copy it
 * into your plugin. VanaCore is GPL-3.0-or-later; this header is the interface only and may be used by a plugin under any
 * licence.
 *
 * The plugin looks for VanaCore at run time, so the same DLL works in Ashita, where the API is NULL:
 *
 *     static const VanaCorePluginApi* vana = NULL;
 *     bool Initialize(IAshitaCore* core, ILogManager* log, uint32_t id) {
 *         vana = vanacore_plugin_api(); // NULL outside VanaCore, or in a VanaCore older than this header
 *         if (vana != NULL) {
 *             vana->gameui_hide("chat_input", 1); // the game's chat line kept off the screen: the plugin draws its own
 *             vana->game_on_top(1, 0);            // the game's menus drawn over the plugin's ImGui windows
 *         }
 *         ...
 *     }
 *
 * Everything a plugin asks for is undone when it unloads (or crashes), whether it undid it or not.
 *
 * Every call is made on the game's thread, from one of the plugin's own callbacks (Initialize, Direct3DPresent,
 * HandleCommand, ...): that's how VanaCore knows which plugin is asking. Off that thread, or from anything that isn't an
 * Ashita plugin, a call does nothing and returns VANACORE_NOT_A_PLUGIN.
 *
 * Coordinates are the game's pixels (the back buffer's), the ones ImGui, Ashita's fonts and primitives and the mouse
 * callbacks use. Strings are UTF-8, NUL-terminated.
 *
 * The table only grows: a field is never removed or moved. A plugin built with a newer header gets NULL from an older
 * VanaCore (vanacore_plugin_api asks for its own VANACORE_PLUGIN_API_VERSION); `size` says how much of the table there is.
 */
#ifndef VANACORE_PLUGIN_API_H
#define VANACORE_PLUGIN_API_H

#include <stdint.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1: the first table (to chat_input_caret). 2: gameui_window, gameui_windows, gameui_press. 3: entity, entity_by_id,
 * entity_vitals, entity_request_max_hp. 4: backdrop. */
#define VANACORE_PLUGIN_API_VERSION 4

/* What a call returns */
#define VANACORE_OK 0             /* done (or undone) */
#define VANACORE_HELD 1           /* move, image: another plugin or addon holds it; yours is kept and takes effect when it
                                     lets go. `holder` names it */
#define VANACORE_NOT_UP 2         /* press: that window isn't up, or it's closing */
#define VANACORE_BUSY 3           /* press: the window isn't taking keys this frame (it has just taken one): try the next */
#define VANACORE_UNKNOWN (-1)     /* an element VanaCore doesn't know, or an action that element doesn't take */
#define VANACORE_BAD_ARGUMENT (-2) /* NULL where a value is needed, a count past the most, a colour that isn't one, a row
                                      the window hasn't */
#define VANACORE_NOT_A_PLUGIN (-3) /* not called from an Ashita plugin's callback on the game's thread */
#define VANACORE_UNAVAILABLE (-4)  /* VanaCore didn't find what it needs in the game for this (vanacore.log says what) */

/* gameui_press's events: what the player's keys give a window */
#define VANACORE_EVENT_NONE 0 /* none: only the cursor put on the row */
#define VANACORE_EVENT_UP 1
#define VANACORE_EVENT_DOWN 2
#define VANACORE_EVENT_RIGHT 3
#define VANACORE_EVENT_LEFT 4
#define VANACORE_EVENT_SELECT 5
#define VANACORE_EVENT_CANCEL 6
#define VANACORE_EVENT_MENU 0x1C /* the menu key */

/* VanaCoreMax's source: where a maximum HP or MP comes from */
#define VANACORE_MAX_NONE 0     /* VanaCore has no number for it: an entity outside the party and alliance (only its percent) */
#define VANACORE_MAX_SELF 1     /* the game's own: the local player's */
#define VANACORE_MAX_FULL 2     /* the value seen while the percent read 100 (to within a percent), kept until a sample disagrees */
#define VANACORE_MAX_ESTIMATE 3 /* worked out from a value and its percent: `low` to `high` is what the samples so far allow */
#define VANACORE_MAX_SERVER 4   /* the server said it when asked (entity_request_max_hp): kept for a minute; any entity it lets you ask about */

/* VanaCoreMaxHpAnswer's status */
#define VANACORE_MAXHP_OK 0        /* hp_max is the entity's maximum */
#define VANACORE_MAXHP_NOT_FOUND 1 /* the server has nothing it may say for that id (not seen by this client, a player outside the party) */
#define VANACORE_MAXHP_DISABLED 2  /* the server has the feature switched off */
#define VANACORE_MAXHP_TOO_FAST 3  /* asked again within the server's minimum interval (100 ms) */
#define VANACORE_MAXHP_HIDDEN 4    /* the server keeps this monster's maximum secret (a boss it marked to hide it) */
#define VANACORE_MAXHP_NO_ANSWER 5 /* VanaCore's own: no reply in 3 s (the server has no such module, or the reply was lost) */

/* game_on_top's and game_on_top_areas' flags */
#define VANACORE_ON_TOP_NOT_CHAT_LOG 1 /* the game's own chat log windows never go over these (a chat that stands for them) */

/* game_on_top_areas: the most areas a plugin has at once */
#define VANACORE_ON_TOP_AREAS_MOST 8

typedef struct VanaCoreArea {
    int32_t left, top, right, bottom; /* the game's pixels */
} VanaCoreArea;

/* gameui_image's picture */
typedef struct VanaCoreImage {
    const char* file;    /* a full path: PNG, BMP or JPEG */
    float width, height; /* in the interface's units; 0: the picture's own pixels as units */
    float x, y;          /* moved from the game's anchor (the target arrow's point, at the picture's bottom middle), units */
    uint32_t argb;       /* a tint, 0xAARRGGBB: 0xFFFFFFFF for none */
} VanaCoreImage;

/* gameui_window's: one of a window's buttons (the main menu's items, a Config page's, a list's visible lines) */
typedef struct VanaCoreWindowRow {
    int32_t id;       /* the game's number for it: the window's `cursor`, gameui_press's row */
    float x, y, w, h; /* the game's pixels */
    const char* text; /* its label as the game's menu data gives it ("Status"); "" for a row with none (a list's lines) */
    int32_t state;    /* the game's own number for it (5 greyed out, 1 just chosen) */
    int32_t cursor;   /* 1: the cursor is on it */
    int32_t disabled; /* 1: greyed out */
    int32_t drawn;    /* 0: the window doesn't draw it itself (the main menu's Map) or doesn't show it */
} VanaCoreWindowRow;

/* A run of text the window's last draw made: letters next to each other in one colour (its labels too) */
typedef struct VanaCoreWindowLine {
    const char* text; /* the game's text as UTF-8 */
    float x, y, w, h;
    uint32_t argb;    /* the colour the game draws it in, 0xAARRGGBB */
} VanaCoreWindowLine;

/* A picture the window's last draw made that isn't a letter or a button's bar: icons, on/off boxes, arrows, its frame */
typedef struct VanaCoreWindowPicture {
    const char* name; /* the game's name for the picture ("menu    keytop") */
    float x, y, w, h;
    uint32_t argb;
} VanaCoreWindowPicture;

/* gameui_window: one of the game's windows as it stands this frame */
typedef struct VanaCoreWindow {
    uint32_t size;        /* sizeof the struct VanaCore filled: a field past it isn't there */
    const char* name;     /* its layout's name ("configwi") */
    float x, y, w, h;     /* its place, the game's pixels: where the game draws it (hidden or not) */
    float scale;          /* the game's pixels to one of its interface units: for sizing your drawing as the game's */
    int32_t cursor;       /* the id of the row the cursor is on */
    int32_t count;        /* how many rows the cursor goes over */
    int32_t focused;      /* 1: the window the keys go to */
    int32_t closing;      /* 1: it's closing (its last frames) */
    int32_t hidden;       /* 1: a plugin or addon hides it (gameui_hide) */
    int32_t drawn;        /* 1: lines and pictures are from a draw of it; 0 the first time a window is asked for */
    uint32_t changes;     /* up by one each time a read finds anything else than your last read of it did: its place,
                             cursor, rows, text, pictures. Keep the last and redo your layout when it moves */
    const VanaCoreWindowRow* rows; /* by id */
    uint32_t row_count;
    const VanaCoreWindowLine* lines;
    uint32_t line_count;
    const VanaCoreWindowPicture* pictures;
    uint32_t picture_count;
} VanaCoreWindow;

/* A maximum HP or MP. The game keeps one for the local player only; for a member of the party or alliance (a trust too) VanaCore
 * has it from the value and percent the server sends, each frame (a value seen at 100% is the maximum; the percents of the
 * values after it narrow it down; a sample that disagrees, a curse or a weakness ending, starts it again). */
typedef struct VanaCoreMax {
    uint32_t source; /* VANACORE_MAX_* */
    uint32_t value;  /* the maximum: 0 for VANACORE_MAX_NONE */
    uint32_t low;    /* it is not below this ... */
    uint32_t high;   /* ... nor above this (0xFFFFFFFF for no upper limit yet). Both are `value` for VANACORE_MAX_SELF */
} VanaCoreMax;

/* entity_vitals: a member of the party or alliance's health. Set `size` to sizeof(VanaCoreVitals) before the call. */
typedef struct VanaCoreVitals {
    uint32_t size;       /* in: the size of the struct your header has; out: what VanaCore filled (a field past it isn't there) */
    uint32_t id;         /* the server id */
    uint32_t party_slot; /* 0-17: party 1 (0 the local player), then the alliance's other two parties */
    uint32_t hp, mp, tp; /* as the game has them */
    uint32_t hpp, mpp;   /* percents */
    VanaCoreMax hp_max;
    VanaCoreMax mp_max;
} VanaCoreVitals;

/* entity, entity_by_id: an entity with all the game's record of it (Ashita's entity_t, flat), and its health where the party
 * has it. Set `size` to sizeof(VanaCoreEntity) before the call. Strings are UTF-8, positions are the game's (z is height). */
typedef struct VanaCoreEntity {
    uint32_t size;       /* in: the size of the struct your header has; out: what VanaCore filled */
    uint32_t index;      /* target index */
    uint32_t id;         /* server id */
    char name[32];
    float x, y, z, heading; /* radians */
    float distance;      /* to the local player, SQUARED yalms, as the game keeps it */
    float speed, anim_speed;
    float model_size, hitbox_size;
    uint32_t hpp;        /* the percent: all the game has of an entity outside the party */
    uint32_t type;       /* 0 player, 1 NPC or monster, 2 NPC with a fixed model, 3 door or similar */
    uint32_t race;
    uint32_t status, status_server, status_event;
    uint32_t spawn_flags; /* 0x01 player, 0x02 NPC, 0x10 monster, 0x0D the local player */
    uint32_t claim;      /* the claimer's server id in the low word, a 0/1 flag in the high */
    uint32_t target_index; /* what it's targeting (not kept for the local player) */
    uint32_t pet_index, fellow_index, trust_owner_index, interaction_target_index;
    uint32_t render[9];  /* Render.Flags0 to Flags8: [0] 0x200 is set while it can be targeted, [1] and [2] name flags */
    uint32_t name_color, linkshell_color, costume_id, mount_id, campaign_name_flag, ballista_flags;
    uint32_t models[9];  /* hair, head, body, hands, legs, feet, main, sub, ranged */
    uint32_t animations[10];
    uint32_t animation_time, animation_step, animation_play;
    uint32_t emote_target_index, emote_id, last_action_id;
    uint32_t zone_id, door_id;
    int32_t valid_target; /* 1: it can be targeted */
    int32_t has_vitals;  /* 1: it is in the party or alliance and `vitals` has its health; 0: only `hpp` is known */
    VanaCoreVitals vitals; /* hp_max.source is VANACORE_MAX_NONE and the numbers 0 while has_vitals is 0, unless the server was
                              asked for the entity's maximum HP (entity_request_max_hp): then only hp_max is set, VANACORE_MAX_SERVER */
} VanaCoreEntity;

/* entity_request_max_hp's answers: one per id asked, in the order asked. */
typedef struct VanaCoreMaxHpAnswer {
    uint32_t id;     /* the server id asked about */
    uint32_t hp_max; /* its maximum HP; 0 unless status is VANACORE_MAXHP_OK */
    uint32_t status; /* VANACORE_MAXHP_* */
    uint32_t hp;     /* its HP at the moment the server answered (a snapshot: ask again for a newer one); 0 unless status is VANACORE_MAXHP_OK */
} VanaCoreMaxHpAnswer;

/* Called on the game's thread, once per frame, with the answers to a request. `user` is what the request gave. */
typedef void(__cdecl* VanaCoreMaxHpCallback)(void* user, const VanaCoreMaxHpAnswer* answers, uint32_t count);

typedef struct VanaCorePluginApi {
    uint32_t size;    /* sizeof the table VanaCore has: a field past it isn't there */
    uint32_t version; /* the VANACORE_PLUGIN_API_VERSION VanaCore implements */
    const char* vanacore_version; /* "0.8.198" */

    /* ---- the game's interface: vana.gameui (docs/addons/07-game-interface.md) ---- */

    /* hidden 1: `element` not drawn ("chat_log", "chat_input", "party_list", "alliance_lists", "target_window",
     * "target_arrow", "status_icons", "macro_bar", "network_window", "nameplates"), or any of the game's windows by its
     * layout's name ("configwi", "menuwind": drawn into nothing, the mouse no longer picks its rows). hidden 0: drawn
     * again. Shared: hidden while any plugin or addon hides it. The player's HUD Manager shows it hidden by the plugin.
     * "chat_log": both chat windows kept closed, without the flicker or bounce of the chat line opening; messages still
     * arrive (HandleIncomingText). "chat_input": the chat line kept off the screen; typing still works and its text is
     * still laid out (IChatManager::GetInputTextDisplay), for a plugin that draws the line itself. */
    int(__cdecl* gameui_hide)(const char* element, int hidden);

    /* `element` ("target_window") put at x, y, percents of the interface (its own anchor: the Target window's middle and
     * top), over the player's own setting; gameui_unmove gives it back. One holder at a time: VANACORE_HELD with
     * `holder` (may be NULL) naming who has it. */
    int(__cdecl* gameui_move)(const char* element, double x, double y, char* holder, uint32_t holder_size);
    int(__cdecl* gameui_unmove)(const char* element);

    /* `element` ("target_arrow") drawn as `image` in place of the game's picture; gameui_unimage gives the game's back.
     * One holder at a time, as move. A picture that can't be read is said in vanacore.log and nothing is drawn. */
    int(__cdecl* gameui_image)(const char* element, const VanaCoreImage* image, char* holder, uint32_t holder_size);
    int(__cdecl* gameui_unimage)(const char* element);

    /* Who has `element` hidden, the plugins' and addons' names ("FancyChat, yunaui"), into `out`: the length of the
     * whole answer (0 for nobody; past `size` - 1 when it was cut). Not the player's own switch. */
    uint32_t(__cdecl* gameui_hidden_by)(const char* element, char* out, uint32_t size);

    /* ---- the game's windows over the plugin's drawing: vana.ui.game_on_top ---- */

    /* on 1: the game's windows (an NPC's menu, item details, shops, the macro bar, any other) are drawn over this plugin's
     * ImGui windows wherever they meet, as VanaCore does for an addon; the mouse there is the game's. on 0: no more.
     * flags: VANACORE_ON_TOP_NOT_CHAT_LOG. Needs the player's [client] menus.ontop (on unless they turned it off). */
    int(__cdecl* game_on_top)(int on, uint32_t flags);

    /* The same over what the plugin draws without ImGui (its own Direct3D drawing, fonts, primitives): `count` areas,
     * VANACORE_ON_TOP_AREAS_MOST at most, kept until the next call; count 0 none. Call it when they change. */
    int(__cdecl* game_on_top_areas)(const VanaCoreArea* areas, uint32_t count, uint32_t flags);

    /* 1 while the game's windows are being drawn over plugins' and addons' drawing ([client] menus.ontop on, and VanaCore
     * found what it needs in the game): a chat need not slide out of the way of the game's menus. */
    int(__cdecl* game_on_top_active)(void);

    /* 1 when a game window drawn over plugins' and addons' drawing is at (x, y) this frame: the mouse there is the
     * game's, so a plugin's mouse callback lets it through. */
    int(__cdecl* game_window_at)(int32_t x, int32_t y);

    /* ---- the chat line ---- */

    /* For a plugin that hides "chat_input" and draws the line itself: the caret is at x, on a line from `top` to `bottom`
     * (shown 1), so the game's auto-translate list (Tab) opens there, over the line, or under it when there's no room
     * above. shown 0: the plugin's line isn't drawn now. Kept until the next call. */
    int(__cdecl* chat_input_caret)(int shown, float x, float top, float bottom);

    /* ---- the game's windows read and worked: vana.gameui.window, windows, press (version 2) ---- */

    /* `name` ("configwi", as gameui_windows lists them) as it stands this frame: its place, cursor, rows, and the text
     * and pictures its last draw made, for a plugin that draws the game's menus its own way while the game runs them
     * (gameui_hide it, read it, draw it, gameui_press the player's choice). NULL when it isn't up. What it gives, and
     * everything it points into, stays as it is until the plugin asks for the same window again, or unloads.
     * Ask every frame, from Direct3DPresent: the game has drawn its windows by then, so it's this frame's. A window's
     * text and pictures are kept while a plugin or addon asks for it (at least every 3 seconds) or hides it: the very
     * first ask has `drawn` 0 and no lines, the next frame's is whole, and so is the first read after it opens. */
    const VanaCoreWindow*(__cdecl* gameui_window)(const char* name);

    /* The names of the game's windows up, in the game's order, comma separated ("helpwind,partywin,menuwind"), into
     * `out`: the length of the whole answer (past `size` - 1 when it was cut). */
    uint32_t(__cdecl* gameui_windows)(char* out, uint32_t size);

    /* The player's choice made for them in `name`: its cursor put on `row` (a row's id; 0 leaves it where it is), then
     * `event` (VANACORE_EVENT_SELECT, _CANCEL, ...; VANACORE_EVENT_NONE for none) given to it through the game's own key
     * input, so the window plays its sound, opens what it opens, and addons' triggers hear it as they hear the keys.
     * VANACORE_NOT_UP, VANACORE_BUSY (try the next frame), VANACORE_BAD_ARGUMENT for a row past `count`. */
    int(__cdecl* gameui_press)(const char* name, int32_t row, int32_t event);

    /* ---- entities (version 3): vana.game.entity, entity_by_id and vitals ---- */

    /* The entity at target index `index` (0 to 0x8FF), all the game's record of it: VANACORE_OK, VANACORE_UNKNOWN when there
     * is none, VANACORE_BAD_ARGUMENT for a NULL `out` or a `size` under 4. The game never says a maximum HP for anyone but you;
     * `vitals` has VanaCore's (VanaCoreMax) for the party and alliance, worked out from the game's own party data each frame,
     * so a plugin need not keep its own. */
    int(__cdecl* entity)(uint32_t index, VanaCoreEntity* out);

    /* The same for the entity with server id `id`. Looks at every slot of the game's table: keep the index (`entity`'s) and
     * ask by that each frame. */
    int(__cdecl* entity_by_id)(uint32_t id, VanaCoreEntity* out);

    /* The health of the party or alliance member with server id `id`, also while they are in another zone (no entity then).
     * VANACORE_UNKNOWN for anyone else. */
    int(__cdecl* entity_vitals)(uint32_t id, VanaCoreVitals* out);

    /* Asks CatsEyeXI's server for the maximum HP of up to 8 entities by server id (`count` 1 to 8), a monster's too: the game
     * has none for anyone but you. The server answers for you, your party and alliance, and the monsters, pets and trusts
     * your client has been told about; for anything else the status is VANACORE_MAXHP_NOT_FOUND. `callback(user, answers,
     * count)` runs on the game's thread, once, in a following frame, with an answer for every id; a server without the
     * module gives VANACORE_MAXHP_NO_ANSWER after 3 s. An answer is also kept for a minute as the entity's hp_max
     * (VANACORE_MAX_SERVER) in `entity`'s and `entity_by_id`'s records. Don't ask faster than every 100 ms. VANACORE_OK when
     * asked; VANACORE_BAD_ARGUMENT for no ids, more than 8 or a NULL callback; VANACORE_UNAVAILABLE when it can't be sent
     * now (not in the game yet, or too many requests waiting). A plugin that is unloaded is never called back. */
    int(__cdecl* entity_request_max_hp)(const uint32_t* ids, uint32_t count, VanaCoreMaxHpCallback callback, void* user);

    /* ---- frosted glass behind a window (version 4): vana.ui.backdrop ---- */

    /* Asks for the picture behind the rectangle (x, y, width, height, and the corners' radius, in the pixels ImGui's draw
     * list takes in the window being drawn) to be shown blurred under that window's fill. Call it every frame, from inside
     * the plugin's ImGui window (between its Begin and End); a window drawn scaled by Addons > Layout is followed. Up to 8 at
     * a time. The blur is of the picture and of the VanaCore windows below the lowest window that asks, under that window and
     * the ones above it, so the window's own fill must be see-through for it to show. Returns 1 when the renderer drew the last frame's request (not in the first frame, and not with a renderer that
     * can't: only DirectX 12 blurs yet), 0 when it didn't or the call was made outside a window, so a plugin can choose a
     * fill that lets the blur show or a plainer one; VANACORE_NOT_A_PLUGIN from anywhere but a plugin's callback.
     * `strength` is how blurred, 0 to 1 (a value outside is held to it): 0 hardly at all, 0.5 the usual frosted glass (what
     * vana.ui.backdrop gives without one), 1 about twice that. */
    int(__cdecl* backdrop)(float x, float y, float width, float height, float radius, float strength);
} VanaCorePluginApi;

typedef const VanaCorePluginApi*(__cdecl* VanaCoreGetPluginApi_f)(uint32_t version);

/* VanaCore's table, or NULL outside VanaCore or in one older than `version`. */
static inline const VanaCorePluginApi* vanacore_plugin_api(void)
{
    HMODULE core = GetModuleHandleA("VanaCore.Core.dll");
    VanaCoreGetPluginApi_f get = NULL;
    if (core == NULL)
        return NULL;
    /* cppcheck-suppress cstyleCast ; a C header too, and C has no other cast */
    get = (VanaCoreGetPluginApi_f)(void*)GetProcAddress(core, "VanaCoreGetPluginApi");
    return get != NULL ? get(VANACORE_PLUGIN_API_VERSION) : NULL;
}

#ifdef __cplusplus
}
#endif

#endif /* VANACORE_PLUGIN_API_H */
