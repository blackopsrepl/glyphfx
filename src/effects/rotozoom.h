// rotozoom: a glyphfx original effect (not in ttfx; outside the parity set).
#ifndef GLYPHFX_EFFECT_ROTOZOOM_H
#define GLYPHFX_EFFECT_ROTOZOOM_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    int64_t roto_frames;
    double turns;
    double zoom_depth;
    double pan_radius;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} RotozoomConfig;

void rotozoom_config_defaults(void *cfg);
void rotozoom_free_config(void *cfg);
Effect *rotozoom_make(const void *cfg);

#endif
