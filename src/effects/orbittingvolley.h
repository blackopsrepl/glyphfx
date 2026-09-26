// orbittingvolley, ported from the reference effects/orbittingvolley.rs.
#ifndef GLYPHFX_EFFECT_ORBITTINGVOLLEY_H
#define GLYPHFX_EFFECT_ORBITTINGVOLLEY_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"
#include "utils/easing.h"

typedef struct {
    char *top_launcher_symbol;
    char *right_launcher_symbol;
    char *bottom_launcher_symbol;
    char *left_launcher_symbol;
    double launcher_movement_speed;
    double character_movement_speed;
    double volley_size;
    int64_t launch_delay;
    Easing character_easing;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} OrbittingVolleyConfig;

void orbittingvolley_config_defaults(void *cfg);
void orbittingvolley_free_config(void *cfg);
Effect *orbittingvolley_make(const void *cfg);

#endif
