#include "midi.h"

#include <stdlib.h>
#include <string.h>

#include "../third_party/tsf.h"

#define MIDI_DRUM_CHANNEL 9
#define MIDI_REMAP_CHANNEL 15 /* RemapMidiDrums: status | 0xF (TOPSYA01 FUN_1000_1BB8, DAT_07B8) */
#define MIDI_MAX_VOICES   64
/* Like the original engine (FUN_1000_1BB8): non-zero delays under 3 ms become 3 ms.
 * Lengthens the tracks by about 1% (TOPSMAIN #8988: 72.60 s -> 73.47 s), faithful to the original.
 * MIDI_MIN_DELAY_MS lives in midi.h (it is also needed for the duration computation in audio.c). */
#define MIDI_GAIN_DB      -3.0f

static tsf *synth;
static int rate;
static uint32_t *events;   /* copy of the records of the current sequence */
static uint32_t n_events, cur;
static int64_t wait_ms1000; /* remaining wait in samples*1000; the negative remainder carries over, so there is no drift */
static int loops_left;
static int playing;
static int held;            /* A_025 pause: the sequencer does not advance */
static int remap;           /* RemapMidiDrums */
static unsigned t_mult, t_div; /* opcode 5C: W+0x22 / W+0x24 of the MIDI event */
static uint16_t muted;      /* opcode 5C: W+0x30.. (one byte per channel in the original) */
static float volume = 1.0f;
static float *tmp;          /* synthesizer render buffer */
static size_t tmp_frames;

int midi_init(const char *sf2_path, int sample_rate)
{
	midi_shutdown();
	synth = tsf_load_filename(sf2_path);
	if (!synth)
		return 0;
	rate = sample_rate;
	/* -3 dB: at 0 dB the densest tracks (e.g. TOPSMAIN #8988) exceed full scale */
	tsf_set_output(synth, TSF_STEREO_INTERLEAVED, rate, MIDI_GAIN_DB);
	tsf_set_max_voices(synth, MIDI_MAX_VOICES); /* no allocations in the audio thread */
	return 1;
}

void midi_shutdown(void)
{
	if (synth)
		tsf_close(synth);
	synth = NULL;
	free(events);
	events = NULL;
	free(tmp);
	tmp = NULL;
	tmp_frames = 0;
	n_events = 0;
	playing = 0;
}

int midi_available(void)
{
	return synth != NULL;
}

/* Percussion channel of the SoundFont: channel 10, or 16 when the drums are remapped there
 * (the remap was meant for base-level synthesizers whose percussion is on channel 16). */
static int is_drum(int ch)
{
	return ch == (remap ? MIDI_REMAP_CHANNEL : MIDI_DRUM_CHANNEL);
}

static void reset_channels(void)
{
	tsf_reset(synth);
	for (int ch = 0; ch < 16; ch++) {
		tsf_channel_set_presetnumber(synth, ch, 0, is_drum(ch));
		tsf_channel_set_volume(synth, ch, 100.0f / 127.0f); /* default GM CC7 */
	}
}

int midi_play(const Lb7Midi *m, int loops, unsigned mult, unsigned div, uint16_t mute)
{
	if (!synth || !m->n_events)
		return 0;
	uint32_t *ev = malloc(m->n_events * sizeof(uint32_t));
	if (!ev)
		return 0;
	for (uint32_t i = 0; i < m->n_events; i++)
		ev[i] = lb7_midi_event(m, i);
	free(events);
	events = ev;
	n_events = m->n_events;
	cur = 0;
	wait_ms1000 = 0;
	loops_left = loops;
	t_mult = mult;
	t_div = div;
	muted = mute;
	reset_channels();
	playing = 1;
	return 1;
}

void midi_stop(void)
{
	playing = 0;
	if (synth)
		tsf_note_off_all(synth);
}

int midi_playing(void)
{
	return playing;
}

void midi_set_volume(int v)
{
	volume = (v < 0 ? 0 : v > 256 ? 256 : v) / 256.0f;
}

void midi_params(unsigned mult, unsigned div, uint16_t mute)
{
	t_mult = mult;
	t_div = div;
	muted = mute;
}

void midi_notes_off(int ch)
{
	if (synth)
		tsf_channel_midi_control(synth, ch & 15, 123, 0);
}

void midi_remap_drums(int on)
{
	remap = on != 0;
}

void midi_pause(int on)
{
	if (on && !held && synth)
		tsf_note_off_all(synth); /* FUN_1000_1840: CC 0x7B on the 16 channels + midiOutReset */
	if (!on && held)
		wait_ms1000 = 0; /* FUN_1000_1802(0): the timer was killed, FUN_1BB8 goes on at once */
	held = on != 0;
}

uint32_t midi_delay_ms(uint32_t ev, unsigned mult, unsigned div)
{
	uint32_t ms = LB7_MIDI_DELAY_MS(ev);
	if (div) /* FUN_1000_43DE (32-bit product) and FUN_1000_4410 (unsigned division) */
		ms = (uint32_t)(ms * (uint32_t)(mult & 0xFFFF)) / (div & 0xFFFF);
	if (ms && ms < MIDI_MIN_DELAY_MS)
		ms = MIDI_MIN_DELAY_MS;
	return ms;
}

/* Executes a short message like midiOutShortMsg. */
static void short_msg(uint32_t ev)
{
	int st = ev & 0xFF, d1 = (ev >> 8) & 0x7F, d2 = (ev >> 16) & 0x7F;
	if ((st & 0x0F) == MIDI_DRUM_CHANNEL && remap)
		st |= 0x0F;
	int ch = st & 0x0F;
	if ((st & 0xF0) == 0x90 && (muted >> ch & 1))
		return; /* opcode 5C: note-on of a muted channel dropped (after the remap, as in 1BB8) */
	switch (st & 0xF0) {
	case 0x80: tsf_channel_note_off(synth, ch, d1); break;
	case 0x90:
		if (d2)
			tsf_channel_note_on(synth, ch, d1, d2 / 127.0f);
		else
			tsf_channel_note_off(synth, ch, d1);
		break;
	case 0xB0: tsf_channel_midi_control(synth, ch, d1, d2); break;
	case 0xC0: tsf_channel_set_presetnumber(synth, ch, d1, is_drum(ch)); break;
	case 0xE0: tsf_channel_set_pitchwheel(synth, ch, d1 | d2 << 7); break;
	default: break; /* aftertouch and system messages: TinySoundFont has no equivalent */
	}
}

/* Advances the sequence up to the next delay; at the end of the sequence it repeats or stops. */
static void advance(void)
{
	while (playing && wait_ms1000 <= 0) {
		if (cur >= n_events) {
			if (loops_left == 0) {
				playing = 0;
				tsf_note_off_all(synth);
				return;
			}
			if (loops_left > 0)
				loops_left--;
			cur = 0;
			continue;
		}
		uint32_t ev = events[cur++];
		if (LB7_MIDI_IS_DELAY(ev))
			wait_ms1000 += (int64_t)midi_delay_ms(ev, t_mult, t_div) * rate;
		else
			short_msg(ev);
	}
}

void midi_render(float *mix, size_t frames)
{
	if (!synth)
		return;
	if (tmp_frames < frames) {
		/* first call or larger buffer: the only out-of-steady-state allocation */
		free(tmp);
		tmp = malloc(frames * 2 * sizeof(float));
		tmp_frames = tmp ? frames : 0;
		if (!tmp)
			return;
	}
	int run = playing && !held;
	size_t done = 0;
	while (done < frames) {
		if (run)
			advance();
		size_t n = frames - done;
		if (run && playing && wait_ms1000 > 0) {
			/* samples until the end of the delay (rounded up) */
			int64_t need = (wait_ms1000 + 999) / 1000;
			if ((uint64_t)need < n)
				n = (size_t)need;
		}
		tsf_render_float(synth, tmp, (int)n, 0);
		for (size_t i = 0; i < n * 2; i++)
			mix[done * 2 + i] += tmp[i] * volume;
		if (run && playing)
			wait_ms1000 -= (int64_t)n * 1000;
		done += n;
	}
}
