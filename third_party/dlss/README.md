# NVIDIA DLSS SDK (the parts VanaCore uses)

NVIDIA's DLSS SDK isn't in this repository. Its licence (the NVIDIA RTX SDKs licence) lets it be distributed only as part
of an application, never on its own, so `fetch-dlss.ps1` downloads it from [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS)
at 374959484e79a640feaba44c93ac8cfb0a03f5b5 (DLSS 310.9.1 SDK), checks each file's SHA-256, and puts it here.
VanaCore.Dlss's build and `build-release.bat` run it when a file is missing; to run it yourself:

```
powershell -NoProfile -ExecutionPolicy Bypass -File third_party\dlss\fetch-dlss.ps1
```

What it fetches, all under NVIDIA's licence (`LICENSE.txt`, fetched with them), not VanaCore's:

- `include/`: the NGX headers
- `lib/nvsdk_ngx_d.lib`: the NGX loader for 64-bit Windows (dynamic C runtime, `/MD`)
- `bin/nvngx_dlss.dll`: DLSS Super Resolution, shipped beside `VanaCore.Dlss.exe`
- `bin/nvngx_dlssg.dll`: DLSS Frame Generation (RTX 40 and later; 3x and 4x on RTX 50), shipped beside it too

A VanaCore build ships the two DLLs inside the application (its `dlss\` folder), with NVIDIA's licence in `licenses\`, as
NVIDIA's licence allows. Never commit any of these files.

## DLSS 5

DLSS 5 (`nvngx_dlssnr.dll`, NGX's feature 18) isn't in NVIDIA's SDK, so VanaCore neither includes nor ships it. A player
who has NVIDIA's `nvngx_dlssnr.dll` puts it in VanaCore's `dlss\` folder, beside `VanaCore.Dlss.exe`; the helper uses
it through the same NGX calls (`[graphics] dlss5`). Without it VanaCore runs as normal, and `/vana`'s DLSS 5 switch is
locked with the reason under it. Its parameters' names (`DLSSNR.*`, in `src/dlss/dlss_helper.cpp`) aren't in the SDK's
headers yet.

DLSS comes for 64-bit programs only, and FFXI (with VanaCore in it) is 32-bit: `VanaCore.Dlss.exe`
(`src/dlss`), a 64-bit helper, runs it on pictures the game's process shares with it on the GPU.
