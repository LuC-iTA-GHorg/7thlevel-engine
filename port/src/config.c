#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <SDL.h>

#include "game.h"

static void set_str(char *dst, size_t cap, const char *v)
{
	snprintf(dst, cap, "%s", v);
}

static int to_bool(const char *v)
{
	return atoi(v) != 0 || !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcasecmp(v, "on");
}

static int clamp(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void apply(Config *c, const char *key, const char *v)
{
	if (!strcasecmp(key, "Resolution")) {
		if (atoi(v) == 640) {
			c->width = 640;
			c->height = 480;
		}
		else {
			c->width = 800;
			c->height = 600;
		}
	}
	else if (!strcasecmp(key, "Scale"))
		c->scale = clamp(atoi(v), 0, 8);
	else if (!strcasecmp(key, "Fullscreen"))
		c->fullscreen = to_bool(v);
	else if (!strcasecmp(key, "Filter"))
		c->linear = !strcasecmp(v, "linear") || !strcasecmp(v, "smooth");
	else if (!strcasecmp(key, "IntegerScale"))
		c->integer_scale = to_bool(v);
	else if (!strcasecmp(key, "Aspect"))
		c->stretch = !strcasecmp(v, "stretch");
	else if (!strcasecmp(key, "Vsync"))
		c->vsync = to_bool(v);
	else if (!strcasecmp(key, "Volume"))
		c->volume = clamp(atoi(v), 0, 100);
	else if (!strcasecmp(key, "MusicVolume"))
		c->music = clamp(atoi(v), 0, 100);
	else if (!strcasecmp(key, "Trace"))
		c->trace = to_bool(v);
	else if (!strcasecmp(key, "DataDir"))
		set_str(c->data, sizeof(c->data), v);
	else if (!strcasecmp(key, "SoundFont"))
		set_str(c->sf2, sizeof(c->sf2), v);
	else if (!strcasecmp(key, "Library"))
		set_str(c->lib, sizeof(c->lib), v);
}

static int read_file(Config *c, const char *path)
{
	FILE *f = fopen(path, "r");
	char line[1024];
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		char *p = line, *eq;
		while (isspace((unsigned char)*p))
			p++;
		if (*p == ';' || *p == '#' || *p == '[' || !(eq = strchr(p, '=')))
			continue;
		*eq = 0;
		char *k = p, *v = eq + 1, *e;
		for (e = k + strlen(k); e > k && isspace((unsigned char)e[-1]); e--)
			;
		*e = 0;
		while (isspace((unsigned char)*v))
			v++;
		for (e = v + strlen(v); e > v && isspace((unsigned char)e[-1]); e--)
			;
		*e = 0;
		apply(c, k, v);
	}
	fclose(f);
	return 1;
}

int config_load(Config *c)
{
	memset(c, 0, sizeof(*c));
	c->width = 800;
	c->height = 600;
	c->scale = 1;
	c->integer_scale = 0; /* in fullscreen, 800x600 on 1920x1080 with integer scale would stay 1:1 */
	c->vsync = 1;
	c->volume = c->music = 100;
	int ok = 0;
	char *base = SDL_GetBasePath();
	if (base) {
		char path[1100];
		snprintf(path, sizeof(path), "%s" GAME_NAME ".ini", base);
		ok = read_file(c, path);
		SDL_free(base);
	}
	if (!ok)
		ok = read_file(c, "" GAME_NAME ".ini");
	return ok;
}
