/*
 * midi - sequencer for 7th Level MIDI records (type 4) + TinySoundFont synthesizer.
 * Replaces midiOut and the timer of TOPSYA01.DLL (FUN_1000_1BB8): records are executed
 * with sample accuracy inside the mixer, instead of with timeSetEvent.
 * The control functions must be called with the audio lock held (audio.c handles it).
 */
#ifndef MIDI_H
#define MIDI_H

#include <stddef.h>
#include <stdint.h>

#include "lb7.h"

/* Minimum delay between records (non-zero delays shorter than that become 3 ms, like the original). */
#define MIDI_MIN_DELAY_MS 3

/* Loads the SoundFont (.sf2). Returns 0 if the file is missing or invalid. */
int midi_init(const char *sf2_path, int sample_rate);
void midi_shutdown(void);
int midi_available(void);

/* Starts the sequence; loops: 0 = once, n = repeat n times, -1 = forever. */
int midi_play(const Lb7Midi *m, int loops);
void midi_stop(void);
int midi_playing(void);
void midi_set_volume(int volume); /* 0..256 */

/* Adds frames stereo samples (float, about -1..1) to the mixer. */
void midi_render(float *mix, size_t frames);

#endif
