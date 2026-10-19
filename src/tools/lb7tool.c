/*
 * lb7tool - command-line tool for "7LB" archives (7th Level Resource Library 1.2).
 * The format is implemented in port\src\lb7.c (shared with the SDL2 port).
 *
 *   lb7tool info    <file.bin>             header, key resources, per-type statistics
 *   lb7tool list    <file.bin>             all entries with their first bytes
 *   lb7tool disasm  <file.bin> [n|all]     verify / disassemble the scripts (type 16)
 *   lb7tool extract <file.bin> <folder>    extracts the resources (bitmap -> .bmp, wave -> .wav)
 *   lb7tool icon    <file.exe> <file.ico>  icon of a 16-bit Windows EXE (NE), for build.bat
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir_p(d) _mkdir(d)
#else
#include <sys/stat.h>
#define mkdir_p(d) mkdir(d, 0755)
#endif

#include "../../port/src/lb7.h"

#define rd16 lb7_rd16
#define rd32 lb7_rd32
#define op_len lb7_op_len

/* ---------------------------------------------------------------- output files */

static int write_wave(const char *path, const uint8_t *d, uint32_t size)
{
	Lb7Wave w;
	if (!lb7_wave_header(d, size, &w))
		return 0;
	size_t pcmlen = lb7_wave_pcm_size(&w);
	uint8_t *pcm = malloc(pcmlen ? pcmlen : 1);
	if (!pcm)
		return 0;
	lb7_wave_decode(d, &w, pcm);
	unsigned bits = w.adpcm ? 16 : w.bits;
	FILE *f = fopen(path, "wb");
	if (!f) {
		free(pcm);
		return 0;
	}
	uint8_t h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0 };
	unsigned align = w.channels * bits / 8;
#define W16(o, v) do { h[o] = (uint8_t)(v); h[o+1] = (uint8_t)((v) >> 8); } while (0)
#define W32(o, v) do { W16(o, v); W16(o + 2, (v) >> 16); } while (0)
	W32(4, 36 + (uint32_t)pcmlen); W16(22, w.channels); W32(24, w.rate); W32(28, w.rate * align);
	W16(32, align); W16(34, bits); memcpy(h + 36, "data", 4); W32(40, (uint32_t)pcmlen);
	fwrite(h, 1, 44, f);
	fwrite(pcm, 1, pcmlen, f);
	fclose(f);
	free(pcm);
	return 1;
}

static void put_vlq(FILE *f, uint32_t v)
{
	uint8_t b[5];
	int n = 0;
	b[n++] = v & 0x7F;
	while (v >>= 7)
		b[n++] = (uint8_t)(0x80 | (v & 0x7F));
	while (n)
		fputc(b[--n], f);
}

/* 7th Level MIDI -> Standard MIDI File type 0: 1 tick = 1 ms (500 ticks/quarter note at 120 BPM). */
static int write_midi(const char *path, const uint8_t *d, uint32_t size, uint32_t *ms_out)
{
	Lb7Midi m;
	if (!lb7_midi_header(d, size, &m))
		return 0;
	FILE *f = fopen(path, "wb");
	if (!f)
		return 0;
	static const uint8_t mthd[] = { 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xF4 };
	fwrite(mthd, 1, sizeof(mthd), f);
	fwrite("MTrk\0\0\0\0", 1, 8, f);
	long start = ftell(f);
	static const uint8_t tempo[] = { 0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20 }; /* 500000 us */
	fwrite(tempo, 1, sizeof(tempo), f);
	uint32_t delta = 0, total = 0;
	for (uint32_t i = 0; i < m.n_events; i++) {
		uint32_t ev = lb7_midi_event(&m, i);
		if (LB7_MIDI_IS_DELAY(ev)) {
			delta += LB7_MIDI_DELAY_MS(ev);
			total += LB7_MIDI_DELAY_MS(ev);
			continue;
		}
		uint8_t st = ev & 0xFF;
		if (st < 0x80 || st >= 0xF0)
			continue; /* not a channel message */
		put_vlq(f, delta);
		delta = 0;
		fputc(st, f);
		fputc((ev >> 8) & 0x7F, f);
		if ((st & 0xE0) != 0xC0) /* program change and channel pressure have only 1 data byte */
			fputc((ev >> 16) & 0x7F, f);
	}
	put_vlq(f, delta);
	fwrite("\xFF\x2F\x00", 1, 3, f);
	long end = ftell(f);
	uint32_t len = (uint32_t)(end - start);
	uint8_t lb[4] = { (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len };
	fseek(f, start - 4, SEEK_SET);
	fwrite(lb, 1, 4, f);
	fclose(f);
	if (ms_out)
		*ms_out = total;
	return 1;
}

static void write_bmp(const char *path, const uint8_t *pix, unsigned w, unsigned stride,
	unsigned h, uint8_t rgb[256][3])
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	unsigned rowsz = (w + 3) & ~3u;
	uint32_t off = 14 + 40 + 1024, sz = off + rowsz * h;
	uint8_t hd[54] = { 'B', 'M' };
#define P32(o, v) do { hd[o] = (uint8_t)(v); hd[o+1] = (uint8_t)((v) >> 8); \
	hd[o+2] = (uint8_t)((v) >> 16); hd[o+3] = (uint8_t)((v) >> 24); } while (0)
	P32(2, sz); P32(10, off); P32(14, 40); P32(18, w); P32(22, h);
	hd[26] = 1; hd[28] = 8; P32(46, 256);
	fwrite(hd, 1, 54, f);
	for (unsigned i = 0; i < 256; i++) {
		uint8_t q[4] = { rgb[i][2], rgb[i][1], rgb[i][0], 0 };
		fwrite(q, 1, 4, f);
	}
	static const uint8_t zero[4];
	/* 7LB pixels are already bottom-up like in Windows DIBs */
	for (unsigned y = 0; y < h; y++) {
		fwrite(pix + (size_t)y * stride, 1, w, f);
		fwrite(zero, 1, rowsz - w, f);
	}
	fclose(f);
}

/* ---------------------------------------------------------------- scripts (type 16) */

#define op_name lb7_op_name

static void print_var(FILE *out, unsigned v)
{
	if (v < 0x13fe)
		fprintf(out, "g%u", v);
	else
		fprintf(out, "l%u", v - 0x13fe);
}

/* Encoded operand of a statement: global/local variable or constant. */
static void print_operand(FILE *out, unsigned v)
{
	if (v < 0x159f)
		print_var(out, v);
	else
		fprintf(out, "%d", (int)(int16_t)(v - 0x8acf));
}

/* RPN expression (Script_EvalExpr, TOPSY.EXE 1008:2464), terminated by 0x00. */
static void print_expr(FILE *out, const uint8_t *e, size_t avail)
{
	static const char *bin[0x22] = {
		[0x12] = "+", [0x13] = "-", [0x14] = "*", [0x15] = "/", [0x16] = "%", [0x17] = "==",
		[0x18] = "<=", [0x19] = "<", [0x1a] = ">=", [0x1b] = ">", [0x1c] = "!=", [0x1d] = "and",
		[0x1e] = "or", [0x1f] = "&", [0x20] = "|", [0x21] = "^" };
	size_t i = 0;
	fprintf(out, "[");
	while (i < avail && e[i]) {
		unsigned t = e[i];
		unsigned w = i + 3 <= avail ? rd16(e + i + 1) : 0;
		switch (t) {
		case 0x01: fprintf(out, " %d", (int)(int16_t)w); i += 3; continue;
		case 0x02: fprintf(out, " "); print_var(out, w); i += 3; continue;
		case 0x03: fprintf(out, " []"); break;
		case 0x04: fprintf(out, " &"); print_var(out, w); i += 3; continue;
		case 0x05: fprintf(out, " deref"); break;
		case 0x06: fprintf(out, " &[]"); break;
		case 0x08: fprintf(out, " ="); break;
		/* builtin id = first value pushed, then the arguments; +3 = no. of arguments */
		case 0x0d: fprintf(out, " builtin/%u", i + 5 <= avail ? rd16(e + i + 3) : 0); i += 5; continue;
		case 0x0e: fprintf(out, " callscript/%u", i + 5 <= avail ? rd16(e + i + 3) : 0); i += 5; continue;
		case 0x0f: fprintf(out, " neg"); break;
		case 0x10: fprintf(out, " !"); break;
		case 0x11: fprintf(out, " ~"); break;
		case 0x22: fprintf(out, " ++pre"); break;
		case 0x23: fprintf(out, " post++"); break;
		case 0x24: fprintf(out, " --pre"); break;
		case 0x25: fprintf(out, " post--"); break;
		case 0x26: fprintf(out, " ||>%u", w); i += 3; continue;
		case 0x27: fprintf(out, " &&>%u", w); i += 3; continue;
		case 0x28: fprintf(out, " dup*"); break;
		default:
			if (t < 0x22 && bin[t])
				fprintf(out, " %s", bin[t]);
			else
				fprintf(out, " ?%02x", t);
		}
		i++;
	}
	fprintf(out, " ]");
}

static void print_insn(FILE *out, const uint8_t *p, unsigned n)
{
	unsigned op = rd16(p);
	const char *nm = op_name(op);
	if (nm)
		fprintf(out, "%-10s", nm);
	else
		fprintf(out, "op_%02x     ", op);
	switch (op) {
	case 0x4c: case 0x59: print_expr(out, p + 4, n - 4); return;
	case 0x4d: case 0x4f:
		fprintf(out, "else->+%u body->+%u ", rd16(p + 2), rd16(p + 4));
		print_expr(out, p + 6, n - 6);
		return;
	case 0x4e:
		fprintf(out, "table->+%u default->+%u n=%u ", rd16(p + 4), rd16(p + 6), p[8]);
		print_expr(out, p + 10, n - 10);
		return;
	case 0x25: fprintf(out, "+%d", (int)(int16_t)rd16(p + 2)); return;
	case 0x26:
		fprintf(out, "->+%u  ", rd16(p + 2));
		print_operand(out, rd16(p + 4));
		fprintf(out, " op%u ", p[8]);
		print_operand(out, rd16(p + 6));
		return;
	case 0x01:
		fprintf(out, "script ");
		print_operand(out, rd16(p + 2));
		fprintf(out, " frame=%u args:", p[4]);
		for (unsigned k = 0; k < p[5]; k++) {
			fprintf(out, " ");
			print_operand(out, rd16(p + 6 + k * 2));
		}
		return;
	case 0x71:
		fprintf(out, "%d -> +%u", (int)(int16_t)rd16(p + 2), rd16(p + 4));
		return;
	case 0x77:
		fprintf(out, "\"%.16s\"", (const char *)p + 2);
		return;
	}
	for (unsigned j = 2; j + 1 < n; j += 2)
		fprintf(out, " %04x", rd16(p + j));
}

/* Walks the script; returns 1 if it ends exactly at the end of the resource. */
static int script_walk(const uint8_t *d, size_t size, FILE *out, unsigned *hist)
{
	size_t pc = 0;
	while (pc < size) {
		unsigned n = op_len(d + pc, size - pc);
		if (!n || pc + n > size) {
			if (out)
				fprintf(out, "  %04zx: ?? opcode %04x\n", pc, size - pc >= 2 ? rd16(d + pc) : 0);
			return 0;
		}
		if (hist)
			hist[rd16(d + pc) & 0xff]++;
		if (out) {
			fprintf(out, "  %04zx: ", pc);
			print_insn(out, d + pc, n);
			fprintf(out, "\n");
		}
		pc += n;
	}
	return 1;
}


/* ---------------------------------------------------------------- commands */

static void cmd_info(const Lb7 *lb, const char *path)
{
	printf("%s: %zu bytes, header %u, title \"%s\"\n", path, lb->size, lb->hdr_size, lb->title);
	printf("palette colors %u, extra slots %u, blocked %u, in-memory chunks %u\n",
		lb->n_colors, lb->extra_slots, lb->blocked, lb->n_memchunks);
	if (lb->bad_checksum)
		printf("  WARNING: bad header checksum\n");
	for (int k = 0; k < LB7_NKEYS; k++)
		if (lb->key_size[k])
			printf("  key %2d: offset 0x%08x size %u\n", k, lb->key_off[k], lb->key_size[k]);
	unsigned hist[256] = { 0 };
	printf("  %u entries in the index\n", lb->n_entries);
	for (uint32_t i = 0; i < lb->n_entries; i++) {
		Lb7Entry e = lb7_entry(lb, i);
		if (e.type < 256)
			hist[e.type]++;
	}
	for (int t = 0; t < 256; t++)
		if (hist[t])
			printf("  type %3d: %u\n", t, hist[t]);
}

static void cmd_list(const Lb7 *lb)
{
	for (uint32_t i = 0; i < lb->n_entries; i++) {
		Lb7Entry e = lb7_entry(lb, i);
		const uint8_t *d = e.type ? lb7_entry_data(lb, e) : NULL;
		printf("%5u type %3u off 0x%08x size %8u", i, e.type, e.off, e.size);
		if (d) {
			printf("  ");
			for (unsigned j = 0; j < 20 && j < e.size; j++)
				printf("%02x ", d[j]);
		}
		printf("\n");
	}
}

static void cmd_extract(const Lb7 *lb, const char *outdir)
{
	char path[1024];
	uint8_t rgb[256][3];
	unsigned nbmp = 0, nbad = 0, nraw = 0, nwav = 0, nmid = 0;
	mkdir_p(outdir);
	lb7_palette(lb, rgb);

	for (int k = 1; k < LB7_NKEYS; k++) {
		if (!lb->key_size[k] || (uint64_t)lb->key_off[k] + lb->key_size[k] > lb->size)
			continue;
		snprintf(path, sizeof(path), "%s/key%02d.bin", outdir, k);
		FILE *f = fopen(path, "wb");
		if (f) {
			fwrite(lb->data + lb->key_off[k], 1, lb->key_size[k], f);
			fclose(f);
		}
	}
	for (uint32_t i = 0; i < lb->n_entries; i++) {
		Lb7Entry e = lb7_entry(lb, i);
		const uint8_t *d = e.type ? lb7_entry_data(lb, e) : NULL;
		if (!d || !e.size)
			continue;
		if (e.type == LB7_T_BITMAP) {
			Lb7Bitmap b;
			uint8_t *pix = NULL;
			int ok = lb7_bitmap_header(d, e.size, &b);
			if (ok) {
				pix = malloc((size_t)b.stride * b.height);
				ok = pix && lb7_bitmap_decode(d, e.size, &b, pix);
			}
			if (ok) {
				snprintf(path, sizeof(path), "%s/%05u_bmp_%ux%u.bmp", outdir, i, b.width, b.height);
				write_bmp(path, pix, b.width, b.stride, b.height, rgb);
				nbmp++;
			}
			else {
				fprintf(stderr, "bitmap %u: decoding failed (flags %04x %ux%u stride %u)\n",
					i, b.flags, b.width, b.height, b.stride);
				nbad++;
			}
			free(pix);
			continue;
		}
		if (e.type == LB7_T_WAVE) {
			snprintf(path, sizeof(path), "%s/%05u_wave.wav", outdir, i);
			if (write_wave(path, d, e.size)) {
				nwav++;
				continue;
			}
			fprintf(stderr, "wave %u: unrecognized header\n", i);
		}
		if (e.type == LB7_T_MIDI) {
			uint32_t ms = 0;
			snprintf(path, sizeof(path), "%s/%05u_midi.mid", outdir, i);
			if (write_midi(path, d, e.size, &ms)) {
				printf("  midi %u: %.1f s\n", i, ms / 1000.0);
				nmid++;
				continue;
			}
			fprintf(stderr, "midi %u: unrecognized header\n", i);
		}
		snprintf(path, sizeof(path), "%s/%05u_t%u.bin", outdir, i, e.type);
		FILE *f = fopen(path, "wb");
		if (f) {
			fwrite(d, 1, e.size, f);
			fclose(f);
			nraw++;
		}
	}
	printf("extracted %u bitmaps (%u failed), %u waves, %u midis, %u raw resources -> %s\n", nbmp,
		nbad, nwav, nmid, nraw, outdir);
}

static void cmd_disasm(const Lb7 *lb, int argc, char **argv)
{
	/* disasm <file>          verify and statistics
	 * disasm <file> <n>      print script n
	 * disasm <file> all      print all scripts */
	unsigned ok = 0, bad = 0, hist[256] = { 0 };
	int all = argc > 3 && !strcmp(argv[3], "all");
	long only = argc > 3 && !all ? atol(argv[3]) : -1;
	int print = all || only >= 0;
	for (uint32_t i = 0; i < lb->n_entries; i++) {
		Lb7Entry e = lb7_entry(lb, i);
		const uint8_t *d = e.type == LB7_T_SCRIPT ? lb7_entry_data(lb, e) : NULL;
		if (!d || (only >= 0 && (long)i != only))
			continue;
		if (print)
			printf("\nscript %u (%u bytes)\n", i, e.size);
		if (script_walk(d, e.size, print ? stdout : NULL, hist))
			ok++;
		else {
			bad++;
			if (bad <= 5 && !print) {
				printf("script %u misaligned:\n", i);
				script_walk(d, e.size, stdout, NULL);
			}
		}
	}
	printf("scripts: %u aligned, %u misaligned\nopcodes used:", ok, bad);
	for (int k = 0; k < 256; k++)
		if (hist[k])
			printf(" %02x:%u", k, hist[k]);
	printf("\n");
}

/* anim <file> [n|all]: verify/print the animations (type 15). For events (0x15) it checks that
 * the contained script instruction has exactly the length of the space it occupies. */
static void cmd_anim(const Lb7 *lb, int argc, char **argv)
{
	unsigned ok = 0, bad = 0, ev = 0, evbad = 0, hist[0x20] = { 0 }, evop[256] = { 0 };
	int all = argc > 3 && !strcmp(argv[3], "all");
	long only = argc > 3 && !all ? atol(argv[3]) : -1;
	int print = all || only >= 0;
	for (uint32_t i = 0; i < lb->n_entries; i++) {
		Lb7Entry e = lb7_entry(lb, i);
		const uint8_t *d = e.type == 15 ? lb7_entry_data(lb, e) : NULL;
		if (!d || (only >= 0 && (long)i != only))
			continue;
		if (print)
			printf("\nanimation %u (%u bytes)\n", i, e.size);
		size_t pc = 0;
		int good = 1;
		while (pc < e.size) {
			unsigned op = rd16(d + pc), n = lb7_anim_len(d + pc, e.size - pc);
			if (!n || pc + n > e.size) {
				if (print || bad < 5)
					printf("  anim %u @%04zx: command %04x misaligned\n", i, pc, op);
				good = 0;
				break;
			}
			hist[op & 0x1F]++;
			if (print) {
				printf("  %04zx: cmd %02x", pc, op);
				for (unsigned j = 2; j + 1 < n; j += 2)
					printf(" %04x", rd16(d + pc + j));
				printf("\n");
			}
			if (op == LB7_ANIM_EVENT) {
				unsigned sl = n >= 6 ? op_len(d + pc + 4, n - 4) : 0;
				ev++;
				evop[rd16(d + pc + 4) & 0xFF]++;
				if (sl != n - 4) {
					evbad++;
					if (evbad <= 5)
						printf("  anim %u @%04zx: event with instruction %04x of %u bytes in %u\n", i, pc,
							rd16(d + pc + 4), sl, n - 4);
				}
				else if (print) {
					printf("        ");
					print_insn(stdout, d + pc + 4, sl);
					printf("\n");
				}
			}
			pc += n;
		}
		good ? ok++ : bad++;
	}
	printf("animations: %u aligned, %u misaligned; events %u (%u with invalid instruction)\n"
		"commands used:", ok, bad, ev, evbad);
	for (int k = 0; k < 0x20; k++)
		if (hist[k])
			printf(" %02x:%u", k, hist[k]);
	printf("\ninstructions in events:");
	for (int k = 0; k < 256; k++)
		if (evop[k])
			printf(" %02x:%u", k, evop[k]);
	printf("\n");
}

/* movie <n> <folder> [every]: movie script, composed frames (.bmp, one every "every")
   and full audio (.wav) */
static void cmd_movie(Lb7 *lb, int argc, char **argv)
{
	unsigned id = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 0) : 0, every = 1;
	const char *dir = argc > 4 ? argv[4] : ".";
	if (argc > 5)
		every = (unsigned)strtoul(argv[5], NULL, 0);
	if (!every)
		every = 1;
	Lb7Movie m;
	if (!lb7_movie_open(lb, id, &m)) {
		printf("entry %u: not a movie (type 10 + stream + index)\n", id);
		return;
	}
	mkdir_p(dir);
	unsigned kinds[3] = { 0 };
	for (unsigned i = 0; i < m.n_rec; i++)
		if (lb7_movie_rec_kind(&m, i) < 3)
			kinds[lb7_movie_rec_kind(&m, i)]++;
	printf("movie %u: %u records (key frames %u, skippable %u, audio %u)\nscript:", id,
		m.n_rec, kinds[0], kinds[1], kinds[2]);
	uint32_t total_ms = 0, frames = 0;
	for (uint32_t pc = 0; pc + 2 <= m.script_size;) {
		unsigned op = rd16(m.script + pc);
		if (op == 1 && pc + 8 <= m.script_size) {
			printf(" [%u x %u ms]", rd16(m.script + pc + 2), rd16(m.script + pc + 6));
			total_ms += rd16(m.script + pc + 2) * rd16(m.script + pc + 6);
			frames += rd16(m.script + pc + 2);
			pc += 8;
		}
		else if (op == 2 && pc + 4 <= m.script_size) {
			printf(" [wait %u ms]", rd16(m.script + pc + 2));
			total_ms += rd16(m.script + pc + 2);
			pc += 4;
		}
		else if (op == 3) { printf(" [audio]"); pc += 30; }
		else if (op == 4) { printf(" [sync %u]", rd16(m.script + pc + 2)); pc += 4; }
		else { printf(" [?? %04x a +%u]", op, pc); break; }
	}
	printf("\nframes in script %u, duration %u ms\n", frames, total_ms);
	uint8_t rgb[256][3];
	lb7_palette(lb, rgb);
	Lb7MovieFrame f = { 0 };
	unsigned n = 0, bad = 0;
	char path[1024];
	for (unsigned i = 0; i < m.n_rec; i++) {
		if (lb7_movie_rec_kind(&m, i) == 2)
			continue;
		if (getenv("LB7_MOVIE_VERBOSE")) { /* type, flags and special pixels of the raw frame */
			const uint8_t *cd = m.stream + rd32(m.index + i * 8 + 4);
			Lb7Bitmap rb;
			if (lb7_bitmap_header(cd, rd16(m.index + i * 8 + 2), &rb)) {
				uint8_t *rp = malloc((size_t)rb.stride * rb.height);
				unsigned c0 = 0, cff = 0;
				if (rp && lb7_bitmap_decode(cd, rd16(m.index + i * 8 + 2), &rb, rp))
					for (size_t k = 0; k < (size_t)rb.stride * rb.height; k++) {
						c0 += rp[k] == 0;
						cff += rp[k] == 0xFF;
					}
				printf("  rec %u type %u: %ux%u flags %04x add %d,%d  px0 %u pxFF %u\n", i,
					lb7_movie_rec_kind(&m, i), rb.width, rb.height, rb.flags, rb.w5, rb.w6, c0, cff);
				free(rp);
			}
		}
		if (!lb7_movie_frame(&m, i, &f)) {
			bad++;
			continue;
		}
		if (n % every == 0) {
			snprintf(path, sizeof(path), "%s/frame_%04u_%ux%u.bmp", dir, n, f.bmp.width, f.bmp.height);
			write_bmp(path, f.pix, f.bmp.width, f.bmp.stride, f.bmp.height, rgb);
		}
		n++;
	}
	lb7_movie_frame_free(&f);
	printf("frames decoded %u, failed %u\n", n, bad);
	Lb7Wave w;
	uint8_t *pcm;
	size_t len = lb7_movie_audio(&m, &w, &pcm);
	if (len) {
		snprintf(path, sizeof(path), "%s/audio.wav", dir);
		FILE *fw = fopen(path, "wb");
		if (fw) {
			unsigned bits = w.adpcm ? 16 : w.bits, align = w.channels * bits / 8;
			uint8_t h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0 };
			W32(4, 36 + (uint32_t)len); W16(22, w.channels); W32(24, w.rate); W32(28, w.rate * align);
			W16(32, align); W16(34, bits); memcpy(h + 36, "data", 4); W32(40, (uint32_t)len);
			fwrite(h, 1, 44, fw);
			fwrite(pcm, 1, len, fw);
			fclose(fw);
		}
		printf("audio: %u Hz, %u channels, %s, %.2f s\n", w.rate, w.channels, w.adpcm ? "IMA ADPCM" : "PCM",
			(double)len / (w.rate * w.channels * 2));
		free(pcm);
	}
}

/* hex <n> [byte]: hex dump of the data of an entry (also those of the key 5 area) */
static void cmd_hex(Lb7 *lb, int argc, char **argv)
{
	unsigned n = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 0) : 0;
	Lb7Entry e = lb7_entry(lb, n);
	const uint8_t *d = lb7_entry_data(lb, e);
	uint32_t max = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 0) : 256;
	printf("entry %u type %u size %u\n", n, e.type, e.size);
	if (!d)
		return;
	for (uint32_t i = 0; i < e.size && i < max; i += 16) {
		printf("%06x:", i);
		for (uint32_t j = i; j < i + 16 && j < e.size && j < max; j++)
			printf(" %02x", d[j]);
		printf("\n");
	}
}

/* First icon (RT_GROUP_ICON 0x800E with its RT_ICON 0x8003) of an NE EXE into an .ico file.
 * build.bat uses it to give upsy.exe the icon of TOPSY.EXE without keeping it in the repository. */
static uint8_t ne_buf[1 << 20];

static int cmd_icon(const char *exe, const char *ico)
{
	FILE *f = fopen(exe, "rb");
	if (!f)
		return 0;
	const uint8_t *d = ne_buf;
	size_t n = fread(ne_buf, 1, sizeof(ne_buf), f);
	fclose(f);
#define W(o) ((size_t)(o) + 2 <= n ? (unsigned)(d[o] | d[(o) + 1] << 8) : 0u)
	unsigned ne = W(0x3C) | W(0x3E) << 16;
	if (n < 0x40 || ne + 0x40 > n || d[ne] != 'N' || d[ne + 1] != 'E')
		return 0;
	/* resource table: alignment, then per type: type, count, 4 bytes and 12-byte entries
	   (offset, length, flags, id, 4 bytes), offset and length in units of 1 << alignment */
	unsigned rt = ne + W(ne + 0x24), shift = W(rt), grp = 0, grp_len = 0;
	for (unsigned p = rt + 2; p + 8 <= n && W(p) && !grp; ) {
		unsigned type = W(p), cnt = W(p + 2);
		p += 8;
		for (unsigned i = 0; i < cnt; i++, p += 12)
			if (type == 0x800E && !grp) {
				grp = W(p) << shift;
				grp_len = W(p + 2) << shift;
			}
	}
	if (!grp || grp + 6 > n)
		return 0;
	unsigned count = W(grp + 4), off = 6 + count * 16;
	uint8_t hdr[6 + 16 * 16];
	const uint8_t *img[16];
	unsigned len[16];
	if (count == 0 || count > 16 || 6 + count * 14 > grp_len)
		return 0;
	memcpy(hdr, d + grp, 6);
	for (unsigned k = 0; k < count; k++) {
		const uint8_t *e = d + grp + 6 + k * 14; /* GRPICONDIRENTRY: 12 bytes as in the .ico + id */
		unsigned id = (unsigned)(e[12] | e[13] << 8) | 0x8000, found = 0;
		for (unsigned p = rt + 2; p + 8 <= n && W(p) && !found; ) {
			unsigned type = W(p), cnt = W(p + 2);
			p += 8;
			for (unsigned i = 0; i < cnt; i++, p += 12)
				if (type == 0x8003 && W(p + 6) == id && !found) {
					img[k] = d + (W(p) << shift);
					found = 1;
				}
		}
		len[k] = (unsigned)(e[8] | e[9] << 8 | e[10] << 16 | (unsigned)e[11] << 24);
		if (!found || (size_t)(img[k] - d) + len[k] > n)
			return 0;
		memcpy(hdr + 6 + k * 16, e, 12);
		uint8_t *o = hdr + 6 + k * 16 + 12;
		o[0] = (uint8_t)off;
		o[1] = (uint8_t)(off >> 8);
		o[2] = (uint8_t)(off >> 16);
		o[3] = (uint8_t)(off >> 24);
		off += len[k];
	}
#undef W
	f = fopen(ico, "wb");
	if (!f)
		return 0;
	fwrite(hdr, 1, 6 + count * 16, f);
	for (unsigned k = 0; k < count; k++)
		fwrite(img[k], 1, len[k], f);
	return fclose(f) == 0;
}

int main(int argc, char **argv)
{
	Lb7 lb;
	if (argc == 4 && !strcmp(argv[1], "icon")) {
		if (cmd_icon(argv[2], argv[3]))
			return 0;
		fprintf(stderr, "no icon in %s\n", argv[2]);
		return 1;
	}
	char err[512];
	if (argc < 3) {
		fprintf(stderr,
			"usage: %s info <file.bin>\n"
			"     %s list <file.bin>\n"
			"     %s disasm <file.bin> [n|all]\n"
			"     %s extract <file.bin> <folder>\n"
			"     %s hex <file.bin> <n> [byte]\n"
			"     %s movie <file.bin> <n> <folder> [every]\n"
			"     %s anim <file.bin> [n]\n"
			"     %s icon <file.exe> <file.ico>\n", argv[0], argv[0], argv[0], argv[0],
			argv[0], argv[0], argv[0], argv[0]);
		return 1;
	}
	if (!lb7_open(&lb, argv[2], err, sizeof(err))) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	int rc = 0;
	if (!strcmp(argv[1], "info"))
		cmd_info(&lb, argv[2]);
	else if (!strcmp(argv[1], "list"))
		cmd_list(&lb);
	else if (!strcmp(argv[1], "disasm"))
		cmd_disasm(&lb, argc, argv);
	else if (!strcmp(argv[1], "anim"))
		cmd_anim(&lb, argc, argv);
	else if (!strcmp(argv[1], "movie"))
		cmd_movie(&lb, argc, argv);
	else if (!strcmp(argv[1], "hex"))
		cmd_hex(&lb, argc, argv);
	else if (!strcmp(argv[1], "extract") && argc > 3)
		cmd_extract(&lb, argv[3]);
	else {
		fprintf(stderr, "unknown command\n");
		rc = 1;
	}
	lb7_close(&lb);
	return rc;
}
