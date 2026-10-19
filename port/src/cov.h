/*
 * cov - coverage counters (--coverage FILE, phase 0.3 of SCALETTA_MOTORE.md).
 * For each opened library: scripts, lists, sprites and animations run at least once,
 * builtins, animation commands and generic instructions (rt_op) with the number of executions.
 * Opcodes translated to C leave no trace at runtime: the report estimates them from the bytecode
 * of the executed scripts (an opcode in a branch never taken still counts as "reached").
 */
#ifndef COV_H
#define COV_H

#include <stdio.h>

#include "rt.h"

enum { COV_SCRIPT, COV_LIST, COV_SPRITE, COV_ANIM, COV_KINDS };

extern int cov_on; /* 0 = counters off (no cost) */

/* When a library is opened (rt_open_library): prepares or resumes its counters. */
void cov_library(const char *file, const Lb7 *lib, const RtLib *code);
void cov_mark(int kind, unsigned id);
void cov_builtin(unsigned id);
void cov_op(unsigned op);   /* instruction passed through rt_op */
void cov_cmd(unsigned cmd); /* animation command executed (run_anim) */
/* Report of all visited libraries, in visiting order. */
void cov_write(FILE *out);
void cov_free(void);

#endif
