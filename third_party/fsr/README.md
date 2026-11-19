# AMD FSR SDK (the parts VanaCore uses)

From AMD's FSR SDK 2.3.0 ("Redstone"), [GPUOpen-LibrariesAndSDKs/FidelityFX-SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)
at 60f4ea81909200d8542eca14dccb2628b763a9a3 (tag `v2.3.0`), `Kits/FidelityFX/`. AMD's licence for the SDK is
`license.md`: every file here is on the list under its MIT part.

In the repository, unmodified, as the SDK lays them out (`ffx_upscale.h` includes `../../api/include/ffx_api.h`):

- `api/include/ffx_api.h`, `ffx_api_types.h`, `dx12/ffx_api_dx12.h`: the FSR API (`ffxCreateContext`, `ffxDispatch`, ...)
- `upscalers/include/ffx_upscale.h`: its upscaler's structures (FSR 4, FSR 3.1)
- `framegeneration/include/ffx_framegeneration.h`, `ffx_framegeneration_api_types.h`: its frame generation's

Not in the repository (69 MB): `fetch-fsr.ps1` downloads them from AMD's repository and checks each one's SHA-256.
VanaCore.Dlss's build and `build-release.bat` run it when one is missing; to run it yourself:

```
powershell -NoProfile -ExecutionPolicy Bypass -File third_party\fsr\fetch-fsr.ps1
```

- `bin/amd_fidelityfx_loader_dx12.dll`: the FSR API's loader, which loads the effect DLLs beside it
- `bin/amd_fidelityfx_upscaler_dx12.dll`: FSR upscaling. FSR 4 (machine learning) on an AMD Radeon RX 7000 or 9000
  series card; FSR 3.1 on any other DirectX 12 card (AMD, NVIDIA, Intel)
- `bin/amd_fidelityfx_framegeneration_dx12.dll`: FSR frame generation. FSR FG 4 (machine learning) on a Radeon RX 9000
  under Windows 11; FSR FG 3.1 on any other DirectX 12 card

A VanaCore build ships the three DLLs, signed by AMD and unmodified, beside `VanaCore.Dlss.exe` in its `dlss\` folder,
with AMD's licence in `licenses\`. Like DLSS they come for 64-bit programs only, and FFXI (with VanaCore in it) is
32-bit: `VanaCore.Dlss.exe` (`src/dlss`), the 64-bit helper, runs FSR on the pictures the game's process shares with it,
as it does DLSS (`[graphics] fsr`, the game drawing with Direct3D 12 or 11: the helper's FSR is Direct3D 12's either
way), and AMD's frame generation with either upscaler (`[graphics] fsr.framegen`, `dlss.framegen=fsr`; Direct3D 12
only), without AMD's swapchain. It loads the loader by name when either is asked for, so nothing of AMD's is linked.
