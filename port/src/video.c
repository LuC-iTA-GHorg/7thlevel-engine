#include "video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

struct Video {
	SDL_Window *win;
	SDL_Renderer *ren;
	SDL_Texture *tex;
	int w, h;
	uint8_t *fb;          /* w*h indices */
	uint32_t pal[256];    /* ARGB8888 */
	uint32_t *rgb;        /* w*h, conversion buffer */
	int fullscreen;
	int fade;             /* 0..256: brightness of the game colors (indices 10..245) */
	int ox, oy;           /* image offset at presentation (builtin 78, window shake) */
	SDL_Cursor *cursors[10]; /* modes 2, 3, 5..9 of opcode 1F: resources 0x3E9..0x3EF of TOPSY.EXE */
	int cursor_mode;
};

static int opt_linear, opt_integer = 1, opt_stretch, opt_vsync = 1;

void video_set_options(int linear, int integer_scale, int stretch, int vsync)
{
	opt_linear = linear;
	opt_integer = integer_scale && !stretch;
	opt_stretch = stretch;
	opt_vsync = vsync;
}

Video *video_create(const char *title, int w, int h, int scale)
{
	Video *v = calloc(1, sizeof(*v));
	if (!v)
		return NULL;
	v->w = w;
	v->h = h;
	v->fb = calloc((size_t)w * h, 1);
	v->rgb = calloc((size_t)w * h, sizeof(uint32_t));
	if (!v->fb || !v->rgb)
		goto fail;
	/* reduces the scale until the window fits in the usable area of the primary monitor */
	SDL_Rect usable;
	if (SDL_GetDisplayUsableBounds(0, &usable) == 0)
		while (scale > 1 && (w * scale > usable.w || h * scale + 40 > usable.h))
			scale--;
	if (scale < 1)
		scale = 1;
	v->win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		w * scale, h * scale, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	if (!v->win)
		goto fail;
	v->ren = SDL_CreateRenderer(v->win, -1, SDL_RENDERER_ACCELERATED | (opt_vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
	if (!v->ren)
		v->ren = SDL_CreateRenderer(v->win, -1, SDL_RENDERER_SOFTWARE);
	if (!v->ren)
		goto fail;
	/* pixel art: normally integer scale with black bars and no filter (<game>.ini changes it) */
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, opt_linear ? "linear" : "nearest");
	/* stretch: no logical size, video_present draws over the whole output and
	   video_mouse_to_game / video_warp_mouse convert the coordinates */
	if (!opt_stretch) {
		SDL_RenderSetLogicalSize(v->ren, w, h);
		SDL_RenderSetIntegerScale(v->ren, opt_integer ? SDL_TRUE : SDL_FALSE);
	}
	v->tex = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
	if (!v->tex)
		goto fail;
	for (int i = 0; i < 256; i++)
		v->pal[i] = 0xFF000000u | (uint32_t)(i * 0x010101);
	v->fade = 256;
	return v;
fail:
	video_destroy(v);
	return NULL;
}

void video_destroy(Video *v)
{
	if (!v)
		return;
	for (int i = 0; i < 10; i++)
		if (v->cursors[i])
			SDL_FreeCursor(v->cursors[i]);
	if (v->tex)
		SDL_DestroyTexture(v->tex);
	if (v->ren)
		SDL_DestroyRenderer(v->ren);
	if (v->win)
		SDL_DestroyWindow(v->win);
	free(v->fb);
	free(v->rgb);
	free(v);
}

int video_width(const Video *v) { return v->w; }
int video_height(const Video *v) { return v->h; }
uint8_t *video_pixels(Video *v) { return v->fb; }

void video_set_palette(Video *v, const uint8_t rgb[256][3])
{
	for (int i = 0; i < 256; i++)
		v->pal[i] = 0xFF000000u | (uint32_t)rgb[i][0] << 16 | (uint32_t)rgb[i][1] << 8 | rgb[i][2];
}

void video_clear(Video *v, uint8_t index)
{
	memset(v->fb, index, (size_t)v->w * v->h);
}

void video_blit(Video *v, const uint8_t *pix, const Lb7Bitmap *b, int x, int y, int transparent)
{
	int x0 = x < 0 ? -x : 0, x1 = b->width;
	if (x + x1 > v->w)
		x1 = v->w - x;
	if (x0 >= x1)
		return;
	for (int row = 0; row < b->height; row++) {
		int sy = y + row;
		if (sy < 0 || sy >= v->h)
			continue;
		/* row 0 of the bitmap is the bottom one */
		const uint8_t *src = pix + (size_t)(b->height - 1 - row) * b->stride;
		uint8_t *dst = v->fb + (size_t)sy * v->w + x;
		if (transparent < 0) {
			memcpy(dst + x0, src + x0, (size_t)(x1 - x0));
			continue;
		}
		for (int i = x0; i < x1; i++)
			if (src[i] != transparent)
				dst[i] = src[i];
	}
}

void video_blit_flip(Video *v, const uint8_t *pix, const Lb7Bitmap *b, int x, int y, int transparent,
	int flip_x, int flip_y)
{
	if (!flip_x && !flip_y) {
		video_blit(v, pix, b, x, y, transparent);
		return;
	}
	for (int row = 0; row < b->height; row++) {
		int sy = y + row;
		if (sy < 0 || sy >= v->h)
			continue;
		int srow = flip_y ? row : b->height - 1 - row; /* row 0 of the bitmap is at the bottom */
		const uint8_t *src = pix + (size_t)srow * b->stride;
		uint8_t *dst = v->fb + (size_t)sy * v->w;
		for (int i = 0; i < b->width; i++) {
			int dx = x + i;
			if (dx < 0 || dx >= v->w)
				continue;
			uint8_t c = src[flip_x ? b->width - 1 - i : i];
			if (transparent < 0 || c != transparent)
				dst[dx] = c;
		}
	}
}

void video_set_title(Video *v, const char *title)
{
	SDL_SetWindowTitle(v->win, title);
}

void video_toggle_fullscreen(Video *v)
{
	v->fullscreen = !v->fullscreen;
	SDL_SetWindowFullscreen(v->win, v->fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

void video_present(Video *v)
{
	size_t n = (size_t)v->w * v->h;
	uint32_t pal[256];
	for (int i = 0; i < 256; i++) { /* palette fade (S_021): game colors only */
		uint32_t c = v->pal[i];
		if (v->fade < 256 && i >= 10 && i < 246)
			c = 0xFF000000u | ((c >> 16 & 0xFF) * (uint32_t)v->fade >> 8) << 16
				| ((c >> 8 & 0xFF) * (uint32_t)v->fade >> 8) << 8 | ((c & 0xFF) * (uint32_t)v->fade >> 8);
		pal[i] = c;
	}
	for (size_t i = 0; i < n; i++)
		v->rgb[i] = pal[v->fb[i]];
	SDL_UpdateTexture(v->tex, NULL, v->rgb, v->w * (int)sizeof(uint32_t));
	/* integer scale only if the window holds at least one whole image: smaller (window
	   resized with the mouse below 800x600) SDL would draw with scale 0, i.e. all black */
	if (opt_integer) {
		int ow, oh;
		SDL_bool fit = SDL_GetRendererOutputSize(v->ren, &ow, &oh) == 0 && ow >= v->w && oh >= v->h;
		if (fit != SDL_RenderGetIntegerScale(v->ren))
			SDL_RenderSetIntegerScale(v->ren, fit);
	}
	SDL_SetRenderDrawColor(v->ren, 0, 0, 0, 255);
	SDL_RenderClear(v->ren);
	SDL_Rect dst = { v->ox, v->oy, v->w, v->h };
	int ow, oh;
	if (opt_stretch && SDL_GetRendererOutputSize(v->ren, &ow, &oh) == 0)
		dst = (SDL_Rect){ v->ox * ow / v->w, v->oy * oh / v->h, ow, oh };
	SDL_RenderCopy(v->ren, v->tex, NULL, &dst);
	SDL_RenderPresent(v->ren);
}

void video_set_offset(Video *v, int dx, int dy)
{
	v->ox = dx;
	v->oy = dy;
}

void video_warp_mouse(Video *v, int x, int y)
{
	int wx, wy;
	if (opt_stretch) {
		SDL_GetWindowSize(v->win, &wx, &wy);
		wx = x * wx / v->w;
		wy = y * wy / v->h;
	}
	else
		SDL_RenderLogicalToWindow(v->ren, (float)x, (float)y, &wx, &wy);
	SDL_WarpMouseInWindow(v->win, wx, wy);
}

void video_mouse_to_game(const Video *v, int *x, int *y)
{
	int ww, wh;
	if (!opt_stretch)
		return;
	SDL_GetWindowSize(v->win, &ww, &wh);
	if (ww > 0 && wh > 0) {
		*x = *x * v->w / ww;
		*y = *y * v->h / wh;
	}
}

int video_save_bmp(Video *v, const char *path)
{
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, v->w, v->h, 8, SDL_PIXELFORMAT_INDEX8);
	if (!s)
		return 0;
	SDL_Color c[256];
	for (int i = 0; i < 256; i++) { /* as on screen, fade included */
		uint32_t f = i >= 10 && i < 246 ? (uint32_t)v->fade : 256;
		c[i].r = (uint8_t)((v->pal[i] >> 16 & 0xFF) * f >> 8);
		c[i].g = (uint8_t)((v->pal[i] >> 8 & 0xFF) * f >> 8);
		c[i].b = (uint8_t)((v->pal[i] & 0xFF) * f >> 8);
		c[i].a = 255;
	}
	SDL_SetPaletteColors(s->format->palette, c, 0, 256);
	for (int y = 0; y < v->h; y++)
		memcpy((uint8_t *)s->pixels + (size_t)y * s->pitch, v->fb + (size_t)y * v->w, (size_t)v->w);
	int ok = SDL_SaveBMP(s, path) == 0;
	SDL_FreeSurface(s);
	return ok;
}

long video_pixel(const Video *v, int x, int y)
{
	if (x < 0 || y < 0 || x >= v->w || y >= v->h)
		return -1;
	unsigned i = v->fb[(size_t)y * v->w + x];
	uint32_t c = v->pal[i], f = i >= 10 && i < 246 ? (uint32_t)v->fade : 256;
	return (long)(((c >> 16 & 0xFF) * f >> 8) << 16 | ((c >> 8 & 0xFF) * f >> 8) << 8 | ((c & 0xFF) * f >> 8));
}

void video_set_fade(Video *v, int level)
{
	v->fade = level < 0 ? 0 : level > 256 ? 256 : level;
}

/* ---------------------------------------------------------------- Windows cursors */

static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* Monochrome 32x32 Windows 3.x cursor (RT_CURSOR resource: hotspot, BITMAPINFOHEADER,
 * 2-color palette, XOR mask and AND mask bottom-up) -> SDL cursor. */
static SDL_Cursor *cursor_from_res(const uint8_t *r, size_t n)
{
	if (n < 4 + 40 + 8 + 256)
		return NULL;
	int hx = rd16le(r), hy = rd16le(r + 2);
	const uint8_t *bih = r + 4, *pal = bih + 40, *xr = pal + 8, *an = xr + 128;
	if (rd16le(bih) != 40 || rd16le(bih + 4) != 32 || rd16le(bih + 14) != 1)
		return NULL;
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 32, 32, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!s)
		return NULL;
	for (int y = 0; y < 32; y++) {
		uint32_t *row = (uint32_t *)((uint8_t *)s->pixels + (size_t)y * s->pitch);
		int sy = 31 - y;
		for (int x = 0; x < 32; x++) {
			int xb = xr[sy * 4 + x / 8] >> (7 - x % 8) & 1, ab = an[sy * 4 + x / 8] >> (7 - x % 8) & 1;
			const uint8_t *c = pal + xb * 4; /* RGBQUAD: blue, green, red */
			if (!ab)
				row[x] = 0xFF000000u | (uint32_t)c[2] << 16 | (uint32_t)c[1] << 8 | c[0];
			else if (xb) /* inverted screen color: black (SDL cannot do it) */
				row[x] = 0xFF000000u;
			else
				row[x] = 0;
		}
	}
	SDL_Cursor *cur = SDL_CreateColorCursor(s, hx, hy);
	SDL_FreeSurface(s);
	return cur;
}

int video_load_cursors(Video *v, const char *exe_path)
{
	FILE *f = fopen(exe_path, "rb");
	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *d = size > 0 ? malloc((size_t)size) : NULL;
	if (!d || fread(d, 1, (size_t)size, f) != (size_t)size) {
		fclose(f);
		free(d);
		return 0;
	}
	fclose(f);
	int loaded = 0;
	if (size < 0x40 || d[0] != 'M' || d[1] != 'Z')
		goto done;
	uint32_t ne = rd16le(d + 0x3C);
	if (ne + 0x40 > (uint32_t)size || d[ne] != 'N' || d[ne + 1] != 'E')
		goto done;
	uint32_t rt = ne + rd16le(d + ne + 0x24), p = rt + 2;
	unsigned shift = rd16le(d + rt);
	/* groups (0x800C): id 0x3E9..0x3EF -> cursor number (0x8001) */
	unsigned group_cur[7] = { 0 };
	struct { uint32_t off, len; unsigned id; } curs[32];
	int ncurs = 0;
	while (p + 8 <= (uint32_t)size) {
		unsigned type = rd16le(d + p), cnt = rd16le(d + p + 2);
		if (!type)
			break;
		p += 8;
		for (unsigned i = 0; i < cnt && p + 12 <= (uint32_t)size; i++, p += 12) {
			uint32_t off = (uint32_t)rd16le(d + p) << shift, len = (uint32_t)rd16le(d + p + 2) << shift;
			unsigned id = rd16le(d + p + 6) & 0x7FFF;
			if (off + len > (uint32_t)size)
				continue;
			if (type == 0x800C && id >= 0x3E9 && id <= 0x3EF && len >= 20)
				group_cur[id - 0x3E9] = rd16le(d + off + 6 + 12);
			else if (type == 0x8001 && ncurs < 32) {
				curs[ncurs].off = off;
				curs[ncurs].len = len;
				curs[ncurs].id = id;
				ncurs++;
			}
		}
	}
	/* cursor mode -> resource (TOPSY 1010:27F6): 2 3E9, 3 3EA, 5 3EB, 6 3EC, 7 3ED, 8 3EE, 9 3EF */
	static const int mode_of[7] = { 2, 3, 5, 6, 7, 8, 9 };
	for (int g = 0; g < 7; g++)
		for (int k = 0; k < ncurs; k++)
			if (group_cur[g] && curs[k].id == group_cur[g] && !v->cursors[mode_of[g]]) {
				v->cursors[mode_of[g]] = cursor_from_res(d + curs[k].off, curs[k].len);
				loaded += v->cursors[mode_of[g]] != NULL;
			}
done:
	free(d);
	return loaded;
}

void video_cursor_mode(Video *v, int mode)
{
	if (mode == v->cursor_mode || mode <= 0 || mode > 9) /* 0: no change (1010:27F6) */
		return;
	v->cursor_mode = mode;
	if (mode == 4) { /* SetCursor(NULL): no Windows cursor, the game draws its own */
		SDL_ShowCursor(SDL_DISABLE);
		return;
	}
	SDL_SetCursor(mode != 1 && v->cursors[mode] ? v->cursors[mode] : SDL_GetDefaultCursor());
	SDL_ShowCursor(SDL_ENABLE);
}
