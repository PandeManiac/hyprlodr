/* Windows x64 process entry shim.
 *
 * The Windows loader calls a PE entry point with (HINSTANCE, HINSTANCE, LPSTR),
 * not (argc, argv). Compiling with /entry:main would therefore hand main() an
 * image base as argc and an uninitialised pointer as argv. This shim rebuilds
 * the argument vector from GetCommandLineA and calls main() normally.
 *
 * Only console executables link this file; the proxy DLL uses dll_main instead.
 */
#include "hypr_win32.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv);

int hyprlodr_entry(void) {
    static char storage[8192];
    static char *args[64];
    const char *command = GetCommandLineA();
    size_t length;
    int argc = 0;
    char *cursor;

    if (!command) return 2;
    length = strlen(command);
    if (length >= sizeof storage) length = sizeof storage - 1;
    memcpy(storage, command, length);
    storage[length] = '\0';

    cursor = storage;
    while (*cursor && argc < 63) {
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (!*cursor) break;
        if (*cursor == '"') {
            ++cursor;
            args[argc++] = cursor;
            while (*cursor && *cursor != '"') ++cursor;
            if (*cursor == '"') *cursor++ = '\0';
        } else {
            args[argc++] = cursor;
            while (*cursor && *cursor != ' ' && *cursor != '\t') ++cursor;
            if (*cursor) *cursor++ = '\0';
        }
    }
    return main(argc, args);
}
