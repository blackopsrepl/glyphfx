// print, ported from the reference effects/print_effect.rs.
#ifndef GLYPHFX_EFFECT_PRINT_H
#define GLYPHFX_EFFECT_PRINT_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    double print_head_return_speed;
    int64_t print_speed;
    Easing print_head_easing;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} PrintConfig;

void print_config_defaults(void *cfg);
void print_free_config(void *cfg);
Effect *print_make(const void *cfg);

#endif
