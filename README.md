# Upsy Daisy – unofficial SDL2 port of the 7th Level engine

A from-scratch reimplementation, in portable C11 on top of SDL2, of the **7th Level Runtime
Player 1.2**, the engine that runs the 1996 Windows 3.1 game *Hot Shots: Upsy Daisy* (and its
second game, *Inside Outwords*). It runs the original game data on modern 64-bit Windows, without
emulators or virtual machines.

> **You need your own copy of the original game.** This repository contains **no game data, no
> original code and no original assets**: only the engine written for this project and the
> tools that read the files of your CD. See [NOTICE.md](NOTICE.md).

## Status

- Upsy Daisy and Inside Outwords can be played from start to end: intro, menu, options, scores,
  both games, exit sequence and credits.
- Screens are compared automatically with the original game: menus, intro, games and exit
  sequence match within about 1–2 % of the pixels; scene timings of the intro match within 1 s.
- Known differences: text rendering (fonts, antialiasing), music (a SoundFont synthesizer instead
  of the Windows MIDI synth), random scenes (different random sequence).

## Requirements

- Windows 10 or 11, 64-bit.
- The files of the original CD: `TUPSYPC.BIN`, `TOPSY.BIN`, `TOPSMAIN.BIN`, `TOPSOPN1.BIN`,
  `TOPSOPN2.BIN`, `TOPSUPSY.BIN`, `TOPSWORD.BIN`, `TOPSFINA.BIN`, `TOPSCRED.BIN`, `TOPSSING.BIN`
  and, for the icon of the executable, `TOPSY.EXE`.
- About 120 MB of free space for the portable build tools (downloaded on request, see below).

Nothing is installed on the system: compiler, SDL2 and SoundFont are unpacked in `tools\`.

## Building

1. Copy the game files listed above into the root of this repository (they are ignored by git),
   or set the environment variable `UPSY_DATA` to the folder that contains them.
2. Run:

   ```bat
   build.bat
   ```

   The first time, `tools\setup_tools.bat build` shows what it is going to download
   (w64devkit GCC, SDL2 for MinGW, the GeneralUser GS SoundFont), asks for confirmation and
   checks the SHA-256 of every file.
3. The scripts of the game (bytecode inside the `.BIN` files) are translated to C by
   `port\tools\scr2c.c` into `build\gamesrc\` and compiled together with the engine. These
   generated files come from your copy of the game: do not publish them.

Output: `build\upsy.exe`, `build\upsy_launcher.exe`, `build\SDL2.dll`, `build\lb7tool.exe`.
`tools\make_dist.bat` collects what is needed to play in `dist\` (without the game files).

Other commands: `build.bat debug` (debug build), `build.bat clean`.

## Running

Start `build\upsy_launcher.exe`: it writes `upsy.ini` (resolution, window size, full screen,
filter, volumes, game folder) and starts the game. `upsy.exe` can also be started directly.

| Key | Effect |
|---|---|
| mouse, keyboard, joystick | as in the original game (keys for the players can be changed in Options) |
| Esc | game menu / skip intro (as in the original) |
| Alt+Enter | full screen on/off |
| Pause | pause |

### `upsy.ini`

Next to the executable (or in the current folder). Keys of the `[Upsy]` section:
`Resolution` (800 or 640), `Scale` (0 = largest that fits), `Fullscreen`, `Filter`
(`nearest`/`linear`), `IntegerScale` (1 = integer multiples only, ignored when the window is
smaller than the picture), `Vsync`, `Volume`, `MusicVolume`, `Trace`, `DataDir` (folder of the
`.BIN` files), `SoundFont`, `Library`. Command-line options take precedence. The game keeps its
own settings and high scores in `7THLEVEL.INI`, as the original did.

### Command-line options of `upsy.exe`

| Option | Effect |
|---|---|
| `--data FOLDER` | folder with the `.BIN` files |
| `--800` / `--640` | logical resolution 800×600 / 640×480 |
| `--fullscreen`, `--scale N` | full screen; window N times larger |
| `--sf2 FILE.sf2` | SoundFont for the music |
| `--lib FILE.BIN` | start from another library |
| `--viewer` | archive viewer (bitmaps, waves, MIDI) |
| `--trace FILE` | trace of the game logic (scripts, events, input), with timestamps |
| `--input FILE` | play timed input events (tests): `MS key VK`, `MS click X Y`, `MS move X Y`, `MS char C`, `MS text WORD`, `MS waitpix X Y RRGGBB [TOL] [MAX]`, `MS shot FILE.bmp`, `MS quit`, ... |
| `--seed N`, `--fixed-step [MS]`, `--fast` | repeatable runs: fixed random seed, simulated clock, no waiting |
| `--ini FILE` | `7THLEVEL.INI` to use |
| `--coverage FILE` | which scripts, sprites and animations were executed |

## How it works

- `port\src\` is the engine: archive reader (`lb7.c`, 7th Level "7LB" resource libraries),
  runtime of the game logic (`rt.c`), sprites and animations (`sprite.c`), 8-bit framebuffer
  and palette (`video.c`), mixer (`audio.c`), MIDI sequencer with TinySoundFont (`midi.c`), text
  (`text.c`, stb_truetype), settings (`config.c`), main loop (`main.c`).
- The game logic is not interpreted: `scr2c` translates every script of the `.BIN` files into a
  C function at build time, with structured `if`/`while` and direct calls; the engine provides
  the instructions (`rt_op`) and functions (`rt_builtin`) the scripts use.
- The engine follows the behaviour of the original, verified by comparing screenshots and
  timings with the original game; technical notes in [docs/ENGINE.md](docs/ENGINE.md).
- `src\tools\lb7tool.c` inspects and extracts the archives (`info`, `list`, `disasm`, `anim`,
  `extract`, `movie`, `icon`).

## Other 7th Level games

The same engine was used by other 7th Level titles of the same years (for example *Hot Shots:
Dual Bowling* and *Timon & Pumbaa's Jungle Pinball*). Adapting the port to them is ongoing work
and will be published in separate repositories.

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
by its author. The game, its names and its data belong to their respective owners
([NOTICE.md](NOTICE.md)).
