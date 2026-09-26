// scattered, ported from the reference effects/scattered.rs.
#ifndef GLYPHFX_EFFECT_SCATTERED_H
#define GLYPHFX_EFFECT_SCATTERED_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    double movement_speed;
    Easing movement_easing;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    int64_t final_gradient_frames;
    GradientDirection final_gradient_direction;
} ScatteredConfig;

void scattered_config_defaults(void *cfg);
void scattered_free_config(void *cfg);
Effect *scattered_make(const void *cfg);

#endif
