/*
 * SDL2 port of the 7th Level Runtime Player 1.2. The game (title, original EXE, first library,
 * file names) is described by its profile games\<name>.h, copied by build.bat to build\game.h;
 * below, GAME = GAME_NAME (e.g. bowl).
 *
 * Current stage (Step 5.3): viewer for a 7LB archive with the game palette, bitmaps,
 * sounds (SDL2 mixer) and MIDI music (TinySoundFont + GeneralUser GS). Used to check graphics,
 * palette, scaling and audio ahead of the script interpreter.
 *
 *   GAME [--data FOLDER] [--lib FILE.BIN] [--entry N] [--800] [--scale N] [--sf2 FILE.sf2]
 *   GAME [--data FOLDER] [--lib FILE.BIN] [--sf2 FILE.sf2] --mixdown N out.wav   (audio test)
 *   GAME [--data FOLDER] --run SECONDS [--trace FILE]   (translated game logic, no window)
 *   GAME [--data FOLDER] [--lib FILE.BIN] [--800] [--scale N] --game [--trace FILE] [--input FILE]
 *        (game; --input = timed events for automated checks, see play_input)
 *
 * Viewer keys: left/right arrows = previous/next bitmap, PageUp/PageDown =
 *        jump by 50, W = play the next wave, M = play the next MIDI, S = stop sounds
 *        and music, F11 = fullscreen, Esc = quit.
 * In the game (--game) keyboard and mouse go to the scripts as in the original (Esc, F11 included);
 *        Alt+Enter = fullscreen, closing the window = quit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "audio.h"
#include "config.h"
#include "lb7.h"
#include "ne.h"
#include "rt.h"
#include "cov.h"
#include "sprite.h"
#include "text.h"
#include "video.h"
#include "game.h"

#define DEFAULT_LIB GAME_VIEWER
#define SF2_NAME    "GeneralUser-GS.sf2"
static Config g_cfg; /* <game>.ini */

typedef struct {
	Lb7 lb;
	uint32_t *bitmaps; /* indices of the bitmap-type entries */
	uint32_t n_bitmaps;
	uint32_t cur;      /* position in bitmaps[] */
	uint32_t wave;     /* last wave entry played */
	uint32_t midi;     /* last MIDI entry played */
	const char *libname;
} Viewer;

/* Plays the wave after vw->wave and returns the entry index (or -1). */
static long play_next_wave(Viewer *vw)
{
	for (uint32_t k = 1; k <= vw->lb.n_entries; k++) {
		uint32_t i = (vw->wave + k) % vw->lb.n_entries;
		if (lb7_entry(&vw->lb, i).type == LB7_T_WAVE && audio_play_wave(&vw->lb, i, 0, 256)) {
			vw->wave = i;
			return i;
		}
	}
	return -1;
}

/* Plays the MIDI after vw->midi and returns the entry index (or -1). */
static long play_next_midi(Viewer *vw)
{
	for (uint32_t k = 1; k <= vw->lb.n_entries; k++) {
		uint32_t i = (vw->midi + k) % vw->lb.n_entries;
		if (lb7_entry(&vw->lb, i).type == LB7_T_MIDI && audio_play_midi(&vw->lb, i, 0)) {
			vw->midi = i;
			return i;
		}
	}
	return -1;
}

/* Looks for the SoundFont: --sf2, then tools\soundfont\ relative to the current folder and the executable. */
static int load_soundfont(const char *sf2)
{
	char path[1024];
	char *base = SDL_GetBasePath();
	const char *cand[3] = { sf2, "tools/soundfont/" SF2_NAME, NULL };
	char a[1024];
	if (base) {
		snprintf(a, sizeof(a), "%s%s", base, SF2_NAME);
		cand[2] = a;
	}
	int ok = 0;
	for (int i = 0; i < 3 && !ok; i++) {
		if (!cand[i])
			continue;
		snprintf(path, sizeof(path), "%s", cand[i]);
		ok = audio_init_midi(path);
	}
	SDL_free(base);
	return ok;
}

/* Writes the mixer output for the entry to a 44100 Hz stereo WAV (offline test):
 * wave (type 9) or MIDI (type 4, with a 2 s tail for the note release). */
static int mixdown(const Lb7 *lb, uint32_t entry, const char *path)
{
	int is_midi = lb7_entry(lb, entry).type == LB7_T_MIDI;
	int id = is_midi ? audio_play_midi(lb, entry, 0) : audio_play_wave(lb, entry, 0, 256);
	if (!id)
		return 0;
	FILE *f = fopen(path, "wb");
	if (!f)
		return 0;
	uint8_t h[44] = { 0 };
	fwrite(h, 1, 44, f);
	uint32_t bytes = 0, tail = is_midi ? AUDIO_RATE * 2 / 1024 : 0;
	int16_t buf[1024 * 2];
	while (is_midi ? (audio_midi_playing() || tail--) : audio_is_playing(id)) {
		audio_render(buf, 1024);
		fwrite(buf, 1, sizeof(buf), f);
		bytes += sizeof(buf);
	}
	memcpy(h, "RIFF", 4);
	memcpy(h + 8, "WAVEfmt ", 8);
	memcpy(h + 36, "data", 4);
	uint32_t v[] = { 36 + bytes, 16, 0x00020001u, AUDIO_RATE, AUDIO_RATE * 4, 0x00100004u, bytes };
	memcpy(h + 4, &v[0], 4);
	memcpy(h + 16, &v[1], 4);
	memcpy(h + 20, &v[2], 4);  /* PCM, 2 channels */
	memcpy(h + 24, &v[3], 4);
	memcpy(h + 28, &v[4], 4);
	memcpy(h + 32, &v[5], 4);  /* block align 4, 16 bit */
	memcpy(h + 40, &v[6], 4);
	fseek(f, 0, SEEK_SET);
	fwrite(h, 1, 44, f);
	fclose(f);
	return 1;
}

/* Data folder: --data, current folder or the executable's folder (not the one above). */
static void find_datadir(const char *datadir, const char *probe, char *out, size_t n)
{
	char *base = SDL_GetBasePath();
	const char *dirs[3] = { datadir, ".", base };
	snprintf(out, n, "%s", datadir ? datadir : ".");
	for (int i = 0; i < 3; i++) {
		char path[1024];
		if (!dirs[i])
			continue;
		snprintf(path, sizeof(path), "%s/%s", dirs[i], probe);
		FILE *f = fopen(path, "rb");
		if (f) {
			fclose(f);
			snprintf(out, n, "%s", dirs[i]);
			break;
		}
	}
	SDL_free(base);
}

/* Game clock, shared by runtime, sprites and --input: real (SDL_GetTicks) or simulated
 * with a fixed step (--fixed-step MS), for repeatable runs. With the fixed step the simulated
 * time starts at 0 and advances by MS at every loop tick (16 ms per frame of a fade);
 * the loop waits for the corresponding real time, except with --fast. */
static unsigned clk_step;   /* 0 = real time */
static int clk_fast;        /* --fast: no waiting (only with --fixed-step) */
static uint32_t clk_sim, clk_real0;
static long opt_seed = -1;  /* --seed N: rand() seed (-1 = from the time, like the original) */
static const char *opt_ini; /* --ini FILE: 7THLEVEL.INI to use (default: current folder) */
static const char *opt_cov; /* --coverage FILE: coverage report at the end of the run */

/* Writes the --coverage report (before rt_shutdown). */
static void write_coverage(void)
{
	if (!opt_cov)
		return;
	FILE *f = fopen(opt_cov, "w");
	if (!f) {
		fprintf(stderr, GAME_NAME ": cannot write %s\n", opt_cov);
		return;
	}
	cov_write(f);
	fclose(f);
	cov_free();
}

static uint32_t clk_now(void)
{
	return clk_step ? clk_sim : SDL_GetTicks();
}

/* End of a tick: advances the simulated time and waits for the real time to catch up. */
static void clk_advance(unsigned ms)
{
	clk_sim += ms;
	while (!clk_fast && SDL_GetTicks() - clk_real0 < clk_sim)
		SDL_Delay(1);
}

/* Options for repeatable tests: INI (--ini) and rand() seed; the original does
 * srand(timeGetTime()) (FUN_1008_C794), with 16-bit int. */
static void test_options(Rt *rt, FILE *trace)
{
	if (opt_ini)
		snprintf(rt->ini_path, sizeof(rt->ini_path), "%s", opt_ini);
	rt->rand_seed = (opt_seed >= 0 ? (uint32_t)opt_seed : SDL_GetTicks()) & 0xFFFF;
	if (trace)
		fprintf(trace, "seed %u, %s %u ms\n", rt->rand_seed, clk_step ? "fixed step" : "real time, minimum tick",
			clk_step ? clk_step : 10);
}

/* Game profile at run time: 7THLEVEL.INI section = string 0xD2 of the original EXE
 * (LoadString in FUN_1000_05E4, DS:5B98), GAME_INI_SECTION if the EXE is missing. */
static void game_profile(Rt *rt, const char *dir, FILE *trace)
{
	char exe[1100];
	snprintf(exe, sizeof(exe), "%s/%s", dir, GAME_EXE_RES);
	int ok = ne_string(exe, 0xD2, rt->app_section, sizeof(rt->app_section)) && rt->app_section[0];
	if (!ok)
		snprintf(rt->app_section, sizeof(rt->app_section), "%s", GAME_INI_SECTION);
#ifdef GAME_INI_NO_UNESCAPE
	rt->ini_raw = 1;
#endif
	if (trace)
		fprintf(trace, "game %s, start %s, INI section [%s]%s\n", GAME_TITLE, GAME_START,
			rt->app_section, ok ? "" : " (" GAME_EXE_RES " not found)");
}

/* Starts the game logic translated from GAME_START (the original EXE opens the .BIN with its
 * own name, FUN_1000_0756) and simulates time in 50 ms steps (or --fixed-step) to trigger timers
 * and library changes. */
static int run_game(const char *datadir, int seconds, const char *tracefile)
{
	static Rt rt;
	char dir[1024];
	FILE *trace = tracefile ? fopen(tracefile, "w") : NULL;
	find_datadir(datadir, GAME_START, dir, sizeof(dir));
	rt_init(&rt, dir, trace ? trace : stdout);
	game_profile(&rt, dir, trace ? trace : stdout);
	test_options(&rt, trace ? trace : stdout);
	rt_load_settings(&rt);
	if (!rt_open_library(&rt, GAME_START)) {
		fprintf(stderr, GAME_NAME ": start from " GAME_START " failed (folder %s)\n", dir);
		return 1;
	}
	unsigned step = clk_step ? clk_step : 50;
	for (uint32_t t = 0; t <= (uint32_t)seconds * 1000u && !rt.quit; t += step)
		rt_tick(&rt, t);
	printf("final library: %s, quit=%d\n", rt.code ? rt.code->file : "-", rt.quit);
	rt_report(&rt, stdout);
	if (trace)
		rt_report(&rt, trace);
	write_coverage();
	rt_shutdown(&rt);
	if (trace)
		fclose(trace);
	return 0;
}

/* Windows virtual-key code (VK_*) of an SDL key, as the scripts received it in WM_KEYDOWN.
 * Letters and digits from the keysym (respects the keyboard layout, like Windows), the other
 * keys from the scancode. 0 = key with no equivalent. */
static unsigned sdl_to_vk(const SDL_Keysym *k)
{
	SDL_Keycode s = k->sym;
	if (s >= SDLK_a && s <= SDLK_z)
		return 'A' + (unsigned)(s - SDLK_a);
	if (s >= SDLK_0 && s <= SDLK_9)
		return '0' + (unsigned)(s - SDLK_0);
	SDL_Scancode c = k->scancode;
	if (c >= SDL_SCANCODE_F1 && c <= SDL_SCANCODE_F12)
		return 0x70 + (unsigned)(c - SDL_SCANCODE_F1);
	if (c >= SDL_SCANCODE_KP_1 && c <= SDL_SCANCODE_KP_9)
		return 0x61 + (unsigned)(c - SDL_SCANCODE_KP_1); /* VK_NUMPAD1.. */
	switch (c) {
	case SDL_SCANCODE_KP_0: return 0x60;
	case SDL_SCANCODE_KP_MULTIPLY: return 0x6A;
	case SDL_SCANCODE_KP_PLUS: return 0x6B;
	case SDL_SCANCODE_KP_MINUS: return 0x6D;
	case SDL_SCANCODE_KP_PERIOD: return 0x6E;
	case SDL_SCANCODE_KP_DIVIDE: return 0x6F;
	case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_RETURN: return 0x0D;
	case SDL_SCANCODE_BACKSPACE: return 0x08;
	case SDL_SCANCODE_TAB: return 0x09;
	case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x10;
	case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x11;
	case SDL_SCANCODE_PAUSE: return 0x13;
	case SDL_SCANCODE_CAPSLOCK: return 0x14;
	case SDL_SCANCODE_ESCAPE: return 0x1B;
	case SDL_SCANCODE_SPACE: return 0x20;
	case SDL_SCANCODE_PAGEUP: return 0x21;
	case SDL_SCANCODE_PAGEDOWN: return 0x22;
	case SDL_SCANCODE_END: return 0x23;
	case SDL_SCANCODE_HOME: return 0x24;
	case SDL_SCANCODE_LEFT: return 0x25;
	case SDL_SCANCODE_UP: return 0x26;
	case SDL_SCANCODE_RIGHT: return 0x27;
	case SDL_SCANCODE_DOWN: return 0x28;
	case SDL_SCANCODE_INSERT: return 0x2D;
	case SDL_SCANCODE_DELETE: return 0x2E;
	case SDL_SCANCODE_NUMLOCKCLEAR: return 0x90;
	case SDL_SCANCODE_SCROLLLOCK: return 0x91;
	case SDL_SCANCODE_SEMICOLON: return 0xBA;
	case SDL_SCANCODE_EQUALS: return 0xBB;
	case SDL_SCANCODE_COMMA: return 0xBC;
	case SDL_SCANCODE_MINUS: return 0xBD;
	case SDL_SCANCODE_PERIOD: return 0xBE;
	case SDL_SCANCODE_SLASH: return 0xBF;
	case SDL_SCANCODE_GRAVE: return 0xC0;
	case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
	case SDL_SCANCODE_BACKSLASH: return 0xDC;
	case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
	case SDL_SCANCODE_APOSTROPHE: return 0xDE;
	default: return 0;
	}
}

/* UTF-8 character from SDL_TEXTINPUT -> Windows ANSI code (cp1252), like WM_CHAR;
 * 0 = not representable (Windows would not have sent it). *p advances to the next character. */
static unsigned utf8_to_cp1252(const char **p)
{
	static const uint16_t hi[32] = { /* 0x80..0x9F of cp1252 */
		0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039,
		0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC,
		0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178 };
	const unsigned char *s = (const unsigned char *)*p;
	unsigned cp = *s++, more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
	if (more)
		cp &= 0x3F >> more;
	for (; more && (*s & 0xC0) == 0x80; more--)
		cp = cp << 6 | (*s++ & 0x3F);
	*p = (const char *)s;
	if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF))
		return cp;
	for (unsigned i = 0; i < 32; i++)
		if (hi[i] == cp)
			return 0x80 + i;
	return 0;
}

/* SDL events -> the original's WndProc (TOPSY 1008:ED3C). Returns 0 to quit. */
/* Open joysticks (SDL), in Windows order: 0 = JOYSTICKID1, 1 = JOYSTICKID2 */
static SDL_Joystick *joys[2];
static int n_joys, fake_joys = -1;

static int joy_index(SDL_JoystickID id)
{
	for (int i = 0; i < n_joys; i++)
		if (SDL_JoystickInstanceID(joys[i]) == id)
			return i;
	return -1;
}

static int game_event(Rt *rt, Video *v, const SDL_Event *ev)
{
	int cx = video_width(v) / 2, cy = video_height(v) / 2; /* viewport origin */
	switch (ev->type) {
	case SDL_QUIT:
		return 0;
	case SDL_WINDOWEVENT: /* WM_ACTIVATEAPP / WM_NCACTIVATE -> FUN_1008_E896 */
		if (ev->window.event == SDL_WINDOWEVENT_FOCUS_LOST || ev->window.event == SDL_WINDOWEVENT_MINIMIZED)
			rt_set_active(rt, 0);
		else if (ev->window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
			rt_set_active(rt, 1);
		break;
	case SDL_KEYDOWN: {
		SDL_Keymod mod = SDL_GetModState();
		if (ev->key.keysym.scancode == SDL_SCANCODE_RETURN && (mod & KMOD_ALT)) {
			if (!ev->key.repeat)
				video_toggle_fullscreen(v);
			break;
		}
		unsigned vk = sdl_to_vk(&ev->key.keysym);
		/* WM_SYSKEYDOWN (Alt pressed) goes to the scripts only for F10 (FUN_1008_EB6C) */
		if (vk && (!(mod & KMOD_ALT) || vk == 0x79)) /* repeats arrive as in Windows */
			rt_key_down(rt, vk, (mod & KMOD_SHIFT) != 0, (mod & KMOD_CTRL) != 0);
		/* WM_CHAR for control keys (TranslateMessage); printable characters arrive
		   from SDL_TEXTINPUT */
		if (!(mod & KMOD_ALT) && (vk == 0x08 || vk == 0x0D || vk == 0x1B))
			rt_char(rt, vk);
		break;
	}
	case SDL_TEXTINPUT: /* printable WM_CHAR */
		for (const char *p = ev->text.text; *p;) {
			unsigned c = utf8_to_cp1252(&p);
			if (c >= 0x20)
				rt_char(rt, c);
		}
		break;
	case SDL_JOYAXISMOTION: { /* MM_JOYnMOVE: position of axes 0 and 1 */
		int j = joy_index(ev->jaxis.which);
		if (j >= 0 && ev->jaxis.axis < 2)
			rt_joy_move(rt, j, SDL_JoystickGetAxis(joys[j], 0), SDL_JoystickGetAxis(joys[j], 1));
		break;
	}
	case SDL_JOYBUTTONDOWN:
	case SDL_JOYBUTTONUP: { /* MM_JOYnBUTTONDOWN/UP */
		int j = joy_index(ev->jbutton.which);
		if (j >= 0)
			rt_joy_button(rt, j, ev->jbutton.button, ev->type == SDL_JOYBUTTONDOWN);
		break;
	}
	case SDL_KEYUP: {
		unsigned vk = sdl_to_vk(&ev->key.keysym);
		if (vk)
			rt_key_up(rt, vk);
		break;
	}
	case SDL_MOUSEMOTION: /* coordinates already in the logical screen (SDL_RenderSetLogicalSize) */
		rt_mouse_move(rt, ev->motion.x - cx, ev->motion.y - cy);
		break;
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP: {
		int b = ev->button.button == SDL_BUTTON_LEFT ? 1 : ev->button.button == SDL_BUTTON_RIGHT ? 2 : 0;
		if (!b)
			break;
		int down = ev->type == SDL_MOUSEBUTTONDOWN;
		SDL_CaptureMouse(down ? SDL_TRUE : SDL_FALSE); /* SetCapture / ReleaseCapture */
		rt_mouse_button(rt, ev->button.x - cx, ev->button.y - cy, b, down);
		break;
	}
	}
	return 1;
}

/* Timed events for checks (--input FILE), one line per event, '#' = comment:
 *   MS key VK | keydown VK | keyup VK      (VK = Windows virtual-key code, e.g. 32 or 0x1B)
 *   MS click X Y | rclick X Y | move X Y   (logical screen coordinates, 0,0 at top left;
 *                                           the click releases the button 100 ms later)
 *   MS joymove J X Y | joybutton J B 0/1   (joystick 0/1, axes -32768..32767, buttons 0..3)
 *   MS char CODE | text WORD                (WM_CHAR: ANSI code, or a word without spaces;
 *                                           key/keydown of 8, 13 and 27 also send WM_CHAR)
 *   MS focus 0/1                           (window deactivated / activated)
 *   MS waitpix X Y RRGGBB [TOL] [MAX]      (synchronization with the original: from MS the events
 *                                           stop until pixel X,Y has that color, within
 *                                           TOL per channel (default 24), for at most MAX ms
 *                                           (default 30000; TOL < 0: until the color differs
 *                                           by more than -TOL); later times shift
 *                                           by the wait, as in tools\scripts\orig_play.ps1)
 *   MS script ID | global N V              (tests only: runs script ID / sets G(N) = V)
 *   MS shot FILE.bmp | quit
 * MS = milliseconds since the game started. Returns 0 after "quit". */
static int play_input(FILE *in, uint32_t elapsed, Rt *rt, Video *v)
{
	static char line[512];
	static int pending, up_btn, up_x, up_y;
	static uint32_t up_ms, shift; /* shift: sum of the waitpix waits */
	static int waiting;
	static uint32_t wait_from;
	uint32_t real = elapsed;
	elapsed = real - shift;
	int cx = video_width(v) / 2, cy = video_height(v) / 2;
	if (up_btn && real >= up_ms) { /* click release, 100 ms after the press */
		rt_mouse_button(rt, up_x - cx, up_y - cy, up_btn, 0);
		up_btn = 0;
	}
	for (;;) {
		if (!pending) {
			if (!fgets(line, sizeof(line), in))
				return 1;
			pending = 1;
		}
		char cmd[32] = "", arg[400] = "";
		unsigned long ms;
		long a = 0, b = 0;
		if (line[0] == '#' || sscanf(line, "%lu %31s %399s", &ms, cmd, arg) < 2) {
			pending = 0;
			continue;
		}
		if (ms > elapsed)
			return 1;
		if (!strcmp(cmd, "waitpix")) {
			long x = 0, y = 0, tol = 24, max = 30000;
			unsigned long rgb = 0;
			sscanf(line, "%*u %*s %ld %ld %lx %ld %ld", &x, &y, &rgb, &tol, &max);
			long c = video_pixel(v, (int)x, (int)y);
			if (!waiting) {
				waiting = 1;
				wait_from = real;
			}
			long d = c < 0 ? 999 : labs((long)(c >> 16 & 0xFF) - (long)(rgb >> 16 & 0xFF));
			if (c >= 0 && labs((long)(c >> 8 & 0xFF) - (long)(rgb >> 8 & 0xFF)) > d)
				d = labs((long)(c >> 8 & 0xFF) - (long)(rgb >> 8 & 0xFF));
			if (c >= 0 && labs((long)(c & 0xFF) - (long)(rgb & 0xFF)) > d)
				d = labs((long)(c & 0xFF) - (long)(rgb & 0xFF));
			int ok = c >= 0 && (tol >= 0 ? d <= tol : d > -tol); /* TOL < 0: different color */
			if (!ok && real - wait_from < (uint32_t)max)
				return 1;
			uint32_t waited = real - (uint32_t)ms - shift; /* delay relative to the scheduled time */
			printf("waitpix %lu: %s after %u ms (pixel %06lX)\n", ms, ok ? "found" : "TIMED OUT", real - wait_from,
				(unsigned long)(c < 0 ? 0 : c));
			shift += waited;
			elapsed = real - shift;
			waiting = 0;
			pending = 0;
			continue;
		}
		pending = 0;
		a = strtol(arg, NULL, 0);
		if (!strcmp(cmd, "key") || !strcmp(cmd, "keydown")) {
			rt_key_down(rt, (unsigned)a, 0, 0);
			if (a == 0x08 || a == 0x0D || a == 0x1B)
				rt_char(rt, (unsigned)a);
		}
		if (!strcmp(cmd, "char"))
			rt_char(rt, (unsigned)a);
		else if (!strcmp(cmd, "text"))
			for (const char *p = arg; *p; p++)
				rt_char(rt, (unsigned char)*p);
		if (!strcmp(cmd, "key") || !strcmp(cmd, "keyup"))
			rt_key_up(rt, (unsigned)a);
		else if (!strcmp(cmd, "click") || !strcmp(cmd, "rclick") || !strcmp(cmd, "move")) {
			sscanf(line, "%*u %*s %ld %ld", &a, &b);
			rt_mouse_move(rt, (int)a - cx, (int)b - cy);
			if (strcmp(cmd, "move")) {
				up_btn = cmd[0] == 'r' ? 2 : 1;
				up_x = (int)a;
				up_y = (int)b;
				up_ms = real + 100;
				rt_mouse_button(rt, (int)a - cx, (int)b - cy, up_btn, 1);
				return 1; /* the release comes a few frames later, as with a user */
			}
		}
		else if (!strcmp(cmd, "joymove") || !strcmp(cmd, "joybutton")) {
			long c = 0;
			sscanf(line, "%*u %*s %ld %ld %ld", &a, &b, &c);
			if (cmd[3] == 'm')
				rt_joy_move(rt, (int)a, (int)b, (int)c);
			else
				rt_joy_button(rt, (int)a, (int)b, c != 0);
		}
		else if (!strcmp(cmd, "script")) { /* tests only: runs a library script immediately */
			fprintf(rt->trace ? rt->trace : stdout, "%7u -- test: script %ld\n", rt->now_ms, a);
			rt_run(rt, (unsigned)a, rt->lstack);
		}
		else if (!strcmp(cmd, "focus")) /* window activated (1) or deactivated (0) */
			rt_set_active(rt, a != 0);
		else if (!strcmp(cmd, "global")) { /* tests only: G(N) = V */
			sscanf(line, "%*u %*s %ld %ld", &a, &b);
			if (rt->g && a >= 0 && a < 0x13FE)
				rt->g[a] = (int16_t)b;
		}
		else if (!strcmp(cmd, "shot"))
			printf("%s %s\n", video_save_bmp(v, arg) ? "saved" : "ERROR saving", arg);
		else if (!strcmp(cmd, "quit"))
			return 0;
	}
}

/* Game window state shared with the runtime's frame callback. */
static Video *game_video;
static SprWorld game_world;
static const RtLib *game_shown;

/* Palette and title of the current library (they change at every link). */
static void game_sync_library(Rt *rt)
{
	if (rt->code == game_shown)
		return;
	uint8_t rgb[256][3];
	lb7_palette(&rt->lib, rgb);
	video_set_palette(game_video, rgb);
	game_shown = rt->code;
	char t[128];
	snprintf(t, sizeof(t), GAME_TITLE " - %s", rt->code ? rt->code->file : "");
	video_set_title(game_video, t);
}

/* Sprite engine ticks. The original calls S_081 (TOPSYS01 1000:0024) at every tick of
 * PumpMessages (1008:0000), a PeekMessage with no waits: thousands of times per second, with the
 * timeGetTime clock (1 ms). Each tick runs at most one command that "consumes the tick" per
 * sprite (sprite.c, run_anim), so the number of ticks matters: here one for every ms elapsed
 * since the last, at most ENG_CATCHUP in a row (after a long stall the original does not recover
 * the lost ticks: it restarts from the current time). Measured under OTVDM: the climb of the Upsy
 * characters (350 steps 0C + 0B 8) takes ~10 ms per step, like 3 ticks of 1 ms + 8 ms of waiting. */
#define ENG_CATCHUP 50
static uint32_t eng_last;  /* last ms already run */

static uint32_t eng_first(uint32_t now)
{
	if ((int32_t)(now - eng_last) > ENG_CATCHUP)
		eng_last = now - ENG_CATCHUP;
	return eng_last + 1;
}

/* Sprites only (during synchronous fades the script is stopped in the fade: no timers). */
static void sprite_ticks(SprWorld *w, uint32_t now)
{
	for (uint32_t t = eng_first(now); (int32_t)(t - now) <= 0; t++)
		spr_update(w, t);
	eng_last = now;
}

/* One frame: animations, drawing, palette fade, presentation (the original's S_001).
 * The runtime also uses it during synchronous fades (rt->frame). */
static void game_frame(Rt *rt)
{
	game_sync_library(rt);
	uint32_t now = clk_now();
	rt->now_ms = now; /* during a fade time also flows for the timers, like timeGetTime */
	sprite_ticks(&game_world, now);
	video_clear(game_video, 0);
	spr_draw(&game_world, game_video);
	video_set_fade(game_video, rt->fade_level);
	video_present(game_video);
	/* one step every ~16 ms even without vsync: a 100-step fade lasts about 1.6 s */
	if (clk_step)
		clk_advance(16);
	else {
		static uint32_t last;
		while (SDL_GetTicks() - last < 16)
			SDL_Delay(1);
		last = SDL_GetTicks();
	}
	SDL_PumpEvents(); /* the window stays responsive; events stay queued as in the original */
}

/* Game with a window: translated logic (rt), sprites and animations (sprite.c), 8-bit video.
 * Frame loop: time -> timers and library changes -> animations -> drawing. */
static int game_window(const char *datadir, const char *lib, int w, int h, int scale, const char *tracefile,
	const char *inputfile, const char *sf2)
{
	FILE *input = inputfile ? fopen(inputfile, "r") : NULL;
	if (inputfile && !input) {
		fprintf(stderr, GAME_NAME ": cannot open %s\n", inputfile);
		return 1;
	}
	static Rt rt;
	SprWorld *wp = &game_world;
	char dir[1024];
	FILE *trace = tracefile ? fopen(tracefile, "w") : NULL;
	find_datadir(datadir, GAME_START, dir, sizeof(dir));
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0)
		return 1;
	audio_init();
	audio_set_master(g_cfg.volume * 256 / 100); /* <game>.ini: Volume / MusicVolume */
	audio_set_music_volume(g_cfg.music * 256 / 100);
	for (int i = 0; i < SDL_NumJoysticks() && n_joys < 2; i++) /* FUN_1008_59C2: at most 2 */
		if ((joys[n_joys] = SDL_JoystickOpen(i)) != NULL)
			n_joys++;
	{ /* fonts for the sprite texts (type 18): the Windows ones or tools\fonts */
		char *base = SDL_GetBasePath();
		if (!text_init(base))
			fprintf(stderr, GAME_NAME ": no font found, texts not drawn\n");
		SDL_free(base);
		int nf = text_add_dir(dir); /* fonts of the game (HYENA.TTF of the Jungle games) */
		if (trace && nf)
			fprintf(trace, "game fonts in the data folder: %d\n", nf);
	}
	if (!load_soundfont(sf2)) /* script music (opcode 50) */
		fprintf(stderr, GAME_NAME ": SoundFont not found, MIDI music muted\n");
	Video *v = video_create(GAME_TITLE, w, h, scale);
	if (!v)
		return 1;
	game_video = v;
	if (g_cfg.fullscreen)
		video_toggle_fullscreen(v);
	{ /* cursors of the original EXE (opcode 1F); without the EXE the arrow remains */
		char exe[1100];
		snprintf(exe, sizeof(exe), "%s/%s", dir, GAME_EXE_RES);
		if (!video_load_cursors(v, exe))
			fprintf(stderr, GAME_NAME ": cursors from " GAME_EXE_RES " not found, using the arrow\n");
		video_cursor_mode(v, 1); /* DAT_1514 = 1 at startup */
		SDL_StartTextInput(); /* WM_CHAR for the text field (opcode 36) */
	}
	rt_init(&rt, dir, trace);
	game_profile(&rt, dir, trace);
	rt.frame = game_frame;
	rt.screen_w = w; /* G(5003)/G(5004): the game picks 800x600 or 640x480 from here */
	rt.screen_h = h;
	rt.spr = wp;
	test_options(&rt, trace);
	rt_load_settings(&rt);
	rt.joy_n = fake_joys >= 0 ? (fake_joys > 2 ? 2 : fake_joys) : n_joys;
	if (rt.joy_n > rt.max_joy) /* MaxJoySticks */
		rt.joy_n = rt.max_joy < 0 ? 0 : rt.max_joy;
	if (!rt_open_library(&rt, lib)) {
		fprintf(stderr, GAME_NAME ": start from %s failed (folder %s)\n", lib, dir);
		return 1;
	}
	clk_real0 = SDL_GetTicks();
	uint32_t start = clk_now();
	eng_last = start;
	for (int running = 1; running && !rt.quit;) {
		SDL_Event ev;
		uint32_t now = clk_now();
		rt.now_ms = now;
		while (running && !rt.quit && SDL_PollEvent(&ev))
			/* with --input only the file counts: the real mouse over the window would alter the test */
			if (!input || ev.type == SDL_QUIT)
				running = game_event(&rt, v, &ev);
		if (rt.warp_mouse) { /* opcode 49 */
			rt.warp_mouse = 0;
			video_warp_mouse(v, rt.mouse_x + w / 2, rt.mouse_y + h / 2);
			/* SetCursorPos generates a WM_MOUSEMOVE: the game cursor follows even without the SDL
			   event (which is discarded with --input) */
			rt_mouse_move(&rt, rt.mouse_x, rt.mouse_y);
		}
		/* PumpMessages: one tick per ms (see sprite_ticks); no timers, queue or animations
		   while paused or out of focus */
		/* a synchronous fade inside rt_tick advances the clock and the ticks (sprite_ticks): we
		   restart from there, time never goes backwards */
		for (uint32_t t = eng_first(now); (int32_t)(t - now) <= 0 && !rt.quit; t = eng_last + 1) {
			if (rt_running(&rt)) {
				rt_tick(&rt, t);
				game_sync_library(&rt);
				if (rt_running(&rt) && (int32_t)(eng_last - t) < 0)
					spr_update(wp, t);
			}
			if ((int32_t)(eng_last - t) < 0)
				eng_last = t;
		}
		now = clk_now();
		if ((int32_t)(eng_last - now) < 0)
			eng_last = now;
		rt.now_ms = now;
		if (rt.text_flush) { /* opcode 36: characters already queued do not go into the new field */
			SDL_FlushEvent(SDL_TEXTINPUT);
			rt.text_flush = 0;
		}
		video_cursor_mode(v, rt.cursor_mode);
		game_sync_library(&rt);
		video_clear(v, 0);
		spr_draw(wp, v);
		if (input && !play_input(input, now - start, &rt, v))
			running = 0;
		video_set_fade(v, rt.fade_level);
		video_present(v);
		if (clk_step)
			clk_advance(clk_step);
		else
			SDL_Delay(10);
	}
	rt_report(&rt, stdout);
	printf("animation commands not yet handled:");
	for (int k = 0; k < 0x20; k++)
		if (wp->unknown_cmd[k])
			printf(" %02X x%u", k, wp->unknown_cmd[k]);
	printf("\n");
	write_coverage();
	spr_clear(wp);
	rt_shutdown(&rt);
	audio_shutdown();
	video_destroy(v);
	if (input)
		fclose(input);
	if (trace)
		fclose(trace);
	SDL_Quit();
	return 0;
}

static void fatal(const char *msg)
{
	fprintf(stderr, GAME_NAME ": %s\n", msg);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GAME_TITLE, msg, NULL);
	exit(1);
}

/* Looks for the archive: --data, current folder, the executable's folder and the one above. */
static int open_lib(Lb7 *lb, const char *datadir, const char *name, char *err, size_t errlen)
{
	char path[1024];
	char *base = SDL_GetBasePath();
	const char *dirs[3] = { datadir, ".", base };
	int ok = 0;
	for (int i = 0; i < 3 && !ok; i++) {
		if (!dirs[i])
			continue;
		snprintf(path, sizeof(path), "%s/%s", dirs[i], name);
		FILE *f = fopen(path, "rb");
		if (!f)
			continue;
		fclose(f);
		ok = lb7_open(lb, path, err, errlen);
	}
	if (!ok && !err[0])
		snprintf(err, errlen, "%s not found (use --data to point to the game folder)", name);
	SDL_free(base);
	return ok;
}

static void show_current(Viewer *vw, Video *v)
{
	char title[256];
	uint32_t idx = vw->bitmaps[vw->cur];
	Lb7Entry e = lb7_entry(&vw->lb, idx);
	const uint8_t *d = lb7_entry_data(&vw->lb, e);
	Lb7Bitmap b;
	video_clear(v, 0);
	if (!d || !lb7_bitmap_header(d, e.size, &b)) {
		snprintf(title, sizeof(title), GAME_TITLE " - %s #%u: invalid header", vw->libname, idx);
		video_set_title(v, title);
		return;
	}
	uint8_t *pix = malloc((size_t)b.stride * b.height);
	int ok = pix && lb7_bitmap_decode(d, e.size, &b, pix);
	if (ok) {
		/* centered: full-resolution backgrounds cover the screen */
		int x = (video_width(v) - b.width) / 2, y = (video_height(v) - b.height) / 2;
		video_blit(v, pix, &b, x, y, (b.flags & LB7_BMP_RLE) ? 0 : -1);
	}
	free(pix);
	snprintf(title, sizeof(title), GAME_TITLE " - %s #%u (%u/%u) %ux%u %s%s", vw->libname, idx,
		vw->cur + 1, vw->n_bitmaps, b.width, b.height, (b.flags & LB7_BMP_RLE) ? "RLE" : "LZW",
		ok ? "" : " - DECODING FAILED");
	video_set_title(v, title);
}

int main(int argc, char **argv)
{
	const char *datadir = NULL, *mixout = NULL, *sf2 = NULL, *tracefile = NULL, *inputfile = NULL;
	int run_seconds = -1, game = 1, lib_given = 0;
	long entry = -1, mixentry = -1;
	char err[512] = "";
	/* icon of the window and the taskbar = resource 1 of the executable
	   (port\res\game.rc, the icon of GAME_EXE_RES if present at build time) */
	SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
	SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
	config_load(&g_cfg); /* <game>.ini; the command line takes precedence */
	int w = g_cfg.width, h = g_cfg.height, scale = g_cfg.scale ? g_cfg.scale : 8; /* 0 = massima */
	Viewer vw = { .libname = DEFAULT_LIB };
	if (g_cfg.data[0])
		datadir = g_cfg.data;
	if (g_cfg.sf2[0])
		sf2 = g_cfg.sf2;
	if (g_cfg.trace)
		tracefile = GAME_NAME "_trace.txt";
	if (g_cfg.lib[0]) {
		vw.libname = g_cfg.lib;
		lib_given = 1;
	}

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--data") && i + 1 < argc)
			datadir = argv[++i];
		else if (!strcmp(argv[i], "--lib") && i + 1 < argc) {
			vw.libname = argv[++i];
			lib_given = 1;
		}
		else if (!strcmp(argv[i], "--entry") && i + 1 < argc) {
			entry = atol(argv[++i]);
			game = 0;
		}
		else if (!strcmp(argv[i], "--viewer"))
			game = 0; /* archive viewer (no options = game) */
		else if (!strcmp(argv[i], "--fullscreen"))
			g_cfg.fullscreen = 1;
		else if (!strcmp(argv[i], "--640")) {
			w = 640;
			h = 480;
		}
		else if (!strcmp(argv[i], "--scale") && i + 1 < argc)
			scale = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--800")) {
			w = 800;
			h = 600;
		}
		else if (!strcmp(argv[i], "--game"))
			game = 1;
		else if (!strcmp(argv[i], "--run") && i + 1 < argc)
			run_seconds = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--trace") && i + 1 < argc)
			tracefile = argv[++i];
		else if (!strcmp(argv[i], "--input") && i + 1 < argc)
			inputfile = argv[++i];
		else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
			opt_seed = strtol(argv[++i], NULL, 0) & 0xFFFF;
		else if (!strcmp(argv[i], "--fixed-step")) { /* optional MS, 10 like the real tick */
			clk_step = 10;
			if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
				clk_step = (unsigned)atoi(argv[++i]);
			if (!clk_step)
				clk_step = 1;
		}
		else if (!strcmp(argv[i], "--coverage") && i + 1 < argc) {
			opt_cov = argv[++i];
			cov_on = 1;
		}
		else if (!strcmp(argv[i], "--ini") && i + 1 < argc)
			opt_ini = argv[++i];
		else if (!strcmp(argv[i], "--fast"))
			clk_fast = 1;
		else if (!strcmp(argv[i], "--proberadius") && i + 1 < argc)
			spr_probe_r = atoi(argv[++i]); /* diagnostics: radius of the --probe area */
		else if (!strcmp(argv[i], "--probe") && i + 2 < argc) { /* diagnostics: sprites under point X Y */
			spr_probe_x = atoi(argv[++i]) - w / 2;
			spr_probe_y = atoi(argv[++i]) - h / 2;
			spr_probe_on = 1;
		}
		else if (!strcmp(argv[i], "--joysticks") && i + 1 < argc)
			fake_joys = atoi(argv[++i]); /* tests: joysticks declared to the game even if absent */
		else if (!strcmp(argv[i], "--sf2") && i + 1 < argc)
			sf2 = argv[++i];
		else if (!strcmp(argv[i], "--mixdown") && i + 2 < argc) {
			mixentry = atol(argv[++i]);
			mixout = argv[++i];
			game = 0;
		}
	}
	if (clk_step)
		g_cfg.vsync = 0; /* the pace comes from the simulated clock, not the screen */
	video_set_options(g_cfg.linear, g_cfg.integer_scale, g_cfg.vsync);

	if (run_seconds >= 0)
		return run_game(datadir, run_seconds, tracefile);
	if (game)
		/* the CD starts with GAME_EXE -> GAME_START (Topsy Turvy: sets the variant G(5068), the
		   menu shows only this game, then link to TOPSY.BIN) */
		return game_window(datadir, lib_given ? vw.libname : GAME_START, w, h, scale, tracefile, inputfile, sf2);

	if (mixout) {
		/* audio test: no window or device, only the mixer */
		if (!open_lib(&vw.lb, datadir, vw.libname, err, sizeof(err))) {
			fprintf(stderr, GAME_NAME ": %s\n", err);
			return 1;
		}
		if (!load_soundfont(sf2))
			fprintf(stderr, GAME_NAME ": SoundFont not found, MIDI music muted\n");
		int ok = mixdown(&vw.lb, (uint32_t)mixentry, mixout);
		fprintf(ok ? stdout : stderr, "mixdown entry %ld -> %s: %s\n", mixentry, mixout,
			ok ? "ok" : "error (not a wave?)");
		lb7_close(&vw.lb);
		return ok ? 0 : 1;
	}

	if (SDL_Init(SDL_INIT_VIDEO) != 0)
		fatal(SDL_GetError());
	if (!audio_init())
		fprintf(stderr, GAME_NAME ": audio unavailable (%s), continuing without sound\n", SDL_GetError());
	if (!load_soundfont(sf2))
		fprintf(stderr, GAME_NAME ": SoundFont %s not found, MIDI music muted (use --sf2)\n", SF2_NAME);
	if (!open_lib(&vw.lb, datadir, vw.libname, err, sizeof(err)))
		fatal(err);

	vw.bitmaps = malloc(sizeof(uint32_t) * (vw.lb.n_entries ? vw.lb.n_entries : 1));
	for (uint32_t i = 0; i < vw.lb.n_entries; i++)
		if (lb7_entry(&vw.lb, i).type == LB7_T_BITMAP)
			vw.bitmaps[vw.n_bitmaps++] = i;
	if (!vw.n_bitmaps)
		fatal("the archive contains no bitmaps");

	/* initial bitmap: the requested one, otherwise the first background as big as the screen */
	for (uint32_t k = 0; k < vw.n_bitmaps; k++) {
		uint32_t idx = vw.bitmaps[k];
		if (entry >= 0) {
			if ((long)idx >= entry) {
				vw.cur = k;
				break;
			}
			continue;
		}
		Lb7Entry e = lb7_entry(&vw.lb, idx);
		const uint8_t *d = lb7_entry_data(&vw.lb, e);
		Lb7Bitmap b;
		if (d && lb7_bitmap_header(d, e.size, &b) && b.width == w && b.height == h) {
			vw.cur = k;
			break;
		}
	}

	Video *v = video_create(GAME_TITLE, w, h, scale);
	if (!v)
		fatal(SDL_GetError());
	uint8_t rgb[256][3];
	lb7_palette(&vw.lb, rgb);
	video_set_palette(v, rgb);
	show_current(&vw, v);

	for (int running = 1; running;) {
		SDL_Event ev;
		int changed = 0;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_QUIT)
				running = 0;
			else if (ev.type == SDL_KEYDOWN) {
				uint32_t n = vw.n_bitmaps;
				switch (ev.key.keysym.sym) {
				case SDLK_ESCAPE: running = 0; break;
				case SDLK_RIGHT: vw.cur = (vw.cur + 1) % n; changed = 1; break;
				case SDLK_LEFT: vw.cur = (vw.cur + n - 1) % n; changed = 1; break;
				case SDLK_PAGEDOWN: vw.cur = (vw.cur + 50) % n; changed = 1; break;
				case SDLK_PAGEUP: vw.cur = (vw.cur + n - 50 % n) % n; changed = 1; break;
				case SDLK_F11: video_toggle_fullscreen(v); break;
				case SDLK_w: {
					long wi = play_next_wave(&vw);
					char t[128];
					snprintf(t, sizeof(t), GAME_TITLE " - %s wave #%ld", vw.libname, wi);
					video_set_title(v, t);
					break;
				}
				case SDLK_m: {
					long mi = play_next_midi(&vw);
					char t[128];
					snprintf(t, sizeof(t), GAME_TITLE " - %s midi #%ld%s", vw.libname, mi,
						mi < 0 ? " (no MIDI, or SoundFont missing)" : "");
					video_set_title(v, t);
					break;
				}
				case SDLK_s: audio_stop_all(); audio_stop_midi(); break;
				default: break;
				}
			}
		}
		if (changed)
			show_current(&vw, v);
		video_present(v);
		SDL_Delay(16);
	}

	audio_shutdown();
	video_destroy(v);
	free(vw.bitmaps);
	lb7_close(&vw.lb);
	SDL_Quit();
	return 0;
}
