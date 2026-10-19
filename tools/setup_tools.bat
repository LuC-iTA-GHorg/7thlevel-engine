@echo off
rem Downloads (after confirmation) and unpacks into tools\ the portable tools, checking SHA-256.
rem   setup_tools.bat            everything (build + decompilation, ~860 MB)
rem   setup_tools.bat build      only w64devkit, SDL2 and the SoundFont (~115 MB)
rem   setup_tools.bat decompile  only JDK and Ghidra
rem Details in tools\setup_tools.ps1 and README.md.
setlocal
set "ONLY=%~1"
if "%ONLY%"=="" set "ONLY=all"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup_tools.ps1" -Only %ONLY% %2
exit /b %ERRORLEVEL%
