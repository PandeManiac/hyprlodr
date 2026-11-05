#include "callsite_hook.h"
#include "hypr_win32.h"
#include "hyprlodr/anchors.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { printf("fingerprint transition: FAIL: %s\n", message); ++failures; } \
} while (0)

static uintptr_t replacement(void *context, void *record, const void *name, uint32_t flags) {
    (void)context; (void)record; (void)name; (void)flags;
    return 0;
}

static void put_call(uint8_t *site, const void *target) {
    int64_t delta = (const uint8_t *)target - (site + 5u);
    int32_t rel = (int32_t)delta;
    site[0] = 0xe8u;
    memcpy(site + 1u, &rel, sizeof rel);
}

static uint8_t *new_image(void) {
    uint8_t *image = (uint8_t *)VirtualAlloc(NULL, 4096u, MEM_RESERVE | MEM_COMMIT,
                                             PAGE_EXECUTE_READWRITE);
    if (image) {
        memset(image, 0x90, 4096u);
        put_call(image + 0x100u, image + 0x200u);
        memcpy(image + 0x300u, "anchor-data", 11u);
    }
    return image;
}

static void test_valid_lifecycle(void) {
    uint8_t *image = new_image();
    CHECK(image != NULL, "synthetic image allocation");
    if (!image) return;
    const hyprlodr_anchor anchors[] = {
        {"call:record_loader", 0x100u, 5u, hyprlodr_fnv1a64(image + 0x100u, 5u)},
        {"data:sentinel", 0x300u, 11u, hyprlodr_fnv1a64(image + 0x300u, 11u)},
    };
    hyprlodr_anchor_report baseline, current;
    CHECK(hyprlodr_anchors_verify(image, 4096u, anchors, 2u, &baseline) == 0,
          "all original anchors match before early initialization");

    hyprlodr_call_hook hook = HYPRLODR_CALL_HOOK_INITIALIZER;
    CHECK(hyprlodr_call_hook_install(&hook, image + 0x100u, image + 0x100u,
          image + 0x200u, (void *)replacement, HYPRLODR_CALL_HOOK_INSTALL_QUIESCENT,
          NULL) == HYPRLODR_CALL_HOOK_OK, "early initialization installs the call hook");
    CHECK(hyprlodr_anchors_verify(image, 4096u, anchors, 2u, &current) == 1,
          "ordinary full fingerprint detects the five-byte hook patch");
    CHECK(hyprlodr_call_hook_site_matches(&hook),
          "the exact patch bytes recorded by the call hook remain installed");
    CHECK(hyprlodr_anchors_verify_hooked(image, 4096u, anchors, 2u, &baseline,
          0x100u, hook.installed_bytes, sizeof hook.installed_bytes, &current) == 0,
          "bootstrap accepts preserved supported baseline plus exact hook state");
    CHECK(current.total == 1u && current.matched == 1u,
          "post-hook verification independently checks every other anchor");

    image[0x300u] ^= 1u;
    CHECK(hyprlodr_anchors_verify_hooked(image, 4096u, anchors, 2u, &baseline,
          0x100u, hook.installed_bytes, sizeof hook.installed_bytes, &current) == 1,
          "unexpected modification to another anchor is rejected");
    image[0x300u] ^= 1u;

    image[0x102u] ^= 1u;
    CHECK(!hyprlodr_call_hook_site_matches(&hook),
          "unexpected post-install callsite change fails hook-state check");
    CHECK(hyprlodr_anchors_verify_hooked(image, 4096u, anchors, 2u, &baseline,
          0x100u, hook.installed_bytes, sizeof hook.installed_bytes, &current) == -1,
          "unexpected callsite change is not authorized by baseline snapshot");
}

static void test_invalid_baseline_stays_invalid(void) {
    uint8_t *image = new_image();
    CHECK(image != NULL, "invalid-image allocation");
    if (!image) return;
    image[0x300u] ^= 1u;
    const hyprlodr_anchor anchors[] = {
        {"call:record_loader", 0x100u, 5u, hyprlodr_fnv1a64(image + 0x100u, 5u)},
        {"data:sentinel", 0x300u, 11u,
         hyprlodr_fnv1a64((const uint8_t *)"anchor-data", 11u)},
    };
    hyprlodr_anchor_report baseline, current;
    CHECK(hyprlodr_anchors_verify(image, 4096u, anchors, 2u, &baseline) == 1,
          "image invalid before hook has an invalid baseline");
    hyprlodr_call_hook hook = HYPRLODR_CALL_HOOK_INITIALIZER;
    CHECK(hyprlodr_call_hook_install(&hook, image + 0x100u, image + 0x100u,
          image + 0x200u, (void *)replacement, HYPRLODR_CALL_HOOK_INSTALL_QUIESCENT,
          NULL) == HYPRLODR_CALL_HOOK_OK, "hook bytes alone can still be patched");
    CHECK(hyprlodr_anchors_verify_hooked(image, 4096u, anchors, 2u, &baseline,
          0x100u, hook.installed_bytes, sizeof hook.installed_bytes, &current) == -1,
          "an unsupported initial image cannot become supported after patching");
}

int main(void) {
    test_valid_lifecycle();
    test_invalid_baseline_stays_invalid();
    if (failures) return 1;
    printf("fingerprint transition checks passed\n");
    return 0;
}
