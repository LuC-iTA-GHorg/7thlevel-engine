/*
 * ne - resources of the original game's 16-bit Windows executables (NE format).
 */
#ifndef NE_H
#define NE_H

#include <stddef.h>

/* String `id` of the string table (RT_STRING) of an NE EXE, like Windows' LoadString:
 * block (id >> 4) + 1, 16 strings with the length in one byte. Returns 1 if found (even if
 * empty), 0 if the EXE or the string is missing. */
int ne_string(const char *exe_path, unsigned id, char *out, size_t cap);

#endif
