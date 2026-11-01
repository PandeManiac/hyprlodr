/*
 * In-process discovery of the game's asset records.
 *
 * The pack (`asset/asset`) is decrypted per slice, but the name -> (offset, size)
 * index is built at run time and has no static copy in the executable, so one
 * live record is needed to start reading real payloads. Rather than call into the
 * resolver (whose context pointer is not known statically), the probe scans the
 * process's committed memory for the record layout that was already measured:
 *
 *   +0x00 u32 offset      +0x04 u32 length      +0x20 std::string name
 *   +0x30 u64 size        +0x38 u64 capacity    +0x40 u32 type tag
 *
 * A candidate is only reported when the adjacent 0x58-byte slot parses too, which
 * is what makes a real record array distinguishable from coincidence. The probe
 * is read-only: it allocates nothing in the game, patches nothing, and writes one
 * JSON line per record to its own log beside the other HYPRLODR evidence files.
 */
#ifndef HYPRLODR_ASSET_PROBE_H
#define HYPRLODR_ASSET_PROBE_H

/* Start the background scan. `log_path` receives the JSON lines. Returns 0 when
 * the probe thread was created, -1 otherwise; it never blocks the caller. */
int hyprlodr_asset_probe_start(const char *game_dir, const char *log_path);

/* Records reported so far (0 until the scan finds an array). */
int hyprlodr_asset_probe_records(void);

/* "scanning", "found", "not_found", "log_open_failed", "no_memory". */
const char *hyprlodr_asset_probe_status(void);

#endif /* HYPRLODR_ASSET_PROBE_H */
