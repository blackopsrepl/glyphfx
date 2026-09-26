// burn, ported from the reference effects/burn.rs.
#ifndef GLYPHFX_EFFECT_BURN_H
#define GLYPHFX_EFFECT_BURN_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    Color starting_color;
    ColorList burn_colors;
    double smoke_chance;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} BurnConfig;

void burn_config_defaults(void *cfg);
void burn_free_config(void *cfg);
Effect *burn_make(const void *cfg);

#endif
