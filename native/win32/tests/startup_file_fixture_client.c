#define HYPRLODR_MINIMAL_WIN32 1
#include "hypr_win32.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

__declspec(dllimport) FILE *fixture_fiopen(const char *path, int mode, int protection)
    __asm("?_Fiopen@std@@YAPEAU_iobuf@@PEBDHH@Z");
void startup_file_fixture_release(void);
LONG startup_file_fixture_status(void);
LONG startup_file_fixture_route_enabled(void);
unsigned startup_file_fixture_waiters(void);

typedef LONG (*call_count_fn)(void);
typedef const char *(*last_path_fn)(void);
typedef struct {
    const char *path;
    FILE *result;
} open_request;

static DWORD WINAPI open_thread(LPVOID context) {
    open_request *request = (open_request *)context;
    request->result = fixture_fiopen(request->path, 0, 0);
    return 0;
}

static int fail(const char *message) {
    printf("startup file fixture: FAIL: %s\n", message);
    return 1;
}

static int contains(const char *text, const char *needle) {
    size_t length = strlen(needle);
    for (const char *at = text; at && *at; ++at)
        if (strncmp(at, needle, length) == 0) return 1;
    return 0;
}

static int wait_for_waiters(unsigned expected) {
    DWORD started = GetTickCount();
    while (startup_file_fixture_waiters() != expected) {
        if (GetTickCount() - started > 10000u) return 0;
        Sleep(0);
    }
    return 1;
}

static HANDLE start_open(open_request *request) {
    DWORD thread_id = 0;
    return CreateThread(NULL, 0, open_thread, request, 0, &thread_id);
}

int main(int argc, char **argv) {
    if (argc != 2) return fail("expected one fixture mode");
    const char *mode = argv[1];
    HMODULE msvcp = GetModuleHandleA("msvcp140.dll");
    if (!msvcp) return fail("static MSVCP fixture dependency is missing");
    call_count_fn call_count = (call_count_fn)GetProcAddress(msvcp, "fixture_fiopen_call_count");
    last_path_fn last_path = (last_path_fn)GetProcAddress(msvcp, "fixture_fiopen_last_path");
    if (!call_count || !last_path) return fail("MSVCP fixture diagnostics are missing");
    if (call_count() != 0) return fail("unexpected pre-entry file open");

    if (strcmp(mode, "worker-fail") == 0) {
        FILE *opened = fixture_fiopen("asset/asset", 0, 0);
        if (opened != (FILE *)(uintptr_t)0x1234u || call_count() != 1 ||
            strcmp(last_path(), "asset/asset") != 0 || startup_file_fixture_status() != 5)
            return fail("worker creation failure did not fall back to original archive");
        printf("startup file fixture: worker creation failure released first archive open\n");
        return 0;
    }

    /* Non-archive opens must never wait for bootstrap. */
    FILE *unrelated = fixture_fiopen("settings/user.cfg", 0, 0);
    if (unrelated != (FILE *)(uintptr_t)0x1234u || call_count() != 1 ||
        strcmp(last_path(), "settings/user.cfg") != 0)
        return fail("unrelated file open was changed or blocked");

    if (strcmp(mode, "timeout") == 0) {
        open_request request = {"asset/asset", NULL};
        HANDLE thread = start_open(&request);
        if (!thread || !wait_for_waiters(1u)) return fail("timeout request did not reach gate");
        if (WaitForSingleObject(thread, 5000u) != 0u)
            return fail("bounded wait did not return");
        CloseHandle(thread);
        if (request.result != (FILE *)(uintptr_t)0x1234u || call_count() != 2 ||
            strcmp(last_path(), "asset/asset") != 0)
            return fail("timeout did not call the original archive path");
        startup_file_fixture_release();
        DWORD started = GetTickCount();
        while (startup_file_fixture_status() != 6 && GetTickCount() - started < 10000u)
            Sleep(0);
        if (startup_file_fixture_status() != 6 || startup_file_fixture_route_enabled() != 0)
            return fail("timeout fallback did not block later route activation");
        FILE *later = fixture_fiopen("asset/asset", 0, 0);
        if (later != (FILE *)(uintptr_t)0x1234u || call_count() != 3 ||
            strcmp(last_path(), "asset/asset") != 0)
            return fail("late bootstrap redirected archive after timeout fallback");
        printf("startup file fixture: bounded wait timed out and fell back to original\n");
        return 0;
    }

    if (strcmp(mode, "concurrent") == 0) {
        open_request first = {"asset/asset", NULL};
        open_request second = {"C:/game/asset/asset", NULL};
        HANDLE a = start_open(&first), b = start_open(&second);
        if (!a || !b || !wait_for_waiters(2u)) return fail("both concurrent opens did not reach gate");
        if (call_count() != 1) return fail("an archive open escaped before publication");
        startup_file_fixture_release();
        if (WaitForSingleObject(a, 5000u) != 0u ||
            WaitForSingleObject(b, 5000u) != 0u)
            return fail("concurrent redirected opens did not finish");
        CloseHandle(a); CloseHandle(b);
        if (first.result != (FILE *)(uintptr_t)0x1234u ||
            second.result != (FILE *)(uintptr_t)0x1234u || call_count() != 3 ||
            !contains(last_path(), "replacement.txt") ||
            startup_file_fixture_route_enabled() != 1)
            return fail("concurrent opens did not use the published replacement");
        printf("startup file fixture: concurrent archive opens stayed held through publication\n");
        return 0;
    }

    open_request request = {"asset/asset", NULL};
    HANDLE thread = start_open(&request);
    if (!thread || !wait_for_waiters(1u)) return fail("first archive open did not reach gate");
    if (call_count() != 1) return fail("first archive reached original before bootstrap release");
    startup_file_fixture_release();
    if (WaitForSingleObject(thread, 5000u) != 0u)
        return fail("first archive open did not resume");
    CloseHandle(thread);
    if (request.result != (FILE *)(uintptr_t)0x1234u || call_count() != 2)
        return fail("first archive open was not forwarded exactly once");

    if (strcmp(mode, "success") == 0) {
        if (startup_file_fixture_status() != 1 ||
            startup_file_fixture_route_enabled() != 1 ||
            !contains(last_path(), "replacement.txt"))
            return fail("first archive did not use published redirect");
        printf("startup file fixture: first _Fiopen blocked until redirect publication\n");
        return 0;
    }
    if (strcmp(mode, "fail") == 0) {
        if (startup_file_fixture_status() != 2 ||
            startup_file_fixture_route_enabled() != 0 ||
            strcmp(last_path(), "asset/asset") != 0)
            return fail("bootstrap failure did not fall back to original archive");
        printf("startup file fixture: failed bootstrap released original archive open\n");
        return 0;
    }
    if (strcmp(mode, "no-redirect") == 0) {
        if (startup_file_fixture_status() != 1 ||
            startup_file_fixture_route_enabled() != 0 ||
            strcmp(last_path(), "asset/asset") != 0)
            return fail("replacement-disabled startup changed the archive path");
        printf("startup file fixture: replacement-disabled startup passed through\n");
        return 0;
    }
    return fail("unknown fixture mode");
}
