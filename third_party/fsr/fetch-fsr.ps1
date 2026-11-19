<#
Fetches the AMD FSR SDK's signed DLLs that VanaCore ships, from AMD's own repository
(https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) at the commit VanaCore is built against (AMD FSR SDK 2.3.0,
tag v2.3.0), into this folder:

  bin\amd_fidelityfx_loader_dx12.dll     the FSR API's loader (ffxCreateContext and the rest), loaded by VanaCore.Dlss.exe
  bin\amd_fidelityfx_upscaler_dx12.dll   FSR upscaling: FSR 4 (an AMD Radeon RX 7000 or 9000) and FSR 3.1 (any
                                         DirectX 12 card), shipped beside VanaCore.Dlss.exe with the loader
  bin\amd_fidelityfx_framegeneration_dx12.dll
                                         FSR frame generation: FSR FG 4 (a Radeon RX 9000 on Windows 11) and FSR FG
                                         3.1 (any DirectX 12 card), shipped beside it too

All three are 64-bit only, like DLSS: the helper runs them. AMD signs them; they're used as AMD ships them, unmodified.
The headers VanaCore builds with (api\include, upscalers\include, framegeneration\include) and AMD's licence
(license.md: these files are under its MIT part) are in the repository; the DLLs aren't (69 MB). Each file is checked
against its SHA-256; one that's here already and right is left alone. VanaCore.Dlss's build and build-release.bat run
this when a file is missing.

  powershell -NoProfile -ExecutionPolicy Bypass -File third_party\fsr\fetch-fsr.ps1
#>
$ErrorActionPreference = 'Stop'
$commit = '60f4ea81909200d8542eca14dccb2628b763a9a3'
$source = "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/$commit"

# here, there (in AMD's repository), SHA-256
$files = @(
    @('bin\amd_fidelityfx_loader_dx12.dll', 'Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll', 'e2d85aa05a9bd9ed8b38935fdf5199372cca6f74c12015143bb6f945ee1608aa'),
    @('bin\amd_fidelityfx_upscaler_dx12.dll', 'Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll', 'd0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46'),
    @('bin\amd_fidelityfx_framegeneration_dx12.dll', 'Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_dx12.dll', '02297beedd285e822d3a64f314cf00faf378dcec0edc47ff0c4dd71b3a8c2f18')
)

# Windows PowerShell 5.1 may not offer TLS 1.2 by itself; its progress bar slows downloads down a great deal
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
$ProgressPreference = 'SilentlyContinue'

$fetched = 0
foreach ($file in $files) {
    $path = Join-Path $PSScriptRoot $file[0]
    if ((Test-Path -LiteralPath $path) -and (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -eq $file[2]) {
        continue
    }
    New-Item -ItemType Directory -Force -Path (Split-Path $path) | Out-Null
    Write-Host "fetch-fsr: $($file[0])"
    $partial = "$path.download"
    Invoke-WebRequest -UseBasicParsing -Uri "$source/$($file[1])" -OutFile $partial
    $hash = (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash
    if ($hash -ne $file[2]) {
        Remove-Item -LiteralPath $partial
        throw "fetch-fsr: $($file[0]) came with SHA-256 $hash, not $($file[2])"
    }
    Move-Item -Force -LiteralPath $partial -Destination $path
    $fetched++
}
if ($fetched -gt 0) {
    Write-Host "fetch-fsr: $fetched file(s) from GPUOpen-LibrariesAndSDKs/FidelityFX-SDK at $($commit.Substring(0, 10)), under AMD's licence (third_party\fsr\license.md)"
}
