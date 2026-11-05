/*
 * Minimal Win32 declarations for the HYPRLODR Windows x64 target.
 *
 * The loader only needs a handful of kernel32 entry points. Declaring them here
 * keeps the Windows build free of any SDK or mingw sysroot dependency: clang
 * compiles the COFF objects and lld-link links them against import libraries
 * generated from the .def files in native/win32.
 *
 * This is NOT a general purpose windows.h. It covers exactly what HYPRLODR uses.
 * Builds with a real Windows SDK (MSVC/mingw) use <windows.h> instead.
 */
#ifndef HYPRLODR_WIN32_H
#define HYPRLODR_WIN32_H

#define HYPRLODR_MINIMAL_WIN32 1

#ifdef __cplusplus
extern "C" {
#endif

typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef long LONG;
typedef void *HANDLE;
typedef void *LPVOID;
typedef const void *LPCVOID;
typedef void *HMODULE;
typedef void *HINSTANCE;
typedef char *LPSTR;
typedef const char *LPCSTR;
/* 16-bit wide character, as on Windows. Declared here because the target has no
 * C runtime header to get it from. */
typedef unsigned short wchar_t;
typedef unsigned int UINT;
#define CP_UTF8 65001u
typedef int (*FARPROC)(void);
typedef unsigned long long ULONG_PTR;
typedef unsigned long long uintptr_t;
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);

#ifndef TRUE
#  define TRUE 1
#  define FALSE 0
#endif
#ifndef NULL
#  define NULL ((void *)0)
#endif

#define WINAPI __stdcall
#define CALLBACK __stdcall

#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 0

#define FILE_ATTRIBUTE_DIRECTORY 0x00000010u
#define MAX_PATH 260

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define CREATE_ALWAYS 2u
#define OPEN_ALWAYS 4u
#define OPEN_EXISTING 3u
#define FILE_ATTRIBUTE_NORMAL 0x00000080u
#define FILE_SHARE_READ 0x00000001u
#define FILE_SHARE_WRITE 0x00000002u
#define FILE_APPEND_DATA 0x00000004u
#define INFINITE 0xffffffffu
#define PAGE_READWRITE 0x00000004u
#define PAGE_EXECUTE_READWRITE 0x00000040u
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define INVALID_HANDLE_VALUE ((HANDLE)(long long)-1)

typedef struct {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME;

#define TH32CS_SNAPMODULE 0x00000008u
#define TH32CS_SNAPMODULE32 0x00000010u

/* MODULEENTRY32A: fixed layout from the Windows SDK; the string buffers are
 * inline, so the whole structure stays POD. */
typedef struct {
    DWORD dwSize;
    DWORD th32ModuleID;
    DWORD th32ProcessID;
    DWORD GlblcntUsage;
    DWORD ProccntUsage;
    BYTE *modBaseAddr;
    DWORD modBaseSize;
    HMODULE hModule;
    char szModule[MAX_PATH];
    char szExePath[MAX_PATH];
} MODULEENTRY32A;

typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    char cFileName[MAX_PATH];
    char cAlternateFileName[14];
} WIN32_FIND_DATAA;

HMODULE WINAPI LoadLibraryA(LPCSTR name);
void WINAPI ExitProcess(UINT exit_code);
FARPROC WINAPI GetProcAddress(HMODULE module, LPCSTR name);
BOOL WINAPI FreeLibrary(HMODULE module);
HMODULE WINAPI GetModuleHandleA(LPCSTR name);
DWORD WINAPI GetModuleFileNameA(HMODULE module, LPSTR buffer, DWORD size);
HANDLE WINAPI CreateThread(void *attributes, size_t stack_size, LPTHREAD_START_ROUTINE start,
                           LPVOID parameter, DWORD flags, DWORD *thread_id);
BOOL WINAPI DisableThreadLibraryCalls(HMODULE module);
void WINAPI Sleep(DWORD milliseconds);
DWORD WINAPI GetCurrentProcessId(void);
DWORD WINAPI GetCurrentThreadId(void);
/* Milliseconds since boot; monotonic within one session, which is all the file
 * log needs to say when an open happened relative to the hook installing. */
DWORD WINAPI GetTickCount(void);
HANDLE WINAPI FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *data);
BOOL WINAPI FindNextFileA(HANDLE find, WIN32_FIND_DATAA *data);
BOOL WINAPI FindClose(HANDLE find);
/* Minimal exception plumbing for the crash diagnostic. */
typedef struct {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    void *ExceptionRecord;
    void *ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[15];
} HYPR_EXCEPTION_RECORD;

typedef struct {
    HYPR_EXCEPTION_RECORD *ExceptionRecord;
    void *ContextRecord;
} HYPR_EXCEPTION_POINTERS;

typedef LONG (*LPTOP_LEVEL_EXCEPTION_FILTER)(HYPR_EXCEPTION_POINTERS *info);
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetUnhandledExceptionFilter(
    LPTOP_LEVEL_EXCEPTION_FILTER filter);

BOOL WINAPI CreateDirectoryA(LPCSTR path, void *security);
HANDLE WINAPI CreateFileA(LPCSTR name, DWORD access, DWORD share, void *security,
                          DWORD disposition, DWORD flags, HANDLE template_file);
HANDLE WINAPI CreateEventA(void *attributes, BOOL manual_reset, BOOL initial_state,
                           LPCSTR name);
BOOL WINAPI SetEvent(HANDLE event);
DWORD WINAPI WaitForSingleObject(HANDLE handle, DWORD milliseconds);
LONG WINAPI InterlockedCompareExchange(volatile LONG *destination, LONG exchange,
                                       LONG comparand);
LONG WINAPI InterlockedExchange(volatile LONG *destination, LONG value);
HANDLE WINAPI CreateFileW(const wchar_t *name, DWORD access, DWORD share, void *security,
                          DWORD disposition, DWORD flags, HANDLE template_file);
BOOL WINAPI WriteFile(HANDLE file, LPCVOID buffer, DWORD bytes, DWORD *written, void *overlapped);
BOOL WINAPI ReadFile(HANDLE file, LPVOID buffer, DWORD bytes, DWORD *read, void *overlapped);
BOOL WINAPI CloseHandle(HANDLE handle);
BOOL WINAPI VirtualProtect(LPVOID address, size_t size, DWORD new_protect, DWORD *old_protect);
LPVOID WINAPI VirtualAlloc(LPVOID address, size_t size, DWORD allocation_type,
                           DWORD protection);
HANDLE WINAPI GetCurrentProcess(void);
BOOL WINAPI FlushInstructionCache(HANDLE process, LPCVOID address, size_t size);

/* --- address-space queries: the asset-record probe --------------------------
 * The probe walks committed, readable regions looking for the game's asset
 * records, so it needs VirtualQuery and the page constants, nothing more. */
#define MEM_COMMIT 0x00001000u
#define MEM_RESERVE 0x00002000u
#define MEM_PRIVATE 0x00020000u
#define MEM_MAPPED 0x00040000u
#define PAGE_NOACCESS 0x00000001u
#define PAGE_READONLY 0x00000002u
#define PAGE_READWRITE 0x00000004u
#define PAGE_WRITECOPY 0x00000008u
#define PAGE_EXECUTE_READ 0x00000020u
#define PAGE_EXECUTE 0x00000010u
#define PAGE_EXECUTE_READWRITE 0x00000040u
#define PAGE_EXECUTE_WRITECOPY 0x00000080u
#define PAGE_GUARD 0x00000100u

/* The x64 MEMORY_BASIC_INFORMATION the kernel actually fills in is 48 bytes;
 * naming it explicitly here avoids depending on a SDK layout macro. */
typedef struct {
    void *BaseAddress;
    void *AllocationBase;
    DWORD AllocationProtect;
    WORD PartitionId;
    WORD Reserved0;
    unsigned long long RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
    DWORD Reserved1;
} MEMORY_BASIC_INFORMATION64A;

size_t WINAPI VirtualQuery(const void *address, MEMORY_BASIC_INFORMATION64A *info,
                           size_t length);
/* Wide/narrow conversion for the _wfopen shim. */
int WINAPI WideCharToMultiByte(UINT codepage, DWORD flags, const wchar_t *wide, int wide_length,
                               char *narrow, int narrow_capacity, const char *default_char,
                               BOOL *used_default);
int WINAPI MultiByteToWideChar(UINT codepage, DWORD flags, const char *narrow, int narrow_length,
                               wchar_t *wide, int wide_capacity);
HANDLE WINAPI GetProcessHeap(void);
void *WINAPI HeapAlloc(HANDLE heap, DWORD flags, size_t bytes);
void *WINAPI HeapReAlloc(HANDLE heap, DWORD flags, void *block, size_t bytes);
BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, void *block);
HANDLE WINAPI GetStdHandle(DWORD std_handle);
HANDLE WINAPI CreateToolhelp32Snapshot(DWORD flags, DWORD process_id);
BOOL WINAPI Module32First(HANDLE snapshot, MODULEENTRY32A *entry);
BOOL WINAPI Module32Next(HANDLE snapshot, MODULEENTRY32A *entry);
LPSTR WINAPI GetCommandLineA(void);
DWORD WINAPI GetFileSize(HANDLE file, DWORD *high);

/* --- user32/gdi32: the title overlay ----------------------------------------
 * Only the entry points the overlay uses; this stays a minimal target, not a
 * general purpose windows.h. */
typedef void *HWND;
typedef void *HDC;
typedef void *HGDIOBJ;
typedef void *HBITMAP;
typedef void *HMENU;
typedef void *HBRUSH;
typedef void *HICON;
typedef void *HCURSOR;
typedef long long LPARAM;
typedef unsigned long long WPARAM;
typedef long long LRESULT;
typedef DWORD COLORREF;
typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);

typedef struct {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
} RECT;

typedef struct {
    LONG x;
    LONG y;
} POINT;

typedef struct {
    HWND hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
    POINT pt;
} MSG;

typedef struct {
    LONG cx;
    LONG cy;
} SIZE;

typedef struct {
    BYTE b;
    BYTE g;
    BYTE r;
    BYTE reserved;
} RGBQUAD;

typedef struct {
    DWORD biSize;
    LONG biWidth;
    LONG biHeight;
    WORD biPlanes;
    WORD biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG biXPelsPerMeter;
    LONG biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
} BITMAPINFOHEADER;

typedef struct {
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD bmiColors[1];
} BITMAPINFO;

typedef struct {
    BYTE BlendOp;
    BYTE BlendFlags;
    BYTE SourceConstantAlpha;
    BYTE AlphaFormat;
} BLENDFUNCTION;

typedef struct {
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCSTR lpszMenuName;
    LPCSTR lpszClassName;
} WNDCLASSA;

#define WS_POPUP 0x80000000u
#define WS_EX_LAYERED 0x00080000u
#define WS_EX_TRANSPARENT 0x00000020u
#define WS_EX_TOPMOST 0x00000008u
#define WS_EX_TOOLWINDOW 0x00000080u
#define WS_EX_NOACTIVATE 0x08000000u
#define SWP_NOACTIVATE 0x0010u
#define SWP_SHOWWINDOW 0x0040u
#define DIB_RGB_COLORS 0u

WORD WINAPI RegisterClassA(const WNDCLASSA *window_class);
HWND WINAPI CreateWindowExA(DWORD ex_style, LPCSTR class_name, LPCSTR window_name, DWORD style,
                            int x, int y, int width, int height, HWND parent, HMENU menu,
                            HINSTANCE instance, void *parameter);
LRESULT WINAPI DefWindowProcA(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
BOOL WINAPI ShowWindow(HWND window, int command);
BOOL WINAPI UpdateWindow(HWND window);
BOOL WINAPI SetWindowTextA(HWND window, LPCSTR text);
int WINAPI GetMessageA(MSG *message, HWND window, UINT first, UINT last);
BOOL WINAPI TranslateMessage(const MSG *message);
LRESULT WINAPI DispatchMessageA(const MSG *message);
void WINAPI PostQuitMessage(int exit_code);
uintptr_t WINAPI SetTimer(HWND window, uintptr_t timer_id, UINT timeout_ms,
                          void *timer_callback);
BOOL WINAPI SetWindowPos(HWND window, HWND insert_after, int x, int y, int width, int height,
                         UINT flags);
BOOL WINAPI UpdateLayeredWindow(HWND window, HDC destination, POINT *destination_point,
                                SIZE *size, HDC source, POINT *source_point, COLORREF color_key,
                                BLENDFUNCTION *blend, DWORD flags);
BOOL WINAPI DestroyWindow(HWND window);
BOOL WINAPI GetWindowRect(HWND window, RECT *rect);
BOOL WINAPI GetClientRect(HWND window, RECT *rect);
BOOL WINAPI IsWindowVisible(HWND window);
BOOL WINAPI EnumWindows(BOOL (*callback)(HWND, LPARAM), LPARAM parameter);
DWORD WINAPI GetWindowThreadProcessId(HWND window, DWORD *process_id);
short WINAPI GetAsyncKeyState(int key);
HDC WINAPI GetDC(HWND window);
int WINAPI ReleaseDC(HWND window, HDC device);
HDC WINAPI CreateCompatibleDC(HDC device);
BOOL WINAPI DeleteDC(HDC device);
HBITMAP WINAPI CreateDIBSection(HDC device, const BITMAPINFO *info, UINT usage, void **bits,
                                HANDLE section, DWORD offset);
HGDIOBJ WINAPI SelectObject(HDC device, HGDIOBJ object);
BOOL WINAPI DeleteObject(HGDIOBJ object);

#ifdef __cplusplus
}
#endif

#endif /* HYPRLODR_WIN32_H */
