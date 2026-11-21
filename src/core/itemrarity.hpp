#pragma once

// [client] items.rarity: the coloured box CatsEyeXI's item icons have round the item for their tier (+1 grey, +2 blue,
// +3 gold, +4 green: a frame in the tier's colour, then two darker bevel lines, 5 pixels in all), left as the icon has
// it (game), taken out (hide: the item square under it shows, as [client] items.slot draws it) or redrawn (style) in
// the tier's colour (items.rarity.color1-4) with the item square's brightness, contrast and opacity, or filled flat
// when the square is solid. items.rarity.names=1 prints an item's name in its tier's colour (the item details window,
// and lists that print the name the item data holds). The tier is read from the icon, so items without "+N" in their
// name but with a box (Republic and Kingdom gear: blue) count too. An item's details (the tooltip) in its tier's colour:
// tips.rarity.name=1 its name, tips.rarity.gradient=1 the colour laid over the top of its box (tips.rarity.gradient.strength
// percent at the top, fading out tips.rarity.gradient.height of the interface's pixels lower).
namespace vanacore::itemrarity {

// `ini` (vanacore.ini): [client] items.rarity* read now, and again by reload whenever vanacore.ini changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Once a frame on the game's thread: hooks the game's drawing of an item's icon (and of a coloured string, for
// items.rarity.names) the first time they're wanted (once FFXiMain.dll is unpacked).
void on_frame();

// Around the game's drawing of an item's details window (chat.cpp's window draw; `ctrl` the window, null for any other): its
// item's tier for its name, and its colour told to the graphics layer (the name in True Font's letters, the gradient)
struct Details {
    explicit Details(const void* ctrl);
    ~Details();
    Details(const Details&) = delete;
    Details& operator=(const Details&) = delete;

private:
    bool on_;
};

} // namespace vanacore::itemrarity
