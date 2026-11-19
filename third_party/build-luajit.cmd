@rem Builds third_party\luajit as a static 32-bit library for VanaCore.Core: the steps of LuaJIT's
@rem src\msvcbuild.bat "static", with VanaCore's static C runtime (/MT, /MTd for Debug) and without
@rem its dllexport'ed stdio. Works on a copy of the sources, so the vendored tree stays untouched.
@rem
@rem   build-luajit.cmd <Debug|Release> <output folder>
@rem
@rem Leaves <output folder>\lua51.lib and the headers in <output folder>\src. The Core project runs
@rem it when a LuaJIT source is newer than the library.
@echo off
setlocal
set CONFIG=%~1
set OUT=%~2
if "%OUT%"=="" (
    echo usage: build-luajit.cmd ^<Debug^|Release^> ^<output folder^>
    exit /b 2
)
if exist "%OUT%\lua51.lib" del "%OUT%\lua51.lib"

rem MSBuild's C++ targets put the x86 compiler on PATH; from a plain prompt, find it.
where cl >nul 2>nul
if not errorlevel 1 goto :have_compiler
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto :no_compiler
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul
:have_compiler

echo Building LuaJIT (%CONFIG%) in %OUT%
robocopy "%~dp0luajit\src" "%OUT%\src" /MIR /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1
robocopy "%~dp0luajit\dynasm" "%OUT%\dynasm" /MIR /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1
copy /y "%~dp0luajit\.relver" "%OUT%\.relver" >nul

set CRT=/MT
if /i "%CONFIG%"=="Debug" set CRT=/MTd
set LJCOMPILE=cl /nologo /c /O2 /W3 /Z7 /arch:SSE2 %CRT% /D_CRT_SECURE_NO_DEPRECATE
set LJLINK=link /nologo
set ALL_LIB=lib_base.c lib_math.c lib_bit.c lib_string.c lib_table.c lib_io.c lib_os.c lib_package.c lib_debug.c lib_jit.c lib_ffi.c lib_buffer.c

pushd "%OUT%\src"
%LJCOMPILE% host\minilua.c || goto :bad
%LJLINK% /out:minilua.exe minilua.obj || goto :bad
rem minilua exits with the size of a pointer: 4 for x86.
.\minilua.exe
if errorlevel 8 goto :not_x86
if not errorlevel 4 goto :bad
.\minilua.exe ..\dynasm\dynasm.lua -LN -D WIN -D JIT -D FFI -D ENDIAN_LE -D FPU -o host\buildvm_arch.h vm_x86.dasc || goto :bad
type ..\.relver > luajit_relver.txt
.\minilua.exe host\genversion.lua || goto :bad
%LJCOMPILE% /I . /I ..\dynasm host\buildvm*.c || goto :bad
%LJLINK% /out:buildvm.exe buildvm*.obj || goto :bad
.\buildvm.exe -m peobj -o lj_vm.obj || goto :bad
.\buildvm.exe -m bcdef -o lj_bcdef.h %ALL_LIB% || goto :bad
.\buildvm.exe -m ffdef -o lj_ffdef.h %ALL_LIB% || goto :bad
.\buildvm.exe -m libdef -o lj_libdef.h %ALL_LIB% || goto :bad
.\buildvm.exe -m recdef -o lj_recdef.h %ALL_LIB% || goto :bad
.\buildvm.exe -m vmdef -o jit\vmdef.lua %ALL_LIB% || goto :bad
.\buildvm.exe -m folddef -o lj_folddef.h lj_opt_fold.c || goto :bad
%LJCOMPILE% lj_*.c lib_*.c || goto :bad
lib /nologo /nodefaultlib /OUT:..\lua51.lib lj_*.obj lib_*.obj || goto :bad
del *.obj minilua.exe buildvm.exe
popd
echo LuaJIT built: %OUT%\lua51.lib
exit /b 0

:not_x86
echo LuaJIT: the compiler on PATH builds 64-bit code; VanaCore needs x86.
:bad
popd
echo LuaJIT build FAILED
if exist "%OUT%\lua51.lib" del "%OUT%\lua51.lib"
exit /b 1

:no_compiler
echo LuaJIT: no Visual Studio C++ compiler found; run from a Visual Studio x86 command prompt.
exit /b 1
