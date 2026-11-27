// VanaCore's weathers other than rain: [graphics] weather* read into a VanaWeather for the graphics (vanacore_gfx_set_weather_fx,
// d3d8_weather.h), from apply_live. An include unit of graphics.cpp (its readers and globals are in scope), as graphics_rain.h is.

static bool g_weather_shelter = false; // the weathers want the zone's collision for the rain's shelter map (track_focus)
static int g_weather_preview = -1;     // weather.preview: the weather shown whatever the server's, -1 none
static bool g_weather_borrow = true;   // weather.sky: a weather the zone has no look for borrows another zone's (track_weather_look)

static void read_weather(When when)
{
    // [graphics] weather=0|1 (on by default): particles in the air in the server's weathers other than rain, in a zone whose model
    // DAT has that weather: embers in hot spells and heat waves, grit and dust clouds in dust and sand storms, leaves in wind and
    // gales, snow and blizzards, motes of light in auroras and stellar glare, ash in gloom and darkness; none under roofs (the
    // rain's shelter, rain.shelter). weather.<kind> (0 none to 3, 1 as made) how much of each.
    VanaWeather wx{};
    wx.on = flag(L"weather", true) ? 1 : 0;
    static const wchar_t* const kinds[6] = {L"weather.fire", L"weather.dust", L"weather.wind", L"weather.snow", L"weather.light", L"weather.dark"};
    for (int i = 0; i < 6; ++i)
        wx.amount[i] = number(kinds[i], 1.0f, 0.0f, 3.0f);
    // weather.preview=<0x057's id> (/vana's Weather preview, this session only: ini::session_only): that weather's particles
    // whatever the server's, -1 none. The game's own sky and light stay the zone's (/freecam weather changes those)
    wx.preview = static_cast<int>(number(L"weather.preview", -1.0f, -1.0f, 19.0f));
    // weather.wind.direction (0 to 360 degrees in the ground plane, 40 as made) the way the particles blow; weather.wind.drift=1
    // (on) lets it wander some 45 degrees either side over minutes, 0 holds it; weather.wind.strength (0 still to 3) times each
    // kind's own wind
    wx.wind_dir = number(L"weather.wind.direction", 40.0f, 0.0f, 360.0f);
    wx.wind = number(L"weather.wind.strength", 1.0f, 0.0f, 3.0f);
    wx.drift = flag(L"weather.wind.drift", true) ? 1 : 0;
    // weather.wind.fixed=1 blows every weather along weather.wind.direction; 0 (as made) each weather as it comes, and each
    // zone, picks a heading of its own
    wx.fixed = flag(L"weather.wind.fixed", false) ? 1 : 0;
    // Randomise on zone in (weather.random, off; vana\weatherpreview.lua rolls at each zone-in, weather.random.chance percent
    // of the time): the roll's weather shown as a preview's is (a Weather preview picked wins), its particles times
    // weather.roll.amount, its wind times weather.roll.wind along weather.roll.dir. The weather.roll.* keys are this session's only
    const int roll = static_cast<int>(number(L"weather.roll.id", -1.0f, -1.0f, 19.0f));
    if (wx.preview < 0 && roll >= 0 && flag(L"weather.random", false)) {
        wx.preview = roll;
        const float more = number(L"weather.roll.amount", 1.0f, 0.0f, 3.0f);
        for (float& a : wx.amount)
            a = std::min(a * more, 3.0f);
        wx.wind = std::min(wx.wind * number(L"weather.roll.wind", 1.0f, 0.0f, 3.0f), 3.0f);
        wx.wind_dir = number(L"weather.roll.dir", 0.0f, 0.0f, 360.0f), wx.fixed = 1;
    }
    g_weather_shelter = wx.on != 0;
    g_weather_preview = wx.preview;
    // weather.sky=0|1 (on): a weather the zone's files have no sky, fog and light for (a Weather preview, or the server's) takes
    // another zone's that has it (zoneweather::look): its sky and fog colour, and its fog's distance and its light as they change
    // there from a clear sky
    g_weather_borrow = flag(L"weather.sky", true);
    static VanaWeather was{};
    static bool said = false;
    if (!said || std::memcmp(&was, &wx, sizeof wx) != 0) {
        if (when != When::Quiet)
            log::write(L"Graphics: the weathers %ls (fire %.2f, dust %.2f, wind %.2f, snow %.2f, light %.2f, dark %.2f; the wind %.0f degrees%ls, strength %.2f)%ls",
                       wx.on ? L"on" : L"off", wx.amount[0], wx.amount[1], wx.amount[2], wx.amount[3], wx.amount[4], wx.amount[5],
                       wx.wind_dir, wx.fixed ? (wx.drift ? L" fixed, drifting" : L" fixed") : L" each weather's own", wx.wind, wx.preview >= 0 ? L", previewing one" : L"");
        was = wx, said = true;
    }
    vanacore_gfx_set_weather_fx(&wx);
}

// The look of a weather the zone has none of (a Weather preview, or a server's weather its files lack), borrowed from a zone
// that has it (zoneweather::look, read in the background) and handed over each frame (vanacore_gfx_set_weather_look,
// d3d8_weather_look.h); none in the meantime or where it isn't wanted, the last one fading out. From track_rain_weathers.
static void track_weather_look(unsigned zone)
{
    std::uint32_t server = 0, weather = 0;
    windower::state_world(server, weather);
    const int shown = g_weather_preview >= 0 ? g_weather_preview : server != 0 ? static_cast<int>(weather) : -1;
    double minute = vanaclock::server_minute(server);
    vanaclock::held_minute(minute);
    zoneweather::Look look;
    VanaWeatherLook out{};
    if (g_weather_borrow && shown >= 0 && zoneweather::look(static_cast<int>(zone), shown, minute, look)) {
        out.active = 1;
        std::memcpy(out.fog, look.fog, sizeof out.fog);
        out.fog_ratio = look.fog_ratio;
        std::memcpy(out.amb_t, look.amb_t, sizeof out.amb_t), std::memcpy(out.sun_t, look.sun_t, sizeof out.sun_t);
        std::memcpy(out.amb_m, look.amb_m, sizeof out.amb_m), std::memcpy(out.sun_m, look.sun_m, sizeof out.sun_m);
        out.sky = look.sky ? 1 : 0;
        std::memcpy(out.slice, look.slice, sizeof out.slice), std::memcpy(out.elev, look.elev, sizeof out.elev);
    }
    static int said = -2;
    const int now = out.active ? shown : -1;
    if (now != said) {
        said = now;
        if (now >= 0)
            log::write(L"Graphics: weather %d has no look in zone %u's files: its sky, fog and light borrowed from zone %d's", now, zone,
                       zoneweather::look_donor(now));
    }
    vanacore_gfx_set_weather_look(&out);
}
