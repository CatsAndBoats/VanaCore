@echo off
rem Code-quality checks over VanaCore's C/C++ (src\), tools\lint\lint.py's: cppcheck (unused variables, leaks, bad
rem practice), lizard (functions too complex, long or with too many parameters), jscpd (copy-pasted code) and file
rem length; and d3d11fx (Direct3D 11's scene effects still build from Direct3D 12's HLSL), which any problem fails. A
rem tool that isn't installed is skipped, with the line that installs it. Reports go to build\lint\.
rem
rem   lint.bat                    all five, failing only for a finding new or grown past its allowance over
rem                               tools\lint\baseline.json (lint.py's ALLOWANCE; d3d11fx fails for any)
rem   lint.bat cppcheck lizard    just those
rem   lint.bat --update-baseline  write today's findings as the baseline (commit it; run it after fixing things too).
rem                               A finding that grew keeps its old value; add --accept-growth to write the new one
rem   lint.bat --report-only      print the findings, never fail
rem   lint.bat --strict           a missing tool fails the run (what CI does)
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
rem The first python on PATH that runs (Windows' own python.exe stub in WindowsApps, for a Python that isn't
rem installed, only opens the Store), else the py launcher
set "PY="
for /f "delims=" %%p in ('where python 2^>nul') do if not defined PY "%%p" --version >nul 2>&1 && set PY="%%p"
if not defined PY py -3 --version >nul 2>&1 && set "PY=py -3"
if not defined PY (
    echo lint: Python 3 isn't installed
    exit /b 1
)
%PY% "%ROOT%tools\lint\lint.py" %*
exit /b %errorlevel%
