// expand, ported from the reference effects/expand.rs.
#ifndef GLYPHFX_EFFECT_EXPAND_H
#define GLYPHFX_EFFECT_EXPAND_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    Easing expand_easing;
    double movement_speed;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} ExpandConfig;

void expand_config_defaults(void *cfg);
void expand_free_config(void *cfg);
Effect *expand_make(const void *cfg);

#endif
