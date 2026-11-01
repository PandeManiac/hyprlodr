#include <math.h>
#include <stdio.h>

#include "hyprlodr/feather_spring.h"

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static float run_at(hyprlodr_feather_spring *spring, float target, float dt, float duration) {
    float elapsed = 0.0f;
    while (elapsed + dt * 0.5f < duration) {
        hyprlodr_feather_spring_step(spring, target, dt);
        elapsed += dt;
    }
    return spring->angle;
}

static void test_frame_rate_independence(void) {
    const float duration = 1.5f;
    const float target = 0.65f;
    hyprlodr_feather_spring at_30;
    hyprlodr_feather_spring at_60;
    hyprlodr_feather_spring at_144;
    hyprlodr_feather_spring at_240;

    hyprlodr_feather_spring_init(&at_30, 64.0f, 9.0f, 0.95f);
    hyprlodr_feather_spring_init(&at_60, 64.0f, 9.0f, 0.95f);
    hyprlodr_feather_spring_init(&at_144, 64.0f, 9.0f, 0.95f);
    hyprlodr_feather_spring_init(&at_240, 64.0f, 9.0f, 0.95f);

    float a30 = run_at(&at_30, target, 1.0f / 30.0f, duration);
    float a60 = run_at(&at_60, target, 1.0f / 60.0f, duration);
    float a144 = run_at(&at_144, target, 1.0f / 144.0f, duration);
    float a240 = run_at(&at_240, target, 1.0f / 240.0f, duration);

    CHECK(fabsf(a30 - target) < 0.01f);
    CHECK(fabsf(a60 - target) < 0.01f);
    CHECK(fabsf(a144 - target) < 0.01f);
    CHECK(fabsf(a240 - target) < 0.01f);
    CHECK(fabsf(a30 - a240) < 0.015f);
    CHECK(fabsf(a60 - a144) < 0.005f);
}

static void test_bounds_and_reversal(void) {
    hyprlodr_feather_spring spring;
    hyprlodr_feather_spring_init(&spring, 64.0f, 9.0f, 0.5f);
    for (int i = 0; i < 600; ++i) {
        float angle = hyprlodr_feather_spring_step(&spring, 100.0f, 1.0f / 60.0f);
        CHECK(fabsf(angle) <= 0.5f * 1.35f + 1e-5f);
    }
    CHECK(fabsf(spring.angle - 0.5f) < 1e-3f);

    for (int i = 0; i < 600; ++i) {
        float angle = hyprlodr_feather_spring_step(&spring, -100.0f, 1.0f / 60.0f);
        CHECK(fabsf(angle) <= 0.5f * 1.35f + 1e-5f);
    }
    CHECK(fabsf(spring.angle + 0.5f) < 1e-3f);
}

static void test_reset_and_bad_input(void) {
    hyprlodr_feather_spring spring;
    hyprlodr_feather_spring_init(&spring, 64.0f, 9.0f, 0.95f);
    for (int i = 0; i < 120; ++i) {
        hyprlodr_feather_spring_step(&spring, 0.8f, 1.0f / 60.0f);
    }
    CHECK(fabsf(spring.angle) > 0.1f);
    hyprlodr_feather_spring_reset(&spring);
    CHECK(spring.angle == 0.0f);
    CHECK(spring.velocity == 0.0f);

    float held = hyprlodr_feather_spring_step(&spring, 0.8f, 0.0f);
    CHECK(held == 0.0f);
    held = hyprlodr_feather_spring_step(&spring, 0.8f, -1.0f);
    CHECK(held == 0.0f);
    held = hyprlodr_feather_spring_step(&spring, 0.8f, NAN);
    CHECK(held == 0.0f);
    held = hyprlodr_feather_spring_step(&spring, NAN, 1.0f / 60.0f);
    CHECK(held == 0.0f);

    hyprlodr_feather_spring bad;
    hyprlodr_feather_spring_init(&bad, NAN, -1.0f, 0.0f);
    CHECK(bad.stiffness == 64.0f);
    CHECK(bad.damping == 9.0f);
    CHECK(bad.limit == 0.95f);
}

int main(void) {
    test_frame_rate_independence();
    test_bounds_and_reversal();
    test_reset_and_bad_input();
    if (failures) {
        printf("%d feather spring check(s) failed\n", failures);
        return 1;
    }
    printf("feather spring checks passed\n");
    return 0;
}
