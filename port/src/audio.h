/*
 * audio - software mixer on top of SDL2. Replaces waveOut and the mixer of TOPSYA01.DLL.
 * Voices are 7LB waves (type 9) already decoded to 16-bit PCM; the mixer resamples them
 * at the output (44100 Hz stereo) with linear interpolation.
 *
 * A voice is a "wave event" of the DLL (A_011 RESCREATEWAVEEVENT): it is created once,
 * holds the decoded data, can be started / restarted / halted many times and is freed with
 * audio_wave_destroy (A_013). The game rules (priority, exclusivity, notifications, delayed
 * starts) live in rt.c; this module only renders, with the gain and fades of the DLL mixer
 * (FUN_1000_11FD / 1070, see docs\AUDIO.md).
 */
#ifndef AUDIO_H
#define AUDIO_H

#include <stddef.h>
#include <stdint.h>

#include "lb7.h"

#define AUDIO_RATE   44100
#define AUDIO_VOICES 128   /* 100 wave events of the DLL + movie audio + test players */
#define AUDIO_LEVEL_FULL 16 /* gain level of the DLL mixer: out = (sample * level) >> 4 */

/* Opens the audio device. Without a device the mixer remains usable for audio_render. */
int audio_init(void);
void audio_shutdown(void);

/* ---- wave events */
/* Decodes a type 9 entry into an idle voice (level 16). Returns a handle > 0, 0 on error. */
int audio_wave_create(const Lb7 *lb, uint32_t entry);
/* Same from 16-bit PCM (the mixer takes ownership and frees it): movie audio. */
int audio_wave_create_pcm(int16_t *pcm, uint32_t samples, unsigned channels, uint32_t rate);
void audio_wave_destroy(int h);
/* Rewinds to sample 0 (FUN_1000_2DC4) and plays (FUN_1000_3D16); loops: 0 = once,
 * n = n more passes, -1 = forever. Level and fades are left as they are. */
void audio_wave_start(int h, int loops);
void audio_wave_halt(int h);           /* the voice stops being mixed (playing = 0) */
int audio_wave_active(int h);          /* still mixed (data not finished) */
/* Gain: target = W+0x81 (fade-in target), level = W+0x82 (current). Values are bytes
 * read as SIGNED char by the mixer, like the original (16 = unchanged, > 16 amplifies). */
void audio_wave_set_level(int h, int target, int level);
int audio_wave_level(int h);           /* current level (W+0x82) */
/* Fades counted in sample values consumed by the mixer (stereo = 2 per frame):
 * fade-in (FUN_1000_384A): level 0, +1 every `interval` values up to the target;
 * fade-out (FUN_1000_38BA): -1 every `interval` values, the first step after interval + delay;
 * the fade-out runs only when no fade-in is active. interval 0 = no fade. */
void audio_wave_fade_in(int h, uint32_t interval);
void audio_wave_fade_out(int h, uint32_t interval, uint32_t delay);
void audio_wave_fades_off(int h);

/* One-shot helpers (test players): create + start, the voice frees itself at the end. */
int audio_play_wave(const Lb7 *lb, uint32_t entry, int loops);
int audio_is_playing(int h);
void audio_stop_all(void);

/* Duration of one pass (ms) of a wave / of a MIDI sequence, 0 if the entry is not valid:
 * like TOPSYA01 (FUN_1000_37EE), the end-of-sound scripts are computed from the duration.
 * MIDI: delays scaled by mult/div (opcode 5C, div 0 = unscaled). */
uint32_t audio_wave_duration_ms(const Lb7 *lb, uint32_t entry);
uint32_t audio_midi_duration_ms(const Lb7 *lb, uint32_t entry, unsigned mult, unsigned div);
void audio_set_wave_volume(int volume); /* 0..256: wave device volume (opcode 5D device 3) */
void audio_pause_waves(int on);          /* builtin 70 (A_042), inactive window (A_005) */
void audio_pause_midi(int on);           /* builtin 99 (A_025): all notes off, sequencer held */
void audio_set_master(int volume); /* 0..256 */

/* MIDI music (type 4) with the given SoundFont; without a SoundFont the music stays muted.
 * loops: 0 = once, n = repeat n times, -1 = forever. */
int audio_init_midi(const char *sf2_path);
int audio_play_midi(const Lb7 *lb, uint32_t entry, int loops, unsigned mult, unsigned div, uint16_t mute);
void audio_stop_midi(void);
int audio_midi_playing(void);
void audio_set_music_volume(int volume); /* 0..256 */
/* Opcode 5C (A_032) parameters of a sequence: tempo mult/div (every delay becomes
 * delay * mult / div, div 0 = unscaled) and the mask of channels whose note-on (0x9n) is
 * dropped. audio_play_midi takes them at the start, audio_midi_params changes the current one. */
void audio_midi_params(unsigned mult, unsigned div, uint16_t mute);
void audio_midi_notes_off(int ch);       /* CC 0x7B on one channel (FUN_1000_1868) */
void audio_midi_remap_drums(int on);     /* 7THLEVEL.INI [audio] RemapMidiDrums */

/* Mixes frames stereo S16 samples into out (used by the SDL callback and by offline tests). */
void audio_render(int16_t *out, size_t frames);

#endif
