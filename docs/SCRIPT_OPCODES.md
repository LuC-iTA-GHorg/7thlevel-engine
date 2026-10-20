# Bytecode of the 7th Level scripts (Runtime Player 1.2)

Reference for the interpreter. Source: TOPSY.EXE (`Script_ExecOp` 1008:CE92, `Script_OpLength`
1008:C9D4, `Script_EvalExpr` 1008:2464, `Engine_Command` 1008:1002). Verification: `lb7tool disasm`
walks all 2469 scripts of the 10 archives of the Upsy Daisy CD without errors.

Legend of the "Certainty" column: **certain** = read from the decompiled code; **probable** =
inferred from the APIs called.

## Execution model

Reconstructed from TOPSY.EXE (`Script_Run` 1008:D754, loop 1008:CB84, startup 1008:C474):

- **Synchronous execution**: a script runs entirely until `end`/`return` (or until an
  instruction returns 0). There are no coroutines: asynchrony lives in the engine (timer of
  builtin 0x93, objects and events), which calls the scripts back.
- **One library at a time**: when a `.BIN` is opened, **entry 0** (the startup script) is run. The 5001 globals are
  **saved per library** (by file name) and initialized only
  once from **key 3** (`{index, value}`); string variables (128 bytes, number in
  header +0xCA) are also per library. **Key 4** is the pool of constant strings
  (id with bit 15 = offset in the pool).
- **Instruction lists (type 11)**: one instruction per entry, executed individually by
  `runrandom`/`runnext` (with flag +0x10) and `runindex`. Example: TOPSY.BIN entry 33 = 11 `link`
  to the game areas.
- **Nesting** at most 30 (error 0x6A).

In the port these scripts are **translated to C** by `port\tools\scr2c.c` at build time
(`build\gamesrc\*.c`, or `port\game\*.c` in a development tree; see `README.md`): the model
above is implemented by `port\src\rt.c`. The instructions contained in the **animation events**
(command 0x15 of type 15) are translated too: see `SPRITES_ANIMATIONS.md`.

- A script is a type-16 resource: a sequence of instructions `word opcode` + 16-bit operands.
- `Script_ExecOp(ip, &advance)` executes one instruction and writes how many bytes to advance
  (for jumps and blocks it is a relative offset). Returns 1 = continue in the same frame,
  0 = suspend the script until the next tick (waits, animations).
- **Variables**: signed 16 bit. Globals in an array (DS:154A); locals in a frame stack
  (`DAT_10C2`), created by `call` (op 0x01) and `callscript` (expression 0x0E).
- **Statement operands** (encoded word):

| Value | Meaning | lb7tool |
|---|---|---|
| `< 0x13FE` | global variable n | `gN` |
| `0x13FE..0x159E` | local variable n (= value − 0x13FE) | `lN` |
| `>= 0x159F` | constant = value − 0x8ACF (`0x8ACF` = 0, `0x8AD0` = 1) | number |

  Resource and script ids in instructions use the same encoding: script
  `0x8ACF + n` is entry n of the index.
- **Return value**: `return` (0x59) saves it in `DAT_40DA` and ends the script.

## Expressions (RPN, 1-byte opcodes, terminated by 0x00)

Used by `expr` (0x4C, from +4), `return` (0x59, from +4), `if`/`while` (0x4D/0x4F, from +6),
`switch` (0x4E, from +10). The value stack is at DS:0724 (index `DAT_40D8`).

| Byte | Operands | Effect |
|---|---|---|
| 00 | – | end: returns the top of the stack |
| 01 | word | push constant (raw, not encoded) |
| 02 | word var | push the value of the variable |
| 03 | – | indexing: `a[i]` (pop i, top = word at address top + 2·i) |
| 04 | word var | push the **address** of the variable |
| 05 | – | dereference the top |
| 06 | – | element address: top = top + 2·i |
| 08 | – | assignment `*addr = val`, leaves `val` |
| 0D | word, word argc | builtin call: id pushed before the arguments; id < 0xB3 → `Engine_Command`, otherwise 1008:2386 |
| 0E | word frame, word argc | calls a script as a function (id from the stack), result = its `return` |
| 0F 10 11 | – | `-x`, `!x`, `~x` |
| 12..16 | – | `+ - * / %` (division by zero → runtime error 0x6F) |
| 17..1C | – | `== <= < >= > !=` |
| 1D 1E | – | logical `and`, `or` (no short-circuit) |
| 1F 20 21 | – | `& | ^` |
| 22 23 | – | `++x`, `x++` (top = address) |
| 24 25 | – | `--x`, `x--` |
| 26 27 | word offset | short-circuit: `||` jumps if top ≠ 0, `&&` jumps if top = 0 |
| 28 | – | duplicates `*addr` (for compound assignments) |

## Statements

Lengths in bytes (L). "var" = length held in a word of the instruction.

| Op | L | Mnemonic | Handler | Certainty | Meaning / APIs reached |
|---|---|---|---|---|---|
| 01 | 6+2n | `call` | 1008:CDAC | certain | +2 script, +4 frame size, +5 number of arguments, +6 arguments → synchronous execution |
| 02 | 4 | `load` | 1008:740E | certain | loads resource +2 according to its type (1 bitmap, 4 MIDI, 9 wave, 10, 11/14/15/16 data, 12, 13, 17, 18); types without a handler → 0 = the script ends |
| 03 | 4 | `unload` | 1008:722A | certain | unloads/stops resource +2 according to type (A_012/A_013 sounds, S_013 sprite); a loaded sprite also unloads its members (FUN_1008_6502): its wave and MIDI members are destroyed, so their sound stops |
| 04 | 20 | `click` | 1008:3924 | certain | **click**: `+2` sprite (0 = rectangular area), `+4` script, `+6..+0xC` x1 y1 x2 y2 of the area, `+0xE` argument, flag `+0x10` all, `+0x11` re-enable only, `+0x12` disable; ported (section "Input") |
| 05 | 8 | `show` | 1008:A990 | certain | +2 sprite (type 17), +4 animation (type 15) or 0 = fixed frame (the **raw** word 0, not decoded: `scr2c` fixed, it used to produce `G(0)`), +6 hide-first flag (`SPRITES_ANIMATIONS.md`) |
| 06 | 16 | | 1008:0654 | certain (comparison with the original game running under OTVDM) | **background** `{mode, 800x600 bitmap, 640x480 bitmap, color, transition, 2 parameters}` (FUN_1008_03CE): mode 0 = bitmap, 1 = solid fill, 2 = `S_031(0xFFFF, -1)`: the composition buffer is no longer cleared (what was drawn stays), any other mode = black (FUN_1008_0104 = `S_031(0, 0)`); ported (`spr_background`). Transition `+10` (types 1..10, FUN_1008_0F10, params `+12`/`+14`, applied only in mode 0 when a bitmap background exists; always 0 in the 9 games): ported (`spr_background_transition`, synchronous, one `rt->frame` per step) |
| 07 | 10 | `drag` | 1008:3A6E | certain | **drag** `{sprite, start script, release script, fixed}`: fixed = 0 makes the sprite draggable; with fixed ≠ 0 on the one being dragged it ends the drag (that is why it reads the cursor); ported. Not used by Upsy |
| 08 | 28 | `wave` | 1008:8EAC | certain | **plays a wave** `{wave, block A_029}`: `+4` repetitions (0 once, n more passes, −1 infinite), `+6` sets volume, `+7` **notify at end of sound**, `+8` no queue flush, `+9` volume 0..16 (kept by the wave for later plays), `+0xA` fade-in ms, `+0xE` fade-out ms (at the end of the last pass), `+0x12`/`+0x16` random start delay min/max (whole seconds; the wave then restarts after every end), `+0x1A` **end-of-sound script**, resolved when the wave starts (can be a local variable); start gate on the **priority** (header +8) and **exclusivity** of non-overlay waves (header +5); without a wave event (100 in use) the script is queued at once if notify is on; resource not of type 9 = the script stops; then the builtin 97 script is queued; ported ([AUDIO.md](AUDIO.md)) |
| 09 | 4 | `cdplay` | 1008:8E92 | certain | A_027: plays CD audio track = low byte of the raw word `+2` (MCI `cdaudio`), result in `DAT_5B6E`. Without a CD audio device (the port) the MCI open fails after flushing the wave queue (current priority back to 0) and `DAT_5B6E` = 0; ported that way ([AUDIO.md](AUDIO.md)) |
| 0A, 0B | 4 | `fadein`, `fadeout` | 1008:4880 | certain | palette fade, +2 speed 1..100 (0 or >100 = 100) = number of steps, one per redrawn frame, **synchronous** (FUN_1008_466C/478C → 4622 → S_021, colors 10..245); fade in acts only with the screen off (`DAT_5B6D`, off at startup), fade out only when on; a `fadeout` during the startup script does not fade but cancels the automatic fade in (DAT_5B75); on library change fade out 100, after the startup script fade in 100. Ported (`rt_fade`, `rt->frame`) |
| 0C | 12 | `key` | 1008:345C | certain | **key → script** `{key, script, Ctrl, Shift, enable, disable, release}`: `+2` Windows virtual key code (0 = all), `+4` script, `+6` with Ctrl, `+7` with Shift, `+8` re-enable, `+9` disable, `+10` on release; ported |
| 0D | 2 | `quit` | inline | certain | with the string DS:5BD8 (opcode 77) empty: `WM_CLOSE`; otherwise a link record `{0x12, name = DS:5BD8, stop audio = 1, go back = 0}` is run through FUN_1008_9224 = **chain to that library** instead of quitting; the script continues. Ported (`rt_quit` → `rt_link`) |
| 0E | 4 | `cdstop` | 1008:AA96 | certain | if `DAT_5B6E` (CD open): A_037 stops the CD; never set without a CD, so nothing happens; ported |
| 0F | 8 | `wavestop` | 1008:AAAA | certain | **stops** wave `+2` (0 = all; a wave not loaded is left alone), `+4` dword fade in ms (A_039); no notification, the delayed restart is cancelled. One wave with a fade: its level falls to 0 in ms and it plays on **silent** up to its natural end (keeping priority and exclusivity). All waves with a fade: the **device** volume falls in 200 ms steps, then every wave stops and the volume comes back; ported ([AUDIO.md](AUDIO.md)) |
| 10 | 24 | `calc` | 1008:9E06 | certain | **arithmetic** `{variable, n, n ≤ 3 × {operand, operation (byte), negate (byte)}}`: in order `var = var op x` (FUN_1008_8D2C) with 0 assigns, 1 +, 2 −, 3 ×, 4 rounded division, 5 \|, 6 &, 7 %; ported |
| 11 | 14 | `ifcall` | 1008:97EE | certain | if (+8 OP +10, operator +12 as in 0x26) runs script +2, otherwise (if flag +6) script +4; script 0 = ends the current script |
| 12 | 20 | `link` | 1008:9224 | certain | library change: +2 file name (16 characters), +0x12 stop audio, +0x13 go back to the previous library; happens after the script, from the message loop (message 0x4C9). At once: pending wave end notifications cancelled (A_008); with +0x12 FUN_1008_7372(`DAT_5B76`): the default sprite (byte +0x14) and its sound/music are no longer kept by the library change; if they are leftovers of a previous library they are destroyed ([AUDIO.md](AUDIO.md), [SPRITES_ANIMATIONS.md](SPRITES_ANIMATIONS.md#default-sprite-byte-0x14)) |
| 13 | 12 | `timer` | 1008:DD20 | certain | **named timer** `{id, script, ms (dword), variable, repeat}`: id = raw word; with `+10` the duration is the operand at `+6`; `+11` repeat; at most 20 (error 0x74); same id = re-armed; checked at every tick of the loop (1008:DED8), the script runs immediately; ported |
| 14 | 6 | `stoptimer` | 1008:DCAC | certain | removes the timer with id `+2` (**raw** word, as in 0x13: not an operand to decode); flag `+4` = all; ported (`rt_stop_timer`) |
| 15 | 4 | | inline | certain | sets `DAT_1502` (deferred script) and, with `DAT_0E18` (always 1 after the initialization), posts message 0x4C8: FUN_1008_EAD0 at the next loop iteration runs `DAT_1502` (FUN_1008_D754) and then destroys the default sprite with its sound and music if one existed (FUN_1008_7372(1), [SPRITES_ANIMATIONS.md](SPRITES_ANIMATIONS.md#default-sprite-byte-0x14)); ported (`rt_tick`) |
| 16 | 18 | `runrandom` | 1008:9D6C | certain | runs a random script from a list (type 13), without immediate repetition |
| 17 | 18 | `runnext` | 1008:A3D2 | certain | runs the next one of a list, cyclic |
| 18 | 12 | `replace` | 1008:E740 | certain | **sprite replacement** `{old, new, animation, script, unload the old (byte), hide first (byte)}`: if the old one is loaded, the new one takes its place in the draw list (S_044), inherits images and position if it has none (FUN_1008_E4F4, S_045; each image shown by the old one gets one more holder), otherwise the old one is hidden; then `show` of the new one and the script right away on the current frame (D754). Unloading the old one goes through FUN_1008_E1D2 (with lazy frames, byte +0x12, a repeated member image loses its holder once: [SPRITES_ANIMATIONS.md](SPRITES_ANIMATIONS.md#lazy-frames-byte-0x12)). 311 uses (poses of the menu characters); ported |
| 19 | 6 | | 1008:8FBE | certain | `{sprite, byte}` = field +0x65 (S_004 = 1000:6B82): the `06` loops of the animation end; ported (`spr_break_loops`) |
| 1A | 6 | | 1008:CED0 case 0x1A | certain | `{sprite, animation}`: if the sprite is loaded (`73DC(id).+4` = DLL handle) → FUN_1008_A990(0, anim, sprite) = show without hiding. **Quirk**: the handler decodes both operands and A990 decodes them again (it expects encoded operands like op 05), so an id below 0x159F is read as the index of a variable/local and a larger one becomes `id + 0x7531`; ported as is (`rt_op`). Not in the translated logic of the 9 games |
| 1B | 6 | `debug` | 1008:91B8 | certain | `{value, format}`: `wvsprintf` + `OutputDebugString` only with `Debug=1`; ported as a trace line (`rt_debug`) |
| 1C | 6 | | 1008:9A20 | certain | `{sprite, level}`: draw level, ordered unsigned reinsertion (S_065); ported (`spr_set_layer`) |
| 1D | 4 | | inline | certain | sets `DAT_1508`: script run when the window becomes active again (FUN_1008_E896, only with `AltTab=0`); in the games it is script 18 (resumes the pause, restarts the music with the script of `G(8)`); ported |
| 1E | 8 | | 1010:25B8 | certain | **deferred call** `{script, arg1, arg2}` (in the decompiled C of FUN_1010_25B8 the order is `(arg2, arg1, script)`): queue of 40 records at DS:5893 run by the main loop (1008:4288 -> 1008:CDAC); ported (`rt->evq`). It does NOT play a wave (an earlier hypothesis was wrong) |
| 1F | 4 | `cursor` | 1010:23E6 | certain | cursor mode (+2 byte, DAT_1514) |
| 20 | 6 | | 1008:D0xx case 0x20 | certain | `{resource, script}`: if the resource is loaded (FUN_1008_74FA = `rt_is_loaded`) runs the script with no arguments in the **same locals frame** (FUN_1008_D6D6 → CB06), or with script 0 ends the current script; otherwise nothing. Loaded: types 1 4 9 10 11 12 14 15 16 18 = record of FUN_1008_73DC set by the loaders (op 02, sprite members, scripts and lists run, animations shown, op 60), 17 = sprite exists, any other type (13 included) = never. Used by the Jungle games (JUNGPRTY.BIN); ported |
| 21 | 18 | `hit` | 1008:91A2 | certain | **collision** between two sprites `{A, B, script, arg1, arg2, repeat (byte), clear all (byte), A also hidden, B also hidden, mode}`: ordered pair (list at DS:5707, 14-byte records, FUN_1008_3E62); at every tick FUN_1008_4124 does the pixel test (S_048) and queues the script; without "repeat" the pair is removed; script 0 = removes the pair; ported |
| 22 | 10 | `runindex` | 1008:A5BE | certain | runs `list[var]`, fallback options and auto-increment |
| 23 | 4 | | inline | certain | sets `DAT_150E`: script of the Pause key (FUN_1008_3140), receives 1 = pause / 0 = resume; a returned value ≠ 0 (opcode 59) cancels the pause; without a script the pause only stops wave and music (A_042); while paused (`DAT_5B85`) the engine does no work and accepts only the Pause key; ported |
| 24 | 8 | | 1008:9AD4 | certain | **relative move** `{sprite, dx, dy}` (S_051), only if the sprite is already loaded (no automatic loading, unlike 3F); ported |
| 25 | 4 | `goto` | inline | certain | relative jump of +2 bytes |
| 26 | 10 | `jumpifnot` | 1008:98CC | certain | compares +4 and +6 with the operator +8; if false jumps by +2 |
| 27 | 8 | `zoom` | 1008:A876 | certain | `{zoom, offset x, offset y}` (operands; zoom outside 0..4 → 0): display magnification of the sprite engine (S_053: +0x876 = zoom, +0x878/+0x87A = offset·zoom − 1; not zoomed if zoom 0 or zoom 1 without offset), drawn with `StretchDIBits` (dest = zoom·pos − offset·zoom + 1), then everything is redrawn (S_027); ported (`spr_set_zoom`, applied to the framebuffer in `spr_draw`). Not "FILLRECT" (that is only the window background of FUN_1008_EBF0) |
| 28 | 8 | `getpos` | 1008:961C | certain | `{sprite, var x, var y}`: position of the sprite (S_054); ported |
| 29 | 8 | `text` | 1010:1E82 | certain | **text** `{type-18 resource, format (string id), value}`: `wsprintf` into the resource buffer and redraw (1010:1DE8); ported (`spr_set_text`) |
| 2A | 10 | `cliprect` | 1008:9022 | certain | `{x1, y1, x2, y2}` (operands, screen-centre relative): clip rectangle of the sprite engine (S_056, +0x86A..+0x870): valid only if x1 < x2 and y1 < y2, then inclusive; repaints happen only inside it, outside the old pixels stay; any other rect (e.g. all 0) = no clip; reset at every library switch (C5F8 `S_056(0, 0)`, S_047); ported (`spr_set_clip`) |
| 2B | 10 | `switchvar` | 1008:A67C | certain | switch on a variable: +4 table, +6 default, +8 number of cases |
| 2C | 0 | `end` | inline | certain | end of the script |
| 2D | 2 | `nop` | inline | certain | |
| 2E | 8 | `random` | 1008:9C8C | certain | `{var, min, max}`: random between min and max, without repeating the previous result (`DAT_0CFA`); ported |
| 2F | 6 | `keystate` | 1008:9996 | certain | `{variable, key}`: variable = 1 if the key (virtual key < 256) is pressed (`GetKeyState`); ported |
| 30 | 10 | `getdate` | 1008:952E | certain | `{var weekday, var month, var day, var year}` (INT 21h AH=2Ah): weekday 0 = Sunday, month 1..12, day 1..31, full year; each destination written only if its operand word ≠ 0 (variables only). Ported (`localtime`) |
| 31 | 12 | `angle` | 1008:79F0 | certain | `{dest, x0, y0, x1, y1}`: dest = `GETANGLE(x1 − x0, y1 − y0)`, like builtin 31. Ported |
| 32 | 20 | `ray` | 1008:7DF6 | certain | exit point of a ray from a rectangle (`TANGENT`, `NORMALIZEANGLE`); ported (`op_ray`) |
| 34 | 8 | `wallpaper` | 1008:9FAA | certain | "7th Level Living Wall Paper" `{resource (type 3 file or type 1 bitmap), file name, tile (byte)}`: writes `<WindowsDir>\name` and makes it the Windows desktop wallpaper (`[Desktop]` of WIN.INI, `SPI_SETDESKWALLPAPER`). **Deliberately a no-op with a trace line**: a port must not change the user's desktop (see "Win16-only features") |
| 35 | 4 | `mute` | A_023 | certain | **global mute** `{signed byte}`: < 0 toggles, 0 off, > 0 on; zeroes the volume of wave, MIDI and CD without stopping the sounds, applies to all libraries (`*` key of the keypad, scripts 11/12); ported |
| 36 | 12 | `textfield` | 1010:22A0 | certain | **text field** `{type-18 text, string (raw id), final script, -, max (byte, 0 = 127), digits only (byte)}`; text 0 = closes the active field. See "Input"; ported |
| 37 | 12 | `getrect` | 1008:92E4 | certain | `{sprite, left, top, right, bottom}`: rectangle of the current image (S_060, fields +8..+0xE); ported |
| 38 | 32 | `sprintf` | 1008:A1B8 | certain | +2 destination string (string variables only), +4 format, +6 number of arguments, +8 up to 6 arguments of 4 bytes {word, byte: 0 = string id, ≠0 = operand}; `wvsprintf`, result truncated to 127 characters |
| 39, 3A | 8 | `strcmp`, `strcmpi` | 1008:A52A | certain | `{var, string, string}`: −1/0/1, variable unchanged if a string is missing; ported |
| 3B, 3C | 10 | `iniread`, `iniwrite` | 1008:B11E | certain | file `7THLEVEL.INI`; +2 variable (integer) or string id, +4 key, +6 section (string id; 0 = title of the library `DAT_5BF8` if not empty, otherwise DS:5B98 = string 0xD2 of TOPSY.EXE (the default INI section name `Topsy Turvy`), loaded by `LoadString` in FUN_1000_05E4), +8 encryption (byte + first character of the key, in two nibbles '0'+n), +9 0 = integer / 1 = string (escapes `\n \r \t \" \\`; reading, FUN_1008_AC00 of TOPSY.EXE: the character after `\` is lowercased first, so `\N` is a newline and an unknown `\X` gives `x`, and a final lone `\` ends the string and is dropped; the Jungle EXE keeps strings raw). Real example: `[Topsy Turvy Games] FreeSpace` |
| 3D | 22 | `hover` | 1008:4252 | certain | cursor pass-over areas `{x1, y1, x2, y2, enter script, arg, exit script, arg, sprite}`, flag `+0x14` = clear all; max 40 (DS:492B); ported (`op_zone`) |
| 3E | 22 | `intersect` | 1008:7B1C | certain | intersection of two lines (`GETINTERSECT`); ported |
| 3F | 8 | | 1008:A0C0 | certain | **moves the sprite** `{sprite, x, y}` absolute (S_038 -> FUN_1000_383C); first **loads** it if absent (FUN_1008_740E(0x11) → 6C08; without this, the stopwatch of the first round of Upsy Daisy stayed in the center); ported (`spr_move`) |
| 40 | 4 | `mousefilter` | inline | certain | sets `DAT_1504` = **mouse filter** script (section "Input") |
| 41, 42 | 6 | `cos`, `sin` | 1008:7D70 | certain | `{dest, angle}`: dest = `COSINE` / `SINE` (10000 · cos/sin, tenths of a degree); ported |
| 43 | 4 | `inidelsection` | 1008:B0C0 | certain | `WritePrivateProfileString(section, NULL, NULL)`: deletes a section of 7THLEVEL.INI (0 = default section); ported |
| 44 | 6 | `visible` | 1008:A7B4 | certain | `{variable, sprite}`: variable = sprite visible (S_063); ported |
| 45 | 2 | `inibackup` | 1008:B20A | certain | copies `<WindowsDir>\7THLEVEL.INI` to the first free `7thlevel.001..999` (`"7thlevel.%03i"`, FUN_1008_AE40; `.999` if all exist) in 1 KB blocks (FUN_1008_AB1C). Ported: same naming, in the folder of the port's INI |
| 46 | 10 | `gettime` | 1008:96FC | certain | `{var hour, var minute, var second, var hundredths}` (INT 21h AH=2Ch), same writing rule as 30. Ported |
| 47 | 6 | | 1008:9122 | certain | `{sprite (0 = all), byte}`: stamps the sprite onto the background (FUN_1000_0426: only with a background **bitmap** (+0x448) and the sprite visible); byte 0 = then removes it (S_025 + S_010); ported (`spr_stamp`) |
| 48 | 6 | `settable` | inline | certain | `table_4090[+2] = +4` |
| 49 | 6 | `setmouse` | inline | certain | `SETCURSORPOS` |
| 4B | 24 | | 1008:941E | certain | **frame ids** `{sprite, 10 variables}`: the first variable is zeroed, then variable i (if ≠ 0) receives the resource id of frame i (S_023, field +0xE); ported |
| 4C | var | `expr` | `Script_EvalExpr` | certain | expression statement (assignments, calls) |
| 4D | var | `if` | 1008:9966 | certain | +2 jump if false, +4 start of the body, +6 expression |
| 4E | var | `switch` | 1008:A720 | certain | switch on an expression (+10) |
| 4F | var (+4) | `while` | 1008:A846 | certain | like `if`: +2 exit, +4 start of the body (= length); the body ends with a `goto` to the condition |
| 50 | 10 | `midi` | 1008:9BBE | certain | **plays a MIDI** `{MIDI, repetitions, notify (byte +6), script +8}`: one music at a time (the previous one stops without notification); ported |
| 51 | 4 | `midistop` | 1008:A4C4 | certain | stops MIDI `+2` (0 = the current one), without notification; ported |
| 52 | 16 | `keyseq` | 1008:4FCE | certain | edits the 50-entry table of the player record (+0xC); operands as 56. Ported (section "Player controllers") |
| 53 | 12 | `padsel` | 1008:535C | certain | selects/turns on/suspends the device of a player; ported |
| 54 | 6 | `padscript` | 1008:5558 | certain | `{player, script}`: events script; ported |
| 55 | 8 | `padlatch` | 1008:5190 | certain | `{player, latch, fire}`: latch flag; with latch 0 and fire ≠ 0 a pending combination is queued; ported |
| 56 | 16 | `padcombo` | 1008:4F0A | certain | edits the 4-entry combination table (+0x1CE) matched against the history; ported |
| 57 | 6 | `padmirror` | 1008:52D0 | certain | `{player, flag}`: mirrored direction codes (table DS:0022); ported |
| 58 | 10 | `padstate` | 1008:5092 | certain | `{player, var direction, var buttons, var pending}`; ported |
| 59 | var | `return` | `Script_EvalExpr` | certain | return value, end of the script |
| 5C | 14 | `midiparams` | 1008:9EAC | certain | A_032 on MIDI `+2` (loaded first): `+5` ≠ 0 sets the tempo, every delay becomes `delay × mult / div` with `+0xA` = divisor and `+0xC` = multiplier (operands; divisor 0 = unscaled; 3 ms minimum after scaling); `+4` ≠ 0 sets the channel mute flags: for each bit of the mask `+6` the channel takes the bit of `+8`, a channel that becomes muted gets All Notes Off (CC 0x7B), then its note-ons are dropped. Kept by the MIDI event until it is unloaded; ported ([AUDIO.md](AUDIO.md)) |
| 5D | 4 | `volume` | 1008:A32A | certain | device volume one step up (`+2` ≠ 0) or down: `+3` 0 all, **1 CD (AUX)**, **2 MIDI**, **3 wave**; 32 levels (`>> 11` of the Windows volume, FUN_1008_8CBE), written to `[audio]` MidiVolume / WaveVolume. Without a CD AUX device device 1 does nothing (A_017 reads 0, A_031 fails, CdVolume not written); ported |
| 5E | 6 | `textcolor` | 1010:1F58 | probable | `{type-18 text, word}`: stores the raw word in the runtime text record (word 2, +4: the colour, an encoded operand) and redraws (1010:1DE8); the record word comes from word 0 of the resource (1010:1CA0) and drawing (1010:1B32) maps the white DrawText pixels to its low byte, so the raw word is not decoded: a constant `c` arrives as `c - 0x7531` and its low byte is used as is; not used by the present games. Ported (`spr_set_text_color`) |
| 60 | 4 | `setpalette` | 1008:0770 | certain | `{palette resource (type 14)}`: loads it (740E(0xE)), copies 0x3B0 bytes = 236 × {R, G, B, flag} into the base palette DS:1110 (= DS:10E8 + 10·4) and applies them to colours 10..245 (S_005); ported (`rt->pal_base`/`rt->pal`, applied by the frame) |
| 61 | 12 | `palfill` | 1008:067C | certain | `{first, count, restore byte, R G B flag}`: in palette mode only (always in the port): `first` (> 9, else nothing), `count` (≤ 0xEC) entries set to the colour at +8, or with the byte ≠ 0 restored from the base palette DS:10E8; applied at once (ANIMATEPALETTE); ported |
| 6A | 4 | | inline | certain | enables the debug hook (`DAT_40DC`) |
| 71 | 6 | `case` | – | certain | table entry `{0x71, value, target}` |
| 77 | 18 | `setstring` | inline | certain | copies 16 characters into the global string DS:5BD8 |
| 8D | 6 | | inline | certain | sets `DAT_40DE` |
| 8F | 6 | `nop6` | inline | certain | |

Opcodes without a handler (`default`: 00, 33, 4A, 5A, 5B, 5F, 62-69, 6B-70, 72-76, 78-8C, 8E,
90-FF) → runtime error 0x73. Ported: `rt_op` calls `rt_error(0x73)` and the script ends.

**Runtime errors** (FUN_1010_2682): message box with the string of the error code, `DAT_1548` =
code (every script stops), then `WM_CLOSE`. The port (`rt_error`) writes the message to stderr
and the trace, sets `rt->stop` and asks the main loop to quit. Besides 0x73, the expression
operators `/` and `%` with a zero divisor give error **0x6F** (`Script_EvalExpr`, 1008:2866):
`scr2c` translates them to `rt_div` / `rt_mod` (before, generated C divided directly, which is
undefined behaviour and crashes on x86). Game logic generated before this change still uses
the plain C operators until it is regenerated.

### State in the port

Each instruction of a script reaches the runtime in one of these three ways (`port\tools\scr2c.c`, `rt_op`
in `port\src\rt.c`):

- **translated to inline C by `scr2c`**: 01 02 03 05 0A 0B 0D 11 12 14 15 16 17 1B 1D 1F 22 23 25
  26 2B 2C 2D 38 3B 3C 40 48 49 4C 4D 4E 4F 59 6A 71 77 8D 8F (flow control, calls,
  expressions, engine variables, INI, cursor, fade, timers);
- **handled by `rt_op`**: 04 06 07 08 09 0C 0E 0F 10 13 18 19 1A 1C 1E 20 21 24 27 28 29 2A 2E 2F
  30 31 32 34 35 36 37 39 3A 3D 3E 3F 41 42 43 44 45 46 47 4B 50 51 52 53 54 55 56 57 58 5C 5D 5E 60
  61;
- **fallback**: no opcode of the original is left for it; the final branch of `rt_op` still
  increments `unimpl_op[op]`, writes `?? op XX not yet ported` into the trace and **continues**.
  A word that is not an opcode of the original at all gives runtime error 0x73 (see above).

Every opcode of the original is handled by `rt_op` (or compiled inline by `scr2c`): no opcode is
left unported.

Partial: 09/0E/5D (CD audio behaves as with no CD drive), 38 (`%l`
arguments stay 16 bit), 12 (the go-back case "previous == current library closes the window"
of FUN_1008_9224 is not modelled). Audio model: [AUDIO.md](AUDIO.md); sprites and resources:
[SPRITES_ANIMATIONS.md](SPRITES_ANIMATIONS.md).

### Win16-only features: design choices

Some instructions and builtins drive Windows 3.1 itself rather than the game. The port keeps the
script-visible result of the original but never touches the user's system:

- **opcode 34** (Living Wall Paper, desktop wallpaper): no-op with a trace line; the result is
  the same as a failed `RESEXTRACTFILE` (nothing happens, no return value);
- **builtin 96** (`WinExec` / `ShellExecute` of a program, with an optional script when it
  closes): disabled, returns 0 like a failed `WinExec` (< 32); a game script must not launch
  programs on a modern PC;
- **builtin 68** (print a bitmap on the default printer): returns 0 = no printer;
- **builtin 94** (`ExtractIcon` of an EXE/DLL/ICO into a bitmap): returns 0 = failure;
- **builtin 8B** (`GetDriveType` of the drive the game was started from): always 3 (fixed disk):
  the port always runs from an installed copy;
- **builtins 8C / 90** (free memory, Windows version): 0x7FFF (maximum detail) and 0x1F (what a
  16-bit program sees on Windows 3.1/NT; 0x5F would mean Windows 95);
- **builtin 9A** (`DAT_5B92`, Ctrl+Shift screen-capture dragging, an authors' debugging aid):
  stored; it only exempts Ctrl+Shift clicks from the rectangle selection, the capture itself is
  not ported;
- **builtin 91** (rectangle selection, `DAT_1546`): the selection works (press, move, release
  → script `(x1, y1, x2, y2)`), but the XOR rubber rectangle (S_080) is not drawn;
- **builtin 78** (window shake with `ScrollDC`): the last frame is presented shifted by
  `(dx, dy)`, then `(−dx, −dy)`, one frame each, then back in place (`rt->shake`, `main.c`);
- **opcode 45** (backup of `<WindowsDir>\7THLEVEL.INI`): done on the port's own INI, in its
  folder.

## Input (WndProc 1008:ED3C)

Ported in `port\src\rt.c` (`rt_key_down/up`, `rt_mouse_move`, `rt_mouse_button`); the loop of
`main.c` translates SDL events into the corresponding Windows messages.

**Coordinates**: relative to the **center of the screen** (viewport origin: TOPSYS01 does
`SetViewportOrgEx(w/2, h/2)`, the EXE subtracts `DAT_10C8/10CA` in FUN_1008_3CA2), like sprites.

**System variables** (globals common to all libraries, DS:3C5C = `G(5001)`, one word per
`G(5000+n)` = DS:3C5A + 2n; `G(5001..5117)`, 0x75 words, are zeroed at startup by
`FUN_1008_C794`, which then calls `FUN_1008_BF32` to fill the first 16). Scripts read them
like any global; `G(5018..5117)` are game variables (e.g. `G(5038)` key choice,
`G(5068)` product variant set by `TUPSYPC.BIN`).

| Global | DS | Meaning and source | Value in the port |
|---|---|---|---|
| `G(5001)` | 3C5C | last mouse button pressed (1 left, 2 right), written by 1008:35B2 | `rt_mouse_button` |
| `G(5002)` | 3C5E | **LoMem** = low memory, reduced detail: `GetPrivateProfileInt` `LoMem` in the "Topsy Turvy" section, default `DAT_5B81` (set to 1 by the memory and CPU check `FUN_1000_04B0`) | `rt->lomem` from `[Topsy Turvy] LoMem` (`rt_load_settings`), default 0 |
| `G(5003)`, `G(5004)` | 3C60, 3C62 | screen resolution: `GetDeviceCaps` 8 (HORZRES) and 10 (VERTRES) in `DAT_1518`/`DAT_151A`; with >= 800 the game chooses the 800x600 backgrounds | `screen_w`/`screen_h` (`main.c`: the logical resolution, 800x600 by default, 640x480 with `--640`) |
| `G(5005)`, `G(5006)` | 3C64, 3C66 | cursor position: at startup `GetCursorPos` minus the viewport origin (`DAT_10C8/10CA`); then WM_MOUSEMOVE (1008:3CEA) and opcode 49 | `mouse_x`/`mouse_y`; zero when the mouse becomes a controller (cursor at the center) |
| `G(5007)`, `G(5008)` | 3C68, 3C6A | point of the last click (1008:35B2) | `rt_mouse_button` |
| `G(5009)`, `G(5010)` | 3C6C, 3C6E | release point of a dragged sprite (1008:3036) | `rt.c` (end of drag) |
| `G(5011)` | 3C70 | `DAT_153E`: command-line option, 0 none, 1 `w`, 2 `k`, 3 `s` (1000:0446) | 0 |
| `G(5012)` | 3C72 | `DAT_5B67`: wave audio available (result of `A_019` at startup) | 1 |
| `G(5013)` | 3C74 | `GetProcessorType` & 0xFF (the port: 5 = Pentium or later) | 5 |
| `G(5014)` | 3C76 | `DAT_5B78`: screen with more than 256 colors (1000:00A0: `GetDeviceCaps` 12 and 14, bits per pixel x planes) | 1 |
| `G(5015)` | 3C78 | `DAT_5B7C`: default 1; the command-line option `s0` sets it to 0 | 1 |
| `G(5016)` | 3C7A | `DAT_40D4`: joysticks captured at startup (FUN_1008_59C2, 0..2); the game offers the joystick only if > 0 | `rt->joy_n` (from `main.c`) |
| `G(5017)` | 3C7C | sprite of the last animation event (commands 15/16, callback 1008:CC5x) | `rt.c` (callback) and `sprite.c` |

Known difference: the original fills `G(5002..5016)` only once at startup (BF32 is called
only by C794); the port repeats the assignment at every library open (`rt_open_library`,
`rt.c`), so all `G(5002..5016)` are rewritten at every `link` (the common globals, for the rest, pass to the new library).

**Keyboard** (WM_KEYDOWN → FUN_1008_3B3C, WM_KEYUP → 3C42): table of 256 11-byte records at
DS:4BFB (zeroed at every library open) `{normal, Shift, Ctrl, release, disabled}`
filled by opcode 0C. On press: if the filter `DAT_1506` (builtin 69) exists it is called
immediately with the key code and, if it returns 0, the key stops there; then, if the key is not
disabled, the script for Shift / Ctrl / normal is run immediately (in this order of priority).
On release the "release" script runs (ignoring the disabled state). The codes are Windows
**virtual keys** (Upsy uses 13, 27, 32, digits, letters, F1/F6/F7/F8/F11, keypad `* + -`).
Alt+key (WM_SYSKEYDOWN) reaches the scripts only for F10. The Pause key (0x13)
pauses the game (FUN_1008_3140, opcode 23): see the table.

**Text field** (opcode 36, `DAT_1500`): the text of the type-18 resource is edited with
WM_CHAR (FUN_1010_230C), with a static `_` cursor at the end (`" "` if empty). Esc restores the
starting text (copy in DS:5C4A), Enter confirms; in both cases the text goes into the
string variable and the final script is queued with no arguments (FUN_1010_20E8). Backspace removes the
last character, Del (WM_KEYDOWN 0x2E) clears everything, Tab and `@` are excluded, with the
"digits only" flag only `0..9` pass, beyond the maximum length the character is rejected
(MessageBeep). With the field active the key scripts do not start (FUN_1008_3B3C) and the
controllers do not queue events (FUN_1008_4DE8); releases do. On activation the queued WM_CHARs
are discarded (FUN_1010_2658). Upsy and Outwords use it for the name in the high-score table (TOPSUPSY
script 7391 → 7392, which writes `Name<n>` into the INI).

**Active window** (WM_ACTIVATEAPP / WM_NCACTIVATE → FUN_1008_E896): on deactivation the
controllers are released, wave and MIDI stop (A_005(0), the music is lost) and a
drag is cancelled; with `AltTab=0` (default) the engine stops working and on
reactivation runs the script `DAT_1508` (opcode 1D). With `AltTab=1` the game keeps running.

**Inactivity timeout** (builtin 93): every mouse or keyboard message (not the joystick)
re-arms it (FUN_1008_EB56); on expiry the script goes into the deferred call queue.

**Mouse**, press (FUN_1008_35B2): nothing if the cursor is in mode 3 or 4 or if a drag is in progress;
`G(5001)`, `G(5007/8)`; filter `DAT_1504` (opcode 40) called immediately with 1 (0 =
stop); then **hit test** on sprites (S_029, from the top): state 2 never, otherwise visible
or state 1; in the rectangle and, without the "rectangle" flag, on a non-transparent pixel. On the sprite
hit: if draggable the drag starts (script `+8`), otherwise, if the click is not
disabled, script `+0` with argument `+2` (data of the type-17 resource, `SPRITES_ANIMATIONS.md`).
Without a script the **rectangular areas** of opcode 04 are searched, from the last registered. The script
found goes into the deferred call queue (like opcode 1E) with arguments `(sprite, arg)`
(sprite = 0 for an area). Release (383A): filter with 2, then end of any drag
(`G(5009/10)`, script `+0xA` with the sprite as argument). Movement (3CEA): `G(5005/6)`, filter with
4, relative move of the dragged sprite, then the script `DAT_1542` (builtin 6F) for
entering/leaving sprites. The filters can be chained: the script saves the previous one with
builtin 81 and calls it back (e.g. TOPSMAIN 3078 → 8729, which moves the **cursor sprite** 8730).

**Cursors** (opcode 1F, FUN_1010_27F6): mode 1 arrow, 2..3 and 5..9 cursors 0x3E9..0x3EF of
TOPSY.EXE (NE resources), 4 and others no cursor; mode 9 (0x3EF) is an **empty** cursor: the
menu draws its own cursor as a sprite. Ported (`video_cursor_mode`): the cursors are read from
the NE resources of the original EXE (`GAME_EXE_RES` of the profile), mode 4 hides the cursor.

## Audio (TOPSYA01, ported in `rt.c` "sounds" + `audio.c`)

Full model in [AUDIO.md](AUDIO.md). One **sound event** per resource: a new `08` on the same wave
restarts it. At the end of the sound, if the "notify" flag is on, the DLL sends the window message
**0x52C** `{sound, script}` (FUN_1000_19E6 for MIDI, A_004 for waves); the WndProc (1008:ED3C)
puts the script in the **deferred call queue** with the sound as argument (like opcode 1E) or,
with script 0, **unloads** the sound. Explicit stop (0F, 51, new 50), a wave cut by an exclusive
wave and a wave refused by the priority gate do not notify. The moment of the end is computed
from the duration (FUN_1000_37EE: size / frequency), × (repetitions + 1); the port does the same,
so notifications arrive even without an audio device or without a SoundFont. On library change
(FUN_1008_C5F8) the resources are unloaded, pending 0x52Cs removed and pauses zeroed (A_041):
the port stops all sounds except the default sound/music of a background sprite. Example: in
TOPSOPN2 wave 5869 notifies script 23, which does the `link` to the menu (end of the
introduction).

Waves have a **priority** (header +8, signed) and an **overlay** flag (header +5): a wave without
overlay cuts every other non-overlay wave when it starts; a start whose priority is lower than the
current one is refused while the device is busy (Upsy: e.g. TOPSMAIN wave 8748, priority 1, is
refused while the overlay wave 8815, priority 10, is in its first pass).

## Player controllers

In the minigames each player (0 or 1) uses a **device**: a 0x201-byte record at DS:44EA, with
the current direction (`+2`), the pressed buttons (`+4`), the events script (`+6`, opcode
**54** `{player, script}`), the type (`+0x1FB`), the source (`+0x1FF`) and the active flag
(`+0x200`). A table at DS:48FD says which player uses each source: 1/2 = key set
0/1, 3/4 = joystick, 5 = mouse.

**Opcode 53** (FUN_1008_535C) `{player, parameter, type (byte), enable (byte), suspend (byte)}`:
first the player's current device is turned on or off according to "enable"
(FUN_1008_4BA6); with "suspend" the pressed buttons and the direction are released
(FUN_1008_4BF6: the script receives 0xFE/0xFD/0xFB/0xF7 and 0). If either flag is 1 the opcode
ends there (TOPSUPSY uses them to suspend and resume the controllers). Otherwise it chooses the
device: type 1 joystick (source parameter + 3), 2 **keyboard** (set = parameter), 5
**mouse**, 0 none; the new device is turned on. `DAT_5B7A` (a player uses the keyboard)
and `DAT_5B7B` (mouse) tell the WndProc to pass keys and mouse to the controllers; opening a
library (C474) zeroes them, the records remain.

**Keyboard.** Two sets of 8 pairs {key, code} at DS:0042 (16 bytes per set). Initial data of
TOPSY.EXE: set 0 = `F V C B` (up, down, left, right) and `1 2 3 4`, set 1 = arrow keys and
`9 0 - =`. Builtin **6B** `{set, slot 1..8, key}` (FUN_1008_5DFC) changes a key: slots
1..8 are the offsets 8, 0xA, 0, 0xC, 2, 6, 4, 0xE of the set (1 button 1, 2 button 2, 3 up, 4
button 4, 5 down, 6 right, 7 left, 8 button 8). TOPSUPSY (script 3058, values from
`G(251..262)` chosen by script 3060 according to `G(5038)`) sets: **player 1 = arrows + `,`
`.`**, **player 2 = `S X Z C` + `1` `2`**. FUN_1008_5D7A registers the keys of the set in a
per-key table (DS:40EA: set, ready, code); keys already pressed count immediately
(GetKeyState). On press (FUN_1008_5BD8) a key of the set no longer goes to the key scripts:
codes 0x10 up, 0x20 down, 0x40 right, 0x80 left are combined into a per-set state
(DS:4903, bits 1/2/4/8) translated by table DS:0062 into the direction code:

| State | none | up | down | right | up+right | down+right | left | up+left | down+left | opposite |
|---|---|---|---|---|---|---|---|---|---|---|
| Code | 0 | 3 | 5 | 7 | 0x0A | 0x0C | 6 | 9 | 0x0B | 0x88 (ignored) |

The other codes are **buttons** (1, 2, 4, 8); Windows auto-repeat does not count
("ready" flag). On release (5C82) buttons send the complement (0xFE, 0xFD, 0xFB, 0xF7);
a direction released while another stays pressed sends the new code after **200 ms**
(timer FUN_1008_5D48, canceled by FUN_1008_DCF0 at every press), so that letting go of a diagonal
does not pass through a single direction.

**Mouse** (FUN_1008_8180 / 80A4 / 81EA): when turned on the cursor goes to the center; moves within
±20 pixels = direction 0, beyond that = quadrant of `GETANGLE(x, y)` (angle in tenths of a degree,
0 = down, 900 = right) shifted by 45°, table DS:0086 → 5 down, 7 right, 3 up, 6 left; beyond
±40 pixels the cursor is brought back to the edge (FUN_1008_788A; the port brings it back to ±38 along the
same direction). An event is sent only when the direction changes. Left button 1/0xFE,
right 2/0xFD. While the mouse is a controller, clicks and moves do not reach the sprites.

Every change goes to **FUN_1008_4DE8**: updates direction/buttons of the record and queues the
events script with `(player, code)` (FUN_1010_25B8: the first argument is the
second parameter of the decompiled call; TOPSUPSY: script 3100, which calls 4988..5003).

**Joystick** (ported with SDL): at startup FUN_1008_59C2 captures at most two
joysticks (`JOYSETCAPTURE`, threshold 10) and measures their center; the number goes into `DAT_40D4` →
`G(5016)` (the game offers the joystick in the options only if present). The messages `MM_JOY1MOVE`/
`MM_JOY2MOVE` (0x3A0/0x3A1) and `MM_JOYnBUTTONDOWN/UP` (0x3B5..0x3B8) arrive at FUN_1008_5AF4.
The position becomes a **zone** 0..8: with the normal reading (FUN_1008_5846) +3 up, +6 down, +1
right, +2 left beyond ±16000 from the center (also diagonals); with builtin **6E** = 1
(`DAT_5B7E`, FUN_1008_578E: TOPSUPSY enables it when a player chooses the joystick) only the
dominant direction beyond ±8000. Buttons 1..4 (FUN_1008_56FC) are zones 9/10, 11/12, 13/14,
15/16 (release/press). Joystick 2 uses zones +17 (builtin 82, `DAT_5B7F`, swaps the
two joysticks; not used). If a player uses the joystick (`DAT_5B79`), FUN_1008_5928 sends to
FUN_1008_4DE8 (source 3/4) the code from table DS:0030 `{0, 7, 6, 3, 0x0A, 9, 5, 0x0C, 0x0B,
0xFE, 1, 0xFD, 2, 0xFB, 4, 0xF7, 8}`, only if the zone is "ready" (DS:4905, 17 bytes per joystick
turned on by FUN_1008_4A60): a direction blocks itself and frees the previous one, a press
frees the release and vice versa. Otherwise the zone runs the script of table DS:4090
(opcode 48). In the port: `rt_joy_move`/`rt_joy_button`, events `SDL_JOYAXISMOTION`/`SDL_JOYBUTTON*`
(axes 0 and 1, center 0); for testing, `--joysticks N` and the `joymove`/`joybutton` commands of
`--input`. The INI key for the choice is `InputMethod1`/`InputMethod2` (in Upsy Daisy in
`[Topsy Turvy Games.Upsy]`; 0 keyboard, 1 joystick, 2 mouse).

**History and combinations** (not used by the present games, ported). FUN_1008_4DE8 also
keeps a history of the codes in the record (`+0`): for directions and button presses (not for
releases or 0) `h = (h & 0xFF) · 16 + code`; the first code of a run arms the timer
FUN_1008_4C9E for `[Input] Timeout` ms of 7THLEVEL.INI (default 200, `DAT_4929`). When it
expires (FUN_1008_4D4E; 4C9E itself is not decompiled, the call is probable) the history is
consumed: the first enabled entry of the **combination table** (opcode **56**, FUN_1008_4F0A, 4
entries at `+0x1CE`) with a script and `(mask & history) == pattern` queues
`script(player, key)`; with the **latch** on (opcode **55**, FUN_1008_5190, `+0x1FD`) it is kept
as pending instead, and `55 {player, 0, fire ≠ 0}` queues it later. Opcode **52**
(FUN_1008_4FCE) edits a second table of 50 entries at `+0xC` with the same operands; nothing
reads it except FUN_1008_4D4E, which copies the pending record from that table at the index of
the matched combination (an original quirk, kept). Table entries are 9 bytes `{key, script,
pattern, mask, enabled}` (raw words of the instruction). Operands of 52/56: `{+2 player, +4 key,
+6 script, +8 pattern, +0xA mask, +0xC all (byte), +0xD enable (byte), +0xE disable (byte)}`:
for one entry (looked up by key, FUN_1008_4ACE) enable/disable set its flag, script 0 removes
it, pattern 0 updates key and script in place, otherwise a new enabled entry is inserted sorted
by descending pattern (FUN_1008_4972); with "all" enable/disable apply to every entry, script 0
empties the table, otherwise every entry gets the script. Opcode **57** (FUN_1008_52D0)
mirrors a player: direction codes sent to the script go through DS:0022 `{0, 1, 2, 3, 4, 5, 7,
6, 8, 0x0A, 9, 0x0C, 0x0B}` (left and right swapped). Opcode **58** (FUN_1008_5092) reads
`{direction (mirrored), buttons, pending combination present}` into variables. Builtin 82 is
ported too.

## Builtin functions (`Engine_Command`, expression 0x0D)

| Id | Function | Certainty |
|---|---|---|
| 2E | random number inclusive between two bounds (CRT `rand`, 1018:03CC); ported | certain |
| 2F | `GETKEYSTATE`; ported | certain |
| 31 | `GETANGLE`; ported | certain |
| 39 / 3A | `lstrcmp` / `lstrcmpi` of two strings (ids), −1/0/1; ported | certain |
| 41 / 42 | `COSINE` / `SINE`; ported | certain |
| 62 | absolute value; ported | certain |
| 44 | sprite visible? (S_063, field +0x56); 0 if not loaded; ported | certain |
| 65 / 66 | copy of a sprite (type 17) into an **extra slot** of the index (`RESCOPYRESOURCE`, first free slot after the entries of the file; the in-memory data, including the click scripts, are copied) and its loading; 66 unloads it and frees the slot. Ported (`lb7_copy_entry`) | certain |
| 69 | sets the **keyboard filter** script (`DAT_1506`), returns the previous one decoded | certain |
| 6F | sets the **cursor-over-sprites** script (`DAT_1542`, FUN_1008_42AE): called with `(sprite, 1)` on entering and `(sprite, 0)` on leaving; returns the previous one | certain |
| 81 | returns the current mouse filter script (`DAT_1504`), to chain filters | certain |
| 82 | `{flag}`: swaps joysticks 1 and 2 (`DAT_5B7F`, FUN_1008_5A64/5AF4); not used by the present games; ported | certain |
| 89 | `{player, free the other}`: the chosen device (opcode 53) becomes the player's and is re-enabled; with the second argument the other player's device, if it is theirs, is turned off (FUN_1008_5636); no return value; ported | certain |
| 6B | `{key set (0/1), slot 1..8, key}`: key table of the keyboard controllers (FUN_1008_5DFC, DS:0042 + set·16); ported | certain |
| 6C | `{sprite (0 = all), pause}`: animations stopped/resumed (S_071 → 1000:66F6, field +0x67); ported | certain |
| 6D | `{pause}`: timers stopped, on resume the deadlines shift (1008:DFEC, `DAT_5B87`); ported | certain |
| 6E | `{flag}`: `DAT_5B7E`, alternative joystick reading (FUN_1008_575C: only the dominant direction, threshold 8000); ported | certain |
| 79 | `{high, low, &high, &low}`: sum on a base-10000 number held in two words (`*&high·10000 + *&low += high·10000 + low`, with carry; CRT 1018:05EE/0554/0620); scores of TOPSMAIN; ported | certain |
| 73 | `{&x, &y, step, steps, x0, y0, xm, ym, x1, y1}`: point of the **parabola** through (x0,y0) at the start, (xm,ym) in the middle and (x1,y1) at the end, at step `step` of `steps` (S_074 → TOPSYS01 1000:1758, integer arithmetic in 1/1024; the same curve as animation command 05); with `steps` = 0 the variables stay unchanged; ported | certain |
| 70 | pause (1) / resume (0) of all audio (A_042: waveOutPause / waveOutRestart and A_025 on the music, only when the flag changes); ported | certain |
| 7C | `COLLIDE` (collision); ported | certain |
| 7E | `DISTANCE`; ported | certain |
| 7F | `REFLECTANGLE`; ported | certain |
| 80 | `SQUAREROOT`: `(int16) trunc(sqrt(a))` (TOPSYU01 1000:1098, `fistp` with truncation); ported | certain |
| 85 | `GETQUADRANT`; ported | certain |
| 90 | `GETVERSION`: 0x1F (0x5F with Windows 95); ported (Windows 3.1: 0x1F) | certain |
| 93 | **inactivity timeout** `(script, seconds)` (`DAT_150C`, 16-bit interval = seconds × 1000): input re-arms it, on expiry the script is queued (FUN_1008_DEAA); it is not `TIMEGETTIME`; ported | certain |
| 96 | launches a program (`WINEXEC`/`SHELLEXECUTE`); disabled in the port (returns 0) | certain |
| 99 | pause (1) / resume (0) of the music with a counter (A_025): the first pause sends All Notes Off on the 16 channels and holds the sequence, the last resume continues it at once (the rest of the interrupted delay is lost); ported | certain |
| 5A, 5B | animation command built by the script `{sprite, command, values...}`: 5A immediate (empties the command buffer), 5B queued (S_066/S_068 -> FUN_1000_59F6) | certain, ported (`spr_command`) |

### Other ported builtins (`rt_builtin`)

They complete the table above. All in the `Engine_Command` switch (1008:1002); `rt_builtin`
is called by the C generated by `scr2c`.

| Id | Arguments | Behavior | Original | Port |
|---|---|---|---|---|
| 2E | `{min, max}` | random **inclusive** between the two bounds (swapped if `max < min`): `min + rand() % (max - min + 1)` | 1008:1002, `rand` 1018:03CC | ported (`rt_rand`, 16-bit CRT) |
| 87 | `{a, b}` | signed minimum | 1008:1002 (then 1F28) | ported |
| 88 | `{a, b}` | signed maximum | 1008:1002 (then 1F28) | ported |
| 8C | – | `DAT_10DE` = free memory (high word of `GetFreeSpace`, 64 KB units); the startup scripts compare it with 53 and 106 to choose the detail level | 1008:1002; written in `FUN_1000_04B0` | ported: always returns 0x7FFF (maximum) |
| 90 | – | `GETVERSION`: the original returns 0x5F if the high byte (minor version) is `_` (0x5F, Windows 95), otherwise 0x1F | 1008:1002 | ported: 0x1F like Windows 3.1 |
| 97 | `{script}` | `DAT_1540` = script (0 = none, reset when a library opens): after every opcode 08 (started or refused by the priority gate) the script is queued with the wave operand as argument. The decompiled code shows `DAT_0E36` (audio context) as the queued script: a register mix-up, `DAT_1540` is assumed; returns the argument (not popped) | 1008:1002, 1008:8EAC | ported |
| 9A | `{value}` | `DAT_5B92 = value` (1 at startup, `FUN_1000_05E4`): if it is on and Ctrl+Shift are pressed, mouse and keyboard messages (1008:35B2, 383A, 3B3C, 3CEA) take the hidden path of dragging with screen capture (a hidden debugging feature of the original); TOPSCRED turns it off with 0; no return value | 1008:1002 | ported: stored (`rt->v5b92`) and returned; it only exempts Ctrl+Shift clicks from the rectangle selection, the capture itself is not ported (see "Win16-only features") |
| 72 | `{&x, &y, step, steps, x0, y0, x1, y1}` | point of the segment: `x = x0 + R((x1 − x0)·step/steps)`, same for y, R = rounded half away from zero (unsigned step/steps); steps 0 = nothing written | S_073 → TOPSYS01 FUN_1000_3F84 | ported |
| 98 | `{dx, dy}` | scrolls the whole scene: every sprite (position, start and targets of a running 05/09) moves by (dx, dy) (FUN_1000_6736); the original also scrolls the window with `ScrollDC` and repaints the uncovered strips | S_085 | ported (`spr_scroll_all`; the screen is redrawn every frame) |

### Other builtins of the original

All present in the `Engine_Command` switch (id < 0xB3). A builtin that leaves no result (`break`
in Engine_Command) leaves the expression cell holding the builtin id itself, so the port returns
the id (as for 83/84/8A when the sprite is missing). Unknown ids < 0xB3 do nothing in the
original; the port counts them in `unimpl_builtin`, writes `?? builtin XX` and returns 0.

| Id | Args | What it does in the original | Port |
|---|---|---|---|
| 02, 03, 05 | 1, 1, 2-3 | like opcodes 02 `load`, 03 `unload`, 05 `show` (FUN_1008_740E / 722A / A990), returning the handler result and never ending the script; for 05 the animation is always a resource id (args: sprite, animation, optional hide first) | ported (`rt_load`, `rt_unload`, `rt_show`) |
| 0A, 0B | 1 | like opcodes `fadein` / `fadeout` (FUN_1008_48EA / 492E, speed 0 or > 100 = 100) | ported (`rt_fade`) |
| 1B | n | `wvsprintf(format, args)` into a 72-byte buffer and `_DEBUGSTR` if `Debug=1` (`DAT_5B73`) | ported: trace, and stderr with `Debug=1` |
| 2F | 1 | `GETKEYSTATE(vk) < 0` | ported |
| 31, 41, 42 | 4, 1, 1 | `GETANGLE`, `COSINE`, `SINE` (TOPSYU01) | ported |
| 5F | 1 | if the sprite is loaded, `DAT_14FE` = sprite (FUN_1008_4324): it is dragged with the cursor until the button is released (release script and G(5009/5010) as for a normal drag); returns 1/0 | ported |
| 63 | 1 | `lstrlen` of a string (id); missing string: no result | ported |
| 64 | 3 | `{string, substring, from}`: index of the substring searched from `from` < `lstrlen` (`strstr`, FUN_1018_0720), −1 if absent or bad arguments | ported (negative `from`: −1) |
| 68 | 1 | prints a type-1 bitmap on the default printer (FUN_1008_8BD0) | 0 = no printer (see "Win16-only features") |
| 72 | 8 | `S_073` linear interpolation of two variables | ported (table above) |
| 74 | 2 | `{string, i}`: `(signed char) s[i]`, 0 if missing or `lstrlen < i` | ported |
| 75 | 3 | `{string variable, i, character}`: writes a character (i < 0x7F, no terminator) | ported |
| 76 | 1 | `atoi` of a string (copy into 112 bytes + CRT 1018:1464) | ported |
| 78 | 2 | `S_075`: window shake by `(dx, dy)` with three `ScrollDC` | ported (`rt->shake`) |
| 7A, 7B | 4 | base-10000 number in two words: 7A multiplies, 7B divides | ported |
| 7C, 7E, 7F, 85 | 8, 4, 2, 1 | `COLLIDE`, `DISTANCE`, `REFLECTANGLE`, `GETQUADRANT` (TOPSYU01) | ported |
| 7D | 2-4 | draws a bitmap into another bitmap (S_019) | ported (`spr_bitmap_blit`) |
| 83, 84, 86, 8A | 1, 2, 5, 3 | sprite user word (S_077/S_078), pixel overlap (S_048), point on sprite (S_079) | ported |
| 8B | 0 | `DAT_14FA`: `GetDriveType` of the letter of the first library | 3 = fixed disk |
| 8E | 1 | `DAT_150A` = script, called with 1/0 when the window is activated / deactivated (FUN_1008_E896) | ported (`rt_set_active`) |
| 91 | 1 | `DAT_1546` = script: a left press starts a rectangle selection, on release the script receives `(x1, y1, x2, y2)`; 0 cancels the selection (FUN_1008_3900) | ported without the rubber rectangle |
| 92 | 2 | `{type-10 movie, value}`: word +0x3A of the movie object (FUN_1010_1550); nothing reads it | trace only |
| 94 | 3 | `{type-1 bitmap, file, index}`: `ExtractIcon` drawn into the bitmap (FUN_1008_87D0), 1/0 | 0 = failure |
| 95 | 1 | writes `guest` (DS:001C) into the string variable | ported |
| 96 | 1-3 | `WinExec(program)`, or with a script FUN_1008_DB8C (window hidden, `ShellExecute`, script when the program ends); 1 on success | disabled, returns 0 |
| 97 | 1 | `DAT_1540` = script queued with the wave operand after every opcode 08 (started or refused by the priority gate) | ported (`rt->wave_hook`, table above) |
| 98 | 2 | `S_085`: scrolls the view and every sprite by `(dx, dy)` | ported (table above) |
| 9B | 3 | leftover: empties the string variable of the third argument (FUN_1008_B298) | ported |
| 9C | 3 | leftover: does nothing (FUN_1008_B2B0) | ported |
| ≥ B3 | – | DLL plug-in functions (`FUN_1008_2386`, `EXTERNALCALL`): they do not exist in the present games | not needed, not ported |
