/*
 * sprite - sprite and animation engine (port of TOPSYS01.DLL, Sprite Library 1.2).
 * Formats in docs\SPRITES_ANIMATIONS.md. Sprites from type 17 resources, frames (types 1 and 12),
 * animation commands 01..17 (03 and 13 one-tick no-ops), layered drawing, hit
 * tests. Events (0x15) are game logic translated to C (rt_anim_event), calls (0x16)
 * go through rt_anim_call.
 */
#ifndef SPRITE_H
#define SPRITE_H

#include <stdint.h>

#include "lb7.h"
#include "video.h"

struct Rt;

#define SPR_MAX        256
#define SPR_MAX_LAYERS 16   /* overlaid cels in a composite frame (cmd 02) */

typedef struct {
	const uint8_t *data;    /* bitmap resource */
	uint32_t size;
	int cx, cy;             /* cel offset (type 12); 0 for direct bitmaps */
	int cel;                /* 1 = entry 12: cx/cy replace the bitmap's w5/w6 anchor */
	uint8_t *pix;           /* decoded pixels (cache) */
	Lb7Bitmap bmp;
	uint16_t text_id;       /* != 0: text member (type 18), rendered with text.c when drawing */
	uint16_t entry;         /* index entry of the member (diagnostics) */
	uint8_t lazy;           /* sprite byte +0x12: record {id, 0} of FUN_1008_6D44 (bitmap/cel not
	                           resident at load), resolved through the EXE when it is shown */
} SprFrame;

typedef struct {
	int used;
	uint16_t id;            /* index entry (type 17) */
	int x, y, layer, visible;
	int seq;                /* insertion order within the same layer (opcode 1C) */
	int break_loops;        /* opcode 19 = field +0x65: loops (cmd 06) do not jump back */
	int flip_x, flip_y;     /* command 07 = fields +0x3E/+0x3F: mirrored image */
	/* click (S_058 from TOPSY 1008:6C08): hit_state = byte +0x15 of the resource (DLL field +0x52:
	   2 = never hit, 1 = hit even if hidden); hit_box = byte +0x11 (field +0x69:
	   the rectangle is enough, no check of transparent pixels) */
	int hit_state, hit_box;
	int16_t user;           /* field +0x44 of the DLL record, free word of the scripts (builtin 83/84) */
	/* default sprite (byte +0x14, DAT_0E3E): is_def = it is the current one; orphan = kept from
	   a closed library (FUN_1008_65F2: no resource id any more, its frames own their pixels);
	   pinned = S_042: last slot of the draw list (on top of every layer), layer -1 */
	int is_def, orphan, pinned;
	/* lazy frames resolved (TOPSYS01 FUN_1000_6674): MRU cache of resource ids, most recent
	   first, at most 20 (sprite +0x30/+0x48); each one holds the resource once */
	uint16_t lz_id[20];
	int lz_n;
	SprFrame *frames;       /* malloc, one slot per member of the type 17 resource */
	int n_frames;
	int shown[SPR_MAX_LAYERS]; /* frames currently shown (indices), -1 = none */
	int n_shown;
	/* animation */
	uint16_t anim_id;
	uint8_t *anim;          /* editable copy of the commands (loop counters change) */
	uint32_t anim_size, pc;
	uint32_t next_ms;       /* time of the next step */
	int cumulative;         /* cmd 0D */
	int step_left;          /* cmd 01: frames still to show */
	int step_frame;
	int waiting;            /* cmd 0B in progress */
	int after_pos;          /* after 07/0C: the tick continues only with a 01/02 (+0x57) */
	/* cmd 11 (S_041/S_036): interval between movement and frame steps (+0x20/+0x24) */
	int move_ms, frame_ms;
	/* cmd 09 in progress (FUN_1000_404A/3FEE): from (sx, sy) to (tx, ty) in steps */
	int moving, mv_sx, mv_sy, mv_tx, mv_ty, mv_step, mv_steps;
	int mv_curve, mv_mx, mv_my; /* cmd 05: curved path through the intermediate point (mx, my) */
	int mv_op, mv_rel;      /* command of the movement (05, 09, 0A) and its relative flag (S_051) */
	int in_event;           /* +0x66 = 0 while the EXE callback of its event runs (FUN_1000_44DC) */
	/* cmd 0E (FUN_1000_6AA4, field +0x60 = 0): frozen image; position and frames
	   change without being seen until cmd 0E 1 (or a cmd 01/02/09) makes it visible again */
	int frozen, fz_x, fz_y, fz_flip_x, fz_flip_y, fz_n_shown;
	int fz_shown[SPR_MAX_LAYERS];
	/* cmd 10 (FUN_1000_6A1A): frame cycle from..to (+0x4E/+0x50, current +0x4C, direction
	   +0x5D, active +0x5F) that advances every frame_ms during movements (cmd 09: 1000:404A) */
	int cyc_on, cyc_cur, cyc_from, cyc_to, cyc_up;
	uint32_t cyc_next;
	int paused;             /* builtin 6C (field +0x67): the animation stops */
	uint32_t paused_at;
	/* movie (cmd 17, type 10 resource): the animation stays halted until it ends */
	int mv_active;
	Lb7Movie mv;
	Lb7MovieFrame mv_frame;
	uint32_t mv_pc, mv_rec, mv_next_ms;
	int mv_left;            /* frames still to show in the movie's command 01 */
	int mv_audio;           /* mixer entry */
} Sprite;

#define SPR_MAX_STAMPS 512
typedef struct {            /* image stamped onto the background (opcode 47, command 04) */
	Lb7Bitmap bmp;
	uint8_t *pix;
	int left, top;          /* relative to the screen center */
} SprStamp;

typedef struct {
	Sprite spr[SPR_MAX];
	SprStamp stamps[SPR_MAX_STAMPS];
	int n_stamps;
	int seq;
	const Video *last_video; /* last screen drawn (for stamps requested by scripts) */
	const Lb7 *lib;
	struct Rt *rt;
	uint32_t now_ms;
	uint32_t unknown_cmd[0x20];
	/* background (opcode 06): bitmaps for 800x600 and 640x480, or a solid color */
	uint16_t bg_id[2];      /* [0] 800x600, [1] 640x480; 0 = none */
	int bg_color;
	SprFrame bg[2];
	/* current text of the type 18 resources (structure +0x2A of 1010:1CA0; opcode 29) */
	struct { uint16_t id; char s[128]; } texts[64];
	int n_texts;
	/* colour of the type 18 resources changed by opcode 5E (word +4 of the 1010:1CA0 record,
	   initially the low byte of word 0 of the resource) */
	struct { uint16_t id; uint8_t color; } text_colors[64];
	int n_text_colors;
	/* type 1 bitmaps changed in memory by builtin 7D (FUN_1008_708A): in the original the
	   pixels of the loaded resource change, so every frame using that bitmap sees them */
	struct { uint16_t id; const uint8_t *data; Lb7Bitmap bmp; uint8_t *pix; } edited[32];
	int n_edited;
	/* event guard of FUN_1000_44DC: world +0xA82 = an event callback is running, +0xA84 = events
	   suppressed (S_061(0), library switch); a 14/15/16 command then waits for a later tick */
	int event_busy, events_off;
	/* opcode 06 mode 2 (S_031(0xFFFF, -1), world +0x880 = 0xFFFF): the screen is not cleared */
	int bg_keep;
	/* opcode 2A (S_056, world +0x86A..+0x870): clip rectangle, screen-centre relative, right and
	   bottom excluded; clip_on = 0 means the whole screen */
	int clip_on, clip[4];
	/* opcode 27 (S_053, world +0x876/+0x878/+0x87A): display zoom 1..4, source offset */
	int zoom, zoom_x, zoom_y;
	/* opcode 06 transition in progress (TOPSY 1008:0F10): screen-sized background buffer (the
	   original's bitmap DAT_0E46) into which rectangles of the new bitmap are copied */
	uint8_t *bg_canvas;
	int bg_cw, bg_ch;
	uint8_t *comp;          /* last composition (op 06 mode 2, op 2A), comp_w x comp_h */
	int comp_w, comp_h;
} SprWorld;

/* Diagnostics (upsy --probe X Y): in the trace, every draw covering point (X, Y) of the
 * logical screen, with sprite, layer, frame and position. */
extern int spr_probe_on, spr_probe_x, spr_probe_y, spr_probe_r;

void spr_init(SprWorld *w, const Lb7 *lib, struct Rt *rt);
void spr_clear(SprWorld *w);              /* library change */
int spr_load(SprWorld *w, unsigned id);    /* creates the sprite from the type 17 resource */
void spr_unload(SprWorld *w, unsigned id);
/* Show instruction (0x05): anim 0 = fixed frame; hide_first = flag +6. */
int spr_show(SprWorld *w, unsigned id, unsigned anim, int hide_first);
void spr_hide(SprWorld *w, unsigned id);
void spr_move(SprWorld *w, unsigned id, int x, int y); /* opcode 3F */
void spr_set_layer(SprWorld *w, unsigned id, int layer); /* opcode 1C (S_065) */
void spr_break_loops(SprWorld *w, unsigned id, int on);  /* opcode 19 (S_004) */
/* Opcode 47 (S_064): stamps sprite id onto the background (0 = all); keep=0 then removes it. */
void spr_stamp(SprWorld *w, const Video *v, unsigned id, int keep);
/* Opcode 06 {mode, bitmap 800x600, bitmap 640x480, color} (FUN_1008_03CE): mode 0 = bitmap,
 * 1 = solid color, 2 = the screen is no longer cleared, other = black. */
void spr_background(SprWorld *w, int mode, unsigned bmp800, unsigned bmp640, int color);
/* Opcode 06 with transition word +10 != 0 (FUN_1008_05D8 -> 0F10): only with mode 0 and a
 * background bitmap already present; the new bitmap enters with effect type 1..10 (parameters
 * p12 = word +12, p14 = word +14), synchronously, a frame (rt->frame) per step. 0 = not done
 * (the caller then runs the plain spr_background, like FUN_1008_0654). */
int spr_background_transition(SprWorld *w, unsigned bmp800, unsigned bmp640, int color,
	int type, unsigned p12, unsigned p14);
/* Opcode 2A (S_056): clip rectangle x1,y1,x2,y2 (inclusive); x1 >= x2 or y1 >= y2 = no clip. */
void spr_set_clip(SprWorld *w, int x1, int y1, int x2, int y2);
/* Opcode 27 (S_053): display zoom 0..4 with source offset; 0 = normal. */
void spr_set_zoom(SprWorld *w, int zoom, int ox, int oy);
/* Builtin 98 (S_085): moves every sprite (and its running movement) by (dx, dy). */
void spr_scroll_all(SprWorld *w, int dx, int dy);
/* Builtin 5A (immediate=1) / 5B (=0): animation command {sprite, command, values...}. */
int spr_command(SprWorld *w, int immediate, unsigned argc, const int16_t *args);
/* Topmost sprite under point (x, y) relative to the screen center (S_029):
 * type 17 resource id, 0 = none. */
unsigned spr_hit(SprWorld *w, int x, int y);
/* Does point (x, y) fall on sprite id? (S_079: test of a single sprite, S_029 rules) */
int spr_hit_id(SprWorld *w, unsigned id, int x, int y);
void spr_move_rel(SprWorld *w, unsigned id, int dx, int dy); /* dragging (S_051) */
int spr_loaded(SprWorld *w, unsigned id);
/* S_023: resource id of the frames (bitmap/cel or text), at most max; -1 = not loaded */
int spr_frame_ids(SprWorld *w, unsigned id, uint16_t *out, int max);
/* Opcode 28 (S_054): position; 0 if the sprite does not exist. */
int spr_get_pos(SprWorld *w, unsigned id, int *x, int *y);
/* Opcode 37 (S_060): rectangle of the current image (relative to center, right/bottom excluded). */
int spr_get_rect(SprWorld *w, unsigned id, int *l, int *t, int *r, int *b);
/* Builtin 6C (S_071): pause (on) or resume the animation; id 0 = all. */
void spr_pause(SprWorld *w, unsigned id, int on);
/* S_048: pixel-level collision between two sprites; hide_a/hide_b = also valid if hidden. */
int spr_collide(SprWorld *w, unsigned a, unsigned b, int hide_a, int hide_b);
/* Drawing layer (+0x3C) and free word (+0x44, S_077/S_078) of a sprite; 0 = absent. */
int spr_get_layer(SprWorld *w, unsigned id, int *layer);
int spr_get_user(SprWorld *w, unsigned id, int16_t *v);
int spr_set_user(SprWorld *w, unsigned id, int16_t v);
int spr_visible(SprWorld *w, unsigned id);
/* Opcode 29: new text for the type 18 resource id; frames that use it are redone. */
const char *spr_get_text(SprWorld *w, unsigned id);
void spr_set_text(SprWorld *w, unsigned id, const char *s);                   /* S_063: campo +0x56 */
/* Opcode 5E (1010:1F58): new colour (palette index) for the type 18 resource id; frames that
 * use it are redone. */
void spr_set_text_color(SprWorld *w, unsigned id, uint8_t color);
/* Opcode 18 (TOPSY FUN_1008_E06C): the new sprite takes the old one's place in the drawing
 * order; if it has no frames it inherits the old one's images, position and mirroring, otherwise
 * the old one is hidden. Unloading the old one is up to the caller. 0 = error. */
int spr_replace(SprWorld *w, unsigned old_id, unsigned new_id); /* 2 = images inherited */
/* Builtin 7D (FUN_1008_708A -> S_019 = TOPSYS01 1000:2890): draws bitmap src (type 1, or a text
 * of type 18) into bitmap dest (type 1); both rectangles are centred on their anchors, src is
 * shifted by (dx, dy) and clipped to dest; index 0 of src is transparent. 1 = something drawn. */
int spr_bitmap_blit(SprWorld *w, unsigned dest, unsigned src, int dx, int dy);
/* Unloading of a type 1 resource: if no loaded sprite holds it, the next use reads its original
 * pixels again (a sprite that is unloaded releases its members the same way). */
void spr_bitmap_forget(SprWorld *w, unsigned id);
/* Sprite byte +0x12 (FUN_1008_6D44): the frames of member id (bitmap or cel) were not loaded
 * with the sprite; they are resolved (rt_lazy_frame) when they become current. */
void spr_set_lazy(SprWorld *w, unsigned sprite, unsigned member);
/* Resolves the current frames of the sprite (FUN_1000_128C: frame 0 when S_003 fills an empty
 * frame list). */
void spr_resolve(SprWorld *w, unsigned sprite);
/* Default sprite (byte +0x14, DAT_0E3E): 1 if there is one (current or kept from an old library). */
int spr_has_default(SprWorld *w);
/* FUN_1008_7372: with destroy the default sprite is removed (S_013(1, 1)); in any case no sprite
 * is the default one any more. */
void spr_default_reset(SprWorld *w, int destroy);
/* Library close (FUN_1008_C5F8 -> S_042, FUN_1008_72B0 -> FUN_1008_65F2): the default sprite
 * is taken out of the world before the library goes away; its frames are decoded into own
 * memory (the original keeps the members' data alive), its movie stops, it loses its resource
 * id and is pinned on top with layer -1. Call it while the old library is still open.
 * 0 = no default sprite. */
int spr_default_detach(SprWorld *w, Sprite *out);
/* After the new library opened (FUN_1008_C5F8 -> S_004(1), FUN_1008_C794 -> S_042): the kept
 * sprite goes back into the world with its loops broken (field +0x65). */
void spr_default_attach(SprWorld *w, const Sprite *s);
void spr_update(SprWorld *w, uint32_t now_ms);
void spr_draw(SprWorld *w, Video *v);

#endif
