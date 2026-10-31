#include "hyprlodr/anchors.h"

uint64_t hyprlodr_fnv1a64(const void *data, size_t length) {
    const uint8_t *bytes = (const uint8_t *)data;
    /* Explicit suffixes: the freestanding Windows CRT shim has no UINT64_C. */
    uint64_t value = 0xcbf29ce484222325ull;
    for (size_t index = 0; index < length; ++index) {
        value ^= (uint64_t)bytes[index];
        value *= 0x100000001b3ull;
    }
    return value;
}

int hyprlodr_anchors_verify(const uint8_t *image, size_t image_size,
                            const hyprlodr_anchor *anchors, size_t count,
                            hyprlodr_anchor_report *report) {
    if (!image || !anchors || !report) return -1;

    report->total = count;
    report->matched = 0;
    report->out_of_range = 0;
    report->first_mismatch = NULL;

    for (size_t index = 0; index < count; ++index) {
        const hyprlodr_anchor *anchor = &anchors[index];
        uint64_t end = (uint64_t)anchor->rva + (uint64_t)anchor->length;
        if (end > (uint64_t)image_size || anchor->length == 0) {
            ++report->out_of_range;
            if (!report->first_mismatch) report->first_mismatch = anchor->name;
            continue;
        }
        if (hyprlodr_fnv1a64(image + anchor->rva, anchor->length) == anchor->fnv1a64) {
            ++report->matched;
        } else if (!report->first_mismatch) {
            report->first_mismatch = anchor->name;
        }
    }

    return report->matched == report->total ? 0 : 1;
}
