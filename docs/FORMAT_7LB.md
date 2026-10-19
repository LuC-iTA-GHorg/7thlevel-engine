# 7LB archive format (7th Level Resource Library 1.2)

Reconstructed from `TOPSYR01.DLL`, implemented in `port\src\lb7.c` and verified with `lb7tool` on all 10 `.BIN`
files of the game. All values are little-endian; "word" = 16 bit, "dword" = 32 bit.

## Header (0x142 bytes)

| Offset | Type | Meaning |
|---|---|---|
| 0x00 | word | magic `0x4C37` ("7L") |
| 0x02 | word | header size (0x142) |
| 0x04 | char[82] | title (empty in the game files) |
| 0x56 | word | version = 2 |
| 0x62 | dword[24] | cumulative end of the chunks of the in-memory data area (key 5) |
| 0xC2 | word | number of in-memory chunks |
| 0xC4 | word | palette colors (236) |
| 0xC8 | word | extra slots in the index (resources created at runtime: 150 in all files). `RESCOPYRESOURCE` copies an entry into the first free slot after the ones of the file (builtin 65), `RESDELETERESOURCE` frees only these slots (builtin 66); in the port `lb7_copy_entry`/`lb7_delete_entry` |
| 0xD0 | byte | "block" file (blocks of 0xFFC bytes + a dword pointing to the next block); 0 in all files |
| 0xE2 | {dword off, dword size}[12] | key resource table |

The sum of all header bytes (mod 256) is 0 (`Lib_ReadHeader`).

## Key resources

| Key | Content |
|---|---|
| 0 | index: 10-byte entries `{word type, dword offset, dword size}` |
| 1 | 80-byte records (`Lib_LoadKey1`) |
| 2 | 28-byte records (`Lib_LoadKey2`) |
| 3, 4 | tables loaded when the file is opened (key 3: initial values of the globals; key 4: to be analysed) |
| 5 | in-memory data area: resources of type 10..18 have an offset relative to this area |
| 6 | palette: `PALETTEENTRY` (R, G, B, flag), usually 236 colors mapped to indices 10..245 |
| 7 | name table (`RESLOADNAMETABLE`) |

## Resource types

| Type | Content | Status |
|---|---|---|
| 1 | bitmap | **decoded** |
| 2 | large stream (2.6 MB in TOPSY.BIN): animation/movie data? | to be analysed |
| 4 | MIDI sequence | **decoded** (conversion to SMF verified) |
| 9 | wave | **decoded** |
| 10 | **movie** (script + stream + index, "Movies" below) | **decoded** |
| 11 | list of script instructions (e.g. `link` to other archives) | **decoded** (`SCRIPT_OPCODES.md`) |
| 12 | cel: `{bitmap id, dx, dy}` | **decoded** (`SPRITES_ANIMATIONS.md`) |
| 13 | list of resources to load together | **decoded** |
| 15 | animation (commands; command 0x15 contains a script instruction) | **decoded** (2230/2230 aligned, `SPRITES_ANIMATIONS.md`) |
| 17 | sprite: position, frames, members | partial (`SPRITES_ANIMATIONS.md`) |
| 18 | **text**: sprite member just like the frames (20 bytes, `SPRITES_ANIMATIONS.md` "Text") | **decoded** |
| 16 | **script** (bytecode) | **instruction encoding complete** |

### Bitmap (type 1)

20-byte header: `+0` header size, `+2` width, `+4` stride, `+6` height,
`+8` flags (`0x80` = RLE), `+0xC` and `+0xE` to be analyzed (hotspot and id?), `+0x10` runtime pointer.
Rows are stored **bottom-up** as in Windows DIBs.

- **RLE** (flag 0x80, `Bitmap_ExpandRLE`): after the header there are `height` words (row
  offsets) and then, for each row: `0` = end of row; `n < 0x80` = repeat the next byte `n` times;
  `n >= 0x80` = copy `~n` literal bytes (`0xFF`: the count is in the next byte).
  Sprites stay RLE in memory and are drawn directly by TOPSYS01.
- **LZW** (flag 0, `Chunk_Decode`): stream of chunks with a one-word header, the 3 high bits = type,
  the 13 low bits = size:
  `0x0000` uncompressed data, `0x4000`/`0x6000`/`0x8000` 10/11/12-bit LZW, `0xE000` end.
  An LZW chunk contains sub-blocks `[word length][data]`, each with a fresh dictionary.
  Fixed-width LZW, MSB-first bits, codes < 256 are literals, end code `(1<<bit)-1`,
  the dictionary grows up to `(1<<bit)-2`.

### Wave (type 9)

28-byte header: `+0` dword data size, `+4` and `+5` flags, `+6` = 4-bit IMA ADPCM,
`+8` word, `+0xA` id, `+0xC` PCM `WAVEFORMAT` (16 bytes, typically mono 22050 Hz).
The ADPCM is **standard IMA** (tables identical to TOPSYA01 DS:01E8 and DS:0778), low nibble first,
initial state 0/0. 16-bit output.

### MIDI (type 4)

Source: the timer-driven sequencer of TOPSYA01 (`FUN_1000_1BB8`, fed by `RESCREATEMIDIEVENT`).

- 6-byte header: `+0` dword event size (multiple of 4), `+4` word id.
- Then 4-byte records (little-endian dword):
  - if `(dword >> 16) & 0xF000 == 0` -> **short MIDI message**, sent as is to
    `midiOutShortMsg`: byte 0 status, bytes 1 and 2 data (e.g. `b0 07 64 00` = CC7 volume 100);
  - otherwise -> **pause** of `dword & 0x7FFFFFFF` **milliseconds** before the next record
    (minimum 3 ms; the engine can scale it with a tempo factor, fields +0x22/+0x24 of the state).
- Channel 10 (index 9) is the GM percussion; with `RemapMidiDrums` in `7THLEVEL.INI` the
  engine moves it to channel 16. A table of 16 per-channel flags can mute the note-ons.
- Repetition is handled by the engine (loop counter), not by the data.

Verification: `lb7tool extract` writes `NNNNN_midi.mid` (SMF type 0, 1 tick = 1 ms). On TOPSMAIN,
TOPSUPSY and TOPSWORD: 16 tracks from 1 to 73 s, balanced note-on/note-off, coherent GM instruments.

### Script (type 16)

Sequence of instructions: opcode word + 16-bit operands. Lengths derived from `Script_OpLength`
(TOPSY.EXE 1008:C9D4) plus the control opcodes; all 2469 scripts of the 10 archives are
walked exactly to the last byte (`lb7tool disasm`).

Operand encoding (`Op26_JumpIfNot`, `Op2B_SwitchVar`):

| Value | Meaning |
|---|---|
| `< 0x13FE` | global variable (DS:154A + 2·n) |
| `0x13FE..0x159E` | local variable of the script stack frame |
| `>= 0x159F` | constant = value − 0x8ACF (e.g. `0x8ACF` = 0) |

Known control opcodes:

| Op | Length | Meaning |
|---|---|---|
| 0x4C, 0x4F, 0x59 | word +2 | explicit length (0x59 = evaluates an expression from +4) |
| 0x4D | word +4 | block: +2 total size, +4 head |
| 0x4E | word +2 | switch on an expression: +4 table offset, +6 default, +8 number of cases, +10 expression |
| 0x2B | 10 | switch on a variable (same table) |
| 0x26 | 10 | conditional jump: +2 relative target, +4/+6 operands, +8 operator |
| 0x71 | 6 | entry of a case table `{0x71, value, target}` |

The meaning of the other ~90 opcodes is derived from their handlers in `Script_ExecOp`.

## Movies (type 10 + two of type 2)

Player in TOPSY.EXE 1010:06AC-1564 (open 1010:1166/0B00, advance 1010:12F0 and 0E02,
frames 0B7C, audio 0D12/0EBA). A movie with id N uses three consecutive entries:

| Entry | Type | Content |
|---|---|---|
| N | 10 | movie script (word commands) |
| N+1 | 2 | stream: audio blocks and frames |
| N+2 | 2 | index: 2 bytes, then 8-byte records `{type u8, flag u8, size u16, offset u32}` |

Record types: 2 = audio, 0 = key frame (always drawn), 1 = frame that can be skipped
when late. Script: `01 {n, counter, ms}` shows n frames of ms each; `02 {ms}`
wait; `03 {28-byte wave header}` starts the audio (same header as type 9: IMA ADPCM, 22050 Hz
mono in TOPSY.BIN); `04 {sync}`; `05 {script instruction}`. Audio = concatenated type-2 blocks,
a single ADPCM stream. Frames = 7LB bitmaps (RESEXPANDBITMAP): flag 1 = the 0xFF pixels
take the previous frame (S_017); flag 2 = drawn over the previous one with 0 as
transparent (S_019); in Upsy the type-1 frames all have flag 2, the type-0 ones are
complete. Example: TOPSY.BIN entry 65 = the publisher logo movie, 205 frames
320x240, 19.1 s. `build\lb7tool.exe movie FILE.BIN N folder [every]` extracts frames and audio.
The earlier hypothesis "type 10 = text" was wrong.
