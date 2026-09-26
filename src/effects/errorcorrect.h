// errorcorrect, ported from the reference effects/errorcorrect.rs.
#ifndef GLYPHFX_EFFECT_ERRORCORRECT_H
#define GLYPHFX_EFFECT_ERRORCORRECT_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    double error_pairs;
    int64_t swap_delay;
    Color error_color;
    Color correct_color;
    double movement_speed;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} ErrorCorrectConfig;

void errorcorrect_config_defaults(void *cfg);
void errorcorrect_free_config(void *cfg);
Effect *errorcorrect_make(const void *cfg);

#endif
