// stack: a glyphfx original effect (not in ttfx; outside the parity set).
#ifndef GLYPHFX_EFFECT_STACK_H
#define GLYPHFX_EFFECT_STACK_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    int64_t gravity_frames;
    int64_t stack_frames;
    int64_t max_moves;
    int64_t clear_frames;
    ColorList piece_colors;
    Color flash_color;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} StackConfig;

void stack_config_defaults(void *cfg);
void stack_free_config(void *cfg);
Effect *stack_make(const void *cfg);

#endif
