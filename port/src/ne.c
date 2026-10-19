/*
 * ne - resources of 16-bit Windows executables (NE format). See ne.h.
 * NE resource table: at NE header + 0x24; shift word, then groups
 * {type, count, 4 bytes} followed by 12-byte entries {offset, length, flags, id, 4 bytes},
 * offset and length in units of (1 << shift). Id with bit 0x8000 = number.
 */
#include "ne.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

int ne_string(const char *exe_path, unsigned id, char *out, size_t cap)
{
	if (cap)
		out[0] = 0;
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
	int found = 0;
	uint32_t n = (uint32_t)size;
	if (n < 0x40 || d[0] != 'M' || d[1] != 'Z')
		goto done;
	uint32_t ne = rd16le(d + 0x3C);
	if (ne + 0x40 > n || d[ne] != 'N' || d[ne + 1] != 'E')
		goto done;
	uint32_t rt = ne + rd16le(d + ne + 0x24), p = rt + 2;
	if (rt + 2 > n)
		goto done;
	unsigned shift = rd16le(d + rt), block = (id >> 4) + 1;
	while (!found && p + 8 <= n) {
		unsigned type = rd16le(d + p), cnt = rd16le(d + p + 2);
		if (!type)
			break;
		p += 8;
		for (unsigned i = 0; i < cnt && p + 12 <= n; i++, p += 12) {
			uint32_t off = (uint32_t)rd16le(d + p) << shift, len = (uint32_t)rd16le(d + p + 2) << shift;
			if (type != 0x8006 || rd16le(d + p + 6) != (0x8000 | block) || off + len > n)
				continue;
			/* 16 strings in a row: skip the first (id & 15) */
			uint32_t q = off, end = off + len;
			for (unsigned k = 0; k < (id & 15) && q < end; k++)
				q += 1u + d[q];
			if (q >= end || q + 1u + d[q] > end)
				break;
			size_t l = d[q];
			if (l >= cap)
				l = cap ? cap - 1 : 0;
			for (size_t k = 0; k < l; k++)
				out[k] = (char)d[q + 1 + k];
			if (cap)
				out[l] = 0;
			found = 1;
			break;
		}
	}
done:
	free(d);
	return found;
}
