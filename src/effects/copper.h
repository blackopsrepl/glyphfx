// copper: a glyphfx original effect (not in ttfx; outside the parity set).
#ifndef GLYPHFX_EFFECT_COPPER_H
#define GLYPHFX_EFFECT_COPPER_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    ColorList bar_colors;
    int64_t bar_height;
    int64_t bounce_frames;
    int64_t settle_frames;
    Color unlit_color;
    Color sweep_color;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} CopperConfig;

void copper_config_defaults(void *cfg);
void copper_free_config(void *cfg);
Effect *copper_make(const void *cfg);

#endif
