/*
 * Damped angular spring for hierarchical feather secondary motion.
 *
 * This is deliberately independent of the renderer and of any game ABI: one
 * instance drives one feather bone's sweep or flare channel.  The caller feeds
 * a target angle derived from airflow/acceleration and calls reset() when the
 * arm teleports or a level transition makes the previous velocity meaningless.
 */
#ifndef HYPRLODR_FEATHER_SPRING_H
#define HYPRLODR_FEATHER_SPRING_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hyprlodr_feather_spring {
    float stiffness;
    float damping;
    float limit;
    float angle;
    float velocity;
} hyprlodr_feather_spring;

/* stiffness and damping are in angular units; limit bounds both the target and
 * the resulting angle.  Non-finite or non-positive values are replaced with
 * safe defaults so a bad asset cannot destabilise the simulation. */
void hyprlodr_feather_spring_init(hyprlodr_feather_spring *spring, float stiffness,
                                  float damping, float limit);

/* Drop all accumulated motion.  Use after a discontinuity (teleport, respawn,
 * level change) rather than letting the spring chase through the jump. */
void hyprlodr_feather_spring_reset(hyprlodr_feather_spring *spring);

/* Advance by dt seconds and return the new angle.  Sub-steps at 240 Hz so the
 * response is stable and nearly identical at 30, 60, 144 and uncapped frame
 * rates.  A zero, negative or non-finite dt leaves the spring untouched. */
float hyprlodr_feather_spring_step(hyprlodr_feather_spring *spring, float target, float dt);

#ifdef __cplusplus
}
#endif

#endif /* HYPRLODR_FEATHER_SPRING_H */
