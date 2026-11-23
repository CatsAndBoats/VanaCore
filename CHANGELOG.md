# Changelog

What's new in each VanaCore release. A release's notes on GitHub are its section here (`## <version>`), under New, Changed, Fixed and Misc: write the next version's section with its version bump.

## 0.9.9

### New
- **Snow Trails:** A new snow.sand setting (off by default) lets sand ground take footprints too, from the next zone.

### Changed
- **/vana:** Advanced Settings groups now show when Enable Advanced Graphics Settings is on, DLSS 5 has a heading of its own, and only the panels' Reset buttons remain.

### Fixed
- **Water:** The far sea now fogs to the sky over the horizon instead of the zone's flat grey, and a tree on the shore no longer leaves its crown in navy on the sea.
- **Name icons:** Name icons now stay clear of the name on ultrawide screens.
- **/vana:** Banners no longer tint their row under the mouse.

## 0.9.8

### New
- **Water:** VanaCore's water now mirrors the world around it, the shore, trees, walls, cliffs and characters, broken up by the waves, with the sky's own clouds and dusk colours in it (by Carver).
- **Water:** Rain now leaves spreading rings where it lands on VanaCore's water, none under roofs or bridges (by Carver).

### Fixed
- **Water:** Lake Mechieume should now be VanaCore's water from shore to shore, without a jagged line across it, and Jugner Forest's and East Sarutabaruta's rivers should run on unbroken (by Carver).
- **Water:** Fountains should no longer show bands of water by the screen's right edge with a large Background resolution, as in Bastok Markets (by Carver).

## 0.9.7

### New
- **Mass Grass:** A new Mega dense option under Coverage fills all the ground the game calls grass, not only its green spots, for thicker grass in places like Konschtat Highlands.
- **Shadows:** A new Sharp shadow fade setting under Advanced Settings sets how gradually the sharp shadows near you blend into the softer ones farther out.

### Changed
- **Shadows:** Sharp shadow range is back to 10 by default, and thin shadows past it should now soften gradually as you walk instead of popping in and out.
- **Mass Grass:** Dry grass now only grows outdoors.

### Fixed
- **Mass Grass:** Grass should no longer grow on the floor of the Windurst Mog House.

## 0.9.6

### New
- **Ink Lines:** Mass Grass can now have its own ink lines, with their own darkness, width, edge sensitivity and fade, under Ink lines > Grass (Grass darkness starts at Off).
- **/vana:** Every panel has a Reset button on its heading (click it twice) that puts everything in the panel back to its default, switches included.

### Changed
- **Addons:** VanaStore now loads on its own at start, like /vana, and stays off once you switch it off.

### Fixed
- **Shadows:** Shadows on snow and other rounded ground should no longer be cut off or vanish as the camera moves under a low sun, such as at dawn and dusk in Qufim Island, Xarcabard, the Dunes, Sarutabaruta and Windurst Woods.
- **/vana:** Resetting a panel should no longer leave switches such as Blend with the game's shading or Dry grass on, and Mass Grass's reset should now also clear its old hidden settings, such as the old tuft limit that thinned the grass.

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
