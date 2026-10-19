@echo off
rem Portable environment of the project: JDK, Ghidra, GCC (w64devkit) and SDL2.
rem Nothing is written to the registry or to the user profile.
set "PORT_TOOLS=%~dp0"
set "PORT_TOOLS=%PORT_TOOLS:~0,-1%"
set "JAVA_HOME=%PORT_TOOLS%\jdk"
set "GHIDRA_HOME=%PORT_TOOLS%\ghidra"
set "SDL2_DIR=%PORT_TOOLS%\SDL2\x86_64-w64-mingw32"
set "PATH=%JAVA_HOME%\bin;%PORT_TOOLS%\w64devkit\bin;%SDL2_DIR%\bin;%PATH%"
set "GHIDRA_JAVA_OPTIONS=-Dapplication.settingsdir=%PORT_TOOLS%\ghidra_user\settings -Dapplication.cachedir=%PORT_TOOLS%\ghidra_user\cache -Dapplication.tempdir=%PORT_TOOLS%\ghidra_user\temp"
if exist "%PORT_TOOLS%\ghidra" if not exist "%PORT_TOOLS%\ghidra_user" mkdir "%PORT_TOOLS%\ghidra_user\settings" "%PORT_TOOLS%\ghidra_user\cache" "%PORT_TOOLS%\ghidra_user\temp"
if exist "%PORT_TOOLS%\ghidra" if not exist "%PORT_TOOLS%\ghidra_projects" mkdir "%PORT_TOOLS%\ghidra_projects"
