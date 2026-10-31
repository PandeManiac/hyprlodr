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
typedef int (*FARPROC)(void);
typedef unsigned long long ULONG_PTR;
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);

#ifndef TRUE
#  define TRUE 1
#  define FALSE 0
#endif
#ifndef NULL
#  define NULL ((void *)0)
#endif

#define WINAPI __stdcall

#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 0

#define FILE_ATTRIBUTE_DIRECTORY 0x00000010u
#define MAX_PATH 260

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define FILE_ATTRIBUTE_NORMAL 0x00000080u
#define FILE_SHARE_READ 0x00000001u
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define INVALID_HANDLE_VALUE ((HANDLE)(long long)-1)

typedef struct {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME;

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
FARPROC WINAPI GetProcAddress(HMODULE module, LPCSTR name);
BOOL WINAPI FreeLibrary(HMODULE module);
HMODULE WINAPI GetModuleHandleA(LPCSTR name);
DWORD WINAPI GetModuleFileNameA(HMODULE module, LPSTR buffer, DWORD size);
HANDLE WINAPI CreateThread(void *attributes, size_t stack_size, LPTHREAD_START_ROUTINE start,
                           LPVOID parameter, DWORD flags, DWORD *thread_id);
BOOL WINAPI DisableThreadLibraryCalls(HMODULE module);
void WINAPI Sleep(DWORD milliseconds);
DWORD WINAPI GetCurrentProcessId(void);
HANDLE WINAPI FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *data);
BOOL WINAPI FindNextFileA(HANDLE find, WIN32_FIND_DATAA *data);
BOOL WINAPI FindClose(HANDLE find);
HANDLE WINAPI CreateFileA(LPCSTR name, DWORD access, DWORD share, void *security,
                          DWORD disposition, DWORD flags, HANDLE template_file);
BOOL WINAPI WriteFile(HANDLE file, LPCVOID buffer, DWORD bytes, DWORD *written, void *overlapped);
BOOL WINAPI ReadFile(HANDLE file, LPVOID buffer, DWORD bytes, DWORD *read, void *overlapped);
BOOL WINAPI CloseHandle(HANDLE handle);
HANDLE WINAPI GetProcessHeap(void);
void *WINAPI HeapAlloc(HANDLE heap, DWORD flags, size_t bytes);
void *WINAPI HeapReAlloc(HANDLE heap, DWORD flags, void *block, size_t bytes);
BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, void *block);
HANDLE WINAPI GetStdHandle(DWORD std_handle);
LPSTR WINAPI GetCommandLineA(void);

#ifdef __cplusplus
}
#endif

#endif /* HYPRLODR_WIN32_H */
