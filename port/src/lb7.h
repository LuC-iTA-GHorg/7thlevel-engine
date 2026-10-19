/*
 * lb7 - reader for "7LB" archives (7th Level Resource Library 1.2) in portable C.
 * Format documented in src\docs\FORMATO_7LB.md. Shared between the port and lb7tool.
 */
#ifndef LB7_H
#define LB7_H

#include <stddef.h>
#include <stdint.h>

#define LB7_MAGIC     0x4c37 /* "7L" */
#define LB7_VERSION   2
#define LB7_HDR_MIN   0x58
#define LB7_NKEYS     12
#define LB7_ENTRY_SZ  10

/* key resource (header +0xE2 + k*8) */
enum {
	LB7_KEY_INDEX = 0, LB7_KEY_MEMDATA = 5, LB7_KEY_PALETTE = 6, LB7_KEY_NAMES = 7
};

/* known resource types (index, "type" word) */
enum {
	LB7_T_BITMAP = 1, LB7_T_STREAM = 2, LB7_T_MIDI = 4, LB7_T_WAVE = 9,
	LB7_T_LINK = 11, LB7_T_LIST = 13, LB7_T_SCRIPT = 16
};

/* First index of the game palette: Windows reserves 0..9 and 246..255 for system colors. */
#define LB7_PAL_FIRST 10

typedef struct {
	uint8_t *data;
	size_t size;
	uint16_t hdr_size;
	char title[83];
	uint32_t key_off[LB7_NKEYS], key_size[LB7_NKEYS];
	uint16_t n_memchunks;      /* +0xC2 */
	uint32_t memchunk_end[24]; /* +0x62, cumulative end of the in-memory chunks */
	uint16_t n_colors;         /* +0xC4 */
	uint16_t extra_slots;      /* +0xC8 */
	uint8_t blocked;           /* +0xD0: block-based file (0xFFC blocks + link; unsupported) */
	uint32_t n_entries;
	int bad_checksum;
	/* entries created at runtime in the extra slots (n_entries..n_entries+extra_slots-1): copies of
	   other entries, sharing their data (RESCOPYRESOURCE of TOPSYR01) */
	struct Lb7Entry *rt_slots;
} Lb7;

typedef struct Lb7Entry {
	uint16_t type;
	uint32_t off, size;
} Lb7Entry;

/* Bitmap header (type 1), 20 bytes. Bottom-up pixels, 8-bit indexed. */
typedef struct {
	uint16_t hdr_size, width, stride, height, flags;
	int16_t w5, w6;   /* +0xA, +0xC: anchor point (TOPSYS01 FUN_1000_0B2C) */
	uint16_t w7;      /* +0xE: to be analyzed (id?) */
} Lb7Bitmap;

#define LB7_BMP_RLE 0x80

/* Wave header (type 9), 28 bytes. */
typedef struct {
	uint32_t data_size;
	uint8_t adpcm;           /* +6: IMA ADPCM 4 bit */
	uint16_t channels, bits; /* from WAVEFORMAT at +0xC */
	uint32_t rate;
} Lb7Wave;

static inline uint16_t lb7_rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t lb7_rd32(const uint8_t *p) { return lb7_rd16(p) | (uint32_t)lb7_rd16(p + 2) << 16; }

/* Open: loads the whole file into memory. Returns 0 and writes the reason to err. */
int lb7_open(Lb7 *lb, const char *path, char *err, size_t errlen);
void lb7_close(Lb7 *lb);

Lb7Entry lb7_entry(const Lb7 *lb, uint32_t i);
/* RESCOPYRESOURCE: copies entry src into the first free extra slot; returns the new id or 0.
 * RESDELETERESOURCE: frees an extra slot (runtime-created entries only); 1 = done. */
uint32_t lb7_copy_entry(Lb7 *lb, uint32_t src);
int lb7_delete_entry(Lb7 *lb, uint32_t id);
/* Entry data (types 10..18 live in the KEY_MEMDATA area); NULL if outside the file. */
const uint8_t *lb7_entry_data(const Lb7 *lb, Lb7Entry e);

/* 256-color RGB palette: Windows system colors + game palette from LB7_PAL_FIRST. */
void lb7_palette(const Lb7 *lb, uint8_t rgb[256][3]);

/* Bitmap: reads the header; decodes into a stride*height buffer (bottom-up). */
int lb7_bitmap_header(const uint8_t *d, size_t size, Lb7Bitmap *b);
int lb7_bitmap_decode(const uint8_t *d, size_t size, const Lb7Bitmap *b, uint8_t *pix);

/* Codecs: chunk stream (LZW 10/11/12 bit or plain data) and row RLE. */
long lb7_chunk_decode(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstcap);
int lb7_rle_decode(const uint8_t *src, size_t srclen, uint8_t *dst, unsigned stride, unsigned height);

/* Wave: reads the header; decodes to PCM (16 bit if ADPCM). Returns the bytes produced, 0 = error.
 * lb7_wave_pcm_size gives the size of the required buffer. */
int lb7_wave_header(const uint8_t *d, size_t size, Lb7Wave *w);
size_t lb7_wave_pcm_size(const Lb7Wave *w);
size_t lb7_wave_decode(const uint8_t *d, const Lb7Wave *w, uint8_t *pcm);

/* MIDI (type 4): 6-byte header {dword size, word id}, then 4-byte records (TOPSYA01
 * FUN_1000_1BB8). Record with (high word & 0xF000) == 0: short message for midiOutShortMsg
 * (byte 0 status, 1 data, 2 data). Otherwise: pause of (record & 0x7FFFFFFF) milliseconds. */
typedef struct {
	uint32_t n_events;
	uint16_t id;
	const uint8_t *events;
} Lb7Midi;

#define LB7_MIDI_IS_DELAY(ev) (((ev) >> 16 & 0xF000) != 0)
#define LB7_MIDI_DELAY_MS(ev) ((ev) & 0x7FFFFFFFu)

int lb7_midi_header(const uint8_t *d, size_t size, Lb7Midi *m);
static inline uint32_t lb7_midi_event(const Lb7Midi *m, uint32_t i) { return lb7_rd32(m->events + i * 4); }

/* Script: instruction length in bytes, 0 = unknown opcode (Script_OpLength + check). */
unsigned lb7_op_len(const uint8_t *p, size_t avail);
/* Opcode mnemonic (src\docs\OPCODE_SCRIPT.md), NULL if not yet identified. */
const char *lb7_op_name(unsigned op);

/* Animations (type 15): command length in bytes, 0 = unknown. Table DS:0C0A of
 * TOPSYS01 (initialized by FUN_1000_46B6) + variable cases of FUN_1000_4612. Command 0x15
 * contains a script instruction (bytes +4 to +len) executed by the engine. */
#define LB7_ANIM_EVENT 0x15
unsigned lb7_anim_len(const uint8_t *p, size_t avail);

/* Movies (TOPSY.EXE 1010:06AC-1564). Entry N type 10 = movie script, N+1 type 2 = stream,
 * N+2 type 2 = index (2 bytes, then 8-byte records {type u8, flag u8, size u16,
 * offset u32}: type 2 audio, 0 key frame, 1 skippable frame).
 * Script: 01 {n, counter, ms} shows n frames; 02 {ms} wait; 03 {28-byte wave header}
 * starts the audio; 04 {sync}; 05 {script instruction}. Frames = 7LB bitmaps; flag 1: 0xFF pixels
 * take the previous frame (S_017); flag 2: drawn over the previous one with 0 transparent
 * (S_019; in Upsy all type 1 frames are like this, type 0 ones are complete). */
#define LB7_MOVIE_MAX_REC 4096
typedef struct {
	const uint8_t *script, *stream, *index;
	uint32_t script_size, stream_size;
	unsigned n_rec;
} Lb7Movie;
typedef struct {            /* current composite frame */
	Lb7Bitmap bmp;          /* width/stride/height and anchor w5/w6 */
	uint8_t *pix;           /* stride*height, bottom-up; NULL = nessuno */
} Lb7MovieFrame;
int lb7_movie_open(const Lb7 *lb, unsigned id, Lb7Movie *m);
static inline unsigned lb7_movie_rec_kind(const Lb7Movie *m, unsigned i) { return m->index[i * 8]; }
/* Complete audio (concatenated type 2 blocks) decoded to 16-bit PCM; malloc, 0 = error. */
size_t lb7_movie_audio(const Lb7Movie *m, Lb7Wave *w, uint8_t **pcm);
/* Decodes record i (type 0/1) and composes it onto f. 1 = ok. */
int lb7_movie_frame(const Lb7Movie *m, unsigned i, Lb7MovieFrame *f);
void lb7_movie_frame_free(Lb7MovieFrame *f);

#endif
