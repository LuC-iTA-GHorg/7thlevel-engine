#include "text.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../third_party/stb_truetype.h"

typedef struct {
	unsigned char *data;
	stbtt_fontinfo info;
	int ok;
} Font;

static Font fonts[2]; /* [0] serif (Times New Roman), [1] sans (Arial) */

/* Fonts supplied by the game in its folder (e.g. HYENA.TTF of the Jungle games, requested as
 * "Hyena" by the type 18 resources): the installer registered them in Windows, the port looks
 * them up by family name (name table, nameID 1) before the system fonts. */
#define MAX_GAME_FONTS 8
static Font game_fonts[MAX_GAME_FONTS];
static char game_names[MAX_GAME_FONTS][64];
static int n_game_fonts;

static unsigned char *load_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *d = n > 0 ? malloc((size_t)n) : NULL;
	if (d && fread(d, 1, (size_t)n, f) != (size_t)n) {
		free(d);
		d = NULL;
	}
	fclose(f);
	return d;
}

static void load_font(Font *f, const char *const *paths, int n)
{
	for (int i = 0; i < n && !f->ok; i++) {
		if (!paths[i])
			continue;
		f->data = load_file(paths[i]);
		if (f->data && stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0)))
			f->ok = 1;
		else {
			free(f->data);
			f->data = NULL;
		}
	}
}

int text_init(const char *basedir)
{
	const char *win = getenv("WINDIR");
	char p[6][1024];
	const char *names[2][2] = { { "times.ttf", "LiberationSerif-Regular.ttf" }, { "arial.ttf", "LiberationSans-Regular.ttf" } };
	for (int k = 0; k < 2; k++) {
		const char *c[6] = { NULL };
		if (win) {
			snprintf(p[0], sizeof(p[0]), "%s/Fonts/%s", win, names[k][0]);
			c[0] = p[0];
		}
		if (basedir) {
			snprintf(p[1], sizeof(p[1]), "%s../tools/fonts/%s", basedir, names[k][1]);
			snprintf(p[2], sizeof(p[2]), "%s%s", basedir, names[k][1]);
			c[1] = p[1];
			c[2] = p[2];
		}
		snprintf(p[3], sizeof(p[3]), "tools/fonts/%s", names[k][1]);
		c[3] = p[3];
		load_font(&fonts[k], c, 4);
	}
	return fonts[0].ok || fonts[1].ok;
}

/* Family name (nameID 1) in lowercase ASCII; first the English Microsoft Unicode entry
 * (UTF-16BE), then the Macintosh Roman one. 0 if missing. */
static int family_name(const stbtt_fontinfo *info, char *out, size_t cap)
{
	int len = 0;
	const char *s = stbtt_GetFontNameString(info, &len, STBTT_PLATFORM_ID_MICROSOFT,
		STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH, 1);
	int step = 2, lo = 1;
	if (!s) {
		s = stbtt_GetFontNameString(info, &len, STBTT_PLATFORM_ID_MAC, STBTT_MAC_EID_ROMAN,
			STBTT_MAC_LANG_ENGLISH, 1);
		step = 1;
		lo = 0;
	}
	if (!s || cap == 0)
		return 0;
	size_t k = 0;
	for (int i = 0; i + lo < len && k + 1 < cap; i += step)
		out[k++] = (char)tolower((unsigned char)s[i + lo]);
	out[k] = 0;
	return k > 0;
}

int text_add_dir(const char *dir)
{
	DIR *d = dir ? opendir(dir) : NULL;
	if (!d)
		return 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL && n_game_fonts < MAX_GAME_FONTS) {
		size_t n = strlen(e->d_name);
		if (n < 5 || (strcasecmp(e->d_name + n - 4, ".ttf") && strcasecmp(e->d_name + n - 4, ".otf")))
			continue;
		char path[1300];
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		const char *one[1] = { path };
		Font *f = &game_fonts[n_game_fonts];
		load_font(f, one, 1);
		if (!f->ok)
			continue;
		if (!family_name(&f->info, game_names[n_game_fonts], sizeof(game_names[0]))) {
			/* no name table: the file name without extension (HYENA.TTF -> hyena) */
			size_t k = 0;
			for (; k + 4 < n && k + 1 < sizeof(game_names[0]); k++)
				game_names[n_game_fonts][k] = (char)tolower((unsigned char)e->d_name[k]);
			game_names[n_game_fonts][k] = 0;
		}
		n_game_fonts++;
	}
	closedir(d);
	return n_game_fonts;
}

void text_shutdown(void)
{
	for (int k = 0; k < 2; k++) {
		free(fonts[k].data);
		memset(&fonts[k], 0, sizeof(fonts[k]));
	}
	for (int k = 0; k < n_game_fonts; k++) {
		free(game_fonts[k].data);
		memset(&game_fonts[k], 0, sizeof(game_fonts[k]));
	}
	n_game_fonts = 0;
}

static Font *pick(const char *face)
{
	char low[64];
	size_t i = 0;
	for (; face && face[i] && i + 1 < sizeof(low); i++)
		low[i] = (char)tolower((unsigned char)face[i]);
	low[i] = 0;
	for (int k = 0; k < n_game_fonts; k++)
		if (!strcmp(low, game_names[k]))
			return &game_fonts[k];
	Font *f = strstr(low, "arial") || strstr(low, "sans") || strstr(low, "helv") ? &fonts[1] : &fonts[0];
	return f->ok ? f : (fonts[0].ok ? &fonts[0] : &fonts[1]);
}

/* Integer advance per character, like GDI character widths (no kerning). */
static int advance(Font *f, float scale, int ch)
{
	int adv, lsb;
	stbtt_GetCodepointHMetrics(&f->info, ch, &adv, &lsb);
	return (int)(adv * scale + 0.5f);
}

static int width_of(Font *f, float scale, const char *s, int n)
{
	int w = 0;
	for (int i = 0; i < n; i++)
		w += advance(f, scale, (unsigned char)s[i]);
	return w;
}

#define MAX_LINES 32

/* Lines like DrawText: '\n' breaks the line; with wordbreak, words that do not fit wrap. */
static int split_lines(Font *f, float scale, const char *s, int box_w, int wordbreak, int start[], int len[])
{
	int n = 0, i = 0, total = (int)strlen(s);
	while (i <= total && n < MAX_LINES) {
		int e = i;
		while (e < total && s[e] != '\n' && s[e] != '\r')
			e++;
		int a = i;
		while (wordbreak && box_w > 0 && width_of(f, scale, s + a, e - a) > box_w && n < MAX_LINES - 1) {
			int cut = -1; /* last space within the width */
			for (int k = a; k < e; k++)
				if (s[k] == ' ' && width_of(f, scale, s + a, k - a) <= box_w)
					cut = k;
			if (cut < 0)
				break;
			start[n] = a;
			len[n++] = cut - a;
			a = cut + 1;
		}
		start[n] = a;
		len[n++] = e - a;
		if (e < total && s[e] == '\r' && e + 1 < total && s[e + 1] == '\n')
			e++;
		if (e >= total)
			break;
		i = e + 1;
	}
	return n;
}

int text_render(const char *face, int height, const char *str, int box_w, int box_h, unsigned flags,
	uint8_t color, uint8_t **pix, int *w, int *h)
{
	Font *f = pick(face);
	*pix = NULL;
	if (!f->ok || height <= 0)
		return 0;
	if (!str || !*str)
		str = " "; /* the original's DS:01F2 */
	float scale = stbtt_ScaleForPixelHeight(&f->info, (float)height); /* lfHeight > 0: cell */
	int asc, desc, gap;
	stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
	int line_h = (int)((asc - desc) * scale + 0.5f), base = (int)(asc * scale + 0.5f);
	int boxed = box_w > 0 && box_h > 0;
	int wordbreak = boxed || (flags & TEXT_DT_WORDBREAK);
	int st[MAX_LINES], ln[MAX_LINES];
	int n = split_lines(f, scale, str, box_w, wordbreak, st, ln), tw = 0;
	for (int k = 0; k < n; k++) {
		int lw = width_of(f, scale, str + st[k], ln[k]);
		if (lw > tw)
			tw = lw;
	}
	/* DT_CALCRECT: multiple lines = box width; then box limits, +1 (1010:1984) */
	int cw = boxed && n > 1 ? box_w : tw, ch = n * line_h;
	if (boxed) {
		if (cw > box_w)
			cw = box_w;
		if (ch > box_h)
			ch = box_h;
	}
	int W = cw + 1, H = ch + 1;
	uint8_t *out = calloc((size_t)W * H, 1);
	if (!out)
		return 0;
	/* DT_VCENTER / DT_BOTTOM only apply to a single line (DT_SINGLELINE is not used): top */
	for (int k = 0; k < n; k++) {
		int lw = width_of(f, scale, str + st[k], ln[k]);
		int x = flags & TEXT_DT_RIGHT ? W - lw : flags & TEXT_DT_CENTER ? (W - lw) / 2 : 0;
		int y = k * line_h + base;
		for (int i = 0; i < ln[k]; i++) {
			int c = (unsigned char)str[st[k] + i], x0, y0, x1, y1;
			stbtt_GetCodepointBitmapBox(&f->info, c, scale, scale, &x0, &y0, &x1, &y1);
			int gw = x1 - x0, gh = y1 - y0;
			if (gw > 0 && gh > 0) {
				unsigned char *g = malloc((size_t)gw * gh);
				if (g) {
					stbtt_MakeCodepointBitmap(&f->info, g, gw, gh, gw, scale, scale, c);
					for (int yy = 0; yy < gh; yy++)
						for (int xx = 0; xx < gw; xx++) {
							int px = x + x0 + xx, py = y + y0 + yy;
							if (g[yy * gw + xx] >= 128 && px >= 0 && py >= 0 && px < W && py < H)
								out[(size_t)py * W + px] = color; /* no antialiasing, like GDI */
						}
					free(g);
				}
			}
			x += advance(f, scale, c);
		}
	}
	*pix = out;
	*w = W;
	*h = H;
	return 1;
}
