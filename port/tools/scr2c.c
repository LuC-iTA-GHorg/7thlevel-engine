/*
 * scr2c - translates the 7th Level scripts (type 16) and instruction lists (type 11) of a
 * .BIN archive into C source for the runtime in port\src\rt.h.
 *
 *   scr2c lib     <FILE.BIN> <out.c>        logic of an archive -> C functions
 *   scr2c registry <out.c> <FILE.BIN>...     table of the translated libraries
 *
 * The generated code goes into port\game\ and is meant to be maintained by hand afterwards: the
 * if/else/while structures are rebuilt from the block offsets in the bytecode; where that is
 * not possible, gotos to L_xxxx labels remain (offsets in the original script).
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/lb7.h"

#define rd16 lb7_rd16

/* ---------------------------------------------------------------- dynamic strings */

static char *fmt(const char *f, ...) __attribute__((format(printf, 1, 2)));
static char *fmt(const char *f, ...)
{
	va_list ap;
	va_start(ap, f);
	int n = vsnprintf(NULL, 0, f, ap);
	va_end(ap);
	char *s = malloc((size_t)n + 1);
	va_start(ap, f);
	vsnprintf(s, (size_t)n + 1, f, ap);
	va_end(ap);
	return s;
}

/* ---------------------------------------------------------------- statistics */

static struct {
	unsigned scripts, lists, insns, ifs, ifelses, whiles, fallback_if, fallback_while, gotos;
	unsigned ptr2int, int2ptr, bad_expr, bad_jump, anim_events;
	unsigned rt_op[256];
} st;

/* ---------------------------------------------------------------- context */

typedef struct {
	const Lb7 *lb;
	const char *prefix;  /* e.g. "topsmain" */
	FILE *out;
	const uint8_t *d;    /* current script */
	size_t size;
	unsigned id;
	uint8_t *is_insn;    /* start of an instruction */
	uint8_t *is_target;  /* target of an explicit jump */
	int in_list;
} Ctx;

static int entry_is(const Ctx *c, unsigned id, unsigned type)
{
	return id < c->lb->n_entries && lb7_entry(c->lb, id).type == type;
}

/* ---------------------------------------------------------------- operands */

/* Encoded operand of a statement -> C expression (value). */
static char *operand_c(uint16_t v)
{
	if (v < 0x13FE)
		return fmt("G(%u)", v);
	if (v < 0x159F)
		return fmt("l[%u]", v - 0x13FE);
	return fmt("%d", (int16_t)(v - 0x8ACF));
}

/* Variable (operands 02/04 of expressions, always variables). */
static char *var_c(uint16_t v)
{
	if (v < 0x13FE)
		return fmt("G(%u)", v);
	return fmt("l[%u]", v - 0x13FE);
}

/* Constant operand? Returns the value in *val. */
static int operand_const(uint16_t v, int *val)
{
	if (v < 0x159F)
		return 0;
	*val = (int16_t)(v - 0x8ACF);
	return 1;
}

/* ---------------------------------------------------------------- expressions */

typedef struct {
	char *s;
	int addr; /* 1 = expression of type int16_t* */
	int is_const;
	int cval;
} Sym;

static char *val(Sym x)
{
	if (!x.addr)
		return x.s;
	st.ptr2int++;
	return fmt("rt_ptr2int(rt, %s)", x.s);
}

static char *addr(Sym x)
{
	if (x.addr)
		return x.s;
	st.int2ptr++;
	return fmt("rt_int2ptr(rt, %s)", x.s);
}

/* *p -> lvalue; simplifies *&X to X. */
static char *deref(Sym x)
{
	char *p = addr(x);
	if (p[0] == '&' && !strpbrk(p + 1, " +-*/"))
		return fmt("%s", p + 1);
	return fmt("(*%s)", p);
}

static Sym mk(char *s, int a)
{
	Sym x = { s, a, 0, 0 };
	return x;
}

/* RPN (Script_EvalExpr) -> C expression. NULL if not translatable. */
static char *expr_c(const Ctx *c, const uint8_t *e, size_t avail)
{
	(void)c;
	Sym s[64];
	int sp = 0;
	size_t i = 0;
#define POP() (sp > 0 ? s[--sp] : mk(fmt("0 /* empty stack */"), 0))
	static const char *binop[0x22] = {
		[0x12] = "+", [0x13] = "-", [0x14] = "*", [0x15] = "/", [0x16] = "%", [0x17] = "==",
		[0x18] = "<=", [0x19] = "<", [0x1A] = ">=", [0x1B] = ">", [0x1C] = "!=", [0x1D] = "&&",
		[0x1E] = "||", [0x1F] = "&", [0x20] = "|", [0x21] = "^" };
	while (i < avail && e[i]) {
		uint8_t t = e[i];
		uint16_t w = i + 3 <= avail ? rd16(e + i + 1) : 0;
		if (sp >= 62) {
			st.bad_expr++;
			return NULL;
		}
		switch (t) {
		case 0x01: s[sp] = mk(fmt("%d", (int16_t)w), 0); s[sp].is_const = 1; s[sp].cval = (int16_t)w; sp++; i += 3; continue;
		case 0x02: s[sp++] = mk(var_c(w), 0); i += 3; continue;
		case 0x04: s[sp++] = mk(fmt("&%s", var_c(w)), 1); i += 3; continue;
		case 0x03: { Sym ix = POP(), b = POP(); s[sp++] = mk(fmt("(%s)[%s]", addr(b), val(ix)), 0); break; }
		case 0x06: { Sym ix = POP(), b = POP(); s[sp++] = mk(fmt("&(%s)[%s]", addr(b), val(ix)), 1); break; }
		case 0x05: { Sym a = POP(); s[sp++] = mk(deref(a), 0); break; }
		case 0x08: { Sym v = POP(), a = POP(); s[sp++] = mk(fmt("(%s = %s)", deref(a), val(v)), 0); break; }
		case 0x0D: case 0x0E: {
			unsigned p1 = i + 5 <= avail ? rd16(e + i + 1) : 0, argc = i + 5 <= avail ? rd16(e + i + 3) : 0;
			if ((int)argc + 1 > sp) {
				st.bad_expr++;
				return NULL;
			}
			char *args = fmt("%s", "");
			for (unsigned k = 0; k < argc; k++) {
				char *a = val(s[sp - (int)argc + (int)k]);
				args = fmt("%s%s%s", args, k ? ", " : "", a);
			}
			sp -= (int)argc;
			Sym target = POP();
			char *list = argc ? fmt("(const int16_t[]){%s}", args) : fmt("NULL");
			if (t == 0x0D)
				s[sp++] = mk(fmt("rt_builtin(rt, 0x%02X, %u, %s)", target.is_const ? (unsigned)target.cval & 0xFFFF : 0u, argc, list), 0);
			else
				s[sp++] = mk(fmt("rt_call(rt, %s, l + %u, %u, %s)", val(target), p1, argc, list), 0);
			if (t == 0x0D && !target.is_const)
				s[sp - 1] = mk(fmt("rt_builtin(rt, %s, %u, %s)", val(target), argc, list), 0);
			i += 5;
			continue;
		}
		case 0x0F: { Sym a = POP(); s[sp++] = mk(fmt("(int16_t)(-%s)", val(a)), 0); break; }
		case 0x10: { Sym a = POP(); s[sp++] = mk(fmt("(!%s)", val(a)), 0); break; }
		case 0x11: { Sym a = POP(); s[sp++] = mk(fmt("(int16_t)(~%s)", val(a)), 0); break; }
		case 0x22: case 0x23: case 0x24: case 0x25: {
			Sym a = POP();
			static const char *f[] = { "(++%s)", "(%s++)", "(--%s)", "(%s--)" };
			s[sp++] = mk(fmt(f[t - 0x22], deref(a)), 0);
			break;
		}
		case 0x26: case 0x27:
			/* short-circuit: in C the following || / && operator already does it; the target is checked */
			if (i + 1 + w >= avail || e[i + 1 + w] != (t == 0x26 ? 0x1E : 0x1D))
				st.bad_expr++;
			i += 3;
			continue;
		case 0x28: { Sym a = s[sp - 1]; s[sp++] = mk(deref(a), 0); break; }
		default:
			if (t < 0x22 && binop[t]) {
				Sym b = POP(), a = POP();
				int ar = t >= 0x12 && t <= 0x14; /* 16-bit arithmetic like the original */
				if (t == 0x15 || t == 0x16) /* zero divisor: runtime error 0x6F (1008:2866) */
					s[sp++] = mk(fmt("rt_%s(rt, %s, %s)", t == 0x15 ? "div" : "mod", val(a), val(b)), 0);
				else if (ar && (a.addr || b.addr))
					s[sp++] = mk(fmt("/* arithmetic on an address */ (%s %s %s)", val(a), binop[t], val(b)), 0);
				else if (ar)
					s[sp++] = mk(fmt("(int16_t)(%s %s %s)", val(a), binop[t], val(b)), 0);
				else
					s[sp++] = mk(fmt("(%s %s %s)", val(a), binop[t], val(b)), 0);
				break;
			}
			st.bad_expr++;
			return NULL;
		}
		i++;
	}
	if (!sp)
		return fmt("0");
	return val(s[sp - 1]);
#undef POP
}

/* ---------------------------------------------------------------- emission */

static void ind(const Ctx *c, int depth)
{
	for (int i = 0; i < depth; i++)
		fputc('\t', c->out);
}

static int valid_target(const Ctx *c, long t)
{
	return t >= 0 && (size_t)t <= c->size && ((size_t)t == c->size || c->is_insn[t]);
}

static void emit_goto(const Ctx *c, int depth, long t)
{
	ind(c, depth);
	if (!valid_target(c, t)) {
		st.bad_jump++;
		fprintf(c->out, "RT_END; /* jump out of the script (+%ld) */\n", t);
		return;
	}
	st.gotos++;
	if ((size_t)t == c->size)
		fprintf(c->out, "RT_END;\n");
	else
		fprintf(c->out, "goto L_%04lx;\n", t);
}

/* Bytes of an instruction as a static array for rt_op. */
static void emit_rt_op(const Ctx *c, int depth, const uint8_t *p, unsigned len)
{
	unsigned op = rd16(p);
	const char *nm = lb7_op_name(op);
	st.rt_op[op & 0xFF]++;
	ind(c, depth);
	fprintf(c->out, "{ /* op %02X%s%s */ static const uint8_t I[] = {", op, nm ? " " : "", nm ? nm : "");
	for (unsigned k = 0; k < len; k++)
		fprintf(c->out, "%s0x%02X", k ? "," : "", p[k]);
	fprintf(c->out, "}; if (!rt_op(rt, l, I)) RT_END; }\n");
}

/* Script called with a constant id: direct call to the translated function. */
static void emit_run(const Ctx *c, int depth, uint16_t enc, const char *frame)
{
	int id;
	ind(c, depth);
	if (operand_const(enc, &id) && entry_is(c, (unsigned)id, LB7_T_SCRIPT))
		fprintf(c->out, "RT_CALL(%s_s%04u, %d, %s);\n", c->prefix, id, id, frame);
	else
		fprintf(c->out, "rt_run(rt, %s, %s); if (rt->stop) RT_END;\n", operand_c(enc), frame);
}

static char *list_fn(const Ctx *c, uint16_t enc)
{
	int id;
	if (operand_const(enc, &id) && entry_is(c, (unsigned)id, 11))
		return fmt("%d", id);
	return operand_c(enc);
}

/* Condition of the comparisons of 0x26 and 0x11 (1008:8C32) as a C expression; a = first operand. */
static char *cmp_c(uint16_t ea, uint16_t eb, uint8_t o)
{
	char *a = operand_c(ea), *b = operand_c(eb), *e;
	if (o & 0x01) e = fmt("%s == %s", a, b);
	else if (o & 0x02) e = fmt("%s > %s", a, b);
	else if (o & 0x04) e = fmt("%s < %s", a, b);
	else if (o & 0x08) e = fmt("(%s | %s) != 0", a, b);
	else if (o & 0x10) e = fmt("(%s & %s) != 0", a, b);
	else if (o & 0x20) e = fmt("%s && (%s %% %s) != 0", b, a, b);
	else e = fmt("%s != 0", a);
	return (o & 0x80) ? fmt("!(%s)", e) : fmt("(%s)", e);
}

/* Simple instruction (not flow control). */
static void emit_stmt(const Ctx *c, int depth, const uint8_t *p, unsigned len)
{
	unsigned op = rd16(p);
#define W(o) rd16(p + (o))
	switch (op) {
	case 0x01: {
		ind(c, depth);
		fprintf(c->out, "{\n");
		ind(c, depth + 1);
		fprintf(c->out, "int16_t *f = l + %u;\n", p[4]);
		for (unsigned k = 0; k < p[5]; k++) {
			ind(c, depth + 1);
			fprintf(c->out, "f[%u] = %s;\n", k, operand_c(W(6 + k * 2)));
		}
		emit_run(c, depth + 1, W(2), "f");
		ind(c, depth);
		fprintf(c->out, "}\n");
		return;
	}
	case 0x02: case 0x03:
		ind(c, depth);
		fprintf(c->out, "if (!rt_%s(rt, %s)) RT_END;\n", op == 2 ? "load" : "unload", operand_c(W(2)));
		return;
	case 0x0D: ind(c, depth); fprintf(c->out, "rt_quit(rt);\n"); return;
	case 0x12: {
		char file[17];
		memcpy(file, p + 2, 16);
		file[16] = 0;
		for (char *q = file; *q; q++)
			if (!isprint((unsigned char)*q) || *q == '"' || *q == '\\')
				*q = '_';
		ind(c, depth);
		fprintf(c->out, "rt_link(rt, \"%s\", %u, %u);\n", file, p[0x12], p[0x13]);
		return;
	}
	case 0x15: ind(c, depth); fprintf(c->out, "rt->v1502 = 0x%04X;\n", W(2)); return;
	case 0x1D: ind(c, depth); fprintf(c->out, "rt->v1508 = 0x%04X;\n", W(2)); return;
	case 0x23: ind(c, depth); fprintf(c->out, "rt->v150e = 0x%04X;\n", W(2)); return;
	case 0x6A: ind(c, depth); fprintf(c->out, "rt->v40dc = 0x%04X;\n", W(2)); return;
	case 0x8D: ind(c, depth); fprintf(c->out, "rt->v40de = 0x%04X;\n", W(2)); return;
	case 0x40:
		ind(c, depth);
		if (W(2))
			fprintf(c->out, "rt->v1504 = (int16_t)(%s - 0x7531);\n", operand_c(W(2)));
		else
			fprintf(c->out, "rt->v1504 = 0;\n");
		return;
	case 0x48:
		ind(c, depth);
		fprintf(c->out, "rt->table4090[%u] = (int16_t)0x%04X;\n", W(2) < 0x22 ? W(2) : 0, W(4));
		return;
	case 0x49:
		ind(c, depth);
		fprintf(c->out, "rt_set_mouse(rt, %s, %s);\n", operand_c(W(2)), operand_c(W(4)));
		return;
	case 0x77: {
		char s[17];
		memcpy(s, p + 2, 16);
		s[16] = 0;
		for (char *q = s; *q; q++)
			if (!isprint((unsigned char)*q) || *q == '"' || *q == '\\')
				*q = '_';
		ind(c, depth);
		fprintf(c->out, "rt_set_string(rt, \"%s\");\n", s);
		return;
	}
	case 0x1B:
		ind(c, depth);
		fprintf(c->out, "rt_debug(rt, 0x%04X, %s);\n", W(4), operand_c(W(2)));
		return;
	case 0x2D: case 0x8F: case 0x71:
		return;
	case 0x16: case 0x17: {
		const char *fn = op == 0x16 ? "rt_run_random" : "rt_run_next";
		ind(c, depth);
		if (p[0x10]) {
			fprintf(c->out, "{ static uint8_t last = %u; if (!%s_list(rt, l, &last, %s)) RT_END; }\n",
				p[3], fn, list_fn(c, W(4)));
		}
		else {
			fprintf(c->out, "{ static uint8_t last = %u; const uint16_t ids[] = {", p[3]);
			for (unsigned k = 0; k < p[2] && k < 6; k++)
				fprintf(c->out, "%s(uint16_t)%s", k ? ", " : "", operand_c(W(4 + k * 2)));
			if (!p[2])
				fprintf(c->out, "0");
			fprintf(c->out, "}; if (!%s(rt, l, &last, %u, ids)) RT_END; }\n", fn, p[2]);
		}
		return;
	}
	case 0x05: /* show: +2 sprite, +4 animation (0 = fixed frame), +6 hide first */
		ind(c, depth);
		/* raw animation 0 = fixed frame, no decoding (1008:A990): it is not G(0) */
		fprintf(c->out, "if (!rt_show(rt, %s, %s, %u)) RT_END;\n", operand_c(W(2)),
			W(4) ? operand_c(W(4)) : "0", p[6]);
		return;
	case 0x38: { /* sprintf: +2 destination, +4 format, +6 n, +8 arguments {word, value byte?} */
		unsigned n = W(6) > 6 ? 6 : W(6);
		ind(c, depth);
		fprintf(c->out, "rt_sprintf(rt, 0x%04X, 0x%04X, %u, ", W(2), W(4), n);
		if (!n)
			fprintf(c->out, "NULL");
		else {
			fprintf(c->out, "(const RtArg[]){");
			for (unsigned k = 0; k < n; k++) {
				const uint8_t *a = p + 8 + k * 4;
				if (a[2])
					fprintf(c->out, "%s{0, %s}", k ? ", " : "", operand_c(rd16(a)));
				else
					fprintf(c->out, "%s{1, (int16_t)0x%04X}", k ? ", " : "", rd16(a));
			}
			fprintf(c->out, "}");
		}
		fprintf(c->out, ");\n");
		return;
	}
	case 0x3B: case 0x3C: { /* INI: +2 variable/string, +4 key, +6 section, +8 encrypted, +9 string */
		ind(c, depth);
		if (op == 0x3B && !p[9])
			fprintf(c->out, "rt_ini_get_int(rt, &%s, 0x%04X, 0x%04X, %u);\n", var_c(W(2)), W(4), W(6), p[8]);
		else if (op == 0x3B)
			fprintf(c->out, "%srt_ini_get_str(rt, 0x%04X, 0x%04X, 0x%04X, %u);\n",
				(p[3] & 0x80) ? "/* constant destination: ignored */ (void)0; // " : "", W(2), W(4), W(6), p[8]);
		else if (!p[9])
			fprintf(c->out, "rt_ini_put_int(rt, %s, 0x%04X, 0x%04X, %u);\n", operand_c(W(2)), W(4), W(6), p[8]);
		else
			fprintf(c->out, "rt_ini_put_str(rt, 0x%04X, 0x%04X, 0x%04X, %u);\n", W(2), W(4), W(6), p[8]);
		return;
	}
	case 0x1F: ind(c, depth); fprintf(c->out, "rt_cursor(rt, %u);\n", p[2]); return;
	case 0x0A: case 0x0B: ind(c, depth); fprintf(c->out, "rt_fade(rt, %d, %u);\n", op == 0x0A, W(2)); return;
	/* the timer id is the raw word, compared as-is with the one of opcode 13 (1008:DCAC) */
	case 0x14: ind(c, depth); fprintf(c->out, "rt_stop_timer(rt, 0x%04X, %u);\n", W(2), p[4]); return;
	case 0x11: { /* if (a OP b) runs +2, otherwise (if +6) runs +4; script 0 = end (1008:97EE) */
		ind(c, depth);
		fprintf(c->out, "if (%s) {\n", cmp_c(W(8), W(10), p[12]));
		if (W(2))
			emit_run(c, depth + 1, W(2), "l");
		else {
			ind(c, depth + 1);
			fprintf(c->out, "RT_END;\n");
		}
		ind(c, depth);
		fprintf(c->out, "}");
		if (p[6]) {
			fprintf(c->out, " else {\n");
			if (W(4))
				emit_run(c, depth + 1, W(4), "l");
			else {
				ind(c, depth + 1);
				fprintf(c->out, "RT_END;\n");
			}
			ind(c, depth);
			fprintf(c->out, "}");
		}
		fprintf(c->out, "\n");
		return;
	}
	case 0x22:
		ind(c, depth);
		fprintf(c->out, "if (!rt_run_index(rt, l, %s, &%s, %s, %u, %u)) RT_END;\n",
			list_fn(c, W(2)), var_c(W(6)), operand_c(W(4)), p[8], p[9]);
		return;
	case 0x4C: {
		char *x = expr_c(c, p + 4, len - 4);
		ind(c, depth);
		if (x)
			fprintf(c->out, "%s%s;\n", x[0] == '(' && strstr(x, " = ") ? "" : "(void)", x);
		else
			fprintf(c->out, "/* untranslatable expression */\n");
		return;
	}
	default:
		emit_rt_op(c, depth, p, len);
		return;
	}
#undef W
}

/* Last instruction that starts in [a, b). */
static long last_insn(const Ctx *c, size_t a, size_t b)
{
	long last = -1;
	for (size_t q = a; q < b; q++)
		if (c->is_insn[q])
			last = (long)q;
	return last;
}

/* Label on a goto absorbed by if/else or while: leaving the block has the same effect. */
static void emit_tail_label(const Ctx *c, long lg)
{
	if (c->is_target[lg])
		fprintf(c->out, "L_%04lx:;\n", lg);
}

/* Emits the code of the range [start, end) of the script. */
static void emit_block(const Ctx *c, size_t start, size_t end, int depth)
{
	size_t pc = start;
	while (pc < end) {
		const uint8_t *p = c->d + pc;
		unsigned op = rd16(p), len = lb7_op_len(p, c->size - pc);
		if (!len) {
			ind(c, depth);
			fprintf(c->out, "RT_END; /* unknown opcode %02X */\n", op);
			return;
		}
		if (c->is_target[pc])
			fprintf(c->out, "L_%04zx:;\n", pc);
		st.insns++;
#define W(o) rd16(p + (o))
		switch (op) {
		case 0x4D: { /* if: +2 jump if false, +4 body start, +6 condition */
			char *cond = expr_c(c, p + 6, len - 6);
			size_t els = pc + W(2), body = pc + W(4);
			if (!cond)
				cond = fmt("0 /* untranslatable condition */");
			if (body == pc + len && els > pc && els <= end) {
				long lg = last_insn(c, body, els);
				if (lg >= 0 && rd16(c->d + lg) == 0x25) {
					long t = lg + (int16_t)rd16(c->d + lg + 2);
					if (t > (long)els && t <= (long)end) {
						st.ifelses++;
						ind(c, depth);
						fprintf(c->out, "if (%s) {\n", cond);
						emit_block(c, body, (size_t)lg, depth + 1);
						emit_tail_label(c, lg); /* the absorbed goto was also a jump target */
						ind(c, depth);
						fprintf(c->out, "} else {\n");
						emit_block(c, els, (size_t)t, depth + 1);
						ind(c, depth);
						fprintf(c->out, "}\n");
						pc = (size_t)t;
						continue;
					}
				}
				st.ifs++;
				ind(c, depth);
				fprintf(c->out, "if (%s) {\n", cond);
				emit_block(c, body, els, depth + 1);
				ind(c, depth);
				fprintf(c->out, "}\n");
				pc = els;
				continue;
			}
			st.fallback_if++;
			ind(c, depth);
			fprintf(c->out, "if (!(%s))\n", cond);
			emit_goto(c, depth + 1, (long)els);
			break;
		}
		case 0x4F: { /* while: like if, the body ends with a goto to the condition */
			char *cond = expr_c(c, p + 6, len - 6);
			size_t ex = pc + W(2), body = pc + W(4);
			if (!cond)
				cond = fmt("0 /* untranslatable condition */");
			long lg = body == pc + len && ex > pc && ex <= end ? last_insn(c, body, ex) : -1;
			if (lg >= 0 && rd16(c->d + lg) == 0x25 && lg + (int16_t)rd16(c->d + lg + 2) == (long)pc) {
				st.whiles++;
				ind(c, depth);
				fprintf(c->out, "while (%s) {\n", cond);
				emit_block(c, body, (size_t)lg, depth + 1);
				emit_tail_label(c, lg);
				ind(c, depth);
				fprintf(c->out, "}\n");
				pc = ex;
				continue;
			}
			st.fallback_while++;
			ind(c, depth);
			fprintf(c->out, "if (!(%s))\n", cond);
			emit_goto(c, depth + 1, (long)ex);
			break;
		}
		case 0x25:
			emit_goto(c, depth, (long)pc + (int16_t)W(2));
			break;
		case 0x26: /* if (!(a OP b)) goto */
			ind(c, depth);
			fprintf(c->out, "if (!%s)\n", cmp_c(W(4), W(6), p[8]));
			emit_goto(c, depth + 1, (long)pc + W(2));
			break;
		case 0x2B: case 0x4E: {
			char *v = op == 0x2B ? operand_c(W(2)) : expr_c(c, p + 10, len - 10);
			ind(c, depth);
			fprintf(c->out, "switch ((uint16_t)(%s)) {\n", v ? v : "0");
			const uint8_t *tab = c->d + pc + W(4);
			for (unsigned k = 0; k < p[8] && pc + W(4) + k * 6 + 6 <= c->size; k++) {
				ind(c, depth);
				fprintf(c->out, "case 0x%04X:\n", rd16(tab + k * 6 + 2));
				emit_goto(c, depth + 1, (long)pc + rd16(tab + k * 6 + 4));
			}
			ind(c, depth);
			fprintf(c->out, "default:\n");
			emit_goto(c, depth + 1, (long)pc + W(6));
			ind(c, depth);
			fprintf(c->out, "}\n");
			break;
		}
		case 0x2C:
			ind(c, depth);
			fprintf(c->out, "RT_END;\n");
			break;
		case 0x59: {
			char *x = expr_c(c, p + 4, len - 4);
			ind(c, depth);
			fprintf(c->out, "rt->retval = %s;\n", x ? x : "0");
			ind(c, depth);
			fprintf(c->out, "RT_END;\n");
			break;
		}
		default:
			emit_stmt(c, depth, p, len);
			break;
		}
#undef W
		pc += len;
	}
}

/* Marks instruction starts and explicit jump targets. */
static int scan(Ctx *c)
{
	c->is_insn = calloc(c->size + 1, 1);
	c->is_target = calloc(c->size + 1, 1);
	for (size_t pc = 0; pc < c->size;) {
		unsigned len = lb7_op_len(c->d + pc, c->size - pc);
		if (!len)
			return 0;
		c->is_insn[pc] = 1;
		pc += len;
	}
	for (size_t pc = 0; pc < c->size;) {
		const uint8_t *p = c->d + pc;
		unsigned op = rd16(p), len = lb7_op_len(p, c->size - pc);
		long t[64];
		int n = 0;
		if (op == 0x25) t[n++] = (long)pc + (int16_t)rd16(p + 2);
		if (op == 0x26 || op == 0x4D || op == 0x4F) t[n++] = (long)pc + rd16(p + 2);
		if (op == 0x2B || op == 0x4E) {
			t[n++] = (long)pc + rd16(p + 6);
			for (unsigned k = 0; k < p[8] && k < 60 && pc + rd16(p + 4) + k * 6 + 6 <= c->size; k++)
				t[n++] = (long)pc + rd16(c->d + pc + rd16(p + 4) + k * 6 + 4);
		}
		for (int k = 0; k < n; k++)
			if (t[k] >= 0 && (size_t)t[k] < c->size)
				c->is_target[t[k]] = 1;
		pc += len;
	}
	return 1;
}

static void emit_script(Ctx *c, unsigned id, const uint8_t *d, uint32_t size)
{
	c->d = d;
	c->size = size;
	c->id = id;
	FILE *o = c->out;
	fprintf(o, "\n/* script %u (%u bytes) */\nstatic void %s_s%04u(Rt *rt, int16_t *l)\n{\n", id, size,
		c->prefix, id);
	fprintf(o, "#define RT_END return\n\t(void)l;\n");
	if (!scan(c))
		fprintf(o, "\t/* misaligned script: not translated */\n");
	else
		emit_block(c, 0, size, 1);
	fprintf(o, "#undef RT_END\n}\n");
	free(c->is_insn);
	free(c->is_target);
	st.scripts++;
}

static void emit_list(Ctx *c, unsigned id, const uint8_t *d, uint32_t size, uint16_t *count)
{
	FILE *o = c->out;
	fprintf(o, "\n/* list %u (type 11, %u bytes): one instruction per item */\n"
		"static int %s_list%04u(Rt *rt, int16_t *l, unsigned item)\n{\n#define RT_END return 0\n"
		"\t(void)l;\n\tswitch (item) {\n", id, size, c->prefix, id);
	unsigned n = 0;
	for (size_t pc = 0; pc < size;) {
		unsigned len = lb7_op_len(d + pc, size - pc);
		if (!len)
			break;
		fprintf(o, "\tcase %u:\n", n);
		emit_stmt(c, 2, d + pc, len);
		fprintf(o, "\t\treturn 1;\n");
		pc += len;
		n++;
	}
	fprintf(o, "\t}\n\treturn 0;\n#undef RT_END\n}\n");
	*count = (uint16_t)n;
	st.lists++;
}

/* Events of an animation (type 15): each 0x15 command contains a script instruction. */
static int emit_anim_events(Ctx *c, unsigned id, const uint8_t *d, uint32_t size)
{
	int any = 0;
	for (size_t pc = 0; pc < size;) {
		unsigned n = lb7_anim_len(d + pc, size - pc);
		if (!n)
			break;
		if (rd16(d + pc) == LB7_ANIM_EVENT && n > 4)
			any = 1;
		pc += n;
	}
	if (!any)
		return 0;
	FILE *o = c->out;
	fprintf(o, "\n/* animation %u (type 15, %u bytes): events; the rest of the animation is data\n"
		" * (frames, timings, movements) played by the sprite engine */\n"
		"static int %s_anim%04u(Rt *rt, unsigned off)\n{\n#define RT_END return 0\n"
		"\tint16_t *l = rt->lstack;\n\t(void)l;\n\tswitch (off) {\n", id, size, c->prefix, id);
	for (size_t pc = 0; pc < size;) {
		unsigned n = lb7_anim_len(d + pc, size - pc);
		if (!n)
			break;
		if (rd16(d + pc) == LB7_ANIM_EVENT && n > 4) {
			unsigned sl = lb7_op_len(d + pc + 4, n - 4);
			fprintf(o, "\tcase 0x%04zx:\n", pc);
			if (sl == n - 4)
				emit_stmt(c, 2, d + pc + 4, sl);
			else
				fprintf(o, "\t\t/* invalid event */\n");
			fprintf(o, "\t\treturn 1;\n");
			st.anim_events++;
		}
		pc += n;
	}
	fprintf(o, "\t}\n\treturn 1;\n#undef RT_END\n}\n");
	return 1;
}

static void prefix_of(const char *path, char *out, size_t n)
{
	const char *b = strrchr(path, '\\');
	const char *b2 = strrchr(path, '/');
	if (!b || (b2 && b2 > b))
		b = b2;
	b = b ? b + 1 : path;
	size_t k = 0;
	/* valid C identifier even with names like T&PPINPC.BIN (Jungle Games): '&' -> '_' */
	for (; *b && *b != '.' && k + 1 < n; b++)
		out[k++] = isalnum((unsigned char)*b) ? (char)tolower((unsigned char)*b) : '_';
	out[k] = 0;
}

static int cmd_lib(const char *bin, const char *outpath)
{
	Lb7 lb;
	char err[512], prefix[64];
	if (!lb7_open(&lb, bin, err, sizeof(err))) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	prefix_of(bin, prefix, sizeof(prefix));
	FILE *o = fopen(outpath, "w");
	if (!o) {
		perror(outpath);
		return 1;
	}
	Ctx c = { .lb = &lb, .prefix = prefix, .out = o };
	const char *base = bin; /* only the file name, also with '/' as separator */
	for (const char *q = bin; *q; q++)
		if (*q == '\\' || *q == '/')
			base = q + 1;
	fprintf(o, "/*\n * Game logic of %s translated to C by port\\tools\\scr2c.c.\n"
		" * Generated from the game files by tools\\gen_game.bat (build.bat): do not edit by hand.\n"
		" * Each function corresponds to a script (type 16) or an instruction list (type 11);\n"
		" * the L_xxxx labels are the offsets in the original script.\n */\n"
		"#include \"rt.h\"\n\n"
		"#pragma GCC diagnostic ignored \"-Wunused-label\"\n"
		"#pragma GCC diagnostic ignored \"-Wunused-variable\"\n", base);
	/* prototypes */
	for (uint32_t i = 0; i < lb.n_entries; i++) {
		Lb7Entry e = lb7_entry(&lb, i);
		if (e.type == LB7_T_SCRIPT)
			fprintf(o, "static void %s_s%04u(Rt *rt, int16_t *l);\n", prefix, i);
		else if (e.type == 11)
			fprintf(o, "static int %s_list%04u(Rt *rt, int16_t *l, unsigned item);\n", prefix, i);
	}
	uint16_t *counts = calloc(lb.n_entries + 1, sizeof(uint16_t));
	uint8_t *has_anim = calloc(lb.n_entries + 1, 1);
	for (uint32_t i = 0; i < lb.n_entries; i++) {
		Lb7Entry e = lb7_entry(&lb, i);
		const uint8_t *d = lb7_entry_data(&lb, e);
		if (!d)
			continue;
		if (e.type == LB7_T_SCRIPT)
			emit_script(&c, i, d, e.size);
		else if (e.type == 11)
			emit_list(&c, i, d, e.size, &counts[i]);
		else if (e.type == 15)
			has_anim[i] = (uint8_t)emit_anim_events(&c, i, d, e.size);
	}
	/* tables */
	fprintf(o, "\nstatic const RtScript scripts[%u] = {\n", lb.n_entries);
	for (uint32_t i = 0; i < lb.n_entries; i++)
		if (lb7_entry(&lb, i).type == LB7_T_SCRIPT)
			fprintf(o, "\t[%u] = %s_s%04u,\n", i, prefix, i);
	fprintf(o, "};\n\nstatic const RtList lists[%u] = {\n", lb.n_entries);
	for (uint32_t i = 0; i < lb.n_entries; i++)
		if (lb7_entry(&lb, i).type == 11)
			fprintf(o, "\t[%u] = %s_list%04u,\n", i, prefix, i);
	fprintf(o, "};\n\nstatic const uint16_t list_len[%u] = {\n", lb.n_entries);
	for (uint32_t i = 0; i < lb.n_entries; i++)
		if (counts[i])
			fprintf(o, "\t[%u] = %u,\n", i, counts[i]);
	fprintf(o, "};\n\nstatic const RtAnimEvent anim_events[%u] = {\n", lb.n_entries);
	for (uint32_t i = 0; i < lb.n_entries; i++)
		if (has_anim[i])
			fprintf(o, "\t[%u] = %s_anim%04u,\n", i, prefix, i);
	fprintf(o, "};\n\nconst RtLib lib_%s = { \"%s\", %u, scripts, lists, list_len, anim_events };\n",
		prefix, base, lb.n_entries);
	fclose(o);

	printf("%s: %u scripts, %u lists, %u instructions; if %u, if/else %u, while %u, goto %u;"
		" unstructured: if %u, while %u; untranslated expressions %u, invalid jumps %u;"
		" ptr2int %u, int2ptr %u; animation events %u\n", base, st.scripts, st.lists, st.insns, st.ifs, st.ifelses,
		st.whiles, st.gotos, st.fallback_if, st.fallback_while, st.bad_expr, st.bad_jump, st.ptr2int,
		st.int2ptr, st.anim_events);
	printf("  rt_op still generic:");
	for (int k = 0; k < 256; k++)
		if (st.rt_op[k]) {
			const char *nm = lb7_op_name((unsigned)k);
			printf(" %02X%s%s%s:%u", k, nm ? "(" : "", nm ? nm : "", nm ? ")" : "", st.rt_op[k]);
		}
	printf("\n");
	free(counts);
	free(has_anim);
	lb7_close(&lb);
	return 0;
}

static int cmd_registry(const char *outpath, int n, char **bins)
{
	FILE *o = fopen(outpath, "w");
	if (!o) {
		perror(outpath);
		return 1;
	}
	fprintf(o, "/* Table of the translated libraries (generated by port\\tools\\scr2c.c). */\n"
		"#include \"rt.h\"\n\n");
	char prefix[64];
	for (int i = 0; i < n; i++) {
		prefix_of(bins[i], prefix, sizeof(prefix));
		fprintf(o, "extern const RtLib lib_%s;\n", prefix);
	}
	fprintf(o, "\nconst RtLib *const game_libs[] = {\n");
	for (int i = 0; i < n; i++) {
		prefix_of(bins[i], prefix, sizeof(prefix));
		fprintf(o, "\t&lib_%s,\n", prefix);
	}
	fprintf(o, "\tNULL\n};\n");
	fclose(o);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 4 && !strcmp(argv[1], "lib"))
		return cmd_lib(argv[2], argv[3]);
	if (argc >= 4 && !strcmp(argv[1], "registry"))
		return cmd_registry(argv[2], argc - 3, argv + 3);
	fprintf(stderr, "usage: %s lib FILE.BIN out.c | registry out.c FILE.BIN...\n", argv[0]);
	return 1;
}
