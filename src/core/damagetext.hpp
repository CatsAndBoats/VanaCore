#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// The numbers and "Miss!" FFXiMain.dll floats over characters when they're hit: as VanaCore's own text
// ([graphics] damage.font=sharp), critical hits picked out ([graphics] damage.crit*), where they appear
// (damage.spawn, damage.offset.*, damage.spread), their colours (damage.color, damage.miss.color,
// damage.taken.color, damage.heal.color; each kind's gradient, <kind>.gradient to <kind>.color2, and the outline's, damage.outline.color), the hits of a multi-hit round (damage.multi*) and the skill's name (damage.label*), a preview
// of them on the screen ([graphics] damage.preview), over everything ([graphics] damage.ontop), and how long they stay up
// ([graphics] damage.duration).
namespace vanacore::damagetext {

constexpr std::uint32_t kGameColour = 0xFFFFFFFF; // a colour setting left to the game's
constexpr float kGameDuration = 2.5f;              // seconds a number stays up in the game: 150 frames of 60 a second
constexpr float kMinDuration = 0.5f, kMaxDuration = 10.0f;

// Where a number first appears next to where the game puts it: there; stacked one above the next when several come
// for a target at once; or anywhere within `spread` of it.
enum class Spawn { Game, Stacked, Random };

// What marks a number of a multi-hit round
enum class MultiIcon { Swords, Count };

struct Settings {
    bool sharp = false;             // VanaCore's text in place of the game's letters
    std::wstring family = L"Segoe UI";
    bool bold = true;
    bool italic = true;
    float text_size = 1.0f;         // times the game's letters' height
    float outline = 1.0f;           // the outline's width, times a pixel at 12 pixels high
    float min = 0.0f;               // the text's size kept to at least this, in the screen's pixels (0: no limit)
    float max = 0.0f;               // and at most this (0: no limit)
    bool commas = false;            // 1,337 for 1337
    bool miss_caps = false;         // MISS! for Miss!
    bool crit = true;               // critical hits in their own colour and size
    std::uint32_t crit_colour = 0xFFD040; // 0xRRGGBB
    float crit_size = 1.3f;         // times the text's size (sharp only)
    bool crit_exclaim = false;      // 1200!! for a critical 1200 (sharp only)
    bool preview = false;           // sample numbers on the screen, as they'd be drawn (sharp only)
    bool on_top = true;             // drawn over everything, characters in front of the target and names too
    float duration = kGameDuration; // seconds a number stays up, rising as far as the game's over it
    // Where: in digit heights (the number's own height, so it scales with distance), x to the right, y up
    Spawn spawn = Spawn::Game;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float spread = 1.0f;            // Random: how far from the game's spot at most; Stacked: the gap between numbers
    // Colours, 0xRRGGBB (kGameColour: the game's): hits you deal, misses, hits on you, HP recovered (a heal is none of
    // the others: hit and taken colours leave it the game's). A critical hit's wins.
    std::uint32_t hit_colour = kGameColour;
    std::uint32_t miss_colour = kGameColour;
    std::uint32_t taken_colour = kGameColour;
    std::uint32_t heal_colour = kGameColour;
    bool heal_plus = false;         // +530 for HP recovered (sharp only)
    // The numbers' look in the picture itself (sharp only): each kind's gradient, a fill from its colour (the one it's drawn
    // in while that's the game's) to its second colour, top to bottom or (gradient_across) left to right; a hit on you
    // with neither a colour nor a gradient of its own takes a hit's. And the outline's colour (0x000000 the dark outline as
    // before). Either set, each number is drawn in its own colours untinted, its game colour (or the one picked) baked
    // into its picture.
    bool hit_gradient = false, miss_gradient = false, taken_gradient = false, heal_gradient = false, crit_gradient = false;
    std::uint32_t hit_second = 0xC81E1E, miss_second = 0x2E9E4A, taken_second = 0x9AA4B4, heal_second = 0x2EA860,
                  crit_second = 0xFF6A00;
    bool gradient_across = false;
    std::uint32_t outline_colour = 0x000000;
    // A round of several hits on one target (double attack, a weapon skill's hits): an icon beside each number of it
    // (sharp only), crossed swords or the hits' count (x2)
    bool multi = false;
    MultiIcon multi_icon = MultiIcon::Swords;
    float multi_size = 0.6f;        // the icon's size, times the number's
    // The weapon skill's, spell's or ability's name above the number it did (sharp only)
    bool label = false;
    float label_size = 0.5f;        // times the number's size

    // Whether the pictures carry their colours (a gradient or an outline colour), drawn untinted
    bool baked() const { return sharp && (any_gradient() || outline_colour != 0x000000); }
    bool any_gradient() const { return hit_gradient || miss_gradient || taken_gradient || heal_gradient || crit_gradient; }
    // Whether anything needs the game's drawing taken over
    bool wants_hook() const
    {
        return sharp || crit || on_top || spawn != Spawn::Game || offset_x != 0.0f || offset_y != 0.0f ||
               duration != kGameDuration || hit_colour != kGameColour || miss_colour != kGameColour || taken_colour != kGameColour ||
               heal_colour != kGameColour;
    }
    // and whether the server's actions are wanted: which hits were critical, one of several, from which skill, or heals
    bool wants_actions() const
    {
        return crit || hit_colour != kGameColour || taken_colour != kGameColour || heal_colour != kGameColour ||
               (sharp && (multi || label || heal_plus));
    }
};

// Each frame, on the game's thread: hooks the game's drawing the first time it's wanted.
void apply(const Settings& settings);

// Each buffer the game receives, as it gets it: the server's actions (0x028), to know which hits were critical.
void on_received(const std::uint8_t* buffer, std::size_t size);

// In Present, between text::begin and text::end on VanaCore's interface (`width` x `height`): the preview, and the
// test hits' numbers.
void draw_preview(void* device, unsigned width, unsigned height);

// The /vana buttons that show how the numbers are placed: VanaCore makes numbers itself (the game's come only with a real
// hit's animation) over the player, placed by the spawn style and offsets, with the colours, icon and skill name the
// settings give: one hit, a round of three hits of a weapon skill, or a barrage of hits over two seconds.
enum class TestKind { Hit, Round, Barrage };
void test(TestKind kind);

} // namespace vanacore::damagetext
