/* Minimal standard I/O for the HYPRLODR Windows x64 target.
 *
 * Implemented in native/win32/crt_impl.c on top of kernel32 only, so the loader
 * and proxy DLL depend on no C runtime at all and behave identically under Wine,
 * Proton and Windows. */
#ifndef HYPRLODR_CRT_STDIO_H
#define HYPRLODR_CRT_STDIO_H

#include <stddef.h>

typedef struct hyprlodr_file FILE;

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
int fflush(FILE *stream);
int fputc(int character, FILE *stream);
int fputs(const char *text, FILE *stream);
char *fgets(char *buffer, int size, FILE *stream);
int printf(const char *format, ...);
int fprintf(FILE *stream, const char *format, ...);
int snprintf(char *buffer, size_t size, const char *format, ...);

/* Diagnostics stream. There is no msvcrt stderr import to lean on, and the real
 * diagnostic channel is the JSON event log, so this never fails fatally. */
FILE *hyprlodr_error_stream(void);
#define stderr hyprlodr_error_stream()

#endif
