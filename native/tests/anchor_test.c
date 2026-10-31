/*
 * Unit tests for runtime build fingerprints.
 *
 * These cover the verifier itself. Whether the committed anchor table matches
 * the real game is a separate, external check:
 *   python3 tools/re/analyze_pe.py <game>/hyperdemon.exe \
 *       --verify native/win32/game_anchors.h
 * and, at run time, the proxy's hyprlodr-attach.json.
 */
#include <stdio.h>
#include <string.h>

#include "hyprlodr/anchors.h"

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static void test_fnv_reference_values(void) {
    CHECK(hyprlodr_fnv1a64("", 0) == 0xcbf29ce484222325ull);
    CHECK(hyprlodr_fnv1a64("a", 1) == 0xaf63dc4c8601ec8cull);
    CHECK(hyprlodr_fnv1a64("foobar", 6) == 0x85944171f73967e8ull);
}

static void test_verification(void) {
    uint8_t image[64];
    memset(image, 0, sizeof image);
    memcpy(image + 8, "anch", 4);
    memcpy(image + 40, "second", 6);

    const hyprlodr_anchor anchors[3] = {
        {"first", 8u, 4u, hyprlodr_fnv1a64(image + 8, 4)},
        {"second", 40u, 6u, hyprlodr_fnv1a64(image + 40, 6)},
        {"beyond", 60u, 8u, 0}, /* 60 + 8 exceeds the image */
    };

    hyprlodr_anchor_report report;

    /* A fully matching subset is success. */
    CHECK(hyprlodr_anchors_verify(image, sizeof image, anchors, 2, &report) == 0);
    CHECK(report.total == 2);
    CHECK(report.matched == 2);
    CHECK(report.out_of_range == 0);
    CHECK(report.first_mismatch == NULL);

    /* An out-of-range anchor is a failure, not a skipped anchor. */
    CHECK(hyprlodr_anchors_verify(image, sizeof image, anchors, 3, &report) == 1);
    CHECK(report.matched == 2);
    CHECK(report.out_of_range == 1);
    CHECK(report.first_mismatch && strcmp(report.first_mismatch, "beyond") == 0);

    /* One flipped byte invalidates the whole build. */
    image[9] = (uint8_t)(image[9] ^ 0xFF);
    CHECK(hyprlodr_anchors_verify(image, sizeof image, anchors, 3, &report) == 1);
    CHECK(report.matched == 1);
    CHECK(report.first_mismatch && strcmp(report.first_mismatch, "first") == 0);

    /* An image smaller than the anchor range never reads out of bounds. */
    CHECK(hyprlodr_anchors_verify(image, 4, anchors, 3, &report) == 1);
    CHECK(report.out_of_range == 3);

    CHECK(hyprlodr_anchors_verify(NULL, sizeof image, anchors, 3, &report) == -1);
    CHECK(hyprlodr_anchors_verify(image, sizeof image, NULL, 3, &report) == -1);
    CHECK(hyprlodr_anchors_verify(image, sizeof image, anchors, 3, NULL) == -1);
}

int main(void) {
    test_fnv_reference_values();
    test_verification();
    if (failures) {
        printf("%d anchor check(s) failed\n", failures);
        return 1;
    }
    printf("anchor checks passed\n");
    return 0;
}
