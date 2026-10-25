/*
 * config - port settings (<game>.ini), read at startup and written by the launcher.
 * Format: key=value lines, ';' or '#' = comment, [section] headers ignored.
 * Precedence: defaults < <game>.ini < command line.
 */
#ifndef CONFIG_H
#define CONFIG_H

typedef struct {
	int width, height;   /* Resolution = 800 (800x600) or 640 (640x480) */
	int scale;           /* Scale = 0 (largest that fits on screen) or 1..8: window = scale * resolution */
	int fullscreen;      /* Fullscreen = 0/1 */
	int linear;          /* Filter = nearest (0, sharp pixels) or linear (1, smooth scaling) */
	int integer_scale;   /* IntegerScale = 1: integer multiples only, black bars; 0 = fills the window */
	int stretch;         /* Aspect = keep (0, original 4:3 with black bars) or stretch (1, fills the window) */
	int vsync;           /* Vsync = 0/1 */
	int volume;          /* Volume = 0..100 (master) */
	int music;           /* MusicVolume = 0..100 */
	int trace;           /* Trace = 1: writes <game>_trace.txt (diagnostics) */
	char data[512];      /* DataDir = folder of the .BIN files ('' = automatic) */
	char sf2[512];       /* SoundFont = .sf2 file ('' = automatic) */
	char lib[64];        /* Library = starting archive ('' = GAME_START) */
} Config;

/* Defaults, then <game>.ini next to the executable (or in the current folder).
 * Returns 1 if the file was read. */
int config_load(Config *c);

#endif
