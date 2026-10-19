/*
 * video - 8-bit indexed screen with palette, presented with SDL2.
 * Replaces GDI/WinG/DirectDraw of TOPSYS01.DLL: the engine always draws into the framebuffer
 * (as on the original DIBs), the conversion to RGB happens only at presentation.
 */
#ifndef VIDEO_H
#define VIDEO_H

#include <stdint.h>

#include "lb7.h"

typedef struct Video Video;

/* Presentation options, to call before video_create: linear = smooth scaling
 * (otherwise sharp pixels), integer_scale = integer multiples only with black bars, vsync. */
void video_set_options(int linear, int integer_scale, int vsync);

/* Creates the window with logical resolution w x h (640x480 or 800x600), window = scale times that. */
Video *video_create(const char *title, int w, int h, int scale);
void video_destroy(Video *v);

int video_width(const Video *v);
int video_height(const Video *v);
uint8_t *video_pixels(Video *v); /* top-down, pitch = width */

void video_set_palette(Video *v, const uint8_t rgb[256][3]);
void video_clear(Video *v, uint8_t index);

/* Draws an already decoded 7LB bitmap (bottom-up) with its top-left corner at (x, y).
 * transparent >= 0: index not to draw (RLE sprites). Clipping is automatic. */
void video_blit(Video *v, const uint8_t *pix, const Lb7Bitmap *b, int x, int y, int transparent);
/* Like video_blit, mirrored horizontally and/or vertically (sprites with command 07). */
void video_blit_flip(Video *v, const uint8_t *pix, const Lb7Bitmap *b, int x, int y, int transparent,
	int flip_x, int flip_y);

void video_set_title(Video *v, const char *title);
void video_toggle_fullscreen(Video *v);
void video_present(Video *v);
/* Palette fade: 0 = black, 256 = full colors; acts on indices 10..245 at presentation. */
void video_set_fade(Video *v, int level);
/* Moves the cursor to point (x, y) of the logical screen (the original's SetCursorPos). */
void video_warp_mouse(Video *v, int x, int y);
/* Saves the last framebuffer (8 bit + palette) as a BMP: automated checks. */
int video_save_bmp(Video *v, const char *path);
/* 0xRRGGBB color of pixel x,y as on screen (fade included); -1 outside the screen */
long video_pixel(const Video *v, int x, int y);

/* The game's Windows cursors (opcode 1F): loads resources 0x3E9..0x3EF from TOPSY.EXE (number
 * loaded, 0 if the EXE is missing) and applies the mode: 1 arrow, 4 no cursor, 2/3/5..9 cursors
 * from the EXE (arrow if missing), 0 no change. */
int video_load_cursors(Video *v, const char *exe_path);
void video_cursor_mode(Video *v, int mode);

#endif
