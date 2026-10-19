#include "rt.h"
#include "audio.h"
#include "cov.h"
#include "sprite.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern const RtLib *const game_libs[]; /* port\game\game_libs.c */

/* Saved state of a visited library (0x1A-byte record of FUN_1008_C282). */
typedef struct LibState {
	char file[32];
	int16_t g[RT_NGLOBALS];
	char (*str)[128];
	uint16_t n_str;
	struct LibState *next;
} LibState;

static LibState *states;
static void run_timers(Rt *rt);
static void check_hits(Rt *rt);
static void pad_timers(Rt *rt);
static void sound_reset(Rt *rt);
static void sound_check(Rt *rt);
static char *str_dest(Rt *rt, unsigned id);
static void queue_call(Rt *rt, uint16_t script, uint16_t a1, uint16_t a2);
static void idle_reset(Rt *rt);
static void apply_volume(Rt *rt);
static void pause_waves(Rt *rt, int on);
static void pause_midi(Rt *rt, int on);
static void pad_release(Rt *rt, unsigned p);
static void pad_enable(Rt *rt, struct RtPad *d, int on);
static int ini_get(const Rt *rt, const char *sec, const char *key, char *out, size_t cap);
static void ini_put(const Rt *rt, const char *sec, const char *key, const char *val);
static void copy127(char *dst, const char *src);
static char datadir[512];
static char prev_file[32];

/* ---------------------------------------------------------------- diagnostics */

static void tr(const Rt *rt, const char *f, ...) __attribute__((format(printf, 2, 3)));
static void tr(const Rt *rt, const char *f, ...)
{
	if (!rt->trace)
		return;
	va_list ap;
	va_start(ap, f);
	fprintf(rt->trace, "%7u %*s", rt->now_ms, rt->depth * 2, ""); /* game-time ms */
	vfprintf(rt->trace, f, ap);
	va_end(ap);
	fflush(rt->trace);
}

void rt_report(const Rt *rt, FILE *out)
{
	fprintf(out, "instructions with handler not yet ported:");
	int any = 0;
	for (int i = 0; i < 256; i++)
		if (rt->unimpl_op[i]) {
			const char *nm = lb7_op_name((unsigned)i);
			fprintf(out, " %02X%s%s%s x%u", i, nm ? "(" : "", nm ? nm : "", nm ? ")" : "", rt->unimpl_op[i]);
			any = 1;
		}
	fprintf(out, "%s\nbuiltins not yet ported:", any ? "" : " none");
	any = 0;
	for (int i = 0; i < 256; i++)
		if (rt->unimpl_builtin[i]) {
			fprintf(out, " %02X x%u", i, rt->unimpl_builtin[i]);
			any = 1;
		}
	fprintf(out, "%s\n", any ? "" : " none");
}

/* ---------------------------------------------------------------- libraries */

int rt_init(Rt *rt, const char *dir, FILE *trace)
{
	memset(rt, 0, sizeof(*rt));
	rt->trace = trace;
	rt->screen_w = 640;
	rt->screen_h = 480;
	rt->vol_wave = rt->vol_midi = 31;
	rt->faded = 1; /* DAT_5B6D = 1 at initialization: the first start does a fade in */
	rt->active = rt->active_audio = 1;
	rt->max_joy = 2;
	/* DS:0042, initial data of TOPSY.EXE: set 0 = F V C B (up down left right) and 1 2 3 4,
	   set 1 = arrows and 9 0 - = (codes 0x10 up, 0x20 down, 0x80 left, 0x40 right, buttons 1 2 4 8) */
	static const uint8_t keyset0[2][16] = {
		{ 'F', 0x10, 'V', 0x20, 'C', 0x80, 'B', 0x40, '1', 1, '2', 2, '3', 4, '4', 8 },
		{ 0x26, 0x10, 0x28, 0x20, 0x25, 0x80, 0x27, 0x40, '9', 1, '0', 2, 0xBD, 4, 0xBB, 8 },
	};
	memcpy(rt->keyset, keyset0, sizeof(rt->keyset));
	snprintf(datadir, sizeof(datadir), "%s", dir ? dir : ".");
	return 1;
}

void rt_shutdown(Rt *rt)
{
	lb7_close(&rt->lib);
	free(rt->obj);
	rt->obj = NULL;
	rt->n_obj = 0;
	while (states) {
		LibState *n = states->next;
		free(states->str);
		free(states);
		states = n;
	}
	rt->g = NULL;
	rt->str = NULL;
}

static LibState *state_for(const Rt *rt, const char *file, int *created)
{
	for (LibState *s = states; s; s = s->next)
		if (!strcasecmp(s->file, file)) {
			*created = 0;
			return s;
		}
	LibState *s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	snprintf(s->file, sizeof(s->file), "%s", file);
	s->n_str = lb7_rd16(rt->lib.data + 0xCA);
	if (s->n_str)
		s->str = calloc(s->n_str, 128);
	s->next = states;
	states = s;
	*created = 1;
	return s;
}

int rt_open_library(Rt *rt, const char *file)
{
	const RtLib *code = NULL;
	for (int i = 0; game_libs[i]; i++)
		if (!strcasecmp(game_libs[i]->file, file))
			code = game_libs[i];
	if (!code) {
		tr(rt, "!! library %s: translated logic missing (regenerate with tools\\gen_game.bat)\n", file);
		return 0;
	}
	char path[1024], err[512];
	snprintf(path, sizeof(path), "%s/%s", datadir, file);
	lb7_close(&rt->lib);
	if (!lb7_open(&rt->lib, path, err, sizeof(err))) {
		tr(rt, "!! %s\n", err);
		return 0;
	}
	if (rt->lib.n_entries != code->n_entries)
		tr(rt, "!! %s: %u entries in the file but %u in the translated logic\n", file, rt->lib.n_entries,
			code->n_entries);
	int created;
	LibState *s = state_for(rt, file, &created);
	if (!s)
		return 0;
	/* G(5001..) shared: carried over from the previous library to the new one */
	if (rt->g && rt->g != s->g)
		memcpy(s->g + RT_GSHARED, rt->g + RT_GSHARED, (RT_NGLOBALS - RT_GSHARED) * sizeof(int16_t));
	rt->code = code;
	rt->g = s->g;
	rt->str = s->str;
	rt->n_str = s->n_str;
	if (created) {
		/* initial variable values: key 3 = {word index, word value} (RESENUMVARIABLES) */
		uint32_t off = rt->lib.key_off[3], n = rt->lib.key_size[3] / 4;
		for (uint32_t i = 0; i < n && (uint64_t)off + i * 4 + 4 <= rt->lib.size; i++) {
			uint16_t idx = lb7_rd16(rt->lib.data + off + i * 4);
			if (idx < RT_GSHARED)
				rt->g[idx] = (int16_t)lb7_rd16(rt->lib.data + off + i * 4 + 2);
			else if (idx < RT_NGLOBALS)
				tr(rt, "   key 3: initial value for shared global %u ignored\n", idx);
		}
	}
	/* system variables (TOPSY 1008:BF32, run on every library open) */
	/* LoMem (low memory = reduced detail): GetPrivateProfileInt in the section DS:5B98
	   ("Topsy Turvy", string 0xD2 of the EXE), default DAT_5B81 = 0; read in rt_load_settings */
	rt->g[5002] = rt->lomem; /* DAT_3C5E */
	rt->g[5003] = (int16_t)rt->screen_w;  /* GetDeviceCaps HORZRES: >= 800 selects 800x600 */
	rt->g[5004] = (int16_t)rt->screen_h;  /* VERTRES */
	rt->g[5005] = (int16_t)rt->mouse_x;   /* cursor (GetCursorPos - area origin) */
	rt->g[5006] = (int16_t)rt->mouse_y;
	rt->g[5011] = 0;                      /* DAT_153E: w/k/s command-line option */
	rt->g[5012] = 1;                      /* DAT_5B67: wave audio available */
	rt->g[5013] = 5;                      /* GetProcessorType: Pentium or better */
	rt->g[5014] = 1;                      /* DAT_5B78: screen with more than 256 colors */
	rt->g[5015] = 1;                      /* DAT_5B7C: default 1 */
	rt->g[5016] = (int16_t)rt->joy_n;     /* DAT_40D4: joysticks found (JOYSETCAPTURE succeeded) */
	if (rt->spr) {
		spr_clear(rt->spr);
		spr_init(rt->spr, &rt->lib, rt);
	}
	/* resets of FUN_1008_C474: key table (DS:4BFB), clickable areas (DS:56FB),
	   script of the sprite under the cursor, deferred call queue (DAT_40E0) */
	rt->v1504 = rt->v1506 = rt->v1508 = rt->v150e = 0;
	memset(rt->table4090, 0, sizeof(rt->table4090));
	rt->timer_script = 0;
	memset(rt->key_script, 0, sizeof(rt->key_script));
	memset(rt->key_off, 0, sizeof(rt->key_off));
	rt->text_id = 0; /* DAT_1500 */
	rt->n_hot = 0;
	rt->n_zones = rt->zone_cur = 0; /* DAT_151E, DAT_1538 */
	rt->hover_script = rt->hover_sprite = rt->drag_sprite = 0;
	rt->evq_n = 0;
	rt->n_timers = 0; /* DAT_1528 */
	rt->timers_paused = 0; /* DAT_5B87 = 0 in C474: the menu pause (builtin 6D) does not survive the link */
	rt->n_hits = 0;   /* DS:5707 */
	/* C474 turns off the use of keyboard and mouse as controllers; the DS:44EA records remain */
	rt->pad_kbd = rt->pad_mouse = rt->pad_joy = 0;
	rt->joy_alt = 0;
	sound_reset(rt);  /* FUN_1008_C5F8: resources unloaded, pending 0x52C removed, A_041 */
	/* sprite input data: the library reopens the resources, hence the values from the file */
	free(rt->obj);
	rt->n_obj = rt->lib.n_entries + rt->lib.extra_slots; /* including the copies created at runtime */
	rt->obj = calloc(rt->n_obj ? rt->n_obj : 1, sizeof(RtObj));
	for (uint32_t i = 0; rt->obj && i < rt->n_obj; i++) {
		Lb7Entry e = lb7_entry(&rt->lib, i);
		const uint8_t *d = e.type == 17 && e.size >= 0x14 ? lb7_entry_data(&rt->lib, e) : NULL;
		if (!d)
			continue;
		RtObj *o = &rt->obj[i];
		o->click = lb7_rd16(d);
		o->click_arg = lb7_rd16(d + 2);
		o->drag = lb7_rd16(d + 8);
		o->drop = lb7_rd16(d + 0xA);
		o->click_off = d[0x10];
		o->draggable = d[0x13];
	}
	tr(rt, "== library %s (%u entries, %s)\n", file, rt->lib.n_entries, created ? "new" : "resumed");
	cov_library(file, &rt->lib, rt->code);
	/* startup script: entry 0 (DAT_1510 + 0x8ACF, DAT_1510 = 0) */
	rt->stop = 0;
	rt->fade_pending = 1;
	rt->in_startup = 1;
	/* G(0): the null operand of a sprite level (+0xE = 0) is read as G(0) (1008:6C08);
	   no script writes it, so it is 0. UPSY_G0=N changes it for experiments (investigation of the
	   faces of TOPSFINA, DA_FARE.md). */
	if (created && getenv("UPSY_G0"))
		rt->g[0] = (int16_t)atoi(getenv("UPSY_G0"));
	rt_run(rt, 0, rt->lstack);
	rt->in_startup = 0;
	if (rt->fade_pending) { /* FUN_1008_C5F8 after C474: fade in 100 unless startup cancelled it */
		tr(rt, "   automatic fade in on open (DAT_5B75)\n");
		rt_fade(rt, 1, 100);
	}
	return 1;
}

void rt_tick(Rt *rt, uint32_t now_ms)
{
	rt->now_ms = now_ms;
	sound_check(rt); /* end of sounds -> deferred call queue (message 0x52C) */
	/* opcode 0x15: script to run as soon as the engine regains control (TOPSY.EXE
	 * runs DAT_1502 and clears it: in TOPSY.BIN it is script 1, after startup) */
	if (rt->v1502) {
		unsigned id = (uint16_t)rt_operand(rt, rt->lstack, (uint16_t)rt->v1502);
		rt->v1502 = 0;
		tr(rt, "-- deferred script %u (DAT_1502)\n", id);
		rt_run(rt, id, rt->lstack);
	}
	if (rt->evq_n) { /* deferred calls (opcode 1E) */
		uint16_t q[40][3];
		int n = rt->evq_n;
		memcpy(q, rt->evq, sizeof(q));
		rt->evq_n = 0;
		for (int i = 0; i < n && !rt->link_pending; i++) {
			unsigned id = (uint16_t)rt_operand(rt, rt->lstack, q[i][0]);
			int16_t a[2];
			unsigned argc = 0;
			for (int k = 1; k <= 2; k++)
				if (q[i][k])
					a[argc++] = rt_operand(rt, rt->lstack, q[i][k]);
			tr(rt, "-- event: script %u (%u arguments)\n", id, argc);
			(void)rt_call(rt, id, rt->lstack, argc, a);
		}
	}
	run_timers(rt); /* opcode 13 */
	pad_timers(rt); /* FUN_1008_5D48: delayed release of a diagonal */
	check_hits(rt); /* opcode 21 (FUN_1008_4124, on every iteration of the loop) */
	if (rt->timer_script && now_ms >= rt->timer_due_ms) { /* FUN_1008_DEAA: queued, without arguments */
		tr(rt, "-- idle timeout\n");
		queue_call(rt, rt->timer_script, 0, 0);
		rt->timer_script = 0;
	}
	if (rt->link_pending) {
		char file[32];
		rt->link_pending = 0;
		if (rt->link_return && prev_file[0])
			snprintf(file, sizeof(file), "%s", prev_file);
		else
			snprintf(file, sizeof(file), "%s", rt->link_file);
		snprintf(prev_file, sizeof(prev_file), "%s", rt->code ? rt->code->file : "");
		rt_fade(rt, 0, 100); /* FUN_1008_C5F8: the library closes with the screen off */
		rt_open_library(rt, file);
	}
}

/* ---------------------------------------------------------------- calls */

int rt_enter(Rt *rt, unsigned id)
{
	if (rt->stop)
		return 0;
	if (rt->depth + 1 >= RT_MAX_DEPTH) {
		tr(rt, "!! nesting beyond %d (error 0x6A)\n", RT_MAX_DEPTH);
		return 0;
	}
	rt->depth++;
	cov_mark(COV_SCRIPT, id); /* lists also pass through here: the report counts only scripts */
	tr(rt, "> script %u\n", id);
	return 1;
}

void rt_leave(Rt *rt)
{
	rt->depth--;
}

void rt_run(Rt *rt, unsigned id, int16_t *frame)
{
	id &= 0xFFFF;
	if (!rt->code || id >= rt->code->n_entries || !rt->code->scripts[id]) {
		tr(rt, "!! %u is not a script (error 0x6C)\n", id);
		return;
	}
	if (frame < rt->lstack || frame >= rt->lstack + RT_LSTACK - 256) {
		tr(rt, "!! locals frame out of bounds\n");
		return;
	}
	if (rt_enter(rt, id)) {
		rt->code->scripts[id](rt, frame);
		rt_leave(rt);
	}
}

int16_t rt_call(Rt *rt, unsigned id, int16_t *frame, unsigned argc, const int16_t *args)
{
	for (unsigned k = 0; k < argc; k++)
		frame[k] = args[k];
	int16_t saved = rt->retval;
	rt->retval = 0;
	rt_run(rt, id, frame);
	int16_t r = rt->retval;
	rt->retval = saved;
	return r;
}

unsigned rt_list_len(const Rt *rt, unsigned id)
{
	return rt->code && id < rt->code->n_entries ? rt->code->list_len[id] : 0;
}

int rt_run_list(Rt *rt, unsigned id, int16_t *l, unsigned item)
{
	if (!rt->code || id >= rt->code->n_entries || !rt->code->lists[id]) {
		tr(rt, "!! %u is not a list\n", id);
		return 0;
	}
	if (!rt_enter(rt, id))
		return 0;
	cov_mark(COV_LIST, id);
	tr(rt, "  (list %u entry %u)\n", id, item);
	int r = rt->code->lists[id](rt, l, item);
	rt_leave(rt);
	return r;
}

int rt_anim_event(Rt *rt, unsigned id, unsigned off)
{
	if (!rt->code || id >= rt->code->n_entries || !rt->code->anim_events[id]) {
		tr(rt, "!! animation %u has no translated events (offset 0x%04X)\n", id, off);
		return 0;
	}
	tr(rt, "-- animation event %u @0x%04X\n", id, off);
	return rt->code->anim_events[id](rt, off);
}

/* Callback of the animation events (EXE, 1008:CC5x): G(5017) = sprite, then for command
 * 0x16 a synchronous call like opcode 01 (FUN_1008_CC14 -> CDAC), frame 0. */
void rt_anim_call(Rt *rt, unsigned sprite, const uint8_t *cmd)
{
	if (!rt->g || rt->stop)
		return;
	G(5017) = (int16_t)sprite;
	unsigned id = (uint16_t)rt_operand(rt, rt->lstack, lb7_rd16(cmd + 2));
	unsigned argc = lb7_rd16(cmd + 4);
	int16_t a[6];
	if (argc > 6)
		argc = 6;
	for (unsigned k = 0; k < argc; k++)
		a[k] = rt_operand(rt, rt->lstack, lb7_rd16(cmd + 6 + k * 2));
	tr(rt, "-- animation of sprite %u: script %u (%u arguments)\n", sprite, id, argc);
	(void)rt_call(rt, id, rt->lstack, argc, a);
}

/* ---------------------------------------------------------------- instructions */

/* Input data of entry id (FUN_1008_7712 + 73DC: the entry is loaded into memory). */
static RtObj *obj_of(Rt *rt, unsigned id)
{
	if (!rt->obj || id >= rt->n_obj)
		return NULL;
	rt->obj[id].loaded = 1;
	return &rt->obj[id];
}

int rt_load(Rt *rt, unsigned id)
{
	unsigned type = lb7_entry(&rt->lib, id).type;
	/* FUN_1008_740E: types without a handler return 0 and the script stops */
	switch (type) {
	case 1: case 4: case 9: case 10: case 11: case 12: case 13: case 14: case 15: case 16:
	case 17: case 18:
		tr(rt, "   load %u (type %u)\n", id, type);
		if (type == 17)
			(void)obj_of(rt, id);
		if (type == 17 && rt->spr)
			return spr_load(rt->spr, id);
		return 1;
	default:
		tr(rt, "   load %u: type %u has no handler\n", id, type);
		return 0;
	}
}

int rt_unload(Rt *rt, unsigned id)
{
	unsigned type = lb7_entry(&rt->lib, id).type;
	tr(rt, "   unload %u (type %u)\n", id, type);
	if (type == 17 && rt->spr)
		spr_unload(rt->spr, id);
	/* unloading a sound stops it (FUN_1008_722A): TOPSFINA wave 59 loops until
	   script 52 unloads it */
	if (type == LB7_T_WAVE) {
		for (int i = 0; i < 16; i++)
			if (rt->snd[i].res == id) {
				audio_stop(rt->snd[i].voice);
				memset(&rt->snd[i], 0, sizeof(rt->snd[i]));
			}
	}
	else if (type == LB7_T_MIDI && rt->music.res == id) {
		audio_stop_midi();
		memset(&rt->music, 0, sizeof(rt->music));
	}
	return 1;
}

int rt_show(Rt *rt, unsigned sprite, unsigned anim, int hide_first)
{
	tr(rt, "   show sprite %u anim %u%s\n", sprite, anim, hide_first ? " (hide first)" : "");
	if (lb7_entry(&rt->lib, sprite).type != 17)
		return 0;
	(void)obj_of(rt, sprite);
	return rt->spr ? spr_show(rt->spr, sprite, anim, hide_first) : 1;
}

void rt_link(Rt *rt, const char *file, int stop_audio, int ret)
{
	tr(rt, "   link -> %s (flag %d %d)\n", file, stop_audio, ret);
	snprintf(rt->link_file, sizeof(rt->link_file), "%s", file);
	rt->link_stop_audio = stop_audio;
	rt->link_return = ret;
	rt->link_pending = 1;
}

void rt_quit(Rt *rt)
{
	tr(rt, "   quit\n");
	rt->quit = 1;
}

static unsigned pick_random(Rt *rt, uint8_t *last, unsigned n)
{
	unsigned i = rt_rand(rt) % n;
	if (i == *last && ++i >= n)
		i = 0;
	*last = (uint8_t)i;
	return i;
}

static unsigned pick_next(uint8_t *last, unsigned n)
{
	unsigned i = *last >= n ? 0 : *last;
	*last = (uint8_t)(i + 1 >= n ? 0 : i + 1);
	return i;
}

int rt_run_random(Rt *rt, int16_t *l, uint8_t *last, unsigned n, const uint16_t *ids)
{
	if (n)
		rt_run(rt, ids[pick_random(rt, last, n)], l);
	return !rt->stop;
}

int rt_run_next(Rt *rt, int16_t *l, uint8_t *last, unsigned n, const uint16_t *ids)
{
	if (n)
		rt_run(rt, ids[pick_next(last, n)], l);
	return !rt->stop;
}

int rt_run_random_list(Rt *rt, int16_t *l, uint8_t *last, unsigned list)
{
	unsigned n = rt_list_len(rt, list);
	return n ? rt_run_list(rt, list, l, pick_random(rt, last, n)) : 1;
}

int rt_run_next_list(Rt *rt, int16_t *l, uint8_t *last, unsigned list)
{
	unsigned n = rt_list_len(rt, list);
	return n ? rt_run_list(rt, list, l, pick_next(last, n)) : 1;
}

int rt_run_index(Rt *rt, int16_t *l, unsigned list, int16_t *var, unsigned fallback,
	int run_fallback, int autoinc)
{
	unsigned n = rt_list_len(rt, list), idx = (uint16_t)*var;
	int r = 1;
	if (!n)
		return 1;
	if (idx < n)
		r = rt_run_list(rt, list, l, idx);
	else if (run_fallback) {
		rt_run(rt, fallback, l);
		r = idx != 0;
	}
	if (autoinc)
		*var = (int16_t)(idx + 1 >= n ? 0 : idx + 1);
	return r && !rt->stop;
}

const char *rt_string(const Rt *rt, unsigned id)
{
	if (!id)
		return NULL;
	if (id & 0x8000) {
		uint32_t off = id & 0x7FFF;
		if (off >= rt->lib.key_size[4])
			return NULL;
		return (const char *)rt->lib.data + rt->lib.key_off[4] + off;
	}
	unsigned i = (id & 0x7FFF) - 1;
	return i < rt->n_str ? rt->str[i] : NULL;
}

void rt_debug(Rt *rt, unsigned str, int value)
{
	const char *s = rt_string(rt, str);
	tr(rt, "   debug: \"%s\" %d\n", s ? s : "", value);
}

/* Opcode 49 (TOPSY 1008:D14x): G(5005)/G(5006) = position, then SetCursorPos relative to the origin
 * of the viewport; here the cursor move is done by the main loop (warp_mouse). */
void rt_set_mouse(Rt *rt, int x, int y)
{
	tr(rt, "   setmouse %d %d\n", x, y);
	rt->mouse_x = x;
	rt->mouse_y = y;
	G(5005) = (int16_t)x;
	G(5006) = (int16_t)y;
	rt->warp_mouse = 1;
}

/* ---------------------------------------------------------------- input */

/* FUN_1010_25B8: deferred call {script, arg1, arg2} (encoded operands), like opcode 1E. */
static void queue_call(Rt *rt, uint16_t script, uint16_t a1, uint16_t a2)
{
	if (rt->evq_n >= 40) {
		tr(rt, "!! deferred call queue full (error 0x70)\n");
		return;
	}
	rt->evq[rt->evq_n][0] = script;
	rt->evq[rt->evq_n][1] = a1;
	rt->evq[rt->evq_n][2] = a2;
	rt->evq_n++;
}

/* "Filter" script (DAT_1504 mouse, DAT_1506 keyboard): called immediately with one argument; if
 * it returns 0 the event stops (FUN_1008_30F4 / 30A8 return DAT_40DA). */
static int hook(Rt *rt, uint16_t script, int arg)
{
	unsigned id = (uint16_t)rt_operand(rt, rt->lstack, script);
	int16_t a = (int16_t)arg;
	tr(rt, "-- input filter: script %u (%d)\n", id, arg);
	return rt_call(rt, id, rt->lstack, 1, &a) != 0;
}

/* ---- player controllers (TOPSY 1008:4BA6..5DFC, 8180..81EA) */

/* DS:0062: pressed directions (bit 1 up, 2 down, 4 right, 8 left) -> code; 0x88 = none */
static const uint8_t pad_dir_code[16] = {
	0x00, 0x03, 0x05, 0x88, 0x07, 0x0A, 0x0C, 0x88, 0x06, 0x09, 0x0B, 0x88, 0x88, 0x88, 0x88, 0x88
};

/* FUN_1008_4DE8: a change of source src reaches the player that uses it, whose event script
 * is queued with (player, code). The key sequences (opcode 52/55, timer
 * FUN_1008_4C9E) are not used by the games and are not ported, like the mirror of opcode 52D0. */
static void pad_event(Rt *rt, uint8_t code, unsigned src)
{
	unsigned p = src < sizeof(rt->pad_map) ? rt->pad_map[src] : 2;
	if (p > 1)
		return;
	struct RtPad *d = &rt->pad[p];
	if (code == 0xF7 || code == 0xFB || code == 0xFD || code == 0xFE)
		d->buttons &= code;
	else if (code == 1 || code == 2 || code == 4 || code == 8)
		d->buttons |= code;
	else
		d->dir = code; /* 0 = stopped */
	tr(rt, "-- controller %u: code 0x%02X\n", p, code);
	if (d->script && !rt->text_id) /* FUN_1008_4DE8: no events while the text field is active */
		queue_call(rt, d->script, (uint16_t)(p + 0x8ACF), (uint16_t)(code + 0x8ACF));
}

/* FUN_1008_5BD8: press of a key of a set; 1 = not a controller key. */
static int pad_key_down(Rt *rt, unsigned vk)
{
	if (vk > 0xFF || !rt->padkey[vk].ready)
		return 1;
	rt->padkey[vk].ready = 0; /* auto-repeat does not count */
	uint8_t code = rt->padkey[vk].code, set = rt->padkey[vk].set & 1;
	if (code == 0x10 || code == 0x20 || code == 0x40 || code == 0x80) {
		uint8_t old = pad_dir_code[rt->pad_state[set]];
		rt->pad_state[set] |= code >> 4;
		rt->pad_rel_on[set] = 0; /* FUN_1008_DCF0 */
		code = pad_dir_code[rt->pad_state[set]];
		if (code == 0x88) {
			if (old == 0x88)
				return 0;
			code = 0;
		}
	}
	pad_event(rt, code, set + 1u);
	return 0;
}

/* FUN_1008_5C82: release. Directions are removed from the state; if one stays pressed the new
 * code arrives after 200 ms (a player releasing a diagonal must not see the passage
 * through a single direction); buttons send the complement. */
static int pad_key_up(Rt *rt, unsigned vk)
{
	if (vk > 0xFF || !rt->padkey[vk].code)
		return 1;
	rt->padkey[vk].ready = 1;
	uint8_t code = rt->padkey[vk].code, set = rt->padkey[vk].set & 1;
	if (code == 0x10 || code == 0x20 || code == 0x40 || code == 0x80) {
		uint8_t old = pad_dir_code[rt->pad_state[set]];
		rt->pad_state[set] &= (uint8_t)~(code >> 4);
		if (rt->pad_state[set] && old != 0x88) {
			rt->pad_rel_on[set] = 1; /* FUN_1008_DE1E(0, 5D48, 200, 0, set) */
			rt->pad_rel_due[set] = rt->now_ms + 200;
			return 0;
		}
		rt->pad_rel_on[set] = 0;
		code = pad_dir_code[rt->pad_state[set]];
		if (code == 0x88)
			return 0;
	}
	else
		code = (uint8_t)~code;
	pad_event(rt, code, set + 1u);
	return 0;
}

static void pad_timers(Rt *rt)
{
	for (unsigned set = 0; set < 2; set++)
		if (rt->pad_rel_on[set] && (int32_t)(rt->now_ms - rt->pad_rel_due[set]) >= 0) {
			rt->pad_rel_on[set] = 0;
			uint8_t code = pad_dir_code[rt->pad_state[set]];
			if (code != 0x88)
				pad_event(rt, code, set + 1);
		}
}

/* FUN_1008_5D7A: removes the set keys from the per-key table and, if on, puts them back with the codes
 * of DS:0042; keys already pressed count immediately (GetKeyState). */
static void pad_keys(Rt *rt, int on, unsigned set)
{
	set &= 1;
	for (unsigned vk = 0; vk < 256; vk++)
		if (rt->padkey[vk].set == set)
			rt->padkey[vk].ready = rt->padkey[vk].code = 0;
	if (!on)
		return;
	rt->pad_state[set] = 0;
	for (unsigned k = 0; k < 8; k++) {
		uint8_t vk = rt->keyset[set][k * 2];
		rt->padkey[vk].set = (uint8_t)set;
		rt->padkey[vk].ready = 1;
		rt->padkey[vk].code = rt->keyset[set][k * 2 + 1];
		if (rt->key_down[vk])
			(void)pad_key_down(rt, vk);
	}
}

/* FUN_1008_4BA6: turns the player device on or off. */
static void pad_enable(Rt *rt, struct RtPad *d, int on)
{
	switch (d->type) {
	case 1: /* FUN_1008_4A60: the 17 joystick zones become ready (or off) */
		if (d->src >= 3 && d->src <= 4)
			memset(&rt->joy_ready[(d->src - 3) * 17], on != 0, 17);
		break;
	case 2:
		pad_keys(rt, on, d->src - 1u);
		break;
	case 5: /* FUN_1008_81EA: dead zone ±20, edge ±40, cursor at the center */
		if (on) {
			rt->pad_mouse_code = 0;
			rt->mouse_x = rt->mouse_y = 0;
			if (rt->g) {
				G(5005) = 0;
				G(5006) = 0;
			}
			rt->warp_mouse = 1;
		}
		break;
	}
}

/* FUN_1008_4BF6: buttons still pressed and direction are released (script queued). */
static void pad_release(Rt *rt, unsigned p)
{
	struct RtPad *d = &rt->pad[p];
	uint8_t b = d->buttons, dir = d->dir;
	d->buttons = d->dir = 0;
	if (!d->script)
		return;
	static const uint8_t rel[4] = { 0xFE, 0xFD, 0xFB, 0xF7 };
	for (int k = 0; k < 4; k++)
		if (b & (1 << k))
			queue_call(rt, d->script, (uint16_t)(p + 0x8ACF), (uint16_t)(rel[k] + 0x8ACF));
	if (dir)
		queue_call(rt, d->script, (uint16_t)(p + 0x8ACF), 0x8ACF);
}

/* Opcode 53 (FUN_1008_535C) {player, parameter, type (byte), enable (byte), suspend (byte)}:
 * with "enable" or "suspend" it turns the current device on/off (suspend also releases the
 * buttons); otherwise it picks the device: 1 joystick parameter, 2 key set parameter,
 * 5 mouse, 0 none. */
static void op_pad(Rt *rt, int16_t *l, const uint8_t *b)
{
	unsigned p = (uint8_t)rt_operand(rt, l, lb7_rd16(b + 2));
	if (p > 1) {
		tr(rt, "!! controller %u does not exist\n", p);
		return;
	}
	struct RtPad *d = &rt->pad[p];
	uint8_t on = b[7], off = b[8], type = b[6];
	tr(rt, "   controller %u: type %u enable %u suspend %u\n", p, type, on, off);
	if (d->src && d->src < sizeof(rt->pad_map) && rt->pad_map[d->src] == p) {
		pad_enable(rt, d, on);
		if (d->type == 5)
			rt->pad_mouse = on;
		d->active = on;
	}
	if (off)
		pad_release(rt, p);
	if (on || off)
		return;
	uint8_t par = (uint8_t)rt_operand(rt, l, lb7_rd16(b + 4));
	if (type == 1)
		d->src = (uint8_t)(par + 3);
	else if (type == 2)
		d->src = (uint8_t)(par + 1);
	else if (type == 5) {
		d->src = 5;
		rt->pad_mouse = 1;
	}
	if (d->src >= sizeof(rt->pad_map))
		d->src = 0;
	d->type = type;
	if (!type) {
		if (d->src == 5 && rt->pad_map[5] == p)
			rt->pad_mouse = 0;
	}
	else
		rt->pad_map[d->src] = (uint8_t)p;
	d->active = type != 0;
	pad_enable(rt, d, d->active);
	rt->pad_kbd = rt->pad[0].type == 2 || rt->pad[1].type == 2;
	rt->pad_joy = rt->pad[0].type == 1 || rt->pad[1].type == 1;
}

/* DS:0030: joystick zone -> code (0 stopped, directions like the keyboard, 9..16 = release and
 * press of buttons 1, 2, 3, 4) */
static const uint8_t joy_code[17] = {
	0x00, 0x07, 0x06, 0x03, 0x0A, 0x09, 0x05, 0x0C, 0x0B, 0xFE, 0x01, 0xFD, 0x02, 0xFB, 0x04, 0xF7, 0x08
};

/* FUN_1008_5928 (joystick as controller) or FUN_1008_5AF4 -> table DS:4090 (script per
 * zone, opcode 48) when no player uses the joystick. kind: 0 movement, 1 press,
 * 2 release. */
static void joy_zone(Rt *rt, int joy, int zone, int kind)
{
	if (!rt->g || !rt->code || joy < 0 || joy > 1 || zone < 0 || zone > 16)
		return;
	int idx = joy * 17 + zone;
	if (!rt->pad_joy) {
		uint16_t s = idx < (int)(sizeof(rt->table4090) / sizeof(rt->table4090[0])) ? (uint16_t)rt->table4090[idx] : 0;
		if (s) /* FUN_1008_D754 */
			rt_run(rt, (uint16_t)rt_operand(rt, rt->lstack, s), rt->lstack);
		return;
	}
	if (!rt->joy_ready[idx])
		return;
	if (kind == 1)
		rt->joy_ready[idx - 1] = 1; /* releasing the same button becomes possible again */
	else if (kind == 2)
		rt->joy_ready[idx + 1] = 1;
	else {
		rt->joy_ready[rt->joy_prev[joy]] = 1;
		rt->joy_prev[joy] = (uint8_t)idx;
	}
	rt->joy_ready[idx] = 0;
	pad_event(rt, joy_code[zone], (unsigned)joy + 3);
}

void rt_joy_move(Rt *rt, int joy, int x, int y)
{
	if (rt->joy_swap)
		joy ^= 1;
	int zone;
	if (!rt->joy_alt) { /* FUN_1008_5846: threshold ±16000, diagonals too */
		zone = (y < -16000 ? 3 : 0) + (y > 16000 ? 6 : 0) + (x > 16000 ? 1 : 0) + (x < -16000 ? 2 : 0);
	}
	else { /* FUN_1008_578E: threshold 8000, dominant direction only */
		int ax = abs(x), ay = abs(y);
		if (ax <= 8000 && ay <= 8000)
			zone = 0;
		else if (ax < ay)
			zone = y <= 0 ? 3 : 6;
		else
			zone = x < 0 ? 2 : 1;
	}
	joy_zone(rt, joy, zone, 0);
}

void rt_joy_button(Rt *rt, int joy, int button, int down)
{
	if (button < 0 || button > 3) /* FUN_1008_56FC: buttons 1..4 */
		return;
	if (rt->joy_swap)
		joy ^= 1;
	joy_zone(rt, joy, 9 + button * 2 + (down != 0), down ? 1 : 2);
}

/* FUN_1008_80A4 / 8180: with the mouse as controller the displacement from the center gives the direction
 * (GETANGLE quadrants: down, right, up, left), the cursor stays within ±38 pixels; the
 * buttons are 1/2 (release 0xFE/0xFD). */
static void pad_mouse_move(Rt *rt, int x, int y)
{
	uint8_t code = 0;
	if (x < -20 || 20 <= x || y < -20 || 20 <= y) {
		double a = atan2((double)x, (double)y) * 1800.0 / 3.14159265358979323846;
		int ang = (int)lround(a);
		if (ang < 0)
			ang += 3600;
		if (x < -40 || 40 <= x || y < -40 || 40 <= y) { /* FUN_1008_788A: brought back to the edge */
			int m = abs(x) > abs(y) ? abs(x) : abs(y);
			x = x * 38 / m;
			y = y * 38 / m;
			rt->warp_mouse = 1;
		}
		static const uint8_t quad[4] = { 5, 7, 3, 6 }; /* DS:0086 */
		code = quad[((ang + 450) % 3600) / 900];
	}
	rt->mouse_x = x;
	rt->mouse_y = y;
	G(5005) = (int16_t)x;
	G(5006) = (int16_t)y;
	if (code == rt->pad_mouse_code)
		return;
	rt->pad_mouse_code = code;
	pad_event(rt, code, 5);
}

/* ---------------------------------------------------------------- text field */

/* FUN_1010_1FF2 + 1DE8: text with the "_" cursor at the end (" " if empty, DS:01F2), redrawn
 * in all sprites that use the resource. */
static void text_redraw(Rt *rt, int cursor)
{
	char s[136];
	snprintf(s, sizeof(s), "%s%s", rt->text_buf, cursor ? "_" : "");
	if (rt->spr)
		spr_set_text(rt->spr, rt->text_id, s[0] ? s : " ");
}

/* FUN_1010_20E8: end of input. Esc restores the initial text; the text goes into the
 * string variable and the final script is queued without arguments (with both Esc and Enter). */
static void text_commit(Rt *rt, int esc)
{
	if (esc)
		snprintf(rt->text_buf, sizeof(rt->text_buf), "%s", rt->text_backup);
	char *dst = str_dest(rt, rt->text_var); /* raw string id, as in sprintf (op 38) */
	if (dst)
		copy127(dst, rt->text_buf);
	tr(rt, "   text field %u %s: \"%s\"\n", rt->text_id, esc ? "cancelled" : "confirmed", rt->text_buf);
	if (rt->text_script)
		queue_call(rt, rt->text_script, 0, 0);
	text_redraw(rt, 0);
	rt->text_id = 0;
}

/* FUN_1010_239C (Del): clears all the text. */
static void text_clear(Rt *rt)
{
	if (!rt->text_buf[0])
		return; /* the original does MessageBeep */
	rt->text_buf[0] = 0;
	text_redraw(rt, 1);
}

/* Opcode 36 (FUN_1010_22A0): {text type 18, string variable, final script, -, max (byte),
 * digits only (byte)}; text 0 = only closes the active field, as with Enter. */
static int text_field(Rt *rt, int16_t *l, const uint8_t *b)
{
	unsigned id = (uint16_t)rt_operand(rt, l, lb7_rd16(b + 2));
	if (id && lb7_entry(&rt->lib, id).type != 18)
		return 1;
	if (rt->text_id)
		text_commit(rt, 0);
	if (!id)
		return 1;
	rt->text_id = (uint16_t)id;
	rt->text_var = lb7_rd16(b + 4);
	rt->text_script = lb7_rd16(b + 6);
	rt->text_max = b[10] ? b[10] : 0x7F;
	rt->text_digits = b[11];
	rt->text_flush = 1; /* FUN_1010_2658(0x102): discard the WM_CHARs already queued */
	const char *cur = rt->spr ? spr_get_text(rt->spr, id) : "";
	snprintf(rt->text_buf, sizeof(rt->text_buf), "%s", strcmp(cur, " ") ? cur : "");
	snprintf(rt->text_backup, sizeof(rt->text_backup), "%s", rt->text_buf);
	tr(rt, "   text field %u (max %u%s, final script %u)\n", id, rt->text_max,
		rt->text_digits ? ", digits only" : "", rt->text_script ? (uint16_t)rt_operand(rt, l, rt->text_script) : 0);
	if (rt->text_script) /* the script is queued with the current value (the queue wants the operand) */
		rt->text_script = (uint16_t)rt_operand(rt, l, rt->text_script) + 0x8ACF;
	text_redraw(rt, 1);
	return 1;
}

void rt_char(Rt *rt, unsigned c)
{
	idle_reset(rt);
	if (rt->paused)
		return;
	if (!rt->text_id || !rt->g || !rt->code)
		return;
	c &= 0xFF;
	size_t n = strlen(rt->text_buf);
	if (c == 0x1B || c == 0x0D) { /* 20E8(1) / 20E8(0) */
		text_commit(rt, c == 0x1B);
		return;
	}
	if (c == 8) { /* 200E(0): removes the last character */
		if (n) {
			rt->text_buf[n - 1] = 0;
			text_redraw(rt, 1);
		}
		return;
	}
	if (c < 0x20 || c == 0x40) /* Tab and '@' excluded */
		return;
	if ((rt->text_digits && (c < '0' || c > '9')) || n >= rt->text_max || n + 1 >= sizeof(rt->text_buf))
		return; /* 21C2: rejected (MessageBeep) */
	rt->text_buf[n] = (char)c;
	rt->text_buf[n + 1] = 0;
	text_redraw(rt, 1);
}

/* ---------------------------------------------------------------- pause and focus */

/* FUN_1008_55FE: player controllers suspended (release of direction and buttons) or re-armed. */
static void pads_hold(Rt *rt, int hold)
{
	for (unsigned p = 0; p < 2; p++)
		if (rt->pad[p].active) {
			if (hold)
				pad_release(rt, p);
			else
				pad_enable(rt, &rt->pad[p], 1);
		}
}

int rt_running(const Rt *rt)
{
	return !rt->paused && rt->active;
}

void rt_toggle_pause(Rt *rt)
{
	int on = !rt->paused;
	if (!rt->g || !rt->code)
		return;
	if (rt->v150e) { /* opcode 23: the script receives 1 (pause) or 0 (resume); a value != 0 is a veto */
		int16_t arg = (int16_t)on;
		tr(rt, "-- pause %s: script %u\n", on ? "yes" : "no", (uint16_t)rt_operand(rt, rt->lstack, (uint16_t)rt->v150e));
		if (rt_call(rt, (uint16_t)rt_operand(rt, rt->lstack, (uint16_t)rt->v150e), rt->lstack, 1, &arg)) {
			tr(rt, "   pause refused by the script\n");
			return;
		}
		if (rt->spr) /* S_001: one frame to show the state (the pause sign) */
			spr_update(rt->spr, rt->now_ms);
	}
	else { /* A_042: pause or resume of wave and music */
		tr(rt, "-- pause %s (sounds)\n", on ? "yes" : "no");
		pause_waves(rt, on);
		pause_midi(rt, on);
	}
	rt->paused = (uint8_t)on;
	pads_hold(rt, on);
}

void rt_set_active(Rt *rt, int a)
{
	a = a != 0;
	if (a != rt->active_audio) {
		tr(rt, "-- window %s\n", a ? "active" : "inactive");
		pads_hold(rt, !a);
		if (rt->code && !a) /* A_005(0): CD, MIDI and wave stopped and closed, no end notifications */
			sound_reset(rt);
		rt->active_audio = (uint8_t)a;
		if (!a)
			rt->drag_sprite = 0; /* FUN_1008_3900: drag cancelled */
	}
	if (!rt->alt_tab && rt->code && a != rt->active) {
		rt->active = (uint8_t)a;
		if (a && rt->v1508) { /* opcode 1D: the script restarts music and game state */
			unsigned id = (uint16_t)rt_operand(rt, rt->lstack, (uint16_t)rt->v1508);
			tr(rt, "-- resume: script %u (DAT_1508)\n", id);
			rt_run(rt, id, rt->lstack);
		}
	}
}

/* Level 0..31 of opcode 5D <-> Windows volume saved in [audio] (TOPSYA01 0D32, "%i"). */
static int vol_from_ini(int v, int deflt)
{
	return v == deflt ? 31 : (int)((uint16_t)v >> 11);
}

void rt_load_settings(Rt *rt)
{
	char buf[64];
	const char *sec = rt->app_section;
	rt->alt_tab = ini_get(rt, sec, "AltTab", buf, sizeof(buf)) ? (uint8_t)(atoi(buf) != 0) : 0;
	rt->debug = ini_get(rt, sec, "Debug", buf, sizeof(buf)) ? (uint8_t)(atoi(buf) != 0) : 0;
	rt->lomem = ini_get(rt, sec, "LoMem", buf, sizeof(buf)) ? (uint8_t)(atoi(buf) != 0) : 0;
	int mode640 = ini_get(rt, sec, "Mode640", buf, sizeof(buf)) ? atoi(buf) != 0 : rt->screen_w < 641;
	rt->bg640 = (uint8_t)(mode640 || rt->lomem); /* FUN_1008_04BA: second bitmap of opcode 06 */
	rt->max_joy = ini_get(rt, sec, "MaxJoySticks", buf, sizeof(buf)) ? atoi(buf) : 2;
	int found = 0;
	if (ini_get(rt, "audio", "WaveVolume", buf, sizeof(buf))) {
		rt->vol_wave = vol_from_ini(atoi(buf), 0);
		found = 1;
	}
	if (ini_get(rt, "audio", "MidiVolume", buf, sizeof(buf))) {
		rt->vol_midi = vol_from_ini(atoi(buf), 1);
		found = 1;
	}
	if (found)
		apply_volume(rt);
	tr(rt, "settings: AltTab %u Debug %u LoMem %u backgrounds 640 %u MaxJoySticks %d volumes %d/%d\n",
		rt->alt_tab, rt->debug, rt->lomem, rt->bg640, rt->max_joy, rt->vol_wave, rt->vol_midi);
}

/* FUN_1008_EB56: every mouse or keyboard message re-arms the idle timeout. */
static void idle_reset(Rt *rt)
{
	if (rt->timer_script)
		rt->timer_due_ms = rt->now_ms + rt->timer_ms;
}

void rt_key_down(Rt *rt, unsigned vk, int shift, int ctrl)
{
	idle_reset(rt);
	if (vk < 0x100)
		rt->key_down[vk] = 1;
	if (!rt->g || !rt->code)
		return;
	tr(rt, "-- key down %u%s%s\n", vk, shift ? " +Shift" : "", ctrl ? " +Ctrl" : "");
	if (rt->text_id && vk == 0x2E) { /* WndProc: Del clears the field (FUN_1010_239C), no 3B3C */
		text_clear(rt);
		return;
	}
	if (rt->pad_kbd && !pad_key_down(rt, vk)) /* FUN_1008_3B3C: the key goes only to the controller */
		return;
	if (vk == 0x13) { /* Pause: FUN_1008_3140, before the DAT_5B85 check */
		rt_toggle_pause(rt);
		return;
	}
	if (rt->paused) /* DAT_5B85: while paused no other key */
		return;
	if (rt->v1506 && !hook(rt, (uint16_t)rt->v1506, (int)vk))
		return;
	if (vk >= 0x100 || rt->key_off[vk] || rt->text_id) /* 3B3C: no scripts while the field is active */
		return;
	uint16_t s = rt->key_script[vk][shift ? 1 : ctrl ? 2 : 0];
	if (s) /* FUN_1008_D754: immediate execution, without arguments */
		rt_run(rt, (uint16_t)rt_operand(rt, rt->lstack, s), rt->lstack);
}

void rt_key_up(Rt *rt, unsigned vk)
{
	idle_reset(rt);
	if (rt->paused) { /* 3C42: no releases while paused */
		if (vk < 0x100)
			rt->key_down[vk] = 0;
		return;
	}
	if (vk < 0x100)
		rt->key_down[vk] = 0;
	if (!rt->g || !rt->code || vk >= 0x100)
		return;
	if (rt->pad_kbd && !pad_key_up(rt, vk)) /* FUN_1008_3C42 */
		return;
	uint16_t s = rt->key_script[vk][3]; /* the release ignores the disable flag */
	if (s) {
		tr(rt, "-- key up %u\n", vk);
		rt_run(rt, (uint16_t)rt_operand(rt, rt->lstack, s), rt->lstack);
	}
}

/* FUN_1008_2BD4: the cursor leaves sprite DAT_1544 -> script DAT_1542 (sprite, 0). */
static void hover_leave(Rt *rt)
{
	queue_call(rt, rt->hover_script, rt->hover_sprite, 0x8ACF);
	rt->hover_sprite = 0;
}

/* FUN_1008_2D86: cursor enter/leave of sprites; 1 = cursor over a sprite. */
static int hover_check(Rt *rt, int x, int y)
{
	unsigned id = rt->spr ? spr_hit(rt->spr, x, y) : 0;
	if (!id) {
		if (rt->hover_sprite)
			hover_leave(rt);
		return 0;
	}
	uint16_t enc = (uint16_t)(0x8ACF + id);
	if (rt->hover_sprite != enc) {
		if (rt->hover_sprite)
			hover_leave(rt);
		rt->hover_sprite = enc;
		queue_call(rt, rt->hover_script, enc, 0x8AD0); /* (sprite, 1) */
	}
	return 1;
}

/* FUN_1008_2C6E: hot zone under the cursor (searching from the last registered); when the zone changes
 * it queues the leave script of the previous one (arg1 = 0) and the enter script of the new one (arg1 = 1),
 * second argument being the zone's own. */
static void zone_check(Rt *rt, int x, int y)
{
	int i = rt->n_zones - 1;
	for (; i >= 0; i--) {
		if (!rt->zones[i].sprite) {
			if (rt->zones[i].x1 <= x && x < rt->zones[i].x2 && rt->zones[i].y1 <= y && y < rt->zones[i].y2)
				break;
		}
		else if (rt->spr && spr_hit_id(rt->spr, rt->zones[i].sprite, x, y))
			break;
	}
	int prev = rt->zone_cur - 1;
	if (prev == i)
		return;
	if (prev >= 0 && rt->zones[prev].leave)
		queue_call(rt, rt->zones[prev].leave, 0x8ACF, rt->zones[prev].leave_arg);
	rt->zone_cur = 0;
	if (i >= 0) {
		if (rt->zones[i].enter)
			queue_call(rt, rt->zones[i].enter, 0x8AD0, rt->zones[i].enter_arg);
		rt->zone_cur = i + 1;
	}
}

/* FUN_1008_3036: end of drag -> G(5009)/G(5010) and drop script (+0xA). */
static void drag_end(Rt *rt, int x, int y)
{
	G(5009) = (int16_t)x;
	G(5010) = (int16_t)y;
	RtObj *o = obj_of(rt, (uint16_t)rt_operand(rt, rt->lstack, rt->drag_sprite));
	if (o && o->drop)
		queue_call(rt, o->drop, rt->drag_sprite, 0);
	rt->drag_sprite = 0;
}

void rt_mouse_move(Rt *rt, int x, int y)
{
	idle_reset(rt);
	if (rt->paused || !rt->active) /* 3CEA: mouse ignored while paused or with an inactive window */
		return;
	if (!rt->g || !rt->code)
		return;
	if (rt->pad_mouse) { /* FUN_1008_3CEA -> 8180 */
		pad_mouse_move(rt, x, y);
		return;
	}
	rt->mouse_x = x;
	rt->mouse_y = y;
	G(5005) = (int16_t)x; /* DAT_3C64/3C66 */
	G(5006) = (int16_t)y;
	if (rt->v1504 && !hook(rt, (uint16_t)rt->v1504, 4))
		return;
	if (rt->cursor_mode == 4)
		return;
	if (rt->drag_sprite) { /* S_051: relative move of the dragged sprite */
		if (rt->spr)
			spr_move_rel(rt->spr, (uint16_t)rt_operand(rt, rt->lstack, rt->drag_sprite),
				x - rt->drag_x, y - rt->drag_y);
		rt->drag_x = x;
		rt->drag_y = y;
		return;
	}
	/* opcode 3D zones only if the cursor is not over a sprite of the DAT_1542 script */
	if ((!rt->hover_script || !hover_check(rt, x, y)) && rt->n_zones)
		zone_check(rt, x, y);
}

void rt_mouse_button(Rt *rt, int x, int y, int button, int down)
{
	idle_reset(rt);
	if (rt->paused || !rt->active)
		return;
	if (!rt->g || !rt->code)
		return;
	tr(rt, "-- mouse %s %s at %d,%d\n", button == 1 ? "left" : "right", down ? "down" : "up", x, y);
	if (rt->pad_mouse) { /* FUN_1008_3720 / 383A -> 8180 */
		pad_event(rt, (uint8_t)(down ? button : ~button), 5);
		return;
	}
	if (!down) { /* FUN_1008_383A */
		if (rt->v1504 && !hook(rt, (uint16_t)rt->v1504, 2))
			return;
		if (rt->drag_sprite)
			drag_end(rt, x, y);
		return;
	}
	/* FUN_1008_35B2: no click with the cursor in mode 3/4 or during a drag */
	if (rt->cursor_mode == 3 || rt->cursor_mode == 4 || rt->drag_sprite)
		return;
	G(5001) = (int16_t)button; /* DAT_3C5C: 1 left, 2 right */
	G(5007) = (int16_t)x;      /* DAT_3C68/3C6A: click point */
	G(5008) = (int16_t)y;
	if (rt->v1504 && !hook(rt, (uint16_t)rt->v1504, 1))
		return;
	uint16_t script = 0, a1 = 0, a2 = 0;
	unsigned id = rt->spr ? spr_hit(rt->spr, x, y) : 0;
	tr(rt, "   sprite hit: %u\n", id);
	if (id) {
		a1 = (uint16_t)(0x8ACF + id); /* S_022: encoded sprite id */
		RtObj *o = obj_of(rt, id);
		if (o && !o->draggable) {
			if (!o->click_off) {
				script = o->click;
				a2 = o->click_arg;
			}
		}
		else if (o) { /* start of drag */
			rt->drag_x = x;
			rt->drag_y = y;
			script = o->drag;
			rt->drag_sprite = a1;
		}
	}
	if (!script && rt->n_hot) { /* rectangular areas, from the last registered */
		a1 = 0x8ACF;
		for (int i = rt->n_hot - 1; i >= 0 && !script; i--) {
			const RtHotspot *h = &rt->hot[i];
			if (!h->off && h->x1 <= x && x < h->x2 && h->y1 <= y && y < h->y2) {
				script = h->script;
				a2 = h->arg;
			}
		}
	}
	if (script) {
		tr(rt, "   click -> script %u\n", (uint16_t)rt_operand(rt, rt->lstack, script));
		queue_call(rt, script, a1, a2);
	}
}

/* Value encoded as a constant (v - 0x7531 of the original), 0 if the operand is 0. */
static uint16_t enc_const(Rt *rt, int16_t *l, uint16_t enc)
{
	return enc ? (uint16_t)(rt_operand(rt, l, enc) + 0x8ACF) : 0;
}

/* Opcode 0C (FUN_1008_345C) {key, script, Ctrl, Shift, enable, disable, release}. */
static void op_key(Rt *rt, int16_t *l, const uint8_t *b)
{
	unsigned vk = (uint16_t)rt_operand(rt, l, lb7_rd16(b + 2));
	if (vk >= 0x100)
		return;
	for (unsigned k = vk ? vk : 0; k < (vk ? vk + 1 : 0x100u); k++) { /* key 0 = all */
		if (b[8])
			rt->key_off[k] = 0;
		else if (b[9])
			rt->key_off[k] = 1;
		else {
			rt->key_script[k][b[6] ? 2 : b[7] ? 1 : b[10] ? 3 : 0] = lb7_rd16(b + 4);
			rt->key_off[k] = 0;
		}
	}
}

/* Opcode 3D (FUN_1008_4252), 22 byte: {x1, y1, x2, y2, enter script, arg, leave script,
 * arg, sprite, "clear all" byte}. Enter 0 = removes the zone (FUN_1008_2DE6), otherwise it
 * adds or updates it (FUN_1008_288C, same sprite or same rectangle; at most 40) and
 * checks the cursor right away. */
static void op_zone(Rt *rt, int16_t *l, const uint8_t *b)
{
	if (b[0x14]) {
		rt->n_zones = rt->zone_cur = 0;
		return;
	}
	int16_t c[4];
	for (int k = 0; k < 4; k++)
		c[k] = rt_operand(rt, l, lb7_rd16(b + 2 + k * 2));
	uint16_t se = lb7_rd16(b + 0x12);
	uint16_t sprite = se ? (uint16_t)rt_operand(rt, l, se) : 0;
	int i = 0;
	for (; i < rt->n_zones; i++)
		if (sprite ? rt->zones[i].sprite == sprite
			: rt->zones[i].x1 == c[0] && rt->zones[i].y1 == c[1] && rt->zones[i].x2 == c[2] && rt->zones[i].y2 == c[3])
			break;
	if (!lb7_rd16(b + 0xA)) { /* removes the zone */
		if (i < rt->n_zones) {
			memmove(&rt->zones[i], &rt->zones[i + 1], (size_t)(rt->n_zones - i - 1) * sizeof(rt->zones[0]));
			rt->n_zones--;
			if (rt->zone_cur && rt->zone_cur - 1 == i)
				rt->zone_cur = 0;
			else if (rt->zone_cur && i < rt->zone_cur - 1)
				rt->zone_cur--;
		}
		return;
	}
	if (i == rt->n_zones) {
		if (rt->n_zones >= 40)
			return;
		rt->n_zones++;
	}
	rt->zones[i].x1 = c[0];
	rt->zones[i].y1 = c[1];
	rt->zones[i].x2 = c[2];
	rt->zones[i].y2 = c[3];
	rt->zones[i].enter = enc_const(rt, l, lb7_rd16(b + 0xA));
	rt->zones[i].enter_arg = enc_const(rt, l, lb7_rd16(b + 0xC));
	rt->zones[i].leave = enc_const(rt, l, lb7_rd16(b + 0xE));
	rt->zones[i].leave_arg = enc_const(rt, l, lb7_rd16(b + 0x10));
	rt->zones[i].sprite = sprite;
	tr(rt, "   hot zone %d: %d,%d-%d,%d sprite %u\n", i, c[0], c[1], c[2], c[3], sprite);
	zone_check(rt, rt->mouse_x, rt->mouse_y);
}

/* FUN_1008_3220: rectangular area {.., script +4, x1 +6, y1 +8, x2 +A, y2 +C, arg +E, .., enable-only
 * +11, disable +12}; an area with the same coordinates is updated. */
static void op_hotspot(Rt *rt, int16_t *l, const uint8_t *b)
{
	int16_t c[4];
	for (int k = 0; k < 4; k++)
		c[k] = rt_operand(rt, l, lb7_rd16(b + 6 + k * 2));
	int i = 0;
	while (i < rt->n_hot && !(rt->hot[i].x1 == c[0] && rt->hot[i].y1 == c[1] && rt->hot[i].x2 == c[2]
		&& rt->hot[i].y2 == c[3]))
		i++;
	if (i == rt->n_hot) {
		if (rt->n_hot >= RT_MAX_HOTSPOTS) {
			tr(rt, "!! too many clickable areas\n");
			return;
		}
		memset(&rt->hot[rt->n_hot++], 0, sizeof(RtHotspot));
	}
	RtHotspot *h = &rt->hot[i];
	if (b[0x12]) {
		h->off = 1;
		return;
	}
	if (!b[0x11]) {
		h->x1 = c[0];
		h->y1 = c[1];
		h->x2 = c[2];
		h->y2 = c[3];
		h->script = enc_const(rt, l, lb7_rd16(b + 4));
		h->arg = enc_const(rt, l, lb7_rd16(b + 0xE));
	}
	h->off = 0;
	tr(rt, "   area %d,%d-%d,%d -> script %u\n", h->x1, h->y1, h->x2, h->y2,
		(uint16_t)rt_operand(rt, l, h->script));
}

/* Opcode 04 (FUN_1008_3924, 20 bytes): click on a sprite or on an area; with flag +0x10 it acts
 * on all (FUN_1008_2BEC / 2FB6). */
static void op_click(Rt *rt, int16_t *l, const uint8_t *b)
{
	if (b[0x10]) {
		uint16_t script = lb7_rd16(b + 4);
		int set_script = !b[0x11] && !b[0x12], off = !b[0x11];
		if (set_script && !script)
			rt->n_hot = 0; /* script 0: the areas are deleted */
		for (int i = 0; i < rt->n_hot; i++) {
			if (set_script)
				rt->hot[i].script = script;
			rt->hot[i].off = set_script ? 0 : (uint8_t)off;
		}
		for (uint32_t i = 0; rt->obj && i < rt->n_obj; i++)
			if (rt->obj[i].loaded && lb7_entry(&rt->lib, i).type == 17) {
				if (set_script)
					rt->obj[i].click = script;
				else
					rt->obj[i].click_off = (uint8_t)off;
			}
		return;
	}
	uint16_t enc = lb7_rd16(b + 2);
	if (!enc) {
		op_hotspot(rt, l, b);
		return;
	}
	RtObj *o = obj_of(rt, (uint16_t)rt_operand(rt, l, enc));
	if (!o)
		return;
	o->click_arg = enc_const(rt, l, lb7_rd16(b + 0xE));
	o->click_off = 0;
	if (b[0x12])
		o->click_off = 1;
	else if (!b[0x11])
		o->click = lb7_rd16(b + 4);
	tr(rt, "   click on sprite %u -> script %u%s\n", (uint16_t)rt_operand(rt, l, enc),
		(uint16_t)rt_operand(rt, l, o->click), o->click_off ? " (disabled)" : "");
}

/* Opcode 07 (FUN_1008_3A6E) {sprite, start script, release script, fixed}: drag. */
static void op_drag(Rt *rt, int16_t *l, const uint8_t *b)
{
	unsigned id = (uint16_t)rt_operand(rt, l, lb7_rd16(b + 2));
	RtObj *o = obj_of(rt, id);
	if (!o)
		return;
	o->drop = lb7_rd16(b + 6);
	o->drag = lb7_rd16(b + 4);
	o->draggable = b[8] == 0;
	if (b[8] && rt->drag_sprite == (uint16_t)(0x8ACF + id))
		drag_end(rt, rt->mouse_x, rt->mouse_y);
}

void rt_set_string(Rt *rt, const char *s)
{
	snprintf(rt->str5bd8, sizeof(rt->str5bd8), "%s", s);
}

/* ---------------------------------------------------------------- strings and INI */

/* Writable destination: string variables only (id without bit 15). */
static char *str_dest(Rt *rt, unsigned id)
{
	if (!id || (id & 0x8000))
		return NULL;
	unsigned i = id - 1;
	return i < rt->n_str ? rt->str[i] : NULL;
}

/* Copy truncated to 127 characters like LSTRCPYN(dst, src, 0x7F) of the original. */
static void copy127(char *dst, const char *src)
{
	size_t n = strlen(src);
	if (n > 127)
		n = 127;
	memcpy(dst, src, n);
	dst[n] = 0;
}

/* wvsprintf reduced to the specifiers used by the games: %[-0][width][l](d|i|u|x|X|c|s|%).
 * The arguments are 16-bit words or strings, like the stack passed by FUN_1008_A1B8. */
static void format16(Rt *rt, char *out, size_t cap, const char *f, unsigned argc, const RtArg *a)
{
	size_t o = 0;
	unsigned k = 0;
	while (*f && o + 1 < cap) {
		if (*f != '%') {
			out[o++] = *f++;
			continue;
		}
		char spec[16] = "%";
		size_t s = 1;
		f++;
		while ((*f == '-' || *f == '0' || *f == '#' || (*f >= '1' && *f <= '9')) && s < 10)
			spec[s++] = *f++;
		while (*f >= '0' && *f <= '9' && s < 12)
			spec[s++] = *f++;
		if (*f == 'l')
			f++; /* arguments are always 16-bit in the games; 32-bit is not used */
		char conv = *f ? *f++ : 0;
		char tmp[300];
		if (conv == '%') {
			out[o++] = '%';
			continue;
		}
		spec[s++] = conv == 'i' ? 'd' : conv;
		spec[s] = 0;
		const RtArg *x = k < argc ? &a[k++] : NULL;
		switch (conv) {
		case 'd': case 'i': snprintf(tmp, sizeof(tmp), spec, x ? (int)x->v : 0); break;
		case 'u': case 'x': case 'X': snprintf(tmp, sizeof(tmp), spec, x ? (unsigned)(uint16_t)x->v : 0u); break;
		case 'c': snprintf(tmp, sizeof(tmp), spec, x ? (int)(uint8_t)x->v : ' '); break;
		case 's': {
			const char *str = x ? rt_string(rt, (uint16_t)x->v) : NULL;
			snprintf(tmp, sizeof(tmp), spec, str ? str : "");
			break;
		}
		default: tmp[0] = 0; break;
		}
		for (char *t = tmp; *t && o + 1 < cap; t++)
			out[o++] = *t;
	}
	out[o] = 0;
}

int rt_sprintf(Rt *rt, unsigned dest, unsigned fmt, unsigned argc, const RtArg *args)
{
	char *d = str_dest(rt, dest);
	const char *f = rt_string(rt, fmt);
	if (!d || !f)
		return 1;
	char buf[1024];
	if (!argc)
		snprintf(buf, sizeof(buf), "%s", f);
	else
		format16(rt, buf, sizeof(buf), f, argc, args);
	copy127(d, buf);
	tr(rt, "   sprintf -> \"%s\"\n", d);
	return 1;
}

/* Section (1008:B11E): string id, or 0 = library title (DAT_5BF8) if present,
 * otherwise DS:5B98 = string 0xD2 of TOPSY.EXE, "Topsy Turvy" (LoadString in FUN_1000_05E4). */
static const char *ini_section(Rt *rt, unsigned section)
{
	if (section)
		return rt_string(rt, section);
	return rt->lib.title[0] ? rt->lib.title : rt->app_section;
}

/* Reading/writing of 7THLEVEL.INI (same format as Windows: [section] key=value). */
static int ini_get(const Rt *rt, const char *sec, const char *key, char *out, size_t cap)
{
	FILE *f = fopen(rt->ini_path[0] ? rt->ini_path : "7THLEVEL.INI", "r");
	char line[512];
	int in = 0;
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '[') {
			char *e = strchr(line, ']');
			if (e)
				*e = 0;
			in = !strcasecmp(line + 1, sec);
			continue;
		}
		char *eq = strchr(line, '=');
		if (in && eq) {
			*eq = 0;
			if (!strcasecmp(line, key)) {
				snprintf(out, cap, "%s", eq + 1);
				fclose(f);
				return out[0] != 0;
			}
		}
	}
	fclose(f);
	return 0;
}

static void ini_put(const Rt *rt, const char *sec, const char *key, const char *val)
{
	const char *path = rt->ini_path[0] ? rt->ini_path : "7THLEVEL.INI";
	char *old = NULL;
	long n = 0;
	FILE *f = fopen(path, "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		n = ftell(f);
		fseek(f, 0, SEEK_SET);
		old = calloc((size_t)n + 1, 1);
		if (old && fread(old, 1, (size_t)n, f) != (size_t)n)
			n = 0;
		fclose(f);
	}
	f = fopen(path, "wb");
	if (!f) {
		free(old);
		return;
	}
	int in = 0, done = 0, seen = 0;
	char *p = old;
	while (p && *p) {
		char *e = strchr(p, '\n');
		size_t len = e ? (size_t)(e - p + 1) : strlen(p);
		char line[512];
		snprintf(line, sizeof(line), "%.*s", (int)len, p);
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '[') {
			if (in && !done && val) {
				fprintf(f, "%s=%s\r\n", key, val);
				done = 1;
			}
			char name[512];
			snprintf(name, sizeof(name), "%s", line + 1);
			char *b = strchr(name, ']');
			if (b)
				*b = 0;
			in = !strcasecmp(name, sec);
			seen |= in;
			if (in && !key) { /* WritePrivateProfileString(sec, NULL, NULL): drop the section */
				p += len;
				continue;
			}
		}
		else if (in && !key) {
			p += len;
			continue;
		}
		else if (in && !done) {
			char *eq = strchr(line, '=');
			if (eq && (size_t)(eq - line) == strlen(key) && !strncasecmp(line, key, strlen(key))) {
				if (val)
					fprintf(f, "%s=%s\r\n", key, val);
				done = 1;
				p += len;
				continue;
			}
		}
		fwrite(p, 1, len, f);
		p += len;
	}
	if (!done && key && val) {
		if (!seen)
			fprintf(f, "[%s]\r\n", sec);
		fprintf(f, "%s=%s\r\n", key, val);
	}
	fclose(f);
	free(old);
}

/* Value encryption (FUN_1008_ADDC/AD7C): byte + key in two nibbles '0'+n, low first. */
static void ini_encrypt(char k, const char *in, char *out, size_t cap)
{
	size_t o = 0;
	for (; *in && o + 2 < cap; in++) {
		uint8_t b = (uint8_t)(*in + k);
		out[o++] = (char)((b & 0xF) + '0');
		out[o++] = (char)(((int8_t)b >> 4) + '0');
	}
	out[o] = 0;
}

static void ini_decrypt(char k, char *s)
{
	char *o = s;
	for (const char *p = s; p[0] && p[1]; p += 2)
		*o++ = (char)((((uint8_t)p[0] - '0') | (uint8_t)(p[1] << 4)) - k);
	*o = 0;
}

/* String escapes (FUN_1008_ACBA / AC00): \n \r \t \" \\ */
static void ini_escape(const char *in, char *out, size_t cap)
{
	size_t o = 0;
	for (; *in && o + 3 < cap; in++) {
		const char *e = *in == '\n' ? "\\n" : *in == '\r' ? "\\r" : *in == '\t' ? "\\t"
			: *in == '"' ? "\\\"" : *in == '\\' ? "\\\\" : NULL;
		if (e) {
			out[o++] = e[0];
			out[o++] = e[1];
		}
		else
			out[o++] = *in;
	}
	out[o] = 0;
}

static void ini_unescape(const char *in, char *out, size_t cap)
{
	size_t o = 0;
	while (*in && o + 1 < cap) {
		if (*in == '\\' && in[1]) {
			char c = in[1];
			out[o++] = c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' : c;
			in += 2;
		}
		else
			out[o++] = *in++;
	}
	out[o] = 0;
}

void rt_ini_get_int(Rt *rt, int16_t *var, unsigned key, unsigned section, int enc)
{
	const char *k = rt_string(rt, key), *s = ini_section(rt, section);
	char v[256];
	if (!k || !s || !ini_get(rt, s, k, v, sizeof(v)))
		return;
	if (enc)
		ini_decrypt(k[0], v);
	*var = (int16_t)atoi(v);
	tr(rt, "   ini [%s] %s -> %d\n", s, k, *var);
}

void rt_ini_get_str(Rt *rt, unsigned str, unsigned key, unsigned section, int enc)
{
	const char *k = rt_string(rt, key), *s = ini_section(rt, section);
	char *d = str_dest(rt, str), v[256];
	if (!d || !k || !s || !ini_get(rt, s, k, v, sizeof(v))) {
		tr(rt, "   ini [%s] %s: missing\n", s ? s : "?", k ? k : "?");
		return;
	}
	char u[256];
	if (rt->ini_raw)
		snprintf(u, sizeof(u), "%s", v);
	else
		ini_unescape(v, u, sizeof(u));
	if (enc)
		ini_decrypt(k[0], u);
	copy127(d, u);
}

void rt_ini_put_int(Rt *rt, int value, unsigned key, unsigned section, int enc)
{
	const char *k = rt_string(rt, key), *s = ini_section(rt, section);
	char v[32], e[80];
	if (!k || !s)
		return;
	snprintf(v, sizeof(v), "%d", value);
	if (enc)
		ini_encrypt(k[0], v, e, sizeof(e));
	ini_put(rt, s, k, enc ? e : v);
	tr(rt, "   ini [%s] %s = %d\n", s, k, value);
}

void rt_ini_put_str(Rt *rt, unsigned str, unsigned key, unsigned section, int enc)
{
	const char *k = rt_string(rt, key), *s = ini_section(rt, section), *src = rt_string(rt, str);
	char v[600], e[600];
	if (!k || !s || !src)
		return;
	ini_escape(src, v, sizeof(v));
	if (enc) {
		ini_encrypt(k[0], v, e, sizeof(e));
		snprintf(v, sizeof(v), "%s", e);
	}
	const char *p = v;
	while (*p == ' ')
		p++;
	ini_put(rt, s, k, *p ? v : NULL); /* empty string = delete the key */
}

/* ---------------------------------------------------------------- misc */

int rt_compare(int a, int b, unsigned op)
{
	int r;
	if (op & 0x01) r = (uint16_t)a == (uint16_t)b;
	else if (op & 0x02) r = (int16_t)b < (int16_t)a;
	else if (op & 0x04) r = (int16_t)a < (int16_t)b;
	else if (op & 0x08) r = (a | b) != 0;
	else if (op & 0x10) r = (a & b) != 0;
	else if (op & 0x20) r = b ? ((int16_t)a % (int16_t)b) != 0 : 0;
	else r = a != 0;
	return (op & 0x80) ? !r : r;
}

void rt_cursor(Rt *rt, int mode)
{
	rt->cursor_mode = mode;
	tr(rt, "   cursor mode %d\n", mode);
}

/* Palette fade in modes with more than 256 colors (FUN_1008_466C in / 478C out -> 4622 -> S_021):
 * speed steps (1..100), one per redrawn frame (S_001), synchronous like the original. Fade
 * in only acts with the screen off, fade out only with the screen on. */
void rt_fade(Rt *rt, int in, int speed)
{
	if (speed <= 0 || speed > 100)
		speed = 100;
	if (!in && rt->in_startup) {
		rt->fade_pending = 0; /* FUN_1008_4880: a fade out during startup cancels the fade in */
		return;
	}
	if (in ? !rt->faded : rt->faded)
		return;
	tr(rt, "   fade %s speed %d\n", in ? "in" : "out", speed);
	rt->fade_pending = in; /* DAT_5B75 */
	for (int k = in ? 1 : 2; k <= speed; k++) { /* DAT_1512: from 1 (in) or from 2 (out) to speed */
		rt->fade_level = 256 * (in ? k : speed - k) / speed;
		if (rt->frame)
			rt->frame(rt);
	}
	rt->faded = !in;
	rt->fade_level = in ? 256 : 0;
}

/* FUN_1008_DCAC / DC0C */
void rt_stop_timer(Rt *rt, unsigned id, int all)
{
	tr(rt, "   stop timer 0x%04X%s\n", id, all ? " (all)" : "");
	if (all) {
		rt->n_timers = 0;
		return;
	}
	for (int i = 0; i < rt->n_timers; i++)
		if (rt->timers[i].id == id) {
			memmove(&rt->timers[i], &rt->timers[i + 1], (size_t)(rt->n_timers - i - 1) * sizeof(rt->timers[0]));
			rt->n_timers--;
			return;
		}
}

/* Opcode 13 (FUN_1008_DD20) {id, script, duration ms (dword), variable, repeat}: with flag +10
 * the duration is the operand at +6. A timer with the same id is re-armed. */
static void op_timer(Rt *rt, int16_t *l, const uint8_t *b)
{
	uint32_t ms = b[10] ? (uint32_t)(uint16_t)rt_operand(rt, l, lb7_rd16(b + 6))
		: lb7_rd16(b + 6) | (uint32_t)lb7_rd16(b + 8) << 16;
	uint16_t id = lb7_rd16(b + 2);
	int i = 0;
	while (i < rt->n_timers && rt->timers[i].id != id)
		i++;
	if (i >= 20) {
		tr(rt, "!! too many timers (error 0x74)\n");
		return;
	}
	if (i == rt->n_timers)
		rt->n_timers++;
	rt->timers[i].id = id;
	rt->timers[i].interval = ms;
	rt->timers[i].due = rt->now_ms + ms;
	rt->timers[i].script = lb7_rd16(b + 4);
	rt->timers[i].repeat = b[11];
	tr(rt, "   timer 0x%04X: script %u in %u ms%s\n", id, (uint16_t)rt_operand(rt, l, rt->timers[i].script),
		ms, b[11] ? " (repeating)" : "");
}

/* ---------------------------------------------------------------- sounds */

static struct RtSound *wave_slot(Rt *rt, unsigned res)
{
	for (int i = 0; i < 16; i++)
		if (rt->snd[i].res == res)
			return &rt->snd[i];
	return NULL;
}

static void wave_stop(struct RtSound *w)
{
	if (w->voice)
		audio_stop(w->voice);
	memset(w, 0, sizeof(*w));
}

/* Library closed: all sounds stop without notifications, library pauses and volumes reset. */
static void sound_reset(Rt *rt)
{
	for (int i = 0; i < 16; i++)
		wave_stop(&rt->snd[i]);
	if (rt->music.res)
		audio_stop_midi();
	memset(&rt->music, 0, sizeof(rt->music));
	if (rt->waves_paused)
		audio_pause_waves(0);
	if (rt->midi_paused)
		audio_pause_midi(0);
	rt->waves_paused = rt->midi_paused = 0;
}

/* End of a sound: like the WndProc with message 0x52C (1008:ED3C). */
static void sound_end(Rt *rt, struct RtSound *x, int is_midi)
{
	unsigned res = x->res;
	uint16_t script = x->script;
	int notify = x->notify;
	memset(x, 0, sizeof(*x));
	if (!notify)
		return;
	if (script) {
		tr(rt, "-- end %s %u: script %u\n", is_midi ? "MIDI" : "wave", res,
			(uint16_t)rt_operand(rt, rt->lstack, script));
		queue_call(rt, script, (uint16_t)(0x8ACF + res), 0);
	}
	else
		tr(rt, "-- end %s %u: unloaded\n", is_midi ? "MIDI" : "wave", res);
}

static void sound_check(Rt *rt)
{
	for (int i = 0; i < 16 && !rt->waves_paused; i++)
		if (rt->snd[i].res && rt->snd[i].end_ms && (int32_t)(rt->now_ms - rt->snd[i].end_ms) >= 0)
			sound_end(rt, &rt->snd[i], 0);
	if (rt->music.res && !rt->midi_paused && rt->music.end_ms && (int32_t)(rt->now_ms - rt->music.end_ms) >= 0)
		sound_end(rt, &rt->music, 1);
}

/* Expected end after loops repeats (-1 = never): 0 = does not end. */
static uint32_t sound_end_ms(Rt *rt, uint32_t dur, int loops)
{
	if (loops < 0)
		return 0;
	uint32_t t = rt->now_ms + dur * (uint32_t)(loops + 1);
	return t ? t : 1;
}

/* Opcode 08 (FUN_1008_8EAC -> A_029), 28 bytes: +2 wave, from +4 the parameter block of
 * TOPSYA01 {+4 repeats (-1 = infinite), +6 set volume, +7 notify at end of sound,
 * +8 (device restart, irrelevant), +9 volume 0..16, +0xA/+0xE fades in ms,
 * +0x12 delay in ms, +0x1A end-of-sound script}. Upsy uses only repeats, notification and script. */
static int op_wave(Rt *rt, int16_t *l, const uint8_t *b)
{
	unsigned res = (uint16_t)rt_operand(rt, l, lb7_rd16(b + 2));
	if (lb7_entry(&rt->lib, res).type != LB7_T_WAVE) {
		tr(rt, "   wave %u: is not a sound (the script stops)\n", res);
		return 0;
	}
	int loops = (int16_t)lb7_rd16(b + 4);
	int vol = b[6] ? (b[9] > 16 ? 16 : b[9]) * 16 : 256;
	if (lb7_rd32(b + 0xA) || lb7_rd32(b + 0xE) || lb7_rd32(b + 0x12))
		tr(rt, "   ?? wave %u: fades/delay not yet ported\n", res);
	struct RtSound *w = wave_slot(rt, res);
	if (w) /* one event per resource: a new play restarts from the beginning */
		wave_stop(w);
	else if (!(w = wave_slot(rt, 0))) {
		w = &rt->snd[0];
		wave_stop(w);
	}
	w->res = (uint16_t)res;
	w->voice = audio_play_wave(&rt->lib, res, loops, vol);
	w->notify = b[7];
	/* the script is resolved now, like Op08 (it may be a local variable); 0 = unload */
	int16_t sc = lb7_rd16(b + 0x1A) ? rt_operand(rt, l, lb7_rd16(b + 0x1A)) : 0;
	w->script = sc ? (uint16_t)(sc + 0x8ACF) : 0;
	w->end_ms = sound_end_ms(rt, audio_wave_duration_ms(&rt->lib, res), loops);
	tr(rt, "   wave %u (repeats %d%s)\n", res, loops, w->notify ? ", notify at end" : "");
	return 1;
}

/* Opcode 0F (FUN_1008_AAAA -> A_039) {wave (0 = all), fade in ms}: no notification. */
static void op_wave_stop(Rt *rt, int16_t *l, const uint8_t *b)
{
	uint16_t enc = lb7_rd16(b + 2);
	if (lb7_rd32(b + 4))
		tr(rt, "   ?? stop wave with fade not yet ported\n");
	if (!enc) {
		for (int i = 0; i < 16; i++)
			wave_stop(&rt->snd[i]);
		return;
	}
	struct RtSound *w = wave_slot(rt, (uint16_t)rt_operand(rt, l, enc));
	if (w)
		wave_stop(w);
}

/* Opcode 50 (FUN_1008_9BBE -> A_028) {MIDI, repeats, notify, script}: only one music track at a
 * time; the previous one stops without notification (FUN_1000_1944 -> 1A1C). */
static int op_midi(Rt *rt, int16_t *l, const uint8_t *b)
{
	unsigned res = (uint16_t)rt_operand(rt, l, lb7_rd16(b + 2));
	if (lb7_entry(&rt->lib, res).type != LB7_T_MIDI) {
		tr(rt, "   MIDI %u: is not a sequence\n", res);
		return 1; /* FUN_1008_740E fallito: the instruction does nothing */
	}
	int loops = (int16_t)lb7_rd16(b + 4);
	if (rt->music.res)
		audio_stop_midi();
	rt->music.res = (uint16_t)res;
	rt->music.notify = b[6];
	rt->music.script = enc_const(rt, l, lb7_rd16(b + 8));
	rt->music.end_ms = sound_end_ms(rt, audio_midi_duration_ms(&rt->lib, res), loops);
	if (!audio_play_midi(&rt->lib, res, loops))
		tr(rt, "   MIDI %u muted (SoundFont missing): the end is still computed\n", res);
	tr(rt, "   MIDI %u (repeats %d%s)\n", res, loops, rt->music.notify ? ", notify at end" : "");
	return 1;
}

/* Opcode 51 (FUN_1008_A4C4 -> A_038) {MIDI, 0 = the current one}: stops without notification. */
static void op_midi_stop(Rt *rt, int16_t *l, const uint8_t *b)
{
	uint16_t enc = lb7_rd16(b + 2);
	if (!rt->music.res || (enc && (uint16_t)rt_operand(rt, l, enc) != rt->music.res))
		return;
	audio_stop_midi();
	memset(&rt->music, 0, sizeof(rt->music));
}

/* Opcode 5D (FUN_1008_A32A) {up, device}: device volume one step (1/31) up or down
 * (FUN_1008_8CBE); device 0 = all, 1 wave, 2 MIDI, 3 auxiliary (CD: absent). */
/* Device volume: levels of opcode 5D, 0 when muted (opcode 35). */
static void apply_volume(Rt *rt)
{
	audio_set_wave_volume(rt->mute ? 0 : rt->vol_wave * 256 / 31);
	audio_set_music_volume(rt->mute ? 0 : rt->vol_midi * 256 / 31);
}

static void op_volume(Rt *rt, const uint8_t *b)
{
	int up = b[2] != 0, dev = b[3];
	int *lv[2] = { dev == 0 || dev == 1 ? &rt->vol_wave : NULL, dev == 0 || dev == 2 ? &rt->vol_midi : NULL };
	for (int k = 0; k < 2; k++)
		if (lv[k]) {
			if (up && *lv[k] < 31)
				(*lv[k])++;
			else if (!up && *lv[k] > 0)
				(*lv[k])--;
		}
	apply_volume(rt);
	char v[16];
	if (lv[0]) {
		snprintf(v, sizeof(v), "%i", (int16_t)(rt->vol_wave * 0x842));
		ini_put(rt, "audio", "WaveVolume", v);
	}
	if (lv[1]) {
		snprintf(v, sizeof(v), "%i", (int16_t)(rt->vol_midi * 0x842));
		ini_put(rt, "audio", "MidiVolume", v);
	}
	tr(rt, "   volume wave %d/31, MIDI %d/31\n", rt->vol_wave, rt->vol_midi);
}

/* Opcode 35 (A_023, TOPSYA01 1000:0E94): {signed byte} < 0 toggles, 0 turns off, > 0 turns on the
 * mute. Zeroes the device volume without stopping the sounds; applies to all libraries. */
static void op_mute(Rt *rt, const uint8_t *b)
{
	int8_t m = (int8_t)b[2];
	uint8_t on = m < 0 ? !rt->mute : m > 0;
	tr(rt, "   mute %s\n", on ? "on" : "off");
	if (on != rt->mute) {
		rt->mute = on;
		apply_volume(rt);
	}
}

/* Sound pause: the expected ends shift by the duration of the pause. */
static void pause_waves(Rt *rt, int on)
{
	if (!on == !rt->waves_paused)
		return;
	rt->waves_paused = on;
	if (on)
		rt->waves_paused_at = rt->now_ms;
	else
		for (int i = 0; i < 16; i++)
			if (rt->snd[i].end_ms)
				rt->snd[i].end_ms += rt->now_ms - rt->waves_paused_at;
	audio_pause_waves(on);
}

/* A_025: counter; the music stops at the first "pause" and restarts when it returns to 0. */
static void pause_midi(Rt *rt, int on)
{
	if (on) {
		if (!rt->midi_paused++) {
			rt->midi_paused_at = rt->now_ms;
			audio_pause_midi(1);
		}
	}
	else if (rt->midi_paused && !--rt->midi_paused) {
		if (rt->music.end_ms)
			rt->music.end_ms += rt->now_ms - rt->midi_paused_at;
		audio_pause_midi(0);
	}
}

/* Opcode 10 (FUN_1008_9E06), 24 bytes: {variable, n, n (at most 3) x {operand, operation,
 * negate}}; var = var op x in order (FUN_1008_8D2C): 0 assign, 1 +, 2 -, 3 *, 4 rounded
 * division, 5 |, 6 &, 7 %; others = 0. The result goes into the variable (global or local). */
static void op_calc(Rt *rt, int16_t *l, const uint8_t *b)
{
	uint16_t enc = lb7_rd16(b + 2);
	int32_t acc = rt_operand(rt, l, enc);
	unsigned n = lb7_rd16(b + 4);
	for (unsigned k = 0; k < n && k < 3; k++) {
		const uint8_t *o = b + 6 + k * 6;
		int32_t x = rt_operand(rt, l, lb7_rd16(o));
		if (o[4])
			x = -x;
		switch (o[2]) {
		case 0: acc = x; break;
		case 1: acc = acc + x; break;
		case 2: acc = acc - x; break;
		case 3: acc = acc * x; break;
		case 4: acc = x ? (acc + x / 2) / x : x; break;
		case 5: acc = acc | x; break;
		case 6: acc = acc & x; break;
		case 7: acc = x ? acc % x : 0; break;
		default: acc = 0; break;
		}
		acc = (int16_t)acc; /* to 16 bits after every operation, like the original word */
	}
	if (enc < 0x13FE)
		G(enc) = (int16_t)acc;
	else if (enc < 0x159F)
		l[enc - 0x13FE] = (int16_t)acc;
}

/* Opcode 18 (FUN_1008_E740), 12 bytes: {old, new, animation, script, unload the old one,
 * show flag}. If the old one is not loaded it does nothing. */
static int op_replace(Rt *rt, int16_t *l, const uint8_t *b)
{
	unsigned old_id = (uint16_t)rt_operand(rt, l, lb7_rd16(b + 2));
	unsigned new_id = (uint16_t)rt_operand(rt, l, lb7_rd16(b + 4));
	if (lb7_entry(&rt->lib, old_id).type != 17 || lb7_entry(&rt->lib, new_id).type != 17)
		return 0;
	SprWorld *sw = rt->spr;
	if (!sw || !spr_loaded(sw, old_id)) /* old one not loaded: nothing */
		return 1;
	tr(rt, "   replace sprite %u with %u%s\n", old_id, new_id, b[10] && new_id != old_id ? " (unload the old one)" : "");
	if (new_id != old_id) {
		(void)obj_of(rt, new_id);
		if (!spr_replace(sw, old_id, new_id))
			return 0;
		if (b[10])
			(void)rt_unload(rt, old_id);
	}
	uint16_t anim = lb7_rd16(b + 6); /* raw 0 = fixed frame (1008:A990) */
	if (!rt_show(rt, new_id, anim ? (uint16_t)rt_operand(rt, l, anim) : 0, b[11]))
		return 0;
	uint16_t s = lb7_rd16(b + 8);
	if (s) /* FUN_1008_D754: immediately, without arguments, on the current frame (DAT_10C2 does not change) */
		rt_run(rt, (uint16_t)rt_operand(rt, l, s), l);
	return !rt->stop;
}

/* Writes to a variable (global or local) given by an operand, like the original handlers
 * that compute the address without decoding the constants. */
static void set_var(Rt *rt, int16_t *l, uint16_t enc, int16_t v)
{
	if (enc < 0x13FE)
		G(enc) = v;
	else if (enc < 0x159F)
		l[enc - 0x13FE] = v;
}

/* Opcode 21 (FUN_1008_3E62), 18 bytes: {sprite A, sprite B, script, arg1, arg2, repeat, clear
 * all, hidden A, hidden B, mode}; script 0 = removes the pair. */
static int op_hit(Rt *rt, int16_t *l, const uint8_t *b)
{
	if (b[0xD]) {
		rt->n_hits = 0;
		return 1;
	}
	unsigned x = (uint16_t)rt_operand(rt, l, lb7_rd16(b + 2)), y = (uint16_t)rt_operand(rt, l, lb7_rd16(b + 4));
	if (lb7_entry(&rt->lib, x).type != 17 || lb7_entry(&rt->lib, y).type != 17)
		return 0;
	uint8_t hx = b[0xE], hy = b[0xF];
	if (y < x) { /* ordered pair, with the flags swapped along with it */
		unsigned t = x; x = y; y = t;
		uint8_t u = hx; hx = hy; hy = u;
	}
	int i = 0;
	while (i < rt->n_hits && !(rt->hits[i].a == x && rt->hits[i].b == y))
		i++;
	uint16_t sc = lb7_rd16(b + 6);
	int16_t scv = sc ? rt_operand(rt, l, sc) : 0;
	if (!scv) {
		if (i < rt->n_hits) {
			memmove(&rt->hits[i], &rt->hits[i + 1], (size_t)(rt->n_hits - i - 1) * sizeof(rt->hits[0]));
			rt->n_hits--;
		}
		return 1;
	}
	if (i == rt->n_hits) {
		if (rt->n_hits >= 64) {
			tr(rt, "!! too many collision pairs\n");
			return 1;
		}
		rt->n_hits++;
	}
	rt->hits[i].a = (uint16_t)x;
	rt->hits[i].b = (uint16_t)y;
	rt->hits[i].script = (uint16_t)(scv + 0x8ACF);
	rt->hits[i].arg1 = enc_const(rt, l, lb7_rd16(b + 8));
	rt->hits[i].arg2 = enc_const(rt, l, lb7_rd16(b + 0xA));
	rt->hits[i].repeat = b[0xC];
	rt->hits[i].hide_a = hx;
	rt->hits[i].hide_b = hy;
	tr(rt, "   collision %u-%u -> script %d\n", x, y, scv);
	return 1;
}

/* FUN_1008_4124: for each pair that touches, deferred call of the script; without "repeat"
 * the pair is removed. */
static void check_hits(Rt *rt)
{
	for (int i = 0; i < rt->n_hits && rt->spr; i++) {
		if (!spr_collide(rt->spr, rt->hits[i].a, rt->hits[i].b, rt->hits[i].hide_a, rt->hits[i].hide_b))
			continue;
		uint16_t s = rt->hits[i].script, a1 = rt->hits[i].arg1, a2 = rt->hits[i].arg2;
		tr(rt, "-- collision %u-%u\n", rt->hits[i].a, rt->hits[i].b);
		if (!rt->hits[i].repeat) {
			memmove(&rt->hits[i], &rt->hits[i + 1], (size_t)(rt->n_hits - i - 1) * sizeof(rt->hits[0]));
			rt->n_hits--;
			i--;
		}
		queue_call(rt, s, a1, a2);
	}
}

/* FUN_1008_DED8: expired timers, in order; the script runs immediately (FUN_1008_D754). */
static void run_timers(Rt *rt)
{
	for (int i = 0; i < rt->n_timers && !rt->link_pending; i++) {
		if (rt->timers_paused || (int32_t)(rt->now_ms - rt->timers[i].due) < 0)
			continue;
		uint16_t s = rt->timers[i].script;
		if (!rt->timers[i].repeat) {
			memmove(&rt->timers[i], &rt->timers[i + 1], (size_t)(rt->n_timers - i - 1) * sizeof(rt->timers[0]));
			rt->n_timers--;
			i--; /* like DAT_615E, decremented by FUN_1008_DC0C */
		}
		else
			rt->timers[i].due = rt->now_ms + rt->timers[i].interval;
		tr(rt, "-- timer: script %u\n", (uint16_t)rt_operand(rt, rt->lstack, s));
		rt_run(rt, (uint16_t)rt_operand(rt, rt->lstack, s), rt->lstack);
	}
}

int16_t rt_operand(Rt *rt, int16_t *l, uint16_t enc)
{
	if (enc < 0x13FE)
		return G(enc);
	if (enc < 0x159F)
		return l[enc - 0x13FE];
	return (int16_t)(enc - 0x8ACF);
}

static void op_ray(Rt *rt, int16_t *l, const uint8_t *bytes);
static void op_intersect(Rt *rt, int16_t *l, const uint8_t *bytes);

int rt_op(Rt *rt, int16_t *l, const uint8_t *bytes)
{
	unsigned op = lb7_rd16(bytes);
	cov_op(op);
	switch (op) {
	case 0x04: /* click on sprite or rectangular area (1008:3924) */
		op_click(rt, l, bytes);
		return 1;
	case 0x07: /* drag of a sprite (1008:3A6E) */
		op_drag(rt, l, bytes);
		return 1;
	case 0x0C: /* key -> script (1008:345C) */
		op_key(rt, l, bytes);
		return 1;
	case 0x08: /* play a wave (1008:8EAC) */
		return op_wave(rt, l, bytes);
	case 0x0F: /* stop the waves (1008:AAAA) */
		op_wave_stop(rt, l, bytes);
		return 1;
	case 0x50: /* play a MIDI (1008:9BBE) */
		return op_midi(rt, l, bytes);
	case 0x51: /* stop the MIDI (1008:A4C4) */
		op_midi_stop(rt, l, bytes);
		return 1;
	case 0x5D: /* device volume (1008:A32A) */
		op_volume(rt, bytes);
		return 1;
	case 0x28: { /* {sprite, var x, var y}: sprite position (1008:961C -> S_054) */
		unsigned id = (uint16_t)rt_operand(rt, l, lb7_rd16(bytes + 2));
		int x = 0, y = 0;
		if (lb7_entry(&rt->lib, id).type != 17)
			return 1;
		if (rt->spr)
			(void)spr_get_pos(rt->spr, id, &x, &y);
		set_var(rt, l, lb7_rd16(bytes + 4), (int16_t)x);
		set_var(rt, l, lb7_rd16(bytes + 6), (int16_t)y);
		return 1;
	}
	case 0x37: { /* {sprite, left, top, right, bottom}: rectangle (1008:92E4 -> S_060) */
		unsigned id = (uint16_t)rt_operand(rt, l, lb7_rd16(bytes + 2));
		int r[4];
		if (lb7_entry(&rt->lib, id).type != 17 || !rt->spr || !spr_get_rect(rt->spr, id, &r[0], &r[1], &r[2], &r[3]))
			return 1; /* sprite not loaded: variables unchanged */
		for (int k = 0; k < 4; k++)
			set_var(rt, l, lb7_rd16(bytes + 4 + k * 2), (int16_t)r[k]);
		return 1;
	}
	case 0x2E: { /* {var, min, max}: random number without repeating the previous draw (1008:9C8C) */
		int lo = rt_operand(rt, l, lb7_rd16(bytes + 4)), hi = rt_operand(rt, l, lb7_rd16(bytes + 6));
		if (hi < lo) { int t = lo; lo = hi; hi = t; }
		unsigned span = (unsigned)(hi - lo + 1), r = rt_rand(rt) % span;
		if (r == rt->last_rand2e)
			r = (r + 1) % span;
		rt->last_rand2e = (uint16_t)r;
		set_var(rt, l, lb7_rd16(bytes + 2), (int16_t)(lo + (int)r));
		return 1;
	}
	case 0x53: /* device of a player (1008:535C) */
		op_pad(rt, l, bytes);
		return 1;
	case 0x54: { /* {player, script}: controller event script (1008:5558) */
		unsigned p = (uint8_t)rt_operand(rt, l, lb7_rd16(bytes + 2));
		if (p < 2)
			rt->pad[p].script = enc_const(rt, l, lb7_rd16(bytes + 4));
		return 1;
	}
	case 0x21: /* sprite pair for collisions (1008:91A2 -> 3E62) */
		return op_hit(rt, l, bytes);
	case 0x29: { /* text {resource of type 18, format, value} (1010:1E82): wsprintf and redraw */
		unsigned id = (uint16_t)rt_operand(rt, l, lb7_rd16(bytes + 2));
		const char *f = rt_string(rt, lb7_rd16(bytes + 4));
		if (lb7_entry(&rt->lib, id).type != 18)
			return 1;
		if (f) {
			char buf[256];
			RtArg a = { 0, rt_operand(rt, l, lb7_rd16(bytes + 6)) };
			format16(rt, buf, sizeof(buf), f, 1, &a);
			buf[127] = 0;
			tr(rt, "   text %u = \"%s\"\n", id, buf);
			if (rt->spr)
				spr_set_text(rt->spr, id, buf);
		}
		return 1;
	}
	case 0x44: /* {variable, sprite}: variable = sprite visible (1008:A7B4 -> S_063) */
	case 0x2F: { /* {variable, key}: variable = key pressed (1008:9996, GetKeyState) */
		uint16_t enc = lb7_rd16(bytes + 2);
		unsigned v = (uint16_t)rt_operand(rt, l, lb7_rd16(bytes + 4));
		int16_t r = op == 0x44 ? (int16_t)(rt->spr && spr_visible(rt->spr, v)) : (int16_t)(v < 0x100 && rt->key_down[v]);
		if (op == 0x2F && v >= 0x100)
			return 1; /* the original writes nothing */
		if (enc < 0x13FE)
			G(enc) = r;
		else if (enc < 0x159F)
			l[enc - 0x13FE] = r;
		return 1;
	}
	case 0x10: /* arithmetic on a variable (1008:9E06) */
		op_calc(rt, l, bytes);
		return 1;
	case 0x18: /* replacement of a sprite (1008:E740) */
		return op_replace(rt, l, bytes);
	case 0x13: /* named timer (1008:DD20) */
		op_timer(rt, l, bytes);
		return 1;
	case 0x06: /* background {mode, bitmap 800x600, bitmap 640x480, color} */
		tr(rt, "   background mode %u bitmap %d/%d color %u\n", lb7_rd16(bytes + 2),
			rt_operand(rt, l, lb7_rd16(bytes + 4)), rt_operand(rt, l, lb7_rd16(bytes + 6)), lb7_rd16(bytes + 8));
		if (rt->spr)
			spr_background(rt->spr, lb7_rd16(bytes + 2),
				(uint16_t)rt_operand(rt, l, lb7_rd16(bytes + 4)),
				(uint16_t)rt_operand(rt, l, lb7_rd16(bytes + 6)), lb7_rd16(bytes + 8));
		return 1;
	case 0x19: /* {sprite, byte}: field +0x65, the animation loops end (S_004) */
		if (rt->spr)
			spr_break_loops(rt->spr, (uint16_t)rt_operand(rt, l, lb7_rd16(bytes + 2)), bytes[4]);
		return 1;
	case 0x1C: /* {sprite, livello}: draw level (1008:9A20 -> S_065) */
		if (rt->spr)
			spr_set_layer(rt->spr, (uint16_t)rt_operand(rt, l, lb7_rd16(bytes + 2)),
				(uint16_t)rt_operand(rt, l, lb7_rd16(bytes + 4)));
		return 1;
	case 0x47: { /* {sprite (0 = all), byte}: stamps onto the background; byte 0 = then removes it (S_064) */
		SprWorld *sw = rt->spr;
		uint16_t enc = lb7_rd16(bytes + 2);
		tr(rt, "   stamp sprite %d (keep %u)\n", enc ? rt_operand(rt, l, enc) : 0, bytes[4]);
		if (sw)
			spr_stamp(sw, sw->last_video, enc ? (uint16_t)rt_operand(rt, l, enc) : 0, bytes[4]);
		return 1;
	}
	case 0x1E: /* deferred call {script, arg1, arg2} (1010:25B8) */
		/* the operands are resolved NOW, when the call is queued (1008:D0xx case 0x1E): the
		   record holds constants; a null operand is G(0) and counts as an argument */
		queue_call(rt, (uint16_t)(rt_operand(rt, l, lb7_rd16(bytes + 2)) + 0x8ACF),
			(uint16_t)(rt_operand(rt, l, lb7_rd16(bytes + 4)) + 0x8ACF),
			(uint16_t)(rt_operand(rt, l, lb7_rd16(bytes + 6)) + 0x8ACF));
		return 1;
	case 0x24: { /* {sprite, dx, dy}: relative move (1008:9AD4 -> S_051), without loading */
		unsigned id = (uint16_t)rt_operand(rt, l, lb7_rd16(bytes + 2));
		int dx = rt_operand(rt, l, lb7_rd16(bytes + 4)), dy = rt_operand(rt, l, lb7_rd16(bytes + 6));
		tr(rt, "   move sprite %u by %d,%d\n", id, dx, dy);
		if (lb7_entry(&rt->lib, id).type == 17 && rt->spr && spr_loaded(rt->spr, id))
			spr_move_rel(rt->spr, id, dx, dy);
		return 1;
	}
	case 0x35:
		op_mute(rt, bytes);
		return 1;
	case 0x4B: { /* {sprite, 10 variables}: frame ids (1008:941E -> S_023) */
		unsigned id = (uint16_t)rt_operand(rt, l, lb7_rd16(bytes + 2));
		if (lb7_entry(&rt->lib, id).type != 17)
			return 1;
		set_var(rt, l, lb7_rd16(bytes + 4), 0);
		uint16_t ids[10];
		int n = rt->spr ? spr_frame_ids(rt->spr, id, ids, 10) : -1;
		for (int k = 0; k < n; k++)
			if (lb7_rd16(bytes + 4 + 2 * k))
				set_var(rt, l, lb7_rd16(bytes + 4 + 2 * k), (int16_t)ids[k]);
		tr(rt, "   members of sprite %u: %d\n", id, n);
		return 1;
	}
	case 0x36: /* text field (1010:22A0) */
		return text_field(rt, l, bytes);
	case 0x20: { /* {resource, script}: if the resource is loaded (FUN_1008_74FA), calls the script
		with no arguments (FUN_1008_D6D6) or, with script 0, ends the current script like op 2C;
		otherwise does nothing. Used by the Jungle games (JUNGPRTY.BIN, sprite 416 -> script 415) */
		unsigned res = (uint16_t)rt_operand(rt, l, lb7_rd16(bytes + 2));
		unsigned type = lb7_entry(&rt->lib, res).type;
		int loaded = 0;
		if (type == 17)
			loaded = rt->spr && spr_loaded(rt->spr, res);
		else
			tr(rt, "   ?? op 20: loaded check for type %u not ported\n", type);
		tr(rt, "   resource %u loaded: %d\n", res, loaded);
		if (!loaded)
			return 1;
		uint16_t s = lb7_rd16(bytes + 4);
		if (!s)
			return 0;
		/* FUN_1008_CB06 runs it without moving the locals pointer (DAT_10C2): same frame */
		(void)rt_call(rt, (uint16_t)rt_operand(rt, l, s), l, 0, NULL);
		return 1;
	}
	case 0x32: /*exit point of a ray from a rectangle (1008:7DF6) */
		op_ray(rt, l, bytes);
		return 1;
	case 0x3E: /* intersection of two lines (1008:7B1C -> GETINTERSECT) */
		op_intersect(rt, l, bytes);
		return 1;
	case 0x3D: /* cursor hot zone (1008:4252) */
		op_zone(rt, l, bytes);
		return 1;
	case 0x43: { /* {section}: deletes a section of 7THLEVEL.INI (1008:B0C0,
		WritePrivateProfileString(section, NULL, NULL)); 0 = default section */
		const char *s = ini_section(rt, lb7_rd16(bytes + 2));
		tr(rt, "   deletes INI section [%s]\n", s ? s : "");
		if (s && s[0])
			ini_put(rt, s, NULL, NULL);
		return 1;
	}
	case 0x39: /* {variabile, stringa, stringa}: lstrcmp (1008:A52A) */
	case 0x3A: { /* lstrcmpi */
		const char *a = rt_string(rt, lb7_rd16(bytes + 4)), *b = rt_string(rt, lb7_rd16(bytes + 6));
		if (a && b) {
			int c = op == 0x3A ? strcasecmp(a, b) : strcmp(a, b);
			set_var(rt, l, lb7_rd16(bytes + 2), (int16_t)(c < 0 ? -1 : c > 0));
		}
		return 1;
	}
	case 0x3F: { /* moves the sprite {sprite, x, y} to an absolute position (1008:A0C0 -> S_038);
		FUN_1008_740E(0x11) first loads it if absent (stopwatch of the first round of Upsy Daisy) */
		unsigned id = (uint16_t)rt_operand(rt, l, lb7_rd16(bytes + 2));
		if (lb7_entry(&rt->lib, id).type != 17 || !rt_load(rt, id))
			return 0;
		if (rt->spr)
			spr_move(rt->spr, id, rt_operand(rt, l, lb7_rd16(bytes + 4)), rt_operand(rt, l, lb7_rd16(bytes + 6)));
		return 1;
	}
	}
	const char *nm = lb7_op_name(op);
	rt->unimpl_op[op & 0xFF]++;
	tr(rt, "   ?? op %02X%s%s not yet ported\n", op, nm ? " " : "", nm ? nm : "");
	return 1;
}

/* ---------------------------------------------------------------- expressions */

unsigned rt_rand(Rt *rt)
{
	/* rand() of the Microsoft C CRT (1018:03CC); seed from timeGetTime (1008:C794) */
	rt->rand_seed = rt->rand_seed * 0x343FDu + 0x269EC3u;
	return (rt->rand_seed >> 16) & 0x7FFF;
}

/* ---- math of TOPSYU01/JUNGU02 (angles in tenths of a degree) ------------------------------
 * Sine table at DS:002E of the DLL (901 words, 10000 * sin): the original computed it with
 * pi = 3.1415 (verified: 0 differences over 901 values, 93 with true pi). Floating-point
 * conversions truncate toward zero and keep the low 16 bits (FUN_1000_13D0: RC = 11, 64-bit
 * fistp). Angle constant DS:26A0 = 1800 / 3.1415. See OPCODE_SCRIPT.md, "Math". */
#define U_ANG_K 572.9746936176986L

static int16_t u_trunc(long double v)
{
	return (int16_t)(int64_t)v;
}

static int16_t u_norm(int16_t a) /* NORMALIZEANGLE (1000:0DC4): a % 3600, non-negative */
{
	int r = a % 3600;
	return (int16_t)(r < 0 ? r + 3600 : r);
}

static const int16_t *u_sintab(void)
{
	static int16_t t[901];
	static int ok;
	if (!ok) {
		for (int i = 0; i <= 900; i++)
			t[i] = (int16_t)lround(10000.0 * sin(i * 3.1415 / 1800.0));
		ok = 1;
	}
	return t;
}

static int16_t u_sine(int16_t a) /* SINE (1000:0DE8) */
{
	int n = u_norm(a), neg = n >= 1800;
	if (neg)
		n -= 1800;
	int i = n % 900;
	if (n >= 900)
		i = 900 - i;
	return (int16_t)(neg ? -u_sintab()[i] : u_sintab()[i]);
}

static int16_t u_cosine(int16_t a) /* COSINE (1000:0CF0) = SINE(a + 900), with a - 900 as 16 bit */
{
	int n = u_norm((int16_t)(a - 900)), neg = n < 1800;
	if (!neg)
		n -= 1800;
	int i = n % 900;
	if (n >= 900)
		i = 900 - i;
	return (int16_t)(neg ? -u_sintab()[i] : u_sintab()[i]);
}

/* atan2(y, x) in tenths without sign correction; GETANGLE (1000:0D4E) adds 3600 if < 0.
 * 0 = down (positive screen y), 900 = right. */
static int16_t u_atan_raw(int32_t y, int32_t x)
{
	return u_trunc(atan2l((long double)y, (long double)x) * U_ANG_K);
}

static int16_t u_getangle(int16_t dx, int16_t dy)
{
	int16_t v = u_atan_raw(dx, dy);
	return (int16_t)(v < 0 ? v + 3600 : v);
}

/* TANGENT (1000:0E42): table of 1801 dwords in ten-thousandths (DS:0738 of the math DLL),
 * equal to lround(10000*tan(i*pi/1800)) except five entries (rounding errors of the
 * original table); beyond 1800 the original flips the sign. */
static int32_t u_tangent(int16_t a)
{
	int n = u_norm(a), neg = n > 1800;
	if (neg)
		n -= 1800;
	int32_t v;
	switch (n) {
	case 898: v = 2864778; break;
	case 899: v = 5729573; break;
	case 900: v = 0; break;
	case 901: v = -5729571; break;
	case 905: v = -1145886; break;
	case 1800: v = -64702; break;
	default: v = (int32_t)lroundl(10000.0L * tanl(n * 3.14159265358979323846L / 1800.0L));
	}
	return neg ? -v : v;
}

/* 1008:7844: value in ten-thousandths -> integer, rounded away from zero */
static int16_t u_unscale(int32_t v)
{
	return (int16_t)(v < 0 ? (v - 5000) / 10000 : (v + 5000) / 10000);
}

/* 1008:788A: from the point (x0, y0) along the angle a (0 = down, 900 = right), the point where
 * the ray leaves the rectangle [xmin..xmax] x [ymin..ymax]. */
static void u_ray(int16_t *out, int ymax, int xmax, int ymin, int xmin, int y0, int x0, int a)
{
	int x, y;
	if (a == 0) {
		x = x0; y = ymax;
	} else if (a == 900) {
		x = xmax; y = y0;
	} else if (a == 1800) {
		x = x0; y = ymin;
	} else if (a == 2700) {
		x = xmin; y = y0;
	} else {
		int q = a < 900 ? 0 : a < 1800 ? 1 : a < 2700 ? 2 : 3;
		int xc = q > 1 ? xmin : xmax, yc = q == 1 || q == 2 ? ymin : ymax;
		int dx = xc - x0, dy = yc - y0;
		if (!dx && !dy) {
			x = x0; y = y0;
		} else {
			int ca = u_getangle((int16_t)dx, (int16_t)dy);
			if (ca == a) {
				x = xc; y = yc;
			} else {
				int lt = ca < a, r = a % 900;
				if (q == 1 || q == 3) {
					lt = !lt;
					if (!lt)
						r = 900 - r;
					dy = -dy; dx = -dx;
				} else if (lt)
					r = 900 - r;
				int32_t t = u_tangent((int16_t)r);
				if (lt) {
					x = xc; y = u_unscale((int32_t)((uint32_t)dx * (uint32_t)t)) + y0;
				} else {
					x = u_unscale((int32_t)((uint32_t)dy * (uint32_t)t)) + x0; y = yc;
				}
			}
		}
	}
	out[0] = (int16_t)x;
	out[1] = (int16_t)y;
}

/* Opcode 32 {var x, var y, x0, y0, angle, x1, y1, x2, y2}: the starting point is
 * first clamped inside the rectangle (1008:7DF6). */
static void op_ray(Rt *rt, int16_t *l, const uint8_t *bytes)
{
	int xa = rt_operand(rt, l, lb7_rd16(bytes + 12)), xb = rt_operand(rt, l, lb7_rd16(bytes + 16));
	int ya = rt_operand(rt, l, lb7_rd16(bytes + 14)), yb = rt_operand(rt, l, lb7_rd16(bytes + 18));
	int xmin = xa < xb ? xa : xb, xmax = xa < xb ? xb : xa;
	int ymin = ya < yb ? ya : yb, ymax = ya < yb ? yb : ya;
	int x0 = rt_operand(rt, l, lb7_rd16(bytes + 6)), y0 = rt_operand(rt, l, lb7_rd16(bytes + 8));
	x0 = x0 < xmin ? xmin : x0 > xmax ? xmax : x0;
	y0 = y0 < ymin ? ymin : y0 > ymax ? ymax : y0;
	int16_t p[2];
	u_ray(p, ymax, xmax, ymin, xmin, y0, x0, u_norm((int16_t)rt_operand(rt, l, lb7_rd16(bytes + 10))));
	int x = p[0] < xmin ? xmin : p[0] > xmax ? xmax : p[0];
	int y = p[1] < ymin ? ymin : p[1] > ymax ? ymax : p[1];
	set_var(rt, l, lb7_rd16(bytes + 2), (int16_t)x);
	set_var(rt, l, lb7_rd16(bytes + 4), (int16_t)y);
}

/* Opcode 3E {var x, var y, xa0, ya0, xa1, ya1, xb0, yb0, xb1, yb1}: intersection of lines
 * A and B (GETINTERSECT, 1000:01F6). Parallel lines -> variables unchanged; like the original,
 * the slopes of vertical lines are 0, so vertical + horizontal = no write. */
static void op_intersect(Rt *rt, int16_t *l, const uint8_t *bytes)
{
	int v[8];
	for (int k = 0; k < 8; k++)
		v[k] = rt_operand(rt, l, lb7_rd16(bytes + 6 + 2 * k));
	int ax0 = v[0], ay0 = v[1], ax1 = v[2], ay1 = v[3];
	int bx0 = v[4], by0 = v[5], bx1 = v[6], by1 = v[7];
	int ka = ax0 == ax1 ? -1 : ay0 == ay1 ? 1 : 0; /* -1 vertical, 1 horizontal */
	int kb = bx0 == bx1 ? -1 : by0 == by1 ? 1 : 0;
	if (ka == kb && ka)
		return;
	long double ma = ka ? 0 : (long double)(ay0 - ay1) / (ax0 - ax1);
	long double mb = kb ? 0 : (long double)(by0 - by1) / (bx0 - bx1);
	if (ma == mb)
		return;
	long double qa = ay0 - ax0 * ma, qb = by0 - bx0 * mb;
	int16_t x, y;
	if (ka == 0 && kb == 0) {
		long double fx = (qb - qa) / (ma - mb);
		x = u_trunc(fx);
		y = u_trunc(ma * fx + qa);
	} else if (ka == 0 && kb == -1) {
		x = (int16_t)bx0; y = u_trunc(ma * x + qa);
	} else if (ka == 0) {
		y = (int16_t)by0; x = u_trunc((y - qa) / ma);
	} else if (ka == 1) {
		y = (int16_t)ay0; x = u_trunc((y - qb) / mb);
	} else {
		x = (int16_t)ax0; y = u_trunc(mb * x + qb);
	}
	set_var(rt, l, lb7_rd16(bytes + 2), x);
	set_var(rt, l, lb7_rd16(bytes + 4), y);
}

/* Builtin 7C COLLIDE (1000:0E8E): collision between two bodies of equal mass along the direction from
 * (a0,a1) to (a2,a3), or along the angle a0 if a1 = 30000. Scripts often pass the same
 * address for the two velocities: everything is read first and written in the original's order. */
static void u_collide(Rt *rt, const int16_t *a)
{
	int16_t *v1 = rt_int2ptr(rt, a[4]), *g1 = rt_int2ptr(rt, a[5]);
	int16_t *v2 = rt_int2ptr(rt, a[6]), *g2 = rt_int2ptr(rt, a[7]);
	if (!v1 || !g1 || !v2 || !g2 || (!*v1 && !*v2))
		return;
	int16_t al = a[1] == 30000 ? a[0] : u_norm(u_getangle((int16_t)(a[2] - a[0]), (int16_t)(a[3] - a[1])));
	*g1 = (int16_t)(*g1 - al);
	*g2 = (int16_t)(*g2 - al);
	int32_t A = (int32_t)u_cosine(*g1) * *v1 / 10000, B = (int32_t)u_sine(*g1) * *v1 / 10000;
	int32_t C = (int32_t)u_cosine(*g2) * *v2 / 10000, D = (int32_t)u_sine(*g2) * *v2 / 10000;
	int32_t s1 = (int32_t)((uint32_t)(C * C) + (uint32_t)(B * B));
	int32_t s2 = (int32_t)((uint32_t)(D * D) + (uint32_t)(A * A));
	*v1 = s1 < 0 ? 0 : u_trunc(sqrtl((long double)s1));
	*v2 = s2 < 0 ? 0 : u_trunc(sqrtl((long double)s2));
	*g1 = u_norm((int16_t)(u_atan_raw(B, C) + al));
	*g2 = u_norm((int16_t)(u_atan_raw(D, A) + al));
}

/* Loaded type 17 sprite (FUN_1008_74FA with type 0x11). */
static int spr_ok(Rt *rt, unsigned id)
{
	return rt->spr && lb7_entry(&rt->lib, id).type == 17 && spr_loaded(rt->spr, id);
}

int16_t rt_builtin(Rt *rt, unsigned id, unsigned argc, const int16_t *a)
{
	(void)argc;
	cov_builtin(id);
	switch (id) {
	case 0x2E: { /* random number between two inclusive bounds */
		int lo = a[0], hi = a[1];
		if (hi < lo) { int t = lo; lo = hi; hi = t; }
		return (int16_t)(lo + (int)(rt_rand(rt) % (unsigned)(hi - lo + 1)));
	}
	case 0x5A: /* immediate animation command {sprite, command, values...} (S_066) */
	case 0x5B: /* queued animation command (S_068) */
		if (rt->spr)
			spr_command(rt->spr, id == 0x5A, argc, a);
		return 0;
	case 0x62: return (int16_t)(a[0] < 0 ? -a[0] : a[0]);
	case 0x80: { /* SQUAREROOT */
		long v = a[0], x = 0;
		while ((x + 1) * (x + 1) <= v)
			x++;
		return (int16_t)x;
	}
	case 0x90: return 0x1F; /* GETVERSION: 0x5F if the high byte (minor version) is 0x5F = Windows 95, otherwise 0x1F (TOPSY Engine_Command) */
	case 0x93: /* idle timeout (DAT_150C): (script, seconds); input re-arms it */
		rt->timer_script = a[0] ? (uint16_t)(a[0] - 0x7531) : 0; /* encoded operand */
		if (rt->timer_script) {
			rt->timer_ms = (uint16_t)(a[1] * 1000); /* 16-bit multiplication like the original */
			rt->timer_due_ms = rt->now_ms + rt->timer_ms;
		}
		tr(rt, "   idle timeout: script %u in %u ms\n", (uint16_t)(rt->timer_script + 0x7531), rt->timer_ms);
		return 0;
	case 0x89: { /* {player, release the other}: player device (1008:5636) */
		unsigned p = (uint8_t)a[0];
		if (p > 1)
			return 0;
		struct RtPad *d = &rt->pad[p];
		int mouse = d->src == 5;
		if (d->src < sizeof(rt->pad_map))
			rt->pad_map[d->src] = (uint8_t)p;
		if (a[1]) {
			unsigned o = p ^ 1;
			struct RtPad *od = &rt->pad[o];
			if (od->src && od->src < sizeof(rt->pad_map) && rt->pad_map[od->src] == o) {
				od->active = 0;
				pad_enable(rt, od, 0);
				if (od->type == 5)
					rt->pad_mouse = mouse;
			}
		}
		d->active = 1;
		pad_enable(rt, d, 1);
		if (d->type == 5)
			rt->pad_mouse = 1;
		tr(rt, "   player %u: device %u active%s\n", p, d->src, a[1] ? ", the other released" : "");
		return 0;
	}
	case 0x82: /* swap of joysticks 1 and 2 (DAT_5B7F) */
		rt->joy_swap = a[0] != 0;
		return 0;
	case 0x87: return a[0] < a[1] ? a[0] : a[1]; /* min */
	case 0x88: return a[0] > a[1] ? a[0] : a[1]; /* max */
	/* DAT_10DE: free memory (GetFreeSpace) in 64 KB units; the startup scripts
	 * compare it with 53 and 106 to choose the detail. On a modern PC: maximum. */
	case 0x8C: return 0x7FFF;
	case 0x9A: return a[0]; /* DAT_5B92 = argument */
	case 0x69: { /* keyboard filter script (DAT_1506); returns the previous one, decoded */
		uint16_t old = (uint16_t)rt->v1506;
		rt->v1506 = (int16_t)(a[0] ? (uint16_t)(a[0] + 0x8ACF) : 0);
		tr(rt, "   keyboard filter = script %d\n", a[0]);
		return rt_operand(rt, rt->lstack, old); /* 0 -> G(0), like the original */
	}
	case 0x65: { /* copy of a sprite (FUN_1008_7188 -> RESCOPYRESOURCE) and loading */
		unsigned src = (uint16_t)a[0];
		if (lb7_entry(&rt->lib, src).type != 17)
			return 0;
		unsigned id = lb7_copy_entry(&rt->lib, src);
		if (!id)
			return 0;
		if (rt->obj && src < rt->n_obj && id < rt->n_obj) /* the in-memory data is copied */
			rt->obj[id] = rt->obj[src];
		tr(rt, "   copy of sprite %u -> %u\n", src, id);
		(void)rt_load(rt, id); /* FUN_1008_740E(0x11, ...) */
		return (int16_t)id;
	}
	case 0x66: { /* deletes a copy (FUN_1008_71CC): unloads the sprite and frees the slot */
		unsigned id = (uint16_t)a[0];
		if (lb7_entry(&rt->lib, id).type != 17 || !rt_unload(rt, id) || !lb7_delete_entry(&rt->lib, id))
			return 0;
		if (rt->obj && id < rt->n_obj)
			memset(&rt->obj[id], 0, sizeof(RtObj));
		tr(rt, "   copy %u deleted\n", id);
		return 1;
	}
	case 0x39: /* lstrcmp / lstrcmpi of two strings (id), -1/0/1; 0 if one is missing (1008:1002) */
	case 0x3A: {
		const char *x = rt_string(rt, (uint16_t)a[0]), *y = rt_string(rt, (uint16_t)a[1]);
		if (!x || !y)
			return 0;
		int c = id == 0x39 ? strcmp(x, y) : strcasecmp(x, y);
		return (int16_t)(c < 0 ? -1 : c > 0);
	}
	case 0x44: /* sprite visible? (1008:1839 -> S_063); 0 if the sprite is not loaded */
		return (int16_t)(rt->spr && spr_visible(rt->spr, (uint16_t)a[0]));
	case 0x6B: /* {set, slot 1..8, key}: key of a controller set (FUN_1008_5DFC) */
		if (a[1] >= 1 && a[1] <= 8) {
			static const uint8_t slot[9] = { 0, 8, 10, 0, 12, 2, 6, 4, 14 }; /* DS:0042 + set*16 */
			rt->keyset[a[0] & 1][slot[a[1]]] = (uint8_t)a[2];
			tr(rt, "   key set %d slot %d = %d\n", a[0] & 1, a[1], a[2]);
		}
		return 0;
	case 0x31: /* GETANGLE {x0, y0, x1, y1}: angle from (x0,y0) to (x1,y1) (1008:12E4) */
		if (argc < 4)
			return 0;
		return u_getangle((int16_t)(a[2] - a[0]), (int16_t)(a[3] - a[1]));
	case 0x41: /* COSINE {angle}: 10000 * cos (1008:13AE) */
		return argc ? u_cosine(a[0]) : 0;
	case 0x42: /* SINE {angle} (1008:13C6) */
		return argc ? u_sine(a[0]) : 0;
	case 0x7E: { /* DISTANCE {x0, y0, x1, y1} (1008:1CCA -> 1000:1044) */
		if (argc < 4)
			return 0;
		int32_t dy = (int16_t)(a[3] - a[1]), dx = (int16_t)(a[2] - a[0]);
		int32_t s = (int32_t)((uint32_t)(dy * dy) + (uint32_t)(dx * dx));
		return s < 0 ? 0 : u_trunc(sqrtl((long double)s));
	}
	case 0x7F: /* REFLECTANGLE {direction, surface} (1008:1D1A -> 1000:10B4) */
		return argc < 2 ? 0 : u_norm((int16_t)(2 * a[1] - a[0]));
	case 0x85: /* GETQUADRANT {angle}, without normalizing (1000:0D84) */
		return argc ? (int16_t)(a[0] < 900 ? 0 : a[0] < 1800 ? 1 : a[0] < 2700 ? 2 : 3) : 0;
	case 0x7C: /* COLLIDE {x0, y0, x1, y1, &v1, &ang1, &v2, &ang2} (1008:1BC2) */
		if (argc >= 8)
			u_collide(rt, a);
		return 0;
	case 0x7A: /* {high, low, &high, &low}: N *= D, numbers in base 10000 over two words (1008:1AE2) */
	case 0x7B: { /* N /= D (1008:1B5A); 32-bit integers, not fixed point */
		if (argc < 4)
			return 0;
		int16_t *ph = rt_int2ptr(rt, a[2]), *pl = rt_int2ptr(rt, a[3]);
		if (!ph || !pl)
			return 0;
		int32_t n = (int32_t)((uint32_t)((int32_t)*ph * 10000) + (uint32_t)(int32_t)*pl);
		int32_t d = (int32_t)((uint32_t)((int32_t)a[0] * 10000) + (uint32_t)(int32_t)a[1]);
		int32_t r;
		if (id == 0x7A)
			r = (int32_t)((uint32_t)n * (uint32_t)d);
		else if (d) /* the original with d = 0 divides by zero: here the number is left as is */
			r = n / d;
		else
			return 0;
		*ph = (int16_t)(r / 10000);
		*pl = (int16_t)(r % 10000);
		return 0;
	}
	case 0x83: { /* {sprite}: word +0x44 of the sprite (S_077); sprite absent = the cell stays 0x83 */
		int16_t v = 0;
		if (!argc || !spr_ok(rt, (uint16_t)a[0]) || !spr_get_user(rt->spr, (uint16_t)a[0], &v))
			return 0x83;
		return v;
	}
	case 0x84: /* {sprite, value}: writes the word +0x44 (S_078) */
		if (argc < 2 || !spr_ok(rt, (uint16_t)a[0]))
			return 0x84;
		spr_set_user(rt->spr, (uint16_t)a[0], a[1]);
		return 0;
	case 0x86: { /* {A, B, A also hidden, B also hidden, same layer}: pixel-level overlap
		(1008:1E50 -> S_048) */
		if (argc < 5 || !spr_ok(rt, (uint16_t)a[0]) || !spr_ok(rt, (uint16_t)a[1]))
			return 0;
		int la = 0, lb = 0;
		if (a[4] && spr_get_layer(rt->spr, (uint16_t)a[0], &la) && spr_get_layer(rt->spr, (uint16_t)a[1], &lb)
			&& la != lb)
			return 0;
		return (int16_t)spr_collide(rt->spr, (uint16_t)a[0], (uint16_t)a[1], a[2], a[3]);
	}
	case 0x8A: /* {sprite, x, y}: the point falls on the sprite (1008:1F78 -> S_079) */
		if (argc < 3 || !spr_ok(rt, (uint16_t)a[0]))
			return 0x8A;
		return (int16_t)spr_hit_id(rt->spr, (uint16_t)a[0], a[1], a[2]);
	case 0x78: /* {dx, dy}: window shake with ScrollDC forward, back and forward
		(S_075): net displacement is zero, not visible in the port */
		tr(rt, "   shake %d,%d\n", argc ? a[0] : 0, argc > 1 ? a[1] : 0);
		return 0;
	case 0x79: { /* {high, low, &high, &low}: (*&high, *&low) += (high, low), number in base
		10000 as two words (scores); 32-bit arithmetic of the CRT (1018:05EE/0554/0620) */
		int16_t *ph = rt_int2ptr(rt, a[2]), *pl = rt_int2ptr(rt, a[3]);
		int32_t v = ((int32_t)a[0] + *ph) * 10000 + *pl + a[1];
		*ph = (int16_t)(v / 10000);
		*pl = (int16_t)(v % 10000);
		return 0;
	}
	case 0x6E: /* DAT_5B7E: alternate joystick reading (FUN_1008_575C); no joystick */
		rt->joy_alt = a[0] != 0;
		return 0;
	case 0x73: { /* {&x, &y, passo, passi, x0, y0, xm, ym, x1, y1}: point of the parabola that passes
		through (x0,y0) at the start, (xm,ym) at the middle and (x1,y1) at the end (S_074 -> TOPSYS01 1000:1758) */
		if (argc < 10 || !(uint16_t)a[3])
			return 0;
		uint32_t total = (uint16_t)a[3];
		int64_t u = (int64_t)(((uint32_t)(uint16_t)a[2] * 0x400u + (total >> 1)) / total);
		int64_t uu = u * u, um = (u & 0xFFFF) * (0x400 - u);
		int16_t r[2];
		for (int k = 0; k < 2; k++) {
			int v0 = a[4 + k], vm = a[6 + k], v1 = a[8 + k];
			int64_t q = uu * (v1 - v0) + um * ((int16_t)((vm - v0) * 4) - (v1 - v0));
			r[k] = (int16_t)((int16_t)(q >> 20) + v0);
		}
		int16_t *px = rt_int2ptr(rt, a[0]), *py = rt_int2ptr(rt, a[1]);
		if (px)
			*px = r[0];
		if (py)
			*py = r[1];
		return 0;
	}
	case 0x6C: /* {sprite (0 = all), pause}: animations stopped or resumed (S_071 -> 1000:66F6) */
		if (rt->spr)
			spr_pause(rt->spr, (uint16_t)a[0], a[1] != 0);
		tr(rt, "   pause sprite %d = %d\n", a[0], a[1]);
		return 0;
	case 0x6D: /* {pause}: timers stopped; on resume the deadlines shift (1008:DFEC) */
		if (!a[0] != !rt->timers_paused) {
			if (a[0])
				rt->timers_paused_at = rt->now_ms;
			else
				for (int k = 0; k < rt->n_timers; k++)
					rt->timers[k].due += rt->now_ms - rt->timers_paused_at;
			rt->timers_paused = a[0] != 0;
		}
		return 0;
	case 0x70: /* pause (1) / resume (0) of all sounds (A_042: wave e musica) */
		if (!a[0] != !rt->waves_paused) {
			pause_waves(rt, a[0] != 0);
			pause_midi(rt, a[0] != 0);
		}
		return 0;
	case 0x99: /* pause (1) / resume (0) of the music, with counter (A_025) */
		pause_midi(rt, a[0] != 0);
		return 0;
	case 0x81: /* current mouse filter script (DAT_1504), decoded: used to chain them */
		return rt_operand(rt, rt->lstack, (uint16_t)rt->v1504);
	case 0x6F: { /* script for the cursor over sprites (DAT_1542, FUN_1008_42AE) */
		uint16_t old = rt->hover_script;
		if (!a[0] || lb7_entry(&rt->lib, (uint16_t)a[0]).type == 16) {
			if (rt->hover_script && rt->hover_sprite)
				hover_leave(rt);
			rt->hover_script = a[0] ? (uint16_t)(a[0] + 0x8ACF) : 0;
			tr(rt, "   cursor-over-sprites script = %d\n", a[0]);
			if (a[0])
				(void)hover_check(rt, rt->mouse_x, rt->mouse_y);
		}
		return rt_operand(rt, rt->lstack, old);
	}
	default:
		rt->unimpl_builtin[id & 0xFF]++;
		tr(rt, "   ?? builtin %02X (%u arguments) not yet ported\n", id, argc);
		return 0;
	}
}

/* Numbering of the original DS: globals at 0x154A + 2n, locals at 0x4084 - 2k (k = position
 * in the frame stack, which in the original grew downward). */
int16_t rt_ptr2int(const Rt *rt, const int16_t *p)
{
	if (p >= rt->g && p < rt->g + RT_NGLOBALS)
		return (int16_t)(0x154A + (p - rt->g) * 2);
	if (p >= rt->lstack && p < rt->lstack + RT_LSTACK)
		return (int16_t)(0x4084 - (p - rt->lstack) * 2);
	return 0;
}

int16_t *rt_int2ptr(Rt *rt, int16_t v)
{
	static int16_t dummy;
	uint16_t a = (uint16_t)v;
	if (a >= 0x154A && a < 0x154A + RT_NGLOBALS * 2 && !(a & 1) == !(0x154A & 1))
		return &rt->g[(a - 0x154A) / 2];
	if (a <= 0x4084 && (0x4084 - a) / 2 < RT_LSTACK)
		return &rt->lstack[(0x4084 - a) / 2];
	tr(rt, "!! invalid address 0x%04X\n", a);
	return &dummy;
}
