/*
 * Title overlay (mod-gated title screen).
 *
 * The game's own title art lives inside the packed asset container, whose format
 * is not decoded yet, so replacing the rendered title screen directly is not
 * possible today. What *is* possible, and is what this implements, is the layer
 * above it: a topmost, click-through, per-pixel-alpha window owned by the game
 * process, sized and positioned over the game window, showing the loader's own
 * title art while at least one mod is loaded.
 *
 * Semantics:
 *   - the proxy calls this only after mods initialized, and only when at least
 *     one mod did, so a run with no mods never shows anything;
 *   - the window does not take focus and is click-through, so the game's own menu
 *     underneath stays usable;
 *   - it disappears on the first key or mouse press (after a short grace period
 *     so the launch click cannot dismiss it) or after `duration_ms`.
 *
 * The image is a 24- or 32-bit uncompressed BMP. Bright pixels become opaque and
 * near-black pixels become transparent, which is what makes the wordmark sit over
 * the game instead of hiding it behind a black rectangle.
 */
#define HYPRLODR_MINIMAL_WIN32 1

#include "hypr_win32.h"
#include "title_overlay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TITLE_CLASS_NAME "HYPRLODRTitleOverlay"
#define TITLE_MAX_BYTES (64u * 1024u * 1024u)
#define TITLE_INPUT_GRACE_MS 900u
#define TITLE_MAX_LIFETIME_MS 600000u
#define TITLE_POLL_MS 30u

static const char *g_status = "not_started";
static HWND g_target;

static void log_line(const char *path, const char *status, int width, int height) {
    FILE *file = fopen(path, "a");
    if (!file) return;
    fprintf(file, "{\"event\":\"title_overlay\",\"status\":\"%s\",\"width\":%d,\"height\":%d}\n",
            status, width, height);
    fclose(file);
}

/* Largest visible window owned by this process: that is the game, and it is the
 * rectangle the overlay has to cover. */
static BOOL CALLBACK find_window(HWND window, LPARAM unused) {
    (void)unused;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(window)) return TRUE;
    RECT rect;
    if (!GetWindowRect(window, &rect)) return TRUE;
    LONG width = rect.right - rect.left;
    LONG height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return TRUE;
    if (g_target) {
        RECT current;
        if (GetWindowRect(g_target, &current)) {
            LONG current_area = (current.right - current.left) * (current.bottom - current.top);
            if (current_area >= width * height) return TRUE;
        }
    }
    g_target = window;
    return TRUE;
}

static unsigned long long read_le(const unsigned char *bytes, int length) {
    unsigned long long value = 0;
    for (int index = length - 1; index >= 0; --index) value = (value << 8) | bytes[index];
    return value;
}

typedef struct {
    int width;
    int height;
    unsigned char *pixels; /* premultiplied BGRA, top-down */
} title_image;

static void free_image(title_image *image) {
    if (!image) return;
    free(image->pixels);
    image->pixels = NULL;
}

/* Decode an uncompressed BMP into premultiplied BGRA. Alpha is the source's own
 * when the file carries a used alpha channel, otherwise the pixel's brightness,
 * so the black background of the art disappears. */
static int load_bmp(const char *path, title_image *image) {
    memset(image, 0, sizeof *image);
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return -1;
    DWORD size = GetFileSize(file, NULL);
    if (size < 54u || size > TITLE_MAX_BYTES) {
        CloseHandle(file);
        return -2;
    }
    unsigned char *raw = (unsigned char *)malloc(size);
    if (!raw) {
        CloseHandle(file);
        return -3;
    }
    DWORD read = 0;
    BOOL ok = ReadFile(file, raw, size, &read, NULL);
    CloseHandle(file);
    if (!ok || read != size) {
        free(raw);
        return -4;
    }
    if (raw[0] != 'B' || raw[1] != 'M') {
        free(raw);
        return -5;
    }

    unsigned long long data_offset = read_le(raw + 10, 4);
    long long width = (long long)read_le(raw + 18, 4);
    long long raw_height = (long long)read_le(raw + 22, 4);
    unsigned bits = (unsigned)read_le(raw + 28, 2);
    unsigned compression = (unsigned)read_le(raw + 30, 4);
    if (compression != 0u || (bits != 24u && bits != 32u) || width <= 0 || raw_height == 0 ||
        width > 16384 || raw_height > 16384 || raw_height < -16384) {
        free(raw);
        return -6;
    }
    int top_down = raw_height < 0;
    long long height = raw_height < 0 ? -raw_height : raw_height;
    size_t stride = (((size_t)width * bits + 31u) / 32u) * 4u;
    unsigned long long needed = data_offset + (unsigned long long)stride * (unsigned long long)height;
    if (needed > (unsigned long long)size) {
        free(raw);
        return -7;
    }

    size_t pixels = (size_t)width * (size_t)height * 4u;
    unsigned char *out = (unsigned char *)malloc(pixels);
    if (!out) {
        free(raw);
        return -3;
    }

    int alpha_used = 0;
    if (bits == 32u) {
        for (size_t index = 3; index < pixels; index += 4u) {
            if (out[index] != 0u) {
                alpha_used = 1;
                break;
            }
        }
    }

    for (long long row = 0; row < height; ++row) {
        long long source_row = top_down ? row : height - 1 - row;
        const unsigned char *line = raw + data_offset + (size_t)source_row * stride;
        unsigned char *target = out + (size_t)row * (size_t)width * 4u;
        for (long long column = 0; column < width; ++column) {
            const unsigned char *source = line + (size_t)column * (bits / 8u);
            unsigned alpha;
            if (bits == 32u && alpha_used) {
                alpha = source[3];
            } else {
                unsigned brightest = source[0];
                if (source[1] > brightest) brightest = source[1];
                if (source[2] > brightest) brightest = source[2];
                /* Lift the very dark end: the art's background is black, the glow
                 * around the letters is not, and a hard threshold would cut it. */
                alpha = brightest > 8u ? (brightest > 200u ? 255u : brightest + 40u) : 0u;
                if (alpha > 255u) alpha = 255u;
            }
            target[column * 4 + 0] = (unsigned char)((unsigned)source[0] * alpha / 255u);
            target[column * 4 + 1] = (unsigned char)((unsigned)source[1] * alpha / 255u);
            target[column * 4 + 2] = (unsigned char)((unsigned)source[2] * alpha / 255u);
            target[column * 4 + 3] = (unsigned char)alpha;
        }
    }

    free(raw);
    image->width = (int)width;
    image->height = (int)height;
    image->pixels = out;
    return 0;
}

/* Nearest-neighbour scale into premultiplied BGRA of the target size. */
static unsigned char *scale_image(const title_image *image, int width, int height) {
    if (width <= 0 || height <= 0) return NULL;
    unsigned char *out = (unsigned char *)malloc((size_t)width * (size_t)height * 4u);
    if (!out) return NULL;
    for (int row = 0; row < height; ++row) {
        int source_row = (int)((long long)row * image->height / height);
        if (source_row >= image->height) source_row = image->height - 1;
        for (int column = 0; column < width; ++column) {
            int source_column = (int)((long long)column * image->width / width);
            if (source_column >= image->width) source_column = image->width - 1;
            const unsigned char *source =
                image->pixels + ((size_t)source_row * (size_t)image->width + (size_t)source_column) * 4u;
            unsigned char *target = out + ((size_t)row * (size_t)width + (size_t)column) * 4u;
            memcpy(target, source, 4u);
        }
    }
    return out;
}

static LRESULT WINAPI title_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcA(window, message, wparam, lparam);
}

static int any_input_pressed(void) {
    for (int key = 1; key < 256; ++key) {
        if (key == 0x0C) continue; /* VK_CLEAR is not a real key press */
        if ((GetAsyncKeyState(key) & 0x8000) != 0) return 1;
    }
    return 0;
}

static DWORD WINAPI overlay_thread(LPVOID parameter) {
    title_overlay_args *args = (title_overlay_args *)parameter;
    int width = 0;
    int height = 0;

    title_image image;
    if (load_bmp(args->image_path, &image) != 0) {
        g_status = "image_invalid";
        log_line(args->log_path, g_status, 0, 0);
        free(args);
        return 0;
    }

    /* Give the game a moment to create its window before measuring it. */
    for (int attempt = 0; attempt < 100 && !g_target; ++attempt) {
        EnumWindows(find_window, 0);
        if (!g_target) Sleep(50);
    }
    if (!g_target) {
        g_status = "no_window";
        log_line(args->log_path, g_status, 0, 0);
        free_image(&image);
        free(args);
        return 0;
    }

    RECT rect;
    if (!GetWindowRect(g_target, &rect)) {
        g_status = "no_rect";
        log_line(args->log_path, g_status, 0, 0);
        free_image(&image);
        free(args);
        return 0;
    }
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    unsigned char *scaled = scale_image(&image, width, height);
    if (!scaled) {
        g_status = "scale_failed";
        log_line(args->log_path, g_status, 0, 0);
        free_image(&image);
        free(args);
        return 0;
    }

    WNDCLASSA window_class;
    memset(&window_class, 0, sizeof window_class);
    window_class.lpfnWndProc = title_window_proc;
    window_class.hInstance = GetModuleHandleA(NULL);
    window_class.lpszClassName = TITLE_CLASS_NAME;
    RegisterClassA(&window_class);

    HWND overlay = CreateWindowExA(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
                                       WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                   TITLE_CLASS_NAME, "HYPRLODR", WS_POPUP, rect.left, rect.top, width,
                                   height, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!overlay) {
        g_status = "create_failed";
        log_line(args->log_path, g_status, 0, 0);
        free(scaled);
        free_image(&image);
        free(args);
        return 0;
    }

    HDC screen = GetDC(NULL);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info;
    memset(&info, 0, sizeof info);
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; /* top-down */
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = 0;
    void *bits = NULL;
    HBITMAP bitmap = CreateDIBSection(memory, &info, 0, &bits, NULL, 0);
    int shown = 0;
    if (bitmap && bits) {
        memcpy(bits, scaled, (size_t)width * (size_t)height * 4u);
        HGDIOBJ previous = SelectObject(memory, bitmap);
        POINT destination;
        destination.x = rect.left;
        destination.y = rect.top;
        SIZE size;
        size.cx = width;
        size.cy = height;
        POINT source;
        source.x = 0;
        source.y = 0;
        BLENDFUNCTION blend;
        blend.BlendOp = 0;      /* AC_SRC_OVER */
        blend.BlendFlags = 0;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = 1;  /* AC_SRC_ALPHA */
        if (UpdateLayeredWindow(overlay, screen, &destination, &size, memory, &source, 0, &blend, 2)) {
            ShowWindow(overlay, 4); /* SW_SHOWNOACTIVATE */
            SetWindowPos(overlay, (HWND)(long long)-1, rect.left, rect.top, width, height,
                         0x0010u | 0x0040u); /* SWP_NOACTIVATE | SWP_SHOWWINDOW */
            shown = 1;
            g_status = "shown";
            log_line(args->log_path, g_status, width, height);
        }
        SelectObject(memory, previous);
    }
    if (!shown && !g_status[0]) g_status = "draw_failed";

    if (shown) {
        DWORD started = GetTickCount();
        unsigned lifetime = args->duration_ms ? args->duration_ms : TITLE_MAX_LIFETIME_MS;
        if (lifetime > TITLE_MAX_LIFETIME_MS) lifetime = TITLE_MAX_LIFETIME_MS;
        for (;;) {
            Sleep(TITLE_POLL_MS);
            DWORD elapsed = GetTickCount() - started;
            if (elapsed >= lifetime) break;
            if (elapsed < TITLE_INPUT_GRACE_MS) continue;
            if (any_input_pressed()) break;
        }
        ShowWindow(overlay, 0);
        log_line(args->log_path, "hidden", width, height);
    }

    if (bitmap) DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(NULL, screen);
    DestroyWindow(overlay);
    free(scaled);
    free_image(&image);
    free(args);
    return 0;
}

int hyprlodr_title_overlay_start(const char *image_path, const char *log_path, unsigned duration_ms) {
    static int started;
    if (started) return 0;
    if (!image_path || !*image_path || !log_path) {
        g_status = "invalid_argument";
        return -1;
    }
    title_overlay_args *args = (title_overlay_args *)malloc(sizeof *args);
    if (!args) {
        g_status = "no_memory";
        return -1;
    }
    snprintf(args->image_path, sizeof args->image_path, "%s", image_path);
    snprintf(args->log_path, sizeof args->log_path, "%s", log_path);
    args->duration_ms = duration_ms;
    started = 1;
    g_status = "starting";
    HANDLE thread = CreateThread(NULL, 0, overlay_thread, args, 0, NULL);
    if (!thread) {
        g_status = "thread_failed";
        free(args);
        return -1;
    }
    CloseHandle(thread);
    return 0;
}

const char *hyprlodr_title_overlay_status(void) { return g_status; }
