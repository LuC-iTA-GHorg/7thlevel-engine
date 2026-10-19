#include "audio.h"

#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "midi.h"

#define RENDER_CHUNK 1024 /* frames per internal pass of audio_render */

/* Wave event of TOPSYA01 (0x8E-byte struct, W+x offsets in the comments). */
typedef struct {
	int id;           /* 0 = free */
	int16_t *pcm;     /* interleaved samples */
	uint32_t frames;  /* samples per channel */
	int channels;
	uint32_t step;    /* position increment in 16.16 (source rate / AUDIO_RATE) */
	uint64_t pos;     /* position in 16.16 */
	int loops;        /* repetitions still to do (W+0x6E): 0 = ends, -1 = forever */
	int playing;      /* W+0x77 */
	int auto_free;    /* one-shot voice: freed at the end */
	uint8_t target;   /* W+0x81: volume / fade-in target */
	uint8_t level;    /* W+0x82: current level, read as signed char by the mixer */
	uint8_t fin, fout;                 /* W+0x7D / W+0x7E */
	uint32_t fin_reload, fin_cnt;      /* W+0x28 / W+0x2C */
	uint32_t fout_reload, fout_cnt;    /* W+0x30 / W+0x34 */
} Voice;

static Voice voices[AUDIO_VOICES];
static int next_id = 1;
static int master = 256;
static int wave_gain = 256;  /* wave device volume (opcode 5D) */
static int waves_paused;
static SDL_AudioDeviceID dev;

static void lock(void)
{
	if (dev)
		SDL_LockAudioDevice(dev);
}

static void unlock(void)
{
	if (dev)
		SDL_UnlockAudioDevice(dev);
}

static void SDLCALL callback(void *user, Uint8 *stream, int len)
{
	(void)user;
	audio_render((int16_t *)stream, (size_t)len / 4);
}

int audio_init(void)
{
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
		return 0;
	SDL_AudioSpec want = { 0 }, have;
	want.freq = AUDIO_RATE;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 1024;
	want.callback = callback;
	dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (!dev)
		return 0;
	SDL_PauseAudioDevice(dev, 0);
	return 1;
}

static void voice_free(Voice *v)
{
	free(v->pcm);
	memset(v, 0, sizeof(*v));
}

/* Voice of handle h (lock held), NULL if it does not exist. */
static Voice *voice(int h)
{
	for (int i = 0; h > 0 && i < AUDIO_VOICES; i++)
		if (voices[i].id == h)
			return &voices[i];
	return NULL;
}

void audio_shutdown(void)
{
	if (dev) {
		SDL_CloseAudioDevice(dev);
		dev = 0;
	}
	for (int i = 0; i < AUDIO_VOICES; i++)
		voice_free(&voices[i]);
	midi_shutdown();
}

int audio_init_midi(const char *sf2_path)
{
	lock();
	int ok = midi_init(sf2_path, AUDIO_RATE);
	unlock();
	return ok;
}

int audio_play_midi(const Lb7 *lb, uint32_t entry, int loops, unsigned mult, unsigned div, uint16_t mute)
{
	Lb7Entry e = lb7_entry(lb, entry);
	const uint8_t *d = e.type == LB7_T_MIDI ? lb7_entry_data(lb, e) : NULL;
	Lb7Midi m;
	if (!d || !lb7_midi_header(d, e.size, &m))
		return 0;
	lock();
	int ok = midi_play(&m, loops, mult, div, mute);
	unlock();
	return ok;
}

void audio_stop_midi(void)
{
	lock();
	midi_stop();
	unlock();
}

int audio_midi_playing(void)
{
	lock();
	int r = midi_playing();
	unlock();
	return r;
}

void audio_set_music_volume(int volume)
{
	lock();
	midi_set_volume(volume);
	unlock();
}

void audio_midi_params(unsigned mult, unsigned div, uint16_t mute)
{
	lock();
	midi_params(mult, div, mute);
	unlock();
}

void audio_midi_notes_off(int ch)
{
	lock();
	midi_notes_off(ch);
	unlock();
}

void audio_midi_remap_drums(int on)
{
	lock();
	midi_remap_drums(on);
	unlock();
}

int audio_wave_create_pcm(int16_t *pcm, uint32_t samples, unsigned channels, uint32_t rate)
{
	if (!pcm || !channels || !rate) {
		free(pcm);
		return 0;
	}
	Voice nv = { 0 };
	nv.pcm = pcm;
	nv.channels = (int)channels;
	nv.frames = samples / (uint32_t)channels;
	nv.step = (uint32_t)(((uint64_t)rate << 16) / AUDIO_RATE);
	nv.target = nv.level = AUDIO_LEVEL_FULL; /* Wave_InitFromHeader: +0x81 = +0x82 = 0x10 */

	lock();
	int slot = -1;
	for (int i = 0; i < AUDIO_VOICES && slot < 0; i++)
		if (!voices[i].id)
			slot = i;
	if (slot < 0) { /* all taken: the event cannot be created (A_011 returns 0) */
		unlock();
		free(pcm);
		return 0;
	}
	nv.id = next_id++;
	if (next_id <= 0)
		next_id = 1;
	voices[slot] = nv;
	unlock();
	return nv.id;
}

int audio_wave_create(const Lb7 *lb, uint32_t entry)
{
	Lb7Entry e = lb7_entry(lb, entry);
	const uint8_t *d = e.type == LB7_T_WAVE ? lb7_entry_data(lb, e) : NULL;
	Lb7Wave w;
	if (!d || !lb7_wave_header(d, e.size, &w))
		return 0;
	/* the mixer works at 16 bit: uncompressed 8-bit waves are converted */
	size_t raw = lb7_wave_pcm_size(&w);
	uint8_t *buf = malloc(raw ? raw : 1);
	if (!buf)
		return 0;
	lb7_wave_decode(d, &w, buf);
	int16_t *pcm;
	uint32_t samples;
	if (w.adpcm || w.bits == 16) {
		pcm = (int16_t *)buf;
		samples = (uint32_t)(raw / 2);
	}
	else {
		samples = (uint32_t)raw;
		pcm = malloc(samples * 2u + 2);
		if (!pcm) {
			free(buf);
			return 0;
		}
		for (uint32_t i = 0; i < samples; i++)
			pcm[i] = (int16_t)((buf[i] - 128) << 8);
		free(buf);
	}
	return audio_wave_create_pcm(pcm, samples, w.channels, w.rate);
}

void audio_wave_destroy(int h)
{
	lock();
	Voice *v = voice(h);
	if (v)
		voice_free(v);
	unlock();
}

void audio_wave_start(int h, int loops)
{
	lock();
	Voice *v = voice(h);
	if (v) {
		v->pos = 0;
		v->loops = loops;
		v->playing = 1;
	}
	unlock();
}

void audio_wave_halt(int h)
{
	lock();
	Voice *v = voice(h);
	if (v)
		v->playing = 0;
	unlock();
}

int audio_wave_active(int h)
{
	lock();
	Voice *v = voice(h);
	int r = v && v->playing;
	unlock();
	return r;
}

void audio_wave_set_level(int h, int target, int level)
{
	lock();
	Voice *v = voice(h);
	if (v) {
		v->target = (uint8_t)target;
		v->level = (uint8_t)level;
	}
	unlock();
}

int audio_wave_level(int h)
{
	lock();
	Voice *v = voice(h);
	int r = v ? v->level : 0;
	unlock();
	return r;
}

void audio_wave_fade_in(int h, uint32_t interval)
{
	lock();
	Voice *v = voice(h);
	if (v) {
		v->fin_reload = v->fin_cnt = interval;
		if (interval) {
			v->level = 0;
			v->fin = 1;
		}
	}
	unlock();
}

void audio_wave_fade_out(int h, uint32_t interval, uint32_t delay)
{
	lock();
	Voice *v = voice(h);
	if (v) {
		v->fout_reload = interval;
		v->fout_cnt = interval + delay;
		if (interval)
			v->fout = 1;
	}
	unlock();
}

void audio_wave_fades_off(int h)
{
	lock();
	Voice *v = voice(h);
	if (v)
		v->fin = v->fout = 0;
	unlock();
}

int audio_play_wave(const Lb7 *lb, uint32_t entry, int loops)
{
	int h = audio_wave_create(lb, entry);
	lock();
	Voice *v = voice(h);
	if (v)
		v->auto_free = 1;
	unlock();
	audio_wave_start(h, loops);
	return h;
}

int audio_is_playing(int h)
{
	return audio_wave_active(h);
}

void audio_stop_all(void)
{
	lock();
	for (int i = 0; i < AUDIO_VOICES; i++)
		voices[i].playing = 0;
	unlock();
}

uint32_t audio_wave_duration_ms(const Lb7 *lb, uint32_t entry)
{
	Lb7Entry e = lb7_entry(lb, entry);
	const uint8_t *d = e.type == LB7_T_WAVE ? lb7_entry_data(lb, e) : NULL;
	Lb7Wave w;
	if (!d || !lb7_wave_header(d, e.size, &w) || !w.rate || !w.channels)
		return 0;
	uint64_t samples = lb7_wave_pcm_size(&w) / (w.adpcm || w.bits == 16 ? 2u : 1u);
	return (uint32_t)(samples / w.channels * 1000u / w.rate);
}

uint32_t audio_midi_duration_ms(const Lb7 *lb, uint32_t entry, unsigned mult, unsigned div)
{
	Lb7Entry e = lb7_entry(lb, entry);
	const uint8_t *d = e.type == LB7_T_MIDI ? lb7_entry_data(lb, e) : NULL;
	Lb7Midi m;
	if (!d || !lb7_midi_header(d, e.size, &m))
		return 0;
	uint64_t ms = 0;
	for (uint32_t i = 0; i < m.n_events; i++) { /* same delays as the midi.c sequencer */
		uint32_t ev = lb7_midi_event(&m, i);
		if (LB7_MIDI_IS_DELAY(ev))
			ms += midi_delay_ms(ev, mult, div);
	}
	return ms > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)ms;
}

void audio_set_wave_volume(int volume)
{
	lock();
	wave_gain = volume < 0 ? 0 : volume > 256 ? 256 : volume;
	unlock();
}

void audio_pause_waves(int on)
{
	lock();
	waves_paused = on;
	unlock();
}

void audio_pause_midi(int on)
{
	lock();
	midi_pause(on);
	unlock();
}

void audio_set_master(int volume)
{
	lock();
	master = volume < 0 ? 0 : volume > 256 ? 256 : volume;
	unlock();
}

static void render_chunk(int16_t *out, size_t frames);

/* Called with the device lock already held (SDL callback) or without a device (tests). */
void audio_render(int16_t *out, size_t frames)
{
	while (frames) {
		size_t n = frames > RENDER_CHUNK ? RENDER_CHUNK : frames;
		render_chunk(out, n);
		out += n * 2;
		frames -= n;
	}
}

/* Fade counters of the DLL mixer (FUN_1000_11FD): once per sample value consumed. The fade-out
 * counts only while no fade-in is active; at level 0 it stops and the voice goes on silent. */
static void fade_ticks(Voice *v, uint32_t n)
{
	while (n-- && (v->fin || v->fout)) {
		if (v->fin) {
			if (--v->fin_cnt == 0) {
				v->fin_cnt = v->fin_reload;
				v->level++;
				if (v->target <= v->level)
					v->fin = 0;
			}
		}
		else if (--v->fout_cnt == 0) {
			v->fout_cnt = v->fout_reload;
			if (--v->level == 0)
				v->fout = 0;
		}
	}
}

static void render_chunk(int16_t *out, size_t frames)
{
	int32_t acc[RENDER_CHUNK * 2];
	float music[RENDER_CHUNK * 2];
	memset(acc, 0, frames * 2 * sizeof(int32_t));
	memset(music, 0, frames * 2 * sizeof(float));
	midi_render(music, frames); /* paused: the sequencer is held, release tails still render */
	for (size_t k = 0; k < frames * 2; k++)
		acc[k] = (int32_t)(music[k] * (32767.0f * master / 256.0f));
	for (int vi = 0; vi < AUDIO_VOICES; vi++) {
		Voice *v = &voices[vi];
		if (!v->id)
			continue;
		if (!v->playing) {
			if (v->auto_free)
				voice_free(v);
			continue;
		}
		if (waves_paused)
			continue;
		int vol = master * wave_gain; /* 0..65536 */
		for (size_t f = 0; f < frames; f++) {
			uint32_t i = (uint32_t)(v->pos >> 16);
			if (i >= v->frames) {
				if (!v->loops) {
					/* data finished: FUN_1000_2B16 -> 3D9E */
					v->playing = 0;
					break;
				}
				if (v->loops > 0)
					v->loops--;
				v->pos -= (uint64_t)v->frames << 16;
				i = (uint32_t)(v->pos >> 16);
			}
			uint32_t j = i + 1 < v->frames ? i + 1 : (v->loops ? 0 : i);
			/* 15-bit fraction: (16-bit difference) * fraction stays within int32 */
			int32_t fr = (int32_t)((v->pos & 0xFFFF) >> 1);
			int32_t l, r;
			if (v->channels == 2) {
				l = v->pcm[i * 2] + (((v->pcm[j * 2] - v->pcm[i * 2]) * fr) >> 15);
				r = v->pcm[i * 2 + 1] + (((v->pcm[j * 2 + 1] - v->pcm[i * 2 + 1]) * fr) >> 15);
			}
			else {
				l = r = v->pcm[i] + (((v->pcm[j] - v->pcm[i]) * fr) >> 15);
			}
			if (v->level != AUDIO_LEVEL_FULL) { /* FUN_1000_11FD: (sample * (char)level) >> 4 */
				l = l * (int8_t)v->level >> 4;
				r = r * (int8_t)v->level >> 4;
			}
			acc[f * 2] += (int32_t)(((int64_t)l * vol) >> 16);
			acc[f * 2 + 1] += (int32_t)(((int64_t)r * vol) >> 16);
			uint64_t old = v->pos;
			v->pos += v->step;
			if (v->fin || v->fout)
				fade_ticks(v, (uint32_t)((v->pos >> 16) - (old >> 16)) * (uint32_t)v->channels);
		}
		if (!v->loops && (v->pos >> 16) >= v->frames)
			v->playing = 0;
		if (!v->playing && v->auto_free)
			voice_free(v);
	}
	for (size_t k = 0; k < frames * 2; k++) {
		int32_t s = acc[k];
		out[k] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
	}
}
