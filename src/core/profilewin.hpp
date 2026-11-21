#pragma once

// The Status screen's Profile panel ("menu    profile": Status > Profile, beside the Status window: Nation, Race, Home
// Point, Rank and its points, Mastery Rank, Mentor, and Monstrosity's Gladiator Rank and Prestige):
//   profile.title.color    its two title bars ("Profile", "Monstrosity": its picture's "menu    gauge" quads, which the
//                          game adds over the frame, light blue to pink at 38%) drawn this colour, "#RRGGBB", blended over
//                          the frame; empty, the game's. The title bars of Status > Job Levels, Combat Skills,
//                          Magic Skills and Craft Skills too (profilewin.cpp kTitled)
//   profile.title.opacity  their opacity, 0 to 100; unset, the game's (38) for the game's colour, 100 for one picked
//   profile.clean=1        laid out again: every label at the left with its colon after its last letter, a row each 15
//                          apart (Rank Points and Mentor on rows of their own), every value from one column
//   craft.brackets=0       Status > Craft Skills' levels without their brackets: "60" for "(60)" (its sprintf's format)
// Its letters' own True Font, profile.font, is chatfont.cpp's; its titles' (profile.title.font, profile.font's keys until set)
// too, their letters switched to it here (TitleQuad), and the titles of Job Levels and the three Skills windows with them.
namespace vanacore::profilewin {

// `ini` (vanacore.ini): [client] profile.* read now, and again by reload whenever vanacore.ini changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Around chat.cpp's draw of each window: craft.brackets put in step first (the craft panel's format); for the Profile panel, its picture changed for that one draw (the title bars'
// colour, the labels' places) and put back as it ends, and the panel's own drawing of its values put in step with
// profile.clean (its code patched, or put back) before it runs. Any other window: nothing.
class Drawing {
public:
    explicit Drawing(const void* ctrl);
    ~Drawing();
    Drawing(const Drawing&) = delete;
    Drawing& operator=(const Drawing&) = delete;

private:
    bool changed_ = false;
};

// Around itemslot.cpp's draw of each quad of a menu picture: a letter of the Profile panel's titles, or of a kTitled window's
// title (noted by Drawing as the window is drawn), drawn in the titles' True Font area (VANA_FONT_PROFILE_TITLE), the area before put back after
class TitleQuad {
public:
    explicit TitleQuad(const char* quad);
    ~TitleQuad();
    TitleQuad(const TitleQuad&) = delete;
    TitleQuad& operator=(const TitleQuad&) = delete;

private:
    int was_ = -1;
};

// The titles' True Font is on: itemslot.cpp hooks the quads' draw for it
bool wants_quads();

} // namespace vanacore::profilewin
