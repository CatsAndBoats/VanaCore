// Mass Grass: [graphics] grass* read into a VanaGrass for the graphics (vanacore_gfx_set_grass), from apply_live. An include
// unit of graphics.cpp (its readers and globals are in scope), moved out of it for its length.

// [graphics] grass=0|1 (Mass Grass, experimental): tufts of grass on the zone's grass-textured ground round the player.
// grass.textures: hex hashes of the
// ground textures that are grass (empty, any mostly green one; grass.log=1 lists the ones it finds in the log and
// logs\grass); grass.radius, grass.density (tufts to a square yalm), grass.tufts (the most tufts at once, in thousands), grass.height,
// grass.slope (the steepest ground, in degrees, it grows on), grass.width, grass.variation (how much the heights differ), grass.brightness, grass.flowers (1: a tenth of the tufts with a
// flower), grass.flower_brightness, grass.flowers_only,
// grass.brightness.auto (dims through the Vana'diel night to grass.brightness.night of it: track_daylight)
void apply_grass(When when)
{
    const bool on = flag(L"grass", false);
    const std::string textures = narrow(read(L"graphics", L"grass.textures"));
    static bool said = false;
    if (on != said && when != When::Quiet)
        log::write(on ? L"Graphics: grass on (grass)" : L"Graphics: grass off (grass)");
    said = on;
    // a zone the shipped data\grass_dirt.json says "grass": false of grows none, whatever grass says (Jeuno's cities); it follows
    // the zone (track_focus)
    const bool here = grass_assets::grows(static_cast<int>(g_grass_zone));
    static unsigned bare_said = 0;
    if (on && !here && bare_said != g_grass_zone)
        log::write(L"Graphics: grass: none in zone %u (data\\grass_dirt.json)", g_grass_zone);
    bare_said = on && !here ? g_grass_zone : 0;
    VanaGrass grass{};
    grass.enabled = on && here ? 1 : 0;
    grass.textures = textures.c_str();
    grass.radius = number(L"grass.radius", 150.0f, 4.0f, 500.0f);
    grass.density = number(L"grass.density", 100.0f, 0.1f, 100.0f);
    grass.height = number(L"grass.height", 0.4818f, 0.05f, 10.0f);
    grass.width = number(L"grass.width", 1.0f, 0.1f, 10.0f);
    grass.variation = number(L"grass.variation", 0.5f, 0.0f, 1.0f);
    grass.brightness = number(L"grass.brightness", 1.0f, 0.05f, 10.0f);
    {
        const bool dims = flag(L"grass.brightness.auto", true);
        const float night = number(L"grass.brightness.night", 0.5f, 0.05f, 1.0f);
        if ((dims != g_grass_auto || (dims && night != g_grass_night)) && when != When::Quiet)
            log::write(dims ? L"Graphics: the grass dims at night, to %.2f of its brightness (grass.brightness.auto, grass.brightness.night)"
                            : L"Graphics: the grass keeps its brightness day and night (grass.brightness.auto)",
                       night);
        g_grass_auto = dims, g_grass_night = night;
    }
    // grass.flowers (0 to 1, default 0): 1 a tenth of the tufts with a flower (2026-11-22: it was the share itself, and 0.05,
    // the panel's least step, was still too many)
    grass.flowers = number(L"grass.flowers", 0.0f, 0.0f, 1.0f) * 0.1f;
    grass.flower_brightness = number(L"grass.flower_brightness", 1.0f, 0.05f, 10.0f);
    grass.flowers_only = flag(L"grass.flowers_only", false) ? 1 : 0;
    // grass.tufts: a ceiling on the tufts at once, in thousands (1000: a million); unset or 0, none: as many as grass.radius and
    // grass.density need (grass.c tuft_limit). grass.limit, in tufts, from before it, while it's unset
    {
        const float tufts = number(L"grass.tufts", number(L"grass.limit", 0.0f, 0.0f, 1000000.0f) / 1000.0f, 0.0f, 1000.0f);
        grass.limit = tufts > 0.0f ? static_cast<unsigned>(tufts * 1000.0f + 0.5f) : 0u;
    }
    grass.slope = number(L"grass.slope", 40.0f, 5.0f, 85.0f);
    // the rings: grass.ring.spacing (percent of grass.radius, 25 unless set) puts rings 1-3 at it, twice it and three times it;
    // grass.ring1-3 (percent) put one where it says instead. grass.ring1-3.amount the share of tufts each keeps (percent; inside
    // ring 1, all of them). grass.c keeps them in order, each keeping no more than the one inside it. grass.rings.show=1 draws
    // them round the player (//vana's Debug)
    {
        static const float amount[3] = {80.0f, 50.0f, 30.0f};
        const float spacing = number(L"grass.ring.spacing", 25.0f, 1.0f, 100.0f);
        grass.rings_set = 1;
        for (int i = 0; i < 3; ++i) {
            const std::wstring key = L"grass.ring" + std::to_wstring(i + 1);
            const float start = spacing * static_cast<float>(i + 1);
            grass.ring[i] = number(key.c_str(), start < 100.0f ? start : 100.0f, 0.0f, 100.0f) / 100.0f;
            grass.ring_amount[i] = number((key + L".amount").c_str(), amount[i], 0.0f, 100.0f) / 100.0f;
        }
        grass.show_rings = flag(L"grass.rings.show", false) ? 1 : 0;
    }
    // grass.cover.show=1 (//vana's Debug): the tufts a floor over them covers (a building's, laid over the ground; left out
    // otherwise) drawn red, those with no floor at or under them to tell yellow
    grass.show_cover = flag(L"grass.cover.show", false) ? 1 : 0;
    grass.log = flag(L"grass.log", false) ? 1 : 0;
    // grass.edges=1 (the default): near the brink of a drop steeper than grass.slope (a ledge, a rock's top) the tufts grow
    // smaller, none hanging out over it
    grass.edges = flag(L"grass.edges", true) ? 1 : 0;
    // grass.edge.fade (yalms, 0 to 10, default 2; 0 off): tufts this near where the grass ends (a drop, a slope steeper than
    // grass.slope, rock or stone after soil, the ground's texture turning from grass) grow shorter, to grass.edge.height percent
    // of their height at it (5 to 100, default 25); climbing from half grass.slope to grass.slope they shorten the same
    grass.edge_fade = number(L"grass.edge.fade", 2.0f, 0.0f, 10.0f);
    grass.edge_low = number(L"grass.edge.height", 25.0f, 5.0f, 100.0f) / 100.0f;
    // grass.cull=1 (the default): a square of tufts wholly outside the camera's view isn't drawn (it's still made and kept, so
    // turning the camera shows it at once); 0 draws every square in reach
    grass.cull = flag(L"grass.cull", true) ? 1 : 0;
    // grass.match (0 to 1, default 0.75): how far the tufts are made the ground's colour under them, lit as the ground is (0 their own)
    grass.match = number(L"grass.match", 0.75f, 0.0f, 1.0f);
    // grass.dry=1 (default 0): the yellow-olive of dry grass (East Sarutabaruta's) counts as grass ground, not only green
    // (outdoors alone, g_grass_outdoors: a Mog House's tan stone floor passed for it)
    grass.dry = flag(L"grass.dry", false) && g_grass_outdoors ? 1 : 0;
    grass.mega = flag(L"grass.mega", false) ? 1 : 0; // Mega dense: every spot of a grass floor (its collision) grows, whatever its colour
    // grass.walkable=1 (the default): tufts only on ground the zone's collision says the player could stand on (path, grass,
    // stone...), not on carts, crates, trees or roofs (grass_floors reads the zone's floors from its DAT)
    grass.walkable = flag(L"grass.walkable", true) ? 1 : 0;
    // grass.nowater=1 (the default): no grass on ground the zone's collision calls shallow water (the game's footsteps splash
    // there: Dragon's Aery's meadow under its waterfalls, Pashhow's marsh), walkable or not; 0 grows it there as on grass
    grass.nowater = flag(L"grass.nowater", true) ? 1 : 0;
    // grass.moss=1 (default 0): low cushions of moss on that shallow water in place of the tufts (with grass.nowater)
    grass.moss = flag(L"grass.moss", false) ? 1 : 0;
    g_grass_floors = grass.enabled && (grass.walkable || grass.nowater);
    // grass.overgrown=1 (an easter egg, default 0): dry grass ground grows on floors the game's footsteps call stone or metal
    // throughout too, as the Nyzul portal in Alzadaal did (its gold is dry grass's colour); 0, only with soft ground under it
    grass.overgrown = flag(L"grass.overgrown", false) ? 1 : 0;
    // grass.shade=1 (the default): in grass ground, the texels in shadow (too dark to read as any colour) and the yellow-olive of
    // dry grass as saturated as dry grass is grow too, not only the green ones (Bhaflau Thickets' forest floors)
    grass.shade = flag(L"grass.shade", true) ? 1 : 0;
    // grass.dry.saturation (0.05 to 1, default 0.25) and grass.dry.hue (1 to 70 degrees, default 30: East Sarutabaruta's pale straw is 28-39): where dry grass's
    // yellow-olive starts, by its least mean saturation over a texture and its reddest hue
    grass.dry_sat = number(L"grass.dry.saturation", 0.25f, 0.05f, 1.0f);
    grass.dry_hue = number(L"grass.dry.hue", 30.0f, 1.0f, 70.0f);
    // grass.pathpad (yalms, 0 to 4; /vana's Path padding): no tuft grows with a path's ground this near it, so paths read wider.
    // The default for the zones data\grass_dirt.json gives no "pathpad" of their own (unset, its top-level one, 0.25); a zone's own
    // always wins (the slider locked there). It follows the zone (track_focus)
    grass.pathpad = grass_assets::pathpad(static_cast<int>(g_grass_zone), read(L"graphics", L"grass.pathpad").empty()
                                                                               ? grass_assets::pathpad_default()
                                                                               : number(L"grass.pathpad", 0.25f, 0.0f, 4.0f));
    // grass.build (ms a frame, default 1.5; -1 no limit): the time a frame spends making squares of tufts, the rest of a
    // square waiting for the next frame, so walking into a field or a ring's upgrade never stalls a frame
    grass.build_ms = number(L"grass.build", 1.5f, -1.0f, 50.0f);
    // grass.wind.speed (0 to 4, default 1): how fast the grass sways, times its own pace (0 stills it);
    // grass.wind.turbulence (0 to 4, default 1): how much the sway differs from place to place (0 a field moving as one)
    grass.wind_speed = number(L"grass.wind.speed", 1.0f, 0.0f, 4.0f);
    grass.wind_turbulence = number(L"grass.wind.turbulence", 1.0f, 0.0f, 4.0f);
    // grass.freecam=1 (the default): while the free camera (/freecam) is on, the grass grows round the ground under it, not round
    // the player, so a flight over a field has grass all the way; 0 keeps it round the player
    g_grass_freecam = flag(L"grass.freecam", true);
    // the zone's dirt level: how colourful its ground has to be to grow grass (higher leaves more of it bare as dirt), VanaCore's
    // own for each zone (data\grass_dirt.json), not a setting; it follows the zone (track_focus)
    grass.dirt = grass_assets::dirt(static_cast<int>(g_grass_zone));
    // grass.blended (unset: the zone's, from the same file; 1 every zone, 0 none): grass-textured ground the game draws blended
    // grows grass too, a layer over other ground (The Sanctuary of Zi'Tah's moss over its soil), as much as its vertices' alpha
    // shows of it. It follows the zone too
    {
        const std::wstring layered = read(L"graphics", L"grass.blended");
        grass.blended = layered.empty() ? (grass_assets::blended(static_cast<int>(g_grass_zone)) ? 1 : 0) : (flag(L"grass.blended", false) ? 1 : 0);
    }
    // the zone's own grass ground (the same file's "ground"): textures that are grass there whatever their colours say (The
    // Sanctuary of Zi'Tah's dark leaf-litter floor, taken for a plant's cut-outs by its darkness)
    const std::string ground = grass_assets::ground(static_cast<int>(g_grass_zone));
    grass.ground = ground.c_str();
    // ... and what's never grass there ("not_grass": the zone's, and every zone's, as the Mog Houses' floors)
    const std::string never = grass_assets::not_ground(static_cast<int>(g_grass_zone));
    grass.not_ground = never.c_str();
    vanacore_gfx_set_grass(&grass);
    // the catalogue (addons\vanacore\vana\grass and the player's config\grass): its pictures, read once, and grass.asset.<id>
    // (on or off) and grass.asset.<id>.amount (its share of the mix) each time
    grass_assets::apply(g_ini, g_dir);
}
