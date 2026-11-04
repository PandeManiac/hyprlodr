#include "hypr_win32.h"

#include <stdio.h>
#include <string.h>

typedef BOOL (WINAPI *enum_windows_fn)(BOOL (*callback)(HWND, LPARAM), LPARAM parameter);
typedef BOOL (WINAPI *is_visible_fn)(HWND window);
typedef int (WINAPI *get_text_fn)(HWND window, LPSTR text, int max_count);
typedef int (WINAPI *get_class_fn)(HWND window, LPSTR text, int max_count);
typedef BOOL (WINAPI *get_client_fn)(HWND window, RECT *rect);
typedef BOOL (WINAPI *client_to_screen_fn)(HWND window, POINT *point);
typedef DWORD (WINAPI *get_thread_fn)(HWND window, DWORD *process_id);
typedef int (WINAPI *get_metrics_fn)(int index);
typedef HWND (WINAPI *foreground_fn)(void);
typedef BOOL (WINAPI *set_foreground_fn)(HWND window);
typedef BOOL (WINAPI *set_cursor_fn)(int x, int y);
typedef void (WINAPI *mouse_event_fn)(DWORD flags, DWORD dx, DWORD dy, DWORD data,
                                      ULONG_PTR extra);
typedef void (WINAPI *keybd_event_fn)(BYTE key, BYTE scan, DWORD flags,
                                      ULONG_PTR extra);

static enum_windows_fn p_enum_windows;
static is_visible_fn p_is_visible;
static get_text_fn p_get_text;
static get_class_fn p_get_class;
static get_client_fn p_get_client;
static client_to_screen_fn p_client_to_screen;
static get_thread_fn p_get_thread;
static get_metrics_fn p_get_metrics;
static foreground_fn p_get_foreground;
static set_foreground_fn p_set_foreground;
static set_cursor_fn p_set_cursor;
static mouse_event_fn p_mouse_event;
static keybd_event_fn p_keybd_event;

static HWND g_target;
static DWORD g_target_pid;
static char g_target_module[512];
static const char *g_match_title = "hyperdemon";
static const char *g_match_module = "hyperdemon.exe";
static const char *g_expect_title;
static const char *g_report_path;

static void copy_text(char *destination, const char *source, size_t capacity) {
    size_t i = 0;
    if (!capacity) return;
    while (source[i] && i + 1 < capacity) {
        destination[i] = source[i];
        ++i;
    }
    destination[i] = '\0';
}

static void report_line(const char *line) {
    if (!g_report_path || !*g_report_path) return;
    HANDLE file = CreateFileA(g_report_path, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line, (DWORD)strlen(line), &written, NULL);
    WriteFile(file, "\n", 1, &written, NULL);
    CloseHandle(file);
}

static int contains(const char *text, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return 1;
    for (; *text; ++text) {
        size_t i = 0;
        while (i < n && text[i]) {
            char a = text[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
            if (a != b) break;
            ++i;
        }
        if (i == n) return 1;
    }
    return 0;
}

static int module_for_process(DWORD process_id, char *module, size_t capacity) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                               process_id);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32A entry;
    entry.dwSize = (DWORD)sizeof entry;
    int found = 0;
    if (Module32First(snapshot, &entry)) {
        do {
            if (contains(entry.szModule, g_match_module) ||
                contains(entry.szExePath, g_match_module)) {
                copy_text(module, entry.szModule, capacity);
                found = 1;
                break;
            }
            entry.dwSize = (DWORD)sizeof entry;
        } while (Module32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

static BOOL WINAPI probe_window(HWND window, LPARAM parameter) {
    (void)parameter;
    char title[256];
    char class_name[128];
    RECT client;
    DWORD pid = 0;
    char module[512];
    title[0] = '\0';
    class_name[0] = '\0';
    module[0] = '\0';
    client.left = client.top = client.right = client.bottom = 0;
    if (p_get_text) p_get_text(window, title, (int)sizeof title);
    if (p_get_class) p_get_class(window, class_name, (int)sizeof class_name);
    if (p_get_client) p_get_client(window, &client);
    if (p_get_thread) p_get_thread(window, &pid);
    if (pid) module_for_process(pid, module, sizeof module);
    int visible = p_is_visible ? p_is_visible(window) : 0;
    int width = (int)(client.right - client.left);
    int height = (int)(client.bottom - client.top);
    if (visible || width > 100 || height > 100) {
        printf("hwnd=%p pid=%lu visible=%d size=%dx%d class='%s' title='%s' module='%s'\n",
               window, (unsigned long)pid, visible, width, height, class_name, title, module);
        char detail[1024];
        snprintf(detail, sizeof detail,
                 "window hwnd=%p pid=%lu visible=%d size=%dx%d class='%s' title='%s' module='%s'",
                 window, (unsigned long)pid, visible, width, height, class_name, title, module);
        report_line(detail);
    }
    /* Require the owning module to identify the actual game. A generic GLFW
     * window or another game's title is not a safe input target. */
    if (visible && contains(module, g_match_module) && contains(title, g_match_title)) {
        g_target = window;
        g_target_pid = pid;
        copy_text(g_target_module, module, sizeof g_target_module);
        report_line("target_match=true");
    }
    return TRUE;
}

static int set_client_cursor(HWND window, int x_percent, int y_percent) {
    RECT client;
    POINT point;
    if (!p_get_client || !p_client_to_screen || !p_set_cursor ||
        !p_get_client(window, &client)) return 0;
    point.x = (int)((client.right - client.left) * x_percent / 100);
    point.y = (int)((client.bottom - client.top) * y_percent / 100);
    if (!p_client_to_screen(window, &point)) return 0;
    return p_set_cursor(point.x, point.y) != 0;
}

static int send_mouse(HWND window, int x_percent, int y_percent) {
    if (!p_mouse_event || !set_client_cursor(window, x_percent, y_percent)) return 0;
    Sleep(150);
    p_mouse_event(0x0002, 0, 0, 0, 0); /* MOUSEEVENTF_LEFTDOWN */
    Sleep(100);
    p_mouse_event(0x0004, 0, 0, 0, 0); /* MOUSEEVENTF_LEFTUP */
    return 1;
}

static int send_enter(void) {
    if (!p_keybd_event) return 0;
    p_keybd_event(0x0d, 0, 0, 0); /* VK_RETURN down */
    Sleep(250);
    p_keybd_event(0x0d, 0, 0x0002, 0); /* KEYEVENTF_KEYUP */
    Sleep(180);
    return 1;
}

static int screen_test(void) {
    int width = p_get_metrics ? p_get_metrics(0) : 0;
    int height = p_get_metrics ? p_get_metrics(1) : 0;
    HWND foreground = p_get_foreground ? p_get_foreground() : NULL;
    if (width <= 0 || height <= 0 || !p_set_cursor || !p_mouse_event) {
        printf("screen-test unavailable (foreground=%p screen=%dx%d)\n",
               foreground, width, height);
        return 2;
    }
    printf("screen-test X display foreground=%p screen=%dx%d; injecting low-level click and Enter\n",
           foreground, width, height);
    p_set_cursor(width / 2, height / 2);
    Sleep(150);
    p_mouse_event(0x0002, 0, 0, 0, 0);
    Sleep(100);
    p_mouse_event(0x0004, 0, 0, 0, 0);
    Sleep(250);
    return send_enter() ? 0 : 2;
}

static int expect_title(const char *needle) {
    if (!needle || !p_get_text) return 1;
    char title[256];
    for (int attempt = 0; attempt < 40; ++attempt) {
        title[0] = '\0';
        p_get_text(g_target, title, (int)sizeof title);
        if (contains(title, needle)) {
            char detail[320];
            snprintf(detail, sizeof detail, "target_response=true title='%s'", title);
            report_line(detail);
            printf("expected target response observed: '%s'\n", title);
            return 1;
        }
        Sleep(50);
    }
    report_line("target_response=false");
    printf("input was sent but target did not report expected title '%s'\n", needle);
    return 0;
}

int main(int argc, char **argv) {
    HMODULE user32 = LoadLibraryA("user32.dll");
    if (!user32) {
        printf("cannot load user32\n");
        return 1;
    }
    p_enum_windows = (enum_windows_fn)GetProcAddress(user32, "EnumWindows");
    p_is_visible = (is_visible_fn)GetProcAddress(user32, "IsWindowVisible");
    p_get_text = (get_text_fn)GetProcAddress(user32, "GetWindowTextA");
    p_get_class = (get_class_fn)GetProcAddress(user32, "GetClassNameA");
    p_get_client = (get_client_fn)GetProcAddress(user32, "GetClientRect");
    p_client_to_screen = (client_to_screen_fn)GetProcAddress(user32, "ClientToScreen");
    p_get_thread = (get_thread_fn)GetProcAddress(user32, "GetWindowThreadProcessId");
    p_get_metrics = (get_metrics_fn)GetProcAddress(user32, "GetSystemMetrics");
    p_get_foreground = (foreground_fn)GetProcAddress(user32, "GetForegroundWindow");
    p_set_foreground = (set_foreground_fn)GetProcAddress(user32, "SetForegroundWindow");
    p_set_cursor = (set_cursor_fn)GetProcAddress(user32, "SetCursorPos");
    p_mouse_event = (mouse_event_fn)GetProcAddress(user32, "mouse_event");
    p_keybd_event = (keybd_event_fn)GetProcAddress(user32, "keybd_event");
    if (!p_enum_windows || !p_get_thread || !p_get_client || !p_client_to_screen) {
        printf("missing window APIs (enum=%p pid=%p client=%p client_to_screen=%p)\n",
               (void *)p_enum_windows, (void *)p_get_thread, (void *)p_get_client,
               (void *)p_client_to_screen);
        return 1;
    }
    const char *action = "play";
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--title=", 8) == 0) g_match_title = argv[i] + 8;
        else if (strncmp(argv[i], "--module=", 9) == 0) g_match_module = argv[i] + 9;
        else if (strncmp(argv[i], "--expect-title=", 15) == 0) g_expect_title = argv[i] + 15;
        else if (strncmp(argv[i], "--report=", 9) == 0) g_report_path = argv[i] + 9;
        else action = argv[i];
    }
    report_line("probe_started=true");
    if (strcmp(action, "screen-test") == 0) return screen_test();
    printf("foreground=%p\n", p_get_foreground ? (void *)p_get_foreground() : 0);
    for (int attempt = 0; attempt < 30 && !g_target; ++attempt) {
        p_enum_windows(probe_window, 0);
        if (!g_target) Sleep(100);
    }
    if (!g_target) {
        report_line("target_match=false");
        printf("no visible window owned by hyperdemon.exe was found\n");
        return 1;
    }
    printf("target hwnd=%p pid=%lu module='%s' action=%s\n", g_target,
           (unsigned long)g_target_pid, g_target_module, action);
    if (p_set_foreground) p_set_foreground(g_target);
    Sleep(300);
    if (!p_get_foreground || p_get_foreground() != g_target) {
        char detail[128];
        snprintf(detail, sizeof detail, "foreground_verified=false target=%p current=%p",
                 g_target, p_get_foreground ? p_get_foreground() : NULL);
        report_line(detail);
        printf("refusing input: target window did not become foreground\n");
        return 2;
    }
    printf("target foreground verified; input uses SetCursorPos/mouse_event/keybd_event\n");
    report_line("foreground_verified=true");
    if (strcmp(action, "play") == 0) {
        if (!send_mouse(g_target, 4, 50)) return 2;
    } else if (strcmp(action, "center") == 0) {
        if (!send_mouse(g_target, 50, 50)) return 2;
    } else if (strcmp(action, "enter") == 0) {
        if (!send_enter()) return 2;
    } else if (strcmp(action, "click-enter") == 0) {
        if (!send_mouse(g_target, 50, 50)) return 2;
        Sleep(200);
        if (!send_enter()) return 2;
    } else {
        printf("unknown action '%s' (play, center, enter, click-enter, screen-test)\n", action);
        return 2;
    }
    if (p_get_foreground() != g_target) {
        report_line("foreground_preserved=false");
        printf("foreground changed during input; no additional input sent\n");
        return 2;
    }
    report_line("input_sequence_sent=true");
    printf("input sequence sent to verified target hwnd=%p pid=%lu\n", g_target,
           (unsigned long)g_target_pid);
    return expect_title(g_expect_title) ? 0 : 3;
}
