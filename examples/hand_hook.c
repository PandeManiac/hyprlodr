/*
 * HYPRLODR hand hook (example.hand-hook).
 *
 * The engine does not resolve glDrawElements through an import slot. It calls
 * through its own dispatch table, an array of driver pointers, at RVA 0x3d9ac8.
 * This mod saves the pointer that is there, replaces it with a shim, and calls
 * the original for every draw. Two adjacent 1,188-index draws are the
 * first-person arm (docs/RE_HAND.md); the shim recognises them by count, type
 * and mode.
 *
 * Modes, read once from <state_dir>/hand-mode.txt:
 *   "suppress"  draw the hand passes with count zero
 *   "cube"      draw a solid test cube through the same program instead
 *   "arm"       draw the authored Berdly arm from <state_dir>/hand-mesh.bin
 *   anything else (or no file)  pass every draw through unchanged
 *
 * Cube mode is the geometry-replacement proof. The first hand draw creates a
 * private VAO/VBO/IBO that mirrors the hand's attribute layout (six float
 * attributes, stride 72, offsets 0/12/24/32/48/60) and holds a 24-vertex cube
 * with 36 GL_UNSIGNED_INT indices. Both owned draws bind that VAO, draw, and
 * restore the engine's VAO. The engine's own buffers are never read or
 * written, so there is nothing to restore and no readback stall.
 *
 * The shim runs on the render thread inside the engine's draw call, so it does
 * not call the host logger or any loader code. An earlier version did and the
 * game died on the first gameplay hand draw. Only GL calls are made there.
 *
 * This example is Windows-only: it needs the live game image and the host's
 * ABI 6 write gate. On a host without a verified image it initializes inert.
 */
#include "hyprlodr/plugin.h"
#include "hypr_win32.h"

#include <stdio.h>
#include <string.h>

/* The Windows target links without a CRT; floating point code references this
 * marker, which the normal CRT would define. */
int _fltused = 0;

#define MOD_ID "example.hand-hook"
#define MOD_VERSION "0.5.0"

/* The engine's draw-table slot, from docs/RE_HAND.md. The host only lets a mod
 * touch the image when build_matches() is true, and this value is tied to the
 * fingerprinted build. */
#define HAND_DISPATCH_RVA 0x3d9ac8ull

#define HAND_INDEX_COUNT 1188
#define CUBE_INDEX_COUNT 36
#define CUBE_VERTEX_COUNT 24
#define CUBE_VBO_BYTES (CUBE_VERTEX_COUNT * 72)
#define CUBE_IBO_BYTES (CUBE_INDEX_COUNT * 4)

#define ARM_HEADER_BYTES 48
#define ARM_MAX_FILE_BYTES (4u * 1024u * 1024u)

#define GL_TRIANGLES 4u
#define GL_FLOAT 0x1406u
/* The live draw declares 5125 = GL_UNSIGNED_INT, and the 4,752-byte element
 * buffer is one run of 1,188 32-bit indices (not two 16-bit runs). */
#define GL_UNSIGNED_INT 0x1405u
#define GL_UNSIGNED_SHORT 0x1403u
#define GL_ARRAY_BUFFER 0x8892u
#define GL_ELEMENT_ARRAY_BUFFER 0x8893u
#define GL_VERTEX_ARRAY_BINDING 0x85B5u

#define GL_TEXTURE_2D 0x0DE1u
#define GL_TEXTURE0 0x84C0u
#define GL_TEXTURE_BINDING_2D 0x8069u
#define GL_ACTIVE_TEXTURE 0x84E0u
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS 0x8B4Du
#define GL_RGBA 0x1908u
#define GL_RGBA8 0x8058u
#define GL_UNSIGNED_BYTE 0x1401u
#define GL_LINEAR 0x2601u
#define GL_TEXTURE_MIN_FILTER 0x2801u
#define GL_TEXTURE_MAG_FILTER 0x2800u
#define GL_TEXTURE_WRAP_S 0x2802u
#define GL_TEXTURE_WRAP_T 0x2803u
#define GL_REPEAT 0x2901u
#define GL_CLAMP_TO_EDGE 0x812Fu
#define BLUE_TEXTURE_SIZE 256

typedef void (WINAPI *draw_elements_fn)(unsigned mode, int count, unsigned type,
                                        const void *indices);
typedef void (WINAPI *bind_buffer_fn)(unsigned target, unsigned buffer);
typedef void (WINAPI *buffer_data_fn)(unsigned target, long long size, const void *data,
                                      unsigned usage);
typedef void (WINAPI *gen_buffers_fn)(int count, unsigned *buffers);
typedef void (WINAPI *gen_vertex_arrays_fn)(int count, unsigned *arrays);
typedef void (WINAPI *bind_vertex_array_fn)(unsigned array);
typedef void (WINAPI *vertex_attrib_pointer_fn)(unsigned index, int size, unsigned type,
                                               unsigned char normalized, int stride,
                                               const void *pointer);
typedef void (WINAPI *enable_vertex_attrib_array_fn)(unsigned index);
typedef void (WINAPI *get_integerv_fn)(unsigned pname, int *value);
typedef int (WINAPI *get_attrib_location_fn)(unsigned program, const char *name);
typedef unsigned (WINAPI *get_error_fn)(void);
typedef unsigned char (WINAPI *is_enabled_fn)(unsigned cap);
typedef void (WINAPI *enable_fn)(unsigned cap);
typedef void (WINAPI *disable_fn)(unsigned cap);
typedef void (WINAPI *gen_textures_fn)(int count, unsigned *textures);
typedef void (WINAPI *bind_texture_fn)(unsigned target, unsigned texture);
typedef void (WINAPI *tex_image_2d_fn)(unsigned target, int level, int internal_format,
                                       int width, int height, int border, unsigned format,
                                       unsigned type, const void *pixels);
typedef void (WINAPI *tex_parameteri_fn)(unsigned target, unsigned pname, int value);
typedef void (WINAPI *active_texture_fn)(unsigned texture);
typedef int (WINAPI *get_uniform_location_fn)(unsigned program, const char *name);
typedef void (WINAPI *get_uniformiv_fn)(unsigned program, int location, int *values);
typedef void *(*wgl_get_proc_fn)(const char *name);

static const hyprlodr_host_api *g_host;
static draw_elements_fn g_original_draw;
static bind_buffer_fn g_bind_buffer;
static buffer_data_fn g_buffer_data;
static gen_buffers_fn g_gen_buffers;
static gen_vertex_arrays_fn g_gen_vertex_arrays;
static bind_vertex_array_fn g_bind_vertex_array;
static vertex_attrib_pointer_fn g_vertex_attrib_pointer;
static enable_vertex_attrib_array_fn g_enable_vertex_attrib_array;
static get_integerv_fn g_get_integerv;
static get_attrib_location_fn g_get_attrib_location;
static get_error_fn g_get_error;
static is_enabled_fn g_is_enabled;
static enable_fn g_enable;
static disable_fn g_disable;
static gen_textures_fn g_gen_textures;
static bind_texture_fn g_bind_texture;
static tex_image_2d_fn g_tex_image_2d;
static tex_parameteri_fn g_tex_parameteri;
static active_texture_fn g_active_texture;
static get_uniform_location_fn g_get_uniform_location;
static get_uniformiv_fn g_get_uniformiv;

static unsigned g_mode; /* 0 pass-through, 1 suppress, 2 cube, 3 arm */
static unsigned long g_hand_draws;
static unsigned long g_substituted;
static int g_active;

/* Private geometry. 0 = not prepared, 1 = ready, -1 = unavailable. */
static int g_geometry_state;
static unsigned g_vao;
static unsigned char g_cube_vbo[CUBE_VBO_BYTES];
static unsigned int g_cube_ibo[CUBE_INDEX_COUNT];

/* The arm's binary is installed beside the mode file. It is read once, before
 * the VAO is built, and copied into GL buffers immediately. */
static unsigned char g_arm_file[ARM_MAX_FILE_BYTES];
static unsigned g_arm_vertex_count;
static unsigned g_arm_index_count;
static unsigned g_geometry_count;

/* Our own blue feather texture, uploaded once with the arm VAO. The game's
 * program and material stay bound; only the diffuse sampler is redirected for
 * the two substituted draws, then every piece of state is restored. */
static unsigned char g_blue_pixels[BLUE_TEXTURE_SIZE * BLUE_TEXTURE_SIZE * 4];
static unsigned g_blue_texture;

/* The two hand passes use different programs but share the same texture object.
 * Each program's existing tex_diffuse unit is read once and cached. */
typedef struct material_state {
    unsigned program;
    int diffuse_sampler;
    int texture_unit;
} material_state;
static material_state g_materials[2];
static unsigned g_material_count;
static int g_texture_ready;

/* Read-only diagnostics for a debugger; never part of the mod API. */
HYPR_EXPORT volatile unsigned long long hypr_hand_debug[40];

static void log_message(const char *level, const char *message) {
    if (g_host && g_host->log) g_host->log(g_host->context, level, MOD_ID, message);
}

static const char *state_path(const hyprlodr_host_api *host, const char *name,
                              char *buffer, size_t capacity) {
    if (!HYPRLODR_HOST_API_HAS(host, state_dir) || !host->state_dir) return NULL;
    const char *directory = host->state_dir(host->context);
    if (!directory || !*directory) return NULL;
    int written = snprintf(buffer, capacity, "%s/%s", directory, name);
    if (written < 0 || (size_t)written >= capacity) return NULL;
    return buffer;
}

/* The minimal Windows C runtime has no memcmp, so compare prefixes directly. */
static int has_prefix(const char *text, const char *prefix) {
    while (*prefix) {
        if (*text++ != *prefix++) return 0;
    }
    return 1;
}

/* Read the mode before anything is patched. Parsed by hand rather than with a
 * scanf the minimal Windows runtime does not provide. */
static unsigned read_mode(const hyprlodr_host_api *host) {
    char path[1024];
    if (!state_path(host, "hand-mode.txt", path, sizeof path)) return 0;
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    char word[32];
    unsigned mode = 0;
    if (fgets(word, (int)sizeof word, file)) {
        if (has_prefix(word, "suppress")) mode = 1;
        else if (has_prefix(word, "cube")) mode = 2;
        else if (has_prefix(word, "arm")) mode = 3;
    }
    fclose(file);
    return mode;
}

static const char *mode_name(unsigned mode) {
    if (mode == 1) return "suppress";
    if (mode == 2) return "cube";
    if (mode == 3) return "arm";
    return "pass-through";
}

/* ------------------------------------------------------- test cube geometry */

static void put_f32(unsigned char *where, float value) {
    memcpy(where, &value, sizeof value);
}

static void write_vertex(unsigned index, const float position[3], const float normal[3],
                         const float uv[2], const float uv1[4], const float tangent[3],
                         const float bitangent[3]) {
    unsigned char *record = g_cube_vbo + (size_t)index * 72;
    put_f32(record + 0, position[0]);
    put_f32(record + 4, position[1]);
    put_f32(record + 8, position[2]);
    put_f32(record + 12, normal[0]);
    put_f32(record + 16, normal[1]);
    put_f32(record + 20, normal[2]);
    put_f32(record + 24, uv[0]);
    put_f32(record + 28, uv[1]);
    put_f32(record + 32, uv1[0]);
    put_f32(record + 36, uv1[1]);
    put_f32(record + 40, uv1[2]);
    put_f32(record + 44, uv1[3]);
    put_f32(record + 48, tangent[0]);
    put_f32(record + 52, tangent[1]);
    put_f32(record + 56, tangent[2]);
    put_f32(record + 60, bitangent[0]);
    put_f32(record + 64, bitangent[1]);
    put_f32(record + 68, bitangent[2]);
}

/* A solid, flat-shaded cube placed at the hand's model-space centre, lifted so
 * it sits inside the view instead of at the bottom edge. The extra vec4 is
 * copied from the source mesh so the material's non-UV attribute keeps a sane
 * value. */
static void build_cube(void) {
    static const float centre[3] = {-0.0281f, 0.0000f, -0.0222f};
    static const float half = 0.120f;
    static const float uv1[4] = {0.023678f, 0.092198f, 0.0f, 0.0f};
    static const float normals[6][3] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
    };
    static const float tangents[6][3] = {
        {0, 1, 0}, {0, 0, 1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {0, 1, 0},
    };
    static const float bitangents[6][3] = {
        {0, 0, 1}, {0, 1, 0}, {1, 0, 0}, {0, 0, 1}, {0, 1, 0}, {1, 0, 0},
    };
    static const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};

    for (unsigned face = 0; face < 6; ++face) {
        for (unsigned corner = 0; corner < 4; ++corner) {
            float u = corners[corner][0];
            float v = corners[corner][1];
            float position[3];
            for (unsigned axis = 0; axis < 3; ++axis) {
                position[axis] = centre[axis] + normals[face][axis] * half +
                                 tangents[face][axis] * u * half +
                                 bitangents[face][axis] * v * half;
            }
            /* A UV taken from an actual hand vertex (0.0643, 0.7835); the
             * centre of the atlas sampled transparent texels and made the test
             * cube invisible. */
            float uv[2] = {0.0643f, 0.7835f};
            write_vertex(face * 4 + corner, position, normals[face], uv, uv1,
                         tangents[face], bitangents[face]);
        }
        unsigned base = face * 4;
        unsigned *out = g_cube_ibo + face * 6;
        out[0] = base;
        out[1] = base + 1;
        out[2] = base + 2;
        out[3] = base;
        out[4] = base + 2;
        out[5] = base + 3;
    }
}

static void resolve_gl(void) {
    HMODULE module = GetModuleHandleA("opengl32.dll");
    if (!module) return;
    wgl_get_proc_fn wgl = (wgl_get_proc_fn)GetProcAddress(module, "wglGetProcAddress");
    if (wgl) {
        g_bind_buffer = (bind_buffer_fn)wgl("glBindBuffer");
        g_buffer_data = (buffer_data_fn)wgl("glBufferData");
        g_gen_buffers = (gen_buffers_fn)wgl("glGenBuffers");
        g_gen_vertex_arrays = (gen_vertex_arrays_fn)wgl("glGenVertexArrays");
        g_bind_vertex_array = (bind_vertex_array_fn)wgl("glBindVertexArray");
        g_vertex_attrib_pointer = (vertex_attrib_pointer_fn)wgl("glVertexAttribPointer");
        g_enable_vertex_attrib_array =
            (enable_vertex_attrib_array_fn)wgl("glEnableVertexAttribArray");
        g_get_integerv = (get_integerv_fn)wgl("glGetIntegerv");
        g_gen_textures = (gen_textures_fn)wgl("glGenTextures");
        g_bind_texture = (bind_texture_fn)wgl("glBindTexture");
        g_tex_image_2d = (tex_image_2d_fn)wgl("glTexImage2D");
        g_tex_parameteri = (tex_parameteri_fn)wgl("glTexParameteri");
        g_active_texture = (active_texture_fn)wgl("glActiveTexture");
        g_get_uniform_location = (get_uniform_location_fn)wgl("glGetUniformLocation");
        g_get_uniformiv = (get_uniformiv_fn)wgl("glGetUniformiv");
    }
    if (!g_bind_buffer) g_bind_buffer = (bind_buffer_fn)GetProcAddress(module, "glBindBuffer");
    if (!g_buffer_data) g_buffer_data = (buffer_data_fn)GetProcAddress(module, "glBufferData");
    if (!g_gen_textures)
        g_gen_textures = (gen_textures_fn)GetProcAddress(module, "glGenTextures");
    if (!g_bind_texture)
        g_bind_texture = (bind_texture_fn)GetProcAddress(module, "glBindTexture");
    if (!g_tex_image_2d)
        g_tex_image_2d = (tex_image_2d_fn)GetProcAddress(module, "glTexImage2D");
    if (!g_tex_parameteri)
        g_tex_parameteri = (tex_parameteri_fn)GetProcAddress(module, "glTexParameteri");
    /* GL 1.1 entry points come from opengl32's export table, not
     * wglGetProcAddress, which returns NULL for them. */
    if (!g_get_integerv) g_get_integerv = (get_integerv_fn)GetProcAddress(module, "glGetIntegerv");
    if (wgl) g_get_attrib_location = (get_attrib_location_fn)wgl("glGetAttribLocation");
    if (!g_get_attrib_location)
        g_get_attrib_location =
            (get_attrib_location_fn)GetProcAddress(module, "glGetAttribLocation");
    g_get_error = (get_error_fn)GetProcAddress(module, "glGetError");
    g_is_enabled = (is_enabled_fn)GetProcAddress(module, "glIsEnabled");
    g_enable = (enable_fn)GetProcAddress(module, "glEnable");
    g_disable = (disable_fn)GetProcAddress(module, "glDisable");
}

/* Blue/cyan feather diffuse with a restrained warm glint. The game's gradient
 * and reflection samplers provide the broad gold iridescence; this texture
 * supplies fine surface color without flattening those material inputs. */
static void build_blue_pixels(void) {
    for (unsigned y = 0; y < BLUE_TEXTURE_SIZE; ++y) {
        for (unsigned x = 0; x < BLUE_TEXTURE_SIZE; ++x) {
            /* The converter packs the five glTF materials into five UV
             * columns. Keep their color identities while tying them together
             * with shared blue feather grain and restrained gold glints. */
            unsigned slot = x * 5u / BLUE_TEXTURE_SIZE;
            unsigned local_x = x * 5u % BLUE_TEXTURE_SIZE;
            unsigned cross = local_x < BLUE_TEXTURE_SIZE / 2u
                                 ? BLUE_TEXTURE_SIZE / 2u - local_x
                                 : local_x - BLUE_TEXTURE_SIZE / 2u;
            unsigned barb = (y * 7u + local_x * 3u) & 31u;
            unsigned row = (y >> 4u) & 1u;
            static const unsigned char palette[5][3] = {
                {20, 66, 154},  /* BerdlyBlue */
                {50, 126, 214}, /* BerdlyLight */
                {116, 91, 39},  /* warm iridescent cream */
                {12, 29, 73},   /* deep blue claws */
                {31, 91, 165},  /* BerdlyIridescent */
            };
            unsigned red = palette[slot][0] + ((y * 5u + local_x * 3u) & 15u);
            unsigned green = palette[slot][1] + ((y * 3u + local_x) & 23u);
            unsigned blue = palette[slot][2] + ((y * 5u + local_x * 2u) & 31u);
            if (barb < 3u) {
                red += slot == 2u ? 20u : 10u;
                green += slot == 2u ? 27u : 31u;
                blue += slot == 2u ? 18u : 29u;
            }
            if (row) {
                red += 5u;
                green += 9u;
                blue += 7u;
            }
            if (cross < 3u) {
                red += slot == 2u ? 23u : 18u;
                green += 34u;
                blue += 27u;
            }
            if (((local_x * 29u + y * 17u) % 251u) == 0u && cross < 14u) {
                /* Tiny warm pinpricks echo the gold reflections without
                 * turning the diffuse albedo into a yellow surface. */
                red = 154u;
                green = 108u;
                blue = 42u;
            }
            unsigned char *pixel =
                g_blue_pixels + ((size_t)y * BLUE_TEXTURE_SIZE + x) * 4u;
            pixel[0] = (unsigned char)(red > 255u ? 255u : red);
            pixel[1] = (unsigned char)(green > 255u ? 255u : green);
            pixel[2] = (unsigned char)(blue > 255u ? 255u : blue);
            pixel[3] = 255u;
        }
    }
}

static void prepare_blue_texture(unsigned program) {
    if (!g_gen_textures || !g_bind_texture || !g_tex_image_2d || !g_tex_parameteri ||
        !g_get_integerv || !g_get_uniform_location || !g_active_texture)
        return;

    build_blue_pixels();
    int previous_texture = 0;
    g_get_integerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    unsigned texture = 0;
    g_gen_textures(1, &texture);
    if (!texture) return;
    g_bind_texture(GL_TEXTURE_2D, texture);
    g_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (int)GL_LINEAR);
    g_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (int)GL_LINEAR);
    g_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (int)GL_CLAMP_TO_EDGE);
    g_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (int)GL_REPEAT);
    g_tex_image_2d(GL_TEXTURE_2D, 0, (int)GL_RGBA8, BLUE_TEXTURE_SIZE, BLUE_TEXTURE_SIZE, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, g_blue_pixels);
    g_bind_texture(GL_TEXTURE_2D, (unsigned)previous_texture);

    int diffuse_sampler = g_get_uniform_location(program, "tex_diffuse");
    int sampler_unit = 0;
    if (diffuse_sampler >= 0 && g_get_uniformiv)
        g_get_uniformiv(program, diffuse_sampler, &sampler_unit);
    if (sampler_unit < 0 || sampler_unit > 31) sampler_unit = 0;
    g_materials[0].program = program;
    g_materials[0].diffuse_sampler = diffuse_sampler;
    g_materials[0].texture_unit = sampler_unit;
    g_material_count = 1;
    g_blue_texture = texture;
    g_texture_ready = 1;
    hypr_hand_debug[20] = (unsigned long long)(diffuse_sampler + 1);
    hypr_hand_debug[21] = texture;
    hypr_hand_debug[22] = (unsigned long long)sampler_unit;
    hypr_hand_debug[23] = 1;
    static const char *const samplers[7] = {"tex_diffuse", "tex_grad", "tex_reflect",
                                            "tex_mask", "tex_edgespec", "tex_spec",
                                            "tex_cns"};
    for (unsigned i = 0; i < 7; ++i) {
        int location = g_get_uniform_location(program, samplers[i]);
        hypr_hand_debug[24 + i] = (unsigned long long)(location + 1);
    }
    hypr_hand_debug[33] = g_get_error ? g_get_error() : 0;
}

/* Resolve the diffuse unit for a second hand program on first sight. The
 * program is current at this point, so the uniform queries are valid. */
static int material_state_for(unsigned program) {
    for (unsigned i = 0; i < g_material_count; ++i)
        if (g_materials[i].program == program) return (int)i;
    if (g_material_count >= 2 || !g_get_uniform_location) return -1;
    int location = g_get_uniform_location(program, "tex_diffuse");
    if (location < 0) return -1;
    int unit = 0;
    if (g_get_uniformiv) g_get_uniformiv(program, location, &unit);
    if (unit < 0 || unit > 31) unit = 0;
    material_state *state = &g_materials[g_material_count];
    state->program = program;
    state->diffuse_sampler = location;
    state->texture_unit = unit;
    ++g_material_count;
    hypr_hand_debug[37] = program;
    return (int)(g_material_count - 1);
}

/* Read and validate <state_dir>/hand-mesh.bin. The file is a little-endian
 * header followed by 72-byte vertex records and u32 indices; it is our own
 * converted asset, not game data. */
static int load_arm_mesh(void) {
    char path[1024];
    if (!state_path(g_host, "hand-mesh.bin", path, sizeof path)) return 0;
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    /* This CRT exposes fopen/fread but not fseek/ftell. A single bounded read
     * is enough: the mesh is well under the 4 MiB ceiling. */
    size_t size = fread(g_arm_file, 1, ARM_MAX_FILE_BYTES, file);
    fclose(file);
    if (size < ARM_HEADER_BYTES || size >= ARM_MAX_FILE_BYTES) return 0;

    static const char magic[8] = {'H', 'Y', 'P', 'R', 'M', 'E', 'S', 'H'};
    for (unsigned i = 0; i < 8; ++i)
        if (g_arm_file[i] != (unsigned char)magic[i]) return 0;

    unsigned version = 0, vertices = 0, indices = 0, stride = 0;
    memcpy(&version, g_arm_file + 8, 4);
    memcpy(&vertices, g_arm_file + 12, 4);
    memcpy(&indices, g_arm_file + 16, 4);
    memcpy(&stride, g_arm_file + 20, 4);
    if (version != 1 || stride != 72 || !vertices || !indices) return 0;
    if ((unsigned long)size !=
        ARM_HEADER_BYTES + (unsigned long)vertices * 72ul + (unsigned long)indices * 4ul)
        return 0;

    g_arm_vertex_count = vertices;
    g_arm_index_count = indices;
    hypr_hand_debug[16] = vertices;
    hypr_hand_debug[17] = indices;
    hypr_hand_debug[18] = 1;
    hypr_hand_debug[19] = (unsigned long long)size;
    return 1;
}

/* Build the private VAO on the render thread, at the first owned draw. It uses
 * the measured hand layout: six float attributes at offsets 0/12/24/32/48/60,
 * stride 72, and a 32-bit index buffer. If anything is unavailable the draws
 * are forwarded untouched. */
static void prepare_geometry(void) {
    if (g_geometry_state != 0) return;
    g_geometry_state = -1;
    ++hypr_hand_debug[0];
    if (g_get_error) g_get_error(); /* clear stale engine errors */
    /* opengl32.dll is loaded by the game's window/GL setup, which happens after
     * mod initialization, so resolve the entry points here on the render
     * thread where the context is current. */
    hypr_hand_debug[7] = (unsigned long long)(size_t)GetModuleHandleA("opengl32.dll");
    resolve_gl();
    hypr_hand_debug[1] = (g_bind_buffer != NULL) | ((g_buffer_data != NULL) << 1) |
                         ((g_gen_buffers != NULL) << 2) |
                         ((g_gen_vertex_arrays != NULL) << 3) |
                         ((g_bind_vertex_array != NULL) << 4) |
                         ((g_vertex_attrib_pointer != NULL) << 5) |
                         ((g_enable_vertex_attrib_array != NULL) << 6) |
                         ((g_get_integerv != NULL) << 7);
    if (!g_bind_buffer || !g_buffer_data || !g_gen_buffers ||
        !g_gen_vertex_arrays || !g_bind_vertex_array || !g_vertex_attrib_pointer ||
        !g_enable_vertex_attrib_array || !g_get_integerv)
        return;

    const void *vertex_data = g_cube_vbo;
    long long vertex_bytes = CUBE_VBO_BYTES;
    const void *index_data = g_cube_ibo;
    long long index_bytes = CUBE_IBO_BYTES;
    unsigned index_count = CUBE_INDEX_COUNT;
    if (g_mode == 3) {
        if (!g_arm_vertex_count || !g_arm_index_count) {
            hypr_hand_debug[18] = 2;
            return;
        }
        vertex_data = g_arm_file + ARM_HEADER_BYTES;
        vertex_bytes = (long long)g_arm_vertex_count * 72;
        index_data = g_arm_file + ARM_HEADER_BYTES + vertex_bytes;
        index_bytes = (long long)g_arm_index_count * 4;
        index_count = g_arm_index_count;
    }

    unsigned array = 0;
    unsigned buffer = 0;
    g_gen_vertex_arrays(1, &array);
    g_gen_buffers(1, &buffer);
    unsigned element = 0;
    g_gen_buffers(1, &element);
    if (!array || !buffer || !element) return;

    int previous = 0;
    g_get_integerv(GL_VERTEX_ARRAY_BINDING, &previous);

    g_bind_vertex_array(array);
    g_bind_buffer(GL_ARRAY_BUFFER, buffer);
    g_buffer_data(GL_ARRAY_BUFFER, vertex_bytes, vertex_data, 0x88E4u); /* GL_STATIC_DRAW */

    static const char *const names[5] = {"in_position", "in_normal", "in_uv0", "in_uv1",
                                          "in_tangent"};
    static const int sizes[5] = {3, 3, 2, 4, 3};
    static const int offsets[5] = {0, 12, 24, 32, 48};
    int program = 0;
    g_get_integerv(0x8B8Du, &program); /* GL_CURRENT_PROGRAM */
    for (unsigned i = 0; i < 5; ++i) {
        int location = -1;
        if (g_get_attrib_location && program > 0)
            location = g_get_attrib_location((unsigned)program, names[i]);
        if (location < 0) location = (int)i;
        hypr_hand_debug[8 + i] = (unsigned long long)location;
        g_enable_vertex_attrib_array((unsigned)location);
        g_vertex_attrib_pointer((unsigned)location, sizes[i], GL_FLOAT, 0, 72,
                                (const void *)(size_t)offsets[i]);
    }

    g_bind_buffer(GL_ELEMENT_ARRAY_BUFFER, element);
    g_buffer_data(GL_ELEMENT_ARRAY_BUFFER, index_bytes, index_data, 0x88E4u);

    g_bind_vertex_array((unsigned)previous);

    g_vao = array;
    g_geometry_count = index_count;
    hypr_hand_debug[2] = 1;
    hypr_hand_debug[13] = (unsigned long long)program;
    if (g_mode == 3) prepare_blue_texture((unsigned)program);
    hypr_hand_debug[14] = g_get_error ? g_get_error() : 0;
    g_geometry_state = 1;
}

static void draw_substitute(void) {
    int previous = 0;
    if (g_get_integerv) g_get_integerv(GL_VERTEX_ARRAY_BINDING, &previous);
    unsigned char depth_on = 0;
    unsigned char cull_on = 0;
    if (g_is_enabled && g_disable) {
        depth_on = g_is_enabled(0x0B71u); /* GL_DEPTH_TEST */
        cull_on = g_is_enabled(0x0B44u);  /* GL_CULL_FACE */
        g_disable(0x0B71u);
        g_disable(0x0B44u);
    }
    int texture_bound = 0;
    int previous_active = 0;
    int previous_texture = 0;
    int diffuse_unit = -1;
    if (g_texture_ready && g_get_integerv && g_active_texture && g_bind_texture) {
        int current_program = 0;
        g_get_integerv(0x8B8Du, &current_program);
        hypr_hand_debug[34] = (unsigned long long)(unsigned)current_program;
        int state_index = material_state_for((unsigned)current_program);
        g_get_integerv(GL_ACTIVE_TEXTURE, &previous_active);
        if (state_index >= 0) {
            diffuse_unit = g_materials[state_index].texture_unit;
            g_active_texture(GL_TEXTURE0 + (unsigned)diffuse_unit);
            g_get_integerv(GL_TEXTURE_BINDING_2D, &previous_texture);
            g_bind_texture(GL_TEXTURE_2D, g_blue_texture);
            texture_bound = 1;
        } else {
            ++hypr_hand_debug[36];
        }
        hypr_hand_debug[38] = (unsigned long long)previous_active;
        hypr_hand_debug[39] = (unsigned long long)previous_texture;
    }
    g_bind_vertex_array(g_vao);
    g_original_draw(GL_TRIANGLES, (int)g_geometry_count, GL_UNSIGNED_INT, NULL);
    g_bind_vertex_array((unsigned)previous);
    if (texture_bound) {
        g_active_texture(GL_TEXTURE0 + (unsigned)diffuse_unit);
        g_bind_texture(GL_TEXTURE_2D, (unsigned)previous_texture);
        g_active_texture((unsigned)previous_active);
        ++hypr_hand_debug[32];
    }
    if (g_enable) {
        if (cull_on) g_enable(0x0B44u);
        if (depth_on) g_enable(0x0B71u);
    }
    ++g_substituted;
    ++hypr_hand_debug[5];
    hypr_hand_debug[15] = g_get_error ? g_get_error() : 0;
}

/* The shim runs on the render thread inside the engine's own draw call. It
 * must not call the host logger or any other code that can take a loader lock:
 * the first version did, and the game died on the first gameplay hand draw. It
 * only counts, substitutes or suppresses, and forwards. */
static void WINAPI hand_draw_shim(unsigned mode, int count, unsigned type,
                                  const void *indices) {
    if (g_active && mode == GL_TRIANGLES && count == HAND_INDEX_COUNT &&
        (type == GL_UNSIGNED_INT || type == GL_UNSIGNED_SHORT)) {
        ++g_hand_draws;
        hypr_hand_debug[4] = g_hand_draws;
        hypr_hand_debug[3] = (unsigned)g_geometry_state;
        hypr_hand_debug[6] = g_mode;
        if (g_mode == 2 || g_mode == 3) {
            prepare_geometry();
            if (g_geometry_state == 1) {
                draw_substitute();
                return;
            }
        }
        if (g_mode == 1) count = 0;
    }
    g_original_draw(mode, count, type, indices);
}

static int mod_initialize(const hyprlodr_host_api *host) {
    g_host = host;

    if (!HYPRLODR_HOST_API_HAS(host, image_write) || !host->image_write ||
        !host->image_read) {
        log_message("warn", "host has no image access; hand hook inactive");
        return 0;
    }
    if (!host->build_matches(host->context)) {
        log_message("warn", "build fingerprint does not match; hand hook inactive");
        return 0;
    }

    void *original = NULL;
    if (host->image_read(host->context, HAND_DISPATCH_RVA, &original,
                         sizeof original) != 0) {
        log_message("error", "cannot read the draw dispatch slot");
        return -1;
    }
    if (!original) {
        log_message("error", "draw dispatch slot is empty");
        return -1;
    }
    g_original_draw = (draw_elements_fn)original;
    g_mode = read_mode(host);
    build_cube();
    if (g_mode == 3) {
        /* Load before patching: this runs on the mod thread, not inside the
         * engine's draw call, so no file or loader work happens on the render
         * thread. */
        if (!load_arm_mesh()) {
            log_message("warn", "arm mode: hand-mesh.bin missing or invalid; forwarding draws");
        } else {
            char mesh_message[160];
            snprintf(mesh_message, sizeof mesh_message,
                     "arm mesh loaded: %u vertices, %u indices, %llu bytes",
                     g_arm_vertex_count, g_arm_index_count, hypr_hand_debug[19]);
            log_message("info", mesh_message);
        }
    }

    void *replacement = (void *)&hand_draw_shim;
    if (host->image_write(host->context, HAND_DISPATCH_RVA, &replacement,
                          sizeof replacement) != 0) {
        log_message("error", "cannot patch the draw dispatch slot");
        return -1;
    }

    void *check = NULL;
    if (host->image_read(host->context, HAND_DISPATCH_RVA, &check, sizeof check) != 0 ||
        check != replacement) {
        log_message("error", "dispatch patch did not take");
        return -1;
    }

    g_active = 1;
    char message[192];
    snprintf(message, sizeof message,
             "dispatch hook installed at rva 0x3d9ac8 (original=0x%lx mode=%s)",
             (unsigned long)(size_t)original, mode_name(g_mode));
    log_message("info", message);
    return 0;
}

static void mod_shutdown(const hyprlodr_host_api *host) {
    g_active = 0;
    size_t restored = 0;
    if (HYPRLODR_HOST_API_HAS(host, image_restore) && host->image_restore)
        restored = host->image_restore(host->context);

    char message[224];
    snprintf(message, sizeof message,
             "hook removed after %lu hand draws (%lu substituted); %lu image byte(s) restored",
             g_hand_draws, g_substituted, (unsigned long)restored);
    host->log(host->context, "info", MOD_ID, message);
}

static const hyprlodr_plugin_descriptor descriptor = {
    (uint32_t)sizeof descriptor,
    HYPRLODR_ABI_VERSION,
    MOD_ID,
    MOD_VERSION,
    NULL,
    0,
    mod_initialize,
    mod_shutdown,
    NULL,
    0,
    "Hand hook",
    "Patches the engine's GL draw dispatch table to suppress or replace the "
    "first-person hand draw.",
    "HYPRLODR",
    0,
};

HYPR_EXPORT const hyprlodr_plugin_descriptor *
hyprlodr_plugin_query(uint32_t host_abi_version) {
    return host_abi_version == HYPRLODR_ABI_VERSION ? &descriptor : NULL;
}
