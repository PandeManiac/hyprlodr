#include <stdio.h>

#include "../../examples/hand_dispatch_state.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

int main(void) {
    int state = HYPR_HAND_DISPATCH_WAITING;
    void *shim = (void *)0x5000;
    void *driver = (void *)0x7000;
    CHECK(!hypr_hand_dispatch_executable_candidate(state, 0, NULL, shim, 0));
    CHECK(!hypr_hand_dispatch_executable_candidate(state, 0, driver, shim, 0));
    CHECK(!hypr_hand_dispatch_executable_candidate(state, 0, shim, shim, 1));
    /* Delayed dispatch initialization remains pending until a verified,
     * executable target appears. */
    CHECK(!hypr_hand_dispatch_executable_candidate(state, 0, NULL, shim, 0));
    CHECK(hypr_hand_dispatch_executable_candidate(state, 0, driver, shim, 1));
    state = HYPR_HAND_DISPATCH_INSTALLING;
    CHECK(hypr_hand_dispatch_finish_install(state, 1, 1) == HYPR_HAND_DISPATCH_INSTALLED);
    CHECK(hypr_hand_dispatch_finish_install(state, 0, 0) == HYPR_HAND_DISPATCH_FAILED);
    CHECK(hypr_hand_dispatch_finish_install(state, 1, 0) == HYPR_HAND_DISPATCH_FAILED);
    CHECK(hypr_hand_dispatch_timeout(HYPR_HAND_DISPATCH_WAITING, 0) ==
          HYPR_HAND_DISPATCH_FAILED);
    CHECK(hypr_hand_dispatch_timeout(HYPR_HAND_DISPATCH_WAITING, 1) ==
          HYPR_HAND_DISPATCH_WAITING);
    CHECK(hypr_hand_dispatch_stop(HYPR_HAND_DISPATCH_WAITING) == HYPR_HAND_DISPATCH_STOPPED);
    CHECK(!hypr_hand_dispatch_executable_candidate(HYPR_HAND_DISPATCH_STOPPED, 1,
                                                   driver, shim, 1));
    CHECK(hypr_hand_dispatch_stop(HYPR_HAND_DISPATCH_INSTALLED) == HYPR_HAND_DISPATCH_STOPPED);
    if (failures) return 1;
    puts("hand dispatch readiness: delayed, invalid, failure, and cleanup paths OK");
    return 0;
}
