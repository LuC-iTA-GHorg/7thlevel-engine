@echo off
rem Prepares GAME_ROOT\dist\ with everything needed to play, except the files of the original game:
rem   <game>.exe, <game>_launcher.exe, SDL2.dll, GeneralUser-GS.sf2 (if present), sample <game>.ini
rem (<game> = GAME_NAME of the profile build.bat copied to build\game.h). Then copy the .BIN files
rem of the game into the same folder and start <game>_launcher.exe. Requires build\ (build.bat).
rem GAME_ROOT as in build.bat (default: the engine folder).
setlocal
set "ENGINE=%~dp0.."
if "%GAME_ROOT%"=="" set "GAME_ROOT=%ENGINE%"
set "OUT=%GAME_ROOT%\dist"
if not exist "%GAME_ROOT%\build\game.h" (
	echo build\game.h not found: run build.bat first
	exit /b 1
)
for /f "tokens=3*" %%a in ('findstr /b /c:"#define GAME_NAME " "%GAME_ROOT%\build\game.h"') do set "NAME=%%~a"
for /f "tokens=3" %%a in ('findstr /b /c:"#define GAME_START " "%GAME_ROOT%\build\game.h"') do set "START=%%~a"
if not exist "%GAME_ROOT%\build\%NAME%.exe" (
	echo build\%NAME%.exe not found: run build.bat first
	exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"
copy /y "%GAME_ROOT%\build\%NAME%.exe" "%OUT%\" >nul
copy /y "%GAME_ROOT%\build\%NAME%_launcher.exe" "%OUT%\" >nul
copy /y "%GAME_ROOT%\build\SDL2.dll" "%OUT%\" >nul
set "SF2="
if exist "%ENGINE%\tools\soundfont\GeneralUser-GS.sf2" set "SF2=%ENGINE%\tools\soundfont\GeneralUser-GS.sf2"
if exist "%GAME_ROOT%\tools\soundfont\GeneralUser-GS.sf2" set "SF2=%GAME_ROOT%\tools\soundfont\GeneralUser-GS.sf2"
if defined SF2 (
	copy /y "%SF2%" "%OUT%\" >nul
) else (
	echo WARNING: no SoundFont in tools\soundfont\: the music will be silent
)
copy /y "%ENGINE%\port\game.ini.example" "%OUT%\%NAME%.ini" >nul
(
	echo Unofficial SDL2 port of the 7th Level engine ^(Disney's Hot Shots / Jungle Arcade games, 1996^).
	echo Not affiliated with or endorsed by Disney or 7th Level. No game data is included.
	echo.
	echo Copy into this folder the files of your original game CD: *.BIN ^(%START% and the others^),
	echo *.EXE and *.TTF if present. Then start %NAME%_launcher.exe, choose the settings and start
	echo the game ^(or run %NAME%.exe directly: it reads %NAME%.ini^).
	echo.
	echo Required files: %NAME%.exe, SDL2.dll, the .BIN files, GeneralUser-GS.sf2 ^(music^).
	echo SDL2: zlib license. SoundFont: GeneralUser GS by S. Christian Collins, own license.
) > "%OUT%\README.txt"
echo Ready: %OUT%   ^(add the .BIN files of the original game^)
exit /b 0
