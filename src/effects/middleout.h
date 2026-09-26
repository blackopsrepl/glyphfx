// middleout, ported from the reference effects/middleout.rs.
#ifndef GLYPHFX_EFFECT_MIDDLEOUT_H
#define GLYPHFX_EFFECT_MIDDLEOUT_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef enum {
    MIDDLEOUT_VERTICAL,
    MIDDLEOUT_HORIZONTAL,
} MiddleoutExpandDirection;

typedef struct {
    Color starting_color;
    MiddleoutExpandDirection expand_direction;
    double center_movement_speed;
    double full_movement_speed;
    Easing center_easing;
    Easing full_easing;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} MiddleoutConfig;

void middleout_config_defaults(void *cfg);
void middleout_free_config(void *cfg);
Effect *middleout_make(const void *cfg);

#endif
