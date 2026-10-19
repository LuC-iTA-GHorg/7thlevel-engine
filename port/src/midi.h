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

/* Starts the sequence; loops: 0 = once, n = repeat n times, -1 = forever; mult/div/mute:
 * parameters of opcode 5C (A_032) kept by the MIDI event (div 0 = tempo unscaled). */
int midi_play(const Lb7Midi *m, int loops, unsigned mult, unsigned div, uint16_t mute);
void midi_stop(void);
int midi_playing(void);
void midi_set_volume(int volume); /* 0..256 */
void midi_params(unsigned mult, unsigned div, uint16_t mute); /* current sequence (A_032) */
void midi_notes_off(int ch);      /* CC 0x7B on one channel (FUN_1000_1868) */
void midi_remap_drums(int on);    /* RemapMidiDrums: channel 10 -> 16 (FUN_1000_1BB8) */
/* A_025: pause = All Notes Off on the 16 channels (FUN_1000_1840) and sequencer held; the
 * synthesizer keeps rendering the release tails. Resume = the sequence continues at once. */
void midi_pause(int on);

/* Delay record -> ms after the tempo scaling of opcode 5C and the 3 ms minimum (0 = no wait). */
uint32_t midi_delay_ms(uint32_t ev, unsigned mult, unsigned div);

/* Adds frames stereo samples (float, about -1..1) to the mixer. */
void midi_render(float *mix, size_t frames);

#endif
