/* Drives the compiled production hand-hook callback through a fake GL dispatch.
 * The capture and deferred writer remain the real example/hand_hook.c code. */
#include "hypr_win32.h"

#include <stdio.h>
#include <string.h>

typedef struct hand_capture_test_result {
    unsigned failure, saved, forwarded, vbo_read_bytes, ibo_read_bytes;
    unsigned array_binding, element_binding, claimed, vbo_capacity, ibo_capacity;
    unsigned read_calls, last_mode, last_count, last_type, last_indices;
} hand_capture_test_result;
typedef int (*capture_run_fn)(const char *, unsigned, unsigned, unsigned, unsigned, int, int,
                              unsigned, unsigned, hand_capture_test_result *);

#define GL_UNSIGNED_INT 0x1405u
#define GL_UNSIGNED_SHORT 0x1403u

static int run(capture_run_fn capture, const char *path, unsigned draw_mode,
               unsigned count, unsigned type,
               unsigned attr, int vbo_size, int ibo_size, unsigned offset,
               unsigned repeat, unsigned failure, unsigned expected_saved,
               unsigned expected_vbo, unsigned expected_ibo, unsigned expected_forwarded) {
    hand_capture_test_result result;
    memset(&result, 0, sizeof result);
    if (capture(path, draw_mode, count, type, attr, vbo_size, ibo_size, offset,
                repeat, &result) != 0) {
        printf("fixture: capture entry failed\n"); return 0;
    }
    if (result.failure != failure || result.saved != expected_saved ||
        result.vbo_read_bytes != expected_vbo || result.ibo_read_bytes != expected_ibo ||
        result.forwarded != expected_forwarded || result.array_binding != 91 ||
        result.element_binding != (attr == 4 ? 0u : 42u) ||
        result.read_calls != (expected_vbo ? 2u : 0u) ||
        result.last_mode != draw_mode || result.last_count != count ||
        result.last_type != type || result.last_indices != offset ||
        (expected_saved == 1 && (result.vbo_capacity != (unsigned)vbo_size ||
                                 result.ibo_capacity != (unsigned)ibo_size))) {
        printf("fixture: mismatch fail=%u saved=%u reads=%u/%u forward=%u bindings=%u/%u\n",
               result.failure, result.saved, result.vbo_read_bytes, result.ibo_read_bytes,
               result.forwarded, result.array_binding, result.element_binding);
        return 0;
    }
    printf("fixture: count=%u failure=%u saved=%u read=%u/%u forwarded=%u\n",
           count, result.failure, result.saved, result.vbo_read_bytes,
           result.ibo_read_bytes, result.forwarded);
    return 1;
}

static const char *make_path(char *buffer, size_t capacity, const char *directory,
                             const char *name) {
    size_t directory_length = strlen(directory), name_length = strlen(name);
    if (directory_length + name_length + 2 > capacity) return NULL;
    memcpy(buffer, directory, directory_length);
    buffer[directory_length] = '\\';
    memcpy(buffer + directory_length + 1, name, name_length + 1);
    return buffer;
}

int main(int argc, char **argv) {
    if (argc != 3) { printf("usage: hand_capture_client.exe <capture-dll> <output-dir>\n"); return 2; }
    HMODULE module = LoadLibraryA(argv[1]);
    capture_run_fn capture = module ? (capture_run_fn)GetProcAddress(module,
                                                    "hand_capture_test_run") : NULL;
    if (!capture) { printf("fixture: test entry not exported\n"); return 3; }
    char path[1024];
    int ok = 1;
#define PATH(name) make_path(path, sizeof path, argv[2], name)
    ok &= run(capture, PATH("original.capture"), 4, 1188, GL_UNSIGNED_INT, 0,
              20000, 6000, 0, 1, 0, 1, 17136, 4752, 2);
    ok &= run(capture, PATH("enlarged.capture"), 4, 1191, GL_UNSIGNED_INT, 0,
              18000, 5000, 12, 0, 0, 1, 17208, 4764, 1);
    ok &= run(capture, PATH("bad-attribute.capture"), 4, 1191, GL_UNSIGNED_INT, 1,
              18000, 5000, 0, 0, 3, 0, 0, 0, 1);
    ok &= run(capture, PATH("short-vbo.capture"), 4, 1191, GL_UNSIGNED_INT, 0,
              17207, 5000, 0, 0, 5, 0, 0, 0, 1);
    ok &= run(capture, PATH("short-ibo.capture"), 4, 1191, GL_UNSIGNED_INT, 0,
              18000, 4763, 0, 0, 6, 0, 0, 0, 1);
    ok &= run(capture, PATH("bad-range.capture"), 4, 1191, GL_UNSIGNED_INT, 0,
              18000, 5000, 240, 0, 6, 0, 0, 0, 1);
    ok &= run(capture, PATH("bad-vbo-id.capture"), 4, 1191, GL_UNSIGNED_INT, 2,
              18000, 5000, 0, 0, 3, 0, 0, 0, 1);
    ok &= run(capture, PATH("bad-ibo-id.capture"), 4, 1191, GL_UNSIGNED_INT, 4,
              18000, 5000, 0, 0, 2, 0, 0, 0, 1);
    ok &= run(capture, PATH("bad-pointer.capture"), 4, 1191, GL_UNSIGNED_INT, 3,
              18000, 5000, 0, 0, 3, 0, 0, 0, 1);
    ok &= run(capture, PATH("unsupported-count.capture"), 4, 1200, GL_UNSIGNED_INT, 0,
              18000, 5000, 0, 0, 0, 0, 0, 0, 1);
    ok &= run(capture, PATH("wrong-index-type.capture"), 4, 1188, GL_UNSIGNED_SHORT, 0,
              20000, 6000, 0, 0, 0, 0, 0, 0, 1);
    ok &= run(capture, PATH("writer-failure\\absent.capture"), 4, 1188, GL_UNSIGNED_INT, 0,
              20000, 6000, 0, 0, 0, (unsigned)-1, 17136, 4752, 1);
    ok &= run(capture, PATH("unaligned-offset.capture"), 4, 1191, GL_UNSIGNED_INT, 0,
              18000, 5000, 2, 0, 4, 0, 0, 0, 1);
    ok &= run(capture, PATH("wrong-draw-mode.capture"), 1, 1191, GL_UNSIGNED_INT, 0,
              18000, 5000, 0, 0, 0, 0, 0, 0, 1);
    if (!ok) return 4;
    FreeLibrary(module);
    return 0;
}
