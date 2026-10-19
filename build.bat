@echo off
rem Builds the SDL2 port of a 7th Level game (build\<game>.exe, build\<game>_launcher.exe) and the
rem tools (build\lb7tool.exe) with the portable GCC in tools\w64devkit and SDL2 in tools\SDL2.
rem Nothing is installed.
rem   build.bat [GAME] [debug^|clean]
rem GAME = a profile in games\ (upsy, bowl, chiseler, foodfeud, bugdrop, burper, hippohop,
rem pinball, slingshooter). Without it: the GAME_PROFILE variable, otherwise the first profile
rem whose original EXE (GAME_EXE) is in the data folder.
rem Folders (environment variables, all optional):
rem   GAME_ROOT  project of the game (default: this folder). A game repository that has the engine
rem              as a submodule in engine\ sets it to its own root and calls engine\build.bat.
rem   GAME_DATA  files of YOUR copy of the game (*.BIN, *.EXE, *.DLL); default GAME_ROOT.
rem Output in GAME_ROOT\build. Game logic: the scripts of the .BIN files are translated to C by
rem scr2c. If GAME_ROOT\port\game\ exists (private development tree) it is used as is; otherwise
rem the C files are generated into build\gamesrc\ from the game files.
setlocal EnableDelayedExpansion
set "ENGINE=%~dp0"
set "ENGINE=%ENGINE:~0,-1%"
if "%GAME_ROOT%"=="" set "GAME_ROOT=%ENGINE%"
for %%I in ("%GAME_ROOT%") do set "GAME_ROOT=%%~fI"
set "DATA=%GAME_DATA%"
if "%DATA%"=="" set "DATA=%GAME_ROOT%"
set "OUT=%GAME_ROOT%\build"

set "GAME=%GAME_PROFILE%"
set "MODE="
for %%A in (%*) do (
	if /i "%%~A"=="debug" (set "MODE=debug") else if /i "%%~A"=="clean" (set "MODE=clean") else set "GAME=%%~A"
)
if "%MODE%"=="clean" (
	if exist "%OUT%" rmdir /s /q "%OUT%"
	echo build\ deleted
	exit /b 0
)
if "%GAME%"=="" (
	for %%P in ("%ENGINE%\games\*.h") do if "!GAME!"=="" (
		for /f "tokens=3" %%a in ('findstr /b /c:"#define GAME_EXE " "%%P"') do if exist "%DATA%\%%~a" set "GAME=%%~nP"
	)
)
if "%GAME%"=="" (
	echo.
	echo No game given and no known game found in "%DATA%".
	echo Usage: build.bat GAME   ^(profiles in games\: upsy, bowl, chiseler, foodfeud, bugdrop,
	echo burper, hippohop, pinball, slingshooter^), with the game files in GAME_DATA or GAME_ROOT.
	goto :error
)
set "PROFILE=%ENGINE%\games\%GAME%.h"
if not exist "%PROFILE%" (
	echo Unknown game "%GAME%": no games\%GAME%.h
	goto :error
)
for /f "tokens=3" %%a in ('findstr /b /c:"#define GAME_NAME " "%PROFILE%"') do set "NAME=%%~a"
for /f "tokens=3" %%a in ('findstr /b /c:"#define GAME_EXE_RES " "%PROFILE%"') do set "EXE=%%~a"
for /f "tokens=3" %%a in ('findstr /b /c:"#define GAME_START " "%PROFILE%"') do set "START=%%~a"

rem Portable tools: those of the game project if present (development tree), otherwise the
rem engine's; on a new PC setup_tools downloads them (asks for confirmation)
set "TOOLS=%ENGINE%\tools"
if exist "%GAME_ROOT%\tools\w64devkit\bin\gcc.exe" if exist "%GAME_ROOT%\tools\SDL2\x86_64-w64-mingw32\bin\SDL2.dll" set "TOOLS=%GAME_ROOT%\tools"
if not exist "%TOOLS%\w64devkit\bin\gcc.exe" set "NEED_TOOLS=1"
if not exist "%TOOLS%\SDL2\x86_64-w64-mingw32\bin\SDL2.dll" set "NEED_TOOLS=1"
if defined NEED_TOOLS (
	echo Build tools missing in %TOOLS%: running tools\setup_tools.bat build
	call "%ENGINE%\tools\setup_tools.bat" build || goto :error
)
set "SDL2_DIR=%TOOLS%\SDL2\x86_64-w64-mingw32"
set "PATH=%TOOLS%\w64devkit\bin;%SDL2_DIR%\bin;%PATH%"

set "CFLAGS=-std=c11 -O2 -Wall -Wextra"
if "%MODE%"=="debug" set "CFLAGS=-std=c11 -O0 -g -Wall -Wextra"
if not exist "%OUT%" mkdir "%OUT%"
echo Game: %GAME% ^(games\%GAME%.h^), data in %DATA%
rem the profile is included as "game.h" by every source (-I build)
copy /y "%PROFILE%" "%OUT%\game.h" >nul || goto :error
set "SRC=%ENGINE%\port\src"

echo [1/7] lb7tool
gcc %CFLAGS% -o "%OUT%\lb7tool.exe" "%ENGINE%\src\tools\lb7tool.c" "%SRC%\lb7.c" || goto :error

echo [2/7] game logic (scripts of the .BIN files translated to C)
set "GAMESRC=%GAME_ROOT%\port\game"
if exist "%GAMESRC%\game_libs.c" goto :gamesrc_ok
set "GAMESRC=%OUT%\gamesrc"
if not exist "%DATA%\%START%" (
	echo.
	echo The original game files were not found in "%DATA%" ^(%START% is missing^).
	echo Copy the files of your game CD ^(*.BIN, *.EXE, *.DLL^) into the project root, or set
	echo GAME_DATA to the folder that contains them, then run build.bat again.
	goto :error
)
if exist "%GAMESRC%\game_libs.c" goto :gamesrc_ok
call "%ENGINE%\tools\gen_game.bat" "%DATA%" "%GAMESRC%" "%OUT%" >"%OUT%\gen_game.log" 2>&1 || (
	echo scr2c failed: see build\gen_game.log
	goto :error
)
:gamesrc_ok
echo       from %GAMESRC%

rem Windows resources (port\res\game.rc): version info, manifest and, if available, the icon of
rem the original EXE (extracted by lb7tool at build time, never stored in the repository).
rem windres passes the -I folders to the preprocessor without quotes and fails with a space in
rem the path (e.g. E:\ALTRI GIOCHI): the resources are compiled from a copy in build\res.
set "RCDEF="
if not exist "%OUT%\res" mkdir "%OUT%\res"
copy /y "%ENGINE%\port\res\*" "%OUT%\res\" >nul
copy /y "%OUT%\game.h" "%OUT%\res\" >nul
if exist "%OUT%\res\game.ico" del "%OUT%\res\game.ico"
if exist "%DATA%\%EXE%" "%OUT%\lb7tool.exe" icon "%DATA%\%EXE%" "%OUT%\res\game.ico" && set "RCDEF=-DGAME_ICON"
pushd "%OUT%\res"
windres %RCDEF% -I. -o ..\game_res.o game.rc || (popd & goto :error)
windres %RCDEF% -I. -o ..\launcher_res.o launcher.rc || (popd & goto :error)
popd

echo [3/7] TinySoundFont and stb_truetype (third_party, without project warnings)
gcc -std=c11 -O2 -w -c -o "%OUT%\tsf_impl.o" "%SRC%\tsf_impl.c" || goto :error
gcc -std=c11 -O2 -w -c -o "%OUT%\text_impl.o" "%SRC%\text_impl.c" || goto :error

echo [4/7] compiling the game logic
if not exist "%OUT%\game" mkdir "%OUT%\game"
set "GAMEOBJ="
for %%F in ("%GAMESRC%\*.c") do (
	gcc %CFLAGS% -Wno-unused-parameter -I"%OUT%" -I"%SRC%" -c -o "%OUT%\game\%%~nF.o" "%%F" || goto :error
	set "GAMEOBJ=!GAMEOBJ! "%OUT%\game\%%~nF.o""
)

echo [5/7] %NAME% (SDL2 port)
gcc %CFLAGS% -I"%OUT%" -I"%SDL2_DIR%\include\SDL2" -o "%OUT%\%NAME%.exe" ^
	"%SRC%\main.c" "%SRC%\video.c" "%SRC%\audio.c" "%SRC%\midi.c" "%SRC%\lb7.c" "%SRC%\ne.c" ^
	"%SRC%\rt.c" "%SRC%\sprite.c" "%SRC%\cov.c" "%SRC%\text.c" "%SRC%\config.c" ^
	"%OUT%\tsf_impl.o" "%OUT%\text_impl.o" "%OUT%\game_res.o" ^
	!GAMEOBJ! -L"%SDL2_DIR%\lib" -lmingw32 -lSDL2main -lSDL2 -lm || goto :error

echo [6/7] %NAME%_launcher (Win32 launcher, no SDL)
gcc -std=c11 -O2 -Wall -Wextra -Wno-missing-field-initializers -mwindows -I"%OUT%" -o "%OUT%\%NAME%_launcher.exe" "%ENGINE%\port\launcher\launcher.c" "%OUT%\launcher_res.o" -lcomctl32 -lcomdlg32 -lshell32 -lole32 || goto :error

echo [7/7] SDL2.dll
copy /y "%SDL2_DIR%\bin\SDL2.dll" "%OUT%\" >nul || goto :error

echo.
echo OK: %OUT%\%NAME%.exe and %OUT%\%NAME%_launcher.exe
echo Run: build\%NAME%_launcher.exe (sets %NAME%.ini and starts the game) or build\%NAME%.exe (looks for the .BIN files next to the executable, otherwise --data FOLDER)
echo Music: SoundFont in tools\soundfont\GeneralUser-GS.sf2 (otherwise --sf2 FILE)
exit /b 0

:error
echo.
echo BUILD FAILED
exit /b 1
