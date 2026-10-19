# Notice

This is an **unofficial**, non-commercial fan project for game preservation and
interoperability. It is **not affiliated with, endorsed by or sponsored by** The Walt Disney
Company, Disney Interactive or 7th Level, Inc.

*Disney*, *Hot Shots*, *Upsy Daisy*, *Inside Outwords* and the related names and characters are
trademarks of their respective owners. They are mentioned only to identify the game this engine
is compatible with.

## What this repository contains

- An engine written from scratch for this project (`port\src\`), a launcher, build scripts and
  tools that read the files of the original game.
- Third-party components with their own licenses (see `README.md`).

## What this repository does NOT contain

- No game data: no `.BIN` archives, executables, DLLs, images, sounds, music, texts or fonts of
  the original game.
- No original program code and no decompiled code of the original executables.
- No game logic: the scripts of the game are translated to C on the user's computer, at build
  time, from the user's own copy of the game (`build\gamesrc\`, ignored by git). Do not commit or
  distribute those generated files, `build\` or `dist\` together with game files.
- The icon of the executables is extracted at build time from the user's own `TOPSY.EXE` and is
  never stored in the repository.

To use this software you must own the original game.
