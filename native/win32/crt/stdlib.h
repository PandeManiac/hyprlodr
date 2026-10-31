/* Minimal standard library for the HYPRLODR Windows x64 target.
 * Implemented on kernel32 heap APIs in native/win32/crt_impl.c. */
#ifndef HYPRLODR_CRT_STDLIB_H
#define HYPRLODR_CRT_STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);
void free(void *pointer);
unsigned long strtoul(const char *text, char **end, int base);
void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *));

#endif
