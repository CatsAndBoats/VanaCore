#pragma once

#include <cstdint>
#include <string>

// The names over characters' heads, as FFXiMain.dll draws them: their shape in any window
// ([graphics] nameplates), the spacing of their letters ([graphics] nameplates.spacing), whose are drawn,
// their icons, size and colours ([graphics] nameplates.*), or their letters as VanaCore's own text
// ([graphics] nameplates.font=sharp).
namespace vanacore::nameplates {

// A colour left as the game has it
constexpr std::uint32_t kGameColour = 0xFFFFFFFF;

// The job master's three stars (a big one between two small ones), as the game draws them, only the big one, or three
// small ones
enum class StarStyle { game, one, tiny };

struct Names {
    bool hide_own = false;     // the name over the local player's head left out
    bool hide_players = false; // other players' names left out (NPCs' and monsters' stay)
    // The same for the rest, by the colour the game gives their name: NPCs (the green ones), monsters (unclaimed, claimed
    // by someone else, by you or your party), trusts, and players' pets
    bool hide_npcs = false;
    bool hide_monsters = false;
    bool hide_trusts = false;
    bool hide_pets = false;
    // Your party's other members' names (players' and trusts') left out, and the alliance's other two parties' (for an addon
    // that draws them in its own party bars); the rest go by the settings above
    bool hide_party = false;
    bool hide_alliance = false;
    bool hides() const
    {
        return hide_own || hide_players || hide_npcs || hide_monsters || hide_trusts || hide_pets || hide_party ||
               hide_alliance;
    }
    bool bazaar = true;        // the bazaar bag before a player's name
    bool linkshell = true;     // the linkshell's pearl before it
    bool mastery = true;       // the job master's stars under it
    bool gm = true;            // a staff member's GM icon as CatsEyeXI's picture of it (gmicons.hpp), not the game's
    float gm_size = 1.25f;     // ... times the game's icon's height
    float gm_x = 0.0f;         // ... moved from beside the name, in the game's pixels at size 1 (as the pearl's)
    float gm_y = -2.0f;
    StarStyle star_style = StarStyle::game; // how they're drawn
    float size = 1.0f;         // the names' size, times the game's
    // Names sized smoothly by their distance: the game sizes them in 80 whole steps of it, so walking up to someone
    // or zooming, their name grows a step at a time
    bool smooth = true;
    // How much of the game's shrinking with distance names keep: 1 the game's, 0 none (a name far off as big as one
    // beside you, till the game stops drawing it)
    float distance = 1.0f;
    // Nearer names drawn over farther ones: the game draws them in its list's order, so a far name could be over a near
    // one's letters
    bool order = true;
    // The linkshell's pearl moved from where the game draws it, in the game's pixels at size 1 (x: right, y: down):
    // x above 0 closes the gap between the pearl and the name.
    float pearl_x = 38.0f;
    float pearl_y = -1.0f;
    // The pearl (and the bazaar bag, which takes its place) drawn this many times its size, about its middle
    float pearl_size = 0.45f;
    // The bazaar bag moved from where the pearl's settings put it (the same units), and this many times the pearl's size
    float bazaar_x = 4.0f;
    float bazaar_y = -7.0f;
    float bazaar_size = 1.0f;
    // The pearl's and the bag's width over their height on the screen, whatever the window's shape (the game's art is
    // narrowed for a stretch to 16:9); 0 leaves them as the game draws them, stretching with the window. Applies while
    // VanaCore shapes the names (the shape fix, spacing or True Font), not with nameplates=stretched and the game's
    // letters.
    float pearl_shape = 16.0f / 9.0f;
    // The name's letters centred on the point over the head: the game centres its icon and the space after it with the
    // letters, so a name in a linkshell sits right of centre by half of them. Centred, the letters are over the point
    // and the icon hangs to their left.
    bool centre = true;
    // Centring and every move below (and the pearl's) apply to players' names only: NPCs', monsters', trusts' and pets'
    // stay as the game draws them
    bool characters_only = true;
    // The whole plate (icon, letters and stars) moved, then each part by itself from there; the same units as the pearl's
    float plate_x = 0.0f;
    float plate_y = 0.0f;
    float name_x = 0.0f;
    float name_y = 0.0f;
    float stars_x = 1.0f;
    float stars_y = 20.0f;
    float stars_spacing = 0.40f; // the stars that share of the game's distance apart, about the line's middle (nameplates_stars.h)
    float stars_size = 0.65f;    // ... and this many times their size, the line grown about its middle
    bool blink = true;          // the target's name pulses, as the game has it (false: it stays steady)
    // Names' colours, 0xRRGGBB (kGameColour: the game's): players' (white), NPCs' (green), unclaimed
    // monsters' (yellow), monsters claimed by someone else (purple), by you or your party (red). Party
    // members' and the rest stay the game's.
    std::uint32_t players = kGameColour;
    std::uint32_t npcs = kGameColour;
    std::uint32_t monsters = kGameColour;
    std::uint32_t claimed = kGameColour;
    std::uint32_t mine = kGameColour;
    std::uint32_t trusts = kGameColour; // trusts' names, whatever colour the game gives them
    std::uint32_t pets = kGameColour;   // players' pets' names, the same
    // The letters as VanaCore's text, at the screen's pixels (the icons and stars stay the game's): in this font (its own,
    // else True Font's default font), bold or not, italic or not, in capitals or not, font_size the interface's pixels at
    // a name's full size (as near as the game draws one, Size 1.00x: as big as a True Font area's letters of that size),
    // this much more between letters (in twentieths of their size; below 0, closer). The shape and spacing settings
    // don't apply to them.
    bool sharp = false;
    std::wstring family = L"Segoe UI";
    bool bold = true;
    bool italic = false;
    bool upper = false;
    float font_size = 18.0f;
    float text_spacing = 0.0f;
    // Set to some seconds (changed while the game runs, or at its start): each of those letters' names, every frame for
    // that long, its size and place into logs\nameplates-trace-<time>.csv
    float trace = 0.0f;
    // With frame generation (Direct3D 12): each name's draws carry its character (vanacore_gfx_label), so the frames it
    // makes draw the name where it would be between two of the game's, not where the game's next frame has it
    bool framegen = true;
};

// Each frame, on the game's thread. fix keeps names at the shape they have in a 4:3 window, in a
// window of `shape` (width over height; 0 while it has none). Without `spaced` their letters are spaced
// as the game does, a cell each; with it, by each letter's own width, with `gap` of the font sheet's
// pixels between the letters' cores (below 0, the cores' outlines overlapping: for a font whose cells
// are already close). With names.sharp, neither fix nor spaced: the letters are VanaCore's.
void apply(bool fix, bool spaced, float gap, float shape, const Names& names);

// Where the name over a target (by its target index) was drawn lately: the middle of its letters and their top and bottom,
// in the game's pixels (its drawing target's, as the quads' own vertices have them). False when its name wasn't drawn in the
// last moments, or the names aren't VanaCore's to see (they're noted at the letters' draw, which is found with the
// nameplates settings). For the numbers over characters to stay near their target's name.
bool name_anchor(unsigned index, float& x, float& top, float& bottom);
// Whether any name has been seen lately
bool anchoring();

// Where the name just drawn is, for widgets.hpp (drawn at it, or laid out and left undrawn when it's hidden): the middle of
// its letters, their left and right, top and bottom, in the game's pixels (the units of the quads' own vertices), their depth
// and rhw, and the size it's drawn at (the game's for its distance, times nameplates.size)
struct Slot {
    float x = 0.0f, left = 0.0f, right = 0.0f, top = 0.0f, bottom = 0.0f;
    float z = 0.0f, rhw = 1.0f;
    float size = 1.0f;
};
// What widgets.hpp draws a quad with in the names' pass: the device the names are drawn on, the game's pixels, and how many
// of the scene's shown pixels one of them is. False until the names' draw is hooked and the game's renderer found.
struct Pass {
    void* device = nullptr;
    float kx = 0.0f, ky = 0.0f;
    float game_w = 0.0f, game_h = 0.0f;
};
bool pass(Pass& out);

} // namespace vanacore::nameplates
