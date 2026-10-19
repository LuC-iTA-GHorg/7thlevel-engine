# Sprites, animations and events (7th Level Sprite Library 1.2)

Reconstructed from TOPSY.EXE (loaders 1008:6756…6EF2, `show` 1008:A990) and TOPSYS01.DLL
(animation interpreter `FUN_1000_4AC2`, commands in `FUN_1000_471A`, lengths
`FUN_1000_46B6`/`4612`). Status: formats and main mechanisms are **certain** where indicated; the
sprite engine is implemented in `port\src\sprite.c`.

## Resource types of the game world

| Type | Role | Loader | Format |
|---|---|---|---|
| 1 | bitmap (frame) | 1008:6756 | `FORMAT_7LB.md`; `+0xA`/`+0xC` = anchor point offset |
| 10 | movie (`FORMAT_7LB.md`) | 1008:6B16 → 1010:1166 | decoded (command 17) |
| 11 | list of instructions | 1008:6934 | `SCRIPT_OPCODES.md` |
| 12 | **cel**: bitmap + offset | 1008:67E6 | 6 bytes `{bitmap id, int16 dx, int16 dy}`; the engine copies the bitmap header (20 bytes) and replaces `+0xA/+0xC` with dx/dy |
| 13 | list of resources to load together | 1008:69CE | n words of ids (n = size/2), loaded in order |
| 15 | **animation** | 1008:6934 | list of commands (below) |
| 17 | **sprite** (actor) | 1008:6C08 | below |
| 18 | **text** (sprite member just like the frames) | 1008:6EF2 → 1010:1CA0 | 20 bytes, "Text" section |

### Sprite (type 17)

| Offset | Meaning | Certainty |
|---|---|---|
| +0 | **click** script (operand; written by opcode 04) | certain (0 in all files) |
| +2 | click argument | certain (0 in all files) |
| +4 | number of frames | certain |
| +6 | number of members | certain |
| +8 | **drag** start script (opcode 07) | certain |
| +0xA | **release** script (opcode 07) | certain |
| +0xE | drawing **level** (operand, signed comparison: −1 = below all) | certain |
| +0x10 | click disabled (opcode 04) | certain |
| +0x11 | click on the whole **rectangle**, ignoring transparent pixels (S_058 → field +0x69 of the DLL); 73 sprites | certain |
| +0x12 | flag: frames loaded on demand | probable |
| +0x13 | draggable (opcode 07) | certain |
| +0x14 | flag: "background" sprite (palette, `DAT_0E3E`) | probable |
| +0x15 | **click state** (S_058 → field +0x52): 0 only if visible, 1 also if hidden (menu buttons), 2 never (cursor sprite) | certain |
| +0x16 | list of **members** (encoded ids) | certain |

The data of the in-memory entry can be modified: opcodes 04/07 write +0..+0x13 and the runtime
copy (builtin 65) duplicates them. In the port they live in `RtObj` (`rt.h`), initialized from the file
each time the library is opened.

Members (`FUN_1008_6D44`): types **1, 12, 18** = frames (in order); **10** = text;
**9** = default sound (`DAT_0E40`); **4** = default music (`DAT_0E42`); other types
are only loaded (e.g. animations 15).

Examples from TOPSY.BIN: sprite 54 = one frame, bitmap 71;
sprite 63 = animation 64 + text 65.

### `show` instruction (0x05, 8 bytes, `1008:A990`)

`+2` sprite (type 17), `+4` animation (type 15) or 0, `+6` flags. Loads the sprite; with the flag
it hides it first (S_025); without an animation it shows the fixed frame (S_010), otherwise it
starts the animation (S_039: copies the commands and starts from the first one).

## Animations (type 15)

Sequence of commands `word code` + arguments. Numeric arguments use **the same encoding as
script operands** (globals/locals/constants, resolved with the pointer to the
globals in DS:044C of the DLL): animations can therefore depend on game variables.

Verification: `lb7tool anim FILE.BIN` → **2230 animations aligned out of 2230** across the 10 archives.

| Cmd | Length | Uses* | Meaning | Certainty |
|---|---|---|---|---|
| 01 | 10 | 10848 | `{from, to, count, duration_ms}`: shows `count` frames running from..to, each for `duration_ms` (−1 = duration unchanged) | certain |
| 02 | (n+4)·2 | 15059 | `{duration, state, n, n frames}`: **composite** frame made of several overlapping cels (−1 = skip), for `duration` | certain |
| 03, 04 | 2 | 0 | | |
| 05 | 30 | 0 | curved path: parabola through three points (FUN_1000_1882 → 1820 → 1758, like builtin 73) | probable |
| 06 | 8 | 568 | `{counter, repetitions, target}`: loop (jumps back to the offset) | certain |
| 07 | 10 | 9 | (1000:42DE) | ? |
| 08 | 2 | 291 | hides the sprite (S_025) | certain |
| 09 | 24 | 12 | **movement** `{steps, speed, relative, x, y, (runtime: start, goal, step)}`: from where it is to the goal (+ position if relative) in `steps` steps, one per interval of cmd 11, rounded interpolation (FUN_1000_3F84); with `speed` ≠ 0 the value is in pixels per step (FUN_1000_4646); `steps`, `x`, `y` encoded, the two flags raw | certain, ported |
| 0A | 8 | 0 | | |
| 0B | 6 | 836 | wait `{ms}` | certain |
| 0C | 8 | 227 | immediate **position** `{x, y, relative}` (FUN_1000_4446 → 383C) | certain, ported |
| 0D | 4 | 1012 | `{flag}`: cumulative timing (does not restart from "now") | certain |
| 0E | 4 | 0* | raw `{0/1}`: with 0 the **image stays frozen** (field +0x60 = 0: position and frames go into the copies +4/+6 and +0x40 without redrawing), with 1 the current state becomes visible again (FUN_1000_6AA4); cmds 01, 02, 05, 09, 0A also put it back in view. *Built by scripts with builtin 5B (FUN_1000_5EB4) | certain, ported |
| 0F | 4 | 0 | frame interval (S_036, field +0x24) | certain, ported |
| 10 | 8 | 0* | `{start, from, to}`: **frame loop** from..to (then from again), starting at `start` if it is within the range (FUN_1000_6A1A: fields +0x4C current, +0x4E/+0x50, direction +0x5D, active +0x5F); advances at every frame interval (cmd 11/0F) while the sprite moves (cmd 05/09/0A: 1000:1882/404A/4138 → 6B04). The animals flying in the menu flap their wings this way. *Built with builtin 5B (FUN_1000_5E2C, same order) | certain, ported |
| 11 | 6 | 12 | `{movement interval, frame interval}` (S_041 → +0x20, S_036 → +0x24 if ≠ 0); the next step starts from now | certain, ported |
| 12, 13 | 4 | 0 | | |
| 14 | 2 | 0 | | |
| 15 | variable | 1284 | **event**: `+2` length, from `+4` a **script instruction** | certain |
| 16 | 18 | 0* | **script call** `{script, n, up to 6 arguments}` (encoded operands, n raw): the DLL passes the command to the EXE (FUN_1000_44DC, length from table DS:0C0A), which performs the call immediately (FUN_1008_CC14 → CDAC). *Built by scripts with builtin 5B | certain, ported |
| 17 | 12 | 7 | text (type 10 resource) (1000:4F68) | probable |

\* sum over the 10 archives.

### Events (command 0x15) — link with the game logic

The DLL copies the bytes of the instruction contained in the command and calls the world callback
registered by the EXE, which executes it like a normal statement (`Script_ExecOp`). Example
(TOPSY.BIN, animation 62):

```
15 00 0c 00 | 05 00 0e 8b 0f 8b 00 00   ->  show sprite 63, animation 64
0d 00 01 00                             ->  cumulative timing
0b 00 87 96 00 00                       ->  wait 3000 ms
15 00 0c 00 | 1e 00 0a 8b 00 00 00 00   ->  (op 1E) deferred call of script 59
```

Instructions found in events: `01` (call, 1092 times), `05` (show, 110), `03`, `08`, `0F`,
`10`, `16`, `1C`, `1E`, `3F`.

**In the port** events are **translated to C** by `scr2c` (functions `<lib>_animNNNN(rt, off)`
in `port\game\*.c`, `anim_events` table of `RtLib`, 1284 events in total). The sprite engine
plays the animation commands as data (frames, timings, movements) and for each
0x15 command calls `rt_anim_event(rt, animation_id, offset)`: the logic stays C code.

## Startup flow of TOPSY.BIN (read from the generated C)

The CD is started with **TUPSYPC.EXE**, which opens TUPSYPC.BIN: script 0 sets `G(5068) = 3`
(product variant: with 0, 1 or 6 the game menu shows all the signs, with 3 only
**Upsy Daisy**; TOPSMAIN menu script) and `G(5069) = 0`, then does the `link` to TOPSY.BIN. The
port in `--game` mode therefore starts from TUPSYPC.BIN (it used to start from TOPSY.BIN and showed 5
games, which live on other CDs).

1. Script 0 → 58, 68, 55: INI, cursor, loads sprites 61, 63 and text 65.
2. Script 56 (`show sprite 61, anim 62`) starts the sequence: sprite 63 shows text
   65, plays waves 59/60, waits, then runs script 57.
3. Scripts 21–31 call script 32 with the area index; 32 does a fade and a
   `runindex` on list 33 = `link` to `topsupsy/topsword/topsmain/…`.
4. Script 0 sets with opcode `0x15` the **deferred script** (`DAT_1502` = script 1), which the
   engine runs as soon as it regains control. Script 1 binds **keys** to scripts with `0C`
   (`+2` key, `+4` script, flag `+6` Ctrl, `+7` Shift, `+10` release) and calls script 56 (show 61/62).
5. Sprites 61 and 63 have no bitmap: startup shows **text** (type 10) and plays waves.

## Drawing, position and background (verified against the original)

- Rectangle of a frame (TOPSYS01 `FUN_1000_0B2C`): `sx = x + cel.dx - (w-1)/2 + hx`,
  `sy = y + cel.dy + hy - (h-1)/2`; `hx, hy` = bitmap header `+0xA/+0xC`; with the sprite
  mirrored (flag `+0x3E/+0x3F`) the signs of `hx`/`hy` are inverted. Origin: top left.
- Animation command `01 {from, to, counter, duration}`: the counter `+6` is written by the engine
  when the command starts (`FUN_1000_6A1A`) = number of frames of the range.
- Script opcode `3F {sprite, x, y}`: absolute position (`S_038` -> `FUN_1000_383C`, relative
  flag = 0).
- Script opcode `06 {mode, 800x600 bitmap, 640x480 bitmap, color}`: background; mode 1 = solid
  fill of the given color.

## Corrections verified by comparison with the original game running under OTVDM

On-screen comparison between the original game running under OTVDM and the port.

- **World origin = center of the screen**: `sx = W/2 + x - (w-1)/2 + ax`,
  `sy = H/2 + y - (h-1)/2 + ay`, with `(ax, ay)` = cel offset `(cel.dx, cel.dy)` for the
  type-12 frames, otherwise the bitmap anchor `(hx, hy)` (+0xA/+0xC). The cel
  **replaces** the anchor, it is not added: verified on the faces of the final scene
  (mouth 4307 on body 4142 with anchor 0/11) and on the comparisons of the menu, Inside Outwords and
  the opening. With the sprite mirrored (+0x3E/+0x3F) `(ax, ay)` changes sign. Same coordinates for
  640x480 and 800x600.
- **Sprite level = word +0x0E of the type-17 resource**, encoded operand,
  **signed** comparison (`cmp`/`jle` in FUN_1000_10D4 and S_065, verified on the disassembly:
  -1 = below all; the port used to compare unsigned and drew on top of the
  sprites that go below, for example the piglet outside the menu arch). The byte +0x11
  used before was wrong.
  Opcode 1C changes it; at equal level the last inserted is on top.
- **Color 0 is transparent in all frames** of sprites, even non-RLE ones; the background is opaque.
- **Commands 01 and 02 make the sprite visible** (FUN_1000_360E: +0x56 = 1): entrance animations
  do `08` (hide), `0B` wait, then the frames.
- **Command 01**: the counter +6 is computed at start (frames of the range) and the last frame
  also stays for its duration before the next command.
- **Command 04**: stamps the sprite onto the background and removes it (FUN_1000_0426).
- **Command 07 {mirror x, mirror y, dx, dy}**: mirror and relative offset
  (FUN_1000_42DE -> 383C(0, 1, ...)).
- **Command 06**: if field +0x65 is set (opcode 19) the loop does not jump back.
- **Command 12 {frame}**: shows the sprite (S_043), -1 = current frame.
- **Command 17 {movie}**: plays a type-10 resource (see FORMAT_7LB.md, "Movies");
  the animation stays still until the end, then the last frame remains.
- **Background** (opcode 06): the original copies it into the buffer with origin `(W-1)/2` and shows it with
  the screen origin `W/2` (FUN_1000_375A, S_026): it is centered like a sprite at 0,0, i.e. at
  `W/2 - (width-1)/2` = **1,1** at 800x600 (row and column 0 black). The port used to draw it at 0,0
  (fixed with the automatic comparison; sprites were already right).
- **Visibility** (+0x56): set only by commands 01/02 (FUN_1000_360E) and 12 (S_043),
  cleared by 08 (S_025). Not even `show` with an animation (FUN_1008_A990 → S_039 1000:4E6C →
  S_010 + 471A) sets it: the sprite appears at the first 01/02/12 (fixed: the
  end credits, sprite 3078 with anim 3079 = `0D`, `0B` of 11.25 s, event, `01`, showed the
  first page immediately instead of after 11 s). A 5B that queues a wait (0B) or a call (16) on a hidden sprite does not
  show it (the port did: the gargoyles hanging from the menu window, script 4006 of
  TOPSMAIN, appeared immediately instead of after 6-12 s).
- **Builtin 5A/5B**: animation command built by the script, args `{sprite, command,
  values...}`: 5B queues it; 5A (FUN_1000_59F6 with first argument 1) for commands **01 02 05
  09 0A 16** empties the command buffer (FUN_1000_54F6) and runs it immediately, for all
  the others (04 07 08 0B 0C 0D 0E 0F 10 11 12) it acts directly on the sprite and **leaves the
  queue intact** (e.g. 0C → FUN_1000_383C, 08 → S_025, 12 → S_043, 0B nothing). Fixed bug:
  the port used to empty the queue for these too, and the position `5A 0C` after the
  `show` of the Upsy characters cancelled cmd 02, which composes body, head (G(1250)) and
  balloons (G(1254)). Arguments: cmd 1 <- (from, to, duration), cmd 2 <- (frames..., duration),
  cmd 12 <- (frame).

## Engine pace and events (verified against the original)

- **How many ticks**: the original has no timer for the engine. The message loop
  (PumpMessages, TOPSY 1008:0000) is a `PeekMessage` without waits that at every
  iteration calls S_081 (TOPSYS01 1000:0024; the decompiled code calls it S_081 but it is S_001) and then
  the runtime timers: thousands of ticks per second, all using the time from `timeGetTime`
  (1 ms resolution). No `SetTimer`, `timeBeginPeriod` or `GetTickCount`; `timeSetEvent` is
  only in the MIDI code (TOPSYA01). In the port (`main.c`, `sprite_ticks`/`game_window` loop): one engine
  tick (`rt_tick` + `spr_update`) per elapsed ms, separate from the video frame;
  after a long stall at most 50 ticks in a row (the original does not catch up lost ticks). The
  port used to do one tick per frame: in real time (vsync, 16.7 ms) the climb of the Upsy
  characters (350 × `0C` + `0B 8`) lasted ~6 s instead of ~3.5.
- **One command per tick** (S_081 → FUN_1000_4AC2): at every tick the engine executes the
  current command of each sprite. If 4AC2 returns 1 (command finished) S_081 calls 471A, which prepares
  the next one (resolving the 06 loops with 427A) or stops the animation if there are none (S_010):
  the next one starts **on the following tick**. They return 0 (tick closed, command still running)
  the first tick of a 0B wait (697C fixes the deadline: a wait always lasts at least 2 ticks) and
  the ticks of 01/02/09 before the deadline. Only **0D 0F 10 11** continue within the same tick
  (jump to LAB_4C44) and **07/0C** when the prepared command is 01 or 02 (471A clears +0x57,
  "position to redraw"); otherwise 07/0C draw immediately (FUN_1000_36E4) and close the
  tick. 04 08 0E 12 finish within the tick itself (they return 1). Example, the climb of Upsy: tick
  A end of the wait, tick B `0C`, tick C `0B` fixes the deadline at C+8, tick C+8 end: ~10 ms per
  step, as measured under OTVDM (~10.2). In the port: `run_anim`, field `after_pos`, `cmd_done`.
- **Events** (15, 16; 14 sends 0x4C8 = deferred script `DAT_1502`, 13 FUN_1008_E67E): the
  DLL copies the command and calls the EXE (callback 1008:CC5x), which puts the sprite in **`G(5017)`**
  (DS:3C7C) and then executes the instruction (15) or the call (16). The next command starts on
  the following tick. In the port: `run_anim` stops after an event.
- **Builtin 5B** on a stopped sprite only **prepares** the command (471A): it starts on the next
  tick. Only **5A** executes immediately (FUN_1000_6044). This way a 16 command that requeues
  itself (TOPSMAIN 4032: wait 15-25 s then 16 → 4032) does not become a recursion.
- "Cumulative" timings (cmd 0D): the wait (0B, FUN_1000_697C) starts from the previous deadline; a
  sprite that has been idle for a long time catches up one step per tick, like the original.

## TOPSMAIN menu (verified flow)

At startup script 3068 → 8660 → 8751(8757) opens the games panel: the banner (copy
of sprite 8747, level 29000) comes down with `11 {41}`, `0D {0}`, `09 {0, y, 32, 1, 0}` and at the
end `16 {8736/8734, level}` calls the script that shows the signs of the four games
(sprites 8763..8777, click → 8716, mouse filter 8717 acting on release). GAMES (8659) closes and reopens
the banner (8751 with the same panel = close). The "Upsy Daisy" sign (8771) opens the next level
(8757 → 8733), which uses opcodes **18** (sprite replacement) and **10** (arithmetic): the
banner shows the game's presentation animation and the gargoyles
change pose. Text and buttons of this level are still to be verified.

## Text (type 18 resource, verified)

20 bytes `{+0 color (palette index), +2 font (string id: three different font families, among
them Times New Roman and Arial), +4 initial text (id), +6 height (operand: 20 or 24, positive lfHeight =
cell height), +8/+0xA point (x, y), +0xC/+0xE box (0 = none; with a box
`DT_WORDBREAK`), +0x10 DrawText flags (1 center, 2 right, 4 vertical center, 8 bottom)}`.
The EXE creates the font (`CreateFontIndirect`, cache of 10), draws with `DrawText` into a bitmap
(background 0 transparent, text in the color, no antialiasing) as wide as the text (or the
box) + 1, and uses it as a frame; anchor (1010:189E): for left alignment `x + (w-1)/2`, for
center `x`, for right `x - w/2` (the same vertically). Empty text = `" "` (DS:01F2).
Opcode 29 rewrites the text (`wsprintf`) and redraws. 43 resources in the game (scores,
stopwatch, player labels). In the port: `text.c` (stb_truetype; Windows fonts or Liberation in
`tools\fonts`), frames with `text_id` in `sprite.c`.

## Opcode 18 and the animation of the old sprite

FUN_1008_E06C calls `S_010` on the old sprite before replacing it: its animation and the
queued commands (5B) stop. Without this, in the menu a gargoyle "at rest" with a 16 command
in the queue came back on stage and interrupted the short scene that precedes every game.

## Starting a game from the menu (verified)

Click on a sign (e.g. "Upsy Daisy", 8771) → 8755 → `G(2054)`(1, 8789) = 8757: menu level 8
(presentation) with sketch 8761/8762; after 1 s script 3074 starts the gargoyle scene
(list 4040, entry = chosen game, `G(5038)`), which lasts about 45 s and at the end
(animation 5573 → 4023) does the `link` to the game (`topsupsy.bin`). In TOPSUPSY: Players/Difficulty/OK
screen, character selection, then the match (stopwatch and scores as text).

## Hit test (S_029, verified)

Sprites from the topmost (draw list scanned backwards): those with state 2 and those
hidden with state other than 1 are excluded; the point (relative to the screen center) must be in the
rectangle of the current frame (or of the cels of the composite frame) and, without flag +0x11,
on a pixel other than 0 (FUN_1000_3096). A freshly loaded sprite has the first frame as its
current frame, even if hidden: this is how the invisible menu buttons (state 1, flag +0x11)
receive clicks. In the port: `spr_hit` in `sprite.c`.

### Names of the TOPSYS01 exports in the decompiled code

Compared with the NE name table: in the decompiled TOPSYS01 the functions at
1000:0024 / 136A / 1428 / 6B82 are called `S_081..S_084` but they are **S_001..S_004**
(the imports of TOPSY.EXE use the real names). Before trusting an S_0xx name, check
the address against the table.
