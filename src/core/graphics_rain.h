// VanaCore's rain on the characters: [graphics] rain* read into a VanaRain for the graphics (vanacore_gfx_set_rain), from apply_live.
// An include unit of graphics.cpp (its readers and globals are in scope), as graphics_water.h is.

static bool g_rain_shelter = false; // VanaCore's rain wants the zone's floors and its collision (track_focus: grass_floors::update, update_collision)

static void read_rain(When when)
{
    // [graphics] rain.wet=0|1 (on by default): the characters drawn wet while it rains - darker and glossier, the sky in them
    // (rain.sheen, 0 to 3), the drops seen landing on them, rings spreading on what faces up and runnels down their sides
    // (rain.drops, 0 to 3), drying off over rain.dry seconds once it stops. rain.force=1 is as if it rained, whatever the weather; 2 that and
    // the look's parts shown as colours (for checking its geometry).
    VanaRain rain{};
    rain.wet = flag(L"rain.wet", true) ? 1 : 0;
    rain.sheen = number(L"rain.sheen", 1.0f, 0.0f, 3.0f);
    rain.drops = number(L"rain.drops", 1.0f, 0.0f, 3.0f);
    rain.dry = number(L"rain.dry", 45.0f, 1.0f, 600.0f);
    rain.force = static_cast<int>(number(L"rain.force", 0.0f, 0.0f, 4.0f)); // 1 as if it rained; 2 that and the look's parts as colours
    // [graphics] rain=0|1 (on): VanaCore's own rain - streaks the world and the characters hide, splashes where the drops land
    // on what faces up with a crown rising off it, the ground's wet sheen; rain.density (0.2 to 3) how much falls, rain.splash
    // (0 to 3), rain.shelter=1 keeps it out from under roofs, bridges and caves (the zone's collision, read as the collision
    // view's is), rain.thunder=1 rains in thunder weather too, as a look (the server's weather stays thunder), in a zone that
    // has rain of its own (track_rain_weathers)
    rain.rain = flag(L"rain", true) ? 1 : 0;
    rain.density = number(L"rain.density", 1.0f, 0.2f, 3.0f);
    rain.splash = number(L"rain.splash", 1.0f, 0.0f, 3.0f);
    rain.shelter = flag(L"rain.shelter", true) ? 1 : 0;
    rain.thunder = flag(L"rain.thunder", true) ? 1 : 0;
    rain.game = flag(L"rain.game", false) ? 1 : 0; // 1 keeps the game's own faint rain drawn under VanaCore's
    rain.lens = number(L"rain.lens", 0.5f, 0.0f, 3.0f); // droplets on the lens while the rain reaches the camera, 0 none
    rain.spatter = number(L"rain.spatter", 1.0f, 0.0f, 3.0f); // how big the spatter thrown off the characters is, 0 none
    rain.lens_on = flag(L"rain.lens.on", true) ? 1 : 0; // the lens's droplets at all (rain.lens kept as it is when off)
    rain.ground = flag(L"rain.ground", true) ? 1 : 0;   // the wet ground and the puddles
    rain.range = number(L"rain.range", 48.0f, 8.0f, 500.0f); // Splash Range: how far round the camera the drops splash, yalms
    rain.freeze = flag(L"rain.freeze", false) ? 1 : 0;  // /vana debug's Freeze rain: the rain held still (never saved: ini::session_only)
    rain.reflect = number(L"rain.reflect", 1.0f, 0.0f, 2.0f); // the wet ground mirroring the scene (Direct3D 12's reflections pass), 0 none
    rain.puddles = number(L"rain.puddles", 1.0f, 0.0f, 2.0f); // ... its puddles in the low ground and the corners, 0 none
    // how far round the camera the rain falls at all, yalms, a fifth fewer drops each 100 yalms out (d3d8_rain.h)
    rain.reach = number(L"rain.far", 150.0f, 52.0f, 500.0f); // Rain Range
    // Debug view 31 (/vana debug's Rain): the rain red over the picture in grey, where it rains (not forced: Preview rain does); [graphics.effects] debug is
    // never saved (ini::session_only)
    const std::wstring dview = effect_enabled(L"debug") ? read(L"graphics.effects", L"debug") : std::wstring();
    // ... and 32 (Rain rings): the picture tinted by the rain's 100-yalm bands, its splash reach and reach as lines
    const long dv = dview.empty() ? 0 : std::lround(std::wcstof(dview.c_str(), nullptr));
    rain.view = dv == 31 ? 1 : dv == 32 ? 2 : 0;
    // the rain is under the Water Shader heading in /vana: [graphics] water=0 (its switch off) turns it off with the water, the
    // rain, the characters' wet look, the wet ground and the lens's drops (their own settings kept as they are)
    if (!flag(L"water", false))
        rain.rain = 0, rain.wet = 0, rain.ground = 0, rain.lens_on = 0, rain.force = 0, rain.view = 0;
    g_rain_shelter = rain.rain != 0; // the drops want the zone's floors to land on; the shelter its collision as well
    static VanaRain was{};
    static bool said = false;
    if (!said || std::memcmp(&was, &rain, sizeof rain) != 0) {
        if (when != When::Quiet)
            log::write(L"Graphics: the rain %ls (density %.2f, splash %.2f, spatter %.2f, shelter %ls, thunder %ls); the characters' wet look %ls (sheen %.2f, drops %.2f, dry %.0f s)%ls",
                       rain.rain ? L"on" : L"off", rain.density, rain.splash, rain.spatter, rain.shelter ? L"on" : L"off", rain.thunder ? L"on" : L"off",
                       rain.wet ? L"on" : L"off", rain.sheen, rain.drops, rain.dry, rain.force ? L", forced" : L"");
        was = rain, said = true;
    }
    vanacore_gfx_set_rain(&rain);
}

// The weathers the zone's model DAT has (zoneweather), read once a zone and handed over each frame (vanacore_gfx_set_zone_weathers),
// from track_focus: VanaCore's rain falls only where the zone has rain or squall of its own, in the server's rain as in thunder
// weather (rain.thunder). Oldton and Newton Movalpolis have thunder and no rain, and it rained in their mines (a player,
// 2026-11-20); Phomiuna Aqueducts is given rain by the server and has none (2026-11-21). A rain or squall folder counts only
// when it carries the rain itself (zoneweather::rains): Alzadaal Undersea Ruins' squall is a sky and light alone, and it
// rained in its halls (a player, 2026-11-22), as in the server's rain in some 15 other indoor zones (Uggalepih, Crawlers'
// Nest, the Boyahda Tree, Ordelle's Caves...). Bit 31 marks the DAT read, so a zone left with no weather bits is still known.
static void track_rain_weathers(unsigned zone)
{
    static unsigned looked = ~0u;
    static std::uint32_t mask = 0;
    if (zone != looked) {
        looked = zone;
        std::uint32_t sky = 0, rain = 0;
        zoneweather::weathers(static_cast<int>(zone), sky);
        zoneweather::rains(static_cast<int>(zone), rain);
        constexpr std::uint32_t kWet = 1u << 6 | 1u << 7; // rain, squall
        mask = sky ? (sky & ~kWet) | (rain & kWet) | 0x80000000u : 0;
    }
    vanacore_gfx_set_zone_weathers(mask);
}
