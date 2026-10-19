/*
 * text - sprite text (type 18 resources), in place of TOPSY.EXE's CreateFontIndirect + DrawText
 * (1010:175A, 1010:1984). TrueType fonts rasterized with stb_truetype, without
 * antialiasing like GDI at 256 colors: pixels are either the text color or 0 (transparent).
 * Fonts: the Windows ones if present (times.ttf, arial.ttf), otherwise Liberation in
 * tools\fonts (identical metrics). See tools\fonts\README.md.
 */
#ifndef TEXT_H
#define TEXT_H

#include <stdint.h>

/* DrawText flags used by the game (type 18 resource, +0x10). */
#define TEXT_DT_CENTER    0x01
#define TEXT_DT_RIGHT     0x02
#define TEXT_DT_VCENTER   0x04
#define TEXT_DT_BOTTOM    0x08
#define TEXT_DT_WORDBREAK 0x10

/* Looks for the fonts; basedir = executable's folder (for tools\fonts), may be NULL. */
int text_init(const char *basedir);
/* Adds the .TTF/.OTF fonts of the game folder (e.g. HYENA.TTF), looked up by family name
 * before the system fonts. Returns how many game fonts are loaded. */
int text_add_dir(const char *dir);
void text_shutdown(void);

/* Draws str with font face ("Times New Roman", "Arial"...) height pixels tall (positive
 * lfHeight = cell height) into an allocated top-down 8-bit bitmap (*pix, w x h):
 * dimensions like DrawText with DT_CALCRECT, limited to the box_w x box_h box if non-zero
 * (with automatic word wrap), plus 1 pixel like the original. 0 = error (font missing). */
int text_render(const char *face, int height, const char *str, int box_w, int box_h, unsigned flags,
	uint8_t color, uint8_t **pix, int *w, int *h);

#endif
