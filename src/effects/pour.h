// pour, ported from the reference effects/pour.rs.
#ifndef GLYPHFX_EFFECT_POUR_H
#define GLYPHFX_EFFECT_POUR_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef enum {
    POUR_UP,
    POUR_DOWN,
    POUR_LEFT,
    POUR_RIGHT,
} PourDirection;

typedef struct {
    PourDirection pour_direction;
    int64_t pour_speed;
    FloatRange movement_speed_range;
    int64_t gap;
    Color starting_color;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    int64_t final_gradient_frames;
    GradientDirection final_gradient_direction;
    Easing movement_easing;
} PourConfig;

void pour_config_defaults(void *cfg);
void pour_free_config(void *cfg);
Effect *pour_make(const void *cfg);

#endif
