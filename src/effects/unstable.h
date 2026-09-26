// unstable, ported from the reference effects/unstable.rs.
#ifndef GLYPHFX_EFFECT_UNSTABLE_H
#define GLYPHFX_EFFECT_UNSTABLE_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"
#include "utils/easing.h"

typedef struct {
    Color unstable_color;
    Easing explosion_ease;
    double explosion_speed;
    Easing reassembly_ease;
    double reassembly_speed;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} UnstableConfig;

void unstable_config_defaults(void *cfg);
void unstable_free_config(void *cfg);
Effect *unstable_make(const void *cfg);

#endif
