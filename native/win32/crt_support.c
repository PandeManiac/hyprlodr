/* Small runtime helpers for the CRT-free-ish Windows target.
 *
 * The loader and host only ever use stderr for human diagnostics; on Windows the
 * real diagnostic channel is the JSON event log, so an unopenable error stream
 * must never be fatal. */
#include <stdio.h>

static FILE *g_error_stream;

FILE *hyprlodr_error_stream(void) {
    if (!g_error_stream) g_error_stream = fopen("NUL", "w");
    return g_error_stream;
}
