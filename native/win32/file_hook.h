/*
 * In-game file interception (ABI 4).
 *
 * hyperdemon.exe imports CreateFileA from KERNEL32.dll, so replacing that one
 * import-table slot lets the loader observe every file the game itself opens and
 * substitute a mod's file when a registered redirect matches. Nothing in the
 * game's code is patched, no address from the analysis is used, and the hook is
 * only installed when the import directory actually contains the entry.
 */
#ifndef HYPRLODR_FILE_HOOK_H
#define HYPRLODR_FILE_HOOK_H

#include "loader.h"

/* Patch the main module's KERNEL32!CreateFileA slot. `log_path` receives one
 * JSON line per observed open, written with WriteFile so logging cannot call
 * back into the hook. Returns 0 when the hook is installed and never aborts the
 * process: a host that cannot patch reports why through the status string. */
int hyprlodr_file_hook_install(hyprlodr_loader *loader, const char *log_path);

/* Patch the main module from DllMain, before any game code runs. hyperdemon.exe
 * opens assets during its own start-up, which raced the worker-thread install and
 * made those opens invisible. `loader` may be NULL here because no redirect is
 * published yet; hyprlodr_file_hook_install() completes the walk over every
 * loaded module and is safe to call afterwards. Idempotent. */
int hyprlodr_file_hook_install_early(hyprlodr_loader *loader, const char *log_path);

/* Called by the bootstrap worker after mod initialization has either published
 * the redirect registry or failed. Early asset/asset opens wait for this state
 * (with a bounded timeout) before resolving their path. */
void hyprlodr_file_hook_mark_redirects_ready(void);

/* "installed", "installed_no_log", "no_image", "create_file_a_not_imported", ...
 * Always non-NULL so it can be copied straight into the attach record. */
const char *hyprlodr_file_hook_status(void);

/* Parsing detail for diagnostics, e.g. the import RVA and descriptor count that
 * were read from the in-memory image. Empty when nothing was parsed. */
const char *hyprlodr_file_hook_detail(void);

/* Number of opens logged so far (saturates at the log limit). */
int hyprlodr_file_hook_logged(void);

/* Number of asset/asset opens currently held at the redirect readiness gate.
 * Intended for diagnostics and deterministic startup fixtures. */
unsigned hyprlodr_file_hook_archive_waiters(void);

typedef enum {
    HYPRLODR_ARCHIVE_GATE_PENDING = 0,
    HYPRLODR_ARCHIVE_GATE_READY = 1,
    HYPRLODR_ARCHIVE_GATE_FALLBACK = 2,
    HYPRLODR_ARCHIVE_GATE_ACTIVATING = 3
} hyprlodr_archive_gate_state;

/* Route activation claims the pending gate before enabling the hook, then
 * completes it immediately afterward. This makes a timeout race fail closed:
 * either timeout owns the gate and routing stays disabled, or activation owns
 * it and waiters observe the enabled route before proceeding. */
int hyprlodr_file_hook_begin_route_activation(void);
int hyprlodr_file_hook_complete_route_activation(void);
hyprlodr_archive_gate_state hyprlodr_file_hook_archive_gate_state(void);
volatile long *hyprlodr_file_hook_archive_gate_word(void);

#endif /* HYPRLODR_FILE_HOOK_H */
