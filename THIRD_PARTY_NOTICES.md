# Third-party notices

VanaCore itself is Copyright (C) 2026 Vekien, under the GNU General Public License v3 or later with additional permissions: `LICENSE` and `LICENSING.md` (`LICENSE.txt` and `LICENSING.md` in a VanaCore download), which also say which licence covers which file.

VanaCore.Core.dll, the DLSS helper in `dlss\` beside it, and the files VanaCore ships with them (`addons\`, `fonts\`, `dlss\`) include the following. Each notice below is reproduced as its authors ship it; the full license texts are in `licenses\` and beside the files they cover.

## xi-on-mac (FFXI on Mac) and XI on Anything

Source: the xi-on-mac repository by Joshua Tyree (rubymatrix), commit 6d9928d (2026-09-26), updated to 5b6eb1e (its sun shadows, per-pixel lighting and scene fixes). `src/gfx/d3d8.c`, `d3d8.h`, `gfx.h`, `gfx_d3d12.c`, `gfx_hlsl.c`, `gfx_hlsl.h`, `gfx_hlsl_shaders.c` and `test/gfx_test.c`, `test/d3d8_test.c` come from its `runtime/portable/` and `tests/`. Modified: every VanaCore-specific change is marked `VANACORE`; the scene effects in `gfx_d3d12.c` (`gfx_scene_done` and what it uses: the sun's shadow maps, their casters and cache, the temporal pass), the depth and mip-chain views they need, `present_cas_fs`, per-pixel lighting and the shadow pass's vertex matrix in `gfx_hlsl.c`, and the debug layer's message queue are ports of its Metal back end (`gfx_metal.m`) to Direct3D 12. Later work is ported from its successor, XI on Anything (https://github.com/rubymatrix/xi-on-anything, up to commit c5acb0b, 2026-10-12), by the same author and under the same license, with his permission: Direct3D 12's slab allocator for static buffers and the shadow casters drawn from buffers the game rewrites (`gfx_d3d12.c`, `gfx_d3d11.c`), the sun's glide, the fixed-light zones and Mog House shadows, ambient occlusion kept off glowing pixels, the scene filter's size limit, `inset_quad` and the game's blob shadow left out per draw (`d3d8.c`, `gfx_d3d12.c`, `gfx_d3d11.c`); water found by the game's model loading (`src/core/water.cpp`, from its `host/host64.c`); Discord Rich Presence (`src/core/discord.cpp`, from `host/discord.c`); the live menu resolution (`src/core/uiscale.cpp`) and the game's Config menu entry (`src/core/configmenu.cpp`), from `host/modern.c`; and the session value LandSandBoat's zone key starts with (`src/core/loader.cpp`, from `host/lsb_login.c`). Licensed under the MIT License (https://github.com/rubymatrix/xi-on-mac and https://github.com/rubymatrix/xi-on-anything, `LICENSE`); VanaCore's changes are under VanaCore's license:

```
MIT License

Copyright (c) 2026 Joshua Tyree

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## FrameFix

Source: FrameFix by rockerudon (https://github.com/rockerudon/FrameFix), `src/FrameFix.cpp` (version 1.3). `src/core/framepacing.cpp` reimplements its 60 FPS timing fixes in VanaCore: the code patterns it finds in FFXiMain.dll (the motion frame and blend ticks, the interface's and the chat's updates, the chat windows, the camera's loops and its push to the side), and its timing (the last four frames averaged for motion, catch-up updates up to four a frame, the chat windows' countdown). Licensed under the MIT License:

```
MIT License

Copyright (c) 2026 rockerudon

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Material Symbols Outlined

Source: Google's Material Symbols (https://fonts.google.com/icons, https://github.com/google/material-design-icons), the Outlined font as Google Fonts serves it, `fonts/material-symbols-outlined.woff2`, unmodified. Copyright Google LLC, licensed under the Apache License, Version 2.0 (https://www.apache.org/licenses/LICENSE-2.0). The full license text is beside it (`fonts/LICENSE-MaterialSymbols.txt`).

## Agave and Font Awesome Free (ImGui's fonts)

`fonts/imgui/`, unmodified, loaded by VanaCore's ImGui as Ashita loads its own: Agave Regular (https://github.com/blobject/agave, release 39, `Agave-Regular.ttf`), Copyright 2013-2026 The agave Project Authors; and Font Awesome Free 6.7.2 (https://github.com/FortAwesome/Font-Awesome, `webfonts/fa-regular-400.ttf`, `fa-brands-400.ttf`, `fa-solid-900.ttf`), Copyright Fonticons, Inc. Both fonts are licensed under the SIL Open Font License, Version 1.1 (https://openfontlicense.org); Font Awesome's icons under CC BY 4.0. The full license texts are beside them (`LICENSE-Agave.txt`, `LICENSE-FontAwesome.txt`).

## LuaJIT 2.1

Source: https://github.com/LuaJIT/LuaJIT, branch v2.1 at commit c6ffc141 (2026-09-08), vendored in `third_party/luajit`. Modified: `src/lj_lex.c` and `src/lj_parse.c` accept the syntax Windower's Lua accepts (every change is in `third_party/luajit-vanacore.patch`, marked "VanaCore" in the source), and `.relver` holds that commit's timestamp (the version number) because the copy has no git history.

```
===============================================================================
LuaJIT -- a Just-In-Time Compiler for Lua. https://luajit.org/

Copyright (C) 2005-2026 Mike Pall. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

[ MIT license: https://www.opensource.org/licenses/mit-license.php ]

===============================================================================
[ LuaJIT includes code from Lua 5.1/5.2, which has this license statement: ]

Copyright (C) 1994-2012 Lua.org, PUC-Rio.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

===============================================================================
[ LuaJIT includes code from dlmalloc, which has this license statement: ]

This is a version (aka dlmalloc) of malloc/free/realloc written by
Doug Lea and released to the public domain, as explained at
https://creativecommons.org/licenses/publicdomain

===============================================================================
```

## LuaSocket

Source: https://github.com/lunarmodules/luasocket at tag v3.1.0, vendored in `third_party/luasocket` (its C sources for Windows, unmodified): `socket.core` and `mime.core`, built into VanaCore.Core.dll for addons.

```
Copyright (C) 2004-2022 Diego Nehab

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```

## stb_image

Source: https://github.com/nothings/stb, `stb_image.h` v2.30, vendored unmodified in `third_party/stb`. VanaCore compiles
only its zlib decoder (`third_party/stb/stb_zlib.c`), for addon packages. Public domain, or under the MIT License, at the
user's choice (its own words at the end of the file):

```
Copyright (c) 2017 Sean Barrett

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Dear ImGui

Source: https://github.com/ocornut/imgui, docking branch at commit a28cb615 (version 1.92.3 WIP), the version Ashita 4.30 builds in, vendored in `third_party/imgui`. Modified: `imconfig.h` defines `IM_ASSERT` to log a failed check instead of stopping (marked "VanaCore"). For Ashita 4.16's plugins and addons, VanaCore also carries ImGui 1.81 WIP's (commit 6487860a, the version Ashita 4.16 builds in) structure layouts and enumeration values: `src/core/compat/ashita/imgui_v181.hpp`, and `src/core/imgui_legacy.inc`, generated by `tools/gen_ashita_gui.py` from the Ashita 4.16 SDK's copy of ImGui's `imgui.h`. Both versions are under the same license.

```
The MIT License (MIT)

Copyright (c) 2014-2025 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## AsmJit and AsmTK

`third_party/asmjit/` (https://github.com/asmjit/asmjit, commit c87860217e, 2025-12-13; its `asmjit/` folder, of which VanaCore builds the core, support and x86 parts) and `third_party/asmtk/` (https://github.com/asmjit/asmtk, commit 1261a46fab, 2025-12-28; its `src/asmtk/` folder), unmodified, built into `VanaCore.Core.dll` for Ashita addons' `ashita.memory.assembler`, which reads x86 assembly text as Ashita 4.30's does. AsmJit Copyright (c) 2008-2025 Petr Kobalicek; AsmTK Copyright (c) 2016 Petr Kobalicek (see each `LICENSE.md`). Both under the zlib license:

```
This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

## Ashita's addon libraries

`addons/vanacore/libs/ashita/`, `ashita-4.16/` and `ashita-4.30/`, unmodified: Ashita v4's `addons/libs` (https://github.com/AshitaXI/Ashita-v4beta), interface 4.16's as the CatsEyeXI client ships it and 4.30's, without 4.30's `annotations` (documentation for editors) and LuaSec (`socket/ssl.lua`, `socket/ssl/https.lua` and `socket/ssl.dll`: the DLL has its own Lua built in, which a LuaJIT state can't run; VanaCore's `ssl` and `ssl.https` are its own, the latter over WinHTTP). What the two versions share is kept once, in `ashita/` (4.30's copy, where the two differ only in comments, `require` style, an unused parameter, a type name fixed and an added `sugar.coroutine`); the files that differ are in each version's folder, with 4.30's additions in `ashita-4.30/`. Ashita addons find their interface's (`[addons] ashita.interface`) first, then the shared ones. `addons/vanacore/libs/NOTICE.md` says the same beside them. The Ashita Development Team's files (Copyright (c) 2021, 2025 Ashita Development Team) are under the GNU General Public License v3 or later (the notice is at the top of each file; the license text is in `third_party/ashita-sdk/LICENSE-GPLv3.md`; in a VanaCore download, in `licenses\`). Others it carries keep their own licenses:

- LuaSocket's Lua modules (`socket.lua`, `mime.lua`, `socket/*.lua`): LuaSocket's MIT license, given under LuaSocket above (the files carry no notice of their own).
- `json.lua` (Copyright (c) 2020 rxi) and `reflect.lua` (Copyright (C) 2014 Peter Cawley): MIT, the full notice at the top of each file.

## Windower's addon libraries and resources

`addons/vanacore/libs/windower/libs/` and `addons/vanacore/libs/windower/res/`, unmodified: Windower 4's `addons/libs` (https://github.com/Windower/Lua) and `res` (https://github.com/Windower/Resources), as Windower's updater installed them on 2026-11-20, without LuaSec (`ssl.lua`, `ssl/https.lua`: it needs a DLL a LuaJIT state can't load). Windower addons read them when `addons\windower\libs` or `addons\windower\res` doesn't have the file. `addons/vanacore/libs/NOTICE.md` says the same beside them. LuaSocket's Lua modules among them (`socket.lua`, `ltn12.lua`, `mime.lua`, `socket/*.lua`) are under LuaSocket's MIT license, given under LuaSocket above. The rest are under the BSD 3-Clause license, the notice at the end of each library file (`actions.lua` is Copyright © 2013, Suji, under the same terms):

```
Copyright © 2013-2026, Windower
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
    * Neither the name of Windower nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL Windower BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Ashita v4 SDK

Source: the Ashita v4 SDK (`plugins/sdk`): interface version 4.16 as the CatsEyeXI client ships it, and 4.30 from https://github.com/AshitaXI/Ashita-v4beta at commit ed757ee915. `src/core/compat/ashita/abi.hpp` is generated from its headers by `tools/gen_ashita_abi.py` and so is a modified version of them, released with VanaCore's source under the same license. The changes: the interface declarations are reformatted (parameters renamed, default arguments dropped), Direct3D, DirectInput and ImGui types are replaced by stand-ins of the same size and passing convention, both versions' interfaces are in one file (in `vanacore::ashita::sdk::v416` and `v430`), and implementation class templates are added for each interface. The game structures (`ffxi/*.h`), enumerations and resource structures are the 4.30 SDK's, unchanged. The full license texts are in `third_party/ashita-sdk/` (`LICENSE-LGPLv3.md`, and `LICENSE-GPLv3.md`, which it builds on); in a VanaCore download, in `licenses\`.

```
Ashita SDK - Copyright (c) 2023 Ashita Development Team
Contact: https://www.ashitaxi.com/
Contact: https://discord.gg/Ashita

This file is part of Ashita.

Ashita is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Ashita is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with Ashita.  If not, see <https://www.gnu.org/licenses/>.
```

## NVIDIA DLSS SDK

Source: NVIDIA's DLSS SDK 310.9.1 (https://github.com/NVIDIA/DLSS, commit 374959484e79a640feaba44c93ac8cfb0a03f5b5). It isn't in VanaCore's source repository (NVIDIA's license allows it only as part of an application): `third_party/dlss/fetch-dlss.ps1` downloads it from NVIDIA's repository and checks it. In a VanaCore download, `dlss\nvngx_dlss.dll` (DLSS Super Resolution) and `dlss\nvngx_dlssg.dll` (DLSS Frame Generation) ship unmodified beside `dlss\VanaCore.Dlss.exe`, which links NVIDIA's NGX loader (`nvsdk_ngx_d.lib`) and uses the SDK's helper headers. This software contains source code provided by NVIDIA Corporation. Licensed under the NVIDIA RTX SDKs license: `licenses\LICENSE-NVIDIA-DLSS.txt` in a VanaCore download (`third_party/dlss/LICENSE.txt` once fetched). DLSS 5's `nvngx_dlssnr.dll` isn't part of that SDK and VanaCore doesn't include it: a player who has it puts it in `dlss\`. NVIDIA, GeForce RTX and DLSS are trademarks of NVIDIA Corporation. VanaCore is not sponsored or endorsed by NVIDIA.

## AMD FSR SDK

Source: AMD's FSR SDK 2.3.0 (https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, tag `v2.3.0`, commit 60f4ea81909200d8542eca14dccb2628b763a9a3), `Kits/FidelityFX/`. In VanaCore's source repository, unmodified, in `third_party/fsr/`: the FSR API's headers (`api/include/ffx_api.h`, `ffx_api_types.h`, `dx12/ffx_api_dx12.h`), its upscaler's (`upscalers/include/ffx_upscale.h`) and its frame generation's (`framegeneration/include/ffx_framegeneration.h`, `ffx_framegeneration_api_types.h`), which `dlss\VanaCore.Dlss.exe` is built with. Not in the repository (69 MB): `third_party/fsr/fetch-fsr.ps1` downloads AMD's signed DLLs from AMD's repository and checks them. In a VanaCore download, `dlss\amd_fidelityfx_loader_dx12.dll` (the FSR API), `dlss\amd_fidelityfx_upscaler_dx12.dll` (FSR 4 and FSR 3.1 upscaling) and `dlss\amd_fidelityfx_framegeneration_dx12.dll` (FSR FG 4 and FSR FG 3.1 frame generation) ship unmodified, as AMD signed them, beside `dlss\VanaCore.Dlss.exe`, which loads them by name. AMD's licence for the SDK is `licenses\LICENSE-AMD-FSR-SDK.md` in a VanaCore download (`third_party/fsr/license.md`, as AMD ships it as `docs/license.md`): each of these files is on its list under the MIT License:

```
Copyright (C) Advanced Micro Devices, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the “Software”), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

AMD, Radeon and FSR are trademarks of Advanced Micro Devices, Inc. VanaCore is not sponsored or endorsed by AMD.

## AMD FidelityFX Super Resolution 1.0

Source: AMD's FidelityFX Super Resolution 1.0 (https://github.com/GPUOpen-Effects/FidelityFX-FSR), its `ffx_fsr1.h`. `fx_easu` and `fx_rcas` in the scene effects' HLSL (`src/gfx/gfx_d3d12.c`, used by `src/gfx/gfx_d3d11.c` too) are ported from its EASU and RCAS (the 32-bit path, reading texels in place of its gathers), and are part of `VanaCore.Core.dll`. Under the MIT License:

```
Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## Microsoft Visual C++ runtime

`dlss\msvcp140.dll`, `dlss\vcruntime140.dll` and `dlss\vcruntime140_1.dll`, the 64-bit C++ runtime the DLSS (and FSR) helper needs, are Microsoft's Visual C++ Redistributable files, unmodified, distributed under the Visual Studio license terms for redistributable code. Copyright Microsoft Corporation.
