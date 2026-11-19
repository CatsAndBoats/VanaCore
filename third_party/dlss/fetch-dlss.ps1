<#
Fetches the parts of NVIDIA's DLSS SDK that VanaCore builds with and ships, from NVIDIA's own repository
(https://github.com/NVIDIA/DLSS) at the commit VanaCore is built against (DLSS SDK 310.9.1), into this folder:

  include\*.h             the NGX headers
  lib\nvsdk_ngx_d.lib     the NGX loader for 64-bit Windows (/MD), linked into VanaCore.Dlss.exe
  bin\nvngx_dlss.dll      DLSS Super Resolution, shipped beside VanaCore.Dlss.exe
  bin\nvngx_dlssg.dll     DLSS Frame Generation, shipped beside it too
  LICENSE.txt             NVIDIA's licence for all of them (the NVIDIA RTX SDKs licence)

They aren't in VanaCore's repository: NVIDIA's licence lets them be distributed only as part of an application, not on
their own. Each file is checked against its SHA-256; one that's here already and right is left alone. VanaCore.Dlss's
build and build-release.bat run this when a file is missing.

  powershell -NoProfile -ExecutionPolicy Bypass -File third_party\dlss\fetch-dlss.ps1

By fetching these you accept NVIDIA's licence (LICENSE.txt, once fetched).
#>
$ErrorActionPreference = 'Stop'
$commit = '374959484e79a640feaba44c93ac8cfb0a03f5b5'
$source = "https://raw.githubusercontent.com/NVIDIA/DLSS/$commit"

# here, there (in NVIDIA's repository), SHA-256
$files = @(
    @('LICENSE.txt', 'LICENSE.txt', '3027f23ca5a46dd9cb8183fbd522983a86f64d7daac5982912bf9f214671f294'),
    @('include\nvsdk_ngx.h', 'include/nvsdk_ngx.h', '96f21266a05ef06bb49f58c89de1a7845f2d9b1e0e0e069fe03740b8eaf0344d'),
    @('include\nvsdk_ngx_defs.h', 'include/nvsdk_ngx_defs.h', 'a9481cbc78d53ee56639f0a573ec095c6c1ea1e5f296d139c20c42bf342c2e1b'),
    @('include\nvsdk_ngx_defs_dlssd.h', 'include/nvsdk_ngx_defs_dlssd.h', 'ab2ef79188233b199cdebb224a283f2b7d8f03ea95adf880a8b225aea2f08586'),
    @('include\nvsdk_ngx_defs_dlssg.h', 'include/nvsdk_ngx_defs_dlssg.h', '81704eb9ac93c97127e79856f601e56ffacfadebb61b8eadd3ba316fac3dd6db'),
    @('include\nvsdk_ngx_defs_vk.h', 'include/nvsdk_ngx_defs_vk.h', '43b821c5d297d86165e6d2f96a476f5c30f2158c7d5a0451425c6b927abfec88'),
    @('include\nvsdk_ngx_helpers.h', 'include/nvsdk_ngx_helpers.h', '4569aa7566667d8c0d93c8e50ddd3f2c4fa1c89da536ffdc2571bbdc97a539e9'),
    @('include\nvsdk_ngx_helpers_cuda.h', 'include/nvsdk_ngx_helpers_cuda.h', '18f2fd3adda56124a975a9e5f7347e61ca2e25bd58e78b7922edf6c5c0857281'),
    @('include\nvsdk_ngx_helpers_d3d.h', 'include/nvsdk_ngx_helpers_d3d.h', '9b3c356cd2d04e7924c0b6b97fc1f1a37e5d30095a8c22133a5a08398cef2c4c'),
    @('include\nvsdk_ngx_helpers_dlssd.h', 'include/nvsdk_ngx_helpers_dlssd.h', '4144ef09594dcf0ddec5c2df1e0f87531049ab76daf995a8607b255c2bdb0df9'),
    @('include\nvsdk_ngx_helpers_dlssd_cuda.h', 'include/nvsdk_ngx_helpers_dlssd_cuda.h', '32a74447c395586dd8c8c4d1ef29df1a327a040dc12b01a101723dc083c47bf1'),
    @('include\nvsdk_ngx_helpers_dlssd_d3d.h', 'include/nvsdk_ngx_helpers_dlssd_d3d.h', 'be5ecfc8f5ecb17861600485ce9fb617dc56faa5f79ee1525081a54bf4281475'),
    @('include\nvsdk_ngx_helpers_dlssd_vk.h', 'include/nvsdk_ngx_helpers_dlssd_vk.h', '89351368b07b0afa28e0923de1b0276991efb9b5e95922c89047dfcfce70bb7d'),
    @('include\nvsdk_ngx_helpers_dlssg.h', 'include/nvsdk_ngx_helpers_dlssg.h', 'a8abdf14b689f2cbbbbed9991cb369facb1f8d5b260c5ae4426a0e9146d95dde'),
    @('include\nvsdk_ngx_helpers_dlssg_d3d.h', 'include/nvsdk_ngx_helpers_dlssg_d3d.h', '6343bc03fe12238f19a6d02c427eb9f9dc72433c58a00c164b05473aca710ed9'),
    @('include\nvsdk_ngx_helpers_dlssg_vk.h', 'include/nvsdk_ngx_helpers_dlssg_vk.h', 'eec70917a474a1f8be114eb11d5da3028cc752c410c3b3f12e03b29017d60726'),
    @('include\nvsdk_ngx_helpers_vk.h', 'include/nvsdk_ngx_helpers_vk.h', '754697d9faa93913985fc91fceae2073763461710d742a1f262ced2a7b6b62c9'),
    @('include\nvsdk_ngx_loader.h', 'include/nvsdk_ngx_loader.h', 'e0f8339e696dc26568a9d4aefee90c8497ce5ae048cc8b24ccc213cd3a8a2073'),
    @('include\nvsdk_ngx_params.h', 'include/nvsdk_ngx_params.h', '51a1f94116cf82f95c63a2c197371ed22e08bddb09918d36cc6e2ac712753c80'),
    @('include\nvsdk_ngx_params_dlssd.h', 'include/nvsdk_ngx_params_dlssd.h', '7e4fc6d5c84b436d31e289f863762a178ab2ef5d8c1ff40b614b01b342385f05'),
    @('include\nvsdk_ngx_params_dlssg.h', 'include/nvsdk_ngx_params_dlssg.h', '82b1d573b4b48d4cefc26ac01162a45646d99f0b84806dac9db3516cc5f733b5'),
    @('include\nvsdk_ngx_standalone_common.h', 'include/nvsdk_ngx_standalone_common.h', '231c6deb2515b5d30dccbc18a992cd5a2470a6bf3833b2a3b0fc7e3912bd9756'),
    @('include\nvsdk_ngx_standalone_cuda.h', 'include/nvsdk_ngx_standalone_cuda.h', '9cca6b101025e2c9e915c8e7f1dd8798dc7cbb7ae45dc5e98d7f73cd3045bfb9'),
    @('include\nvsdk_ngx_vk.h', 'include/nvsdk_ngx_vk.h', '3be54d0103cce02174fd34912c60c9f40c73b5eec919e42613a2268c589c53ad'),
    @('lib\nvsdk_ngx_d.lib', 'lib/Windows_x86_64/x64/nvsdk_ngx_d.lib', '4b6cecad7f1906571c94010241f650e4a5457e64fad49ddaccb82de79f6c2999'),
    @('bin\nvngx_dlss.dll', 'lib/Windows_x86_64/rel/nvngx_dlss.dll', '3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983'),
    @('bin\nvngx_dlssg.dll', 'lib/Windows_x86_64/rel/nvngx_dlssg.dll', 'ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82')
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
    Write-Host "fetch-dlss: $($file[0])"
    $partial = "$path.download"
    Invoke-WebRequest -UseBasicParsing -Uri "$source/$($file[1])" -OutFile $partial
    $hash = (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash
    if ($hash -ne $file[2]) {
        Remove-Item -LiteralPath $partial
        throw "fetch-dlss: $($file[0]) came with SHA-256 $hash, not $($file[2])"
    }
    Move-Item -Force -LiteralPath $partial -Destination $path
    $fetched++
}
if ($fetched -gt 0) {
    Write-Host "fetch-dlss: $fetched file(s) from NVIDIA/DLSS at $($commit.Substring(0, 10)), under NVIDIA's licence (third_party\dlss\LICENSE.txt)"
}
