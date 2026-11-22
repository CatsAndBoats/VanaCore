# Changelog

What's new in each VanaCore release. A release's notes on GitHub are its section here (`## <version>`), under New, Changed, Fixed and Misc: write the next version's section with its version bump.

## 0.9.5

### New
- **Mass Grass:** Grass now grows shorter toward where it ends and as it climbs toward its steepest slope, instead of stopping at full height in a line (along La Theine Plateau's hills, for one).
- **Mass Grass:** Grass wholly outside the camera's view is now skipped.
- **Mass Grass:** An eye on each Grass Library row shows that plant alone.
- **Addons:** Addons can now tell which server the game is on.

### Changed
- **Mass Grass:** How much grass is drawn now follows Range and Quantity alone.
- **Mass Grass:** Grass is now lit by its ground's slope as the game lights it, so it should match the ground through dawn, dusk and night.
- **Mass Grass:** Some zones now have their own path padding (none in Wajaom Woodlands), and the slider is locked there.
- **Mass Grass:** New defaults: 150 yalm range, full quantity, shorter grass, no flowers and dry grass off.
- **Water:** VanaCore's water is now off in 87 zones with no water of their own, such as the Promyvions, Castle Zvahl, Crawlers' Nest, Xarcabard and the battlefields.
- **/vana:** The Mass Grass panel is reworked: Range in tens of yalms, Quantity and Height as percents, and an Advanced Settings group.

### Fixed
- **Shadows:** La Theine Plateau's hills should no longer have dark squares down their slopes under a high sun.
- **Shadows:** Mass Grass should no longer look grey all over with shadows on and ambient occlusion off.
- **Shadows:** Shadows under and on trees should no longer flicker with Shadows from things off screen turned off (Wajaom Woodlands).
- **Water:** The water's glint should no longer come and go as the camera moves where the walls are unlit (Lower Jeuno).
- **Sliders:** With flat sliders on, the party list's HP, MP and TP bars should no longer be drawn as sliders.

## 0.9.4

### Changed
- **Water:** VanaCore's water is now also off in the Sea of Al'Taieu's zones (Al'Taieu, the Grand Palace of Hu'Xzoi, the Garden of Ru'Hmet and Empyreal Paradox), leaving the game's own water there.

### Fixed
- **Tooltips:** The chat logs should no longer drop down under an item's details when they're taller than 12 rows.

## 0.9.3

### Changed
- **/vana:** Client's About and VanaCore tabs are now one, Client > VanaCore, with the banner and the version at the top.
- **Water:** VanaCore's water is off in Alzadaal Undersea Ruins, leaving the game's own water there.

### Fixed
- **Rain:** Rain should no longer fall in indoor zones such as Monastic Cavern, Qulun Dome, the Boyahda Tree and the Temple of Uggalepih.
- **Mass Grass:** Beadeaux and Beadeaux [S] should have grass again.

## 0.9.1

### Changed
- **Rain:** VanaCore's rain now falls only in zones that have rain of their own, in the server's rain as in thunder.

### Fixed
- **Rain:** Phomiuna Aqueducts should no longer have rain indoors.
- **Water:** Selbina's ferry's lower deck should no longer be drawn as water, or leave patches of water behind as it sails.

### Misc
- **Source:** VanaCore's source is now on GitHub.
