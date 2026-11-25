# Puts VanaCore App's files (VanaCoreApp.exe and VanaCoreApp.dll, signed, from an App release's zip: app.yaml makes
# them) where VanaCore has them: into a VanaCore package folder, into a VanaCore release's zip, and into a build on R2.
# The App is released on its own (app.yaml), and VanaCore always carries its newest published one:
#
#   release.yaml   -Folder build/package/VanaCore              the package it pushes to vcdev and zips
#   promote.yaml   -Build vanacore/vanacore -Release <tag>     after promoting: vcdev's copy may be older than the newest
#   app.yaml prod  -App <tag> -Build vanacore/vanacore -Release latest
#                                                              a new App into the VanaCore players have, no VanaCore release
#
#   -App      the App release's tag (app-v0.1.2), draft or not; "" the newest published one (none: nothing done)
#   -Folder   copy the two files into this folder
#   -Build    push them into this R2 build (push_build.py --partial: nothing else on it touched)
#   -Release  put them into this VanaCore release's VanaCore-<version>.zip (re-uploaded under the same name) and say
#             "Includes VanaCore App <version>." at the end of its notes; "latest" for the release marked latest
#   -DryRun   push_build.py lists what it would push; the release is left as it is
#
# GH_TOKEN for GitHub, and with -Build the R2 secrets push_build.py reads. Prints the App's version.
param(
  [Parameter(Mandatory)][string]$Repo,
  [string]$App = '',
  [string]$Folder = '',
  [string]$Build = '',
  [string]$Release = '',
  [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$work = Join-Path ([IO.Path]::GetTempPath()) "bundle-app-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $work | Out-Null

# A release asset by its id (a draft's too), with the token
function Get-Asset($asset, $to) {
  Invoke-WebRequest -UseBasicParsing -OutFile $to -Uri "https://api.github.com/repos/$Repo/releases/assets/$($asset.id)" `
    -Headers @{ Authorization = "Bearer $env:GH_TOKEN"; Accept = 'application/octet-stream' }
}

$releases = @(gh api "repos/$Repo/releases?per_page=100" | ConvertFrom-Json)
if ($LASTEXITCODE) { throw "Couldn't list $Repo's releases" }

# ---- The App release
$apps = @($releases | Where-Object { $_.tag_name -like 'app-v*' })
$app = if ($App) { @($apps | Where-Object tag_name -eq $App)[0] }
       else { @($apps | Where-Object { -not $_.draft } | Sort-Object { [datetime]$_.published_at } -Descending)[0] }
if (-not $app) {
  if ($App) { throw "No App release $App" }
  'No VanaCore App release published yet: nothing bundled'
  return
}
$appZip = @($app.assets | Where-Object name -like 'VanaCoreApp-*.zip')[0]
if (-not $appZip) { throw "$($app.name) has no VanaCoreApp-*.zip" }
Get-Asset $appZip (Join-Path $work app.zip)
Expand-Archive -Path (Join-Path $work app.zip) -DestinationPath (Join-Path $work app)
$files = 'VanaCoreApp.exe', 'VanaCoreApp.dll'
foreach ($f in $files) {
  $path = Join-Path $work "app/$f"
  if (-not (Test-Path $path)) { throw "$($app.name)'s zip has no $f" }
  if ((Get-AuthenticodeSignature -LiteralPath $path).Status -ne 'Valid') { throw "$($app.name)'s $f has no valid signature" }
}
$version = $app.tag_name -replace '^app-v', ''
"VanaCore App $version ($($app.tag_name)$(if ($app.draft) { ', draft' }))"

function Copy-App($to) {
  foreach ($f in $files) { Copy-Item -LiteralPath (Join-Path $work "app/$f") -Destination (Join-Path $to $f) -Force }
}

# ---- Into a package folder
if ($Folder) {
  if (-not (Test-Path $Folder)) { throw "No folder $Folder" }
  Copy-App $Folder
  "  into $Folder"
}

# ---- Into a build on R2
if ($Build) {
  $push = Join-Path $work push
  New-Item -ItemType Directory -Path $push | Out-Null
  Copy-App $push
  $flags = @('--partial')
  if ($DryRun) { $flags += '--dry-run' }
  python (Join-Path $PSScriptRoot push_build.py) $Build --folder $push @flags
  if ($LASTEXITCODE) { throw "Pushing VanaCore App to $Build failed" }
}

# ---- Into a VanaCore release's zip
if ($Release -and -not $DryRun) {
  $target = if ($Release -eq 'latest') { gh api "repos/$Repo/releases/latest" | ConvertFrom-Json }
            else { @($releases | Where-Object tag_name -eq $Release)[0] }
  if (-not $target) { throw "No VanaCore release $Release" }
  $zipAsset = @($target.assets | Where-Object name -like 'VanaCore-*.zip')[0]
  if (-not $zipAsset) { throw "$($target.name) has no VanaCore-*.zip" }
  $zip = Join-Path $work $zipAsset.name
  Get-Asset $zipAsset $zip
  $unzipped = Join-Path $work vanacore
  Expand-Archive -Path $zip -DestinationPath $unzipped
  if (-not (Test-Path (Join-Path $unzipped 'VanaCore/VanaCore.exe'))) { throw "$($zipAsset.name) has no VanaCore\VanaCore.exe" }
  Copy-App (Join-Path $unzipped VanaCore)
  Remove-Item -LiteralPath $zip
  Compress-Archive -Path (Join-Path $unzipped VanaCore) -DestinationPath $zip
  gh release upload $target.tag_name $zip --clobber --repo $Repo
  if ($LASTEXITCODE) { throw "Uploading $($zipAsset.name) failed" }
  # its notes say which App it has: the last line, replaced
  $body = ((($target.body -replace "`r", '') -split "`n" | Where-Object { $_ -notmatch '^Includes VanaCore App ' }) -join "`n").TrimEnd()
  Set-Content -Path (Join-Path $work notes.md) -Value "$body`n`nIncludes VanaCore App $version." -Encoding utf8
  gh release edit $target.tag_name --notes-file (Join-Path $work notes.md) --repo $Repo
  if ($LASTEXITCODE) { throw "Editing $($target.name)'s notes failed" }
  "  into $($target.name)'s $($zipAsset.name)"
}
