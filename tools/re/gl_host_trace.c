/*
 * Host-side OpenGL trace for HYPER DEMON under Wine.
 *
 * The Windows-side proxy cannot see the engine's GL calls: GLFW resolves them
 * through wgl and the engine imports no gl* names. Wine's opengl32 ultimately
 * calls the host Mesa driver, so a preload shim around libGL's exported entry
 * points and glXGetProcAddress can observe the same calls from the Linux side.
 *
 * This is a research instrument. It never changes a draw, never uploads data,
 * and logs only call signatures and shader-source hashes/interesting tokens.
 * Shader text itself is game content and is not written to the log.
 *
 * Build:
 *   cc -shared -fPIC -O2 -Wall -Wextra -o tools/re/gl_host_trace.so \
 *      tools/re/gl_host_trace.c -ldl -pthread
 *
 * Run:
 *   LD_PRELOAD=$PWD/tools/re/gl_host_trace.so \
 *   HYPR_GL_LOG=/tmp/hyprlodr-host-gl.log wine hyperdemon.exe
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include <GL/gl.h>
#include <GL/glx.h>

typedef void (*gl_shader_source_fn)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void (*gl_link_fn)(GLuint);
typedef void (*gl_use_fn)(GLuint);
typedef void (*gl_draw_elements_fn)(GLenum, GLsizei, GLenum, const void *);
typedef void (*gl_draw_arrays_fn)(GLenum, GLint, GLsizei);
typedef void (*gl_draw_elements_instanced_fn)(GLenum, GLsizei, GLenum, const void *, GLsizei);
typedef void (*gl_draw_arrays_instanced_fn)(GLenum, GLint, GLsizei, GLsizei);
typedef void (*gl_bind_vao_fn)(GLuint);
typedef void (*gl_buffer_data_fn)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void (*gl_get_integer_fn)(GLenum, GLint *);
typedef __GLXextFuncPtr (*glx_get_proc_fn)(const GLubyte *);
typedef void (*glx_swap_fn)(Display *, GLXDrawable);
typedef GLuint (*gl_create_fn)(void);
typedef GLuint (*gl_create_shader_fn)(GLenum);

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_log;
static unsigned long g_frames;
static unsigned long g_draws;
static int g_disabled;
static unsigned long g_unique_count;
static uint64_t g_unique[4096];
static unsigned long g_source_count;

static gl_shader_source_fn real_shader_source;
static gl_link_fn real_link;
static gl_use_fn real_use;
static gl_draw_elements_fn real_draw_elements;
static gl_draw_arrays_fn real_draw_arrays;
static gl_draw_elements_instanced_fn real_draw_elements_instanced;
static gl_draw_arrays_instanced_fn real_draw_arrays_instanced;
static gl_bind_vao_fn real_bind_vao;
static gl_buffer_data_fn real_buffer_data;
static gl_get_integer_fn real_get_integer;
static gl_create_fn real_create_program;
static gl_create_shader_fn real_create_shader;

static void *next_symbol(const char *name) {
    void *symbol = dlsym(RTLD_NEXT, name);
    if (!symbol) {
        /* Some Wine builds resolve GL through libGLX rather than libGL. */
        void *handle = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
        if (handle) symbol = dlsym(handle, name);
    }
    return symbol;
}

static void log_open(void) {
    if (g_log) return;
    const char *path = getenv("HYPR_GL_LOG");
    if (!path || !*path) path = "/tmp/hyprlodr-host-gl.log";
    g_log = fopen(path, "w");
    if (!g_log) {
        g_disabled = 1;
        return;
    }
    setvbuf(g_log, NULL, _IOLBF, 0);
    fprintf(g_log, "hyprlodr host GL trace pid=%ld\n", (long)getpid());
}

static void logf_line(const char *format, ...) {
    va_list args;
    if (g_disabled) return;
    pthread_mutex_lock(&g_lock);
    log_open();
    if (g_log) {
        va_start(args, format);
        vfprintf(g_log, format, args);
        va_end(args);
        fputc('\n', g_log);
    }
    pthread_mutex_unlock(&g_lock);
}

static uint64_t hash_bytes(const char *data, size_t length) {
    uint64_t hash = 1469598103934665603ull;
    for (size_t index = 0; index < length; ++index) {
        hash ^= (unsigned char)data[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

static int contains(const char *haystack, const char *needle) {
    return haystack && needle && strstr(haystack, needle) != NULL;
}

static GLint current_program(void) {
    if (!real_get_integer) real_get_integer = (gl_get_integer_fn)next_symbol("glGetIntegerv");
    GLint value = -1;
    if (real_get_integer) real_get_integer(GL_CURRENT_PROGRAM, &value);
    return value;
}

static GLint current_vao(void) {
    if (!real_get_integer) real_get_integer = (gl_get_integer_fn)next_symbol("glGetIntegerv");
    GLint value = -1;
    if (real_get_integer) real_get_integer(0x85B5 /* GL_VERTEX_ARRAY_BINDING */, &value);
    return value;
}

static void log_new_draw(const char *kind, GLenum mode, GLint count, GLenum type) {
    GLint program = current_program();
    GLint vao = current_vao();
    uint64_t key = ((uint64_t)(uint32_t)program << 32) ^ ((uint64_t)(uint32_t)vao << 16)
                   ^ ((uint64_t)(uint16_t)count << 4) ^ (uint64_t)mode;
    for (unsigned long index = 0; index < g_unique_count; ++index) {
        if (g_unique[index] == key) return;
    }
    if (g_unique_count < sizeof g_unique / sizeof g_unique[0]) {
        g_unique[g_unique_count++] = key;
    }
    logf_line("NEWDRAW frame=%lu kind=%s mode=0x%x count=%d type=0x%x program=%d vao=%d",
              g_frames, kind, (unsigned)mode, (int)count, (unsigned)type, (int)program, (int)vao);
}

static void trace_shader_source(GLuint shader, GLsizei count, const GLchar *const *strings,
                                const GLint *lengths) {
    if (!real_shader_source) real_shader_source = (gl_shader_source_fn)next_symbol("glShaderSource");
    size_t total = 0;
    uint64_t hash = 1469598103934665603ull;
    int hand = 0, bone = 0, entity = 0, hand_vs = 0;
    for (GLsizei index = 0; index < count; ++index) {
        const char *text = strings ? strings[index] : NULL;
        if (!text) continue;
        size_t piece = lengths && lengths[index] >= 0 ? (size_t)lengths[index] : strlen(text);
        hash ^= hash_bytes(text, piece);
        total += piece;
        /* Inspect only the source's readable prefix; the tokens are ASCII. */
        hand |= contains(text, "hand");
        hand_vs |= contains(text, "HAND_VS");
        bone |= contains(text, "bone_transforms") || contains(text, "bone_matrices");
        entity |= contains(text, "entity_");
    }
    if (g_source_count < 128) {
        ++g_source_count;
        logf_line("SHADER id=%u bytes=%zu hash=%016llx hand=%d hand_vs=%d bone=%d entity=%d",
                  shader, total, (unsigned long long)hash, hand, hand_vs, bone, entity);
    }
    if (real_shader_source) real_shader_source(shader, count, strings, lengths);
}

static void trace_link(GLuint program) {
    if (!real_link) real_link = (gl_link_fn)next_symbol("glLinkProgram");
    logf_line("LINK program=%u", program);
    if (real_link) real_link(program);
}

static void trace_use(GLuint program) {
    if (!real_use) real_use = (gl_use_fn)next_symbol("glUseProgram");
    logf_line("USE program=%u", program);
    if (real_use) real_use(program);
}

static void trace_draw_elements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    if (!real_draw_elements) real_draw_elements = (gl_draw_elements_fn)next_symbol("glDrawElements");
    ++g_draws;
    log_new_draw("elements", mode, count, type);
    if (real_draw_elements) real_draw_elements(mode, count, type, indices);
}

static void trace_draw_arrays(GLenum mode, GLint first, GLsizei count) {
    if (!real_draw_arrays) real_draw_arrays = (gl_draw_arrays_fn)next_symbol("glDrawArrays");
    ++g_draws;
    log_new_draw("arrays", mode, count, 0);
    if (real_draw_arrays) real_draw_arrays(mode, first, count);
}

static void trace_draw_elements_instanced(GLenum mode, GLsizei count, GLenum type,
                                           const void *indices, GLsizei instance_count) {
    if (!real_draw_elements_instanced)
        real_draw_elements_instanced = (gl_draw_elements_instanced_fn)next_symbol("glDrawElementsInstanced");
    ++g_draws;
    log_new_draw("elements_instanced", mode, count, type);
    if (real_draw_elements_instanced)
        real_draw_elements_instanced(mode, count, type, indices, instance_count);
}

static void trace_draw_arrays_instanced(GLenum mode, GLint first, GLsizei count,
                                         GLsizei instance_count) {
    if (!real_draw_arrays_instanced)
        real_draw_arrays_instanced = (gl_draw_arrays_instanced_fn)next_symbol("glDrawArraysInstanced");
    ++g_draws;
    log_new_draw("arrays_instanced", mode, count, 0);
    if (real_draw_arrays_instanced) real_draw_arrays_instanced(mode, first, count, instance_count);
}

static void trace_bind_vao(GLuint vao) {
    if (!real_bind_vao) real_bind_vao = (gl_bind_vao_fn)next_symbol("glBindVertexArray");
    logf_line("VAO id=%u", vao);
    if (real_bind_vao) real_bind_vao(vao);
}

static void trace_buffer_data(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
    if (!real_buffer_data) real_buffer_data = (gl_buffer_data_fn)next_symbol("glBufferData");
    if (size >= 1024) logf_line("BUFFER target=0x%x bytes=%ld usage=0x%x data=%d",
                                (unsigned)target, (long)size, (unsigned)usage, data != NULL);
    if (real_buffer_data) real_buffer_data(target, size, data, usage);
}

static void trace_swap(Display *display, GLXDrawable drawable) {
    if (!real_get_integer) real_get_integer = (gl_get_integer_fn)next_symbol("glGetIntegerv");
    ++g_frames;
    logf_line("FRAME index=%lu draws_total=%lu", g_frames, g_draws);
    if (g_frames >= 240) g_disabled = 1; /* keep the capture bounded */
    static glx_swap_fn real_swap;
    if (!real_swap) real_swap = (glx_swap_fn)next_symbol("glXSwapBuffers");
    if (real_swap) real_swap(display, drawable);
    (void)drawable;
}

static __GLXextFuncPtr glx_proc(const GLubyte *name) {
    static glx_get_proc_fn real_get_proc;
    if (!real_get_proc) real_get_proc = (glx_get_proc_fn)next_symbol("glXGetProcAddress");
    if (!real_get_proc) real_get_proc = (glx_get_proc_fn)next_symbol("glXGetProcAddressARB");
    if (!name) return real_get_proc ? real_get_proc(name) : NULL;
    if (strcmp((const char *)name, "glShaderSource") == 0) return (__GLXextFuncPtr)trace_shader_source;
    if (strcmp((const char *)name, "glLinkProgram") == 0) return (__GLXextFuncPtr)trace_link;
    if (strcmp((const char *)name, "glUseProgram") == 0) return (__GLXextFuncPtr)trace_use;
    if (strcmp((const char *)name, "glDrawElements") == 0) return (__GLXextFuncPtr)trace_draw_elements;
    if (strcmp((const char *)name, "glDrawArrays") == 0) return (__GLXextFuncPtr)trace_draw_arrays;
    if (strcmp((const char *)name, "glDrawElementsInstanced") == 0)
        return (__GLXextFuncPtr)trace_draw_elements_instanced;
    if (strcmp((const char *)name, "glDrawArraysInstanced") == 0)
        return (__GLXextFuncPtr)trace_draw_arrays_instanced;
    if (strcmp((const char *)name, "glBindVertexArray") == 0) return (__GLXextFuncPtr)trace_bind_vao;
    if (strcmp((const char *)name, "glBufferData") == 0) return (__GLXextFuncPtr)trace_buffer_data;
    return real_get_proc ? real_get_proc(name) : NULL;
}

__GLXextFuncPtr glXGetProcAddress(const GLubyte *name) { return glx_proc(name); }
__GLXextFuncPtr glXGetProcAddressARB(const GLubyte *name) { return glx_proc(name); }
void glXSwapBuffers(Display *display, GLXDrawable drawable) { trace_swap(display, drawable); }

/* Direct symbols: Wine sometimes resolves these from libGL's export table
 * instead of asking glXGetProcAddress. */
void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths) {
    trace_shader_source(shader, count, strings, lengths);
}
void glLinkProgram(GLuint program) { trace_link(program); }
void glUseProgram(GLuint program) { trace_use(program); }
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    trace_draw_elements(mode, count, type, indices);
}
void glDrawArrays(GLenum mode, GLint first, GLsizei count) { trace_draw_arrays(mode, first, count); }
void glBindVertexArray(GLuint vao) { trace_bind_vao(vao); }
void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
    trace_buffer_data(target, size, data, usage);
}

__attribute__((constructor)) static void init_trace(void) {
    log_open();
    if (g_log) fprintf(g_log, "loaded host trace\n");
}
