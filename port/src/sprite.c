#include "sprite.h"

#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "cov.h"
#include "rt.h"
#include "text.h"

#define rd16 lb7_rd16

int spr_probe_on, spr_probe_x, spr_probe_y, spr_probe_r;

typedef void (*SprDrawFn)(void *ctx, const uint8_t *pix, const Lb7Bitmap *b, int left, int top, int transparent,
	int flip_x, int flip_y);
static void sprite_images(SprWorld *w, const Video *v, Sprite *s, SprDrawFn fn, void *ctx);
static int cmp_layer(const void *a, const void *b);
static uint8_t *bitmap_pixels(SprWorld *w, const uint8_t *data, uint32_t size, const Lb7Bitmap *b);
static void edited_release(SprWorld *w, unsigned id);
static void resolve_shown(SprWorld *w, Sprite *s);
static void add_stamp(void *ctx, const uint8_t *pix, const Lb7Bitmap *b, int l, int t, int tr, int fx, int fy);

static void movie_stop(SprWorld *w, Sprite *s)
{
	if (s->mv_audio) { /* FUN_1010_10F2: A_013 frees the movie's wave event */
		if (w->rt)
			rt_movie_audio_end(w->rt, s->mv_audio);
		else
			audio_wave_destroy(s->mv_audio);
	}
	lb7_movie_frame_free(&s->mv_frame);
	s->mv_active = 0;
	s->mv_audio = 0;
}

/* Advances the sprite's movie (TOPSY 1010:12F0 / 0E02): times are cumulative from the start, like
 * the original, so the frames stay aligned with the audio. 1 = finished. */
static int movie_step(SprWorld *w, Sprite *s)
{
	const Lb7Movie *m = &s->mv;
	for (int guard = 0; guard < 64 && w->now_ms >= s->mv_next_ms; guard++) {
		if (s->mv_pc + 2 > m->script_size)
			return 1;
		const uint8_t *p = m->script + s->mv_pc;
		switch (rd16(p)) {
		case 1: /* {n, counter, ms}: n frames */
			if (s->mv_left <= 0)
				s->mv_left = rd16(p + 2);
			while (s->mv_rec < m->n_rec && lb7_movie_rec_kind(m, s->mv_rec) == 2)
				s->mv_rec++;
			if (s->mv_rec < m->n_rec)
				lb7_movie_frame(m, s->mv_rec++, &s->mv_frame);
			s->mv_next_ms += rd16(p + 6);
			if (--s->mv_left <= 0)
				s->mv_pc += 8;
			break;
		case 2: /* {ms} */
			s->mv_next_ms += rd16(p + 2);
			s->mv_pc += 4;
			break;
		case 3: { /* {wave header}: audio of the whole movie */
			Lb7Wave wv;
			uint8_t *pcm;
			size_t len = lb7_movie_audio(m, &wv, &pcm);
			/* FUN_1010_0D12: A_011 on the header + A_029 with block NULL (priority and
			   exclusivity of the header apply, no notification) */
			if (len && w->rt)
				s->mv_audio = rt_movie_audio(w->rt, &wv, (int16_t *)pcm, (uint32_t)(len / 2));
			else if (len && (s->mv_audio = audio_wave_create_pcm((int16_t *)pcm, (uint32_t)(len / 2),
				wv.channels, wv.rate)) != 0)
				audio_wave_start(s->mv_audio, 0);
			s->mv_pc += 30;
			break;
		}
		case 4: /* {sync}: skipping of late frames, not needed here */
			s->mv_pc += 4;
			break;
		default: /* 05 (script instruction) not used in Upsy's movies: end */
			return 1;
		}
	}
	return 0;
}

void spr_init(SprWorld *w, const Lb7 *lib, struct Rt *rt)
{
	memset(w, 0, sizeof(*w));
	w->lib = lib;
	w->rt = rt;
}

static void spr_free(SprWorld *w, Sprite *s)
{
	movie_stop(w, s);
	for (int i = 0; i < s->n_frames; i++)
		free(s->frames[i].pix);
	free(s->frames);
	free(s->anim);
	memset(s, 0, sizeof(*s));
}

void spr_clear(SprWorld *w)
{
	for (int i = 0; i < SPR_MAX; i++)
		if (w->spr[i].used)
			spr_free(w, &w->spr[i]);
	for (int k = 0; k < 2; k++) {
		free(w->bg[k].pix);
		memset(&w->bg[k], 0, sizeof(w->bg[k]));
		w->bg_id[k] = 0;
	}
	for (int i = 0; i < w->n_stamps; i++)
		free(w->stamps[i].pix);
	w->n_stamps = 0;
	w->n_texts = 0;
	w->n_text_colors = 0;
	for (int i = 0; i < w->n_edited; i++)
		free(w->edited[i].pix);
	w->n_edited = 0;
	free(w->comp);
	w->comp = NULL;
	free(w->bg_canvas);
	w->bg_canvas = NULL;
	/* S_047 (library switch): no zoom, no clip, background colour 0 (S_031(0, 0)) */
	w->zoom = w->zoom_x = w->zoom_y = 0;
	w->clip_on = 0;
	w->bg_keep = 0;
	w->bg_color = 0;
}

/* Background used at this resolution: [0] 800x600, [1] 640x480 (Mode640/LoMem). */
static int bg_index(SprWorld *w, const Video *v)
{
	return (w->rt ? w->rt->bg640 : video_width(v) < 800) ? 1 : 0;
}

void spr_background(SprWorld *w, int mode, unsigned bmp800, unsigned bmp640, int color)
{
	unsigned ids[2] = { bmp800, bmp640 };
	/* FUN_1008_03CE: mode 1 -> 0104 + S_031(color); mode 2 -> 0104 (S_031(0, 0) = black) then
	   S_031(0xFFFF, -1): world +0x880 = 0xFFFF, the composition buffer is no longer cleared and
	   what was drawn stays (trails); any other mode -> 0104 alone = solid black */
	free(w->bg_canvas); /* a new background replaces the buffer composed by a transition */
	w->bg_canvas = NULL;
	w->bg_keep = mode == 2;
	w->bg_color = mode == 1 ? color : mode == 0 ? color : 0;
	for (int i = 0; i < w->n_stamps; i++) /* new background: the previous stamps disappear */
		free(w->stamps[i].pix);
	w->n_stamps = 0;
	for (int k = 0; k < 2; k++) {
		unsigned id = mode == 0 ? ids[k] : 0;
		if (w->bg_id[k] == id)
			continue;
		free(w->bg[k].pix);
		memset(&w->bg[k], 0, sizeof(w->bg[k]));
		w->bg_id[k] = 0;
		Lb7Entry e = lb7_entry(w->lib, id);
		const uint8_t *d = id ? lb7_entry_data(w->lib, e) : NULL;
		if (d && e.type == 1 && lb7_bitmap_header(d, e.size, &w->bg[k].bmp)) {
			w->bg[k].data = d;
			w->bg[k].size = e.size;
			w->bg_id[k] = (uint16_t)id;
		}
	}
}

/* Transition of opcode 06 (TOPSY 1008:0F10). State of one run: the canvas being changed (the
   background bitmap DAT_0E46), the new image at screen size (DAT_071C) and the screen size
   (DAT_14F8 = width, DAT_14F6 = height). */
typedef struct {
	SprWorld *w;
	uint8_t *dst, *src;
	int W, H;
	unsigned n_present, every;
} BgTr;

/* FUN_1008_0B4C: S_008 copies rectangle {l, t, r, b} of the new bitmap into the background,
   S_028 invalidates it. */
static void tr_copy(BgTr *t, int l, int tp, int r, int b)
{
	if (l < 0)
		l = 0;
	if (tp < 0)
		tp = 0;
	if (r > t->W)
		r = t->W;
	if (b > t->H)
		b = t->H;
	for (int y = tp; y < b && l < r; y++)
		memcpy(t->dst + (size_t)y * t->W + l, t->src + (size_t)y * t->W + l, (size_t)(r - l));
}

/* S_001 (DAT_0E38): one world frame. The original repaints only the invalidated rectangles, so
   the effects that call it once per scan line (types 1 and 6) present every 'every' calls. */
static void tr_frame(BgTr *t)
{
	if (++t->n_present % t->every)
		return;
	if (t->w->rt && t->w->rt->frame)
		t->w->rt->frame(t->w->rt);
}

static int tr_has_frames(BgTr *t)
{
	return t->w->rt && t->w->rt->frame;
}

static uint32_t tr_now(BgTr *t)
{
	return t->w->rt ? t->w->rt->now_ms : 0;
}

/* FUN_1008_0C0E: rectangle i of n of the shrinking frame; mode 0 = full width. */
static void tr_rect(BgTr *t, int mode, unsigned n, unsigned i, int r[4])
{
	r[0] = mode ? (int)((uint32_t)i * (uint32_t)t->W / (n * 2 - 1)) : 0;
	r[2] = t->W - r[0];
	r[1] = (int)((uint32_t)i * (uint32_t)t->H / (n * 2 - 1));
	r[3] = t->H - r[1];
}

/* FUN_1008_0BC6 (top and bottom bands) + FUN_1008_0B80 (left and right bands, mode != 0)
   between the inner rectangle c and the outer rectangle o. */
static void tr_ring(BgTr *t, int mode, const int c[4], const int o[4])
{
	tr_copy(t, o[0], o[1], o[2], c[1]);
	tr_copy(t, o[0], c[3], o[2], o[3]);
	if (mode) {
		tr_copy(t, o[0], c[1], c[0], c[3]);
		tr_copy(t, c[2], c[1], o[2], c[3]);
	}
}

/* Background bitmap f (or the solid colour) at screen size, placed as spr_draw does. */
static uint8_t *bg_render(SprWorld *w, SprFrame *f, int color, int vw, int vh)
{
	uint8_t *out = malloc((size_t)vw * (size_t)vh);
	if (!out)
		return NULL;
	memset(out, (uint8_t)color, (size_t)vw * (size_t)vh);
	if (f->data && !f->pix)
		f->pix = bitmap_pixels(w, f->data, f->size, &f->bmp);
	if (!f->pix)
		return out;
	const Lb7Bitmap *b = &f->bmp;
	int x = vw / 2 - ((int)b->width - 1) / 2, y = vh / 2 - ((int)b->height - 1) / 2;
	for (int row = 0; row < b->height; row++) {
		int sy = y + row;
		if (sy < 0 || sy >= vh)
			continue;
		const uint8_t *src = f->pix + (size_t)(b->height - 1 - row) * b->stride; /* bottom-up */
		for (int i = 0; i < b->width; i++)
			if (x + i >= 0 && x + i < vw)
				out[(size_t)sy * vw + x + i] = src[i];
	}
	return out;
}

int spr_background_transition(SprWorld *w, unsigned bmp800, unsigned bmp640, int color,
	int type, unsigned p12, unsigned p14)
{
	/* FUN_1008_05D8: only with a background bitmap already present (mode 0 is checked by the
	   caller); the new bitmap (+4, or +6 at 640x480) must load */
	const Video *v = w->last_video;
	if (!v || type < 1 || type > 10)
		return 0;
	int k = bg_index(w, v);
	unsigned id = k && bmp640 ? bmp640 : bmp800;
	SprFrame *old = &w->bg[k];
	Lb7Entry e = lb7_entry(w->lib, id);
	const uint8_t *d = id ? lb7_entry_data(w->lib, e) : NULL;
	SprFrame nf = { 0 };
	if ((!old->data && !w->bg_canvas) || !d || e.type != 1 || !lb7_bitmap_header(d, e.size, &nf.bmp))
		return 0;
	nf.data = d;
	nf.size = e.size;
	BgTr t = { w, NULL, NULL, video_width(v), video_height(v), 0, 1 };
	if (w->bg_canvas && w->bg_cw == t.W && w->bg_ch == t.H) { /* a previous transition */
		t.dst = w->bg_canvas;
		w->bg_canvas = NULL;
	}
	else
		t.dst = bg_render(w, old, w->bg_color, t.W, t.H);
	t.src = bg_render(w, &nf, w->bg_color, t.W, t.H);
	free(nf.pix);
	if (!t.dst || !t.src) {
		free(t.dst);
		free(t.src);
		return 0;
	}
	/* the stamps lived in the old background buffer: the canvas has them under the new image */
	for (int i = 0; i < w->n_stamps; i++) {
		const SprStamp *st = &w->stamps[i];
		for (int row = 0; row < st->bmp.height; row++) {
			int sy = t.H / 2 + st->top + row;
			if (sy < 0 || sy >= t.H)
				continue;
			const uint8_t *src = st->pix + (size_t)(st->bmp.height - 1 - row) * st->bmp.stride;
			for (int i2 = 0; i2 < st->bmp.width; i2++) {
				int sx = t.W / 2 + st->left + i2;
				if (sx >= 0 && sx < t.W && src[i2])
					t.dst[(size_t)sy * t.W + sx] = src[i2];
			}
		}
		free(st->pix);
	}
	w->n_stamps = 0;
	free(w->bg_canvas);
	w->bg_canvas = t.dst;
	w->bg_cw = t.W;
	w->bg_ch = t.H;
	int events_off = w->events_off; /* S_061(0): events suppressed during the effect */
	w->events_off = 1;
	unsigned W = (unsigned)t.W, H = (unsigned)t.H;
	switch (type) {
	case 1: { /* FUN_1008_09AE: interlaced scan lines, p12 = interleave (default 10) */
		unsigned n = p12 ? p12 : 10, per = (H + n - 1) / n;
		t.every = H / 60 ? H / 60 : 1;
		for (unsigned p = 0; p < n; p++) {
			unsigned row = p;
			for (unsigned c = 0; c < per && row <= H; c++, row += n) {
				tr_copy(&t, 0, (int)row, t.W, (int)row + 1);
				tr_frame(&t);
			}
		}
		break;
	}
	case 2: case 4: { /* FUN_1008_0A32(mode, 30): a closing frame, from the edges to the centre */
		int mode = type == 2, c[4], o[4];
		tr_rect(&t, mode, 30, 29, c);
		tr_rect(&t, mode, 30, 0, o);
		for (unsigned i = 1; i < 30; i++) {
			tr_rect(&t, mode, 30, i, c);
			tr_ring(&t, mode, c, o);
			tr_frame(&t);
			memcpy(o, c, sizeof(o));
		}
		tr_copy(&t, c[0], c[1], c[2], c[3]);
		break;
	}
	case 3: case 5: { /* FUN_1008_0AC0(mode, 30): an opening frame, from the centre to the edges
		(the inner rectangle is never updated: each step fills the ring up to rectangle i) */
		int mode = type == 3, c[4], o[4];
		tr_rect(&t, mode, 30, 29, c);
		tr_copy(&t, c[0], c[1], c[2], c[3]);
		for (int i = 28; i >= 0; i--) {
			tr_frame(&t);
			tr_rect(&t, mode, 30, (unsigned)i, o);
			tr_ring(&t, mode, c, o);
		}
		break;
	}
	case 6: { /* FUN_1008_0C72(ms = p14, strips = p12): timed interleaved scan lines */
		unsigned n = p12 ? (p12 > H ? H : p12) : 10;
		unsigned stride = n == 1 ? 1 : n - 1, lim = stride, e2 = 0;
		uint32_t step = p14 / (H / n + n - 1), next = tr_now(&t);
		t.every = H / 60 ? H / 60 : 1;
		while (e2 < H) {
			if (tr_has_frames(&t) && (int32_t)(tr_now(&t) - next) < 0) {
				t.n_present = t.every - 1;
				tr_frame(&t);
				continue;
			}
			next += step;
			for (unsigned r = e2; r < lim; r += stride) {
				tr_copy(&t, 0, (int)r, t.W, (int)r + 1);
				tr_frame(&t);
			}
			e2 += e2 < stride ? 1 : n;
			lim = lim + n < H ? lim + n : H;
		}
		break;
	}
	default: { /* 7/8 FUN_1008_0D80 horizontal (7 = from the right), 9/10 FUN_1008_0E48
		vertical (9 = from the bottom): p12 strips (default 60), p14 total ms */
		int horiz = type <= 8, dir = (type & 1) ? -1 : 1;
		unsigned size = horiz ? W : H, n = p12 ? (p12 > size ? size : p12) : 60;
		int delta = (int)(size / n) * dir, a, b;
		if (delta < 0) {
			a = (int)size + delta;
			b = (int)size;
		}
		else {
			a = 0;
			b = delta;
		}
		uint32_t next = tr_now(&t);
		while (delta && a >= 0 && (unsigned)a < size && b >= 0) {
			tr_frame(&t);
			if (!tr_has_frames(&t) || (int32_t)(tr_now(&t) - next) >= 0) {
				next += p14 / n;
				if (horiz)
					tr_copy(&t, a, 0, b, t.H);
				else
					tr_copy(&t, 0, a, t.W, b);
				a += delta;
				b += delta;
			}
		}
		break;
	}
	}
	w->events_off = events_off;
	free(t.src);
	/* DAT_0E46 keeps the composed image (strips the effect did not reach keep the old pixels):
	   the canvas stays the background until the next opcode 06 or library change */
	for (int j = 0; j < 2; j++) {
		free(w->bg[j].pix);
		memset(&w->bg[j], 0, sizeof(w->bg[j]));
		w->bg_id[j] = 0;
	}
	w->bg[k] = nf;
	w->bg[k].pix = NULL;
	w->bg_id[k] = (uint16_t)id;
	w->bg_keep = 0;
	w->bg_color = color;
	return 1;
}

static Sprite *find(SprWorld *w, unsigned id)
{
	for (int i = 0; i < SPR_MAX; i++)
		if (w->spr[i].used && w->spr[i].id == id && !w->spr[i].orphan) /* orphan: no id (65F2) */
			return &w->spr[i];
	return NULL;
}

/* Encoded operand of an animation: same encoding as the scripts, globals of the library. */
static int anim_val(SprWorld *w, uint16_t v)
{
	if (v < 0x13FE)
		return w->rt->g[v];
	if (v < 0x159F)
		return w->rt->lstack[v - 0x13FE];
	return (int16_t)(v - 0x8ACF);
}

/* Adds a frame from an entry of type 1 (bitmap) or 12 (cel). */
static void add_frame(SprWorld *w, Sprite *s, unsigned id)
{
	Lb7Entry e = lb7_entry(w->lib, id);
	const uint8_t *d = lb7_entry_data(w->lib, e);
	if (!d || !s->frames)
		return;
	SprFrame f = { 0 };
	if (e.type == 12 && e.size >= 6) { /* cel: {bitmap, dx, dy} */
		unsigned bid = (uint16_t)(rd16(d) - 0x8ACF);
		Lb7Entry be = lb7_entry(w->lib, bid);
		const uint8_t *bd = lb7_entry_data(w->lib, be);
		if (!bd || be.type != 1 || !lb7_bitmap_header(bd, be.size, &f.bmp))
			return;
		f.data = bd;
		f.size = be.size;
		f.cx = (int16_t)rd16(d + 2);
		f.cy = (int16_t)rd16(d + 4);
		f.cel = !getenv("UPSY_CELHOT");
	}
	else if (e.type == 1) {
		if (!lb7_bitmap_header(d, e.size, &f.bmp))
			return;
		f.data = d;
		f.size = e.size;
	}
	else
		return;
	f.entry = (uint16_t)id;
	s->frames[s->n_frames++] = f;
}

int spr_load(SprWorld *w, unsigned id)
{
	if (find(w, id))
		return 1;
	Lb7Entry e = lb7_entry(w->lib, id);
	const uint8_t *d = lb7_entry_data(w->lib, e);
	if (!d || e.type != 17 || e.size < 0x16)
		return 0;
	Sprite *s = NULL;
	for (int i = 0; i < SPR_MAX && !s; i++)
		if (!w->spr[i].used)
			s = &w->spr[i];
	if (!s)
		return 0;
	memset(s, 0, sizeof(*s));
	s->used = 1;
	s->id = (uint16_t)id;
	/* +0/+2 are not the position but the click script and argument (opcode 04, in rt.c): the
	   sprite starts at the origin until a script moves it */
	s->x = s->y = 0;
	s->hit_box = d[0x11] != 0;
	s->hit_state = d[0x15];
	/* +0x0E: layer, encoded operand; SIGNED comparison (jle in FUN_1000_10D4 and S_065:
	   -1 = below everything) */
	s->layer = (int16_t)anim_val(w, rd16(d + 0x0E));
	s->seq = ++w->seq;
	unsigned n = rd16(d + 6);
	s->frames = calloc(n ? n : 1, sizeof(SprFrame));
	if (!s->frames) {
		s->used = 0;
		return 0;
	}
	for (unsigned k = 0; k < n && 0x16 + k * 2 + 2 <= e.size; k++) {
		unsigned m = (uint16_t)(rd16(d + 0x16 + k * 2) - 0x8ACF);
		unsigned t = lb7_entry(w->lib, m).type;
		if (t == 1 || t == 12)
			add_frame(w, s, m);
		else if (t == 18 && s->frames) { /* text: rendered at the first draw (1010:1CA0) */
			SprFrame f = { 0 };
			f.text_id = (uint16_t)m;
			s->frames[s->n_frames++] = f;
		}
		/* 10 (movie) used by cmd 17. 9/4: the original preloads the wave / MIDI event (no sound
		   starts); here the event is created at the first opcode 08/50. Unloading the sprite
		   destroys them (rt_unload). */
	}
	/* +0x14: default sprite, FUN_1008_6BAA (old default destroyed if it is a leftover, palette
	   block installed at 0xF6 - DAT_1530), then DAT_0E3E = this one; its last wave/MIDI members
	   and the sprite itself survive a link (spr_default_detach) */
	if (d[0x14] && w->rt) {
		rt_default_sprite(w->rt, id);
		s->is_def = 1;
	}
	/* current frame = the first one, even when hidden: needed by the hit test of sprites with
	   state 1 (clickable even if invisible, e.g. the menu buttons of TOPSMAIN) */
	s->n_shown = s->n_frames ? 1 : 0;
	s->shown[0] = 0;
	return 1;
}

void spr_unload(SprWorld *w, unsigned id)
{
	Sprite *s = find(w, id);
	if (s) {
		spr_free(w, s);
		edited_release(w, 0); /* FUN_1008_6502: members whose count drops to 0 are freed */
	}
}

void spr_move(SprWorld *w, unsigned id, int x, int y)
{
	Sprite *s = find(w, id);
	if (s) {
		s->x = (int16_t)x;
		s->y = (int16_t)y;
	}
}

void spr_hide(SprWorld *w, unsigned id)
{
	Sprite *s = find(w, id);
	if (s)
		s->visible = 0;
}

static void stop_anim(SprWorld *w, Sprite *s)
{
	movie_stop(w, s);
	free(s->anim);
	s->anim = NULL;
	s->anim_size = s->pc = 0;
	s->step_left = 0;
	s->waiting = 0;
	s->moving = 0;
	s->mv_op = 0;
	s->after_pos = 0;
	/* S_010: image back in view (+0x60), no cumulative timing (+0x63) and no
	   loop exit (+0x65) */
	s->frozen = 0;
	s->cumulative = 0;
	s->break_loops = 0;
}

int spr_show(SprWorld *w, unsigned id, unsigned anim, int hide_first)
{
	/* TOPSY FUN_1008_A990: with animation 0 an unloaded sprite is left as is (no load) */
	if (!anim && !find(w, id))
		return 1;
	if (!spr_load(w, id))
		return 0;
	cov_mark(COV_SPRITE, id);
	if (anim)
		cov_mark(COV_ANIM, anim);
	Sprite *s = find(w, id);
	if (w->rt && w->rt->trace) {
		fprintf(w->rt->trace, "%7u    [sprite %u: pos %d,%d layer %d, %d frames", w->now_ms, id, s->x, s->y,
			s->layer, s->n_frames);
		for (int k = 0; k < s->n_frames && k < 3; k++)
			fprintf(w->rt->trace, "; f%d %ux%u cel %d,%d anchor %d,%d", k, s->frames[k].bmp.width,
				s->frames[k].bmp.height, s->frames[k].cx, s->frames[k].cy, s->frames[k].bmp.w5,
				s->frames[k].bmp.w6);
		Lb7Entry e17 = lb7_entry(w->lib, id);
		const uint8_t *d17 = lb7_entry_data(w->lib, e17);
		if (d17) {
			fprintf(w->rt->trace, "; members:");
			for (unsigned k = 0; k < lb7_rd16(d17 + 6) && k < 6; k++) {
				unsigned m = (uint16_t)(lb7_rd16(d17 + 0x16 + k * 2) - 0x8ACF);
				fprintf(w->rt->trace, " %u(t%u)", m, lb7_entry(w->lib, m).type);
			}
		}
		fprintf(w->rt->trace, "]\n");
	}
	if (hide_first)
		s->visible = 0;
	stop_anim(w, s);
	if (!anim) { /* S_010: just stops the animation; with the flag the sprite is already hidden (S_025) */
		if (getenv("UPSY_OLDSHOW")) {
			s->visible = 1;
			s->n_shown = s->n_frames ? 1 : 0;
			s->shown[0] = 0;
		}
		return 1;
	}
	Lb7Entry e = lb7_entry(w->lib, anim);
	const uint8_t *d = lb7_entry_data(w->lib, e);
	if (!d || e.type != 15 || !e.size)
		return 0;
	s->anim = malloc(e.size);
	if (!s->anim)
		return 0;
	memcpy(s->anim, d, e.size);
	s->anim_id = (uint16_t)anim;
	s->anim_size = e.size;
	s->pc = 0;
	s->next_ms = w->now_ms;
	s->cumulative = 0;
	/* no visibility here: S_039 (TOPSYS01 1000:4E6C) -> S_010 + 471A do not touch +0x56; the
	   sprite becomes visible from the animation's first 01/02/12 (the end credits, sprite 3078 with
	   anim 3079 = 0B of ~11 s before the first 01, appeared immediately) */
	return 1;
}

static void run_anim(SprWorld *w, Sprite *s);

/* Builtin 5A/5B (TOPSY 1008:1854 -> S_066/S_068 -> FUN_1000_59F6): animation command
 * built by the script, args = {sprite, command, values...}. 5A (immediate) empties the sprite's
 * command buffer and runs it right away; 5B queues it and starts it if the sprite is idle.
 * The values are already computed: they are written as encoded constants (v + 0x8ACF, as the DLL
 * does with "- 0x7531"), so run_anim reads them like the other commands. */
int spr_command(SprWorld *w, int immediate, unsigned argc, const int16_t *a)
{
	if (argc < 2 || !spr_load(w, (uint16_t)a[0]))
		return 0;
	Sprite *s = find(w, (uint16_t)a[0]);
	unsigned op = (uint16_t)a[1], nv = argc - 2;
	if (w->rt && w->rt->trace) {
		fprintf(w->rt->trace, "%7u    [%s sprite %u cmd %02X:", w->now_ms, immediate ? "5A" : "5B", (uint16_t)a[0], op);
		for (unsigned i = 2; i < argc; i++)
			fprintf(w->rt->trace, " %d", a[i]);
		fprintf(w->rt->trace, "]\n");
	}
	const int16_t *v = a + 2;
	uint16_t c[32] = { (uint16_t)op };
	unsigned n = 1; /* words of the command */
#define K(x) ((uint16_t)((x) + 0x8ACF))
	switch (op) {
	case 0x01: /* {from, to, counter, duration} <- (from, to, duration) */
		c[1] = K(nv > 0 ? v[0] : 0); c[2] = K(nv > 1 ? v[1] : 0); c[3] = 0;
		c[4] = K(nv > 2 ? v[2] : 0); n = 5;
		break;
	case 0x02: { /* {duration, state, n, frames...} <- (frames..., duration) */
		unsigned k = nv ? nv - 1 : 0;
		if (k < 2 || k > 10) /* FUN_1000_55B0: argc - 2 in 3..11 (2..10 frames), else failure */
			return 0;
		c[1] = K(nv ? v[nv - 1] : 0); c[2] = 0; c[3] = (uint16_t)k;
		for (unsigned i = 0; i < k; i++)
			c[4 + i] = K(v[i]);
		n = 4 + k;
		break;
	}
	case 0x12: /* {frame}: without an argument -1 (S_043) */
		c[1] = nv ? (uint16_t)v[0] : 0xFFFF; n = 2;
		break;
	case 0x09: /* {steps, speed, relative, x, y, ...} <- (x, y, steps, speed, relative) (58E8) */
		c[4] = K(nv > 0 ? v[0] : 0); c[5] = K(nv > 1 ? v[1] : 0); c[1] = K(nv > 2 ? v[2] : 0);
		c[2] = nv > 3 && v[3]; c[3] = nv > 4 && v[4]; n = 12;
		break;
	case 0x05: /* {steps, relative, xm, ym, x1, y1, ...} <- (xm, ym, x1, y1, steps [, relative]) (56A2) */
		if (nv < 5)
			return 0;
		c[3] = K(v[0]); c[4] = K(v[1]); c[5] = K(v[2]); c[6] = K(v[3]); c[1] = K(v[4]);
		c[2] = nv > 5 && v[5]; n = 15;
		break;
	case 0x0A: /* {relative, x, y} <- (x, y, relative), exactly 3 values (5B74) */
		if (nv != 3)
			return 0;
		c[2] = K(v[0]); c[3] = K(v[1]); c[1] = v[2] != 0; n = 4;
		break;
	case 0x0C: /* {x, y, relative} <- (x, y [, relative]) (5D20) */
		if (nv < 2)
			return 0;
		c[1] = K(v[0]); c[2] = K(v[1]); c[3] = nv > 2 && v[2]; n = 4;
		break;
	case 0x11: /* {movement interval, frame interval} (5F92) */
		c[1] = K(nv ? v[0] : 0); c[2] = nv > 1 ? K(v[1]) : 0; n = 3;
		break;
	case 0x0E: /* {0/1} raw (5EB4) */
		c[1] = nv && v[0]; n = 2;
		break;
	case 0x16: { /* {script, n, arguments} <- (script, arguments...) (5C2E): 1..7 values */
		if (nv < 1 || nv > 7)
			return 0;
		c[1] = K(v[0]); c[2] = (uint16_t)(nv - 1);
		for (unsigned i = 1; i < nv; i++)
			c[2 + i] = K(v[i]);
		n = 9;
		break;
	}
	default: { /* same field order as the command */
		uint8_t probe[64] = { (uint8_t)op };
		unsigned len = lb7_anim_len(probe, sizeof(probe));
		n = len >= 2 ? len / 2 : 1;
		if (n > 32) n = 32;
		for (unsigned i = 1; i < n; i++)
			c[i] = K(i - 1 < nv ? v[i - 1] : 0);
		break;
	}
	}
#undef K
	/* FUN_1000_59F6 with 5A: only 01 02 05 09 0A 16 empty the queue (FUN_1000_54F6) and start
	   immediately; the others act on the sprite and leave the running animation untouched (e.g. the
	   cmd 02 of a show followed by a 5A 0C position: without this the heads and
	   balloons of Upsy's characters were lost at the start of the game) */
	if (immediate && op != 0x01 && op != 0x02 && op != 0x05 && op != 0x09 && op != 0x0A && op != 0x16) {
		uint8_t one[64];
		for (unsigned i = 0; i < n; i++) {
			one[i * 2] = (uint8_t)c[i];
			one[i * 2 + 1] = (uint8_t)(c[i] >> 8);
		}
		uint8_t *anim = s->anim;
		uint32_t size = s->anim_size, pc = s->pc, next = s->next_ms;
		int step_left = s->step_left, waiting = s->waiting, moving = s->moving, mv = s->mv_active;
		int after_pos = s->after_pos;
		uint8_t *tmp = malloc(n * 2);
		if (!tmp)
			return 0;
		memcpy(tmp, one, n * 2);
		s->anim = tmp;
		s->anim_size = n * 2;
		s->pc = 0;
		s->step_left = s->waiting = s->moving = s->mv_active = s->after_pos = 0;
		s->next_ms = w->now_ms;
		run_anim(w, s); /* runs the command and frees the temporary buffer at the end of the queue */
		if (s->anim == tmp)
			free(tmp);
		s->anim = anim;
		s->anim_size = size;
		s->pc = pc;
		s->next_ms = next;
		s->step_left = step_left;
		s->waiting = waiting;
		s->moving = moving;
		s->mv_active = mv;
		s->after_pos = after_pos;
		return 1;
	}
	uint32_t bytes = n * 2, keep = immediate || !s->anim ? 0 : s->anim_size;
	uint8_t *buf = realloc(immediate ? NULL : s->anim, keep + bytes);
	if (!buf)
		return 0;
	if (immediate) {
		free(s->anim);
		s->step_left = 0;
		s->waiting = 0;
		s->after_pos = 0;
		s->pc = 0;
	}
	for (unsigned i = 0; i < n; i++) {
		buf[keep + i * 2] = (uint8_t)c[i];
		buf[keep + i * 2 + 1] = (uint8_t)(c[i] >> 8);
	}
	int idle = !s->anim || s->pc >= s->anim_size;
	s->anim = buf;
	s->anim_size = keep + bytes;
	if (immediate || idle) {
		if (!immediate)
			s->pc = keep;
		s->next_ms = w->now_ms;
		/* visibility (+0x56) is given only by cmds 01/02 (FUN_1000_360E) and 12 (S_043), in
		   run_anim: a 5B 0B (wait) or 16 on a hidden sprite does not show it (the gargoyles
		   hanging from the menu window, TOPSMAIN 4006, appear only after 6-12 s) */
		/* 5A runs immediately (FUN_1000_6044); 5B on an idle sprite only prepares (471A): the
		   command starts at the next engine tick (S_081), so a command 16 that requeues
		   itself does not turn into recursion */
		if (immediate)
			run_anim(w, s);
	}
	return 1;
}

/* Next instant: cumulative (cmd 0D) or starting from now. */
static void schedule(SprWorld *w, Sprite *s, int ms)
{
	uint32_t base = s->cumulative && s->next_ms ? s->next_ms : w->now_ms;
	s->next_ms = base + (uint32_t)(ms < 0 ? 0 : ms);
}

/* Point of the parabola for v0 at the start, vm in the middle and v1 at the end, at step `step`
 * of total (TOPSYS01 1000:1758, same as builtin 0x73 in rt.c): u in 1/1024. */
static int parab(int step, int total, int v0, int vm, int v1)
{
	if (total <= 0)
		return v1;
	int64_t u = (int64_t)(((uint32_t)(uint16_t)step * 0x400u + ((uint32_t)total >> 1)) / (uint32_t)total);
	int64_t q = u * u * (v1 - v0) + (u & 0xFFFF) * (0x400 - u) * ((int16_t)((vm - v0) * 4) - (v1 - v0));
	return (int16_t)((int16_t)(q >> 20) + v0);
}

/* d * cur / total rounded half away from zero (TOPSYS01 FUN_1000_3F84). */
static int lerp_round(int d, int cur, int total)
{
	long half = total / 2, p = (long)d * cur;
	return (int)(d < 0 ? (p - half) / total : (p + half) / total);
}

/* FUN_1000_6AA4: on = 0 freezes the visible image, on = 1 shows the current state. */
static void set_live(Sprite *s, int on)
{
	if ((!on) == s->frozen)
		return;
	s->frozen = !on;
	if (on)
		return;
	s->fz_x = s->x;
	s->fz_y = s->y;
	s->fz_flip_x = s->flip_x;
	s->fz_flip_y = s->flip_y;
	s->fz_n_shown = s->n_shown;
	memcpy(s->fz_shown, s->shown, sizeof(s->fz_shown));
}

/* FUN_1000_6B04: shows the current frame of the cmd 10 cycle and moves on to the next
 * (from..to, then from again). Called when the frame interval expires (+0x1C/+0x24). */
static void cycle_step(SprWorld *w, Sprite *s)
{
	if (!s->cyc_on || (int32_t)(w->now_ms - s->cyc_next) < 0)
		return;
	if (s->cyc_cur >= 0 && s->cyc_cur < s->n_frames) {
		s->shown[0] = s->cyc_cur;
		s->n_shown = 1;
		resolve_shown(w, s);
	}
	if (s->cyc_up)
		s->cyc_cur = s->cyc_cur + 1 > s->cyc_to ? s->cyc_from : s->cyc_cur + 1;
	else
		s->cyc_cur = s->cyc_cur - 1 < s->cyc_to ? s->cyc_from : s->cyc_cur - 1;
	s->cyc_next = w->now_ms + (uint32_t)(s->frame_ms > 0 ? s->frame_ms : 0);
}

/* End of a command (4AC2 returns 1): S_081 calls 471A, which prepares the next one (skipping
 * the 06 loops) or, if there are none, stops the animation (S_010) in the same tick. The next
 * command is executed on the following tick. */
static void cmd_done(Sprite *s)
{
	if (s->anim && s->pc >= s->anim_size) {
		free(s->anim);
		s->anim = NULL;
		s->anim_size = s->pc = 0;
		s->after_pos = 0;
	}
}

/* One engine tick for the sprite (S_081 -> FUN_1000_4AC2): executes the current command; almost
 * all of them "consume the tick" (executing or finishing a command ends the tick, the next one
 * starts on the following tick). Only 0D 0F 10 11 and the 06 loops (471A/427A) continue within the
 * same tick, and 07/0C when the next command is 01 or 02 (471A clears +0x57 and 4AC2 continues
 * without drawing; otherwise it draws with 36E4 and ends the tick). */
static void run_anim(SprWorld *w, Sprite *s)
{
	for (int guard = 0; s->anim && guard < 256; guard++) {
		if (s->mv_active) { /* cmd 17 in progress: the last frame stays visible at the end */
			if (!movie_step(w, s))
				return;
			s->mv_active = 0;
			s->pc += lb7_anim_len(s->anim + s->pc, s->anim_size - s->pc);
			continue;
		}
		if (s->moving) { /* cmd 09: one step per interval (FUN_1000_404A -> 3FEE -> 3F84) */
			cycle_step(w, s); /* frames of cmd 10, with their interval */
			if (w->now_ms < s->next_ms)
				return;
			s->mv_step++;
			if (s->mv_curve) { /* cmd 05 (FUN_1000_1882 -> 1758), absolute position */
				s->x = parab(s->mv_step, s->mv_steps, s->mv_sx, s->mv_mx, s->mv_tx);
				s->y = parab(s->mv_step, s->mv_steps, s->mv_sy, s->mv_my, s->mv_ty);
			}
			else {
				s->x = s->mv_sx + lerp_round(s->mv_tx - s->mv_sx, s->mv_step, s->mv_steps);
				s->y = s->mv_sy + lerp_round(s->mv_ty - s->mv_sy, s->mv_step, s->mv_steps);
			}
			schedule(w, s, s->move_ms);
			if (s->mv_step < s->mv_steps)
				return;
			s->moving = 0;
			s->mv_op = 0;
			s->pc += lb7_anim_len(s->anim + s->pc, s->anim_size - s->pc);
			cmd_done(s);
			return;
		}
		if (s->step_left > 0 || s->waiting) {
			if (w->now_ms < s->next_ms)
				return;
			if (s->waiting) { /* end of the wait: 697C/360E/0D42 return 1 */
				s->waiting = 0;
				cmd_done(s);
				return;
			}
			/* cmd 01: next frame in the interval */
			const uint8_t *p = s->anim + s->pc;
			int lo = anim_val(w, rd16(p + 2)), hi = anim_val(w, rd16(p + 4));
			int span = (hi >= lo ? hi - lo : lo - hi) + 1;
			int f = hi >= lo ? lo + s->step_frame % span : lo - s->step_frame % span;
			s->step_frame++;
			s->shown[0] = f;
			s->n_shown = 1;
			resolve_shown(w, s);
			s->visible = 1; /* FUN_1000_360E: +0x56 = 1, even after a cmd 08 */
			if (--s->step_left > 0) {
				int dur = (int16_t)rd16(p + 8) == -1 ? 0 : anim_val(w, rd16(p + 8));
				schedule(w, s, dur);
				return;
			}
			s->pc += lb7_anim_len(p, s->anim_size - s->pc);
			int dur = (int16_t)rd16(p + 8) == -1 ? 0 : anim_val(w, rd16(p + 8));
			schedule(w, s, dur);
			/* the last frame also stays for its duration before the next command
			   (FUN_1000_360E: with counter at 0 and duration > 0 it does not advance) */
			s->waiting = dur > 0;
			if (!s->waiting)
				cmd_done(s);
			return;
		}
		if (s->pc >= s->anim_size) { /* end: the last frame remains (also for a movie) */
			free(s->anim);
			s->anim = NULL;
			s->anim_size = s->pc = 0;
			return;
		}
		uint8_t *p = s->anim + s->pc;
		unsigned op = rd16(p), n = lb7_anim_len(p, s->anim_size - s->pc);
		if (!n) {
			free(s->anim);
			s->anim = NULL;
			s->anim_size = s->pc = 0;
			return;
		}
		if (s->after_pos) { /* after 07/0C: continues only if the command is 01 or 02 */
			if (op == 0x06)
				; /* the loop is resolved by 427A inside 471A */
			else {
				s->after_pos = 0;
				if (op != 0x01 && op != 0x02)
					return;
			}
		}
		cov_cmd(op);
		switch (op) {
		case 0x01: { /* {from, to, counter, duration}: the counter (+6) is computed when the
			command starts = frames of the interval (TOPSYS01 FUN_1000_6A1A, called by 4ac2/360e) */
			int lo = anim_val(w, rd16(p + 2)), hi = anim_val(w, rd16(p + 4));
			set_live(s, 1); /* FUN_1000_6AA4(0, 1, ...) */
			s->cyc_on = 0;  /* cmd 01 sets its own interval (+0x4E/+0x50) */
			if ((unsigned)(uint16_t)lo >= (unsigned)s->n_frames || (unsigned)(uint16_t)hi >= (unsigned)s->n_frames) {
				/* FUN_1000_6A1A: from or to outside the frames (+0x46, unsigned) -> counter 0:
				   360E finishes at once, nothing shown, no wait */
				s->pc += n;
				cmd_done(s);
				return;
			}
			s->step_left = (hi >= lo ? hi - lo : lo - hi) + 1;
			s->step_frame = 0;
			if (s->step_left <= 0) {
				s->pc += n;
				continue;
			}
			s->next_ms = w->now_ms;
			continue;
		}
		case 0x02: { /* {duration, state, n, frames}: composite frame */
			unsigned k = rd16(p + 6);
			set_live(s, 1);
			s->cyc_on = 0;
			s->visible = 1; /* like cmd 01 */
			int bad = 0, sh[SPR_MAX_LAYERS], nsh = 0;
			for (unsigned i = 0; i < k; i++) {
				unsigned f = (uint16_t)anim_val(w, rd16(p + 8 + i * 2));
				if (f == 0xFFFF) /* FUN_1000_0D42: member -1 is skipped */
					continue;
				if (f >= (unsigned)s->n_frames) { /* >= +0x46: the whole command is dropped */
					bad = 1;
					break;
				}
				if (nsh < SPR_MAX_LAYERS)
					sh[nsh++] = (int)f;
			}
			s->pc += n;
			int dur = (int16_t)rd16(p + 2) == -1 ? 0 : anim_val(w, rd16(p + 2));
			schedule(w, s, dur); /* the deadline (+0x1C) is advanced before the members are read */
			if (bad) { /* goto 0D79: returns 1, nothing shown */
				cmd_done(s);
				return;
			}
			s->n_shown = nsh;
			memcpy(s->shown, sh, sizeof(int) * (size_t)nsh);
			resolve_shown(w, s); /* FUN_1000_0D42: the parts of the composite frame */
			s->waiting = 1;
			return;
		}
		case 0x06: { /* loop {counter, repetitions, target} (FUN_1000_427A) */
			uint16_t cnt = rd16(p + 2);
			if (cnt == 1 || s->break_loops) { /* +0x65 (opcode 19): leaves the loop */
				uint16_t rep = rd16(p + 4);
				p[2] = (uint8_t)rep;
				p[3] = (uint8_t)(rep >> 8);
				s->pc += n;
			}
			else {
				if (cnt) {
					cnt--;
					p[2] = (uint8_t)cnt;
					p[3] = (uint8_t)(cnt >> 8);
				}
				s->pc = rd16(p + 6);
			}
			continue;
		}
		case 0x07: { /* {mirror x, mirror y, dx, dy} relative (FUN_1000_42DE -> 383C(0, 1, ...)) */
			int fx = anim_val(w, rd16(p + 2)) != 0, fy = anim_val(w, rd16(p + 4)) != 0;
			int dx = anim_val(w, rd16(p + 6)), dy = anim_val(w, rd16(p + 8));
			s->flip_x = fx;
			s->flip_y = fy;
			s->x += dx;
			s->y += dy;
			s->pc += n;
			s->after_pos = 1;
			cmd_done(s);
			continue;
		}
		case 0x08: s->visible = 0; s->pc += n; cmd_done(s); return;
		case 0x04: /* stamps the sprite onto the background and removes it (FUN_1000_0426(0, ...)):
			only with a background bitmap (+0x448) and the sprite visible (+0x56); then S_025 hides it
			and S_010 stops the animation (the remaining commands are dropped) */
			s->pc += n;
			if (w->last_video && s->visible && w->bg[bg_index(w, w->last_video)].data) {
				sprite_images(w, w->last_video, s, add_stamp, w);
				s->visible = 0;
				stop_anim(w, s);
				return;
			}
			cmd_done(s);
			return;
		case 0x03: /* no case in 4AC2 (default: returns 1) and none in 471A: one-tick no-op */
		case 0x13: /* no case in 4AC2 either; the EXE handler (1008:CD0E case 0x13, FUN_1008_E67E)
			is never reached because the DLL does not call the callback for it */
			s->pc += n;
			cmd_done(s);
			return;
		case 0x14: /* {}: FUN_1000_44DC like 15/16 -> EXE 1008:CD0E posts message 0x4C8
			(FUN_1008_EAD0: runs the deferred script DAT_1502 = rt->v1502, then destroys the default
			sprite; rt_tick handles it at the start of the next tick). Blocked while an event runs
			or events are off */
			if (w->event_busy || w->events_off)
				return;
			if (w->rt)
				w->rt->post_4c8 = 1;
			s->pc += n;
			return;
		case 0x17: { /* movie {type 10 resource, ...} (TOPSYS01 4AC2 -> 4F68; TOPSY 1010) */
			unsigned id = (uint16_t)anim_val(w, rd16(p + 2));
			set_live(s, 1); /* FUN_1000_4F68 starts with 6AA4(0, 1) */
			movie_stop(w, s);
			if (!lb7_movie_open(w->lib, id, &s->mv)) {
				w->unknown_cmd[0x17]++;
				s->pc += n;
				continue;
			}
			s->mv_active = 1;
			s->mv_pc = s->mv_rec = 0;
			s->mv_left = 0;
			s->mv_next_ms = w->now_ms;
			s->visible = 1;
			continue;
		}
		case 0x12: { /* shows the sprite, frame +2 (-1 = the current one) (S_043) */
			uint16_t f = rd16(p + 2);
			set_live(s, 1); /* S_043 calls 6AA4(0, 1) first */
			s->visible = 1;
			if (f < (unsigned)s->n_frames) {
				s->shown[0] = f;
				s->n_shown = 1;
			}
			else if (!s->n_shown && s->n_frames) {
				s->shown[0] = 0;
				s->n_shown = 1;
			}
			resolve_shown(w, s);
			s->pc += n;
			cmd_done(s);
			return;
		}
		case 0x0B: /* wait {ms}: the first tick sets the deadline and returns 0 (697C) */
			s->pc += n;
			schedule(w, s, anim_val(w, rd16(p + 2)));
			s->waiting = 1;
			return;
		case 0x0D: /* {byte}: +0x63 cumulative timing; the movement and frame deadlines
			(+0x18..+0x1E) become 0: the next wait counts from now */
			s->cumulative = (uint8_t)rd16(p + 2) != 0;
			s->next_ms = 0;
			s->pc += n;
			continue;
		case 0x09: { /* movement {steps, speed, relative, x, y} (FUN_1000_471A, case 9) */
			set_live(s, 1);
			s->mv_sx = s->x;
			s->mv_sy = s->y;
			s->mv_tx = anim_val(w, rd16(p + 8)) + (rd16(p + 6) ? s->x : 0);
			s->mv_ty = anim_val(w, rd16(p + 10)) + (rd16(p + 6) ? s->y : 0);
			s->mv_steps = anim_val(w, rd16(p + 2));
			if (rd16(p + 4)) { /* FUN_1000_4646: the value is in pixels per step */
				int dx = abs(s->mv_tx - s->mv_sx), dy = abs(s->mv_ty - s->mv_sy), d = dx > dy ? dx : dy;
				int sp = s->mv_steps > 0 ? s->mv_steps : 1;
				s->mv_steps = d / sp + (d % sp != 0);
			}
			s->mv_step = 0;
			s->mv_curve = 0;
			s->mv_op = 0x09;
			if (s->mv_steps <= 0) {
				s->pc += n;
				continue;
			}
			s->moving = 1;
			s->visible = 1; /* FUN_1000_6AA4(0, 1, ...) like cmds 01/02 */
			schedule(w, s, s->move_ms);
			continue;
		}
		case 0x05: { /* curved path {steps, relative, xm, ym, x1, y1, ...} from the current position
			(FUN_1000_471A case 5; one step per movement interval like cmd 09) */
			int rel = rd16(p + 4) != 0;
			set_live(s, 1);
			s->mv_sx = s->x;
			s->mv_sy = s->y;
			s->mv_mx = anim_val(w, rd16(p + 6)) + (rel ? s->x : 0);
			s->mv_my = anim_val(w, rd16(p + 8)) + (rel ? s->y : 0);
			s->mv_tx = anim_val(w, rd16(p + 10)) + (rel ? s->x : 0);
			s->mv_ty = anim_val(w, rd16(p + 12)) + (rel ? s->y : 0);
			s->mv_steps = anim_val(w, rd16(p + 2));
			s->mv_step = 0;
			s->mv_curve = 1;
			s->mv_op = 0x05;
			if (s->mv_steps <= 0) { /* the original would divide by 0 */
				s->pc += n;
				continue;
			}
			s->moving = 1;
			s->visible = 1;
			schedule(w, s, s->move_ms);
			continue;
		}
		case 0x0A: /* position {relative, x, y} at the next movement deadline (FUN_1000_4138):
			like a one-step cmd 09 */
			set_live(s, 1);
			s->mv_sx = s->x;
			s->mv_sy = s->y;
			s->mv_tx = anim_val(w, rd16(p + 4)) + (rd16(p + 2) ? s->x : 0);
			s->mv_ty = anim_val(w, rd16(p + 6)) + (rd16(p + 2) ? s->y : 0);
			s->mv_steps = 1;
			s->mv_step = 0;
			s->mv_curve = 0;
			s->mv_op = 0x0A;
			s->mv_rel = rd16(p + 2) != 0;
			s->moving = 1;
			s->visible = 1;
			schedule(w, s, s->move_ms);
			continue;
		case 0x0C: /* position {x, y, relative} (FUN_1000_4446 -> 383C) */
			if (rd16(p + 6)) {
				s->x += anim_val(w, rd16(p + 2));
				s->y += anim_val(w, rd16(p + 4));
			}
			else {
				s->x = anim_val(w, rd16(p + 2));
				s->y = anim_val(w, rd16(p + 4));
			}
			s->pc += n;
			s->after_pos = 1;
			cmd_done(s);
			continue;
		case 0x11: /* {movement interval, frame interval} (S_041, S_036) */
			s->move_ms = anim_val(w, rd16(p + 2));
			if (rd16(p + 4))
				s->frame_ms = anim_val(w, rd16(p + 4));
			/* S_041/S_036 reset both deadlines: +0x18 = now + movement interval, +0x1C = now +
			   frame interval */
			s->next_ms = w->now_ms + (uint32_t)(s->move_ms > 0 ? s->move_ms : 0);
			s->cyc_next = w->now_ms + (uint32_t)(s->frame_ms > 0 ? s->frame_ms : 0);
			s->pc += n;
			continue;
		case 0x10: { /* {start, from, to}: frame cycle during movements (FUN_1000_6A1A) */
			unsigned start = (uint16_t)anim_val(w, rd16(p + 2)), from = (uint16_t)anim_val(w, rd16(p + 4));
			unsigned to = (uint16_t)anim_val(w, rd16(p + 6));
			s->pc += n;
			if (from >= (unsigned)s->n_frames || to >= (unsigned)s->n_frames)
				continue; /* outside the frames: nothing (6A1A returns 0) */
			s->cyc_from = (int)from;
			s->cyc_to = (int)to;
			s->cyc_up = from <= to;
			s->cyc_on = from != to;
			unsigned lo = s->cyc_up ? from : to, hi = s->cyc_up ? to : from;
			s->cyc_cur = start < lo || start > hi ? (int)from : (int)start;
			if (!s->cyc_on) { /* a single frame: +0x5B = 1 shows it at the next redraw */
				s->shown[0] = (int)from;
				s->n_shown = 1;
				resolve_shown(w, s);
			}
			s->cyc_next = w->now_ms;
			continue;
		}
		case 0x0E: /* {raw 0/1}: frozen image / back in view (FUN_1000_6AA4(1, v)) */
			set_live(s, rd16(p + 2) != 0);
			s->pc += n;
			cmd_done(s);
			return;
		case 0x0F: /* {frame interval} (S_036) */
			s->frame_ms = anim_val(w, rd16(p + 2));
			/* S_036 also resets both deadlines (+0x18 = now + movement interval, +0x1C = now +
			   frame interval) */
			s->next_ms = w->now_ms + (uint32_t)(s->move_ms > 0 ? s->move_ms : 0);
			s->cyc_next = w->now_ms + (uint32_t)(s->frame_ms > 0 ? s->frame_ms : 0);
			s->pc += n;
			continue;
		case 0x16: { /* script call {script, n, arguments} (EXE 1008:CC14 via callback) */
			uint8_t cmd[18] = { 0 };
			uint16_t sid = s->id;
			/* FUN_1000_44DC: with an event already running (+0xA82) or events off (+0xA84) it
			   returns 0 without advancing: the command is retried on a later tick */
			if (w->event_busy || w->events_off)
				return;
			memcpy(cmd, p, n < sizeof(cmd) ? n : sizeof(cmd));
			s->pc += n;
			int busy = w->event_busy;
			w->event_busy = 1;
			s->in_event = 1; /* +0x66 = 0: S_001 skips the sprite during the callback */
			rt_anim_call(w->rt, sid, cmd);
			w->event_busy = busy;
			s->in_event = 0;
			/* as for 0x15: the next command starts on the next tick (44DC -> 471A) */
			return;
		}
		case LB7_ANIM_EVENT: {
			uint32_t off = s->pc;
			uint16_t aid = s->anim_id, sid = s->id;
			if (w->event_busy || w->events_off) /* FUN_1000_44DC: retried on a later tick */
				return;
			s->pc += n;
			w->rt->g[5017] = (int16_t)sid; /* DS:3C7C: event's sprite (callback 1008:CC5x) */
			int busy = w->event_busy;
			w->event_busy = 1;
			s->in_event = 1;
			rt_anim_event(w->rt, aid, off);
			w->event_busy = busy;
			s->in_event = 0;
			/* FUN_1000_44DC: after the event the engine prepares the next command, which is
			   executed on the next tick (S_081); the event may also have restarted the sprite */
			return;
		}
		default:
			w->unknown_cmd[op & 0x1F]++;
			s->pc += n;
			continue;
		}
	}
}

/* S_001 (S_081): the sprites are ticked in draw-list order (ascending layer, then insertion
 * order, FUN_1000_10D4), skipping inactive (+0x66) and paused (+0x67) ones. A sprite removed or
 * replaced during the pass (its slot reused: other seq) is skipped. */
void spr_update(SprWorld *w, uint32_t now_ms)
{
	w->now_ms = now_ms;
	Sprite *order[SPR_MAX];
	int seq[SPR_MAX], n = 0;
	for (int i = 0; i < SPR_MAX; i++)
		if (w->spr[i].used && w->spr[i].anim)
			order[n++] = &w->spr[i];
	if (n > 1)
		qsort(order, (size_t)n, sizeof(order[0]), cmp_layer);
	for (int i = 0; i < n; i++)
		seq[i] = order[i]->seq;
	for (int i = 0; i < n; i++) {
		Sprite *s = order[i];
		if (s->used && s->seq == seq[i] && s->anim && !s->paused && !s->in_event)
			run_anim(w, s);
	}
}

/* Decodes (once) the pixels of a frame. */
/* Current text of the type 18 resource (initially the string at +4 of the resource). */
static const char *text_of(SprWorld *w, unsigned id)
{
	for (int i = 0; i < w->n_texts; i++)
		if (w->texts[i].id == id)
			return w->texts[i].s;
	Lb7Entry e = lb7_entry(w->lib, id);
	const uint8_t *d = lb7_entry_data(w->lib, e);
	const char *s = d && e.size >= 6 && w->rt ? rt_string(w->rt, rd16(d + 4)) : NULL;
	return s ? s : "";
}

const char *spr_get_text(SprWorld *w, unsigned id)
{
	return text_of(w, id);
}

/* The frames with this text are redone when drawing (1010:1DE8 rebuilds the bitmap). */
static void text_invalidate(SprWorld *w, unsigned id)
{
	for (int k = 0; k < SPR_MAX; k++)
		for (int j = 0; j < w->spr[k].n_frames; j++)
			if (w->spr[k].used && w->spr[k].frames[j].text_id == id) {
				free(w->spr[k].frames[j].pix);
				w->spr[k].frames[j].pix = NULL;
			}
}

void spr_set_text(SprWorld *w, unsigned id, const char *s)
{
	int i = 0;
	while (i < w->n_texts && w->texts[i].id != id)
		i++;
	if (i == w->n_texts) {
		if (w->n_texts >= 64)
			return;
		w->n_texts++;
	}
	w->texts[i].id = (uint16_t)id;
	snprintf(w->texts[i].s, sizeof(w->texts[i].s), "%s", s);
	text_invalidate(w, id);
}

/* Colour of the text: record word +4, copied from word 0 of the resource by 1010:1CA0 and
 * replaced (raw) by opcode 5E; 1010:1B32 maps the white pixels of the DrawText bitmap to its low
 * byte (MAPBYTES table: 0 -> 0, 0xFF -> colour). */
static uint8_t text_color(SprWorld *w, unsigned id, const uint8_t *d)
{
	for (int i = 0; i < w->n_text_colors; i++)
		if (w->text_colors[i].id == id)
			return w->text_colors[i].color;
	return d[0];
}

void spr_set_text_color(SprWorld *w, unsigned id, uint8_t color)
{
	int i = 0;
	while (i < w->n_text_colors && w->text_colors[i].id != id)
		i++;
	if (i == w->n_text_colors) {
		if (w->n_text_colors >= 64)
			return;
		w->n_text_colors++;
	}
	w->text_colors[i].id = (uint16_t)id;
	w->text_colors[i].color = color;
	text_invalidate(w, id);
}

/* Type 18 member (20 bytes): {color, font, text, height, x, y, width, height of the
 * box, DrawText flags, ...}; anchor point as in FUN_1010_189E. */
static int text_frame(SprWorld *w, SprFrame *f)
{
	Lb7Entry e = lb7_entry(w->lib, f->text_id);
	const uint8_t *d = lb7_entry_data(w->lib, e);
	if (!d || e.size < 0x12 || !w->rt)
		return 0;
	unsigned flags = rd16(d + 0x10);
	int x = (int16_t)rd16(d + 8), y = (int16_t)rd16(d + 0xA), W, H;
	uint8_t *top;
	if (!text_render(rt_string(w->rt, rd16(d + 2)), anim_val(w, rd16(d + 6)), text_of(w, f->text_id),
			(int16_t)rd16(d + 0xC), (int16_t)rd16(d + 0xE), flags, text_color(w, f->text_id, d), &top, &W, &H))
		return 0;
	f->pix = malloc((size_t)W * H);
	if (!f->pix) {
		free(top);
		return 0;
	}
	for (int r = 0; r < H; r++) /* sprite bitmaps are bottom-up */
		memcpy(f->pix + (size_t)(H - 1 - r) * W, top + (size_t)r * W, (size_t)W);
	free(top);
	memset(&f->bmp, 0, sizeof(f->bmp));
	f->bmp.width = (uint16_t)W;
	f->bmp.height = (uint16_t)H;
	f->bmp.stride = (uint16_t)W;
	f->bmp.w5 = (int16_t)(flags & TEXT_DT_RIGHT ? x - (W >> 1) : flags & TEXT_DT_CENTER ? x : x + ((W - 1) >> 1));
	f->bmp.w6 = (int16_t)(flags & TEXT_DT_BOTTOM ? y - (H >> 1) : flags & TEXT_DT_VCENTER ? y : y + ((H - 1) >> 1));
	return 1;
}

static int frame_pix(SprWorld *w, SprFrame *f)
{
	if (f->pix)
		return 1;
	if (f->text_id)
		return text_frame(w, f);
	if (!f->data) /* frame of a kept default sprite that could not be decoded */
		return 0;
	if (w->rt && w->rt->trace) /* diagnostics: first draw of each frame */
		fprintf(w->rt->trace, "%7u    [draw %ux%u anchor %d,%d cel %d,%d flags %X]\n", w->now_ms, f->bmp.width,
			f->bmp.height, f->bmp.w5, f->bmp.w6, f->cx, f->cy, f->bmp.flags);
	f->pix = bitmap_pixels(w, f->data, f->size, &f->bmp); /* also the pixels changed by builtin 7D */
	return f->pix != NULL;
}

/* TOPSYS01 FUN_1000_0B2C: bitmap centered on the position, then shifted by the header's anchor
 * (+0xA, +0xC); the cel adds its own offset. World origin = screen center
 * (the same anchors apply to 640x480 and 800x600). */
static void frame_pos(const Video *v, const Lb7Bitmap *b, int x, int y, int ax, int ay, int fx, int fy,
	int *left, int *top)
{
	/* mirrored: the anchor (ax, ay = anchor + cel offset) changes sign */
	*left = video_width(v) / 2 + x - (b->width - 1) / 2 + (fx ? -ax : ax);
	*top = video_height(v) / 2 + y - (b->height - 1) / 2 + (fy ? -ay : ay);
}

/* Images to draw for a sprite (movie frame or cels shown); calls fn for each one.
 * Color 0 is transparent for all frames, even non-RLE ones (compared with
 * the original: the 392x332 house of TOPSOPN1 has no black box); the movie is opaque. */
static void sprite_images(SprWorld *w, const Video *v, Sprite *s, SprDrawFn fn, void *ctx)
{
	int l, t;
	/* with cmd 0E 0 the frozen image is seen (DLL fields +4/+6 and +0x40) */
	int x = s->frozen ? s->fz_x : s->x, y = s->frozen ? s->fz_y : s->y;
	int fx = s->frozen ? s->fz_flip_x : s->flip_x, fy = s->frozen ? s->fz_flip_y : s->flip_y;
	const int *shown = s->frozen ? s->fz_shown : s->shown;
	int n_shown = s->frozen ? s->fz_n_shown : s->n_shown;
	if (s->mv_frame.pix) {
		frame_pos(v, &s->mv_frame.bmp, x, y, s->mv_frame.bmp.w5, s->mv_frame.bmp.w6, 0, 0, &l, &t);
		fn(ctx, s->mv_frame.pix, &s->mv_frame.bmp, l, t, -1, 0, 0);
		return;
	}
	for (int k = 0; k < n_shown; k++) {
		if (shown[k] < 0 || shown[k] >= s->n_frames)
			continue;
		SprFrame *f = &s->frames[shown[k]];
		if (!f->pix && w->rt && w->rt->trace)
			fprintf(w->rt->trace, "%7u    [sprite %u layer %d frame %d]\n", w->now_ms, s->id,
				s->layer, shown[k]);
		if (!frame_pix(w, f))
			continue;
		/* cel frame (entry 12): its position replaces the image's anchor
		   (faces in the final scene: mouth 4307 lands on body 4142 only without anchor 11) */
		int ax = f->cel ? f->cx : f->bmp.w5, ay = f->cel ? f->cy : f->bmp.w6;
		frame_pos(v, &f->bmp, x, y, ax, ay, fx, fy, &l, &t);
		if (spr_probe_on && w->rt && w->rt->trace) {
			int px = spr_probe_x + video_width(v) / 2, py = spr_probe_y + video_height(v) / 2;
			if (px + spr_probe_r >= l && px - spr_probe_r < l + f->bmp.width && py + spr_probe_r >= t && py - spr_probe_r < t + f->bmp.height)
				fprintf(w->rt->trace, "%7u    [probe sprite %u layer %d seq %d frame %d (entry %u) %ux%u at %d,%d anchor %d,%d cel %d,%d flags %04X]\n",
					w->now_ms, s->id, s->layer, s->seq, shown[k], f->entry, f->bmp.width, f->bmp.height, l, t,
					f->bmp.w5, f->bmp.w6, f->cx, f->cy, f->bmp.flags);
		}
		fn(ctx, f->pix, &f->bmp, l, t, 0, fx, fy);
	}
}

static void draw_to_video(void *ctx, const uint8_t *pix, const Lb7Bitmap *b, int l, int t, int tr, int fx, int fy)
{
	video_blit_flip((Video *)ctx, pix, b, l, t, tr, fx, fy);
}

/* Opcode 47 / command 04 (TOPSYS01 FUN_1000_0426): the sprite's current image is stamped
 * onto the background (here: copied into a list of stamps drawn right after the background). */
static void add_stamp(void *ctx, const uint8_t *pix, const Lb7Bitmap *b, int l, int t, int tr, int fx, int fy)
{
	SprWorld *w = ctx;
	(void)tr;
	if (w->n_stamps >= SPR_MAX_STAMPS)
		return;
	SprStamp *st = &w->stamps[w->n_stamps];
	st->pix = malloc((size_t)b->stride * b->height);
	if (!st->pix)
		return;
	for (unsigned y = 0; y < b->height; y++) { /* the stamp keeps any mirroring */
		const uint8_t *src = pix + (size_t)(fy ? b->height - 1 - y : y) * b->stride;
		uint8_t *dst = st->pix + (size_t)y * b->stride;
		for (unsigned x = 0; x < b->width; x++)
			dst[x] = src[fx ? b->width - 1 - x : x];
	}
	st->bmp = *b;
	st->left = l - video_width(w->last_video) / 2; /* relative to the center, like the world */
	st->top = t - video_height(w->last_video) / 2;
	w->n_stamps++;
}

void spr_stamp(SprWorld *w, const Video *v, unsigned id, int keep)
{
	if (!v)
		return;
	w->last_video = v;
	if (!w->bg[bg_index(w, v)].data) /* FUN_1000_0426: nothing without a background bitmap (+0x448) */
		return;
	for (int i = 0; i < SPR_MAX; i++) {
		Sprite *s = &w->spr[i];
		if (!s->used || !s->visible || (id && s->id != id))
			continue;
		sprite_images(w, v, s, add_stamp, w);
		if (!keep) {
			stop_anim(w, s);
			s->visible = 0;
		}
	}
}

void spr_set_layer(SprWorld *w, unsigned id, int layer)
{
	Sprite *s = find(w, id);
	if (s) {
		s->layer = (int16_t)layer;
		s->seq = ++w->seq; /* reinserted in the list: above those of the same layer (S_065) */
	}
}

int spr_replace(SprWorld *w, unsigned old_id, unsigned new_id)
{
	Sprite *o = find(w, old_id);
	if (!o || !spr_load(w, new_id))
		return 0;
	Sprite *n = find(w, new_id);
	o = find(w, old_id); /* spr_load does not move sprites, but just to be safe */
	if (o != n)
		stop_anim(w, o); /* S_010: the old one's animation (and queued commands) stops */
	int inherited = 0;
	if (!n->n_frames && o->n_frames) { /* FUN_1008_E4F4 + S_045: inherits the images */
		SprFrame *f = calloc((size_t)o->n_frames, sizeof(SprFrame));
		if (!f)
			return 0;
		for (int i = 0; i < o->n_frames; i++) {
			f[i] = o->frames[i];
			f[i].pix = NULL; /* decoded again: the cache stays with the old one */
			f[i].lazy = 0;   /* S_002: direct pointers, not {id, 0} records */
		}
		free(n->frames);
		n->frames = f;
		n->n_frames = o->n_frames;
		memcpy(n->shown, o->shown, sizeof(n->shown));
		n->n_shown = o->n_shown;
		n->x = o->x;
		n->y = o->y;
		n->flip_x = o->flip_x;
		n->flip_y = o->flip_y;
		n->visible = o->visible;
		o->visible = 0;
		inherited = 1;
	}
	else
		o->visible = 0; /* S_025 */
	/* S_044(0, 1, new, old): swap in the draw list only if the new one is above */
	if (cmp_layer(&n, &o) > 0) {
		int l = n->layer, q = n->seq;
		n->layer = o->layer;
		n->seq = o->seq;
		o->layer = l;
		o->seq = q;
	}
	return inherited ? 2 : 1;
}

int spr_loaded(SprWorld *w, unsigned id)
{
	return find(w, id) != NULL;
}

int spr_frame_ids(SprWorld *w, unsigned id, uint16_t *out, int max)
{
	Sprite *s = find(w, id);
	if (!s)
		return -1;
	/* S_023 (TOPSYS01 1000:6CF8): the images shown NOW (one with cmd 01, the list of cmd
	   02), not all the members; opcode 4B (TOPSY 1008:941E) writes its id. The Jungle
	   Pinball spring computes the launch force this way from the current frame. */
	int n = 0;
	for (; n < s->n_shown && n < max; n++) {
		int f = s->shown[n];
		out[n] = f < 0 || f >= s->n_frames ? 0
			: s->frames[f].text_id ? s->frames[f].text_id : s->frames[f].entry;
	}
	return n;
}

int spr_get_pos(SprWorld *w, unsigned id, int *x, int *y)
{
	Sprite *s = find(w, id);
	if (!s)
		return 0;
	*x = s->x;
	*y = s->y;
	return 1;
}

/* Images of a sprite collected by sprite_images (rectangle and collision tests). */
typedef struct {
	const uint8_t *pix;
	const Lb7Bitmap *b;
	int l, t, tr, fx, fy;
} SprImg;

typedef struct {
	SprImg img[SPR_MAX_LAYERS + 1];
	int n;
} SprImgs;

static void collect_image(void *ctx, const uint8_t *pix, const Lb7Bitmap *b, int l, int t, int tr, int fx, int fy)
{
	SprImgs *c = ctx;
	if (c->n < SPR_MAX_LAYERS + 1)
		c->img[c->n++] = (SprImg){ pix, b, l, t, tr, fx, fy };
}

static int images_of(SprWorld *w, Sprite *s, SprImgs *c)
{
	c->n = 0;
	if (w->last_video)
		sprite_images(w, w->last_video, s, collect_image, c);
	return c->n;
}

int spr_get_rect(SprWorld *w, unsigned id, int *l, int *t, int *r, int *b)
{
	Sprite *s = find(w, id);
	SprImgs c;
	if (!s || !images_of(w, s, &c))
		return 0;
	int cx = video_width(w->last_video) / 2, cy = video_height(w->last_video) / 2;
	*l = *t = 32767;
	*r = *b = -32768;
	for (int i = 0; i < c.n; i++) { /* union of the rectangles (fields +8..+0xE) */
		int L = c.img[i].l - cx, T = c.img[i].t - cy;
		if (L < *l) *l = L;
		if (T < *t) *t = T;
		if (L + (int)c.img[i].b->width > *r) *r = L + c.img[i].b->width;
		if (T + (int)c.img[i].b->height > *b) *b = T + c.img[i].b->height;
	}
	return 1;
}

void spr_pause(SprWorld *w, unsigned id, int on)
{
	for (int i = 0; i < SPR_MAX; i++) {
		Sprite *s = &w->spr[i];
		if (!s->used || (id && s->id != id) || !on == !s->paused)
			continue;
		if (on) /* FUN_1000_66F6: on resume the times shift by the duration of the pause */
			s->paused_at = w->now_ms;
		else {
			s->next_ms += w->now_ms - s->paused_at;
			s->mv_next_ms += w->now_ms - s->paused_at;
		}
		s->paused = on;
	}
}

/* Opaque pixel of an image at point (x, y) of the screen. */
static int opaque_at(const SprImg *m, int x, int y)
{
	int col = x - m->l, row = y - m->t;
	if (col < 0 || row < 0 || col >= (int)m->b->width || row >= (int)m->b->height)
		return 0;
	if (m->tr < 0)
		return 1;
	int srow = m->fy ? row : (int)m->b->height - 1 - row;
	return m->pix[(size_t)srow * m->b->stride + (m->fx ? (int)m->b->width - 1 - col : col)] != m->tr;
}

int spr_collide(SprWorld *w, unsigned a, unsigned b, int hide_a, int hide_b)
{
	Sprite *sa = find(w, a), *sb = find(w, b);
	if (!sa || !sb || (!sa->visible && !hide_a) || (!sb->visible && !hide_b))
		return 0;
	SprImgs ca, cb;
	if (!images_of(w, sa, &ca) || !images_of(w, sb, &cb))
		return 0;
	for (int i = 0; i < ca.n; i++)
		for (int j = 0; j < cb.n; j++) {
			const SprImg *p = &ca.img[i], *q = &cb.img[j];
			int x0 = p->l > q->l ? p->l : q->l, y0 = p->t > q->t ? p->t : q->t;
			int x1 = p->l + (int)p->b->width, y1 = p->t + (int)p->b->height;
			if (q->l + (int)q->b->width < x1) x1 = q->l + q->b->width;
			if (q->t + (int)q->b->height < y1) y1 = q->t + q->b->height;
			for (int y = y0; y < y1; y++) /* intersection of the rectangles, then pixels (1000:234F) */
				for (int x = x0; x < x1; x++)
					if (opaque_at(p, x, y) && opaque_at(q, x, y))
						return 1;
		}
	return 0;
}

int spr_get_layer(SprWorld *w, unsigned id, int *layer)
{
	Sprite *s = find(w, id);
	if (s)
		*layer = s->layer;
	return s != NULL;
}

int spr_get_user(SprWorld *w, unsigned id, int16_t *v)
{
	Sprite *s = find(w, id);
	if (s)
		*v = s->user;
	return s != NULL;
}

int spr_set_user(SprWorld *w, unsigned id, int16_t v)
{
	Sprite *s = find(w, id);
	if (s)
		s->user = v;
	return s != NULL;
}

int spr_visible(SprWorld *w, unsigned id)
{
	Sprite *s = find(w, id);
	return s && s->visible;
}

/* S_051 (TOPSYS01 1000:0058): relative move; with a 05 or 09 running its targets move too (05:
 * middle and end point, 09: end point; the start is not shifted). For the 0A, decoded here at
 * the start, a relative jump is computed by the original from the position at the deadline. */
void spr_move_rel(SprWorld *w, unsigned id, int dx, int dy)
{
	Sprite *s = find(w, id);
	if (s) {
		s->x += dx;
		s->y += dy;
		if (s->moving && (s->mv_op == 0x05 || s->mv_op == 0x09 || (s->mv_op == 0x0A && s->mv_rel))) {
			s->mv_tx += dx;
			s->mv_ty += dy;
			if (s->mv_op == 0x05) {
				s->mv_mx += dx;
				s->mv_my += dy;
			}
		}
	}
}

/* S_085 (builtin 98) -> FUN_1000_6736 for every sprite: position, start and targets of the
 * running 05/09 are shifted by (dx, dy). The original also scrolls the window (ScrollDC) and
 * repaints the uncovered strips: here the whole screen is redrawn every frame. */
void spr_scroll_all(SprWorld *w, int dx, int dy)
{
	for (int i = 0; i < SPR_MAX; i++) {
		Sprite *s = &w->spr[i];
		if (!s->used)
			continue;
		s->x += dx;
		s->y += dy;
		if (s->moving) {
			s->mv_sx += dx;
			s->mv_sy += dy;
			s->mv_tx += dx;
			s->mv_ty += dy;
			s->mv_mx += dx;
			s->mv_my += dy;
		}
	}
}

/* S_056: clip rectangle (world +0x86A..+0x870); valid only if x1 < x2 and y1 < y2, then made
 * inclusive (x2 + 1, y2 + 1) and intersected with the screen by FUN_1000_0354. */
void spr_set_clip(SprWorld *w, int x1, int y1, int x2, int y2)
{
	w->clip_on = x1 < x2 && y1 < y2;
	w->clip[0] = x1;
	w->clip[1] = y1;
	w->clip[2] = x2 + 1;
	w->clip[3] = y2 + 1;
}

/* S_053: zoom factor (+0x876) and offsets (+0x878/+0x87A = offset * zoom - 1); "zoomed" only if
 * not (zoom 0, or zoom 1 without offset). */
void spr_set_zoom(SprWorld *w, int zoom, int ox, int oy)
{
	if (zoom < 0 || zoom > 4)
		zoom = 0;
	int on = !(zoom == 0 || (zoom == 1 && ox == 0 && oy == 0));
	w->zoom = on ? zoom : 0;
	w->zoom_x = on ? ox : 0;
	w->zoom_y = on ? oy : 0;
}

/* Hit test on a sprite image (TOPSYS01 FUN_1000_3096): inside the rectangle and,
 * without hit_box, on a non-transparent pixel (color 0; the movie is opaque). */
typedef struct {
	int x, y;               /* point in screen coordinates */
	int box, hit;
} HitCtx;

static void hit_image(void *ctx, const uint8_t *pix, const Lb7Bitmap *b, int l, int t, int tr, int fx, int fy)
{
	HitCtx *h = ctx;
	int col = h->x - l, row = h->y - t;
	if (h->hit || col < 0 || row < 0 || col >= (int)b->width || row >= (int)b->height)
		return;
	if (h->box || tr < 0) {
		h->hit = 1;
		return;
	}
	int srow = fy ? row : (int)b->height - 1 - row; /* row 0 of the bitmap is at the bottom */
	uint8_t c = pix[(size_t)srow * b->stride + (fx ? (int)b->width - 1 - col : col)];
	h->hit = c != tr;
}

unsigned spr_hit(SprWorld *w, int x, int y)
{
	if (!w->last_video)
		return 0;
	Sprite *order[SPR_MAX];
	int n = 0;
	for (int i = 0; i < SPR_MAX; i++) {
		Sprite *s = &w->spr[i];
		/* S_029: state 2 never; otherwise only if visible, or state 1 */
		/* the kept default sprite has no resource any more: nothing to click */
		if (s->used && !s->orphan && s->hit_state != 2 && (s->visible || s->hit_state == 1))
			order[n++] = s;
	}
	qsort(order, (size_t)n, sizeof(order[0]), cmp_layer);
	for (int i = n - 1; i >= 0; i--) { /* from the top, like the draw list scanned backwards */
		HitCtx h = { video_width(w->last_video) / 2 + x, video_height(w->last_video) / 2 + y,
			order[i]->hit_box, 0 };
		sprite_images(w, w->last_video, order[i], hit_image, &h);
		if (h.hit)
			return order[i]->id;
	}
	return 0;
}

int spr_hit_id(SprWorld *w, unsigned id, int x, int y)
{
	Sprite *s = find(w, id);
	if (!w->last_video || !s || s->hit_state == 2 || !(s->visible || s->hit_state == 1))
		return 0; /* same rule as S_029 */
	HitCtx h = { video_width(w->last_video) / 2 + x, video_height(w->last_video) / 2 + y, s->hit_box, 0 };
	sprite_images(w, w->last_video, s, hit_image, &h);
	return h.hit;
}

void spr_break_loops(SprWorld *w, unsigned id, int on)
{
	Sprite *s = find(w, id);
	if (s)
		s->break_loops = on;
}

static int cmp_layer(const void *a, const void *b)
{
	const Sprite *x = *(Sprite *const *)a, *y = *(Sprite *const *)b;
	if (x->pinned != y->pinned) /* S_042: the pinned sprite keeps the last slot (FUN_1000_10D4) */
		return x->pinned ? 1 : -1;
	if (x->layer != y->layer)
		return x->layer < y->layer ? -1 : 1;
	return x->seq < y->seq ? -1 : x->seq > y->seq;
}

void spr_draw(SprWorld *w, Video *v)
{
	w->last_video = v;
	SprFrame *bg = &w->bg[bg_index(w, v)];
	int vw = video_width(v), vh = video_height(v);
	uint8_t *fb = video_pixels(v);
	size_t fbsize = (size_t)vw * (size_t)vh;
	/* composition buffer of the DLL (+0x444): it survives between frames; needed when the
	   background is not cleared (op 06 mode 2) or only the clip rectangle is repainted (op 2A) */
	int keep = (w->bg_keep || w->clip_on) && w->comp && w->comp_w == vw && w->comp_h == vh;
	uint8_t *prev = NULL;
	if (keep) {
		memcpy(fb, w->comp, fbsize);
		if (w->clip_on && (prev = malloc(fbsize)) != NULL)
			memcpy(prev, w->comp, fbsize);
	}
	if (w->bg_keep && keep)
		; /* +0x880 = 0xFFFF: FUN_1000_3152 is skipped, the old pixels stay */
	else if (w->bg_canvas && w->bg_cw == vw && w->bg_ch == vh)
		memcpy(fb, w->bg_canvas, fbsize); /* background composed by an opcode 06 transition */
	else if (!bg->data)
		video_clear(v, (uint8_t)w->bg_color);
	else {
		if (!bg->pix)
			bg->pix = bitmap_pixels(w, bg->data, bg->size, &bg->bmp);
		if (bg->pix) /* centered like a sprite at 0,0 (FUN_1000_375A with the buffer origin at
			   (W-1)/2 and the screen origin at W/2): at 800x600 at 1,1, row and column 0 black */
			video_blit(v, bg->pix, &bg->bmp, video_width(v) / 2 - ((int)bg->bmp.width - 1) / 2,
				video_height(v) / 2 - ((int)bg->bmp.height - 1) / 2, -1);
	}
	for (int i = 0; i < w->n_stamps; i++)
		video_blit(v, w->stamps[i].pix, &w->stamps[i].bmp, video_width(v) / 2 + w->stamps[i].left,
			video_height(v) / 2 + w->stamps[i].top, 0);
	Sprite *order[SPR_MAX];
	int n = 0;
	for (int i = 0; i < SPR_MAX; i++)
		if (w->spr[i].used && w->spr[i].visible)
			order[n++] = &w->spr[i];
	qsort(order, (size_t)n, sizeof(order[0]), cmp_layer);
	for (int i = 0; i < n; i++)
		sprite_images(w, v, order[i], draw_to_video, v);
	if (prev) { /* S_056: outside the clip rectangle nothing is repainted */
		int x1 = vw / 2 + w->clip[0], y1 = vh / 2 + w->clip[1], x2 = vw / 2 + w->clip[2], y2 = vh / 2 + w->clip[3];
		for (int y = 0; y < vh; y++)
			for (int x = 0; x < vw; x++)
				if (x < x1 || x >= x2 || y < y1 || y >= y2)
					fb[(size_t)y * vw + x] = prev[(size_t)y * vw + x];
		free(prev);
	}
	if (w->bg_keep || w->clip_on) {
		if (!w->comp || w->comp_w != vw || w->comp_h != vh) {
			free(w->comp);
			w->comp = malloc(fbsize);
			w->comp_w = vw;
			w->comp_h = vh;
		}
		if (w->comp)
			memcpy(w->comp, fb, fbsize);
	}
	if (w->zoom > 0) { /* S_053 -> FUN_1000_3926: StretchDIBits, dest = zoom * pos - (offset * zoom - 1) */
		uint8_t *src = malloc(fbsize);
		if (src) {
			memcpy(src, fb, fbsize);
			int z = w->zoom, ox = w->zoom_x * z - 1, oy = w->zoom_y * z - 1;
			for (int y = 0; y < vh; y++) {
				int sy = y + oy >= 0 ? (y + oy) / z : -1;
				for (int x = 0; x < vw; x++) {
					int sx = x + ox >= 0 ? (x + ox) / z : -1;
					fb[(size_t)y * vw + x] = sx >= 0 && sx < vw && sy >= 0 && sy < vh ? src[(size_t)sy * vw + sx] : 0;
				}
			}
			free(src);
		}
	}
}

/* ---------------------------------------------------------------- builtin 7D */

static int edited_find(SprWorld *w, const uint8_t *data)
{
	for (int i = 0; i < w->n_edited; i++)
		if (w->edited[i].data == data)
			return i;
	return -1;
}

/* Frames (and background) that use the bitmap redo their pixels at the next draw. */
static void edited_refresh(SprWorld *w, const uint8_t *data)
{
	for (int k = 0; k < SPR_MAX; k++)
		for (int j = 0; w->spr[k].used && j < w->spr[k].n_frames; j++) {
			SprFrame *f = &w->spr[k].frames[j];
			if (!f->text_id && f->data == data) {
				free(f->pix);
				f->pix = NULL;
			}
		}
	for (int k = 0; k < 2; k++)
		if (w->bg[k].data == data) {
			free(w->bg[k].pix);
			w->bg[k].pix = NULL;
		}
}

/* Pixels (bottom-up) of a bitmap: the copy changed by 7D if there is one, otherwise decoded from
 * the library. The caller frees them. */
static uint8_t *bitmap_pixels(SprWorld *w, const uint8_t *data, uint32_t size, const Lb7Bitmap *b)
{
	size_t n = (size_t)b->stride * b->height;
	uint8_t *pix = malloc(n ? n : 1);
	if (!pix)
		return NULL;
	int e = edited_find(w, data);
	if (e >= 0)
		memcpy(pix, w->edited[e].pix, n);
	else if (!lb7_bitmap_decode(data, size, b, pix)) {
		free(pix);
		return NULL;
	}
	return pix;
}

/* Source of 1000:2890: mask of the pixels that are written. Raw bitmaps skip index 0; in RLE
 * rows, runs of 0 (byte < 0x80) and the zeros of long literals (0xFF, count) skip, while short
 * literals (0x80..0xFE, ~byte bytes) are copied as they are, zeros included (1000:28ED, 29B0). */
static void rle_mask(const uint8_t *d, uint32_t size, const Lb7Bitmap *b, uint8_t *mask)
{
	size_t si = b->hdr_size + b->height * 2u;
	for (unsigned y = 0; y < b->height; y++) {
		uint8_t *row = mask + (size_t)y * b->stride, *end = row + b->stride;
		for (;;) {
			if (si >= size)
				return;
			uint8_t c = d[si++];
			if (c == 0)
				break;
			if (c < 0x80) {
				if (si >= size || row + c > end)
					return;
				memset(row, d[si++] != 0, c);
				row += c;
				continue;
			}
			unsigned n = (uint8_t)~c;
			int keep_zero = 1;
			if (c == 0xFF) {
				if (si >= size)
					return;
				n = d[si++];
				keep_zero = 0;
			}
			if (si + n > size || row + n > end)
				return;
			for (unsigned i = 0; i < n; i++)
				row[i] = keep_zero || d[si + i] != 0;
			row += n;
			si += n;
		}
	}
}

/* S_076: rectangle of a bitmap centred on its anchor (+0xA, +0xC), right/bottom excluded. */
static void bitmap_rect(const Lb7Bitmap *b, int r[4])
{
	r[0] = b->w5 + (1 - (int)b->width) / 2;
	r[1] = b->w6 - ((int)b->height - 1) / 2;
	r[2] = r[0] + b->width;
	r[3] = r[1] + b->height;
}

int spr_bitmap_blit(SprWorld *w, unsigned dest, unsigned src, int dx, int dy)
{
	/* destination: type 1 only (FUN_1008_740E with type 1) */
	Lb7Entry de = lb7_entry(w->lib, dest);
	const uint8_t *dd = lb7_entry_data(w->lib, de);
	Lb7Bitmap db;
	if (!dd || de.type != 1 || !lb7_bitmap_header(dd, de.size, &db))
		return 0;
	/* source: bitmap (1) or text (18) */
	Lb7Entry se = lb7_entry(w->lib, src);
	const uint8_t *sd = lb7_entry_data(w->lib, se);
	SprFrame sf = { 0 };
	uint8_t *mask = NULL;
	if (sd && se.type == 1 && lb7_bitmap_header(sd, se.size, &sf.bmp)) {
		sf.pix = bitmap_pixels(w, sd, se.size, &sf.bmp);
		mask = sf.pix ? malloc((size_t)sf.bmp.stride * sf.bmp.height) : NULL;
		if (mask) {
			size_t n = (size_t)sf.bmp.stride * sf.bmp.height;
			if ((sf.bmp.flags & LB7_BMP_RLE) && edited_find(w, sd) < 0) {
				memset(mask, 0, n);
				rle_mask(sd, se.size, &sf.bmp, mask);
			}
			else
				for (size_t i = 0; i < n; i++)
					mask[i] = sf.pix[i] != 0;
		}
	}
	else if (sd && se.type == 18) {
		sf.text_id = (uint16_t)src;
		if (text_frame(w, &sf) && (mask = malloc((size_t)sf.bmp.stride * sf.bmp.height)) != NULL)
			for (size_t i = 0; i < (size_t)sf.bmp.stride * sf.bmp.height; i++)
				mask[i] = sf.pix[i] != 0;
	}
	if (!mask) {
		free(sf.pix);
		return 0;
	}
	/* OffsetRect + IntersectRect */
	int rd[4], rs[4], ri[4];
	bitmap_rect(&db, rd);
	bitmap_rect(&sf.bmp, rs);
	rs[0] += dx, rs[2] += dx, rs[1] += dy, rs[3] += dy;
	ri[0] = rd[0] > rs[0] ? rd[0] : rs[0];
	ri[1] = rd[1] > rs[1] ? rd[1] : rs[1];
	ri[2] = rd[2] < rs[2] ? rd[2] : rs[2];
	ri[3] = rd[3] < rs[3] ? rd[3] : rs[3];
	int drawn = 0;
	if (ri[0] < ri[2] && ri[1] < ri[3]) {
		int e = edited_find(w, dd);
		if (e < 0 && w->n_edited < (int)(sizeof(w->edited) / sizeof(w->edited[0]))) {
			uint8_t *pix = bitmap_pixels(w, dd, de.size, &db);
			if (pix) {
				e = w->n_edited++;
				w->edited[e].id = (uint16_t)dest;
				w->edited[e].data = dd;
				w->edited[e].bmp = db;
				w->edited[e].pix = pix;
			}
		}
		if (e >= 0) {
			/* rows top-down in rectangle coordinates; memory bottom-up */
			int width = ri[2] - ri[0], height = ri[3] - ri[1];
			int dx0 = ri[0] - rd[0], dy0 = ri[1] - rd[1], sx0 = ri[0] - rs[0], sy0 = ri[1] - rs[1];
			for (int y = 0; y < height; y++) {
				uint8_t *drow = w->edited[e].pix + (size_t)(db.height - 1 - (dy0 + y)) * db.stride + dx0;
				size_t so = (size_t)(sf.bmp.height - 1 - (sy0 + y)) * sf.bmp.stride + sx0;
				for (int x = 0; x < width; x++)
					if (mask[so + x])
						drow[x] = sf.pix[so + x];
			}
			edited_refresh(w, dd);
			drawn = 1;
		}
	}
	if (w->rt && w->rt->trace)
		fprintf(w->rt->trace, "%7u    [7D: bitmap %u into %u at %d,%d: %s]\n", w->now_ms, src, dest, dx, dy,
			drawn ? "drawn" : "no overlap");
	free(mask);
	free(sf.pix);
	return drawn;
}

/* Is the bitmap a member of a loaded sprite (count entry[2] of the resource table)? */
static int bitmap_held(SprWorld *w, const uint8_t *data)
{
	for (int k = 0; k < SPR_MAX; k++)
		for (int j = 0; w->spr[k].used && j < w->spr[k].n_frames; j++)
			if (!w->spr[k].frames[j].text_id && w->spr[k].frames[j].data == data)
				return 1;
	return 0;
}

/* The original frees a type 1 resource (FUN_1008_60EE) only when no sprite holds it any more;
 * the next load reads the file again. id = 0: every changed bitmap no longer held. */
static void edited_release(SprWorld *w, unsigned id)
{
	for (int i = 0; i < w->n_edited; i++)
		if ((!id || w->edited[i].id == id) && !bitmap_held(w, w->edited[i].data)) {
			const uint8_t *data = w->edited[i].data;
			free(w->edited[i].pix);
			w->edited[i--] = w->edited[--w->n_edited];
			edited_refresh(w, data);
		}
}

void spr_bitmap_forget(SprWorld *w, unsigned id)
{
	if (id)
		edited_release(w, id);
}

int spr_has_default(SprWorld *w)
{
	for (int i = 0; i < SPR_MAX; i++)
		if (w->spr[i].used && w->spr[i].is_def)
			return 1;
	return 0;
}

void spr_default_reset(SprWorld *w, int destroy)
{
	for (int i = 0; i < SPR_MAX; i++) {
		Sprite *s = &w->spr[i];
		if (!s->used || !s->is_def)
			continue;
		if (destroy) {
			if (w->rt && w->rt->trace)
				fprintf(w->rt->trace, "%7u    [default sprite %u%s destroyed]\n", w->now_ms, s->id,
					s->orphan ? " (old library)" : "");
			/* S_013(1, 1): FUN_1000_119A releases the lazy frames resolved (6618 -> callback 1);
			   not for a sprite kept from an old library: its ids would name resources of the
			   new one (the original would release those) */
			for (int k = 0; k < s->lz_n && w->rt && !s->orphan; k++)
				(void)rt_lazy_frame(w->rt, s->lz_id[k], 1);
			spr_free(w, s); /* hidden, images freed, out of the list */
			edited_release(w, 0);
		}
		else
			s->is_def = 0;
	}
}

int spr_default_detach(SprWorld *w, Sprite *out)
{
	Sprite *s = NULL;
	for (int i = 0; i < SPR_MAX && !s; i++)
		if (w->spr[i].used && w->spr[i].is_def)
			s = &w->spr[i];
	if (!s)
		return 0;
	movie_stop(w, s); /* the movie reads the old library's file */
	for (int i = 0; i < s->n_frames; i++) {
		SprFrame *f = &s->frames[i];
		(void)frame_pix(w, f); /* texts are rendered now: their resource goes away */
		f->data = NULL;
		f->size = 0;
		f->text_id = 0;
		f->lazy = 0;
	}
	s->orphan = 1;
	s->pinned = 1; /* S_042 at the start of FUN_1008_C5F8 */
	s->layer = -1;
	if (w->rt && w->rt->trace)
		fprintf(w->rt->trace, "%7u    [default sprite %u kept across the library change, %d frames]\n", w->now_ms,
			s->id, s->n_frames);
	*out = *s;
	memset(s, 0, sizeof(*s)); /* its memory now belongs to out */
	return 1;
}

void spr_default_attach(SprWorld *w, const Sprite *s)
{
	for (int i = 0; i < SPR_MAX; i++)
		if (!w->spr[i].used) {
			w->spr[i] = *s;
			w->spr[i].seq = ++w->seq;
			w->spr[i].break_loops = 1; /* S_004(1) after FUN_1008_C474 */
			return;
		}
	Sprite tmp = *s;
	spr_free(w, &tmp);
}

void spr_set_lazy(SprWorld *w, unsigned sprite, unsigned member)
{
	Sprite *s = find(w, sprite);
	for (int i = 0; s && i < s->n_frames; i++)
		if (!s->frames[i].text_id && s->frames[i].entry == member)
			s->frames[i].lazy = 1;
}

/* FUN_1000_6674: a lazy frame that becomes current is looked up in the sprite's cache (a hit
 * moves it to the front, FUN_1000_650E); otherwise the EXE loads it (FUN_1000_6598 -> callback
 * FUN_1008_7780(0, id)) and it goes to the front, the oldest of 20 being released
 * (FUN_1000_649C -> 665A -> callback 1). */
static void resolve_frame(SprWorld *w, Sprite *s, int idx)
{
	if (idx < 0 || idx >= s->n_frames || !s->frames[idx].lazy || !w->rt)
		return;
	uint16_t id = s->frames[idx].entry;
	int k = 0;
	while (k < s->lz_n && s->lz_id[k] != id)
		k++;
	if (k == s->lz_n) {
		if (!rt_lazy_frame(w->rt, id, 0))
			return;
		if (s->lz_n == 20)
			(void)rt_lazy_frame(w->rt, s->lz_id[--s->lz_n], 1);
		k = s->lz_n++;
	}
	memmove(s->lz_id + 1, s->lz_id, (size_t)k * sizeof(s->lz_id[0]));
	s->lz_id[0] = id;
}

static void resolve_shown(SprWorld *w, Sprite *s)
{
	for (int k = 0; k < s->n_shown; k++)
		resolve_frame(w, s, s->shown[k]);
}

void spr_resolve(SprWorld *w, unsigned sprite)
{
	Sprite *s = find(w, sprite);
	if (s)
		resolve_shown(w, s);
}
