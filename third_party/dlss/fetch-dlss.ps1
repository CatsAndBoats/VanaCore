<#
Fetches the parts of NVIDIA's DLSS SDK that VanaCore builds with and ships, from NVIDIA's own repository
(https://github.com/NVIDIA/DLSS) at the commit VanaCore is built against (DLSS SDK 310.9.1), into this folder:

  include\*.h             the NGX headers
  lib\nvsdk_ngx_d.lib     the NGX loader for 64-bit Windows (/MD), linked into VanaCore.Dlss.exe
  bin\nvngx_dlss.dll      DLSS Super Resolution, shipped beside VanaCore.Dlss.exe
  bin\nvngx_dlssg.dll     DLSS Frame Generation, shipped beside it too
  LICENSE.txt             NVIDIA's licence for all of them (the NVIDIA RTX SDKs licence)

They aren't in VanaCore's repository: NVIDIA's licence lets them be distributed only as part of an application, not on
their own. Each file is checked against its SHA-256 (a text file's with its line ends as NVIDIA's, LF, so a copy
that git checked out with CRLF counts as the same file); one that's here already and right is left alone.
VanaCore.Dlss's build and build-release.bat run this when a file is missing.

  powershell -NoProfile -ExecutionPolicy Bypass -File third_party\dlss\fetch-dlss.ps1

By fetching these you accept NVIDIA's licence (LICENSE.txt, once fetched).
#>
$ErrorActionPreference = 'Stop'
$commit = '374959484e79a640feaba44c93ac8cfb0a03f5b5'
$source = "https://raw.githubusercontent.com/NVIDIA/DLSS/$commit"

# here, there (in NVIDIA's repository), SHA-256
$files = @(
    @('LICENSE.txt', 'LICENSE.txt', 'd4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0'),
    @('include\nvsdk_ngx.h', 'include/nvsdk_ngx.h', 'dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848'),
    @('include\nvsdk_ngx_defs.h', 'include/nvsdk_ngx_defs.h', 'ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e'),
    @('include\nvsdk_ngx_defs_dlssd.h', 'include/nvsdk_ngx_defs_dlssd.h', '2e97fe1595b1334f71b51724f2cb67496e8c296a7a2eec5606da53055882e716'),
    @('include\nvsdk_ngx_defs_dlssg.h', 'include/nvsdk_ngx_defs_dlssg.h', '98ec3a6faa5aa250828c174bf8f3491aafac693dfe57cc798049c8918597fc60'),
    @('include\nvsdk_ngx_defs_vk.h', 'include/nvsdk_ngx_defs_vk.h', '0a24d0861ace7d6b9362a67b7f08bea1b33ea123c5ce730b19133de8a891d031'),
    @('include\nvsdk_ngx_helpers.h', 'include/nvsdk_ngx_helpers.h', '5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471'),
    @('include\nvsdk_ngx_helpers_cuda.h', 'include/nvsdk_ngx_helpers_cuda.h', 'f2851a76107bdf4fbcb7b261f3c35573c312d8ecb4bc18fb02d9e2cec7705042'),
    @('include\nvsdk_ngx_helpers_d3d.h', 'include/nvsdk_ngx_helpers_d3d.h', 'ec75224f36ed6580baaf250fa83620405ad81a1d98c2c436654a0c9cf6a6b8ba'),
    @('include\nvsdk_ngx_helpers_dlssd.h', 'include/nvsdk_ngx_helpers_dlssd.h', 'a25fdda925b479dfe869593aeba311ef08793da54e0391f2fe2fe756cbef5532'),
    @('include\nvsdk_ngx_helpers_dlssd_cuda.h', 'include/nvsdk_ngx_helpers_dlssd_cuda.h', '7e35cde214d905346a487b9a28c05d35e1c55b480e237fe3de69580a7e332730'),
    @('include\nvsdk_ngx_helpers_dlssd_d3d.h', 'include/nvsdk_ngx_helpers_dlssd_d3d.h', '6df9d02c6f47feee3afc8d12340a9f554a7ddce4ddacb1b6a13a42b26c2c6293'),
    @('include\nvsdk_ngx_helpers_dlssd_vk.h', 'include/nvsdk_ngx_helpers_dlssd_vk.h', '11df8fbda532a97fe867c8ef2b40058b49b7898183710f3468e55b1eedb8f40b'),
    @('include\nvsdk_ngx_helpers_dlssg.h', 'include/nvsdk_ngx_helpers_dlssg.h', '6881d21419c40c78ec948b326902ece09fc112b0223144d51c4905f3fced7a1f'),
    @('include\nvsdk_ngx_helpers_dlssg_d3d.h', 'include/nvsdk_ngx_helpers_dlssg_d3d.h', '6335ebcb95de39a361e87fe0257d6ce0723e5d8deaa15db3b35afacf2b3f4220'),
    @('include\nvsdk_ngx_helpers_dlssg_vk.h', 'include/nvsdk_ngx_helpers_dlssg_vk.h', 'b371d90b9f4372b9a415e68a0afbdc2f31b7b552e219a9637d848749bc965316'),
    @('include\nvsdk_ngx_helpers_vk.h', 'include/nvsdk_ngx_helpers_vk.h', 'c192bff72138f12f770db48e12c1d8f712dfc81ea0e8e055ebb27d8dd1b31623'),
    @('include\nvsdk_ngx_loader.h', 'include/nvsdk_ngx_loader.h', 'abdf5f66a692620538357d29fdf78e3e2a677ee8140b7a638962ecb766585e75'),
    @('include\nvsdk_ngx_params.h', 'include/nvsdk_ngx_params.h', '943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc'),
    @('include\nvsdk_ngx_params_dlssd.h', 'include/nvsdk_ngx_params_dlssd.h', 'd41e3ba8f1d72583b55273092c99bf426dc4a0d46de827229e25d5b1325a370e'),
    @('include\nvsdk_ngx_params_dlssg.h', 'include/nvsdk_ngx_params_dlssg.h', 'c24fa5aa68ffa2808c828b75cfa32843f1606f80a145f84055ac450d7e6c74ad'),
    @('include\nvsdk_ngx_standalone_common.h', 'include/nvsdk_ngx_standalone_common.h', '38413c3338b0aea0202c07da95d426bfee7080a42913823fa2d9dd585b087b53'),
    @('include\nvsdk_ngx_standalone_cuda.h', 'include/nvsdk_ngx_standalone_cuda.h', 'b23436ed5040051ac8e81554c9af080f2463c941435bbc3b82222e5431a09499'),
    @('include\nvsdk_ngx_vk.h', 'include/nvsdk_ngx_vk.h', '2d364ce7132881eb669e9498fd570d74cba563b1fbebc82c235f8e1ad2dd8b6d'),
    @('lib\nvsdk_ngx_d.lib', 'lib/Windows_x86_64/x64/nvsdk_ngx_d.lib', '4b6cecad7f1906571c94010241f650e4a5457e64fad49ddaccb82de79f6c2999'),
    @('bin\nvngx_dlss.dll', 'lib/Windows_x86_64/rel/nvngx_dlss.dll', '3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983'),
    @('bin\nvngx_dlssg.dll', 'lib/Windows_x86_64/rel/nvngx_dlssg.dll', 'ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82')
)

# Windows PowerShell 5.1 may not offer TLS 1.2 by itself; its progress bar slows downloads down a great deal
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
$ProgressPreference = 'SilentlyContinue'

# A file's SHA-256; a text file's (.h, .txt) with CRLF taken back to LF first, as NVIDIA's repository has it
function Get-Sha256([string] $path, [string] $name = $path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    if ($name -match '\.(h|txt)$') {
        $latin1 = [Text.Encoding]::GetEncoding(28591)  # one byte, one character, both ways
        $bytes = $latin1.GetBytes($latin1.GetString($bytes).Replace("`r`n", "`n"))
    }
    $sha = [Security.Cryptography.SHA256]::Create()
    return ($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }) -join ''
}

$fetched = 0
foreach ($file in $files) {
    $path = Join-Path $PSScriptRoot $file[0]
    if ((Test-Path -LiteralPath $path) -and (Get-Sha256 $path) -eq $file[2]) {
        continue
    }
    New-Item -ItemType Directory -Force -Path (Split-Path $path) | Out-Null
    Write-Host "fetch-dlss: $($file[0])"
    $partial = "$path.download"
    Invoke-WebRequest -UseBasicParsing -Uri "$source/$($file[1])" -OutFile $partial
    $hash = Get-Sha256 $partial $file[0]
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
