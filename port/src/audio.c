#include "audio.h"

#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "midi.h"

#define RENDER_CHUNK 1024 /* frames per internal pass of audio_render */

typedef struct {
	int id;           /* 0 = free */
	int16_t *pcm;     /* interleaved samples */
	uint32_t frames;  /* samples per channel */
	int channels;
	uint32_t step;    /* position increment in 16.16 (source rate / AUDIO_RATE) */
	uint64_t pos;     /* position in 16.16 */
	int loops;        /* repetitions still to do: 0 = ends, -1 = forever */
	int volume;
} Voice;

static Voice voices[AUDIO_VOICES];
static int next_id = 1;
static int master = 256;
static int wave_gain = 256;  /* wave device volume (opcode 5D) */
static int waves_paused, midi_paused;
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

int audio_play_midi(const Lb7 *lb, uint32_t entry, int loops)
{
	Lb7Entry e = lb7_entry(lb, entry);
	const uint8_t *d = e.type == LB7_T_MIDI ? lb7_entry_data(lb, e) : NULL;
	Lb7Midi m;
	if (!d || !lb7_midi_header(d, e.size, &m))
		return 0;
	lock();
	int ok = midi_play(&m, loops);
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

int audio_play_wave(const Lb7 *lb, uint32_t entry, int loops, int volume)
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
	return audio_play_pcm(pcm, samples, w.channels, w.rate, loops, volume);
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

uint32_t audio_midi_duration_ms(const Lb7 *lb, uint32_t entry)
{
	Lb7Entry e = lb7_entry(lb, entry);
	const uint8_t *d = e.type == LB7_T_MIDI ? lb7_entry_data(lb, e) : NULL;
	Lb7Midi m;
	if (!d || !lb7_midi_header(d, e.size, &m))
		return 0;
	uint64_t ms = 0;
	for (uint32_t i = 0; i < m.n_events; i++) { /* same delays as the midi.c sequencer */
		uint32_t ev = lb7_midi_event(&m, i);
		if (LB7_MIDI_IS_DELAY(ev)) {
			uint32_t t = LB7_MIDI_DELAY_MS(ev);
			ms += t && t < MIDI_MIN_DELAY_MS ? MIDI_MIN_DELAY_MS : t;
		}
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
	midi_paused = on;
	unlock();
}

int audio_play_pcm(int16_t *pcm, uint32_t samples, unsigned channels, uint32_t rate, int loops, int volume)
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
	nv.loops = loops;
	nv.volume = volume < 0 ? 0 : volume > 256 ? 256 : volume;

	lock();
	int slot = -1;
	for (int i = 0; i < AUDIO_VOICES && slot < 0; i++)
		if (!voices[i].id)
			slot = i;
	if (slot < 0)
		slot = 0; /* all busy: replaces the first */
	free(voices[slot].pcm);
	nv.id = next_id++;
	if (next_id <= 0)
		next_id = 1;
	voices[slot] = nv;
	unlock();
	return nv.id;
}

void audio_stop(int id)
{
	lock();
	for (int i = 0; i < AUDIO_VOICES; i++)
		if (voices[i].id == id)
			voice_free(&voices[i]);
	unlock();
}

void audio_stop_all(void)
{
	lock();
	for (int i = 0; i < AUDIO_VOICES; i++)
		voice_free(&voices[i]);
	unlock();
}

int audio_is_playing(int id)
{
	int r = 0;
	lock();
	for (int i = 0; i < AUDIO_VOICES; i++)
		if (voices[i].id == id)
			r = 1;
	unlock();
	return r;
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

static void render_chunk(int16_t *out, size_t frames)
{
	int32_t acc[RENDER_CHUNK * 2];
	float music[RENDER_CHUNK * 2];
	memset(acc, 0, frames * 2 * sizeof(int32_t));
	memset(music, 0, frames * 2 * sizeof(float));
	if (!midi_paused)
		midi_render(music, frames);
	for (size_t k = 0; k < frames * 2; k++)
		acc[k] = (int32_t)(music[k] * (32767.0f * master / 256.0f));
	for (int vi = 0; vi < AUDIO_VOICES; vi++) {
		Voice *v = &voices[vi];
		if (!v->id || waves_paused)
			continue;
		int vol = (int)((int64_t)v->volume * master * wave_gain >> 8);
		for (size_t f = 0; f < frames; f++) {
			uint32_t i = (uint32_t)(v->pos >> 16);
			if (i >= v->frames) {
				if (!v->loops) {
					/* finished voices are freed after the loop */
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
			acc[f * 2] += (int32_t)(((int64_t)l * vol) >> 16);
			acc[f * 2 + 1] += (int32_t)(((int64_t)r * vol) >> 16);
			v->pos += v->step;
		}
		if (!v->loops && (v->pos >> 16) >= v->frames)
			voice_free(v);
	}
	for (size_t k = 0; k < frames * 2; k++) {
		int32_t s = acc[k];
		out[k] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
	}
}
