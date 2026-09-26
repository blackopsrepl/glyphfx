// bubbles, ported from the reference effects/bubbles.rs.
#ifndef GLYPHFX_EFFECT_BUBBLES_H
#define GLYPHFX_EFFECT_BUBBLES_H

#include <stdbool.h>
#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef enum {
    POP_ROW,
    POP_BOTTOM,
    POP_ANYWHERE,
} PopCondition;

typedef struct {
    bool rainbow;
    ColorList bubble_colors;
    Color pop_color;
    double bubble_speed;
    int64_t bubble_delay;
    PopCondition pop_condition;
    Easing movement_easing;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} BubblesConfig;

void bubbles_config_defaults(void *cfg);
void bubbles_free_config(void *cfg);
Effect *bubbles_make(const void *cfg);

#endif
