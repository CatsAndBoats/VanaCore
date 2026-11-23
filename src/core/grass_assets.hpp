#pragma once

#include <string>
#include <vector>

// Mass Grass's catalogue: what the tufts can be drawn from. The drawn blades (built in, Type 1 and Type 2), VanaCore's
// own assets (addons\vanacore\vana\grass\*.json, shipped) and the player's library (config\grass\*.json, from //vana grass
// capture), each a sheet with the rectangles of the sprites on it. A sheet is a recipe: a texture in one of the game's DATs, read
// from the player's own files (through pivot's overlays, as the game reads them), so no game art ships with VanaCore:
//
//   { "name": "Ru'Aun Gardens flowers", "kind": "flower", "dat": "ROM2/22/4.DAT", "texture": "model   e_w02c",
//     "size": [256, 256], "height": 1.3, "sprites": [[4, 7, 64, 128], [95, 3, 137, 128]], "zone": 130 }
//
// `size` is the texture's size the rectangles are in (a client with a smaller copy of it gets them scaled). A capture
// that no game file has is kept as a picture instead (`"picture": "z130-....png"` beside the .json), the player's own.
//
// An asset's id is its file's name without .json (the drawn blades: blades1, Type 1, and blades, Type 2). vanacore.ini's
// [graphics] grass.asset.<id>=1 grows it and grass.asset.<id>.amount (0 to 100) is its share of the mix (the drawn blades of
// Type 2: on, 100 until set; the rest 0).
namespace vanacore::grass_assets {

struct Info {
    std::string id;
    std::string name;
    std::string kind;
    bool shipped = false; // VanaCore's own (not the player's library)
    bool picture = false; // a PNG of its own, not a recipe from the game's files
    float amount = 0.0f;  // its share of the mix until one is set (grass.asset.<id>.amount), on when over 0: its file's "default"
};

// Loads the catalogue the first time (and after reload()) and hands the grass the pictures and the ini's
// weights. `ini` is vanacore.ini, `dir` VanaCore.exe's folder. Called with the other [graphics] settings.
void apply(const std::wstring& ini, const std::wstring& dir);
// The catalogue is read again at the next apply (and applied now when the settings are known), and the zones' dirt levels.
void reload();
// The dirt level VanaCore gives a zone's grass (VanaGrass.dirt: how colourful its ground has to be to grow grass, 1 to 100), from
// addons\vanacore\vana\data\grass_dirt.json (shipped, not a setting: read once, and again by reload()); 50 for a zone it
// doesn't list or before the first apply()
float dirt(int zone);
// Whether a zone's grass grows on its blended ground too (VanaGrass.blended: a layer the game draws over other ground, as The
// Sanctuary of Zi'Tah's moss), from the same file's "blended": true; false for a zone it doesn't list
bool blended(int zone);
// How far back from a path's ground a zone's grass is kept (VanaGrass.pathpad, yalms, 0 to 4): the same file's "pathpad" for the
// zone, 0 where the zone has no paths, whose mottled floors it left spotty (Wajaom Woodlands' leaf litter, each fleck ringed bare);
// `rest` for a zone it gives none ([graphics] grass.pathpad, /vana's Path padding, else pathpad_default: its top-level "pathpad",
// 0.25 without it). pathpad_own: whether the zone you're in has its own (and it in value), for the slider's lock
float pathpad(int zone, float rest);
float pathpad_default();
bool pathpad_own(float& value);
// One library entry grown alone, whole, whatever its share and the others' (the /vana window's eye on its row: what it looks
// like), till solo("") or the game's end; never saved. solo_id(): the one shown alone, "" none
void solo(const std::string& id);
std::string solo_id();
// The textures that are grass ground in a zone whatever their colours say (VanaGrass.ground: hex hashes, space-separated), from the
// same file's "ground": [ "65b719e367503797", ... ] (The Sanctuary of Zi'Tah's dark floor); empty for a zone it doesn't list
std::string ground(int zone);
// The textures never grass ground in a zone whatever their colours say (VanaGrass.not_ground), from the same file's "not_grass":
// the zone's and the top level's, which every zone takes (the Mog Houses' floors)
std::string not_ground(int zone);
// Whether a zone grows grass at all: false for one the same file says "grass": false of (Jeuno's three cities, no grass ground in
// them, where a stone town's tan tile could pass for dry grass), true for the rest
bool grows(int zone);
// The assets in the order the /vana window lists them (the drawn blades first, Type 1 then Type 2).
std::vector<Info> list();

// //vana grass capture [name]: the plant sheets drawn near the player over the next frames go into the
// library, each found in the game's files (the zone's DAT, then the DATs the game opened lately: pivot's
// opened_dats()) on a thread. False with `message` when it can't start.
bool capture(const std::string& name, std::string& message);
// Each frame: a capture's sheets are handed to the search, a finished search is written to the library
// (config\grass) and the catalogue read again; recipes that waited for the game are read once it's loaded.
void on_frame();
// What the last capture did, once (empty when there's nothing new to say).
std::string take_status();
// The zone the player is in (the capture names its files and entries by it).
void set_zone(int zone);

} // namespace vanacore::grass_assets
