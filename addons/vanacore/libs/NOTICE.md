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

The files in `windower/` are not VanaCore's either. They are copies of Windower 4's addon libraries (`windower/libs/`,
Windower's `addons/libs`) and resources (`windower/res/`, Windower's `res`), included unmodified so Windower addons run
when only the addon's own folder was copied into `addons\windower`: VanaCore reads them when `addons\windower\libs` or
`addons\windower\res` doesn't have the file. LuaSec (`ssl.lua`, `ssl/https.lua`) is left out: it needs a DLL a LuaJIT
state can't load.

- **Source:** Windower 4's `addons/libs` (https://github.com/Windower/Lua) and `res`
  (https://github.com/Windower/Resources), as Windower's updater installed them on 2026-11-20
- **Copyright:** (c) 2013-2026 Windower; `actions.lua` (c) 2013 Suji
- **License:** BSD 3-Clause. The notice is at the end of each library file; Resources' is in its repository.

LuaSocket's Lua modules among them (`socket.lua`, `ltn12.lua`, `mime.lua`, `socket/*.lua`) are LuaSocket's, MIT.

The full text of each of these licenses is in `THIRD_PARTY_NOTICES.md` in the VanaCore install.
