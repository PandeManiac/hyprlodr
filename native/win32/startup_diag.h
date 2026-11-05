#ifndef HYPRLODR_STARTUP_DIAG_H
#define HYPRLODR_STARTUP_DIAG_H

#include <stdint.h>

typedef enum {
    HYPRLODR_DIAG_EARLY_HOOK = 0,
    HYPRLODR_DIAG_FILE_HOOK,
    HYPRLODR_DIAG_BOOTSTRAP_START,
    HYPRLODR_DIAG_ARCHIVE_ATTEMPT,
    HYPRLODR_DIAG_ARCHIVE_WAIT,
    HYPRLODR_DIAG_ARCHIVE_OPEN,
    HYPRLODR_DIAG_REDIRECT_PUBLICATION,
    HYPRLODR_DIAG_ROUTE_PREFLIGHT,
    HYPRLODR_DIAG_ROUTE_ACTIVATION,
    HYPRLODR_DIAG_GATE_RELEASE,
    HYPRLODR_DIAG_FIRST_CALLSITE,
    HYPRLODR_DIAG_FIRST_HAND2,
    HYPRLODR_DIAG_FIRST_TARGET,
    HYPRLODR_DIAG_HAND2_LOADER_CALL,
    HYPRLODR_DIAG_HAND2_LOADER_RETURN,
    HYPRLODR_DIAG_TARGET_LOADER_CALL,
    HYPRLODR_DIAG_TARGET_LOADER_RETURN,
    HYPRLODR_DIAG_KIND_COUNT
} hyprlodr_startup_diag_kind;

/* Event payload flags are caller-defined. Emitters only reserve and copy into
 * fixed process storage; JSON formatting and file I/O happen on the worker. */
void hyprlodr_startup_diag_enable(int enabled);
int hyprlodr_startup_diag_enabled(void);
void hyprlodr_startup_diag_emit(hyprlodr_startup_diag_kind kind,
                                uint32_t tag, uint32_t offset, uint32_t size,
                                uint32_t gate_state, uint32_t flags,
                                uintptr_t result, uintptr_t parsed_object,
                                const char *text);
void hyprlodr_startup_diag_emit_once(hyprlodr_startup_diag_kind kind,
                                     uint32_t tag, uint32_t offset, uint32_t size,
                                     uint32_t gate_state, uint32_t flags,
                                     uintptr_t result, uintptr_t parsed_object,
                                     const char *text);
int hyprlodr_startup_diag_start(const char *path);
void hyprlodr_startup_diag_drain(void);
void hyprlodr_startup_diag_request_stop(void);
void hyprlodr_startup_diag_pump(void);

#endif
