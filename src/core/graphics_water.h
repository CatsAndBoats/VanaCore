// VanaCore's water: [graphics] water* read into a VanaWater for the graphics (vanacore_gfx_set_water), from apply_live. An include
// unit of graphics.cpp (its readers and globals are in scope): it was apply_live's longest block.

static void read_water(When when)
{
    // [graphics] water=0|1 (off by default): VanaCore's own water in place of the game's. water.waves (how steep: 1 as made, 0 flat), water.speed,
    // water.size (the waves' length), water.reflection (the sky in it), water.glint (the sun's and moon's), water.brightness, water.colour (0 blue,
    // 0.5 teal, 1 green) or water.tint (#RRGGBB: the shallows' colour), water.shallowness (the lighter shallow colour), water.turbulence, water.direction (degrees), water.glint.size
    VanaWater water{};
    water.enabled = flag(L"water", false) ? 1 : 0;
    water.waves = number(L"water.waves", 1.0f, 0.0f, 4.0f);
    water.speed = number(L"water.speed", 1.0f, 0.0f, 5.0f);
    water.size = number(L"water.size", 1.0f, 0.25f, 4.0f);
    water.reflection = number(L"water.reflection", 1.0f, 0.0f, 3.0f);
    water.glint = number(L"water.glint", 1.0f, 0.0f, 4.0f);
    water.brightness = number(L"water.brightness", 1.0f, 0.1f, 4.0f);
    water.colour = number(L"water.colour", 0.5f, 0.0f, 1.0f);
    water.shallowness = number(L"water.shallowness", 1.0f, 0.0f, 3.0f);
    water.turbulence = number(L"water.turbulence", 1.0f, 0.0f, 4.0f);
    water.direction = number(L"water.direction", 0.0f, 0.0f, 360.0f);
    water.glint_size = number(L"water.glint.size", 1.0f, 0.2f, 4.0f);
    // the water pass (d3d8_water.h): water.pass=1 (the default) draws the water after the world over copies of the scene, the bed
    // through it by its depth (water.depth, yalms), bent by the waves (water.refraction), a soft edge and foam at the shore (water.shore,
    // yalms; water.foam); 0 draws it in its place, opaque, as before. water.ripples, water.ripple.size: the fine ripples over the swell
    water.pass = flag(L"water.pass", true) ? 1 : 0;
    water.clarity = number(L"water.depth", 4.0f, 0.5f, 20.0f);
    water.refraction = number(L"water.refraction", 1.0f, 0.0f, 3.0f);
    water.shore = number(L"water.shore", 0.5f, 0.0f, 3.0f);
    water.foam = number(L"water.foam", 1.0f, 0.0f, 3.0f);
    water.ripples = number(L"water.ripples", 1.0f, 0.0f, 3.0f);
    water.ripple_size = number(L"water.ripple.size", 1.0f, 0.25f, 4.0f);
    const std::uint32_t tint = colour(L"water.tint");
    water.has_tint = tint != nameplates::kGameColour ? 1 : 0;
    water.tint = water.has_tint ? tint : 0;
    // water.detect: what is the water - 0 the flat tiles found by their shape, 1 the game's water models (water.cpp: the vertex buffers
    // made while the game loaded a model under a water directory of the zone's DAT), 2 either (the default). [graphics.effects] debug=11
    // is its debug view, the water found drawn in colour; water.log=1 logs every map model the game loads with the directories above it
    water.detect = static_cast<int>(number(L"water.detect", 2.0f, 0.0f, 2.0f) + 0.5f);
    const std::wstring view = effect_enabled(L"debug") ? read(L"graphics.effects", L"debug") : std::wstring();
    water.view = !view.empty() && std::lround(std::wcstof(view.c_str(), nullptr)) == 11 ? 1 : 0;
    water.probe = static_cast<int>(number(L"water.probe", 0.0f, 0.0f, 3.0f)); // what the pass reads from the scene (never saved)
    water::set_log(flag(L"water.log", false));
    water.log = flag(L"water.log", false) ? 1 : 0;
    water.horizon = flag(L"water.horizon", false) ? 1 : 0; // experimental: the sea drawn on past the zone's edge to the horizon
    // the zone's floors: the sheets are told by the footsteps' water (grass_floor_near, water.detect=2); the sea to the horizon needs
    // the floors' box to tell the sea's tiles (at the zone's rim) from a marsh's, whatever water.detect is (d3d8_water.h sea_note)
    g_water_floors = water.enabled != 0 && (water.detect >= 2 || water.horizon != 0 || water.sea != 0);
    water.falls = number(L"water.falls", 1.0f, 0.0f, 3.0f);    // a waterfall's churn and foam on the water it lands in
    water.caustics = number(L"water.caustics", 1.0f, 0.0f, 3.0f); // the rippling light on the bed in shallow water
    water.relief = number(L"water.relief", 1.0f, 0.0f, 3.0f);     // the wavelets lit as slopes, the refraction with them
    water.sea = flag(L"water.sea", true) ? 1 : 0;                // the game's sea tiles replaced by VanaCore's one sea plane
    water.shadows = flag(L"water.shadows", false) ? 1 : 0;        // the sun's shadows on the water (off: none fall on it)
    // the world mirrored in the water (d3d8_water_mirror.h): water.mirror its strength (0 off), water.mirror.scale the mirror's size
    // as a share of the scene's each way, water.mirror.reach how far from the camera what's mirrored reaches (yalms, 0 all of it),
    // water.mirror.waves how far the waves push it about
    water.mirror = number(L"water.mirror", 1.0f, 0.0f, 2.0f);
    water.mirror_scale = number(L"water.mirror.scale", 0.5f, 0.25f, 1.0f);
    water.mirror_reach = number(L"water.mirror.reach", 300.0f, 0.0f, 2000.0f);
    water.mirror_waves = number(L"water.mirror.waves", 1.0f, 0.0f, 4.0f);
    water::set_wanted(water.detect != 0 || water.view != 0 || flag(L"water.log", false)); // the game's model loading is hooked once something wants its water
    static int said_detect = 0;
    if (water.detect != said_detect && when != When::Quiet)
        log::write(water.detect == 1   ? L"Graphics: the water is the game's water models (water.detect=1)"
                   : water.detect == 2 ? L"Graphics: the water is the flat tiles found by their shape and the game's water models "
                                         L"(water.detect=2)"
                                       : L"Graphics: the water is the flat tiles found by their shape (water.detect=0)");
    said_detect = water.detect;
    static bool said = false;
    if ((water.enabled != 0) != said && when != When::Quiet)
        log::write(water.enabled ? L"Graphics: VanaCore's water on (water)" : L"Graphics: the game's water (water=0)");
    said = water.enabled != 0;
    water::read_zones(g_dir); // the zones it's off in (data\water_zones.json), before the settings take the zone's mode
    vanacore_gfx_set_water(&water);
}
