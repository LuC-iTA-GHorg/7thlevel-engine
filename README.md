# 7thlevel-engine – unofficial SDL2 port of the 7th Level Runtime Player

A from-scratch reimplementation, in portable C11 on top of SDL2, of the **7th Level Runtime
Player 1.2**, the engine of several 1996 Windows 3.1 games by 7th Level. One engine for all of
them: it runs the original game data on modern 64-bit Windows, without emulators or virtual
machines.

> **You need your own copy of the original games.** This repository contains **no game data, no
> original code and no original assets**: only the engine written for this project and the
> tools that read the files of your CD. See [NOTICE.md](NOTICE.md).

## Supported games

Each game has a profile in `games\` and its own repository that uses this engine as a git
submodule (`engine\`).

| Profile | Game | Family | First library |
|---|---|---|---|
| `upsy` | Hot Shots: Upsy Daisy (and Inside Outwords) | Topsy Turvy | `TUPSYPC.BIN` |
| `bowl` | Djali Bowling | Topsy Turvy | `TBOWLPC.BIN` |
| `chiseler` | Chiseler | Topsy Turvy | `TCHISPC.BIN` |
| `foodfeud` | Le Food Feud | Topsy Turvy | `TFOODPC.BIN` |
| `bugdrop` | Bug Drop | Pumbaa and Timon's Jungle Arcade | `T&PBUGPC.BIN` |
| `burper` | Burper | Jungle Arcade | `T&PBURPC.BIN` |
| `hippohop` | Hippo Hop | Jungle Arcade | `T&PHIPPC.BIN` |
| `pinball` | Jungle Pinball | Jungle Arcade | `T&PPINPC.BIN` |
| `slingshooter` | Slingshooter | Jungle Arcade | `T&PSHTPC.BIN` |

Status: Upsy Daisy can be played from start to end and matches the original screen by screen.
The other games start, reach their menus and can be played (Djali Bowling, Jungle Pinball,
Hippo Hop...); they are less tested.

All the games share the same engine and the same script instructions; the differences between
the two families (7THLEVEL.INI section, escapes in INI strings, fonts) are in the profiles.

## Requirements

- Windows 10 or 11, 64-bit.
- The files of the original CD of the game (`*.BIN`, the `.EXE`, and `*.TTF` when present).
- About 120 MB of free space for the portable build tools (downloaded on request, see below).

Nothing is installed on the system: compiler, SDL2 and SoundFont are unpacked in `tools\`.

## Building

1. Copy the files of your game CD into the root of this repository (they are ignored by git), or
   set the environment variable `GAME_DATA` to the folder that contains them.
2. Run:

   ```bat
   build.bat            rem the game is recognised from the files present
   build.bat pinball    rem or name the profile
   ```

   The first time, `tools\setup_tools.bat build` shows what it is going to download
   (w64devkit GCC, SDL2 for MinGW, the GeneralUser GS SoundFont), asks for confirmation and
   checks the SHA-256 of every file.
3. The scripts of the game (bytecode inside the `.BIN` files) are translated to C by
   `port\tools\scr2c.c` into `build\gamesrc\` and compiled together with the engine. These
   generated files come from your copy of the game: do not publish them.

Output: `build\<game>.exe`, `build\<game>_launcher.exe`, `build\SDL2.dll`, `build\lb7tool.exe`.
`tools\make_dist.bat` collects what is needed to play in `dist\` (without the game files).
Other commands: `build.bat GAME debug` (debug build), `build.bat clean`.

### Using the engine from a game repository

A game repository contains the engine as a submodule in `engine\` and a small `build.bat` that
sets `GAME_ROOT` to its own folder and calls `engine\build.bat <profile>`. Output, game data and
(in a development tree) the translated logic in `port\game\` then live in the game repository.

## Running

Start `build\<game>_launcher.exe`: it writes `<game>.ini` (resolution, window size, full screen,
filter, volumes, game folder) and starts the game. `<game>.exe` can also be started directly.

| Key | Effect |
|---|---|
| mouse, keyboard, joystick | as in the original game |
| Esc | game menu / skip intro (as in the original) |
| Alt+Enter | full screen on/off |
| Pause | pause |

### `<game>.ini`

Next to the executable (or in the current folder): `Resolution` (800 or 640), `Scale` (0 =
largest that fits), `Fullscreen`, `Filter` (`nearest`/`linear`), `IntegerScale`, `Vsync`,
`Volume`, `MusicVolume`, `Trace`, `DataDir` (folder of the `.BIN` files), `SoundFont`, `Library`.
Command-line options take precedence. The game keeps its own settings and high scores in
`7THLEVEL.INI`, as the original did.

### Command-line options

| Option | Effect |
|---|---|
| `--data FOLDER` | folder with the `.BIN` files |
| `--800` / `--640` | logical resolution 800×600 / 640×480 |
| `--fullscreen`, `--scale N` | full screen; window N times larger |
| `--sf2 FILE.sf2` | SoundFont for the music |
| `--lib FILE.BIN` | start from another library |
| `--viewer` | archive viewer (bitmaps, waves, MIDI) |
| `--trace FILE` | trace of the game logic (scripts, events, input), with timestamps |
| `--input FILE` | play timed input events (tests): `MS key VK`, `MS keydown VK`, `MS keyup VK`, `MS click X Y`, `MS move X Y`, `MS char C`, `MS text WORD`, `MS waitpix X Y RRGGBB [TOL] [MAX]`, `MS shot FILE.bmp`, `MS quit`, ... |
| `--seed N`, `--fixed-step [MS]`, `--fast` | repeatable runs: fixed random seed, simulated clock, no waiting |
| `--ini FILE` | `7THLEVEL.INI` to use |
| `--coverage FILE` | which scripts, sprites and animations were executed |

## How it works

- `port\src\` is the engine: archive reader (`lb7.c`, 7th Level "7LB" resource libraries),
  runtime of the game logic (`rt.c`), sprites and animations (`sprite.c`), 8-bit framebuffer
  and palette (`video.c`), mixer (`audio.c`), MIDI sequencer with TinySoundFont (`midi.c`), text
  (`text.c`, stb_truetype, also with the fonts shipped with a game), settings (`config.c`),
  string resources of the original EXE (`ne.c`), main loop (`main.c`).
- The game logic is not interpreted: `scr2c` translates every script of the `.BIN` files into a
  C function at build time, with structured `if`/`while` and direct calls; the engine provides
  the instructions (`rt_op`) and functions (`rt_builtin`) the scripts use, including the math
  library of the Jungle games (angles, collisions, intersections) with the original rounding.
- The engine follows the behaviour of the original, verified by comparing screenshots and
  timings with the original games; technical notes in [docs/ENGINE.md](docs/ENGINE.md), script
  instructions in [docs/SCRIPT_OPCODES.md](docs/SCRIPT_OPCODES.md).
- `src\tools\lb7tool.c` inspects and extracts the archives (`info`, `list`, `disasm`, `anim`,
  `extract`, `movie`, `icon`).
- `tools\scripts\` contains the test tools: repeatable scenes (`run_scenes.ps1`), screenshots
  and comparisons with the original game.

## Third-party components

| Component | License | Where |
|---|---|---|
| SDL2 2.32.10 | zlib | downloaded into `tools\SDL2` by `setup_tools` |
| w64devkit 2.10.1 (GCC) | Unlicense (tools); GCC runtime exceptions | downloaded into `tools\w64devkit` |
| TinySoundFont | MIT | `port\third_party\tsf.h` |
| stb_truetype | MIT / public domain | `port\third_party\stb_truetype.h` |
| GeneralUser GS 2.0.3 SoundFont | GeneralUser GS license | downloaded into `tools\soundfont` |
| Liberation fonts 2.1.5 | SIL Open Font License 1.1 | `tools\fonts` (fallback when Windows fonts are missing) |

## License

No license has been chosen yet for the code of this project: until then, all rights are reserved
by its author. The games, their names and their data belong to their respective owners
([NOTICE.md](NOTICE.md)).
