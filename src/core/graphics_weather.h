// VanaCore's weathers other than rain: [graphics] weather* read into a VanaWeather for the graphics (vanacore_gfx_set_weather_fx,
// d3d8_weather.h), from apply_live. An include unit of graphics.cpp (its readers and globals are in scope), as graphics_rain.h is.

static bool g_weather_shelter = false; // the weathers want the zone's collision for the rain's shelter map (track_focus)

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
    g_weather_shelter = wx.on != 0;
    static VanaWeather was{};
    static bool said = false;
    if (!said || std::memcmp(&was, &wx, sizeof wx) != 0) {
        if (when != When::Quiet)
            log::write(L"Graphics: the weathers %ls (fire %.2f, dust %.2f, wind %.2f, snow %.2f, light %.2f, dark %.2f)%ls",
                       wx.on ? L"on" : L"off", wx.amount[0], wx.amount[1], wx.amount[2], wx.amount[3], wx.amount[4], wx.amount[5],
                       wx.preview >= 0 ? L", previewing one" : L"");
        was = wx, said = true;
    }
    vanacore_gfx_set_weather_fx(&wx);
}
