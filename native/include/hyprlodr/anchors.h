/*
 * Runtime build fingerprints.
 *
 * Computers are cheap; silent memory corruption is not. Before HYPRLODR ever
 * touches an address that came from static analysis, it proves the running
 * executable is the build that analysis described.
 *
 * An anchor is a fixed RVA plus the FNV-1a 64 hash of the bytes that should be
 * there. Anchors are deliberately data-free: the repository stores hashes, not
 * game content, and the generated header (native/win32/game_anchors.h) is
 * produced by tools/re/analyze_pe.py.
 *
 * Verification is total: a build that matches three of four anchors is an
 * unknown build, not a mostly-known one. Callers must treat any mismatch as
 * "do not install hooks".
 */
#ifndef HYPRLODR_ANCHORS_H
#define HYPRLODR_ANCHORS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    uint32_t rva;
    uint32_t length;
    uint64_t fnv1a64;
} hyprlodr_anchor;

typedef struct {
    size_t total;
    size_t matched;
    size_t out_of_range;
    const char *first_mismatch; /* NULL when every anchor matched */
} hyprlodr_anchor_report;

/* FNV-1a 64 over `length` bytes. Stable across platforms and compilers. */
uint64_t hyprlodr_fnv1a64(const void *data, size_t length);

/* Compare every anchor against the mapped image.
 * Returns 0 when all anchors match, 1 when any mismatch or range error is
 * found, -1 on invalid arguments. Always fills `report` when it is non-NULL. */
int hyprlodr_anchors_verify(const uint8_t *image, size_t image_size,
                            const hyprlodr_anchor *anchors, size_t count,
                            hyprlodr_anchor_report *report);

#endif /* HYPRLODR_ANCHORS_H */
