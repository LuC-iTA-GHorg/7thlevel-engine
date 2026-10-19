#include "cov.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern const RtLib *const game_libs[]; /* port\game\game_libs.c */

int cov_on;

typedef struct CovLib {
	char file[32];
	uint32_t n;                 /* index entries (without the extra slots of the copies) */
	uint8_t *can[COV_KINDS];    /* 1 = the entry is of this kind (translated script, list...) */
	uint32_t *hit[COV_KINDS];   /* executions per entry */
	uint32_t (*ops)[8];         /* per script: set of the bytecode opcodes (256 bits) */
	uint32_t builtin[256], op[256], cmd[0x20];
	uint32_t opens;
	struct CovLib *next;
} CovLib;

static CovLib *libs, *cur;

static const char *kind_name[COV_KINDS] = { "script", "lists", "sprite (type 17)", "animations (type 15)" };

/* Opcodes present in the script (same walk as lb7tool disasm, script_walk). */
static void script_ops(const Lb7 *lib, Lb7Entry e, uint32_t set[8])
{
	const uint8_t *d = lb7_entry_data(lib, e);
	if (!d)
		return;
	for (size_t pc = 0; pc < e.size;) {
		unsigned n = lb7_op_len(d + pc, e.size - pc);
		if (!n || pc + n > e.size)
			return;
		unsigned op = lb7_rd16(d + pc) & 0xFF;
		set[op >> 5] |= 1u << (op & 31);
		pc += n;
	}
}

void cov_library(const char *file, const Lb7 *lib, const RtLib *code)
{
	if (!cov_on)
		return;
	CovLib **pp = &libs;
	for (; *pp; pp = &(*pp)->next)
		if (!strcasecmp((*pp)->file, file)) {
			cur = *pp;
			cur->opens++;
			return;
		}
	CovLib *c = calloc(1, sizeof(*c));
	if (!c)
		return;
	snprintf(c->file, sizeof(c->file), "%s", file);
	c->n = lib->n_entries;
	c->ops = calloc(c->n ? c->n : 1, sizeof(*c->ops));
	for (int k = 0; k < COV_KINDS; k++) {
		c->can[k] = calloc(c->n ? c->n : 1, 1);
		c->hit[k] = calloc(c->n ? c->n : 1, sizeof(uint32_t));
	}
	for (uint32_t i = 0; i < c->n; i++) {
		Lb7Entry e = lb7_entry(lib, i);
		int has_code = code && i < code->n_entries;
		c->can[COV_SCRIPT][i] = has_code && code->scripts[i] != NULL;
		c->can[COV_LIST][i] = has_code && code->lists[i] != NULL;
		c->can[COV_SPRITE][i] = e.type == 17;
		c->can[COV_ANIM][i] = e.type == 15;
		if (c->can[COV_SCRIPT][i] && e.type == LB7_T_SCRIPT)
			script_ops(lib, e, c->ops[i]);
	}
	c->opens = 1;
	*pp = c; /* at the tail: the report follows the visiting order */
	cur = c;
}

void cov_mark(int kind, unsigned id)
{
	if (cov_on && cur && id < cur->n)
		cur->hit[kind][id]++;
}

void cov_builtin(unsigned id)
{
	if (cov_on && cur)
		cur->builtin[id & 0xFF]++;
}

void cov_op(unsigned op)
{
	if (cov_on && cur)
		cur->op[op & 0xFF]++;
}

void cov_cmd(unsigned cmd)
{
	if (cov_on && cur)
		cur->cmd[cmd & 0x1F]++;
}

/* List of ids as ranges: "3 5-9 12". */
static void print_ids(FILE *out, const CovLib *c, int kind, int want_hit)
{
	unsigned run = 0, first = 0, col = 0;
	for (uint32_t i = 0; i <= c->n; i++) {
		int in = i < c->n && c->can[kind][i] && (c->hit[kind][i] != 0) == want_hit;
		if (in) {
			if (!run)
				first = i;
			run++;
			continue;
		}
		if (!run)
			continue;
		col += (unsigned)fprintf(out, run == 1 ? " %u" : " %u-%u", first, first + run - 1);
		if (col > 90) {
			fprintf(out, "\n     ");
			col = 0;
		}
		run = 0;
	}
	fprintf(out, "\n");
}

static void print_counts(FILE *out, const char *title, const uint32_t *v, unsigned n)
{
	fprintf(out, "  %s:", title);
	int any = 0;
	for (unsigned i = 0; i < n; i++)
		if (v[i]) {
			fprintf(out, " %02X x%u", i, v[i]);
			any = 1;
		}
	fprintf(out, "%s\n", any ? "" : " none");
}

void cov_write(FILE *out)
{
	uint32_t tot_hit[COV_KINDS] = { 0 }, tot_can[COV_KINDS] = { 0 };
	fprintf(out, "Coverage (phase 0.3): run at least once / present\n");
	for (const CovLib *c = libs; c; c = c->next) {
		fprintf(out, "\n== %s (opens: %u)\n", c->file, c->opens);
		for (int k = 0; k < COV_KINDS; k++) {
			uint32_t h = 0, n = 0;
			for (uint32_t i = 0; i < c->n; i++)
				if (c->can[k][i]) {
					n++;
					h += c->hit[k][i] != 0;
				}
			tot_hit[k] += h;
			tot_can[k] += n;
			fprintf(out, "  %-22s %5u / %5u  (%5.1f %%)\n", kind_name[k], h, n, n ? 100.0 * h / n : 0.0);
		}
		print_counts(out, "builtins executed", c->builtin, 256);
		print_counts(out, "animation commands executed", c->cmd, 0x20);
		print_counts(out, "instructions passed through rt_op", c->op, 256);
		uint32_t run[8] = { 0 }, all[8] = { 0 };
		for (uint32_t i = 0; i < c->n; i++)
			for (int w = 0; w < 8; w++) {
				all[w] |= c->ops[i][w];
				if (c->hit[COV_SCRIPT][i])
					run[w] |= c->ops[i][w];
			}
		fprintf(out, "  opcodes in executed scripts (estimated from the bytecode):");
		for (unsigned op = 0; op < 256; op++)
			if (run[op >> 5] >> (op & 31) & 1)
				fprintf(out, " %02X", op);
		fprintf(out, "\n  opcodes only in never-executed scripts:");
		int any = 0;
		for (unsigned op = 0; op < 256; op++)
			if ((all[op >> 5] & ~run[op >> 5]) >> (op & 31) & 1) {
				fprintf(out, " %02X", op);
				any = 1;
			}
		fprintf(out, "%s\n", any ? "" : " none");
		for (int k = 0; k < COV_KINDS; k++) {
			fprintf(out, "  %s never executed:", kind_name[k]);
			print_ids(out, c, k, 0);
		}
		/* to merge the reports of several runs (tools\scripts\run_scenes.ps1) */
		for (int k = 0; k < COV_KINDS; k++) {
			fprintf(out, "  %s executed:", kind_name[k]);
			print_ids(out, c, k, 1);
		}
	}
	fprintf(out, "\n== total\n");
	for (int k = 0; k < COV_KINDS; k++)
		fprintf(out, "  %-22s %5u / %5u  (%5.1f %%)\n", kind_name[k], tot_hit[k], tot_can[k],
			tot_can[k] ? 100.0 * tot_hit[k] / tot_can[k] : 0.0);
	fprintf(out, "  libraries never opened:");
	int any = 0;
	for (int i = 0; game_libs[i]; i++) {
		const CovLib *c = libs;
		while (c && strcasecmp(c->file, game_libs[i]->file))
			c = c->next;
		if (!c) {
			fprintf(out, " %s", game_libs[i]->file);
			any = 1;
		}
	}
	fprintf(out, "%s\n", any ? "" : " none");
}

void cov_free(void)
{
	while (libs) {
		CovLib *n = libs->next;
		free(libs->ops);
		for (int k = 0; k < COV_KINDS; k++) {
			free(libs->can[k]);
			free(libs->hit[k]);
		}
		free(libs);
		libs = n;
	}
	cur = NULL;
}
