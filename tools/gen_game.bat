@echo off
rem Translates to C the game logic (type 16 scripts and type 11 lists) of all the .BIN files of a game.
rem   gen_game.bat [game_folder] [output_folder] [build_folder]
rem Default output: port\game\  (one .c file per archive + game_libs.c); scr2c.exe goes into
rem build_folder (default build\).
rem build.bat calls it automatically (output in GAME_ROOT\build\gamesrc\) when GAME_ROOT\port\game\
rem does not exist. To translate a single archive: build\scr2c.exe lib FILE.BIN out.c
setlocal EnableDelayedExpansion
set "ROOT=%~dp0.."
where gcc >nul 2>&1 || call "%~dp0env.bat"
set "GAME=%~1"
if "%GAME%"=="" set "GAME=%ROOT%"
set "OUT=%~2"
if "%OUT%"=="" set "OUT=%ROOT%\port\game"
set "BUILD=%~3"
if "%BUILD%"=="" set "BUILD=%ROOT%\build"
if not exist "%BUILD%" mkdir "%BUILD%"
if not exist "%OUT%" mkdir "%OUT%"
gcc -std=c11 -O2 -Wall -Wextra -o "%BUILD%\scr2c.exe" "%ROOT%\port\tools\scr2c.c" "%ROOT%\port\src\lb7.c" || exit /b 1
set "BINS="
for %%F in ("%GAME%\*.bin") do (
	set "N=%%~nF"
	"%BUILD%\scr2c.exe" lib "%%F" "%OUT%\!N!.c" || exit /b 1
	set "BINS=!BINS! "%%F""
)
"%BUILD%\scr2c.exe" registry "%OUT%\game_libs.c" %BINS% || exit /b 1
endlocal
