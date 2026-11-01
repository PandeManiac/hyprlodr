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

/* "installed", "installed_no_log", "no_image", "create_file_a_not_imported", ...
 * Always non-NULL so it can be copied straight into the attach record. */
const char *hyprlodr_file_hook_status(void);

/* Parsing detail for diagnostics, e.g. the import RVA and descriptor count that
 * were read from the in-memory image. Empty when nothing was parsed. */
const char *hyprlodr_file_hook_detail(void);

/* Number of opens logged so far (saturates at the log limit). */
int hyprlodr_file_hook_logged(void);

#endif /* HYPRLODR_FILE_HOOK_H */
