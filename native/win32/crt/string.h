/* Minimal string functions for the HYPRLODR Windows x64 target.
 * Implemented in native/win32/crt_impl.c (compiled with -fno-builtin). */
#ifndef HYPRLODR_CRT_STRING_H
#define HYPRLODR_CRT_STRING_H

#include <stddef.h>

void *memcpy(void *destination, const void *source, size_t count);
void *memmove(void *destination, const void *source, size_t count);
void *memset(void *destination, int value, size_t count);
size_t strlen(const char *text);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t count);
char *strchr(const char *text, int character);

#endif
