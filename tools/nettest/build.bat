@echo off
rem Builds harness.exe: one LuaJIT state running VanaCore's real vana.lua, settings.lua and net.lua over the real engine
rem (src\core\net.cpp, lua_net.cpp). Needs the Release LuaJIT the Core build makes (build\obj\luajit\Release).
setlocal
set ROOT=%~dp0..\..
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d %~dp0
cl /nologo /std:c++17 /EHsc /MT /O1 /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
  /I %ROOT%\build\obj\luajit\Release\src /I %ROOT%\src\core /I %ROOT%\src\shared ^
  harness.cpp %ROOT%\src\core\net.cpp %ROOT%\src\core\lua_net.cpp %ROOT%\build\obj\luajit\Release\lua51.lib /Fe:harness.exe
