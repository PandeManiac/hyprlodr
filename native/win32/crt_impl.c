/* Self-contained C runtime subset for the HYPRLODR Windows x64 target.
 *
 * Everything the loader, host and proxy DLL need is implemented on kernel32:
 * heap allocation, string operations, a printf-compatible formatter, and file
 * I/O. This removes any dependency on msvcrt/UCRT, whose per-version behaviour
 * (missing snprintf, CRT start-up requirements, data imports) differs between
 * Wine, Proton and Windows.
 *
 * Compile with -fno-builtin so the compiler cannot turn these loops back into
 * calls to themselves.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hypr_win32.h"

#define HYPRLODR_IO_BUFFER 1024

struct hyprlodr_file {
    HANDLE handle;
    int writable;
    int at_end;
    size_t position;
    size_t length;
    unsigned char buffer[HYPRLODR_IO_BUFFER];
};

/* ------------------------------------------------------------------- memory */

void *malloc(size_t size) {
    if (size == 0) size = 1;
    return HeapAlloc(GetProcessHeap(), 0, size);
}

void *calloc(size_t count, size_t size) {
    size_t total = count * size;
    void *block;
    if (count != 0 && total / count != size) return NULL;
    block = malloc(total);
    if (block) memset(block, 0, total);
    return block;
}

void free(void *pointer) {
    if (pointer) HeapFree(GetProcessHeap(), 0, pointer);
}

void *realloc(void *pointer, size_t size) {
    if (!pointer) return malloc(size);
    if (size == 0) {
        free(pointer);
        return NULL;
    }
    return HeapReAlloc(GetProcessHeap(), 0, pointer, size);
}

/* ------------------------------------------------------------------ strings */

void *memcpy(void *destination, const void *source, size_t count) {
    unsigned char *out = (unsigned char *)destination;
    const unsigned char *in = (const unsigned char *)source;
    while (count--) *out++ = *in++;
    return destination;
}

void *memmove(void *destination, const void *source, size_t count) {
    unsigned char *out = (unsigned char *)destination;
    const unsigned char *in = (const unsigned char *)source;
    if (out == in || count == 0) return destination;
    if (out < in) {
        while (count--) *out++ = *in++;
    } else {
        out += count;
        in += count;
        while (count--) *--out = *--in;
    }
    return destination;
}

void *memset(void *destination, int value, size_t count) {
    unsigned char *out = (unsigned char *)destination;
    while (count--) *out++ = (unsigned char)value;
    return destination;
}

size_t strlen(const char *text) {
    size_t length = 0;
    if (!text) return 0;
    while (text[length]) ++length;
    return length;
}

int strcmp(const char *left, const char *right) {
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

int strncmp(const char *left, const char *right, size_t count) {
    while (count && *left && *left == *right) {
        ++left;
        ++right;
        --count;
    }
    if (!count) return 0;
    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

char *strchr(const char *text, int character) {
    char target = (char)character;
    while (*text) {
        if (*text == target) return (char *)text;
        ++text;
    }
    return target == '\0' ? (char *)text : NULL;
}

unsigned long strtoul(const char *text, char **end, int base) {
    unsigned long value = 0;
    int digits = 0;
    if (!text) {
        if (end) *end = NULL;
        return 0;
    }
    while (*text == ' ' || *text == '\t') ++text;
    if (!base) base = 10;
    for (;; ++text) {
        int digit;
        char c = *text;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'z') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') digit = c - 'A' + 10;
        else break;
        if (digit >= base) break;
        value = value * (unsigned long)base + (unsigned long)digit;
        ++digits;
    }
    if (end) *end = (char *)(digits ? text : NULL);
    return value;
}

void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *)) {
    unsigned char *items = (unsigned char *)base;
    unsigned char *scratch = (unsigned char *)malloc(size);
    if (!scratch || count < 2) {
        free(scratch);
        return;
    }
    for (size_t i = 1; i < count; ++i) {
        size_t j = i;
        memcpy(scratch, items + i * size, size);
        while (j > 0 && compare(items + (j - 1) * size, scratch) > 0) {
            memcpy(items + j * size, items + (j - 1) * size, size);
            --j;
        }
        memcpy(items + j * size, scratch, size);
    }
    free(scratch);
}

/* ---------------------------------------------------------------- formatting */

typedef struct {
    char *out;
    size_t capacity;
    size_t length;
    int truncated;
} format_sink;

static void sink_char(format_sink *sink, char c) {
    if (sink->length + 1 < sink->capacity) {
        sink->out[sink->length++] = c;
    } else {
        sink->truncated = 1;
        ++sink->length;
    }
}

static void sink_padded(format_sink *sink, const char *text, size_t length,
                        int width, char pad, int left_align) {
    if (width > 0 && (size_t)width > length && !left_align)
        for (size_t i = 0; i < (size_t)width - length; ++i) sink_char(sink, pad);
    for (size_t i = 0; i < length; ++i) sink_char(sink, text[i]);
    if (width > 0 && (size_t)width > length && left_align)
        for (size_t i = 0; i < (size_t)width - length; ++i) sink_char(sink, ' ');
}

static int format_into(char *out, size_t capacity, const char *format,
                       __builtin_va_list args) {
    format_sink sink = {out, capacity, 0, 0};
    for (const char *cursor = format; *cursor; ++cursor) {
        int left_align = 0, zero_pad = 0, width = 0, precision = -1;
        if (*cursor != '%') {
            sink_char(&sink, *cursor);
            continue;
        }
        ++cursor;
        if (*cursor == '%') {
            sink_char(&sink, '%');
            continue;
        }
        for (;; ++cursor) {
            if (*cursor == '-') left_align = 1;
            else if (*cursor == '0') zero_pad = 1;
            else if (*cursor == '+' || *cursor == ' ' || *cursor == '#') continue;
            else break;
        }
        while (*cursor >= '0' && *cursor <= '9') width = width * 10 + (*cursor++ - '0');
        if (*cursor == '.') {
            ++cursor;
            precision = 0;
            while (*cursor >= '0' && *cursor <= '9') precision = precision * 10 + (*cursor++ - '0');
        }
        /* Windows is LLP64: long stays 32-bit, size_t and long long are 64-bit.
         * Reading the wrong width here corrupts every vararg that follows. */
        int wide = 0;
        while (*cursor == 'l' || *cursor == 'z' || *cursor == 'h') {
            if (*cursor == 'z') {
                wide = 1;
            } else if (*cursor == 'l') {
                if (cursor[1] == 'l') {
                    wide = 1;
                    ++cursor;
                } else {
                    wide = 0;
                }
            }
            ++cursor;
        }
        switch (*cursor) {
        case 's': {
            const char *text = __builtin_va_arg(args, const char *);
            size_t length;
            if (!text) text = "(null)";
            length = strlen(text);
            if (precision >= 0 && (size_t)precision < length) length = (size_t)precision;
            sink_padded(&sink, text, length, width, ' ', left_align);
            break;
        }
        case 'c': {
            char c = (char)__builtin_va_arg(args, int);
            sink_padded(&sink, &c, 1, width, ' ', left_align);
            break;
        }
        case 'd':
        case 'i': {
            long long value = wide ? __builtin_va_arg(args, long long)
                                   : (long long)__builtin_va_arg(args, int);
            char digits[24];
            int count = 0, negative = value < 0;
            unsigned long long magnitude =
                negative ? (unsigned long long)(-value) : (unsigned long long)value;
            do {
                digits[count++] = (char)('0' + (int)(magnitude % 10ull));
                magnitude /= 10ull;
            } while (magnitude);
            if (negative) sink_char(&sink, '-');
            while (count > 0) sink_char(&sink, digits[--count]);
            (void)zero_pad;
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            unsigned long long value = wide ? __builtin_va_arg(args, unsigned long long)
                                            : (unsigned long long)__builtin_va_arg(args, unsigned);
            const char *alphabet = (*cursor == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            unsigned base = (*cursor == 'u') ? 10u : 16u;
            char digits[32];
            int count = 0;
            do {
                digits[count++] = alphabet[value % base];
                value /= base;
            } while (value);
            if (zero_pad && width > 0)
                for (int i = count; i < width; ++i) sink_char(&sink, '0');
            while (count > 0) sink_char(&sink, digits[--count]);
            break;
        }
        case 'p': {
            unsigned long long value = (unsigned long long)(size_t)__builtin_va_arg(args, void *);
            char digits[32];
            int count = 0;
            sink_char(&sink, '0');
            sink_char(&sink, 'x');
            do {
                digits[count++] = "0123456789abcdef"[value % 16u];
                value /= 16u;
            } while (value);
            while (count > 0) sink_char(&sink, digits[--count]);
            break;
        }
        case '\0':
            cursor -= 1;
            break;
        default:
            sink_char(&sink, '%');
            sink_char(&sink, *cursor);
            break;
        }
    }
    if (capacity > 0) out[sink.length < capacity ? sink.length : capacity - 1] = '\0';
    return (int)sink.length;
}

int snprintf(char *buffer, size_t size, const char *format, ...) {
    __builtin_va_list args;
    int result;
    __builtin_va_start(args, format);
    result = format_into(buffer, size, format, args);
    __builtin_va_end(args);
    return result;
}

/* ------------------------------------------------------------------- files */

FILE *fopen(const char *path, const char *mode) {
    FILE *file;
    DWORD access, disposition;
    HANDLE handle;
    if (!path || !mode) return NULL;
    if (mode[0] == 'r') {
        access = GENERIC_READ;
        disposition = OPEN_EXISTING;
    } else if (mode[0] == 'a') {
        /* Append: FILE_APPEND_DATA makes every WriteFile land at the end, and
         * OPEN_ALWAYS keeps what is already there. Treating this as "w" would
         * silently truncate, which is exactly what a log must never do. */
        access = FILE_APPEND_DATA;
        disposition = OPEN_ALWAYS;
    } else {
        access = GENERIC_WRITE;
        disposition = CREATE_ALWAYS;
    }
    handle = CreateFileA(path, access, FILE_SHARE_READ, NULL, disposition,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) return NULL;
    file = (FILE *)calloc(1, sizeof(FILE));
    if (!file) {
        CloseHandle(handle);
        return NULL;
    }
    file->handle = handle;
    file->writable = access != GENERIC_READ;
    return file;
}

int fflush(FILE *stream) {
    if (!stream) return -1;
    if (stream->writable && stream->length > 0) {
        DWORD written = 0;
        if (!WriteFile(stream->handle, stream->buffer, (DWORD)stream->length, &written, NULL) ||
            written != stream->length) {
            stream->length = 0;
            return -1;
        }
        stream->length = 0;
    }
    return 0;
}

int fputc(int character, FILE *stream) {
    if (!stream) return -1;
    if (!stream->writable) return -1;
    if (stream->length == HYPRLODR_IO_BUFFER && fflush(stream) != 0) return -1;
    stream->buffer[stream->length++] = (unsigned char)character;
    return (unsigned char)character;
}

int fputs(const char *text, FILE *stream) {
    if (!stream || !text) return -1;
    for (const char *cursor = text; *cursor; ++cursor)
        if (fputc(*cursor, stream) < 0) return -1;
    return 0;
}

char *fgets(char *buffer, int size, FILE *stream) {
    int count = 0;
    if (!buffer || size < 1 || !stream) return NULL;
    while (count < size - 1) {
        if (stream->position == stream->length) {
            DWORD read = 0;
            if (stream->at_end) break;
            if (!ReadFile(stream->handle, stream->buffer, HYPRLODR_IO_BUFFER, &read, NULL)) break;
            stream->position = 0;
            stream->length = read;
            if (read == 0) {
                stream->at_end = 1;
                break;
            }
        }
        buffer[count++] = (char)stream->buffer[stream->position++];
        if (buffer[count - 1] == '\n') break;
    }
    if (count == 0) return NULL;
    buffer[count] = '\0';
    return buffer;
}

int fprintf(FILE *stream, const char *format, ...) {
    __builtin_va_list args;
    char local[512];
    char *text = local;
    int length;
    int result;
    __builtin_va_start(args, format);
    length = format_into(local, sizeof local, format, args);
    __builtin_va_end(args);
    if (length >= (int)sizeof local) {
        text = (char *)malloc((size_t)length + 1);
        if (!text) return -1;
        __builtin_va_start(args, format);
        format_into(text, (size_t)length + 1, format, args);
        __builtin_va_end(args);
    }
    result = fputs(text, stream);
    if (text != local) free(text);
    return result == 0 ? length : -1;
}

int printf(const char *format, ...) {
    __builtin_va_list args;
    char local[1024];
    char *text = local;
    int length;
    int result;
    __builtin_va_start(args, format);
    length = format_into(local, sizeof local, format, args);
    __builtin_va_end(args);
    if (length >= (int)sizeof local) {
        text = (char *)malloc((size_t)length + 1);
        if (!text) return -1;
        __builtin_va_start(args, format);
        format_into(text, (size_t)length + 1, format, args);
        __builtin_va_end(args);
    }
    /* Console output via the standard handle, kept independent of stdio. */
    {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0;
        if (out != INVALID_HANDLE_VALUE && text)
            WriteFile(out, text, (DWORD)strlen(text), &written, NULL);
    }
    result = length;
    if (text != local) free(text);
    return result;
}

int fclose(FILE *stream) {
    int status;
    if (!stream) return -1;
    status = fflush(stream);
    CloseHandle(stream->handle);
    free(stream);
    return status;
}

static FILE *g_error_stream;

FILE *hyprlodr_error_stream(void) {
    if (!g_error_stream) g_error_stream = fopen("NUL", "w");
    return g_error_stream;
}
