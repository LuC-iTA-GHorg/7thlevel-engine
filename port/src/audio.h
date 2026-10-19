/*
 * audio - software mixer on top of SDL2. Replaces waveOut of TOPSYA01.DLL.
 * Voices are 7LB waves (type 9) already decoded to 16-bit PCM; the mixer resamples them
 * at the output (44100 Hz stereo) with linear interpolation.
 */
#ifndef AUDIO_H
#define AUDIO_H

#include <stddef.h>
#include <stdint.h>

#include "lb7.h"

#define AUDIO_RATE   44100
#define AUDIO_VOICES 16

/* Opens the audio device. Without a device the mixer remains usable for audio_render. */
int audio_init(void);
void audio_shutdown(void);

/* Plays a type 9 entry of the archive. volume 0..256 (256 = original); loops: 0 = once,
 * n = repeat n times, -1 = forever. Returns an id > 0, or 0 on error. */
int audio_play_wave(const Lb7 *lb, uint32_t entry, int loops, int volume);
/* Already decoded 16-bit PCM (the mixer takes ownership and frees it): movie audio */
int audio_play_pcm(int16_t *pcm, uint32_t samples, unsigned channels, uint32_t rate, int loops, int volume);
/* Duration of one pass (ms) of a wave / of a MIDI sequence, 0 if the entry is not valid:
 * like TOPSYA01 (FUN_1000_37EE), the end-of-sound scripts are computed from the duration. */
uint32_t audio_wave_duration_ms(const Lb7 *lb, uint32_t entry);
uint32_t audio_midi_duration_ms(const Lb7 *lb, uint32_t entry);
void audio_set_wave_volume(int volume); /* 0..256: wave device volume (opcode 5D) */
void audio_pause_waves(int on);          /* builtin 70 (A_042) */
void audio_pause_midi(int on);           /* builtin 99 (A_025) */
void audio_stop(int id);
void audio_stop_all(void);
int audio_is_playing(int id);
void audio_set_master(int volume); /* 0..256 */

/* MIDI music (type 4) with the given SoundFont; without a SoundFont the music stays muted.
 * loops: 0 = once, n = repeat n times, -1 = forever. */
int audio_init_midi(const char *sf2_path);
int audio_play_midi(const Lb7 *lb, uint32_t entry, int loops);
void audio_stop_midi(void);
int audio_midi_playing(void);
void audio_set_music_volume(int volume); /* 0..256 */

/* Mixes frames stereo S16 samples into out (used by the SDL callback and by offline tests). */
void audio_render(int16_t *out, size_t frames);

#endif
