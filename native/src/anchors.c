#include "hyprlodr/anchors.h"

static int bytes_equal(const uint8_t *left, const uint8_t *right, size_t length) {
    for (size_t index = 0; index < length; ++index)
        if (left[index] != right[index]) return 0;
    return 1;
}

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

int hyprlodr_anchors_verify_except(const uint8_t *image, size_t image_size,
                                   const hyprlodr_anchor *anchors, size_t count,
                                   uint32_t excluded_rva, uint32_t excluded_length,
                                   hyprlodr_anchor_report *report) {
    if (!image || !anchors || !report || excluded_length == 0) return -1;
    report->total = 0;
    report->matched = 0;
    report->out_of_range = 0;
    report->first_mismatch = NULL;
    size_t excluded = 0;
    for (size_t index = 0; index < count; ++index) {
        const hyprlodr_anchor *anchor = &anchors[index];
        if (anchor->rva == excluded_rva && anchor->length == excluded_length) {
            ++excluded;
            continue;
        }
        ++report->total;
        uint64_t end = (uint64_t)anchor->rva + (uint64_t)anchor->length;
        if (anchor->length == 0 || end > (uint64_t)image_size) {
            ++report->out_of_range;
            if (!report->first_mismatch) report->first_mismatch = anchor->name;
        } else if (hyprlodr_fnv1a64(image + anchor->rva, anchor->length) ==
                   anchor->fnv1a64) {
            ++report->matched;
        } else if (!report->first_mismatch) {
            report->first_mismatch = anchor->name;
        }
    }
    if (excluded != 1u) return -1;
    return report->matched == report->total ? 0 : 1;
}

int hyprlodr_anchors_verify_hooked(const uint8_t *image, size_t image_size,
                                   const hyprlodr_anchor *anchors, size_t count,
                                   const hyprlodr_anchor_report *baseline,
                                   uint32_t patch_rva, const uint8_t *patch_bytes,
                                   uint32_t patch_length,
                                   hyprlodr_anchor_report *live_report) {
    if (!image || !anchors || !baseline || !patch_bytes || !live_report ||
        patch_length == 0 || baseline->total != count ||
        baseline->matched != count || baseline->out_of_range != 0 ||
        baseline->first_mismatch != NULL ||
        (uint64_t)patch_rva + patch_length > (uint64_t)image_size ||
        !bytes_equal(image + patch_rva, patch_bytes, patch_length))
        return -1;
    return hyprlodr_anchors_verify_except(image, image_size, anchors, count,
                                           patch_rva, patch_length, live_report);
}
