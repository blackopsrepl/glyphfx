// colorshift, ported from the reference effects/colorshift.rs.
#ifndef GLYPHFX_EFFECT_COLORSHIFT_H
#define GLYPHFX_EFFECT_COLORSHIFT_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    ColorList gradient_stops;
    IntList gradient_steps;
    int64_t gradient_frames;
    bool no_travel;
    GradientDirection travel_direction;
    bool reverse_travel_direction;
    bool no_loop;
    int64_t cycles;
    bool skip_final_gradient;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} ColorShiftConfig;

void colorshift_config_defaults(void *cfg);
void colorshift_free_config(void *cfg);
Effect *colorshift_make(const void *cfg);

#endif
