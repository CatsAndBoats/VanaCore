@rem Tests the Lua VanaCore builds into VanaCore.Core.dll, with the same patched LuaJIT (build\obj\luajit\Release,
@rem made by a Release build of VanaCore.Core): Windower's pack (string.pack and string.unpack in
@rem src\core\lua\compat\windower.lua), byte for byte, and, given Windower's libraries, every packet its packets
@rem library knows built and parsed back; then the https adapter (socket.ssl.https over WinHTTP, in
@rem src\core\lua\compat\ashita.lua), against real servers (needs a network); then how an addon's require finds
@rem modules (the package.path code in src\core\lua_api.cpp, run against a made-up folder); then addons' settings
@rem (vana.settings and vana.json, src\core\lua\settings.lua with src\core\lua\vana.lua, in build\obj\luatest\settings_test).
@rem
@rem   run.cmd [Windower libs folder]   (default: build\Debug\addons\windower\libs, if it's there)
@echo off
setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%..\..\"
set "LUAJIT=%ROOT%build\obj\luajit\Release"
if not exist "%LUAJIT%\lua51.lib" (
    echo luatest: no %LUAJIT%\lua51.lib: build VanaCore.Core in Release first
    exit /b 1
)
set "LIBS=%~1"
if "%LIBS%"=="" if exist "%ROOT%build\Debug\addons\windower\libs\packets.lua" set "LIBS=%ROOT%build\Debug\addons\windower\libs"

where cl >nul 2>nul
if not errorlevel 1 goto :have_compiler
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul
:have_compiler
set "OUT=%ROOT%build\obj\luatest"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /MT /O1 /I "%LUAJIT%\src" "%HERE%luarun.c" /Fo"%OUT%\luarun.obj" /link "%LUAJIT%\lua51.lib" /OUT:"%OUT%\luarun.exe" >nul || exit /b 1

if defined LIBS (
    "%OUT%\luarun.exe" "%HERE%pack_test.lua" "%ROOT%src\core\lua\compat\windower.lua" "%LIBS%"
) else (
    "%OUT%\luarun.exe" "%HERE%pack_test.lua" "%ROOT%src\core\lua\compat\windower.lua"
)

"%OUT%\luarun.exe" "%HERE%https_test.lua" "%ROOT%src\core\lua\compat\ashita.lua"

"%OUT%\luarun.exe" "%HERE%require_test.lua" "%ROOT%src\core\lua_api.cpp" "%ROOT%src\core\lua\compat\ashita.lua"

"%OUT%\luarun.exe" "%HERE%settings_test.lua" "%ROOT%src\core\lua\vana.lua" "%ROOT%src\core\lua\settings.lua" "%OUT%\settings_test"
