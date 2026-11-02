#include "hypr_win32.h"

#include <stdio.h>
#include <string.h>

typedef BOOL (WINAPI *enum_windows_fn)(BOOL (*callback)(HWND, LPARAM), LPARAM parameter);
typedef BOOL (WINAPI *is_visible_fn)(HWND window);
typedef int (WINAPI *get_text_fn)(HWND window, LPSTR text, int max_count);
typedef int (WINAPI *get_class_fn)(HWND window, LPSTR text, int max_count);
typedef BOOL (WINAPI *get_client_fn)(HWND window, RECT *rect);
typedef DWORD (WINAPI *get_thread_fn)(HWND window, DWORD *process_id);
typedef BOOL (WINAPI *post_fn)(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
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
static get_thread_fn p_get_thread;
static post_fn p_post;
static foreground_fn p_get_foreground;
static set_foreground_fn p_set_foreground;
static set_cursor_fn p_set_cursor;
static mouse_event_fn p_mouse_event;
static keybd_event_fn p_keybd_event;

static HWND g_target;
static DWORD g_target_pid;

static int contains(const char *text, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return 1;
    for (; *text; ++text) {
        size_t i = 0;
        while (i < n && text[i] && text[i] == needle[i]) ++i;
        if (i == n) return 1;
    }
    return 0;
}

static BOOL WINAPI probe_window(HWND window, LPARAM parameter) {
    (void)parameter;
    char title[256];
    char class_name[128];
    RECT client;
    DWORD pid = 0;
    title[0] = '\0';
    class_name[0] = '\0';
    client.left = client.top = client.right = client.bottom = 0;
    if (p_get_text) p_get_text(window, title, (int)sizeof title);
    if (p_get_class) p_get_class(window, class_name, (int)sizeof class_name);
    if (p_get_client) p_get_client(window, &client);
    if (p_get_thread) p_get_thread(window, &pid);
    int visible = p_is_visible ? p_is_visible(window) : 0;
    int width = (int)(client.right - client.left);
    int height = (int)(client.bottom - client.top);
    if (visible || width > 100 || height > 100) {
        printf("hwnd=%p pid=%lu visible=%d size=%dx%d class='%s' title='%s'\n",
               window, (unsigned long)pid, visible, width, height, class_name, title);
    }
    if (visible && (contains(title, "hyperdemon") || contains(title, "HYPER") ||
                    contains(class_name, "GLFW"))) {
        g_target = window;
        g_target_pid = pid;
    }
    return TRUE;
}

static void send_mouse(HWND window, int x, int y, int down) {
    if (!p_post) return;
    LPARAM point = (LPARAM)((y << 16) | (x & 0xffff));
    p_post(window, 0x0200, 0, point); /* WM_MOUSEMOVE */
    if (down) {
        Sleep(180);
        p_post(window, 0x0201, 1, point); /* WM_LBUTTONDOWN */
        Sleep(300);
        p_post(window, 0x0202, 0, point); /* WM_LBUTTONUP */
        Sleep(180);
    }
}

static void send_enter(HWND window) {
    if (!p_post) return;
    p_post(window, 0x0100, 0x0d, 1);          /* WM_KEYDOWN VK_RETURN */
    Sleep(250);
    p_post(window, 0x0101, 0x0d, 0xC0000001); /* WM_KEYUP */
    Sleep(180);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
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
    p_get_thread = (get_thread_fn)GetProcAddress(user32, "GetWindowThreadProcessId");
    p_post = (post_fn)GetProcAddress(user32, "PostMessageA");
    p_get_foreground = (foreground_fn)GetProcAddress(user32, "GetForegroundWindow");
    p_set_foreground = (set_foreground_fn)GetProcAddress(user32, "SetForegroundWindow");
    p_set_cursor = (set_cursor_fn)GetProcAddress(user32, "SetCursorPos");
    p_mouse_event = (mouse_event_fn)GetProcAddress(user32, "mouse_event");
    p_keybd_event = (keybd_event_fn)GetProcAddress(user32, "keybd_event");
    if (!p_enum_windows || !p_post) {
        printf("missing user32 entry points (enum=%p post=%p)\n",
               (void *)p_enum_windows, (void *)p_post);
        return 1;
    }
    printf("foreground=%p\n", p_get_foreground ? (void *)p_get_foreground() : 0);
    p_enum_windows(probe_window, 0);
    if (!g_target) {
        printf("no hyperdemon window found\n");
        return 1;
    }
    const char *action = argc > 1 ? argv[1] : "all";
    printf("target hwnd=%p pid=%lu action=%s\n", g_target,
           (unsigned long)g_target_pid, action);
    if (p_set_foreground) p_set_foreground(g_target);
    Sleep(300);
    if (strcmp(action, "input") == 0) {
        if (p_set_cursor && p_mouse_event) {
            p_set_cursor(70, 535);
            Sleep(250);
            p_mouse_event(0x0002, 0, 0, 0, 0); /* MOUSEEVENTF_LEFTDOWN */
            Sleep(300);
            p_mouse_event(0x0004, 0, 0, 0, 0); /* MOUSEEVENTF_LEFTUP */
            Sleep(2500);
            p_set_cursor(960, 540);
            Sleep(250);
            p_mouse_event(0x0002, 0, 0, 0, 0);
            Sleep(300);
            p_mouse_event(0x0004, 0, 0, 0, 0);
        }
        if (p_keybd_event) {
            Sleep(500);
            p_keybd_event(0x0d, 0, 0, 0); /* VK_RETURN down */
            Sleep(250);
            p_keybd_event(0x0d, 0, 0x0002, 0); /* KEYEVENTF_KEYUP */
        }
    } else if (strcmp(action, "play") == 0) {
        send_mouse(g_target, 70, 535, 1);
    } else if (strcmp(action, "center") == 0) {
        send_mouse(g_target, 960, 540, 1);
    } else if (strcmp(action, "enter") == 0) {
        send_enter(g_target);
    } else {
        send_mouse(g_target, 70, 535, 1);
        Sleep(3000);
        send_mouse(g_target, 960, 540, 1);
        Sleep(1000);
        send_enter(g_target);
    }
    printf("posted %s\n", action);
    return 0;
}
