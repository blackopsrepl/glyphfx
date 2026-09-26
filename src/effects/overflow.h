// overflow, ported from the reference effects/overflow.rs.
#ifndef GLYPHFX_EFFECT_OVERFLOW_H
#define GLYPHFX_EFFECT_OVERFLOW_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    ColorList overflow_gradient_stops;
    IntRange overflow_cycles_range;
    int64_t overflow_speed;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} OverflowConfig;

void overflow_config_defaults(void *cfg);
void overflow_free_config(void *cfg);
Effect *overflow_make(const void *cfg);

#endif
