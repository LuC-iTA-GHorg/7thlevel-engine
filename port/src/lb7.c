/*
 * lb7 - reader for "7LB" archives (7th Level Resource Library 1.2).
 * Reconstructed from TOPSYR01.DLL (RESOPENFILE, RESLOADRESOURCE, Chunk_Decode, Bitmap_ExpandRLE)
 * and TOPSYA01.DLL (Ima_DecodeTo16). See src\docs\FORMATO_7LB.md.
 */
#include "lb7.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define rd16 lb7_rd16
#define rd32 lb7_rd32

/* ---------------------------------------------------------------- archive */

int lb7_open(Lb7 *lb, const char *path, char *err, size_t errlen)
{
	memset(lb, 0, sizeof(*lb));
	FILE *f = fopen(path, "rb");
	if (!f) {
		snprintf(err, errlen, "%s: cannot open the file", path);
		return 0;
	}
	fseek(f, 0, SEEK_END);
	lb->size = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	lb->data = malloc(lb->size ? lb->size : 1);
	if (!lb->data || fread(lb->data, 1, lb->size, f) != lb->size) {
		fclose(f);
		lb7_close(lb);
		snprintf(err, errlen, "%s: read error", path);
		return 0;
	}
	fclose(f);

	const uint8_t *h = lb->data;
	if (lb->size < LB7_HDR_MIN || rd16(h) != LB7_MAGIC || rd16(h + 0x56) != LB7_VERSION) {
		lb7_close(lb);
		snprintf(err, errlen, "%s: not a 7LB v2 archive", path);
		return 0;
	}
	lb->hdr_size = rd16(h + 2);
	if (lb->hdr_size < 0xE2 + LB7_NKEYS * 8 || lb->hdr_size > lb->size) {
		lb7_close(lb);
		snprintf(err, errlen, "%s: 7LB header too short (%u)", path, lb->hdr_size);
		return 0;
	}
	uint8_t sum = 0;
	for (unsigned i = 0; i < lb->hdr_size; i++)
		sum += h[i];
	lb->bad_checksum = sum != 0;

	memcpy(lb->title, h + 4, 82);
	for (int k = 0; k < LB7_NKEYS; k++) {
		lb->key_off[k] = rd32(h + 0xE2 + k * 8);
		lb->key_size[k] = rd32(h + 0xE6 + k * 8);
	}
	lb->n_memchunks = rd16(h + 0xC2);
	for (int i = 0; i < lb->n_memchunks && i < 24; i++)
		lb->memchunk_end[i] = rd32(h + 0x62 + i * 4);
	lb->n_colors = rd16(h + 0xC4);
	lb->extra_slots = rd16(h + 0xC8);
	lb->blocked = h[0xD0];
	if ((uint64_t)lb->key_off[LB7_KEY_INDEX] + lb->key_size[LB7_KEY_INDEX] > lb->size) {
		lb7_close(lb);
		snprintf(err, errlen, "%s: index outside the file", path);
		return 0;
	}
	lb->n_entries = lb->key_size[LB7_KEY_INDEX] / LB7_ENTRY_SZ;
	return 1;
}

void lb7_close(Lb7 *lb)
{
	free(lb->data);
	free(lb->rt_slots);
	lb->data = NULL;
	lb->rt_slots = NULL;
	lb->size = 0;
	lb->n_entries = 0;
}

Lb7Entry lb7_entry(const Lb7 *lb, uint32_t i)
{
	Lb7Entry e = { 0, 0, 0 };
	if (i >= lb->n_entries) {
		if (lb->rt_slots && i - lb->n_entries < lb->extra_slots)
			e = lb->rt_slots[i - lb->n_entries];
		return e;
	}
	const uint8_t *p = lb->data + lb->key_off[LB7_KEY_INDEX] + i * LB7_ENTRY_SZ;
	e.type = rd16(p);
	e.off = rd32(p + 2);
	e.size = rd32(p + 6);
	return e;
}

/* TOPSYR01 1000:0C06: first free slot starting from the end of the file's index. The data is not
 * duplicated: in the port the resources are read-only and the modifiable state of the
 * copies (position, click script...) lives in the runtime. */
uint32_t lb7_copy_entry(Lb7 *lb, uint32_t src)
{
	Lb7Entry e = lb7_entry(lb, src);
	if (!e.type || !lb->extra_slots)
		return 0;
	if (!lb->rt_slots && !(lb->rt_slots = calloc(lb->extra_slots, sizeof(Lb7Entry))))
		return 0;
	for (uint32_t k = 0; k < lb->extra_slots; k++)
		if (!lb->rt_slots[k].type) {
			lb->rt_slots[k] = e;
			return lb->n_entries + k;
		}
	return 0;
}

/* TOPSYR01 1000:0D5C */
int lb7_delete_entry(Lb7 *lb, uint32_t id)
{
	if (!lb->rt_slots || id < lb->n_entries || id - lb->n_entries >= lb->extra_slots)
		return 0;
	memset(&lb->rt_slots[id - lb->n_entries], 0, sizeof(Lb7Entry));
	return 1;
}

const uint8_t *lb7_entry_data(const Lb7 *lb, Lb7Entry e)
{
	uint64_t base = (e.type >= 10 && e.type <= 18) ? lb->key_off[LB7_KEY_MEMDATA] : 0;
	if (base + e.off + e.size > lb->size)
		return NULL;
	return lb->data + base + e.off;
}

/* Windows static colors (indices 0..9 and 246..255 of the 256-color system palette). */
static const uint8_t win_static[20][3] = {
	{ 0, 0, 0 }, { 128, 0, 0 }, { 0, 128, 0 }, { 128, 128, 0 }, { 0, 0, 128 },
	{ 128, 0, 128 }, { 0, 128, 128 }, { 192, 192, 192 }, { 192, 220, 192 }, { 166, 202, 240 },
	{ 255, 251, 240 }, { 160, 160, 164 }, { 128, 128, 128 }, { 255, 0, 0 }, { 0, 255, 0 },
	{ 255, 255, 0 }, { 0, 0, 255 }, { 255, 0, 255 }, { 0, 255, 255 }, { 255, 255, 255 }
};

void lb7_palette(const Lb7 *lb, uint8_t rgb[256][3])
{
	memset(rgb, 0, 256 * 3);
	for (int i = 0; i < 10; i++) {
		memcpy(rgb[i], win_static[i], 3);
		memcpy(rgb[246 + i], win_static[10 + i], 3);
	}
	uint32_t n = lb->key_size[LB7_KEY_PALETTE] / 4;
	if (!n || (uint64_t)lb->key_off[LB7_KEY_PALETTE] + n * 4 > lb->size)
		return;
	if (n > 256 - LB7_PAL_FIRST)
		n = 256 - LB7_PAL_FIRST;
	const uint8_t *p = lb->data + lb->key_off[LB7_KEY_PALETTE];
	for (uint32_t i = 0; i < n; i++)
		memcpy(rgb[LB7_PAL_FIRST + i], p + i * 4, 3); /* PALETTEENTRY: R, G, B, flag */
}

/* ---------------------------------------------------------------- codec */

/* Fixed-width LZW, MSB-first, codes < 256 are literals, end = (1<<bits)-1. */
static long lzw_decode(const uint8_t *src, size_t srclen, int bits, uint8_t *dst, size_t dstcap)
{
	enum { MAXC = 1 << 12 };
	uint16_t prefix[MAXC];
	uint8_t suffix[MAXC];
	uint8_t stack[MAXC + 1];
	const unsigned endcode = (1u << bits) - 1, maxcode = (1u << bits) - 2;
	unsigned next = 256, prev = 0xffff;
	uint32_t acc = 0;
	int nacc = 0;
	size_t si = 0, di = 0;

	for (unsigned c = 0; c < 256; c++)
		suffix[c] = (uint8_t)c;
	for (;;) {
		while (nacc < bits) {
			if (si >= srclen)
				return -1; /* input ended without an end code */
			acc = (acc << 8) | src[si++];
			nacc += 8;
		}
		unsigned code = (acc >> (nacc - bits)) & endcode;
		nacc -= bits;
		if (code == endcode)
			return (long)di;
		if (prev == 0xffff) {
			if (code > 255 || di >= dstcap)
				return -1;
			dst[di++] = (uint8_t)code;
			prev = code;
			continue;
		}
		unsigned cur = code, n = 0;
		int kwk = 0;
		if (code >= next) { /* KwKwK case */
			cur = prev;
			kwk = 1;
		}
		while (cur >= 256) {
			stack[n++] = suffix[cur];
			cur = prefix[cur];
		}
		stack[n++] = (uint8_t)cur;
		uint8_t fc = (uint8_t)cur;
		if (di + n + (unsigned)kwk > dstcap)
			return -1;
		while (n)
			dst[di++] = stack[--n];
		if (kwk)
			dst[di++] = fc;
		if (next <= maxcode) {
			prefix[next] = (uint16_t)prev;
			suffix[next] = fc;
			next++;
		}
		prev = code;
	}
}

long lb7_chunk_decode(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstcap)
{
	size_t si = 0, di = 0;
	while (si + 2 <= srclen) {
		uint16_t w = rd16(src + si);
		unsigned kind = w & 0xE000, n = w & 0x1FFF;
		si += 2;
		if (kind == 0xE000)
			return (long)di;
		if (si + n > srclen)
			return -1;
		if (kind == 0) {
			if (di + n > dstcap)
				return -1;
			memcpy(dst + di, src + si, n);
			di += n;
		}
		else {
			int bits = kind == 0x4000 ? 10 : kind == 0x6000 ? 11 : kind == 0x8000 ? 12 : 0;
			if (!bits)
				return -1;
			/* the chunk contains sub-blocks [word len][len bytes], fresh dictionary for each */
			size_t bi = si, bend = si + n;
			while (bi + 2 <= bend) {
				unsigned bl = rd16(src + bi);
				bi += 2;
				if (bi + bl > bend)
					return -1;
				long got = lzw_decode(src + bi, bl, bits, dst + di, dstcap - di);
				if (got <= 0)
					return -1;
				di += (size_t)got;
				bi += bl;
			}
		}
		si += n;
	}
	return (long)di;
}

int lb7_rle_decode(const uint8_t *src, size_t srclen, uint8_t *dst, unsigned stride, unsigned height)
{
	size_t si = 0;
	for (unsigned y = 0; y < height; y++) {
		uint8_t *row = dst + (size_t)y * stride, *end = row + stride;
		for (;;) {
			if (si >= srclen)
				return 0;
			uint8_t b = src[si++];
			if (b == 0)
				break;
			if (b < 0x80) { /* run */
				if (si >= srclen || row + b > end)
					return 0;
				memset(row, src[si++], b);
				row += b;
			}
			else { /* literals: ~b bytes, or count in the next byte if b = 0xFF */
				unsigned n = (uint8_t)~b;
				if (b == 0xFF) {
					if (si >= srclen)
						return 0;
					n = src[si++];
				}
				if (si + n > srclen || row + n > end)
					return 0;
				memcpy(row, src + si, n);
				row += n;
				si += n;
			}
		}
	}
	return 1;
}

/* ---------------------------------------------------------------- bitmap */

int lb7_bitmap_header(const uint8_t *d, size_t size, Lb7Bitmap *b)
{
	if (size < 20)
		return 0;
	b->hdr_size = rd16(d);
	b->width = rd16(d + 2);
	b->stride = rd16(d + 4);
	b->height = rd16(d + 6);
	b->flags = rd16(d + 8);
	b->w5 = (int16_t)rd16(d + 10);
	b->w6 = (int16_t)rd16(d + 12);
	b->w7 = rd16(d + 14);
	return b->hdr_size >= 20 && b->hdr_size <= size && b->width && b->height && b->stride >= b->width;
}

int lb7_bitmap_decode(const uint8_t *d, size_t size, const Lb7Bitmap *b, uint8_t *pix)
{
	size_t npix = (size_t)b->stride * b->height;
	memset(pix, 0, npix);
	if (b->flags & LB7_BMP_RLE) {
		size_t skip = b->hdr_size + b->height * 2u; /* table of row offsets */
		if (skip > size)
			return 0;
		return lb7_rle_decode(d + skip, size - skip, pix, b->stride, b->height);
	}
	return lb7_chunk_decode(d + b->hdr_size, size - b->hdr_size, pix, npix) == (long)npix;
}

/* ---------------------------------------------------------------- audio */

/* Standard IMA ADPCM (tables identical to TOPSYA01 DS:01E8/DS:0778), low nibble first. */
static const int16_t ima_step[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
	66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371,
	408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
	2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845,
	8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
	29794, 32767
};
static const int8_t ima_index[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

typedef struct { int pred, index; } ImaState;

static int16_t ima_nibble(ImaState *s, unsigned n)
{
	int step = ima_step[s->index];
	int d = step >> 3;
	if (n & 4) d += step;
	if (n & 2) d += step >> 1;
	if (n & 1) d += step >> 2;
	s->pred += (n & 8) ? -d : d;
	if (s->pred > 32767) s->pred = 32767;
	if (s->pred < -32768) s->pred = -32768;
	s->index += ima_index[n & 7];
	if (s->index < 0) s->index = 0;
	if (s->index > 88) s->index = 88;
	return (int16_t)s->pred;
}

int lb7_wave_header(const uint8_t *d, size_t size, Lb7Wave *w)
{
	if (size < 28)
		return 0;
	w->data_size = rd32(d);
	w->adpcm = d[6];
	w->channels = rd16(d + 14);
	w->rate = rd32(d + 16);
	w->bits = rd16(d + 26);
	return w->data_size <= size - 28 && w->channels >= 1 && w->channels <= 2 && w->rate;
}

size_t lb7_wave_pcm_size(const Lb7Wave *w)
{
	return w->adpcm ? (size_t)w->data_size * 4 : w->data_size;
}

size_t lb7_wave_decode(const uint8_t *d, const Lb7Wave *w, uint8_t *pcm)
{
	const uint8_t *src = d + 28;
	if (!w->adpcm) {
		memcpy(pcm, src, w->data_size);
		return w->data_size;
	}
	/* each byte = 2 16-bit samples (mono) or 1 sample per channel (stereo) */
	ImaState st[2] = { { 0, 0 }, { 0, 0 } };
	for (uint32_t i = 0; i < w->data_size; i++) {
		int16_t a = ima_nibble(&st[0], src[i] & 15);
		int16_t b = ima_nibble(&st[w->channels - 1], src[i] >> 4);
		memcpy(pcm + i * 4, &a, 2);
		memcpy(pcm + i * 4 + 2, &b, 2);
	}
	return (size_t)w->data_size * 4;
}

/* ---------------------------------------------------------------- MIDI */

int lb7_midi_header(const uint8_t *d, size_t size, Lb7Midi *m)
{
	if (size < 6)
		return 0;
	uint32_t n = rd32(d);
	if (n > size - 6 || n % 4)
		return 0;
	m->n_events = n / 4;
	m->id = rd16(d + 4);
	m->events = d + 6;
	return 1;
}

/* ---------------------------------------------------------------- script */

unsigned lb7_anim_len(const uint8_t *p, size_t avail)
{
	static const uint8_t len[0x18] = {
		0, 10, 0, 2, 2, 30, 8, 10, 2, 24, 8, 6, 8, 4, 4, 4, 8, 6, 4, 4, 2, 0, 18, 12 };
	if (avail < 2)
		return 0;
	unsigned op = rd16(p);
	if (op == 0x02) /* (n + 4) words: list of n values */
		return avail < 8 ? 0 : (rd16(p + 6) + 4u) * 2;
	if (op == LB7_ANIM_EVENT)
		return avail < 4 ? 0 : rd16(p + 2);
	return op < 0x18 ? len[op] : 0;
}

/* Entries with '?' are guesses inferred from the APIs called by the handler. */
const char *lb7_op_name(unsigned op)
{
	switch (op) {
	case 0x01: return "call";        case 0x02: return "load";        case 0x03: return "unload";
	case 0x05: return "show?";       case 0x07: return "getmouse?";   case 0x0d: return "quit";
	case 0x12: return "link";        case 0x16: return "runrandom";   case 0x17: return "runnext";
	case 0x1b: return "debug";       case 0x1f: return "cursor";     case 0x22: return "runindex";
	case 0x25: return "goto";        case 0x26: return "jumpifnot";   case 0x27: return "fillrect?";
	case 0x2b: return "switchvar";   case 0x2c: return "end";         case 0x2d: return "nop";
	case 0x35: return "volume?";     case 0x38: return "sprintf";     case 0x39: return "strcmp";
	case 0x3a: return "strcmpi";     case 0x3b: return "iniread";     case 0x3c: return "iniwrite";
	case 0x41: return "cos";         case 0x42: return "sin";         case 0x43: return "iniwrite?";
	case 0x48: return "settable";    case 0x49: return "setmouse";    case 0x4c: return "expr";
	case 0x4d: return "if";          case 0x4e: return "switch";      case 0x4f: return "while";
	case 0x59: return "return";      case 0x60: return "palette?";    case 0x61: return "palette?";
	case 0x71: return "case";        case 0x77: return "setstring";   case 0x8f: return "nop6";
	default: return NULL;
	}
}

unsigned lb7_op_len(const uint8_t *p, size_t avail)
{
	if (avail < 2)
		return 0;
	switch (rd16(p)) {
	case 0x01: return avail < 6 ? 0 : (p[5] + 3u) * 2;
	case 0x02: case 0x03: case 0x09: case 0x0a: case 0x0b: case 0x0e: case 0x15: case 0x1d:
	case 0x1f: case 0x23: case 0x25: case 0x35: case 0x40: case 0x43: case 0x51: case 0x5d:
	case 0x60: case 0x6a: return 4;
	case 0x04: case 0x12: case 0x32: return 20;
	case 0x05: case 0x0f: case 0x1e: case 0x24: case 0x27: case 0x28: case 0x29: case 0x2e:
	case 0x34: case 0x39: case 0x3a: case 0x3f: case 0x55: return 8;
	case 0x06: case 0x52: case 0x56: return 16;
	case 0x07: case 0x22: case 0x2a: case 0x30: case 0x3b: case 0x3c: case 0x46: case 0x50:
	case 0x53: case 0x58: return 10;
	case 0x08: return 28;
	case 0x0c: case 0x13: case 0x18: case 0x31: case 0x36: case 0x37: case 0x61: return 12;
	case 0x0d: case 0x2c: case 0x2d: case 0x45: return 2;
	case 0x10: case 0x4b: return 24;
	case 0x11: case 0x5c: return 14;
	case 0x14: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x20: case 0x2f: case 0x41:
	case 0x42: case 0x44: case 0x47: case 0x48: case 0x49: case 0x54: case 0x57: case 0x5e:
	case 0x8d: case 0x8f: return 6;
	case 0x16: case 0x17: case 0x21: case 0x77: return 18;
	case 0x38: return 32;
	case 0x3d: case 0x3e: return 22;
	case 0x4c: case 0x59:                            /* explicit length at +2 */
		return avail < 4 ? 0 : rd16(p + 2);
	/* if (0x4D) and while (0x4F): +2 jump if false, +4 start of the body (= header length).
	 * Note: 0x4F is not in Script_OpLength; the layout is that of its handler (1008:A846). */
	case 0x4d: case 0x4f: return avail < 6 ? 0 : rd16(p + 4);
	case 0x4e: return avail < 4 ? 0 : rd16(p + 2); /* switch: +2 header */
	case 0x71: return 6;                           /* case table entry */
	case 0x26: case 0x2b: return 10;               /* conditional jump, switch on variable */
	default: return 0;
	}
}

/* ---------------------------------------------------------------- movies */

int lb7_movie_open(const Lb7 *lb, unsigned id, Lb7Movie *m)
{
	memset(m, 0, sizeof(*m));
	Lb7Entry es = lb7_entry(lb, id), ev = lb7_entry(lb, id + 1), ei = lb7_entry(lb, id + 2);
	if (es.type != 10 || ev.type != LB7_T_STREAM || ei.type != LB7_T_STREAM || ei.size < 10)
		return 0;
	m->script = lb7_entry_data(lb, es);
	m->stream = lb7_entry_data(lb, ev);
	m->index = lb7_entry_data(lb, ei);
	if (!m->script || !m->stream || !m->index)
		return 0;
	m->index += 2; /* FUN_1010_0B00: (size - 2) / 8 records */
	m->script_size = es.size;
	m->stream_size = ev.size;
	m->n_rec = (ei.size - 2) / 8;
	for (unsigned i = 0; i < m->n_rec; i++) /* record outside the stream: index truncated there */
		if (rd32(m->index + i * 8 + 4) + rd16(m->index + i * 8 + 2) > m->stream_size) {
			m->n_rec = i;
			break;
		}
	return m->n_rec > 0;
}

size_t lb7_movie_audio(const Lb7Movie *m, Lb7Wave *w, uint8_t **pcm)
{
	*pcm = NULL;
	/* wave header in command 03 (FUN_1010_0D12: A_011 with the pointer to the command + 2) */
	const uint8_t *hdr = NULL;
	for (uint32_t pc = 0; pc + 2 <= m->script_size && !hdr;) {
		unsigned op = rd16(m->script + pc);
		if (op == 3 && pc + 30 <= m->script_size)
			hdr = m->script + pc + 2;
		else if (op == 1)
			pc += 8;
		else if (op == 2 || op == 4)
			pc += 4;
		else
			break;
	}
	if (!hdr)
		return 0;
	w->adpcm = hdr[6];
	w->channels = rd16(hdr + 14);
	w->rate = rd32(hdr + 16);
	w->bits = rd16(hdr + 26);
	if (w->channels < 1 || w->channels > 2 || !w->rate)
		return 0;
	uint32_t total = 0;
	for (unsigned i = 0; i < m->n_rec; i++)
		if (lb7_movie_rec_kind(m, i) == 2)
			total += rd16(m->index + i * 8 + 2);
	w->data_size = total;
	uint8_t *raw = malloc(total ? total : 1);
	if (!raw)
		return 0;
	uint32_t at = 0;
	for (unsigned i = 0; i < m->n_rec; i++)
		if (lb7_movie_rec_kind(m, i) == 2) {
			uint16_t n = rd16(m->index + i * 8 + 2);
			memcpy(raw + at, m->stream + rd32(m->index + i * 8 + 4), n);
			at += n;
		}
	/* same format as the body of a type 9 wave: decode with a fake 28-byte header */
	size_t out = lb7_wave_pcm_size(w);
	*pcm = malloc(out ? out : 1);
	uint8_t *tmp = malloc((size_t)total + 28);
	if (!*pcm || !tmp) {
		free(raw);
		free(tmp);
		free(*pcm);
		*pcm = NULL;
		return 0;
	}
	memcpy(tmp + 28, raw, total);
	free(raw);
	out = lb7_wave_decode(tmp, w, *pcm);
	free(tmp);
	return out;
}

/* Rectangle of a bitmap in centered coordinates (TOPSY 1010:08C4), top to bottom. */
static void movie_rect(const Lb7Bitmap *b, int *l, int *t)
{
	*l = (1 - (int)b->width) / 2 + b->w5;
	*t = -(((int)b->height - 1) / 2 - b->w6);
}

int lb7_movie_frame(const Lb7Movie *m, unsigned i, Lb7MovieFrame *f)
{
	if (i >= m->n_rec || lb7_movie_rec_kind(m, i) == 2)
		return 0;
	const uint8_t *d = m->stream + rd32(m->index + i * 8 + 4);
	uint32_t size = rd16(m->index + i * 8 + 2);
	Lb7Bitmap nb;
	if (!lb7_bitmap_header(d, size, &nb))
		return 0;
	uint8_t *np = malloc((size_t)nb.stride * nb.height);
	if (!np || !lb7_bitmap_decode(d, size, &nb, np)) {
		free(np);
		return 0;
	}
	int nl, nt, ol = 0, ot = 0;
	movie_rect(&nb, &nl, &nt);
	if (f->pix)
		movie_rect(&f->bmp, &ol, &ot);
	if ((nb.flags & 2) && f->pix) { /* S_019 -> FUN_1000_2890: over the current one, 0 transparent */
		for (unsigned y = 0; y < nb.height; y++) {
			int oy = nt + (int)y - ot;
			if (oy < 0 || oy >= f->bmp.height)
				continue;
			const uint8_t *src = np + (size_t)(nb.height - 1 - y) * nb.stride;
			uint8_t *dst = f->pix + (size_t)(f->bmp.height - 1 - oy) * f->bmp.stride;
			for (unsigned x = 0; x < nb.width; x++) {
				int ox = nl + (int)x - ol;
				if (src[x] && ox >= 0 && ox < f->bmp.width)
					dst[ox] = src[x];
			}
		}
		free(np);
		return 1;
	}
	if ((nb.flags & 1) && f->pix) /* S_017: 0xFF = pixel unchanged */
		for (unsigned y = 0; y < nb.height; y++) {
			int oy = nt + (int)y - ot;
			if (oy < 0 || oy >= f->bmp.height)
				continue;
			uint8_t *dst = np + (size_t)(nb.height - 1 - y) * nb.stride;
			const uint8_t *src = f->pix + (size_t)(f->bmp.height - 1 - oy) * f->bmp.stride;
			for (unsigned x = 0; x < nb.width; x++) {
				int ox = nl + (int)x - ol;
				if (dst[x] == 0xFF && ox >= 0 && ox < f->bmp.width)
					dst[x] = src[ox];
			}
		}
	free(f->pix);
	f->pix = np;
	f->bmp = nb;
	return 1;
}

void lb7_movie_frame_free(Lb7MovieFrame *f)
{
	free(f->pix);
	f->pix = NULL;
}
