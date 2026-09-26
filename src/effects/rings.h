// rings, ported from the reference effects/rings.rs.
#ifndef GLYPHFX_EFFECT_RINGS_H
#define GLYPHFX_EFFECT_RINGS_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    ColorList ring_colors;
    double ring_gap;
    int64_t spin_duration;
    FloatRange spin_speed;
    int64_t disperse_duration;
    int64_t spin_disperse_cycles;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} RingsConfig;

void rings_config_defaults(void *cfg);
void rings_free_config(void *cfg);
Effect *rings_make(const void *cfg);

#endif
