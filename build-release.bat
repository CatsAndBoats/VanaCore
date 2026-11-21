@echo off
rem Builds VanaCore's Release and puts together what a player's VanaCore folder holds, in build\package\VanaCore (made
rem afresh each time from the build and the repository, never from build\Release\, which may hold a test run's files).
rem Debug symbols go to build\package\symbols.
rem
rem   VanaCore.exe, VanaCore.Core.dll   the build (x86 Release)
rem   VanaCore.Core.map                 the linker's map of the DLL, beside it: /vana stats names VanaCore's own
rem                                     functions from it (a tester's report says where the time went)
rem   dlss\                             VanaCore.Dlss.exe (the 64-bit DLSS helper, x64 Release), NVIDIA's DLSS and
rem                                     frame generation DLLs (third_party\dlss\fetch-dlss.ps1 fetches and checks them;
rem                                     those two by name, never another DLL in third_party\dlss\bin: DLSS 5's
rem                                     nvngx_dlssnr.dll isn't NVIDIA's to give out in the SDK, so a player adds it),
rem                                     AMD's FSR DLLs (amd_fidelityfx_loader_dx12.dll, amd_fidelityfx_upscaler_dx12.dll,
rem                                     amd_fidelityfx_framegeneration_dx12.dll: third_party\fsr\fetch-fsr.ps1 fetches
rem                                     and checks them; the same helper runs FSR and its frame generation),
rem                                     and the 64-bit C++ runtime the helper needs (Visual Studio's redistributable,
rem                                     beside it: a player may not have it installed)
rem   vanacore.example.ini              the launcher makes vanacore.ini from it
rem   scripts\boot.txt                  the default (the launcher never replaces a player's)
rem   addons\                           VanaCore's own addons (Ashita's are mirrored in by the launcher)
rem   plugins\vanacore\                 Ashita plugins a distribution ships with VanaCore, loaded before plugins\ashita's:
rem                                     whatever is in plugins\vanacore here (not in the repository; see its README.md)
rem   fonts\                            Material Symbols, and fonts\imgui\ (ImGui's); not fonts\game\
rem   docs\addons\                      the addon API docs (docs\addons in the repository: the series, its examples)
rem   docs\html\index.html              the same series as one page that opens from disk (docs\html, tools\gen_docs_html.py)
rem   LICENSE.txt, LICENSING.md         VanaCore's licence (GPL-3.0-or-later, with its additional permissions)
rem   THIRD_PARTY_NOTICES.md, licenses\ what VanaCore includes and ships, and their licenses (they go wherever
rem                                     VanaCore goes)
rem
rem Double-clicked (Explorer runs it as cmd /c <this file>), the window stays open at the end, however it ended. Not
rem from a command prompt, nor in CI.
setlocal EnableExtensions
set "ROOT=%~dp0"
set "PAUSE_AT_END="
echo(%cmdcmdline%| find /i "%~nx0" >nul && set "PAUSE_AT_END=1"
if defined CI set "PAUSE_AT_END="
call :main %*
set "RESULT=%errorlevel%"
if defined PAUSE_AT_END pause
exit /b %RESULT%

:main
if not "%~1"=="" (
    echo build-release: takes no arguments
    exit /b 1
)
set "PACKAGE=%ROOT%build\package"
set "STAGE=%PACKAGE%\VanaCore"
set "SYMBOLS=%PACKAGE%\symbols"
set "OUT=%ROOT%build\Release"
set "DLSS=%ROOT%third_party\dlss"
set "FSR=%ROOT%third_party\fsr"

rem ---- The version (src\shared\vanacore\version.hpp), for the summary
set "VERSION="
for /f tokens^=2^ delims^=^" %%v in ('findstr /l /c:"kVersion[] = L" "%ROOT%src\shared\vanacore\version.hpp"') do set "VERSION=%%v"

rem ---- NVIDIA's DLSS SDK: fetched from NVIDIA's repository when missing, and every file checked against its SHA-256,
rem so what ships is exactly NVIDIA's
powershell -NoProfile -ExecutionPolicy Bypass -File "%DLSS%\fetch-dlss.ps1"
if errorlevel 1 (
    echo build-release: couldn't fetch NVIDIA's DLSS SDK ^(third_party\dlss\fetch-dlss.ps1^)
    exit /b 1
)

rem ---- AMD's FSR DLLs: likewise from AMD's repository, each checked against its SHA-256 (the headers are in the repository)
powershell -NoProfile -ExecutionPolicy Bypass -File "%FSR%\fetch-fsr.ps1"
if errorlevel 1 (
    echo build-release: couldn't fetch AMD's FSR DLLs ^(third_party\fsr\fetch-fsr.ps1^)
    exit /b 1
)

rem ---- MSBuild, from the newest Visual Studio with C++
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo build-release: Visual Studio isn't installed ^(no vswhere.exe^)
    exit /b 1
)
set "MSBUILD="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
if not defined MSBUILD (
    echo build-release: no MSBuild with the C++ tools ^(Desktop development with C++^)
    exit /b 1
)

rem Rebuilt from clean: Release links with whole-program optimization, which refuses objects made by
rem another compiler version, and MSBuild keeps objects a different Visual Studio made (2022's, when
rem this picks 2026) as up to date.
echo Building VanaCore %VERSION% ^(Release, x86; the DLSS helper x64^)
"%MSBUILD%" "%ROOT%VanaCore.sln" "-t:VanaCore_Launcher:Rebuild;VanaCore_Core:Rebuild;VanaCore_Dlss:Rebuild" -p:Configuration=Release -p:Platform=x86 -m -v:minimal -nologo
if errorlevel 1 (
    echo build-release: the build failed
    exit /b 1
)
for %%f in (VanaCore.exe VanaCore.Core.dll dlss\VanaCore.Dlss.exe) do if not exist "%OUT%\%%f" (
    echo build-release: the build made no %%f
    exit /b 1
)

rem ---- The 64-bit C++ runtime for the DLSS helper: the newest Visual Studio redistributable's (a newer
rem runtime runs what an older compiler built)
set "VSPATH="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
set "CRT="
for /d %%v in ("%VSPATH%\VC\Redist\MSVC\1*") do for /d %%c in ("%%v\x64\Microsoft.VC*.CRT") do set "CRT=%%c"
if not defined CRT (
    echo build-release: no 64-bit C++ runtime in %VSPATH%\VC\Redist\MSVC ^(the C++ redistributable files^)
    exit /b 1
)
for %%f in (msvcp140.dll vcruntime140.dll vcruntime140_1.dll) do if not exist "%CRT%\%%f" (
    echo build-release: %CRT% has no %%f
    exit /b 1
)

rem ---- The package, afresh
echo Packaging into %STAGE%
if exist "%PACKAGE%" rmdir /s /q "%PACKAGE%"
mkdir "%STAGE%" "%STAGE%\scripts" "%STAGE%\licenses" "%STAGE%\dlss" "%SYMBOLS%" || exit /b 1
copy /y "%OUT%\VanaCore.exe" "%STAGE%\" >nul || exit /b 1
copy /y "%OUT%\VanaCore.Core.dll" "%STAGE%\" >nul || exit /b 1
if exist "%OUT%\VanaCore.Core.map" (
    copy /y "%OUT%\VanaCore.Core.map" "%STAGE%\" >nul || exit /b 1
) else (
    echo build-release: VanaCore.Core.map isn't in %OUT%: the package goes without it
)
copy /y "%OUT%\dlss\VanaCore.Dlss.exe" "%STAGE%\dlss\" >nul || exit /b 1
for %%f in (nvngx_dlss.dll nvngx_dlssg.dll) do copy /y "%DLSS%\bin\%%f" "%STAGE%\dlss\" >nul || exit /b 1
for %%f in (amd_fidelityfx_loader_dx12.dll amd_fidelityfx_upscaler_dx12.dll amd_fidelityfx_framegeneration_dx12.dll) do copy /y "%FSR%\bin\%%f" "%STAGE%\dlss\" >nul || exit /b 1
for %%f in (msvcp140.dll vcruntime140.dll vcruntime140_1.dll) do copy /y "%CRT%\%%f" "%STAGE%\dlss\" >nul || exit /b 1
copy /y "%ROOT%LICENSE" "%STAGE%\LICENSE.txt" >nul || exit /b 1
copy /y "%ROOT%LICENSING.md" "%STAGE%\" >nul || exit /b 1
copy /y "%ROOT%THIRD_PARTY_NOTICES.md" "%STAGE%\" >nul || exit /b 1
copy /y "%DLSS%\LICENSE.txt" "%STAGE%\licenses\LICENSE-NVIDIA-DLSS.txt" >nul || exit /b 1
copy /y "%FSR%\license.md" "%STAGE%\licenses\LICENSE-AMD-FSR-SDK.md" >nul || exit /b 1
copy /y "%ROOT%third_party\ashita-sdk\LICENSE-*.md" "%STAGE%\licenses\" >nul || exit /b 1
copy /y "%ROOT%vanacore.example.ini" "%STAGE%\" >nul || exit /b 1
copy /y "%ROOT%scripts\boot.txt" "%STAGE%\scripts\" >nul || exit /b 1
robocopy "%ROOT%addons" "%STAGE%\addons" /E /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1
rem Ashita plugins a distribution ships with VanaCore: VanaCore loads them from plugins\vanacore before plugins\ashita,
rem and the launcher's Ashita mirror never touches that folder. Only what's there besides the folder's README.
robocopy "%ROOT%plugins\vanacore" "%STAGE%\plugins\vanacore" /E /XF README.md /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1
robocopy "%ROOT%fonts" "%STAGE%\fonts" /E /XD "%ROOT%fonts\game" /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1
rem The addon API docs (the series docs\addons, its examples included), for addon authors, beside the addons they describe,
rem and docs\html\index.html, the series as one page that opens from disk (tools\gen_docs_html.py builds it)
robocopy "%ROOT%docs\addons" "%STAGE%\docs\addons" /E /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1
mkdir "%STAGE%\docs\html" || exit /b 1
copy /y "%ROOT%docs\html\index.html" "%STAGE%\docs\html\" >nul || exit /b 1
copy /y "%OUT%\VanaCore.pdb" "%SYMBOLS%\" >nul
copy /y "%OUT%\VanaCore.Core.pdb" "%SYMBOLS%\" >nul
copy /y "%OUT%\dlss\VanaCore.Dlss.pdb" "%SYMBOLS%\" >nul

echo VanaCore %VERSION% packaged in %STAGE%
exit /b 0
