#include "hyprlodr/feather_spring.h"

#include <math.h>

#define HYPRLODR_SPRING_SUBSTEP (1.0f / 240.0f)
#define HYPRLODR_SPRING_MAX_STEPS 4096
#define HYPRLODR_SPRING_OVERSHOOT 1.35f

static int finite_positive(float value) {
    return isfinite(value) && value > 0.0f;
}

static float clampf(float value, float limit) {
    if (value > limit) {
        return limit;
    }
    if (value < -limit) {
        return -limit;
    }
    return value;
}

static void sanitise(hyprlodr_feather_spring *spring) {
    if (spring == 0) {
        return;
    }
    if (!finite_positive(spring->stiffness)) {
        spring->stiffness = 64.0f;
    }
    if (!finite_positive(spring->damping)) {
        spring->damping = 9.0f;
    }
    if (!finite_positive(spring->limit)) {
        spring->limit = 0.95f;
    }
    if (!isfinite(spring->angle)) {
        spring->angle = 0.0f;
    }
    if (!isfinite(spring->velocity)) {
        spring->velocity = 0.0f;
    }
}

void hyprlodr_feather_spring_init(hyprlodr_feather_spring *spring, float stiffness,
                                  float damping, float limit) {
    if (spring == 0) {
        return;
    }
    spring->stiffness = stiffness;
    spring->damping = damping;
    spring->limit = limit;
    spring->angle = 0.0f;
    spring->velocity = 0.0f;
    sanitise(spring);
}

void hyprlodr_feather_spring_reset(hyprlodr_feather_spring *spring) {
    if (spring == 0) {
        return;
    }
    spring->angle = 0.0f;
    spring->velocity = 0.0f;
}

float hyprlodr_feather_spring_step(hyprlodr_feather_spring *spring, float target, float dt) {
    int steps = 0;
    float remaining;

    if (spring == 0) {
        return 0.0f;
    }
    sanitise(spring);
    if (!isfinite(dt) || dt <= 0.0f) {
        return spring->angle;
    }
    if (!isfinite(target)) {
        hyprlodr_feather_spring_reset(spring);
        return spring->angle;
    }

    target = clampf(target, spring->limit);
    remaining = dt;
    while (remaining > 1e-7f && steps < HYPRLODR_SPRING_MAX_STEPS) {
        float h = remaining < HYPRLODR_SPRING_SUBSTEP ? remaining : HYPRLODR_SPRING_SUBSTEP;
        float acceleration = spring->stiffness * (target - spring->angle)
                             - spring->damping * spring->velocity;
        spring->velocity += acceleration * h;
        spring->angle += spring->velocity * h;
        ++steps;

        if (!isfinite(spring->angle) || !isfinite(spring->velocity)) {
            hyprlodr_feather_spring_reset(spring);
            return spring->angle;
        }
        if (fabsf(spring->angle) > spring->limit * HYPRLODR_SPRING_OVERSHOOT) {
            spring->angle = copysignf(spring->limit * HYPRLODR_SPRING_OVERSHOOT,
                                      spring->angle);
            spring->velocity *= -0.15f;
        }
        remaining -= h;
    }
    return spring->angle;
}
