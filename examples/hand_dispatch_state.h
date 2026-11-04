#ifndef HYPRLODR_HAND_DISPATCH_STATE_H
#define HYPRLODR_HAND_DISPATCH_STATE_H

/* Portable dispatch readiness state logic. The Windows worker owns the actual
 * polling and image write; keeping transitions here lets native tests exercise
 * delayed readiness, invalid candidates, write failure, and shutdown. */
enum hypr_hand_dispatch_state {
    HYPR_HAND_DISPATCH_FAILED = -1,
    HYPR_HAND_DISPATCH_WAITING = 0,
    HYPR_HAND_DISPATCH_INSTALLING = 1,
    HYPR_HAND_DISPATCH_INSTALLED = 2,
    HYPR_HAND_DISPATCH_STOPPED = 3
};

static inline int hypr_hand_dispatch_executable_candidate(int state, int stopped,
                                                          const void *candidate,
                                                          const void *shim,
                                                          int executable) {
    return state == HYPR_HAND_DISPATCH_WAITING && !stopped && candidate &&
           candidate != shim && executable;
}

static inline int hypr_hand_dispatch_finish_install(int state, int write_ok,
                                                    int verify_ok) {
    if (state != HYPR_HAND_DISPATCH_INSTALLING) return state;
    return write_ok && verify_ok ? HYPR_HAND_DISPATCH_INSTALLED
                                 : HYPR_HAND_DISPATCH_FAILED;
}

static inline int hypr_hand_dispatch_stop(int state) {
    return state == HYPR_HAND_DISPATCH_INSTALLED || state == HYPR_HAND_DISPATCH_WAITING
               ? HYPR_HAND_DISPATCH_STOPPED : state;
}

static inline int hypr_hand_dispatch_timeout(int state, int stopped) {
    return state == HYPR_HAND_DISPATCH_WAITING && !stopped
               ? HYPR_HAND_DISPATCH_FAILED : state;
}

#endif
