# CatsEyeXI on a Mac (CrossOver): setup for frame rate

Player guide, 2026-10-19. What to give Mac players so the game runs on VanaCore's Direct3D 11 through Metal, not the
game's own Direct3D 8 through Wine. The developer background is at the end.

For Apple Silicon Macs running CatsEyeXI in CrossOver. Tested on an M5 Max with CrossOver 26.3. In Southern San d'Oria's
busiest square the frame rate went from about 15 fps to about 53 fps.

## Why

FFXI draws with DirectX 8. Whatever backend you pick, CrossOver runs DirectX 8 through Wine's own translation, which
is slow per call, so busy towns drop to around 15 fps. VanaCore can replace the game's DirectX 8 with its own
DirectX 11 renderer. With the bottle on **DXMT**, that DirectX 11 goes straight to Metal. D3DMetal can't help here: it
only supports 64-bit games, and FFXI is 32-bit.

## Setup (about 2 minutes)

1. **Quit the game and the CatsEyeXI launcher.** You don't need to quit CrossOver itself.
2. **Switch the bottle to DXMT.** In CrossOver, select your CatsEyeXI bottle, open its settings, and under
   **Graphics** set the backend to **DXMT**. If your CrossOver doesn't list DXMT, update CrossOver.
3. **Pick DirectX 11 in the launcher.** Open the launcher, go to **Accounts**, and set **Renderer** to
   **DirectX 11** for each account. Not DirectX 12: it can't run on a Mac. VanaCore versions from before the fall-back
   change (launcher PR #58) then drop to the game's slow DirectX 8; later ones switch to DirectX 11 by themselves.
4. **Start the game.** The first start is slow: DXMT is converting the game's shaders to Metal and saving them.
   Expect stutter for the first minute or so, and the first time you visit an area. Later starts are fast.

## Optional: turn off the Ashita plugins you don't need

On a Mac, the Ashita plugins **Minimap**, **FindAll**, **Screenshot** and **Thirdparty** together cost about 20 ms a
frame. Turning them off took the test machine from about 23 fps to 53 fps. To turn them off for good, use the launcher's
VanaCore settings, **Addons & Plugins** tab. Or try it in game first: type `/plugin unload minimap` (and the same for
the others). Leave **cexidats** and **cexislots** on: they load CatsEyeXI's own content.

## Check it worked

In game, type `/vana stats`. The top line shows the frame rate, and "Direct3D calls" should be a few milliseconds a
frame, not 30 or more. In VanaCore's `logs/vanacore.log`, look for a line like
`Graphics: gfx: Direct3D 11 (feature level 11_1) on Apple M5 Max`.

## If something's wrong

- **The game looks the same as before, or the log says "didn't come up":** check that Renderer says DirectX 11, and
  that the bottle's backend is DXMT, not D3DMetal or Auto.
- **The game freezes for a few seconds at a time:** the bottle is most likely still on Wine's own DirectX 11. Set the
  backend to DXMT and restart the game.
- **To go back:** set Renderer to "DirectX 8 (game's)" and the bottle's backend back to what it was.

## Background (for developers)

- **32-bit:** FFXI (and VanaCore.exe) is 32-bit. CrossOver's D3DMetal (`lib64/apple_gptk`) ships x86_64 DLLs only, so
  with the D3DMetal backend a 32-bit process gets Wine's builtins: wined3d for d3d8/d3d11, vkd3d on MoltenVK for
  d3d12. DXMT ships i386 `d3d11.dll`, `dxgi.dll` and `winemetal.dll` (`lib/dxmt/i386-windows`), which is why the
  bottle must be on DXMT. Check with `lsof -p <VanaCore.exe pid>`: `lib/dxmt/i386-windows/d3d11.dll` should be mapped.
- **Direct3D 12:** vkd3d reports resource binding tier 1 wherever Vulkan has 16 or fewer samplers per stage, as
  MoltenVK does, and the D3D12 back end's unbounded SRV and sampler tables need tier 2 (`gfx_d3d12.c` `make_root`).
  Wine's d3dcompiler_47 can't build its SM 5.1 shaders either. So `renderer=d3d12` can't draw on a Mac.
- **wined3d's Direct3D 11:** the fallback when the bottle isn't on DXMT. Measured at 21–26 fps with the plugins loaded;
  with them unloaded the CPU ran ahead and stalled for seconds at a time (1–9 s frames).
- **Measured** (M5 Max, CrossOver 26.3, Southern San d'Oria arrival square, plugged in, `/vana stats save`):

  | | DirectX 8 (game's, wined3d) | DirectX 11 on DXMT, plugins loaded | DirectX 11 on DXMT, 4 plugins off |
  |---|---|---|---|
  | frame | 69.0 ms (14.5 fps) | 44.0 ms (22.7 fps) | 18.9 ms (52.8 fps) |
  | game's own work | 31.3 ms | 18.5 ms | 13.9 ms |
  | Direct3D calls | 36.9 ms | 3.4 ms | 3.4 ms |
  | Ashita plugins | (off) | 20.2 ms | 0.06 ms |

  Turning the scene effects off changed nothing (21 fps either way). Which of the four plugins costs the 20 ms on DXMT
  wasn't isolated; under wined3d, unloading Minimap alone removed it.
