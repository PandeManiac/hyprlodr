#define HYPRLODR_MINIMAL_WIN32 1
#include "hypr_win32.h"
#include "startup_diag.h"

#include <string.h>

static char *find_last_separator(char *text) {
    char *result = NULL;
    for (char *at = text; *at; ++at)
        if (*at == '\\') result = at;
    return result;
}

static int has_text(const char *text, const char *needle) {
    size_t needle_length = strlen(needle);
    if (!needle_length) return 1;
    for (const char *at = text; *at; ++at) {
        size_t index = 0;
        while (index < needle_length && at[index] == needle[index]) ++index;
        if (index == needle_length) return 1;
    }
    return 0;
}

static DWORD WINAPI emit_from_worker(LPVOID unused) {
    (void)unused;
    hyprlodr_startup_diag_emit(HYPRLODR_DIAG_ARCHIVE_ATTEMPT,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, "fixture worker event");
    return 0;
}

int main(void) {
    char path[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, path, (DWORD)sizeof path);
    if (!length || length >= sizeof path) return 1;
    char *separator = find_last_separator(path);
    if (!separator) return 2;
    separator[1] = '\0';
    const char suffix[] = "startup-diag.jsonl";
    if (strlen(path) + sizeof suffix > sizeof path) return 3;
    memcpy(path + strlen(path), suffix, sizeof suffix);

    hyprlodr_startup_diag_enable(1);
    hyprlodr_startup_diag_emit_once(HYPRLODR_DIAG_EARLY_HOOK,
        0u, 0u, 0u, 0u, 1u, 0u, 0u, "fixture early event");
    if (hyprlodr_startup_diag_start(path) != 0) return 4;
    HANDLE producer = CreateThread(NULL, 0, emit_from_worker, NULL, 0, NULL);
    if (!producer) return 5;
    if (WaitForSingleObject(producer, 5000u) != 0u) return 6;
    CloseHandle(producer);

    for (unsigned attempt = 0; attempt < 100u; ++attempt) {
        HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            char contents[4096];
            DWORD read = 0;
            DWORD size = GetFileSize(file, NULL);
            if (size < sizeof contents - 1u && ReadFile(file, contents, size, &read, NULL)) {
                contents[read] = '\0';
                CloseHandle(file);
                if (has_text(contents, "early_hook_install") &&
                    has_text(contents, "bootstrap_start") &&
                    has_text(contents, "first_archive_attempt")) {
                    hyprlodr_startup_diag_request_stop();
                    Sleep(60u);
                    return 0;
                }
            } else {
                CloseHandle(file);
            }
        }
        Sleep(10u);
    }
    hyprlodr_startup_diag_request_stop();
    return 5;
}
