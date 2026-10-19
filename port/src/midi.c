#include "midi.h"

#include <stdlib.h>
#include <string.h>

#include "../third_party/tsf.h"

#define MIDI_DRUM_CHANNEL 9
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

static void reset_channels(void)
{
	tsf_reset(synth);
	for (int ch = 0; ch < 16; ch++) {
		tsf_channel_set_presetnumber(synth, ch, 0, ch == MIDI_DRUM_CHANNEL);
		tsf_channel_set_volume(synth, ch, 100.0f / 127.0f); /* default GM CC7 */
	}
}

int midi_play(const Lb7Midi *m, int loops)
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

/* Executes a short message like midiOutShortMsg. */
static void short_msg(uint32_t ev)
{
	int st = ev & 0xFF, d1 = (ev >> 8) & 0x7F, d2 = (ev >> 16) & 0x7F, ch = st & 0x0F;
	switch (st & 0xF0) {
	case 0x80: tsf_channel_note_off(synth, ch, d1); break;
	case 0x90:
		if (d2)
			tsf_channel_note_on(synth, ch, d1, d2 / 127.0f);
		else
			tsf_channel_note_off(synth, ch, d1);
		break;
	case 0xB0: tsf_channel_midi_control(synth, ch, d1, d2); break;
	case 0xC0: tsf_channel_set_presetnumber(synth, ch, d1, ch == MIDI_DRUM_CHANNEL); break;
	case 0xE0: tsf_channel_set_pitchwheel(synth, ch, d1 | d2 << 7); break;
	default: break; /* aftertouch and system messages: ignored, as by the engine */
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
		if (LB7_MIDI_IS_DELAY(ev)) {
			uint32_t ms = LB7_MIDI_DELAY_MS(ev);
			if (ms && ms < MIDI_MIN_DELAY_MS)
				ms = MIDI_MIN_DELAY_MS;
			wait_ms1000 += (int64_t)ms * rate;
		}
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
	size_t done = 0;
	while (done < frames) {
		advance();
		size_t n = frames - done;
		if (playing && wait_ms1000 > 0) {
			/* samples until the end of the delay (rounded up) */
			int64_t need = (wait_ms1000 + 999) / 1000;
			if ((uint64_t)need < n)
				n = (size_t)need;
		}
		tsf_render_float(synth, tmp, (int)n, 0);
		for (size_t i = 0; i < n * 2; i++)
			mix[done * 2 + i] += tmp[i] * volume;
		if (playing)
			wait_ms1000 -= (int64_t)n * 1000;
		done += n;
	}
}
