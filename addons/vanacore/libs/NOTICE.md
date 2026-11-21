# Third-party libraries in this folder

The files in `ashita/`, `ashita-4.16/` and `ashita-4.30/` are not VanaCore's. They are copies of Ashita's addon
libraries, included unmodified so Ashita addons that `require` them work in VanaCore: `ashita-4.16/` and `ashita-4.30/`
hold each version's own where the two differ, and `ashita/` what both versions share. VanaCore gives addons `ashita-4.30/`
(`ashita-4.16/` with `[addons] ashita.libs=4.16`) and then `ashita/`; the ImGui 1.81 names 4.30's `imgui.lua` lacks are
defined by VanaCore's own `compat/ashita.lua`, not added to these files.

- **Source:** Ashita v4's `addons/libs` (https://github.com/AshitaXI/Ashita-v4beta): 4.16 as CatsEyeXI's client ships it,
  and 4.30
- **Copyright:** (c) 2021, 2025 Ashita Development Team. All rights belong to them.
- **License:** GNU General Public License v3 or later. The notice is at the top of each file; the license text is in
  `LICENSE-GPLv3.md` in the `licenses` folder of the VanaCore install.

A few files in `ashita/` are other people's and keep their own licenses:

- LuaSocket's Lua modules (`socket.lua`, `mime.lua`, `socket/*.lua`): Copyright (C) 2004-2022 Diego Nehab, MIT
- `json.lua`: Copyright (c) 2020 rxi, MIT

And in `ashita-4.30/`: `reflect.lua`, Copyright (C) 2014 Peter Cawley, MIT.

The full text of each of these licenses is in `THIRD_PARTY_NOTICES.md` in the VanaCore install.
