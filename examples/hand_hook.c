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
 *   "palette"   keep the native animated hand; recolor its tex_grad ramp or,
 *               when the program has none, its tex_diffuse skin atlas
 *   "overlay"   keep the native animated hand and draw authored feather cards
 *               bound to its live vertex positions
 *   "overlay-only"  diagnostic: suppress the native hand, draw only the
 *               feather cards deformed from its live vertex positions
 *   "capture"   pass the native draw through and capture its first matched
 *               vertex/index buffers; the second line must be an output path
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
 * game died on the first gameplay hand draw. Capture mode performs one
 * validated GL readback into fixed storage, then signals a deferred writer;
 * the draw callback does no allocation, formatting, file I/O, or host calls.
 *
 * This example is Windows-only: it needs the live game image and the host's
 * ABI 6 write gate. On a host without a verified image it initializes inert.
 */
#include "hyprlodr/plugin.h"
#include "hypr_win32.h"
#include "hand_dispatch_state.h"

#include <stdio.h>
#include <string.h>

/* The Windows target links without a CRT; floating point code references this
 * marker, which the normal CRT would define. */
int _fltused = 0;

#define MOD_ID "example.hand-hook"
#define MOD_VERSION "0.9.5"

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

/* The original arm is a 238-vertex, 72-byte posed buffer. The feather overlay
 * binds to those vertices and follows their per-frame deltas. */
#define HAND_VERTEX_COUNT 238
#define HAND_VBO_BYTES (HAND_VERTEX_COUNT * 72)
#define HAND_CAPTURE_HEADER_BYTES 104u
#define HAND_CAPTURE_INDEX_BYTES (HAND_INDEX_COUNT * 4u)
#define HAND_DISPATCH_RETRY_MS 10u
#define HAND_DISPATCH_RETRY_COUNT 3000u
#define FEATHER_HEADER_BYTES 32
#define FEATHER_VERTEX_BYTES 64
#define FEATHER_MAX_VERTICES 65536
#define FEATHER_MAX_INDICES 131072
#define FEATHER_MAX_BYTES (4u * 1024u * 1024u)

#define GL_TRIANGLES 4u
#define GL_FLOAT 0x1406u
/* The live draw declares 5125 = GL_UNSIGNED_INT, and the 4,752-byte element
 * buffer is one run of 1,188 32-bit indices (not two 16-bit runs). */
#define GL_UNSIGNED_INT 0x1405u
#define GL_UNSIGNED_SHORT 0x1403u
#define GL_ARRAY_BUFFER 0x8892u
#define GL_ELEMENT_ARRAY_BUFFER 0x8893u
#define GL_VERTEX_ARRAY_BINDING 0x85B5u
#define GL_ARRAY_BUFFER_BINDING 0x8894u
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895u
#define GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING 0x889Fu
#define GL_VERTEX_ATTRIB_ARRAY_ENABLED 0x8622u
#define GL_VERTEX_ATTRIB_ARRAY_SIZE 0x8623u
#define GL_VERTEX_ATTRIB_ARRAY_STRIDE 0x8624u
#define GL_VERTEX_ATTRIB_ARRAY_TYPE 0x8625u
#define GL_VERTEX_ATTRIB_ARRAY_POINTER 0x8645u
#define GL_BUFFER_SIZE 0x8764u
#define GL_CURRENT_PROGRAM 0x8B8Du
#define GL_CULL_FACE 0x0B44u
#define GL_DEPTH_TEST 0x0B71u

#define GL_TEXTURE_2D 0x0DE1u
#define GL_TEXTURE0 0x84C0u
#define GL_TEXTURE_BINDING_2D 0x8069u
#define GL_TEXTURE_WIDTH 0x1000u
#define GL_TEXTURE_HEIGHT 0x1001u
#define GL_MAX_TEXTURE_SIZE 0x0D33u
#define GL_PIXEL_PACK_BUFFER 0x88EBu
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88EDu
#define GL_PACK_ROW_LENGTH 0x0D02u
#define GL_PACK_SKIP_ROWS 0x0D03u
#define GL_PACK_SKIP_PIXELS 0x0D04u
#define GL_PACK_ALIGNMENT 0x0D05u
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
#define BLUE_TEXTURE_MAX_SIZE 256
/* The hand's skin is a small colour atlas sampled through tex_diffuse; the
 * screen-space blend pass also uses tex_diffuse but binds a viewport-sized
 * texture. Anything larger than this cap is treated as that screen texture and
 * is never recoloured. */
#define BLUE_DIFFUSE_MAX_SIZE 1024

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
typedef void (WINAPI *get_tex_level_parameteriv_fn)(unsigned target, int level,
                                                    unsigned parameter, int *value);
typedef void (WINAPI *get_tex_parameteriv_fn)(unsigned target, unsigned parameter,
                                               int *value);
typedef void (WINAPI *get_tex_image_fn)(unsigned target, int level, unsigned format,
                                        unsigned type, void *pixels);
typedef void (WINAPI *pixel_storei_fn)(unsigned pname, int value);
typedef void (WINAPI *tex_parameteri_fn)(unsigned target, unsigned pname, int value);
typedef void (WINAPI *active_texture_fn)(unsigned texture);
typedef int (WINAPI *get_uniform_location_fn)(unsigned program, const char *name);
typedef void (WINAPI *get_uniformiv_fn)(unsigned program, int location, int *values);
typedef void (WINAPI *get_vertex_attribiv_fn)(unsigned index, unsigned pname, int *value);
typedef void (WINAPI *get_buffer_subdata_fn)(unsigned target, long long offset,
                                             long long size, void *data);
typedef void (WINAPI *get_buffer_parameteriv_fn)(unsigned target, unsigned pname, int *value);
typedef void (WINAPI *get_vertex_attrib_pointerv_fn)(unsigned index, unsigned pname,
                                                     void **pointer);
typedef void (WINAPI *buffer_sub_data_fn)(unsigned target, long long offset,
                                          long long size, const void *data);
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
static get_tex_level_parameteriv_fn g_get_tex_level_parameteriv;
static get_tex_parameteriv_fn g_get_tex_parameteriv;
static get_tex_image_fn g_get_tex_image;
static pixel_storei_fn g_pixel_storei;
static tex_parameteri_fn g_tex_parameteri;
static active_texture_fn g_active_texture;
static get_uniform_location_fn g_get_uniform_location;
static get_uniformiv_fn g_get_uniformiv;
static get_vertex_attribiv_fn g_get_vertex_attribiv;
static get_buffer_subdata_fn g_get_buffer_subdata;
static get_buffer_parameteriv_fn g_get_buffer_parameteriv;
static get_vertex_attrib_pointerv_fn g_get_vertex_attrib_pointerv;
static buffer_sub_data_fn g_buffer_sub_data;
static int g_capture_gl_resolved;

static unsigned g_mode; /* 0 pass-through, 1 suppress, 2 cube, 3 arm, 4 palette */
static unsigned long g_hand_draws;
static unsigned long g_substituted;
static int g_active;
static char g_capture_path[1024];
static HANDLE g_capture_event;
static HANDLE g_capture_thread;
static volatile LONG g_capture_claimed;
static volatile LONG g_capture_ready;
static volatile LONG g_capture_saved;
static volatile LONG g_capture_stop;
static HANDLE g_dispatch_thread;
static volatile LONG g_dispatch_stop;
static volatile LONG g_dispatch_state; /* 0 waiting, 1 installing, 2 installed, -1 failed */
static unsigned char g_capture_vertices[HAND_VBO_BYTES];
static unsigned char g_capture_indices[HAND_CAPTURE_INDEX_BYTES];
static unsigned long long g_capture_index_offset;
static unsigned g_capture_array_buffer;
static unsigned g_capture_element_buffer;
static unsigned g_capture_attribute_offsets[6];
static unsigned g_capture_draw_ordinal;
static unsigned g_capture_timestamp_ms;
static int g_capture_status;
static unsigned g_capture_failure;

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

/* Scratch storage for one source ramp at a time. Each hand program retains
 * its own recolored copy. The game's diffuse screen sample, reflection and
 * specular inputs stay bound; only tex_grad is redirected. */
static unsigned char g_blue_pixels[BLUE_TEXTURE_MAX_SIZE * BLUE_TEXTURE_MAX_SIZE * 4];
static unsigned char g_blue_diffuse_pixels[BLUE_DIFFUSE_MAX_SIZE * BLUE_DIFFUSE_MAX_SIZE * 4];

/* Authored five-column feather palette for the replacement mesh. The converter
 * packs the source material slot into the U coordinate, so each column is one
 * material: deep blue body, lit blue, cream, dark claw and gold accent. The
 * game's reflection and screen-blend samplers stay bound; only the skin colour
 * sampler is redirected. */
#define ARM_PALETTE_COLUMNS 5
#define ARM_PALETTE_COLUMN_WIDTH 64
#define ARM_PALETTE_WIDTH (ARM_PALETTE_COLUMNS * ARM_PALETTE_COLUMN_WIDTH)
#define ARM_PALETTE_HEIGHT 128
static unsigned char g_arm_palette_pixels[ARM_PALETTE_WIDTH * ARM_PALETTE_HEIGHT * 4];
static int g_arm_palette_ready;

/* The hand color result is built from tex_reflection1, not tex_diffuse. Keep
 * a recolored copy of that animated reflection so the authored blue arm still
 * receives the native gold highlights; tex_diffuse remains the screen blend. */
#define REFLECTION_MAX_SIZE 1024
static unsigned char g_reflection_pixels[REFLECTION_MAX_SIZE * REFLECTION_MAX_SIZE * 4];
static unsigned g_feather_reflection_texture;
static unsigned g_feather_reflection_source;
static unsigned g_feather_reflection_width;
static unsigned g_feather_reflection_height;

/* Feather overlay: authored bind-pose vertices plus four weights into the live
 * 238-vertex arm buffer. The overlay is expanded into the same 72-byte layout
 * the hand program already consumes. */
typedef struct feather_vertex {
    float position[3];
    float normal[3];
    float uv[2];
    unsigned bind[4];
    float weight[4];
} feather_vertex;

static unsigned char g_feather_file[FEATHER_MAX_BYTES];
static feather_vertex g_feather_vertices[FEATHER_MAX_VERTICES];
static unsigned g_feather_indices[FEATHER_MAX_INDICES];
static unsigned g_feather_vertex_count;
static unsigned g_feather_index_count;
static unsigned char g_feather_vbo[FEATHER_MAX_VERTICES * 72];
static unsigned char g_native_vbo[HAND_VBO_BYTES];
static unsigned char g_bind_vbo[HAND_VBO_BYTES];
static int g_bind_valid;
static int g_feather_state; /* 0 unprepared, 1 ready, -1 unavailable */
static unsigned g_feather_vao;
static unsigned g_feather_gl_vbo;
static unsigned g_feather_palette_texture;

/* The two hand passes may use different programs and gradient ramps. Keep a
 * recolored texture per program so each pass preserves its own source colors. */
typedef struct material_state {
    unsigned program;
    int gradient_sampler;
    int gradient_unit;
    unsigned gradient_texture;
    unsigned palette_kind; /* 1 = tex_grad ramp, 2 = tex_diffuse skin atlas */
} material_state;
static material_state g_materials[2];
static unsigned g_material_count;

/* Read-only diagnostics for a debugger; never part of the mod API. */
HYPR_EXPORT volatile unsigned long long hypr_hand_debug[64];

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

static void put_u32(unsigned char *out, unsigned value) {
    out[0] = (unsigned char)value;
    out[1] = (unsigned char)(value >> 8);
    out[2] = (unsigned char)(value >> 16);
    out[3] = (unsigned char)(value >> 24);
}

static void put_u64(unsigned char *out, unsigned long long value) {
    put_u32(out, (unsigned)value);
    put_u32(out + 4, (unsigned)(value >> 32));
}

static int capture_path_is_local(const char *path) {
    /* This research capture contains proprietary runtime geometry. Requiring
     * an explicit .tmp path keeps it out of the installation and Git tree. */
    int absolute = ((path[0] >= 'A' && path[0] <= 'Z') ||
                    (path[0] >= 'a' && path[0] <= 'z')) &&
                   path[1] == ':' && (path[2] == '\\' || path[2] == '/');
    absolute = absolute || (path[0] == '\\' && path[1] == '\\');
    if (!absolute) return 0;
    for (const char *p = path; p[0]; ++p) {
        int segment_start = p == path || p[-1] == '\\' || p[-1] == '/';
        if (segment_start && p[0] == '.' && p[1] == 't' && p[2] == 'm' &&
            p[3] == 'p' && (p[4] == '\0' || p[4] == '\\' || p[4] == '/')) return 1;
    }
    return 0;
}

static DWORD WINAPI capture_writer(void *unused) {
    (void)unused;
    for (;;) {
        if (WaitForSingleObject(g_capture_event, INFINITE) != 0) return 0;
        if (InterlockedCompareExchange(&g_capture_ready, 0, 0) == 1) {
            unsigned char header[HAND_CAPTURE_HEADER_BYTES];
            for (unsigned i = 0; i < sizeof header; ++i) header[i] = 0;
            memcpy(header, "HYPRCAP1", 8);
            put_u32(header + 8, 1);
            put_u32(header + 12, HAND_CAPTURE_HEADER_BYTES);
            put_u32(header + 16, g_capture_draw_ordinal);
            /* The measured game issues two adjacent hand passes per frame.
             * This is a pair-derived frame label, not a swap-hook frame count. */
            put_u32(header + 20, (g_capture_draw_ordinal - 1u) / 2u);
            put_u32(header + 24, 7);
            put_u32(header + 28, HAND_INDEX_COUNT);
            put_u32(header + 32, GL_UNSIGNED_INT);
            put_u32(header + 36, HAND_VERTEX_COUNT);
            put_u32(header + 40, 72);
            put_u32(header + 44, HAND_VBO_BYTES);
            put_u32(header + 48, HAND_INDEX_COUNT);
            put_u32(header + 52, 4);
            put_u64(header + 56, g_capture_index_offset);
            put_u64(header + 64, g_capture_timestamp_ms);
            put_u32(header + 72, g_capture_array_buffer);
            put_u32(header + 76, g_capture_element_buffer);
            for (unsigned i = 0; i < 6; ++i)
                put_u32(header + 80 + i * 4u, g_capture_attribute_offsets[i]);

            HANDLE file = CreateFileA(g_capture_path, GENERIC_WRITE, FILE_SHARE_READ,
                                      NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (file == INVALID_HANDLE_VALUE) {
                InterlockedExchange(&g_capture_saved, -1);
            } else {
                DWORD written = 0;
                int ok = WriteFile(file, header, sizeof header, &written, NULL) &&
                         written == sizeof header &&
                         WriteFile(file, g_capture_vertices, HAND_VBO_BYTES, &written, NULL) &&
                         written == HAND_VBO_BYTES &&
                         WriteFile(file, g_capture_indices, HAND_CAPTURE_INDEX_BYTES,
                                   &written, NULL) &&
                         written == HAND_CAPTURE_INDEX_BYTES;
                CloseHandle(file);
                InterlockedExchange(&g_capture_saved, ok ? 1 : -1);
            }
            return 0;
        }
        if (InterlockedCompareExchange(&g_capture_stop, 0, 0)) return 0;
    }
}

static int start_capture_writer(void) {
    g_capture_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g_capture_event) return 0;
    g_capture_thread = CreateThread(NULL, 0, capture_writer, NULL, 0, NULL);
    if (!g_capture_thread) {
        CloseHandle(g_capture_event);
        g_capture_event = NULL;
        return 0;
    }
    return 1;
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
        else if (has_prefix(word, "palette")) mode = 4;
        else if (has_prefix(word, "overlay-only")) mode = 6;
        else if (has_prefix(word, "overlay")) mode = 5;
        else if (has_prefix(word, "capture")) {
            char output[1024];
            if (fgets(output, (int)sizeof output, file)) {
                size_t length = strlen(output);
                while (length && (output[length - 1] == '\r' || output[length - 1] == '\n' ||
                                  output[length - 1] == ' ' || output[length - 1] == '\t'))
                    output[--length] = '\0';
                if (length < sizeof g_capture_path && capture_path_is_local(output)) {
                    memcpy(g_capture_path, output, length + 1);
                    mode = 7;
                }
            }
        }
    }
    fclose(file);
    return mode;
}

static const char *mode_name(unsigned mode) {
    if (mode == 1) return "suppress";
    if (mode == 2) return "cube";
    if (mode == 3) return "arm";
    if (mode == 4) return "palette";
    if (mode == 5) return "overlay";
    if (mode == 6) return "overlay-only";
    if (mode == 7) return "capture";
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
        g_get_vertex_attribiv = (get_vertex_attribiv_fn)wgl("glGetVertexAttribiv");
        g_get_buffer_subdata = (get_buffer_subdata_fn)wgl("glGetBufferSubData");
        g_get_buffer_parameteriv = (get_buffer_parameteriv_fn)wgl("glGetBufferParameteriv");
        g_get_vertex_attrib_pointerv =
            (get_vertex_attrib_pointerv_fn)wgl("glGetVertexAttribPointerv");
        g_buffer_sub_data = (buffer_sub_data_fn)wgl("glBufferSubData");
    }
    if (!g_bind_buffer) g_bind_buffer = (bind_buffer_fn)GetProcAddress(module, "glBindBuffer");
    if (!g_buffer_data) g_buffer_data = (buffer_data_fn)GetProcAddress(module, "glBufferData");
    if (!g_gen_textures)
        g_gen_textures = (gen_textures_fn)GetProcAddress(module, "glGenTextures");
    if (!g_bind_texture)
        g_bind_texture = (bind_texture_fn)GetProcAddress(module, "glBindTexture");
    if (!g_tex_image_2d)
        g_tex_image_2d = (tex_image_2d_fn)GetProcAddress(module, "glTexImage2D");
    g_get_tex_level_parameteriv =
        (get_tex_level_parameteriv_fn)GetProcAddress(module, "glGetTexLevelParameteriv");
    g_get_tex_parameteriv =
        (get_tex_parameteriv_fn)GetProcAddress(module, "glGetTexParameteriv");
    g_get_tex_image = (get_tex_image_fn)GetProcAddress(module, "glGetTexImage");
    g_pixel_storei = (pixel_storei_fn)GetProcAddress(module, "glPixelStorei");
    if (!g_tex_parameteri)
        g_tex_parameteri = (tex_parameteri_fn)GetProcAddress(module, "glTexParameteri");
    /* GL 1.1 entry points come from opengl32's export table, not
     * wglGetProcAddress, which returns NULL for them. */
    if (!g_get_integerv) g_get_integerv = (get_integerv_fn)GetProcAddress(module, "glGetIntegerv");
    if (!g_get_vertex_attribiv)
        g_get_vertex_attribiv =
            (get_vertex_attribiv_fn)GetProcAddress(module, "glGetVertexAttribiv");
    if (!g_get_buffer_subdata)
        g_get_buffer_subdata =
            (get_buffer_subdata_fn)GetProcAddress(module, "glGetBufferSubData");
    if (!g_get_buffer_parameteriv)
        g_get_buffer_parameteriv =
            (get_buffer_parameteriv_fn)GetProcAddress(module, "glGetBufferParameteriv");
    if (!g_get_vertex_attrib_pointerv)
        g_get_vertex_attrib_pointerv =
            (get_vertex_attrib_pointerv_fn)GetProcAddress(module, "glGetVertexAttribPointerv");
    if (!g_buffer_sub_data)
        g_buffer_sub_data = (buffer_sub_data_fn)GetProcAddress(module, "glBufferSubData");
    if (wgl) g_get_attrib_location = (get_attrib_location_fn)wgl("glGetAttribLocation");
    if (!g_get_attrib_location)
        g_get_attrib_location =
            (get_attrib_location_fn)GetProcAddress(module, "glGetAttribLocation");
    g_get_error = (get_error_fn)GetProcAddress(module, "glGetError");
    g_is_enabled = (is_enabled_fn)GetProcAddress(module, "glIsEnabled");
    g_enable = (enable_fn)GetProcAddress(module, "glEnable");
    g_disable = (disable_fn)GetProcAddress(module, "glDisable");
}

/* Resolve only the six entry points needed by one passive geometry snapshot.
 * This runs on the first matching draw, where the WGL context is current. */
static void resolve_capture_gl(void) {
    if (g_capture_gl_resolved) return;
    g_capture_gl_resolved = 1;
    HMODULE module = GetModuleHandleA("opengl32.dll");
    if (!module) return;
    wgl_get_proc_fn wgl = (wgl_get_proc_fn)GetProcAddress(module, "wglGetProcAddress");
    if (wgl) {
        g_bind_buffer = (bind_buffer_fn)wgl("glBindBuffer");
        g_get_integerv = (get_integerv_fn)wgl("glGetIntegerv");
        g_get_vertex_attribiv = (get_vertex_attribiv_fn)wgl("glGetVertexAttribiv");
        g_get_vertex_attrib_pointerv =
            (get_vertex_attrib_pointerv_fn)wgl("glGetVertexAttribPointerv");
        g_get_buffer_parameteriv = (get_buffer_parameteriv_fn)wgl("glGetBufferParameteriv");
        g_get_buffer_subdata = (get_buffer_subdata_fn)wgl("glGetBufferSubData");
    }
    if (!g_bind_buffer) g_bind_buffer = (bind_buffer_fn)GetProcAddress(module, "glBindBuffer");
    if (!g_get_integerv) g_get_integerv = (get_integerv_fn)GetProcAddress(module, "glGetIntegerv");
    if (!g_get_vertex_attribiv)
        g_get_vertex_attribiv = (get_vertex_attribiv_fn)GetProcAddress(module, "glGetVertexAttribiv");
    if (!g_get_vertex_attrib_pointerv)
        g_get_vertex_attrib_pointerv = (get_vertex_attrib_pointerv_fn)
            GetProcAddress(module, "glGetVertexAttribPointerv");
    if (!g_get_buffer_parameteriv)
        g_get_buffer_parameteriv = (get_buffer_parameteriv_fn)
            GetProcAddress(module, "glGetBufferParameteriv");
    if (!g_get_buffer_subdata)
        g_get_buffer_subdata = (get_buffer_subdata_fn)GetProcAddress(module, "glGetBufferSubData");
}

/* Recolor the game's own tex_grad ramp in place. Its original dimensions and
 * texels define how the shader samples color, so preserving that structure is
 * a better fit for the native animated hand than projecting the replacement
 * mesh's five-column atlas onto it. Keep only the brightest warm ramp texels
 * as gold accents; shift warm midtones and cooler body tones to layered
 * navy/cobalt/cyan so the hand reads blue at gameplay scale. The original
 * reflection samplers remain the source of broad, moving iridescence. */
static void build_blue_pixels_into(unsigned char *pixels, unsigned width, unsigned height) {
    size_t count = (size_t)width * height;
    for (size_t i = 0; i < count; ++i) {
        unsigned char *pixel = pixels + i * 4u;
        unsigned red = pixel[0], green = pixel[1], blue = pixel[2];
        unsigned luma = (red * 54u + green * 183u + blue * 19u) >> 8u;
        /* Gold ramps often reach yellow rather than orange at their bright
         * end, where red and green are nearly equal. Use the blue separation
         * to distinguish saturated gold from neutral white highlights. */
        unsigned original[3] = {red, green, blue};
        /* Preserve more of each warm ramp as gold and soften its edge into
         * blue. The hue gate excludes neutral highlights; the broader ramp
         * keeps gold visible at hand scale. */
        int warm_hue = red + 16u >= green && green > blue + 20u &&
                       red > 110u && green > 62u;
        unsigned gold_mix = 0;
        if (warm_hue && luma > 128u) {
            /* Preserve a broader warm shoulder so gold remains visible at
             * first-person scale instead of collapsing to a one-pixel rim.
             * Ease toward the source gold so the broad shoulder does not sit
             * as long in a low-saturation RGB midpoint. The cubic ease-out
             * makes the warm band read as gold while leaving the cool ramps
             * blue. */
            gold_mix = (luma - 128u) * 255u / 107u;
            if (gold_mix > 255u) gold_mix = 255u;
            unsigned remaining = 255u - gold_mix;
            gold_mix = 255u - remaining * remaining * remaining / (255u * 255u);
        }
        /* Keep green below half of blue so cool ramps read cobalt rather than
         * cyan. Warm highlights remain sourced from the original gold ramp,
         * and the reflection path still supplies moving metallic iridescence. */
        unsigned recolored[3] = {3u + luma / 12u,
                                 14u + luma * 48u / 100u,
                                 38u + luma * 95u / 100u};
        for (unsigned channel = 0; channel < 3; ++channel) {
            if (recolored[channel] > 255u) recolored[channel] = 255u;
            unsigned value = recolored[channel] * (255u - gold_mix) +
                             original[channel] * gold_mix;
            pixel[channel] = (unsigned char)((value + 127u) / 255u);
        }
    }
}

static void build_blue_pixels(unsigned width, unsigned height) {
    build_blue_pixels_into(g_blue_pixels, width, height);
}

/* The native reflection map is mostly warm gold, so the broader gold shoulder
 * used for the skin ramp would preserve the whole arm. Keep only the brightest
 * saturated warm texels as gold and map the rest to layered cobalt/cyan. The
 * secondary reflection and edge-specular samplers stay native. */
static void build_blue_reflection_into(unsigned char *pixels, unsigned width,
                                       unsigned height) {
    size_t count = (size_t)width * height;
    for (size_t i = 0; i < count; ++i) {
        unsigned char *pixel = pixels + i * 4u;
        unsigned red = pixel[0], green = pixel[1], blue = pixel[2];
        unsigned luma = (red * 54u + green * 183u + blue * 19u) >> 8u;
        unsigned original[3] = {red, green, blue};
        int warm_hue = red + 12u >= green && green > blue + 24u;
        unsigned gold_mix = 0;
        if (warm_hue && luma > 185u && red > 200u && green > 120u) {
            gold_mix = (luma - 185u) * 255u / 70u;
            if (gold_mix > 255u) gold_mix = 255u;
            unsigned remaining = 255u - gold_mix;
            gold_mix = 255u - remaining * remaining / 255u;
        }
        unsigned recolored[3] = {4u + luma / 9u,
                                 20u + luma * 55u / 100u,
                                 56u + luma};
        for (unsigned channel = 0; channel < 3; ++channel) {
            if (recolored[channel] > 255u) recolored[channel] = 255u;
            unsigned value = recolored[channel] * (255u - gold_mix) +
                             original[channel] * gold_mix;
            pixel[channel] = (unsigned char)((value + 127u) / 255u);
        }
    }
}

static void build_arm_palette_texture(void) {
    if (g_arm_palette_ready) return;
    static const unsigned char base[ARM_PALETTE_COLUMNS][3] = {
        {14, 46, 142},   /* deep cobalt body */
        {48, 116, 208},  /* lit blue coverts */
        {206, 214, 218}, /* cream wrist/underwing */
        {5, 14, 38},     /* dark blue claw */
        {228, 176, 54},  /* warm gold accent */
    };
    for (unsigned column = 0; column < ARM_PALETTE_COLUMNS; ++column) {
        for (unsigned y = 0; y < ARM_PALETTE_HEIGHT; ++y) {
            /* Brighter toward the feather tip, with fine barbs every few rows
             * so linear filtering still reads as a surface rather than a
             * flat colour. */
            unsigned lift = y * 46u / (ARM_PALETTE_HEIGHT - 1u);
            unsigned barb = (y & 7u) < 3u ? 10u : 0u;
            for (unsigned x = 0; x < ARM_PALETTE_COLUMN_WIDTH; ++x) {
                size_t index = ((size_t)y * ARM_PALETTE_WIDTH +
                                column * ARM_PALETTE_COLUMN_WIDTH + x) * 4u;
                unsigned edge = (x < 3u || x + 3u >= ARM_PALETTE_COLUMN_WIDTH) ? 12u : 0u;
                for (unsigned channel = 0; channel < 3; ++channel) {
                    unsigned value = base[column][channel] + lift + barb;
                    if (channel == 2 && edge) value += edge; /* cool rim */
                    if (channel == 0 && column == 4) value += barb; /* gold shiver */
                    g_arm_palette_pixels[index + channel] =
                        (unsigned char)(value > 255u ? 255u : value);
                }
                g_arm_palette_pixels[index + 3] = 255;
            }
        }
    }
    g_arm_palette_ready = 1;
}

/* Recolor the hand's own skin atlas. The live hand program has no tex_grad, so
 * this is the texture that makes the stock arm gold. Keep the authored
 * luminance and the brightest saturated warm band as gold, and map the body to
 * deep cobalt with a cool cyan upper range. The alpha channel is untouched:
 * the same atlas carries the hand's cutout/coverage data. */
static void build_blue_diffuse_pixels(unsigned width, unsigned height) {
    size_t count = (size_t)width * height;
    for (size_t i = 0; i < count; ++i) {
        unsigned char *pixel = g_blue_diffuse_pixels + i * 4u;
        unsigned red = pixel[0], green = pixel[1], blue = pixel[2];
        unsigned luma = (red * 54u + green * 183u + blue * 19u) >> 8u;
        /* Gold skin is warm: red leads blue and green sits above blue. Neutral
         * white specular highlights have all three channels close together and
         * are deliberately excluded so they stay white. */
        int warm = red > blue + 14u && green > blue + 8u && red > 72u;
        unsigned gold_mix = 0;
        if (warm && luma > 176u) {
            gold_mix = (luma - 176u) * 255u / 70u;
            if (gold_mix > 255u) gold_mix = 255u;
            /* Smooth shoulder instead of a hard gold/blue seam. */
            unsigned remaining = 255u - gold_mix;
            gold_mix = 255u - remaining * remaining / 255u;
        }
        /* Deep cobalt body: blue tracks luminance, green stays well below it,
         * red stays low. The shader squares the sampled RGB, so keep the blue
         * channel high enough to survive that square. */
        unsigned recolored[3] = {6u + luma * 20u / 100u,
                                 22u + luma * 54u / 100u,
                                 72u + luma * 100u / 100u};
        for (unsigned channel = 0; channel < 3; ++channel) {
            unsigned base = recolored[channel] > 255u ? 255u : recolored[channel];
            unsigned original = channel == 0 ? red : (channel == 1 ? green : blue);
            unsigned value = base * (255u - gold_mix) + original * gold_mix;
            pixel[channel] = (unsigned char)((value + 127u) / 255u);
        }
        /* pixel[3] is the source alpha. */
    }
}

/* Rebinding a unit changes every sampler assigned to it. Only replace the
 * gradient when it has a dedicated unit so the screen blend and reflections
 * continue reading the game's original textures. */
static int gradient_unit_is_exclusive(unsigned program, int gradient_sampler,
                                     int gradient_unit) {
    int max_units = 0;
    if (!g_get_integerv) return 0;
    g_get_integerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &max_units);
    if (gradient_unit < 0 || max_units <= gradient_unit) return 0;

    static const char *const protected_samplers[] = {
        "tex_diffuse", "tex_reflect", "tex_mask", "tex_edgespec", "tex_spec", "tex_cns"
    };
    for (unsigned i = 0; i < sizeof protected_samplers / sizeof protected_samplers[0]; ++i) {
        int location = g_get_uniform_location(program, protected_samplers[i]);
        if (location < 0 || location == gradient_sampler) continue;
        int unit = -1;
        g_get_uniformiv(program, location, &unit);
        if (unit == gradient_unit) return 0;
    }
    return 1;
}

static int material_state_for(unsigned program) {
    for (unsigned i = 0; i < g_material_count; ++i)
        if (g_materials[i].program == program) return (int)i;
    if (g_material_count >= sizeof g_materials / sizeof g_materials[0]) {
        hypr_hand_debug[35] = 10; /* the hand uses more programs than the cache supports */
        return -1;
    }

    if (!g_gen_textures || !g_bind_texture || !g_tex_image_2d || !g_tex_parameteri ||
        !g_get_integerv || !g_get_uniform_location || !g_get_uniformiv || !g_active_texture ||
        !g_get_tex_level_parameteriv || !g_get_tex_image || !g_pixel_storei || !g_bind_buffer) {
        hypr_hand_debug[35] = 2; /* required GL query/upload entry point unavailable */
        return -1;
    }

    int gradient_sampler = g_get_uniform_location(program, "tex_grad");
    int diffuse_sampler = g_get_uniform_location(program, "tex_diffuse");
    unsigned palette_kind = 1;
    /* Prefer the skin atlas when it is a small texture. The live colour pass
     * carries both: tex_grad is only its iridescence ramp, while the visible
     * gold skin comes from tex_diffuse. The depth pass carries neither, and the
     * screen-blend pass binds a viewport-sized tex_diffuse, which the size gate
     * rejects. */
    if (diffuse_sampler >= 0) {
        int diffuse_unit = 0;
        g_get_uniformiv(program, diffuse_sampler, &diffuse_unit);
        if (diffuse_unit >= 0) {
            int previous_active = 0, source_texture = 0, width = 0, height = 0;
            g_get_integerv(GL_ACTIVE_TEXTURE, &previous_active);
            g_active_texture(GL_TEXTURE0 + (unsigned)diffuse_unit);
            g_get_integerv(GL_TEXTURE_BINDING_2D, &source_texture);
            if (source_texture != 0) {
                g_get_tex_level_parameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
                g_get_tex_level_parameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
            }
            g_active_texture((unsigned)previous_active);
            if (source_texture != 0 && width >= 8 && height >= 8 &&
                width <= BLUE_DIFFUSE_MAX_SIZE && height <= BLUE_DIFFUSE_MAX_SIZE) {
                gradient_sampler = diffuse_sampler;
                palette_kind = 2;
            } else if (g_mode == 3) {
                /* The replacement mesh owns its colour sample even when the
                 * stock program binds a viewport-sized texture there. */
                gradient_sampler = diffuse_sampler;
                palette_kind = 2;
            } else if (gradient_sampler < 0) {
                hypr_hand_debug[35] = 12; /* only a screen-sized diffuse exists */
                return -1;
            }
        }
    }
    if (gradient_sampler < 0) {
        hypr_hand_debug[35] = 3; /* neither palette sampler is present */
        return -1;
    }
    int sampler_unit = 0;
    g_get_uniformiv(program, gradient_sampler, &sampler_unit);
    if (sampler_unit < 0) {
        hypr_hand_debug[35] = 4; /* invalid texture unit */
        return -1;
    }
    if (!gradient_unit_is_exclusive(program, gradient_sampler, sampler_unit)) {
        hypr_hand_debug[35] = 5; /* unit is shared with a protected game sampler */
        return -1;
    }

    /* The replacement mesh carries its own five-column feather palette. Bind
     * that instead of recolouring the stock atlas; the active program still
     * supplies its original reflection, mask and screen-blend inputs. */
    if (g_mode == 3) {
        int previous_active = 0;
        int previous_texture = 0;
        g_get_integerv(GL_ACTIVE_TEXTURE, &previous_active);
        g_active_texture(GL_TEXTURE0 + (unsigned)sampler_unit);
        g_get_integerv(GL_TEXTURE_BINDING_2D, &previous_texture);
        build_arm_palette_texture();
        unsigned texture = 0;
        g_gen_textures(1, &texture);
        if (!texture) {
            g_active_texture((unsigned)previous_active);
            hypr_hand_debug[35] = 6;
            return -1;
        }
        g_bind_texture(GL_TEXTURE_2D, texture);
        g_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (int)GL_LINEAR);
        g_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (int)GL_LINEAR);
        g_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (int)GL_CLAMP_TO_EDGE);
        g_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (int)GL_REPEAT);
        g_tex_image_2d(GL_TEXTURE_2D, 0, (int)GL_RGBA8, ARM_PALETTE_WIDTH,
                       ARM_PALETTE_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                       g_arm_palette_pixels);
        g_bind_texture(GL_TEXTURE_2D, (unsigned)previous_texture);
        g_active_texture((unsigned)previous_active);

        material_state *state = &g_materials[g_material_count];
        state->program = program;
        state->gradient_sampler = gradient_sampler;
        state->gradient_unit = sampler_unit;
        state->gradient_texture = texture;
        state->palette_kind = 3;
        ++g_material_count;
        hypr_hand_debug[35] = 14; /* authored arm palette bound */
        hypr_hand_debug[20] = (unsigned long long)(gradient_sampler + 1);
        hypr_hand_debug[21] = texture;
        hypr_hand_debug[22] = (unsigned long long)sampler_unit;
        hypr_hand_debug[23] = 1;
        return (int)(g_material_count - 1);
    }

    int previous_active = 0;
    int source_texture = 0;
    int width = 0, height = 0, max_size = 0;
    int min_filter = (int)GL_LINEAR;
    int mag_filter = (int)GL_LINEAR;
    int wrap_s = (int)GL_CLAMP_TO_EDGE;
    int wrap_t = (int)GL_REPEAT;
    g_get_integerv(GL_ACTIVE_TEXTURE, &previous_active);
    g_active_texture(GL_TEXTURE0 + (unsigned)sampler_unit);
    g_get_integerv(GL_TEXTURE_BINDING_2D, &source_texture);
    if (source_texture != 0) {
        g_get_tex_level_parameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        g_get_tex_level_parameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
        g_get_integerv(GL_MAX_TEXTURE_SIZE, &max_size);
        if (g_get_tex_parameteriv) {
            g_get_tex_parameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &min_filter);
            g_get_tex_parameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &mag_filter);
            g_get_tex_parameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrap_s);
            g_get_tex_parameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wrap_t);
            /* The replacement contains level 0 only; mipmap filters would
             * make it incomplete, so use the source's linear base filter. */
            if (min_filter >= 0x2700 && min_filter <= 0x2703)
                min_filter = (int)GL_LINEAR;
        }
    }
    if (source_texture == 0) {
        g_active_texture((unsigned)previous_active);
        hypr_hand_debug[35] = 8; /* tex_grad unit has no source texture */
        return -1;
    }
    unsigned limit = palette_kind == 1 ? BLUE_TEXTURE_MAX_SIZE : BLUE_DIFFUSE_MAX_SIZE;
    if (width <= 0 || height <= 0 || width > (int)limit || height > (int)limit ||
        width > max_size || height > max_size) {
        g_active_texture((unsigned)previous_active);
        /* A viewport-sized tex_diffuse is the game's screen blend, not skin. */
        hypr_hand_debug[35] = palette_kind == 1 ? 9 : 12;
        return -1;
    }
    unsigned char *pixels = palette_kind == 1 ? g_blue_pixels : g_blue_diffuse_pixels;
    /* glGetTexImage follows global pack state. Normalize it and detach any
     * pixel-pack buffer so the destination is always the CPU scratch array,
     * then restore every value the game had configured. */
    int pack_alignment = 0, pack_row_length = 0, pack_skip_rows = 0, pack_skip_pixels = 0;
    int pack_buffer = 0;
    g_get_integerv(GL_PACK_ALIGNMENT, &pack_alignment);
    g_get_integerv(GL_PACK_ROW_LENGTH, &pack_row_length);
    g_get_integerv(GL_PACK_SKIP_ROWS, &pack_skip_rows);
    g_get_integerv(GL_PACK_SKIP_PIXELS, &pack_skip_pixels);
    g_get_integerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    g_bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
    g_pixel_storei(GL_PACK_ALIGNMENT, 1);
    g_pixel_storei(GL_PACK_ROW_LENGTH, 0);
    g_pixel_storei(GL_PACK_SKIP_ROWS, 0);
    g_pixel_storei(GL_PACK_SKIP_PIXELS, 0);
    g_get_tex_image(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    g_pixel_storei(GL_PACK_ALIGNMENT, pack_alignment);
    g_pixel_storei(GL_PACK_ROW_LENGTH, pack_row_length);
    g_pixel_storei(GL_PACK_SKIP_ROWS, pack_skip_rows);
    g_pixel_storei(GL_PACK_SKIP_PIXELS, pack_skip_pixels);
    g_bind_buffer(GL_PIXEL_PACK_BUFFER, (unsigned)pack_buffer);
    g_active_texture((unsigned)previous_active);
    if (palette_kind == 1)
        build_blue_pixels((unsigned)width, (unsigned)height);
    else
        build_blue_diffuse_pixels((unsigned)width, (unsigned)height);

    int previous_texture = 0;
    g_get_integerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    unsigned texture = 0;
    g_gen_textures(1, &texture);
    if (!texture) {
        hypr_hand_debug[35] = 6; /* GL did not allocate a texture object */
        return -1;
    }
    g_bind_texture(GL_TEXTURE_2D, texture);
    g_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
    g_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_filter);
    g_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_s);
    g_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_t);
    g_tex_image_2d(GL_TEXTURE_2D, 0, (int)GL_RGBA8, width, height, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, pixels);
    g_bind_texture(GL_TEXTURE_2D, (unsigned)previous_texture);

    material_state *state = &g_materials[g_material_count];
    state->program = program;
    state->gradient_sampler = gradient_sampler;
    state->gradient_unit = sampler_unit;
    state->gradient_texture = texture;
    state->palette_kind = palette_kind;
    ++g_material_count;
    hypr_hand_debug[35] = palette_kind == 1 ? 1 : 13; /* 1 = ramp, 13 = skin atlas */
    hypr_hand_debug[25] = palette_kind;
    hypr_hand_debug[26] = (unsigned long long)width;
    hypr_hand_debug[27] = (unsigned long long)height;
    hypr_hand_debug[20] = (unsigned long long)(gradient_sampler + 1);
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
    int gradient_unit = -1;
    if (g_mode == 3 && g_get_integerv && g_active_texture && g_bind_texture) {
        int current_program = 0;
        g_get_integerv(0x8B8Du, &current_program);
        hypr_hand_debug[34] = (unsigned long long)(unsigned)current_program;
        if (current_program > 0) {
            int state_index = material_state_for((unsigned)current_program);
            g_get_integerv(GL_ACTIVE_TEXTURE, &previous_active);
            if (state_index >= 0) {
                gradient_unit = g_materials[state_index].gradient_unit;
                g_active_texture(GL_TEXTURE0 + (unsigned)gradient_unit);
                g_get_integerv(GL_TEXTURE_BINDING_2D, &previous_texture);
                g_bind_texture(GL_TEXTURE_2D,
                               g_materials[state_index].gradient_texture);
                texture_bound = 1;
            } else {
                ++hypr_hand_debug[36];
            }
            hypr_hand_debug[38] = (unsigned long long)previous_active;
            hypr_hand_debug[39] = (unsigned long long)previous_texture;
        }
    }
    g_bind_vertex_array(g_vao);
    g_original_draw(GL_TRIANGLES, (int)g_geometry_count, GL_UNSIGNED_INT, NULL);
    g_bind_vertex_array((unsigned)previous);
    if (texture_bound) {
        g_active_texture(GL_TEXTURE0 + (unsigned)gradient_unit);
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

/* Palette mode leaves the game's VAO, indices, and posed vertex buffer in
 * place. It only redirects a dedicated tex_grad unit around the original draw,
 * so native animation, proportions, and draw state remain intact. */
static void draw_native_palette(unsigned mode, int count, unsigned type,
                                const void *indices) {
    /* Replacement modes resolve these while preparing a private VAO. Palette
     * mode keeps the engine VAO, so it must resolve GL itself on the first
     * owned draw, when the game's context is current. */
    if (!g_get_integerv || !g_active_texture || !g_bind_texture)
        resolve_gl();

    int texture_bound = 0;
    int previous_active = 0;
    int previous_texture = 0;
    int gradient_unit = -1;
    if (!g_get_integerv || !g_active_texture || !g_bind_texture) {
        hypr_hand_debug[35] = 2; /* GL state/bind entry point unavailable */
        g_original_draw(mode, count, type, indices);
        return;
    }

    int current_program = 0;
    g_get_integerv(0x8B8Du, &current_program); /* GL_CURRENT_PROGRAM */
    hypr_hand_debug[34] = (unsigned long long)(unsigned)current_program;
    if (current_program <= 0) hypr_hand_debug[35] = 7; /* no active shader program */
    if (current_program > 0) {
        int state_index = material_state_for((unsigned)current_program);
        g_get_integerv(GL_ACTIVE_TEXTURE, &previous_active);
        if (state_index >= 0) {
            gradient_unit = g_materials[state_index].gradient_unit;
            g_active_texture(GL_TEXTURE0 + (unsigned)gradient_unit);
            g_get_integerv(GL_TEXTURE_BINDING_2D, &previous_texture);
            g_bind_texture(GL_TEXTURE_2D, g_materials[state_index].gradient_texture);
            texture_bound = 1;
            hypr_hand_debug[38] = (unsigned long long)previous_active;
            hypr_hand_debug[39] = (unsigned long long)previous_texture;
        } else {
            ++hypr_hand_debug[36]; /* this native hand program has no safe gradient unit */
        }
    }
    g_original_draw(mode, count, type, indices);
    if (texture_bound) {
        g_active_texture(GL_TEXTURE0 + (unsigned)gradient_unit);
        g_bind_texture(GL_TEXTURE_2D, (unsigned)previous_texture);
        g_active_texture((unsigned)previous_active);
        ++hypr_hand_debug[32]; /* native draw used the custom gradient texture */
    }
}

/* Read the exact live mesh storage once, without changing its contents or the
 * VAO's element binding. glGetBufferSubData may synchronize with the driver,
 * so this runs for one hand draw only in the explicitly selected capture mode.
 * The render callback does no allocation, formatting, file I/O or host calls. */
static void capture_native_hand(unsigned count, unsigned type, const void *indices) {
    static const int expected_sizes[6] = {3, 3, 2, 4, 3, 3};
    static const unsigned expected_offsets[6] = {0, 12, 24, 32, 48, 60};
    if (InterlockedCompareExchange(&g_capture_claimed, 1, 0) != 0) return;
    g_capture_status = -1;
    resolve_capture_gl();
    if (count != HAND_INDEX_COUNT || type != GL_UNSIGNED_INT || !g_capture_event ||
        !g_get_integerv || !g_get_vertex_attribiv || !g_get_vertex_attrib_pointerv ||
        !g_get_buffer_parameteriv || !g_get_buffer_subdata || !g_bind_buffer) {
        g_capture_failure = 1;
        return;
    }

    int array_buffer = 0, element_buffer = 0;
    g_get_integerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &element_buffer);
    if (element_buffer <= 0) { g_capture_failure = 2; return; }

    for (unsigned i = 0; i < 6; ++i) {
        int enabled = 0, size = 0, stride = 0, attribute_type = 0, source = 0;
        void *pointer = NULL;
        g_get_vertex_attribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
        g_get_vertex_attribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
        g_get_vertex_attribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
        g_get_vertex_attribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attribute_type);
        g_get_vertex_attribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &source);
        g_get_vertex_attrib_pointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
        if (i == 0) array_buffer = source;
        if (!enabled || size != expected_sizes[i] || stride != 72 ||
            (unsigned)attribute_type != GL_FLOAT || source <= 0 || source != array_buffer ||
            (unsigned long long)(uintptr_t)pointer != expected_offsets[i])
            { g_capture_failure = 3; return; }
        g_capture_attribute_offsets[i] = expected_offsets[i];
    }

    unsigned long long index_offset = (unsigned long long)(uintptr_t)indices;
    if (index_offset > 0xffffffffull || (index_offset & 3u)) {
        g_capture_failure = 4;
        return;
    }
    int saved_array_buffer = 0, vertex_buffer_size = 0, index_buffer_size = 0;
    g_get_integerv(GL_ARRAY_BUFFER_BINDING, &saved_array_buffer);
    g_bind_buffer(GL_ARRAY_BUFFER, (unsigned)array_buffer);
    g_get_buffer_parameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &vertex_buffer_size);
    if (vertex_buffer_size < (int)HAND_VBO_BYTES) {
        g_bind_buffer(GL_ARRAY_BUFFER, (unsigned)saved_array_buffer);
        g_capture_failure = 5;
        return;
    }
    g_get_buffer_parameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &index_buffer_size);
    if (index_buffer_size < 0 || index_offset + HAND_CAPTURE_INDEX_BYTES >
                                     (unsigned)index_buffer_size) {
        g_bind_buffer(GL_ARRAY_BUFFER, (unsigned)saved_array_buffer);
        g_capture_failure = 6;
        return;
    }
    g_get_buffer_subdata(GL_ARRAY_BUFFER, 0, HAND_VBO_BYTES, g_capture_vertices);
    g_get_buffer_subdata(GL_ELEMENT_ARRAY_BUFFER, (long long)index_offset,
                         HAND_CAPTURE_INDEX_BYTES, g_capture_indices);
    g_bind_buffer(GL_ARRAY_BUFFER, (unsigned)saved_array_buffer);

    g_capture_index_offset = index_offset;
    g_capture_array_buffer = (unsigned)array_buffer;
    g_capture_element_buffer = (unsigned)element_buffer;
    g_capture_draw_ordinal = (unsigned)g_hand_draws;
    g_capture_timestamp_ms = GetTickCount();
    g_capture_status = 0;
    InterlockedExchange(&g_capture_ready, 1);
    if (!SetEvent(g_capture_event)) {
        g_capture_status = -1;
        g_capture_failure = 7;
        InterlockedExchange(&g_capture_saved, -1);
        return;
    }
    g_capture_status = 1; /* queued for the deferred writer */
}

/* ------------------------------------------------------ feather overlay */

static void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

/* The Windows target links without a math library, so normalise with a small
 * Newton iteration instead of pulling in sqrt(). */
static float local_sqrt(float value) {
    if (value <= 0.0f) return 0.0f;
    float guess = value > 1.0f ? value : 1.0f;
    for (unsigned i = 0; i < 16; ++i) guess = 0.5f * (guess + value / guess);
    return guess;
}

static void normalise3(float value[3]) {
    float length = value[0] * value[0] + value[1] * value[1] + value[2] * value[2];
    if (length <= 1e-12f) {
        value[0] = 0.0f; value[1] = 0.0f; value[2] = 1.0f;
        return;
    }
    length = local_sqrt(length);
    value[0] /= length; value[1] /= length; value[2] /= length;
}

/* Rotate the authored normal by the same shortest-arc rotation that takes the
 * native bind-pose normal to its current normal. Using the current native
 * normal directly (as the shell originally did) discards the authored
 * curvature and makes a dense mesh look like a flat card. */
static void rotate_between_normals(const float authored[3], const float from[3],
                                   const float to[3], float out[3]) {
    float source[3] = {authored[0], authored[1], authored[2]};
    normalise3(source);
    float axis[3];
    cross3(from, to, axis);
    float sine_sq = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
    float cosine = from[0] * to[0] + from[1] * to[1] + from[2] * to[2];
    if (cosine < -1.0f) cosine = -1.0f;
    if (cosine > 1.0f) cosine = 1.0f;
    if (sine_sq < 1e-10f) {
        if (cosine >= 0.0f) {
            out[0] = source[0]; out[1] = source[1]; out[2] = source[2];
            return;
        }
        /* 180 degrees: rotate around any axis perpendicular to the surface. */
        float perp[3] = {1.0f, 0.0f, 0.0f};
        if (from[1] * from[1] + from[2] * from[2] > 0.1f) {
            perp[0] = 0.0f; perp[1] = 1.0f; perp[2] = 0.0f;
        }
        float side[3];
        cross3(from, perp, side);
        normalise3(side);
        float along_side = source[0] * side[0] + source[1] * side[1] +
                           source[2] * side[2];
        out[0] = 2.0f * along_side * side[0] - source[0];
        out[1] = 2.0f * along_side * side[1] - source[1];
        out[2] = 2.0f * along_side * side[2] - source[2];
        normalise3(out);
        return;
    }
    float axis_dot_source = axis[0] * source[0] + axis[1] * source[1] +
                            axis[2] * source[2];
    float factor = (1.0f - cosine) / sine_sq;
    float rotated[3];
    rotated[0] = source[0] * cosine + axis[1] * source[2] - axis[2] * source[1] +
                 axis[0] * axis_dot_source * factor;
    rotated[1] = source[1] * cosine + axis[2] * source[0] - axis[0] * source[2] +
                 axis[1] * axis_dot_source * factor;
    rotated[2] = source[2] * cosine + axis[0] * source[1] - axis[1] * source[0] +
                 axis[2] * axis_dot_source * factor;
    normalise3(rotated);
    out[0] = rotated[0]; out[1] = rotated[1]; out[2] = rotated[2];
}

static void feather_write_record(unsigned index, const float position[3],
                                 const float normal[3]) {
    unsigned char *record = g_feather_vbo + (size_t)index * 72u;
    memcpy(record + 0, position, 12);
    memcpy(record + 12, normal, 12);
    memcpy(record + 24, g_feather_vertices[index].uv, 8);
    static const float uv1[4] = {0.023678f, 0.092198f, 0.0f, 0.0f};
    memcpy(record + 32, uv1, 16);
    float tangent[3], bitangent[3];
    static const float up[3] = {0.0f, 0.0f, 1.0f};
    cross3(up, normal, tangent);
    if (tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2] <
        1e-8f) {
        static const float fallback[3] = {1.0f, 0.0f, 0.0f};
        cross3(fallback, normal, tangent);
    }
    normalise3(tangent);
    cross3(normal, tangent, bitangent);
    normalise3(bitangent);
    memcpy(record + 48, tangent, 12);
    memcpy(record + 60, bitangent, 12);
}

static int load_feather_mesh(void) {
    char path[1024];
    if (!state_path(g_host, "feather-overlay.bin", path, sizeof path)) return 0;
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    size_t size = fread(g_feather_file, 1, FEATHER_MAX_BYTES, file);
    fclose(file);
    if (size < FEATHER_HEADER_BYTES || size >= FEATHER_MAX_BYTES) return 0;
    static const char magic[8] = {'H', 'Y', 'P', 'R', 'F', 'E', 'A', 'T'};
    for (unsigned i = 0; i < 8; ++i)
        if (g_feather_file[i] != (unsigned char)magic[i]) return 0;
    unsigned version = 0, vertices = 0, indices = 0, bind_count = 0;
    memcpy(&version, g_feather_file + 8, 4);
    memcpy(&vertices, g_feather_file + 12, 4);
    memcpy(&indices, g_feather_file + 16, 4);
    memcpy(&bind_count, g_feather_file + 20, 4);
    if (version != 1 || bind_count != 4 || !vertices || !indices ||
        vertices > FEATHER_MAX_VERTICES || indices > FEATHER_MAX_INDICES)
        return 0;
    if ((unsigned long)size != FEATHER_HEADER_BYTES +
        (unsigned long)vertices * FEATHER_VERTEX_BYTES + (unsigned long)indices * 4ul)
        return 0;

    for (unsigned i = 0; i < vertices; ++i) {
        const unsigned char *source = g_feather_file + FEATHER_HEADER_BYTES +
                                     (size_t)i * FEATHER_VERTEX_BYTES;
        feather_vertex *vertex = &g_feather_vertices[i];
        memcpy(vertex->position, source + 0, 12);
        memcpy(vertex->normal, source + 12, 12);
        memcpy(vertex->uv, source + 24, 8);
        memcpy(vertex->bind, source + 32, 16);
        memcpy(vertex->weight, source + 48, 16);
        float total = 0.0f;
        for (unsigned j = 0; j < 4; ++j) {
            if (vertex->bind[j] >= HAND_VERTEX_COUNT) return 0;
            total += vertex->weight[j];
        }
        if (total < 0.99f || total > 1.01f) return 0;
    }
    const unsigned char *source_indices = g_feather_file + FEATHER_HEADER_BYTES +
                                          (size_t)vertices * FEATHER_VERTEX_BYTES;
    for (unsigned i = 0; i < indices; ++i) {
        memcpy(&g_feather_indices[i], source_indices + (size_t)i * 4u, 4);
        if (g_feather_indices[i] >= vertices) return 0;
    }
    g_feather_vertex_count = vertices;
    g_feather_index_count = indices;
    hypr_hand_debug[40] = 1;
    hypr_hand_debug[41] = vertices;
    hypr_hand_debug[42] = indices;
    return 1;
}

static int load_bind_pose(void) {
    char path[1024];
    if (!state_path(g_host, "hand-bind.bin", path, sizeof path)) return 0;
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    size_t size = fread(g_bind_vbo, 1, HAND_VBO_BYTES, file);
    fclose(file);
    if (size != HAND_VBO_BYTES) return 0;
    g_bind_valid = 1;
    hypr_hand_debug[43] = 1;
    return 1;
}

static void prepare_feather_geometry(void) {
    if (g_feather_state != 0) return;
    g_feather_state = -1;
    resolve_gl();
    if (!g_bind_vertex_array || !g_bind_buffer || !g_buffer_data ||
        !g_vertex_attrib_pointer || !g_enable_vertex_attrib_array ||
        !g_get_integerv || !g_get_vertex_attribiv || !g_get_buffer_subdata ||
        !g_buffer_sub_data || !g_gen_buffers || !g_gen_vertex_arrays)
        return;
    for (unsigned i = 0; i < g_feather_vertex_count; ++i)
        feather_write_record(i, g_feather_vertices[i].position, g_feather_vertices[i].normal);

    unsigned array = 0, buffer = 0, element = 0;
    g_gen_vertex_arrays(1, &array);
    g_gen_buffers(1, &buffer);
    g_gen_buffers(1, &element);
    if (!array || !buffer || !element) return;
    int previous = 0;
    g_get_integerv(GL_VERTEX_ARRAY_BINDING, &previous);
    g_bind_vertex_array(array);
    g_bind_buffer(GL_ARRAY_BUFFER, buffer);
    g_buffer_data(GL_ARRAY_BUFFER, (long long)g_feather_vertex_count * 72,
                  g_feather_vbo, 0x88E8u); /* GL_DYNAMIC_DRAW */
    static const char *const names[5] = {"in_position", "in_normal", "in_uv0", "in_uv1",
                                          "in_tangent"};
    static const int sizes[5] = {3, 3, 2, 4, 3};
    static const int offsets[5] = {0, 12, 24, 32, 48};
    int program = 0;
    g_get_integerv(GL_CURRENT_PROGRAM, &program);
    for (unsigned i = 0; i < 5; ++i) {
        int location = (g_get_attrib_location && program > 0)
                           ? g_get_attrib_location((unsigned)program, names[i])
                           : (int)i;
        if (location < 0) location = (int)i;
        g_enable_vertex_attrib_array((unsigned)location);
        g_vertex_attrib_pointer((unsigned)location, sizes[i], GL_FLOAT, 0, 72,
                                (const void *)(size_t)offsets[i]);
    }
    g_bind_buffer(GL_ELEMENT_ARRAY_BUFFER, element);
    g_buffer_data(GL_ELEMENT_ARRAY_BUFFER, (long long)g_feather_index_count * 4,
                  g_feather_indices, 0x88E4u); /* GL_STATIC_DRAW */
    g_bind_vertex_array((unsigned)previous);
    g_feather_vao = array;
    g_feather_gl_vbo = buffer;
    g_feather_state = 1;
    hypr_hand_debug[48] = 1;
}

static unsigned long long float_bits(float value) {
    unsigned bits = 0;
    memcpy(&bits, &value, 4);
    return (unsigned long long)bits;
}

static int update_feather_pose(void) {
    if (!g_bind_valid) {
        int hand = 0;
        g_get_vertex_attribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &hand);
        if (!hand) return 0;
    }
    int previous = 0;
    g_get_integerv(GL_ARRAY_BUFFER_BINDING, &previous);
    int hand_buffer = 0;
    g_get_vertex_attribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &hand_buffer);
    if (!hand_buffer) return 0;
    g_bind_buffer(GL_ARRAY_BUFFER, (unsigned)hand_buffer);
    g_get_buffer_subdata(GL_ARRAY_BUFFER, 0, HAND_VBO_BYTES, g_native_vbo);
    g_bind_buffer(GL_ARRAY_BUFFER, (unsigned)previous);
    if (!g_bind_valid) {
        memcpy(g_bind_vbo, g_native_vbo, HAND_VBO_BYTES);
        g_bind_valid = 1;
        hypr_hand_debug[43] = 2;
    }

    float bounds_min[3] = {1.0e9f, 1.0e9f, 1.0e9f};
    float bounds_max[3] = {-1.0e9f, -1.0e9f, -1.0e9f};
    float max_delta = 0.0f;
    for (unsigned i = 0; i < g_feather_vertex_count; ++i) {
        const feather_vertex *vertex = &g_feather_vertices[i];
        float position[3] = {vertex->position[0], vertex->position[1], vertex->position[2]};
        float bind_normal[3] = {0.0f, 0.0f, 0.0f};
        float current_normal[3] = {0.0f, 0.0f, 0.0f};
        for (unsigned j = 0; j < 4; ++j) {
            unsigned index = vertex->bind[j];
            const float *current = (const float *)(g_native_vbo + (size_t)index * 72u);
            const float *bind = (const float *)(g_bind_vbo + (size_t)index * 72u);
            float weight = vertex->weight[j];
            for (unsigned axis = 0; axis < 3; ++axis) {
                position[axis] += weight * (current[axis] - bind[axis]);
                bind_normal[axis] += weight * bind[3 + axis];
                current_normal[axis] += weight * current[3 + axis];
            }
        }
        normalise3(bind_normal);
        normalise3(current_normal);
        float normal[3];
        rotate_between_normals(vertex->normal, bind_normal, current_normal, normal);
        feather_write_record(i, position, normal);
        for (unsigned axis = 0; axis < 3; ++axis) {
            if (position[axis] < bounds_min[axis]) bounds_min[axis] = position[axis];
            if (position[axis] > bounds_max[axis]) bounds_max[axis] = position[axis];
        }
        float delta = position[0] - vertex->position[0];
        if (delta < 0.0f) delta = -delta;
        if (delta > max_delta) max_delta = delta;
    }
    hypr_hand_debug[49] = float_bits(bounds_min[0]);
    hypr_hand_debug[50] = float_bits(bounds_min[1]);
    hypr_hand_debug[51] = float_bits(bounds_min[2]);
    hypr_hand_debug[52] = float_bits(bounds_max[0]);
    hypr_hand_debug[53] = float_bits(bounds_max[1]);
    hypr_hand_debug[54] = float_bits(bounds_max[2]);
    hypr_hand_debug[55] = float_bits(max_delta);

    g_bind_buffer(GL_ARRAY_BUFFER, g_feather_gl_vbo);
    g_buffer_sub_data(GL_ARRAY_BUFFER, 0, (long long)g_feather_vertex_count * 72,
                      g_feather_vbo);
    g_bind_buffer(GL_ARRAY_BUFFER, (unsigned)previous);
    ++hypr_hand_debug[44];
    return 1;
}

static int ensure_feather_palette(unsigned *out_texture) {
    if (!g_feather_palette_texture) {
        if (!g_gen_textures || !g_bind_texture || !g_tex_image_2d || !g_tex_parameteri ||
            !g_active_texture || !g_get_integerv)
            return 0;
        build_arm_palette_texture();
        int previous_active = 0, previous_texture = 0;
        g_get_integerv(GL_ACTIVE_TEXTURE, &previous_active);
        g_active_texture(GL_TEXTURE0);
        g_get_integerv(GL_TEXTURE_BINDING_2D, &previous_texture);
        unsigned texture = 0;
        g_gen_textures(1, &texture);
        if (!texture) {
            g_active_texture((unsigned)previous_active);
            return 0;
        }
        g_bind_texture(GL_TEXTURE_2D, texture);
        g_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (int)GL_LINEAR);
        g_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (int)GL_LINEAR);
        g_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (int)GL_CLAMP_TO_EDGE);
        g_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (int)GL_REPEAT);
        g_tex_image_2d(GL_TEXTURE_2D, 0, (int)GL_RGBA8, ARM_PALETTE_WIDTH,
                       ARM_PALETTE_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                       g_arm_palette_pixels);
        g_bind_texture(GL_TEXTURE_2D, (unsigned)previous_texture);
        g_active_texture((unsigned)previous_active);
        g_feather_palette_texture = texture;
        hypr_hand_debug[47] = texture;
    }
    *out_texture = g_feather_palette_texture;
    return g_feather_palette_texture != 0;
}

/* Recolor the native reflection arm colour once per source texture. The
 * reflection is the entity hand's base colour; replacing only this unit keeps
 * the screen-space hand blend and the remaining reflection/edge-specular
 * samplers native, so their moving gold highlights survive. */
static int ensure_feather_reflection(unsigned *out_texture, int *out_unit) {
    if (!g_get_integerv || !g_get_uniform_location || !g_get_uniformiv ||
        !g_active_texture || !g_bind_texture || !g_gen_textures ||
        !g_tex_image_2d || !g_tex_parameteri || !g_get_tex_level_parameteriv ||
        !g_get_tex_image || !g_pixel_storei)
        return 0;
    int program = 0;
    g_get_integerv(GL_CURRENT_PROGRAM, &program);
    if (program <= 0) return 0;
    int sampler = g_get_uniform_location((unsigned)program, "tex_reflection1");
    if (sampler < 0) return 0;
    int unit = -1;
    g_get_uniformiv((unsigned)program, sampler, &unit);
    if (unit < 0) return 0;

    int previous_active = 0, source = 0, width = 0, height = 0;
    g_get_integerv(GL_ACTIVE_TEXTURE, &previous_active);
    g_active_texture(GL_TEXTURE0 + (unsigned)unit);
    g_get_integerv(GL_TEXTURE_BINDING_2D, &source);
    if (source) {
        g_get_tex_level_parameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        g_get_tex_level_parameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    }
    if (source && g_feather_reflection_texture &&
        (unsigned)source == g_feather_reflection_source &&
        (unsigned)width == g_feather_reflection_width &&
        (unsigned)height == g_feather_reflection_height) {
        g_active_texture((unsigned)previous_active);
        *out_texture = g_feather_reflection_texture;
        *out_unit = unit;
        return 1;
    }
    if (!source || width < 1 || height < 1 ||
        width > REFLECTION_MAX_SIZE || height > REFLECTION_MAX_SIZE) {
        g_active_texture((unsigned)previous_active);
        hypr_hand_debug[63] = 1; /* missing or oversized reflection */
        return 0;
    }

    int pack_alignment = 0;
    g_get_integerv(GL_PACK_ALIGNMENT, &pack_alignment);
    g_pixel_storei(GL_PACK_ALIGNMENT, 1);
    if (g_get_error) (void)g_get_error();
    g_get_tex_image(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_reflection_pixels);
    unsigned error = g_get_error ? g_get_error() : 0;
    g_pixel_storei(GL_PACK_ALIGNMENT, pack_alignment);
    if (error) {
        g_active_texture((unsigned)previous_active);
        hypr_hand_debug[63] = 2; /* source is not a readable 2D reflection */
        return 0;
    }

    build_blue_reflection_into(g_reflection_pixels, (unsigned)width, (unsigned)height);
    unsigned texture = 0;
    g_gen_textures(1, &texture);
    if (!texture) {
        g_active_texture((unsigned)previous_active);
        hypr_hand_debug[63] = 3;
        return 0;
    }
    g_bind_texture(GL_TEXTURE_2D, texture);
    g_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (int)GL_LINEAR);
    g_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (int)GL_LINEAR);
    g_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (int)GL_CLAMP_TO_EDGE);
    g_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (int)GL_CLAMP_TO_EDGE);
    g_tex_image_2d(GL_TEXTURE_2D, 0, (int)GL_RGBA8, width, height, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, g_reflection_pixels);
    g_bind_texture(GL_TEXTURE_2D, (unsigned)source);
    g_active_texture((unsigned)previous_active);
    g_feather_reflection_texture = texture;
    g_feather_reflection_source = (unsigned)source;
    g_feather_reflection_width = (unsigned)width;
    g_feather_reflection_height = (unsigned)height;
    hypr_hand_debug[56] = (unsigned long long)(unsigned)program;
    hypr_hand_debug[57] = 1;
    hypr_hand_debug[58] = (unsigned long long)(unsigned)unit;
    hypr_hand_debug[59] = texture;
    hypr_hand_debug[60] = (unsigned long long)(unsigned)source;
    hypr_hand_debug[61] = (unsigned long long)(unsigned)width;
    hypr_hand_debug[62] = (unsigned long long)(unsigned)height;
    hypr_hand_debug[63] = 0;
    *out_texture = texture;
    *out_unit = unit;
    return 1;
}

static void draw_feather_overlay(void) {
    if (g_feather_state == 0) prepare_feather_geometry();
    if (g_feather_state != 1) return;
    if (!update_feather_pose()) return;
    int program = 0;
    g_get_integerv(GL_CURRENT_PROGRAM, &program);
    unsigned reflection_texture = 0;
    int reflection_unit = -1;
    int have_reflection = ensure_feather_reflection(&reflection_texture, &reflection_unit);
    unsigned palette_texture = 0;
    int have_palette = 0;
    int previous_vao = 0, previous_active = 0, previous_texture = 0;
    int previous_textures[8] = {0};
    int extra_units[2] = {-1, -1};
    unsigned extra_textures[2] = {0, 0};
    int extra_count = 0;
    int bound_units = 0;
    g_get_integerv(GL_VERTEX_ARRAY_BINDING, &previous_vao);
    if (have_reflection && reflection_unit >= 0) {
        static const char *const extra_samplers[2] = {
            "tex_reflection2", "tex_reflection3"
        };
        g_get_integerv(GL_ACTIVE_TEXTURE, &previous_active);
        for (int i = 0; i < 2; ++i) {
            int location = g_get_uniform_location((unsigned)program, extra_samplers[i]);
            if (location < 0) continue;
            int unit = -1;
            g_get_uniformiv((unsigned)program, location, &unit);
            if (unit < 0 || unit == reflection_unit) continue;
            int duplicate = 0;
            for (int j = 0; j < extra_count; ++j)
                if (extra_units[j] == unit) duplicate = 1;
            if (duplicate) continue;
            g_active_texture(GL_TEXTURE0 + (unsigned)unit);
            g_get_integerv(GL_TEXTURE_BINDING_2D, &previous_textures[extra_count]);
            g_bind_texture(GL_TEXTURE_2D, reflection_texture);
            extra_units[extra_count] = unit;
            extra_textures[extra_count] = (unsigned)previous_textures[extra_count];
            ++extra_count;
        }
        g_active_texture(GL_TEXTURE0 + (unsigned)reflection_unit);
        g_get_integerv(GL_TEXTURE_BINDING_2D, &previous_texture);
        g_bind_texture(GL_TEXTURE_2D, reflection_texture);
        hypr_hand_debug[56] = (unsigned long long)(unsigned)program;
        hypr_hand_debug[57] = 1;
        hypr_hand_debug[58] = (unsigned long long)(unsigned)reflection_unit;
        hypr_hand_debug[59] = reflection_texture;
    } else {
        /* If the reflection is not a readable 2D texture, keep the authored
         * mesh visible with the original all-unit palette rather than
         * falling back to the native gold arm. */
        have_palette = ensure_feather_palette(&palette_texture);
        if (have_palette) {
            int max_units = 0;
            g_get_integerv(0x8872, &max_units); /* GL_MAX_TEXTURE_IMAGE_UNITS */
            if (max_units < 1) max_units = 1;
            if (max_units > 8) max_units = 8;
            g_get_integerv(GL_ACTIVE_TEXTURE, &previous_active);
            for (int unit = 0; unit < max_units; ++unit) {
                g_active_texture(GL_TEXTURE0 + (unsigned)unit);
                g_get_integerv(GL_TEXTURE_BINDING_2D, &previous_textures[unit]);
                g_bind_texture(GL_TEXTURE_2D, palette_texture);
            }
            bound_units = max_units;
            hypr_hand_debug[56] = (unsigned long long)(unsigned)program;
            hypr_hand_debug[57] = 2;
            hypr_hand_debug[58] = (unsigned long long)(unsigned)max_units;
            hypr_hand_debug[59] = palette_texture;
        }
    }
    unsigned char cull_on = 0;
    unsigned char depth_on = 0;
    if (g_is_enabled && g_disable) {
        cull_on = g_is_enabled(GL_CULL_FACE);
        g_disable(GL_CULL_FACE);
        if (g_mode == 5) {
            /* In combined mode the native gold arm has already written depth.
             * The authored surface is not coplanar, so draw over it. */
            depth_on = g_is_enabled(GL_DEPTH_TEST);
            g_disable(GL_DEPTH_TEST);
        }
        /* Authored-only mode draws with the depth test left on: the native
         * hand wrote no depth, so the authored layers occlude each other
         * correctly instead of stacking into one feather mass. */
    }
    g_bind_vertex_array(g_feather_vao);
    g_original_draw(GL_TRIANGLES, (int)g_feather_index_count, GL_UNSIGNED_INT, NULL);
    g_bind_vertex_array((unsigned)previous_vao);
    if (g_enable && cull_on) g_enable(GL_CULL_FACE);
    if (g_enable && depth_on) g_enable(GL_DEPTH_TEST);
    if (have_reflection) {
        for (int i = 0; i < extra_count; ++i) {
            g_active_texture(GL_TEXTURE0 + (unsigned)extra_units[i]);
            g_bind_texture(GL_TEXTURE_2D, extra_textures[i]);
        }
        g_active_texture(GL_TEXTURE0 + (unsigned)reflection_unit);
        g_bind_texture(GL_TEXTURE_2D, (unsigned)previous_texture);
        g_active_texture((unsigned)previous_active);
    }
    if (have_palette) {
        for (int unit = 0; unit < bound_units; ++unit) {
            g_active_texture(GL_TEXTURE0 + (unsigned)unit);
            g_bind_texture(GL_TEXTURE_2D, (unsigned)previous_textures[unit]);
        }
        g_active_texture((unsigned)previous_active);
    }
    ++hypr_hand_debug[45];
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
        if (g_mode == 7) capture_native_hand((unsigned)count, type, indices);
        if (g_mode == 2 || g_mode == 3) {
            prepare_geometry();
            if (g_geometry_state == 1) {
                draw_substitute();
                return;
            }
        }
        if (g_mode == 4) {
            draw_native_palette(mode, count, type, indices);
            return;
        }
        if (g_mode == 6) {
            /* Diagnostic: suppress the stock arm but keep its posed VBO so the
             * feather shell can be inspected, fitted and palette-checked on
             * its own. */
            g_original_draw(mode, 0, type, indices);
            if ((g_hand_draws & 1u) != 0) draw_feather_overlay();
            return;
        }
        if (g_mode == 5) {
            /* Keep the game's own animated arm draw; add the authored feather
             * shell after the first color pass, whose program owns the skin and
             * mask samplers. The second pass is the screen blend. */
            g_original_draw(mode, count, type, indices);
            if ((g_hand_draws & 1u) != 0) draw_feather_overlay();
            return;
        }
        if (g_mode == 1) count = 0;
    }
    g_original_draw(mode, count, type, indices);
}

static int executable_address(const void *address) {
    MEMORY_BASIC_INFORMATION64A info;
    if (!address || VirtualQuery(address, &info, sizeof info) != sizeof info) return 0;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
    return (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY)) != 0;
}

static DWORD WINAPI dispatch_ready_worker(void *unused) {
    (void)unused;
    for (unsigned attempt = 0; attempt < HAND_DISPATCH_RETRY_COUNT; ++attempt) {
        if (InterlockedCompareExchange(&g_dispatch_stop, 0, 0)) break;
        void *candidate = NULL;
        if (g_host->image_read(g_host->context, HAND_DISPATCH_RVA,
                               &candidate, sizeof candidate) == 0 &&
            hypr_hand_dispatch_executable_candidate(
                (int)InterlockedCompareExchange(&g_dispatch_state, 0, 0),
                InterlockedCompareExchange(&g_dispatch_stop, 0, 0) != 0,
                candidate, (void *)&hand_draw_shim, executable_address(candidate))) {
            LONG previous = InterlockedCompareExchange(&g_dispatch_state, 1, 0);
            if (previous != 0) return 0;

            /* A renderer may have initialized the slot between polling and
             * claiming it. Never overwrite a different dispatch target. */
            void *current = NULL;
            if (InterlockedCompareExchange(&g_dispatch_stop, 0, 0) ||
                g_host->image_read(g_host->context, HAND_DISPATCH_RVA,
                                   &current, sizeof current) != 0 ||
                current != candidate) {
                InterlockedExchange(&g_dispatch_state, HYPR_HAND_DISPATCH_FAILED);
                log_message("warn", "draw dispatch changed during readiness check; hook not installed");
                return 0;
            }

            g_original_draw = (draw_elements_fn)candidate;
            g_active = 1;
            void *replacement = (void *)&hand_draw_shim;
            if (g_host->image_write(g_host->context, HAND_DISPATCH_RVA,
                                    &replacement, sizeof replacement) != 0) {
                g_active = 0;
                InterlockedExchange(&g_dispatch_state, hypr_hand_dispatch_finish_install(
                    HYPR_HAND_DISPATCH_INSTALLING, 0, 0));
                log_message("error", "draw dispatch became ready but patch failed");
                return 0;
            }
            void *check = NULL;
            if (g_host->image_read(g_host->context, HAND_DISPATCH_RVA,
                                   &check, sizeof check) != 0 || check != replacement) {
                g_active = 0;
                InterlockedExchange(&g_dispatch_state, hypr_hand_dispatch_finish_install(
                    HYPR_HAND_DISPATCH_INSTALLING, 1, 0));
                log_message("error", "draw dispatch patch verification failed");
                return 0;
            }
            InterlockedExchange(&g_dispatch_state, hypr_hand_dispatch_finish_install(
                HYPR_HAND_DISPATCH_INSTALLING, 1, 1));
            char message[192];
            snprintf(message, sizeof message,
                     "dispatch hook installed after readiness (rva=0x3d9ac8 original=0x%lx mode=%s attempts=%u)",
                     (unsigned long)(size_t)candidate, mode_name(g_mode), attempt + 1u);
            log_message("info", message);
            return 0;
        }
        Sleep(HAND_DISPATCH_RETRY_MS);
    }
    if (!InterlockedCompareExchange(&g_dispatch_stop, 0, 0)) {
        LONG state = InterlockedCompareExchange(&g_dispatch_state, 0, 0);
        InterlockedExchange(&g_dispatch_state, hypr_hand_dispatch_timeout(
            (int)state, 0));
        log_message("warn", "draw dispatch did not become executable within 30 seconds; hook inactive");
    }
    return 0;
}

static int mod_initialize(const hyprlodr_host_api *host) {
    g_host = host;
    if (g_dispatch_thread) {
        log_message("error", "dispatch readiness worker is already active");
        return -1;
    }
    InterlockedExchange(&g_dispatch_stop, 0);
    InterlockedExchange(&g_dispatch_state, HYPR_HAND_DISPATCH_WAITING);
    g_active = 0;

    if (!HYPRLODR_HOST_API_HAS(host, image_write) || !host->image_write ||
        !host->image_read) {
        log_message("warn", "host has no image access; hand hook inactive");
        return 0;
    }
    if (!host->build_matches(host->context)) {
        log_message("warn", "build fingerprint does not match; hand hook inactive");
        return 0;
    }

    g_mode = read_mode(host);
    InterlockedExchange(&g_capture_claimed, 0);
    InterlockedExchange(&g_capture_ready, 0);
    InterlockedExchange(&g_capture_saved, 0);
    InterlockedExchange(&g_capture_stop, 0);
    g_capture_status = 0;
    g_capture_failure = 0;
    g_capture_draw_ordinal = 0;
    build_cube();
    if (g_mode == 7 && !start_capture_writer()) {
        log_message("error", "capture mode cannot start without its deferred writer");
        return -1;
    }
    if (g_mode == 5 || g_mode == 6) {
        int mesh_ok = load_feather_mesh();
        int bind_ok = load_bind_pose();
        if (mesh_ok)
            log_message("info", "overlay mode: feather mesh loaded; native arm stays live");
        else
            log_message("warn", "overlay mode: feather mesh missing/invalid; native arm only");
        if (!bind_ok)
            log_message("info", "overlay mode: no hand-bind.bin; first live pose becomes bind");
    }
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

    g_dispatch_thread = CreateThread(NULL, 0, dispatch_ready_worker, NULL, 0, NULL);
    if (!g_dispatch_thread) {
        g_mode = 0;
        log_message("error", "could not start bounded draw dispatch readiness worker");
        return -1;
    }
    log_message("info", "capture mode configured; waiting up to 30 seconds for executable draw dispatch");
    return 0;
}

static void mod_shutdown(const hyprlodr_host_api *host) {
    InterlockedExchange(&g_dispatch_stop, 1);
    InterlockedCompareExchange(&g_dispatch_state,
                               hypr_hand_dispatch_stop((int)InterlockedCompareExchange(
                                   &g_dispatch_state, 0, 0)), 0);
    if (g_dispatch_thread) {
        (void)WaitForSingleObject(g_dispatch_thread, INFINITE);
        CloseHandle(g_dispatch_thread);
        g_dispatch_thread = NULL;
    }
    g_active = 0;
    if (g_mode == 7 && g_capture_event) {
        InterlockedExchange(&g_capture_stop, 1);
        (void)SetEvent(g_capture_event);
        if (g_capture_thread) {
            (void)WaitForSingleObject(g_capture_thread, INFINITE);
            CloseHandle(g_capture_thread);
            g_capture_thread = NULL;
        }
        CloseHandle(g_capture_event);
        g_capture_event = NULL;
    }
    size_t restored = 0;
    if (HYPRLODR_HOST_API_HAS(host, image_restore) && host->image_restore)
        restored = host->image_restore(host->context);

    char message[224];
    if (g_mode == 7) {
        snprintf(message, sizeof message,
                 "capture mode: dispatch_state=%ld draws=%lu capture_status=%d failure=%u writer_status=%ld; hook restored %lu byte(s)",
                 (long)InterlockedCompareExchange(&g_dispatch_state, 0, 0),
                 g_hand_draws, g_capture_status, g_capture_failure,
                 (long)InterlockedCompareExchange(&g_capture_saved, 0, 0),
                 (unsigned long)restored);
    } else {
        snprintf(message, sizeof message,
                 "hook removed after %lu hand draws (%lu substituted); %lu image byte(s) restored",
                 g_hand_draws, g_substituted, (unsigned long)restored);
    }
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
