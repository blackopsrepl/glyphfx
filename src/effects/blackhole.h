// blackhole, ported from the reference effects/blackhole.rs.
#ifndef GLYPHFX_EFFECT_BLACKHOLE_H
#define GLYPHFX_EFFECT_BLACKHOLE_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    Color blackhole_color;
    ColorList star_colors;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} BlackholeConfig;

void blackhole_config_defaults(void *cfg);
void blackhole_free_config(void *cfg);
Effect *blackhole_make(const void *cfg);

#endif
