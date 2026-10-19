# Engine overview

The 7th Level Runtime Player 1.2 is a Windows 3.1 (16-bit) program made of an executable
(`TOPSY.EXE`) and four DLLs: resources (`TOPSYR01`), sprites (`TOPSYS01`), audio (`TOPSYA01`)
and math utilities (`TOPSYU01`). A game is a set of resource libraries (`.BIN`, "7LB" format)
containing bitmaps, sounds, MIDI, animations, sprites, texts and the game logic as bytecode
scripts. This port reimplements the runtime in C11 + SDL2; its behaviour was reconstructed by
analysing the original program and verified by comparing screenshots and timings with the
original game.

Detailed documents:
- [FORMAT_7LB.md](FORMAT_7LB.md): library format, resource types, bitmap/wave/MIDI/movie codecs;
- [SCRIPT_OPCODES.md](SCRIPT_OPCODES.md): script bytecode, instructions, engine functions
  (builtins), system variables, input;
- [SPRITES_ANIMATIONS.md](SPRITES_ANIMATIONS.md): sprites, animation commands, drawing, timing.

## Game logic

- Script operands: global variable if the value is < 0x13FE, local if < 0x159F, otherwise a
  constant equal to value − 0x8ACF.
- `port\tools\scr2c.c` translates every script (type 16) and list (type 11) into a C function
  with structured control flow; the generated code calls the runtime (`port\src\rt.c`):
  `rt_call` (script as a function), `rt_builtin` (engine functions), `rt_op` (instructions whose
  handler lives in the runtime), `G(n)` and `l[n]` for global and local variables.
- At the end of a run `upsy.exe` lists the instructions, builtins and animation commands that
  were met but are not implemented: new games must be checked for these.

## Main loop and timing

- The original engine has no timer for animations: its message loop calls `PeekMessage`
  continuously and, at every iteration ("tick"), runs one step of every sprite and the script
  timers, reading the time with `timeGetTime` (1 ms resolution).
- Every tick executes at most one animation command per sprite; only a few commands (0D, 0F,
  10, 11, the 06 loops, and 07/0C when followed by 01/02) continue within the same tick. A wait
  (0B) always lasts at least two ticks.
- The port runs one engine tick per elapsed millisecond (runtime + sprites), separated from the
  video frame; after a long stall it runs at most 50 ticks in a row, like the original that does
  not recover lost ticks.
- Synchronous palette fades are frame-based; script timers use the same clock.

## Sprites and drawing

- 8-bit framebuffer with the library palette (236 colours mapped to indexes 10..245); RGB
  conversion only when presenting the frame.
- Sprite position: left = W/2 + x − ((w−1)/2 − hx); the background (instruction 06) is placed
  like a sprite at 0,0, i.e. at (1,1) on an 800×600 screen; mouse coordinates have their origin
  at W/2.
- Visibility is set only by the animation commands 01, 02 and 12 and cleared by 08: neither
  showing a sprite with an animation nor queueing commands makes it visible.
- Builtin 5A executes a command immediately (clearing the queue only for 01 02 05 09 0A 16);
  5B appends it to the queue.

## Randomness

The original seeds `rand()` with `timeGetTime()`: random scenes differ from run to run. The port
can use a fixed seed (`--seed N`) for repeatable runs.

## Window

The picture is drawn at the original resolution (800×600 or 640×480) and scaled to the window
with SDL's logical size: any window size and full screen (Alt+Enter) keep the aspect ratio with
black bars.
