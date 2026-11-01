/*
 * In-process OpenGL capture (research instrument, not part of the mod API).
 *
 * HYPER DEMON imports no GL entry points: GLFW loads opengl32.dll at run time and
 * every GL 2.0+ function above the 1.1 core comes back from wglGetProcAddress.
 * Both resolution paths therefore have to be filtered to see the renderer's real
 * entry points:
 *   - KERNEL32!GetProcAddress, in every module (GLFW lives inside the exe and
 *     resolves wglGetProcAddress through that slot);
 *   - opengl32!wglGetProcAddress, whose import slot GLFW calls directly.
 *
 * The interceptors hand back shims for a small set of functions and delegate
 * everything else untouched. No game code is patched and no static address is
 * used; when a symbol cannot be resolved the engine gets the real function and
 * the run behaves exactly as before.
 *
 * The log answers, in the order the title work needs the answers:
 *   - which shaders are compiled and their full source (title material/animation
 *     live in the engine's own shader text);
 *   - which uniforms each program exposes;
 *   - which textures are uploaded, at which size and format (font atlas vs art);
 *   - which (program, texture) pair every draw uses, so a visible draw maps back
 *     to a shader and an asset.
 *
 * Shader text is game content: the log stays beside the game and is never
 * committed to the repository.
 */
#define HYPRLODR_MINIMAL_WIN32 1

#include "hypr_win32.h"
#include "gl_hook.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HYPRLODR_MAX_HOOK_MODULES 128u
#define HYPRLODR_MAX_IMPORT_DESCRIPTORS 256u
#define HYPRLODR_MAX_IMPORT_SLOTS 4096u
#define HYPRLODR_MAX_LOG_BYTES (8u * 1024u * 1024u)
#define HYPRLODR_MAX_SHADER_BYTES 24000u
#define HYPRLODR_MAX_DRAW_RECORDS 800u
#define HYPRLODR_IMPORT_DESCRIPTOR_BYTES 20u
#define HYPRLODR_SECTION_HEADER_BYTES 40u

typedef unsigned int gl_enum;
typedef int gl_int;
typedef unsigned int gl_uint;
typedef int gl_sizei;
typedef unsigned char gl_ubyte;
typedef char gl_char;

#define GL_COMPILE_STATUS 0x8B81u
#define GL_LINK_STATUS 0x8B82u
#define GL_TEXTURE_2D 0x0DE1u
#define GL_TEXTURE0 0x84C0u
#define GL_VERTEX_SHADER 0x8B31u
#define GL_FRAGMENT_SHADER 0x8B30u

typedef FARPROC(WINAPI *get_proc_fn)(HMODULE, LPCSTR);
typedef void *(*wgl_get_proc_fn)(const char *);

static get_proc_fn g_original_get_proc;
static wgl_get_proc_fn g_original_wgl_get_proc;
static void *g_opengl_module;
static HANDLE g_log = INVALID_HANDLE_VALUE;
/* Frame dumps are written beside the game; the bootstrap sets this path. */
static char g_game_dir[1024] = "";
static DWORD g_start_tick;
static unsigned g_bytes_written;
static unsigned g_draw_records;
static int g_installed;
static int g_started;
static int g_complete;
static const char *g_status = "not_attempted";
static char g_detail[256] = "";
/* Driver-assigned ids; tracking the last binding attributes an upload or a draw
 * without querying GL from inside a shim. */
static gl_uint g_bound_texture;
static gl_uint g_current_program;

/* Which unit a binding belongs to, and how large each texture was uploaded. A
 * title draw is interesting for the textures it reads, so both are needed. */
#define HYPRLODR_TEXTURE_UNITS 8u
#define HYPRLODR_MAX_TRACKED_TEXTURES 256u
#define HYPRLODR_MAX_MARKED_SHADERS 512u
#define HYPRLODR_MAX_DRAW_UNMARKED 400u
#define HYPRLODR_MAX_DRAW_MARKED 200u

static gl_uint g_active_unit;
static gl_uint g_unit_texture[HYPRLODR_TEXTURE_UNITS];
static gl_uint g_program_ids[256];

typedef struct {
    gl_uint id;
    gl_int width;
    gl_int height;
    gl_enum format;
} tracked_texture;

static tracked_texture g_textures[HYPRLODR_MAX_TRACKED_TEXTURES];
static unsigned g_texture_count;
/* Shaders whose source carries the title's window constant, recorded at upload
 * time so a draw can be attributed without parsing the log afterwards. */
static gl_uint g_marked_shaders[HYPRLODR_MAX_MARKED_SHADERS];
static unsigned g_marked_shader_count;
static unsigned g_draw_unmarked;
static unsigned g_draw_marked;

/* ------------------------------------------------------------ small helpers */

static uint16_t read_u16(const uint8_t *bytes) {
    uint16_t value;
    memcpy(&value, bytes, sizeof value);
    return value;
}

static uint32_t read_u32(const uint8_t *bytes) {
    uint32_t value;
    memcpy(&value, bytes, sizeof value);
    return value;
}

static uint64_t read_u64(const uint8_t *bytes) {
    uint64_t value;
    memcpy(&value, bytes, sizeof value);
    return value;
}

static int equals(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        ++a;
        ++b;
    }
    return *a == *b;
}

static int equals_ignore_case(const char *a, const char *b) {
    while (*a && *b) {
        char left = *a, right = *b;
        if (left >= 'A' && left <= 'Z') left = (char)(left - 'A' + 'a');
        if (right >= 'A' && right <= 'Z') right = (char)(right - 'A' + 'a');
        if (left != right) return 0;
        ++a;
        ++b;
    }
    return *a == *b;
}

static int contains_ignore_case(const char *text, const char *needle) {
    size_t length = strlen(needle);
    if (length == 0) return 1;
    for (; *text; ++text) {
        size_t index = 0;
        while (index < length && text[index]) {
            char left = text[index], right = needle[index];
            if (left >= 'A' && left <= 'Z') left = (char)(left - 'A' + 'a');
            if (right >= 'A' && right <= 'Z') right = (char)(right - 'A' + 'a');
            if (left != right) break;
            ++index;
        }
        if (index == length) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- logging */

static void log_raw(const char *text, unsigned length) {
    if (g_log == INVALID_HANDLE_VALUE || !text || !length) return;
    if (g_bytes_written >= HYPRLODR_MAX_LOG_BYTES) return;
    DWORD written = 0;
    WriteFile(g_log, text, length, &written, NULL);
    g_bytes_written += written;
}

static void log_header(void) {
    char prefix[64];
    int length = snprintf(prefix, sizeof prefix, "{\"t_ms\":%lu,",
                          (unsigned long)(GetTickCount() - g_start_tick));
    if (length > 0)
        log_raw(prefix, (unsigned)(length < (int)sizeof prefix ? length : (int)sizeof prefix - 1));
}

static void log_body(const char *body) {
    log_header();
    log_raw(body, (unsigned)strlen(body));
    log_raw("}\n", 2);
}

/* JSON string escaping with truncation: a shader source must not be able to
 * overflow the buffer or the log. */
static unsigned escape_into(char *out, unsigned capacity, const char *text, unsigned length) {
    unsigned written = 0;
    for (unsigned index = 0; index < length && written + 8 < capacity; ++index) {
        unsigned char value = (unsigned char)text[index];
        if (value == '"' || value == '\\') {
            out[written++] = '\\';
            out[written++] = (char)value;
        } else if (value == '\n') {
            out[written++] = '\\';
            out[written++] = 'n';
        } else if (value == '\r') {
            out[written++] = '\\';
            out[written++] = 'r';
        } else if (value == '\t') {
            out[written++] = '\\';
            out[written++] = 't';
        } else if (value < 0x20) {
            written += (unsigned)snprintf(out + written, capacity - written, "\\u%04x", value);
        } else {
            out[written++] = (char)value;
        }
    }
    out[written] = '\0';
    return written;
}

/* Diagnostic: every GL name the engine resolves, and whether we had a shim for
 * it. If this list stays empty the caller is not going through either patched
 * path, which is a different problem from "the engine drew nothing". */
static unsigned g_lookup_records;

static void log_escaped_field(const char *prefix, const char *text);

static void log_lookup(const char *via, const char *name, int has_shim) {
    if (g_lookup_records >= 6000 || !name || !*name) return;
    int interesting = 0;
    if ((name[0] == 'g' || name[0] == 'G') && (name[1] == 'l' || name[1] == 'L')) interesting = 1;
    if ((name[0] == 'w' || name[0] == 'W') && (name[1] == 'g' || name[1] == 'G')) interesting = 1;
    if (!interesting) return;
    ++g_lookup_records;
    char prefix[128];
    snprintf(prefix, sizeof prefix, "\"event\":\"lookup\",\"via\":\"%s\",\"has_shim\":%s,\"name\":\"",
             via, has_shim ? "true" : "false");
    log_escaped_field(prefix, name);
}

static void log_escaped_field(const char *prefix, const char *text) {
    static char escaped[4096];
    unsigned length = escape_into(escaped, sizeof escaped, text, (unsigned)strlen(text));
    log_header();
    log_raw(prefix, (unsigned)strlen(prefix));
    log_raw(escaped, length);
    log_raw("\"}\n", 3);
}

/* ------------------------------------------------------------- PE patching */

typedef struct {
    const uint8_t *base;
    const uint8_t *sections;
    unsigned section_count;
    uint32_t import_rva;
} pe_layout;

static const uint8_t *rva_to_ptr(const pe_layout *layout, uint32_t rva) {
    for (unsigned index = 0; index < layout->section_count; ++index) {
        const uint8_t *header = layout->sections + (size_t)index * HYPRLODR_SECTION_HEADER_BYTES;
        uint32_t virtual_size = read_u32(header + 8);
        uint32_t virtual_address = read_u32(header + 12);
        uint32_t raw_size = read_u32(header + 16);
        uint32_t raw_pointer = read_u32(header + 20);
        uint32_t span = virtual_size ? virtual_size : raw_size;
        if (rva < virtual_address || rva >= virtual_address + span) continue;
        uint32_t delta = rva - virtual_address;
        if (delta >= raw_size) return NULL;
        return layout->base + raw_pointer + delta;
    }
    return NULL;
}

static int prepare_layout(const uint8_t *base, pe_layout *layout) {
    if (!base || base[0] != 'M' || base[1] != 'Z') return -1;
    uint32_t pe_offset = read_u32(base + 0x3c);
    if (pe_offset < 0x40u || pe_offset > 0x1000u) return -1;
    const uint8_t *pe = base + pe_offset;
    if (!(pe[0] == 'P' && pe[1] == 'E' && pe[2] == 0 && pe[3] == 0)) return -1;
    const uint8_t *coff = pe + 4;
    const uint8_t *optional = coff + HYPRLODR_IMPORT_DESCRIPTOR_BYTES;
    if (read_u16(optional) != 0x20bu) return -1;
    unsigned section_count = (unsigned)read_u16(coff + 2);
    uint16_t optional_size = read_u16(coff + 16);
    if (section_count == 0 || optional_size < 120u) return -1;
    uint32_t import_rva = read_u32(optional + 120);
    if (import_rva == 0) return -1;
    layout->base = base;
    layout->sections = optional + optional_size;
    layout->section_count = section_count;
    layout->import_rva = import_rva;
    return 0;
}

/* 1 = patched, 2 = already ours, 0 = absent, -1 = cannot write. */
static int patch_slot(void *slot, void *replacement, void **original) {
    if (read_u64((const uint8_t *)slot) == (uint64_t)(uintptr_t)replacement) return 2;
    if (original) {
        *original = (void *)(uintptr_t)read_u64((const uint8_t *)slot);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }
    DWORD previous = 0;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &previous)) return -1;
    memcpy(slot, &replacement, sizeof replacement);
    VirtualProtect(slot, sizeof(void *), previous, &previous);
    return 1;
}

/* --------------------------------------------------------------- shims */

static FARPROC WINAPI shim_get_proc(HMODULE module, LPCSTR name);
static void *shim_wgl_get_proc(const char *name);

static void *resolve_real(const char *name) {
    if (!g_opengl_module) g_opengl_module = (void *)GetModuleHandleA("opengl32.dll");
    if (g_original_wgl_get_proc) {
        void *address = g_original_wgl_get_proc(name);
        if (address) return address;
    }
    if (g_opengl_module && g_original_get_proc) {
        FARPROC address = g_original_get_proc((HMODULE)g_opengl_module, name);
        if (address) return (void *)(uintptr_t)address;
    }
    return NULL;
}

typedef struct {
    const char *name;
    void *shim;
    void *real;
} gl_entry;

static void *real_for(const char *name);
static void *shim_for(const char *name);

typedef gl_uint(WINAPI *create_shader_fn)(gl_enum);
typedef void(WINAPI *shader_source_fn)(gl_uint, gl_sizei, const gl_char *const *, const gl_int *);
typedef void(WINAPI *compile_shader_fn)(gl_uint);
typedef void(WINAPI *get_iv_fn)(gl_uint, gl_enum, gl_int *);
typedef void(WINAPI *get_info_log_fn)(gl_uint, gl_sizei, gl_sizei *, gl_char *);
typedef gl_uint(WINAPI *create_program_fn)(void);
typedef void(WINAPI *attach_fn)(gl_uint, gl_uint);
typedef void(WINAPI *link_fn)(gl_uint);
typedef void(WINAPI *get_active_uniform_fn)(gl_uint, gl_uint, gl_sizei, gl_sizei *, gl_int *,
                                            gl_enum *, gl_char *);
typedef void(WINAPI *bind_texture_fn)(gl_enum, gl_uint);
typedef void(WINAPI *active_texture_fn)(gl_enum);
typedef void(WINAPI *tex_image_fn)(gl_enum, gl_int, gl_int, gl_sizei, gl_sizei, gl_int, gl_enum,
                                   gl_enum, const void *);
typedef void(WINAPI *tex_sub_image_fn)(gl_enum, gl_int, gl_int, gl_int, gl_sizei, gl_sizei, gl_enum,
                                       gl_enum, const void *);
typedef void(WINAPI *use_program_fn)(gl_uint);
typedef void(WINAPI *draw_arrays_fn)(gl_enum, gl_int, gl_sizei);
typedef void(WINAPI *draw_elements_fn)(gl_enum, gl_sizei, gl_enum, const void *);
typedef void(WINAPI *viewport_fn)(gl_int, gl_int, gl_sizei, gl_sizei);

/* program -> attached shaders, so the log says which sources make a program. */
static gl_uint g_program_shaders[256 * 8];
static unsigned g_program_shader_counts[256];

/* A program is "marked" when one of its shaders carried the title's constant.
 * The stored id guards the slot against reuse by a different program. */
static int program_is_marked(gl_uint program) {
    unsigned slot = program & 0xffu;
    if (!program || g_program_ids[slot] != program) return 0;
    unsigned count = g_program_shader_counts[slot];
    for (unsigned index = 0; index < count; ++index) {
        gl_uint shader = g_program_shaders[slot * 8 + index];
        for (unsigned mark = 0; mark < g_marked_shader_count; ++mark)
            if (g_marked_shaders[mark] == shader) return 1;
    }
    return 0;
}

static const tracked_texture *texture_info(gl_uint id) {
    for (unsigned index = 0; index < g_texture_count; ++index)
        if (g_textures[index].id == id) return &g_textures[index];
    return NULL;
}

static gl_uint WINAPI shim_create_shader(gl_enum type) {
    create_shader_fn real = (create_shader_fn)real_for("glCreateShader");
    gl_uint shader = real ? real(type) : 0;
    char body[160];
    snprintf(body, sizeof body, "\"event\":\"create_shader\",\"shader\":%u,\"type\":\"%s\"", shader,
             type == GL_VERTEX_SHADER ? "vertex"
                                      : (type == GL_FRAGMENT_SHADER ? "fragment" : "other"));
    log_body(body);
    return shader;
}

static void WINAPI shim_shader_source(gl_uint shader, gl_sizei count, const gl_char *const *strings,
                                      const gl_int *lengths) {
    shader_source_fn real = (shader_source_fn)real_for("glShaderSource");
    static char raw[HYPRLODR_MAX_SHADER_BYTES];
    static char escaped[HYPRLODR_MAX_SHADER_BYTES + 512];
    static char prefix[128];
    unsigned total = 0;
    if (strings) {
        for (gl_sizei index = 0; index < count && total < HYPRLODR_MAX_SHADER_BYTES; ++index) {
            const char *text = strings[index];
            if (!text) continue;
            int declared = lengths ? lengths[index] : -1;
            unsigned piece = declared >= 0 ? (unsigned)declared : (unsigned)strlen(text);
            if (piece > HYPRLODR_MAX_SHADER_BYTES - total) piece = HYPRLODR_MAX_SHADER_BYTES - total;
            memcpy(raw + total, text, piece);
            total += piece;
        }
    }
    if (total < sizeof raw) raw[total] = '\0';
    else raw[sizeof raw - 1] = '\0';
    if (contains_ignore_case(raw, "854.0") &&
        g_marked_shader_count < HYPRLODR_MAX_MARKED_SHADERS) {
        g_marked_shaders[g_marked_shader_count++] = shader;
        char marked[128];
        snprintf(marked, sizeof marked,
                 "\"event\":\"mark\",\"shader\":%u,\"why\":\"854.0\",\"bytes\":%u", shader, total);
        log_body(marked);
    }
    unsigned escaped_length = escape_into(escaped, sizeof escaped, raw, total);
    int prefix_length = snprintf(prefix, sizeof prefix,
                                 "{\"event\":\"shader_source\",\"shader\":%u,\"bytes\":%u,\"source\":\"",
                                 shader, total);
    if (prefix_length > 0) {
        log_header();
        log_raw(prefix, (unsigned)(prefix_length < (int)sizeof prefix ? prefix_length : (int)sizeof prefix - 1));
        log_raw(escaped, escaped_length);
        log_raw("\"}\n", 3);
    }
    if (real) real(shader, count, strings, lengths);
}

static void WINAPI shim_compile_shader(gl_uint shader) {
    compile_shader_fn real = (compile_shader_fn)real_for("glCompileShader");
    if (real) real(shader);
    get_iv_fn get_iv = (get_iv_fn)real_for("glGetShaderiv");
    gl_int status = -1;
    if (get_iv) get_iv(shader, GL_COMPILE_STATUS, &status);
    char body[128];
    snprintf(body, sizeof body, "\"event\":\"compile\",\"shader\":%u,\"ok\":%s", shader,
             status == 1 ? "true" : (status < 0 ? "unknown" : "false"));
    log_body(body);
}

static void WINAPI shim_get_shader_iv(gl_uint shader, gl_enum name, gl_int *value) {
    get_iv_fn real = (get_iv_fn)real_for("glGetShaderiv");
    if (real) real(shader, name, value);
}

static void WINAPI shim_get_shader_info_log(gl_uint shader, gl_sizei capacity, gl_sizei *length,
                                            gl_char *log) {
    get_info_log_fn real = (get_info_log_fn)real_for("glGetShaderInfoLog");
    if (real) real(shader, capacity, length, log);
    if (!log || !*log) return;
    char prefix[96];
    snprintf(prefix, sizeof prefix, "\"event\":\"shader_log\",\"shader\":%u,\"text\":\"", shader);
    log_escaped_field(prefix, log);
}

static gl_uint WINAPI shim_create_program(void) {
    create_program_fn real = (create_program_fn)real_for("glCreateProgram");
    gl_uint program = real ? real() : 0;
    char body[96];
    snprintf(body, sizeof body, "\"event\":\"create_program\",\"program\":%u", program);
    log_body(body);
    return program;
}

static void WINAPI shim_attach_shader(gl_uint program, gl_uint shader) {
    attach_fn real = (attach_fn)real_for("glAttachShader");
    if (real) real(program, shader);
    unsigned slot = program & 0xffu;
    if (g_program_shader_counts[slot] < 8) {
        g_program_shaders[slot * 8 + g_program_shader_counts[slot]] = shader;
        ++g_program_shader_counts[slot];
    }
    char body[96];
    snprintf(body, sizeof body, "\"event\":\"attach\",\"program\":%u,\"shader\":%u", program, shader);
    log_body(body);
}

static void WINAPI shim_link_program(gl_uint program) {
    link_fn real = (link_fn)real_for("glLinkProgram");
    if (real) real(program);
    get_iv_fn get_iv = (get_iv_fn)real_for("glGetProgramiv");
    gl_int status = -1;
    if (get_iv) get_iv(program, GL_LINK_STATUS, &status);
    char body[128];
    snprintf(body, sizeof body, "\"event\":\"link\",\"program\":%u,\"ok\":%s", program,
             status == 1 ? "true" : (status < 0 ? "unknown" : "false"));
    log_body(body);
}

static void WINAPI shim_get_program_iv(gl_uint program, gl_enum name, gl_int *value) {
    get_iv_fn real = (get_iv_fn)real_for("glGetProgramiv");
    if (real) real(program, name, value);
}

static void WINAPI shim_get_program_info_log(gl_uint program, gl_sizei capacity, gl_sizei *length,
                                             gl_char *log) {
    get_info_log_fn real = (get_info_log_fn)real_for("glGetProgramInfoLog");
    if (real) real(program, capacity, length, log);
    if (!log || !*log) return;
    char prefix[96];
    snprintf(prefix, sizeof prefix, "\"event\":\"program_log\",\"program\":%u,\"text\":\"", program);
    log_escaped_field(prefix, log);
}

static void WINAPI shim_get_active_uniform(gl_uint program, gl_uint index, gl_sizei capacity,
                                           gl_sizei *length, gl_int *size, gl_enum *type,
                                           gl_char *name) {
    get_active_uniform_fn real = (get_active_uniform_fn)real_for("glGetActiveUniform");
    if (real) real(program, index, capacity, length, size, type, name);
    if (!name || !*name) return;
    char prefix[128];
    snprintf(prefix, sizeof prefix,
             "\"event\":\"uniform\",\"program\":%u,\"index\":%u,\"name\":\"", program, index);
    log_escaped_field(prefix, name);
}

static void WINAPI shim_active_texture(gl_enum unit) {
    active_texture_fn real = (active_texture_fn)real_for("glActiveTexture");
    if (real) real(unit);
    gl_uint index = (gl_uint)(unit - GL_TEXTURE0);
    g_active_unit = index < HYPRLODR_TEXTURE_UNITS ? index : 0u;
}

static void WINAPI shim_bind_texture(gl_enum target, gl_uint texture) {
    bind_texture_fn real = (bind_texture_fn)real_for("glBindTexture");
    if (target == GL_TEXTURE_2D) {
        g_bound_texture = texture;
        if (g_active_unit < HYPRLODR_TEXTURE_UNITS) g_unit_texture[g_active_unit] = texture;
    }
    if (real) real(target, texture);
}

static void WINAPI shim_tex_image_2d(gl_enum target, gl_int level, gl_int internal_format,
                                     gl_sizei width, gl_sizei height, gl_int border, gl_enum format,
                                     gl_enum type, const void *pixels) {
    tex_image_fn real = (tex_image_fn)real_for("glTexImage2D");
    if (real) real(target, level, internal_format, width, height, border, format, type, pixels);
    if (level > 0) return;
    char body[256];
    snprintf(body, sizeof body,
             "\"event\":\"texture\",\"texture\":%u,\"target\":%u,\"width\":%d,\"height\":%d,"
             "\"internal_format\":%d,\"format\":%u,\"type\":%u,\"uploaded\":%s",
             g_bound_texture, target, width, height, internal_format, format, type,
             pixels ? "true" : "false");
    log_body(body);
    if (g_texture_count < HYPRLODR_MAX_TRACKED_TEXTURES) {
        tracked_texture *info = &g_textures[g_texture_count++];
        info->id = g_bound_texture;
        info->width = width;
        info->height = height;
        info->format = format;
    }
}

static void WINAPI shim_tex_sub_image_2d(gl_enum target, gl_int level, gl_int x, gl_int y,
                                         gl_sizei width, gl_sizei height, gl_enum format,
                                         gl_enum type, const void *pixels) {
    tex_sub_image_fn real = (tex_sub_image_fn)real_for("glTexSubImage2D");
    if (real) real(target, level, x, y, width, height, format, type, pixels);
    if (level > 0 || width < 32 || height < 32) return;
    char body[256];
    snprintf(body, sizeof body,
             "\"event\":\"sub_texture\",\"texture\":%u,\"width\":%d,\"height\":%d,\"x\":%d,\"y\":%d,"
             "\"format\":%u,\"type\":%u",
             g_bound_texture, width, height, x, y, format, type);
    log_body(body);
}

static void WINAPI shim_use_program(gl_uint program) {
    use_program_fn real = (use_program_fn)real_for("glUseProgram");
    g_current_program = program;
    if (real) real(program);
}

/* Marked draws get their own allowance: the menu issues far more unmarked draws
 * per second than there are title draws in a session, so a shared cap would let
 * the title be crowded out of its own capture. */
static void log_draw(const char *kind, gl_enum mode, gl_int first, gl_sizei count) {
    int marked = program_is_marked(g_current_program);
    if (marked) {
        if (g_draw_marked >= HYPRLODR_MAX_DRAW_MARKED) return;
        ++g_draw_marked;
    } else {
        if (g_draw_unmarked >= HYPRLODR_MAX_DRAW_UNMARKED) return;
        ++g_draw_unmarked;
    }
    char body[768];
    int written = snprintf(body, sizeof body,
                           "\"event\":\"%s\",\"kind\":\"%s\",\"program\":%u,\"mode\":%u,"
                           "\"first\":%d,\"count\":%d,\"texture\":%u",
                           marked ? "draw_title" : "draw", kind, g_current_program, mode, first,
                           count, g_bound_texture);
    if (written < 0) return;
    if (written > (int)sizeof body - 1) written = (int)sizeof body - 1;
    if (marked) {
        for (gl_uint unit = 0; unit < HYPRLODR_TEXTURE_UNITS; ++unit) {
            gl_uint id = g_unit_texture[unit];
            const tracked_texture *info = texture_info(id);
            int room = (int)sizeof body - written;
            if (room < 64) break;
            int piece = snprintf(body + written, (size_t)room,
                                 ",\"unit%u\":{\"texture\":%u,\"width\":%d,\"height\":%d,\"format\":%u}",
                                 unit, id, info ? info->width : -1, info ? info->height : -1,
                                 info ? info->format : 0u);
            if (piece < 0) break;
            written += piece;
        }
    }
    log_body(body);
}

static void WINAPI shim_draw_arrays(gl_enum mode, gl_int first, gl_sizei count) {
    draw_arrays_fn real = (draw_arrays_fn)real_for("glDrawArrays");
    if (real) real(mode, first, count);
    if (g_draw_records >= HYPRLODR_MAX_DRAW_RECORDS) return;
    ++g_draw_records;
    log_draw("arrays", mode, first, count);
}

static void WINAPI shim_draw_elements(gl_enum mode, gl_sizei count, gl_enum type, const void *indices) {
    draw_elements_fn real = (draw_elements_fn)real_for("glDrawElements");
    if (real) real(mode, count, type, indices);
    if (g_draw_records >= HYPRLODR_MAX_DRAW_RECORDS) return;
    ++g_draw_records;
    log_draw("elements", mode, -1, count);
}

/* --- frame probe -----------------------------------------------------------
 * The engine resolves wglSwapBuffers through GetProcAddress, so a shim here is
 * called once per frame with a live context. That is enough to read the state
 * the renderer never exposes through names: the GL version/renderer, the
 * viewport, the program and texture bound by the last draw, and the pixels that
 * were just drawn. Everything below uses GL 1.1 entry points, which opengl32
 * exports directly, so no GL 2.0+ pointer is needed. */
typedef const unsigned char *(WINAPI *get_string_fn)(gl_enum);
typedef void(WINAPI *get_integerv_fn)(gl_enum, gl_int *);
typedef void(WINAPI *read_pixels_fn)(gl_int, gl_int, gl_sizei, gl_sizei, gl_enum, gl_enum, void *);
typedef BOOL(WINAPI *swap_buffers_fn)(void *);

#define GL_VENDOR 0x1F00u
#define GL_RENDERER 0x1F01u
#define GL_VERSION 0x1F02u
#define GL_VIEWPORT 0x0BA2u
#define GL_CURRENT_PROGRAM 0x8CA9u
#define GL_TEXTURE_BINDING_2D 0x8069u
#define GL_RGBA 0x1908u
#define GL_UNSIGNED_BYTE 0x1401u

static unsigned g_frames;

static void *gl11(const char *name) {
    if (!g_opengl_module) g_opengl_module = (void *)GetModuleHandleA("opengl32.dll");
    if (!g_opengl_module || !g_original_get_proc) return NULL;
    return (void *)(uintptr_t)g_original_get_proc((HMODULE)g_opengl_module, name);
}

static void write_bmp(const char *path, const unsigned char *rgba, int width, int height) {
    HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    int row = width * 3;
    int padding = (4 - (row % 4)) % 4;
    unsigned image_bytes = (unsigned)((row + padding) * height);
    unsigned char header[54];
    memset(header, 0, sizeof header);
    header[0] = 'B';
    header[1] = 'M';
    unsigned total = 54u + image_bytes;
    memcpy(header + 2, &total, 4);
    unsigned offset = 54;
    memcpy(header + 10, &offset, 4);
    unsigned info = 40;
    memcpy(header + 14, &info, 4);
    memcpy(header + 18, &width, 4);
    memcpy(header + 22, &height, 4);
    unsigned short planes = 1, bits = 24;
    memcpy(header + 26, &planes, 2);
    memcpy(header + 28, &bits, 2);
    memcpy(header + 34, &image_bytes, 4);
    DWORD written = 0;
    WriteFile(file, header, 54, &written, NULL);
    static unsigned char buffer[3 * 4096 + 8];
    for (int y = 0; y < height; ++y) {
        const unsigned char *source = rgba + (size_t)y * (size_t)width * 4u;
        unsigned length = 0;
        for (int x = 0; x < width; ++x) {
            buffer[length++] = source[x * 4 + 2];
            buffer[length++] = source[x * 4 + 1];
            buffer[length++] = source[x * 4 + 0];
        }
        for (int p = 0; p < padding; ++p) buffer[length++] = 0;
        WriteFile(file, buffer, length, &written, NULL);
    }
    CloseHandle(file);
}

/* --- texture readback ------------------------------------------------------
 * The engine imports no GL library and resolves no gl* name through either
 * patched loader path (measured), so per-call tracing cannot see its draw calls.
 * The swap hook it does go through is still enough: at swap time the frame is
 * finished, so binding each texture name and reading level 0 with GL 1.1
 * entry points observes what the renderer actually uploaded or rendered into.
 * Only observation is performed; the previous binding is restored. */
#define GL_TEXTURE_WIDTH 0x1000u
#define GL_TEXTURE_HEIGHT 0x1001u
#define HYPRLODR_MAX_TEX_DUMP_BYTES (48u * 1024u * 1024u)
#define HYPRLODR_TEX_SCAN_LIMIT 512u
#define HYPRLODR_TEX_MAX_SIDE 4096u

typedef void(WINAPI *get_tex_level_fn)(gl_enum, gl_int, gl_enum, gl_int *);
typedef void(WINAPI *get_tex_image_fn)(gl_enum, gl_int, gl_enum, gl_enum, void *);

static void dump_textures(int frame) {
    get_tex_level_fn get_level = (get_tex_level_fn)gl11("glGetTexLevelParameteriv");
    get_tex_image_fn get_image = (get_tex_image_fn)gl11("glGetTexImage");
    get_integerv_fn get_integerv = (get_integerv_fn)gl11("glGetIntegerv");
    bind_texture_fn bind = (bind_texture_fn)gl11("glBindTexture");
    if (!get_level || !get_image || !bind || !g_game_dir[0]) return;
    gl_int previous = 0;
    if (get_integerv) get_integerv(GL_TEXTURE_BINDING_2D, &previous);
    char path[1024];
    snprintf(path, sizeof path, "%s\\hyprlodr-tex-%d.bin", g_game_dir, frame);
    HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    HANDLE heap = GetProcessHeap();
    unsigned total = 0, kept = 0;
    DWORD written = 0;
    for (gl_uint id = 1; id <= HYPRLODR_TEX_SCAN_LIMIT && total < HYPRLODR_MAX_TEX_DUMP_BYTES; ++id) {
        bind(GL_TEXTURE_2D, id);
        gl_int width = 0, height = 0;
        get_level(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        get_level(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
        if (width < 64 || height < 32) continue;
        if ((gl_uint)width > HYPRLODR_TEX_MAX_SIDE || (gl_uint)height > HYPRLODR_TEX_MAX_SIDE) continue;
        size_t bytes = (size_t)width * (size_t)height * 4u;
        if (total + (unsigned)bytes > HYPRLODR_MAX_TEX_DUMP_BYTES) break;
        unsigned char *pixels = (unsigned char *)HeapAlloc(heap, 0, bytes);
        if (!pixels) continue;
        memset(pixels, 0, bytes);
        get_image(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        uint32_t record[3];
        record[0] = id;
        record[1] = (uint32_t)width;
        record[2] = (uint32_t)height;
        WriteFile(file, record, sizeof record, &written, NULL);
        WriteFile(file, pixels, (DWORD)bytes, &written, NULL);
        HeapFree(heap, 0, pixels);
        total += (unsigned)bytes;
        ++kept;
    }
    CloseHandle(file);
    bind(GL_TEXTURE_2D, (gl_uint)previous);
    char body[160];
    snprintf(body, sizeof body,
             "\"event\":\"tex_dump\",\"frame\":%d,\"textures\":%u,\"bytes\":%u", frame, kept,
             total);
    log_body(body);
}

static void dump_frame(unsigned frame, int width, int height) {
    if (!g_game_dir[0] || width <= 0 || height <= 0) return;
    read_pixels_fn read_pixels = (read_pixels_fn)gl11("glReadPixels");
    if (!read_pixels) return;
    HANDLE heap = GetProcessHeap();
    size_t bytes = (size_t)width * (size_t)height * 4u;
    unsigned char *pixels = (unsigned char *)HeapAlloc(heap, 0, bytes);
    if (!pixels) return;
    read_pixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    char path[1024];
    snprintf(path, sizeof path, "%s\\hyprlodr-frame-%u.bmp", g_game_dir, frame);
    write_bmp(path, pixels, width, height);
    HeapFree(heap, 0, pixels);
    char body[128];
    snprintf(body, sizeof body, "\"event\":\"frame_dump\",\"frame\":%u,\"ok\":true", frame);
    log_body(body);
}

/* --- title mask replacement -------------------------------------------------
 * Measured in-game: the title pass samples an 853x480 texture whose alpha holds
 * the letterforms and whose RGB is zero everywhere, and its fragment shader uses
 * (1.0 - alpha) times the background inside an 854x480 window. The engine
 * resolves GL invisibly, so the mask is supplied exactly the way the game
 * consumes it: by writing our glyph mask into that texture. The shader, the
 * background texture, the window maths and the whole post chain stay the game's
 * own. */
#define HYPRLODR_TITLE_MASK_WIDTH 853
#define HYPRLODR_TITLE_MASK_HEIGHT 480

static gl_uint g_title_mask_texture;
static int g_title_mask_state;

/* A title mask is identified by what it is, not by a driver-assigned name. The
 * name seen in one session was 81, but texture names are allocation counters: a
 * different settings combination or load order moves them. The signature is the
 * one measured from the live renderer - exactly 853x480, RGB zero everywhere,
 * and an alpha channel that is genuinely mixed. */
static int texture_is_title_mask(gl_uint id) {
    get_tex_level_fn get_level = (get_tex_level_fn)gl11("glGetTexLevelParameteriv");
    get_tex_image_fn get_image = (get_tex_image_fn)gl11("glGetTexImage");
    bind_texture_fn bind = (bind_texture_fn)gl11("glBindTexture");
    if (!get_level || !get_image || !bind) return 0;
    gl_int width = 0, height = 0;
    bind(GL_TEXTURE_2D, id);
    get_level(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    get_level(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    if (width != HYPRLODR_TITLE_MASK_WIDTH || height != HYPRLODR_TITLE_MASK_HEIGHT) return 0;
    size_t bytes = (size_t)width * (size_t)height * 4u;
    HANDLE heap = GetProcessHeap();
    unsigned char *pixels = (unsigned char *)HeapAlloc(heap, 0, bytes);
    if (!pixels) return 0;
    memset(pixels, 0, bytes);
    get_image(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    int colour_is_zero = 1;
    unsigned opaque = 0, clear = 0;
    for (size_t index = 0; index < bytes; index += 4u) {
        if (pixels[index] | pixels[index + 1] | pixels[index + 2]) {
            colour_is_zero = 0;
            break;
        }
        if (pixels[index + 3] == 0) ++clear;
        else if (pixels[index + 3] == 255) ++opaque;
    }
    HeapFree(heap, 0, pixels);
    if (!colour_is_zero) return 0;
    return clear > 0 && opaque > 0;
}

static gl_uint find_title_mask(void) {
    for (gl_uint id = 1; id <= HYPRLODR_TEX_SCAN_LIMIT; ++id)
        if (texture_is_title_mask(id)) return id;
    return 0;
}

static void apply_title_mask(void) {
    if (g_title_mask_state < 0 || !g_game_dir[0]) return;
    static unsigned char pixels[HYPRLODR_TITLE_MASK_WIDTH * HYPRLODR_TITLE_MASK_HEIGHT * 4];
    tex_sub_image_fn upload = (tex_sub_image_fn)gl11("glTexSubImage2D");
    bind_texture_fn bind = (bind_texture_fn)gl11("glBindTexture");
    get_integerv_fn get_integerv = (get_integerv_fn)gl11("glGetIntegerv");
    get_tex_level_fn get_level = (get_tex_level_fn)gl11("glGetTexLevelParameteriv");
    if (!upload || !bind || !get_integerv || !get_level) {
        g_title_mask_state = -1;
        return;
    }
    if (g_title_mask_state == 0) {
        char path[1024];
        snprintf(path, sizeof path, "%s\\hyprlodr-title-mask.raw", g_game_dir);
        HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) {
            g_title_mask_state = -1;
            return;
        }
        DWORD read = 0;
        ReadFile(file, pixels, (DWORD)sizeof pixels, &read, NULL);
        CloseHandle(file);
        if ((size_t)read != sizeof pixels) {
            g_title_mask_state = -1;
            log_body("\"event\":\"title_mask_bad_size\"");
            return;
        }
        g_title_mask_state = 1;
        log_body("\"event\":\"title_mask_loaded\"");
    }
    if (!g_title_mask_texture) {
        g_title_mask_texture = find_title_mask();
        if (!g_title_mask_texture) return;
        char found[128];
        snprintf(found, sizeof found, "\"event\":\"title_mask_found\",\"texture\":%u",
                 g_title_mask_texture);
        log_body(found);
    }
    gl_int previous = 0;
    get_integerv(GL_TEXTURE_BINDING_2D, &previous);
    bind(GL_TEXTURE_2D, g_title_mask_texture);
    upload(GL_TEXTURE_2D, 0, 0, 0, HYPRLODR_TITLE_MASK_WIDTH, HYPRLODR_TITLE_MASK_HEIGHT, GL_RGBA,
           GL_UNSIGNED_BYTE, pixels);
    bind(GL_TEXTURE_2D, (gl_uint)previous);
}

static BOOL WINAPI shim_swap_buffers(void *device) {
    swap_buffers_fn real = (swap_buffers_fn)real_for("wglSwapBuffers");
    ++g_frames;
    if (g_frames >= 20) apply_title_mask();
    if (g_frames == 1 || g_frames == 120) {
        get_string_fn get_string = (get_string_fn)gl11("glGetString");
        get_integerv_fn get_integerv = (get_integerv_fn)gl11("glGetIntegerv");
        const char *version = get_string ? (const char *)get_string(GL_VERSION) : NULL;
        const char *renderer = get_string ? (const char *)get_string(GL_RENDERER) : NULL;
        gl_int viewport[4] = {0, 0, 0, 0};
        gl_int program = -1, texture = -1;
        if (get_integerv) {
            get_integerv(GL_VIEWPORT, viewport);
            get_integerv(GL_CURRENT_PROGRAM, &program);
            get_integerv(GL_TEXTURE_BINDING_2D, &texture);
        }
        char body[320];
        snprintf(body, sizeof body,
                 "\"event\":\"frame\",\"frame\":%u,\"width\":%d,\"height\":%d,\"program\":%d,"
                 "\"texture\":%d,\"version\":\"%s\",\"renderer\":\"%s\"",
                 g_frames, viewport[2], viewport[3], program, texture, version ? version : "?",
                 renderer ? renderer : "?");
        log_body(body);
    }
    if (g_frames >= 120 && g_frames <= 720 && (g_frames % 180u) == 0u)
        dump_textures((int)g_frames);
    if (g_frames == 120 || g_frames == 300 || g_frames == 480) {
        get_integerv_fn get_integerv = (get_integerv_fn)gl11("glGetIntegerv");
        gl_int viewport[4] = {0, 0, 0, 0};
        if (get_integerv) get_integerv(GL_VIEWPORT, viewport);
        dump_frame(g_frames, viewport[2], viewport[3]);
    }
    if (real) return real(device);
    return 1;
}

static void WINAPI shim_viewport(gl_int x, gl_int y, gl_sizei width, gl_sizei height) {
    viewport_fn real = (viewport_fn)real_for("glViewport");
    if (real) real(x, y, width, height);
    char body[160];
    snprintf(body, sizeof body, "\"event\":\"viewport\",\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d",
             x, y, width, height);
    log_body(body);
}

static gl_entry g_entries[] = {
    {"glCreateShader", (void *)(uintptr_t)&shim_create_shader, NULL},
    {"glShaderSource", (void *)(uintptr_t)&shim_shader_source, NULL},
    {"glCompileShader", (void *)(uintptr_t)&shim_compile_shader, NULL},
    {"glGetShaderiv", (void *)(uintptr_t)&shim_get_shader_iv, NULL},
    {"glGetShaderInfoLog", (void *)(uintptr_t)&shim_get_shader_info_log, NULL},
    {"glCreateProgram", (void *)(uintptr_t)&shim_create_program, NULL},
    {"glAttachShader", (void *)(uintptr_t)&shim_attach_shader, NULL},
    {"glLinkProgram", (void *)(uintptr_t)&shim_link_program, NULL},
    {"glGetProgramiv", (void *)(uintptr_t)&shim_get_program_iv, NULL},
    {"glGetProgramInfoLog", (void *)(uintptr_t)&shim_get_program_info_log, NULL},
    {"glGetActiveUniform", (void *)(uintptr_t)&shim_get_active_uniform, NULL},
    {"glActiveTexture", (void *)(uintptr_t)&shim_active_texture, NULL},
    {"glBindTexture", (void *)(uintptr_t)&shim_bind_texture, NULL},
    {"glTexImage2D", (void *)(uintptr_t)&shim_tex_image_2d, NULL},
    {"glTexSubImage2D", (void *)(uintptr_t)&shim_tex_sub_image_2d, NULL},
    {"glUseProgram", (void *)(uintptr_t)&shim_use_program, NULL},
    {"glDrawArrays", (void *)(uintptr_t)&shim_draw_arrays, NULL},
    {"glDrawElements", (void *)(uintptr_t)&shim_draw_elements, NULL},
    {"glViewport", (void *)(uintptr_t)&shim_viewport, NULL},
    {"wglSwapBuffers", (void *)(uintptr_t)&shim_swap_buffers, NULL},
};

static void *real_for(const char *name) {
    size_t count = sizeof g_entries / sizeof g_entries[0];
    for (size_t index = 0; index < count; ++index) {
        gl_entry *entry = &g_entries[index];
        if (!equals(entry->name, name)) continue;
        if (!entry->real) entry->real = resolve_real(name);
        return entry->real;
    }
    return NULL;
}

static void *shim_for(const char *name) {
    size_t count = sizeof g_entries / sizeof g_entries[0];
    for (size_t index = 0; index < count; ++index)
        if (equals(g_entries[index].name, name)) return g_entries[index].shim;
    return NULL;
}

/* Both resolution paths land here. Only names we have a real pointer for are
 * replaced; a function we could not resolve is handed back untouched, so the
 * game never receives a shim that would have to fail. */
static FARPROC WINAPI shim_get_proc(HMODULE module, LPCSTR name) {
    if (name) {
        log_lookup("getproc", name, shim_for(name) != NULL);
        if (equals_ignore_case(name, "GetProcAddress"))
            return (FARPROC)(uintptr_t)&shim_get_proc;
        if (equals_ignore_case(name, "wglGetProcAddress")) {
            if (!g_original_wgl_get_proc) {
                FARPROC real = g_original_get_proc ? g_original_get_proc(module, name) : NULL;
                if (real) {
                    g_original_wgl_get_proc = (wgl_get_proc_fn)(uintptr_t)real;
                    log_body("\"event\":\"wgl_bind\",\"source\":\"getproc\",\"ok\":true");
                    return (FARPROC)(uintptr_t)&shim_wgl_get_proc;
                }
            }
            log_body("\"event\":\"wgl_bind\",\"source\":\"getproc\",\"ok\":false");
        }
        if (shim_for(name) && real_for(name)) return (FARPROC)(uintptr_t)shim_for(name);
    }
    if (!g_original_get_proc) return NULL;
    return g_original_get_proc(module, name);
}

static void *shim_wgl_get_proc(const char *name) {
    if (!name) return NULL;
    log_lookup("wgl", name, shim_for(name) != NULL);
    if (shim_for(name) && real_for(name)) return shim_for(name);
    if (g_original_wgl_get_proc) return g_original_wgl_get_proc(name);
    return NULL;
}

/* ------------------------------------------------------------- install */

/* Wine and Proton commonly delay-load opengl32.dll, and the delay helper writes
 * the resolved address straight into the slot, overwriting a shim installed at
 * load time. A short watcher re-asserts the patches during start-up, which is
 * when GLFW resolves its entry points, and records every re-assertion so the
 * clobbering is visible rather than guessed. */
static unsigned patch_all_modules(unsigned *module_count);

static DWORD WINAPI gl_watch(LPVOID unused) {
    (void)unused;
    for (int round = 0; round < 80; ++round) {
        Sleep(250);
        unsigned modules = 0;
        unsigned patched = patch_all_modules(&modules);
        if (patched) {
            char body[160];
            snprintf(body, sizeof body, "\"event\":\"repatch\",\"slots\":%u,\"modules\":%u",
                     patched, modules);
            log_body(body);
        }
    }
    return 0;
}

static void log_delay_imports(const pe_layout *layout, const uint8_t *base) {
    const uint8_t *pe = base + read_u32(base + 0x3c);
    const uint8_t *optional = pe + 4 + HYPRLODR_IMPORT_DESCRIPTOR_BYTES;
    uint32_t delay_rva = read_u32(optional + 112 + 13 * 8);
    char body[96];
    snprintf(body, sizeof body, "\"event\":\"delay_imports\",\"rva\":%u", delay_rva);
    log_body(body);
    (void)layout;
}

/* The engine resolves no GL name through either patched path (measured: only
 * wglSwapBuffers was ever looked up, yet draws happened), which means opengl32 is
 * linked in and its GL entry points arrive through the import table. Patching
 * those slots is what makes shader and draw recording possible at all. */
static unsigned patch_direct_gl_imports(const pe_layout *layout) {
    const uint8_t *descriptor = rva_to_ptr(layout, layout->import_rva);
    if (!descriptor) return 0;
    unsigned changed = 0;
    for (unsigned index = 0; index < HYPRLODR_MAX_IMPORT_DESCRIPTORS; ++index) {
        const uint8_t *entry = descriptor + (size_t)index * HYPRLODR_IMPORT_DESCRIPTOR_BYTES;
        uint32_t name_rva = read_u32(entry + 12);
        uint32_t thunk_rva = read_u32(entry + 16);
        uint32_t names_rva = read_u32(entry);
        if (name_rva == 0) break;
        const char *dll = (const char *)rva_to_ptr(layout, name_rva);
        if (!dll || !contains_ignore_case(dll, "opengl32")) continue;
        if (thunk_rva == 0 || names_rva == 0) break;
        uint8_t *thunk = (uint8_t *)rva_to_ptr(layout, thunk_rva);
        const uint8_t *names = rva_to_ptr(layout, names_rva);
        if (!thunk || !names) break;
        for (unsigned slot_index = 0; slot_index < HYPRLODR_MAX_IMPORT_SLOTS; ++slot_index) {
            uint64_t name_value = read_u64(names + (size_t)slot_index * 8u);
            if (name_value == 0) break;
            if ((name_value >> 63) != 0) continue; /* import by ordinal */
            const uint8_t *by_name = rva_to_ptr(layout, (uint32_t)(name_value & 0x7fffffffu));
            if (!by_name) continue;
            const char *symbol = (const char *)(by_name + 2);
            void *shim = shim_for(symbol);
            if (!shim || !real_for(symbol)) continue;
            int result = patch_slot(thunk + (size_t)slot_index * 8u, shim, NULL);
            if (result == 1) {
                ++changed;
                char body[128];
                snprintf(body, sizeof body, "\"event\":\"direct_import\",\"name\":\"%s\"", symbol);
                log_body(body);
            }
        }
        break; /* a module imports opengl32 at most once */
    }
    return changed;
}

/* Patch every import slot for `symbol`, whichever DLL it was imported from.
 * Proton routes loader calls through kernelbase.dll, so filtering on "kernel32"
 * misses exactly the slot a loader resolves its GL entry points through. */
static unsigned patch_symbol_any_dll(const pe_layout *layout, const char *symbol, void *replacement,
                                     void **original) {
    const uint8_t *descriptor = rva_to_ptr(layout, layout->import_rva);
    if (!descriptor) return 0;
    unsigned patched = 0;
    void *first_original = NULL;
    for (unsigned index = 0; index < HYPRLODR_MAX_IMPORT_DESCRIPTORS; ++index) {
        const uint8_t *entry = descriptor + (size_t)index * HYPRLODR_IMPORT_DESCRIPTOR_BYTES;
        uint32_t name_rva = read_u32(entry + 12);
        uint32_t thunk_rva = read_u32(entry + 16);
        uint32_t names_rva = read_u32(entry);
        if (name_rva == 0) break;
        if (thunk_rva == 0 || names_rva == 0) continue;
        uint8_t *thunk = (uint8_t *)rva_to_ptr(layout, thunk_rva);
        const uint8_t *names = rva_to_ptr(layout, names_rva);
        if (!thunk || !names) continue;
        for (unsigned slot_index = 0; slot_index < HYPRLODR_MAX_IMPORT_SLOTS; ++slot_index) {
            uint64_t address = read_u64(thunk + (size_t)slot_index * 8u);
            uint64_t name_value = read_u64(names + (size_t)slot_index * 8u);
            if (address == 0 || name_value == 0) break;
            if ((name_value >> 63) != 0) continue; /* import by ordinal */
            const uint8_t *by_name = rva_to_ptr(layout, (uint32_t)(name_value & 0x7fffffffu));
            if (!by_name) continue;
            if (!equals_ignore_case((const char *)(by_name + 2), symbol)) continue;
            void *previous = NULL;
            int result = patch_slot(thunk + (size_t)slot_index * 8u, replacement, &previous);
            if (result == -1) continue;
            if (result == 1 || result == 2) ++patched;
            if (result == 1 && !first_original && previous && previous != replacement)
                first_original = previous;
        }
    }
    if (original && first_original && !*original) *original = first_original;
    return patched;
}

static unsigned patch_targets_in(const pe_layout *layout) {
    unsigned patched = 0;
    patched += patch_symbol_any_dll(layout, "GetProcAddress", (void *)(uintptr_t)&shim_get_proc,
                                    (void **)&g_original_get_proc);
    patched += patch_symbol_any_dll(layout, "wglGetProcAddress",
                                    (void *)(uintptr_t)&shim_wgl_get_proc,
                                    (void **)&g_original_wgl_get_proc);
    patched += patch_direct_gl_imports(layout);
    return patched;
}

static unsigned patch_all_modules(unsigned *module_count) {
    unsigned modules = 0, patched = 0;
    HANDLE snapshot =
        CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    const uint8_t *self = (const uint8_t *)(uintptr_t)&g_entries;
    MODULEENTRY32A entry;
    memset(&entry, 0, sizeof entry);
    entry.dwSize = (DWORD)sizeof entry;
    for (BOOL more = Module32First(snapshot, &entry); more; more = Module32Next(snapshot, &entry)) {
        if (modules >= HYPRLODR_MAX_HOOK_MODULES) break;
        const uint8_t *base = (const uint8_t *)entry.modBaseAddr;
        if (self >= base && self < base + entry.modBaseSize) continue;
        pe_layout layout;
        if (prepare_layout(base, &layout) != 0) continue;
        ++modules;
        patched += patch_targets_in(&layout);
    }
    CloseHandle(snapshot);
    if (module_count) *module_count = modules;
    return patched;
}

int hyprlodr_gl_hook_install_early(const char *log_path) {
    if (g_started) return g_installed ? 0 : -1;
    g_started = 1;
    g_start_tick = GetTickCount();
    if (!log_path || !*log_path) {
        g_status = "no_log_path";
        return -1;
    }
    g_log = CreateFileA(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log == INVALID_HANDLE_VALUE) {
        g_status = "log_open_failed";
        return -1;
    }
    pe_layout layout;
    if (prepare_layout((const uint8_t *)GetModuleHandleA(NULL), &layout) != 0) {
        g_status = "main_module_unparsed";
        return -1;
    }
    log_delay_imports(&layout, layout.base);
    unsigned patched = patch_targets_in(&layout);
    g_installed = patched > 0;
    snprintf(g_detail, sizeof g_detail, "early_slots=%u", patched);
    g_status = g_installed ? "installed_early" : "early_no_slot";
    return g_installed ? 0 : -1;
}

int hyprlodr_gl_hook_install(const char *log_path) {
    if (g_complete) return 0;
    if (!g_started && hyprlodr_gl_hook_install_early(log_path) != 0) return -1;
    unsigned modules = 0;
    unsigned patched = patch_all_modules(&modules);
    snprintf(g_detail, sizeof g_detail, "GetProcAddress+wglGetProcAddress slots=%u modules=%u",
             patched, modules);
    g_complete = 1;
    g_status = patched ? "installed" : "installed_no_slot";
    log_body("\"event\":\"capture_ready\"");
    CreateThread(NULL, 0, gl_watch, NULL, 0, NULL);
    return patched ? 0 : -1;
}

void hyprlodr_gl_hook_set_game_dir(const char *game_dir) {
    if (!game_dir) return;
    snprintf(g_game_dir, sizeof g_game_dir, "%s", game_dir);
}

const char *hyprlodr_gl_hook_status(void) { return g_status; }

const char *hyprlodr_gl_hook_detail(void) { return g_detail; }
