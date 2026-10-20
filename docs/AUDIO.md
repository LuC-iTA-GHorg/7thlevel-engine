# Audio

Audio of the 7th Level Runtime Player 1.2 lives in `TOPSYA01.DLL` (exports `A_002`..`A_042`) and
in a few handlers of `TOPSY.EXE`. The port splits it in two:

- `port\src\rt.c`, section "sounds": the rules of the DLL (wave events, priority, exclusivity,
  notifications, delayed starts, device volume fade, MIDI parameters, CD), driven by game time,
  so that traces are repeatable and the end-of-sound scripts run even without an audio device;
- `port\src\audio.c` + `midi.c`: SDL2 mixer and TinySoundFont sequencer, which only render
  (gain, fades, MIDI tempo/mute).

Addresses: `A_0xx` and `FUN_1000_xxxx` = TOPSYA01.DLL, `FUN_1008_xxxx` = TOPSY.EXE (names of
the decompilation of the original program, which is not part of this repository). `W+x` = field
of the DLL wave event (0x8E bytes), `ctx+x` = field of the DLL context.

## Wave events

- One **event per wave resource** (A_011, created when the wave is loaded: opcode 02, opcode 08
  through FUN_1008_740E, or a sprite member), at most **100** (FUN_1000_267C). The port creates
  it at the first opcode 08 (decoding the data once) and destroys it when the wave is unloaded
  (A_013: opcode 03, unload of a sprite that has it as member, 0x52C with script 0, library
  change). A playing wave that is destroyed stops without notification.
- Header (28 bytes, Wave_InitFromHeader 1000:2786): `+0` size, `+4` "in memory / allowed during
  the CD", `+5` **overlay**, `+6` IMA ADPCM, `+8` **priority** (signed int16), `+0xA` id (wParam of
  0x52C), `+0xC` WAVEFORMAT. Movie audio (animation command 17) uses the same header and goes
  through the same rules (A_011 + A_029 with a NULL block; freed by FUN_1010_10F2).
- Without an event (100 in use) FUN_1008_74FA reports "not loaded": opcode 08 then queues its
  end script at once if notify is on, and plays nothing.

### Opcode 08 (FUN_1008_8EAC -> A_029 -> FUN_1000_3ADA)

Block (op + 4, 24 bytes): `+0` repeats (0 once, n more passes, −1 forever), `+2` set volume,
`+3` notify at end, `+4` no flush, `+5` volume, `+6` fade-in ms, `+0xA` fade-out ms,
`+0xE`/`+0x12` start delay min/max ms, `+0x16` end script (resolved by the EXE). Order:

1. **Start gate**: the start happens only if `(overlay && priority == 0)`, or the device has no
   queued buffer, or `current priority <= priority` (signed). Otherwise nothing at all happens: no
   sound, no notification, no state change (trace `not started`).
2. Fades cleared; with `+2` the volume `W+0x81 = +5` (kept for the next plays, not clamped); the
   current level = the volume; repeats stored.
3. A running "stop all" device fade is cancelled and the device volume restored.
4. Fade-in if `+6 != 0` and volume ≠ 0; fade-out if `+0xA != 0`, volume ≠ 0 and repeats ≥ 0.
5. Rewind. Delayed start pending cleared.
6. With `+0xE != 0`: **delayed start** (below) and stop here.
7. If the wave is not overlay: every other playing non-overlay wave stops at once, **without
   notification** (FUN_1000_3C6E, trace `cut by`), and the current priority becomes 0.
8. If buffers are queued and `+4 == 0`: queue flush (FUN_1000_3600: current priority 0, no queued
   buffer).
9. Plays (FUN_1000_3D16): the current priority becomes the wave's (unless overlay with priority
   0); notify and script stored; `W+0x3C` = now + one pass (FUN_1000_37EE).

Then the EXE queues the builtin 97 script, if set, with the wave operand as argument.

### Current priority and "queued buffers"

`ctx+0x40` is reset to 0 at every A_004 tick and raised to the priority of every event whose
first pass is not over (`now < W+0x3C`); it is set by every start (step 9) and reset by the
flushes and the exclusive cut. The ticks that fill a waveOut buffer (one every ~190 ms, while
A_004 runs at every sprite step) compute it from the voices being mixed instead: the port models
the frequent case. "Queued buffers" (`ctx+0xC`) is modelled as "some wave is playing" at the last
tick, cleared by the flushes (also by opcode 09 without a CD, see below).

Consequences visible in the shipped games: in the Upsy Daisy scene tests a few short
low-priority waves (e.g. TOPSMAIN 8748, priority 1, during the first pass of overlay wave 8815,
priority 10) are refused, and a few looping waves are cut by an exclusive wave (TOPSMAIN 3090 by
5196); Djali Bowling and Le Food Feud show the same kind of behaviour.

### End of a wave

The mixer reaches the end of the data (FUN_1000_2B16 -> 3D9E): the wave stops playing; with a
delayed start pending it is re-armed, otherwise if notify is on the end is posted by A_004 as
0x52C `{id, script}` once one pass has elapsed. The port computes the end as
`start + one pass × (repeats + 1)` (shifted by pauses and by the time the window is inactive).

### Delayed start (FUN_1000_3A60 / 3A20, A_004)

`smin = (byte)(min / 1000)`, `smax = (byte)(max / 1000)` (whole seconds),
`range = smax > smin ? smax − smin + 1 : 0`, delay = `smin + (rand() & 0xFF) % range` (the DLL's
own MS CRT `rand`, seeded once; the port seeds it from `--seed`). A playing wave stops; fades are
already armed, but exclusivity, flush, priority and notification are not done. When the time
comes A_004 rewinds and plays it (priority set, no gate, no exclusivity). At every natural end a
**new** random delay is armed: the wave repeats forever until opcode 0F on it, an undelayed
opcode 08, its unload, a library change or the deactivation of the window. It never notifies.

### Gain and fades (mixer FUN_1000_11FD / 1070)

- `out = (sample × level) >> 4`, level 0..16 (a byte read as signed char: values above 16
  amplify, as in the original), mixed sum clipped to 16 bit.
- Fades count **sample values** consumed by the mixer (stereo = 2 per frame), so they last the
  requested time at any format: `S(ms) = ms × nAvgBytesPerSec / 1000`, halved on a 16-bit device
  (the port mixes at 16 bit: `S = ms × rate × channels / 1000`, 32-bit arithmetic).
- Fade-in: level 0, `interval = S / volume`; every `interval` values the level goes up by one
  until the volume. Interval 0 (too short): no fade.
- Fade-out at the end: `interval = S / (volume + 1)`; the first step is delayed by
  `total − S`, with `total` = sample values of all the passes minus the fade-in part, so that the
  ramp ends with the data. It counts only when no fade-in is active. At level 0 the voice goes on
  silent.

### Opcode 0F (FUN_1008_AAAA -> A_039)

- One wave (FUN_1000_3CB6): notification and delayed start cancelled; if it is playing, with no
  duration (or level 0) it stops at once (+ queue flush); with a duration the level falls to 0 in
  `ms` (`interval = S / (level + 1)`) and then the wave **plays on silent up to its natural end**
  (the DLL has no code that stops it: it keeps its priority and exclusivity; a looping wave stays
  silent forever). A wave that is not loaded is left alone.
- All waves (FUN_1000_3DE8): with no duration every wave stops at once. With a duration the
  **device** volume falls (FUN_1000_3472): `steps = ms × 5 / 1000` (one step per 200 ms tick),
  `delta = (volume + steps / 2) / steps`; at the last step or at volume 0 every wave stops and the
  volume comes back. A request during such a fade is ignored; with 0 steps (< 200 ms) the device
  volume is just set to 0 (it stays 0 until the next volume change, as in the original).

## Volumes

- Opcode 5D `{up, device}` (FUN_1008_A32A): device 0 = all, **1 = CD** (AUX, A_017/A_031),
  **2 = MIDI** (A_018/A_033), **3 = wave** (A_021/A_036). 32 levels: `level = volume >> 11`, up only
  if ≤ 30, down only if ≠ 0, stored as `level × 0xFFFF / 31` and written to `[audio]` MidiVolume /
  WaveVolume of 7THLEVEL.INI. Without a CD AUX device (always in the port) device 1 does nothing.
- Opcode 35 (A_023): mute saves and zeroes the device volumes without stopping the sounds.
- The wave device volume multiplies the whole wave mix; the per-wave level is separate.

## Pause, window, library change

- Builtin 70 (A_042): waveOutPause / waveOutRestart (the voices freeze) and A_025 on the music.
- Builtin 99 (A_025): pause counter of the music; the first pause sends All Notes Off on the 16
  channels and midiOutReset (FUN_1000_1840) and stops the sequencer; the last resume continues
  the sequence at once (the rest of the interrupted delay is lost).
- Window deactivated (A_005(0)): every wave stops without notification (delayed starts
  cancelled), the MIDI port is closed and the current sequence forgotten (FUN_1000_171E ->
  1CB2), so on reactivation (A_005(1)) the music does **not** restart: A_005(1)'s restart branch is
  only reached when the port had never been opened. Waves started while inactive are not mixed
  until the window is active again (W+0x74).
- Library change (FUN_1008_C5F8): every wave and MIDI event is destroyed without notification
  (FUN_1008_72B0), pending 0x52C removed, A_041 (current priority 0, pauses cleared), builtin 97
  cleared.

### Default sound and music (background sprite, byte +0x14)

A sprite with byte `+0x14` is the default sprite (`DAT_0E3E`); its last wave and MIDI members are
the default sound and music (`DAT_0E40`/`DAT_0E42`, FUN_1008_6D44). They are **not** started by the
engine, but the library teardown skips them: if they are playing at a `link`, they keep playing in
the next library (the port keeps them as events that cannot be addressed by id). They are
destroyed (FUN_1008_7372(1)) by a later `link` with the stop-audio byte `+0x12`, or when another
default sprite is loaded; a `link` with `+0x12` in the same library only clears the defaults, so
the normal unload stops them. Every `link` also cancels all pending wave notifications (A_008).
Message 0x4C8 (FUN_1008_EAD0, posted by script opcode 15 and animation command 14) destroys them
with the default sprite after running the deferred script `DAT_1502`. The sprite itself (kept
across the link, pinned on top of the draw list, own palette block) is described in
[SPRITES_ANIMATIONS.md](SPRITES_ANIMATIONS.md#default-sprite-byte-0x14). No shipped game sets
byte `+0x14`.

## MIDI

- One sequence at a time (A_028): a new one stops the previous without notification. Repeats as
  for waves; non-zero delays under 3 ms become 3 ms; the end is computed from the delays.
- Opcode 5C (A_032, 1000:1AB2), kept by the MIDI event until it is unloaded: tempo
  `delay × mult / div` (W+0x22 / W+0x24; 32-bit product, unsigned division, then the 3 ms
  minimum; `div` = 0 unscaled); channel mute flags: note-on (0x9n) of a muted channel is dropped,
  and a channel that becomes muted gets CC 0x7B at once. When the sequence playing is changed the
  port moves its expected end by the tempo ratio.
- `[audio] RemapMidiDrums=1`: messages of channel 10 are sent on channel 16 (FUN_1000_1BB8,
  before the mute check). The port then makes channel 16 the percussion channel of the SoundFont,
  since the remap targets base-level synthesizers whose drums are on channel 16.
- Aftertouch and channel pressure have no TinySoundFont equivalent and are ignored.

## CD audio

The port has no CD audio. Opcode 09 (A_027) then behaves like the original with no CD:
FUN_1000_014E tries to open MCI `cdaudio` after flushing the wave queue (FUN_1000_0D7C(1) -> 3600:
current priority 0), fails, and `DAT_5B6E` = 0. Opcode 0E (A_037) acts only if `DAT_5B6E`, so it
does nothing; the implicit CD stops of the EXE (resource loads, library close) are never reached.
Opcode 5D device 1 does nothing. The header byte `+4` matters only while a CD plays. A mapping of
tracks to files would need a convention for the track files that no shipped game has, so it is
not provided.

## 7THLEVEL.INI `[audio]` keys (FUN_1000_0CB8)

| Key | Original | Port |
|---|---|---|
| WaveVolume, MidiVolume | device volumes (written by opcode 5D) | read at startup, written by opcode 5D |
| DisableWaveVolume | waveOutSetVolume never called | the wave device volume (opcode 5D device 3, mute, "stop all" fade) is not applied |
| RemapMidiDrums | channel 10 -> 16 | ported (see MIDI) |
| CdVolume | AUX volume of the CD | no equivalent (no CD) |
| WaveFormatMask | formats allowed for waveOut | no equivalent: SDL converts every format |
| ProfileCD | timing profile of the MCI CD device | no equivalent |
| MaxWaveBuffers | waveOut buffers (4..8 × 8 KB): latency and the "queued" tail | no equivalent: the mixer pulls 1024-frame blocks |

## Known approximations

- The current priority follows the non-filling A_004 ticks (see above); "queued buffers" ignores
  the up-to-1.5 s tail of the waveOut queue after the last voice ends.
- The level read by a "stop one wave with fade" comes from the mixer thread, so the moment it is
  read can differ by a few ms between runs (no game uses fades).
- The original applies the gain to unsigned 8-bit samples (an offset quirk); the port converts 8-bit
  waves to 16 bit first.
- Waves count toward the 100-event limit from their first opcode 08, not from their load: the port
  can only reach the limit later than the original.
