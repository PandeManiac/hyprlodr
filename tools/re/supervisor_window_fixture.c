#include "hypr_win32.h"

#define WM_DESTROY 0x0002u
#define WM_TIMER 0x0113u
#define WS_OVERLAPPEDWINDOW 0x00cf0000u
#define SW_SHOW 5

static LRESULT WINAPI window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    (void)lparam;
    if (message == WM_TIMER && wparam == 1u) {
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
    cls.lpfnWndProc = window_proc;
    cls.cbClsExtra = 0;
    cls.cbWndExtra = 0;
    cls.hInstance = instance;
    cls.hIcon = NULL;
    cls.hCursor = NULL;
    cls.hbrBackground = NULL;
    cls.lpszMenuName = NULL;
    cls.lpszClassName = "HYPERDEMONSupervisorFixture";
    if (!RegisterClassA(&cls)) return 2;
    HWND window = CreateWindowExA(0, cls.lpszClassName,
        "HYPERDEMON supervisor fixture window", WS_OVERLAPPEDWINDOW,
        150, 100, 640, 480, NULL, NULL, instance, NULL);
    if (!window) return 3;
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    if (!SetTimer(window, 1u, 1500u, NULL)) return 4;
    while (GetMessageA(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    return 0;
}
