@echo off
rem Builds harness.exe: src\core\dsound_follow.cpp over real DirectSound, called the way FFXiMain.dll calls it.
rem   harness.exe          plays a tone through a wrapped buffer and moves it to the default output
rem   harness.exe --flip   also switches Windows' default output to another device and back (changes sound settings)
setlocal
set ROOT=%~dp0..\..
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d %~dp0
cl /nologo /std:c++17 /EHsc /MT /O1 /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
  /I %ROOT%\src\core harness.cpp %ROOT%\src\core\dsound_follow.cpp /Fe:harness.exe /link ole32.lib
