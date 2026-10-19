@echo off
rem Opens a prompt with JDK, GCC and SDL2 in the PATH.
call "%~dp0env.bat"
cmd /k "echo Port environment ready: java, gcc, SDL2 & cd /d %~dp0.."
