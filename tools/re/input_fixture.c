/* Small Win32 window that proves the Proton input probe delivered mouse and
 * keyboard events to the selected target. Build only as a local test artifact. */
#include "hypr_win32.h"

#define WM_DESTROY 0x0002u
#define WM_KEYDOWN 0x0100u
#define WM_LBUTTONDOWN 0x0201u
#define WM_TIMER 0x0113u
#define VK_RETURN 0x0du
#define WS_OVERLAPPEDWINDOW 0x00cf0000u
#define SW_SHOW 5

static int saw_mouse;
static int saw_key;

static LRESULT WINAPI fixture_window_proc(HWND window, UINT message,
                                           WPARAM wparam, LPARAM lparam) {
    (void)lparam;
    if (message == WM_LBUTTONDOWN) saw_mouse = 1;
    if (message == WM_KEYDOWN && wparam == VK_RETURN) saw_key = 1;
    if (saw_mouse && saw_key) {
        SetWindowTextA(window, "HYPRLODR INPUT OK");
        SetTimer(window, 1, 1500, NULL);
    }
    if (message == WM_TIMER && wparam == 1) {
        DestroyWindow(window);
        PostQuitMessage(0);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

int main(void) {
    WNDCLASSA cls;
    MSG message;
    HINSTANCE instance = GetModuleHandleA(NULL);
    cls.style = 0;
    cls.lpfnWndProc = fixture_window_proc;
    cls.cbClsExtra = 0;
    cls.cbWndExtra = 0;
    cls.hInstance = instance;
    cls.hIcon = NULL;
    cls.hCursor = NULL;
    cls.hbrBackground = NULL;
    cls.lpszMenuName = NULL;
    cls.lpszClassName = "HYPRLODRInputFixture";
    if (!RegisterClassA(&cls)) return 2;
    HWND window = CreateWindowExA(0, cls.lpszClassName,
                                  "HYPRLODR input fixture", WS_OVERLAPPEDWINDOW,
                                  150, 100, 500, 400, NULL, NULL, instance, NULL);
    if (!window) return 3;
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    while (GetMessageA(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    return saw_mouse && saw_key ? 0 : 4;
}
